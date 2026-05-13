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
 *   - Strategy must be deterministic: the cell chosen by next_click must be a
 *     pure function of its inputs (same revealed pattern + meta → same cell
 *     every time).  For most strategies this means a pure function of
 *     (board, meta).  C++ strategies that export strategy_next_click_sv (see
 *     below) extend this to (board, meta, full_sv).
 *   - Opt-in: strategy source file must contain the string "sphere:stateless"
 *     within its first 50 lines.  evaluate.py enforces this before routing here.
 *   - Currently C++ only for board-filter strategies.  Python and JS strategies
 *     may use sphere:stateless but do not receive full_sv and must perform their
 *     own filtering on every call (see strategy_bridge.h).
 *
 * Parallelism (--threads N):
 *   The board set is split into N equal chunks.  Each of N pre-allocated worker
 *   threads owns one StrategyBridge and runs a full independent tree walk over
 *   its board chunk.  Workers never communicate during the walk — they produce
 *   independent NodeResult values that are combined at the end via a
 *   probability-weighted sum.
 *
 *   Correctness: because the strategy is deterministic given its inputs, every
 *   thread makes identical cell choices at every tree node.  The branch
 *   probabilities (p_color = |branch in chunk| / |chunk|) differ per thread,
 *   but the weighted combination E[X] = Σ (chunk_i/total) * E_i[X] recovers
 *   the exact population mean.
 *
 *   full_sv correctness: each thread maintains its own full_sv — the set of
 *   ALL boards (across the full population, not just the thread's chunk) that
 *   are consistent with the click history at the current tree node.  At the
 *   root, full_sv = [0..n_boards-1].  At each branch, full_sv is filtered by
 *   the revealed cell color (filter_full_sv) independently of board_indices.
 *   Because the same filter is applied on every thread, all threads carry
 *   identical full_sv at any given tree node — so all threads pass the same
 *   full_sv to the strategy, and all threads make the same cell choice.
 *
 *   The strategy never sees board_indices (the chunk) — it only sees
 *   (revealed[], meta, full_sv).  Chunking affects only probability weighting,
 *   never the strategy's view of the board distribution.
 *
 * sv-aware C++ strategies (strategy_next_click_sv):
 *   If a C++ strategy exports the symbol strategy_next_click_sv, the harness
 *   calls it instead of strategy_next_click, passing:
 *     const int* sv_ptr   — pointer to full_sv.data()
 *     int        sv_len   — full_sv.size()
 *   The strategy receives the pre-filtered board index list and can skip its
 *   own filtering step entirely.  If the symbol is absent the harness falls
 *   back to strategy_next_click (full_sv is not passed).
 *   See strategies/templates/ot_treewalk_template.cpp for the two-export
 *   pattern, and strategies/ot/colblitz_v8_heuristics_stateless.cpp for a
 *   complete working implementation.
 *
 * WalkContext:
 *   Values constant for an entire variant walk (fbs, total_ship_cells,
 *   n_colors, bridge, progress) are bundled into a WalkContext struct and
 *   passed by const-ref rather than as individual parameters.  Per-node
 *   varying state (board_indices, full_sv, revealed[], ships_hit, etc.)
 *   remains as explicit parameters.
 *
 * Progress reporting:
 *   Each thread maintains two atomic counters: strategy_calls (incremented at
 *   every next_click invocation) and terminals (incremented at every terminal
 *   node).  A background thread wakes every 10 s, sums the counters across all
 *   threads, and prints elapsed time + cumulative calls + rolling calls/s.  If
 *   a prior run's total call count is known (passed via --expected-calls-N),
 *   the line also includes an estimated percentage and ETA.  The per-thread
 *   slots are cache-line padded to avoid false sharing.
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
 *   Rare-color weights (per-n_colors, without-replacement draw):
 *     6-color: spL=0.6697  spD=0.3303  spR=0.0000  spW=0.0000
 *     7-color: spL=0.8182  spD=0.6071  spR=0.4156  spW=0.1591  (>=1 of spL/spD required)
 *     8-color: spL=0.9063  spD=0.8750  spR=0.7500  spW=0.4688
 *     9-color: all four always assigned
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
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
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
// Per-n_colors appearance weights for var-rare color identities.
// Row index = n_colors - 6.  6-color: spR/spW never appear; 7-color: at least
// one of spL/spD always appears; 8-color: all four possible; 9-color: all four
// always assigned.
static constexpr double VAR_WEIGHT_BY_NC[4][N_VAR_COLORS] = {
    {0.669734, 0.330266, 0.0,      0.0     },  // 6-color (n_var_rare=1)
    {0.818182, 0.607143, 0.415584, 0.159091},  // 7-color (n_var_rare=2)
    {0.906250, 0.875000, 0.750000, 0.468750},  // 8-color (n_var_rare=3)
    {1.0,      1.0,      1.0,      1.0     },  // 9-color (n_var_rare=4, all always assigned)
};
static inline double var_weight(int n_colors, int c) {
    return VAR_WEIGHT_BY_NC[n_colors - 6][c];
}
// 7-color hard constraint: a complete assignment must contain at least one of
// spL (index 0) or spD (index 1).
static inline bool var_assignment_valid(int n_colors, uint8_t used) {
    if (n_colors == 7) return (used & 0x3u) != 0;
    return true;
}
static const char*      VAR_COLOR_NAME[N_VAR_COLORS] = {"spL", "spD", "spR", "spW"};
static constexpr int    VAR_SP[N_VAR_COLORS]         = {76, 104, 150, 500};

// Fixed-ship SP by detailed-color index (0=blue handled separately)
static constexpr int FIXED_SP[5] = {OT_BLUE_VALUE, 20, 35, 55, 90};

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

    double remaining_weight(int n_colors) const {
        double w = 0.0;
        for (int c = 0; c < N_VAR_COLORS; ++c)
            if (!(used_mask & (1u << c))) w += var_weight(n_colors, c);
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
    double ev_sp          = 0.0;   // E[score]
    double ev_sp2         = 0.0;   // E[score²]        — for stdev_ev
    double ship_frac      = 0.0;   // E[ship_cells_revealed / total_ship_cells]
    double ship_frac2     = 0.0;   // E[(ship_frac)²]  — for stdev_ship_clicks
    double perf_prob      = 0.0;   // P(all 25 cells clicked)
    double avg_clicks     = 0.0;   // E[total click count]
    double avg_clicks2    = 0.0;   // E[clicks²]       — for stdev_clicks
    double loss_5050      = 0.0;   // P(game-ending blue had 0.25 < P(blue) < 0.75)
    double all_ships      = 0.0;   // P(all ship cells revealed)
};

// ---------------------------------------------------------------------------
// Phase 2 entry stats (populated when --with-stats is passed)
//
// Recorded at the exact moment a path first crosses ships_hit == SHIPS_THRESHOLD,
// i.e. in the parent node's ship-click branch when new_ships_hit == SHIPS_THRESHOLD.
// Thread 0 records using exact full-population probabilities derived from
// full_sv sizes (not chunk-dependent p_color estimates), so stats are exact
// and independent of thread count.
// ---------------------------------------------------------------------------

struct Phase2StatsEntry {
    double prob_mass    = 0.0;  // probability weight of paths entering Phase 2 here
    double weighted_sv  = 0.0;  // prob-weighted sum of n_boards at entry
    double weighted_sv2 = 0.0;  // prob-weighted sum of n_boards² (for stdev)
};

struct Phase2Stats {
    // key = blues_used * 10 + ships_hit  (ships_hit always == SHIPS_THRESHOLD at entry)
    std::map<int, Phase2StatsEntry> by_bn;
};

static void print_phase2_stats(const Phase2Stats& stats, int n_colors, bool has_sv) {
    printf("\n=== Phase 2 entry stats: n_colors=%d ===\n", n_colors);

    // Compute overall totals
    double total_prob = 0.0, total_wsv = 0.0, total_wsv2 = 0.0;
    for (const auto& [key, e] : stats.by_bn) {
        total_prob  += e.prob_mass;
        total_wsv   += e.weighted_sv;
        total_wsv2  += e.weighted_sv2;
    }

    if (total_prob > 0.0 && has_sv) {
        double mean = total_wsv / total_prob;
        double var  = total_wsv2 / total_prob - mean * mean;
        double sd   = var > 0.0 ? std::sqrt(var) : 0.0;
        printf("  overall:  prob=%.4f  mean_boards=%.1f  stdev_boards=%.1f\n",
               total_prob, mean, sd);
    } else if (total_prob > 0.0) {
        printf("  overall:  count=%.0f\n", total_prob);
    }

    if (has_sv) {
        printf("  %-8s  %-10s  %-12s  %-12s\n", "(b, n)", "prob", "mean_boards", "stdev_boards");
    } else {
        printf("  %-8s  %-10s\n", "(b, n)", "count");
    }

    for (const auto& [key, e] : stats.by_bn) {
        if (e.prob_mass <= 0.0) continue;
        int b = key / 10;
        int n = key % 10;
        if (has_sv) {
            double mean = e.weighted_sv / e.prob_mass;
            double var  = e.weighted_sv2 / e.prob_mass - mean * mean;
            double sd   = var > 0.0 ? std::sqrt(var) : 0.0;
            printf("  (%d, %d)    %-10.4f  %-12.1f  %-12.1f\n", b, n, e.prob_mass, mean, sd);
        } else {
            printf("  (%d, %d)    %-10.0f\n", b, n, e.prob_mass);
        }
    }
    printf("==========================================\n");
    fflush(stdout);
}

// accumulate: result += weight * subtree(r) with sp_delta earned at this node.
// E[(sp_delta + X)²] = sp_delta² + 2*sp_delta*E[X] + E[X²]
// Click delta is always 1 per node: E[(1 + clicks)²] = 1 + 2*E[clicks] + E[clicks²]
// ship_frac has no per-node delta (terminal-only value): pure weighted mean of squares.
static inline void accumulate(NodeResult& result,
                               double weight,
                               const NodeResult& r,
                               double sp_delta)
{
    double ev_child = r.ev_sp + sp_delta;
    result.ev_sp       += weight * ev_child;
    result.ev_sp2      += weight * (r.ev_sp2 + 2.0 * sp_delta * r.ev_sp
                                    + sp_delta * sp_delta);
    result.ship_frac   += weight * r.ship_frac;
    result.ship_frac2  += weight * r.ship_frac2;
    result.perf_prob   += weight * r.perf_prob;
    result.avg_clicks  += weight * r.avg_clicks;
    result.avg_clicks2 += weight * (r.avg_clicks2 + 2.0 * r.avg_clicks + 1.0);
    result.loss_5050   += weight * r.loss_5050;
    result.all_ships   += weight * r.all_ships;
}

// ---------------------------------------------------------------------------
// Per-thread progress slot — cache-line padded to prevent false sharing
// ---------------------------------------------------------------------------

struct alignas(64) ProgressSlot {
    std::atomic<uint64_t> strategy_calls{0};  // incremented at every next_click call
    std::atomic<uint64_t> terminals{0};       // incremented at every terminal node
    std::atomic<uint8_t>  active{0};          // 1 while this thread's worker is running
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
// WalkContext — values that are constant for an entire tree-walk variant.
// Passed by const-ref into tree_walk to avoid repeating them in every
// recursive call.
// ---------------------------------------------------------------------------

struct WalkContext {
    const FlatBoardSet& fbs;
    int                 total_ship_cells;
    int                 n_colors;
    StrategyBridge&     bridge;
    ProgressSlot&       progress;
    bool                record_stats = false;   // true for thread 0 when --with-stats
    Phase2Stats*        p2stats      = nullptr; // non-null when record_stats
};

// ---------------------------------------------------------------------------
// filter_full_sv — apply one reveal to the full-population surviving index
// list.  Keeps only boards where cell `cell` has detailed color `dc`.
//
// This mirrors the strategy's applyRevealIdx logic but uses the integer dc
// directly (which the harness already has from the tree branch) rather than
// a color-name string, so no rareColorGroups bookkeeping is needed here.
// The harness knows the exact dc slot at each branch; "which var-rare slot"
// is unambiguous.
// ---------------------------------------------------------------------------

static void filter_full_sv(const FlatBoardSet& fbs,
                            std::vector<int>&   sv,
                            int                 cell,
                            int                 dc)
{
    if (sv.empty()) return;
    int out = 0;
    for (int i = 0, n = (int)sv.size(); i < n; ++i)
        if (cell_dc(fbs, sv[i], cell) == dc) sv[out++] = sv[i];
    sv.resize(out);
}

// ---------------------------------------------------------------------------
// tree_walk — recursive exact EV computation over a board subset.
//
// ctx:           constant walk-wide context (fbs, n_colors, bridge, progress,
//                total_ship_cells).
// board_indices: this thread's chunk of boards consistent with the click path.
//                Used only for probability weighting (p_color = branch/total).
//                Never passed to the strategy.
// full_sv:       full-population surviving boards consistent with the click
//                path (independent of chunking).  Passed to the strategy so
//                it sees the correct board distribution regardless of which
//                chunk this thread owns.
// revealed[]:    cells clicked so far.
// revealed_dc[]: detailed color at each revealed cell.
// ships_hit:     ship cells revealed so far.
// blues_used:    blue clicks spent so far (Extra Chance not counted).
// game_over:     true if the game ended before reaching this node.
// ca:            current var-rare color assignment state.
// ships_revealed:   ship cells revealed so far.
// click_count:      total clicks so far.
// ---------------------------------------------------------------------------

static NodeResult tree_walk(
    const WalkContext&      ctx,
    const std::vector<int>& board_indices,
    const std::vector<int>& full_sv,
    const bool              revealed[N_CELLS],
    const uint8_t           revealed_dc[N_CELLS],
    int                     ships_hit,
    int                     blues_used,
    bool                    game_over,
    ColorAssign             ca,
    int                     ships_revealed,
    int                     click_count,
    double                  path_weight = 1.0)  // product of p_exact along path from root
{
    int n_boards = (int)board_indices.size();
    if (n_boards == 0) return {};

    // Helper: record terminal node and return result.
    auto make_terminal = [&]() -> NodeResult {
        double perf  = (click_count == N_CELLS) ? 1.0 : 0.0;
        double sfrac = (ctx.total_ship_cells > 0)
            ? (double)ships_revealed / ctx.total_ship_cells : 1.0;
        double all_s = (ships_revealed == ctx.total_ship_cells) ? 1.0 : 0.0;
        ctx.progress.terminals.fetch_add(1, std::memory_order_relaxed);
        return {
            .ev_sp          = 0.0,
            .ev_sp2         = 0.0,
            .ship_frac      = sfrac,
            .ship_frac2     = sfrac * sfrac,
            .perf_prob      = perf,
            .avg_clicks     = (double)click_count,
            .avg_clicks2    = (double)click_count * (double)click_count,
            .loss_5050      = 0.0,
            .all_ships      = all_s,
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
    // full_sv is the full-population surviving set — correct board distribution
    // regardless of this thread's chunk.  board_indices (the chunk subset) is
    // used only for probability weighting below; it is never shown to the strategy.
    std::vector<Cell> board_vec;
    build_board_vec(revealed, revealed_dc, ca, board_vec);
    std::string meta = build_meta_json(ctx.n_colors, ships_hit, blues_used);

    Click c = ctx.bridge.next_click(board_vec, meta, full_sv);
    ctx.progress.strategy_calls.fetch_add(1, std::memory_order_relaxed);
    int cell = rc_to_idx(c.row, c.col);
    if (cell < 0 || cell >= N_CELLS || revealed[cell])
        cell = unclicked[0];  // fallback: first unclicked cell

    // Partition boards by detailed color at the chosen cell.
    // board_indices: per-chunk (for probability), full_sv: full population (for strategy).
    std::vector<int> by_color[MAX_DC];
    for (int bi : board_indices)
        by_color[cell_dc(ctx.fbs, bi, cell)].push_back(bi);

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

        int new_ships_hit   = ships_hit      + (is_ship ? 1 : 0);
        int new_ships_rev   = ships_revealed  + (is_ship ? 1 : 0);
        int new_click_count = click_count     + 1;

        // Compute the child full_sv: full-population boards surviving this reveal.
        std::vector<int> child_full_sv = full_sv;
        filter_full_sv(ctx.fbs, child_full_sv, cell, color);

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
            if (new_game_over && ships_revealed < ctx.total_ship_cells) {
                double p_blue = (double)by_color[0].size() / n_boards;
                if (p_blue > 0.25 && p_blue < 0.75)
                    extra_loss_5050 = 1.0;
            }

            // Exact probability of this branch over the full board population.
            double p_exact_blue = (full_sv.size() > 0)
                ? (double)child_full_sv.size() / (double)full_sv.size() : p_color;

            NodeResult r = tree_walk(
                ctx, by_color[color], child_full_sv, rev2, rev_dc2,
                new_ships_hit, new_blues_used, new_game_over, ca,
                new_ships_rev, new_click_count,
                path_weight * p_exact_blue);
            r.loss_5050 += extra_loss_5050;
            accumulate(result, p_color, r, (double)OT_BLUE_VALUE);

        } else if (color <= 4) {
            // ---- Fixed-ship click (teal/green/yellow/spO) ----
            // Exact probability of this branch over the full board population.
            double p_exact = (full_sv.size() > 0)
                ? (double)child_full_sv.size() / (double)full_sv.size() : p_color;
            // Phase 2 entry: record when this ship click pushes ships_hit to threshold.
            if (ctx.record_stats && new_ships_hit == SHIPS_THRESHOLD) {
                int sv_size = (int)child_full_sv.size();
                double w = path_weight * p_exact;
                auto& e = ctx.p2stats->by_bn[blues_used * 10 + new_ships_hit];
                e.prob_mass    += w;
                e.weighted_sv  += w * sv_size;
                e.weighted_sv2 += w * sv_size * sv_size;
            }
            NodeResult r = tree_walk(
                ctx, by_color[color], child_full_sv, rev2, rev_dc2,
                new_ships_hit, blues_used, false, ca,
                new_ships_rev, new_click_count,
                path_weight * p_exact);
            accumulate(result, p_color, r, (double)FIXED_SP[color]);

        } else {
            // ---- Var-rare slot click ----
            int slot = color - 5;
            // Exact probability of this color branch over the full board population.
            double p_exact = (full_sv.size() > 0)
                ? (double)child_full_sv.size() / (double)full_sv.size() : p_color;
            if (ca.is_assigned(slot)) {
                int var_c = (int)ca.color[slot];
                // Phase 2 entry: record when this ship click pushes ships_hit to threshold.
                if (ctx.record_stats && new_ships_hit == SHIPS_THRESHOLD) {
                    int sv_size = (int)child_full_sv.size();
                    double w = path_weight * p_exact;
                    auto& e = ctx.p2stats->by_bn[blues_used * 10 + new_ships_hit];
                    e.prob_mass    += w;
                    e.weighted_sv  += w * sv_size;
                    e.weighted_sv2 += w * sv_size * sv_size;
                }
                NodeResult r = tree_walk(
                    ctx, by_color[color], child_full_sv, rev2, rev_dc2,
                    new_ships_hit, blues_used, false, ca,
                    new_ships_rev, new_click_count,
                    path_weight * p_exact);
                accumulate(result, p_color, r, (double)VAR_SP[var_c]);
            } else {
                // Fan out over all possible var-rare identities (without-replacement draw).
                // Skip colors absent in this mode (weight == 0).
                // When assigning the last slot, also skip assignments that violate the
                // mode constraint (7-color: must have >=1 of spL/spD).  After collecting
                // valid branches renormalize so weights sum to 1.
                bool is_last_slot =
                    (__builtin_popcount((unsigned)ca.used_mask) + 1 == ca.n_var);

                // Collect valid branches: raw weights (before renorm).
                struct VarBranch { int vc; double raw_w; ColorAssign ca2; };
                VarBranch branches[N_VAR_COLORS];
                int n_branches = 0;
                double total_valid_w = 0.0;
                double rem_w = ca.remaining_weight(ctx.n_colors);

                for (int vc = 0; vc < N_VAR_COLORS; ++vc) {
                    if (ca.used_mask & (1u << vc)) continue;
                    double wraw = var_weight(ctx.n_colors, vc);
                    if (wraw == 0.0) continue;  // color absent in this mode
                    ColorAssign ca2 = ca.with_assign(slot, vc);
                    // For the last slot, enforce the mode constraint.
                    if (is_last_slot && !var_assignment_valid(ctx.n_colors, ca2.used_mask))
                        continue;
                    branches[n_branches++] = {vc, wraw / rem_w, ca2};
                    total_valid_w += wraw;
                }

                // Renormalize if constraint pruned some branches.
                double renorm = (total_valid_w > 0.0 && rem_w > 0.0)
                                ? rem_w / total_valid_w : 1.0;

                NodeResult var_result{};
                for (int bi = 0; bi < n_branches; ++bi) {
                    double wc       = branches[bi].raw_w * renorm;
                    int    vc       = branches[bi].vc;
                    double sp_delta = (double)VAR_SP[vc];
                    // Phase 2 entry for unassigned var-rare slot: record per identity
                    // branch so that wc (the identity probability) is included in the
                    // path weight.  The identity weights sum to 1, so the total recorded
                    // prob_mass is path_weight * p_exact * Σwc = path_weight * p_exact.
                    if (ctx.record_stats && new_ships_hit == SHIPS_THRESHOLD) {
                        int sv_size = (int)child_full_sv.size();
                        double w = path_weight * p_exact * wc;
                        auto& e = ctx.p2stats->by_bn[blues_used * 10 + new_ships_hit];
                        e.prob_mass    += w;
                        e.weighted_sv  += w * sv_size;
                        e.weighted_sv2 += w * sv_size * sv_size;
                    }
                    NodeResult r = tree_walk(
                        ctx, by_color[color], child_full_sv, rev2, rev_dc2,
                        new_ships_hit, blues_used, false, branches[bi].ca2,
                        new_ships_rev, new_click_count,
                        path_weight * p_exact * wc);
                    double ev_child = r.ev_sp + sp_delta;
                    var_result.ev_sp       += wc * ev_child;
                    var_result.ev_sp2      += wc * (r.ev_sp2
                                                    + 2.0 * sp_delta * r.ev_sp
                                                    + sp_delta * sp_delta);
                    var_result.ship_frac   += wc * r.ship_frac;
                    var_result.ship_frac2  += wc * r.ship_frac2;
                    var_result.perf_prob   += wc * r.perf_prob;
                    var_result.avg_clicks  += wc * r.avg_clicks;
                    var_result.avg_clicks2 += wc * (r.avg_clicks2 + 2.0 * r.avg_clicks + 1.0);
                    var_result.loss_5050   += wc * r.loss_5050;
                    var_result.all_ships   += wc * r.all_ships;
                }
                result.ev_sp       += p_color * var_result.ev_sp;
                result.ev_sp2      += p_color * var_result.ev_sp2;
                result.ship_frac   += p_color * var_result.ship_frac;
                result.ship_frac2  += p_color * var_result.ship_frac2;
                result.perf_prob   += p_color * var_result.perf_prob;
                result.avg_clicks  += p_color * var_result.avg_clicks;
                result.avg_clicks2 += p_color * var_result.avg_clicks2;
                result.loss_5050   += p_color * var_result.loss_5050;
                result.all_ships   += p_color * var_result.all_ships;
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
    double&             variant_elapsed_out,
    double&             init_elapsed_out,
    uint64_t            expected_calls = 0,   // from a prior run; 0 = unknown
    bool                with_stats     = false)
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

    print_ts();
    printf("  n_colors=%d: initialising %d bridge%s ...\n",
           n_colors, n_threads, n_threads == 1 ? "" : "s");
    fflush(stdout);
    auto tb0 = std::chrono::steady_clock::now();

    std::vector<std::unique_ptr<StrategyBridge>> bridges(n_threads);
    for (int t = 0; t < n_threads; ++t) {
        bridges[t] = StrategyBridge::load(strategy_path, "ot");
        bridges[t]->init_evaluation_run();
    }

    init_elapsed_out = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - tb0).count();
    {
        print_ts();
        printf("  n_colors=%d: bridges ready  (%.1fs)\n", n_colors, init_elapsed_out);
        fflush(stdout);
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

    // Background progress thread: wakes every 10 s and prints a heartbeat line
    // showing cumulative strategy calls and rolling calls/s.  If expected_calls
    // is known from a prior run it also shows percentage complete and ETA.
    // Never suppressed — the loop only exits when walk_done is set after all
    // workers join.
    std::thread progress_thread([&]() {
        using namespace std::chrono;
        auto     start          = steady_clock::now();
        double   next_time_secs = 10.0;
        uint64_t prev_calls     = 0;
        double   prev_elapsed   = 0.0;

        std::unique_lock<std::mutex> lk(progress_mutex);
        while (true) {
            progress_cv.wait_for(lk, milliseconds(1000),
                [&]{ return walk_done.load(std::memory_order_relaxed); });
            if (walk_done.load(std::memory_order_relaxed)) break;

            double elapsed = duration<double>(steady_clock::now() - start).count();
            if (elapsed < next_time_secs) continue;

            // Sum strategy_calls and active thread count across all threads
            uint64_t total_calls  = 0;
            int      active_count = 0;
            for (int t = 0; t < n_threads; ++t) {
                total_calls  += progress[t].strategy_calls.load(std::memory_order_relaxed);
                active_count += progress[t].active.load(std::memory_order_relaxed);
            }

            double interval    = elapsed - prev_elapsed;
            double calls_per_s = (interval > 0.0)
                ? (double)(total_calls - prev_calls) / interval : 0.0;

            print_ts();
            if (expected_calls > 0) {
                double pct = std::min(100.0 * (double)total_calls / (double)expected_calls, 100.0);
                double eta = (calls_per_s > 0.0 && pct < 100.0)
                    ? (double)(expected_calls - total_calls) / calls_per_s : 0.0;
                printf("  n_colors=%d:  elapsed=%.0fs  threads=%d/%d"
                       "  calls=%llu  calls/s=%.0f  %.1f%%  eta=%.0fs\n",
                       n_colors, elapsed, active_count, n_threads,
                       (unsigned long long)total_calls, calls_per_s,
                       pct, eta);
            } else {
                printf("  n_colors=%d:  elapsed=%.0fs  threads=%d/%d"
                       "  calls=%llu  calls/s=%.0f\n",
                       n_colors, elapsed, active_count, n_threads,
                       (unsigned long long)total_calls, calls_per_s);
            }
            fflush(stdout);

            prev_calls   = total_calls;
            prev_elapsed = elapsed;
            next_time_secs = elapsed + 10.0;
        }
    });

    // -----------------------------------------------------------------------
    // Launch worker threads
    // -----------------------------------------------------------------------

    std::vector<std::thread> workers;
    workers.reserve(n_threads);

    // full_sv at the root of each thread's walk = all boards [0..n_boards-1].
    // This is independent of the chunk — every thread's strategy sees the full
    // population, not just the chunk it is responsible for.
    std::vector<int> root_full_sv(n_boards);
    std::iota(root_full_sv.begin(), root_full_sv.end(), 0);

    // Phase 2 stats: collected by thread 0 only (using exact full_sv probabilities).
    Phase2Stats p2stats;

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

            bool record = with_stats && (t == 0);
            WalkContext ctx{fbs, total_ship_cells, n_colors, *bridges[t], progress[t],
                            record, record ? &p2stats : nullptr};

            progress[t].active.store(1, std::memory_order_relaxed);
            results[t] = tree_walk(
                ctx, indices, root_full_sv, revealed, revealed_dc,
                0, 0, false, ca,
                0, 0, /*path_weight=*/1.0);
            progress[t].active.store(0, std::memory_order_relaxed);
        });
    }

    for (auto& w : workers) w.join();

    // Print a final 100% progress line now that all workers are done.
    {
        double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
        uint64_t final_calls = 0;
        for (int t = 0; t < n_threads; ++t)
            final_calls += progress[t].strategy_calls.load(std::memory_order_relaxed);
        print_ts();
        printf("  n_colors=%d:  elapsed=%.0fs  calls=%llu  100.0%%\n",
               n_colors, elapsed, (unsigned long long)final_calls);
        fflush(stdout);
    }

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

    // Print Phase 2 entry stats if requested (collected by thread 0).
    if (with_stats) {
        print_phase2_stats(p2stats, n_colors, /*has_sv=*/true);
    }

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
        combined.ev_sp       += cw * results[t].ev_sp;
        combined.ev_sp2      += cw * results[t].ev_sp2;
        combined.ship_frac   += cw * results[t].ship_frac;
        combined.ship_frac2  += cw * results[t].ship_frac2;
        combined.perf_prob   += cw * results[t].perf_prob;
        combined.avg_clicks  += cw * results[t].avg_clicks;
        combined.avg_clicks2 += cw * results[t].avg_clicks2;
        combined.loss_5050   += cw * results[t].loss_5050;
        combined.all_ships   += cw * results[t].all_ships;
    }

    double mean_sp   = combined.ev_sp;
    double variance  = combined.ev_sp2 - mean_sp * mean_sp;
    double stdev_ev  = (variance > 0.0) ? std::sqrt(variance) : 0.0;

    double var_cl    = combined.avg_clicks2 - combined.avg_clicks * combined.avg_clicks;
    double stdev_cl  = (var_cl > 0.0) ? std::sqrt(var_cl) : 0.0;

    double sc_mean        = combined.ship_frac  * (double)total_ship_cells;
    double sc_mean2       = combined.ship_frac2 * (double)(total_ship_cells * total_ship_cells);
    double var_sc         = sc_mean2 - sc_mean * sc_mean;
    double stdev_sc       = (var_sc > 0.0) ? std::sqrt(var_sc) : 0.0;

    print_ts();
    printf("  n_colors=%d: done in %.1fs  ev=%.4f  stdev=%.4f\n",
           n_colors, elapsed_ev, mean_sp, stdev_ev);
    fflush(stdout);

    // Sum total strategy calls across all threads
    uint64_t total_calls = 0;
    for (int t = 0; t < n_threads; ++t)
        total_calls += progress[t].strategy_calls.load(std::memory_order_relaxed);

    OTVariantResult r;
    r.n_colors              = n_colors;
    r.n_boards              = (uint64_t)n_boards;
    r.ev                    = mean_sp;
    r.stdev_ev              = stdev_ev;
    r.avg_clicks            = combined.avg_clicks;
    r.stdev_clicks          = stdev_cl;
    r.avg_ship_clicks       = sc_mean;
    r.stdev_ship_clicks     = stdev_sc;
    r.perfect_rate          = combined.perf_prob;
    r.all_ships_rate        = combined.all_ships;
    r.loss_5050_rate        = combined.loss_5050;
    r.total_strategy_calls  = total_calls;
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
    bool with_stats = false;
    // expected_calls[i] = expected total strategy calls for n_colors = 6+i (0 = unknown)
    uint64_t expected_calls[4] = {0, 0, 0, 0};
#ifdef _OPENMP
    n_threads = omp_get_max_threads();
#else
    {
        int hw = (int)std::thread::hardware_concurrency();
        if (hw > 0) n_threads = hw;
    }
#endif

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--strategy")          && i+1 < argc) strategy_path = argv[++i];
        else if (!strcmp(argv[i], "--boards-dir")         && i+1 < argc) boards_dir    = argv[++i];
        else if (!strcmp(argv[i], "--n-colors")           && i+1 < argc) n_colors_arg  = argv[++i];
        else if (!strcmp(argv[i], "--threads")            && i+1 < argc) n_threads     = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--expected-calls-6")   && i+1 < argc) expected_calls[0] = (uint64_t)atoll(argv[++i]);
        else if (!strcmp(argv[i], "--expected-calls-7")   && i+1 < argc) expected_calls[1] = (uint64_t)atoll(argv[++i]);
        else if (!strcmp(argv[i], "--expected-calls-8")   && i+1 < argc) expected_calls[2] = (uint64_t)atoll(argv[++i]);
        else if (!strcmp(argv[i], "--expected-calls-9")   && i+1 < argc) expected_calls[3] = (uint64_t)atoll(argv[++i]);
        else if (!strcmp(argv[i], "--with-stats"))                        with_stats = true;
    }

    if (strategy_path.empty()) {
        fprintf(stderr,
            "Usage: evaluate_ot_treewalk --strategy <path> [--boards-dir <dir>]\n"
            "                            [--n-colors 6|7|8|9|all] [--threads N]\n"
            "                            [--expected-calls-6 N] [--expected-calls-7 N]\n"
            "                            [--expected-calls-8 N] [--expected-calls-9 N]\n"
            "                            [--with-stats]\n");
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
    double   total_init_elapsed    = 0.0;

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
        double variant_init_elapsed = 0.0;
        OTVariantResult vr = evaluate_variant_treewalk(fbs, nc, strategy_path,
                                                       n_threads, variant_elapsed,
                                                       variant_init_elapsed,
                                                       expected_calls[nc - 6],
                                                       with_stats);
        int vi = nc - 6;
        overall_result.variants[vi] = vr;
        variant_timings.emplace_back(nc, variant_elapsed);

        weighted_ev           += vr.ev * (double)vr.n_boards;
        total_boards_weighted += (double)vr.n_boards;
        total_init_elapsed    += variant_init_elapsed;
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
           "\"aggregate_ev\":%.4f,\"evaluator\":\"treewalk\","
           "\"init_run_elapsed_s\":%.4f,\"variants\":[",
           strategy_path.c_str(), overall_result.aggregate_ev, total_init_elapsed);
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
               "\"all_ships_rate\":%.4f,\"loss_5050_rate\":%.4f,"
               "\"total_strategy_calls\":%llu}",
               vr.n_colors, (unsigned long long)vr.n_boards,
               vr.ev, vr.stdev_ev,
               vr.avg_clicks, vr.stdev_clicks,
               vr.avg_ship_clicks, vr.stdev_ship_clicks,
               vr.perfect_rate,
               vr.all_ships_rate, vr.loss_5050_rate,
               (unsigned long long)vr.total_strategy_calls);
    }
    printf("]}\n");
    fflush(stdout);
    return 0;
}
