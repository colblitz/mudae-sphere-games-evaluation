#pragma once
// Source: https://tksglass.github.io/OT/
// GitHub: https://github.com/tksglass/OT
// Synced: 2026-06-24 (commit 68a48151803183bd)
// Commit: "Update index.html"
// To check: curl -s "https://api.github.com/repos/tksglass/OT/commits?per_page=1" | \
//   python3 -c "import json,sys; d=json.load(sys.stdin); print(d[0]['sha'][:16], d[0]['commit']['committer']['date'])"
/**
 * tksglass_20260624_engine.h
 *
 * Shared engine for the tksglass OT Visualizer strategy family,
 * synced to commit 68a48151803183bd (2026-06-24).
 *
 * Source: https://tksglass.github.io/OT/
 * Created by @tksglass
 *
 * This header is designed to be included by exactly one .cpp per .so — each
 * strategy compiles to its own shared library, so there are no ODR violations.
 *
 * Usage in a strategy .cpp:
 *
 *   // sphere:stateless
 *   #include "tksglass_20260624_engine.h"
 *
 *   class MyOTStrategy : public OTStrategy {
 *   public:
 *       void next_click(const std::vector<Cell>& board,
 *                       const std::string& meta_json,
 *                       ClickResult& out) override
 *       {
 *           tksglass_next_click(board, meta_json, out, /*forcedAggressive=*\/false);
 *       }
 *   };
 *   TKSGLASS_EXPORTS(MyOTStrategy)
 *
 * The TKSGLASS_EXPORTS macro emits all five extern "C" symbols required by
 * the harness, including the board JSON parser in strategy_next_click.
 *
 * The forcedAggressive parameter controls the two variant-specific behaviours:
 *   false (safe/default): mirrors forcedStartMode = null.
 *     - 6/7-color games start in safe mode and switch to hunt at ships_hit >= 5.
 *     - Opening: corners {0,4,20,24} for 6/7c; {8,12,16} for 8/9c.
 *   true (aggressive): mirrors forcedStartMode = "aggressive".
 *     - Hunt mode active from move 1 for all color counts.
 *     - Opening: center-adjacent {7,11,13,17} for 6/7c; {8,12,16} for 8/9c.
 *
 * === Algorithm overview ===
 *
 * Opening move (board empty, mirrors highlightStartingCorners):
 *   - n_colors 8/9 (rareCount >= 4): click cells in order from {8, 12, 16}.
 *   - n_colors 6/7, forcedAggressive=true: cells {7, 11, 13, 17}.
 *   - n_colors 6/7, forcedAggressive=false (default/safe): cells {0, 4, 20, 24}.
 *   All tiles in each set are equally optimal (recommended-equal in the UI).
 *   We pick the first unclicked cell in the set in list order.
 *
 * Mid-game (board non-empty, mirrors runExactSolver + highlightRecommendations):
 *   1. Enumerate all valid board placements via DFS (enumerateAllBoardsMaskNative /
 *      enumerateBoardsDFS in JS), storing up to FULL_BOARD_STORE_LIMIT boards.
 *   2. Compute p[i] = fraction of valid boards with any ship at cell i.
 *   3. Check for guaranteed moves (findGuaranteedMove):
 *      - Safe mode: if blues_used >= 3 AND ships_hit < 5, click any p==0 cell.
 *      - Hunt mode: if missEndsGame (ships_hit >= 5 AND blues_used >= 3), click
 *        any p==1 cell.
 *   4. Otherwise, compute per-candidate statistics from the board set:
 *        expectedRemaining  = sum_k(count_k^2) / N  (expected board set size)
 *        worstRemaining     = max_k(count_k)
 *        weightedEV         = sum_boards(EV(color@cell)) / N
 *        size4/3/2          = fraction of boards where cell belongs to undiscovered
 *                             ship of each size tier
 *        forcedEV/Tiles     = forced EV/tiles in the largest outcome bucket
 *   5. Score each candidate with scoreStrategicCandidate (default preset weights):
 *      Hunt mode (ships_hit >= 5 OR n_colors >= 8, OR forcedAggressive):
 *        score = hitChance*1.25 + infoGain*0.95 + shipDiscovery*0.85
 *              + normalizedEV*0.55 + worstSafety*0.45
 *              - blueChance*0.45 - blueChance*megPressure*0.70
 *              - 0.05 if partial-ship tile and phase != "late"
 *      Safe mode:
 *        score = infoGain*1.50 + shipDiscovery*0.85 + freeBlue*0.55
 *              + worstSafety*0.65 - hitChance*1.15
 *              - blueChance*megPressure*1.75
 *              - 0.08 if partial-ship tile and phase != "late"
 *      Where:
 *        infoGain      = 1 - expectedRemaining/N  (fallback: 2*p*(1-p))
 *        worstSafety   = clamp01(1 - worstRemaining/N)
 *        megPressure   = clamp01((hitPressure + missPressure) / 2)
 *        freeBlue      = (1-p) if ships_hit < 5, else 0  (safe mode only)
 *        shipDiscovery = clamp01(size4*1.0 + size3*0.75 + size2*0.45)
 *        normalizedEV  = clamp01(weightedEV / 500)
 *   6. Sort candidates by score descending (EPSILON tie-tolerance); tiebreak:
 *      infoGain descending, then lower cell index.
 *   7. Identify safest candidate (getSafestMove): highest blue-chance (safe) or
 *      hit-chance (hunt) within SAFE_RISK_BAND=0.05, tiebroken by
 *      undiscoveredSize4 > size3 > size2 > safeBoardReduction > safeWeightedEV >
 *      safePrimaryChance > bestSafeWorstOutcomeForcedEV > forcedTiles (all desc).
 *   8. Primary click = topMoves[0] = candidates[0] unless candidates[0] == safest,
 *      in which case candidates[1].  Matches the JS recommended-best cell.
 *
 * Count-only fallback (> FULL_BOARD_STORE_LIMIT boards):
 *   Full outcome-bucket stats are unavailable.  Score falls back to:
 *     infoGain = 2*p*(1-p), worstSafety = infoGain, weightedEV = 0.
 *   shipDiscovery uses countOnly tier stats (accumulated during DFS).
 *
 * === Faithfulness notes ===
 *
 * DFS enumeration (mirrors enumerateAllBoardsMaskNative / enumerateBoardsDFS):
 *   - Ship list: teal(4), green(3), yellow(3), orange(2) first among rares,
 *     then other identified rares in first-reveal order, then anonymous rare_N.
 *   - Sort key: placements.size() - knownCells * 5; tiebreak: non-rare before rare.
 *     Mirrors prepareSortedShips() in JS.
 *   - canPlaceShip checks: knownMissMask, occupiedMask, diagonal-mask for same ship.
 *   - Anonymous rare deduplication: monotone placement-key ordering
 *     (lastRarePlacementKey), mirrors getLegalPlacementsForShip.
 *   - isFirstAnonymousRareFromIndex guard: skip non-first anonymous-rare DFS slots.
 *
 * Placement generation (mirrors buildRawPlacements + getValidPlacements):
 *   - Iterates r=0..4, c=0..4, horiz=[true,false].
 *   - Placement.key = cells[0] (top-left cell index).
 *   - diagonalMask precomputed per placement.
 *   - getValidPlacements filters by: direction constraint, respectsKnownMask,
 *     isPlacementValid (no conflicting revealed colors), passesDiagonalRulePlacement.
 *
 * EV values: raw TILE_EV constants (getSphereEVValue is identity at all defaults:
 *   prSphereBonus=0, spherePremium=false, sphereBonusMultiplierPercent=0).
 *
 * Advanced settings: all at defaults — getAdvancedStrategicScoreModifier = 0,
 *   safeAvoidAdjacentKnown = false, safeAggressiveAvoidHighProb = false.
 *
 * Hunt-mode trigger (safe variant): ships_hit >= 5 OR n_colors >= 8.
 *   Matches: naturalAggressive = hits >= 5; getDefaultModeForRareCount(n) =
 *   n <= 3 ? "safe" : "aggressive".  forcedStartMode = null (no override).
 *
 * Hunt-mode trigger (aggressive variant): always true.
 *   Matches: forcedStartMode = "aggressive" → mode = "aggressive" → huntMode = true.
 *
 * isRareSetAllowedForMode constraints (for getAnonymousRareEVByShipId):
 *   n_colors 6: set must not contain red or rainbow.
 *   n_colors 7: set must not contain both red and rainbow simultaneously.
 *   n_colors 8/9: no additional restrictions.
 *   Orange is always required in every valid rare set.
 *
 * Opening tiles are all recommended-equal (no priority in the UI); we pick
 * the first unclicked cell in list order as the canonical click.
 */

