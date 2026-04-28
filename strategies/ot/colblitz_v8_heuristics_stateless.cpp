// sphere:stateless
// Upgrade strategy .so from the default -O2 to -O3 to enable
// auto-vectorization and additional inlining.
#pragma GCC optimize("O3")

/**
 * colblitz_v8_heuristics_stateless.cpp — Tree-walk-compatible CORTANA_V8 strategy.
 *
 * Implements the same CORTANA_V8 board-filter heuristic as
 * colblitz_v8_heuristics.cpp, adapted for use with the tree-walk evaluator.
 *
 * Relationship to colblitz_v8_heuristics.cpp (stateful sibling):
 *   colblitz_v8_heuristics.cpp filters arr_ in-place and carries the result
 *   across calls as per-game member state.  This is fast for sequential
 *   evaluation but NOT compatible with the tree-walk evaluator, which never
 *   calls init_game_payload between boards.
 *
 *   This file keeps fullBoards_[idx] immutable and uses a std::vector<int>
 *   index list (sv) as the surviving set.  Two execution paths are provided:
 *
 * TWO EXECUTION PATHS
 * -------------------
 *
 *   Path A — sv-passing (strategy_next_click_sv, primary fast path):
 *     Used by the tree-walk evaluator (evaluate_ot_treewalk).
 *     The harness maintains the full-population surviving board index list as
 *     it descends the game tree and passes it directly via sv_ptr/sv_len.
 *     No filtering runs in the strategy — the method reads sv_ptr, builds
 *     rareColorGroups from the board argument (needed for scoring terms), and
 *     proceeds directly to scoring.  O(|sv| × U × F) per call (scoring only).
 *
 *   Path B — delta cache (strategy_next_click, fallback):
 *     Used by the sequential evaluator (evaluate_ot) and any harness that
 *     does not export strategy_next_click_sv.
 *     Each instance caches (revealed_mask, rareColorGroups, sv) from the last
 *     call.  Parent→child calls (revealed_mask grows by one bit) apply only
 *     the delta reveals to the cached sv — O(delta × |sv|).  On backtrack or
 *     cold start the full sv is rebuilt from fullBoards_ — O(N × k × F).
 *     The output is a pure function of (board, meta); the cache is an
 *     unobservable optimisation.  sphere:stateless holds for this path too.
 *
 * sphere:stateless contract:
 *   Path A: output is a deterministic function of (board, meta, full_sv).
 *     full_sv is provided by the harness and is identical across all threads
 *     at any given tree node (full population filtered by the current revealed
 *     path, independent of per-thread chunking).
 *   Path B: output is a deterministic function of (board, meta).
 *   In both cases the contract required by the tree-walk evaluator is satisfied.
 *
 * Process-wide board cache (BoardCache / std::call_once):
 *   The tree-walk evaluator creates one bridge instance per thread (up to 20).
 *   Without the shared cache, each instance would decompress the four board
 *   files independently (~44 s total init).  BoardCache loads the files exactly
 *   once via std::call_once; all instances share the immutable data via
 *   const BoardSet* pointers.  Init time for instances 2–N is near-zero.
 *
 * External data files:
 *   data/sphere_trace_boards_2.bin.lzma  (~874 KB)  n_colors=6
 *   data/sphere_trace_boards_3.bin.lzma  (~3.8 MB)  n_colors=7
 *   data/sphere_trace_boards_4.bin.lzma  (~5.8 MB)  n_colors=8
 *   data/sphere_trace_boards_5.bin.lzma  (~5.5 MB)  n_colors=9
 *   data/trace_v8_weights.json           (~15 KB)   CORTANA_V8 weights
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <numeric>
#include <string>
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

static constexpr int V8_N_DEPTHS = 5;
static constexpr int V8_N_BLUES  = 4;
static constexpr int V8_N_TERMS  = 7;
static constexpr int V8_N_WEIGHTS = V8_N_DEPTHS * V8_N_BLUES * V8_N_TERMS; // 140

static constexpr double V8_EV_DENOM  = 500.0;
static constexpr double V8_VAR_DENOM = 500.0 * 500.0;

// SP values for each detailed slot index:
//   0=blue (unused), 1=teal=20, 2=green=35, 3=yellow=55, 4=spO=90, 5+=var-rare~101
static constexpr double SLOT_SP_FIXED[5] = {0.0, 20.0, 35.0, 55.0, 90.0};

// Empirical appearance weights for variable-rare colors (Light, Dark, Red, Rainbow)
static constexpr double VAR_RARE_SP[4]     = {76.0, 104.0, 150.0, 500.0};
static constexpr double VAR_RARE_WEIGHT[4] = {0.7143, 0.4052, 0.1332, 0.0508};

static double computeVarRareEV() {
    double wsum = 0.0, ev = 0.0;
    for (int i = 0; i < 4; ++i) { wsum += VAR_RARE_WEIGHT[i]; ev += VAR_RARE_WEIGHT[i] * VAR_RARE_SP[i]; }
    return ev / wsum;
}
static const double VAR_RARE_EV = computeVarRareEV(); // ~101 SP

static constexpr double LN6 = 1.791759469228327;  // log(6)
static constexpr double LN9 = 2.1972245773362196; // log(9)

static constexpr int SHIPS_HIT_THRESHOLD = 5;

// Phase-D per-depth weights for (w_info6, w_hfull), indexed by [n_rare-2][depth]
// n_rare = n_colors - 4; n_rare in {2,3,4,5} -> index 0..3
static constexpr double PHASE_D_WEIGHTS[4][5][2] = {
    // n_rare=2 (6-color)
    {{0.50, 0.30}, {0.50, 0.20}, {0.50, 0.30}, {0.60, 0.40}, {0.40, 0.30}},
    // n_rare=3 (7-color)
    {{0.40, 0.00}, {0.40, 0.60}, {0.00, 0.70}, {0.00, 1.00}, {0.00, 0.90}},
    // n_rare=4 (8-color)
    {{0.00, 0.00}, {0.30, 0.80}, {0.00, 0.80}, {0.00, 0.90}, {0.00, 1.00}},
    // n_rare=5 (9-color)
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
// Board storage — fullBoards_ is immutable after init_evaluation_run.
// Filtering operates on a std::vector<int> of surviving row indices.
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
// Minimal JSON weight extraction
// ---------------------------------------------------------------------------

static bool extractWeights(const std::string& json, int n_colors,
                            std::array<double, V8_N_WEIGHTS>& out) {
    std::string key = "\"" + std::to_string(n_colors) + "\"";
    const char* p = strstr(json.c_str(), key.c_str());
    if (!p) return false;
    p += key.size();
    p = strstr(p, "\"weights\"");
    if (!p) return false;
    p = strchr(p, '[');
    if (!p) return false;
    ++p;
    for (int i = 0; i < V8_N_WEIGHTS; ++i) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',') ++p;
        if (*p == ']') return false;
        char* end;
        out[i] = strtod(p, &end);
        if (end == p) return false;
        p = end;
    }
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
// Index-based filtering
//
// All filter functions take:
//   const BoardSet& fbs   — immutable full board data (never modified)
//   std::vector<int>& sv  — surviving row indices (modified in-place)
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

    // Step 1: keep rows where any var-rare column has this bit
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

    // Update color group
    int32_t existing = 0;
    auto it = rareColorGroups.find(color);
    if (it != rareColorGroups.end()) existing = it->second;
    rareColorGroups[color] = existing | bit;

    // Step 2: grouping permutation constraint
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

// Apply a single reveal to the surviving index list.
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
// Outcome counts — index-based
// ---------------------------------------------------------------------------

// Compute both counts_dc (detailed, slot_stride = 1+fields) and counts6 (6-bucket)
// in a single pass over the surviving board set.
//
// counts_dc layout: [blue, teal, green, yellow, spO, var0, var1, ...]  (1 + fields entries)
// counts6 layout:   [blue, teal, green, yellow, spO, var_all]          (6 entries, var_all = Σvar_k)
//
// Previously two separate O(|sv|×nu×fields) passes; now one.
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

        // Derive counts6 from counts_dc — no extra board scan needed.
        // slots: 0=blue, 1=teal, 2=green, 3=yellow, 4=spO, 5+=var-rare
        c6[0] = dc[0];                          // blue
        c6[1] = dc[1 + COL_TEAL];               // teal
        c6[2] = dc[1 + COL_GREEN];              // green
        c6[3] = dc[1 + COL_YELLOW];             // yellow
        c6[4] = dc[1 + COL_RARE_START];         // spO
        int var_sum = 0;
        for (int k = COL_RARE_START + 1; k < fields; ++k) var_sum += dc[1 + k];
        c6[5] = var_sum;                         // all var-rare combined
    }
}

// ---------------------------------------------------------------------------
// Score terms (unchanged — operate on counts arrays, not board data)
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
// Identified slots — index-based
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

static std::vector<double> buildSlotSp(int n_rare) {
    std::vector<double> sp(SLOT_SP_FIXED, SLOT_SP_FIXED + 5);
    for (int k = 0; k < n_rare - 1; ++k) sp.push_back(VAR_RARE_EV);
    return sp;
}

// ---------------------------------------------------------------------------
// Compute occ (bitmask of all occupied cells) for each surviving row
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
// CP pre-filter — unchanged interface (works on counts6 arrays)
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
// Phase 1 cell picker — CORTANA_V8 (index-based)
// ---------------------------------------------------------------------------

static int pickPhase1CellV8Idx(
    const BoardSet& fbs,
    const std::vector<int>& sv,
    const std::vector<int>& unclicked,
    int n_rare, int ships_hit, int blues_used,
    const std::unordered_map<std::string, int32_t>& rareColorGroups,
    const std::array<double, V8_N_WEIGHTS>& weights)
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

    int d = std::min(ships_hit, V8_N_DEPTHS - 1);
    int b = std::min(blues_used, V8_N_BLUES - 1);
    int base = d * 28 + b * 7;
    double w_blue    = weights[base + 0];
    double w_info6   = weights[base + 1];
    double w_hfull   = weights[base + 2];
    double w_ev      = weights[base + 3];
    double w_gini    = weights[base + 4];
    double w_var_sp  = weights[base + 5];
    double w_rare_id = weights[base + 6];

    int n_var = n_rare - 1;
    std::vector<double> slot_sp;
    std::vector<bool>   identified;
    if (std::abs(w_ev) > 1e-15 || std::abs(w_var_sp) > 1e-15 || std::abs(w_rare_id) > 1e-15) {
        identified = getIdentifiedSlotsIdx(fbs, sv, rareColorGroups, n_rare);
        slot_sp    = buildSlotSp(n_rare);
    } else {
        identified.assign(n_var, false);
    }

    double inv_n = (n > 0) ? 1.0 / n : 0.0;
    int best = unclicked[0]; double best_s = -1e18;
    int nu = (int)unclicked.size();
    for (int ii = 0; ii < nu; ++ii) {
        const int* c6  = &counts6[ii * 6];
        const int* cdc = &counts_dc[ii * slot_stride];
        double score = 0.0;
        if (std::abs(w_blue)    > 1e-15) score += w_blue    * (c6[0] * inv_n);
        if (std::abs(w_info6)   > 1e-15) score += w_info6   * termInfo6(c6, n);
        if (std::abs(w_hfull)   > 1e-15) score += w_hfull   * termHfull(cdc, slot_stride, n);
        if (std::abs(w_ev)      > 1e-15 && !slot_sp.empty())
                                          score += w_ev      * termEvNorm(cdc, slot_stride, n, slot_sp);
        if (std::abs(w_gini)    > 1e-15) score += w_gini    * termGini(cdc, slot_stride, n);
        if (std::abs(w_var_sp)  > 1e-15 && !slot_sp.empty())
                                          score += w_var_sp  * termVarSp(cdc, slot_stride, n, slot_sp);
        if (std::abs(w_rare_id) > 1e-15) score += w_rare_id * termRareId(cdc, slot_stride, n, identified, n_var);
        if (score > best_s) { best_s = score; best = unclicked[ii]; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Phase 1 cell picker — Phase-D fallback (index-based)
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
// Phase 2 — SafeP2 (index-based)
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
// Process-wide board + weight cache.
//
// Board files are large (up to ~870 K rows × 8 int32s ≈ 27 MB per variant)
// and slow to decompress (xzcat).  When the tree-walk evaluator initialises
// N bridge instances in sequence, without this cache each instance runs 4
// xzcat calls — 20 threads × 4 files = 80 xzcat processes.
//
// The cache loads each file exactly once (on the first init_evaluation_run
// call) and shares the immutable data across all instances.  Subsequent
// init_evaluation_run calls on any instance are near-zero cost.
// ---------------------------------------------------------------------------

struct BoardCache {
    BoardSet boards[4];       // indexed by n_rare - 2
    bool     loaded[4] = {};

    std::array<double, V8_N_WEIGHTS> weights[4];
    bool                              weightsLoaded[4] = {};
};

static BoardCache&   g_board_cache() {
    static BoardCache cache;
    return cache;
}
static std::once_flag g_board_cache_flag;

static void load_board_cache() {
    BoardCache& c = g_board_cache();
    for (int n_colors = 6; n_colors <= 9; ++n_colors) {
        int n_rare = n_colors - 4;
        int idx    = n_rare - 2;
        int fields = 3 + n_rare;
        std::string path = repoPath(
            "data/sphere_trace_boards_" + std::to_string(n_rare) + ".bin.lzma");
        std::vector<int32_t> raw;
        if (loadLzma(path, raw, fields) && !raw.empty()) {
            int n = (int)(raw.size() / fields);
            c.boards[idx].data   = std::move(raw);
            c.boards[idx].n      = n;
            c.boards[idx].fields = fields;
            c.loaded[idx]        = true;
        }
    }
    std::string wpath = repoPath("data/trace_v8_weights.json");
    FILE* fp = fopen(wpath.c_str(), "r");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        rewind(fp);
        std::string json(sz, '\0');
        if (fread(&json[0], 1, sz, fp) != (size_t)sz) json.clear();
        fclose(fp);
        for (int n_colors = 6; n_colors <= 9; ++n_colors) {
            int idx = n_colors - 4 - 2;
            if (extractWeights(json, n_colors, c.weights[idx]))
                c.weightsLoaded[idx] = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Strategy class — stateless, index-based filtering version
// ---------------------------------------------------------------------------

class ColblitzV8StatelessOTStrategy : public OTStrategy {
public:
    // -----------------------------------------------------------------------
    // Run-level state: pointers into the process-wide board cache.
    // The cache owns the data; these are non-owning views.
    // -----------------------------------------------------------------------

    const BoardSet* fullBoards_[4]  = {};  // indexed by n_rare - 2
    bool            boardsLoaded_[4] = {};

    // -----------------------------------------------------------------------
    // Incremental delta cache (one slot per board variant, per instance).
    //
    // The tree-walk evaluator is DFS and uses one strategy instance per thread.
    // Consecutive calls within one thread are almost always parent→child
    // (revealed_mask grows by one bit).  We cache the last (revealed_mask,
    // rareColorGroups, sv) so that parent→child calls only apply the delta.
    // On a backtrack (revealed_mask not a superset of cached), we rebuild fully.
    //
    // cache_valid_[i]: true once the cache has been populated for variant i.
    // cache_revealed_[i]: bitmask of all clicked cells from the last call.
    // cache_rare_[i]: rareColorGroups map from the last call.
    // cache_sv_[i]: surviving row indices from the last call.
    // -----------------------------------------------------------------------

    bool     cache_valid_[4]    = {};
    uint32_t cache_revealed_[4] = {};
    std::unordered_map<std::string, int32_t> cache_rare_[4];
    std::vector<int> cache_sv_[4];

    std::array<double, V8_N_WEIGHTS> weights_[4];
    bool                              weightsLoaded_[4] = {};

    // -----------------------------------------------------------------------
    // init_evaluation_run: load board arrays and weights
    // -----------------------------------------------------------------------

    void init_evaluation_run() override {
        // Reset incremental delta cache (safe to call multiple times).
        for (int i = 0; i < 4; ++i) {
            cache_valid_[i]    = false;
            cache_revealed_[i] = 0;
            cache_rare_[i].clear();
            cache_sv_[i].clear();
        }

        // Load board data and weights into the process-wide cache exactly once.
        // All subsequent instances share the same data without re-decompressing.
        std::call_once(g_board_cache_flag, load_board_cache);

        // Point this instance's views at the shared cache.
        const BoardCache& c = g_board_cache();
        for (int i = 0; i < 4; ++i) {
            fullBoards_[i]    = c.loaded[i] ? &c.boards[i] : nullptr;
            boardsLoaded_[i]  = c.loaded[i];
            if (c.weightsLoaded[i]) {
                weights_[i]        = c.weights[i];
                weightsLoaded_[i]  = true;
            }
        }
    }

    // init_game_payload is a no-op — all state is rebuilt from board each call.

    // -----------------------------------------------------------------------
    // next_click — incremental delta cache.
    //
    // Maintains one cached (revealed_mask, rareColorGroups, sv) per board
    // variant.  On each call:
    //   - Parent→child (revealed_mask is a strict superset of cache): apply
    //     only the new reveal(s) to a copy of the cached sv.
    //   - Backtrack or first call (revealed_mask not a superset): full rebuild
    //     from fullBoards_, then update cache.
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

        // Build unclicked list, revealed bitmask, and sorted reveals list.
        std::vector<int> unclicked;
        unclicked.reserve(25);
        std::vector<std::pair<int, std::string>> reveals; // (cell_idx, color)
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

        // Sort for determinism.
        std::sort(reveals.begin(),   reveals.end(),
                  [](const auto& a, const auto& b){ return a.first < b.first; });
        std::sort(unclicked.begin(), unclicked.end());

        if (unclicked.empty()) { out.row = 0; out.col = 0; return; }

        if (!boardsLoaded_[bs_idx]) {
            out.row = unclicked[0] / GRID;
            out.col = unclicked[0] % GRID;
            return;
        }

        const BoardSet& fbs = *fullBoards_[bs_idx];

        // Working surviving index list and rareColorGroups for this call.
        std::vector<int> sv;
        std::unordered_map<std::string, int32_t> rareColorGroups;

        uint32_t cached = cache_valid_[bs_idx] ? cache_revealed_[bs_idx] : ~0u;
        bool is_superset = cache_valid_[bs_idx]
                        && ((cached & revealed_mask) == cached)   // cached ⊆ current
                        && (cached != revealed_mask);             // strictly more reveals now

        if (revealed_mask == 0) {
            // No reveals yet — full board, no filtering needed.
            sv.resize(fbs.n);
            std::iota(sv.begin(), sv.end(), 0);
            // rareColorGroups stays empty.
        } else if (is_superset) {
            // Parent→child: start from cached sv and apply only the delta reveals.
            sv              = cache_sv_[bs_idx];
            rareColorGroups = cache_rare_[bs_idx];

            uint32_t delta_mask = revealed_mask & ~cached;
            // Apply delta reveals in cell-index order (already sorted in reveals[]).
            for (const auto& [cidx, color] : reveals) {
                if (delta_mask & (1u << cidx))
                    applyRevealIdx(fbs, sv, rareColorGroups, cidx, color, n_rare);
            }
        } else {
            // Backtrack or cache cold: full rebuild from scratch.
            sv.resize(fbs.n);
            std::iota(sv.begin(), sv.end(), 0);
            for (const auto& [cidx, color] : reveals)
                applyRevealIdx(fbs, sv, rareColorGroups, cidx, color, n_rare);
        }

        // Update cache with the result for this call.
        cache_valid_[bs_idx]    = true;
        cache_revealed_[bs_idx] = revealed_mask;
        cache_rare_[bs_idx]     = rareColorGroups;
        cache_sv_[bs_idx]       = sv;

        if (sv.empty()) {
            out.row = unclicked[0] / GRID;
            out.col = unclicked[0] % GRID;
            return;
        }

        int chosen = -1;

        if (ships_hit < SHIPS_HIT_THRESHOLD) {
            if (weightsLoaded_[bs_idx]) {
                chosen = pickPhase1CellV8Idx(fbs, sv, unclicked, n_rare,
                                              ships_hit, blues_used,
                                              rareColorGroups, weights_[bs_idx]);
            } else {
                chosen = pickPhase1CellPhaseDIdx(fbs, sv, unclicked, n_rare, ships_hit);
            }
        } else {
            std::vector<int32_t> occ_sv;
            computeOccIdx(fbs, sv, occ_sv);
            chosen = pickSafeP2CellIdx(fbs, sv, occ_sv, unclicked);
        }

        if (chosen < 0) chosen = unclicked[0];
        out.row = chosen / GRID;
        out.col = chosen % GRID;
    }

    // -----------------------------------------------------------------------
    // next_click_with_sv — sv-aware variant for the tree-walk harness.
    //
    // Receives the pre-filtered full-population surviving board index list
    // directly from the harness, skipping the strategy's own filtering step
    // entirely.  The harness maintains the correct sv as it descends and
    // backtracks the tree, so filtering is never redundant here.
    //
    // The board argument is still parsed to extract rareColorGroups (needed
    // for scoring terms that track which var-rare colors have been identified),
    // but no filter functions are called.
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

        // Build unclicked list and reveals (for rareColorGroups only — no filtering).
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

        if (!boardsLoaded_[bs_idx] || sv_len == 0) {
            out.row = unclicked[0] / GRID;
            out.col = unclicked[0] % GRID;
            return;
        }

        const BoardSet& fbs = *fullBoards_[bs_idx];

        // Use the provided sv directly — no filtering needed.
        std::vector<int> sv(sv_ptr, sv_ptr + sv_len);

        // Reconstruct rareColorGroups from revealed var-rare cells.
        // This is needed by scoring terms (rare_id) but does not modify sv.
        std::unordered_map<std::string, int32_t> rareColorGroups;
        for (const auto& [cidx, color] : reveals) {
            if (color == "spL" || color == "spD" || color == "spR" || color == "spW") {
                int32_t bit = (int32_t)(1 << cidx);
                rareColorGroups[color] |= bit;
            }
        }

        int chosen = -1;

        if (ships_hit < SHIPS_HIT_THRESHOLD) {
            if (weightsLoaded_[bs_idx]) {
                chosen = pickPhase1CellV8Idx(fbs, sv, unclicked, n_rare,
                                              ships_hit, blues_used,
                                              rareColorGroups, weights_[bs_idx]);
            } else {
                chosen = pickPhase1CellPhaseDIdx(fbs, sv, unclicked, n_rare, ships_hit);
            }
        } else {
            std::vector<int32_t> occ_sv;
            computeOccIdx(fbs, sv, occ_sv);
            chosen = pickSafeP2CellIdx(fbs, sv, occ_sv, unclicked);
        }

        if (chosen < 0) chosen = unclicked[0];
        out.row = chosen / GRID;
        out.col = chosen % GRID;
    }
};

// ---------------------------------------------------------------------------
// C exports required by the harness
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new ColblitzV8StatelessOTStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* inst) {
    static_cast<ColblitzV8StatelessOTStrategy*>(inst)->init_evaluation_run();
}

extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {
    static_cast<ColblitzV8StatelessOTStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
}


extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<ColblitzV8StatelessOTStrategy*>(inst);
    std::vector<Cell> brd = parse_board_json(board_json ? board_json : "[]");
    ClickResult out;
    s->next_click(brd, meta_json ? meta_json : "{}", out);
    buf = "{\"row\":" + std::to_string(out.row) + ",\"col\":" + std::to_string(out.col) + "}";
    return buf.c_str();
}

// sv-aware variant: receives the pre-filtered full-population surviving board
// index list from the tree-walk harness.  Skips all internal filtering.
extern "C" const char* strategy_next_click_sv(void* inst,
                                               const char* board_json,
                                               const char* meta_json,
                                               const int*  sv_ptr,
                                               int         sv_len)
{
    thread_local static std::string buf;
    auto* s = static_cast<ColblitzV8StatelessOTStrategy*>(inst);
    std::vector<Cell> brd = parse_board_json(board_json ? board_json : "[]");
    ClickResult out;
    s->next_click_with_sv(brd, meta_json ? meta_json : "{}", sv_ptr, sv_len, out);
    buf = "{\"row\":" + std::to_string(out.row) + ",\"col\":" + std::to_string(out.col) + "}";
    return buf.c_str();
}
