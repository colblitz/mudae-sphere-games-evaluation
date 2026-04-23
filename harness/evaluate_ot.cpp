/**
 * evaluate_ot.cpp — Exhaustive evaluator for /sphere trace (ot).
 *
 * Runs the strategy against all boards for each n_colors variant (6–9) and
 * reports, per variant and aggregated:
 *   ev                — mean score
 *   stdev_ev          — standard deviation of score
 *   avg_clicks        — average total clicks per game (blue + free ship cells)
 *   stdev_clicks      — standard deviation of total clicks
 *   avg_ship_clicks   — average number of clicks that hit a ship cell
 *   stdev_ship_clicks — standard deviation of ship-cell clicks
 *   perfect_rate      — fraction of games where all ship cells were revealed
 *   all_ships_rate    — fraction of games where all ships were hit (≥1 cell each)
 *   loss_5050_rate    — fraction of games ended by a blue click where
 *                       P(blue) was between 0.25 and 0.75 ("true 50/50 loss")
 *
 * Board model (n_colors = 6..9, n_rare = n_colors - 4):
 *   5×5 grid, 25 cells start covered.  Blue click budget: 4.
 *   Ships: teal(4), green(3), yellow(3), orange(2), var_rare_k(2) × n_var_rare.
 *   Clicking a ship cell is FREE (does not cost a blue click).
 *   Extra Chance: if blues_used < 4 and ships_hit < 5 after the 4th blue,
 *     the game continues; each additional blue while ships_hit < 5 extends it.
 *     After ships_hit ≥ 5, the next blue ends the game.
 *
 * Ship SP values: spT=20 spG=35 spY=55 spO=90 spL=76 spD=104 spR=150 spW=500
 * Blue (spB) = 0 SP (just an empty cell).
 *
 * Rare-ship color weighting:
 *   Each board stores only the spatial placements of var_rare ships; their
 *   identities (spL/spD/spR/spW) are not fixed in the board file.  Each
 *   possible assignment of identities to slots is weighted by the product of
 *   per-color Mudae drop probabilities (without replacement across slots):
 *     spL: 0.7143   spD: 0.4052   spR: 0.1332   spW: 0.0508
 *   Per-board EV = Σ_assignment  weight(assignment) × score(assignment).
 *   The weight of an assignment is:
 *     w(c0) / W  ×  w(c1) / (W - w(c0))  ×  ...
 *   where W = Σ w(c) over all four var colors.
 *   The Welford accumulator receives one per-board weighted EV observation,
 *   so stdev reflects across-board spatial variance (not color-identity variance).
 *
 * Evaluation is run with OpenMP parallelism (--threads N, default: all cores).
 *
 * Output: JSON to stdout on completion, progress to stdout during run.
 *
 * Usage:
 *   evaluate_ot --strategy <path> [--boards-dir <dir>]
 *               [--n-colors 6|7|8|9|all] [--threads N]
 *               [--trace N] [--seed S]
 *                 (trace mode: sample N boards; requires specific --n-colors, not "all")
 */

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <random>
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
// ot constants
// ---------------------------------------------------------------------------

static constexpr int OT_BASE_CLICKS = 4;

static int ot_ship_value(const std::string& color) {
    if (color == "spT") return 20;
    if (color == "spG") return 35;
    if (color == "spY") return 55;
    if (color == "spO") return 90;
    if (color == "spL") return 76;
    if (color == "spD") return 104;
    if (color == "spR") return 150;
    if (color == "spW") return 500;
    return 0;  // spB (blue = 0)
}

static bool ot_is_ship(const std::string& color) {
    return color != "spB";
}

// ---------------------------------------------------------------------------
// Rare-ship color identity: weights and names
//
// The board files store only spatial placements of var_rare ships; their
// color identities are drawn from {spL, spD, spR, spW} according to Mudae
// drop probabilities (without replacement across slots within a board).
// ---------------------------------------------------------------------------

static constexpr int    N_VAR_COLORS = 4;
static constexpr double VAR_WEIGHT[N_VAR_COLORS] = {0.7143, 0.4052, 0.1332, 0.0508};
static const char*      VAR_COLOR_NAMES[N_VAR_COLORS] = {"spL", "spD", "spR", "spW"};

