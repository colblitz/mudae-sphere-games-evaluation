/**
 * zavex_heuristic_fast.cpp — Optimized C++ port of the heuristic strategy for /sphere trace (ot).
 *
 * Algorithmically identical to zavex_heuristic.cpp (and the JS reference at
 * https://ouro-trace.zavex.workers.dev/).  All decision-tree logic, MCTS book
 * entries, TURN0_PROBS constants, LAMBDA values, and budget=5000 are unchanged.
 *
 * Performance changes vs. zavex_heuristic.cpp
 * -------------------------------------------
 *
 * 1. Color enum (Color : uint8_t)
 *    All std::string color keys replaced with a small integer enum.
 *    - CellProb   : std::map<string,double>   → double[N_COLORS] (indexed by enum)
 *    - ShipEntry  : pair<string,int>          → {Color, int}  (constexpr arrays)
 *    - colorValue : string chain              → constexpr double[] lookup
 *    - isHV       : 4 string comparisons      → range check
 *    - harnessToColor : 8 string comparisons  → switch on sp[2]
 *    - filterPlacements maps                  → int[MAX_SHIPS] parallel arrays
 *    - pShip in perCellProbs                  → double[MAX_SHIPS][25] stack array
 *    - vals / expectedHVValue                 → double[N_COLORS] stack array
 *
 * 2. InsertionOrderCounts replaced by integer-indexed structures
 *    Ship names are numbered 0..nShips-1 (sorted rank).  counts[cell][i] stores
 *    the tally for ship at sorted rank i.  firstCoverOrder[cell][k] records the
 *    sorted-rank index of each ship the first time it covers cell c, replicating
 *    JS Object.entries(counts[c]) insertion-order semantics exactly.  The output
 *    loop iterates firstCoverOrder[c] to accumulate shipTotal in first-cover order
 *    (matching JS), then appends Blue.  Eliminates the O(n) linear string scan of
 *    the original InsertionOrderCounts::inc() inside the inner backtrack leaf loop.
 *
 * 3. Revealed state as bitmask + Color[25]
 *    std::map<int,string> + std::set<int>  →  uint32_t mask + Color[25].
 *    All revealed.count(c) probes → (mask >> c) & 1.
 *    All revealed iteration → bit-loop over mask.
 *    Eliminates tree allocation and per-cell string allocation every call.
 *
 * 4. Single filterPlacements call in maybeExactProbs
 *    The original calls filterPlacements 2-3× per move (upper-bound check,
 *    then again inside exactPerCellProbs, then possibly in perCellProbs fallback).
 *    Now computed once and threaded through.
 *
 * 5. std::function backtrack → BacktrackCtx struct with plain recursive method
 *    Removes type-erased call overhead and enables compiler inlining.
 *
 * 6. Board parsing without std::string per cell
 *    Color is decoded directly into a Color enum via a char switch; no heap
 *    allocation per cell.
 *
 * 7. tryBook early-exit guard
 *    If revealed count > 2, return -1 immediately (no book entry has >2 reveals).
 *
 * JS parity notes (identical to zavex_heuristic.cpp)
 * ---------------------------------------------------
 * - stable_sort for ship ordering by placement count (matches JS Array.sort stability).
 * - InsertionOrderCounts semantics: ship index i in the sorted order == JS
 *   first-cover insertion order (the first board to complete covers cells in
 *   shipNames[0], shipNames[1], ... order, so index = stable-sort rank).
 * - cellEV and giniImpurity iterate ships-list order then Blue, matching JS
 *   Object.entries(d) insertion order for the approx path.
 * - FP parity: #pragma GCC optimize("fp-contract=off") disables FMA to match
 *   JS strict left-to-right IEEE 754.
 */

#pragma GCC optimize("fp-contract=off")

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../interface/strategy.h"

using namespace sphere;

// ---------------------------------------------------------------------------
// Color enum — replaces all std::string color names
// ---------------------------------------------------------------------------

enum Color : uint8_t {
    COL_BLUE    = 0,
    COL_TEAL    = 1,
    COL_GREEN   = 2,
    COL_YELLOW  = 3,
    COL_ORANGE  = 4,
    COL_LIGHT   = 5,
    COL_DARK    = 6,
    COL_RED     = 7,
    COL_RAINBOW = 8,
    COL_SLOT1   = 9,   // u1
    COL_SLOT2   = 10,  // u2
    COL_SLOT3   = 11,  // u3
    COL_UNKNOWN = 15,
    N_COLORS    = 12   // valid indices 0..11
};

// Per-color expected value (mirrors VALUES on the page).
// Light rounded 75.9→76, Dark 104.5→105.
static constexpr double COLOR_VALUE[N_COLORS] = {
    /* BLUE    */ 10.0,
    /* TEAL    */ 20.0,
    /* GREEN   */ 35.0,
    /* YELLOW  */ 55.0,
    /* ORANGE  */ 90.0,
    /* LIGHT   */ 76.0,
    /* DARK    */ 105.0,
    /* RED     */ 150.0,
    /* RAINBOW */ 500.0,
    /* SLOT1   */ 0.0,  // overwritten per-game with real or mean HV value
    /* SLOT2   */ 0.0,
    /* SLOT3   */ 0.0,
};

static inline double colorValue(Color c) {
    if (c < N_COLORS) return COLOR_VALUE[c];
    return 0.0;
}

// HV colors are LIGHT..RAINBOW (inclusive).
static inline bool isHV(Color c) { return c >= COL_LIGHT && c <= COL_RAINBOW; }
static inline bool isSlot(Color c) { return c >= COL_SLOT1 && c <= COL_SLOT3; }

// Harness sp-code → Color enum.  All codes are exactly 3 chars; dispatch on sp[2].
static inline Color harnessToColor(const char* sp) {
    switch (sp[2]) {
        case 'T': return COL_TEAL;
        case 'G': return COL_GREEN;
        case 'Y': return COL_YELLOW;
        case 'O': return COL_ORANGE;
        case 'L': return COL_LIGHT;
        case 'D': return COL_DARK;
        case 'R': return COL_RED;
        case 'W': return COL_RAINBOW;
        case 'B': return COL_BLUE;
        default:  return COL_UNKNOWN;
    }
}

// ---------------------------------------------------------------------------
// CellProb — per-cell color probability distribution, indexed by Color enum.
// Zero-initialized; unused slots remain 0.0.
// Replaces std::map<std::string, double>.
// ---------------------------------------------------------------------------

using CellProb = std::array<double, N_COLORS>;

// ---------------------------------------------------------------------------
// Ship configuration
// ---------------------------------------------------------------------------

static constexpr int MAX_SHIPS = 7;

struct ShipEntry {
    Color name;
    int   size;
};

static constexpr ShipEntry SHIPS5[5] = {
    { COL_TEAL, 4 }, { COL_GREEN, 3 }, { COL_YELLOW, 3 },
    { COL_ORANGE, 2 }, { COL_SLOT1, 2 }
};
static constexpr ShipEntry SHIPS6[6] = {
    { COL_TEAL, 4 }, { COL_GREEN, 3 }, { COL_YELLOW, 3 },
    { COL_ORANGE, 2 }, { COL_SLOT1, 2 }, { COL_SLOT2, 2 }
};
static constexpr ShipEntry SHIPS7[7] = {
    { COL_TEAL, 4 }, { COL_GREEN, 3 }, { COL_YELLOW, 3 },
    { COL_ORANGE, 2 }, { COL_SLOT1, 2 }, { COL_SLOT2, 2 }, { COL_SLOT3, 2 }
};

struct ShipList {
    const ShipEntry* ships;
    int              count;
};

static ShipList shipsByCount(int n) {
    if (n == 5) return { SHIPS5, 5 };
    if (n == 6) return { SHIPS6, 6 };
    if (n == 7) return { SHIPS7, 7 };
    return { nullptr, 0 };
}

// ---------------------------------------------------------------------------
// Revealed state — bitmask + Color[25].
// Replaces std::map<int,string> + std::set<int>.
// ---------------------------------------------------------------------------

struct RevealedState {
    uint32_t mask;      // bit i = cell i is revealed
    Color    col[25];   // COL_UNKNOWN if not revealed

