/**
 * svessinn_solver.cpp — Faithful C++ port of Svessinn's Trace solver for /sphere trace (ot).
 *
 * Source: https://github.com/Svessinn/Svessinn.github.io/blob/main/Mudae/Spheres/Trace/solver.html
 *
 * ALGORITHM OVERVIEW
 * ------------------
 * The board contains horizontal/vertical ship segments (fixed lengths per color)
 * plus blue (spB) empty cells.  Blue clicks cost 1 of the 4-click budget; ship
 * cell reveals are free.
 *
 * The solver runs a 4-pass deduction engine each turn, then applies phase logic:
 *
 * Pass 1 — Geometric certainty (intersection):
 *   For each ship color with remaining un-found segments, enumerate all valid
 *   placements consistent with already-revealed cells.  Any cell that appears
 *   in EVERY valid placement for that ship is certain to be that ship color.
 *   Iterate this to fixed-point.
 *
 * Pass 2 — Ship weights:
 *   For each ship color, distribute weight 1/|validPlacements| equally across
 *   each cell of each valid placement (excluding already-revealed cells and
 *   cells whose color is already certain from Pass 1).
 *
 * Pass 3 — Rare ship identity weighting:
 *   For ships whose color is not yet identified, distribute a "?" weight
 *   across valid size-2 placements of unoccupied cells.
 *
 * Pass 4 — Blue normalisation:
 *   Compute the expected number of blue cells remaining and spread that mass
 *   across unresolved cells.  Normalise all heatmap entries to sum to 1.
 *
 * Phase logic:
 *   isHuntingBlue = ships_hit < 5
 *     → click cell with HIGHEST blue probability (safe, avoids ships)
 *   else
 *     → click cell with LOWEST blue probability (most likely to be a ship)
 *
 * State: per-game data stored in revealedMap_ member (reset in init_game_payload).
 */

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../interface/strategy.h"

using namespace sphere;

// ---------------------------------------------------------------------------
// Ship definitions — mirrors ALL_COLORS in solver.html
// Single-char internal codes:
//   B=blue, T=teal, G=green, Y=yellow, O=orange, L=light, D=dark, R=red, W=rainbow
// ---------------------------------------------------------------------------

struct ShipDef {
    char   letter;
    int    len;
    bool   always;  // true = present in all n_colors; false = optional rare
};

static const ShipDef ALL_COLORS[] = {
    { 'B', 0, true  },  // blue — handled separately
    { 'T', 4, true  },
    { 'G', 3, true  },
    { 'Y', 3, true  },
    { 'O', 2, true  },
    { 'L', 2, true  },  // present from 6-color onward
    { 'D', 2, false },
    { 'R', 2, false },
    { 'W', 2, false },
};
static const int N_COLORS = 9;

static char spToLetter(const std::string& sp) {
    if (sp == "spB") return 'B';
    if (sp == "spT") return 'T';
    if (sp == "spG") return 'G';
    if (sp == "spY") return 'Y';
    if (sp == "spO") return 'O';
    if (sp == "spL") return 'L';
    if (sp == "spD") return 'D';
    if (sp == "spR") return 'R';
    if (sp == "spW") return 'W';
    return '?';
}

static int shipLen(char letter) {
    for (int i = 0; i < N_COLORS; ++i)
        if (ALL_COLORS[i].letter == letter) return ALL_COLORS[i].len;
    return 0;
}

static bool isAlways(char letter) {
    for (int i = 0; i < N_COLORS; ++i)
        if (ALL_COLORS[i].letter == letter) return ALL_COLORS[i].always;
    return false;
}

static bool isRareLetter(char letter) {
    for (int i = 0; i < N_COLORS; ++i)
        if (ALL_COLORS[i].letter == letter) return !ALL_COLORS[i].always;
    return false;
}

// ---------------------------------------------------------------------------
// Placement enumeration — mirrors getPossiblePlacements(len) in solver.html
// Returns array of arrays; each inner array = flat cell indices covered.
// ---------------------------------------------------------------------------

static std::vector<std::vector<int>> getPossiblePlacements(int len) {
    std::vector<std::vector<int>> placements;
    // Horizontal
    for (int r = 0; r < 5; ++r) {
        for (int c = 0; c <= 5 - len; ++c) {
            std::vector<int> ship;
            for (int i = 0; i < len; ++i) ship.push_back(r * 5 + (c + i));
            placements.push_back(ship);
        }
    }
    // Vertical
    for (int c = 0; c < 5; ++c) {
        for (int r = 0; r <= 5 - len; ++r) {
            std::vector<int> ship;
            for (int i = 0; i < len; ++i) ship.push_back((r + i) * 5 + c);
            placements.push_back(ship);
        }
    }
    return placements;
}