#pragma GCC optimize("O3,fp-contract=off")

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../interface/strategy.h"

using namespace sphere;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr int    GRID                  = 25;
static constexpr int    FULL_BOARD_STORE_LIMIT = 120000;
static constexpr double SAFE_RISK_BAND        = 0.05;
static constexpr double EPSILON               = 0.000001;

// Raw TILE_EV values (getSphereEVValue is identity at all defaults)
static constexpr double EV_MISS    = 10.0;
static constexpr double EV_TEAL    = 20.0;
static constexpr double EV_GREEN   = 35.0;
static constexpr double EV_YELLOW  = 55.0;
static constexpr double EV_ORANGE  = 90.0;
static constexpr double EV_WHITE   = 100.0;  // spW
static constexpr double EV_BLACK   = 120.0;  // spD
static constexpr double EV_RED     = 150.0;  // spR
static constexpr double EV_RAINBOW = 500.0;  // spL

// Strategic score preset weights (default preset)
struct StrategicWeights {
    double safeInfoGain, safeShipDiscovery, safeFreeBlue, safeWorstSafety;
    double safeHitRiskPenalty, safeMegBluePenalty, safePartialPenalty;
    double aggressiveHitChance, aggressiveInfoGain, aggressiveShipDiscovery;
    double aggressiveEV, aggressiveWorstSafety, aggressiveMissPenalty;
    double aggressiveMegBluePenalty, aggressivePartialPenalty;
};

static constexpr StrategicWeights WEIGHTS_DEFAULT = {
    1.50, 0.85, 0.55, 0.65, 1.15, 1.75, 0.08,  // safe
    1.25, 0.95, 0.85, 0.55, 0.45, 0.45, 0.70, 0.05  // aggressive
};

// ---------------------------------------------------------------------------
// Cell color enum
// ---------------------------------------------------------------------------

enum CellColor : uint8_t {
    CC_TEAL    = 0,
    CC_GREEN   = 1,
    CC_YELLOW  = 2,
    CC_ORANGE  = 3,
    CC_LIGHT   = 4,  // spL = rainbow rare (EV 500)
    CC_DARK    = 5,  // spD = black rare (EV 120)
    CC_RED     = 6,
    CC_WHITE   = 7,  // spW (EV 100)
    CC_BLUE    = 8,
    CC_UNKNOWN = 15,
};

// RARE_COLORS in JS order: orange, red, black, white, rainbow
static constexpr CellColor RARE_COLORS_ORDER[5] = {
    CC_ORANGE, CC_RED, CC_DARK, CC_WHITE, CC_LIGHT
};

static inline bool isRareColor(CellColor c) {
    return c == CC_ORANGE || c == CC_RED || c == CC_DARK ||
           c == CC_WHITE  || c == CC_LIGHT;
}

static inline CellColor parseColor(const char* sp) {
    if (!sp || sp[0] == '\0') return CC_UNKNOWN;
    if (sp[0] == 'm') return CC_BLUE;  // "miss"
    if (sp[0] != 's' || sp[1] != 'p') return CC_UNKNOWN;
    switch (sp[2]) {
        case 'T': return CC_TEAL;
        case 'G': return CC_GREEN;
        case 'Y': return CC_YELLOW;
        case 'O': return CC_ORANGE;
        case 'L': return CC_LIGHT;
        case 'D': return CC_DARK;
        case 'R': return CC_RED;
        case 'W': return CC_WHITE;
        case 'B': return CC_BLUE;
        default:  return CC_UNKNOWN;
    }
}

static inline double colorToEV(CellColor c) {
    switch (c) {
        case CC_TEAL:   return EV_TEAL;
        case CC_GREEN:  return EV_GREEN;
        case CC_YELLOW: return EV_YELLOW;
        case CC_ORANGE: return EV_ORANGE;
        case CC_WHITE:  return EV_WHITE;
        case CC_DARK:   return EV_BLACK;
        case CC_RED:    return EV_RED;
        case CC_LIGHT:  return EV_RAINBOW;
        case CC_BLUE:   return EV_MISS;
        default:        return 0.0;
    }
}