    RevealedState() : mask(0) {
        for (int i = 0; i < 25; ++i) col[i] = COL_UNKNOWN;
    }

    bool has(int c) const { return (mask >> c) & 1; }
    void set(int c, Color color) { mask |= 1u << c; col[c] = color; }
    int  count() const { return __builtin_popcount(mask); }
};

// ---------------------------------------------------------------------------
// Placement bitmasks for sizes 2, 3, 4 — mirrors genPlacements() on the page.
// ---------------------------------------------------------------------------

static std::vector<int> genPlacements(int size) {
    std::vector<int> pl;
    // Horizontal
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c <= 5 - size; ++c) {
            int m = 0;
            for (int i = 0; i < size; ++i) m |= 1 << (r * 5 + c + i);
            pl.push_back(m);
        }
    // Vertical
    for (int c = 0; c < 5; ++c)
        for (int r = 0; r <= 5 - size; ++r) {
            int m = 0;
            for (int i = 0; i < size; ++i) m |= 1 << ((r + i) * 5 + c);
            pl.push_back(m);
        }
    return pl;
}

static const std::vector<int>& PLACEMENTS(int size) {
    static std::vector<int> p2 = genPlacements(2);
    static std::vector<int> p3 = genPlacements(3);
    static std::vector<int> p4 = genPlacements(4);
    if (size == 2) return p2;
    if (size == 3) return p3;
    return p4;
}

// ---------------------------------------------------------------------------
// TURN0_PROBS — precomputed per-cell color probabilities for empty board.
// Already remapped to slot names (COL_SLOT1/2/3).
// Indexed by nShips (5,6,7), then cell 0..24.
// ---------------------------------------------------------------------------

using Turn0ProbEntry = std::array<CellProb, 25>;

// Helper: build a CellProb from an initializer list of (Color, double) pairs.
// Remaining entries stay 0.
static CellProb makeProb(std::initializer_list<std::pair<Color,double>> pairs) {
    CellProb p{};
    for (auto& kv : pairs) p[kv.first] = kv.second;
    return p;
}

static Turn0ProbEntry buildTurn0_5() {
    return {{
        makeProb({{COL_TEAL,0.140828},{COL_SLOT1,0.061543},{COL_ORANGE,0.061543},{COL_YELLOW,0.085884},{COL_GREEN,0.085884},{COL_BLUE,0.564318}}),
        makeProb({{COL_TEAL,0.181477},{COL_GREEN,0.111639},{COL_ORANGE,0.080238},{COL_SLOT1,0.080238},{COL_YELLOW,0.111639},{COL_BLUE,0.434768}}),
        makeProb({{COL_TEAL,0.168702},{COL_GREEN,0.151247},{COL_YELLOW,0.151247},{COL_SLOT1,0.073417},{COL_ORANGE,0.073417},{COL_BLUE,0.381970}}),
        makeProb({{COL_TEAL,0.181477},{COL_ORANGE,0.080238},{COL_SLOT1,0.080238},{COL_GREEN,0.111639},{COL_YELLOW,0.111639},{COL_BLUE,0.434768}}),
        makeProb({{COL_SLOT1,0.061543},{COL_ORANGE,0.061543},{COL_YELLOW,0.085884},{COL_GREEN,0.085884},{COL_TEAL,0.140828},{COL_BLUE,0.564318}}),
        makeProb({{COL_GREEN,0.111639},{COL_SLOT1,0.080238},{COL_ORANGE,0.080238},{COL_YELLOW,0.111639},{COL_TEAL,0.181477},{COL_BLUE,0.434768}}),
        makeProb({{COL_GREEN,0.115050},{COL_ORANGE,0.092617},{COL_SLOT1,0.092617},{COL_YELLOW,0.115050},{COL_TEAL,0.162596},{COL_BLUE,0.422070}}),
        makeProb({{COL_GREEN,0.136647},{COL_YELLOW,0.136647},{COL_SLOT1,0.089523},{COL_ORANGE,0.089523},{COL_TEAL,0.137045},{COL_BLUE,0.410614}}),
        makeProb({{COL_ORANGE,0.092617},{COL_SLOT1,0.092617},{COL_YELLOW,0.115050},{COL_GREEN,0.115050},{COL_TEAL,0.162596},{COL_BLUE,0.422070}}),
        makeProb({{COL_ORANGE,0.080238},{COL_SLOT1,0.080238},{COL_YELLOW,0.111639},{COL_GREEN,0.111639},{COL_TEAL,0.181477},{COL_BLUE,0.434768}}),
        makeProb({{COL_YELLOW,0.151247},{COL_SLOT1,0.073417},{COL_ORANGE,0.073417},{COL_GREEN,0.151247},{COL_TEAL,0.168702},{COL_BLUE,0.381970}}),
        makeProb({{COL_YELLOW,0.136647},{COL_SLOT1,0.089523},{COL_ORANGE,0.089523},{COL_GREEN,0.136647},{COL_TEAL,0.137045},{COL_BLUE,0.410614}}),
        makeProb({{COL_YELLOW,0.151575},{COL_SLOT1,0.089694},{COL_ORANGE,0.089694},{COL_GREEN,0.151575},{COL_TEAL,0.111495},{COL_BLUE,0.405967}}),
        makeProb({{COL_SLOT1,0.089523},{COL_ORANGE,0.089523},{COL_YELLOW,0.136647},{COL_GREEN,0.136647},{COL_TEAL,0.137045},{COL_BLUE,0.410614}}),
        makeProb({{COL_SLOT1,0.073417},{COL_ORANGE,0.073417},{COL_YELLOW,0.151247},{COL_GREEN,0.151247},{COL_TEAL,0.168702},{COL_BLUE,0.381970}}),
        makeProb({{COL_SLOT1,0.080238},{COL_ORANGE,0.080238},{COL_YELLOW,0.111639},{COL_GREEN,0.111639},{COL_TEAL,0.181477},{COL_BLUE,0.434768}}),
        makeProb({{COL_SLOT1,0.092617},{COL_ORANGE,0.092617},{COL_YELLOW,0.115050},{COL_GREEN,0.115050},{COL_TEAL,0.162596},{COL_BLUE,0.422070}}),
        makeProb({{COL_SLOT1,0.089523},{COL_ORANGE,0.089523},{COL_YELLOW,0.136647},{COL_GREEN,0.136647},{COL_TEAL,0.137045},{COL_BLUE,0.410614}}),
        makeProb({{COL_SLOT1,0.092617},{COL_ORANGE,0.092617},{COL_YELLOW,0.115050},{COL_GREEN,0.115050},{COL_TEAL,0.162596},{COL_BLUE,0.422070}}),
        makeProb({{COL_SLOT1,0.080238},{COL_ORANGE,0.080238},{COL_YELLOW,0.111639},{COL_GREEN,0.111639},{COL_TEAL,0.181477},{COL_BLUE,0.434768}}),
        makeProb({{COL_SLOT1,0.061543},{COL_ORANGE,0.061543},{COL_YELLOW,0.085884},{COL_GREEN,0.085884},{COL_TEAL,0.140828},{COL_BLUE,0.564318}}),
        makeProb({{COL_SLOT1,0.080238},{COL_ORANGE,0.080238},{COL_YELLOW,0.111639},{COL_GREEN,0.111639},{COL_TEAL,0.181477},{COL_BLUE,0.434768}}),
        makeProb({{COL_SLOT1,0.073417},{COL_ORANGE,0.073417},{COL_YELLOW,0.151247},{COL_GREEN,0.151247},{COL_TEAL,0.168702},{COL_BLUE,0.381970}}),
        makeProb({{COL_SLOT1,0.080238},{COL_ORANGE,0.080238},{COL_YELLOW,0.111639},{COL_GREEN,0.111639},{COL_TEAL,0.181477},{COL_BLUE,0.434768}}),
        makeProb({{COL_SLOT1,0.061543},{COL_ORANGE,0.061543},{COL_YELLOW,0.085884},{COL_GREEN,0.085884},{COL_TEAL,0.140828},{COL_BLUE,0.564318}}),
    }};
}

