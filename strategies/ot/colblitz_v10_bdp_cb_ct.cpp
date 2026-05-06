// sphere:stateless
// Upgrade strategy .so from the default -O2 to -O3 to enable
// auto-vectorization and additional inlining.
#pragma GCC optimize("O3")
// Disable FMA to match JS/Python floating-point rounding behaviour.
#pragma GCC optimize("fp-contract=off")

/**
 * colblitz_v10_bdp_cb_ct.cpp — Beam-DP / Capped-Branching / Cross-Terms strategy.
 *
 * Extends colblitz_v8_heuristics_stateless.cpp with two enhancements:
 *
 *   1. Cross-Terms scorer (CB / CT):
 *      The V8 7-term heuristic (T_BLUE, T_INFO6, T_HFULL, T_EV, T_GINI,
 *      T_VAR_SP, T_RARE_ID) is extended with three pairwise cross-product terms:
 *        T_BLUE_X_HFULL   (index 7): P(blue) × H_full/ln9
 *        T_BLUE_X_RARE_ID (index 8): P(blue) × P(rare_id)
 *        T_BLUE_X_EV      (index 9): P(blue) × EV_norm
 *      Weight vector expands from 140 → 200 floats per n_colors, laid out as
 *      w[d * N_BLUES * N_TERMS_X + b * N_TERMS_X + t]  (d∈[0,4], b∈[0,3], t∈[0,9]).
 *      Weights are loaded from the most-recent data/trace_v10_weights_*.json.
 *      A 140-weight (base 7-term) file is also accepted and remapped into the
 *      200-slot layout with cross-term slots zero-filled (identical to V8 scorer).
 *
 *   2. Beam-DP / Capped-Branching policy (BDP):
 *      For 6-color (n_rare=2) and 7-color (n_rare=3) games a precomputed
 *      ShallowDP policy is loaded and consulted on every next_click call
 *      before falling back to the cross-terms scorer.
 *
 *      Policy file format (v10s — pre-sorted flat binary):
 *        4-byte LE uint32 entry count N
 *        N × 31-byte entries:
 *          uint8_t revealed_color[25]  — 0=unclicked, 1=teal, 2=green,
 *                                        3=yellow, 4=spO, 5+k=var-rare-slot-k
 *          uint8_t blues_counted       — saturating count (0..4)
 *          uint8_t best_cell           — cell index 0..24
 *          float   ev_sp               — not used at runtime
 *        Entries are sorted lexicographically by (revealed_color[25], blues_counted).
 *        Lookup: build the 26-byte key from board state → binary search.
 *
 *      Memory strategy: if /proc/meminfo MemAvailable ≥ required + 4 GB safety
 *      margin the policy is decompressed into a std::vector<SdpEntry> (27 bytes/
 *      entry).  Otherwise the entries are written to a .bin sidecar file and
 *      mmap'd (OS pages in only the accessed 27-byte entries on demand).
 *
 * Fallback hierarchy (Phase 1, ships_hit < 5):
 *   1. CP pre-filter: if a guaranteed-blue cell exists, click it (free reveal).
 *   2. BDP policy hit (6/7-color only): if observed_colors key found in policy
 *      and the cell is still unclicked, use that cell.
 *   3. Cross-terms scorer: pickPhase1CellV10Idx with the 200-weight array.
 *      If only a 140-weight file is loaded the scorer behaves identically to V8.
 *   4. Phase-D fallback: if no weights are loaded at all.
 *
 * Phase 2 (ships_hit ≥ 5): SafeP2 (argmin P(blue)).
 *
 * TWO EXECUTION PATHS (identical to V8 stateless)
 * ------------------------------------------------
 *   Path A — sv-passing (strategy_next_click_sv, tree-walk fast path).
 *   Path B — delta cache (strategy_next_click, sequential evaluator fallback).
 *
 * Process-wide BoardCache / std::call_once (identical to V8 stateless):
 *   Board files, weights, and SDP policies are loaded exactly once across all
 *   thread instances.  Subsequent init_evaluation_run calls cost near-zero.
 *
 * External data files:
 *   data/trace_v10_weights_YYYYMMDD_HHMMSS.json  — cross-terms weights (200-weight)
 *                                                   (most-recent file is selected)
 *   data/trace_shallow_dp_v10s_2_t50_b5_20260506_014421.bin.lzma
 *       — 6-color BDP policy (~2 GB compressed, 774 M entries, ~20 GB RAM)
 *   data/trace_shallow_dp_v10s_3_t200_b4_20260504_142925.bin.lzma
 *       — 7-color BDP policy (~257 MB compressed, 89 M entries, ~2.4 GB RAM)
 *   data/sphere_trace_boards_{2..5}.bin.lzma  — board files (shared with V8)
 */

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <numeric>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "../../interface/strategy.h"

using namespace sphere;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr int N_CELLS         = 25;
static constexpr int GRID             = 5;
static constexpr int COL_TEAL        = 0;
static constexpr int COL_GREEN       = 1;
static constexpr int COL_YELLOW      = 2;
static constexpr int COL_RARE_START  = 3;  // spO column; var-rare begin at +1

// V8 base terms (7)
static constexpr int V8_N_DEPTHS  = 5;
static constexpr int V8_N_BLUES   = 4;
static constexpr int V8_N_TERMS   = 7;
static constexpr int V8_N_WEIGHTS = V8_N_DEPTHS * V8_N_BLUES * V8_N_TERMS;  // 140

// V10 cross-terms extension (10 terms, 200 weights)
static constexpr int N_TERMS_X    = 10;  // base 7 + 3 cross-terms
static constexpr int N_WEIGHTS_CT = V8_N_DEPTHS * V8_N_BLUES * N_TERMS_X;   // 200

// Term indices
static constexpr int T_BLUE         = 0;
static constexpr int T_INFO6        = 1;
static constexpr int T_HFULL        = 2;
static constexpr int T_EV           = 3;
static constexpr int T_GINI         = 4;
static constexpr int T_VAR_SP       = 5;
static constexpr int T_RARE_ID      = 6;
static constexpr int T_BLUE_X_HFULL   = 7;
static constexpr int T_BLUE_X_RARE_ID = 8;
static constexpr int T_BLUE_X_EV      = 9;

static constexpr double V8_EV_DENOM  = 500.0;
static constexpr double V8_VAR_DENOM = 500.0 * 500.0;

// SP values for each detailed slot index:
//   0=blue (unused), 1=teal=20, 2=green=35, 3=yellow=55, 4=spO=90, 5+=var-rare (per-mode EV)
static constexpr double SLOT_SP_FIXED[5] = {0.0, 20.0, 35.0, 55.0, 90.0};

// SP values and per-n_colors appearance weights for variable-rare colors.
static constexpr double VAR_RARE_SP[4]            = {76.0, 104.0, 150.0, 500.0};
static constexpr double VAR_RARE_WEIGHT_BY_NC[4][4] = {
    {0.669734, 0.330266, 0.0,      0.0     },  // 6-color
    {0.818182, 0.607143, 0.415584, 0.159091},  // 7-color
    {0.906250, 0.875000, 0.750000, 0.468750},  // 8-color
    {1.0,      1.0,      1.0,      1.0     },  // 9-color
};

static double computeVarRareEV(int n_colors) {
    double wsum = 0.0, ev = 0.0;
    const double* w = VAR_RARE_WEIGHT_BY_NC[n_colors - 6];
    for (int i = 0; i < 4; ++i) { wsum += w[i]; ev += w[i] * VAR_RARE_SP[i]; }
    return wsum > 0.0 ? ev / wsum : 0.0;
}

static constexpr double LN6 = 1.791759469228327;
static constexpr double LN9 = 2.1972245773362196;

static constexpr int SHIPS_HIT_THRESHOLD = 5;