// ---------------------------------------------------------------------------
// Diagonal mask table (mirrors JS DIAGONAL_MASKS)
// ---------------------------------------------------------------------------

static uint32_t DIAGONAL_MASKS[GRID];

static void initDiagonalMasks() {
    for (int i = 0; i < GRID; ++i) {
        int r = i / 5, c = i % 5;
        uint32_t m = 0;
        const int drs[4] = {r-1, r-1, r+1, r+1};
        const int dcs[4] = {c-1, c+1, c-1, c+1};
        for (int k = 0; k < 4; ++k)
            if (drs[k] >= 0 && dcs[k] >= 0 && drs[k] < 5 && dcs[k] < 5)
                m |= 1u << (drs[k] * 5 + dcs[k]);
        DIAGONAL_MASKS[i] = m;
    }
}

// ---------------------------------------------------------------------------
// Placements (mirrors buildRawPlacements in JS)
// Each placement: mask, diagonalMask, key=cells[0], horiz
// ---------------------------------------------------------------------------

struct Placement {
    uint32_t mask;
    uint32_t diagonalMask;
    int      key;   // cells[0], canonical ordering key for anonymous rares
    bool     horiz;
};

static std::vector<Placement> buildRawPlacements(int size) {
    std::vector<Placement> pl;
    for (int r = 0; r < 5; ++r) {
        for (int c = 0; c < 5; ++c) {
            for (int h = 1; h >= 0; --h) {  // horiz=true first, then false
                bool horiz = (h == 1);
                int cells[4]; int n = 0; bool valid = true;
                for (int k = 0; k < size; ++k) {
                    int rr = r + (horiz ? 0 : k);
                    int cc = c + (horiz ? k : 0);
                    if (rr >= 5 || cc >= 5) { valid = false; break; }
                    cells[n++] = rr * 5 + cc;
                }
                if (!valid || n != size) continue;
                uint32_t mask = 0, diag = 0;
                for (int k = 0; k < n; ++k) {
                    mask |= 1u << cells[k];
                    diag |= DIAGONAL_MASKS[cells[k]];
                }
                pl.push_back({mask, diag, cells[0], horiz});
            }
        }
    }
    return pl;
}

static const std::vector<Placement>& rawPlacements(int size) {
    static bool inited = false;
    static std::vector<Placement> p2, p3, p4;
    if (!inited) {
        initDiagonalMasks();
        p2 = buildRawPlacements(2);
        p3 = buildRawPlacements(3);
        p4 = buildRawPlacements(4);
        inited = true;
    }
    if (size == 2) return p2;
    if (size == 3) return p3;
    return p4;
}

// ---------------------------------------------------------------------------
// Revealed state
// ---------------------------------------------------------------------------

struct RevealedState {
    uint32_t  clickedMask;
    uint32_t  missMask;
    uint32_t  colorMask[9];   // bitmask per CellColor (0..8)
    CellColor col[GRID];
    int       hits;           // ships_hit from meta
    int       misses;         // blues_used from meta
    int       foundColorsMask; // bit k set if CC k (0..7, non-blue) seen

    void build(const std::vector<Cell>& board, int shipsHit, int bluesUsed) {
        clickedMask = 0; missMask = 0;
        hits = shipsHit; misses = bluesUsed;
        foundColorsMask = 0;
        memset(colorMask, 0, sizeof(colorMask));
        for (int i = 0; i < GRID; ++i) col[i] = CC_UNKNOWN;
        for (const Cell& cell : board) {
            if (!cell.clicked) continue;
            int idx = cell.row * 5 + cell.col;
            clickedMask |= 1u << idx;
            CellColor cc = cell.color.empty() ? CC_UNKNOWN
                                              : parseColor(cell.color.c_str());
            col[idx] = cc;
            if (cc != CC_UNKNOWN) {
                colorMask[cc] |= 1u << idx;
                if (cc == CC_BLUE) missMask |= 1u << idx;
                else foundColorsMask |= 1 << (int)cc;
            }
        }
    }

    int foundColorsCount() const { return __builtin_popcount(foundColorsMask); }
};

// ---------------------------------------------------------------------------
// Ship slot
// ---------------------------------------------------------------------------

struct ShipSlot {
    int       size;
    CellColor color;
    uint32_t  knownMask;
    int       knownCount;
    int       dirConstraint;  // 0=any, 1=horiz, 2=vert
    bool      isAnon;
};

static int computeDirConstraint(uint32_t knownMask) {
    if (__builtin_popcount(knownMask) < 2) return 0;
    int firstRow = __builtin_ctz(knownMask) / 5;
    bool sameRow = true;
    for (uint32_t m = knownMask; m; m &= m-1)
        if (__builtin_ctz(m) / 5 != firstRow) { sameRow = false; break; }
    if (sameRow) return 1;
    int firstCol = __builtin_ctz(knownMask) % 5;
    bool sameCol = true;
    for (uint32_t m = knownMask; m; m &= m-1)
        if (__builtin_ctz(m) % 5 != firstCol) { sameCol = false; break; }
    if (sameCol) return 2;
    return 0;
}

