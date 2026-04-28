#pragma once
/**
 * svessinn_solver_v2_engine.h
 *
 * Shared engine for the Svessinn v2 Trace solver strategy family.
 * Ported from Svessinn's solver.html (standard / anti-teal / ship-hunter modes).
 *
 * This header is designed to be included by exactly one .cpp per .so — each
 * strategy compiles to its own shared library, so there are no ODR violations.
 *
 * Usage in a strategy .cpp:
 *
 *   #include "svessinn_solver_v2_engine.h"
 *
 *   class MyOTStrategy : public OTStrategy {
 *   public:
 *       void init_game_payload(const std::string&) override {}
 *       void next_click(const std::vector<Cell>& board,
 *                       const std::string& meta_json,
 *                       ClickResult& out) override
 *       {
 *           SvessinnV2State st = svessinn_v2_parse(board, meta_json);
 *           svessinn_v2_certain_early_return(st, out);  // returns if a certain non-blue cell exists
 *           // ... strategy-specific selection using st.heatmap, st.certain, st.isHuntingBlue ...
 *           out.row = ...; out.col = ...;
 *       }
 *   };
 *
 *   SVESSINN_V2_EXPORTS(MyOTStrategy)
 *
 * The SVESSINN_V2_EXPORTS macro emits all five extern "C" symbols required by
 * the harness, including the board JSON parser in strategy_next_click.
 */

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "../../interface/strategy.h"

using namespace sphere;

// ---------------------------------------------------------------------------
// Color index table
// ---------------------------------------------------------------------------
// Letters: B T G Y O R W L D ? (10 total, indices 0-9)
// Order matches JS ALL_COLORS key-insertion order for floating-point parity.
//
// v2 order/always flags (changed from v1):
//   Old: B(T) T(T) G(T) Y(T) O(T) L(T) D(F) R(F) W(F)
//   New: B(T) T(T) G(T) Y(T) O(T) R(F) W(F) L(F) D(F)
// L demoted from always:true to always:false. 6-color games now have 1 rare slot.

static uint8_t charToIdx[128];
static char    idxToChar[10];
static bool    charTableInit = false;

struct ShipDef { char letter; int len; bool always; };
static const ShipDef ALL_COLORS[] = {
    { 'B', 0, true  },
    { 'T', 4, true  },
    { 'G', 3, true  },
    { 'Y', 3, true  },
    { 'O', 2, true  },
    { 'R', 2, false },
    { 'W', 2, false },
    { 'L', 2, false },
    { 'D', 2, false },
};
static const int N_COLORS_DEF = 9;
static const char QMARK = '?';

static void initCharTable() {
    if (charTableInit) return;
    memset(charToIdx, 255, sizeof(charToIdx));
    for (int i = 0; i < N_COLORS_DEF; ++i) {
        charToIdx[(uint8_t)ALL_COLORS[i].letter] = (uint8_t)i;
        idxToChar[i] = ALL_COLORS[i].letter;
    }
    charToIdx[(uint8_t)QMARK] = 9;
    idxToChar[9] = QMARK;
    charTableInit = true;
}

static inline int  cidx(char c)     { return (int)(uint8_t)charToIdx[(uint8_t)c]; }
static inline int  shipLen(int si)  { return ALL_COLORS[si].len; }
static inline bool isAlways(int si) { return ALL_COLORS[si].always; }
static inline bool isRare(char c) {
    uint8_t i = charToIdx[(uint8_t)c];
    return i < N_COLORS_DEF && !ALL_COLORS[i].always;
}

static char spToLetter(const char* sp, int len) {
    if (len < 3) return '?';
    switch (sp[2]) {
        case 'B': return 'B';
        case 'T': return 'T';
        case 'G': return 'G';
        case 'Y': return 'Y';
        case 'O': return 'O';
        case 'L': return 'L';
        case 'D': return 'D';
        case 'R': return 'R';
        case 'W': return 'W';
        default:  return '?';
    }
}

// ---------------------------------------------------------------------------
// Bitmask placement tables
// ---------------------------------------------------------------------------

struct PlacementTable {
    uint32_t masks[40];
    int      n;
};

static PlacementTable buildTable(int len) {
    PlacementTable t; t.n = 0;
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c <= 5 - len; ++c) {
            uint32_t m = 0;
            for (int i = 0; i < len; ++i) m |= 1u << (r * 5 + c + i);
            t.masks[t.n++] = m;
        }
    for (int c = 0; c < 5; ++c)
        for (int r = 0; r <= 5 - len; ++r) {
            uint32_t m = 0;
            for (int i = 0; i < len; ++i) m |= 1u << ((r + i) * 5 + c);
            t.masks[t.n++] = m;
        }
    return t;
}

