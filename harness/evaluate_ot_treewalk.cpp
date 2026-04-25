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
 * Stdev is computed in the same single pass by tracking E[score²] alongside
 * E[score]; stdev = sqrt(E[score²] - E[score]²).
 *
 * Requirements:
 *   - Strategy must be stateless: next_click(board, meta) is a pure function
 *     of its arguments (same revealed pattern + meta → same cell every time).
 *   - Opt-in: strategy source file must contain the string "sphere:stateless"
 *     within its first 50 lines.  evaluate.py enforces this before routing here.
 *
 * Parallelism (--threads N):
 *   The board set is split into N equal chunks.  Each of N pre-allocated worker
 *   threads owns one StrategyBridge and runs a full independent tree walk over
 *   its board chunk.  Workers never communicate during the walk — they produce
 *   independent NodeResult values that are combined at the end via a
 *   probability-weighted sum.
 *
 *   Correctness: because the strategy is stateless, every thread makes identical
 *   cell choices at every tree node (same revealed[] + meta → same cell).  Only
 *   the branch probabilities (p_color = |branch in chunk| / |chunk|) differ
 *   per thread, and the weighted combination E[X] = Σ (chunk_i/total) * E_i[X]
 *   recovers the exact population mean.  Chunking does not alter the strategy's
 *   view of the game — it only sees (revealed[], meta), never the board counts.
 *
 * Progress reporting:
 *   Each thread accumulates a "terminal weight" counter: whenever a terminal
 *   node is reached, it adds (leaf_boards / total_boards) to its per-thread
 *   slot.  A background thread sums the slots every 10 s and prints elapsed
 *   time + percentage complete.  The per-thread slots are cache-line padded to
 *   avoid false sharing.  No atomic operations in the hot path.
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
 * Detailed-color index convention:
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
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
static constexpr int    SHIPS_THRESHOLD = 5;

static constexpr int    N_VAR_COLORS    = 4;
static constexpr double VAR_WEIGHT[N_VAR_COLORS] = {0.7143, 0.4052, 0.1332, 0.0508};
static const char*      VAR_COLOR_NAME[N_VAR_COLORS] = {"spL", "spD", "spR", "spW"};
static constexpr int    VAR_SP[N_VAR_COLORS]         = {76, 104, 150, 500};

// Fixed-ship SP by detailed-color index (0=blue handled separately)
static constexpr int FIXED_SP[5] = {OT_BLUE_VALUE, 20, 35, 55, 90};

static constexpr double W_TOTAL =
    VAR_WEIGHT[0] + VAR_WEIGHT[1] + VAR_WEIGHT[2] + VAR_WEIGHT[3];

// MAX_DC: maximum number of detailed-color buckets
static constexpr int MAX_DC = 9;  // blue + 4 fixed ships + 4 var slots

// Maximum supported thread count (for the progress slot array)
static constexpr int MAX_THREADS = 256;

// ---------------------------------------------------------------------------
// FlatBoardSet
// ---------------------------------------------------------------------------

struct FlatBoardSet {
    std::vector<int32_t> data;
    int n_boards = 0;
    int fields   = 0;   // 3 + n_rare  (teal, green, yellow, spO, var_rare...)
    int n_var    = 0;   // n_rare - 1

    int32_t get(int board, int col) const {
        return data[board * fields + col];
    }
};

static FlatBoardSet build_flat(const std::vector<OTBoard>& boards, int n_rare) {
    FlatBoardSet fbs;
    fbs.n_boards = (int)boards.size();
    fbs.fields   = 3 + n_rare;
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
    if (row[0] & bit) return 1;
    if (row[1] & bit) return 2;
    if (row[2] & bit) return 3;
    if (row[3] & bit) return 4;
    for (int k = 0; k < fbs.n_var; ++k)
        if (row[4 + k] & bit) return 5 + k;
    return 0;
}

// ---------------------------------------------------------------------------
// ColorAssign
// ---------------------------------------------------------------------------

struct ColorAssign {
    int8_t  color[4];
    uint8_t used_mask;
    int     n_var;

    ColorAssign() : used_mask(0), n_var(0) {
        color[0] = color[1] = color[2] = color[3] = -1;
    }
    explicit ColorAssign(int nv) : used_mask(0), n_var(nv) {
        color[0] = color[1] = color[2] = color[3] = -1;
    }

