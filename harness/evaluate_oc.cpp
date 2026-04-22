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
 */

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
// Simulate one oc game
// ---------------------------------------------------------------------------

// Returns {score, red_clicked}
static std::pair<double, bool> run_oc_game(
    const OCBoard&  board,
    StrategyBridge& strategy,
    std::string&    game_state_json)
{
    std::vector<Cell> revealed;
    double score      = 0.0;
    bool   red_found  = false;
    int    clicks_left = 5;

    // init_game_payload at the start of each game
    std::string meta = "{\"clicks_left\":5,\"max_clicks\":5}";
    game_state_json = strategy.init_game_payload(meta, game_state_json);

    bool clicked[N_CELLS] = {};

    while (clicks_left > 0) {
        // Build current meta
        meta = "{\"clicks_left\":" + std::to_string(clicks_left) + ",\"max_clicks\":5}";

        Click c = strategy.next_click(revealed, meta, game_state_json);
        // Thread state from bridge (Python bridge stores it internally)
        // For C++/JS bridges the state is embedded in the return; we use a
        // workaround: each bridge returns updated state as a side-channel.
        // For Python strategies the game_state_json is extracted from the return tuple
        // and stored in last_game_state() on the bridge.
        // The Python bridge stores last_state_ internally; we read it back.
        // For C++ and JS bridges, state is returned inline in the JSON response.
        // This is handled by each bridge's next_click updating last_state_.
        // We pull it via dynamic_cast.
        if (auto* pb = dynamic_cast<PythonBridge*>(&strategy)) {
            game_state_json = pb->last_game_state();
        }

        int idx = rc_to_idx(c.row, c.col);
        if (idx < 0 || idx >= N_CELLS || clicked[idx]) {
            // Invalid or repeated click — skip (don't penalise, just waste a click)
            --clicks_left;
            continue;
        }
        clicked[idx] = true;

        uint8_t color_int = board.cells[idx];
        const char* color = OC_COLOR_NAMES[color_int];
        int value         = OC_COLOR_VALUES[color_int];

        revealed.push_back({static_cast<int8_t>(c.row),
                             static_cast<int8_t>(c.col),
                             color});
        score += value;
        if (color_int == 0) red_found = true;  // spR
        --clicks_left;
    }

    return {score, red_found};
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

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--strategy") && i + 1 < argc) strategy_path = argv[++i];
        else if (!strcmp(argv[i], "--boards")  && i + 1 < argc) boards_path = argv[++i];
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) n_threads   = atoi(argv[++i]);
    }
    if (strategy_path.empty()) {
        fprintf(stderr, "Usage: evaluate_oc --strategy <path> [--boards <path>] [--threads N]\n");
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
    printf("Strategy loaded. Running %zu boards (threads=%d) ...\n", boards.size(), n_threads);
    fflush(stdout);

    // Per-thread bridges and accumulators
    uint64_t total = boards.size();
    std::vector<std::unique_ptr<StrategyBridge>> bridges(n_threads);
    bridges[0] = std::move(bridge);
    for (int t = 1; t < n_threads; ++t)
        bridges[t] = StrategyBridge::load(strategy_path, "oc");

    std::vector<std::string> evaluation_run_states(n_threads);
    for (int t = 0; t < n_threads; ++t)
        evaluation_run_states[t] = bridges[t]->init_evaluation_run();

    std::vector<Welford>  ev_acc(n_threads);
    std::vector<uint64_t> red_count(n_threads, 0);

    std::atomic<uint64_t> done_count(0);
    ProgressReporter prog(total, 2000);

    // Release the GIL so OpenMP threads can each re-acquire it via PyGILState_Ensure
    PyThreadState* _tstate = PyEval_SaveThread();

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
        auto [score, red_found] = run_oc_game(boards[i], *bridges[tid], evaluation_run_states[tid]);
        ev_acc[tid].update(score);
        if (red_found) ++red_count[tid];

        uint64_t d = done_count.fetch_add(1) + 1;
        if (d % prog.interval == 0)
            prog.report(d, ev_acc[tid].mean);
    }
    prog.done(ev_acc[0].mean);

    // Re-acquire the GIL on the main thread
    PyEval_RestoreThread(_tstate);

    // Merge per-thread accumulators (Chan's parallel Welford)
    double   mean_total = 0.0, M2_total = 0.0;
    uint64_t count_total = 0, total_red = 0;
    for (int t = 0; t < n_threads; ++t) {
        uint64_t nb = ev_acc[t].count;
        if (nb == 0) continue;
        double delta = ev_acc[t].mean - mean_total;
        uint64_t nc  = count_total + nb;
        mean_total  += delta * static_cast<double>(nb) / static_cast<double>(nc);
        M2_total    += ev_acc[t].M2 + delta * delta
                       * static_cast<double>(count_total)
                       * static_cast<double>(nb) / static_cast<double>(nc);
        count_total  = nc;
        total_red   += red_count[t];
    }
    double stdev_total = count_total > 1
        ? std::sqrt(M2_total / static_cast<double>(count_total - 1)) : 0.0;

    // Print JSON result
    double red_rate = static_cast<double>(total_red) / static_cast<double>(count_total);
    printf("\nRESULT_JSON: {\"game\":\"oc\","
           "\"strategy\":\"%s\","
           "\"n_boards\":%llu,"
           "\"ev\":%.4f,"
           "\"stdev\":%.4f,"
           "\"red_rate\":%.4f}\n",
           strategy_path.c_str(),
           (unsigned long long)count_total,
           mean_total,
           stdev_total,
           red_rate);
    fflush(stdout);
    return 0;
}
