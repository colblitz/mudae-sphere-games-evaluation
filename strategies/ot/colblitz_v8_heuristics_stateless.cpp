// sphere:stateless
// Upgrade strategy .so from the default -O2 to -O3 to enable
// auto-vectorization and additional inlining.
#pragma GCC optimize("O3")

/**
 * colblitz_v8_heuristics_stateless.cpp — Stateless variant of colblitz_v8_heuristics.cpp.
 *
 * Identical algorithm to colblitz_v8_heuristics.cpp (CORTANA_V8 board-filter
 * heuristic).  The only difference is how per-call state is handled:
 *
 *   colblitz_v8_heuristics.cpp  (incremental, stateful for tree-walk purposes)
 *     Maintains arr_, prevRevealedMask_, rareColorGroups_ as per-game members.
 *     Applies each reveal once and carries the filtered set across calls.
 *     Correct and fast for sequential evaluation; NOT compatible with the
 *     tree-walk evaluator (which never calls init_game_payload between boards).
 *
 *   colblitz_v8_heuristics_stateless.cpp  (from-scratch, stateless)  ← this file
 *     On every next_click call, starts from a full copy of fullBoards_[idx] and
 *     re-applies ALL clicked cells from the board argument before scoring.
 *     Output is identical to the incremental version for any given (board, meta)
 *     input.  Compatible with the tree-walk evaluator.
 *
 * The per-call rebuild is O(all_reveals × boards) instead of O(new_reveals × boards),
 * but because the tree-walk evaluator calls next_click far fewer times than the
 * sequential runner, the net wall time is typically lower overall.
 *
 * External data files (same as colblitz_v8_heuristics.cpp):
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
// Outcome counts
// ---------------------------------------------------------------------------

static void computeOutcomeCounts6(
    const BoardSet& bs,
    const std::vector<int32_t>& occ,
    const std::vector<int>& unclicked,
    std::vector<int>& counts6)
{
    int nu = (int)unclicked.size();
    counts6.assign(nu * 6, 0);
    const int32_t* data = bs.data.data();
    const int fields    = bs.fields;
    const int n         = bs.n;
    for (int ii = 0; ii < nu; ++ii) {
        int cell = unclicked[ii];
        int32_t bit = (int32_t)(1 << cell);
        int* c = &counts6[ii * 6];
        int n_blue=0, n_teal=0, n_green=0, n_yellow=0, n_spo=0, n_var=0;
        for (int row = 0; row < n; ++row) {
            const int32_t* rowp = data + row * fields;
            if ((occ[row] & bit) == 0) { ++n_blue; continue; }
            if (rowp[COL_TEAL]        & bit) { ++n_teal;   continue; }
            if (rowp[COL_GREEN]       & bit) { ++n_green;  continue; }
            if (rowp[COL_YELLOW]      & bit) { ++n_yellow; continue; }
            if (rowp[COL_RARE_START]  & bit) { ++n_spo;    continue; }
            ++n_var;
        }
        c[0]=n_blue; c[1]=n_teal; c[2]=n_green;
        c[3]=n_yellow; c[4]=n_spo; c[5]=n_var;
    }
}

static void computeOutcomeCountsDetailed(
    const BoardSet& bs,
    const std::vector<int32_t>& occ,
    const std::vector<int>& unclicked,
    std::vector<int>& counts_dc,
    int& slot_stride)
{
    int nu     = (int)unclicked.size();
    int fields = bs.fields;
    slot_stride = 1 + fields;
    counts_dc.assign(nu * slot_stride, 0);
    const int32_t* data = bs.data.data();
    const int n         = bs.n;
    for (int ii = 0; ii < nu; ++ii) {
        int cell    = unclicked[ii];
        int32_t bit = (int32_t)(1 << cell);
        int* c      = &counts_dc[ii * slot_stride];
        for (int row = 0; row < n; ++row) {
            const int32_t* rowp = data + row * fields;
            if ((occ[row] & bit) == 0) { ++c[0]; continue; }
            for (int col = 0; col < fields; ++col)
                if (rowp[col] & bit) { ++c[1 + col]; break; }
        }
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
// Identified slots
// ---------------------------------------------------------------------------

static std::vector<bool> getIdentifiedSlots(
    const BoardSet& bs,
    const std::unordered_map<std::string, int32_t>& rareColorGroups,
    int n_rare)
{
    int n_var = n_rare - 1;
    std::vector<bool> identified(n_var, false);
    if (n_var <= 0 || rareColorGroups.empty() || bs.n == 0) return identified;
    int var_start = COL_RARE_START + 1;
    const int32_t* data = bs.data.data();
    int fields = bs.fields, n = bs.n;
    for (int k = 0; k < n_var; ++k) {
        int col = var_start + k;
        for (const auto& kv : rareColorGroups) {
            int32_t bits = kv.second;
            bool all_match = true, any_found = false;
            for (int row = 0; row < n; ++row) {
                int32_t colval = data[row * fields + col];
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
// CP pre-filter
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
// Phase 1 cell picker — CORTANA_V8
// ---------------------------------------------------------------------------

static int pickPhase1CellV8(
    const BoardSet& bs,
    const std::vector<int32_t>& occ,
    const std::vector<int>& unclicked,
    int n_rare, int ships_hit, int blues_used,
    const std::unordered_map<std::string, int32_t>& rareColorGroups,
    const std::array<double, V8_N_WEIGHTS>& weights)
{
    if (unclicked.empty()) return -1;
    int n = bs.n;
    std::vector<int> counts6;
    computeOutcomeCounts6(bs, occ, unclicked, counts6);
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

    bool need_dc = (std::abs(w_hfull)  > 1e-15 || std::abs(w_ev)      > 1e-15 ||
                    std::abs(w_gini)   > 1e-15 || std::abs(w_var_sp)  > 1e-15 ||
                    std::abs(w_rare_id) > 1e-15);

    std::vector<int> counts_dc;
    int slot_stride = 0;
    if (need_dc) computeOutcomeCountsDetailed(bs, occ, unclicked, counts_dc, slot_stride);

    std::vector<double> slot_sp;
    std::vector<bool>   identified;
    int n_var = n_rare - 1;
    if (need_dc && (std::abs(w_ev) > 1e-15 || std::abs(w_var_sp) > 1e-15 ||
                    std::abs(w_rare_id) > 1e-15)) {
        identified = getIdentifiedSlots(bs, rareColorGroups, n_rare);
        slot_sp    = buildSlotSp(n_rare);
    } else {
        identified.assign(n_var, false);
    }

    double inv_n = (n > 0) ? 1.0 / n : 0.0;
    int best = unclicked[0]; double best_s = -1e18;
    int nu = (int)unclicked.size();
    for (int ii = 0; ii < nu; ++ii) {
        const int* c6 = &counts6[ii * 6];
        double score = 0.0;
        if (std::abs(w_blue)  > 1e-15) score += w_blue  * (c6[0] * inv_n);
        if (std::abs(w_info6) > 1e-15) score += w_info6 * termInfo6(c6, n);
        if (need_dc) {
            const int* cdc = &counts_dc[ii * slot_stride];
            if (std::abs(w_hfull)   > 1e-15) score += w_hfull   * termHfull(cdc, slot_stride, n);
            if (std::abs(w_ev)      > 1e-15 && !slot_sp.empty())
                                              score += w_ev      * termEvNorm(cdc, slot_stride, n, slot_sp);
            if (std::abs(w_gini)    > 1e-15) score += w_gini    * termGini(cdc, slot_stride, n);
            if (std::abs(w_var_sp)  > 1e-15 && !slot_sp.empty())
                                              score += w_var_sp  * termVarSp(cdc, slot_stride, n, slot_sp);
            if (std::abs(w_rare_id) > 1e-15) score += w_rare_id * termRareId(cdc, slot_stride, n, identified, n_var);
        }
        if (score > best_s) { best_s = score; best = unclicked[ii]; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Phase 1 cell picker — Phase-D fallback
// ---------------------------------------------------------------------------

static int pickPhase1CellPhaseD(
    const BoardSet& bs,
    const std::vector<int32_t>& occ,
    const std::vector<int>& unclicked,
    int n_rare, int ships_hit)
{
    if (unclicked.empty()) return -1;
    int n = bs.n;
    std::vector<int> counts6;
    computeOutcomeCounts6(bs, occ, unclicked, counts6);
    int forced = cpPrefilter(unclicked, counts6, n);
    if (forced >= 0) return forced;

    int nri = std::max(0, std::min(n_rare - 2, 3));
    int d   = std::max(0, std::min(ships_hit, 4));
    double w_info6 = PHASE_D_WEIGHTS[nri][d][0];
    double w_hfull = PHASE_D_WEIGHTS[nri][d][1];

    std::vector<int> counts_dc;
    int slot_stride = 0;
    if (w_hfull > 0.0) computeOutcomeCountsDetailed(bs, occ, unclicked, counts_dc, slot_stride);

    double inv_n = (n > 0) ? 1.0 / n : 0.0;
    int best = unclicked[0]; double best_s = -1e18;
    int nu = (int)unclicked.size();
    for (int ii = 0; ii < nu; ++ii) {
        const int* c6 = &counts6[ii * 6];
        double score  = c6[0] * inv_n;
        if (w_info6 > 0.0) score += w_info6 * termInfo6(c6, n);
        if (w_hfull > 0.0) {
            const int* cdc = &counts_dc[ii * slot_stride];
            score += w_hfull * termHfull(cdc, slot_stride, n);
        }
        if (score > best_s) { best_s = score; best = unclicked[ii]; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Phase 2 — SafeP2
// ---------------------------------------------------------------------------

static int pickSafeP2Cell(
    const BoardSet& bs,
    const std::vector<int32_t>& occ,
    const std::vector<int>& unclicked)
{
    if (unclicked.empty()) return -1;
    int n = bs.n;
    if (n == 0) return unclicked[0];
    int best = unclicked[0], best_blue = n + 1;
    for (int cell : unclicked) {
        int32_t bit = (int32_t)(1 << cell);
        int n_blue = 0;
        for (int row = 0; row < n; ++row)
            if ((occ[row] & bit) == 0) ++n_blue;
        if (n_blue < best_blue) { best_blue = n_blue; best = cell; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Board filtering helpers
// ---------------------------------------------------------------------------

static void filterBlue(BoardSet& bs, int32_t bit) {
    const int32_t* src = bs.data.data();
    int fields = bs.fields, n = bs.n;
    std::vector<int32_t> tmp; tmp.reserve(n * fields);
    int out_n = 0;
    for (int row = 0; row < n; ++row) {
        const int32_t* rowp = src + row * fields;
        int32_t occ = 0;
        for (int col = 0; col < fields; ++col) occ |= rowp[col];
        if ((occ & bit) == 0) { tmp.insert(tmp.end(), rowp, rowp + fields); ++out_n; }
    }
    bs.data = std::move(tmp); bs.n = out_n;
}

static void filterFixed(BoardSet& bs, int32_t bit, int col) {
    const int32_t* src = bs.data.data();
    int fields = bs.fields, n = bs.n;
    std::vector<int32_t> tmp; tmp.reserve(n * fields);
    int out_n = 0;
    for (int row = 0; row < n; ++row) {
        const int32_t* rowp = src + row * fields;
        if (rowp[col] & bit) { tmp.insert(tmp.end(), rowp, rowp + fields); ++out_n; }
    }
    bs.data = std::move(tmp); bs.n = out_n;
}

static void filterVarRare(
    BoardSet& bs, int32_t bit,
    const std::string& color,
    std::unordered_map<std::string, int32_t>& rareColorGroups,
    int n_rare)
{
    int n_var_rare = n_rare - 1;
    if (n_var_rare <= 0) { filterFixed(bs, bit, COL_RARE_START); return; }
    int var_start = COL_RARE_START + 1;

    // Step 1: keep rows where any var-rare column has this bit
    {
        const int32_t* src = bs.data.data();
        int fields = bs.fields, n = bs.n;
        std::vector<int32_t> tmp; tmp.reserve(n * fields);
        int out_n = 0;
        for (int row = 0; row < n; ++row) {
            const int32_t* rowp = src + row * fields;
            bool found = false;
            for (int k = 0; k < n_var_rare; ++k)
                if (rowp[var_start + k] & bit) { found = true; break; }
            if (found) { tmp.insert(tmp.end(), rowp, rowp + fields); ++out_n; }
        }
        bs.data = std::move(tmp); bs.n = out_n;
    }
    if (bs.n == 0) return;

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

    const int32_t* src = bs.data.data();
    int fields = bs.fields, n = bs.n;
    std::vector<int32_t> tmp; tmp.reserve(n * fields);
    int out_n = 0;

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
    for (int row = 0; row < n; ++row) {
        const int32_t* rowp = src + row * fields;
        if (hasPerm(0, 0, rowp)) { tmp.insert(tmp.end(), rowp, rowp + fields); ++out_n; }
    }
    bs.data = std::move(tmp); bs.n = out_n;
}

static void computeOcc(const BoardSet& bs, std::vector<int32_t>& occ) {
    int n = bs.n, fields = bs.fields;
    const int32_t* data = bs.data.data();
    occ.resize(n);
    for (int row = 0; row < n; ++row) {
        const int32_t* rowp = data + row * fields;
        int32_t o = 0;
        for (int col = 0; col < fields; ++col) o |= rowp[col];
        occ[row] = o;
    }
}

static int jsonGetInt(const char* json, const char* key, int def = 0) {
    const char* p = strstr(json, key);
    if (!p) return def;
    p += strlen(key);
    while (*p == ' ' || *p == ':' || *p == '\t') ++p;
    return atoi(p);
}

// ---------------------------------------------------------------------------
// Apply a single reveal to a BoardSet (used in from-scratch rebuild)
// ---------------------------------------------------------------------------

static void applyReveal(BoardSet& arr,
                        std::unordered_map<std::string, int32_t>& rareColorGroups,
                        int cell_idx, const std::string& color, int n_rare)
{
    if (arr.n == 0) return;
    int32_t bit = (int32_t)(1 << cell_idx);
    if      (color == "spB") filterBlue(arr, bit);
    else if (color == "spT") filterFixed(arr, bit, COL_TEAL);
    else if (color == "spG") filterFixed(arr, bit, COL_GREEN);
    else if (color == "spY") filterFixed(arr, bit, COL_YELLOW);
    else if (color == "spO") filterFixed(arr, bit, COL_RARE_START);
    else if (color == "spL" || color == "spD" || color == "spR" || color == "spW")
        filterVarRare(arr, bit, color, rareColorGroups, n_rare);
    // "spU" (still covered) is ignored
}

// ---------------------------------------------------------------------------
// Strategy class — stateless version
// ---------------------------------------------------------------------------

class ColblitzV8StatelessOTStrategy : public OTStrategy {
public:
    // -----------------------------------------------------------------------
    // Run-level state (loaded once, never mutated after init_evaluation_run)
    // -----------------------------------------------------------------------

    BoardSet fullBoards_[4];           // indexed by n_rare - 2
    bool     boardsLoaded_[4] = {};

    std::array<double, V8_N_WEIGHTS> weights_[4];
    bool                              weightsLoaded_[4] = {};

    // -----------------------------------------------------------------------
    // init_evaluation_run: load board arrays and weights
    // -----------------------------------------------------------------------

    void init_evaluation_run() override {
        for (int n_colors = 6; n_colors <= 9; ++n_colors) {
            int n_rare = n_colors - 4;
            int idx    = n_rare - 2;
            int fields = 3 + n_rare;
            std::string path = repoPath(
                "data/sphere_trace_boards_" + std::to_string(n_rare) + ".bin.lzma");
            std::vector<int32_t> raw;
            if (loadLzma(path, raw, fields) && !raw.empty()) {
                int n = (int)(raw.size() / fields);
                fullBoards_[idx].data   = std::move(raw);
                fullBoards_[idx].n      = n;
                fullBoards_[idx].fields = fields;
                boardsLoaded_[idx]      = true;
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
                if (extractWeights(json, n_colors, weights_[idx]))
                    weightsLoaded_[idx] = true;
            }
        }
    }

    // init_game_payload is a no-op — all state is rebuilt from board each call.

    // -----------------------------------------------------------------------
    // next_click — rebuilds filtered board set from scratch on every call
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

        // Build unclicked list; also collect sorted reveals for deterministic
        // filter application order (sort by flat index ascending).
        std::vector<int> unclicked;
        unclicked.reserve(25);
        std::vector<std::pair<int, std::string>> reveals; // (cell_idx, color)
        reveals.reserve(25);

        for (const Cell& c : board) {
            int idx = c.row * GRID + c.col;
            if (c.clicked) {
                reveals.push_back({idx, c.color});
            } else {
                unclicked.push_back(idx);
            }
        }

        // Sort both for determinism (same order as the incremental version,
        // which processes reveals in the order they appear in board iteration,
        // which itself is sorted by cell index by the harness).
        std::sort(reveals.begin(),   reveals.end(),
                  [](const auto& a, const auto& b){ return a.first < b.first; });
        std::sort(unclicked.begin(), unclicked.end());

        if (unclicked.empty()) { out.row = 0; out.col = 0; return; }

        if (!boardsLoaded_[bs_idx]) {
            out.row = unclicked[0] / GRID;
            out.col = unclicked[0] % GRID;
            return;
        }

        // Start from a fresh copy of the full board set for this variant.
        BoardSet arr = fullBoards_[bs_idx];

        // Apply all reveals in cell-index order.
        std::unordered_map<std::string, int32_t> rareColorGroups;
        for (const auto& [cidx, color] : reveals)
            applyReveal(arr, rareColorGroups, cidx, color, n_rare);

        if (arr.n == 0) {
            out.row = unclicked[0] / GRID;
            out.col = unclicked[0] % GRID;
            return;
        }

        std::vector<int32_t> occ;
        computeOcc(arr, occ);

        int chosen = -1;
        if (ships_hit < SHIPS_HIT_THRESHOLD) {
            if (weightsLoaded_[bs_idx]) {
                chosen = pickPhase1CellV8(arr, occ, unclicked, n_rare,
                                          ships_hit, blues_used,
                                          rareColorGroups, weights_[bs_idx]);
            } else {
                chosen = pickPhase1CellPhaseD(arr, occ, unclicked, n_rare, ships_hit);
            }
        } else {
            chosen = pickSafeP2Cell(arr, occ, unclicked);
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

    std::vector<Cell> brd;
    brd.reserve(25);
    const char* p = board_json;
    while ((p = strstr(p, "\"row\":")) != nullptr) {
        Cell c;
        c.row = atoi(p + 6);
        const char* cp   = strstr(p, "\"col\":");    if (cp)   c.col = atoi(cp + 6);
        const char* colp = strstr(p, "\"color\":\"");
        if (colp) {
            colp += 9;
            const char* e = strchr(colp, '"');
            if (e) c.color = std::string(colp, e - colp);
        }
        const char* clkp = strstr(p, "\"clicked\":"); if (clkp) {
            clkp += 10;
            while (*clkp == ' ') ++clkp;
            c.clicked = (strncmp(clkp, "true", 4) == 0);
        }
        brd.push_back(c);
        p += 6;
    }

    ClickResult out;
    s->next_click(brd, meta_json ? meta_json : "{}", out);
    buf = "{\"row\":" + std::to_string(out.row) + ",\"col\":" + std::to_string(out.col) + "}";
    return buf.c_str();
}
