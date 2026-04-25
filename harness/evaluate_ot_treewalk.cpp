/**
 * evaluate_ot_treewalk.cpp — Exact EV tree-walk evaluator for /sphere trace (ot).
 *
 * Replaces the sequential game-runner with a single shared recursive tree walk
 * over all boards simultaneously.  At each tree node the strategy is called once
 * for the current board partition; boards are then split by the revealed cell's
 * color and each branch recurses independently.  Boards that follow the same
 * click path share every strategy call up to their divergence point, so the
 * total number of strategy calls is proportional to the number of distinct
 * observable game states — far fewer than boards × assignments × game_length.
 *
 * Stdev is computed in the same single pass.  The tree walk accumulates both
 * E[score] and E[score²] simultaneously; stdev = sqrt(E[score²] - E[score]²).
 * No second sweep is needed.
 *
 * Requirements:
 *   - Strategy must be stateless: next_click(board, meta) is a pure function
 *     of its arguments (same revealed pattern + meta → same cell every time).
 *   - Opt-in: strategy source file must contain the string "sphere:stateless"
 *     within its first 50 lines.  evaluate.py enforces this before routing here.
 *
 * Output: same RESULT_JSON format as evaluate_ot, with an additional
 *   "evaluator": "treewalk"  field.
 *
 * Usage:
 *   evaluate_ot_treewalk --strategy <path> [--boards-dir <dir>]
 *                        [--n-colors 6|7|8|9|all] [--threads N]
 *
 * Game model (same as evaluate_ot.cpp):
 *   5×5 grid, 25 cells start covered.  Blue click budget: 4.
 *   Ships: teal(4), green(3), yellow(3), orange(2), var_rare_k(2) × n_var_rare.
 *   Ship clicks are FREE.  Extra Chance: 4th blue is free while ships_hit < 5.
 *   SP values: spT=20 spG=35 spY=55 spO=90 spL=76 spD=104 spR=150 spW=500 spB=10
 *   Rare-color weights (without-replacement draw):
 *     spL: 0.7143  spD: 0.4052  spR: 0.1332  spW: 0.0508
 *
 * Detailed-color index convention (matches evaluate_trace_strategies_v8.cpp):
 *   0 = blue (spB)
 *   1 = teal (spT)
 *   2 = green (spG)
 *   3 = yellow (spY)
 *   4 = orange (spO)
 *   5+k = var-rare slot k  (k = 0..n_var_rare-1)
 */

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "common/board_io.h"
#include "common/progress.h"
#include "common/stats.h"
#include "common/strategy_bridge.h"
#include "common/types.h"

#ifndef REPO_ROOT
#define REPO_ROOT "."
#endif

using namespace sphere;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr int    OT_BASE_CLICKS  = 4;
static constexpr int    OT_BLUE_VALUE   = 10;
static constexpr int    SHIPS_THRESHOLD = 5;   // Extra Chance threshold

static constexpr int    N_VAR_COLORS    = 4;
static constexpr double VAR_WEIGHT[N_VAR_COLORS] = {0.7143, 0.4052, 0.1332, 0.0508};
static const char*      VAR_COLOR_NAME[N_VAR_COLORS] = {"spL", "spD", "spR", "spW"};
static constexpr int    VAR_SP[N_VAR_COLORS]         = {76, 104, 150, 500};

// Fixed-ship SP by detailed-color index (0=blue handled separately)
static constexpr int FIXED_SP[5] = {OT_BLUE_VALUE, 20, 35, 55, 90};  // spB,spT,spG,spY,spO

static constexpr double W_TOTAL =
    VAR_WEIGHT[0] + VAR_WEIGHT[1] + VAR_WEIGHT[2] + VAR_WEIGHT[3];

// MAX_DC: maximum number of detailed-color buckets (blue + 4 fixed ships + 4 var slots)
static constexpr int MAX_DC = 9;

// ---------------------------------------------------------------------------
// FlatBoardSet — boards stored as flat row-major int32 array for fast access
// ---------------------------------------------------------------------------

struct FlatBoardSet {
    std::vector<int32_t> data;   // n_boards × fields int32s, row-major
    int n_boards = 0;
    int fields   = 0;            // = 3 + n_rare  (teal,green,yellow,spO,var_rare...)
    int n_var    = 0;            // = n_rare - 1  (number of var-rare slots)

    int32_t get(int board, int col) const {
        return data[board * fields + col];
    }
};