static Turn0ProbEntry buildTurn0_6() {
    return {{
        makeProb({{COL_TEAL,0.145630},{COL_SLOT2,0.064362},{COL_SLOT1,0.064362},{COL_ORANGE,0.064362},{COL_YELLOW,0.089414},{COL_GREEN,0.089414},{COL_BLUE,0.482455}}),
        makeProb({{COL_TEAL,0.184360},{COL_GREEN,0.111978},{COL_ORANGE,0.080664},{COL_SLOT1,0.080664},{COL_SLOT2,0.080664},{COL_YELLOW,0.111978},{COL_BLUE,0.349692}}),
        makeProb({{COL_TEAL,0.172539},{COL_GREEN,0.153337},{COL_YELLOW,0.153337},{COL_SLOT1,0.071763},{COL_SLOT2,0.071763},{COL_ORANGE,0.071763},{COL_BLUE,0.305498}}),
        makeProb({{COL_TEAL,0.184360},{COL_ORANGE,0.080664},{COL_SLOT1,0.080664},{COL_SLOT2,0.080664},{COL_GREEN,0.111978},{COL_YELLOW,0.111978},{COL_BLUE,0.349692}}),
        makeProb({{COL_SLOT2,0.064362},{COL_SLOT1,0.064362},{COL_ORANGE,0.064362},{COL_YELLOW,0.089414},{COL_GREEN,0.089414},{COL_TEAL,0.145630},{COL_BLUE,0.482455}}),
        makeProb({{COL_GREEN,0.111978},{COL_SLOT2,0.080664},{COL_SLOT1,0.080664},{COL_ORANGE,0.080664},{COL_YELLOW,0.111978},{COL_TEAL,0.184360},{COL_BLUE,0.349692}}),
        makeProb({{COL_GREEN,0.112862},{COL_ORANGE,0.091593},{COL_SLOT1,0.091593},{COL_SLOT2,0.091593},{COL_YELLOW,0.112862},{COL_TEAL,0.154922},{COL_BLUE,0.344574}}),
        makeProb({{COL_GREEN,0.133547},{COL_YELLOW,0.133547},{COL_SLOT1,0.088672},{COL_SLOT2,0.088672},{COL_ORANGE,0.088672},{COL_TEAL,0.131279},{COL_BLUE,0.335609}}),
        makeProb({{COL_ORANGE,0.091593},{COL_SLOT1,0.091593},{COL_SLOT2,0.091593},{COL_YELLOW,0.112862},{COL_GREEN,0.112862},{COL_TEAL,0.154922},{COL_BLUE,0.344574}}),
        makeProb({{COL_ORANGE,0.080664},{COL_SLOT1,0.080664},{COL_SLOT2,0.080664},{COL_YELLOW,0.111978},{COL_GREEN,0.111978},{COL_TEAL,0.184360},{COL_BLUE,0.349692}}),
        makeProb({{COL_YELLOW,0.153337},{COL_SLOT2,0.071763},{COL_SLOT1,0.071763},{COL_ORANGE,0.071763},{COL_GREEN,0.153337},{COL_TEAL,0.172539},{COL_BLUE,0.305498}}),
        makeProb({{COL_YELLOW,0.133547},{COL_SLOT1,0.088672},{COL_SLOT2,0.088672},{COL_ORANGE,0.088672},{COL_GREEN,0.133547},{COL_TEAL,0.131279},{COL_BLUE,0.335609}}),
        makeProb({{COL_YELLOW,0.147536},{COL_SLOT2,0.089126},{COL_SLOT1,0.089126},{COL_ORANGE,0.089126},{COL_GREEN,0.147536},{COL_TEAL,0.107636},{COL_BLUE,0.329915}}),
        makeProb({{COL_SLOT1,0.088672},{COL_SLOT2,0.088672},{COL_ORANGE,0.088672},{COL_YELLOW,0.133547},{COL_GREEN,0.133547},{COL_TEAL,0.131279},{COL_BLUE,0.335609}}),
        makeProb({{COL_SLOT1,0.071763},{COL_SLOT2,0.071763},{COL_ORANGE,0.071763},{COL_YELLOW,0.153337},{COL_GREEN,0.153337},{COL_TEAL,0.172539},{COL_BLUE,0.305498}}),
        makeProb({{COL_SLOT2,0.080664},{COL_SLOT1,0.080664},{COL_ORANGE,0.080664},{COL_YELLOW,0.111978},{COL_GREEN,0.111978},{COL_TEAL,0.184360},{COL_BLUE,0.349692}}),
        makeProb({{COL_SLOT2,0.091593},{COL_SLOT1,0.091593},{COL_ORANGE,0.091593},{COL_YELLOW,0.112862},{COL_GREEN,0.112862},{COL_TEAL,0.154922},{COL_BLUE,0.344574}}),
        makeProb({{COL_SLOT2,0.088672},{COL_SLOT1,0.088672},{COL_ORANGE,0.088672},{COL_YELLOW,0.133547},{COL_GREEN,0.133547},{COL_TEAL,0.131279},{COL_BLUE,0.335609}}),
        makeProb({{COL_SLOT2,0.091593},{COL_SLOT1,0.091593},{COL_ORANGE,0.091593},{COL_YELLOW,0.112862},{COL_GREEN,0.112862},{COL_TEAL,0.154922},{COL_BLUE,0.344574}}),
        makeProb({{COL_SLOT2,0.080664},{COL_SLOT1,0.080664},{COL_ORANGE,0.080664},{COL_YELLOW,0.111978},{COL_GREEN,0.111978},{COL_TEAL,0.184360},{COL_BLUE,0.349692}}),
        makeProb({{COL_SLOT2,0.064362},{COL_SLOT1,0.064362},{COL_ORANGE,0.064362},{COL_YELLOW,0.089414},{COL_GREEN,0.089414},{COL_TEAL,0.145630},{COL_BLUE,0.482455}}),
        makeProb({{COL_SLOT2,0.080664},{COL_SLOT1,0.080664},{COL_ORANGE,0.080664},{COL_YELLOW,0.111978},{COL_GREEN,0.111978},{COL_TEAL,0.184360},{COL_BLUE,0.349692}}),
        makeProb({{COL_SLOT2,0.071763},{COL_SLOT1,0.071763},{COL_ORANGE,0.071763},{COL_YELLOW,0.153337},{COL_GREEN,0.153337},{COL_TEAL,0.172539},{COL_BLUE,0.305498}}),
        makeProb({{COL_SLOT2,0.080664},{COL_SLOT1,0.080664},{COL_ORANGE,0.080664},{COL_YELLOW,0.111978},{COL_GREEN,0.111978},{COL_TEAL,0.184360},{COL_BLUE,0.349692}}),
        makeProb({{COL_SLOT2,0.064362},{COL_SLOT1,0.064362},{COL_ORANGE,0.064362},{COL_YELLOW,0.089414},{COL_GREEN,0.089414},{COL_TEAL,0.145630},{COL_BLUE,0.482455}}),
    }};
}

