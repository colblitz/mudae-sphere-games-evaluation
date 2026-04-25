/**
 * evaluate_oh.cpp — Monte Carlo evaluator for /sphere harvest (oh).
 *
 * Runs the strategy against N randomly generated boards (default 100,000)
 * and reports:
 *   ev       — mean score across all games
 *   stdev    — standard deviation of per-game scores
 *   oc_rate  — fraction of games where the strategy actually clicked the chest cell
 *
 * Board model (corrected, matches cortana3 evaluate_harvest_strategies.cpp):
 *   25 cells: 10 are revealed at game start, 15 start covered (spU).
 *   The 10 initially revealed cells are included in `revealed` on the first
 *   next_click call — strategies can read them before making any decision.
 *   50% of boards have one "chest" covered cell worth ~345 SP on average.
 *   Blue  (spB) reveals 3 random covered cells when clicked.
 *   Teal  (spT) reveals 1 random covered cell when clicked.
 *   Purple (spP) clicks are FREE (do not cost a click from the budget).
 *   Dark  (spD) transforms into another color stochastically when clicked.
 *   Dark → purple: the click is refunded (free click on the transform result).
 *   Light (spL) has a fixed mean value (~76 SP) — treated as flat for simulation.
 *   Covered (spU) resolves stochastically when clicked directly.
 *   Chest covered cell: resolves to a high-value outcome (~345 SP average).
 *
 * Cell appearance rates and dark transform distribution are loaded from
 * boards/oh_dark_stats.json.  Covered cell resolution uses a flat
 * approximation based on the same appearance rates.
 *
 * Output: JSON to stdout on completion.
 *
 * Usage:
 *   evaluate_oh --strategy <path> [--games N] [--seed S] [--threads N]
 *               [--dark-stats path/to/oh_dark_stats.json]
 *               [--trace N]   (trace mode: sample N games, emit TRACE_JSON)
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
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
// oh color constants
// ---------------------------------------------------------------------------

enum OHColor : int8_t {
    OH_SPB    = 0,
    OH_SPT    = 1,
    OH_SPG    = 2,
    OH_SPY    = 3,
    OH_SPL    = 4,
    OH_SPO    = 5,
    OH_SPR    = 6,
    OH_SPW    = 7,
    OH_SPP    = 8,
    OH_SPD    = 9,
    OH_CHEST  = 10,  // special: chest covered cell
    OH_SPU    = 11,  // plain covered cell
    OH_UNKNOWN = -1,
};

static constexpr int N_SLOTS          = 25;
static constexpr int N_REVEALED_START = 10;
static constexpr int N_COV_START      = 15;
static constexpr int MAX_CLICKS       = 5;
static constexpr double CHEST_BOARD_PROB = 0.5;
static constexpr double CHEST_EV         = 344.7;

static const char* oh_color_name(OHColor c) {
    switch (c) {
        case OH_SPB:   return "spB";
        case OH_SPT:   return "spT";
        case OH_SPG:   return "spG";
        case OH_SPY:   return "spY";
        case OH_SPL:   return "spL";
        case OH_SPO:   return "spO";
        case OH_SPR:   return "spR";
        case OH_SPW:   return "spW";
        case OH_SPP:   return "spP";
        case OH_SPD:   return "spD";
        case OH_SPU:   return "spU";
        default:       return "spU";
    }
}

static OHColor oh_color_from_name(const std::string& s) {
    if (s == "spB") return OH_SPB;
    if (s == "spT") return OH_SPT;
    if (s == "spG") return OH_SPG;
    if (s == "spY") return OH_SPY;
    if (s == "spL") return OH_SPL;
    if (s == "spO") return OH_SPO;
    if (s == "spR" || s == "sp") return OH_SPR;
    if (s == "spW") return OH_SPW;
    if (s == "spP") return OH_SPP;
    if (s == "spD") return OH_SPD;
    return OH_SPU;
}

// Base point values
static double oh_base_ev(OHColor c) {
    switch (c) {
        case OH_SPB:  return 10.0;
        case OH_SPT:  return 20.0;
        case OH_SPG:  return 35.0;
        case OH_SPY:  return 55.0;
        case OH_SPL:  return 76.0;
        case OH_SPO:  return 90.0;
        case OH_SPR:  return 150.0;
        case OH_SPW:  return 300.0;
        case OH_SPP:  return 5.0;
        case OH_SPD:  return 104.0;
        case OH_CHEST: return CHEST_EV;
        default:      return 15.0;  // covered cell: avg of non-chest revealed EVs
    }
}

// ---------------------------------------------------------------------------
// Sampling distribution
// ---------------------------------------------------------------------------

struct Dist {
    std::vector<OHColor> colors;
    std::vector<double>  cum;

    void build(const std::map<std::string, double>& rates) {
        colors.clear(); cum.clear();
        std::vector<double> probs;
        double total = 0.0;
        for (auto& kv : rates) {
            OHColor c = oh_color_from_name(kv.first);
            if (c == OH_SPU || c == OH_CHEST) continue;  // not in appearance dist
            if (kv.second <= 0.0) continue;
            colors.push_back(c);
            probs.push_back(kv.second);
            total += kv.second;
        }
        double acc = 0.0;
        for (size_t i = 0; i < probs.size(); ++i) {
            acc += probs[i] / total;
            cum.push_back(acc);
        }
    }

    OHColor sample(std::mt19937_64& rng) const {
        if (colors.empty()) return OH_SPG;
        double r = std::uniform_real_distribution<double>(0.0, 1.0)(rng);
        size_t lo = 0, hi = cum.size() - 1;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (cum[mid] < r) lo = mid + 1; else hi = mid;
        }
        return colors[lo];
    }
};

// ---------------------------------------------------------------------------
// Minimal JSON parsing for oh_dark_stats.json
// ---------------------------------------------------------------------------

static std::string read_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return "";
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); rewind(f);
    std::string buf(static_cast<size_t>(sz), '\0');
    size_t n = fread(&buf[0], 1, static_cast<size_t>(sz), f);
    fclose(f);
    buf.resize(n);
    return buf;
}

// Parse {"distribution": {"spX": {"rate": N}, ...}} → map<color, rate>
static std::map<std::string, double> parse_distribution(const std::string& json) {
    std::map<std::string, double> result;
    const char* p = strstr(json.c_str(), "\"distribution\"");
    if (!p) return result;
    p = strchr(p, '{');
    if (!p) return result;
    ++p;
    while (*p && *p != '}') {
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',' || *p == '\r')) ++p;
        if (*p != '"') break;
        ++p;
        const char* ns = p;
        while (*p && *p != '"') ++p;
        std::string name(ns, p - ns);
        if (*p) ++p;
        while (*p && *p != '{') ++p;
        if (*p != '{') break;
        const char* inner = p + 1;
        const char* rp = strstr(inner, "\"rate\"");
        double rate = 0.0;
        if (rp) {
            rp = strchr(rp, ':');
            if (rp) rate = strtod(rp + 1, nullptr);
        }
        result[name] = rate;
        int depth = 1; ++p;
        while (*p && depth > 0) {
            if (*p == '{') ++depth;
            else if (*p == '}') --depth;
            ++p;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// oh board generation
// ---------------------------------------------------------------------------

struct OHBoard {
    OHColor slot_colors[N_SLOTS];
    bool    revealed[N_SLOTS];
    bool    has_chest;
};

static OHBoard make_oh_board(const Dist& appearance_dist, std::mt19937_64& rng) {
    OHBoard b;
    b.has_chest = false;

    // Assign colors to all 25 cells
    for (int i = 0; i < N_SLOTS; ++i)
        b.slot_colors[i] = appearance_dist.sample(rng);

    // Chest board: 50% chance
    if (std::uniform_real_distribution<double>(0.0, 1.0)(rng) < CHEST_BOARD_PROB) {
        int chest_slot = N_REVEALED_START
            + std::uniform_int_distribution<int>(0, N_COV_START - 1)(rng);
        b.slot_colors[chest_slot] = OH_CHEST;
        b.has_chest = true;
    }

    // First 10 start revealed, last 15 start covered
    for (int i = 0; i < N_REVEALED_START; ++i) b.revealed[i] = true;
    for (int i = N_REVEALED_START; i < N_SLOTS;  ++i) b.revealed[i] = false;

    return b;
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
    bool   is_free;
};

struct GameTrace {
    int    game_index;
    uint64_t game_seed;
    double score;
    bool   has_chest;
    std::string initial_board[N_SLOTS];  // color name for revealed, "?" for covered
    std::string actual_board[N_SLOTS];   // true color of every cell
    std::vector<MoveRecord> moves;
};

// ---------------------------------------------------------------------------
// Simulate one oh game
// ---------------------------------------------------------------------------

struct OHGameResult {
    double score;
    bool   clicked_chest;
};

static OHGameResult run_oh_game(
    const OHBoard& board,
    const Dist&    dark_dist,
    StrategyBridge& strategy,
    std::mt19937_64& rng,
    uint64_t       game_seed,
    GameTrace*     trace = nullptr)
{
    OHColor slot_colors[N_SLOTS];
    memcpy(slot_colors, board.slot_colors, sizeof(slot_colors));

    // Full 25-cell board passed to strategy each turn.
    // Initially: all cells start as (color="spU", clicked=false).
    // The 10 pre-revealed cells get their real color but clicked=false
    // (they are visible but not disabled — the strategy can click them).
    std::array<Cell, N_CELLS> game_board;
    for (int i = 0; i < N_CELLS; ++i) {
        game_board[i].row     = static_cast<int8_t>(idx_to_row(i));
        game_board[i].col     = static_cast<int8_t>(idx_to_col(i));
        game_board[i].color   = "spU";
        game_board[i].clicked = false;
    }
    // Set real colors for the 10 initially-visible cells
    for (int i = 0; i < N_SLOTS; ++i) {
        if (board.revealed[i]) {
            game_board[i].color = oh_color_name(slot_colors[i]);
            // clicked stays false — visible but not spent
        }
    }

    double score         = 0.0;
    bool   clicked_chest = false;
    int    clicks_left   = MAX_CLICKS;
    int    move_num      = 0;

    std::string meta = "{\"clicks_left\":" + std::to_string(clicks_left)
                     + ",\"max_clicks\":" + std::to_string(MAX_CLICKS)
                     + ",\"game_seed\":"  + std::to_string(game_seed) + "}";
    strategy.init_game_payload(meta);

    // do_reveal: passively reveals a cell (sets color, does NOT set clicked)
    auto do_reveal = [&](int idx) {
        if (idx < 0 || idx >= N_SLOTS || game_board[idx].color != "spU") return;
        game_board[idx].color = oh_color_name(slot_colors[idx]);
        // clicked stays false — passive reveal does not disable the cell
    };

    // Reveal N random covered cells
    auto reveal_n_covered = [&](int n) {
        std::vector<int> covered;
        for (int i = 0; i < N_SLOTS; ++i)
            if (game_board[i].color == "spU") covered.push_back(i);
        std::shuffle(covered.begin(), covered.end(), rng);
        int cnt = std::min(n, (int)covered.size());
        for (int k = 0; k < cnt; ++k) do_reveal(covered[k]);
    };

    while (clicks_left > 0) {
        meta = "{\"clicks_left\":" + std::to_string(clicks_left)
             + ",\"max_clicks\":" + std::to_string(MAX_CLICKS) + "}";
        int clicks_before = clicks_left;

        std::vector<Cell> board_vec(game_board.begin(), game_board.end());
        Click c = strategy.next_click(board_vec, meta);

        int idx = rc_to_idx(c.row, c.col);
        if (idx < 0 || idx >= N_SLOTS || game_board[idx].clicked) {
            --clicks_left;
            continue;
        }
        game_board[idx].clicked = true;

        OHColor color = slot_colors[idx];
        bool is_free  = false;
        double delta  = 0.0;
        std::string color_reported;

    handle_click:
        switch (color) {
            case OH_SPP:
                // Purple: free click
                delta        = oh_base_ev(OH_SPP);
                score       += delta;
                is_free      = true;
                color_reported = "spP";
                do_reveal(idx);
                break;

            case OH_SPD: {
                // Dark: transforms stochastically
                OHColor transform = dark_dist.sample(rng);
                do_reveal(idx);  // show it revealed as dark first
                if (transform == OH_SPP) {
                    // Dark → purple: refund the click
                    delta        = oh_base_ev(OH_SPP);
                    score       += delta;
                    is_free      = true;
                    color_reported = "spD→spP";
                } else {
                    delta        = oh_base_ev(transform);
                    score       += delta;
                    color_reported = std::string("spD→") + oh_color_name(transform);
                }
                break;
            }

            case OH_SPB:
                // Blue: reveals 3 covered cells, gives its base value
                delta        = oh_base_ev(OH_SPB);
                score       += delta;
                color_reported = "spB";
                do_reveal(idx);
                reveal_n_covered(3);
                break;

            case OH_SPT:
                // Teal: reveals 1 covered cell
                delta        = oh_base_ev(OH_SPT);
                score       += delta;
                color_reported = "spT";
                do_reveal(idx);
                reveal_n_covered(1);
                break;

            case OH_CHEST:
                // Chest covered cell: high EV
                delta        = CHEST_EV;
                score       += delta;
                clicked_chest = true;
                color_reported = "chest";
                do_reveal(idx);
                break;

            case OH_SPU: {
                // Plain covered cell: resolves to a random color
                OHColor resolved = dark_dist.sample(rng);  // use appearance dist as proxy
                slot_colors[idx] = resolved;
                game_board[idx].color = oh_color_name(resolved);  // update board color
                color = resolved;
                goto handle_click;
            }

            default:
                // Flat: spG spY spL spO spR spW
                delta        = oh_base_ev(color);
                score       += delta;
                color_reported = oh_color_name(color);
                do_reveal(idx);
                break;
        }

        ++move_num;
        if (trace) {
            MoveRecord mr;
            mr.move_num           = move_num;
            mr.row                = c.row;
            mr.col                = c.col;
            mr.color              = color_reported;
            mr.sp_delta           = delta;
            mr.running_score      = score;
            mr.clicks_left_before = clicks_before;
            mr.is_free            = is_free;
            trace->moves.push_back(mr);
        }

        if (!is_free) --clicks_left;
    }

    return {score, clicked_chest};
}

// ---------------------------------------------------------------------------
// JSON helpers for trace output
// ---------------------------------------------------------------------------

static void escape_json_string(const std::string& s, char* buf, size_t bufsz) {
    size_t out = 0;
    for (char ch : s) {
        if (out + 4 >= bufsz) break;
        if (ch == '"' || ch == '\\') buf[out++] = '\\';
        buf[out++] = ch;
    }
    buf[out] = '\0';
}

static void print_trace_json(const std::vector<GameTrace>& traces) {
    printf("TRACE_JSON: [");
    for (size_t gi = 0; gi < traces.size(); ++gi) {
        const auto& t = traces[gi];
        if (gi > 0) printf(",");
        printf("{\"game_index\":%d,\"game_seed\":%llu,\"score\":%.1f,"
               "\"has_chest\":%s,\"initial_board\":[",
               t.game_index,
               (unsigned long long)t.game_seed,
               t.score,
               t.has_chest ? "true" : "false");
        for (int i = 0; i < N_SLOTS; ++i) {
            if (i > 0) printf(",");
            printf("\"%s\"", t.initial_board[i].c_str());
        }
        printf("],\"actual_board\":[");
        for (int i = 0; i < N_SLOTS; ++i) {
            if (i > 0) printf(",");
            printf("\"%s\"", t.actual_board[i].c_str());
        }
        printf("],\"moves\":[");
        for (size_t mi = 0; mi < t.moves.size(); ++mi) {
            const auto& m = t.moves[mi];
            if (mi > 0) printf(",");
            char cbuf[64];
            escape_json_string(m.color, cbuf, sizeof(cbuf));
            printf("{\"move_num\":%d,\"row\":%d,\"col\":%d,"
                   "\"color\":\"%s\",\"sp_delta\":%.1f,\"running_score\":%.1f,"
                   "\"meta\":{\"clicks_left\":%d,\"max_clicks\":%d},"
                   "\"free\":%s}",
                   m.move_num, m.row, m.col,
                   cbuf, m.sp_delta, m.running_score,
                   m.clicks_left_before, MAX_CLICKS,
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
    std::string dark_stats_path = std::string(REPO_ROOT) + "/boards/oh_dark_stats.json";
    uint64_t    n_games         = 1000000;
    uint64_t    seed            = 42;
    int         n_threads       = 1;
#ifdef _OPENMP
    n_threads = omp_get_max_threads();
#endif
    int trace_n = 0;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--strategy") && i + 1 < argc)  strategy_path  = argv[++i];
        else if (!strcmp(argv[i], "--games")    && i + 1 < argc)  n_games    = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--seed")     && i + 1 < argc)  seed       = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--threads")  && i + 1 < argc)  n_threads  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dark-stats") && i + 1 < argc) dark_stats_path = argv[++i];
        else if (!strcmp(argv[i], "--trace")    && i + 1 < argc)  trace_n    = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--boards-dir") && i + 1 < argc) {
            // derive dark-stats path from boards-dir
            dark_stats_path = std::string(argv[++i]) + "/oh_dark_stats.json";
        }
    }
    if (strategy_path.empty()) {
        fprintf(stderr, "Usage: evaluate_oh --strategy <path> [--games N] [--seed S] [--threads N]\n"
                        "                   [--trace N]\n");
        return 1;
    }

    // Load dark stats
    printf("Loading dark stats from %s ...\n", dark_stats_path.c_str());
    fflush(stdout);
    std::string dark_json = read_file(dark_stats_path);
    if (dark_json.empty()) {
        fprintf(stderr, "ERROR: cannot read %s\n", dark_stats_path.c_str());
        return 1;
    }
    auto dark_rates = parse_distribution(dark_json);
    // Build appearance distribution from dark stats (both use similar color sets)
    Dist appearance_dist, dark_dist;
    appearance_dist.build(dark_rates);
    dark_dist.build(dark_rates);

    // Load strategy
    printf("Loading strategy %s ...\n", strategy_path.c_str());
    fflush(stdout);
    std::unique_ptr<StrategyBridge> bridge;
    try {
        bridge = StrategyBridge::load(strategy_path, "oh");
    } catch (const std::exception& e) {
        fprintf(stderr, "ERROR: %s\n", e.what());
        return 1;
    }

    // ------------------------------------------------------------------
    // TRACE MODE
    // ------------------------------------------------------------------
    if (trace_n > 0) {
        printf("Trace mode: sampling %d games (seed=%llu) ...\n",
               trace_n, (unsigned long long)seed);
        fflush(stdout);

        bridge->init_evaluation_run();
        std::vector<GameTrace> traces;
        traces.reserve(trace_n);

        for (int gi = 0; gi < trace_n; ++gi) {
            uint64_t game_seed = seed ^ (static_cast<uint64_t>(gi) * 6364136223846793005ULL + 1442695040888963407ULL);
            std::mt19937_64 rng(game_seed);
            OHBoard board = make_oh_board(appearance_dist, rng);

            GameTrace gt;
            gt.game_index = gi;
            gt.game_seed  = game_seed;
            gt.has_chest  = board.has_chest;
            // Build initial board: revealed cells show color, covered show "?"
            // actual_board reveals the true color of every cell
            for (int i = 0; i < N_SLOTS; ++i) {
                gt.actual_board[i]  = oh_color_name(board.slot_colors[i]);
                gt.initial_board[i] = board.revealed[i]
                    ? gt.actual_board[i] : "?";
            }

            auto result = run_oh_game(board, dark_dist, *bridge, rng, game_seed, &gt);
            gt.score = result.score;
            traces.push_back(std::move(gt));
        }

        print_trace_json(traces);
        return 0;
    }

    // ------------------------------------------------------------------
    // NORMAL EVALUATION MODE
    // ------------------------------------------------------------------
    printf("Strategy loaded. Running %llu games (seed=%llu, threads=%d) ...\n",
           (unsigned long long)n_games, (unsigned long long)seed, n_threads);
    fflush(stdout);

    // Per-thread accumulators
    std::vector<Welford>  ev_acc(n_threads);
    std::vector<uint64_t> chest_clicked_count(n_threads, 0);

    // Per-thread strategy bridges and state (each thread needs its own instance)
    std::vector<std::unique_ptr<StrategyBridge>> bridges(n_threads);
    bridges[0] = std::move(bridge);
    for (int t = 1; t < n_threads; ++t)
        bridges[t] = StrategyBridge::load(strategy_path, "oh");

    for (int t = 0; t < n_threads; ++t)
        bridges[t]->init_evaluation_run();

    std::atomic<uint64_t> done_count(0);
    ProgressReporter prog(n_games, 10000);

    // Release the GIL so OpenMP threads can each re-acquire it via PyGILState_Ensure
    PyThreadState* _tstate = Py_IsInitialized() ? PyEval_SaveThread() : nullptr;

#ifdef _OPENMP
    omp_set_num_threads(n_threads);
    #pragma omp parallel for schedule(dynamic, 1024)
#endif
    for (int64_t g = 0; g < static_cast<int64_t>(n_games); ++g) {
#ifdef _OPENMP
        int tid = omp_get_thread_num();
#else
        int tid = 0;
#endif
        // Each game uses a unique seed derived from the master seed and game index,
        // ensuring deterministic results for the same (seed, n_games) regardless of
        // thread count or scheduling.
        uint64_t game_seed = seed ^ (static_cast<uint64_t>(g) * 6364136223846793005ULL + 1442695040888963407ULL);
        std::mt19937_64 rng(game_seed);
        OHBoard board = make_oh_board(appearance_dist, rng);
        // game_seed is forwarded into run_oh_game → init_game_payload meta so strategies can
        // seed their own RNG deterministically, producing identical results across runs.
        OHGameResult result{};
        try {
            result = run_oh_game(board, dark_dist, *bridges[tid], rng, game_seed);
        } catch (const std::exception& e) {
            fprintf(stderr, "\nERROR on game %lld (seed=%llu): %s\n",
                    (long long)g, (unsigned long long)game_seed, e.what());
            exit(1);
        }
        if (result.clicked_chest) ++chest_clicked_count[tid];
        ev_acc[tid].update(result.score);

        uint64_t d = done_count.fetch_add(1) + 1;
        if (d % prog.interval == 0)
            prog.report(d, ev_acc[tid].mean);
    }
    prog.done(ev_acc[0].mean);

    // Re-acquire the GIL on the main thread
    if (_tstate) PyEval_RestoreThread(_tstate);

    // Merge per-thread accumulators (Chan's parallel Welford)
    double   mean_total = 0.0, M2_total = 0.0;
    uint64_t count_total = 0;
    uint64_t total_chest_clicks = 0;
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
        total_chest_clicks += chest_clicked_count[t];
    }
    double stdev_total = count_total > 1
        ? std::sqrt(M2_total / static_cast<double>(count_total - 1)) : 0.0;

    double oc_rate = static_cast<double>(total_chest_clicks) / static_cast<double>(count_total);
    printf("\nRESULT_JSON: {\"game\":\"oh\","
           "\"strategy\":\"%s\","
           "\"n_games\":%llu,"
           "\"seed\":%llu,"
           "\"ev\":%.4f,"
           "\"stdev\":%.4f,"
           "\"oc_rate\":%.4f}\n",
           strategy_path.c_str(),
           (unsigned long long)count_total,
           (unsigned long long)seed,
           mean_total,
           stdev_total,
           oc_rate);
    fflush(stdout);
    return 0;
}