static const PlacementTable& ptable(int len) {
    static PlacementTable p2 = buildTable(2);
    static PlacementTable p3 = buildTable(3);
    static PlacementTable p4 = buildTable(4);
    if (len == 2) return p2;
    if (len == 3) return p3;
    return p4;
}

// ---------------------------------------------------------------------------
// CellHeat — per-cell probability map with insertion-order tracking
// ---------------------------------------------------------------------------

struct CellHeat {
    double  v[10];
    uint8_t order[10];
    int     n_keys;

    void clear() { memset(v, 0, sizeof(v)); n_keys = 0; }

    double& operator[](char k) {
        int i = cidx(k);
        if (v[i] == 0.0) {
            bool found = false;
            for (int j = 0; j < n_keys; ++j) if (order[j] == i) { found = true; break; }
            if (!found) order[n_keys++] = (uint8_t)i;
        }
        return v[i];
    }

    double get(char k) const { return v[cidx(k)]; }

    bool contains(char k) const {
        int i = cidx(k);
        for (int j = 0; j < n_keys; ++j) if (order[j] == i) return true;
        return false;
    }

    void assign_single(char k, double val) {
        memset(v, 0, sizeof(v));
        n_keys = 0;
        int i = cidx(k);
        v[i] = val;
        order[0] = (uint8_t)i;
        n_keys = 1;
    }

    void divide_all(double total) {
        for (int j = 0; j < n_keys; ++j) v[order[j]] /= total;
    }

    // sum of all entries
    double sum() const {
        double t = 0.0;
        for (int j = 0; j < n_keys; ++j) t += v[order[j]];
        return t;
    }

    // sum of all non-blue entries (ship probability)
    double shipSum() const { return sum() - v[cidx('B')]; }
};

using Heatmap = std::array<CellHeat, 25>;

// ---------------------------------------------------------------------------
// ActiveShips
// ---------------------------------------------------------------------------

struct ActiveShips {
    int  idx[N_COLORS_DEF];
    char letter[N_COLORS_DEF];
    int  n;
};

static ActiveShips getActiveShips(int nColors, const char revealedArr[25]) {
    bool rarePresent[N_COLORS_DEF] = {};
    int  nRaresPresent = 0;
    for (int i = 0; i < 25; ++i) {
        char c = revealedArr[i];
        if (c && isRare(c)) {
            int ci = cidx(c);
            if (!rarePresent[ci]) { rarePresent[ci] = true; ++nRaresPresent; }
        }
    }
    int totalRaresAllowed = std::max(0, nColors - 5);

    ActiveShips as; as.n = 0;
    if (nRaresPresent < totalRaresAllowed) {
        for (int i = 0; i < N_COLORS_DEF; ++i) {
            as.idx[as.n]    = i;
            as.letter[as.n] = ALL_COLORS[i].letter;
            ++as.n;
        }
    } else {
        for (int i = 0; i < N_COLORS_DEF; ++i) {
            if (ALL_COLORS[i].always) {
                as.idx[as.n]    = i;
                as.letter[as.n] = ALL_COLORS[i].letter;
                ++as.n;
            }
        }
        for (int i = 0; i < N_COLORS_DEF; ++i) {
            if (!ALL_COLORS[i].always && rarePresent[i]) {
                as.idx[as.n]    = i;
                as.letter[as.n] = ALL_COLORS[i].letter;
                ++as.n;
            }
        }
    }
    return as;
}

// ---------------------------------------------------------------------------
// getDeductions — four-pass constraint solver
// ---------------------------------------------------------------------------

struct DeductionResult {
    char    certain[25];
    Heatmap heatmap;
};