// ---------------------------------------------------------------------------
// Precomputed placements for all lengths
// (built once at program start via static init)
// ---------------------------------------------------------------------------

static const std::vector<std::vector<int>>& placements(int len) {
    static std::vector<std::vector<int>> p2 = getPossiblePlacements(2);
    static std::vector<std::vector<int>> p3 = getPossiblePlacements(3);
    static std::vector<std::vector<int>> p4 = getPossiblePlacements(4);
    if (len == 2) return p2;
    if (len == 3) return p3;
    return p4;
}

// ---------------------------------------------------------------------------
// getActiveShips — mirrors getActiveShips() in solver.html
// Returns list of active ship letters given nColors and the revealed map.
// ---------------------------------------------------------------------------

static std::vector<char> getActiveShips(
    int nColors,
    const std::unordered_map<int, char>& revealedMap)
{
    // Common (always-present) ships
    std::vector<char> common;
    for (int i = 0; i < N_COLORS; ++i)
        if (ALL_COLORS[i].always) common.push_back(ALL_COLORS[i].letter);

    // Rare ships that have been identified from revealed
    std::set<char> userPlacedRares;
    for (const auto& kv : revealedMap) {
        if (isRareLetter(kv.second)) userPlacedRares.insert(kv.second);
    }
    int totalRaresAllowed = std::max(0, nColors - 5);

    if ((int)userPlacedRares.size() < totalRaresAllowed) {
        // Identity not fully known — include all optional rares
        std::vector<char> result = common;
        for (int i = 0; i < N_COLORS; ++i)
            if (!ALL_COLORS[i].always) result.push_back(ALL_COLORS[i].letter);
        return result;
    }
    // All rares identified — only include the identified ones
    std::vector<char> result = common;
    for (char r : userPlacedRares) result.push_back(r);
    return result;
}

// ---------------------------------------------------------------------------
// Heatmap — per-cell distribution over color letters (and '?' for unknown rare)
// ---------------------------------------------------------------------------

// InsertionMap: a tiny ordered map that preserves key insertion order, exactly
// matching the JS Object key-iteration semantics used in getDeductions().
//
// JS Object iterates keys in insertion order; std::map iterates in ASCII order.
// Because IEEE-754 double addition is not commutative, a different summation
// order in Pass 4 (normalisation) produces ≥1-ULP differences in P(blue) that
// can flip which cell the strategy selects when two candidates are tied.
// Using InsertionMap makes the C++ summation order identical to JS.
//
// The heatmap has at most ~10 distinct keys per cell; linear search is fine.
struct InsertionMap {
    std::vector<std::pair<char, double>> entries;

    // Return reference to value for key, inserting with value 0.0 if absent.
    double& operator[](char k) {
        for (auto& e : entries) if (e.first == k) return e.second;
        entries.push_back({k, 0.0});
        return entries.back().second;
    }

    // Return value for key, or 0.0 if absent (non-inserting).
    double get(char k) const {
        for (const auto& e : entries) if (e.first == k) return e.second;
        return 0.0;
    }

    bool contains(char k) const {
        for (const auto& e : entries) if (e.first == k) return true;
        return false;
    }

    void clear() { entries.clear(); }

    // Assign a single-entry map (mirrors JS: heatmap[idx] = { [color]: 1 })
    void assign_single(char k, double v) {
        entries.clear();
        entries.push_back({k, v});
    }

    // Divide all values by total (in-place normalisation)
    void divide_all(double total) {
        for (auto& e : entries) e.second /= total;
    }

    double sum() const {
        double t = 0.0;
        for (const auto& e : entries) t += e.second;
        return t;
    }
};

using Heatmap = std::array<InsertionMap, 25>;

// ---------------------------------------------------------------------------
// getDeductions — direct port of getDeductions() from solver.html
// ---------------------------------------------------------------------------

struct DeductionResult {
    std::unordered_map<int, char> certain;  // cell idx → color letter
    Heatmap heatmap;
};

