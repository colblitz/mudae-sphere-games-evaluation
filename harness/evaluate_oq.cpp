/**
 * evaluate_oq.cpp — Exhaustive evaluator for /sphere quest (oq).
 *
 * Runs the strategy against all 12,650 valid boards and reports:
 *   ev       — mean score across all boards
 *   stdev    — standard deviation of per-board scores
 *   red_rate — fraction of boards where the red sphere was clicked
 *
 * Board model:
 *   5×5 grid, all cells start covered.  Non-purple click budget: 7.
 *   4 purple spheres (spP) are hidden; their positions determine all other
 *   cell colors via neighbor-count rules:
 *     spB=0 spT=1 spG=2 spY=3 spO=4 purple neighbors
 *   Clicking a purple is FREE (does not cost a click).
 *   Click 3 purples → 4th converts to red (spR, 150 SP) — free click.
 *   After red, remaining budget is spent: derive all hidden colors from
 *   constraints and click greedily in descending value order.
 *
 * Output: JSON to stdout on completion.
 *
 * Usage:
 *   evaluate_oq --strategy path/to/strategy.py [--boards path/to/oq_boards.bin.lzma]
 *               [--threads N]
 */

#include <algorithm>
#include <array>
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

#ifndef REPO_ROOT
#define REPO_ROOT "."
#endif

using namespace sphere;

// ---------------------------------------------------------------------------
// oq game constants
// ---------------------------------------------------------------------------

static constexpr int N_PURPLES  = 4;
static constexpr int MAX_CLICKS = 7;

// Sphere values
static constexpr int VAL_SPP = 5;
static constexpr int VAL_SPR = 150;
static constexpr int VAL_SPB = 10;
static constexpr int VAL_SPT = 20;
static constexpr int VAL_SPG = 35;
static constexpr int VAL_SPY = 55;
static constexpr int VAL_SPO = 90;

// 8-neighbour masks for each cell
static uint32_t g_nb_mask[N_CELLS];

static void build_nb_masks() {
    for (int i = 0; i < N_CELLS; ++i) {
        int r = idx_to_row(i), c = idx_to_col(i);
        uint32_t m = 0;
        for (int dr = -1; dr <= 1; ++dr)
            for (int dc = -1; dc <= 1; ++dc) {
                if (dr == 0 && dc == 0) continue;
                int rr = r + dr, cc = c + dc;
                if (rr >= 0 && rr < GRID_SIZE && cc >= 0 && cc < GRID_SIZE)
                    m |= 1u << rc_to_idx(rr, cc);
            }
        g_nb_mask[i] = m;
    }
}

// Derive cell color from purple_mask and cell index
static const char* oq_cell_color(uint32_t purple_mask, int idx) {
    if ((purple_mask >> idx) & 1) return "spP";
    int n = __builtin_popcount(purple_mask & g_nb_mask[idx]);
    switch (n) {
        case 0: return "spB";
        case 1: return "spT";
        case 2: return "spG";
        case 3: return "spY";
        default: return "spO";
    }
}

static int oq_cell_value(const char* color) {
    if (!strcmp(color, "spR")) return VAL_SPR;
    if (!strcmp(color, "spO")) return VAL_SPO;
    if (!strcmp(color, "spY")) return VAL_SPY;
    if (!strcmp(color, "spG")) return VAL_SPG;
    if (!strcmp(color, "spT")) return VAL_SPT;
    if (!strcmp(color, "spP")) return VAL_SPP;
    return VAL_SPB;
}

// ---------------------------------------------------------------------------
// Simulate one oq game
// ---------------------------------------------------------------------------