// A single assignment of rare-color identities to var_rare slots.
struct ColorAssignment {
    int    color_idx[4];  // VAR_COLOR_NAMES index for each slot (0..3)
    double weight;        // normalized probability of this full assignment
};

// Enumerate all P(N_VAR_COLORS, n_var_rare) assignments of distinct rare colors
// to n_var_rare slots, weighted by the without-replacement draw probabilities.
// Appends results to `out`.
static void enumerate_color_assignments(int n_var_rare,
                                        std::vector<ColorAssignment>& out)
{
    static constexpr double W_TOTAL =
        VAR_WEIGHT[0] + VAR_WEIGHT[1] + VAR_WEIGHT[2] + VAR_WEIGHT[3];

    // Recursive fill: slot = current slot being assigned, used = bitmask of
    // already-assigned color indices, running_w = cumulative weight so far,
    // remaining_w = sum of weights of colors not yet used.
    std::function<void(int, uint8_t, double, double, ColorAssignment&)> fill =
        [&](int slot, uint8_t used, double running_w, double remaining_w,
            ColorAssignment& cur) {
            if (slot == n_var_rare) {
                cur.weight = running_w;
                out.push_back(cur);
                return;
            }
            for (int c = 0; c < N_VAR_COLORS; ++c) {
                if (used & (1 << c)) continue;
                cur.color_idx[slot] = c;
                fill(slot + 1,
                     static_cast<uint8_t>(used | (1 << c)),
                     running_w * (VAR_WEIGHT[c] / remaining_w),
                     remaining_w - VAR_WEIGHT[c],
                     cur);
            }
        };

    ColorAssignment cur{};
    fill(0, 0, 1.0, W_TOTAL, cur);
}

// Build the 25-element color array for a board given a specific rare-color
// assignment.  Fixed ships (teal/green/yellow/spo) are unchanged; each
// var_rare slot k gets the color VAR_COLOR_NAMES[assignment.color_idx[k]].
static std::vector<std::string> ot_board_colors_assigned(
    const OTBoard& b, const ColorAssignment& assignment)
{
    std::vector<std::string> colors(N_CELLS, "spB");
    auto paint = [&](int32_t mask, const char* color) {
        for (int i = 0; i < N_CELLS; ++i)
            if ((mask >> i) & 1) colors[i] = color;
    };
    paint(b.teal,   "spT");
    paint(b.green,  "spG");
    paint(b.yellow, "spY");
    paint(b.spo,    "spO");
    for (int k = 0; k < b.n_var_rare; ++k)
        paint(b.var_rare[k], VAR_COLOR_NAMES[assignment.color_idx[k]]);
    return colors;
}

// ---------------------------------------------------------------------------
// Extra Chance logic
// ---------------------------------------------------------------------------

// Returns true if game should end after this blue click.
// Game ends when: blues_used >= 4 AND ships_hit >= 5.
// Extra Chance keeps the game alive if blues_used >= 4 but ships_hit < 5.
static bool ot_game_over(int blues_used, int ships_hit) {
    return blues_used >= OT_BASE_CLICKS && ships_hit >= 5;
}

// ---------------------------------------------------------------------------
// Simulate one ot game
// ---------------------------------------------------------------------------

struct OTGameResult {
    double score        = 0.0;
    int    total_clicks = 0;
    int    ship_clicks  = 0;   // number of clicks that hit a ship cell
    bool   perfect      = false;  // all ship cells revealed
    bool   all_ships    = false;  // all distinct ships hit
    bool   loss_5050    = false;  // lost on a ~50/50 blue decision
};

struct MoveRecord {
    int    move_num;
    int    row, col;
    std::string color;
    double sp_delta;
    double running_score;
    int    ships_hit_before;
    int    blues_used_before;
    bool   is_free;  // true for ship hits (free clicks)
};

struct GameTrace {
    int    board_index;
    int    n_colors;
    std::string color_assignment[4];  // rare color names assigned to var_rare slots
    double score;
    std::string initial_board[N_CELLS];  // all "?" for ot
    std::string actual_board[N_CELLS];   // true color of every cell
    std::vector<MoveRecord> moves;
};