// Build FlatBoardSet from vector<OTBoard> (loaded by board_io.h)
static FlatBoardSet build_flat(const std::vector<OTBoard>& boards, int n_rare) {
    FlatBoardSet fbs;
    fbs.n_boards = (int)boards.size();
    fbs.fields   = 3 + n_rare;   // teal + green + yellow + spO + var_rare...
    fbs.n_var    = n_rare - 1;
    fbs.data.resize((size_t)fbs.n_boards * fbs.fields);

    for (int i = 0; i < fbs.n_boards; ++i) {
        int32_t* row = fbs.data.data() + i * fbs.fields;
        row[0] = boards[i].teal;
        row[1] = boards[i].green;
        row[2] = boards[i].yellow;
        row[3] = boards[i].spo;
        for (int k = 0; k < fbs.n_var; ++k)
            row[4 + k] = boards[i].var_rare[k];
    }
    return fbs;
}

// Detailed color for cell on board bi:
//   0=blue, 1=teal, 2=green, 3=yellow, 4=spO, 5+k=var-rare slot k
static inline int cell_dc(const FlatBoardSet& fbs, int bi, int cell) {
    int32_t bit = (int32_t)(1 << cell);
    const int32_t* row = fbs.data.data() + bi * fbs.fields;
    if (row[0] & bit) return 1;  // teal
    if (row[1] & bit) return 2;  // green
    if (row[2] & bit) return 3;  // yellow
    if (row[3] & bit) return 4;  // spO
    for (int k = 0; k < fbs.n_var; ++k)
        if (row[4 + k] & bit) return 5 + k;
    return 0;  // blue
}

// ---------------------------------------------------------------------------
// ColorAssign — tracks which var-rare slots have been identity-resolved
// ---------------------------------------------------------------------------

struct ColorAssign {
    int8_t  color[4];    // VAR color index (0-3) or -1 if unassigned
    uint8_t used_mask;   // bitmask of assigned VAR color indices
    int     n_var;

    explicit ColorAssign(int nv) : used_mask(0), n_var(nv) {
        color[0] = color[1] = color[2] = color[3] = -1;
    }

    bool is_assigned(int slot) const { return color[slot] >= 0; }

    // Return a new ColorAssign with slot assigned to var_color_idx c
    ColorAssign with_assign(int slot, int c) const {
        ColorAssign ca2 = *this;
        ca2.color[slot] = (int8_t)c;
        ca2.used_mask   = (uint8_t)(used_mask | (1u << c));
        return ca2;
    }

    double remaining_weight() const {
        double w = 0.0;
        for (int c = 0; c < N_VAR_COLORS; ++c)
            if (!(used_mask & (1u << c))) w += VAR_WEIGHT[c];
        return w;
    }
};

// ---------------------------------------------------------------------------
// NodeResult — accumulated stats for a subtree
//
// All fields are probability-weighted sums (not averages).  At the root,
// divide by n_boards to get the population mean.
//
// ev_sp and ev_sp2 let us compute stdev = sqrt(E[sp²] - E[sp]²) in one pass.
// ---------------------------------------------------------------------------

struct NodeResult {
    double ev_sp      = 0.0;   // E[score]
    double ev_sp2     = 0.0;   // E[score²]   — for stdev
    double ship_frac  = 0.0;   // E[ship_cells_revealed / total_ship_cells]
    double perf_prob  = 0.0;   // P(all ship cells revealed)
    double avg_clicks = 0.0;   // E[total click count]
    double loss_5050  = 0.0;   // P(game-ending blue had 0.25 < P(blue) < 0.75)
    double all_ships  = 0.0;   // P(all distinct ships hit)
};

// Accumulate: result += weight * (NodeResult r, plus sp_delta added to score)
// sp_delta is the score earned at THIS node; r is the subtree result.
// Because E[(sp_delta + X)²] = sp_delta² + 2*sp_delta*E[X] + E[X²]:
static inline void accumulate(NodeResult& result,
                               double weight,
                               const NodeResult& r,
                               double sp_delta)
{
    double ev_child = r.ev_sp + sp_delta;
    result.ev_sp      += weight * ev_child;
    result.ev_sp2     += weight * (r.ev_sp2 + 2.0 * sp_delta * r.ev_sp
                                   + sp_delta * sp_delta);
    result.ship_frac  += weight * r.ship_frac;
    result.perf_prob  += weight * r.perf_prob;
    result.avg_clicks += weight * r.avg_clicks;
    result.loss_5050  += weight * r.loss_5050;
    result.all_ships  += weight * r.all_ships;
}