static Turn0ProbEntry buildTurn0_7() {
    return {{
        makeProb({{COL_TEAL,0.149505},{COL_SLOT3,0.067624},{COL_SLOT2,0.067624},{COL_SLOT1,0.067624},{COL_ORANGE,0.067624},{COL_YELLOW,0.093475},{COL_GREEN,0.093475},{COL_BLUE,0.393050}}),
        makeProb({{COL_TEAL,0.186623},{COL_GREEN,0.111739},{COL_ORANGE,0.081055},{COL_SLOT1,0.081055},{COL_SLOT2,0.081055},{COL_SLOT3,0.081055},{COL_YELLOW,0.111739},{COL_BLUE,0.265679}}),
        makeProb({{COL_TEAL,0.175765},{COL_GREEN,0.155269},{COL_YELLOW,0.155269},{COL_SLOT1,0.069742},{COL_SLOT2,0.069742},{COL_SLOT3,0.069742},{COL_ORANGE,0.069742},{COL_BLUE,0.234730}}),
        makeProb({{COL_TEAL,0.186623},{COL_ORANGE,0.081055},{COL_SLOT1,0.081055},{COL_SLOT2,0.081055},{COL_SLOT3,0.081055},{COL_GREEN,0.111739},{COL_YELLOW,0.111739},{COL_BLUE,0.265679}}),
        makeProb({{COL_SLOT3,0.067624},{COL_SLOT2,0.067624},{COL_SLOT1,0.067624},{COL_ORANGE,0.067624},{COL_YELLOW,0.093475},{COL_GREEN,0.093475},{COL_TEAL,0.149505},{COL_BLUE,0.393050}}),
        makeProb({{COL_GREEN,0.111739},{COL_SLOT3,0.081055},{COL_SLOT2,0.081055},{COL_SLOT1,0.081055},{COL_ORANGE,0.081055},{COL_YELLOW,0.111739},{COL_TEAL,0.186623},{COL_BLUE,0.265679}}),
        makeProb({{COL_GREEN,0.111009},{COL_ORANGE,0.090570},{COL_SLOT1,0.090570},{COL_SLOT2,0.090570},{COL_SLOT3,0.090570},{COL_YELLOW,0.111009},{COL_TEAL,0.148471},{COL_BLUE,0.267233}}),
        makeProb({{COL_GREEN,0.130750},{COL_YELLOW,0.130750},{COL_SLOT1,0.087890},{COL_SLOT2,0.087890},{COL_SLOT3,0.087890},{COL_ORANGE,0.087890},{COL_TEAL,0.126754},{COL_BLUE,0.260184}}),
        makeProb({{COL_ORANGE,0.090570},{COL_SLOT1,0.090570},{COL_SLOT2,0.090570},{COL_SLOT3,0.090570},{COL_YELLOW,0.111009},{COL_GREEN,0.111009},{COL_TEAL,0.148471},{COL_BLUE,0.267233}}),
        makeProb({{COL_ORANGE,0.081055},{COL_SLOT1,0.081055},{COL_SLOT2,0.081055},{COL_SLOT3,0.081055},{COL_YELLOW,0.111739},{COL_GREEN,0.111739},{COL_TEAL,0.186623},{COL_BLUE,0.265679}}),
        makeProb({{COL_YELLOW,0.155269},{COL_SLOT3,0.069742},{COL_SLOT2,0.069742},{COL_SLOT1,0.069742},{COL_ORANGE,0.069742},{COL_GREEN,0.155269},{COL_TEAL,0.175765},{COL_BLUE,0.234730}}),
        makeProb({{COL_YELLOW,0.130750},{COL_SLOT1,0.087890},{COL_SLOT2,0.087890},{COL_SLOT3,0.087890},{COL_ORANGE,0.087890},{COL_GREEN,0.130750},{COL_TEAL,0.126754},{COL_BLUE,0.260184}}),
        makeProb({{COL_YELLOW,0.144076},{COL_SLOT2,0.088257},{COL_SLOT3,0.088257},{COL_SLOT1,0.088257},{COL_ORANGE,0.088257},{COL_GREEN,0.144076},{COL_TEAL,0.105037},{COL_BLUE,0.253781}}),
        makeProb({{COL_SLOT1,0.087890},{COL_SLOT2,0.087890},{COL_SLOT3,0.087890},{COL_ORANGE,0.087890},{COL_YELLOW,0.130750},{COL_GREEN,0.130750},{COL_TEAL,0.126754},{COL_BLUE,0.260184}}),
        makeProb({{COL_SLOT1,0.069742},{COL_SLOT2,0.069742},{COL_SLOT3,0.069742},{COL_ORANGE,0.069742},{COL_YELLOW,0.155269},{COL_GREEN,0.155269},{COL_TEAL,0.175765},{COL_BLUE,0.234730}}),
        makeProb({{COL_SLOT2,0.081055},{COL_SLOT3,0.081055},{COL_SLOT1,0.081055},{COL_ORANGE,0.081055},{COL_YELLOW,0.111739},{COL_GREEN,0.111739},{COL_TEAL,0.186623},{COL_BLUE,0.265679}}),
        makeProb({{COL_SLOT2,0.090570},{COL_SLOT3,0.090570},{COL_SLOT1,0.090570},{COL_ORANGE,0.090570},{COL_YELLOW,0.111009},{COL_GREEN,0.111009},{COL_TEAL,0.148471},{COL_BLUE,0.267233}}),
        makeProb({{COL_SLOT3,0.087890},{COL_SLOT2,0.087890},{COL_SLOT1,0.087890},{COL_ORANGE,0.087890},{COL_YELLOW,0.130750},{COL_GREEN,0.130750},{COL_TEAL,0.126754},{COL_BLUE,0.260184}}),
        makeProb({{COL_SLOT3,0.090570},{COL_SLOT2,0.090570},{COL_SLOT1,0.090570},{COL_ORANGE,0.090570},{COL_YELLOW,0.111009},{COL_GREEN,0.111009},{COL_TEAL,0.148471},{COL_BLUE,0.267233}}),
        makeProb({{COL_SLOT3,0.081055},{COL_SLOT2,0.081055},{COL_SLOT1,0.081055},{COL_ORANGE,0.081055},{COL_YELLOW,0.111739},{COL_GREEN,0.111739},{COL_TEAL,0.186623},{COL_BLUE,0.265679}}),
        makeProb({{COL_SLOT3,0.067624},{COL_SLOT2,0.067624},{COL_SLOT1,0.067624},{COL_ORANGE,0.067624},{COL_YELLOW,0.093475},{COL_GREEN,0.093475},{COL_TEAL,0.149505},{COL_BLUE,0.393050}}),
        makeProb({{COL_SLOT3,0.081055},{COL_SLOT2,0.081055},{COL_SLOT1,0.081055},{COL_ORANGE,0.081055},{COL_YELLOW,0.111739},{COL_GREEN,0.111739},{COL_TEAL,0.186623},{COL_BLUE,0.265679}}),
        makeProb({{COL_SLOT3,0.069742},{COL_SLOT2,0.069742},{COL_SLOT1,0.069742},{COL_ORANGE,0.069742},{COL_YELLOW,0.155269},{COL_GREEN,0.155269},{COL_TEAL,0.175765},{COL_BLUE,0.234730}}),
        makeProb({{COL_SLOT3,0.081055},{COL_SLOT2,0.081055},{COL_SLOT1,0.081055},{COL_ORANGE,0.081055},{COL_YELLOW,0.111739},{COL_GREEN,0.111739},{COL_TEAL,0.186623},{COL_BLUE,0.265679}}),
        makeProb({{COL_SLOT3,0.067624},{COL_SLOT2,0.067624},{COL_SLOT1,0.067624},{COL_ORANGE,0.067624},{COL_YELLOW,0.093475},{COL_GREEN,0.093475},{COL_TEAL,0.149505},{COL_BLUE,0.393050}}),
    }};
}

// ---------------------------------------------------------------------------
// MCTS_BOOK — verbatim from the page.
// Keys: nShips → map of "cell,ColorName[;cell,ColorName]" → recommended cell.
// Color names in the key use the original string names (not enum), because
// the keys are literal string constants from the page.
// ---------------------------------------------------------------------------

using MctsBook  = std::unordered_map<std::string, int>;
using MctsBooks = std::unordered_map<int, MctsBook>;