// Phase-D per-depth weights for (w_info6, w_hfull), indexed by [n_rare-2][depth]
static constexpr double PHASE_D_WEIGHTS[4][5][2] = {
    {{0.50, 0.30}, {0.50, 0.20}, {0.50, 0.30}, {0.60, 0.40}, {0.40, 0.30}},
    {{0.40, 0.00}, {0.40, 0.60}, {0.00, 0.70}, {0.00, 1.00}, {0.00, 0.90}},
    {{0.00, 0.00}, {0.30, 0.80}, {0.00, 0.80}, {0.00, 0.90}, {0.00, 1.00}},
    {{0.20, 0.00}, {0.10, 1.00}, {0.00, 0.50}, {0.00, 1.00}, {0.00, 1.00}},
};

// 4-connected adjacency bitmasks for all 25 cells
static uint32_t buildAdjMask(int cell) {
    int r = cell / GRID, c = cell % GRID;
    uint32_t m = 0;
    if (r > 0) m |= 1u << ((r-1)*GRID + c);
    if (r < 4) m |= 1u << ((r+1)*GRID + c);
    if (c > 0) m |= 1u << (r*GRID + (c-1));
    if (c < 4) m |= 1u << (r*GRID + (c+1));
    return m;
}
static uint32_t ADJ_MASK[N_CELLS];
struct AdjMaskInit {
    AdjMaskInit() { for (int i = 0; i < N_CELLS; ++i) ADJ_MASK[i] = buildAdjMask(i); }
};
static AdjMaskInit _adjInit;

// ---------------------------------------------------------------------------
// Board storage
// ---------------------------------------------------------------------------

struct BoardSet {
    std::vector<int32_t> data;
    int n    = 0;
    int fields = 0;
};

// ---------------------------------------------------------------------------
// Minimal lzma decompression via xzcat
// ---------------------------------------------------------------------------

static bool loadLzma(const std::string& path, std::vector<int32_t>& out, int fields) {
    std::string cmd = "xzcat \"" + path + "\"";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return false;
    static constexpr size_t CHUNK = 65536;
    std::vector<uint8_t> raw;
    raw.reserve(1 << 22);
    uint8_t buf[CHUNK];
    size_t n;
    while ((n = fread(buf, 1, CHUNK, fp)) > 0)
        raw.insert(raw.end(), buf, buf + n);
    pclose(fp);
    if (raw.size() % sizeof(int32_t) != 0) return false;
    size_t nInts = raw.size() / sizeof(int32_t);
    if (fields > 0 && nInts % fields != 0) return false;
    out.resize(nInts);
    memcpy(out.data(), raw.data(), raw.size());
    return true;
}

// ---------------------------------------------------------------------------
// Repo-root path helper
// ---------------------------------------------------------------------------

#ifndef REPO_ROOT
#define REPO_ROOT "."
#endif

static std::string repoPath(const std::string& rel) {
    return std::string(REPO_ROOT) + "/" + rel;
}

// ---------------------------------------------------------------------------
// /proc/meminfo — available RAM in bytes (returns 0 on failure)
// ---------------------------------------------------------------------------

static uint64_t getAvailableRAMBytes() {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256];
    uint64_t avail_kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemAvailable:", 13) == 0) {
            avail_kb = (uint64_t)strtoull(line + 13, nullptr, 10);
            break;
        }
    }
    fclose(f);
    return avail_kb * 1024ULL;
}

// ---------------------------------------------------------------------------
// SDP policy entry — 27 bytes: 26-byte key + 1-byte best cell
//
// key[0..24] = revealed_color[cell]  (0=unclicked, 1=teal, 2=green,
//              3=yellow, 4=spO, 5+k=var-rare-slot-k, note: +1 offset from
//              cell_color_detailed which returns 0=blue, 1=teal etc.)
//              So "blue" → detailed=0 → key byte = 0+1 = 1.
//              "teal"  → detailed=1 → key byte = 2, etc.
//              Unclicked cells store 0 (not 1), so the offset is:
//                unclicked → 0
//                blue      → 1  (ship hit: teal=2, green=3, yellow=4, spO=5, var-k=6+k)
//              Wait — re-reading trace_v10.cpp:1700:
//                child_obs[cell] = (uint8_t)(color + 1);  // 1-indexed; 0=unclicked
//              where color is cell_color_detailed (0=blue, 1=teal, …, 4=spO, 5+k=var-k).
//              So: blue→1, teal→2, green→3, yellow→4, spO→5, var-slot-0→6, var-slot-1→7.
// key[25]    = blues_counted (saturating, 0..4)
// ---------------------------------------------------------------------------

struct SdpEntry {
    uint8_t key[26];
    uint8_t cell;
} __attribute__((packed));
static_assert(sizeof(SdpEntry) == 27, "SdpEntry must be 27 bytes");

// ---------------------------------------------------------------------------
// SdpPolicyStore — either in-RAM vector or mmap'd flat file
// ---------------------------------------------------------------------------

struct SdpPolicyStore {
    // in-RAM path
    std::vector<SdpEntry> vec;

    // mmap path
    void*       mmap_addr = nullptr;
    size_t      mmap_size = 0;
    int         mmap_fd   = -1;

    // common
    const SdpEntry* ptr = nullptr;
    size_t          n   = 0;
    bool            loaded = false;

    ~SdpPolicyStore() {
        if (mmap_addr && mmap_addr != MAP_FAILED)
            munmap(mmap_addr, mmap_size);
        if (mmap_fd >= 0)
            close(mmap_fd);
    }

    // Non-copyable, non-movable (pointers into this struct are shared)
    SdpPolicyStore() = default;
    SdpPolicyStore(const SdpPolicyStore&) = delete;
    SdpPolicyStore& operator=(const SdpPolicyStore&) = delete;
};

// Comparator for binary search (lexicographic over 26-byte key)
static inline bool sdpEntryLess(const SdpEntry& a, const SdpEntry& b) {
    return memcmp(a.key, b.key, 26) < 0;
}

// ---------------------------------------------------------------------------
// Load SDP policy from .lzma file into store.
//
// Policy file format: 4-byte LE uint32 N, then N×31-byte entries:
//   uint8_t revealed_color[25] + uint8_t blues_counted + uint8_t best_cell
//   + float ev_sp (4 bytes, discarded)
// Files named "v10s" are pre-sorted; others would need sorting (not expected here).
//
// Memory strategy: use RAM if MemAvailable ≥ required + 4 GB margin.
// Otherwise decompress to a .bin sidecar file (same path with .lzma stripped)
// and mmap it.
// ---------------------------------------------------------------------------