static OTGameResult run_ot_game(
    const OTBoard&              board,
    const std::vector<std::string>& colors,  // pre-derived cell colors
    int                         n_colors,
    StrategyBridge&             strategy,
    std::string&                game_state_json,
    GameTrace*                  trace = nullptr)
{
    bool clicked[N_CELLS] = {};
    std::vector<Cell> revealed;
    double score      = 0.0;
    int    blues_used = 0;
    int    ships_hit  = 0;
    int    total_clicks = 0;

    // Compute total ship cells and per-ship membership for perfect/all_ships tracking
    int total_ship_cells = 0;
    int32_t all_ship_mask = board.teal | board.green | board.yellow | board.spo;
    for (int k = 0; k < board.n_var_rare; ++k) all_ship_mask |= board.var_rare[k];
    total_ship_cells = __builtin_popcount(static_cast<uint32_t>(all_ship_mask));

    // Per-ship hit tracking for all_ships_rate
    // Ships: teal, green, yellow, spo, var_rare_0..
    int n_ships = 4 + board.n_var_rare;
    std::vector<bool> ship_hit(n_ships, false);

    std::string meta = "{\"n_colors\":" + std::to_string(n_colors)
                     + ",\"ships_hit\":0,\"blues_used\":0"
                     + ",\"max_clicks\":" + std::to_string(OT_BASE_CLICKS) + "}";
    game_state_json = strategy.init_game_payload(meta, game_state_json);

    auto ship_index_for_cell = [&](int idx) -> int {
        int32_t bit = 1 << idx;
        if (board.teal   & bit) return 0;
        if (board.green  & bit) return 1;
        if (board.yellow & bit) return 2;
        if (board.spo    & bit) return 3;
        for (int k = 0; k < board.n_var_rare; ++k)
            if (board.var_rare[k] & bit) return 4 + k;
        return -1;
    };

    int revealed_ship_cells = 0;
    int ship_clicks         = 0;  // clicks that hit a ship cell
    // Track whether game-ending blue was ~50/50
    bool last_blue_was_5050 = false;
    int  move_num = 0;

    while (true) {
        // Check game over condition
        if (ot_game_over(blues_used, ships_hit)) break;

        int ships_before = ships_hit;
        int blues_before = blues_used;

        // Build meta
        meta = "{\"n_colors\":" + std::to_string(n_colors)
             + ",\"ships_hit\":" + std::to_string(ships_hit)
             + ",\"blues_used\":" + std::to_string(blues_used)
             + ",\"max_clicks\":" + std::to_string(OT_BASE_CLICKS) + "}";

        Click c = strategy.next_click(revealed, meta, game_state_json);
        game_state_json = strategy.last_game_state();

        int idx = rc_to_idx(c.row, c.col);
        if (idx < 0 || idx >= N_CELLS || clicked[idx]) {
            // Invalid click — count as blue (worst case)
            ++blues_used;
            ++total_clicks;
            if (ot_game_over(blues_used, ships_hit)) break;
            continue;
        }
        clicked[idx] = true;
        ++total_clicks;
        ++move_num;

        const std::string& color = colors[idx];
        bool is_ship = ot_is_ship(color);
        double delta = 0.0;

        revealed.push_back({static_cast<int8_t>(c.row),
                             static_cast<int8_t>(c.col), color});

        if (is_ship) {
            delta = ot_ship_value(color);
            score += delta;
            ++ships_hit;
            ++revealed_ship_cells;
            ++ship_clicks;
            int si = ship_index_for_cell(idx);
            if (si >= 0 && si < n_ships) ship_hit[si] = true;
            // Ship click is free — no blues_used increment
        } else {
            // Blue click
            // Estimate P(blue) for 50/50 detection: count remaining unclicked cells
            int unclicked_count = 0, unclicked_blue = 0;
            for (int i = 0; i < N_CELLS; ++i) {
                if (!clicked[i]) {
                    ++unclicked_count;
                    if (!ot_is_ship(colors[i])) ++unclicked_blue;
                }
            }
            double p_blue = unclicked_count > 0
                          ? static_cast<double>(unclicked_blue) / static_cast<double>(unclicked_count)
                          : 0.0;
            last_blue_was_5050 = (p_blue > 0.25 && p_blue < 0.75);

            ++blues_used;
            if (ot_game_over(blues_used, ships_hit)) {
                if (trace) {
                    MoveRecord mr;
                    mr.move_num          = move_num;
                    mr.row               = c.row;
                    mr.col               = c.col;
                    mr.color             = color;
                    mr.sp_delta          = 0.0;
                    mr.running_score     = score;
                    mr.ships_hit_before  = ships_before;
                    mr.blues_used_before = blues_before;
                    mr.is_free           = false;
                    trace->moves.push_back(mr);
                }
                break;
            }
        }

        if (trace) {
            MoveRecord mr;
            mr.move_num          = move_num;
            mr.row               = c.row;
            mr.col               = c.col;
            mr.color             = color;
            mr.sp_delta          = delta;
            mr.running_score     = score;
            mr.ships_hit_before  = ships_before;
            mr.blues_used_before = blues_before;
            mr.is_free           = is_ship;
            trace->moves.push_back(mr);
        }
    }

    OTGameResult res;
    res.score        = score;
    res.total_clicks = total_clicks;
    res.ship_clicks  = ship_clicks;
    res.perfect      = (revealed_ship_cells == total_ship_cells);
    res.all_ships    = true;
    for (int s = 0; s < n_ships; ++s)
        if (!ship_hit[s]) { res.all_ships = false; break; }
    // 50/50 loss: game ended (blues_used >= 4 && ships_hit >= 5 is a normal end;
    // we flag loss if ships_hit < total_ship_cells at game end AND last blue was ~50/50)
    res.loss_5050 = (!res.perfect && last_blue_was_5050);
    return res;
}

