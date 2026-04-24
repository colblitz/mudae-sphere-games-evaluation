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
 *   Click 3 purples → 4th converts to red (spR, 150 SP) — costs 1 click.
 *   The strategy sees spR in revealed and decides when to click it.
 *
 * Output: JSON to stdout on completion.
 *
 * Usage:
 *   evaluate_oq --strategy path/to/strategy.py [--boards path/to/oq_boards.bin.lzma]
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
#include <cstdint>
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

// Derive cell color from purple_mask and cell index.
// purples_clicked is the number of purples already clicked before this cell;
// if it equals 3, the 4th purple appears as red (spR) to the strategy.
static const char* oq_cell_color(uint32_t purple_mask, int idx, int purples_clicked) {
    if ((purple_mask >> idx) & 1) return purples_clicked >= 3 ? "spR" : "spP";
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
// Trace record
// ---------------------------------------------------------------------------

struct MoveRecord {
    int    move_num;
    int    row, col;
    std::string color;
    double sp_delta;
    double running_score;
    int    clicks_left_before;
    int    purples_found_before;
    bool   is_free;
};

struct GameTrace {
    int    board_index;
    double score;
    std::string initial_board[N_CELLS];  // all "?" for oq
    std::string actual_board[N_CELLS];   // true color of every cell
    std::vector<MoveRecord> moves;
};

// ---------------------------------------------------------------------------
// Simulate one oq game
// ---------------------------------------------------------------------------

static std::pair<double, bool> run_oq_game(
    uint32_t        purple_mask,
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
    int    purples    = 0;
    int    clicks_left = MAX_CLICKS;
    int    move_num = 0;

    std::string meta = "{\"clicks_left\":" + std::to_string(clicks_left)
                     + ",\"max_clicks\":" + std::to_string(MAX_CLICKS)
                     + ",\"purples_found\":0}";
    strategy.init_game_payload(meta);

    while (clicks_left > 0) {
        int clicks_before  = clicks_left;
        int purples_before = purples;
        meta = "{\"clicks_left\":" + std::to_string(clicks_left)
             + ",\"max_clicks\":" + std::to_string(MAX_CLICKS)
             + ",\"purples_found\":" + std::to_string(purples) + "}";

        std::vector<Cell> board_vec(game_board.begin(), game_board.end());
        Click c = strategy.next_click(board_vec, meta);

        int idx = rc_to_idx(c.row, c.col);
        if (idx < 0 || idx >= N_CELLS || game_board[idx].clicked) {
            --clicks_left;
            continue;
        }

        // Pass purples already clicked so the 4th purple shows as spR
        const char* color = oq_cell_color(purple_mask, idx, purples);
        bool is_purple = ((purple_mask >> idx) & 1);
        int  value     = oq_cell_value(color);

        game_board[idx].color   = color;
        game_board[idx].clicked = true;
        score += value;
        ++move_num;

        if (is_purple) {
            if (purples == 3) {
                red_found = true;  // this click was the red (4th purple)
                --clicks_left;     // spR costs a click (unlike spP which is free)
            }
            ++purples;
            // Only the first 3 purples (spP) are free; spR costs a click

            // Auto-reveal spR: after the 3rd purple is clicked (purples now == 3),
            // find the remaining unclicked purple in purple_mask and reveal it as spR
            if (purples == 3) {
                for (int bit = 0; bit < N_CELLS; ++bit) {
                    if (((purple_mask >> bit) & 1) && !game_board[bit].clicked) {
                        game_board[bit].color   = "spR";
                        game_board[bit].clicked = false;  // visible but not spent
                        break;
                    }
                }
            }
        } else {
            --clicks_left;
        }

        if (trace) {
            MoveRecord mr;
            mr.move_num              = move_num;
            mr.row                   = c.row;
            mr.col                   = c.col;
            mr.color                 = color;
            mr.sp_delta              = value;
            mr.running_score         = score;
            mr.clicks_left_before    = clicks_before;
            mr.purples_found_before  = purples_before;
            mr.is_free               = is_purple;
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
                   "\"meta\":{\"clicks_left\":%d,\"max_clicks\":%d,"
                   "\"purples_found\":%d},"
                   "\"free\":%s}",
                   m.move_num, m.row, m.col,
                   m.color.c_str(), m.sp_delta, m.running_score,
                   m.clicks_left_before, MAX_CLICKS,
                   m.purples_found_before,
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
    std::string boards_path = std::string(REPO_ROOT) + "/boards/oq_boards.bin.lzma";
    int         n_threads   = 1;
#ifdef _OPENMP
    n_threads = omp_get_max_threads();
#endif
    int      trace_n    = 0;
    uint64_t trace_seed = 42;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--strategy") && i + 1 < argc) strategy_path = argv[++i];
        else if (!strcmp(argv[i], "--boards")  && i + 1 < argc) boards_path = argv[++i];
        else if (!strcmp(argv[i], "--boards-dir") && i + 1 < argc) {
            boards_path = std::string(argv[++i]) + "/oq_boards.bin.lzma";
        }
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) n_threads   = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trace")   && i + 1 < argc) trace_n     = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed")    && i + 1 < argc) trace_seed  = strtoull(argv[++i], nullptr, 10);
    }
    if (strategy_path.empty()) {
        fprintf(stderr, "Usage: evaluate_oq --strategy <path> [--boards <path>] [--threads N]\n"
                        "                   [--trace N] [--seed S]\n");
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

    // ------------------------------------------------------------------
    // TRACE MODE
    // ------------------------------------------------------------------
    if (trace_n > 0) {
        printf("Trace mode: sampling %d boards (seed=%llu) ...\n",
               trace_n, (unsigned long long)trace_seed);
        fflush(stdout);

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
            for (int c = 0; c < N_CELLS; ++c) {
                gt.initial_board[c] = "?";
                // Derive true color: purples show as spP (pass purples_clicked=0)
                gt.actual_board[c]  = oq_cell_color(boards[idx], c, 0);
            }

            auto [score, _red] = run_oq_game(boards[idx], *bridge, &gt);
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
        bridges[t] = StrategyBridge::load(strategy_path, "oq");

    for (int t = 0; t < n_threads; ++t)
        bridges[t]->init_evaluation_run();

    std::vector<Welford>  ev_acc(n_threads);
    std::vector<uint64_t> red_count(n_threads, 0);

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
            auto [s, r] = run_oq_game(boards[i], *bridges[tid]);
            score = s; red_found = r;
        } catch (const std::exception& e) {
            fprintf(stderr, "\nERROR on board %lld: %s\n", (long long)i, e.what());
            exit(1);
        }
        ev_acc[tid].update(score);
        if (red_found) ++red_count[tid];

        uint64_t d = done_count.fetch_add(1) + 1;
        if (d % prog.interval == 0)
            prog.report(d, ev_acc[tid].mean);
    }
    prog.done(ev_acc[0].mean);

    // Re-acquire the GIL on the main thread
    if (_tstate) PyEval_RestoreThread(_tstate);

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
