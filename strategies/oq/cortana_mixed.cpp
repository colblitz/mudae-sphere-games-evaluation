/**
 * cortana_mixed.cpp — C++ port of the MIXED(alpha=1.0, beta=0.1) heuristic
 *                     for /sphere quest (oq).
 *
 * Ported from:
 *   cortana3/strategy/sphere_quest.py  — SphereQuestGame / _next_click_impl
 *   MagiBot/magibot/policy/quest.py    — QuestPolicy / _compute_full_state
 *
 * Algorithm
 * ---------
 * Each game maintains a set of candidate purple-placement bitmasks (all
 * C(25,4)=12,650 possible ways to place 4 purples on the 5×5 grid).  As cells
 * are revealed the set is filtered down using the Minesweeper-style constraints:
 *
 *   Purple / red reveal  → cell must occupy a purple position in the mask.
 *   Non-purple reveal    → cell must NOT be purple; its 8-neighbour count of
 *                          purples must equal the colour's encoded value.
 *
 * Colour encoding:  spB=0, spT=1, spG=2, spY=3, spO=4 purple neighbours.
 *
 * Phase logic (evaluated in order each call):
 *
 *   1. Red visible (spR, not yet clicked) → click it immediately.
 *   2. Purple visible (spP, not yet clicked) → click it (free).
 *   3. All 4 purples known (3 clicked + spR auto-revealed) → post-red greedy:
 *      derive each unclicked cell's colour from the purple mask and click the
 *      highest-value non-purple cell.
 *   4. Default: MIXED search — score each unclicked cell as
 *         score = ALPHA * P(purple) + BETA * Gini_impurity
 *      where Gini = 1 − Σ_c P(outcome=c)².  Pick the highest-scoring cell;
 *      ties broken toward the lower flat index.
 *
 * Constants (cortana3 sphere_constants.py):
 *   spB=10, spT=20, spG=35, spY=55, spO=90, spR=150
 *   ALPHA=1.0, BETA=0.1
 *
 * Performance notes
 * -----------------
 * •  All 12,650 purple-placement bitmasks are precomputed once at construction
 *    as a flat uint32_t array.
 * •  Per-game state is two flat arrays: surviving_[12650] (bitmasks of still-
 *    consistent placements) and n_surviving_ (count).  Filtering is a single
 *    in-place compaction pass — no heap allocation per call.
 * •  outcome_counts is a 25×6 flat array on the stack; the inner loop over
 *    surviving masks is branchless (bit ops) and cache-friendly.
 * •  Colour comparisons use a Color enum (uint8_t) resolved once per board
 *    parse rather than repeated string comparisons.
 * •  NB_MASK and NB_COUNT lookup tables are compile-time constants.
 * •  Scoring loop uses integer counts throughout; divides only when computing
 *    the final float score per candidate cell, not per mask.
 */

#pragma GCC optimize("O3,unroll-loops")

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../interface/strategy.h"

using namespace sphere;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr int N_CELLS    = 25;
static constexpr int N_PURPLES  = 4;
static constexpr int GRID       = 5;
static constexpr double ALPHA   = 1.0;
static constexpr double BETA    = 0.1;

// Point values (cortana3 SPHERE_BASE_VALUES)
static constexpr int VAL_B = 10;   // spB – 0 purple neighbours
static constexpr int VAL_T = 20;   // spT – 1 purple neighbour
static constexpr int VAL_G = 35;   // spG – 2 purple neighbours
static constexpr int VAL_Y = 55;   // spY – 3 purple neighbours
static constexpr int VAL_O = 90;   // spO – 4 purple neighbours
static constexpr int VAL_R = 150;  // spR – red sphere

// Total number of C(25,4) placements
static constexpr int N_MASKS = 12650;

// ---------------------------------------------------------------------------
// Color enum — avoids repeated std::string comparisons
// ---------------------------------------------------------------------------

enum Color : uint8_t {
    COL_UNK = 0,  // spU (covered/unknown)
    COL_P   = 1,  // spP (purple)
    COL_R   = 2,  // spR (red)
    COL_B   = 3,  // spB – 0nb
    COL_T   = 4,  // spT – 1nb
    COL_G   = 5,  // spG – 2nb
    COL_Y   = 6,  // spY – 3nb
    COL_O   = 7,  // spO – 4nb
};

static inline Color parse_color(const char* s, int len) {
    // All relevant names start with "sp" (2 chars)
    if (len < 3) return COL_UNK;
    switch (s[2]) {
        case 'U': return COL_UNK;
        case 'P': return COL_P;
        case 'R': return COL_R;
        case 'B': return COL_B;
        case 'T': return COL_T;
        case 'G': return COL_G;
        case 'Y': return COL_Y;
        case 'O': return COL_O;
        default:  return COL_UNK;
    }
}

