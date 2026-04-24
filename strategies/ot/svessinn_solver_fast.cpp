/**
 * svessinn_solver_fast.cpp — Optimised C++ port of Svessinn's Trace solver.
 *
 * Identical algorithm to svessinn_solver.cpp / svessinn_solver.js.
 * Numeric parity with the JS reference is preserved (see notes on CellHeat).
 *
 * Optimisations vs svessinn_solver.cpp:
 *
 *  1. char[25] flat arrays instead of unordered_map<int,char>
 *     virtualRevealed, certain, revealedArr — O(1) indexed lookup, no hashing.
 *
 *  2. uint32_t bitmask placements
 *     All placement tables are static uint32_t arrays (one bit per cell).
 *     Membership, inclusion, and conflict checks collapse to single bitwise ops.
 *     Pass 1 intersection = AND-reduce of valid placement masks: O(|valid|) not
 *     O(|valid|^2 * len).
 *
 *  3. CellHeat: double[10] + insertion-order tracking
 *     Replaces InsertionMap (vector<pair<char,double>>, linear scan).
 *     Lookups are O(1) via a 128-byte charToIdx table.
 *     sum() / divide_all() iterate the insertion-order array, so Pass 4
 *     normalisation accumulates in the same key order as the JS Object does —
 *     preserving exact floating-point parity with svessinn_solver.js.
 *
 *  4. Cache Pass 1 final validMasks per ship for reuse in Pass 2
 *     After the while-loop settles, validMasks[ship] already holds the answer
 *     Pass 2 needs.  Pass 2 skips the re-enumeration entirely.
 *
 *  5. Incremental colorCount[] in Pass 1
 *     colorCount[10] is initialised once and updated by +1 each time a
 *     certainty is added, eliminating the full virtualRevealed scan that
 *     previously rebuilt counts every iteration.
 *
 *  6. Sequential single-pass board JSON parser
 *     The original parser called strstr from the start of each cell's "row":
 *     key for every subsequent field — O(N^2) total.  The new parser advances
 *     a single pointer forward after each field, making the parse O(N).
 *
 *  7. Stack-allocated validMasks[40] buffers
 *     Inner-loop placement filtering uses uint32_t validMasks[40] + int nValid
 *     on the stack instead of std::vector<uint32_t>, eliminating heap
 *     allocation in the tightest loops.
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
// Letters used: B T G Y O L D R W ? (10 total)
// Index is assigned in the order they first appear in ALL_COLORS so that
// CellHeat's insertion-order iteration matches the JS Object key order.
// ---------------------------------------------------------------------------

// Map ASCII char → index 0..9 (255 = unknown).
static uint8_t charToIdx[128];
// Map index → char (for completeness; not used in hot paths).
static char idxToChar[10];
static bool charTableInit = false;

// Color definitions — same order as ALL_COLORS in solver.html / svessinn_solver.cpp.
struct ShipDef { char letter; int len; bool always; };
static const ShipDef ALL_COLORS[] = {
    { 'B', 0, true  },
    { 'T', 4, true  },
    { 'G', 3, true  },
    { 'Y', 3, true  },
    { 'O', 2, true  },
    { 'L', 2, true  },
    { 'D', 2, false },
    { 'R', 2, false },
    { 'W', 2, false },
};
static const int N_COLORS_DEF = 9;
// '?' is not in ALL_COLORS but is used as a heatmap key (rare identity unknown).
static const char QMARK = '?';

static void initCharTable() {
    if (charTableInit) return;
    memset(charToIdx, 255, sizeof(charToIdx));
    for (int i = 0; i < N_COLORS_DEF; ++i) {
        charToIdx[(uint8_t)ALL_COLORS[i].letter] = (uint8_t)i;
        idxToChar[i] = ALL_COLORS[i].letter;
    }
    // '?' gets index 9 (appended after the 9 defined colors, index 0..8)
    charToIdx[(uint8_t)QMARK] = 9;
    idxToChar[9] = QMARK;
    charTableInit = true;
}

static inline int cidx(char c) { return (int)(uint8_t)charToIdx[(uint8_t)c]; }
static inline int shipLen(int si)  { return ALL_COLORS[si].len; }
static inline bool isAlways(int si){ return ALL_COLORS[si].always; }
static inline bool isRare(char c)  {
    uint8_t i = charToIdx[(uint8_t)c];
    return i < N_COLORS_DEF && !ALL_COLORS[i].always;
}

static char spToLetter(const char* sp, int len) {
    // sp is e.g. "spB", "spT", etc.  len is the string length.
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
// One uint32_t per placement, bit i = cell i is covered.
// ---------------------------------------------------------------------------

struct PlacementTable {
    uint32_t masks[40];  // max 40 for len=2
    int      n;
};

static PlacementTable buildTable(int len) {
    PlacementTable t; t.n = 0;
    // Horizontal
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c <= 5 - len; ++c) {
            uint32_t m = 0;
            for (int i = 0; i < len; ++i) m |= 1u << (r * 5 + c + i);
            t.masks[t.n++] = m;
        }
    // Vertical
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
// CellHeat — Opt 3
// Replaces InsertionMap.  Uses a fixed double[10] array (one slot per color
// index) plus uint8_t order[10] / int n_keys to track insertion order.
// sum() and divide_all() iterate order[] so that floating-point accumulation
// happens in the same sequence as JS Object key iteration — preserving parity.
// ---------------------------------------------------------------------------

struct CellHeat {
    double  v[10];
    uint8_t order[10];  // indices in insertion order
    int     n_keys;

    void clear() {
        memset(v, 0, sizeof(v));
        n_keys = 0;
    }

    // Return reference; insert key with 0.0 if absent (preserves insertion order).
    double& operator[](char k) {
        int i = cidx(k);
        // If this key hasn't been seen yet, record it in order[].
        if (v[i] == 0.0) {
            // Check if already in order (could be 0.0 legitimately — use a
            // presence flag via the order array itself).
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

// ---------------------------------------------------------------------------
// Heatmap: 25 CellHeat entries
// ---------------------------------------------------------------------------

using Heatmap = std::array<CellHeat, 25>;

// ---------------------------------------------------------------------------
// getActiveShips — same logic as svessinn_solver.cpp
// Uses char[25] revealedArr instead of unordered_map (Opt 1).
// Returns parallel arrays of (ship-def index, letter) for active ships.
// ---------------------------------------------------------------------------

struct ActiveShips {
    int   idx[N_COLORS_DEF];  // index into ALL_COLORS
    char  letter[N_COLORS_DEF];
    int   n;
};

static ActiveShips getActiveShips(int nColors, const char revealedArr[25]) {
    // Identify rare colors present in revealedArr
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
        // Include all ships (common + all optional rares)
        for (int i = 0; i < N_COLORS_DEF; ++i) {
            as.idx[as.n]    = i;
            as.letter[as.n] = ALL_COLORS[i].letter;
            ++as.n;
        }
    } else {
        // Common ships + identified rares only
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
// getDeductions — fully optimised
// ---------------------------------------------------------------------------

struct DeductionResult {
    char    certain[25];   // '\0' = not certain; else color letter (Opt 1)
    Heatmap heatmap;
};

static DeductionResult getDeductions(const char revealedArr[25], int nColors) {
    initCharTable();

    DeductionResult dr;
    char* certain = dr.certain;
    memset(certain, 0, 25);
    for (auto& ch : dr.heatmap) ch.clear();

    // virtualRevealed: char[25], '\0' = unset (Opt 1)
    char virtualRevealed[25];
    memcpy(virtualRevealed, revealedArr, 25);

    // Build bitmasks for revealedArr and virtualRevealed (Opt 2)
    // vrMask: bit i set if virtualRevealed[i] != 0
    uint32_t vrMask = 0;
    for (int i = 0; i < 25; ++i) if (virtualRevealed[i]) vrMask |= 1u << i;

    ActiveShips active = getActiveShips(nColors, revealedArr);

    // Rare-identity bookkeeping (same as original)
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

    // ---- PASS 1: INTERSECTION (GEOMETRIC CERTAINTY) ---- (Opt 4, 5, 7)
    //
    // Per-ship valid placement cache: we fill these during Pass 1's final
    // iteration and reuse them in Pass 2 (Opt 4).
    uint32_t cachedValidMasks[N_COLORS_DEF][40];
    int      cachedNValid[N_COLORS_DEF];
    memset(cachedNValid, 0, sizeof(cachedNValid));

    // Incremental colorCount (Opt 5): count cells per color in virtualRevealed.
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
            if (len == 0) continue;  // skip blue

            int ci = cidx(color);
            if (colorCount[ci] >= len) { cachedNValid[ai] = 0; continue; }

            // foundMask: bits for cells of this color in virtualRevealed
            uint32_t foundMask = 0;
            for (int i = 0; i < 25; ++i)
                if (virtualRevealed[i] == color) foundMask |= 1u << i;

            // conflictMask: bits for cells assigned to a DIFFERENT color
            uint32_t conflictMask = vrMask & ~foundMask;

            // Filter placements (Opt 7: stack buffer)
            const PlacementTable& pt = ptable(len);
            uint32_t validMasks[40];
            int      nValid = 0;
            for (int pi = 0; pi < pt.n; ++pi) {
                uint32_t m = pt.masks[pi];
                if ((m & foundMask) != foundMask) continue;   // missing a found cell
                if (m & conflictMask)              continue;   // overlaps a different color
                validMasks[nValid++] = m;
            }

            // Cache for Pass 2 (Opt 4)
            memcpy(cachedValidMasks[ai], validMasks, nValid * sizeof(uint32_t));
            cachedNValid[ai] = nValid;

            if (nValid == 0) continue;

            // Intersection: AND-reduce all valid masks, then mask off already-set cells (Opt 2)
            uint32_t intersect = validMasks[0];
            for (int vi = 1; vi < nValid; ++vi) intersect &= validMasks[vi];
            intersect &= ~vrMask;  // only unset cells

            if (!intersect) continue;

            // Each set bit in intersect is a new certainty
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
                // Update cache: re-filter is deferred to next iteration
                // (cachedNValid[ai] will be recomputed next time around)
            }
        }
    }

    // ---- PASS 2: SHIP WEIGHTS ---- (Opt 4: reuse cachedValidMasks)
    // revealedMask: bits for cells in the ORIGINAL revealedArr
    uint32_t revealedMask = 0;
    for (int i = 0; i < 25; ++i) if (revealedArr[i]) revealedMask |= 1u << i;
    // certainMask: bits for deduced-certain cells
    uint32_t certainMask = 0;
    for (int i = 0; i < 25; ++i) if (certain[i]) certainMask |= 1u << i;
    uint32_t skipMask = revealedMask | certainMask;

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
            }
        }
    }

    // ---- PASS 3: IDENTITY-AWARE RARE LOGIC ----
    if (missingRaresCount > 0) {
        // Unidentified rare letters (in original ALL_COLORS order for JS parity)
        char unidentifiedRares[N_COLORS_DEF];
        int  nUnidentified = 0;
        for (int i = 0; i < N_COLORS_DEF; ++i) {
            if (!ALL_COLORS[i].always && !rarePresent[i])
                unidentifiedRares[nUnidentified++] = ALL_COLORS[i].letter;
        }

        // Valid size-2 placements where ALL cells are free in virtualRevealed
        const PlacementTable& pt2 = ptable(2);
        uint32_t validMasks[40];
        int      nValid = 0;
        for (int pi = 0; pi < pt2.n; ++pi) {
            if (!(pt2.masks[pi] & vrMask))
                validMasks[nValid++] = pt2.masks[pi];
        }

        if (nValid > 0) {
            double categoryWeight   = missingRaresCount / (double)nValid;
            double individualWeight = (nUnidentified > 0)
                ? categoryWeight / (double)nUnidentified : 0.0;

            for (int vi = 0; vi < nValid; ++vi) {
                uint32_t cells = validMasks[vi] & ~skipMask;
                while (cells) {
                    int idx = __builtin_ctz(cells);
                    cells &= cells - 1;
                    dr.heatmap[idx][QMARK] += categoryWeight;
                    for (int ri = 0; ri < nUnidentified; ++ri)
                        dr.heatmap[idx][unidentifiedRares[ri]] += individualWeight;
                }
            }
        }
    }

    // ---- PASS 4: BLUE NORMALISATION ----
    int totalShipSegments  = 12 + totalRaresExpected * 2;
    int totalBluesExpected = 25 - totalShipSegments;
    int bluesFound = 0;
    for (int i = 0; i < 25; ++i) if (revealedArr[i] == 'B') ++bluesFound;

    int nUnresolved = 0;
    for (int i = 0; i < 25; ++i)
        if (!revealedArr[i] && !certain[i]) ++nUnresolved;

    double blueWeight = (nUnresolved > 0)
        ? std::max(0.0, (double)(totalBluesExpected - bluesFound) / (double)nUnresolved)
        : 0.0;

    for (int i = 0; i < 25; ++i) {
        if (revealedArr[i] || certain[i]) continue;
        dr.heatmap[i]['B'] = blueWeight;  // appends B in insertion order (JS parity)
        double total = dr.heatmap[i].sum();
        if (total > 0.0) {
            dr.heatmap[i].divide_all(total);
        } else {
            dr.heatmap[i].assign_single('B', 1.0);
        }
    }

    return dr;
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------

class SvessinnFastOTStrategy : public OTStrategy {
public:
    void init_game_payload(const std::string& /*meta_json*/) override {}

    void next_click(const std::vector<Cell>& board,
                    const std::string& meta_json,
                    ClickResult& out) override
    {
        // Parse meta (unchanged from original)
        const char* mj = meta_json.c_str();
        int nColors  = 6, shipsHit = 0;
        const char* p;
        if ((p = strstr(mj, "\"n_colors\":")))  { p += 11; while (*p == ' ') ++p; nColors  = atoi(p); }
        if ((p = strstr(mj, "\"ships_hit\":"))) { p += 12; while (*p == ' ') ++p; shipsHit = atoi(p); }

        // Build revealedArr from clicked cells (Opt 1: char[25]) 
        char revealedArr[25] = {};
        for (const Cell& cell : board) {
            if (!cell.clicked) continue;
            int idx = cell.row * 5 + cell.col;
            char letter = spToLetter(cell.color.c_str(), (int)cell.color.size());
            if (letter != '?') revealedArr[idx] = letter;
        }

        DeductionResult dr = getDeductions(revealedArr, nColors);
        const char* certain   = dr.certain;
        const Heatmap& heatmap = dr.heatmap;

        bool isHuntingBlue = (shipsHit < 5);

        // Available cells
        uint32_t availMask = 0;
        for (int i = 0; i < 25; ++i)
            if (!revealedArr[i]) availMask |= 1u << i;

        // Click any certain non-blue cell immediately
        uint32_t bits = availMask;
        while (bits) {
            int idx = __builtin_ctz(bits);
            bits &= bits - 1;
            if (certain[idx] && certain[idx] != 'B') {
                out.row = idx / 5; out.col = idx % 5; return;
            }
        }

        // During ship-hunting phase, click any near-certain ship cell (P(blue) < 0.001)
        if (!isHuntingBlue) {
            bits = availMask;
            while (bits) {
                int idx = __builtin_ctz(bits);
                bits &= bits - 1;
                if (!certain[idx] && heatmap[idx].get('B') < 0.001) {
                    out.row = idx / 5; out.col = idx % 5; return;
                }
            }
        }

        // Pick best move based on phase
        int targetIdx = -1;
        if (availMask) {
            if (isHuntingBlue) {
                double maxBlue = -1.0;
                bits = availMask;
                while (bits) {
                    int idx = __builtin_ctz(bits); bits &= bits - 1;
                    double bp = heatmap[idx].get('B');
                    if (bp > maxBlue) { maxBlue = bp; targetIdx = idx; }
                }
            } else {
                double minBlue = std::numeric_limits<double>::infinity();
                bits = availMask;
                while (bits) {
                    int idx = __builtin_ctz(bits); bits &= bits - 1;
                    double bp = heatmap[idx].get('B');
                    if (bp < minBlue) { minBlue = bp; targetIdx = idx; }
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

extern "C" sphere::StrategyBase* create_strategy()                         { return new SvessinnFastOTStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* inst) {
    static_cast<SvessinnFastOTStrategy*>(inst)->init_evaluation_run();
}

extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {
    static_cast<SvessinnFastOTStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
}

// Opt 6: sequential single-pass board JSON parser
extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<SvessinnFastOTStrategy*>(inst);

    std::vector<Cell> board;
    board.reserve(25);
    const char* p = board_json;
    // Advance p past the opening '[' if present
    while (*p && *p != '{') ++p;

    while (*p == '{') {
        Cell c{};
        // "row": N
        const char* f = strstr(p, "\"row\":");
        if (!f) break;
        f += 6; while (*f == ' ') ++f;
        c.row = (int8_t)atoi(f);
        while (*f && *f != ',' && *f != '}') ++f;  // advance past value

        // "col": N  — search forward from f
        f = strstr(f, "\"col\":");
        if (!f) break;
        f += 6; while (*f == ' ') ++f;
        c.col = (int8_t)atoi(f);
        while (*f && *f != ',' && *f != '}') ++f;

        // "color": "spX"  — search forward from f
        f = strstr(f, "\"color\":\"");
        if (!f) break;
        f += 9;
        const char* e = strchr(f, '"');
        if (!e) break;
        c.color = std::string(f, e - f);
        f = e + 1;

        // "clicked": true/false  — search forward from f
        f = strstr(f, "\"clicked\":");
        if (!f) break;
        f += 10; while (*f == ' ') ++f;
        c.clicked = (strncmp(f, "true", 4) == 0);
        while (*f && *f != '}') ++f;

        board.push_back(c);
        // Advance to next cell object
        while (*f && *f != '{' && *f != ']') ++f;
        p = f;
    }

    ClickResult out;
    s->next_click(board, meta_json ? meta_json : "{}", out);
    buf = "{\"row\":" + std::to_string(out.row) + ",\"col\":" + std::to_string(out.col) + "}";
    return buf.c_str();
}