static DeductionResult getDeductions(const char revealedArr[25], int nColors) {
    initCharTable();

    DeductionResult dr;
    char* certain = dr.certain;
    memset(certain, 0, 25);
    for (auto& ch : dr.heatmap) ch.clear();

    char virtualRevealed[25];
    memcpy(virtualRevealed, revealedArr, 25);

    uint32_t vrMask = 0;
    for (int i = 0; i < 25; ++i) if (virtualRevealed[i]) vrMask |= 1u << i;

    ActiveShips active = getActiveShips(nColors, revealedArr);

    bool rarePresent[N_COLORS_DEF] = {};
    int  nRaresPresent = 0;
    for (int i = 0; i < 25; ++i) {
        char c = revealedArr[i];
        if (c && isRare(c)) {
            int ci = cidx(c);
            if (!rarePresent[ci]) { rarePresent[ci] = true; ++nRaresPresent; }
        }
    }
    int totalRaresExpected = nColors - 5;
    int missingRaresCount  = totalRaresExpected - nRaresPresent;

    // ---- PASS 1: GEOMETRIC CERTAINTY ----

    uint32_t cachedValidMasks[N_COLORS_DEF][40];
    int      cachedNValid[N_COLORS_DEF];
    memset(cachedNValid, 0, sizeof(cachedNValid));

    int colorCount[10] = {};
    for (int i = 0; i < 25; ++i) {
        char c = virtualRevealed[i];
        if (c) colorCount[cidx(c)]++;
    }

    bool foundNewCertainty = true;
    while (foundNewCertainty) {
        foundNewCertainty = false;

        for (int ai = 0; ai < active.n; ++ai) {
            char  color = active.letter[ai];
            int   si    = active.idx[ai];
            int   len   = shipLen(si);
            if (len == 0) continue;

            int ci = cidx(color);
            if (colorCount[ci] >= len) { cachedNValid[ai] = 0; continue; }

            uint32_t foundMask = 0;
            for (int i = 0; i < 25; ++i)
                if (virtualRevealed[i] == color) foundMask |= 1u << i;

            uint32_t conflictMask = vrMask & ~foundMask;

            const PlacementTable& pt = ptable(len);
            uint32_t validMasks[40];
            int      nValid = 0;
            for (int pi = 0; pi < pt.n; ++pi) {
                uint32_t m = pt.masks[pi];
                if ((m & foundMask) != foundMask) continue;
                if (m & conflictMask)              continue;
                validMasks[nValid++] = m;
            }

            memcpy(cachedValidMasks[ai], validMasks, nValid * sizeof(uint32_t));
            cachedNValid[ai] = nValid;

            if (nValid == 0) continue;

            uint32_t intersect = validMasks[0];
            for (int vi = 1; vi < nValid; ++vi) intersect &= validMasks[vi];
            intersect &= ~vrMask;

            if (!intersect) continue;

            uint32_t bits = intersect;
            while (bits) {
                int idx = __builtin_ctz(bits);
                bits &= bits - 1;
                virtualRevealed[idx] = color;
                vrMask              |= 1u << idx;
                certain[idx]         = color;
                dr.heatmap[idx].assign_single(color, 1.0);
                colorCount[ci]++;
                foundNewCertainty = true;
            }
        }
    }

    // ---- PASS 2: COMBINATORIAL SHIP WEIGHTS ----

    uint32_t revealedMask = 0;
    for (int i = 0; i < 25; ++i) if (revealedArr[i]) revealedMask |= 1u << i;
    uint32_t certainMask = 0;
    for (int i = 0; i < 25; ++i) if (certain[i]) certainMask |= 1u << i;
    uint32_t skipMask = revealedMask | certainMask;

    bool canContainShip[25] = {};

    for (int ai = 0; ai < active.n; ++ai) {
        char  color  = active.letter[ai];
        int   si     = active.idx[ai];
        int   len    = shipLen(si);
        if (len == 0) continue;

        int ci = cidx(color);
        if (colorCount[ci] >= len) continue;

        int nValid = cachedNValid[ai];
        if (nValid == 0) continue;

        double weight = 1.0 / (double)nValid;
        for (int vi = 0; vi < nValid; ++vi) {
            uint32_t m = cachedValidMasks[ai][vi];
            uint32_t cells = m & ~skipMask;
            while (cells) {
                int idx = __builtin_ctz(cells);
                cells &= cells - 1;
                dr.heatmap[idx][color] += weight;
                canContainShip[idx] = true;
            }
        }
    }

    // ---- PASS 3: RARE SHIP LOGIC ----
    // '?' weight is 1/nValid (missingRaresCount multiplier dropped vs. v1).

    if (missingRaresCount > 0) {
        char unidentifiedRares[N_COLORS_DEF];
        int  nUnidentified = 0;
        for (int i = 0; i < N_COLORS_DEF; ++i) {
            if (!ALL_COLORS[i].always && !rarePresent[i])
                unidentifiedRares[nUnidentified++] = ALL_COLORS[i].letter;
        }

        const PlacementTable& pt2 = ptable(2);
        uint32_t validMasks[40];
        int      nValid = 0;
        for (int pi = 0; pi < pt2.n; ++pi) {
            if (!(pt2.masks[pi] & vrMask))
                validMasks[nValid++] = pt2.masks[pi];
        }

        if (nValid > 0) {
            double qmarkWeight      = 1.0 / (double)nValid;
            double individualWeight = (nUnidentified > 0)
                ? 1.0 / ((double)nValid * (double)nUnidentified) : 0.0;

            for (int vi = 0; vi < nValid; ++vi) {
                uint32_t cells = validMasks[vi] & ~skipMask;
                while (cells) {
                    int idx = __builtin_ctz(cells);
                    cells &= cells - 1;
                    dr.heatmap[idx][QMARK] += qmarkWeight;
                    canContainShip[idx] = true;
                    for (int ri = 0; ri < nUnidentified; ++ri)
                        dr.heatmap[idx][unidentifiedRares[ri]] += individualWeight;
                }
            }
        }
    }

    // ---- PASS 4: SMART BLUE LOGIC ----
    // Implicit certain blues + no normalisation.

    int totalShipSegments  = 12 + totalRaresExpected * 2;
    int totalBluesExpected = 25 - totalShipSegments;
    int bluesFound = 0;
    for (int i = 0; i < 25; ++i) if (revealedArr[i] == 'B') ++bluesFound;

    // Step 1: cells that cannot contain any ship → certain blue.
    for (int i = 0; i < 25; ++i) {
        if (!revealedArr[i] && !certain[i] && !canContainShip[i]) {
            certain[i] = 'B';
            dr.heatmap[i].assign_single('B', 1.0);
            certainMask |= 1u << i;
        }
    }

    // Step 2: count all certain blues not yet revealed.
    int bluesCertainCount = 0;
    for (int i = 0; i < 25; ++i)
        if (certain[i] == 'B' && !revealedArr[i]) ++bluesCertainCount;

    // Step 3: distribute remaining blue probability across mystery cells.
    int bluesToDistribute = std::max(0, totalBluesExpected - (bluesFound + bluesCertainCount));

    int mysteryCellCount = 0;
    for (int i = 0; i < 25; ++i)
        if (!revealedArr[i] && !certain[i]) ++mysteryCellCount;

    double mysteryBlueProb = (mysteryCellCount > 0)
        ? (double)bluesToDistribute / (double)mysteryCellCount
        : 0.0;

    for (int i = 0; i < 25; ++i) {
        if (revealedArr[i] || certain[i]) continue;
        dr.heatmap[i]['B'] = mysteryBlueProb;
    }

    return dr;
}