static bool loadSdpPolicy(const std::string& lzma_path, SdpPolicyStore& store) {
    fprintf(stderr, "[colblitz_v10] Loading SDP policy: %s\n", lzma_path.c_str());
    fflush(stderr);

    // Step 1: peek at entry count (requires full decompress; do it via xzcat)
    // Decompress into a raw byte buffer first to get the count.
    std::string cmd = "xzcat \"" + lzma_path + "\"";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) {
        fprintf(stderr, "[colblitz_v10] popen failed for: %s\n", lzma_path.c_str());
        return false;
    }

    // Read 4-byte header first to get N
    uint8_t hdr[4];
    if (fread(hdr, 1, 4, fp) != 4) {
        pclose(fp);
        fprintf(stderr, "[colblitz_v10] Failed to read header from: %s\n", lzma_path.c_str());
        return false;
    }
    uint32_t n_entries = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8)
                       | ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);

    static constexpr size_t ON_DISK_ENTRY = 31;   // key26 + cell1 + ev_sp4
    static constexpr size_t STORED_ENTRY  = 27;   // key26 + cell1 (ev_sp discarded)
    size_t required_bytes = (size_t)n_entries * STORED_ENTRY;

    // Decision: RAM vs mmap
    uint64_t avail = getAvailableRAMBytes();
    static constexpr uint64_t SAFETY_MARGIN = 4ULL * 1024 * 1024 * 1024;  // 4 GB
    bool use_ram = (avail >= required_bytes + SAFETY_MARGIN);

    fprintf(stderr, "[colblitz_v10]   %u entries, %.1f GB stored  |  MemAvailable=%.1f GB  |  %s\n",
            n_entries,
            (double)required_bytes / (1024.0*1024.0*1024.0),
            (double)avail / (1024.0*1024.0*1024.0),
            use_ram ? "loading into RAM" : "will mmap sidecar");
    fflush(stderr);

    if (use_ram) {
        // ---- Load into RAM ----
        store.vec.resize(n_entries);
        uint8_t entry_buf[ON_DISK_ENTRY];
        for (uint32_t i = 0; i < n_entries; ++i) {
            size_t nr = fread(entry_buf, 1, ON_DISK_ENTRY, fp);
            if (nr != ON_DISK_ENTRY) {
                pclose(fp);
                fprintf(stderr, "[colblitz_v10] Truncated policy file at entry %u\n", i);
                store.vec.clear();
                return false;
            }
            SdpEntry& e = store.vec[i];
            memcpy(e.key, entry_buf, 26);       // revealed_color[25] + blues_counted
            e.cell = entry_buf[26];              // best_cell (ev_sp at [27..30] discarded)
        }
        pclose(fp);
        store.ptr    = store.vec.data();
        store.n      = n_entries;
        store.loaded = true;
        fprintf(stderr, "[colblitz_v10]   Loaded %u entries into RAM (%.1f GB)\n",
                n_entries, (double)required_bytes / (1024.0*1024.0*1024.0));
        fflush(stderr);
        return true;

    } else {
        // ---- Decompress to sidecar .bin, then mmap ----

        // Derive sidecar path: strip ".lzma" suffix
        std::string sidecar = lzma_path;
        if (sidecar.size() >= 5 && sidecar.substr(sidecar.size()-5) == ".lzma")
            sidecar = sidecar.substr(0, sidecar.size()-5);
        sidecar += ".sdp27.bin";

        size_t sidecar_size = (size_t)n_entries * STORED_ENTRY;

        // Check if sidecar already exists and has the right size
        bool sidecar_ok = false;
        {
            struct stat st;
            if (stat(sidecar.c_str(), &st) == 0 && (size_t)st.st_size == sidecar_size)
                sidecar_ok = true;
        }

        if (!sidecar_ok) {
            fprintf(stderr, "[colblitz_v10]   Writing sidecar: %s (%.1f GB)\n",
                    sidecar.c_str(), (double)sidecar_size / (1024.0*1024.0*1024.0));
            fflush(stderr);

            FILE* out = fopen(sidecar.c_str(), "wb");
            if (!out) {
                pclose(fp);
                fprintf(stderr, "[colblitz_v10] Cannot open sidecar for write: %s: %s\n",
                        sidecar.c_str(), strerror(errno));
                return false;
            }

            uint8_t entry_buf[ON_DISK_ENTRY];
            SdpEntry stored;
            for (uint32_t i = 0; i < n_entries; ++i) {
                size_t nr = fread(entry_buf, 1, ON_DISK_ENTRY, fp);
                if (nr != ON_DISK_ENTRY) {
                    pclose(fp);
                    fclose(out);
                    remove(sidecar.c_str());
                    fprintf(stderr, "[colblitz_v10] Truncated policy at entry %u\n", i);
                    return false;
                }
                memcpy(stored.key, entry_buf, 26);
                stored.cell = entry_buf[26];
                fwrite(&stored, STORED_ENTRY, 1, out);
            }
            fclose(out);
            fprintf(stderr, "[colblitz_v10]   Sidecar written.\n");
            fflush(stderr);
        } else {
            // Sidecar exists — drain the popen pipe we opened (need to close it cleanly)
            // Just discard remaining bytes
            uint8_t discard[4096];
            while (fread(discard, 1, sizeof(discard), fp) > 0) {}
            fprintf(stderr, "[colblitz_v10]   Sidecar already exists, reusing.\n");
            fflush(stderr);
        }
        pclose(fp);

        // mmap the sidecar
        int fd = open(sidecar.c_str(), O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "[colblitz_v10] Cannot open sidecar for mmap: %s: %s\n",
                    sidecar.c_str(), strerror(errno));
            return false;
        }

        void* addr = mmap(nullptr, sidecar_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (addr == MAP_FAILED) {
            close(fd);
            fprintf(stderr, "[colblitz_v10] mmap failed: %s\n", strerror(errno));
            return false;
        }

        // Advise sequential then random (binary search pattern)
        madvise(addr, sidecar_size, MADV_RANDOM);

        store.mmap_addr = addr;
        store.mmap_size = sidecar_size;
        store.mmap_fd   = fd;
        store.ptr       = static_cast<const SdpEntry*>(addr);
        store.n         = n_entries;
        store.loaded    = true;
        fprintf(stderr, "[colblitz_v10]   mmap'd sidecar: %u entries (%.1f GB)\n",
                n_entries, (double)sidecar_size / (1024.0*1024.0*1024.0));
        fflush(stderr);
        return true;
    }
}

// ---------------------------------------------------------------------------
// SDP lookup: binary search over sorted SdpEntry array.
// Returns cell index (0..24) on hit, -1 on miss.
// ---------------------------------------------------------------------------

static int sdpLookup(const SdpPolicyStore& store,
                     const uint8_t observed_colors[N_CELLS],
                     int blues_counted)
{
    if (!store.loaded || store.n == 0) return -1;

    // Build probe key
    SdpEntry probe;
    memcpy(probe.key, observed_colors, N_CELLS);
    probe.key[N_CELLS] = (uint8_t)blues_counted;
    probe.cell = 0;

    // Binary search over the sorted array
    const SdpEntry* begin = store.ptr;
    const SdpEntry* end   = store.ptr + store.n;
    const SdpEntry* it    = std::lower_bound(begin, end, probe,
        [](const SdpEntry& a, const SdpEntry& b) {
            return memcmp(a.key, b.key, 26) < 0;
        });
    if (it != end && memcmp(it->key, probe.key, 26) == 0)
        return (int)it->cell;
    return -1;
}

// ---------------------------------------------------------------------------
// V10 weights file discovery and parsing
// ---------------------------------------------------------------------------

// Find the most recent data/trace_v10_weights_*.json.
// Returns empty string if not found (caller should error out).
static std::string findLatestV10WeightsFile() {
    std::string data_dir = repoPath("data");
    DIR* d = opendir(data_dir.c_str());
    if (!d) return "";
    std::string best;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        const char* name = ent->d_name;
        size_t len = strlen(name);
        if (len < 5 || strcmp(name + len - 5, ".json") != 0) continue;
        if (strncmp(name, "trace_v10_weights_", 18) == 0) {
            std::string full = data_dir + "/" + name;
            if (full > best) best = full;
        }
    }
    closedir(d);
    return best;
}