static std::pair<double, bool> run_oq_game(
    uint32_t        purple_mask,
    StrategyBridge& strategy,
    std::string&    state_json)
{
    std::vector<Cell> revealed;
    double score      = 0.0;
    bool   red_found  = false;
    int    purples    = 0;
    int    clicks_left = MAX_CLICKS;
    bool   clicked[N_CELLS] = {};
    bool   red_visible = false;

    std::string meta = "{\"clicks_left\":" + std::to_string(clicks_left)
                     + ",\"max_clicks\":" + std::to_string(MAX_CLICKS)
                     + ",\"purples_found\":0}";
    state_json = strategy.init_run(meta, state_json);

    while (clicks_left > 0) {
        // If red is visible, click it immediately (free)
        if (red_visible) {
            // Find red in revealed (it's the 4th purple slot converted)
            for (int i = 0; i < N_CELLS; ++i) {
                if (!clicked[i]) {
                    const char* col = oq_cell_color(purple_mask, i);
                    // After 3 purples clicked, the 4th purple becomes red
                    if ((purple_mask >> i) & 1) {
                        if (purples == 3) {
                            // This is the red cell
                            clicked[i] = true;
                            revealed.push_back({static_cast<int8_t>(idx_to_row(i)),
                                                static_cast<int8_t>(idx_to_col(i)), "spR"});
                            score     += VAL_SPR;
                            red_found  = true;
                            red_visible = false;
                            goto after_red;
                        }
                    }
                }
            }
            after_red:;
            if (!red_found) red_visible = false;
            continue;
        }

        meta = "{\"clicks_left\":" + std::to_string(clicks_left)
             + ",\"max_clicks\":" + std::to_string(MAX_CLICKS)
             + ",\"purples_found\":" + std::to_string(purples) + "}";

        Click c = strategy.next_click(revealed, meta, state_json);
        if (auto* pb = dynamic_cast<PythonBridge*>(&strategy))
            state_json = pb->last_state();

        int idx = rc_to_idx(c.row, c.col);
        if (idx < 0 || idx >= N_CELLS || clicked[idx]) {
            --clicks_left;
            continue;
        }
        clicked[idx] = true;

        const char* color = oq_cell_color(purple_mask, idx);
        bool is_purple = ((purple_mask >> idx) & 1);

        revealed.push_back({static_cast<int8_t>(c.row),
                             static_cast<int8_t>(c.col), color});
        score += oq_cell_value(color);

        if (is_purple) {
            ++purples;
            // Purple click is free
            if (purples == 3) red_visible = true;  // 4th purple becomes red next step
        } else {
            --clicks_left;
        }
    }

    return {score, red_found};
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IOLBF, 0);  // line-buffer stdout so progress streams through pipes
    std::string strategy_path;
    std::string boards_path = std::string(REPO_ROOT) + "/boards/oq_boards.bin.lzma";
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
        fprintf(stderr, "Usage: evaluate_oq --strategy <path> [--boards <path>] [--threads N]\n");
        return 1;
    }

    build_nb_masks();

    printf("Loading boards from %s ...\n", boards_path.c_str());
    fflush(stdout);
    auto boards = load_oq_boards(boards_path);
    if (boards.empty()) {
        fprintf(stderr, "ERROR: failed to load boards from %s\n", boards_path.c_str());
        return 1;
    }
    printf("Loaded %zu boards.\n", boards.size());
    fflush(stdout);

    printf("Loading strategy %s ...\n", strategy_path.c_str());
    fflush(stdout);
    std::unique_ptr<StrategyBridge> bridge;
    try {
        bridge = StrategyBridge::load(strategy_path, "oq");
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
        bridges[t] = StrategyBridge::load(strategy_path, "oq");

    std::vector<std::string> state_jsons(n_threads);
    for (int t = 0; t < n_threads; ++t)
        state_jsons[t] = bridges[t]->init_payload();

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
        auto [score, red_found] = run_oq_game(boards[i], *bridges[tid], state_jsons[tid]);
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

    double red_rate = static_cast<double>(total_red) / static_cast<double>(count_total);
    printf("\nRESULT_JSON: {\"game\":\"oq\","
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