    bool is_assigned(int slot) const { return color[slot] >= 0; }

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
// NodeResult
//
// All fields are probability-weighted means (not sums).  The tree walk
// weights each branch by p_color = |branch| / |parent|, so the root result
// is already the population mean — no division by n_boards needed afterwards.
// ---------------------------------------------------------------------------

struct NodeResult {
    double ev_sp      = 0.0;   // E[score]
    double ev_sp2     = 0.0;   // E[score²]  — for stdev
    double ship_frac  = 0.0;   // E[ship_cells_revealed / total_ship_cells]
    double perf_prob  = 0.0;   // P(all ship cells revealed)
    double avg_clicks = 0.0;   // E[total click count]
    double loss_5050  = 0.0;   // P(game-ending blue had 0.25 < P(blue) < 0.75)
    double all_ships  = 0.0;   // P(all distinct ships hit)
};

// accumulate: result += weight * subtree(r) with sp_delta earned at this node.
// E[(sp_delta + X)²] = sp_delta² + 2*sp_delta*E[X] + E[X²]
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
// Per-thread progress slot — cache-line padded to prevent false sharing
// ---------------------------------------------------------------------------

struct alignas(64) ProgressSlot {
    double weight_done = 0.0;   // sum of (leaf_boards / total_boards) at terminals
};

// ---------------------------------------------------------------------------
// Build board_vec and meta_json for a strategy call
// ---------------------------------------------------------------------------

static void build_board_vec(const bool         revealed[N_CELLS],
                             const uint8_t      revealed_dc[N_CELLS],
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
    return "{\"n_colors\":"   + std::to_string(n_colors)
         + ",\"ships_hit\":"  + std::to_string(ships_hit)
         + ",\"blues_used\":" + std::to_string(blues_used)
         + ",\"max_clicks\":" + std::to_string(OT_BASE_CLICKS) + "}";
}

// ---------------------------------------------------------------------------
// tree_walk — recursive exact EV computation over a board subset
//
// board_indices: subset of boards consistent with the click history.
// revealed[]:    cells clicked so far (shared click history for this subtree).
// revealed_dc[]: detailed color at each revealed cell.
// ships_hit:     ship cells revealed so far.
// blues_used:    blue clicks spent so far (Extra Chance not counted).
// game_over:     true if the game ended before reaching this node.
// ca:            current var-rare color assignment state.
// total_boards:  size of the root board set for this thread (used for progress).
// total_ship_cells: total ship cells on this board set.
// ships_revealed:   ship cells revealed so far.
// ship_hit_mask:    bitmask of distinct ships hit so far.
// click_count:      total clicks so far.
// bridge:           strategy instance (one per thread, never shared).
// n_colors:         6/7/8/9.
// progress:         per-thread progress slot; incremented at terminal nodes.
// ---------------------------------------------------------------------------

static NodeResult tree_walk(
    const FlatBoardSet&     fbs,
    const std::vector<int>& board_indices,
    const bool              revealed[N_CELLS],
    const uint8_t           revealed_dc[N_CELLS],
    int                     ships_hit,
    int                     blues_used,
    bool                    game_over,
    ColorAssign             ca,
    int                     total_boards,
    int                     total_ship_cells,
    int                     ships_revealed,
    uint32_t                ship_hit_mask,
    int                     click_count,
    StrategyBridge&         bridge,
    int                     n_colors,
    ProgressSlot&           progress)
{
    int n_boards = (int)board_indices.size();
    if (n_boards == 0) return {};

    int n_ships_total    = 4 + fbs.n_var;
    uint32_t all_ships_mask = (1u << n_ships_total) - 1u;

    // Helper: record terminal node and return result.
    // Adds (n_boards / total_boards) to the progress counter.
    auto make_terminal = [&]() -> NodeResult {
        double perf  = (ships_revealed == total_ship_cells) ? 1.0 : 0.0;
        double sfrac = (total_ship_cells > 0)
            ? (double)ships_revealed / total_ship_cells : 1.0;
        double all_s = ((ship_hit_mask & all_ships_mask) == all_ships_mask) ? 1.0 : 0.0;
        // Progress: weight of this terminal = boards in this leaf / total boards for thread
        progress.weight_done += (double)n_boards / (double)total_boards;
        return {
            .ev_sp      = 0.0,
            .ev_sp2     = 0.0,
            .ship_frac  = sfrac,
            .perf_prob  = perf,
            .avg_clicks = (double)click_count,
            .loss_5050  = 0.0,
            .all_ships  = all_s,
        };
    };

    if (game_over) return make_terminal();

    // Build unclicked list
    std::vector<int> unclicked;
    unclicked.reserve(N_CELLS);
    for (int i = 0; i < N_CELLS; ++i)
        if (!revealed[i]) unclicked.push_back(i);

    if (unclicked.empty()) return make_terminal();

    // Ask the strategy which cell to click.
    // The strategy only sees (revealed[], meta) — it has no knowledge of
    // n_boards, the chunk size, or which thread is calling it.
    std::vector<Cell> board_vec;
    build_board_vec(revealed, revealed_dc, ca, board_vec);
    std::string meta = build_meta_json(n_colors, ships_hit, blues_used);

    Click c = bridge.next_click(board_vec, meta);
    int cell = rc_to_idx(c.row, c.col);
    if (cell < 0 || cell >= N_CELLS || revealed[cell])
        cell = unclicked[0];  // fallback: first unclicked cell

    // Partition boards by detailed color at the chosen cell
    std::vector<int> by_color[MAX_DC];
    for (int bi : board_indices)
        by_color[cell_dc(fbs, bi, cell)].push_back(bi);

    NodeResult result{};

    for (int color = 0; color < MAX_DC; ++color) {
        if (by_color[color].empty()) continue;

        double p_color = (double)by_color[color].size() / n_boards;
        bool   is_ship = (color != 0);

        bool    rev2[N_CELLS];
        uint8_t rev_dc2[N_CELLS];
        memcpy(rev2,    revealed,    sizeof(rev2));
        memcpy(rev_dc2, revealed_dc, sizeof(rev_dc2));
        rev2[cell]    = true;
        rev_dc2[cell] = (uint8_t)color;

        int new_ships_hit   = ships_hit    + (is_ship ? 1 : 0);
        int new_ships_rev   = ships_revealed + (is_ship ? 1 : 0);
        int new_click_count = click_count  + 1;

        uint32_t new_ship_hit_mask = ship_hit_mask;
        if (is_ship) {
            int ship_idx = color - 1;  // teal→0, green→1, yellow→2, spO→3, var_k→4+k
            new_ship_hit_mask |= (1u << ship_idx);
        }

        if (color == 0) {
            // ---- Blue click ----
            int  new_blues_used;
            bool new_game_over;
            if (blues_used == 3 && ships_hit < SHIPS_THRESHOLD) {
                // Extra Chance: this blue is free
                new_blues_used = 3;
                new_game_over  = false;
            } else {
                new_blues_used = blues_used + 1;
                new_game_over  = (new_blues_used >= OT_BASE_CLICKS &&
                                  new_ships_hit  >= SHIPS_THRESHOLD);
            }

            // loss_5050: was this a near-50/50 game-ending blue?
            double extra_loss_5050 = 0.0;
            if (new_game_over && ships_revealed < total_ship_cells) {
                double p_blue = (double)by_color[0].size() / n_boards;
                if (p_blue > 0.25 && p_blue < 0.75)
                    extra_loss_5050 = 1.0;
            }

            NodeResult r = tree_walk(
                fbs, by_color[color], rev2, rev_dc2,
                new_ships_hit, new_blues_used, new_game_over, ca,
                total_boards, total_ship_cells, new_ships_rev, new_ship_hit_mask,
                new_click_count, bridge, n_colors, progress);
            r.loss_5050 += extra_loss_5050;
            accumulate(result, p_color, r, (double)OT_BLUE_VALUE);

        } else if (color <= 4) {
            // ---- Fixed-ship click (teal/green/yellow/spO) ----
            NodeResult r = tree_walk(
                fbs, by_color[color], rev2, rev_dc2,
                new_ships_hit, blues_used, false, ca,
                total_boards, total_ship_cells, new_ships_rev, new_ship_hit_mask,
                new_click_count, bridge, n_colors, progress);
            accumulate(result, p_color, r, (double)FIXED_SP[color]);

        } else {
            // ---- Var-rare slot click ----
            int slot = color - 5;
            if (ca.is_assigned(slot)) {
                int var_c = (int)ca.color[slot];
                NodeResult r = tree_walk(
                    fbs, by_color[color], rev2, rev_dc2,
                    new_ships_hit, blues_used, false, ca,
                    total_boards, total_ship_cells, new_ships_rev, new_ship_hit_mask,
                    new_click_count, bridge, n_colors, progress);
                accumulate(result, p_color, r, (double)VAR_SP[var_c]);
            } else {
                // Fan out over all possible var-rare identities (without-replacement draw)
                double rem_w = ca.remaining_weight();
                NodeResult var_result{};
                for (int vc = 0; vc < N_VAR_COLORS; ++vc) {
                    if (ca.used_mask & (1u << vc)) continue;
                    double wc       = VAR_WEIGHT[vc] / rem_w;
                    double sp_delta = (double)VAR_SP[vc];
                    ColorAssign ca2 = ca.with_assign(slot, vc);
                    NodeResult r = tree_walk(
                        fbs, by_color[color], rev2, rev_dc2,
                        new_ships_hit, blues_used, false, ca2,
                        total_boards, total_ship_cells, new_ships_rev, new_ship_hit_mask,
                        new_click_count, bridge, n_colors, progress);
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
// Evaluate one n_colors variant via parallel chunked tree walks
// ---------------------------------------------------------------------------

static OTVariantResult evaluate_variant_treewalk(
    const FlatBoardSet& fbs,
    int                 n_colors,
    const std::string&  strategy_path,
    int                 n_threads,
    double&             variant_elapsed_out)
{
    int total_ship_cells = 4 + 3 + 3 + 2 + fbs.n_var * 2;
    int n_boards         = fbs.n_boards;

    print_ts();
    printf("  n_colors=%d: %d boards  (treewalk, %d thread%s)\n",
           n_colors, n_boards, n_threads, n_threads == 1 ? "" : "s");
    fflush(stdout);

    auto t0 = std::chrono::steady_clock::now();

    // -----------------------------------------------------------------------
    // Build per-thread bridges and board-index chunks
    // -----------------------------------------------------------------------

    std::vector<std::unique_ptr<StrategyBridge>> bridges(n_threads);
    for (int t = 0; t < n_threads; ++t) {
        bridges[t] = StrategyBridge::load(strategy_path, "ot");
        bridges[t]->init_evaluation_run();
    }

    // Split board indices into n_threads equal chunks.
    // Chunk t gets boards [chunk_start[t], chunk_start[t+1]).
    std::vector<int> chunk_start(n_threads + 1);
    for (int t = 0; t <= n_threads; ++t)
        chunk_start[t] = (int)((int64_t)n_boards * t / n_threads);

    // -----------------------------------------------------------------------
    // Per-thread progress slots (cache-line padded, no false sharing)
    // -----------------------------------------------------------------------

    std::vector<ProgressSlot> progress(n_threads);
    // progress[t].weight_done accumulates (leaf_boards / chunk_size) during
    // the walk.  Because each chunk's tree walk weights terminals by
    // n_boards_at_leaf / chunk_size and the chunks partition the full board
    // set, summing progress[t].weight_done / n_threads gives overall fraction.
    // More precisely: overall fraction = Σ_t (chunk_size_t/n_boards) * progress[t].weight_done

    // -----------------------------------------------------------------------
    // Per-thread results
    // -----------------------------------------------------------------------

    std::vector<NodeResult> results(n_threads);

    // -----------------------------------------------------------------------
    // Background progress thread: prints every 10 s
    // -----------------------------------------------------------------------

    std::atomic<bool>       walk_done{false};
    std::mutex              progress_mutex;
    std::condition_variable progress_cv;

    // Print a progress line when elapsed >= next_time_threshold OR
    // progress >= next_pct_threshold, whichever comes first.
    // Check frequently (every 1s) so neither threshold is missed.
    std::thread progress_thread([&]() {
        using namespace std::chrono;
        auto     start              = steady_clock::now();
        double   next_time_secs     = 10.0;   // print at least every 10s
        double   next_pct_threshold = 5.0;    // print at every 5% milestone
        std::unique_lock<std::mutex> lk(progress_mutex);
        while (true) {
            // Wake every 1s to check both thresholds, or immediately on done
            progress_cv.wait_for(lk, milliseconds(1000),
                [&]{ return walk_done.load(std::memory_order_relaxed); });
            if (walk_done.load(std::memory_order_relaxed)) break;

            double elapsed = duration<double>(steady_clock::now() - start).count();
            double total_progress = 0.0;
            for (int t = 0; t < n_threads; ++t) {
                double chunk_frac = (double)(chunk_start[t+1] - chunk_start[t]) / n_boards;
                total_progress += chunk_frac * progress[t].weight_done;
            }
            total_progress = std::min(total_progress, 1.0);
            double pct = total_progress * 100.0;

            bool time_trigger = (elapsed >= next_time_secs);
            bool pct_trigger  = (pct    >= next_pct_threshold);
            if (!time_trigger && !pct_trigger) continue;
            // Suppress prints once progress has hit 100% — walk is just wrapping up
            if (pct >= 100.0 - 1e-6) continue;

            double eta = (total_progress > 1e-6 && total_progress < 1.0 - 1e-6)
                       ? elapsed / total_progress * (1.0 - total_progress) : 0.0;
            print_ts();
            printf("  n_colors=%d:  elapsed=%.0fs  %.1f%%  eta=%.0fs\n",
                   n_colors, elapsed, pct, eta);
            fflush(stdout);

            if (time_trigger) next_time_secs     = elapsed + 10.0;
            if (pct_trigger)  next_pct_threshold = std::floor(pct / 5.0) * 5.0 + 5.0;
        }
    });

    // -----------------------------------------------------------------------
    // Launch worker threads
    // -----------------------------------------------------------------------

    std::vector<std::thread> workers;
    workers.reserve(n_threads);

    for (int t = 0; t < n_threads; ++t) {
        workers.emplace_back([&, t]() {
            int start = chunk_start[t];
            int end   = chunk_start[t + 1];
            int chunk_size = end - start;
            if (chunk_size == 0) return;

            std::vector<int> indices(chunk_size);
            for (int i = 0; i < chunk_size; ++i) indices[i] = start + i;

            bool    revealed[N_CELLS]    = {};
            uint8_t revealed_dc[N_CELLS] = {};
            ColorAssign ca(fbs.n_var);

            results[t] = tree_walk(
                fbs, indices, revealed, revealed_dc,
                0, 0, false, ca,
                chunk_size,          // total_boards for this thread (for progress weights)
                total_ship_cells, 0, 0,
                0, *bridges[t], n_colors, progress[t]);
        });
    }

    for (auto& w : workers) w.join();

    // Signal progress thread to wake and exit, then join it
    {
        std::lock_guard<std::mutex> lk(progress_mutex);
        walk_done.store(true, std::memory_order_relaxed);
    }
    progress_cv.notify_all();
    progress_thread.join();

    double elapsed_ev = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    variant_elapsed_out = elapsed_ev;

    // -----------------------------------------------------------------------
    // Combine per-chunk NodeResults into the population mean.
    //
    // Each chunk t has chunk_weight = chunk_size_t / n_boards.
    // E[X] = Σ_t chunk_weight_t * E_t[X]
    //
    // ev_sp2 combination for stdev:
    //   E[X²] = Σ_t chunk_weight_t * E_t[X²]
    //   (This is correct because expectations are linear; each chunk's E_t[X²]
    //    is the conditional second moment over that chunk's board subset.)
    // -----------------------------------------------------------------------

    NodeResult combined{};
    for (int t = 0; t < n_threads; ++t) {
        int    csz    = chunk_start[t+1] - chunk_start[t];
        if (csz == 0) continue;
        double cw     = (double)csz / n_boards;
        combined.ev_sp      += cw * results[t].ev_sp;
        combined.ev_sp2     += cw * results[t].ev_sp2;
        combined.ship_frac  += cw * results[t].ship_frac;
        combined.perf_prob  += cw * results[t].perf_prob;
        combined.avg_clicks += cw * results[t].avg_clicks;
        combined.loss_5050  += cw * results[t].loss_5050;
        combined.all_ships  += cw * results[t].all_ships;
    }

    double mean_sp   = combined.ev_sp;
    double variance  = combined.ev_sp2 - mean_sp * mean_sp;
    double stdev_ev  = (variance > 0.0) ? std::sqrt(variance) : 0.0;

    print_ts();
    printf("  n_colors=%d: done in %.1fs  ev=%.4f  stdev=%.4f\n",
           n_colors, elapsed_ev, mean_sp, stdev_ev);
    fflush(stdout);

    OTVariantResult r;
    r.n_colors          = n_colors;
    r.n_boards          = (uint64_t)n_boards;
    r.ev                = mean_sp;
    r.stdev_ev          = stdev_ev;
    r.avg_clicks        = combined.avg_clicks;
    r.stdev_clicks      = 0.0;
    r.avg_ship_clicks   = combined.ship_frac * total_ship_cells;
    r.stdev_ship_clicks = 0.0;
    r.perfect_rate      = combined.perf_prob;
    r.all_ships_rate    = combined.all_ships;
    r.loss_5050_rate    = combined.loss_5050;
    return r;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IOLBF, 0);

    std::string strategy_path;
    std::string boards_dir   = std::string(REPO_ROOT) + "/boards";
    std::string n_colors_arg = "all";
    int n_threads = 1;
#ifdef _OPENMP
    n_threads = omp_get_max_threads();
#else
    {
        int hw = (int)std::thread::hardware_concurrency();
        if (hw > 0) n_threads = hw;
    }
#endif

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--strategy")   && i+1 < argc) strategy_path = argv[++i];
        else if (!strcmp(argv[i], "--boards-dir")  && i+1 < argc) boards_dir    = argv[++i];
        else if (!strcmp(argv[i], "--n-colors")    && i+1 < argc) n_colors_arg  = argv[++i];
        else if (!strcmp(argv[i], "--threads")     && i+1 < argc) n_threads     = atoi(argv[++i]);
    }

    if (strategy_path.empty()) {
        fprintf(stderr,
            "Usage: evaluate_ot_treewalk --strategy <path> [--boards-dir <dir>]\n"
            "                            [--n-colors 6|7|8|9|all] [--threads N]\n");
        return 1;
    }
    if (n_threads < 1) n_threads = 1;
    if (n_threads > MAX_THREADS) n_threads = MAX_THREADS;

    std::vector<int> variants;
    if (n_colors_arg == "all") variants = {6, 7, 8, 9};
    else                       variants = {atoi(n_colors_arg.c_str())};

    print_ts();
    printf("evaluate_ot_treewalk  strategy=%s  threads=%d\n",
           strategy_path.c_str(), n_threads);
    fflush(stdout);

    // Verify the strategy loads before committing to loading N copies of it
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

    // Release Python GIL (same pattern as evaluate_ot.cpp)
    PyThreadState* _tstate = Py_IsInitialized() ? PyEval_SaveThread() : nullptr;

    OTResult overall_result;
    double   total_boards_weighted = 0.0;
    double   weighted_ev           = 0.0;

    auto run_start = std::chrono::steady_clock::now();
    std::vector<std::pair<int,double>> variant_timings;

    for (int nc : variants) {
        int n_rare = nc - 4;
        std::string board_path = boards_dir + "/ot_boards_" + std::to_string(n_rare) + ".bin.lzma";
        print_ts();
        printf("Loading %s ...\n", board_path.c_str());
        fflush(stdout);
        auto boards = load_ot_boards(board_path, n_rare);
        if (boards.empty()) {
            fprintf(stderr, "WARNING: could not load boards for n_colors=%d, skipping.\n", nc);
            continue;
        }
        print_ts();
        printf("Loaded %zu boards for n_colors=%d\n", boards.size(), nc);
        fflush(stdout);

        FlatBoardSet fbs = build_flat(boards, n_rare);
        boards.clear();

        double variant_elapsed = 0.0;
        OTVariantResult vr = evaluate_variant_treewalk(fbs, nc, strategy_path,
                                                       n_threads, variant_elapsed);
        int vi = nc - 6;
        overall_result.variants[vi] = vr;
        variant_timings.emplace_back(nc, variant_elapsed);

        weighted_ev           += vr.ev * (double)vr.n_boards;
        total_boards_weighted += (double)vr.n_boards;
    }

    double total_run_secs = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - run_start).count();

    if (_tstate) PyEval_RestoreThread(_tstate);

    if (total_boards_weighted > 0.0)
        overall_result.aggregate_ev = weighted_ev / total_boards_weighted;

    // Timing summary
    printf("\n  Timing summary:\n");
    for (auto& [nc, secs] : variant_timings)
        printf("    n_colors=%d:  %.1fs\n", nc, secs);
    printf("    total:        %.1fs\n", total_run_secs);
    fflush(stdout);

    // RESULT_JSON — same format as evaluate_ot, plus "evaluator":"treewalk"
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
