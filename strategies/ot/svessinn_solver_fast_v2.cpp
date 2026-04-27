// sphere:stateless
/**
 * svessinn_solver_fast_v2.cpp — Optimised C++ port of Svessinn's updated Trace solver.
 *
 * Source diff: https://github.com/Svessinn/Svessinn.github.io/commit/ffa7c3ba6cfe07725d4074c1518b031c9eb7c9e1
 *
 * Based on svessinn_solver_fast.cpp with the following changes to match the
 * updated solver.html:
 *
 *  v2 Change 1 — ALL_COLORS reorder + L demoted from always:true to always:false
 *     Old order/flags: B(T), T(T), G(T), Y(T), O(T), L(T), D(F), R(F), W(F)
 *     New order/flags: B(T), T(T), G(T), Y(T), O(T), R(F), W(F), L(F), D(F)
 *     Common ships drop from 6 to 5; L joins the rare pool {R,W,L,D}.
 *     6-color games now have 1 rare slot (previously 0).
 *
 *  v2 Change 2 — canContainShip[25] tracking (prerequisite for new Pass 4)
 *     Pass 2 and Pass 3 set canContainShip[idx]=true for every cell that
 *     receives any ship heatmap weight.
 *
 *  v2 Change 3 — Pass 3 '?' weight formula changed
 *     Old: categoryWeight = missingRaresCount / validPos.length
 *     New: 1.0 / validPos.length  (missingRaresCount multiplier dropped)
 *     individualWeight stays 1.0 / (validPos.length * nUnidentified).
 *
 *  v2 Change 4 — Pass 4 completely rewritten (implicit certain blues, no normalisation)
 *     Old: spread blueWeight = (totalBluesExpected - bluesFound) / nUnresolved
 *          across all non-revealed non-certain cells, then normalise heatmap to sum 1.
 *     New:
 *       Step 1: cells that cannot contain any ship → promote to certain['B'],
 *               heatmap = {B:1}.  ("Implicitly Certain Blues")
 *       Step 2: count all certain blues (geometric + implicit), subtract from
 *               totalBluesExpected together with already-revealed blues to get
 *               bluesToDistribute.
 *       Step 3: spread mysteryBlueProb = bluesToDistribute / mysteryCells.count
 *               across remaining mystery cells.  NO normalisation.
 *
 *  v2 Change 5 — Selection metric: ship score sum instead of P(Blue)
 *     Old: hunting blue → max P(B); hunting ships → min P(B).
 *     New: shipSum = sum of all non-B heatmap weights;
 *          hunting blue → min(shipSum); hunting ships → max(shipSum).
 *     The P(B) < 0.001 near-certain-ship early-return is removed (absent from new HTML).
 *
 * All other optimisations from svessinn_solver_fast.cpp are preserved:
 *   - char[25] flat arrays (Opt 1)
 *   - uint32_t bitmask placements (Opt 2)
 *   - CellHeat struct with insertion-order tracking (Opt 3)
 *   - Pass 1 validMasks cached for Pass 2 reuse (Opt 4)
 *   - Incremental colorCount[] in Pass 1 (Opt 5)
 *   - Sequential single-pass board JSON parser (Opt 6)
 *   - Stack-allocated validMasks[40] buffers (Opt 7)
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
// Letters used: B T G Y O R W L D ? (10 total)
// Index is assigned in the order they appear in ALL_COLORS so that
// CellHeat's insertion-order iteration matches the JS Object key order.
// ---------------------------------------------------------------------------

static uint8_t charToIdx[128];
static char    idxToChar[10];
static bool    charTableInit = false;

// v2 Change 1: new order and always flags.
// Old: B(T) T(T) G(T) Y(T) O(T) L(T) D(F) R(F) W(F)
// New: B(T) T(T) G(T) Y(T) O(T) R(F) W(F) L(F) D(F)
struct ShipDef { char letter; int len; bool always; };
static const ShipDef ALL_COLORS[] = {
    { 'B', 0, true  },
    { 'T', 4, true  },
    { 'G', 3, true  },
    { 'Y', 3, true  },
    { 'O', 2, true  },
    { 'R', 2, false },   // was index 7 (rare), now index 5
    { 'W', 2, false },   // was index 8 (rare), now index 6
    { 'L', 2, false },   // was index 5 (always:true), now index 7 (always:false)
    { 'D', 2, false },   // was index 6 (rare), now index 8
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
    // '?' gets index 9
    charToIdx[(uint8_t)QMARK] = 9;
    idxToChar[9] = QMARK;
    charTableInit = true;
}

static inline int cidx(char c) { return (int)(uint8_t)charToIdx[(uint8_t)c]; }
static inline int shipLen(int si)   { return ALL_COLORS[si].len; }
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
// Bitmask placement tables (Opt 2)
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
// CellHeat (Opt 3)
// ---------------------------------------------------------------------------

struct CellHeat {
    double  v[10];
    uint8_t order[10];
    int     n_keys;

    void clear() {
        memset(v, 0, sizeof(v));
        n_keys = 0;
    }

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

    double sum() const {
        double t = 0.0;
        for (int j = 0; j < n_keys; ++j) t += v[order[j]];
        return t;
    }
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
// getDeductions
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

    // ---- PASS 1: GEOMETRIC CERTAINTY (Opt 4, 5, 7) ----

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

    // ---- PASS 2: SHIP WEIGHTS (Opt 4) ----
    // v2 Change 2: track canContainShip[].

    uint32_t revealedMask = 0;
    for (int i = 0; i < 25; ++i) if (revealedArr[i]) revealedMask |= 1u << i;
    uint32_t certainMask = 0;
    for (int i = 0; i < 25; ++i) if (certain[i]) certainMask |= 1u << i;
    uint32_t skipMask = revealedMask | certainMask;

    bool canContainShip[25] = {};  // v2 Change 2

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
                canContainShip[idx] = true;  // v2 Change 2
            }
        }
    }

    // ---- PASS 3: RARE SHIP LOGIC ----
    // v2 Change 3: '?' weight is now 1/nValid (not missingRaresCount/nValid).
    // v2 Change 2: set canContainShip[idx] for cells receiving rare weight.

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
            // v2 Change 3: weight for '?' is 1/nValid (dropped missingRaresCount multiplier)
            double qmarkWeight     = 1.0 / (double)nValid;
            double individualWeight = (nUnidentified > 0)
                ? 1.0 / ((double)nValid * (double)nUnidentified) : 0.0;

            for (int vi = 0; vi < nValid; ++vi) {
                uint32_t cells = validMasks[vi] & ~skipMask;
                while (cells) {
                    int idx = __builtin_ctz(cells);
                    cells &= cells - 1;
                    dr.heatmap[idx][QMARK] += qmarkWeight;
                    canContainShip[idx] = true;  // v2 Change 2
                    for (int ri = 0; ri < nUnidentified; ++ri)
                        dr.heatmap[idx][unidentifiedRares[ri]] += individualWeight;
                }
            }
        }
    }

    // ---- PASS 4: REVISED SMART BLUE LOGIC ----
    // v2 Change 4: implicit certain blues + no normalisation.

    int totalShipSegments  = 12 + totalRaresExpected * 2;
    int totalBluesExpected = 25 - totalShipSegments;
    int bluesFound = 0;
    for (int i = 0; i < 25; ++i) if (revealedArr[i] == 'B') ++bluesFound;

    // Step 1: promote cells that cannot contain any ship to certain blue.
    for (int i = 0; i < 25; ++i) {
        if (!revealedArr[i] && !certain[i] && !canContainShip[i]) {
            certain[i] = 'B';
            dr.heatmap[i].assign_single('B', 1.0);
            certainMask |= 1u << i;
        }
    }

    // Step 2: count all certain blues (geometric + implicit) not yet in revealedArr.
    int bluesCertainCount = 0;
    for (int i = 0; i < 25; ++i)
        if (certain[i] == 'B' && !revealedArr[i]) ++bluesCertainCount;

    // Step 3: blues left to distribute among mystery cells.
    int bluesToDistribute = std::max(0, totalBluesExpected - (bluesFound + bluesCertainCount));

    // Step 4: mystery cells = not revealed and not certain.
    int mysteryCellCount = 0;
    for (int i = 0; i < 25; ++i)
        if (!revealedArr[i] && !certain[i]) ++mysteryCellCount;

    double mysteryBlueProb = (mysteryCellCount > 0)
        ? (double)bluesToDistribute / (double)mysteryCellCount
        : 0.0;

    // Step 5: apply blue probability — no normalisation.
    for (int i = 0; i < 25; ++i) {
        if (revealedArr[i] || certain[i]) continue;
        dr.heatmap[i]['B'] = mysteryBlueProb;
    }

    return dr;
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------

class SvessinnFastV2OTStrategy : public OTStrategy {
public:
    void init_game_payload(const std::string& /*meta_json*/) override {}

    void next_click(const std::vector<Cell>& board,
                    const std::string& meta_json,
                    ClickResult& out) override
    {
        const char* mj = meta_json.c_str();
        int nColors  = 6, shipsHit = 0;
        const char* p;
        if ((p = strstr(mj, "\"n_colors\":")))  { p += 11; while (*p == ' ') ++p; nColors  = atoi(p); }
        if ((p = strstr(mj, "\"ships_hit\":"))) { p += 12; while (*p == ' ') ++p; shipsHit = atoi(p); }

        char revealedArr[25] = {};
        for (const Cell& cell : board) {
            if (!cell.clicked) continue;
            int idx = cell.row * 5 + cell.col;
            char letter = spToLetter(cell.color.c_str(), (int)cell.color.size());
            if (letter != '?') revealedArr[idx] = letter;
        }

        DeductionResult dr = getDeductions(revealedArr, nColors);
        const char*    certain = dr.certain;
        const Heatmap& heatmap = dr.heatmap;

        bool isHuntingBlue = (shipsHit < 5);

        uint32_t availMask = 0;
        for (int i = 0; i < 25; ++i)
            if (!revealedArr[i]) availMask |= 1u << i;

        // Click any certain non-blue cell immediately.
        uint32_t bits = availMask;
        while (bits) {
            int idx = __builtin_ctz(bits);
            bits &= bits - 1;
            if (certain[idx] && certain[idx] != 'B') {
                out.row = idx / 5; out.col = idx % 5; return;
            }
        }

        // v2 Change 5: select by ship score sum (not P(Blue)).
        // The P(B) < 0.001 near-certain shortcut is removed (absent from new HTML).
        int targetIdx = -1;
        if (availMask) {
            if (isHuntingBlue) {
                // Hunting blues: pick cell with LOWEST ship probability.
                double minShip = std::numeric_limits<double>::infinity();
                bits = availMask;
                while (bits) {
                    int idx = __builtin_ctz(bits); bits &= bits - 1;
                    double shipSum = heatmap[idx].sum() - heatmap[idx].get('B');
                    if (shipSum < minShip) { minShip = shipSum; targetIdx = idx; }
                }
            } else {
                // Hunting ships: pick cell with HIGHEST ship probability.
                double maxShip = -1.0;
                bits = availMask;
                while (bits) {
                    int idx = __builtin_ctz(bits); bits &= bits - 1;
                    double shipSum = heatmap[idx].sum() - heatmap[idx].get('B');
                    if (shipSum > maxShip) { maxShip = shipSum; targetIdx = idx; }
                }
            }
        }

        if (targetIdx == -1 && availMask) targetIdx = __builtin_ctz(availMask);
        if (targetIdx == -1) { out.row = 0; out.col = 0; return; }
        out.row = targetIdx / 5;
        out.col = targetIdx % 5;
    }
};

// ---------------------------------------------------------------------------
// C exports required by the harness
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new SvessinnFastV2OTStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* inst) {
    static_cast<SvessinnFastV2OTStrategy*>(inst)->init_evaluation_run();
}

extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {
    static_cast<SvessinnFastV2OTStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
}

// Opt 6: sequential single-pass board JSON parser
extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<SvessinnFastV2OTStrategy*>(inst);

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

    ClickResult out;
    s->next_click(board, meta_json ? meta_json : "{}", out);
    buf = "{\"row\":" + std::to_string(out.row) + ",\"col\":" + std::to_string(out.col) + "}";
    return buf.c_str();
}