// ---------------------------------------------------------------------------
// SvessinnV2State — parsed inputs + deduction result, ready for selection
// ---------------------------------------------------------------------------

struct SvessinnV2State {
    char           revealedArr[25];
    int            nColors;
    int            shipsHit;
    int            bluesFound;
    int            totalBluesExpected;
    bool           isHuntingBlue;
    uint32_t       availMask;
    DeductionResult dr;
};

// Parse board + meta into a SvessinnV2State.
// Call this at the top of every next_click implementation.
static SvessinnV2State svessinn_v2_parse(const std::vector<Cell>& board,
                                         const std::string& meta_json)
{
    SvessinnV2State st{};

    const char* mj = meta_json.c_str();
    const char* p;
    st.nColors  = 6;
    st.shipsHit = 0;
    if ((p = strstr(mj, "\"n_colors\":")))  { p += 11; while (*p == ' ') ++p; st.nColors  = atoi(p); }
    if ((p = strstr(mj, "\"ships_hit\":"))) { p += 12; while (*p == ' ') ++p; st.shipsHit = atoi(p); }

    memset(st.revealedArr, 0, 25);
    for (const Cell& cell : board) {
        if (!cell.clicked) continue;
        int  idx    = cell.row * 5 + cell.col;
        char letter = spToLetter(cell.color.c_str(), (int)cell.color.size());
        if (letter != '?') st.revealedArr[idx] = letter;
    }

    int totalRaresExpected  = st.nColors - 5;
    int totalShipSegments   = 12 + totalRaresExpected * 2;
    st.totalBluesExpected   = 25 - totalShipSegments;

    st.bluesFound = 0;
    for (int i = 0; i < 25; ++i) if (st.revealedArr[i] == 'B') ++st.bluesFound;

    // JS: isHuntingBlue = nonBlueFound < 5 && !allBluesFound
    st.isHuntingBlue = (st.shipsHit < 5) && (st.bluesFound < st.totalBluesExpected);

    st.availMask = 0;
    for (int i = 0; i < 25; ++i)
        if (!st.revealedArr[i]) st.availMask |= 1u << i;

    st.dr = getDeductions(st.revealedArr, st.nColors);
    return st;
}

