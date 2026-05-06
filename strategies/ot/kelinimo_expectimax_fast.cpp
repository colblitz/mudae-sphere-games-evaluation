// sphere:stateless
// Source: https://solvespheres.kelinimo.workers.dev/ot.js
// Synced: 2026-05-06
// Source hash (SHA-256, first 16 hex): 4b08b64c88ecedcc
// To check: curl -s https://solvespheres.kelinimo.workers.dev/ot.js | sha256sum | cut -c1-16
// Disable FMA contraction so floating-point arithmetic matches JavaScript's
// strict IEEE 754 double precision (no fused multiply-add).  Without this,
// g++ -march=native folds expressions like `p * pC * cVal` in the rollout DP
// into single FMA instructions that round once instead of twice, producing
// ULP-level EV differences from V8's two-step rounding.  Those ULP diffs
// flip sort tie-breaks, causing different move choices and genuinely divergent
// game scores.
#pragma GCC optimize("fp-contract=off")
// Upgrade strategy .so from the default -O2 (set by CppBridge::load) to -O3
// to enable auto-vectorization and additional inlining.  The fp-contract=off
// pragma above takes precedence and keeps FMA disabled.
#pragma GCC optimize("O3")

/**
 * kelinimo_expectimax_fast.cpp — optimised build of kelinimo_expectimax.cpp.
 *
 * Identical algorithm and JS-parity guarantees; five performance improvements:
 *
 *   1. Turn-0 / turn-1 probability cache.
 *      otComputeProbs is skipped on the first two clicks of every game:
 *        - Turn 0 (empty board): result depends only on nRare (4 entries).
 *        - Turn 1 (one cell revealed): result depends on (nRare, cellIdx,
 *          colorClass) where colorClass ∈ {teal,green,yellow,rare,blue}.
 *          All 5 specific rare types (spO/spL/spD/spR/spW) are geometrically
 *          equivalent at turn 1 and share a single cache entry.
 *          Total: 4 × 25 × 5 = 500 entries (~500 KB).
 *      Both caches are populated in init_evaluation_run().
 *
 *   2. Flat rareCombosCombined_ array parallel to rareCombos_[N].
 *      Avoids struct field indirection (.combined) and separates the
 *      combined-mask data that the inner loop actually needs.
 *
 *   3. Reusable rcCompatFlag_ scratch array (uint8_t, one entry per rare combo).
 *      Replaces the per-call heap allocation of the compatRC index vector used
 *      in the CSR inner loop.  Allocated once per nRare in init_evaluation_run.
 *
 *   4. CSR (Compressed Sparse Row) table of per-TGY non-overlapping rare combos.
 *      For each of the 2,520 unique TGY combined bitmasks, the table stores
 *      only the rare combo *indices* that are geometrically non-overlapping with
 *      that TGY mask.  The inner loop then iterates O(108–554) entries (the
 *      CSR row) instead of O(686–157,000) (the full rareCombos list), and uses
 *      a single uint8_t array lookup to skip combos excluded by the board state.
 *      Memory: ~14 MB across all four nRare values (stored as combo *indices*,
 *      not combined masks, so rcCompatFlag_ lookups remain O(1)).
 *
 *   5. volatile removed from otRolloutValue.
 *      The `volatile double ppC / ppB` pattern was preventing register
 *      promotion and vectorisation of the DP inner loop.  It was originally
 *      added to inhibit FMA fusion, but the #pragma fp-contract=off at the top
 *      of the file already guarantees that globally — volatile is redundant.
 *
 * All changes preserve bit-for-bit identical move choices with the JS original
 * and with the unoptimised kelinimo_expectimax.cpp port.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
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
//
// Fast path: caller may supply a pre-filtered CSR row (csrRow / csrLen) giving
// the indices of rare combos that are already known to be geometrically
// non-overlapping with the current TGY mask.  The caller then only needs to
// gate each entry against rcCompatFlag (a uint8_t array indexed by combo
// index, 1 = compatible with current board state).
//
// If csrOffsets is nullptr the function falls back to the original O(|compatRC|)
// scan — used during cache construction where no CSR is available yet.
// ---------------------------------------------------------------------------

struct OtProbsContext {
    // Run mask lists (read-only, set once in init_evaluation_run)
    const std::vector<int>*       tealMasks;
    const std::vector<int>*       greenMasks;
    const std::vector<int>*       yellowMasks;
    const std::vector<RareCombo>* rareCombos;    // full list for a given nRare
    const std::vector<int>*       rcCombined;    // flat combined-masks parallel to rareCombos

    // CSR table (nullptr → fall back to linear scan)
    const std::unordered_map<int,int>* tgyMaskToIdx; // combined TGY mask → CSR row index
    const std::vector<int>*            csrOffsets;   // size = numUniqueTgyMasks + 1
    const std::vector<int>*            csrData;      // flat combo-index list

    // Per-call compat flag array (1 = combo passes board-state filter)
    // Caller must fill this before calling otComputeProbs.
    const uint8_t* rcCompatFlag;  // indexed by combo index in rareCombos
};

static bool otComputeProbs(
    const int  knownColorClass[25],
    bool       knownMask[25],
    int        nRare,
    const std::vector<std::pair<std::string, int>>& rareKnown,
    const OtProbsContext& ctx,
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

    const int knownOther = knownBlue | knownTeal | knownGreen | knownYellow | knownRareAny;

    // Filter compatible ship masks (mirrors JS compatT/G/Y)
    std::vector<int> compatT, compatG, compatY;
    for (int m : *ctx.tealMasks) {
        if ((m & knownTeal) == knownTeal &&
            !(m & (knownOther ^ knownTeal)))
            compatT.push_back(m);
    }
    for (int m : *ctx.greenMasks) {
        if ((m & knownGreen) == knownGreen &&
            !(m & (knownOther ^ knownGreen)))
            compatG.push_back(m);
    }
    for (int m : *ctx.yellowMasks) {
        if ((m & knownYellow) == knownYellow &&
            !(m & (knownOther ^ knownYellow)))
            compatY.push_back(m);
    }

    // Filter compatible rare combos; also populate rcCompatFlag for CSR path.
    // We need a local compat list for the fallback path; the CSR path only
    // needs rcCompatFlag (filled below).
    bool hasCSR = (ctx.tgyMaskToIdx != nullptr);

    // Temporary local compatRC list (used only in fallback path, but we need
    // it to check emptiness in both paths).
    std::vector<int> compatRCLocal;
    bool anyCompatRC = false;

    if (hasCSR) {
        // Fill rcCompatFlag and check if any combo passes.
        // We cast away const to fill — caller guarantees this buffer is writable.
        uint8_t* flag = const_cast<uint8_t*>(ctx.rcCompatFlag);
        const std::vector<RareCombo>& rc = *ctx.rareCombos;
        const int n = (int)rc.size();
        std::memset(flag, 0, n * sizeof(uint8_t));
        for (int j = 0; j < n; ++j) {
            const int cm = (*ctx.rcCombined)[j];
            if ((cm & knownRareAny) == knownRareAny &&
                !(cm & knownBlue) &&
                !(cm & knownTeal) &&
                !(cm & knownGreen) &&
                !(cm & knownYellow) &&
                isOtRareComboCompatible(rc[j], rareKnown))
            {
                flag[j] = 1;
                anyCompatRC = true;
            }
        }
    } else {
        // Fallback: build compatRCLocal index list (original behaviour)
        const std::vector<RareCombo>& rc = *ctx.rareCombos;
        for (int j = 0; j < (int)rc.size(); ++j) {
            const int cm = (*ctx.rcCombined)[j];
            if ((cm & knownRareAny) == knownRareAny &&
                !(cm & knownBlue) &&
                !(cm & knownTeal) &&
                !(cm & knownGreen) &&
                !(cm & knownYellow) &&
                isOtRareComboCompatible(rc[j], rareKnown))
            {
                compatRCLocal.push_back(j);
                anyCompatRC = true;
            }
        }
    }

    if (compatT.empty() || compatG.empty() || compatY.empty() || !anyCompatRC)
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

    // Accumulate per-cell counts
    double cellTeal[25]   = {};
    double cellGreen[25]  = {};
    double cellYellow[25] = {};
    double cellRare[25]   = {};
    double totalConfigs   = 0.0;

    if (hasCSR) {
        // Fast path: for each TGY triple, look up its pre-filtered CSR row of
        // non-overlapping rare combo indices, then gate each via rcCompatFlag.
        const uint8_t* flag = ctx.rcCompatFlag;
        const std::vector<int>& data    = *ctx.csrData;
        const std::vector<int>& offsets = *ctx.csrOffsets;
        const std::vector<int>& rcComb  = *ctx.rcCombined;

        for (const TGY& tgy : tgyList) {
            auto it = ctx.tgyMaskToIdx->find(tgy.mask);
            if (it == ctx.tgyMaskToIdx->end()) continue; // shouldn't happen
            const int rowIdx = it->second;
            const int lo = offsets[rowIdx];
            const int hi = offsets[rowIdx + 1];

            for (int k = lo; k < hi; ++k) {
                const int j   = data[k];         // rare combo index
                if (!flag[j]) continue;           // filtered by board state
                const int rcm = rcComb[j];
                totalConfigs += 1.0;
                for (uint32_t b = (uint32_t)tgy.t; b; b &= b - 1) cellTeal  [otCtz(b)] += 1.0;
                for (uint32_t b = (uint32_t)tgy.g; b; b &= b - 1) cellGreen [otCtz(b)] += 1.0;
                for (uint32_t b = (uint32_t)tgy.y; b; b &= b - 1) cellYellow[otCtz(b)] += 1.0;
                for (uint32_t b = (uint32_t)rcm;   b; b &= b - 1) cellRare  [otCtz(b)] += 1.0;
            }
        }
    } else {
        // Fallback path: original linear scan with overlap check
        const std::vector<int>& rcComb = *ctx.rcCombined;
        // Build a flat list of combined-masks for the compat combos
        std::vector<int> rcCombFiltered;
        rcCombFiltered.reserve(compatRCLocal.size());
        for (int j : compatRCLocal) rcCombFiltered.push_back(rcComb[j]);

        for (const TGY& tgy : tgyList) {
            for (int rcm : rcCombFiltered) {
                if (tgy.mask & rcm) continue;
                totalConfigs += 1.0;
                for (uint32_t b = (uint32_t)tgy.t; b; b &= b - 1) cellTeal  [otCtz(b)] += 1.0;
                for (uint32_t b = (uint32_t)tgy.g; b; b &= b - 1) cellGreen [otCtz(b)] += 1.0;
                for (uint32_t b = (uint32_t)tgy.y; b; b &= b - 1) cellYellow[otCtz(b)] += 1.0;
                for (uint32_t b = (uint32_t)rcm;   b; b &= b - 1) cellRare  [otCtz(b)] += 1.0;
            }
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

                // Colored outcome.
                // Force left-to-right evaluation (p*pC first, then *cVal) to
                // match JS: `ev += p * pC * cVal` evaluates as (p*pC)*cVal.
                // fp-contract=off at the top of the file disables FMA globally,
                // so no volatile is needed here — the pragma is sufficient.
                if (pC > 1e-14) {
                    double ppC = p * pC;
                    ev += ppC * cVal;
                    int nc = (c < G) ? c + 1 : G;
                    dpB[k * DP_G + nc] += ppC;
                    alive = true;
                }

                // Blue outcome
                if (pB > 1e-14) {
                    double ppB = p * pB;
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

class KelimoExpectimaxFastOTStrategy : public OTStrategy {
public:
    void init_evaluation_run() override {
        // ------------------------------------------------------------------
        // Step 1: precompute run masks (same as original)
        // ------------------------------------------------------------------
        tealMasks_   = otRunMasks(4);
        greenMasks_  = otRunMasks(3);
        yellowMasks_ = otRunMasks(3);
        rareMasks_   = otRunMasks(2);

        // ------------------------------------------------------------------
        // Step 2: precompute rare combos + flat combined arrays
        // ------------------------------------------------------------------
        for (int N = 2; N <= 5; ++N) {
            rareCombos_[N]         = computeRareCombos(rareMasks_, N);
            const int sz           = (int)rareCombos_[N].size();
            rareCombosCombined_[N].resize(sz);
            for (int j = 0; j < sz; ++j)
                rareCombosCombined_[N][j] = rareCombos_[N][j].combined;
            // Allocate compat flag array (zeroed; filled per-call)
            rcCompatFlag_[N].assign(sz, 0);
        }

        // ------------------------------------------------------------------
        // Step 3: build CSR table for each nRare
        //
        // For every unique combined TGY bitmask (2,520 total across all games),
        // store the indices of rare combos that are geometrically compatible
        // (non-overlapping) with that TGY mask.  At call time the inner loop
        // iterates only those O(108–554) entries and gates each via
        // rcCompatFlag_ instead of scanning all O(686–157,000) rare combos.
        // ------------------------------------------------------------------

        // Enumerate all unique TGY combined masks from the full (unconstrained)
        // teal × green × yellow triple set.
        std::vector<int> allTgyMasks;
        allTgyMasks.reserve(6000);
        for (int t : tealMasks_) {
            for (int g : greenMasks_) {
                if (t & g) continue;
                int tg = t | g;
                for (int y : yellowMasks_) {
                    if (tg & y) continue;
                    allTgyMasks.push_back(tg | y);
                }
            }
        }
        // Deduplicate while preserving first-occurrence order (for stable
        // CSR row indices).
        {
            std::vector<int> seen;
            seen.reserve(allTgyMasks.size());
            std::unordered_map<int,int> dedupMap;
            dedupMap.reserve(3000);
            for (int m : allTgyMasks) {
                if (dedupMap.find(m) == dedupMap.end()) {
                    dedupMap[m] = (int)seen.size();
                    seen.push_back(m);
                }
            }
            allTgyMasks = std::move(seen);
            // Build tgyMaskToIdx_ from dedupMap
            tgyMaskToIdx_.reserve(dedupMap.size());
            for (auto& kv : dedupMap)
                tgyMaskToIdx_[kv.first] = kv.second;
        }

        const int numTgy = (int)allTgyMasks.size();

        for (int N = 2; N <= 5; ++N) {
            const std::vector<int>& rcComb = rareCombosCombined_[N];
            const int rcSz = (int)rcComb.size();

            // Build CSR: for each TGY mask, collect non-overlapping RC indices
            csrOffsets_[N].resize(numTgy + 1);
            csrData_[N].clear();
            csrData_[N].reserve(numTgy * 500); // rough upper bound

            for (int ti = 0; ti < numTgy; ++ti) {
                csrOffsets_[N][ti] = (int)csrData_[N].size();
                const int tgyMask = allTgyMasks[ti];
                for (int j = 0; j < rcSz; ++j) {
                    if (!(tgyMask & rcComb[j]))
                        csrData_[N].push_back(j);
                }
            }
            csrOffsets_[N][numTgy] = (int)csrData_[N].size();
        }

        // ------------------------------------------------------------------
        // Step 4: precompute turn-0 and turn-1 probability caches
        //
        // Turn 0 (empty board): result is fully determined by nRare alone.
        //   4 entries (one per nRare ∈ {2,3,4,5}).
        //
        // Turn 1 (one cell revealed): result is determined by
        //   (nRare, cellIdx, colorClass) where colorClass ∈ {0..4}
        //   (0=teal, 1=green, 2=yellow, 3=rare, 4=blue).
        //   All 5 specific rare sp-codes are geometrically equivalent at
        //   turn 1 (proven: isOtRareComboCompatible reduces to a pure
        //   containment check when only one rare cell is known), so we use
        //   a single cache entry for colorClass=3 regardless of rare type.
        //   500 entries total (4 × 25 × 5).
        //
        // Cache construction uses the fallback (no-CSR) path because the CSR
        // table is available but building with it here is fine too.
        // ------------------------------------------------------------------

        // Build a context struct for cache construction (no CSR needed since
        // we're only running this at startup for fixed known states — but we
        // *can* use CSR to make cache construction faster too).
        // We'll use the CSR path since it's already built.

        for (int N = 2; N <= 5; ++N) {
            OtProbsContext ctx;
            ctx.tealMasks      = &tealMasks_;
            ctx.greenMasks     = &greenMasks_;
            ctx.yellowMasks    = &yellowMasks_;
            ctx.rareCombos     = &rareCombos_[N];
            ctx.rcCombined     = &rareCombosCombined_[N];
            ctx.tgyMaskToIdx   = &tgyMaskToIdx_;
            ctx.csrOffsets     = &csrOffsets_[N];
            ctx.csrData        = &csrData_[N];
            ctx.rcCompatFlag   = rcCompatFlag_[N].data();

            // Turn-0 cache: all-unknown board
            {
                int  kcc[25]; std::fill(kcc, kcc + 25, -1);
                bool km[25];  std::fill(km,  km  + 25, false);
                std::vector<std::pair<std::string,int>> rk;
                bool ok = otComputeProbs(kcc, km, N, rk, ctx, turn0Probs_[N]);
                (void)ok; // must succeed on empty board
            }

            // Turn-1 cache: one cell revealed, each possible colorClass
            for (int cell = 0; cell < 25; ++cell) {
                for (int cc = 0; cc < 5; ++cc) {
                    int  kcc[25]; std::fill(kcc, kcc + 25, -1);
                    bool km[25];  std::fill(km,  km  + 25, false);
                    kcc[cell] = cc;
                    km[cell]  = true;
                    std::vector<std::pair<std::string,int>> rk;
                    if (cc == 3) {
                        // Use "spO" as canonical rare type — all 5 types are
                        // geometrically equivalent at turn 1.
                        rk.push_back({ "spO", 1 << cell });
                    }
                    bool ok = otComputeProbs(kcc, km, N, rk, ctx,
                                             turn1Probs_[N][cell][cc]);
                    if (!ok) {
                        // Should not happen for valid (cell, colorClass) pairs,
                        // but mark as uncomputed by zeroing the array so the
                        // caller falls through to the full computation.
                        turn1Probs_[N][cell][cc].fill({ 0,0,0,0,0 });
                        turn1Valid_[N][cell][cc] = false;
                    } else {
                        turn1Valid_[N][cell][cc] = true;
                    }
                }
            }
        }
    }

    // No per-game state; init_game_payload is a no-op.

    void next_click(const std::vector<Cell>& board,
                    const std::string& meta_json,
                    ClickResult& out) override
    {
        int nColors      = jsonGetInt(meta_json.c_str(), "\"n_colors\"", 6);
        int blueClicks   = jsonGetInt(meta_json.c_str(), "\"blues_used\"", 0);
        int coloredFound = jsonGetInt(meta_json.c_str(), "\"ships_hit\"",  0);
        int nRare        = nColors - 4;

        // Build knownColorClass and knownMask from clicked cells
        int  knownColorClass[25];
        bool knownMask[25];
        std::fill(knownColorClass, knownColorClass + 25, -1);
        std::fill(knownMask,       knownMask       + 25, false);

        bool clickedSet[25] = {};
        int  clickCount     = 0;

        // rareKnown: color code → bitmask of confirmed cells
        std::vector<std::pair<std::string, int>> rareKnown;

        // For turn-1 cache: track the single revealed cell
        int  t1Cell = -1, t1CC = -1;

        for (const Cell& cell : board) {
            if (!cell.clicked) continue;
            int idx = cell.row * 5 + cell.col;
            clickedSet[idx] = true;
            knownMask[idx]  = true;
            ++clickCount;
            const std::string& sp = cell.color;
            if      (sp == "spT") { knownColorClass[idx] = 0; t1Cell = idx; t1CC = 0; }
            else if (sp == "spG") { knownColorClass[idx] = 1; t1Cell = idx; t1CC = 1; }
            else if (sp == "spY") { knownColorClass[idx] = 2; t1Cell = idx; t1CC = 2; }
            else if (sp == "spB") { knownColorClass[idx] = 4; t1Cell = idx; t1CC = 4; }
            else if (isRare(sp))  {
                knownColorClass[idx] = 3;
                t1Cell = idx; t1CC = 3;
                bool found = false;
                for (auto& kv : rareKnown) {
                    if (kv.first == sp) { kv.second |= (1 << idx); found = true; break; }
                }
                if (!found) rareKnown.push_back({ sp, 1 << idx });
            }
        }

        // ------------------------------------------------------------------
        // Probability computation — use cache for turns 0 and 1
        // ------------------------------------------------------------------
        std::array<CellProb, 25> probs;
        bool ok = false;

        if (clickCount == 0) {
            // Turn-0 cache hit
            probs = turn0Probs_[nRare];
            ok    = true;
        } else if (clickCount == 1 && t1Cell >= 0 && t1CC >= 0 &&
                   turn1Valid_[nRare][t1Cell][t1CC]) {
            // Turn-1 cache hit
            probs = turn1Probs_[nRare][t1Cell][t1CC];
            ok    = true;
        } else {
            // General path: run the full constraint engine
            OtProbsContext ctx;
            ctx.tealMasks    = &tealMasks_;
            ctx.greenMasks   = &greenMasks_;
            ctx.yellowMasks  = &yellowMasks_;
            ctx.rareCombos   = &rareCombos_[nRare];
            ctx.rcCombined   = &rareCombosCombined_[nRare];
            ctx.tgyMaskToIdx = &tgyMaskToIdx_;
            ctx.csrOffsets   = &csrOffsets_[nRare];
            ctx.csrData      = &csrData_[nRare];
            ctx.rcCompatFlag = rcCompatFlag_[nRare].data();

            ok = otComputeProbs(knownColorClass, knownMask,
                                nRare, rareKnown, ctx, probs);
        }

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
        // Use stable_sort to match JS Array.prototype.sort (stable, ties break
        // by original insertion order = ascending cell index).
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
        // moves array — so among certain cells with equal EV, their relative
        // order in `moves` (= ascending cell index after stable sort) is kept.
        if (!graceActive) {
            int certainIdx  = -1;
            double certainEv = -1e18;
            for (const Move& m : moves) {
                if (m.blueProb <= 0.0001 && m.ev > certainEv) {
                    certainEv  = m.ev;
                    certainIdx = m.cellIdx;
                }
            }
            if (certainIdx >= 0) {
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
        out.row  = best / 5;
        out.col  = best % 5;
    }

private:
    // Run mask lists
    std::vector<int>       tealMasks_;
    std::vector<int>       greenMasks_;
    std::vector<int>       yellowMasks_;
    std::vector<int>       rareMasks_;

    // Rare combos: full struct list + flat combined-mask array (parallel)
    std::vector<RareCombo> rareCombos_[6];          // indexed by nRare (2..5)
    std::vector<int>       rareCombosCombined_[6];  // rareCombos_[N][j].combined

    // Per-call compat flag scratch: 1 = combo passes board-state filter
    // uint8_t for fast random access (std::vector<bool> is bit-packed)
    std::vector<uint8_t>   rcCompatFlag_[6];        // indexed by combo index

    // CSR table: per unique TGY combined-mask → list of non-overlapping RC indices
    std::unordered_map<int,int> tgyMaskToIdx_;      // TGY combined mask → CSR row index
    std::vector<int>            csrOffsets_[6];     // [nRare]: size = numTgyMasks+1
    std::vector<int>            csrData_[6];        // [nRare]: flat combo-index entries

    // Turn-0 cache: probs for empty board, indexed by nRare
    std::array<CellProb,25>    turn0Probs_[6];

    // Turn-1 cache: probs for one revealed cell, indexed [nRare][cellIdx][colorClass]
    // colorClass: 0=teal 1=green 2=yellow 3=rare(any) 4=blue
    std::array<CellProb,25>    turn1Probs_[6][25][5];
    bool                       turn1Valid_[6][25][5];
};

// ---------------------------------------------------------------------------
// C exports required by the harness
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new KelimoExpectimaxFastOTStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* inst) {
    static_cast<KelimoExpectimaxFastOTStrategy*>(inst)->init_evaluation_run();
}

extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {
    static_cast<KelimoExpectimaxFastOTStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<KelimoExpectimaxFastOTStrategy*>(inst);

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