static MctsBooks buildMctsBooks() {
    MctsBooks books;

    books[5] = {
        {"0,Blue", 1},
        {"0,Blue;11,Yellow", 21},
        {"0,Blue;12,Yellow", 14},
        {"0,Green", 4},
        {"0,Green;12,Orange", 17},
        {"0,Green;4,Orange", 9},
        {"0,Green;4,Yellow", 3},
        {"0,Orange", 4},
        {"0,Orange;12,Teal", 1},
        {"0,Orange;12,Yellow", 5},
        {"0,Orange;4,Yellow", 1},
        {"0,Teal", 4},
        {"0,Teal;11,Green", 6},
        {"0,Teal;11,Orange", 6},
        {"0,Teal;11,Yellow", 13},
        {"0,Teal;11,u1", 12},
        {"0,Teal;12,Green", 7},
        {"0,Teal;12,Orange", 7},
        {"0,Teal;12,Yellow", 17},
        {"0,Teal;7,Green", 13},
        {"0,Teal;7,Orange", 8},
        {"0,Teal;7,Yellow", 17},
        {"0,Yellow", 4},
        {"0,u1", 4},
        {"0,u1;12,Teal", 8},
    };

    books[6] = {
        {"3,Blue;8,Orange", 7},
        {"3,Orange;8,Orange", 12},
        {"3,Teal;8,Orange", 11},
        {"3,u1;8,Orange", 2},
        {"5,Green;8,Teal", 15},
        {"5,Teal;8,Teal", 16},
        {"5,Yellow;8,Teal", 7},
        {"5,u1;8,Teal", 23},
        {"6,Blue;8,Green", 16},
        {"6,Green;8,Yellow", 18},
        {"6,Yellow;8,Green", 18},
        {"6,Yellow;8,Yellow", 2},
        {"7,Blue;8,Orange", 3},
        {"7,Blue;8,Yellow", 17},
        {"7,Orange;8,Orange", 14},
        {"7,Teal;8,Orange", 9},
        {"7,Teal;8,Yellow", 22},
        {"7,Teal;8,u1", 9},
        {"7,Yellow;8,Yellow", 9},
        {"7,u1;8,u1", 11},
        {"7,u1;8,u2", 9},
        {"8,Blue", 18},
        {"8,Blue;11,Blue", 18},
        {"8,Blue;11,Green", 21},
        {"8,Blue;11,Teal", 18},
        {"8,Blue;12,Yellow", 10},
        {"8,Blue;17,Blue", 6},
        {"8,Blue;17,Green", 15},
        {"8,Blue;17,Teal", 15},
        {"8,Blue;17,Yellow", 15},
        {"8,Blue;18,Green", 6},
        {"8,Blue;18,Teal", 6},
        {"8,Blue;18,u1", 19},
        {"8,Green", 11},
        {"8,Green;11,Blue", 22},
        {"8,Green;11,Yellow", 18},
        {"8,Green;11,u1", 18},
        {"8,Green;12,Blue", 15},
        {"8,Green;12,Teal", 10},
        {"8,Green;18,Blue", 16},
        {"8,Green;18,Green", 11},
        {"8,Green;18,Yellow", 11},
        {"8,Orange", 3},
        {"8,Orange;12,Blue", 3},
        {"8,Orange;12,Green", 17},
        {"8,Orange;12,Teal", 10},
        {"8,Orange;12,u1", 13},
        {"8,Orange;13,Teal", 3},
        {"8,Orange;13,Yellow", 3},
        {"8,Teal", 5},
        {"8,Teal;11,Blue", 5},
        {"8,Teal;11,Green", 13},
        {"8,Teal;11,u1", 16},
        {"8,Teal;12,Blue", 23},
        {"8,Teal;17,Blue", 23},
        {"8,Teal;17,Orange", 18},
        {"8,Teal;17,Yellow", 7},
        {"8,Teal;17,u1", 18},
        {"8,Yellow", 6},
        {"8,Yellow;18,Blue", 3},
        {"8,Yellow;18,Green", 11},
        {"8,Yellow;18,Orange", 11},
        {"8,Yellow;18,Yellow", 2},
        {"8,Yellow;18,u1", 17},
        {"8,u1", 9},
        {"8,u1;12,Blue", 9},
        {"8,u1;12,Orange", 13},
        {"8,u1;12,u2", 7},
        {"8,u1;13,Blue", 16},
        {"8,u1;13,Green", 11},
        {"8,u1;13,Orange", 3},
        {"8,u1;13,Teal", 3},
        {"8,u1;13,Yellow", 7},
        {"8,u1;9,Blue", 13},
        {"8,u1;9,u1", 2},
        {"8,u1;9,u2", 14},
    };

    books[7] = {
        {"2,Blue;8,Blue", 6},
        {"2,Green;8,Blue", 6},
        {"2,Orange;8,Blue", 11},
        {"2,Teal;8,Blue", 6},
        {"2,u1;8,Blue", 12},
        {"6,Blue;8,Green", 2},
        {"6,Green;8,Green", 17},
        {"6,Teal;8,Green", 21},
        {"6,Yellow;8,Yellow", 16},
        {"6,u1;8,Green", 7},
        {"6,u1;8,Yellow", 11},
        {"7,Blue;8,Orange", 11},
        {"7,Blue;8,u1", 3},
        {"7,Green;8,u1", 5},
        {"7,Orange;8,Orange", 16},
        {"7,Teal;8,u1", 3},
        {"7,Yellow;8,Orange", 17},
        {"7,u1;8,u1", 18},
        {"7,u1;8,u2", 13},
        {"8,Blue", 2},
        {"8,Blue;11,Blue", 12},
        {"8,Blue;11,Green", 1},
        {"8,Blue;11,Orange", 18},
        {"8,Blue;11,u1", 16},
        {"8,Blue;12,Green", 6},
        {"8,Blue;12,Orange", 18},
        {"8,Blue;12,u1", 11},
        {"8,Blue;17,Blue", 6},
        {"8,Blue;17,Green", 15},
        {"8,Blue;17,Yellow", 15},
        {"8,Blue;17,u1", 6},
        {"8,Green", 6},
        {"8,Green;12,Teal", 6},
        {"8,Green;12,Yellow", 18},
        {"8,Green;12,u1", 17},
        {"8,Green;18,Green", 16},
        {"8,Green;18,Orange", 13},
        {"8,Green;18,u1", 13},
        {"8,Orange", 13},
        {"8,Orange;12,Blue", 9},
        {"8,Orange;12,Green", 13},
        {"8,Teal", 12},
        {"8,Teal;11,Blue", 5},
        {"8,Teal;11,Orange", 18},
        {"8,Teal;11,u1", 16},
        {"8,Teal;12,Green", 6},
        {"8,Teal;17,Blue", 23},
        {"8,Teal;17,Green", 16},
        {"8,Yellow", 16},
        {"8,Yellow;12,Teal", 18},
        {"8,Yellow;12,u1", 18},
        {"8,Yellow;16,Blue", 18},
        {"8,Yellow;16,u1", 17},
        {"8,Yellow;18,Yellow", 16},
        {"8,Yellow;18,u1", 17},
        {"8,u1", 7},
        {"8,u1;11,Green", 3},
        {"8,u1;11,u2", 12},
        {"8,u1;12,Blue", 9},
        {"8,u1;12,Green", 7},
        {"8,u1;12,Orange", 7},
        {"8,u1;12,Yellow", 13},
        {"8,u1;12,u2", 7},
        {"8,u1;13,Blue", 17},
        {"8,u1;13,Green", 17},
        {"8,u1;13,Orange", 12},
        {"8,u1;13,Teal", 17},
        {"8,u1;13,Yellow", 23},
        {"8,u1;13,u1", 11},
        {"8,u1;13,u2", 12},
    };

    return books;
}

static const MctsBooks MCTS_BOOK = buildMctsBooks();

// Map Color enum → canonical name string for MCTS book key construction.
static const char* colorName(Color c) {
    switch (c) {
        case COL_BLUE:    return "Blue";
        case COL_TEAL:    return "Teal";
        case COL_GREEN:   return "Green";
        case COL_YELLOW:  return "Yellow";
        case COL_ORANGE:  return "Orange";
        case COL_LIGHT:   return "Light";
        case COL_DARK:    return "Dark";
        case COL_RED:     return "Red";
        case COL_RAINBOW: return "Rainbow";
        case COL_SLOT1:   return "u1";
        case COL_SLOT2:   return "u2";
        case COL_SLOT3:   return "u3";
        default:          return "";
    }
}

// ---------------------------------------------------------------------------
// HV bindings — map first HV color seen → u1/u2/u3 slot.
// Replaces getBindings() which used string maps.
// ---------------------------------------------------------------------------

struct Bindings {
    // slotToReal[0] = real Color of slot u1 (COL_UNKNOWN if unbound)
    Color slotToReal[3];  // indexed 0=u1, 1=u2, 2=u3
    int   nextSlot;       // 0..3: how many HV colors have been bound

    Bindings() : nextSlot(0) {
        slotToReal[0] = slotToReal[1] = slotToReal[2] = COL_UNKNOWN;
    }