// Parse the "weights" array for n_colors from a v10 weights JSON file.
// Accepts both 140-weight (base 7-term) and 200-weight (cross-terms) files.
// Remaps 140-weight flat layout [d*28+b*7+t] to extended [d*40+b*10+t].
// Returns true on success; sets cross_terms_out to true only for 200-weight files.
static bool extractWeightsV10(const std::string& json, int n_colors,
                               std::array<double, N_WEIGHTS_CT>& out,
                               bool& cross_terms_out)
{
    cross_terms_out = false;
    out.fill(0.0);

    std::string key = "\"" + std::to_string(n_colors) + "\"";
    const char* p = strstr(json.c_str(), key.c_str());
    if (!p) return false;
    p += key.size();
    p = strstr(p, "\"weights\"");
    if (!p) return false;
    p = strchr(p, '[');
    if (!p) return false;
    ++p;

    // Read up to N_WEIGHTS_CT values
    double tmp[N_WEIGHTS_CT] = {};
    int n_read = 0;
    for (int i = 0; i < N_WEIGHTS_CT; ++i) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',') ++p;
        if (*p == ']') break;
        char* end;
        double v = strtod(p, &end);
        if (end == p) return false;
        tmp[n_read++] = v;
        p = end;
    }
    if (n_read == 0) return false;

    if (n_read == V8_N_WEIGHTS) {
        // 140-weight base file: remap d*28+b*7+t → d*40+b*10+t (cross-term slots = 0)
        for (int fi = 0; fi < V8_N_WEIGHTS; ++fi) {
            int d = fi / (V8_N_BLUES * V8_N_TERMS);
            int b = (fi / V8_N_TERMS) % V8_N_BLUES;
            int t = fi % V8_N_TERMS;
            out[d * (V8_N_BLUES * N_TERMS_X) + b * N_TERMS_X + t] = tmp[fi];
        }
        cross_terms_out = false;
    } else if (n_read == N_WEIGHTS_CT) {
        // 200-weight cross-terms file: copy directly
        for (int i = 0; i < N_WEIGHTS_CT; ++i) out[i] = tmp[i];
        cross_terms_out = true;
    } else {
        // Unknown size — try to use whatever we got (partial fill)
        int copy_n = std::min(n_read, N_WEIGHTS_CT);
        for (int i = 0; i < copy_n; ++i) out[i] = tmp[i];
        cross_terms_out = (n_read > V8_N_WEIGHTS);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Index-based filtering (unchanged from V8)
// ---------------------------------------------------------------------------

static void filterBlueIdx(const BoardSet& fbs, std::vector<int>& sv, int32_t bit) {
    const int32_t* data   = fbs.data.data();
    int            fields = fbs.fields;
    int out = 0;
    for (int i = 0, n = (int)sv.size(); i < n; ++i) {
        const int32_t* rowp = data + sv[i] * fields;
        int32_t occ = 0;
        for (int c = 0; c < fields; ++c) occ |= rowp[c];
        if ((occ & bit) == 0) sv[out++] = sv[i];
    }
    sv.resize(out);
}

static void filterFixedIdx(const BoardSet& fbs, std::vector<int>& sv, int32_t bit, int col) {
    const int32_t* data   = fbs.data.data();
    int            fields = fbs.fields;
    int out = 0;
    for (int i = 0, n = (int)sv.size(); i < n; ++i) {
        if (data[sv[i] * fields + col] & bit) sv[out++] = sv[i];
    }
    sv.resize(out);
}

static void filterVarRareIdx(
    const BoardSet& fbs, std::vector<int>& sv, int32_t bit,
    const std::string& color,
    std::unordered_map<std::string, int32_t>& rareColorGroups,
    int n_rare)
{
    int n_var_rare = n_rare - 1;
    if (n_var_rare <= 0) { filterFixedIdx(fbs, sv, bit, COL_RARE_START); return; }
    int var_start  = COL_RARE_START + 1;
    const int32_t* data   = fbs.data.data();
    int            fields = fbs.fields;

    {
        int out = 0;
        for (int i = 0, n = (int)sv.size(); i < n; ++i) {
            const int32_t* rowp = data + sv[i] * fields;
            bool found = false;
            for (int k = 0; k < n_var_rare; ++k)
                if (rowp[var_start + k] & bit) { found = true; break; }
            if (found) sv[out++] = sv[i];
        }
        sv.resize(out);
    }
    if (sv.empty()) return;

    int32_t existing = 0;
    auto it = rareColorGroups.find(color);
    if (it != rareColorGroups.end()) existing = it->second;
    rareColorGroups[color] = existing | bit;

    std::vector<int32_t> group_bits;
    group_bits.reserve(rareColorGroups.size());
    for (const auto& kv : rareColorGroups) group_bits.push_back(kv.second);
    int n_groups = (int)group_bits.size();

    std::function<bool(int, uint32_t, const int32_t*)> hasPerm;
    hasPerm = [&](int g, uint32_t used_mask, const int32_t* rowp) -> bool {
        if (g == n_groups) return true;
        for (int k = 0; k < n_var_rare; ++k) {
            if (used_mask & (1u << k)) continue;
            int32_t colval = rowp[var_start + k];
            if ((colval & group_bits[g]) == group_bits[g])
                if (hasPerm(g + 1, used_mask | (1u << k), rowp)) return true;
        }
        return false;
    };

    int out = 0;
    for (int i = 0, n = (int)sv.size(); i < n; ++i) {
        const int32_t* rowp = data + sv[i] * fields;
        if (hasPerm(0, 0, rowp)) sv[out++] = sv[i];
    }
    sv.resize(out);
}

static void applyRevealIdx(const BoardSet& fbs,
                            std::vector<int>& sv,
                            std::unordered_map<std::string, int32_t>& rareColorGroups,
                            int cell_idx, const std::string& color, int n_rare)
{
    if (sv.empty()) return;
    int32_t bit = (int32_t)(1 << cell_idx);
    if      (color == "spB") filterBlueIdx (fbs, sv, bit);
    else if (color == "spT") filterFixedIdx(fbs, sv, bit, COL_TEAL);
    else if (color == "spG") filterFixedIdx(fbs, sv, bit, COL_GREEN);
    else if (color == "spY") filterFixedIdx(fbs, sv, bit, COL_YELLOW);
    else if (color == "spO") filterFixedIdx(fbs, sv, bit, COL_RARE_START);
    else if (color == "spL" || color == "spD" || color == "spR" || color == "spW")
        filterVarRareIdx(fbs, sv, bit, color, rareColorGroups, n_rare);
    // "spU" (still covered) is ignored
}

// ---------------------------------------------------------------------------
// Outcome counts — index-based (unchanged from V8)
// ---------------------------------------------------------------------------

static void computeOutcomeCountsBothIdx(
    const BoardSet& fbs,
    const std::vector<int>& sv,
    const std::vector<int32_t>& occ_sv,
    const std::vector<int>& unclicked,
    std::vector<int>& counts_dc,
    std::vector<int>& counts6,
    int& slot_stride)
{
    int nu     = (int)unclicked.size();
    int n      = (int)sv.size();
    int fields = fbs.fields;
    slot_stride = 1 + fields;

    counts_dc.assign(nu * slot_stride, 0);
    counts6.assign(nu * 6, 0);

    const int32_t* data = fbs.data.data();

    for (int ii = 0; ii < nu; ++ii) {
        int cell    = unclicked[ii];
        int32_t bit = (int32_t)(1 << cell);
        int* dc     = &counts_dc[ii * slot_stride];
        int* c6     = &counts6[ii * 6];

        for (int i = 0; i < n; ++i) {
            const int32_t* rowp = data + sv[i] * fields;
            if ((occ_sv[i] & bit) == 0) { ++dc[0]; continue; }
            for (int col = 0; col < fields; ++col) {
                if (rowp[col] & bit) { ++dc[1 + col]; break; }
            }
        }

        c6[0] = dc[0];
        c6[1] = dc[1 + COL_TEAL];
        c6[2] = dc[1 + COL_GREEN];
        c6[3] = dc[1 + COL_YELLOW];
        c6[4] = dc[1 + COL_RARE_START];
        int var_sum = 0;
        for (int k = COL_RARE_START + 1; k < fields; ++k) var_sum += dc[1 + k];
        c6[5] = var_sum;
    }
}

// ---------------------------------------------------------------------------
// Score terms (unchanged from V8)
// ---------------------------------------------------------------------------

static inline double termInfo6(const int* c6, int n) {
    if (n == 0) return 0.0;
    double inv = 1.0 / n, s = 0.0;
    for (int i = 0; i < 6; ++i)
        if (c6[i] > 0) { double p = c6[i] * inv; s -= p * std::log(p); }
    return s / LN6;
}

static inline double termHfull(const int* cdc, int slots, int n) {
    if (n == 0) return 0.0;
    double inv = 1.0 / n, s = 0.0;
    for (int i = 0; i < slots; ++i)
        if (cdc[i] > 0) { double p = cdc[i] * inv; s -= p * std::log(p); }
    return s / LN9;
}

static inline double termEvNorm(const int* cdc, int slots, int n,
                                const std::vector<double>& slot_sp) {
    if (n == 0) return 0.0;
    double inv = 1.0 / n, ev = 0.0;
    int lim = std::min(slots, (int)slot_sp.size());
    for (int i = 1; i < lim; ++i)
        if (cdc[i] > 0) ev += cdc[i] * inv * slot_sp[i];
    return ev / V8_EV_DENOM;
}

static inline double termGini(const int* cdc, int slots, int n) {
    if (n == 0) return 0.0;
    double inv = 1.0 / n, sq = 0.0;
    for (int i = 0; i < slots; ++i)
        if (cdc[i] > 0) { double p = cdc[i] * inv; sq += p * p; }
    return 1.0 - sq;
}

static inline double termVarSp(const int* cdc, int slots, int n,
                                const std::vector<double>& slot_sp) {
    if (n == 0) return 0.0;
    double inv = 1.0 / n, ev = 0.0, ev2 = 0.0;
    int lim = std::min(slots, (int)slot_sp.size());
    for (int i = 1; i < lim; ++i) {
        if (cdc[i] > 0) {
            double sp = slot_sp[i], p = cdc[i] * inv;
            ev += p * sp; ev2 += p * sp * sp;
        }
    }
    return (ev2 - ev * ev) / V8_VAR_DENOM;
}

static inline double termRareId(const int* cdc, int slots, int n,
                                 const std::vector<bool>& identified, int n_var) {
    if (n == 0) return 0.0;
    double inv = 1.0 / n;
    int total = 0;
    for (int k = 0; k < n_var; ++k) {
        if (!identified[k]) {
            int idx = 5 + k;
            if (idx < slots) total += cdc[idx];
        }
    }
    return total * inv;
}

// ---------------------------------------------------------------------------
// Identified slots (unchanged from V8)
// ---------------------------------------------------------------------------

static std::vector<bool> getIdentifiedSlotsIdx(
    const BoardSet& fbs,
    const std::vector<int>& sv,
    const std::unordered_map<std::string, int32_t>& rareColorGroups,
    int n_rare)
{
    int n_var = n_rare - 1;
    std::vector<bool> identified(n_var, false);
    if (n_var <= 0 || rareColorGroups.empty() || sv.empty()) return identified;
    int var_start   = COL_RARE_START + 1;
    const int32_t* data   = fbs.data.data();
    int            fields = fbs.fields;
    int            n      = (int)sv.size();
    for (int k = 0; k < n_var; ++k) {
        int col = var_start + k;
        for (const auto& kv : rareColorGroups) {
            int32_t bits = kv.second;
            bool all_match = true, any_found = false;
            for (int i = 0; i < n; ++i) {
                int32_t colval = data[sv[i] * fields + col];
                bool has_any = (colval & bits) != 0;
                bool has_all = (colval & bits) == bits;
                if (has_any) any_found = true;
                if (has_any != has_all) { all_match = false; break; }
            }
            if (all_match && any_found) { identified[k] = true; break; }
        }
    }
    return identified;
}

static std::vector<double> buildSlotSp(int n_rare, int n_colors) {
    std::vector<double> sp(SLOT_SP_FIXED, SLOT_SP_FIXED + 5);
    double ev = computeVarRareEV(n_colors);
    for (int k = 0; k < n_rare - 1; ++k) sp.push_back(ev);
    return sp;
}

// ---------------------------------------------------------------------------
// Compute occ bitmask for each surviving row (unchanged from V8)
// ---------------------------------------------------------------------------

static void computeOccIdx(const BoardSet& fbs, const std::vector<int>& sv,
                           std::vector<int32_t>& occ_sv) {
    int            fields = fbs.fields;
    const int32_t* data   = fbs.data.data();
    int            n      = (int)sv.size();
    occ_sv.resize(n);
    for (int i = 0; i < n; ++i) {
        const int32_t* rowp = data + sv[i] * fields;
        int32_t o = 0;
        for (int c = 0; c < fields; ++c) o |= rowp[c];
        occ_sv[i] = o;
    }
}

// ---------------------------------------------------------------------------
// Build observed_colors[N_CELLS] for SDP policy lookup.
//
// Encoding (matching trace_v10.cpp tree_walk_sdp):
//   unclicked → 0
//   blue      → 1  (cell_color_detailed returns 0 for blue → +1 = 1)
//   teal      → 2  (ccd=1 → +1 = 2)
//   green     → 3
//   yellow    → 4
//   spO       → 5
//   var-rare-slot-k → 6 + k
//
// For variable-rare cells, we determine the slot index k by checking which
// column in the first surviving board contains the bit for that cell.
// (After filtering, all surviving boards agree on the slot assignment.)
// ---------------------------------------------------------------------------

static void buildObservedColors(
    const BoardSet& fbs,
    const std::vector<int>& sv,
    const std::vector<std::pair<int,std::string>>& reveals,
    int n_rare,
    uint8_t obs[N_CELLS])
{
    memset(obs, 0, N_CELLS);

    int n_var_rare = n_rare - 1;
    const int32_t* data   = fbs.data.data();
    int            fields = fbs.fields;

    // Use the first surviving board to resolve var-rare slot indices.
    // Safe only after filtering; caller guarantees sv non-empty when calling this.
    int ref_board = sv.empty() ? 0 : sv[0];
    const int32_t* ref_row = data + ref_board * fields;

    for (const auto& [cidx, color] : reveals) {
        int32_t bit = (int32_t)(1 << cidx);
        uint8_t code;
        if      (color == "spB") code = 1;
        else if (color == "spT") code = 2;
        else if (color == "spG") code = 3;
        else if (color == "spY") code = 4;
        else if (color == "spO") code = 5;
        else {
            // Variable-rare: determine slot k from reference board
            code = 6;  // fallback: slot 0
            for (int k = 0; k < n_var_rare; ++k) {
                if (ref_row[COL_RARE_START + 1 + k] & bit) {
                    code = (uint8_t)(6 + k);
                    break;
                }
            }
        }
        obs[cidx] = code;
    }
}

// ---------------------------------------------------------------------------
// CP pre-filter (unchanged from V8)
// ---------------------------------------------------------------------------

static int cpPrefilter(
    const std::vector<int>& unclicked,
    const std::vector<int>& counts6,
    int n_boards)
{
    if (n_boards == 0) return -1;
    int nu = (int)unclicked.size();
    double inv = 1.0 / n_boards;
    int cellToIdx[N_CELLS];
    memset(cellToIdx, -1, sizeof(cellToIdx));
    for (int ii = 0; ii < nu; ++ii) cellToIdx[unclicked[ii]] = ii;
    int best_cell = -1;
    double best_adj = -1.0;
    for (int ii = 0; ii < nu; ++ii) {
        const int* c6 = &counts6[ii * 6];
        int n_ship = c6[1] + c6[2] + c6[3] + c6[4] + c6[5];
        if (n_ship != 0) continue;
        int cell = unclicked[ii];
        uint32_t adj = ADJ_MASK[cell];
        double adj_val = 0.0;
        while (adj) {
            int nb = __builtin_ctz(adj); adj &= adj - 1;
            int nbi = cellToIdx[nb];
            if (nbi >= 0) {
                const int* nc6 = &counts6[nbi * 6];
                adj_val += (nc6[1] + nc6[2] + nc6[3] + nc6[4] + nc6[5]) * inv;
            }
        }
        if (best_cell < 0 || adj_val > best_adj) { best_adj = adj_val; best_cell = cell; }
    }
    return best_cell;
}

// ---------------------------------------------------------------------------
// Phase 1 cell picker — V10 cross-terms scorer
//
// Identical to V8's pickPhase1CellV8Idx except:
//   - Weights array is 200-element (stride N_TERMS_X=10 instead of 7)
//   - Computes 3 cross-term products when the corresponding weights are non-zero
// ---------------------------------------------------------------------------

static int pickPhase1CellV10Idx(
    const BoardSet& fbs,
    const std::vector<int>& sv,
    const std::vector<int>& unclicked,
    int n_rare, int n_colors, int ships_hit, int blues_used,
    const std::unordered_map<std::string, int32_t>& rareColorGroups,
    const std::array<double, N_WEIGHTS_CT>& weights)
{
    if (unclicked.empty()) return -1;
    int n = (int)sv.size();

    std::vector<int32_t> occ_sv;
    computeOccIdx(fbs, sv, occ_sv);

    std::vector<int> counts_dc, counts6;
    int slot_stride = 0;
    computeOutcomeCountsBothIdx(fbs, sv, occ_sv, unclicked, counts_dc, counts6, slot_stride);

    int forced = cpPrefilter(unclicked, counts6, n);
    if (forced >= 0) return forced;

    int d    = std::min(ships_hit, V8_N_DEPTHS - 1);
    int b    = std::min(blues_used, V8_N_BLUES - 1);
    int base = d * (V8_N_BLUES * N_TERMS_X) + b * N_TERMS_X;

    double w_blue        = weights[base + T_BLUE];
    double w_info6       = weights[base + T_INFO6];
    double w_hfull       = weights[base + T_HFULL];
    double w_ev          = weights[base + T_EV];
    double w_gini        = weights[base + T_GINI];
    double w_var_sp      = weights[base + T_VAR_SP];
    double w_rare_id     = weights[base + T_RARE_ID];
    double w_blue_hfull  = weights[base + T_BLUE_X_HFULL];
    double w_blue_rid    = weights[base + T_BLUE_X_RARE_ID];
    double w_blue_ev     = weights[base + T_BLUE_X_EV];

    bool need_slot_sp  = (std::abs(w_ev)     > 1e-15 || std::abs(w_var_sp)  > 1e-15 ||
                          std::abs(w_rare_id) > 1e-15 || std::abs(w_blue_rid) > 1e-15 ||
                          std::abs(w_blue_ev) > 1e-15);
    bool need_cross    = (std::abs(w_blue_hfull) > 1e-15 ||
                          std::abs(w_blue_rid)   > 1e-15 ||
                          std::abs(w_blue_ev)    > 1e-15);

    int n_var = n_rare - 1;
    std::vector<double> slot_sp;
    std::vector<bool>   identified;
    if (need_slot_sp) {
        identified = getIdentifiedSlotsIdx(fbs, sv, rareColorGroups, n_rare);
        slot_sp    = buildSlotSp(n_rare, n_colors);
    } else {
        identified.assign(n_var, false);
    }

    double inv_n = (n > 0) ? 1.0 / n : 0.0;
    int best = unclicked[0]; double best_s = -1e18;
    int nu = (int)unclicked.size();
    for (int ii = 0; ii < nu; ++ii) {
        const int* c6  = &counts6[ii * 6];
        const int* cdc = &counts_dc[ii * slot_stride];

        // Base terms
        double t_blue    = (std::abs(w_blue)    > 1e-15 || need_cross) ? c6[0] * inv_n : 0.0;
        double t_info6   = (std::abs(w_info6)   > 1e-15) ? termInfo6(c6, n) : 0.0;
        double t_hfull   = (std::abs(w_hfull)   > 1e-15 || std::abs(w_blue_hfull) > 1e-15)
                               ? termHfull(cdc, slot_stride, n) : 0.0;
        double t_ev      = (std::abs(w_ev)      > 1e-15 || std::abs(w_blue_ev) > 1e-15)
                               && !slot_sp.empty() ? termEvNorm(cdc, slot_stride, n, slot_sp) : 0.0;
        double t_gini    = (std::abs(w_gini)    > 1e-15) ? termGini(cdc, slot_stride, n) : 0.0;
        double t_var_sp  = (std::abs(w_var_sp)  > 1e-15) && !slot_sp.empty()
                               ? termVarSp(cdc, slot_stride, n, slot_sp) : 0.0;
        double t_rare_id = (std::abs(w_rare_id) > 1e-15 || std::abs(w_blue_rid) > 1e-15)
                               ? termRareId(cdc, slot_stride, n, identified, n_var) : 0.0;

        double score = w_blue    * t_blue
                     + w_info6   * t_info6
                     + w_hfull   * t_hfull
                     + w_ev      * t_ev
                     + w_gini    * t_gini
                     + w_var_sp  * t_var_sp
                     + w_rare_id * t_rare_id;

        // Cross-term products
        if (need_cross) {
            score += w_blue_hfull * (t_blue * t_hfull);
            score += w_blue_rid   * (t_blue * t_rare_id);
            score += w_blue_ev    * (t_blue * t_ev);
        }

        if (score > best_s) { best_s = score; best = unclicked[ii]; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Phase 1 cell picker — Phase-D fallback (unchanged from V8)
// ---------------------------------------------------------------------------

static int pickPhase1CellPhaseDIdx(
    const BoardSet& fbs,
    const std::vector<int>& sv,
    const std::vector<int>& unclicked,
    int n_rare, int ships_hit)
{
    if (unclicked.empty()) return -1;
    int n = (int)sv.size();

    std::vector<int32_t> occ_sv;
    computeOccIdx(fbs, sv, occ_sv);

    std::vector<int> counts_dc, counts6;
    int slot_stride = 0;
    computeOutcomeCountsBothIdx(fbs, sv, occ_sv, unclicked, counts_dc, counts6, slot_stride);

    int forced = cpPrefilter(unclicked, counts6, n);
    if (forced >= 0) return forced;

    int nri = std::max(0, std::min(n_rare - 2, 3));
    int d   = std::max(0, std::min(ships_hit, 4));
    double w_info6 = PHASE_D_WEIGHTS[nri][d][0];
    double w_hfull = PHASE_D_WEIGHTS[nri][d][1];

    double inv_n = (n > 0) ? 1.0 / n : 0.0;
    int best = unclicked[0]; double best_s = -1e18;
    int nu = (int)unclicked.size();
    for (int ii = 0; ii < nu; ++ii) {
        const int* c6  = &counts6[ii * 6];
        const int* cdc = &counts_dc[ii * slot_stride];
        double score   = c6[0] * inv_n;
        if (w_info6 > 0.0) score += w_info6 * termInfo6(c6, n);
        if (w_hfull > 0.0) score += w_hfull * termHfull(cdc, slot_stride, n);
        if (score > best_s) { best_s = score; best = unclicked[ii]; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Phase 2 — SafeP2 (unchanged from V8)
// ---------------------------------------------------------------------------

static int pickSafeP2CellIdx(
    const BoardSet& fbs,
    const std::vector<int>& sv,
    const std::vector<int32_t>& occ_sv,
    const std::vector<int>& unclicked)
{
    if (unclicked.empty()) return -1;
    int n = (int)sv.size();
    if (n == 0) return unclicked[0];
    int best = unclicked[0], best_blue = n + 1;
    for (int cell : unclicked) {
        int32_t bit = (int32_t)(1 << cell);
        int n_blue = 0;
        for (int i = 0; i < n; ++i)
            if ((occ_sv[i] & bit) == 0) ++n_blue;
        if (n_blue < best_blue) { best_blue = n_blue; best = cell; }
    }
    return best;
}

static int jsonGetInt(const char* json, const char* key, int def = 0) {
    const char* p = strstr(json, key);
    if (!p) return def;
    p += strlen(key);
    while (*p == ' ' || *p == ':' || *p == '\t') ++p;
    return atoi(p);
}

// ---------------------------------------------------------------------------
// Process-wide board + weight + SDP policy cache.
// ---------------------------------------------------------------------------

struct BoardCache {
    // Board data (same as V8)
    BoardSet boards[4];
    bool     loaded[4] = {};

    // V10 cross-terms weights (200-slot arrays; 140-weight files remapped)
    std::array<double, N_WEIGHTS_CT> weights[4];
    bool                              weightsLoaded[4]      = {};
    bool                              weightsCrossTerms[4]  = {};

    // SDP policies — only for n_rare=2 (6-color) and n_rare=3 (7-color)
    // Indexed by n_rare - 2: sdp[0]=6c, sdp[1]=7c
    SdpPolicyStore sdp[2];
};

static BoardCache& g_board_cache() {
    static BoardCache cache;
    return cache;
}
static std::once_flag g_board_cache_flag;

static void load_board_cache() {
    BoardCache& c = g_board_cache();

    // ---- Load board files ----
    for (int n_colors = 6; n_colors <= 9; ++n_colors) {
        int n_rare = n_colors - 4;
        int idx    = n_rare - 2;
        int fields = 3 + n_rare;
        std::string path = repoPath(
            "data/sphere_trace_boards_" + std::to_string(n_rare) + ".bin.lzma");
        std::vector<int32_t> raw;
        if (loadLzma(path, raw, fields) && !raw.empty()) {
            c.boards[idx].data   = std::move(raw);
            c.boards[idx].n      = (int)(c.boards[idx].data.size() / fields);
            c.boards[idx].fields = fields;
            c.loaded[idx]        = true;
        }
    }

    // ---- Load V10 weights (most-recent trace_v10_weights_*.json, required) ----
    std::string wpath = findLatestV10WeightsFile();
    if (wpath.empty()) {
        fprintf(stderr,
            "[colblitz_v10] ERROR: no trace_v10_weights_*.json found in data/. "
            "Run generate_trace_weights.py --cross-terms to generate one.\n");
        fflush(stderr);
        // No weights — strategy will use Phase-D fallback for all games.
    } else {
        fprintf(stderr, "[colblitz_v10] Loading weights: %s\n", wpath.c_str());
        fflush(stderr);
        FILE* fp = fopen(wpath.c_str(), "r");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            rewind(fp);
            std::string json(sz, '\0');
            if (fread(&json[0], 1, sz, fp) == (size_t)sz) {
                for (int n_colors = 6; n_colors <= 9; ++n_colors) {
                    int idx = n_colors - 6;
                    bool ct = false;
                    if (extractWeightsV10(json, n_colors, c.weights[idx], ct)) {
                        c.weightsLoaded[idx]     = true;
                        c.weightsCrossTerms[idx] = ct;
                    }
                }
            }
            fclose(fp);
        }
    }

    // ---- Load SDP policies ----
    // 6-color policy (n_rare=2)
    {
        std::string path = repoPath(
            "data/trace_shallow_dp_v10s_2_t50_b5_20260506_014421.bin.lzma");
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
            loadSdpPolicy(path, c.sdp[0]);
        } else {
            fprintf(stderr, "[colblitz_v10] 6-color policy not found: %s\n", path.c_str());
            fflush(stderr);
        }
    }
    // 7-color policy (n_rare=3)
    {
        std::string path = repoPath(
            "data/trace_shallow_dp_v10s_3_t200_b4_20260504_142925.bin.lzma");
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
            loadSdpPolicy(path, c.sdp[1]);
        } else {
            fprintf(stderr, "[colblitz_v10] 7-color policy not found: %s\n", path.c_str());
            fflush(stderr);
        }
    }
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------

class ColblitzV10BdpCbCtOTStrategy : public OTStrategy {
public:
    // Run-level: non-owning views into the process-wide cache.
    const BoardSet*    fullBoards_[4]        = {};
    bool               boardsLoaded_[4]      = {};

    std::array<double, N_WEIGHTS_CT> weights_[4];
    bool               weightsLoaded_[4]     = {};
    bool               weightsCrossTerms_[4] = {};

    const SdpPolicyStore* sdp_[2]  = {};  // 0=6c, 1=7c
    bool                  sdpLoaded_[2] = {};

    // Per-instance incremental delta cache (Path B — sequential evaluator)
    bool     cache_valid_[4]    = {};
    uint32_t cache_revealed_[4] = {};
    std::unordered_map<std::string, int32_t> cache_rare_[4];
    std::vector<int> cache_sv_[4];

    // -----------------------------------------------------------------------
    // init_evaluation_run
    // -----------------------------------------------------------------------

    void init_evaluation_run() override {
        for (int i = 0; i < 4; ++i) {
            cache_valid_[i]    = false;
            cache_revealed_[i] = 0;
            cache_rare_[i].clear();
            cache_sv_[i].clear();
        }

        std::call_once(g_board_cache_flag, load_board_cache);

        const BoardCache& c = g_board_cache();
        for (int i = 0; i < 4; ++i) {
            fullBoards_[i]           = c.loaded[i] ? &c.boards[i] : nullptr;
            boardsLoaded_[i]         = c.loaded[i];
            if (c.weightsLoaded[i]) {
                weights_[i]          = c.weights[i];
                weightsLoaded_[i]    = true;
                weightsCrossTerms_[i] = c.weightsCrossTerms[i];
            }
        }
        for (int j = 0; j < 2; ++j) {
            sdp_[j]       = &c.sdp[j];
            sdpLoaded_[j] = c.sdp[j].loaded;
        }
    }

    // init_game_payload: no-op (all state rebuilt from board each call)

    // -----------------------------------------------------------------------
    // Shared decision logic
    //
    // Given the surviving board set (sv), unclicked list, rareColorGroups,
    // reveals, and game meta, return the chosen cell index.
    // -----------------------------------------------------------------------

    int chooseCell(
        const BoardSet& fbs, int bs_idx,
        const std::vector<int>& sv,
        const std::vector<int>& unclicked,
        const std::unordered_map<std::string, int32_t>& rareColorGroups,
        const std::vector<std::pair<int,std::string>>& reveals,
        int n_rare, int n_colors, int ships_hit, int blues_used) const
    {
        int n = (int)sv.size();
        if (n == 0) return unclicked.empty() ? 0 : unclicked[0];

        if (ships_hit >= SHIPS_HIT_THRESHOLD) {
            // Phase 2: SafeP2
            std::vector<int32_t> occ_sv;
            computeOccIdx(fbs, sv, occ_sv);
            return pickSafeP2CellIdx(fbs, sv, occ_sv, unclicked);
        }

        // Phase 1: compute outcome counts first (needed for CP pre-filter and scorer)
        // CP pre-filter is baked into the picker functions, but we need to check
        // the SDP policy before calling the full scorer.
        // So: check policy first (fast binary search), then fall through to scorer.

        // ---- SDP policy lookup (6/7-color only) ----
        int sdp_idx = n_rare - 2;
        if (sdp_idx >= 0 && sdp_idx < 2 && sdpLoaded_[sdp_idx] && sdp_[sdp_idx]) {
            uint8_t obs[N_CELLS] = {};
            buildObservedColors(fbs, sv, reveals, n_rare, obs);
            int policy_cell = sdpLookup(*sdp_[sdp_idx], obs, blues_used);
            if (policy_cell >= 0) {
                // Verify the cell is still unclicked
                for (int uc : unclicked) {
                    if (uc == policy_cell) return policy_cell;
                }
                // Policy cell already revealed — fall through to scorer
            }
        }

        // ---- V10 cross-terms scorer (or Phase-D fallback) ----
        if (weightsLoaded_[bs_idx]) {
            return pickPhase1CellV10Idx(fbs, sv, unclicked, n_rare, n_colors,
                                         ships_hit, blues_used,
                                         rareColorGroups, weights_[bs_idx]);
        } else {
            return pickPhase1CellPhaseDIdx(fbs, sv, unclicked, n_rare, ships_hit);
        }
    }

    // -----------------------------------------------------------------------
    // next_click — delta cache (Path B, sequential evaluator)
    // -----------------------------------------------------------------------

    void next_click(const std::vector<Cell>& board,
                    const std::string& meta_json,
                    ClickResult& out) override
    {
        int ships_hit  = jsonGetInt(meta_json.c_str(), "\"ships_hit\"",  0);
        int blues_used = jsonGetInt(meta_json.c_str(), "\"blues_used\"", 0);
        int n_colors   = jsonGetInt(meta_json.c_str(), "\"n_colors\"",   6);
        int n_rare     = n_colors - 4;
        int bs_idx     = n_rare - 2;

        std::vector<int> unclicked;
        unclicked.reserve(25);
        std::vector<std::pair<int, std::string>> reveals;
        reveals.reserve(25);
        uint32_t revealed_mask = 0;

        for (const Cell& c : board) {
            int idx = c.row * GRID + c.col;
            if (c.clicked) {
                revealed_mask |= (1u << idx);
                reveals.push_back({idx, c.color});
            } else {
                unclicked.push_back(idx);
            }
        }

        std::sort(reveals.begin(),   reveals.end(),
                  [](const auto& a, const auto& b){ return a.first < b.first; });
        std::sort(unclicked.begin(), unclicked.end());

        if (unclicked.empty()) { out.row = 0; out.col = 0; return; }

        if (bs_idx < 0 || bs_idx > 3 || !boardsLoaded_[bs_idx]) {
            out.row = unclicked[0] / GRID;
            out.col = unclicked[0] % GRID;
            return;
        }

        const BoardSet& fbs = *fullBoards_[bs_idx];

        std::vector<int> sv;
        std::unordered_map<std::string, int32_t> rareColorGroups;

        uint32_t cached = cache_valid_[bs_idx] ? cache_revealed_[bs_idx] : ~0u;
        bool is_superset = cache_valid_[bs_idx]
                        && ((cached & revealed_mask) == cached)
                        && (cached != revealed_mask);

        if (revealed_mask == 0) {
            sv.resize(fbs.n);
            std::iota(sv.begin(), sv.end(), 0);
        } else if (is_superset) {
            sv              = cache_sv_[bs_idx];
            rareColorGroups = cache_rare_[bs_idx];
            uint32_t delta_mask = revealed_mask & ~cached;
            for (const auto& [cidx, color] : reveals) {
                if (delta_mask & (1u << cidx))
                    applyRevealIdx(fbs, sv, rareColorGroups, cidx, color, n_rare);
            }
        } else {
            sv.resize(fbs.n);
            std::iota(sv.begin(), sv.end(), 0);
            for (const auto& [cidx, color] : reveals)
                applyRevealIdx(fbs, sv, rareColorGroups, cidx, color, n_rare);
        }

        cache_valid_[bs_idx]    = true;
        cache_revealed_[bs_idx] = revealed_mask;
        cache_rare_[bs_idx]     = rareColorGroups;
        cache_sv_[bs_idx]       = sv;

        if (sv.empty()) {
            out.row = unclicked[0] / GRID;
            out.col = unclicked[0] % GRID;
            return;
        }

        int chosen = chooseCell(fbs, bs_idx, sv, unclicked, rareColorGroups,
                                 reveals, n_rare, n_colors, ships_hit, blues_used);
        if (chosen < 0) chosen = unclicked[0];
        out.row = chosen / GRID;
        out.col = chosen % GRID;
    }

    // -----------------------------------------------------------------------
    // next_click_with_sv — sv-passing (Path A, tree-walk harness)
    // -----------------------------------------------------------------------

    void next_click_with_sv(const std::vector<Cell>& board,
                             const std::string& meta_json,
                             const int* sv_ptr, int sv_len,
                             ClickResult& out)
    {
        int ships_hit  = jsonGetInt(meta_json.c_str(), "\"ships_hit\"",  0);
        int blues_used = jsonGetInt(meta_json.c_str(), "\"blues_used\"", 0);
        int n_colors   = jsonGetInt(meta_json.c_str(), "\"n_colors\"",   6);
        int n_rare     = n_colors - 4;
        int bs_idx     = n_rare - 2;

        std::vector<int> unclicked;
        unclicked.reserve(25);
        std::vector<std::pair<int, std::string>> reveals;
        reveals.reserve(25);

        for (const Cell& c : board) {
            int idx = c.row * GRID + c.col;
            if (c.clicked) {
                reveals.push_back({idx, c.color});
            } else {
                unclicked.push_back(idx);
            }
        }

        std::sort(reveals.begin(), reveals.end(),
                  [](const auto& a, const auto& b){ return a.first < b.first; });
        std::sort(unclicked.begin(), unclicked.end());

        if (unclicked.empty()) { out.row = 0; out.col = 0; return; }

        if (bs_idx < 0 || bs_idx > 3 || !boardsLoaded_[bs_idx] || sv_len == 0) {
            out.row = unclicked[0] / GRID;
            out.col = unclicked[0] % GRID;
            return;
        }

        const BoardSet& fbs = *fullBoards_[bs_idx];

        // Use the harness-provided sv directly
        std::vector<int> sv(sv_ptr, sv_ptr + sv_len);

        // Reconstruct rareColorGroups for scoring terms
        std::unordered_map<std::string, int32_t> rareColorGroups;
        for (const auto& [cidx, color] : reveals) {
            if (color == "spL" || color == "spD" || color == "spR" || color == "spW") {
                int32_t bit = (int32_t)(1 << cidx);
                rareColorGroups[color] |= bit;
            }
        }

        int chosen = chooseCell(fbs, bs_idx, sv, unclicked, rareColorGroups,
                                 reveals, n_rare, n_colors, ships_hit, blues_used);
        if (chosen < 0) chosen = unclicked[0];
        out.row = chosen / GRID;
        out.col = chosen % GRID;
    }
};

// ---------------------------------------------------------------------------
// C exports required by the harness
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new ColblitzV10BdpCbCtOTStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* inst) {
    static_cast<ColblitzV10BdpCbCtOTStrategy*>(inst)->init_evaluation_run();
}

extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {
    static_cast<ColblitzV10BdpCbCtOTStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<ColblitzV10BdpCbCtOTStrategy*>(inst);
    std::vector<Cell> brd = parse_board_json(board_json ? board_json : "[]");
    ClickResult out;
    s->next_click(brd, meta_json ? meta_json : "{}", out);
    buf = "{\"row\":" + std::to_string(out.row) + ",\"col\":" + std::to_string(out.col) + "}";
    return buf.c_str();
}

extern "C" const char* strategy_next_click_sv(void* inst,
                                               const char* board_json,
                                               const char* meta_json,
                                               const int*  sv_ptr,
                                               int         sv_len)
{
    thread_local static std::string buf;
    auto* s = static_cast<ColblitzV10BdpCbCtOTStrategy*>(inst);
    std::vector<Cell> brd = parse_board_json(board_json ? board_json : "[]");
    ClickResult out;
    s->next_click_with_sv(brd, meta_json ? meta_json : "{}", sv_ptr, sv_len, out);
    buf = "{\"row\":" + std::to_string(out.row) + ",\"col\":" + std::to_string(out.col) + "}";
    return buf.c_str();
}