// ---------------------------------------------------------------------------
// Build board_vec and meta_json for a strategy call at a given tree node
// ---------------------------------------------------------------------------

static void build_board_vec(const bool      revealed[N_CELLS],
                             const uint8_t   revealed_dc[N_CELLS],
                             const ColorAssign& ca,
                             std::vector<Cell>& out)
{
    out.resize(N_CELLS);
    for (int i = 0; i < N_CELLS; ++i) {
        out[i].row     = static_cast<int8_t>(i / 5);
        out[i].col     = static_cast<int8_t>(i % 5);
        out[i].clicked = revealed[i];
        if (!revealed[i]) {
            out[i].color = "spU";
        } else {
            int dc = revealed_dc[i];
            switch (dc) {
                case 0: out[i].color = "spB"; break;
                case 1: out[i].color = "spT"; break;
                case 2: out[i].color = "spG"; break;
                case 3: out[i].color = "spY"; break;
                case 4: out[i].color = "spO"; break;
                default: {
                    int slot = dc - 5;
                    if (slot >= 0 && slot < ca.n_var && ca.is_assigned(slot))
                        out[i].color = VAR_COLOR_NAME[(int)ca.color[slot]];
                    else
                        out[i].color = "spL";  // placeholder for unresolved var slot
                    break;
                }
            }
        }
    }
}

static std::string build_meta_json(int n_colors, int ships_hit, int blues_used) {
    return "{\"n_colors\":"  + std::to_string(n_colors)
         + ",\"ships_hit\":" + std::to_string(ships_hit)
         + ",\"blues_used\":" + std::to_string(blues_used)
         + ",\"max_clicks\":" + std::to_string(OT_BASE_CLICKS) + "}";
}

// ---------------------------------------------------------------------------
// Per-ship hit tracking for all_ships_rate
//   n_ships = 4 + n_var_rare
//   ship_hit_mask: bit s set if ship s has been hit (at least one cell revealed)
//   Ship index: 0=teal,1=green,2=yellow,3=spO,4+=var_rare_k
// ---------------------------------------------------------------------------

static inline uint32_t ship_index_mask_for_cell(const FlatBoardSet& fbs, int bi, int cell) {
    int dc = cell_dc(fbs, bi, cell);
    if (dc == 0) return 0;  // blue
    return 1u << (dc - 1);  // teal→0, green→1, yellow→2, spO→3, var_k→4+k
}

// ---------------------------------------------------------------------------
// tree_walk — recursive exact EV computation
//
// board_indices: subset of boards consistent with the click history so far.
// revealed[]:    which cells have been clicked (all boards share this history).
// revealed_dc[]: detailed color at each revealed cell (relative to ColorAssign ca).
// ships_hit:     number of ship cells revealed so far.
// blues_used:    number of blue clicks spent so far (Extra Chance not counted).
// game_over:     true if the game has ended before this node.
// ca:            current var-rare color assignment state.
// total_ship_cells: total ship cells on this board set.
// ships_revealed:   ship cells revealed so far (for perfect_rate).
// ship_hit_mask:    which distinct ships have been hit (for all_ships_rate).
// click_count:   total clicks so far (for avg_clicks).
// score_so_far:  accumulated score SP so far (for ev_sp2 propagation).
// bridge:        strategy instance.
// n_colors:      6/7/8/9.
// ---------------------------------------------------------------------------