// Neighbour count encoded by each colour (index = Color enum value)
// COL_UNK=0, COL_P=0, COL_R=0, COL_B=0, COL_T=1, COL_G=2, COL_Y=3, COL_O=4
static constexpr int NB_COUNT_FOR_COLOR[8] = { 0, 0, 0, 0, 1, 2, 3, 4 };

// Point value for post-red greedy (index = Color enum value)
static constexpr int COLOR_VALUE[8] = { 0, 0, VAL_R, VAL_B, VAL_T, VAL_G, VAL_Y, VAL_O };

// Outcome index for MIXED scoring (purple=0, others=nb_count+1)
// COL_B→1, COL_T→2, COL_G→3, COL_Y→4, COL_O→5
static constexpr int OUTCOME_IDX[8] = { -1, -1, -1, 1, 2, 3, 4, 5 };

// ---------------------------------------------------------------------------
// Precomputed geometry — filled once at class construction
// ---------------------------------------------------------------------------

// 8-neighbour bitmask for each flat cell index (uint32_t, bits 0..24)
static uint32_t NB_MASK[N_CELLS];

// All C(25,4) purple-placement bitmasks — initialised once
static uint32_t ALL_MASKS[N_MASKS];

static bool geometry_ready = false;

static void init_geometry() {
    if (geometry_ready) return;

    // Build neighbour masks
    for (int r = 0; r < GRID; ++r) {
        for (int c = 0; c < GRID; ++c) {
            uint32_t m = 0;
            for (int dr = -1; dr <= 1; ++dr)
                for (int dc = -1; dc <= 1; ++dc) {
                    if (dr == 0 && dc == 0) continue;
                    int nr = r + dr, nc = c + dc;
                    if (nr >= 0 && nr < GRID && nc >= 0 && nc < GRID)
                        m |= 1u << (nr * GRID + nc);
                }
            NB_MASK[r * GRID + c] = m;
        }
    }

    // Generate all C(25,4) combinations (combinatorial number system order)
    int idx = 0;
    for (int a = 0;    a < N_CELLS; ++a)
    for (int b = a+1;  b < N_CELLS; ++b)
    for (int c = b+1;  c < N_CELLS; ++c)
    for (int d = c+1;  d < N_CELLS; ++d)
        ALL_MASKS[idx++] = (1u<<a)|(1u<<b)|(1u<<c)|(1u<<d);

    geometry_ready = true;
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------

class CortanaMixedOQStrategy : public OQStrategy {
public:
    CortanaMixedOQStrategy() {
        init_geometry();
    }

    // ------------------------------------------------------------------
    // init_evaluation_run — geometry already set up in constructor; no-op
    // ------------------------------------------------------------------
    void init_evaluation_run() override {}

    // ------------------------------------------------------------------
    // init_game_payload — reset per-game state
    // ------------------------------------------------------------------
    void init_game_payload(const std::string& /*meta_json*/) override {
        // Copy all 12,650 masks into the per-game surviving set
        n_surviving_ = N_MASKS;
        std::memcpy(surviving_, ALL_MASKS, sizeof(uint32_t) * N_MASKS);

        // Clear previous reveal tracking
        std::memset(known_color_, 0, sizeof(known_color_));  // all COL_UNK
        std::memset(known_revealed_, 0, sizeof(known_revealed_));
    }

    // ------------------------------------------------------------------
    // next_click
    // ------------------------------------------------------------------
    void next_click(const std::vector<Cell>& board,
                    const std::string& /*meta_json*/,
                    ClickResult& out) override
    {
        // ----------------------------------------------------------------
        // 1. Parse board into flat arrays
        // ----------------------------------------------------------------
        bool      clicked[N_CELLS]    = {};
        Color     board_color[N_CELLS] = {};

        for (const Cell& c : board) {
            int idx = c.row * GRID + c.col;
            clicked[idx]     = c.clicked;
            board_color[idx] = parse_color(c.color.c_str(), (int)c.color.size());
        }

        // ----------------------------------------------------------------
        // 2. Update surviving_ with newly revealed cells
        // ----------------------------------------------------------------
        for (int i = 0; i < N_CELLS; ++i) {
            Color col = board_color[i];
            // A cell is "revealed" when it has a real (non-UNK) color
            if (!known_revealed_[i] && col != COL_UNK) {
                known_revealed_[i] = true;
                known_color_[i]    = col;
                filter_masks(i, col);
            }
        }

        // ----------------------------------------------------------------
        // 3. Classify cells
        // ----------------------------------------------------------------
        int unclicked[N_CELLS];
        int n_unclicked = 0;

        int clicked_purples[N_CELLS];  // flat indices
        int n_clicked_purples = 0;
        int red_cell = -1;             // flat index of visible spR (unclicked)

        for (int i = 0; i < N_CELLS; ++i) {
            if (!clicked[i]) {
                unclicked[n_unclicked++] = i;
                if (board_color[i] == COL_R) {
                    red_cell = i;
                }
            } else {
                // clicked and purple
                if (board_color[i] == COL_P) {
                    clicked_purples[n_clicked_purples++] = i;
                }
            }
        }

        // ----------------------------------------------------------------
        // 4. Phase 1: Click visible red immediately
        // ----------------------------------------------------------------
        if (red_cell >= 0) {
            out.row = red_cell / GRID;
            out.col = red_cell % GRID;
            return;
        }

        // ----------------------------------------------------------------
        // 5. Phase 2: Click any visible unclicked purple (free click)
        // ----------------------------------------------------------------
        for (int i = 0; i < n_unclicked; ++i) {
            int idx = unclicked[i];
            if (board_color[idx] == COL_P) {
                out.row = idx / GRID;
                out.col = idx % GRID;
                return;
            }
        }

        if (n_unclicked == 0) {
            out.row = 0; out.col = 0;
            return;
        }

        // ----------------------------------------------------------------
        // 6. Phase 3: Post-red greedy — all 4 purples known
        //    Condition: 3 clicked purples + 1 spR auto-revealed somewhere
        //    (spR is in board_color but with clicked=false initially, then
        //    once clicked it's in clicked with color spR)
        // ----------------------------------------------------------------
        // Count total known purples: clicked purples + any red cell that
        // has already been revealed (clicked red counts as a known purple
        // position because it was the 4th purple converted to red)
        int red_clicked_idx = -1;
        for (int i = 0; i < N_CELLS; ++i) {
            if (clicked[i] && board_color[i] == COL_R) {
                red_clicked_idx = i;
                break;
            }
        }

        int n_all_purples = n_clicked_purples + (red_clicked_idx >= 0 ? 1 : 0);

        if (n_all_purples == N_PURPLES) {
            // Build purple mask from all known purple positions
            uint32_t purple_mask = 0;
            for (int k = 0; k < n_clicked_purples; ++k)
                purple_mask |= 1u << clicked_purples[k];
            if (red_clicked_idx >= 0)
                purple_mask |= 1u << red_clicked_idx;

            int best = post_red_greedy(unclicked, n_unclicked, purple_mask);
            if (best >= 0) {
                out.row = best / GRID;
                out.col = best % GRID;
            } else {
                out.row = unclicked[0] / GRID;
                out.col = unclicked[0] % GRID;
            }
            return;
        }

        // ----------------------------------------------------------------
        // 7. Phase 4: MIXED search
        // ----------------------------------------------------------------
        int best = mixed_search(unclicked, n_unclicked);
        out.row = best / GRID;
        out.col = best % GRID;
    }

private:
    // -----------------------------------------------------------------------
    // Per-game state
    // -----------------------------------------------------------------------

    uint32_t surviving_[N_MASKS];
    int      n_surviving_;

    // Track which cells have already been incorporated into surviving_
    bool  known_revealed_[N_CELLS];
    Color known_color_[N_CELLS];

    // -----------------------------------------------------------------------
    // filter_masks — in-place compaction keeping only consistent placements
    // -----------------------------------------------------------------------
    void filter_masks(int cell_idx, Color col) {
        uint32_t cell_bit = 1u << cell_idx;

        if (col == COL_P || col == COL_R) {
            // Purple / red reveal: cell must be a purple in the mask
            int out = 0;
            for (int i = 0; i < n_surviving_; ++i) {
                if (surviving_[i] & cell_bit)
                    surviving_[out++] = surviving_[i];
            }
            n_surviving_ = out;
        } else {
            // Non-purple reveal: cell must NOT be purple; neighbour purple
            // count must equal the value encoded by the colour
            int k        = NB_COUNT_FOR_COLOR[col];
            uint32_t nb  = NB_MASK[cell_idx];
            int out = 0;
            for (int i = 0; i < n_surviving_; ++i) {
                uint32_t m = surviving_[i];
                if (!(m & cell_bit) && __builtin_popcount(m & nb) == k)
                    surviving_[out++] = m;
            }
            n_surviving_ = out;
        }
    }

    // -----------------------------------------------------------------------
    // post_red_greedy — pick highest-value derivable non-purple cell
    // -----------------------------------------------------------------------
    int post_red_greedy(const int* unclicked, int n_unclicked,
                        uint32_t purple_mask) const
    {
        int best_idx = -1;
        int best_val = -1;

        for (int i = 0; i < n_unclicked; ++i) {
            int ci = unclicked[i];
            if ((purple_mask >> ci) & 1) continue;  // skip remaining purples

            // Derive colour from neighbour count
            int nb_purples = __builtin_popcount(purple_mask & NB_MASK[ci]);
            // Clamp to 4 (game max), then map to Color
            if (nb_purples > 4) nb_purples = 4;
            // nb→Color: 0→COL_B, 1→COL_T, 2→COL_G, 3→COL_Y, 4→COL_O
            static constexpr Color nb_to_col[5] = {COL_B, COL_T, COL_G, COL_Y, COL_O};
            Color derived = nb_to_col[nb_purples];
            int val = COLOR_VALUE[derived];
            if (val > best_val) {
                best_val = val;
                best_idx = ci;
            }
        }

        return best_idx;
    }

    // -----------------------------------------------------------------------
    // mixed_search — ALPHA*P(purple) + BETA*Gini; lower index breaks ties
    // -----------------------------------------------------------------------
    int mixed_search(const int* unclicked, int n_unclicked) const
    {
        if (n_unclicked == 0) return 0;
        if (n_surviving_ == 0) return unclicked[0];

        // outcome_counts[cell][outcome]: count of surviving masks where
        // clicking cell yields that outcome.
        // outcome 0 = purple, 1..5 = 0..4 non-purple neighbours
        // Only compute for unclicked cells; use a flat 25×6 layout.
        int oc[N_CELLS][6] = {};  // zero-initialised on stack

        // Single pass over all surviving masks
        for (int mi = 0; mi < n_surviving_; ++mi) {
            uint32_t mask = surviving_[mi];
            for (int i = 0; i < n_unclicked; ++i) {
                int ci = unclicked[i];
                if ((mask >> ci) & 1u) {
                    oc[ci][0]++;
                } else {
                    int nb = __builtin_popcount(mask & NB_MASK[ci]);
                    if (nb > 4) nb = 4;
                    oc[ci][nb + 1]++;
                }
            }
        }

        double inv_n = 1.0 / (double)n_surviving_;
        int    best_idx   = unclicked[0];
        double best_score = -1.0;

        for (int i = 0; i < n_unclicked; ++i) {
            int ci = unclicked[i];
            const int* counts = oc[ci];

            double p_purple = counts[0] * inv_n;

            // Gini = 1 - sum((c/n)^2)
            double sum_sq = 0.0;
            for (int c = 0; c < 6; ++c) {
                double p = counts[c] * inv_n;
                sum_sq += p * p;
            }
            double gini  = 1.0 - sum_sq;
            double score = ALPHA * p_purple + BETA * gini;

            if (score > best_score) {
                best_score = score;
                best_idx   = ci;
            }
            // Ties: lower index wins (unclicked is iterated in row-major order,
            // so the first occurrence of a tied score is already the lower index)
        }

        return best_idx;
    }
};

// ---------------------------------------------------------------------------
// C exports required by the harness — do not rename these functions
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()
    { return new CortanaMixedOQStrategy(); }
extern "C" void destroy_strategy(sphere::StrategyBase* s)
    { delete s; }

extern "C" const char* strategy_init_evaluation_run(void* inst) {
    static_cast<CortanaMixedOQStrategy*>(inst)->init_evaluation_run();
    return "{}";
}

extern "C" const char* strategy_init_game_payload(void* inst,
                                                   const char* meta_json,
                                                   const char* /*game_state_json*/)
{
    static_cast<CortanaMixedOQStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
    return "{}";
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json,
                                            const char* /*game_state_json*/)
{
    thread_local static std::string buf;
    auto* s = static_cast<CortanaMixedOQStrategy*>(inst);

    // Parse board JSON into Cell vector
    // Format: [{"row":R,"col":C,"color":"spX","clicked":true/false}, ...]
    std::vector<Cell> board;
    board.reserve(N_CELLS);
    const char* p = board_json;
    while ((p = strstr(p, "\"row\":")) != nullptr) {
        Cell c;
        c.row = atoi(p + 6);
        const char* cp = strstr(p, "\"col\":");
        if (cp) c.col = atoi(cp + 6);
        const char* colp = strstr(p, "\"color\":\"");
        if (colp) {
            colp += 9;
            const char* e = strchr(colp, '"');
            if (e) c.color = std::string(colp, e - colp);
        }
        const char* clkp = strstr(p, "\"clicked\":");
        if (clkp) {
            clkp += 10;
            while (*clkp == ' ') ++clkp;
            c.clicked = (strncmp(clkp, "true", 4) == 0);
        }
        board.push_back(c);
        p += 6;
    }

    ClickResult out;
    s->next_click(board, meta_json ? meta_json : "{}", out);

    buf = "{\"row\":" + std::to_string(out.row) +
          ",\"col\":" + std::to_string(out.col) + "}";
    return buf.c_str();
}