// Emit an immediate click for a certain non-blue cell if one exists.
// Returns true and sets out if an early click was made; caller should return.
// This mirrors the JS "SAFE" cell logic — certain non-blue cells are always
// optimal to click regardless of phase.
static bool svessinn_v2_certain_early_return(const SvessinnV2State& st, ClickResult& out) {
    uint32_t bits = st.availMask;
    while (bits) {
        int idx = __builtin_ctz(bits);
        bits &= bits - 1;
        if (st.dr.certain[idx] && st.dr.certain[idx] != 'B') {
            out.row = idx / 5; out.col = idx % 5;
            return true;
        }
    }
    return false;
}

// Fallback: click the lowest-index available cell.
static void svessinn_v2_fallback(const SvessinnV2State& st, ClickResult& out) {
    if (st.availMask) {
        int idx = __builtin_ctz(st.availMask);
        out.row = idx / 5; out.col = idx % 5;
    } else {
        out.row = 0; out.col = 0;
    }
}

// ---------------------------------------------------------------------------
// Board JSON parser (used inside SVESSINN_V2_EXPORTS)
// ---------------------------------------------------------------------------

static std::vector<Cell> svessinn_v2_parse_board_json(const char* board_json) {
    std::vector<Cell> board;
    board.reserve(25);
    const char* p = board_json;
    while (*p && *p != '{') ++p;

    while (*p == '{') {
        Cell c{};
        const char* f = strstr(p, "\"row\":");
        if (!f) break;
        f += 6; while (*f == ' ') ++f;
        c.row = (int8_t)atoi(f);
        while (*f && *f != ',' && *f != '}') ++f;

        f = strstr(f, "\"col\":");
        if (!f) break;
        f += 6; while (*f == ' ') ++f;
        c.col = (int8_t)atoi(f);
        while (*f && *f != ',' && *f != '}') ++f;

        f = strstr(f, "\"color\":\"");
        if (!f) break;
        f += 9;
        const char* e = strchr(f, '"');
        if (!e) break;
        c.color = std::string(f, e - f);
        f = e + 1;

        f = strstr(f, "\"clicked\":");
        if (!f) break;
        f += 10; while (*f == ' ') ++f;
        c.clicked = (strncmp(f, "true", 4) == 0);
        while (*f && *f != '}') ++f;

        board.push_back(c);
        while (*f && *f != '{' && *f != ']') ++f;
        p = f;
    }
    return board;
}

// ---------------------------------------------------------------------------
// SVESSINN_V2_EXPORTS(ClassName)
// ---------------------------------------------------------------------------
// Emits all five extern "C" symbols required by the harness.
// Place once at the bottom of each strategy .cpp after the class definition.

#define SVESSINN_V2_EXPORTS(ClassName)                                              \
extern "C" sphere::StrategyBase* create_strategy()                                 \
    { return new ClassName(); }                                                     \
extern "C" void destroy_strategy(sphere::StrategyBase* s)                          \
    { delete s; }                                                                   \
extern "C" void strategy_init_evaluation_run(void* inst)                           \
    { static_cast<ClassName*>(inst)->init_evaluation_run(); }                      \
extern "C" void strategy_init_game_payload(void* inst, const char* meta_json)      \
    { static_cast<ClassName*>(inst)->init_game_payload(                            \
        meta_json ? meta_json : "{}"); }                                            \
extern "C" const char* strategy_next_click(void* inst,                             \
                                            const char* board_json,                 \
                                            const char* meta_json)                  \
{                                                                                   \
    thread_local static std::string _svbuf;                                        \
    auto* s = static_cast<ClassName*>(inst);                                       \
    std::vector<Cell> board = svessinn_v2_parse_board_json(board_json);            \
    ClickResult out;                                                                \
    s->next_click(board, meta_json ? meta_json : "{}", out);                       \
    _svbuf = "{\"row\":" + std::to_string(out.row) +                               \
             ",\"col\":" + std::to_string(out.col) + "}";                          \
    return _svbuf.c_str();                                                          \
}