// Valid placements for a slot (mirrors getValidPlacements + canPlaceShip)
static std::vector<const Placement*> computeValidPlacements(
    const ShipSlot& slot,
    const RevealedState& rev)
{
    const std::vector<Placement>& pl = rawPlacements(slot.size);
    std::vector<const Placement*> result;
    result.reserve(pl.size());

    for (const Placement& p : pl) {
        if (slot.dirConstraint == 1 && !p.horiz) continue;
        if (slot.dirConstraint == 2 &&  p.horiz) continue;
        if ((p.mask & slot.knownMask) != slot.knownMask) continue;
        if (p.mask & rev.missMask) continue;

        // isPlacementValid: no conflicting revealed colors
        bool valid = true;
        for (uint32_t tmp = p.mask; tmp; tmp &= tmp-1) {
            int i = __builtin_ctz(tmp);
            CellColor c = rev.col[i];
            if (c == CC_UNKNOWN || c == CC_BLUE) continue;
            if (slot.isAnon)              { valid = false; break; }
            if (c != slot.color)          { valid = false; break; }
        }
        if (!valid) continue;

        // passesDiagonalRulePlacement
        if (p.diagonalMask & slot.knownMask) continue;

        result.push_back(&p);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Ship list (mirrors getShipList + prepareSortedShips)
// ---------------------------------------------------------------------------

struct ShipInfo {
    ShipSlot slot;
    std::vector<const Placement*> placements;
    int sortScore;
};

static std::vector<ShipInfo> buildShipList(const RevealedState& rev, int nColors) {
    int rareCount = nColors - 4;

    // Identified rares: orange always first (getShipList pre-injects it),
    // then other found rares in RARE_COLORS_ORDER order (mirrors getUsedRare).
    std::vector<CellColor> identifiedRares;
    // Orange is always added first regardless of visibility
    identifiedRares.push_back(CC_ORANGE);
    for (int k = 0; k < 5; ++k) {
        CellColor rc = RARE_COLORS_ORDER[k];
        if (rc == CC_ORANGE) continue;
        if (rev.colorMask[rc] != 0) identifiedRares.push_back(rc);
    }

    int nAnon = std::max(0, rareCount - (int)identifiedRares.size());

    std::vector<ShipInfo> ships;
    ships.reserve(3 + (int)identifiedRares.size() + nAnon);

    auto addShip = [&](int size, CellColor color, bool anon) {
        ShipSlot slot;
        slot.size         = size;
        slot.color        = color;
        slot.isAnon       = anon;
        slot.knownMask    = anon ? 0u : rev.colorMask[color];
        slot.knownCount   = __builtin_popcount(slot.knownMask);
        slot.dirConstraint = computeDirConstraint(slot.knownMask);
        ShipInfo si;
        si.slot       = slot;
        si.placements = computeValidPlacements(slot, rev);
        si.sortScore  = (int)si.placements.size() - slot.knownCount * 5;
        ships.push_back(std::move(si));
    };

    addShip(4, CC_TEAL,   false);
    addShip(3, CC_GREEN,  false);
    addShip(3, CC_YELLOW, false);
    for (CellColor rc : identifiedRares) addShip(2, rc, false);
    for (int k = 0; k < nAnon; ++k)     addShip(2, CC_UNKNOWN, true);

    // Sort: ascending sortScore, tiebreak non-rare before rare.
    // Mirrors JS prepareSortedShips: tiebreak uses ship.startsWith('rare_'),
    // which only classifies anonymous rare_N slots as "rare" — named identified
    // rares (orange, red, etc.) are non-rare in the tiebreak and keep stable order.
    std::stable_sort(ships.begin(), ships.end(), [](const ShipInfo& a, const ShipInfo& b) {
        if (a.sortScore != b.sortScore) return a.sortScore < b.sortScore;
        // Only anonymous slots are "rare" for tiebreak purposes (mirrors startsWith('rare_'))
        if ( a.slot.isAnon && !b.slot.isAnon) return false;
        if (!a.slot.isAnon &&  b.slot.isAnon) return true;
        return false;
    });

    return ships;
}

// ---------------------------------------------------------------------------
// Board storage and DFS enumeration
// ---------------------------------------------------------------------------

using Board = std::array<uint8_t, GRID>;

struct EnumResult {
    std::vector<Board> boards;
    uint32_t           valid;
    double             probs[GRID];
    double             tierSize4[GRID];
    double             tierSize3[GRID];
    double             tierSize2[GRID];
    bool               fullBoardsAvailable;
};

struct DFSCtx {
    const std::vector<ShipInfo>* ships;
    int      nShips;
    uint32_t occupiedMask;
    uint8_t  board[GRID];
    uint32_t shipMask[16];   // per-slot placed-cells mask (for diagonal check)
    uint32_t counts[GRID];
    uint32_t tier4[GRID], tier3[GRID], tier2[GRID];
    uint32_t total;
    bool     storeBoards;
    std::vector<Board>* boardOut;
    bool     slotFound[16];
    int      slotSize[16];

    int tierForSlot(int slotIdx) const {
        if ((*ships)[slotIdx].slot.isAnon) return 2;
        CellColor c = (*ships)[slotIdx].slot.color;
        if (isRareColor(c)) return slotFound[slotIdx] ? 0 : 2;
        return slotFound[slotIdx] ? 0 : slotSize[slotIdx];
    }

    void run(int idx, int lastRareKey) {
        if (idx == nShips) {
            ++total;
            for (int i = 0; i < GRID; ++i) {
                if (!board[i]) continue;
                counts[i]++;
                int tier = tierForSlot(board[i] - 1);
                if      (tier == 4) tier4[i]++;
                else if (tier == 3) tier3[i]++;
                else if (tier == 2) tier2[i]++;
            }
            if (storeBoards) {
                Board b;
                memcpy(b.data(), board, GRID);
                boardOut->push_back(b);
            }
            return;
        }

        const ShipInfo& si = (*ships)[idx];
        const bool isAnon  = si.slot.isAnon;

        // isFirstAnonymousRareFromIndex: skip non-first anon slots
        if (isAnon) {
            bool isFirst = false;
            for (int j = idx; j < nShips; ++j) {
                if ((*ships)[j].slot.isAnon) { isFirst = (j == idx); break; }
            }
            if (!isFirst) { run(idx + 1, lastRareKey); return; }
        }

        const uint8_t slotId = (uint8_t)(idx + 1);

        for (const Placement* p : si.placements) {
            if (isAnon && p->key < lastRareKey) continue;
            if (p->mask & occupiedMask) continue;
            if (p->diagonalMask & shipMask[idx]) continue;

            occupiedMask   |= p->mask;
            shipMask[idx]  |= p->mask;
            for (uint32_t tmp = p->mask; tmp; tmp &= tmp-1)
                board[__builtin_ctz(tmp)] = slotId;

            run(idx + 1, isAnon ? p->key : lastRareKey);

            occupiedMask  &= ~p->mask;
            shipMask[idx] &= ~p->mask;
            for (uint32_t tmp = p->mask; tmp; tmp &= tmp-1)
                board[__builtin_ctz(tmp)] = 0;
        }
    }
};

static EnumResult enumerateBoards(
    const std::vector<ShipInfo>& ships,
    int nColors)
{
    EnumResult res;
    res.valid = 0;
    res.fullBoardsAvailable = false;
    memset(res.probs,     0, sizeof(res.probs));
    memset(res.tierSize4, 0, sizeof(res.tierSize4));
    memset(res.tierSize3, 0, sizeof(res.tierSize3));
    memset(res.tierSize2, 0, sizeof(res.tierSize2));

    for (const auto& si : ships)
        if (si.placements.empty()) return res;

    int nShips = (int)ships.size();
    DFSCtx ctx;
    ctx.ships = &ships; ctx.nShips = nShips;
    ctx.occupiedMask = 0; ctx.total = 0;
    ctx.storeBoards = false; ctx.boardOut = nullptr;
    memset(ctx.board,    0, sizeof(ctx.board));
    memset(ctx.shipMask, 0, sizeof(ctx.shipMask));
    memset(ctx.counts,   0, sizeof(ctx.counts));
    memset(ctx.tier4,    0, sizeof(ctx.tier4));
    memset(ctx.tier3,    0, sizeof(ctx.tier3));
    memset(ctx.tier2,    0, sizeof(ctx.tier2));
    for (int i = 0; i < nShips && i < 16; ++i) {
        ctx.slotFound[i] = (ships[i].slot.knownCount > 0);
        ctx.slotSize[i]  = ships[i].slot.size;
    }

    ctx.run(0, 0);
    res.valid = ctx.total;
    if (res.valid == 0) return res;

    double inv = 1.0 / (double)res.valid;
    for (int i = 0; i < GRID; ++i) {
        res.probs[i]     = ctx.counts[i] * inv;
        res.tierSize4[i] = ctx.tier4[i]  * inv;
        res.tierSize3[i] = ctx.tier3[i]  * inv;
        res.tierSize2[i] = ctx.tier2[i]  * inv;
    }

    if (res.valid > (uint32_t)FULL_BOARD_STORE_LIMIT) return res;

    // Full board storage pass
    res.boards.reserve(res.valid);
    memset(ctx.board,    0, sizeof(ctx.board));
    memset(ctx.shipMask, 0, sizeof(ctx.shipMask));
    memset(ctx.counts,   0, sizeof(ctx.counts));
    memset(ctx.tier4,    0, sizeof(ctx.tier4));
    memset(ctx.tier3,    0, sizeof(ctx.tier3));
    memset(ctx.tier2,    0, sizeof(ctx.tier2));
    ctx.occupiedMask = 0; ctx.total = 0;
    ctx.storeBoards = true; ctx.boardOut = &res.boards;
    ctx.run(0, 0);
    res.fullBoardsAvailable = true;
    return res;
}

// ---------------------------------------------------------------------------
// Candidate statistics
// ---------------------------------------------------------------------------

struct CandStat {
    int    i;
    double p;
    double expectedRemaining;
    double worstRemaining;
    double weightedEV;
    double size4, size3, size2;
    double forcedEV, forcedTiles;
    // Derived
    double infoGain, worstSafety, shipDiscovery, normalizedEV, megPressure;
    double score;
    // For getSafestMove
    double safeBoardReduction;
    double safePrimaryChance;
    double bestSafeWorstOutcomeForcedEV;
    double bestSafeWorstOutcomeForcedTiles;
};

// getAnonymousRareEVByShipId — average EV for anonymous rare slots
static double computeAnonRareEV(const RevealedState& rev, int nColors) {
    int rareCount = nColors - 4;

    // Required rares: orange always, plus any found rares
    std::vector<CellColor> required;
    required.push_back(CC_ORANGE);
    for (int k = 0; k < 5; ++k) {
        CellColor rc = RARE_COLORS_ORDER[k];
        if (rc == CC_ORANGE) continue;
        if (rev.colorMask[rc] != 0) required.push_back(rc);
    }
    if ((int)required.size() > rareCount) return 0.0;

    int remaining = rareCount - (int)required.size();

    std::vector<CellColor> pool;
    for (int k = 0; k < 5; ++k) {
        CellColor rc = RARE_COLORS_ORDER[k];
        bool inReq = false;
        for (CellColor r : required) if (r == rc) { inReq = true; break; }
        if (!inReq) pool.push_back(rc);
    }

    int poolSz = (int)pool.size();
    double total = 0.0; int count = 0;
    std::vector<int> combo;

    std::function<void(int,int)> enumerate = [&](int start, int left) {
        if (left == 0) {
            bool hasRed = false, hasRainbow = false;
            for (int idx : combo) {
                if (pool[idx] == CC_RED)   hasRed = true;
                if (pool[idx] == CC_LIGHT) hasRainbow = true;
            }
            for (CellColor r : required) {
                if (r == CC_RED)   hasRed = true;
                if (r == CC_LIGHT) hasRainbow = true;
            }
            if (nColors == 6 && (hasRed || hasRainbow)) return;
            if (nColors == 7 && hasRed && hasRainbow)   return;

            double evSum = 0.0; int hCount = 0;
            for (CellColor r : required) {
                if (r == CC_ORANGE) continue;
                if (rev.colorMask[r] != 0) continue;
                evSum += colorToEV(r); ++hCount;
            }
            for (int idx : combo) {
                CellColor rc = pool[idx];
                if (rev.colorMask[rc] != 0) continue;
                evSum += colorToEV(rc); ++hCount;
            }
            if (hCount == 0) return;
            total += evSum / hCount; ++count;
            return;
        }
        for (int i = start; i <= poolSz - left; ++i) {
            combo.push_back(i);
            enumerate(i + 1, left - 1);
            combo.pop_back();
        }
    };
    enumerate(0, remaining);

    if (count == 0) {
        return (EV_ORANGE + EV_RED + EV_BLACK + EV_WHITE + EV_RAINBOW) / 5.0;
    }
    return total / count;
}

static double computeWeightedEV(
    int cellIdx,
    const std::vector<Board>& boards,
    const std::vector<ShipInfo>& ships,
    double anonRareEV)
{
    if (boards.empty()) return 0.0;
    double tot = 0.0;
    for (const Board& b : boards) {
        uint8_t slot = b[cellIdx];
        if (slot == 0) tot += EV_MISS;
        else {
            const ShipSlot& ss = ships[slot - 1].slot;
            tot += ss.isAnon ? anonRareEV : colorToEV(ss.color);
        }
    }
    return tot / (double)boards.size();
}

static void computeTierStats(
    int cellIdx,
    const std::vector<Board>& boards,
    const std::vector<ShipInfo>& ships,
    double& s4, double& s3, double& s2)
{
    s4 = s3 = s2 = 0.0;
    if (boards.empty()) return;
    for (const Board& b : boards) {
        uint8_t slot = b[cellIdx];
        if (slot == 0) continue;
        const ShipInfo& si = ships[slot - 1];
        int tier;
        if      (si.slot.isAnon)               tier = 2;
        else if (isRareColor(si.slot.color))   tier = (si.slot.knownCount > 0) ? 0 : 2;
        else                                   tier = (si.slot.knownCount > 0) ? 0 : si.slot.size;
        if      (tier == 4) s4++;
        else if (tier == 3) s3++;
        else if (tier == 2) s2++;
    }
    double inv = 1.0 / (double)boards.size();
    s4 *= inv; s3 *= inv; s2 *= inv;
}

static void computeForcedScores(
    int cellIdx,
    const std::vector<Board>& boards,
    const std::vector<ShipInfo>& ships,
    double anonRareEV,
    const RevealedState& rev,
    double& forcedEV, double& forcedTiles)
{
    forcedEV = 0.0; forcedTiles = 0.0;
    if (boards.empty()) return;

    // Bucket boards by outcome (slot value) at cellIdx
    std::unordered_map<uint8_t, std::vector<int>> buckets;
    for (int bi = 0; bi < (int)boards.size(); ++bi)
        buckets[boards[bi][cellIdx]].push_back(bi);

    // Find largest bucket (worst case).
    // Tiebreak: higher slot-index key wins, mirroring JS object integer-key
    // ascending iteration order (last seen with strict > is the highest key).
    int worstSize = 0; uint8_t worstKey = 0;
    for (auto& kv : buckets) {
        int sz = (int)kv.second.size();
        if (sz > worstSize || (sz == worstSize && kv.first > worstKey)) {
            worstSize = sz; worstKey = kv.first;
        }
    }
    if (worstSize == 0) return;

    const std::vector<int>& wBoards = buckets[worstKey];

    // Forced tiles: cells where all boards in worst bucket have a ship,
    // excluding cells already revealed on the real board (mirrors JS
    // countForcedTilesInBoards / countForcedEVInBoards: `if (grid[i] !== null) continue`).
    uint32_t allShip = 0xFFFFFFFFu;
    for (int bi : wBoards) {
        uint32_t shipMask = 0;
        for (int i = 0; i < GRID; ++i)
            if (boards[bi][i] != 0) shipMask |= 1u << i;
        allShip &= shipMask;
    }
    // Remove already-clicked cells (revealed on the real board)
    allShip &= ~rev.clickedMask;

    double ft = 0.0, fev = 0.0;
    for (int i = 0; i < GRID; ++i) {
        if (!((allShip >> i) & 1)) continue;
        ++ft;
        uint8_t slot = boards[wBoards[0]][i];
        if (slot == 0) fev += EV_MISS;
        else {
            const ShipSlot& ss = ships[slot - 1].slot;
            fev += ss.isAnon ? anonRareEV : colorToEV(ss.color);
        }
    }
    forcedTiles = ft; forcedEV = fev;
}

static std::vector<CandStat> buildCandidateStats(
    const EnumResult& res,
    const std::vector<ShipInfo>& ships,
    const RevealedState& rev,
    int nColors)
{
    double N = (double)res.valid;
    double anonRareEV = computeAnonRareEV(rev, nColors);
    std::vector<CandStat> cands;
    cands.reserve(25);

    for (int i = 0; i < GRID; ++i) {
        if ((rev.clickedMask >> i) & 1) continue;
        double p = res.probs[i];
        CandStat cs;
        cs.i = i; cs.p = p;
        cs.expectedRemaining = 2.0 * p * (1.0 - p);
        cs.worstRemaining    = cs.expectedRemaining;
        cs.weightedEV        = 0.0;
        cs.size4 = res.tierSize4[i];
        cs.size3 = res.tierSize3[i];
        cs.size2 = res.tierSize2[i];
        cs.forcedEV = 0.0; cs.forcedTiles = 0.0;

        if (res.fullBoardsAvailable) {
            // Outcome buckets for this cell
            std::unordered_map<uint8_t, int> buckets;
            for (const Board& b : res.boards) buckets[b[i]]++;

            double expRem = 0.0, worst = 0.0;
            for (auto& kv : buckets) {
                double cnt = kv.second;
                expRem += cnt * cnt;
                if (cnt > worst) worst = cnt;
            }
            cs.expectedRemaining = expRem / N;
            cs.worstRemaining    = worst;
            cs.weightedEV        = computeWeightedEV(i, res.boards, ships, anonRareEV);
            computeTierStats(i, res.boards, ships, cs.size4, cs.size3, cs.size2);
            computeForcedScores(i, res.boards, ships, anonRareEV, rev,
                                cs.forcedEV, cs.forcedTiles);
        }
        cands.push_back(cs);
    }
    return cands;
}

// ---------------------------------------------------------------------------
// Helpers: phase, partial ship mask, clamp, meg pressure
// ---------------------------------------------------------------------------

static const char* getGamePhase(int shipsHit, int bluesUsed, int foundColorsCount) {
    if (shipsHit < 5 && foundColorsCount < 3) return "early";
    if (shipsHit >= 5 || bluesUsed >= 2 || foundColorsCount >= 5) return "late";
    return "mid";
}

static uint32_t getPartialShipMask(
    const RevealedState& rev,
    const std::vector<ShipInfo>& ships)
{
    uint32_t knownMask = rev.clickedMask;
    uint32_t partial = 0;
    for (const ShipInfo& si : ships) {
        if (si.slot.size != 2 || si.slot.isAnon || si.slot.knownCount != 1) continue;
        for (const Placement* p : si.placements)
            partial |= p->mask & ~knownMask;
    }
    return partial;
}

static inline double clamp01(double v) {
    if (!std::isfinite(v)) return 0.0;
    return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
}

static double getMegPressure(int shipsHit, int bluesUsed) {
    double p = 0.0;
    if      (shipsHit >= 5) p += 1.0;
    else if (shipsHit == 4) p += 0.35;
    if      (bluesUsed >= 3) p += 1.0;
    else if (bluesUsed == 2) p += 0.65;
    else if (bluesUsed == 1) p += 0.25;
    return clamp01(p / 2.0);
}

// ---------------------------------------------------------------------------
// Fill derived scoring fields
// forcedAggressive: when true, huntMode is always true (aggressive variant)
// ---------------------------------------------------------------------------

static void fillDerivedFields(
    std::vector<CandStat>& cands,
    const EnumResult& res,
    const RevealedState& rev,
    const std::vector<ShipInfo>& ships,
    int shipsHit, int bluesUsed, int nColors,
    bool forcedAggressive)
{
    double N = (double)res.valid;
    double meg = getMegPressure(shipsHit, bluesUsed);
    int foundColors = rev.foundColorsCount();
    const char* phase = getGamePhase(shipsHit, bluesUsed, foundColors);
    bool isLate = (phase[0] == 'l');
    uint32_t partialMask = getPartialShipMask(rev, ships);

    bool huntMode = forcedAggressive || (shipsHit >= 5) || (nColors >= 8);

    for (CandStat& cs : cands) {
        if (res.fullBoardsAvailable && N > 0) {
            cs.infoGain    = 1.0 - cs.expectedRemaining / N;
            cs.worstSafety = clamp01(1.0 - cs.worstRemaining / N);
        } else {
            cs.infoGain    = 2.0 * cs.p * (1.0 - cs.p);
            cs.worstSafety = cs.infoGain;
        }

        cs.shipDiscovery = clamp01(cs.size4 * 1.0 + cs.size3 * 0.75 + cs.size2 * 0.45);
        cs.normalizedEV  = clamp01(cs.weightedEV / EV_RAINBOW);
        cs.megPressure   = meg;

        cs.safeBoardReduction              = cs.infoGain;
        cs.bestSafeWorstOutcomeForcedEV    = cs.forcedEV;
        cs.bestSafeWorstOutcomeForcedTiles = cs.forcedTiles;

        bool isPartial = !isLate && ((partialMask >> cs.i) & 1);

        double p = cs.p;
        double hitChance  = p;
        double blueChance = 1.0 - p;
        double score = 0.0;

        if (huntMode) {
            score += hitChance       * WEIGHTS_DEFAULT.aggressiveHitChance;
            score += cs.infoGain     * WEIGHTS_DEFAULT.aggressiveInfoGain;
            score += cs.shipDiscovery * WEIGHTS_DEFAULT.aggressiveShipDiscovery;
            score += cs.normalizedEV  * WEIGHTS_DEFAULT.aggressiveEV;
            score += cs.worstSafety   * WEIGHTS_DEFAULT.aggressiveWorstSafety;
            score -= blueChance      * WEIGHTS_DEFAULT.aggressiveMissPenalty;
            score -= blueChance * meg * WEIGHTS_DEFAULT.aggressiveMegBluePenalty;
            if (isPartial) score -= WEIGHTS_DEFAULT.aggressivePartialPenalty;
            cs.safePrimaryChance = hitChance;
        } else {
            double freeBlue = (shipsHit < 5) ? blueChance : 0.0;
            score += cs.infoGain     * WEIGHTS_DEFAULT.safeInfoGain;
            score += cs.shipDiscovery * WEIGHTS_DEFAULT.safeShipDiscovery;
            score += freeBlue        * WEIGHTS_DEFAULT.safeFreeBlue;
            score += cs.worstSafety   * WEIGHTS_DEFAULT.safeWorstSafety;
            score -= hitChance       * WEIGHTS_DEFAULT.safeHitRiskPenalty;
            score -= blueChance * meg * WEIGHTS_DEFAULT.safeMegBluePenalty;
            if (isPartial) score -= WEIGHTS_DEFAULT.safePartialPenalty;
            cs.safePrimaryChance = blueChance;
        }

        cs.score = score;
    }
}

// ---------------------------------------------------------------------------
// getSafestMove (both avoidance toggles off at defaults)
// ---------------------------------------------------------------------------

static int getSafestMoveIdx(const std::vector<CandStat>& cands) {
    if (cands.empty()) return -1;

    double best = -1.0;
    for (const CandStat& cs : cands)
        if (cs.safePrimaryChance > best) best = cs.safePrimaryChance;

    std::vector<int> pool;
    for (int k = 0; k < (int)cands.size(); ++k)
        if (cands[k].safePrimaryChance >= best - SAFE_RISK_BAND)
            pool.push_back(k);

    if ((int)pool.size() == 1) return pool[0];

    // bestSafeAggressive tiebreaker chain (all higher-is-better).
    // Mirrors JS DEFAULT_TIEBREAKER_CONFIG.bestSafeAggressive with default
    // enabled flags: bestSafeWorstOutcomeForcedEV and bestSafeWorstOutcomeForcedTiles
    // are DISABLED by default in JS (tieBreakerEnabled.bestSafeAggressive.*=false),
    // so they are omitted here.
    auto better = [&](int a, int b) -> bool {
        const CandStat& ca = cands[a];
        const CandStat& cb = cands[b];
#define TB(field) \
        if (std::abs(ca.field - cb.field) > EPSILON) return ca.field > cb.field;
        TB(size4)
        TB(size3)
        TB(size2)
        TB(safeBoardReduction)
        TB(weightedEV)
        TB(safePrimaryChance)
#undef TB
        return ca.i < cb.i;  // lower cell index as final tiebreak
    };

    int best_idx = pool[0];
    for (int k = 1; k < (int)pool.size(); ++k)
        if (better(pool[k], best_idx)) best_idx = pool[k];
    return best_idx;
}

// ---------------------------------------------------------------------------
// rankAndPick — sort candidates, remove safest from strategic pool,
// return cell index of topMoves[0] (the recommended-best cell)
// ---------------------------------------------------------------------------

static int rankAndPick(std::vector<CandStat>& cands) {
    if (cands.empty()) return -1;

    std::stable_sort(cands.begin(), cands.end(), [](const CandStat& a, const CandStat& b) {
        double diff = b.score - a.score;
        if (diff >  EPSILON) return false;
        if (diff < -EPSILON) return true;
        if (std::abs(a.infoGain - b.infoGain) > EPSILON) return a.infoGain > b.infoGain;
        return a.i < b.i;
    });

    int safestIdx = getSafestMoveIdx(cands);

    // topMoves[0] = first candidate that isn't safest
    for (int k = 0; k < (int)cands.size(); ++k)
        if (safestIdx < 0 || cands[k].i != cands[safestIdx].i)
            return cands[k].i;
    return cands[0].i;
}

// ---------------------------------------------------------------------------
// Opening move (mirrors highlightStartingCorners)
// forcedAggressive controls the 6/7-color tile set
// ---------------------------------------------------------------------------

static int openingMove(int nColors, const RevealedState& rev, bool forcedAggressive) {
    int rareCount = nColors - 4;

    static const int tiles89[3]   = {8, 12, 16};
    static const int tilesAgg[4]  = {7, 11, 13, 17};
    static const int tilesSafe[4] = {0, 4, 20, 24};

    const int* tiles; int nTiles;
    if (rareCount >= 4) {
        // 8/9-color: always use89ColorOpening regardless of mode
        tiles = tiles89; nTiles = 3;
    } else if (forcedAggressive) {
        tiles = tilesAgg; nTiles = 4;
    } else {
        tiles = tilesSafe; nTiles = 4;
    }

    for (int k = 0; k < nTiles; ++k)
        if (!((rev.clickedMask >> tiles[k]) & 1)) return tiles[k];

    for (int i = 0; i < GRID; ++i)
        if (!((rev.clickedMask >> i) & 1)) return i;
    return 0;
}

// ---------------------------------------------------------------------------
// Main entry point — called from strategy class next_click
// forcedAggressive: false = safe/default variant, true = aggressive variant
// ---------------------------------------------------------------------------

static void tksglass_next_click(
    const std::vector<Cell>& board,
    const std::string& meta_json,
    ClickResult& out,
    bool forcedAggressive)
{
    // Parse meta
    const char* mj = meta_json.c_str();
    int nColors = 6, shipsHit = 0, bluesUsed = 0;
    const char* p;
    if ((p = strstr(mj, "\"n_colors\":")))   { p += 11; while (*p==' ') ++p; nColors  = atoi(p); }
    if ((p = strstr(mj, "\"ships_hit\":")))  { p += 12; while (*p==' ') ++p; shipsHit = atoi(p); }
    if ((p = strstr(mj, "\"blues_used\":"))) { p += 13; while (*p==' ') ++p; bluesUsed= atoi(p); }

    RevealedState rev;
    rev.build(board, shipsHit, bluesUsed);

    // Opening: no clicks yet (mirrors !hasStarted)
    if (rev.clickedMask == 0) {
        int idx = openingMove(nColors, rev, forcedAggressive);
        out.row = idx / 5; out.col = idx % 5;
        return;
    }

    std::vector<ShipInfo> ships = buildShipList(rev, nColors);
    EnumResult res = enumerateBoards(ships, nColors);

    if (res.valid == 0) {
        for (int i = 0; i < GRID; ++i)
            if (!((rev.clickedMask >> i) & 1)) { out.row = i/5; out.col = i%5; return; }
        out.row = 0; out.col = 0; return;
    }

    bool huntMode    = forcedAggressive || (shipsHit >= 5) || (nColors >= 8);
    bool missEndsGame = (shipsHit >= 5) && (bluesUsed >= 3);

    // findGuaranteedMove
    if (!huntMode) {
        // Safe mode: click p==0 only when blues_used >= 3 AND ships_hit < 5
        if (shipsHit < 5 && bluesUsed >= 3) {
            for (int i = 0; i < GRID; ++i) {
                if ((rev.clickedMask >> i) & 1) continue;
                if (res.probs[i] == 0.0) { out.row = i/5; out.col = i%5; return; }
            }
        }
    } else {
        // Hunt mode: click p==1 only when missEndsGame
        if (missEndsGame) {
            for (int i = 0; i < GRID; ++i) {
                if ((rev.clickedMask >> i) & 1) continue;
                if (res.probs[i] == 1.0) { out.row = i/5; out.col = i%5; return; }
            }
        }
    }

    std::vector<CandStat> cands = buildCandidateStats(res, ships, rev, nColors);

    // Filter: hunt excludes p>=1, safe excludes p<=0
    {
        auto it = std::remove_if(cands.begin(), cands.end(), [&](const CandStat& cs) {
            return huntMode ? (cs.p >= 1.0) : (cs.p <= 0.0);
        });
        cands.erase(it, cands.end());
    }

    if (cands.empty()) {
        for (int i = 0; i < GRID; ++i)
            if (!((rev.clickedMask >> i) & 1)) { out.row = i/5; out.col = i%5; return; }
        out.row = 0; out.col = 0; return;
    }

    fillDerivedFields(cands, res, rev, ships, shipsHit, bluesUsed, nColors, forcedAggressive);

    int best = rankAndPick(cands);
    if (best < 0) {
        for (int i = 0; i < GRID; ++i)
            if (!((rev.clickedMask >> i) & 1)) { out.row = i/5; out.col = i%5; return; }
        out.row = 0; out.col = 0; return;
    }
    out.row = best / 5; out.col = best % 5;
}

// ---------------------------------------------------------------------------
// Board JSON parser (shared between both variants via the macro)
// ---------------------------------------------------------------------------

static std::vector<Cell> tksglass_parse_board(const char* board_json) {
    std::vector<Cell> board;
    board.reserve(25);
    const char* p = board_json;
    while ((p = strstr(p, "\"row\":")) != nullptr) {
        Cell c;
        c.row = atoi(p + 6);
        const char* cp   = strstr(p, "\"col\":");
        if (cp) c.col = atoi(cp + 6);
        const char* colp = strstr(p, "\"color\":\"");
        if (colp) {
            colp += 9;
            const char* e = strchr(colp, '"');
            if (e) c.color = std::string(colp, e - colp);
        }
        const char* clkp = strstr(p, "\"clicked\":");
        if (clkp) { clkp += 10; while (*clkp == ' ') ++clkp; c.clicked = (strncmp(clkp, "true", 4) == 0); }
        board.push_back(c);
        p += 6;
    }
    return board;
}

// ---------------------------------------------------------------------------
// TKSGLASS_EXPORTS macro — emits all five extern "C" harness symbols
// ---------------------------------------------------------------------------

#define TKSGLASS_EXPORTS(ClassName)                                                     \
extern "C" sphere::StrategyBase* create_strategy()  { return new ClassName(); }         \
extern "C" void destroy_strategy(sphere::StrategyBase* s) { delete s; }                \
extern "C" void strategy_init_evaluation_run(void* inst) {                              \
    static_cast<ClassName*>(inst)->init_evaluation_run();                               \
}                                                                                       \
extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {        \
    static_cast<ClassName*>(inst)->init_game_payload(meta_json ? meta_json : "{}");    \
}                                                                                       \
extern "C" const char* strategy_next_click(void* inst,                                 \
                                           const char* board_json,                     \
                                           const char* meta_json) {                    \
    thread_local static std::string buf;                                                \
    auto* s = static_cast<ClassName*>(inst);                                            \
    std::vector<Cell> board = tksglass_parse_board(board_json ? board_json : "[]");    \
    ClickResult out;                                                                    \
    s->next_click(board, meta_json ? meta_json : "{}", out);                           \
    buf = "{\"row\":" + std::to_string(out.row) +                                      \
          ",\"col\":" + std::to_string(out.col) + "}";                                 \
    return buf.c_str();                                                                 \
}
