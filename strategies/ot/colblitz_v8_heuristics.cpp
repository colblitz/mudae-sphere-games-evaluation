// Upgrade strategy .so from the default -O2 to -O3 to enable
// auto-vectorization and additional inlining.
#pragma GCC optimize("O3")

/**
 * colblitz_v8_heuristics.cpp — CORTANA_V8 board-filter heuristic for /sphere trace (ot).
 *
 * Port of the Python strategy from:
 *   cortana3/cortana3/strategy/sphere_trace.py  (SphereTraceGame / CORTANA_V8)
 *   MagiBot/magibot/policy/trace.py             (TracePolicy / _SphereTraceGame)
 *
 * Algorithm overview
 * ------------------
 * Maintains a filtered set of surviving board configurations (bit-packed int32
 * ship-placement masks) for the current game.  Each revealed cell eliminates
 * inconsistent boards.  On every click:
 *
 *   Phase 1 (ships_hit < 5, Extra Chance active):
 *     Scores each unclicked cell with a 7-term weighted sum (CORTANA_V8):
 *       score = w0*T_blue + w1*T_info6 + w2*T_hfull + w3*T_ev
 *             + w4*T_gini + w5*T_var_sp + w6*T_rare_id
 *     Weights are indexed by (depth d = ships_hit, blues_used b) and loaded
 *     from data/trace_v8_weights.json.  Falls back to the 3-term Phase-D
 *     heuristic (T_blue + w_info6*T_info6 + w_hfull*T_hfull) when weights
 *     are absent.
 *     CP pre-filter: if any cell is guaranteed blue (n_ship==0 across all
 *     surviving boards) it is clicked first, tiebroken by adj P(ship) sum.
 *
 *   Phase 2 (ships_hit >= 5):
 *     SafeP2: argmin P(blue) = argmin n_blue over all unclicked cells.
 *
 * External data files
 * -------------------
 *   data/sphere_trace_boards_2.bin.lzma  (~874 KB)  n_colors=6
 *   data/sphere_trace_boards_3.bin.lzma  (~3.8 MB)  n_colors=7
 *   data/sphere_trace_boards_4.bin.lzma  (~5.8 MB)  n_colors=8
 *   data/sphere_trace_boards_5.bin.lzma  (~5.5 MB)  n_colors=9
 *   data/trace_v8_weights.json           (~15 KB)   CORTANA_V8 weights
 *
 * Board file format: lzma-compressed stream of little-endian int32 values,
 * (N × fields) row-major where fields = 3 + n_rare.  Column order:
 *   col 0 = teal bitmask (25-bit, length-4 ship)
 *   col 1 = green bitmask (length-3)
 *   col 2 = yellow bitmask (length-3)
 *   col 3 = spO bitmask (fixed rare, length-2, always present)
 *   col 4+ = var-rare bitmask per slot (length-2 each, n_rare-1 slots)
 *
 * Weight layout: w[d*28 + b*7 + t]
 *   d = ships_hit clamped to [0,4]   (5 depths)
 *   b = blues_used clamped to [0,3]  (4 blues levels)
 *   t = term index 0..6
 *
 * Performance notes
 * -----------------
 * - Board arrays are loaded once in init_evaluation_run() and stored flat
 *   (row-major int32 vectors), strided by `fields`.
 * - Per-game state: arr_ is a copy of the full board array reset each game;
 *   reveals are applied incrementally (only new reveals processed each call).
 * - Outcome counts are computed via a single O(N × unclicked) loop with
 *   bitwise ops — no heap allocation per cell in the hot path.
 * - Grouping constraint uses at most 4! = 24 permutation checks.
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
//   0=blue (unused), 1=teal=20, 2=green=35, 3=yellow=55, 4=spO=90, 5+=var-rare (per-mode EV)
static constexpr double SLOT_SP_FIXED[5] = {0.0, 20.0, 35.0, 55.0, 90.0};

// SP values and per-n_colors appearance weights for variable-rare colors.
// Row index = n_colors - 6.  Weights reflect per-mode Mudae appearance rates.
//   6-color: spR/spW absent (~85 SP/cell EV)
//   7-color: all four possible, spL/spD required (~113 SP/cell EV)
//   8-color: all four possible (~132 SP/cell EV)
//   9-color: all four always assigned (~208 SP/cell EV)
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

// A board set: N rows × fields columns, stored row-major as a flat int32 vector.
// Row i, column j = data[i * fields + j].
struct BoardSet {
    std::vector<int32_t> data;
    int n    = 0;      // number of surviving boards
    int fields = 0;   // number of columns (3 + n_rare)
};

// ---------------------------------------------------------------------------
// Minimal lzma decompression via xzcat
// ---------------------------------------------------------------------------

static bool loadLzma(const std::string& path, std::vector<int32_t>& out, int fields) {
    std::string cmd = "xzcat \"" + path + "\"";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return false;

    // Read in 64 KB chunks
    static constexpr size_t CHUNK = 65536;
    std::vector<uint8_t> raw;
    raw.reserve(1 << 22); // 4 MB initial
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
// Finds "\"N\": {...\"weights\": [f0,f1,...,f139]...}" and parses 140 doubles.
// ---------------------------------------------------------------------------

static bool extractWeights(const std::string& json, int n_colors,
                            std::array<double, V8_N_WEIGHTS>& out) {
    // Look for the n_colors key: e.g. "\"6\":"
    std::string key = "\"" + std::to_string(n_colors) + "\"";
    const char* p = strstr(json.c_str(), key.c_str());
    if (!p) return false;
    p += key.size();

    // Find "weights" array within this block
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
// Repo-root path helper — matches REPO_ROOT macro used by the harness
// ---------------------------------------------------------------------------

#ifndef REPO_ROOT
#define REPO_ROOT "."
#endif

static std::string repoPath(const std::string& rel) {
    return std::string(REPO_ROOT) + "/" + rel;
}

// ---------------------------------------------------------------------------
// Outcome counts
// Two variants:
//   computeOutcomeCounts6     — 6-bucket coarse counts per cell
//   computeOutcomeCountsDetailed — per-slot (1 + fields) counts per cell
//
// Both return results packed into flat arrays indexed by position in `unclicked`.
// ---------------------------------------------------------------------------

// counts6[i * 6 + bucket] for i = index into unclicked
static void computeOutcomeCounts6(
    const BoardSet& bs,
    const std::vector<int32_t>& occ,  // per-board OR of all columns, length n
    const std::vector<int>& unclicked,
    std::vector<int>& counts6)        // output: unclicked.size() * 6 ints
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
            if (rowp[COL_TEAL]   & bit) { ++n_teal;   continue; }
            if (rowp[COL_GREEN]  & bit) { ++n_green;  continue; }
            if (rowp[COL_YELLOW] & bit) { ++n_yellow; continue; }
            if (rowp[COL_RARE_START] & bit) { ++n_spo; continue; }
            ++n_var;
        }
        c[0]=n_blue; c[1]=n_teal; c[2]=n_green;
        c[3]=n_yellow; c[4]=n_spo; c[5]=n_var;
    }
}

// counts_dc[i * max_slots + slot] where max_slots = 1 + fields (up to 9 for 9-color)
// slot 0 = blue, slot 1..fields = ship columns
static void computeOutcomeCountsDetailed(
    const BoardSet& bs,
    const std::vector<int32_t>& occ,
    const std::vector<int>& unclicked,
    std::vector<int>& counts_dc,  // output: unclicked.size() * (1+fields) ints
    int& slot_stride)             // output: 1 + fields
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
            // Find which column contains this bit
            for (int col = 0; col < fields; ++col) {
                if (rowp[col] & bit) { ++c[1 + col]; break; }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Score terms (operate on a single cell's count arrays)
// ---------------------------------------------------------------------------

static inline double termInfo6(const int* c6, int n) {
    if (n == 0) return 0.0;
    double inv = 1.0 / n, s = 0.0;
    for (int i = 0; i < 6; ++i) {
        if (c6[i] > 0) { double p = c6[i] * inv; s -= p * std::log(p); }
    }
    return s / LN6;
}

static inline double termHfull(const int* cdc, int slots, int n) {
    if (n == 0) return 0.0;
    double inv = 1.0 / n, s = 0.0;
    for (int i = 0; i < slots; ++i) {
        if (cdc[i] > 0) { double p = cdc[i] * inv; s -= p * std::log(p); }
    }
    return s / LN9;
}

static inline double termEvNorm(const int* cdc, int slots, int n,
                                const std::vector<double>& slot_sp) {
    if (n == 0) return 0.0;
    double inv = 1.0 / n, ev = 0.0;
    int lim = std::min(slots, (int)slot_sp.size());
    for (int i = 1; i < lim; ++i) {
        if (cdc[i] > 0) ev += cdc[i] * inv * slot_sp[i];
    }
    return ev / V8_EV_DENOM;
}

static inline double termGini(const int* cdc, int slots, int n) {
    if (n == 0) return 0.0;
    double inv = 1.0 / n, sq = 0.0;
    for (int i = 0; i < slots; ++i) {
        if (cdc[i] > 0) { double p = cdc[i] * inv; sq += p * p; }
    }
    return 1.0 - sq;
}

static inline double termVarSp(const int* cdc, int slots, int n,
                                const std::vector<double>& slot_sp) {
    if (n == 0) return 0.0;
    double inv = 1.0 / n, ev = 0.0, ev2 = 0.0;
    int lim = std::min(slots, (int)slot_sp.size());
    for (int i = 1; i < lim; ++i) {
        if (cdc[i] > 0) {
            double sp = slot_sp[i];
            double p  = cdc[i] * inv;
            ev  += p * sp;
            ev2 += p * sp * sp;
        }
    }
    return (ev2 - ev * ev) / V8_VAR_DENOM;
}

static inline double termRareId(const int* cdc, int slots, int n,
                                 const std::vector<bool>& identified,
                                 int n_var) {
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
// Identified slots: which var-rare columns are fully bound
// ---------------------------------------------------------------------------

// Returns a bool vector of length n_var: true if slot k is identified.
// A slot is identified when every surviving board that covers any bit of a
// color group covers ALL bits of that group and no group shares two columns.
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
    int fields = bs.fields;
    int n      = bs.n;

    for (int k = 0; k < n_var; ++k) {
        int col = var_start + k;
        for (const auto& kv : rareColorGroups) {
            int32_t bits = kv.second;
            bool all_match = true;
            bool any_found = false;
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

// Build per-slot SP lookup for EV/Var terms
static std::vector<double> buildSlotSp(int n_rare, int n_colors) {
    std::vector<double> sp(SLOT_SP_FIXED, SLOT_SP_FIXED + 5);
    double ev = computeVarRareEV(n_colors);
    for (int k = 0; k < n_rare - 1; ++k) sp.push_back(ev);
    return sp;
}

// ---------------------------------------------------------------------------
// CP pre-filter: guaranteed-blue cells
// ---------------------------------------------------------------------------

// Returns the flat index of the best guaranteed-blue cell, or -1 if none.
// Among guaranteed-blue cells (n_ship == 0), picks by max adj P(ship) sum.
static int cpPrefilter(
    const std::vector<int>& unclicked,
    const std::vector<int>& counts6,  // unclicked.size() * 6
    int n_boards)
{
    if (n_boards == 0) return -1;
    int nu = (int)unclicked.size();
    double inv = 1.0 / n_boards;

    // Build quick lookup: cell -> index in unclicked (-1 if not present)
    // Use a flat array indexed by cell (0..24)
    int cellToIdx[N_CELLS];
    memset(cellToIdx, -1, sizeof(cellToIdx));
    for (int ii = 0; ii < nu; ++ii) cellToIdx[unclicked[ii]] = ii;

    int best_cell = -1;
    double best_adj = -1.0;

    for (int ii = 0; ii < nu; ++ii) {
        const int* c6 = &counts6[ii * 6];
        int n_ship = c6[1] + c6[2] + c6[3] + c6[4] + c6[5];
        if (n_ship != 0) continue;

        // Guaranteed blue: score by sum of P(ship) over unclicked neighbours
        int cell = unclicked[ii];
        uint32_t adj = ADJ_MASK[cell];
        double adj_val = 0.0;
        while (adj) {
            int nb = __builtin_ctz(adj);
            adj &= adj - 1;
            int nbi = cellToIdx[nb];
            if (nbi >= 0) {
                const int* nc6 = &counts6[nbi * 6];
                adj_val += (nc6[1] + nc6[2] + nc6[3] + nc6[4] + nc6[5]) * inv;
            }
        }
        if (best_cell < 0 || adj_val > best_adj) {
            best_adj = adj_val;
            best_cell = cell;
        }
    }
    return best_cell;
}

// ---------------------------------------------------------------------------
// Phase 1 cell picker — CORTANA_V8 (7-term weighted scorer)
// ---------------------------------------------------------------------------

static int pickPhase1CellV8(
    const BoardSet& bs,
    const std::vector<int32_t>& occ,
    const std::vector<int>& unclicked,
    int n_rare,
    int n_colors,
    int ships_hit,
    int blues_used,
    const std::unordered_map<std::string, int32_t>& rareColorGroups,
    const std::array<double, V8_N_WEIGHTS>& weights)
{
    if (unclicked.empty()) return -1;
    int n = bs.n;

    // 6-bucket counts for CP pre-filter and T_blue/T_info6
    std::vector<int> counts6;
    computeOutcomeCounts6(bs, occ, unclicked, counts6);

    // CP pre-filter
    int forced = cpPrefilter(unclicked, counts6, n);
    if (forced >= 0) return forced;

    // Weight lookup
    int d    = std::min(ships_hit, V8_N_DEPTHS - 1);
    int b    = std::min(blues_used, V8_N_BLUES - 1);
    int base = d * 28 + b * 7;

    double w_blue    = weights[base + 0];
    double w_info6   = weights[base + 1];
    double w_hfull   = weights[base + 2];
    double w_ev      = weights[base + 3];
    double w_gini    = weights[base + 4];
    double w_var_sp  = weights[base + 5];
    double w_rare_id = weights[base + 6];

    bool need_dc = (std::abs(w_hfull)   > 1e-15 ||
                    std::abs(w_ev)       > 1e-15 ||
                    std::abs(w_gini)     > 1e-15 ||
                    std::abs(w_var_sp)   > 1e-15 ||
                    std::abs(w_rare_id)  > 1e-15);

    // Detailed counts (only if needed)
    std::vector<int> counts_dc;
    int slot_stride = 0;
    if (need_dc) {
        computeOutcomeCountsDetailed(bs, occ, unclicked, counts_dc, slot_stride);
    }

    // Slot SP and identified slots (only if EV/Var/RareId terms are active)
    std::vector<double> slot_sp;
    std::vector<bool>   identified;
    int n_var = n_rare - 1;
    if (need_dc && (std::abs(w_ev) > 1e-15 || std::abs(w_var_sp) > 1e-15 ||
                    std::abs(w_rare_id) > 1e-15)) {
        identified = getIdentifiedSlots(bs, rareColorGroups, n_rare);
        slot_sp    = buildSlotSp(n_rare, n_colors);
    } else {
        identified.assign(n_var, false);
    }

    double inv_n  = (n > 0) ? 1.0 / n : 0.0;
    int    best   = unclicked[0];
    double best_s = -1e18;
    int nu = (int)unclicked.size();

    for (int ii = 0; ii < nu; ++ii) {
        const int* c6  = &counts6[ii * 6];
        double score   = 0.0;

        if (std::abs(w_blue)  > 1e-15) score += w_blue  * (c6[0] * inv_n);
        if (std::abs(w_info6) > 1e-15) score += w_info6 * termInfo6(c6, n);

        if (need_dc) {
            const int* cdc = &counts_dc[ii * slot_stride];
            if (std::abs(w_hfull)  > 1e-15)
                score += w_hfull  * termHfull(cdc, slot_stride, n);
            if (std::abs(w_ev)     > 1e-15 && !slot_sp.empty())
                score += w_ev     * termEvNorm(cdc, slot_stride, n, slot_sp);
            if (std::abs(w_gini)   > 1e-15)
                score += w_gini   * termGini(cdc, slot_stride, n);
            if (std::abs(w_var_sp) > 1e-15 && !slot_sp.empty())
                score += w_var_sp * termVarSp(cdc, slot_stride, n, slot_sp);
            if (std::abs(w_rare_id) > 1e-15)
                score += w_rare_id * termRareId(cdc, slot_stride, n, identified, n_var);
        }

        if (score > best_s) { best_s = score; best = unclicked[ii]; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Phase 1 cell picker — Phase-D fallback (3-term)
// ---------------------------------------------------------------------------

static int pickPhase1CellPhaseD(
    const BoardSet& bs,
    const std::vector<int32_t>& occ,
    const std::vector<int>& unclicked,
    int n_rare,
    int ships_hit)
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

    // Detailed counts only if w_hfull > 0
    std::vector<int> counts_dc;
    int slot_stride = 0;
    if (w_hfull > 0.0) {
        computeOutcomeCountsDetailed(bs, occ, unclicked, counts_dc, slot_stride);
    }

    double inv_n = (n > 0) ? 1.0 / n : 0.0;
    int    best  = unclicked[0];
    double best_s = -1e18;
    int nu = (int)unclicked.size();

    for (int ii = 0; ii < nu; ++ii) {
        const int* c6 = &counts6[ii * 6];
        double score  = c6[0] * inv_n;   // T_blue (weight=1.0 fixed)
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
// Phase 2 — SafeP2: argmin P(blue) = argmin n_blue
// ---------------------------------------------------------------------------

static int pickSafeP2Cell(
    const BoardSet& bs,
    const std::vector<int32_t>& occ,
    const std::vector<int>& unclicked)
{
    if (unclicked.empty()) return -1;
    int n = bs.n;
    if (n == 0) return unclicked[0];

    const int32_t* data = bs.data.data();
    int fields = bs.fields;

    int best      = unclicked[0];
    int best_blue = n + 1;  // sentinel

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
// Board filtering helpers — modify bs in-place
// ---------------------------------------------------------------------------

static void filterBlue(BoardSet& bs, int32_t bit) {
    // Keep rows where occ & bit == 0 (cell is not in any ship column)
    const int32_t* src  = bs.data.data();
    int fields          = bs.fields;
    int n               = bs.n;
    std::vector<int32_t> tmp;
    tmp.reserve(n * fields);
    int out_n = 0;

    for (int row = 0; row < n; ++row) {
        const int32_t* rowp = src + row * fields;
        // Compute occ for this row
        int32_t occ = 0;
        for (int col = 0; col < fields; ++col) occ |= rowp[col];
        if ((occ & bit) == 0) {
            tmp.insert(tmp.end(), rowp, rowp + fields);
            ++out_n;
        }
    }
    bs.data = std::move(tmp);
    bs.n    = out_n;
}

static void filterFixed(BoardSet& bs, int32_t bit, int col) {
    // Keep rows where data[col] & bit != 0
    const int32_t* src = bs.data.data();
    int fields         = bs.fields;
    int n              = bs.n;
    std::vector<int32_t> tmp;
    tmp.reserve(n * fields);
    int out_n = 0;

    for (int row = 0; row < n; ++row) {
        const int32_t* rowp = src + row * fields;
        if (rowp[col] & bit) {
            tmp.insert(tmp.end(), rowp, rowp + fields);
            ++out_n;
        }
    }
    bs.data = std::move(tmp);
    bs.n    = out_n;
}

// Filter to rows where any var-rare column (col4+) has the bit set,
// then apply the grouping permutation constraint.
static void filterVarRare(
    BoardSet& bs, int32_t bit,
    const std::string& color,
    std::unordered_map<std::string, int32_t>& rareColorGroups,
    int n_rare)
{
    int n_var_rare = n_rare - 1;
    if (n_var_rare <= 0) {
        // 6-color has no variable rares — treat as spO fallback
        filterFixed(bs, bit, COL_RARE_START);
        return;
    }

    int var_start = COL_RARE_START + 1;
    // Step 1: keep rows where any var-rare column has this bit
    {
        const int32_t* src = bs.data.data();
        int fields         = bs.fields;
        int n              = bs.n;
        std::vector<int32_t> tmp;
        tmp.reserve(n * fields);
        int out_n = 0;
        for (int row = 0; row < n; ++row) {
            const int32_t* rowp = src + row * fields;
            bool found = false;
            for (int k = 0; k < n_var_rare; ++k)
                if (rowp[var_start + k] & bit) { found = true; break; }
            if (found) {
                tmp.insert(tmp.end(), rowp, rowp + fields);
                ++out_n;
            }
        }
        bs.data = std::move(tmp);
        bs.n    = out_n;
    }
    if (bs.n == 0) return;

    // Update color group
    int32_t existing = 0;
    auto it = rareColorGroups.find(color);
    if (it != rareColorGroups.end()) existing = it->second;
    rareColorGroups[color] = existing | bit;

    // Step 2: grouping permutation constraint
    // For each surviving board, check whether there exists a permutation of
    // n_var_rare columns that maps each observed color group to a column that
    // is a superset of the group's bits.
    const auto& groups_map = rareColorGroups;
    std::vector<int32_t> group_bits;
    group_bits.reserve(groups_map.size());
    for (const auto& kv : groups_map) group_bits.push_back(kv.second);
    int n_groups = (int)group_bits.size();

    // Enumerate permutations of n_var_rare columns choosing n_groups
    // Max: P(4,4) = 24 permutations
    std::vector<int> perm(n_groups);
    std::vector<bool> used(n_var_rare, false);

    // Collect valid boards
    const int32_t* src = bs.data.data();
    int fields         = bs.fields;
    int n              = bs.n;
    std::vector<int32_t> tmp;
    tmp.reserve(n * fields);
    int out_n = 0;

    // Precompute: for each board row and each (col, group) pair,
    // is (col_data & group_bits) == group_bits?
    // We iterate boards in the outer loop; for each board run backtracking.

    std::function<bool(int, uint32_t, const int32_t*)> hasPerm;
    hasPerm = [&](int g, uint32_t used_mask, const int32_t* rowp) -> bool {
        if (g == n_groups) return true;
        for (int k = 0; k < n_var_rare; ++k) {
            if (used_mask & (1u << k)) continue;
            int32_t colval = rowp[var_start + k];
            if ((colval & group_bits[g]) == group_bits[g]) {
                if (hasPerm(g + 1, used_mask | (1u << k), rowp)) return true;
            }
        }
        return false;
    };

    for (int row = 0; row < n; ++row) {
        const int32_t* rowp = src + row * fields;
        if (hasPerm(0, 0, rowp)) {
            tmp.insert(tmp.end(), rowp, rowp + fields);
            ++out_n;
        }
    }
    bs.data = std::move(tmp);
    bs.n    = out_n;
}

// Compute per-board occ (OR of all columns)
static void computeOcc(const BoardSet& bs, std::vector<int32_t>& occ) {
    int n      = bs.n;
    int fields = bs.fields;
    const int32_t* data = bs.data.data();
    occ.resize(n);
    for (int row = 0; row < n; ++row) {
        const int32_t* rowp = data + row * fields;
        int32_t o = 0;
        for (int col = 0; col < fields; ++col) o |= rowp[col];
        occ[row] = o;
    }
}

// Parse an integer from a JSON string by key, e.g. "\"ships_hit\":"
static int jsonGetInt(const char* json, const char* key, int def = 0) {
    const char* p = strstr(json, key);
    if (!p) return def;
    p += strlen(key);
    while (*p == ' ' || *p == ':' || *p == '\t') ++p;
    return atoi(p);
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------

class ColblitzV8HeuristicsOTStrategy : public OTStrategy {
public:
    // -----------------------------------------------------------------------
    // Run-level state (loaded once in init_evaluation_run)
    // -----------------------------------------------------------------------

    // Full board sets per n_colors variant, indexed by n_rare - 2 (0=n6, 1=n7, 2=n8, 3=n9)
    BoardSet fullBoards_[4];
    bool     boardsLoaded_[4] = {};

    // V8 weight arrays per n_colors variant; indexed by n_rare - 2
    std::array<double, V8_N_WEIGHTS> weights_[4];
    bool                              weightsLoaded_[4] = {};

    // -----------------------------------------------------------------------
    // Per-game state (reset in init_game_payload)
    // -----------------------------------------------------------------------

    BoardSet arr_;           // currently filtered board set for this game
    int      n_rare_ = 2;   // n_colors - 4
    int      fields_ = 5;   // 3 + n_rare

    // Map: observed variable-rare color string -> bitmask of all cells with that color
    std::unordered_map<std::string, int32_t> rareColorGroups_;

    // Bitmask of cell indices already applied to arr_
    uint32_t prevRevealedMask_ = 0;

    // -----------------------------------------------------------------------
    // init_evaluation_run: load board arrays and weights
    // -----------------------------------------------------------------------

    void init_evaluation_run() override {
        for (int n_colors = 6; n_colors <= 9; ++n_colors) {
            int n_rare  = n_colors - 4;
            int idx     = n_rare - 2;
            int fields  = 3 + n_rare;

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

        // Load weights JSON
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

    // -----------------------------------------------------------------------
    // init_game_payload: reset per-game state
    // -----------------------------------------------------------------------

    void init_game_payload(const std::string& meta_json) override {
        int n_colors = jsonGetInt(meta_json.c_str(), "\"n_colors\"", 6);
        n_rare_  = n_colors - 4;
        fields_  = 3 + n_rare_;
        int idx  = n_rare_ - 2;

        rareColorGroups_.clear();
        prevRevealedMask_ = 0;

        if (boardsLoaded_[idx]) {
            arr_ = fullBoards_[idx];  // copy the full board set
        } else {
            arr_.data.clear();
            arr_.n      = 0;
            arr_.fields = fields_;
        }
    }

    // -----------------------------------------------------------------------
    // next_click
    // -----------------------------------------------------------------------

    void next_click(const std::vector<Cell>& board,
                    const std::string& meta_json,
                    ClickResult& out) override
    {
        int ships_hit  = jsonGetInt(meta_json.c_str(), "\"ships_hit\"",  0);
        int blues_used = jsonGetInt(meta_json.c_str(), "\"blues_used\"", 0);
        int n_colors   = jsonGetInt(meta_json.c_str(), "\"n_colors\"",   6);

        // If n_colors changed (shouldn't happen, but guard), reinit game state
        if (n_colors - 4 != n_rare_) {
            std::string meta2 = "{\"n_colors\":" + std::to_string(n_colors) + "}";
            init_game_payload(meta2);
        }

        // Build unclicked list (sorted ascending by flat index for deterministic tiebreak)
        // Also collect new reveals to apply incrementally.
        std::vector<int> unclicked;
        unclicked.reserve(25);

        // Process new reveals: cells that are clicked AND not yet in prevRevealedMask_
        // Apply them to arr_ before scoring.
        for (const Cell& c : board) {
            int idx = c.row * GRID + c.col;
            if (c.clicked) {
                uint32_t bit = 1u << idx;
                if (!(prevRevealedMask_ & bit)) {
                    // New reveal — apply filter
                    applyReveal(idx, c.color);
                    prevRevealedMask_ |= bit;
                }
            } else {
                unclicked.push_back(idx);
            }
        }
        // Sort for deterministic tie-breaking (board order from harness may vary)
        std::sort(unclicked.begin(), unclicked.end());

        if (unclicked.empty()) { out.row = 0; out.col = 0; return; }

        int n = arr_.n;
        if (n == 0) {
            // No consistent boards — fall back to first unclicked cell
            out.row = unclicked[0] / GRID;
            out.col = unclicked[0] % GRID;
            return;
        }

        // Compute per-board occ
        std::vector<int32_t> occ;
        computeOcc(arr_, occ);

        int chosen = -1;
        int idx    = n_rare_ - 2;

        if (ships_hit < SHIPS_HIT_THRESHOLD) {
            // Phase 1
            if (weightsLoaded_[idx]) {
                chosen = pickPhase1CellV8(arr_, occ, unclicked, n_rare_,
                                          n_rare_ + 4,
                                          ships_hit, blues_used,
                                          rareColorGroups_, weights_[idx]);
            } else {
                chosen = pickPhase1CellPhaseD(arr_, occ, unclicked,
                                              n_rare_, ships_hit);
            }
        } else {
            // Phase 2 — SafeP2
            chosen = pickSafeP2Cell(arr_, occ, unclicked);
        }

        if (chosen < 0) chosen = unclicked[0];
        out.row = chosen / GRID;
        out.col = chosen % GRID;
    }

private:
    // Apply a single reveal to arr_ and update rareColorGroups_
    void applyReveal(int cell_idx, const std::string& color) {
        if (arr_.n == 0) return;
        int32_t bit = (int32_t)(1 << cell_idx);

        if (color == "spB") {
            filterBlue(arr_, bit);
        } else if (color == "spT") {
            filterFixed(arr_, bit, COL_TEAL);
        } else if (color == "spG") {
            filterFixed(arr_, bit, COL_GREEN);
        } else if (color == "spY") {
            filterFixed(arr_, bit, COL_YELLOW);
        } else if (color == "spO") {
            filterFixed(arr_, bit, COL_RARE_START);
        } else {
            // Variable rare: spL, spD, spR, spW
            filterVarRare(arr_, bit, color, rareColorGroups_, n_rare_);
        }
        // Unknown/covered colors ("spU") are ignored — cell not yet revealed
    }
};

// ---------------------------------------------------------------------------
// C exports required by the harness
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new ColblitzV8HeuristicsOTStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* inst) {
    static_cast<ColblitzV8HeuristicsOTStrategy*>(inst)->init_evaluation_run();
}

extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {
    static_cast<ColblitzV8HeuristicsOTStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<ColblitzV8HeuristicsOTStrategy*>(inst);

    // Parse board JSON — field order emitted by harness: row/col/color/clicked
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