static NodeResult tree_walk(
    const FlatBoardSet&    fbs,
    const std::vector<int>& board_indices,
    const bool             revealed[N_CELLS],
    const uint8_t          revealed_dc[N_CELLS],
    int                    ships_hit,
    int                    blues_used,
    bool                   game_over,
    ColorAssign            ca,
    int                    total_ship_cells,
    int                    ships_revealed,
    uint32_t               ship_hit_mask,
    int                    click_count,
    double                 score_so_far,
    StrategyBridge&        bridge,
    int                    n_colors)
{
    int n_boards = (int)board_indices.size();
    if (n_boards == 0) return {};

    // Terminal: game over or all cells revealed
    if (game_over) {
        int n_ships_total = 4 + fbs.n_var;
        uint32_t all_ships_mask = (1u << n_ships_total) - 1u;
        double perf = (ships_revealed == total_ship_cells) ? 1.0 : 0.0;
        double sfrac = (total_ship_cells > 0)
            ? (double)ships_revealed / total_ship_cells : 1.0;
        double all_s = ((ship_hit_mask & all_ships_mask) == all_ships_mask) ? 1.0 : 0.0;
        return {
            .ev_sp      = 0.0,
            .ev_sp2     = 0.0,
            .ship_frac  = sfrac,
            .perf_prob  = perf,
            .avg_clicks = (double)click_count,
            .loss_5050  = 0.0,
            .all_ships  = all_s,
        };
    }

    // Build unclicked list
    std::vector<int> unclicked;
    unclicked.reserve(N_CELLS);
    for (int i = 0; i < N_CELLS; ++i)
        if (!revealed[i]) unclicked.push_back(i);

    if (unclicked.empty()) {
        // All cells clicked — treat as terminal
        int n_ships_total = 4 + fbs.n_var;
        uint32_t all_ships_mask = (1u << n_ships_total) - 1u;
        double perf  = (ships_revealed == total_ship_cells) ? 1.0 : 0.0;
        double sfrac = (total_ship_cells > 0)
            ? (double)ships_revealed / total_ship_cells : 1.0;
        double all_s = ((ship_hit_mask & all_ships_mask) == all_ships_mask) ? 1.0 : 0.0;
        return {
            .ev_sp      = 0.0,
            .ev_sp2     = 0.0,
            .ship_frac  = sfrac,
            .perf_prob  = perf,
            .avg_clicks = (double)click_count,
            .loss_5050  = 0.0,
            .all_ships  = all_s,
        };
    }

    // Ask the strategy which cell to click
    std::vector<Cell> board_vec;
    build_board_vec(revealed, revealed_dc, ca, board_vec);
    std::string meta = build_meta_json(n_colors, ships_hit, blues_used);

    // init_game_payload is NOT called between boards in the tree walk —
    // stateless strategies ignore game_state so this is correct.
    Click c = bridge.next_click(board_vec, meta);

    int cell = rc_to_idx(c.row, c.col);
    // Validate: must be a valid unclicked cell
    if (cell < 0 || cell >= N_CELLS || revealed[cell]) {
        // Fall back to first unclicked cell
        cell = unclicked[0];
    }

    // Partition boards by detailed color at this cell
    std::vector<int> by_color[MAX_DC];
    for (int bi : board_indices)
        by_color[cell_dc(fbs, bi, cell)].push_back(bi);

    NodeResult result{};

    for (int color = 0; color < MAX_DC; ++color) {
        if (by_color[color].empty()) continue;

        double p_color = (double)by_color[color].size() / n_boards;
        bool   is_ship = (color != 0);

        // Update revealed state for child
        bool    rev2[N_CELLS];
        uint8_t rev_dc2[N_CELLS];
        memcpy(rev2,    revealed,    sizeof(rev2));
        memcpy(rev_dc2, revealed_dc, sizeof(rev_dc2));
        rev2[cell]    = true;
        rev_dc2[cell] = (uint8_t)color;

        int new_ships_hit    = ships_hit    + (is_ship ? 1 : 0);
        int new_ships_rev    = ships_revealed + (is_ship ? 1 : 0);
        int new_click_count  = click_count  + 1;

        // Ship-hit mask update
        uint32_t new_ship_hit_mask = ship_hit_mask;
        if (is_ship) {
            // dc 1..4 → ship indices 0..3; dc 5+k → ship index 4+k
            int ship_idx = color - 1;  // 1→0, 2→1, ... valid for color 1..MAX_DC-1
            new_ship_hit_mask |= (1u << ship_idx);
        }

        if (color == 0) {
            // ---- Blue click ----
            // Extra Chance: if blues_used==3 and ships_hit < SHIPS_THRESHOLD,
            // this blue is free (blues_used stays at 3).
            int new_blues_used;
            bool new_game_over;
            if (blues_used == 3 && ships_hit < SHIPS_THRESHOLD) {
                // Free blue under Extra Chance
                new_blues_used  = 3;
                new_game_over   = false;
            } else {
                new_blues_used  = blues_used + 1;
                new_game_over   = (new_blues_used >= OT_BASE_CLICKS &&
                                   new_ships_hit  >= SHIPS_THRESHOLD);
            }

            // loss_5050 diagnostic: at game-ending blue clicks,
            // check whether the pre-click P(blue) was near 50/50.
            // P(blue for this cell) = |by_color[0]| / n_boards (pre-click partition).
            double this_loss_5050 = 0.0;
            if (new_game_over && ships_revealed < total_ship_cells) {
                double p_blue = (double)by_color[0].size() / n_boards;
                if (p_blue > 0.25 && p_blue < 0.75)
                    this_loss_5050 = 1.0;
            }

            double sp_delta = OT_BLUE_VALUE;
            NodeResult r = tree_walk(
                fbs, by_color[color], rev2, rev_dc2,
                new_ships_hit, new_blues_used, new_game_over, ca,
                total_ship_cells, new_ships_rev, new_ship_hit_mask,
                new_click_count, score_so_far + sp_delta, bridge, n_colors);
            // Inject loss_5050 at this node
            r.loss_5050 += this_loss_5050;
            accumulate(result, p_color, r, sp_delta);

        } else if (color <= 4) {
            // ---- Fixed-ship click (teal/green/yellow/spO) ----
            double sp_delta = FIXED_SP[color];
            NodeResult r = tree_walk(
                fbs, by_color[color], rev2, rev_dc2,
                new_ships_hit, blues_used, false, ca,
                total_ship_cells, new_ships_rev, new_ship_hit_mask,
                new_click_count, score_so_far + sp_delta, bridge, n_colors);
            accumulate(result, p_color, r, sp_delta);

        } else {
            // ---- Var-rare slot click ----
            int slot = color - 5;
            if (ca.is_assigned(slot)) {
                // Identity already known
                int var_c = (int)ca.color[slot];
                double sp_delta = VAR_SP[var_c];
                NodeResult r = tree_walk(
                    fbs, by_color[color], rev2, rev_dc2,
                    new_ships_hit, blues_used, false, ca,
                    total_ship_cells, new_ships_rev, new_ship_hit_mask,
                    new_click_count, score_so_far + sp_delta, bridge, n_colors);
                accumulate(result, p_color, r, sp_delta);
            } else {
                // Branch over all possible var-rare identities (without-replacement draw)
                double rem_w = ca.remaining_weight();
                NodeResult var_result{};
                for (int vc = 0; vc < N_VAR_COLORS; ++vc) {
                    if (ca.used_mask & (1u << vc)) continue;
                    double wc        = VAR_WEIGHT[vc] / rem_w;
                    double sp_delta  = VAR_SP[vc];
                    ColorAssign ca2  = ca.with_assign(slot, vc);
                    // When we assign this identity, update the revealed_dc for this
                    // cell so the strategy sees the actual color string.
                    uint8_t rev_dc3[N_CELLS];
                    memcpy(rev_dc3, rev_dc2, sizeof(rev_dc3));
                    // rev_dc3[cell] stays as color (5+slot), which build_board_vec
                    // will resolve via ca2.  Already set above.
                    NodeResult r = tree_walk(
                        fbs, by_color[color], rev2, rev_dc3,
                        new_ships_hit, blues_used, false, ca2,
                        total_ship_cells, new_ships_rev, new_ship_hit_mask,
                        new_click_count, score_so_far + sp_delta, bridge, n_colors);
                    // Accumulate into var_result with weight wc
                    double ev_child = r.ev_sp + sp_delta;
                    var_result.ev_sp      += wc * ev_child;
                    var_result.ev_sp2     += wc * (r.ev_sp2
                                                   + 2.0 * sp_delta * r.ev_sp
                                                   + sp_delta * sp_delta);
                    var_result.ship_frac  += wc * r.ship_frac;
                    var_result.perf_prob  += wc * r.perf_prob;
                    var_result.avg_clicks += wc * r.avg_clicks;
                    var_result.loss_5050  += wc * r.loss_5050;
                    var_result.all_ships  += wc * r.all_ships;
                }
                // Add var_result to main result with p_color weight (already folded sp_delta in)
                result.ev_sp      += p_color * var_result.ev_sp;
                result.ev_sp2     += p_color * var_result.ev_sp2;
                result.ship_frac  += p_color * var_result.ship_frac;
                result.perf_prob  += p_color * var_result.perf_prob;
                result.avg_clicks += p_color * var_result.avg_clicks;
                result.loss_5050  += p_color * var_result.loss_5050;
                result.all_ships  += p_color * var_result.all_ships;
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Evaluate one n_colors variant via tree walk
// ---------------------------------------------------------------------------

static OTVariantResult evaluate_variant_treewalk(
    const FlatBoardSet&    fbs,
    int                    n_colors,
    const std::string&     strategy_path,
    int                    n_threads)
{
    int total_ship_cells = 4 + 3 + 3 + 2 + fbs.n_var * 2;

    printf("  n_colors=%d: %d boards  (treewalk, threads=%d)\n",
           n_colors, fbs.n_boards, n_threads);
    fflush(stdout);

    // -----------------------------------------------------------------------
    // EV + stdev pass — single tree walk over ALL boards simultaneously.
    // Computes E[score] and E[score²] in one pass; stdev = sqrt(E[sp²]-E[sp]²).
    // -----------------------------------------------------------------------

    auto t0 = std::chrono::steady_clock::now();

    // One bridge for the main EV tree walk
    auto ev_bridge = StrategyBridge::load(strategy_path, "ot");
    ev_bridge->init_evaluation_run();

    // Prepare full board index list
    std::vector<int> all_indices(fbs.n_boards);
    for (int i = 0; i < fbs.n_boards; ++i) all_indices[i] = i;

    bool    revealed[N_CELLS]    = {};
    uint8_t revealed_dc[N_CELLS] = {};
    ColorAssign ca(fbs.n_var);

    // The EV tree walk is sequential (single bridge, shared state).
    // It naturally accumulates per-board-weighted E[sp] and E[sp²].
    NodeResult root = tree_walk(
        fbs, all_indices, revealed, revealed_dc,
        0, 0, false, ca,
        total_ship_cells, 0, 0,
        0, 0.0,
        *ev_bridge, n_colors);

    double elapsed_ev = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    // root.ev_sp  = sum over all boards of board_ev / n_boards  (already normalised
    //               because the tree walk weights by 1/n_boards at each split)
    // Actually: the tree walk does NOT divide by n_boards — it works in absolute
    // probability-weighted sums.  At the root, each board contributes 1/n_boards
    // to the partition weight.  We divide by n_boards here.
    double mean_sp     = root.ev_sp     / fbs.n_boards;
    double mean_sp2    = root.ev_sp2    / fbs.n_boards;
    double ship_frac   = root.ship_frac / fbs.n_boards;
    double perf_prob   = root.perf_prob / fbs.n_boards;
    double avg_clicks  = root.avg_clicks / fbs.n_boards;
    double loss_5050   = root.loss_5050 / fbs.n_boards;
    double all_ships   = root.all_ships / fbs.n_boards;

    double variance    = mean_sp2 - mean_sp * mean_sp;
    double stdev_ev    = (variance > 0.0) ? std::sqrt(variance) : 0.0;

    printf("  n_colors=%d: EV pass done in %.1fs  mean_sp=%.4f  stdev=%.4f\n",
           n_colors, elapsed_ev, mean_sp, stdev_ev);
    fflush(stdout);

    // avg_clicks and ship stats don't have a meaningful stdev in the treewalk
    // (we only track E[clicks], not Var[clicks]).  Report 0.0 for stdev_clicks
    // and stdev_ship_clicks — these fields exist in OTVariantResult for
    // compatibility with the sequential evaluator but are not computed here.

    OTVariantResult r;
    r.n_colors          = n_colors;
    r.n_boards          = (uint64_t)fbs.n_boards;
    r.ev                = mean_sp;
    r.stdev_ev          = stdev_ev;
    r.avg_clicks        = avg_clicks;
    r.stdev_clicks      = 0.0;   // not computed by treewalk
    r.avg_ship_clicks   = ship_frac * total_ship_cells;  // approx: E[ship_cells_revealed]
    r.stdev_ship_clicks = 0.0;   // not computed by treewalk
    r.perfect_rate      = perf_prob;
    r.all_ships_rate    = all_ships;
    r.loss_5050_rate    = loss_5050;
    return r;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IOLBF, 0);

    std::string strategy_path;
    std::string boards_dir  = std::string(REPO_ROOT) + "/boards";
    std::string n_colors_arg = "all";
    int n_threads = 1;
#ifdef _OPENMP
    n_threads = omp_get_max_threads();
#endif

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--strategy")  && i+1 < argc) strategy_path = argv[++i];
        else if (!strcmp(argv[i], "--boards-dir") && i+1 < argc) boards_dir   = argv[++i];
        else if (!strcmp(argv[i], "--n-colors")   && i+1 < argc) n_colors_arg = argv[++i];
        else if (!strcmp(argv[i], "--threads")    && i+1 < argc) n_threads    = atoi(argv[++i]);
    }

    if (strategy_path.empty()) {
        fprintf(stderr,
            "Usage: evaluate_ot_treewalk --strategy <path> [--boards-dir <dir>]\n"
            "                            [--n-colors 6|7|8|9|all] [--threads N]\n");
        return 1;
    }

    std::vector<int> variants;
    if (n_colors_arg == "all") variants = {6, 7, 8, 9};
    else                       variants = {atoi(n_colors_arg.c_str())};

    printf("evaluate_ot_treewalk  strategy=%s  threads=%d\n",
           strategy_path.c_str(), n_threads);
    fflush(stdout);

    // Verify the strategy loads
    printf("Loading strategy %s ...\n", strategy_path.c_str());
    fflush(stdout);
    {
        std::unique_ptr<StrategyBridge> probe;
        try {
            probe = StrategyBridge::load(strategy_path, "ot");
        } catch (const std::exception& e) {
            fprintf(stderr, "ERROR: %s\n", e.what());
            return 1;
        }
    }

    // Release Python GIL for the evaluation passes (same pattern as evaluate_ot.cpp)
    PyThreadState* _tstate = Py_IsInitialized() ? PyEval_SaveThread() : nullptr;

    OTResult overall_result;
    double   total_boards_weighted = 0.0;
    double   weighted_ev           = 0.0;

    auto run_start = std::chrono::steady_clock::now();

    for (int nc : variants) {
        int n_rare = nc - 4;
        std::string board_path = boards_dir + "/ot_boards_" + std::to_string(n_rare) + ".bin.lzma";
        printf("Loading %s ...\n", board_path.c_str());
        fflush(stdout);
        auto boards = load_ot_boards(board_path, n_rare);
        if (boards.empty()) {
            fprintf(stderr, "WARNING: could not load boards for n_colors=%d, skipping.\n", nc);
            continue;
        }
        printf("Loaded %zu boards for n_colors=%d\n", boards.size(), nc);
        fflush(stdout);

        FlatBoardSet fbs = build_flat(boards, n_rare);
        boards.clear();  // free OTBoard memory — FlatBoardSet owns the data now

        OTVariantResult vr = evaluate_variant_treewalk(fbs, nc, strategy_path, n_threads);
        int vi = nc - 6;
        overall_result.variants[vi] = vr;

        weighted_ev           += vr.ev * (double)vr.n_boards;
        total_boards_weighted += (double)vr.n_boards;
    }

    {
        double run_secs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - run_start).count() / 1000.0;
        printf("  [done] total elapsed=%.1fs\n", run_secs);
        fflush(stdout);
    }

    if (_tstate) PyEval_RestoreThread(_tstate);

    if (total_boards_weighted > 0.0)
        overall_result.aggregate_ev = weighted_ev / total_boards_weighted;

    // Emit RESULT_JSON — same format as evaluate_ot, plus "evaluator":"treewalk"
    printf("\nRESULT_JSON: {\"game\":\"ot\",\"strategy\":\"%s\","
           "\"aggregate_ev\":%.4f,\"evaluator\":\"treewalk\",\"variants\":[",
           strategy_path.c_str(), overall_result.aggregate_ev);
    bool first = true;
    for (int nc : variants) {
        if (!first) printf(",");
        first = false;
        const OTVariantResult& vr = overall_result.variants[nc - 6];
        if (vr.n_boards == 0) continue;
        printf("{\"n_colors\":%d,\"n_boards\":%llu,"
               "\"ev\":%.4f,\"stdev_ev\":%.4f,"
               "\"avg_clicks\":%.4f,\"stdev_clicks\":%.4f,"
               "\"avg_ship_clicks\":%.4f,\"stdev_ship_clicks\":%.4f,"
               "\"perfect_rate\":%.4f,"
               "\"all_ships_rate\":%.4f,\"loss_5050_rate\":%.4f}",
               vr.n_colors, (unsigned long long)vr.n_boards,
               vr.ev, vr.stdev_ev,
               vr.avg_clicks, vr.stdev_clicks,
               vr.avg_ship_clicks, vr.stdev_ship_clicks,
               vr.perfect_rate,
               vr.all_ships_rate, vr.loss_5050_rate);
    }
    printf("]}\n");
    fflush(stdout);
    return 0;
}
