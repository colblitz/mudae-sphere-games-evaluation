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
 */

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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
    std::string&    state_json)
{
    std::vector<Cell> revealed;
    double score      = 0.0;
    bool   red_found  = false;
    int    clicks_left = 5;

    // init_run at the start of each game
    std::string meta = "{\"clicks_left\":5,\"max_clicks\":5}";
    state_json = strategy.init_run(meta, state_json);

    bool clicked[N_CELLS] = {};

    while (clicks_left > 0) {
        // Build current meta
        meta = "{\"clicks_left\":" + std::to_string(clicks_left) + ",\"max_clicks\":5}";

        Click c = strategy.next_click(revealed, meta, state_json);
        // Thread state from bridge (Python bridge stores it internally)
        // For C++/JS bridges the state is embedded in the return; we use a
        // workaround: each bridge returns updated state as a side-channel.
        // For simplicity all bridges expose last_state_json() via a cast.
        // Actually: for now we pass state_json to next_click and expect it back.
        // The Python bridge stores last_state_ internally; we read it back.
        // For C++ and JS bridges, state is returned inline in the JSON response.
        // This is handled by each bridge's next_click updating last_state_.
        // We pull it via dynamic_cast.
        if (auto* pb = dynamic_cast<PythonBridge*>(&strategy)) {
            state_json = pb->last_state();
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
    std::string strategy_path;
    std::string boards_path = std::string(REPO_ROOT) + "/boards/oc_boards.bin.lzma";

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--strategy") && i + 1 < argc) strategy_path = argv[++i];
        else if (!strcmp(argv[i], "--boards") && i + 1 < argc)  boards_path  = argv[++i];
    }
    if (strategy_path.empty()) {
        fprintf(stderr, "Usage: evaluate_oc --strategy <path> [--boards <path>]\n");
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
    printf("Strategy loaded.\n");
    fflush(stdout);

    // Initialise per-evaluation state
    std::string state_json = bridge->init_payload();

    // Run evaluation
    uint64_t total     = boards.size();
    uint64_t red_count = 0;
    Welford  ev_acc;
    ProgressReporter prog(total, 2000);

    for (size_t i = 0; i < boards.size(); ++i) {
        auto [score, red_found] = run_oc_game(boards[i], *bridge, state_json);
        ev_acc.update(score);
        if (red_found) ++red_count;

        if ((i + 1) % prog.interval == 0)
            prog.report(i + 1, ev_acc.mean);
    }
    prog.done(ev_acc.mean);

    // Print JSON result
    double red_rate = static_cast<double>(red_count) / static_cast<double>(total);
    printf("\nRESULT_JSON: {\"game\":\"oc\","
           "\"strategy\":\"%s\","
           "\"n_boards\":%llu,"
           "\"ev\":%.4f,"
           "\"stdev\":%.4f,"
           "\"red_rate\":%.4f}\n",
           strategy_path.c_str(),
           (unsigned long long)total,
           ev_acc.mean,
           ev_acc.stdev(),
           red_rate);
    fflush(stdout);
    return 0;
}
