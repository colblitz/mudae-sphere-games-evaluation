// sphere:stateless
/**
 * zavex_heuristic.cpp — Faithful C++ port of the heuristic strategy for /sphere trace (ot).
 *
 * Port of the solver logic from https://ouro-trace.zavex.workers.dev/
 *
 * Algorithm (mirrors getBestMove() on the page):
 *   1. Turn-0 opener: hardcoded from TURN0_PROBS (cell 8 for 6/7-ship games,
 *      cell 0 for 5-ship games).
 *   2. MCTS book lookup: if the current revealed state matches an entry in
 *      MCTS_BOOK, return that move.
 *   3. Per-cell probability computation:
 *      - Try exact joint enumeration (backtracker, budget=5000 boards).
 *      - Fall back to independence approximation if budget exceeded.
 *   4. Classify cells: certain-ship (P>=0.999), certain-blue (P(blue)>=0.999),
 *      or candidate (ambiguous).
 *   5. Decision tree (exact port):
 *      - Nothing ambiguous → harvest: best certain ship, else certain blue.
 *      - Fishing window closed (n>=5) AND certain ship exists → take it.
 *      - Danger zone (b+1>=4 AND n>=5) → min P(blue) among candidates.
 *      - Fishing mode (5-ship: n<5; 6-ship: b>=2 AND n<=3) →
 *          max(EV + LAMBDA_FISH * P(blue)).
 *      - Default → max(EV + LAMBDA_GINI * Gini-impurity).
 *
 * HV ship abstraction:
 *   Light, Dark, Red, Rainbow are all size-2 ships ("HV colors"). Until a
 *   slot is revealed the solver treats them abstractly as u1, u2, u3.
 *   The first HV color revealed in cell order binds to u1, second to u2, etc.
 *
 * n_colors → nShips mapping:
 *   n_colors 6 → 5 ships, 7 → 6 ships, 8 → 7 ships.
 *   9-color (8 ships) not in SHIPS_BY_COUNT → random fallback.
 *
 * Color values (VALUES on the page):
 *   Light rounded 75.9→76, Dark 104.5→105
 *
 * Tuned constants:
 *   LAMBDA_FISH = 300
 *   LAMBDA_GINI = 250
 *
 * -------------------------------------------------------------------------
 * C++ port notes — differences from the JS reference implementation
 * -------------------------------------------------------------------------
 *
 * This file is a faithful C++ port of zavex_heuristic.js.  The algorithm,
 * constants, data tables, and decision tree are identical.  The following
 * notes document every deliberate divergence from the JS source.
 *
 * 1. InsertionOrderCounts (exactPerCellProbs)
 *    JS uses a plain object for counts[c] inside the backtracker:
 *      counts[c][name] = (counts[c][name] || 0) + 1;
 *    Keys accumulate in first-cover order — the order each ship first covers
 *    cell c during the backtrack traversal.  Object.entries(counts[c]) then
 *    returns them in that order, so shipTotal = Σ(cnt/total) is summed in
 *    first-cover order and d[BLUE] = 1 - shipTotal is bit-identical to JS.
 *
 *    std::map<string,long> would iterate alphabetically instead, producing
 *    1-ULP differences in d[BLUE] that flip min-P(blue) comparisons in the
 *    danger zone.  We use InsertionOrderCounts (vector<pair<string,long>>)
 *    to reproduce the JS insertion-order semantics exactly.
 *
 * 2. stable_sort in exactPerCellProbs
 *    JS Array.sort() has been stable since ES2019.  std::sort is not stable.
 *    We use std::stable_sort when sorting ships by valid-placement count so
 *    that ties preserve ships-list order, matching JS behaviour.
 *
 * 3. cellEV and giniImpurity iterate *ships then Blue
 *    Both JS functions iterate Object.entries/Object.values(d) in d's
 *    insertion order.  For the approx path (perCellProbs) d is built by
 *    iterating ships then appending Blue, so iterating *ships + Blue here
 *    produces bit-identical results.  For the exact path d is in first-cover
 *    order (see note 1), so cellEV/giniImpurity differ from JS by at most
 *    1 ULP; this only affects harmless tie-breaks between equivalent moves.
 *
 * 4. WASM endgame solver omitted
 *    The original page uses a WASM C solver for exact enumeration that can
 *    handle millions of boards in ~7 ms.  Without WASM the JS port falls back
 *    to a JS backtracker with budget=5000 before switching to the independence
 *    approximation.  This C++ port uses the same budget=5000 backtracker.
 *
 * 5. MctsBook uses std::unordered_map
 *    Book lookup is a single key-equality test; iteration order is never
 *    used.  unordered_map is fine here and is faster than std::map.
 *
 * 6. Board parsing via minimal strstr loop
 *    The JS port receives the board as a JS array.  The C++ port receives
 *    board_json as a JSON string and parses it with a simple strstr loop.
 *    The field order emitted by the harness is always row/col/color/clicked,
 *    so the parsing is correct for all harness-generated input.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../interface/strategy.h"

using namespace sphere;

// ---------------------------------------------------------------------------
// Color names (string-based to match JS exactly)
// ---------------------------------------------------------------------------

static const std::string BLUE    = "Blue";
static const std::string TEAL    = "Teal";
static const std::string GREEN   = "Green";
static const std::string YELLOW  = "Yellow";
static const std::string ORANGE  = "Orange";
static const std::string LIGHT   = "Light";
static const std::string DARK    = "Dark";
static const std::string RED     = "Red";
static const std::string RAINBOW = "Rainbow";

static const std::string HV_COLORS[] = { "Light", "Dark", "Red", "Rainbow" };
static const int N_HV = 4;

static bool isHV(const std::string& c) {
    for (int i = 0; i < N_HV; ++i) if (HV_COLORS[i] == c) return true;
    return false;
}

static bool isSlotName(const std::string& s) {
    return s.size() >= 2 && s[0] == 'u' && s[1] >= '1' && s[1] <= '9';
}

// Harness sp-code → color name
static std::string harnessToColor(const std::string& sp) {
    if (sp == "spT") return TEAL;
    if (sp == "spG") return GREEN;
    if (sp == "spY") return YELLOW;
    if (sp == "spO") return ORANGE;
    if (sp == "spL") return LIGHT;
    if (sp == "spD") return DARK;
    if (sp == "spR") return RED;
    if (sp == "spW") return RAINBOW;
    if (sp == "spB") return BLUE;
    return "";
}

// ---------------------------------------------------------------------------
// Ship configurations by nShips (SHIPS_BY_COUNT on the page)
// Only 5/6/7 defined.  Pairs of (name, size).
// ---------------------------------------------------------------------------

using ShipList = std::vector<std::pair<std::string, int>>;

static ShipList SHIPS5 = {
    { "Teal", 4 }, { "Green", 3 }, { "Yellow", 3 }, { "Orange", 2 }, { "u1", 2 }
};
static ShipList SHIPS6 = {
    { "Teal", 4 }, { "Green", 3 }, { "Yellow", 3 }, { "Orange", 2 }, { "u1", 2 }, { "u2", 2 }
};
static ShipList SHIPS7 = {
    { "Teal", 4 }, { "Green", 3 }, { "Yellow", 3 }, { "Orange", 2 }, { "u1", 2 }, { "u2", 2 }, { "u3", 2 }
};

static const ShipList* shipsByCount(int n) {
    if (n == 5) return &SHIPS5;
    if (n == 6) return &SHIPS6;
    if (n == 7) return &SHIPS7;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Cell values (VALUES on the page)
// ---------------------------------------------------------------------------

static double colorValue(const std::string& col) {
    if (col == "Teal")    return 20.0;
    if (col == "Green")   return 35.0;
    if (col == "Yellow")  return 55.0;
    if (col == "Orange")  return 90.0;
    if (col == "Light")   return 76.0;
    if (col == "Dark")    return 105.0;
    if (col == "Red")     return 150.0;
    if (col == "Rainbow") return 500.0;
    if (col == "Blue")    return 10.0;
    return 0.0;
}

static const double LAMBDA_FISH = 300.0;
static const double LAMBDA_GINI = 250.0;

// ---------------------------------------------------------------------------
// Placement bitmasks for sizes 2, 3, 4 — mirrors genPlacements() on the page
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
// TURN0_PROBS — precomputed per-cell color probabilities for empty board
// Already remapped to slot names (u1/u2/u3) — mirrors TURN0_PROBS on page.
// Indexed by nShips (5,6,7), then cell 0..24.
// ---------------------------------------------------------------------------

// CellProb stores per-color probabilities for one cell.
// std::map gives O(log n) keyed lookup; iteration order is alphabetical but
// we never rely on that — cellEV and giniImpurity drive their own iteration
// order via the ships parameter to match JS Object.entries insertion order.
using CellProb = std::map<std::string, double>;
using Turn0ProbEntry = std::vector<CellProb>;

// We store only what's needed.  The full table is embedded verbatim from the JS.

static Turn0ProbEntry buildTurn0_5() {
    return {{
        {{"Teal",0.140828},{"u1",0.061543},{"Orange",0.061543},{"Yellow",0.085884},{"Green",0.085884},{"Blue",0.564318}},
        {{"Teal",0.181477},{"Green",0.111639},{"Orange",0.080238},{"u1",0.080238},{"Yellow",0.111639},{"Blue",0.434768}},
        {{"Teal",0.168702},{"Green",0.151247},{"Yellow",0.151247},{"u1",0.073417},{"Orange",0.073417},{"Blue",0.381970}},
        {{"Teal",0.181477},{"Orange",0.080238},{"u1",0.080238},{"Green",0.111639},{"Yellow",0.111639},{"Blue",0.434768}},
        {{"u1",0.061543},{"Orange",0.061543},{"Yellow",0.085884},{"Green",0.085884},{"Teal",0.140828},{"Blue",0.564318}},
        {{"Green",0.111639},{"u1",0.080238},{"Orange",0.080238},{"Yellow",0.111639},{"Teal",0.181477},{"Blue",0.434768}},
        {{"Green",0.115050},{"Orange",0.092617},{"u1",0.092617},{"Yellow",0.115050},{"Teal",0.162596},{"Blue",0.422070}},
        {{"Green",0.136647},{"Yellow",0.136647},{"u1",0.089523},{"Orange",0.089523},{"Teal",0.137045},{"Blue",0.410614}},
        {{"Orange",0.092617},{"u1",0.092617},{"Yellow",0.115050},{"Green",0.115050},{"Teal",0.162596},{"Blue",0.422070}},
        {{"Orange",0.080238},{"u1",0.080238},{"Yellow",0.111639},{"Green",0.111639},{"Teal",0.181477},{"Blue",0.434768}},
        {{"Yellow",0.151247},{"u1",0.073417},{"Orange",0.073417},{"Green",0.151247},{"Teal",0.168702},{"Blue",0.381970}},
        {{"Yellow",0.136647},{"u1",0.089523},{"Orange",0.089523},{"Green",0.136647},{"Teal",0.137045},{"Blue",0.410614}},
        {{"Yellow",0.151575},{"u1",0.089694},{"Orange",0.089694},{"Green",0.151575},{"Teal",0.111495},{"Blue",0.405967}},
        {{"u1",0.089523},{"Orange",0.089523},{"Yellow",0.136647},{"Green",0.136647},{"Teal",0.137045},{"Blue",0.410614}},
        {{"u1",0.073417},{"Orange",0.073417},{"Yellow",0.151247},{"Green",0.151247},{"Teal",0.168702},{"Blue",0.381970}},
        {{"u1",0.080238},{"Orange",0.080238},{"Yellow",0.111639},{"Green",0.111639},{"Teal",0.181477},{"Blue",0.434768}},
        {{"u1",0.092617},{"Orange",0.092617},{"Yellow",0.115050},{"Green",0.115050},{"Teal",0.162596},{"Blue",0.422070}},
        {{"u1",0.089523},{"Orange",0.089523},{"Yellow",0.136647},{"Green",0.136647},{"Teal",0.137045},{"Blue",0.410614}},
        {{"u1",0.092617},{"Orange",0.092617},{"Yellow",0.115050},{"Green",0.115050},{"Teal",0.162596},{"Blue",0.422070}},
        {{"u1",0.080238},{"Orange",0.080238},{"Yellow",0.111639},{"Green",0.111639},{"Teal",0.181477},{"Blue",0.434768}},
        {{"u1",0.061543},{"Orange",0.061543},{"Yellow",0.085884},{"Green",0.085884},{"Teal",0.140828},{"Blue",0.564318}},
        {{"u1",0.080238},{"Orange",0.080238},{"Yellow",0.111639},{"Green",0.111639},{"Teal",0.181477},{"Blue",0.434768}},
        {{"u1",0.073417},{"Orange",0.073417},{"Yellow",0.151247},{"Green",0.151247},{"Teal",0.168702},{"Blue",0.381970}},
        {{"u1",0.080238},{"Orange",0.080238},{"Yellow",0.111639},{"Green",0.111639},{"Teal",0.181477},{"Blue",0.434768}},
        {{"u1",0.061543},{"Orange",0.061543},{"Yellow",0.085884},{"Green",0.085884},{"Teal",0.140828},{"Blue",0.564318}},
    }};
}

static Turn0ProbEntry buildTurn0_6() {
    return {{
        {{"Teal",0.145630},{"u2",0.064362},{"u1",0.064362},{"Orange",0.064362},{"Yellow",0.089414},{"Green",0.089414},{"Blue",0.482455}},
        {{"Teal",0.184360},{"Green",0.111978},{"Orange",0.080664},{"u1",0.080664},{"u2",0.080664},{"Yellow",0.111978},{"Blue",0.349692}},
        {{"Teal",0.172539},{"Green",0.153337},{"Yellow",0.153337},{"u1",0.071763},{"u2",0.071763},{"Orange",0.071763},{"Blue",0.305498}},
        {{"Teal",0.184360},{"Orange",0.080664},{"u1",0.080664},{"u2",0.080664},{"Green",0.111978},{"Yellow",0.111978},{"Blue",0.349692}},
        {{"u2",0.064362},{"u1",0.064362},{"Orange",0.064362},{"Yellow",0.089414},{"Green",0.089414},{"Teal",0.145630},{"Blue",0.482455}},
        {{"Green",0.111978},{"u2",0.080664},{"u1",0.080664},{"Orange",0.080664},{"Yellow",0.111978},{"Teal",0.184360},{"Blue",0.349692}},
        {{"Green",0.112862},{"Orange",0.091593},{"u1",0.091593},{"u2",0.091593},{"Yellow",0.112862},{"Teal",0.154922},{"Blue",0.344574}},
        {{"Green",0.133547},{"Yellow",0.133547},{"u1",0.088672},{"u2",0.088672},{"Orange",0.088672},{"Teal",0.131279},{"Blue",0.335609}},
        {{"Orange",0.091593},{"u1",0.091593},{"u2",0.091593},{"Yellow",0.112862},{"Green",0.112862},{"Teal",0.154922},{"Blue",0.344574}},
        {{"Orange",0.080664},{"u1",0.080664},{"u2",0.080664},{"Yellow",0.111978},{"Green",0.111978},{"Teal",0.184360},{"Blue",0.349692}},
        {{"Yellow",0.153337},{"u2",0.071763},{"u1",0.071763},{"Orange",0.071763},{"Green",0.153337},{"Teal",0.172539},{"Blue",0.305498}},
        {{"Yellow",0.133547},{"u1",0.088672},{"u2",0.088672},{"Orange",0.088672},{"Green",0.133547},{"Teal",0.131279},{"Blue",0.335609}},
        {{"Yellow",0.147536},{"u2",0.089126},{"u1",0.089126},{"Orange",0.089126},{"Green",0.147536},{"Teal",0.107636},{"Blue",0.329915}},
        {{"u1",0.088672},{"u2",0.088672},{"Orange",0.088672},{"Yellow",0.133547},{"Green",0.133547},{"Teal",0.131279},{"Blue",0.335609}},
        {{"u1",0.071763},{"u2",0.071763},{"Orange",0.071763},{"Yellow",0.153337},{"Green",0.153337},{"Teal",0.172539},{"Blue",0.305498}},
        {{"u2",0.080664},{"u1",0.080664},{"Orange",0.080664},{"Yellow",0.111978},{"Green",0.111978},{"Teal",0.184360},{"Blue",0.349692}},
        {{"u2",0.091593},{"u1",0.091593},{"Orange",0.091593},{"Yellow",0.112862},{"Green",0.112862},{"Teal",0.154922},{"Blue",0.344574}},
        {{"u2",0.088672},{"u1",0.088672},{"Orange",0.088672},{"Yellow",0.133547},{"Green",0.133547},{"Teal",0.131279},{"Blue",0.335609}},
        {{"u2",0.091593},{"u1",0.091593},{"Orange",0.091593},{"Yellow",0.112862},{"Green",0.112862},{"Teal",0.154922},{"Blue",0.344574}},
        {{"u2",0.080664},{"u1",0.080664},{"Orange",0.080664},{"Yellow",0.111978},{"Green",0.111978},{"Teal",0.184360},{"Blue",0.349692}},
        {{"u2",0.064362},{"u1",0.064362},{"Orange",0.064362},{"Yellow",0.089414},{"Green",0.089414},{"Teal",0.145630},{"Blue",0.482455}},
        {{"u2",0.080664},{"u1",0.080664},{"Orange",0.080664},{"Yellow",0.111978},{"Green",0.111978},{"Teal",0.184360},{"Blue",0.349692}},
        {{"u2",0.071763},{"u1",0.071763},{"Orange",0.071763},{"Yellow",0.153337},{"Green",0.153337},{"Teal",0.172539},{"Blue",0.305498}},
        {{"u2",0.080664},{"u1",0.080664},{"Orange",0.080664},{"Yellow",0.111978},{"Green",0.111978},{"Teal",0.184360},{"Blue",0.349692}},
        {{"u2",0.064362},{"u1",0.064362},{"Orange",0.064362},{"Yellow",0.089414},{"Green",0.089414},{"Teal",0.145630},{"Blue",0.482455}},
    }};
}

static Turn0ProbEntry buildTurn0_7() {
    return {{
        {{"Teal",0.149505},{"u3",0.067624},{"u2",0.067624},{"u1",0.067624},{"Orange",0.067624},{"Yellow",0.093475},{"Green",0.093475},{"Blue",0.393050}},
        {{"Teal",0.186623},{"Green",0.111739},{"Orange",0.081055},{"u1",0.081055},{"u2",0.081055},{"u3",0.081055},{"Yellow",0.111739},{"Blue",0.265679}},
        {{"Teal",0.175765},{"Green",0.155269},{"Yellow",0.155269},{"u1",0.069742},{"u2",0.069742},{"u3",0.069742},{"Orange",0.069742},{"Blue",0.234730}},
        {{"Teal",0.186623},{"Orange",0.081055},{"u1",0.081055},{"u2",0.081055},{"u3",0.081055},{"Green",0.111739},{"Yellow",0.111739},{"Blue",0.265679}},
        {{"u3",0.067624},{"u2",0.067624},{"u1",0.067624},{"Orange",0.067624},{"Yellow",0.093475},{"Green",0.093475},{"Teal",0.149505},{"Blue",0.393050}},
        {{"Green",0.111739},{"u3",0.081055},{"u2",0.081055},{"u1",0.081055},{"Orange",0.081055},{"Yellow",0.111739},{"Teal",0.186623},{"Blue",0.265679}},
        {{"Green",0.111009},{"Orange",0.090570},{"u1",0.090570},{"u2",0.090570},{"u3",0.090570},{"Yellow",0.111009},{"Teal",0.148471},{"Blue",0.267233}},
        {{"Green",0.130750},{"Yellow",0.130750},{"u1",0.087890},{"u2",0.087890},{"u3",0.087890},{"Orange",0.087890},{"Teal",0.126754},{"Blue",0.260184}},
        {{"Orange",0.090570},{"u1",0.090570},{"u2",0.090570},{"u3",0.090570},{"Yellow",0.111009},{"Green",0.111009},{"Teal",0.148471},{"Blue",0.267233}},
        {{"Orange",0.081055},{"u1",0.081055},{"u2",0.081055},{"u3",0.081055},{"Yellow",0.111739},{"Green",0.111739},{"Teal",0.186623},{"Blue",0.265679}},
        {{"Yellow",0.155269},{"u3",0.069742},{"u2",0.069742},{"u1",0.069742},{"Orange",0.069742},{"Green",0.155269},{"Teal",0.175765},{"Blue",0.234730}},
        {{"Yellow",0.130750},{"u1",0.087890},{"u2",0.087890},{"u3",0.087890},{"Orange",0.087890},{"Green",0.130750},{"Teal",0.126754},{"Blue",0.260184}},
        {{"Yellow",0.144076},{"u2",0.088257},{"u3",0.088257},{"u1",0.088257},{"Orange",0.088257},{"Green",0.144076},{"Teal",0.105037},{"Blue",0.253781}},
        {{"u1",0.087890},{"u2",0.087890},{"u3",0.087890},{"Orange",0.087890},{"Yellow",0.130750},{"Green",0.130750},{"Teal",0.126754},{"Blue",0.260184}},
        {{"u1",0.069742},{"u2",0.069742},{"u3",0.069742},{"Orange",0.069742},{"Yellow",0.155269},{"Green",0.155269},{"Teal",0.175765},{"Blue",0.234730}},
        {{"u2",0.081055},{"u3",0.081055},{"u1",0.081055},{"Orange",0.081055},{"Yellow",0.111739},{"Green",0.111739},{"Teal",0.186623},{"Blue",0.265679}},
        {{"u2",0.090570},{"u3",0.090570},{"u1",0.090570},{"Orange",0.090570},{"Yellow",0.111009},{"Green",0.111009},{"Teal",0.148471},{"Blue",0.267233}},
        {{"u3",0.087890},{"u2",0.087890},{"u1",0.087890},{"Orange",0.087890},{"Yellow",0.130750},{"Green",0.130750},{"Teal",0.126754},{"Blue",0.260184}},
        {{"u3",0.090570},{"u2",0.090570},{"u1",0.090570},{"Orange",0.090570},{"Yellow",0.111009},{"Green",0.111009},{"Teal",0.148471},{"Blue",0.267233}},
        {{"u3",0.081055},{"u2",0.081055},{"u1",0.081055},{"Orange",0.081055},{"Yellow",0.111739},{"Green",0.111739},{"Teal",0.186623},{"Blue",0.265679}},
        {{"u3",0.067624},{"u2",0.067624},{"u1",0.067624},{"Orange",0.067624},{"Yellow",0.093475},{"Green",0.093475},{"Teal",0.149505},{"Blue",0.393050}},
        {{"u3",0.081055},{"u2",0.081055},{"u1",0.081055},{"Orange",0.081055},{"Yellow",0.111739},{"Green",0.111739},{"Teal",0.186623},{"Blue",0.265679}},
        {{"u3",0.069742},{"u2",0.069742},{"u1",0.069742},{"Orange",0.069742},{"Yellow",0.155269},{"Green",0.155269},{"Teal",0.175765},{"Blue",0.234730}},
        {{"u3",0.081055},{"u2",0.081055},{"u1",0.081055},{"Orange",0.081055},{"Yellow",0.111739},{"Green",0.111739},{"Teal",0.186623},{"Blue",0.265679}},
        {{"u3",0.067624},{"u2",0.067624},{"u1",0.067624},{"Orange",0.067624},{"Yellow",0.093475},{"Green",0.093475},{"Teal",0.149505},{"Blue",0.393050}},
    }};
}

// ---------------------------------------------------------------------------
// MCTS_BOOK — verbatim from the page
// Keys: nShips → map of "cell,Color[;cell,Color]" → recommended cell
// ---------------------------------------------------------------------------

using MctsBook = std::unordered_map<std::string, int>;
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

// Static init
static const MctsBooks MCTS_BOOK = buildMctsBooks();

// ---------------------------------------------------------------------------
// getBindings — map first HV color seen → u1, second → u2, etc.
// Takes revealed as ordered map (cell idx → color name)
// ---------------------------------------------------------------------------

struct Bindings {
    std::unordered_map<std::string, std::string> slotToColor;  // u1 → Light, etc.
    std::unordered_map<std::string, std::string> colorToSlot;  // Light → u1, etc.
};

static Bindings getBindings(const std::map<int, std::string>& revealed) {
    Bindings b;
    int next = 1;
    for (const auto& kv : revealed) {
        const std::string& col = kv.second;
        if (!isHV(col) || b.colorToSlot.count(col)) continue;
        std::string slot = "u" + std::to_string(next);
        b.slotToColor[slot] = col;
        b.colorToSlot[col]  = slot;
        ++next;
    }
    return b;
}

// ---------------------------------------------------------------------------
// toSolverReveals — translate HV colors to slot names
// ---------------------------------------------------------------------------

static std::map<int, std::string> toSolverReveals(
    const std::map<int, std::string>& revealed,
    const std::unordered_map<std::string, std::string>& colorToSlot)
{
    std::map<int, std::string> out;
    for (const auto& kv : revealed) {
        const std::string& col = kv.second;
        if (isHV(col) && colorToSlot.count(col))
            out[kv.first] = colorToSlot.at(col);
        else
            out[kv.first] = col;
    }
    return out;
}

// ---------------------------------------------------------------------------
// expectedHVValue — mean value of HV colors not yet revealed
// ---------------------------------------------------------------------------

static double expectedHVValue(const std::map<int, std::string>& revealed) {
    std::set<std::string> revealedSet;
    for (const auto& kv : revealed) revealedSet.insert(kv.second);
    double sum = 0; int cnt = 0;
    for (int i = 0; i < N_HV; ++i) {
        if (!revealedSet.count(HV_COLORS[i])) {
            sum += colorValue(HV_COLORS[i]);
            ++cnt;
        }
    }
    return cnt > 0 ? sum / cnt : 0.0;
}

// ---------------------------------------------------------------------------
// filterPlacements — mirrors filterPlacements() on the page
// Returns map: ship_name → list of valid placement bitmasks
// ---------------------------------------------------------------------------

static std::unordered_map<std::string, std::vector<int>> filterPlacements(
    const std::map<int, std::string>& revealed,
    const ShipList& ships)
{
    // Build per-ship must/avoid masks
    std::unordered_map<std::string, int> revealSelf, revealOther;
    for (const auto& sp : ships) { revealSelf[sp.first] = 0; revealOther[sp.first] = 0; }

    for (const auto& kv : revealed) {
        int bit = 1 << kv.first;
        const std::string& color = kv.second;
        if (color == BLUE) {
            for (const auto& sp : ships) revealOther[sp.first] |= bit;
        } else {
            if (revealSelf.count(color)) revealSelf[color] |= bit;
            for (const auto& sp : ships)
                if (sp.first != color) revealOther[sp.first] |= bit;
        }
    }

    std::unordered_map<std::string, std::vector<int>> valid;
    for (const auto& sp : ships) {
        const std::string& ship = sp.first;
        int size = sp.second;
        int must  = revealSelf[ship];
        int avoid = revealOther[ship];
        std::vector<int> out;
        for (int p : PLACEMENTS(size)) {
            if ((p & must) == must && (p & avoid) == 0) out.push_back(p);
        }
        valid[ship] = out;
    }
    return valid;
}

// ---------------------------------------------------------------------------
// perCellProbs — independence approximation (fallback)
// Returns vector of CellProb (index 0..24; empty map for revealed cells)
// ---------------------------------------------------------------------------

static std::vector<CellProb> perCellProbs(
    const std::map<int, std::string>& revealed,
    const ShipList& ships)
{
    auto valid = filterPlacements(revealed, ships);

    // Per-ship per-cell probability
    std::map<std::string, std::array<double, 25>> pShip;
    for (const auto& sp : ships) {
        const std::string& ship = sp.first;
        const auto& vl = valid[ship];
        int n = (int)vl.size();
        std::array<double, 25> cnt; cnt.fill(0.0);
        if (n > 0) {
            for (int p : vl) {
                for (int b = p; b; b &= b-1) cnt[__builtin_ctz(b)] += 1.0;
            }
            for (auto& v : cnt) v /= n;
        }
        pShip[ship] = cnt;
    }

    std::vector<CellProb> out(25);
    for (int c = 0; c < 25; ++c) {
        if (revealed.count(c)) { out[c] = {}; continue; }
        double pBlue   = 1.0;
        double shipSum = 0.0;
        for (const auto& sp : ships) {
            double ps = pShip[sp.first][c];
            pBlue   *= (1.0 - ps);
            shipSum += ps;
        }
        double total = shipSum + pBlue;
        CellProb d;
        if (total <= 0.0) { d[BLUE] = 1.0; out[c] = d; continue; }
        double scale = 1.0 / total;
        for (const auto& sp : ships) {
            double v = pShip[sp.first][c] * scale;
            if (v > 1e-9) d[sp.first] = v;
        }
        d[BLUE] = pBlue * scale;
        out[c] = d;
    }
    return out;
}

// ---------------------------------------------------------------------------
// exactPerCellProbs — backtracking joint enumeration with budget cap
// Mirrors exactPerCellProbs() on the page.
// ---------------------------------------------------------------------------

struct ExactResult {
    std::vector<CellProb> probs;  // empty if aborted
    long total;
    bool aborted;
};

// InsertionOrderCounts — preserves the order in which ship names are first
// inserted, mirroring JS plain-object insertion order for counts[c].
//
// JS exactPerCellProbs uses counts[c] as a plain object:
//   counts[c][name] = (counts[c][name] || 0) + 1;
// Object properties accumulate in insertion order (the first board that covers
// cell c by ship X determines X's position in counts[c]).  When the output loop
// does `for (const [color, cnt] of Object.entries(counts[c]))` it therefore
// iterates in that first-cover order.
//
// Using std::map here would iterate alphabetically instead — a different
// summation order for shipTotal = Σ(cnt/total), causing 1-ULP differences in
// d[BLUE] = 1 - shipTotal that flip min-P(blue) comparisons in the danger zone.
struct InsertionOrderCounts {
    // Pairs in insertion order; at most nShips entries per cell.
    std::vector<std::pair<std::string, long>> entries;

    // Increment count for `name`; inserts at the end on first access.
    void inc(const std::string& name) {
        for (auto& kv : entries) {
            if (kv.first == name) { ++kv.second; return; }
        }
        entries.push_back({ name, 1L });
    }
};

static ExactResult exactPerCellProbs(
    const std::map<int, std::string>& revealed,
    const ShipList& ships,
    int budget)
{
    auto valid = filterPlacements(revealed, ships);
    for (const auto& sp : ships)
        if (valid[sp.first].empty()) return { {}, 0, true };

    int nShips = (int)ships.size();

    // Sort ships by number of valid placements ascending (tightest constraint first).
    // JS Array.sort() is stable; use stable_sort to match tie-breaking behaviour.
    std::vector<int> order(nShips);
    for (int i = 0; i < nShips; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&ships, &valid](int a, int b) {
        return valid[ships[a].first].size() < valid[ships[b].first].size();
    });

    std::vector<const std::vector<int>*> placementLists(nShips);
    std::vector<std::string> shipNames(nShips);
    for (int i = 0; i < nShips; ++i) {
        placementLists[i] = &valid[ships[order[i]].first];
        shipNames[i]       = ships[order[i]].first;
    }

    std::vector<int> assignment(nShips, 0);
    // Use InsertionOrderCounts instead of std::map so that Object.entries(counts[c])
    // iteration order (= first-cover order during backtracking) is reproduced exactly.
    std::vector<InsertionOrderCounts> counts(25);
    long total   = 0;
    bool aborted = false;

    std::function<void(int, int)> backtrack = [&](int idx, int occ) {
        if (aborted) return;
        if (idx == nShips) {
            ++total;
            if (total > budget) { aborted = true; return; }
            for (int i = 0; i < nShips; ++i) {
                int m = assignment[i];
                const std::string& name = shipNames[i];
                for (int b = m; b; b &= b-1) {
                    counts[__builtin_ctz(b)].inc(name);
                }
            }
            return;
        }
        for (int p : *placementLists[idx]) {
            if (p & occ) continue;
            assignment[idx] = p;
            backtrack(idx + 1, occ | p);
            if (aborted) return;
        }
    };
    backtrack(0, 0);

    if (aborted || total == 0) return { {}, total, aborted };

    std::vector<CellProb> out(25);
    for (int c = 0; c < 25; ++c) {
        if (revealed.count(c)) { out[c] = {}; continue; }
        CellProb d;
        double shipTotal = 0;
        // Iterate in insertion order — mirrors JS Object.entries(counts[c]).
        // The summation order determines d[BLUE] = 1 - shipTotal via floating-point
        // accumulation, so this must match JS exactly.
        for (const auto& kv : counts[c].entries) {
            d[kv.first] = (double)kv.second / (double)total;
            shipTotal   += d[kv.first];
        }
        d[BLUE] = std::max(0.0, 1.0 - shipTotal);
        out[c] = d;
    }
    return { out, total, false };
}

// ---------------------------------------------------------------------------
// maybeExactProbs — turn-0 fast path + exact + independence fallback
// Mirrors maybeExactProbs() on the page.
// ---------------------------------------------------------------------------

struct ProbResult {
    std::vector<CellProb> probs;
    bool exact;
};

static ProbResult maybeExactProbs(
    const std::map<int, std::string>& revealed,
    const ShipList& ships,
    int budget = 5000)
{
    // Turn-0 fast path
    if (revealed.empty()) {
        int n = (int)ships.size();
        static Turn0ProbEntry t5 = buildTurn0_5();
        static Turn0ProbEntry t6 = buildTurn0_6();
        static Turn0ProbEntry t7 = buildTurn0_7();
        if (n == 5) return { t5, true };
        if (n == 6) return { t6, true };
        if (n == 7) return { t7, true };
    }

    // Upper-bound check: only attempt exact when board count is small
    auto valid = filterPlacements(revealed, ships);
    long upper = 1;
    for (const auto& sp : ships) {
        long vl = (long)valid[sp.first].size();
        if (vl == 0) return { perCellProbs(revealed, ships), false };
        upper *= vl;
        if (upper > (long)(50 * budget)) return { perCellProbs(revealed, ships), false };
    }

    auto res = exactPerCellProbs(revealed, ships, budget);
    if (res.aborted || res.probs.empty())
        return { perCellProbs(revealed, ships), false };
    return { res.probs, true };
}

// ---------------------------------------------------------------------------
// cellEV — expected value of clicking a cell
// ---------------------------------------------------------------------------
//
// JS: for (const [col, p] of Object.entries(d)) s += p * (vals[col] || 0)
// Object.entries(d) returns keys in d's insertion order.
//
// approx path (perCellProbs): d is built by iterating ships list then Blue,
//   so insertion order == ships-list + Blue.  Iterating *ships then Blue here
//   produces bit-identical sums.
//
// exact path (exactPerCellProbs): d is populated from InsertionOrderCounts in
//   first-cover backtrack order, which is NOT ships-list order in general.
//   cellEV therefore accumulates in a different order than JS for this path,
//   causing at most 1-ULP differences in the final score.  Score-diverging
//   decisions on this path are driven by P(blue) comparisons (danger zone,
//   fishing), which are fixed by InsertionOrderCounts — the 1-ULP cellEV
//   delta only ever affects harmless tie-breaks between equivalent moves.
//
// The extra `ships` parameter is threaded through all callers below.

static double cellEV(const CellProb& d,
                     const std::map<std::string, double>& vals,
                     const ShipList& ships) {
    double s = 0;
    // Ship keys first, in ships-list order (mirrors JS insertion order)
    for (const auto& sp : ships) {
        auto dit = d.find(sp.first);
        if (dit == d.end()) continue;
        auto vit = vals.find(sp.first);
        if (vit != vals.end()) s += dit->second * vit->second;
    }
    // Blue last (as JS appends it after the backtracking loop)
    {
        auto dit = d.find(BLUE);
        if (dit != d.end()) {
            auto vit = vals.find(BLUE);
            if (vit != vals.end()) s += dit->second * vit->second;
        }
    }
    return s;
}

// ---------------------------------------------------------------------------
// giniImpurity — 1 − Σ P(outcome)²
// ---------------------------------------------------------------------------
//
// JS: for (const p of Object.values(d)) s += p * p  (insertion order)
// We iterate ships then Blue, matching cellEV and the JS insertion order for
// the approx path.  Sum-of-squares is order-independent mathematically, but
// consistent iteration avoids last-bit divergence in the default-path score.

static double giniImpurity(const CellProb& d, const ShipList& ships) {
    double s = 0;
    for (const auto& sp : ships) {
        auto it = d.find(sp.first);
        if (it != d.end()) s += it->second * it->second;
    }
    {
        auto it = d.find(BLUE);
        if (it != d.end()) s += it->second * it->second;
    }
    return 1.0 - s;
}

// ---------------------------------------------------------------------------
// classify — split unrevealed cells into certain-blue, certain-ship, candidates
// ---------------------------------------------------------------------------

struct Classified {
    std::vector<int> cb;    // certain blue
    std::vector<int> cs;    // certain ship
    std::vector<int> cand;  // ambiguous
};

static Classified classify(
    const std::vector<CellProb>& probs,
    const std::map<int, std::string>& revealed)
{
    Classified cls;
    for (int c = 0; c < 25; ++c) {
        if (revealed.count(c)) continue;
        const CellProb& d = probs[c];
        auto bit = d.find(BLUE);
        double pb = (bit != d.end()) ? bit->second : 0.0;
        double maxShipP = 0.0;
        for (const auto& kv : d)
            if (kv.first != BLUE && kv.second > maxShipP) maxShipP = kv.second;
        if      (pb > 0.999)       cls.cb.push_back(c);
        else if (maxShipP > 0.999) cls.cs.push_back(c);
        else                       cls.cand.push_back(c);
    }
    return cls;
}

// ---------------------------------------------------------------------------
// harvestPick — pick best certain ship (by value), else first certain blue
// ---------------------------------------------------------------------------

static int harvestPick(
    const std::vector<int>& cs,
    const std::vector<int>& cb,
    const std::vector<CellProb>& probs,
    const std::map<std::string, double>& vals)
{
    if (!cs.empty()) {
        int best = cs[0]; double bestV = -1e18;
        for (int c : cs) {
            std::string maxCol = BLUE; double maxP = 0.0;
            for (const auto& kv : probs[c]) if (kv.second > maxP) { maxP = kv.second; maxCol = kv.first; }
            auto it = vals.find(maxCol);
            double v = (it != vals.end()) ? it->second : 0.0;
            if (v > bestV) { bestV = v; best = c; }
        }
        return best;
    }
    if (!cb.empty()) return cb[0];
    return -1;
}

// ---------------------------------------------------------------------------
// tryBook — MCTS book lookup, mirrors tryBook() on the page
// ---------------------------------------------------------------------------

static int tryBook(const std::map<int, std::string>& revealed, int nShips) {
    auto it = MCTS_BOOK.find(nShips);
    if (it == MCTS_BOOK.end()) return -1;
    const MctsBook& book = it->second;

    // Normalize HV colors to u1/u2/... in cell order
    int nextU = 1;
    std::unordered_map<std::string, std::string> hvMap;
    std::vector<std::pair<int, std::string>> translated;
    for (const auto& kv : revealed) {  // map is sorted by cell idx
        const std::string& col = kv.second;
        std::string outCol = col;
        if (isHV(col)) {
            if (!hvMap.count(col)) hvMap[col] = "u" + std::to_string(nextU++);
            outCol = hvMap[col];
        }
        translated.push_back({ kv.first, outCol });
    }

    // Build key
    std::string key;
    for (int i = 0; i < (int)translated.size(); ++i) {
        if (i) key += ";";
        key += std::to_string(translated[i].first) + "," + translated[i].second;
    }

    auto kit = book.find(key);
    return (kit != book.end()) ? kit->second : -1;
}

// ---------------------------------------------------------------------------
// getBestMove — main decision function, mirrors getBestMove() on the page
// ---------------------------------------------------------------------------

static int getBestMove(
    const std::map<int, std::string>& revealed,
    int b, int n, int nShips)
{
    const ShipList* ships = shipsByCount(nShips);
    if (!ships) return -1;  // unsupported nShips

    // Turn-0 fast path
    if (revealed.empty()) return (nShips == 5) ? 0 : 8;

    // MCTS book
    int bk = tryBook(revealed, nShips);
    if (bk >= 0) return bk;

    // Compute bindings and translate reveals
    Bindings bindings = getBindings(revealed);
    std::map<int, std::string> solverRevealed = toSolverReveals(revealed, bindings.colorToSlot);

    auto probRes = maybeExactProbs(solverRevealed, *ships);
    const auto& slotProbs = probRes.probs;

    // Build value table — slot names use real or mean HV value
    double unboundV = expectedHVValue(revealed);
    std::map<std::string, double> vals;
    // Set base values for all known colors
    for (const std::string& c : { TEAL, GREEN, YELLOW, ORANGE, LIGHT, DARK, RED, RAINBOW, BLUE })
        vals[c] = colorValue(c);
    // Bound slots get real value
    for (const auto& kv : bindings.slotToColor) vals[kv.first] = colorValue(kv.second);
    // Unbound slots get mean HV value
    for (const auto& sp : *ships) {
        if (isSlotName(sp.first) && !bindings.slotToColor.count(sp.first))
            vals[sp.first] = unboundV;
    }

    Classified cls = classify(slotProbs, solverRevealed);
    const auto& cb   = cls.cb;
    const auto& cs   = cls.cs;
    const auto& cand = cls.cand;

    // Everything determined → harvest
    if (cand.empty()) return harvestPick(cs, cb, slotProbs, vals);

    // Fishing window closed AND certain ship available → take it
    if (n >= 5 && !cs.empty()) return harvestPick(cs, cb, slotProbs, vals);

    // Danger zone: next blue ends the game → min P(blue)
    if (b + 1 >= 4 && n >= 5) {
        int best = cand[0]; double minBlue = 1e18;
        for (int c : cand) {
            auto it = slotProbs[c].find(BLUE);
            double pb = (it != slotProbs[c].end()) ? it->second : 0.0;
            if (pb < minBlue) { minBlue = pb; best = c; }
        }
        return best;
    }

    // Fishing mode
    bool shouldFish = (nShips == 5 && n < 5)
                   || (nShips == 6 && b >= 2 && n <= 3);
    if (shouldFish) {
        int best = cand[0]; double bestScore = -1e18;
        for (int c : cand) {
            auto it = slotProbs[c].find(BLUE);
            double pb = (it != slotProbs[c].end()) ? it->second : 0.0;
            double score = cellEV(slotProbs[c], vals, *ships) + LAMBDA_FISH * pb;
            if (score > bestScore) { bestScore = score; best = c; }
        }
        return best;
    }

    // Default: max EV + Gini tiebreaker
    {
        int best = cand[0]; double bestScore = -1e18;
        for (int c : cand) {
            double score = cellEV(slotProbs[c], vals, *ships) + LAMBDA_GINI * giniImpurity(slotProbs[c], *ships);
            if (score > bestScore) { bestScore = score; best = c; }
        }
        return best;
    }
}

// ---------------------------------------------------------------------------
// Parse integer from JSON — minimal helper
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

class HeuristicOTStrategy : public OTStrategy {
public:
    // No cross-game precomputation or per-game state.

    void next_click(const std::vector<Cell>& board,
                    const std::string& meta_json,
                    ClickResult& out) override
    {
        int nColors  = jsonGetInt(meta_json.c_str(), "\"n_colors\"",  6);
        int bluesUsed = jsonGetInt(meta_json.c_str(), "\"blues_used\"", 0);
        int shipsHit  = jsonGetInt(meta_json.c_str(), "\"ships_hit\"",  0);
        int nShips    = nColors - 1;

        // Build revealed dict (sorted by cell idx — matches JS Object.entries sort behavior)
        std::map<int, std::string> revDict;
        std::set<int> revealedSet;
        for (const Cell& cell : board) {
            if (!cell.clicked) continue;
            int idx = cell.row * 5 + cell.col;
            revealedSet.insert(idx);
            std::string color = harnessToColor(cell.color);
            if (!color.empty()) revDict[idx] = color;
        }

        // 9-color (8 ships) not in SHIPS_BY_COUNT → random fallback
        if (!shipsByCount(nShips)) {
            for (int r = 0; r < 5; ++r)
                for (int c = 0; c < 5; ++c)
                    if (!revealedSet.count(r * 5 + c)) {
                        out.row = r; out.col = c; return;
                    }
            out.row = 0; out.col = 0; return;
        }

        int move = getBestMove(revDict, bluesUsed, shipsHit, nShips);

        if (move >= 0 && !revealedSet.count(move)) {
            out.row = move / 5;
            out.col = move % 5;
            return;
        }

        // Fallback: first unrevealed cell
        for (int r = 0; r < 5; ++r)
            for (int c = 0; c < 5; ++c)
                if (!revealedSet.count(r * 5 + c)) {
                    out.row = r; out.col = c; return;
                }

        out.row = 0; out.col = 0;
    }
};

// ---------------------------------------------------------------------------
// C exports required by the harness
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new HeuristicOTStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* inst) {
    static_cast<HeuristicOTStrategy*>(inst)->init_evaluation_run();
}

extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {
    static_cast<HeuristicOTStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<HeuristicOTStrategy*>(inst);

    std::vector<Cell> board;
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

    ClickResult out;
    s->next_click(board, meta_json ? meta_json : "{}", out);
    buf = "{\"row\":" + std::to_string(out.row) + ",\"col\":" + std::to_string(out.col) + "}";
    return buf.c_str();
}