static DeductionResult getDeductions(
    const std::unordered_map<int, char>& revealedMap,
    int nColors)
{
    // virtualRevealed is extended with certainties from Pass 1
    std::unordered_map<int, char> virtualRevealed = revealedMap;
    Heatmap heatmap;
    std::unordered_map<int, char> certain;
    bool foundNewCertainty = true;

    std::vector<char> active = getActiveShips(nColors, revealedMap);

    // Collect all optional rare letters
    std::vector<char> allPossibleRares;
    for (int i = 0; i < N_COLORS; ++i)
        if (!ALL_COLORS[i].always) allPossibleRares.push_back(ALL_COLORS[i].letter);

    int totalRaresExpected = nColors - 5;

    std::vector<char> identifiedRares;
    for (const auto& kv : revealedMap)
        if (isRareLetter(kv.second)) {
            bool found = false;
            for (char c : identifiedRares) if (c == kv.second) { found = true; break; }
            if (!found) identifiedRares.push_back(kv.second);
        }
    int missingRaresCount = totalRaresExpected - (int)identifiedRares.size();

    // ---- PASS 1: INTERSECTION (GEOMETRIC CERTAINTY) ----
    while (foundNewCertainty) {
        foundNewCertainty = false;

        // Count how many of each color are revealed in virtualRevealed
        std::unordered_map<char, int> currentCounts;
        for (const auto& kv : virtualRevealed) currentCounts[kv.second]++;

        for (char color : active) {
            int len = shipLen(color);
            if (len == 0) continue;  // skip blue

            std::vector<int> foundIdx;
            for (const auto& kv : virtualRevealed)
                if (kv.second == color) foundIdx.push_back(kv.first);

            if (currentCounts[color] >= len) continue;  // ship fully found

            // Valid placements
            std::vector<std::vector<int>> validPos;
            for (const auto& pos : placements(len)) {
                // Must include all already-found indices
                bool hasAll = true;
                for (int fi : foundIdx) {
                    bool inPos = false;
                    for (int idx : pos) if (idx == fi) { inPos = true; break; }
                    if (!inPos) { hasAll = false; break; }
                }
                if (!hasAll) continue;
                // Must not overlap cells assigned to a different color
                bool noConflict = true;
                for (int idx : pos) {
                    auto it = virtualRevealed.find(idx);
                    if (it != virtualRevealed.end() && it->second != color) {
                        noConflict = false; break;
                    }
                }
                if (noConflict) validPos.push_back(pos);
            }

            if (!validPos.empty()) {
                // Intersection: cells present in ALL valid placements
                for (int idx : validPos[0]) {
                    bool inAll = true;
                    for (const auto& pos : validPos) {
                        bool inThis = false;
                        for (int v : pos) if (v == idx) { inThis = true; break; }
                        if (!inThis) { inAll = false; break; }
                    }
                    if (inAll && virtualRevealed.find(idx) == virtualRevealed.end()) {
                        virtualRevealed[idx] = color;
                        certain[idx] = color;
                        heatmap[idx].assign_single(color, 1.0);
                        foundNewCertainty = true;
                    }
                }
            }
        }
    }

    // ---- PASS 2: SHIP WEIGHTS ----
    for (char color : active) {
        int len = shipLen(color);
        if (len == 0) continue;

        int currentCount = 0;
        for (const auto& kv : virtualRevealed) if (kv.second == color) ++currentCount;
        if (currentCount >= len) continue;

        std::vector<int> foundIdx;
        for (const auto& kv : virtualRevealed)
            if (kv.second == color) foundIdx.push_back(kv.first);

        std::vector<std::vector<int>> validPos;
        for (const auto& pos : placements(len)) {
            bool hasAll = true;
            for (int fi : foundIdx) {
                bool inPos = false;
                for (int idx : pos) if (idx == fi) { inPos = true; break; }
                if (!inPos) { hasAll = false; break; }
            }
            if (!hasAll) continue;
            bool noConflict = true;
            for (int idx : pos) {
                auto it = virtualRevealed.find(idx);
                if (it != virtualRevealed.end() && it->second != color) {
                    noConflict = false; break;
                }
            }
            if (noConflict) validPos.push_back(pos);
        }

        if (!validPos.empty()) {
            double weight = 1.0 / (double)validPos.size();
            for (const auto& pos : validPos) {
                for (int idx : pos) {
                    if (!revealedMap.count(idx) && !certain.count(idx)) {
                        heatmap[idx][color] += weight;
                    }
                }
            }
        }
    }

    // ---- PASS 3: IDENTITY-AWARE RARE LOGIC ----
    if (missingRaresCount > 0) {
        // Unidentified rare letters
        std::vector<char> unidentifiedRares;
        for (char r : allPossibleRares) {
            bool identified = false;
            for (char ir : identifiedRares) if (ir == r) { identified = true; break; }
            if (!identified) unidentifiedRares.push_back(r);
        }

        // Valid size-2 placements with no virtual reveals
        std::vector<std::vector<int>> validPos;
        for (const auto& pos : placements(2)) {
            bool allFree = true;
            for (int idx : pos) {
                if (virtualRevealed.count(idx)) { allFree = false; break; }
            }
            if (allFree) validPos.push_back(pos);
        }

        if (!validPos.empty()) {
            double categoryWeight = missingRaresCount / (double)validPos.size();
            for (const auto& pos : validPos) {
                for (int idx : pos) {
                    if (!revealedMap.count(idx) && !certain.count(idx)) {
                        heatmap[idx]['?'] += categoryWeight;
                        double individualWeight = categoryWeight / (double)unidentifiedRares.size();
                        for (char r : unidentifiedRares) {
                            heatmap[idx][r] += individualWeight;
                        }
                    }
                }
            }
        }
    }

    // ---- PASS 4: BLUE NORMALISATION ----
    int totalShipSegments = 12 + totalRaresExpected * 2;
    int totalBluesExpected = 25 - totalShipSegments;
    int bluesFound = 0;
    for (const auto& kv : revealedMap) if (kv.second == 'B') ++bluesFound;

    std::vector<int> unresolved;
    for (int i = 0; i < 25; ++i)
        if (!revealedMap.count(i) && !certain.count(i)) unresolved.push_back(i);

    double blueWeight = unresolved.empty()
        ? 0.0
        : std::max(0.0, (double)(totalBluesExpected - bluesFound)) / (double)unresolved.size();

    for (int i = 0; i < 25; ++i) {
        if (revealedMap.count(i) || certain.count(i)) continue;
        heatmap[i]['B'] = blueWeight;  // InsertionMap: appends B if not present, updates if present
        double total = heatmap[i].sum();
        if (total > 0) {
            heatmap[i].divide_all(total);
        } else {
            heatmap[i].assign_single('B', 1.0);
        }
    }

    return { certain, heatmap };
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

class SvessinnOTStrategy : public OTStrategy {
public:
    // No cross-game precomputation needed.

    void init_game_payload(const std::string& /*meta_json*/) override {
        // Per-game state is rebuilt from board each call; nothing to reset.
    }

    void next_click(const std::vector<Cell>& board,
                    const std::string& meta_json,
                    ClickResult& out) override
    {
        int nColors  = jsonGetInt(meta_json.c_str(), "\"n_colors\"",  6);
        int shipsHit = jsonGetInt(meta_json.c_str(), "\"ships_hit\"", 0);

        // Rebuild revealedMap from clicked cells
        std::unordered_map<int, char> revealedMap;
        for (const Cell& cell : board) {
            if (!cell.clicked) continue;
            int idx = cell.row * 5 + cell.col;
            char letter = spToLetter(cell.color);
            if (letter != '?') revealedMap[idx] = letter;
        }

        // Run the deduction engine
        DeductionResult dr = getDeductions(revealedMap, nColors);
        const auto& certain = dr.certain;
        const auto& heatmap = dr.heatmap;

        bool isHuntingBlue = (shipsHit < 5);

        // Available cells: not in revealedMap
        std::vector<int> available;
        for (int i = 0; i < 25; ++i)
            if (!revealedMap.count(i)) available.push_back(i);

        // Click any certain non-blue cell immediately
        for (int idx : available) {
            auto it = certain.find(idx);
            if (it != certain.end() && it->second != 'B') {
                out.row = idx / 5;
                out.col = idx % 5;
                return;
            }
        }

        // During ship-hunting phase, click any near-certain ship cell (P(blue) < 0.001)
        if (!isHuntingBlue) {
            for (int idx : available) {
                if (!certain.count(idx)) {
                    double bp = heatmap[idx].get('B');
                    if (bp < 0.001) {
                        out.row = idx / 5;
                        out.col = idx % 5;
                        return;
                    }
                }
            }
        }

        // Pick best move based on phase
        int targetIdx = -1;

        if (!available.empty()) {
            if (isHuntingBlue) {
                // Highest blue probability
                double maxBlue = -1.0;
                for (int idx : available) {
                    double bp = heatmap[idx].get('B');
                    if (bp > maxBlue) { maxBlue = bp; targetIdx = idx; }
                }
            } else {
                // Lowest blue probability
                double minBlue = std::numeric_limits<double>::infinity();
                for (int idx : available) {
                    double bp = heatmap[idx].get('B');
                    if (bp < minBlue) { minBlue = bp; targetIdx = idx; }
                }
            }
        }

        if (targetIdx == -1 && !available.empty()) targetIdx = available[0];
        if (targetIdx == -1) { out.row = 0; out.col = 0; return; }

        out.row = targetIdx / 5;
        out.col = targetIdx % 5;
    }
};

// ---------------------------------------------------------------------------
// C exports required by the harness
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new SvessinnOTStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* inst) {
    static_cast<SvessinnOTStrategy*>(inst)->init_evaluation_run();
}

extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {
    static_cast<SvessinnOTStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<SvessinnOTStrategy*>(inst);

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
