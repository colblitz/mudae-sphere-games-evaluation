// sphere:stateless
// Upgrade strategy .so from the default -O2 to -O3 to enable
// auto-vectorization and additional inlining.
#pragma GCC optimize("O3")
// Disable FMA to match JS/Python floating-point rounding behaviour.
#pragma GCC optimize("fp-contract=off")

/**
 * colblitz_v11_bdp_cb_ct.cpp — Beam-DP / Capped-Branching / Cross-Terms strategy (V11).
 *
 * Extends colblitz_v10_bdp_cb_ct.cpp with a revised 9-term scorer:
 *
 *   1. Cross-Terms scorer (CB / CT) — V11 changes vs V10:
 *      - T_HFULL removed (was index 2 in V10; was redundant with T_INFO6
 *        after a var-rare symmetry fix in T_GINI).
 *      - T_BLUE_X_HFULL removed (was index 7 in V10).
 *      - T_BLUE_X_INFO6 added at index 6 (= P(blue) × T_INFO6).
 *      - T_GINI now uses a collapsed 6-bucket distribution (all var-rare
 *        slots merged into one bucket) to fix a rotational symmetry bug.
 *      - Weight stride changes from 10 to 9 per (d,b) block.
 *
 *      Full 9-term scorer:
 *        0: T_BLUE         = P(blue) for this cell
 *        1: T_INFO6        = Shannon entropy over 6-bucket distribution / ln(6)
 *        2: T_EV           = expected SP / 500
 *        3: T_GINI         = Gini impurity (6-bucket collapsed)
 *        4: T_VAR_SP       = variance of SP / 500²
 *        5: T_RARE_ID      = P(unidentified var-rare)
 *        6: T_BLUE_X_INFO6 = T_BLUE × T_INFO6  (new cross-term)
 *        7: T_BLUE_X_RID   = T_BLUE × T_RARE_ID
 *        8: T_BLUE_X_EV    = T_BLUE × T_EV
 *
 *      Weight vector: w[d * N_BLUES * N_TERMS_X + b * N_TERMS_X + t]
 *        (d∈[0,4], b∈[0,3], t∈[0,8]) — 180 floats per n_colors variant.
 *
 *      Backward-compat remapping:
 *        200-weight V10 file → drop old t=2 (T_HFULL) and t=7 (T_BLUE_X_HFULL),
 *          zero-fill T_BLUE_X_INFO6 slot (index 6).
 *        140-weight V8 file → drop old t=2 (T_HFULL), zero-fill all cross-terms.
 *
 *      Weights are loaded from the most-recent data/trace_v11_weights_*.json.
 *      Missing file is an error (no V10 fallback); Phase-D is used instead.
 *
 *   2. Beam-DP / Capped-Branching policy (BDP):
 *      For 6-color (n_rare=2) and 7-color (n_rare=3) games, auto-discovered
 *      V11 policy files are loaded:
 *        data/trace_shallow_dp_v11s_2_*.bin.lzma  (most-recent 6-color policy)
 *        data/trace_shallow_dp_v11s_3_*.bin.lzma  (most-recent 7-color policy)
 *      Same format as V10 (31-byte on-disk entries, 27-byte stored entries).
 *      Graceful skip if not found.
 *
 * Fallback hierarchy (Phase 1, ships_hit < 5):
 *   1. CP pre-filter: if a guaranteed-blue cell exists, click it (free reveal).
 *   2. BDP policy hit (6/7-color only): if observed_colors key found in policy
 *      and the cell is still unclicked, use that cell.
 *   3. V11 cross-terms scorer: pickPhase1CellV11Idx with the 180-weight array.
 *   4. Phase-D fallback: if no V11 weights are loaded.
 *
 * Phase 2 (ships_hit ≥ 5): SafeP2 (argmin P(blue)).
 *
 * TWO EXECUTION PATHS (identical to V10/V8 stateless):
 *   Path A — sv-passing (strategy_next_click_sv, tree-walk fast path).
 *   Path B — delta cache (strategy_next_click, sequential evaluator fallback).
 *
 * Process-wide BoardCache / std::call_once:
 *   Board files, weights, and SDP policies are loaded exactly once across all
 *   thread instances.
 *
 * External data files:
 *   data/trace_v11_weights_YYYYMMDD_HHMMSS.json  — V11 weights (180-weight,
 *                                                   or 200/140-weight remapped)
 *   data/trace_shallow_dp_v11s_2_t50_b5_YYYYMMDD_HHMMSS.bin.lzma
 *       — 6-color BDP policy (same format as V10)
 *   data/trace_shallow_dp_v11s_3_t200_b4_YYYYMMDD_HHMMSS.bin.lzma
 *       — 7-color BDP policy (same format as V10)
 *   data/sphere_trace_boards_{2..5}.bin.lzma  — board files (shared with V8/V10)
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

// Shared depth/blues layout (same as V8/V10)
static constexpr int V8_N_DEPTHS  = 5;
static constexpr int V8_N_BLUES   = 4;

// V8 base weight count (used for backward-compat remapping)
static constexpr int V8_N_TERMS   = 7;
static constexpr int V8_N_WEIGHTS = V8_N_DEPTHS * V8_N_BLUES * V8_N_TERMS;  // 140

// V10 weight count (used for backward-compat remapping)
static constexpr int V10_N_TERMS   = 10;
static constexpr int V10_N_WEIGHTS = V8_N_DEPTHS * V8_N_BLUES * V10_N_TERMS;  // 200

// V11 scorer: 9 terms, 180 weights per n_colors variant.
static constexpr int N_TERMS_X    = 9;
static constexpr int N_WEIGHTS_CT = V8_N_DEPTHS * V8_N_BLUES * N_TERMS_X;   // 180

// V11 term indices
static constexpr int T_BLUE           = 0;
static constexpr int T_INFO6          = 1;
static constexpr int T_EV             = 2;
static constexpr int T_GINI           = 3;
static constexpr int T_VAR_SP         = 4;
static constexpr int T_RARE_ID        = 5;
static constexpr int T_BLUE_X_INFO6   = 6;  // new in V11
static constexpr int T_BLUE_X_RARE_ID = 7;
static constexpr int T_BLUE_X_EV      = 8;

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
// key[0..24] = revealed_color[cell]  (0=unclicked, 1=blue, 2=teal, 3=green,
//              4=yellow, 5=spO, 6+k=var-rare-slot-k)
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

// ---------------------------------------------------------------------------
// Load SDP policy from .lzma file into store.
//
// Policy file format: 4-byte LE uint32 N, then N×31-byte entries:
//   uint8_t revealed_color[25] + uint8_t blues_counted + uint8_t best_cell
//   + float ev_sp (4 bytes, discarded)
// Files named "v11s" are pre-sorted.
//
// Memory strategy: use RAM if MemAvailable ≥ required + 4 GB margin.
// Otherwise decompress to a .sdp27.bin sidecar and mmap it.
// ---------------------------------------------------------------------------

static bool loadSdpPolicy(const std::string& lzma_path, SdpPolicyStore& store) {
    fprintf(stderr, "[colblitz_v11] Loading SDP policy: %s\n", lzma_path.c_str());
    fflush(stderr);

    std::string cmd = "xzcat \"" + lzma_path + "\"";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) {
        fprintf(stderr, "[colblitz_v11] popen failed for: %s\n", lzma_path.c_str());
        return false;
    }

    // Read 4-byte header to get N
    uint8_t hdr[4];
    if (fread(hdr, 1, 4, fp) != 4) {
        pclose(fp);
        fprintf(stderr, "[colblitz_v11] Failed to read header from: %s\n", lzma_path.c_str());
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

    fprintf(stderr, "[colblitz_v11]   %u entries, %.1f GB stored  |  MemAvailable=%.1f GB  |  %s\n",
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
                fprintf(stderr, "[colblitz_v11] Truncated policy file at entry %u\n", i);
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
        fprintf(stderr, "[colblitz_v11]   Loaded %u entries into RAM (%.1f GB)\n",
                n_entries, (double)required_bytes / (1024.0*1024.0*1024.0));
        fflush(stderr);
        return true;

    } else {
        // ---- Decompress to sidecar .bin, then mmap ----

        // Derive sidecar path: strip ".lzma" suffix
        std::string sidecar = lzma_path;
        if (sidecar.size() >= 9 && sidecar.substr(sidecar.size()-9) == ".bin.lzma")
            sidecar = sidecar.substr(0, sidecar.size()-9);
        else if (sidecar.size() >= 5 && sidecar.substr(sidecar.size()-5) == ".lzma")
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
            fprintf(stderr, "[colblitz_v11]   Writing sidecar: %s (%.1f GB)\n",
                    sidecar.c_str(), (double)sidecar_size / (1024.0*1024.0*1024.0));
            fflush(stderr);

            FILE* out = fopen(sidecar.c_str(), "wb");
            if (!out) {
                pclose(fp);
                fprintf(stderr, "[colblitz_v11] Cannot open sidecar for write: %s: %s\n",
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
                    fprintf(stderr, "[colblitz_v11] Truncated policy at entry %u\n", i);
                    return false;
                }
                memcpy(stored.key, entry_buf, 26);
                stored.cell = entry_buf[26];
                fwrite(&stored, STORED_ENTRY, 1, out);
            }
            fclose(out);
            fprintf(stderr, "[colblitz_v11]   Sidecar written.\n");
            fflush(stderr);
        } else {
            // Sidecar exists — drain the popen pipe we opened
            uint8_t discard[4096];
            while (fread(discard, 1, sizeof(discard), fp) > 0) {}
            fprintf(stderr, "[colblitz_v11]   Sidecar already exists, reusing.\n");
            fflush(stderr);
        }
        pclose(fp);

        // mmap the sidecar
        int fd = open(sidecar.c_str(), O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "[colblitz_v11] Cannot open sidecar for mmap: %s: %s\n",
                    sidecar.c_str(), strerror(errno));
            return false;
        }

        void* addr = mmap(nullptr, sidecar_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (addr == MAP_FAILED) {
            close(fd);
            fprintf(stderr, "[colblitz_v11] mmap failed: %s\n", strerror(errno));
            return false;
        }

        madvise(addr, sidecar_size, MADV_RANDOM);

        store.mmap_addr = addr;
        store.mmap_size = sidecar_size;
        store.mmap_fd   = fd;
        store.ptr       = static_cast<const SdpEntry*>(addr);
        store.n         = n_entries;
        store.loaded    = true;
        fprintf(stderr, "[colblitz_v11]   mmap'd sidecar: %u entries (%.1f GB)\n",
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

    SdpEntry probe;
    memcpy(probe.key, observed_colors, N_CELLS);
    probe.key[N_CELLS] = (uint8_t)blues_counted;
    probe.cell = 0;

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
// File discovery helpers
// ---------------------------------------------------------------------------

// Find the most recent data/trace_v11_weights_*.json.
// Returns empty string if not found.
static std::string findLatestV11WeightsFile() {
    std::string data_dir = repoPath("data");
    DIR* d = opendir(data_dir.c_str());
    if (!d) return "";
    std::string best;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        const char* name = ent->d_name;
        size_t len = strlen(name);
        if (len < 5 || strcmp(name + len - 5, ".json") != 0) continue;
        if (strncmp(name, "trace_v11_weights_", 18) == 0) {
            std::string full = data_dir + "/" + name;
            if (full > best) best = full;
        }
    }
    closedir(d);
    return best;
}

// Find the most recent data/trace_shallow_dp_v11s_{prefix}_*.bin.lzma.
// prefix_str is e.g. "2" or "3" (the n_rare index part of the filename).
// Returns empty string if not found.
static std::string findLatestSdpPolicyFile(const std::string& prefix_str) {
    std::string data_dir = repoPath("data");
    DIR* d = opendir(data_dir.c_str());
    if (!d) return "";
    // pattern prefix: "trace_shallow_dp_v11s_<prefix_str>_"
    std::string pat = "trace_shallow_dp_v11s_" + prefix_str + "_";
    std::string best;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        const char* name = ent->d_name;
        size_t len = strlen(name);
        if (len < 10) continue;
        // Must end with ".bin.lzma"
        if (strcmp(name + len - 9, ".bin.lzma") != 0) continue;
        if (strncmp(name, pat.c_str(), pat.size()) == 0) {
            std::string full = data_dir + "/" + name;
            if (full > best) best = full;
        }
    }
    closedir(d);
    return best;
}

// ---------------------------------------------------------------------------
// V11 weights file parsing.
//
// Accepts three input sizes and remaps to the 180-slot V11 layout:
//
//   180-weight (native V11):  w[d*36 + b*9 + t]  — copy directly.
//
//   200-weight (V10):         w[d*40 + b*10 + t_v10]
//     V10 term mapping:  t_v10 → V11 index
//       0: T_BLUE       →  0
//       1: T_INFO6      →  1
//       2: T_HFULL      →  (dropped)
//       3: T_EV         →  2
//       4: T_GINI       →  3
//       5: T_VAR_SP     →  4
//       6: T_RARE_ID    →  5
//       7: T_BLUE_X_HFULL → (dropped)
//       8: T_BLUE_X_RID →  7
//       9: T_BLUE_X_EV  →  8
//     T_BLUE_X_INFO6 (V11 index 6) → zero-filled.
//
//   140-weight (V8):          w[d*28 + b*7 + t_v8]
//     V8 term mapping:   t_v8 → V11 index
//       0: T_BLUE       →  0
//       1: T_INFO6      →  1
//       2: T_HFULL      →  (dropped)
//       3: T_EV         →  2
//       4: T_GINI       →  3
//       5: T_VAR_SP     →  4
//       6: T_RARE_ID    →  5
//     Cross-terms (V11 indices 6,7,8) → zero-filled.
// ---------------------------------------------------------------------------

static bool extractWeightsV11(const std::string& json, int n_colors,
                               std::array<double, N_WEIGHTS_CT>& out,
                               bool& native_v11_out)
{
    native_v11_out = false;
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

    // Maximum we'd ever read is V10_N_WEIGHTS=200; read up to that.
    double tmp[V10_N_WEIGHTS] = {};
    int n_read = 0;
    for (int i = 0; i < V10_N_WEIGHTS; ++i) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',') ++p;
        if (*p == ']') break;
        char* end;
        double v = strtod(p, &end);
        if (end == p) return false;
        tmp[n_read++] = v;
        p = end;
    }
    if (n_read == 0) return false;

    if (n_read == N_WEIGHTS_CT) {
        // 180-weight native V11: copy directly.
        for (int i = 0; i < N_WEIGHTS_CT; ++i) out[i] = tmp[i];
        native_v11_out = true;

    } else if (n_read == V10_N_WEIGHTS) {
        // 200-weight V10 file: remap d*40+b*10+t_v10 → d*36+b*9+t_v11.
        // V10 term index → V11 term index (or -1 = drop):
        //   0→0, 1→1, 2→-1(T_HFULL), 3→2, 4→3, 5→4, 6→5, 7→-1(T_BXH), 8→7, 9→8
        static constexpr int V10_TO_V11[10] = {0, 1, -1, 2, 3, 4, 5, -1, 7, 8};
        for (int fi = 0; fi < V10_N_WEIGHTS; ++fi) {
            int d    = fi / (V8_N_BLUES * V10_N_TERMS);
            int b    = (fi / V10_N_TERMS) % V8_N_BLUES;
            int t_v10 = fi % V10_N_TERMS;
            int t_v11 = V10_TO_V11[t_v10];
            if (t_v11 < 0) continue;  // drop T_HFULL and T_BLUE_X_HFULL
            out[d * (V8_N_BLUES * N_TERMS_X) + b * N_TERMS_X + t_v11] = tmp[fi];
        }
        // T_BLUE_X_INFO6 (index 6) stays zero-filled.
        native_v11_out = false;

    } else if (n_read == V8_N_WEIGHTS) {
        // 140-weight V8 file: remap d*28+b*7+t_v8 → d*36+b*9+t_v11.
        // V8 term index → V11 term index (or -1 = drop):
        //   0→0, 1→1, 2→-1(T_HFULL), 3→2, 4→3, 5→4, 6→5
        static constexpr int V8_TO_V11[7] = {0, 1, -1, 2, 3, 4, 5};
        for (int fi = 0; fi < V8_N_WEIGHTS; ++fi) {
            int d    = fi / (V8_N_BLUES * V8_N_TERMS);
            int b    = (fi / V8_N_TERMS) % V8_N_BLUES;
            int t_v8 = fi % V8_N_TERMS;
            int t_v11 = V8_TO_V11[t_v8];
            if (t_v11 < 0) continue;  // drop T_HFULL
            out[d * (V8_N_BLUES * N_TERMS_X) + b * N_TERMS_X + t_v11] = tmp[fi];
        }
        // Cross-terms (indices 6,7,8) stay zero-filled.
        native_v11_out = false;

    } else {
        // Unknown size — partial fill into the native V11 layout.
        int copy_n = std::min(n_read, N_WEIGHTS_CT);
        for (int i = 0; i < copy_n; ++i) out[i] = tmp[i];
        native_v11_out = (n_read >= N_WEIGHTS_CT);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Index-based filtering (unchanged from V8/V10)
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
// Outcome counts — index-based (unchanged from V8/V10)
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
// Score terms
// ---------------------------------------------------------------------------

static inline double termInfo6(const int* c6, int n) {
    if (n == 0) return 0.0;
    double inv = 1.0 / n, s = 0.0;
    for (int i = 0; i < 6; ++i)
        if (c6[i] > 0) { double p = c6[i] * inv; s -= p * std::log(p); }
    return s / LN6;
}

// termHfull kept for Phase-D fallback (uses full detailed-slot counts / ln9)
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

// V11 Gini: collapse var-rare slots (indices 5+) into a single bucket,
// then compute Gini over the resulting 6-element distribution.
// This fixes the rotational symmetry bug present in the original per-slot counts.
static inline double termGiniV11(const int* cdc, int slot_stride, int n) {
    if (n == 0) return 0.0;
    // Build 6-bucket collapsed counts: [blue, teal, green, yellow, spO, var-rare-all]
    // slot layout: 0=blue, 1=teal, 2=green, 3=yellow, 4=spO, 5..slot_stride-1=var-rare-k
    double inv = 1.0 / n, sq = 0.0;
    // Buckets 0..4 are direct.
    for (int i = 0; i < 5 && i < slot_stride; ++i) {
        if (cdc[i] > 0) { double p = cdc[i] * inv; sq += p * p; }
    }
    // Bucket 5: sum all var-rare slots.
    int var_sum = 0;
    for (int i = 5; i < slot_stride; ++i) var_sum += cdc[i];
    if (var_sum > 0) { double p = var_sum * inv; sq += p * p; }
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
// Identified slots (unchanged from V8/V10)
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
// Compute occ bitmask for each surviving row (unchanged from V8/V10)
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
// Build observed_colors[N_CELLS] for SDP policy lookup (unchanged from V10).
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
// CP pre-filter (unchanged from V8/V10)
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
// Phase 1 cell picker — V11 cross-terms scorer (9 terms, 180 weights).
//
// Term indices:
//   0: T_BLUE           P(blue)
//   1: T_INFO6          entropy over 6-bucket distribution / ln6
//   2: T_EV             expected SP / 500
//   3: T_GINI           Gini impurity (6-bucket collapsed, V11 symmetry fix)
//   4: T_VAR_SP         variance of SP / 500²
//   5: T_RARE_ID        P(unidentified var-rare ship cell)
//   6: T_BLUE_X_INFO6   T_BLUE × T_INFO6  (new in V11)
//   7: T_BLUE_X_RID     T_BLUE × T_RARE_ID
//   8: T_BLUE_X_EV      T_BLUE × T_EV
// ---------------------------------------------------------------------------

static int pickPhase1CellV11Idx(
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
    double w_ev          = weights[base + T_EV];
    double w_gini        = weights[base + T_GINI];
    double w_var_sp      = weights[base + T_VAR_SP];
    double w_rare_id     = weights[base + T_RARE_ID];
    double w_blue_info6  = weights[base + T_BLUE_X_INFO6];
    double w_blue_rid    = weights[base + T_BLUE_X_RARE_ID];
    double w_blue_ev     = weights[base + T_BLUE_X_EV];

    // Determine which expensive computations are actually needed.
    bool need_slot_sp = (std::abs(w_ev)      > 1e-15 || std::abs(w_var_sp)   > 1e-15 ||
                         std::abs(w_rare_id)  > 1e-15 || std::abs(w_blue_rid) > 1e-15 ||
                         std::abs(w_blue_ev)  > 1e-15);
    bool need_cross   = (std::abs(w_blue_info6) > 1e-15 ||
                         std::abs(w_blue_rid)   > 1e-15 ||
                         std::abs(w_blue_ev)    > 1e-15);
    // t_info6 is needed for w_info6 OR for the new T_BLUE_X_INFO6 cross-term.
    bool need_info6   = (std::abs(w_info6)     > 1e-15 || std::abs(w_blue_info6) > 1e-15);
    // t_blue is needed for w_blue OR for any cross-term.
    bool need_blue    = (std::abs(w_blue)      > 1e-15 || need_cross);

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

        double t_blue    = need_blue    ? c6[0] * inv_n : 0.0;
        double t_info6   = need_info6   ? termInfo6(c6, n) : 0.0;
        double t_ev      = (std::abs(w_ev)     > 1e-15 || std::abs(w_blue_ev)  > 1e-15)
                               && !slot_sp.empty() ? termEvNorm(cdc, slot_stride, n, slot_sp) : 0.0;
        double t_gini    = (std::abs(w_gini)   > 1e-15) ? termGiniV11(cdc, slot_stride, n) : 0.0;
        double t_var_sp  = (std::abs(w_var_sp) > 1e-15) && !slot_sp.empty()
                               ? termVarSp(cdc, slot_stride, n, slot_sp) : 0.0;
        double t_rare_id = (std::abs(w_rare_id) > 1e-15 || std::abs(w_blue_rid) > 1e-15)
                               ? termRareId(cdc, slot_stride, n, identified, n_var) : 0.0;

        double score = w_blue    * t_blue
                     + w_info6   * t_info6
                     + w_ev      * t_ev
                     + w_gini    * t_gini
                     + w_var_sp  * t_var_sp
                     + w_rare_id * t_rare_id;

        // Cross-term products (V11)
        if (need_cross) {
            score += w_blue_info6 * (t_blue * t_info6);
            score += w_blue_rid   * (t_blue * t_rare_id);
            score += w_blue_ev    * (t_blue * t_ev);
        }

        if (score > best_s) { best_s = score; best = unclicked[ii]; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Phase 1 cell picker — Phase-D fallback (unchanged from V8/V10)
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
// Phase 2 — SafeP2 (unchanged from V8/V10)
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
    // Board data
    BoardSet boards[4];
    bool     loaded[4] = {};

    // V11 weights (180-slot arrays; older files remapped via extractWeightsV11)
    std::array<double, N_WEIGHTS_CT> weights[4];
    bool                              weightsLoaded[4]   = {};
    bool                              weightsNativeV11[4] = {};

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

    // ---- Load V11 weights (most-recent trace_v11_weights_*.json, required) ----
    std::string wpath = findLatestV11WeightsFile();
    if (wpath.empty()) {
        fprintf(stderr,
            "[colblitz_v11] ERROR: no trace_v11_weights_*.json found in data/. "
            "Strategy will use Phase-D fallback for all games.\n");
        fflush(stderr);
    } else {
        fprintf(stderr, "[colblitz_v11] Loading weights: %s\n", wpath.c_str());
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
                    bool native = false;
                    if (extractWeightsV11(json, n_colors, c.weights[idx], native)) {
                        c.weightsLoaded[idx]    = true;
                        c.weightsNativeV11[idx] = native;
                        fprintf(stderr,
                            "[colblitz_v11]   n_colors=%d: %d weights loaded (%s)\n",
                            n_colors, N_WEIGHTS_CT,
                            native ? "native V11" : "remapped from older format");
                        fflush(stderr);
                    }
                }
            }
            fclose(fp);
        }
    }

    // ---- Load SDP policies (auto-discover most-recent V11 files) ----
    // 6-color policy (n_rare=2)
    {
        std::string path = findLatestSdpPolicyFile("2");
        if (path.empty()) {
            fprintf(stderr, "[colblitz_v11] No 6-color V11 SDP policy found in data/ "
                            "(trace_shallow_dp_v11s_2_*.bin.lzma); BDP disabled for 6c.\n");
            fflush(stderr);
        } else {
            loadSdpPolicy(path, c.sdp[0]);
        }
    }
    // 7-color policy (n_rare=3)
    {
        std::string path = findLatestSdpPolicyFile("3");
        if (path.empty()) {
            fprintf(stderr, "[colblitz_v11] No 7-color V11 SDP policy found in data/ "
                            "(trace_shallow_dp_v11s_3_*.bin.lzma); BDP disabled for 7c.\n");
            fflush(stderr);
        } else {
            loadSdpPolicy(path, c.sdp[1]);
        }
    }
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------

class ColblitzV11BdpCbCtOTStrategy : public OTStrategy {
public:
    // Run-level: non-owning views into the process-wide cache.
    const BoardSet*    fullBoards_[4]       = {};
    bool               boardsLoaded_[4]     = {};

    std::array<double, N_WEIGHTS_CT> weights_[4];
    bool               weightsLoaded_[4]    = {};
    bool               weightsNativeV11_[4] = {};

    const SdpPolicyStore* sdp_[2]    = {};  // 0=6c, 1=7c
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
            fullBoards_[i]          = c.loaded[i] ? &c.boards[i] : nullptr;
            boardsLoaded_[i]        = c.loaded[i];
            if (c.weightsLoaded[i]) {
                weights_[i]         = c.weights[i];
                weightsLoaded_[i]   = true;
                weightsNativeV11_[i] = c.weightsNativeV11[i];
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

        // ---- V11 cross-terms scorer (or Phase-D fallback) ----
        if (weightsLoaded_[bs_idx]) {
            return pickPhase1CellV11Idx(fbs, sv, unclicked, n_rare, n_colors,
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

extern "C" sphere::StrategyBase* create_strategy()                         { return new ColblitzV11BdpCbCtOTStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* inst) {
    static_cast<ColblitzV11BdpCbCtOTStrategy*>(inst)->init_evaluation_run();
}

extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {
    static_cast<ColblitzV11BdpCbCtOTStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<ColblitzV11BdpCbCtOTStrategy*>(inst);
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
    auto* s = static_cast<ColblitzV11BdpCbCtOTStrategy*>(inst);
    std::vector<Cell> brd = parse_board_json(board_json ? board_json : "[]");
    ClickResult out;
    s->next_click_with_sv(brd, meta_json ? meta_json : "{}", sv_ptr, sv_len, out);
    buf = "{\"row\":" + std::to_string(out.row) + ",\"col\":" + std::to_string(out.col) + "}";
    return buf.c_str();
}
