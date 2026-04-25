// sphere:stateless
// Disable FMA contraction so floating-point arithmetic matches JavaScript's
// strict IEEE 754 double precision (no fused multiply-add).  Without this,
// g++ -march=native folds expressions like `p * pC * cVal` in the rollout DP
// into single FMA instructions that round once instead of twice, producing
// ULP-level EV differences from V8's two-step rounding.  Those ULP diffs
// flip sort tie-breaks, causing different move choices and genuinely divergent
// game scores.
#pragma GCC optimize("fp-contract=off")

/**
 * kelinimo_expectimax.cpp — 1-ply expectimax + greedy DP strategy for /sphere trace (ot).
 *
 * Faithful C++ port of kelinimo_expectimax.js.
 *
 * Algorithm (ported from solvespheres.kelinimo.workers.dev / ot.js):
 *
 *   1. Precompute all valid run-placement bitmasks for teal(4), green(3),
 *      yellow(3), and rare(2) ships, plus all non-overlapping rare-combo sets
 *      for each n_rare value (2–5), in init_evaluation_run().
 *
 *   2. Each call, rebuild `knownByIdx` and `rareKnown` from the board, then
 *      run the constraint engine (otComputeProbs) to compute per-cell
 *      marginal probabilities of being teal / green / yellow / rare / blue.
 *
 *   3. Score each unrevealed cell with a 1-ply expectimax lookahead followed
 *      by a greedy DP rollout (otScoreMove + otRolloutValue).
 *
 *   4. After grace ends, if any cell is guaranteed non-blue, promote the
 *      highest-EV certain cell to the top.
 *
 * Color mapping (harness sp-codes → internal indices):
 *   spT=0 teal    (run length 4)
 *   spG=1 green   (run length 3)
 *   spY=2 yellow  (run length 3)
 *   spO/spL/spD/spR/spW = rare  (run length 2)
 *   spB = blue    (empty, costs a click)
 *
 * State: all per-run data in member variables; no per-game state needed
 *   (strategy is stateless across games — rebuilt from board each call).
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "../../interface/strategy.h"

using namespace sphere;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const double OT_VAL_BLUE    = 10.0;
static const double OT_VAL_TEAL    = 20.0;
static const double OT_VAL_GREEN   = 35.0;
static const double OT_VAL_YELLOW  = 55.0;
static const double OT_VAL_ORANGE  = 90.0;
static const double OT_VAL_RED     = 150.0;
static const double OT_VAL_RAINBOW = 500.0;
static const double OT_VAL_LIGHT   = 80.0;
static const double OT_VAL_DARK    = 104.0;

// Mean rare value (all 5 rare types equally likely when unknown)
static const double OT_RARE_EV_UNK =
    (OT_VAL_ORANGE + OT_VAL_RED + OT_VAL_LIGHT + OT_VAL_DARK + OT_VAL_RAINBOW) / 5.0;

// DP dimensions (mirrors JS _DP_K and _DP_G)
static const int DP_K = 4;  // paid blues dimension: 0..3
static const int DP_G = 6;  // colored dimension: 0..5 (5 = sentinel "grace over")

// ---------------------------------------------------------------------------
// Color name → rare value lookup (matching JS OT_VALUES)
// ---------------------------------------------------------------------------

static double rareTypeValue(const std::string& color) {
    if (color == "spO") return OT_VAL_ORANGE;
    if (color == "spL") return OT_VAL_LIGHT;
    if (color == "spD") return OT_VAL_DARK;
    if (color == "spR") return OT_VAL_RED;
    if (color == "spW") return OT_VAL_RAINBOW;
    return OT_RARE_EV_UNK;
}

static bool isRare(const std::string& color) {
    return color == "spO" || color == "spL" || color == "spD" ||
           color == "spR" || color == "spW";
}

// ---------------------------------------------------------------------------
// Bitmask helpers — 5×5 grid, bit index = row*5+col
// ---------------------------------------------------------------------------

static inline int otBit(int r, int c) { return 1 << (r * 5 + c); }

// Count trailing zeros (index of lowest set bit)
static inline int otCtz(uint32_t v) {
    // __builtin_ctz is well-defined for non-zero v
    return __builtin_ctz(v);
}

// ---------------------------------------------------------------------------
// Run mask generation — mirrors otRunMasks(len) in JS
// ---------------------------------------------------------------------------

static std::vector<int> otRunMasks(int len) {
    std::vector<int> masks;
    for (int axis = 0; axis < 2; ++axis) {       // 0=horizontal, 1=vertical
        for (int line = 0; line < 5; ++line) {
            for (int start = 0; start <= 5 - len; ++start) {
                int mask = 0;
                for (int i = 0; i < len; ++i) {
                    int r = (axis == 0) ? line      : start + i;
                    int c = (axis == 0) ? start + i : line;
                    mask |= otBit(r, c);
                }
                masks.push_back(mask);
            }
        }
    }
    return masks;
}

// ---------------------------------------------------------------------------
// Rare combo — mirrors computeRareCombos in JS
// ---------------------------------------------------------------------------

struct RareCombo {
    int combined;
    std::vector<int> masks;
};

static void computeRareCombosRecurse(
    const std::vector<int>& rareMasks,
    int N, int start, int combined,
    std::vector<int>& current,
    std::vector<RareCombo>& combos)
{
    if ((int)current.size() == N) {
        combos.push_back({ combined, current });
        return;
    }
    for (int i = start; i < (int)rareMasks.size(); ++i) {
        int m = rareMasks[i];
        if (!(combined & m)) {
            current.push_back(m);
            computeRareCombosRecurse(rareMasks, N, i + 1, combined | m, current, combos);
            current.pop_back();
        }
    }
}

static std::vector<RareCombo> computeRareCombos(const std::vector<int>& rareMasks, int N) {
    std::vector<RareCombo> combos;
    std::vector<int> current;
    computeRareCombosRecurse(rareMasks, N, 0, 0, current, combos);
    return combos;
}

// ---------------------------------------------------------------------------
// isOtRareComboCompatible — mirrors JS isOtRareComboCompatible
// rareKnown: array of (colorCode, bitmask) pairs for identified rare cells
// ---------------------------------------------------------------------------

static bool isOtRareComboCompatible(
    const RareCombo& combo,
    const std::vector<std::pair<std::string, int>>& rareKnown)
{
    // For each known rare color: find a mask in combo that contains all its bits
    std::vector<int> usedMasks;
    for (const auto& kv : rareKnown) {
        int bits = kv.second;
        if (!bits) continue;
        int matched = -1;
        for (int m : combo.masks) {
            if ((m & bits) == bits) { matched = m; break; }
        }
        if (matched == -1) return false;
        // Check uniqueness: this mask must not already be used by another color
        for (int used : usedMasks) {
            if (used == matched) return false;
        }
        usedMasks.push_back(matched);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Per-cell probability struct
// ---------------------------------------------------------------------------

struct CellProb {
    double teal, green, yellow, rare, blue;
};

// ---------------------------------------------------------------------------
// otComputeProbs — mirrors JS otComputeProbs
// knownByIdx: bit i set means cell i is known; color stored in knownColor[i]
//   0=teal 1=green 2=yellow 3=rare 4=blue
// rareKnown: pairs (color_string, bitmask_of_confirmed_cells)
// ---------------------------------------------------------------------------

static bool otComputeProbs(
    const int knownColorClass[25],   // -1=unknown, 0=teal, 1=green, 2=yellow, 3=rare, 4=blue
    bool knownMask[25],
    int nRare,
    const std::vector<std::pair<std::string, int>>& rareKnown,
    const std::vector<int>& tealMasks,
    const std::vector<int>& greenMasks,
    const std::vector<int>& yellowMasks,
    const std::vector<RareCombo>& rareCombos,
    std::array<CellProb, 25>& probs)
{
    int knownTeal = 0, knownGreen = 0, knownYellow = 0, knownBlue = 0, knownRareAny = 0;
    for (int i = 0; i < 25; ++i) {
        if (!knownMask[i]) continue;
        int bit = 1 << i;
        switch (knownColorClass[i]) {
            case 0: knownTeal    |= bit; break;
            case 1: knownGreen   |= bit; break;
            case 2: knownYellow  |= bit; break;
            case 3: knownRareAny |= bit; break;
            case 4: knownBlue    |= bit; break;
        }
    }

    // Filter compatible masks (mirrors JS compatT/G/Y/rareCombos)
    std::vector<int> compatT, compatG, compatY;
    for (int m : tealMasks) {
        if ((m & knownTeal) == knownTeal &&
            !(m & (knownBlue | knownGreen | knownYellow | knownRareAny)))
            compatT.push_back(m);
    }
    for (int m : greenMasks) {
        if ((m & knownGreen) == knownGreen &&
            !(m & (knownBlue | knownTeal | knownYellow | knownRareAny)))
            compatG.push_back(m);
    }
    for (int m : yellowMasks) {
        if ((m & knownYellow) == knownYellow &&
            !(m & (knownBlue | knownTeal | knownGreen | knownRareAny)))
            compatY.push_back(m);
    }

    std::vector<int> compatRC;
    for (int j = 0; j < (int)rareCombos.size(); ++j) {
        const RareCombo& combo = rareCombos[j];
        if ((combo.combined & knownRareAny) == knownRareAny &&
            !(combo.combined & knownBlue) &&
            !(combo.combined & knownTeal) &&
            !(combo.combined & knownGreen) &&
            !(combo.combined & knownYellow) &&
            isOtRareComboCompatible(combo, rareKnown))
        {
            compatRC.push_back(j);
        }
    }

    if (compatT.empty() || compatG.empty() || compatY.empty() || compatRC.empty())
        return false;

    // Build all TGY triples
    struct TGY { int t, g, y, mask; };
    std::vector<TGY> tgyList;
    tgyList.reserve(compatT.size() * compatG.size() * compatY.size() / 4);
    for (int t : compatT) {
        for (int g : compatG) {
            if (t & g) continue;
            int tg = t | g;
            for (int y : compatY) {
                if (tg & y) continue;
                tgyList.push_back({ t, g, y, tg | y });
            }
        }
    }
    if (tgyList.empty()) return false;

    // Precompute combined bitmasks for compat rare combos
    std::vector<int> rcCombined;
    rcCombined.reserve(compatRC.size());
    for (int j : compatRC) rcCombined.push_back(rareCombos[j].combined);

    // Accumulate per-cell counts
    double cellTeal[25]   = {};
    double cellGreen[25]  = {};
    double cellYellow[25] = {};
    double cellRare[25]   = {};
    double totalConfigs   = 0.0;

    for (const TGY& tgy : tgyList) {
        for (int rcm : rcCombined) {
            if (tgy.mask & rcm) continue;
            totalConfigs += 1.0;
            for (uint32_t b = (uint32_t)tgy.t;  b; b &= b - 1) cellTeal  [otCtz(b)] += 1.0;
            for (uint32_t b = (uint32_t)tgy.g;  b; b &= b - 1) cellGreen [otCtz(b)] += 1.0;
            for (uint32_t b = (uint32_t)tgy.y;  b; b &= b - 1) cellYellow[otCtz(b)] += 1.0;
            for (uint32_t b = (uint32_t)rcm;    b; b &= b - 1) cellRare  [otCtz(b)] += 1.0;
        }
    }

    if (totalConfigs == 0.0) return false;

    for (int i = 0; i < 25; ++i) {
        double t = cellTeal[i]   / totalConfigs;
        double g = cellGreen[i]  / totalConfigs;
        double y = cellYellow[i] / totalConfigs;
        double r = cellRare[i]   / totalConfigs;
        probs[i] = { t, g, y, r, 1.0 - t - g - y - r };
    }
    return true;
}

// ---------------------------------------------------------------------------
// otComputeRareEv — mirrors JS otComputeRareEv
// rareKnown: pairs (color_string, bitmask); bitmask==0 means not yet identified
// ---------------------------------------------------------------------------

static double otComputeRareEv(
    const std::vector<std::pair<std::string, int>>& rareKnown,
    int nRare)
{
    // Rare type values in same order as JS OT_RARE_TYPES_ARR
    static const char* RARE_TYPES[] = { "spO", "spR", "spL", "spD", "spW" };
    static const int N_RARE_TYPES = 5;

    // Collect known types (bitmask != 0)
    std::vector<std::string> knownTypes;
    for (const auto& kv : rareKnown) {
        if (kv.second) knownTypes.push_back(kv.first);
    }
    int unknownSlots = nRare - (int)knownTypes.size();

    if (unknownSlots <= 0) {
        if (knownTypes.empty()) return OT_RARE_EV_UNK;
        double s = 0;
        for (const auto& t : knownTypes) s += rareTypeValue(t);
        return s / (double)knownTypes.size();
    }
    if (knownTypes.empty()) return OT_RARE_EV_UNK;

    // Unknown types = all rare types not in knownTypes
    double unknownSum = 0;
    int unknownCount  = 0;
    for (int i = 0; i < N_RARE_TYPES; ++i) {
        bool found = false;
        for (const auto& kt : knownTypes) {
            if (kt == RARE_TYPES[i]) { found = true; break; }
        }
        if (!found) { unknownSum += rareTypeValue(RARE_TYPES[i]); ++unknownCount; }
    }
    double unknownAvg = (unknownCount > 0) ? unknownSum / unknownCount : 0.0;

    double knownSum = 0;
    for (const auto& t : knownTypes) knownSum += rareTypeValue(t);
    double knownAvg = knownSum / (double)knownTypes.size();

    return (knownAvg * knownTypes.size() + unknownAvg * unknownSlots) / nRare;
}

// ---------------------------------------------------------------------------
// otRolloutValue — mirrors JS otRolloutValue
// sortedCells: indices sorted by P(blue) ascending
// pb0: paid blues used so far; C0: colored cells found so far
// ---------------------------------------------------------------------------

static double otRolloutValue(
    const std::vector<int>& sortedCells,
    const std::array<CellProb, 25>& probs25,
    int pb0, int C0, double rareEv)
{
    int budget = 4 - pb0;
    if (budget <= 0) return 0.0;

    int graceNeed = (C0 < 5) ? (5 - C0) : 0;
    int G = graceNeed;

    // dpA[k * DP_G + c] = probability of state (k paid blues, c extra colored)
    double dpA[DP_K * DP_G] = {};
    double dpB[DP_K * DP_G] = {};
    dpA[0] = 1.0;

    double ev    = 0.0;
    bool   alive = false;

    for (int idx : sortedCells) {
        const CellProb& pr = probs25[idx];
        double pB = pr.blue;
        double pC = 1.0 - pB;

        double cVal = 0.0;
        if (pC > 1e-14) {
            cVal = (pr.teal * OT_VAL_TEAL + pr.green * OT_VAL_GREEN +
                    pr.yellow * OT_VAL_YELLOW + pr.rare * rareEv) / pC;
        }

        std::fill(dpB, dpB + DP_K * DP_G, 0.0);
        alive = false;

        for (int k = 0; k < budget; ++k) {
            for (int c = 0; c <= G; ++c) {
                double p = dpA[k * DP_G + c];
                if (p < 1e-14) continue;

                bool graceOn = (pb0 + k >= 3) && (c < G);

                // Colored outcome
                // Force left-to-right evaluation (p*pC first, then *cVal) to
                // match JS: `ev += p * pC * cVal` evaluates as (p*pC)*cVal.
                if (pC > 1e-14) {
                    volatile double ppC = p * pC;
                    ev += ppC * cVal;
                    int nc = (c < G) ? c + 1 : G;
                    dpB[k * DP_G + nc] += ppC;
                    alive = true;
                }

                // Blue outcome
                if (pB > 1e-14) {
                    volatile double ppB = p * pB;
                    ev += ppB * OT_VAL_BLUE;
                    if (graceOn) {
                        dpB[k * DP_G + c] += ppB;
                        alive = true;
                    } else {
                        int nk = k + 1;
                        if (nk < budget) {
                            dpB[nk * DP_G + c] += ppB;
                            alive = true;
                        }
                    }
                }
            }
        }

        if (!alive) break;
        std::copy(dpB, dpB + DP_K * DP_G, dpA);
    }

    return ev;
}

// ---------------------------------------------------------------------------
// otScoreMove — mirrors JS otScoreMove
// ---------------------------------------------------------------------------

static double otScoreMove(
    int cellIdx,
    const std::vector<int>& sortedUnrevealed,
    const std::array<CellProb, 25>& probs25,
    int pb0, int C0, double rareEv)
{
    const CellProb& pr = probs25[cellIdx];
    double pB = pr.blue;
    double pC = 1.0 - pB;
    bool blueIsFree = (pb0 >= 3) && (C0 < 5);

    // remaining = sortedUnrevealed without cellIdx (preserves sort order)
    std::vector<int> remaining;
    remaining.reserve(sortedUnrevealed.size());
    for (int i : sortedUnrevealed) {
        if (i != cellIdx) remaining.push_back(i);
    }

    double ev = 0.0;

    if (pC > 1e-10) {
        double cVal = (pr.teal * OT_VAL_TEAL + pr.green * OT_VAL_GREEN +
                       pr.yellow * OT_VAL_YELLOW + pr.rare * rareEv) / pC;
        double fut = otRolloutValue(remaining, probs25, pb0, C0 + 1, rareEv);
        ev += pC * (cVal + fut);
    }

    if (pB > 1e-10) {
        if (blueIsFree) {
            double fut = otRolloutValue(remaining, probs25, pb0, C0, rareEv);
            ev += pB * (OT_VAL_BLUE + fut);
        } else {
            int nb = pb0 + 1;
            if (nb >= 4) {
                ev += pB * OT_VAL_BLUE;
            } else {
                double fut = otRolloutValue(remaining, probs25, nb, C0, rareEv);
                ev += pB * (OT_VAL_BLUE + fut);
            }
        }
    }

    return ev;
}

// ---------------------------------------------------------------------------
// Parse integer from JSON value — minimal helper
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

class KelimoExpectimaxOTStrategy : public OTStrategy {
public:
    void init_evaluation_run() override {
        // Precompute run masks for each ship size
        tealMasks_   = otRunMasks(4);
        greenMasks_  = otRunMasks(3);
        yellowMasks_ = otRunMasks(3);
        rareMasks_   = otRunMasks(2);

        // Precompute rare combos for nRare in {2,3,4,5}
        for (int N = 2; N <= 5; ++N) {
            rareCombos_[N] = computeRareCombos(rareMasks_, N);
        }
    }

    // No per-game state; init_game_payload is a no-op.

    void next_click(const std::vector<Cell>& board,
                    const std::string& meta_json,
                    ClickResult& out) override
    {
        int nColors = jsonGetInt(meta_json.c_str(), "\"n_colors\"", 6);
        int blueClicks   = jsonGetInt(meta_json.c_str(), "\"blues_used\"", 0);
        int coloredFound = jsonGetInt(meta_json.c_str(), "\"ships_hit\"",  0);
        int nRare = nColors - 4;

        // Build knownColorClass and knownMask from clicked cells
        int  knownColorClass[25];  // 0=teal,1=green,2=yellow,3=rare,4=blue,-1=unknown
        bool knownMask[25];
        std::fill(knownColorClass, knownColorClass + 25, -1);
        std::fill(knownMask, knownMask + 25, false);

        bool clickedSet[25] = {};

        // rareKnown: color code → bitmask of confirmed cells
        std::vector<std::pair<std::string, int>> rareKnown;

        for (const Cell& cell : board) {
            if (!cell.clicked) continue;
            int idx = cell.row * 5 + cell.col;
            clickedSet[idx] = true;
            knownMask[idx]  = true;
            const std::string& sp = cell.color;
            if      (sp == "spT") knownColorClass[idx] = 0;
            else if (sp == "spG") knownColorClass[idx] = 1;
            else if (sp == "spY") knownColorClass[idx] = 2;
            else if (sp == "spB") knownColorClass[idx] = 4;
            else if (isRare(sp))  {
                knownColorClass[idx] = 3;
                // Update rareKnown entry
                bool found = false;
                for (auto& kv : rareKnown) {
                    if (kv.first == sp) { kv.second |= (1 << idx); found = true; break; }
                }
                if (!found) rareKnown.push_back({ sp, 1 << idx });
            }
        }

        // Compute probabilities
        std::array<CellProb, 25> probs;
        bool ok = otComputeProbs(
            knownColorClass, knownMask,
            nRare, rareKnown,
            tealMasks_, greenMasks_, yellowMasks_,
            rareCombos_[nRare],
            probs);

        if (!ok) {
            // Fallback: click first unrevealed cell
            for (int i = 0; i < 25; ++i) {
                if (!clickedSet[i]) { out.row = i / 5; out.col = i % 5; return; }
            }
            out.row = 0; out.col = 0; return;
        }

        // Derive phase (mirrors JS getEffectiveClicksUsed / analyzeOt)
        bool graceActive = (coloredFound < 5);
        int effectiveClicksUsed;
        if (graceActive)
            effectiveClicksUsed = std::min(blueClicks, 3);
        else
            effectiveClicksUsed = std::min(blueClicks, 4);

        double rareEv = otComputeRareEv(rareKnown, nRare);

        // Collect unrevealed cells
        std::vector<int> unrevealed;
        for (int i = 0; i < 25; ++i)
            if (!clickedSet[i]) unrevealed.push_back(i);

        // Pre-sort by P(blue) ascending — passed into otScoreMove.
        // Use stable_sort to match JS Array.prototype.sort (stable, ties break by
        // original insertion order = ascending cell index).
        std::vector<int> sortedUnrevealed = unrevealed;
        std::stable_sort(sortedUnrevealed.begin(), sortedUnrevealed.end(),
                         [&probs](int a, int b) { return probs[a].blue < probs[b].blue; });

        // Score all unrevealed cells
        struct Move { int cellIdx; double ev; double blueProb; };
        std::vector<Move> moves;
        moves.reserve(unrevealed.size());
        for (int cellIdx : unrevealed) {
            double score = otScoreMove(
                cellIdx, sortedUnrevealed, probs,
                effectiveClicksUsed, coloredFound, rareEv);
            moves.push_back({ cellIdx, score, probs[cellIdx].blue });
        }

        // Sort by EV descending — stable to match JS sort stability.
        // Ties break by original order in `unrevealed` (ascending cell index).
        std::stable_sort(moves.begin(), moves.end(),
                         [](const Move& a, const Move& b) { return a.ev > b.ev; });

        // After grace ends: promote guaranteed non-blue cell with highest EV.
        // JS does certainMoves.sort((a,b) => b.ev - a.ev) on the already-sorted
        // moves array — so among certain cells with equal EV, their relative order
        // in `moves` (= ascending cell index after stable sort) is preserved.
        if (!graceActive) {
            int certainIdx = -1;
            double certainEv = -1e18;
            for (const Move& m : moves) {
                // Walk in moves order: first certain cell with highest EV wins.
                // Since moves is sorted by EV desc (stable), we just take the
                // first certain cell (highest EV, ties broken by cell index).
                if (m.blueProb <= 0.0001 && m.ev > certainEv) {
                    certainEv  = m.ev;
                    certainIdx = m.cellIdx;
                }
            }
            if (certainIdx >= 0) {
                // Move it to front
                int pos = -1;
                for (int i = 0; i < (int)moves.size(); ++i) {
                    if (moves[i].cellIdx == certainIdx) { pos = i; break; }
                }
                if (pos > 0) {
                    Move top = moves[pos];
                    moves.erase(moves.begin() + pos);
                    moves.insert(moves.begin(), top);
                }
            }
        }

        if (moves.empty()) { out.row = 0; out.col = 0; return; }

        int best = moves[0].cellIdx;
        out.row = best / 5;
        out.col = best % 5;
    }

private:
    std::vector<int>       tealMasks_;
    std::vector<int>       greenMasks_;
    std::vector<int>       yellowMasks_;
    std::vector<int>       rareMasks_;
    std::vector<RareCombo> rareCombos_[6];  // indexed by nRare (2..5)
};

// ---------------------------------------------------------------------------
// C exports required by the harness
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new KelimoExpectimaxOTStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* inst) {
    static_cast<KelimoExpectimaxOTStrategy*>(inst)->init_evaluation_run();
}

extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {
    static_cast<KelimoExpectimaxOTStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<KelimoExpectimaxOTStrategy*>(inst);

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
