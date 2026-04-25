/**
 * evaluate_oc.cpp — Exhaustive evaluator for /sphere chest (oc).
 *
 * Runs the strategy against all 16,800 valid boards and reports:
 *   ev       — mean score across all boards
 *   stdev    — standard deviation of per-board scores
 *   red_rate — fraction of boards where the red sphere was clicked
 *
 * Board model:
 *   5×5 grid, all cells start covered.  Click budget: 5.
 *   One red sphere (spR, 150 SP) is hidden at a non-center position.
 *   Its position determines all other cell colors via spatial zones:
 *     orth    (|dr|+|dc|==1)            → orange spO (90 SP)   [2 cells]
 *     diag    (|dr|==|dc|, dr!=0)       → yellow spY (55 SP)   [3 cells]
 *     rowcol  (same row/col, not above)  → green  spG (35 SP)   [4 cells]
 *     none    (no geometric relation)    → blue   spB (10 SP)
 *     residual orth/rowcol              → teal   spT (20 SP)
 *   The board file encodes this as a 25-char ASCII string (ROYGTTB...).
 *
 * Output: JSON to stdout on completion, progress to stdout during run.
 *
 * Usage:
 *   evaluate_oc --strategy path/to/strategy.py [--boards path/to/oc_boards.bin.lzma]
 *               [--threads N]
 *               [--trace N] [--seed S]   (trace mode: sample N boards, emit TRACE_JSON)
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// Repo root: injected at compile time via -DREPO_ROOT=\"...\"
#ifndef REPO_ROOT
#define REPO_ROOT "."
#endif

using namespace sphere;

// ---------------------------------------------------------------------------
// Trace record
// ---------------------------------------------------------------------------

struct MoveRecord {
    int    move_num;
    int    row, col;
    std::string color;
    double sp_delta;
    double running_score;
    int    clicks_left_before;  // meta value at the time of the click decision
    bool   is_free;             // oc: never free, but kept for consistency
};

struct GameTrace {
    int    board_index;
    double score;
    std::string initial_board[N_CELLS];  // "?" for covered, color name for revealed
    std::string actual_board[N_CELLS];   // true color of every cell
    std::vector<MoveRecord> moves;
};

// ---------------------------------------------------------------------------
// Simulate one oc game
// ---------------------------------------------------------------------------

// Returns {score, red_clicked}
static std::pair<double, bool> run_oc_game(
    const OCBoard&  board,
    StrategyBridge& strategy,
    GameTrace*      trace = nullptr)
{
    // Full 25-cell board; all start as (color="spU", clicked=false)
    std::array<Cell, N_CELLS> game_board;
    for (int i = 0; i < N_CELLS; ++i) {
        game_board[i].row     = static_cast<int8_t>(idx_to_row(i));
        game_board[i].col     = static_cast<int8_t>(idx_to_col(i));
        game_board[i].color   = "spU";
        game_board[i].clicked = false;
    }

    double score      = 0.0;
    bool   red_found  = false;
    int    clicks_left = 5;

    // init_game_payload at the start of each game
    std::string meta = "{\"clicks_left\":5,\"max_clicks\":5}";
    strategy.init_game_payload(meta);

    int move_num = 0;

    while (clicks_left > 0) {
        meta = "{\"clicks_left\":" + std::to_string(clicks_left) + ",\"max_clicks\":5}";
        int clicks_before = clicks_left;

        std::vector<Cell> board_vec(game_board.begin(), game_board.end());
        Click c = strategy.next_click(board_vec, meta);

        int idx = rc_to_idx(c.row, c.col);
        if (idx < 0 || idx >= N_CELLS || game_board[idx].clicked) {
            // Invalid or repeated click — skip (don't penalise, just waste a click)
            --clicks_left;
            continue;
        }

        uint8_t color_int = board.cells[idx];
        const char* color = OC_COLOR_NAMES[color_int];
        int value         = OC_COLOR_VALUES[color_int];

        game_board[idx].color   = color;
        game_board[idx].clicked = true;
        score += value;
        if (color_int == 0) red_found = true;  // spR
        --clicks_left;
        ++move_num;

        if (trace) {
            MoveRecord mr;
            mr.move_num           = move_num;
            mr.row                = c.row;
            mr.col                = c.col;
            mr.color              = color;
            mr.sp_delta           = value;
            mr.running_score      = score;
            mr.clicks_left_before = clicks_before;
            mr.is_free            = false;
            trace->moves.push_back(mr);
        }
    }

    return {score, red_found};
}

// ---------------------------------------------------------------------------
// JSON helpers for trace output
// ---------------------------------------------------------------------------

static void print_trace_json(const std::vector<GameTrace>& traces) {
    printf("TRACE_JSON: [");
    for (size_t gi = 0; gi < traces.size(); ++gi) {
        const auto& t = traces[gi];
        if (gi > 0) printf(",");
        printf("{\"board_index\":%d,\"score\":%.1f,\"initial_board\":[",
               t.board_index, t.score);
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
                   "\"meta\":{\"clicks_left\":%d,\"max_clicks\":5},"
                   "\"free\":%s}",
                   m.move_num, m.row, m.col,
                   m.color.c_str(), m.sp_delta, m.running_score,
                   m.clicks_left_before,
                   m.is_free ? "true" : "false");
        }
        printf("]}");
    }
    printf("]\n");
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IOLBF, 0);  // line-buffer stdout so progress streams through pipes
    std::string strategy_path;
    std::string boards_path = std::string(REPO_ROOT) + "/boards/oc_boards.bin.lzma";
    int         n_threads   = 1;
#ifdef _OPENMP
    n_threads = omp_get_max_threads();
#endif
    int      trace_n   = 0;
    uint64_t trace_seed = 42;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--strategy") && i + 1 < argc) strategy_path = argv[++i];
        else if (!strcmp(argv[i], "--boards")  && i + 1 < argc) boards_path = argv[++i];
        else if (!strcmp(argv[i], "--boards-dir") && i + 1 < argc) {
            // evaluate_oc uses --boards, not --boards-dir; accept and derive path
            boards_path = std::string(argv[++i]) + "/oc_boards.bin.lzma";
        }
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) n_threads   = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trace")   && i + 1 < argc) trace_n     = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed")    && i + 1 < argc) trace_seed  = strtoull(argv[++i], nullptr, 10);
    }
    if (strategy_path.empty()) {
        fprintf(stderr, "Usage: evaluate_oc --strategy <path> [--boards <path>] [--threads N]\n"
                        "                   [--trace N] [--seed S]\n");
        return 1;
    }

    // Load boards
    printf("Loading boards from %s ...\n", boards_path.c_str());
    fflush(stdout);
    auto boards = load_oc_boards(boards_path);
    if (boards.empty()) {
        fprintf(stderr, "ERROR: failed to load boards from %s\n", boards_path.c_str());
        return 1;
    }
    printf("Loaded %zu boards.\n", boards.size());
    fflush(stdout);

    // Compute per-board weights matching the DP's uniform-over-red-positions prior.
    // Each red position gets equal total weight (1/24); weight per board = 1/count(red_pos).
    // Weights are then normalised to sum to 1 across all boards.
    std::vector<double> board_weights;
    {
        // Count boards per red position
        uint32_t red_counts[N_CELLS] = {};
        for (const auto& b : boards) {
            for (int i = 0; i < N_CELLS; ++i) {
                if (b.cells[i] == 0) { ++red_counts[i]; break; }
            }
        }
        // Assign unnormalised weight = 1 / red_counts[red_pos]; normalise to sum=1
        double total_w = 0.0;
        board_weights.resize(boards.size());
        for (size_t bi = 0; bi < boards.size(); ++bi) {
            int red_pos = -1;
            for (int i = 0; i < N_CELLS; ++i) {
                if (boards[bi].cells[i] == 0) { red_pos = i; break; }
            }
            double w = (red_pos >= 0 && red_counts[red_pos] > 0)
                       ? 1.0 / static_cast<double>(red_counts[red_pos]) : 0.0;
            board_weights[bi] = w;
            total_w += w;
        }
        for (double& w : board_weights) w /= total_w;
    }

    // Load strategy
    printf("Loading strategy %s ...\n", strategy_path.c_str());
    fflush(stdout);
    std::unique_ptr<StrategyBridge> bridge;
    try {
        bridge = StrategyBridge::load(strategy_path, "oc");
    } catch (const std::exception& e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        return 1;
    }

    // ------------------------------------------------------------------
    // TRACE MODE
    // ------------------------------------------------------------------
    if (trace_n > 0) {
        printf("Trace mode: sampling %d boards (seed=%llu) ...\n",
               trace_n, (unsigned long long)trace_seed);
        fflush(stdout);

        // Sample board indices
        std::mt19937_64 rng(trace_seed);
        std::vector<int> indices(boards.size());
        for (int i = 0; i < (int)boards.size(); ++i) indices[i] = i;
        std::shuffle(indices.begin(), indices.end(), rng);
        if (trace_n > (int)indices.size()) trace_n = (int)indices.size();
        indices.resize(trace_n);

        bridge->init_evaluation_run();
        std::vector<GameTrace> traces;
        traces.reserve(trace_n);

        for (int idx : indices) {
            GameTrace gt;
            gt.board_index = idx;
            // All cells covered at start; actual_board reveals true colors
            for (int c = 0; c < N_CELLS; ++c) {
                gt.initial_board[c] = "?";
                gt.actual_board[c]  = OC_COLOR_NAMES[boards[idx].cells[c]];
            }

            auto [score, _red] = run_oc_game(boards[idx], *bridge, &gt);
            gt.score = score;
            traces.push_back(std::move(gt));
        }

        print_trace_json(traces);
        return 0;
    }

    // ------------------------------------------------------------------
    // NORMAL EVALUATION MODE
    // ------------------------------------------------------------------
    printf("Strategy loaded. Running %zu boards (threads=%d) ...\n", boards.size(), n_threads);
    fflush(stdout);

    // Per-thread bridges and accumulators
    uint64_t total = boards.size();
    std::vector<std::unique_ptr<StrategyBridge>> bridges(n_threads);
    bridges[0] = std::move(bridge);
    for (int t = 1; t < n_threads; ++t)
        bridges[t] = StrategyBridge::load(strategy_path, "oc");

    for (int t = 0; t < n_threads; ++t)
        bridges[t]->init_evaluation_run();

    std::vector<WeightedWelford> ev_acc(n_threads);
    std::vector<double>          red_wsum(n_threads, 0.0);  // weighted red count

    std::atomic<uint64_t> done_count(0);
    ProgressReporter prog(total, 2000);

    // Release the GIL so OpenMP threads can each re-acquire it via PyGILState_Ensure
    PyThreadState* _tstate = Py_IsInitialized() ? PyEval_SaveThread() : nullptr;

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
        double score = 0.0; bool red_found = false;
        try {
            auto [s, r] = run_oc_game(boards[i], *bridges[tid]);
            score = s; red_found = r;
        } catch (const std::exception& e) {
            fprintf(stderr, "\nERROR on board %lld: %s\n", (long long)i, e.what());
            exit(1);
        }
        double w = board_weights[static_cast<size_t>(i)];
        ev_acc[tid].update(score, w);
        if (red_found) red_wsum[tid] += w;

        uint64_t d = done_count.fetch_add(1) + 1;
        if (d % prog.interval == 0)
            prog.report(d, ev_acc[tid].mean);
    }
    prog.done(ev_acc[0].mean);

    // Re-acquire the GIL on the main thread
    if (_tstate) PyEval_RestoreThread(_tstate);

    // Merge per-thread accumulators
    WeightedWelford merged;
    double total_red_w = 0.0;
    for (int t = 0; t < n_threads; ++t) {
        merged.merge(ev_acc[t]);
        total_red_w += red_wsum[t];
    }

    // Print JSON result
    // red_rate: total weight of boards where red was clicked (sums to fraction in [0,1])
    printf("\nRESULT_JSON: {\"game\":\"oc\","
           "\"strategy\":\"%s\","
           "\"n_boards\":%llu,"
           "\"ev\":%.4f,"
           "\"stdev\":%.4f,"
           "\"red_rate\":%.4f}\n",
           strategy_path.c_str(),
           (unsigned long long)merged.count,
           merged.mean,
           merged.stdev(),
           total_red_w);
    fflush(stdout);
    return 0;
}