// ---------------------------------------------------------------------------
// JSON helpers for trace output
// ---------------------------------------------------------------------------

static void print_trace_json(const std::vector<GameTrace>& traces) {
    printf("TRACE_JSON: [");
    for (size_t gi = 0; gi < traces.size(); ++gi) {
        const auto& t = traces[gi];
        if (gi > 0) printf(",");
        printf("{\"board_index\":%d,\"n_colors\":%d,\"score\":%.1f,"
               "\"color_assignment\":[",
               t.board_index, t.n_colors, t.score);
        for (int k = 0; k < 4; ++k) {
            if (k > 0) printf(",");
            if (!t.color_assignment[k].empty())
                printf("\"%s\"", t.color_assignment[k].c_str());
            else
                printf("null");
        }
        printf("],\"initial_board\":[");
        for (int i = 0; i < N_CELLS; ++i) {
            if (i > 0) printf(",");
            printf("\"%s\"", t.initial_board[i].c_str());
        }
        printf("],\"actual_board\":[");
        for (int i = 0; i < N_CELLS; ++i) {
            if (i > 0) printf(",");
            printf("\"%s\"", t.actual_board[i].c_str());
        }
        printf("],\"moves\":[");
        for (size_t mi = 0; mi < t.moves.size(); ++mi) {
            const auto& m = t.moves[mi];
            if (mi > 0) printf(",");
            printf("{\"move_num\":%d,\"row\":%d,\"col\":%d,"
                   "\"color\":\"%s\",\"sp_delta\":%.1f,\"running_score\":%.1f,"
                   "\"meta\":{\"n_colors\":%d,\"ships_hit\":%d,"
                   "\"blues_used\":%d,\"max_clicks\":%d},"
                   "\"free\":%s}",
                   m.move_num, m.row, m.col,
                   m.color.c_str(), m.sp_delta, m.running_score,
                   t.n_colors, m.ships_hit_before, m.blues_used_before,
                   OT_BASE_CLICKS,
                   m.is_free ? "true" : "false");
        }
        printf("]}");
    }
    printf("]\n");
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Evaluate one n_colors variant
// ---------------------------------------------------------------------------

static OTVariantResult evaluate_variant(
    const std::vector<OTBoard>& boards,
    int                         n_colors,
    const std::string&          strategy_path,
    int                         n_threads)
{
    uint64_t total = boards.size();
    printf("  n_colors=%d: %llu boards\n", n_colors, (unsigned long long)total);
    fflush(stdout);

    // Precompute all rare-color assignments for this variant's n_var_rare.
    // Each board will be evaluated once per assignment; per-board weighted EV
    // (= Σ_assignment weight × score) is fed as a single Welford observation.
    int n_var_rare = n_colors - 5;  // 6→1, 7→2, 8→3, 9→4
    std::vector<ColorAssignment> assignments;
    if (n_var_rare > 0) {
        enumerate_color_assignments(n_var_rare, assignments);
    } else {
        // 5-color (no var_rare): single trivial assignment with weight 1
        ColorAssignment trivial{};
        trivial.weight = 1.0;
        assignments.push_back(trivial);
    }

    // Per-thread accumulators
    std::vector<Welford>  ev_acc(n_threads);
    std::vector<Welford>  clicks_acc(n_threads);
    std::vector<Welford>  ship_clicks_acc(n_threads);
    // For fractional stats (weighted per board): accumulate as doubles
    std::vector<double>   perfect_acc(n_threads, 0.0);
    std::vector<double>   all_ships_acc(n_threads, 0.0);
    std::vector<double>   loss_5050_acc(n_threads, 0.0);

    std::atomic<uint64_t> done_count(0);
    ProgressReporter prog(total, std::max<uint64_t>(total / 20, 50000));

    // Each thread gets its own strategy instance
    std::vector<std::unique_ptr<StrategyBridge>> bridges(n_threads);
    for (int t = 0; t < n_threads; ++t) {
        bridges[t] = StrategyBridge::load(strategy_path, "ot");
    }

    std::vector<std::string> evaluation_run_states(n_threads);
    for (int t = 0; t < n_threads; ++t)
        evaluation_run_states[t] = bridges[t]->init_evaluation_run();

#ifdef _OPENMP
    omp_set_num_threads(n_threads);
    #pragma omp parallel for schedule(dynamic, 256)
#endif
    for (int64_t i = 0; i < static_cast<int64_t>(total); ++i) {
#ifdef _OPENMP
        int tid = omp_get_thread_num();
#else
        int tid = 0;
#endif
        // Accumulate per-board weighted stats across all color assignments.
        // Each assignment runs an independent game; the per-board observation
        // fed into Welford is the probability-weighted sum of scores, so stdev
        // reflects across-board spatial variance only.
        double board_ev          = 0.0;
        double board_clicks      = 0.0;
        double board_ship_clicks = 0.0;
        double board_perfect     = 0.0;
        double board_allships    = 0.0;
        double board_loss5050    = 0.0;

        for (const auto& assign : assignments) {
            auto colors = ot_board_colors_assigned(boards[i], assign);
            auto res    = run_ot_game(boards[i], colors, n_colors,
                                      *bridges[tid], evaluation_run_states[tid]);
            board_ev          += assign.weight * res.score;
            board_clicks      += assign.weight * static_cast<double>(res.total_clicks);
            board_ship_clicks += assign.weight * static_cast<double>(res.ship_clicks);
            board_perfect     += assign.weight * (res.perfect   ? 1.0 : 0.0);
            board_allships    += assign.weight * (res.all_ships  ? 1.0 : 0.0);
            board_loss5050    += assign.weight * (res.loss_5050  ? 1.0 : 0.0);
        }

        ev_acc[tid].update(board_ev);
        clicks_acc[tid].update(board_clicks);
        ship_clicks_acc[tid].update(board_ship_clicks);
        perfect_acc[tid]  += board_perfect;
        all_ships_acc[tid] += board_allships;
        loss_5050_acc[tid] += board_loss5050;

        uint64_t d = done_count.fetch_add(1) + 1;
        if (d % prog.interval == 0) prog.report(d, ev_acc[tid].mean);
    }
    prog.done(ev_acc[0].mean);

    // Merge per-thread accumulators via Chan's parallel Welford algorithm
    double mean_total        = 0.0;
    double clicks_total      = 0.0;
    double ship_clicks_total = 0.0;
    double M2_ev = 0.0, M2_cl = 0.0, M2_sc = 0.0;
    uint64_t count_total = 0;
    double perf = 0.0, all_s = 0.0, loss5050 = 0.0;

    for (int t = 0; t < n_threads; ++t) {
        uint64_t nb = ev_acc[t].count;
        if (nb == 0) continue;
        double delta    = ev_acc[t].mean        - mean_total;
        double delta_c  = clicks_acc[t].mean    - clicks_total;
        double delta_sc = ship_clicks_acc[t].mean - ship_clicks_total;
        uint64_t nc = count_total + nb;
        double w = static_cast<double>(nb) / static_cast<double>(nc);
        mean_total        += delta    * w;
        clicks_total      += delta_c  * w;
        ship_clicks_total += delta_sc * w;
        double cross = static_cast<double>(count_total) * static_cast<double>(nb)
                       / static_cast<double>(nc);
        M2_ev += ev_acc[t].M2        + delta    * delta    * cross;
        M2_cl += clicks_acc[t].M2    + delta_c  * delta_c  * cross;
        M2_sc += ship_clicks_acc[t].M2 + delta_sc * delta_sc * cross;
        count_total = nc;
        perf     += perfect_acc[t];
        all_s    += all_ships_acc[t];
        loss5050 += loss_5050_acc[t];
    }

    double denom = count_total > 1 ? static_cast<double>(count_total - 1) : 1.0;
    OTVariantResult r;
    r.n_colors            = n_colors;
    r.n_boards            = count_total;
    r.ev                  = mean_total;
    r.stdev_ev            = count_total > 1 ? std::sqrt(M2_ev / denom) : 0.0;
    r.avg_clicks          = clicks_total;
    r.stdev_clicks        = count_total > 1 ? std::sqrt(M2_cl / denom) : 0.0;
    r.avg_ship_clicks     = ship_clicks_total;
    r.stdev_ship_clicks   = count_total > 1 ? std::sqrt(M2_sc / denom) : 0.0;
    r.perfect_rate        = count_total > 0 ? perf     / static_cast<double>(count_total) : 0.0;
    r.all_ships_rate      = count_total > 0 ? all_s    / static_cast<double>(count_total) : 0.0;
    r.loss_5050_rate      = count_total > 0 ? loss5050 / static_cast<double>(count_total) : 0.0;
    return r;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IOLBF, 0);  // line-buffer stdout so progress streams through pipes
    std::string strategy_path;
    std::string boards_dir = std::string(REPO_ROOT) + "/boards";
    std::string n_colors_arg = "all";
    int n_threads = 1;
#ifdef _OPENMP
    n_threads = omp_get_max_threads();
#endif
    int      trace_n    = 0;
    uint64_t trace_seed = 42;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--strategy")  && i + 1 < argc) strategy_path = argv[++i];
        else if (!strcmp(argv[i], "--boards-dir") && i + 1 < argc) boards_dir   = argv[++i];
        else if (!strcmp(argv[i], "--n-colors")   && i + 1 < argc) n_colors_arg = argv[++i];
        else if (!strcmp(argv[i], "--threads")    && i + 1 < argc) n_threads    = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trace")      && i + 1 < argc) trace_n      = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed")       && i + 1 < argc) trace_seed   = strtoull(argv[++i], nullptr, 10);
    }
    if (strategy_path.empty()) {
        fprintf(stderr,
            "Usage: evaluate_ot --strategy <path> [--boards-dir <dir>]\n"
            "                   [--n-colors 6|7|8|9|all] [--threads N]\n"
            "                   [--trace N] [--seed S]\n");
        return 1;
    }

    std::vector<int> variants;
    if (n_colors_arg == "all") variants = {6, 7, 8, 9};
    else variants = {atoi(n_colors_arg.c_str())};

    // ------------------------------------------------------------------
    // TRACE MODE
    // ------------------------------------------------------------------
    if (trace_n > 0) {
        if (variants.size() != 1) {
            fprintf(stderr, "ERROR: --trace requires a specific --n-colors value (6, 7, 8, or 9), not 'all'.\n");
            return 1;
        }
        int nc = variants[0];
        int n_var_rare = nc - 5;  // 6→1, 7→2, 8→3, 9→4

        int n_rare = nc - 4;
        std::string board_path = boards_dir + "/ot_boards_" + std::to_string(n_rare) + ".bin.lzma";
        printf("Loading %s ...\n", board_path.c_str());
        fflush(stdout);
        auto boards = load_ot_boards(board_path, n_rare);
        if (boards.empty()) {
            fprintf(stderr, "ERROR: failed to load boards from %s\n", board_path.c_str());
            return 1;
        }
        printf("Loaded %zu boards for n_colors=%d.\n", boards.size(), nc);

        printf("Loading strategy %s ...\n", strategy_path.c_str());
        fflush(stdout);
        std::unique_ptr<StrategyBridge> bridge;
        try {
            bridge = StrategyBridge::load(strategy_path, "ot");
        } catch (const std::exception& e) {
            fprintf(stderr, "ERROR: %s\n", e.what());
            return 1;
        }

        // Precompute color assignments
        std::vector<ColorAssignment> assignments;
        if (n_var_rare > 0) {
            enumerate_color_assignments(n_var_rare, assignments);
        } else {
            ColorAssignment trivial{};
            trivial.weight = 1.0;
            assignments.push_back(trivial);
        }

        printf("Trace mode: sampling %d boards (seed=%llu, n_colors=%d) ...\n",
               trace_n, (unsigned long long)trace_seed, nc);
        fflush(stdout);

        // Sample board indices
        std::mt19937_64 rng(trace_seed);
        std::vector<int> indices(boards.size());
        for (int i = 0; i < (int)boards.size(); ++i) indices[i] = i;
        std::shuffle(indices.begin(), indices.end(), rng);
        if (trace_n > (int)indices.size()) trace_n = (int)indices.size();
        indices.resize(trace_n);

        std::string eval_run_state = bridge->init_evaluation_run();
        std::vector<GameTrace> traces;
        traces.reserve(trace_n);

        for (int bidx : indices) {
            // Sample one color assignment by weight
            double total_w = 0.0;
            for (auto& a : assignments) total_w += a.weight;
            double r = std::uniform_real_distribution<double>(0.0, total_w)(rng);
            double acc = 0.0;
            int chosen = 0;
            for (int ai = 0; ai < (int)assignments.size(); ++ai) {
                acc += assignments[ai].weight;
                if (r <= acc) { chosen = ai; break; }
            }
            const auto& assign = assignments[chosen];
            auto colors = ot_board_colors_assigned(boards[bidx], assign);

            GameTrace gt;
            gt.board_index = bidx;
            gt.n_colors    = nc;
            for (int k = 0; k < 4; ++k) {
                if (k < n_var_rare)
                    gt.color_assignment[k] = VAR_COLOR_NAMES[assign.color_idx[k]];
                else
                    gt.color_assignment[k] = "";
            }
            for (int c = 0; c < N_CELLS; ++c) {
                gt.initial_board[c] = "?";
                gt.actual_board[c]  = colors[c];
            }

            run_ot_game(boards[bidx], colors, nc, *bridge, eval_run_state, &gt);

            // Compute score from moves
            gt.score = gt.moves.empty() ? 0.0 : gt.moves.back().running_score;
            traces.push_back(std::move(gt));
        }

        print_trace_json(traces);
        return 0;
    }

    // ------------------------------------------------------------------
    // NORMAL EVALUATION MODE
    // ------------------------------------------------------------------
    printf("evaluate_ot  strategy=%s  threads=%d\n", strategy_path.c_str(), n_threads);
    fflush(stdout);

    // Load strategy (used inside evaluate_variant per-thread, but we verify it loads first)
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

    // Release the GIL so OpenMP threads can each re-acquire it via PyGILState_Ensure
    PyThreadState* _tstate = Py_IsInitialized() ? PyEval_SaveThread() : nullptr;

    OTResult overall_result;
    double   total_boards_weighted = 0.0;
    double   weighted_ev           = 0.0;

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

        OTVariantResult vr = evaluate_variant(boards, nc, strategy_path, n_threads);
        int vi = nc - 6;
        overall_result.variants[vi] = vr;

        weighted_ev           += vr.ev * static_cast<double>(vr.n_boards);
        total_boards_weighted += static_cast<double>(vr.n_boards);
    }

    // Re-acquire the GIL on the main thread
    if (_tstate) PyEval_RestoreThread(_tstate);

    if (total_boards_weighted > 0.0)
        overall_result.aggregate_ev = weighted_ev / total_boards_weighted;

    // Print JSON result
    printf("\nRESULT_JSON: {\"game\":\"ot\",\"strategy\":\"%s\",\"aggregate_ev\":%.4f,\"variants\":[",
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