    // Which slot does this real HV color map to? COL_UNKNOWN if not bound yet.
    Color realToSlot(Color c) const {
        for (int i = 0; i < nextSlot; ++i)
            if (slotToReal[i] == c) return (Color)(COL_SLOT1 + i);
        return COL_UNKNOWN;
    }
};

static Bindings getBindings(const RevealedState& rev) {
    Bindings b;
    for (int m = rev.mask; m; m &= m - 1) {
        int c = __builtin_ctz(m);
        Color col = rev.col[c];
        if (!isHV(col)) continue;
        // Check if already bound
        bool found = false;
        for (int i = 0; i < b.nextSlot; ++i)
            if (b.slotToReal[i] == col) { found = true; break; }
        if (!found && b.nextSlot < 3) {
            b.slotToReal[b.nextSlot++] = col;
        }
    }
    return b;
}

// Build a solver-view RevealedState with HV colors replaced by their slots.
static RevealedState toSolverReveals(const RevealedState& rev, const Bindings& b) {
    RevealedState out;
    out.mask = rev.mask;
    for (int m = rev.mask; m; m &= m - 1) {
        int c = __builtin_ctz(m);
        Color col = rev.col[c];
        if (isHV(col)) {
            Color slot = b.realToSlot(col);
            out.col[c] = (slot != COL_UNKNOWN) ? slot : col;
        } else {
            out.col[c] = col;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// expectedHVValue — mean value of HV colors not yet revealed.
// ---------------------------------------------------------------------------

static double expectedHVValue(const RevealedState& rev) {
    // Which HV colors (LIGHT, DARK, RED, RAINBOW) appear in revealed?
    static constexpr Color HV[4] = { COL_LIGHT, COL_DARK, COL_RED, COL_RAINBOW };
    double sum = 0; int cnt = 0;
    for (int i = 0; i < 4; ++i) {
        // Check if this HV color appears anywhere in revealed
        bool seen = false;
        for (int m = rev.mask; m; m &= m - 1)
            if (rev.col[__builtin_ctz(m)] == HV[i]) { seen = true; break; }
        if (!seen) { sum += colorValue(HV[i]); ++cnt; }
    }
    return cnt > 0 ? sum / cnt : 0.0;
}

// ---------------------------------------------------------------------------
// filterPlacements — mirrors filterPlacements() on the page.
// Returns per-ship valid placement lists, indexed by ship position 0..nShips-1.
// ---------------------------------------------------------------------------

struct FilterResult {
    std::vector<int> lists[MAX_SHIPS];
};

static FilterResult filterPlacements(const RevealedState& rev, const ShipList& sl) {
    FilterResult res;
    int nShips = sl.count;

    // Build per-ship must/avoid bitmasks
    int revealSelf[MAX_SHIPS]  = {};
    int revealOther[MAX_SHIPS] = {};

    for (int m = rev.mask; m; m &= m - 1) {
        int cell = __builtin_ctz(m);
        int bit  = 1 << cell;
        Color col = rev.col[cell];
        if (col == COL_BLUE) {
            for (int i = 0; i < nShips; ++i) revealOther[i] |= bit;
        } else {
            // Find which ship this color belongs to
            for (int i = 0; i < nShips; ++i) {
                if (sl.ships[i].name == col) revealSelf[i] |= bit;
                else                         revealOther[i] |= bit;
            }
        }
    }

    for (int i = 0; i < nShips; ++i) {
        int must  = revealSelf[i];
        int avoid = revealOther[i];
        for (int p : PLACEMENTS(sl.ships[i].size)) {
            if ((p & must) == must && (p & avoid) == 0)
                res.lists[i].push_back(p);
        }
    }
    return res;
}

// ---------------------------------------------------------------------------
// perCellProbs — independence approximation (fallback).
// Takes pre-computed FilterResult to avoid redundant filterPlacements call.
// ---------------------------------------------------------------------------

static std::array<CellProb, 25> perCellProbs(
    const RevealedState& rev,
    const ShipList& sl,
    const FilterResult& filt)
{
    int nShips = sl.count;

    // Per-ship per-cell probability — plain 2D stack array, no allocation
    double pShip[MAX_SHIPS][25] = {};
    for (int i = 0; i < nShips; ++i) {
        const auto& vl = filt.lists[i];
        int n = (int)vl.size();
        if (n > 0) {
            double invN = 1.0 / n;
            for (int p : vl)
                for (int b = p; b; b &= b - 1)
                    pShip[i][__builtin_ctz(b)] += invN;
        }
    }

    std::array<CellProb, 25> out{};
    for (int c = 0; c < 25; ++c) {
        if (rev.has(c)) continue;  // empty CellProb (all zeros)
        double pBlue   = 1.0;
        double shipSum = 0.0;
        for (int i = 0; i < nShips; ++i) {
            double ps = pShip[i][c];
            pBlue   *= (1.0 - ps);
            shipSum += ps;
        }
        double total = shipSum + pBlue;
        if (total <= 0.0) { out[c][COL_BLUE] = 1.0; continue; }
        double scale = 1.0 / total;
        // Iterate ships first (insertion order), then Blue — matches JS.
        for (int i = 0; i < nShips; ++i) {
            double v = pShip[i][c] * scale;
            if (v > 1e-9) out[c][sl.ships[i].name] = v;
        }
        out[c][COL_BLUE] = pBlue * scale;
    }
    return out;
}

// ---------------------------------------------------------------------------
// exactPerCellProbs — backtracking joint enumeration with budget cap.
// Mirrors exactPerCellProbs() on the page.
//
// InsertionOrderCounts semantics: the JS uses Object.entries(counts[c]) in
// first-cover order.  We replicate this with firstCoverOrder[cell][k], which
// records the sorted-rank index of each ship the first time it covers cell c
// across all completed boards.  Iterating in this order when accumulating
// shipTotal = Σ(cnt/total) matches JS floating-point accumulation exactly,
// preserving the last-bit value of d[BLUE] = 1 - shipTotal.
// ---------------------------------------------------------------------------

struct ExactResult {
    std::array<CellProb, 25> probs;
    long total;
    bool aborted;
};

// BacktrackCtx replaces std::function — plain struct, no heap closure.
//
// firstCoverOrder[cell][k] = sorted-rank index of the k-th ship to first cover `cell`
// across all completed boards.  Mirrors JS Object.entries(counts[c]) insertion order,
// which is determined by the first board in which each ship covers that cell.
//
// Because the backtracker iterates ships in sorted-rank order (i=0,1,...,nShips-1)
// and each board covers every cell by exactly one ship, the first time a given
// ship-index i covers cell c determines i's position in firstCoverOrder[c].
struct BacktrackCtx {
    int           nShips;
    const std::vector<int>* placementLists;  // [nShips]
    int           assignment[MAX_SHIPS];
    // counts[cell][ship_index] — raw tallies, indexed by sorted rank
    long          counts[25][MAX_SHIPS];
    // firstCoverOrder[cell][k] — sorted-rank index of the k-th first-seen ship for cell
    uint8_t       firstCoverOrder[25][MAX_SHIPS];
    uint8_t       firstCoverCount[25];   // how many ships have ever covered cell c
    long          total;
    long          budget;
    bool          aborted;

    void run(int idx, int occ) {
        if (aborted) return;
        if (idx == nShips) {
            ++total;
            if (total > budget) { aborted = true; return; }
            for (int i = 0; i < nShips; ++i) {
                int m = assignment[i];
                for (int b = m; b; b &= b - 1) {
                    int cell = __builtin_ctz(b);
                    if (counts[cell][i] == 0) {
                        // First time this ship covers this cell: record insertion order
                        firstCoverOrder[cell][firstCoverCount[cell]++] = (uint8_t)i;
                    }
                    counts[cell][i]++;
                }
            }
            return;
        }
        for (int p : placementLists[idx]) {
            if (p & occ) continue;
            assignment[idx] = p;
            run(idx + 1, occ | p);
            if (aborted) return;
        }
    }
};

// Takes pre-computed FilterResult to avoid double-computing filterPlacements.
static ExactResult exactPerCellProbs(
    const RevealedState& rev,
    const ShipList& sl,
    const FilterResult& filt,
    int budget)
{
    int nShips = sl.count;
    for (int i = 0; i < nShips; ++i)
        if (filt.lists[i].empty()) return { {}, 0, true };

    // Sort ships by number of valid placements ascending (tightest constraint first).
    // JS Array.sort() is stable; use stable_sort to match tie-breaking behaviour.
    int order[MAX_SHIPS];
    for (int i = 0; i < nShips; ++i) order[i] = i;
    std::stable_sort(order, order + nShips, [&](int a, int b) {
        return filt.lists[a].size() < filt.lists[b].size();
    });

    // Build sorted placement lists and ship name array (in sorted order).
    std::vector<int> sortedLists[MAX_SHIPS];
    Color             shipNames[MAX_SHIPS];
    for (int i = 0; i < nShips; ++i) {
        sortedLists[i] = filt.lists[order[i]];
        shipNames[i]   = sl.ships[order[i]].name;
    }

    BacktrackCtx ctx;
    ctx.nShips = nShips;
    ctx.placementLists = sortedLists;
    std::memset(ctx.counts, 0, sizeof(ctx.counts));
    std::memset(ctx.firstCoverCount, 0, sizeof(ctx.firstCoverCount));
    ctx.total   = 0;
    ctx.budget  = budget;
    ctx.aborted = false;

    ctx.run(0, 0);

    if (ctx.aborted || ctx.total == 0)
        return { {}, ctx.total, ctx.aborted };

    std::array<CellProb, 25> out{};
    for (int c = 0; c < 25; ++c) {
        if (rev.has(c)) continue;
        // Iterate ships in first-cover insertion order, then Blue.
        // This mirrors JS Object.entries(counts[c]) which returns keys in
        // first-insertion order — i.e. the order each ship first covered cell c
        // across all completed boards.  The summation order of shipTotal matters
        // for the last-bit value of d[BLUE] = 1 - shipTotal, which drives
        // min-P(blue) comparisons in the danger zone.
        double shipTotal = 0;
        for (int k = 0; k < ctx.firstCoverCount[c]; ++k) {
            int i = ctx.firstCoverOrder[c][k];
            double v = (double)ctx.counts[c][i] / (double)ctx.total;
            out[c][shipNames[i]] = v;
            shipTotal += v;
        }
        out[c][COL_BLUE] = std::max(0.0, 1.0 - shipTotal);
    }
    return { out, ctx.total, false };
}

// ---------------------------------------------------------------------------
// maybeExactProbs — turn-0 fast path + exact + independence fallback.
// Mirrors maybeExactProbs() on the page.
// filterPlacements is computed once here and threaded through both paths.
// ---------------------------------------------------------------------------

struct ProbResult {
    std::array<CellProb, 25> probs;
    bool exact;
};

static ProbResult maybeExactProbs(
    const RevealedState& rev,
    const ShipList& sl,
    int budget = 5000)
{
    // Turn-0 fast path
    if (rev.mask == 0) {
        int n = sl.count;
        static Turn0ProbEntry t5 = buildTurn0_5();
        static Turn0ProbEntry t6 = buildTurn0_6();
        static Turn0ProbEntry t7 = buildTurn0_7();
        if (n == 5) return { t5, true };
        if (n == 6) return { t6, true };
        if (n == 7) return { t7, true };
    }

    // Compute filterPlacements once — reused for upper-bound, exact, and fallback.
    FilterResult filt = filterPlacements(rev, sl);

    // Upper-bound check: only attempt exact when board count is small
    int nShips = sl.count;
    long upper = 1;
    for (int i = 0; i < nShips; ++i) {
        long vl = (long)filt.lists[i].size();
        if (vl == 0) return { perCellProbs(rev, sl, filt), false };
        upper *= vl;
        if (upper > (long)(50 * budget)) return { perCellProbs(rev, sl, filt), false };
    }

    auto res = exactPerCellProbs(rev, sl, filt, budget);
    if (res.aborted || res.total == 0)
        return { perCellProbs(rev, sl, filt), false };
    return { res.probs, true };
}

// ---------------------------------------------------------------------------
// cellEV — expected value of clicking a cell.
// Iterates ships-list order then Blue — matches JS Object.entries(d) insertion
// order for the approx path (preserves bit-identical results).
// ---------------------------------------------------------------------------

static double cellEV(const CellProb& d,
                     const double vals[N_COLORS],
                     const ShipList& sl)
{
    double s = 0;
    for (int i = 0; i < sl.count; ++i) {
        Color c = sl.ships[i].name;
        s += d[c] * vals[c];
    }
    s += d[COL_BLUE] * vals[COL_BLUE];
    return s;
}

// ---------------------------------------------------------------------------
// giniImpurity — 1 − Σ P(outcome)².
// Iterates ships-list order then Blue, consistent with cellEV.
// ---------------------------------------------------------------------------

static double giniImpurity(const CellProb& d, const ShipList& sl) {
    double s = 0;
    for (int i = 0; i < sl.count; ++i) { double v = d[sl.ships[i].name]; s += v * v; }
    { double v = d[COL_BLUE]; s += v * v; }
    return 1.0 - s;
}

// ---------------------------------------------------------------------------
// classify — split unrevealed cells into certain-blue, certain-ship, candidates.
// ---------------------------------------------------------------------------

struct Classified {
    int cb[25];   int ncb;   // certain blue
    int cs[25];   int ncs;   // certain ship
    int cand[25]; int ncand; // ambiguous

    Classified() : ncb(0), ncs(0), ncand(0) {}
};

static Classified classify(
    const std::array<CellProb, 25>& probs,
    const RevealedState& rev)
{
    Classified cls;
    for (int c = 0; c < 25; ++c) {
        if (rev.has(c)) continue;
        const CellProb& d = probs[c];
        double pb = d[COL_BLUE];
        double maxShipP = 0.0;
        for (int i = 0; i < N_COLORS; ++i)
            if (i != COL_BLUE && d[i] > maxShipP) maxShipP = d[i];
        if      (pb > 0.999)       cls.cb[cls.ncb++]     = c;
        else if (maxShipP > 0.999) cls.cs[cls.ncs++]     = c;
        else                       cls.cand[cls.ncand++] = c;
    }
    return cls;
}

// ---------------------------------------------------------------------------
// harvestPick — pick best certain ship (by value), else first certain blue.
// ---------------------------------------------------------------------------

static int harvestPick(
    const Classified& cls,
    const std::array<CellProb, 25>& probs,
    const double vals[N_COLORS])
{
    if (cls.ncs > 0) {
        int best = cls.cs[0]; double bestV = -1e18;
        for (int k = 0; k < cls.ncs; ++k) {
            int c = cls.cs[k];
            // Find dominant color
            int maxIdx = 0; double maxP = 0.0;
            for (int i = 0; i < N_COLORS; ++i)
                if (probs[c][i] > maxP) { maxP = probs[c][i]; maxIdx = i; }
            double v = vals[maxIdx];
            if (v > bestV) { bestV = v; best = c; }
        }
        return best;
    }
    if (cls.ncb > 0) return cls.cb[0];
    return -1;
}

// ---------------------------------------------------------------------------
// tryBook — MCTS book lookup, mirrors tryBook() on the page.
// Uses original string names for key construction (book keys are string literals).
// Early-exit: no book entry has more than 2 reveals — skip key build if >2.
// ---------------------------------------------------------------------------

static int tryBook(const RevealedState& rev, int nShips) {
    // Early exit: all book entries have at most 2 revealed cells in the key
    if (rev.count() > 2) return -1;

    auto it = MCTS_BOOK.find(nShips);
    if (it == MCTS_BOOK.end()) return -1;
    const MctsBook& book = it->second;

    // Normalize HV colors to u1/u2/u3 in cell order (same as getBindings order).
    int nextU = 1;
    // hvMap: real HV Color → slot string name
    Color hvBound[4];   // real colors seen so far
    int   hvSlot[4];    // slot number (1-based)
    int   hvCount = 0;

    // Collect revealed cells in ascending cell-index order (mask iterates LSB first = ascending)
    // Then build key string
    std::string key;
    key.reserve(64);
    bool first = true;
    for (int m = rev.mask; m; m &= m - 1) {
        int cell = __builtin_ctz(m);
        Color col = rev.col[cell];

        const char* colStr;
        if (isHV(col)) {
            // Find or assign slot
            int slotNum = -1;
            for (int i = 0; i < hvCount; ++i)
                if (hvBound[i] == col) { slotNum = hvSlot[i]; break; }
            if (slotNum < 0) {
                slotNum = nextU++;
                hvBound[hvCount] = col;
                hvSlot[hvCount]  = slotNum;
                ++hvCount;
            }
            // "u1", "u2", "u3"
            static const char* slots[] = { "u1", "u2", "u3" };
            colStr = slots[slotNum - 1];
        } else {
            colStr = colorName(col);
        }

        if (!first) key += ';';
        first = false;
        key += std::to_string(cell);
        key += ',';
        key += colStr;
    }

    auto kit = book.find(key);
    return (kit != book.end()) ? kit->second : -1;
}

// ---------------------------------------------------------------------------
// getBestMove — main decision function, mirrors getBestMove() on the page.
// ---------------------------------------------------------------------------

static const double LAMBDA_FISH = 300.0;
static const double LAMBDA_GINI = 250.0;

static int getBestMove(
    const RevealedState& rev,
    int b, int n, int nShips)
{
    ShipList sl = shipsByCount(nShips);
    if (!sl.ships) return -1;

    // Turn-0 fast path
    if (rev.mask == 0) return (nShips == 5) ? 0 : 8;

    // MCTS book
    int bk = tryBook(rev, nShips);
    if (bk >= 0) return bk;

    // Compute bindings and translate reveals to slot-view
    Bindings bindings   = getBindings(rev);
    RevealedState solRev = toSolverReveals(rev, bindings);

    auto probRes = maybeExactProbs(solRev, sl);
    const auto& slotProbs = probRes.probs;

    // Build value table — COL_SLOT* get real or mean HV value
    double vals[N_COLORS];
    for (int i = 0; i < N_COLORS; ++i) vals[i] = COLOR_VALUE[i];
    // Bound slots: use real color value
    for (int i = 0; i < bindings.nextSlot; ++i)
        vals[COL_SLOT1 + i] = colorValue(bindings.slotToReal[i]);
    // Unbound slots: use mean HV value
    {
        double unboundV = expectedHVValue(rev);
        for (int i = 0; i < sl.count; ++i) {
            Color nm = sl.ships[i].name;
            if (isSlot(nm)) {
                // Check if this slot is bound
                int slotIdx = nm - COL_SLOT1;
                if (slotIdx >= bindings.nextSlot)
                    vals[nm] = unboundV;
            }
        }
    }

    Classified cls = classify(slotProbs, solRev);

    // Everything determined → harvest
    if (cls.ncand == 0) return harvestPick(cls, slotProbs, vals);

    // Fishing window closed AND certain ship available → take it
    if (n >= 5 && cls.ncs > 0) return harvestPick(cls, slotProbs, vals);

    // Danger zone: next blue ends the game → min P(blue)
    if (b + 1 >= 4 && n >= 5) {
        int best = cls.cand[0]; double minBlue = 1e18;
        for (int k = 0; k < cls.ncand; ++k) {
            int c = cls.cand[k];
            double pb = slotProbs[c][COL_BLUE];
            if (pb < minBlue) { minBlue = pb; best = c; }
        }
        return best;
    }

    // Fishing mode
    bool shouldFish = (nShips == 5 && n < 5)
                   || (nShips == 6 && b >= 2 && n <= 3);
    if (shouldFish) {
        int best = cls.cand[0]; double bestScore = -1e18;
        for (int k = 0; k < cls.ncand; ++k) {
            int c = cls.cand[k];
            double score = cellEV(slotProbs[c], vals, sl) + LAMBDA_FISH * slotProbs[c][COL_BLUE];
            if (score > bestScore) { bestScore = score; best = c; }
        }
        return best;
    }

    // Default: max EV + Gini tiebreaker
    {
        int best = cls.cand[0]; double bestScore = -1e18;
        for (int k = 0; k < cls.ncand; ++k) {
            int c = cls.cand[k];
            double score = cellEV(slotProbs[c], vals, sl) + LAMBDA_GINI * giniImpurity(slotProbs[c], sl);
            if (score > bestScore) { bestScore = score; best = c; }
        }
        return best;
    }
}

// ---------------------------------------------------------------------------
// Parse integer from JSON — minimal helper (single linear scan, reusable ptr)
// ---------------------------------------------------------------------------

static int jsonGetInt(const char* json, const char* key, int def = 0) {
    const char* p = strstr(json, key);
    if (!p) return def;
    p += strlen(key);
    while (*p == ' ' || *p == ':') ++p;
    return atoi(p);
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------

class FastHeuristicOTStrategy : public OTStrategy {
public:
    void next_click(const std::vector<Cell>& board,
                    const std::string& meta_json,
                    ClickResult& out) override
    {
        int nColors   = jsonGetInt(meta_json.c_str(), "\"n_colors\"",  6);
        int bluesUsed = jsonGetInt(meta_json.c_str(), "\"blues_used\"", 0);
        int shipsHit  = jsonGetInt(meta_json.c_str(), "\"ships_hit\"",  0);
        int nShips    = nColors - 1;

        // Build revealed state — bitmask + Color[25], no heap allocation
        RevealedState rev;
        uint32_t clickedMask = 0;
        for (const Cell& cell : board) {
            int idx = cell.row * 5 + cell.col;
            if (cell.clicked) clickedMask |= 1u << idx;
            if (!cell.clicked) continue;
            // Decode color directly via char switch — no string allocation
            if (cell.color.size() >= 3) {
                Color c = harnessToColor(cell.color.c_str());
                if (c != COL_UNKNOWN) rev.set(idx, c);
            }
        }

        // 9-color (8 ships) not in SHIPS_BY_COUNT → random fallback
        if (!shipsByCount(nShips).ships) {
            for (int r = 0; r < 5; ++r)
                for (int c = 0; c < 5; ++c)
                    if (!((clickedMask >> (r * 5 + c)) & 1)) {
                        out.row = r; out.col = c; return;
                    }
            out.row = 0; out.col = 0; return;
        }

        int move = getBestMove(rev, bluesUsed, shipsHit, nShips);

        if (move >= 0 && !((clickedMask >> move) & 1)) {
            out.row = move / 5;
            out.col = move % 5;
            return;
        }

        // Fallback: first unclicked cell
        for (int r = 0; r < 5; ++r)
            for (int c = 0; c < 5; ++c)
                if (!((clickedMask >> (r * 5 + c)) & 1)) {
                    out.row = r; out.col = c; return;
                }

        out.row = 0; out.col = 0;
    }
};

// ---------------------------------------------------------------------------
// C exports required by the harness
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new FastHeuristicOTStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* inst) {
    static_cast<FastHeuristicOTStrategy*>(inst)->init_evaluation_run();
}

extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {
    static_cast<FastHeuristicOTStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<FastHeuristicOTStrategy*>(inst);

    std::vector<Cell> board;
    board.reserve(25);
    const char* p = board_json;
    while ((p = strstr(p, "\"row\":")) != nullptr) {
        Cell c;
        c.row = atoi(p + 6);
        const char* cp   = strstr(p, "\"col\":");    if (cp)   c.col = atoi(cp + 6);
        const char* colp = strstr(p, "\"color\":\"");
        if (colp) { colp += 9; const char* e = strchr(colp, '"'); if (e) c.color = std::string(colp, e - colp); }
        const char* clkp = strstr(p, "\"clicked\":"); if (clkp) { clkp += 10; while (*clkp == ' ') ++clkp; c.clicked = (strncmp(clkp, "true", 4) == 0); }
        board.push_back(c); p += 6;
    }

    ClickResult result;
    s->next_click(board, meta_json ? meta_json : "{}", result);
    buf = "{\"row\":" + std::to_string(result.row) + ",\"col\":" + std::to_string(result.col) + "}";
    return buf.c_str();
}
