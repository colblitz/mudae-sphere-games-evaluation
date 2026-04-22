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
 */

#include <algorithm>
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
    std::string&   state_json,
    std::mt19937_64& rng,
    uint64_t       game_seed)
{
    OHColor slot_colors[N_SLOTS];
    bool    revealed[N_SLOTS];
    bool    clicked[N_SLOTS] = {};

    memcpy(slot_colors, board.slot_colors, sizeof(slot_colors));
    memcpy(revealed,    board.revealed,    sizeof(revealed));

    std::vector<Cell> revealed_cells;
    double score         = 0.0;
    bool   clicked_chest = false;
    int    clicks_left   = MAX_CLICKS;

    // Add initially revealed cells
    for (int i = 0; i < N_SLOTS; ++i) {
        if (revealed[i]) {
            revealed_cells.push_back({static_cast<int8_t>(idx_to_row(i)),
                                       static_cast<int8_t>(idx_to_col(i)),
                                       oh_color_name(slot_colors[i])});
            clicked[i] = true;
        }
    }

    std::string meta = "{\"clicks_left\":" + std::to_string(clicks_left)
                     + ",\"max_clicks\":" + std::to_string(MAX_CLICKS)
                     + ",\"game_seed\":"  + std::to_string(game_seed) + "}";
    state_json = strategy.init_game_payload(meta, state_json);

    auto do_reveal = [&](int idx) {
        if (idx < 0 || idx >= N_SLOTS || revealed[idx]) return;
        revealed[idx] = true;
        revealed_cells.push_back({static_cast<int8_t>(idx_to_row(idx)),
                                    static_cast<int8_t>(idx_to_col(idx)),
                                    oh_color_name(slot_colors[idx])});
    };

    // Reveal N random covered cells
    auto reveal_n_covered = [&](int n) {
        std::vector<int> covered;
        for (int i = 0; i < N_SLOTS; ++i)
            if (!revealed[i]) covered.push_back(i);
        std::shuffle(covered.begin(), covered.end(), rng);
        int cnt = std::min(n, (int)covered.size());
        for (int k = 0; k < cnt; ++k) do_reveal(covered[k]);
    };

    while (clicks_left > 0) {
        // Build available (unrevealed and unclicked) cells list — for validity check
        meta = "{\"clicks_left\":" + std::to_string(clicks_left)
             + ",\"max_clicks\":" + std::to_string(MAX_CLICKS) + "}";

        Click c = strategy.next_click(revealed_cells, meta, state_json);
        if (auto* pb = dynamic_cast<PythonBridge*>(&strategy))
            state_json = pb->last_state();

        int idx = rc_to_idx(c.row, c.col);
        if (idx < 0 || idx >= N_SLOTS || clicked[idx]) {
            --clicks_left;
            continue;
        }
        clicked[idx] = true;

        OHColor color = slot_colors[idx];
        bool is_free  = false;

    handle_click:
        switch (color) {
            case OH_SPP:
                // Purple: free click
                score   += oh_base_ev(OH_SPP);
                is_free  = true;
                do_reveal(idx);
                break;

            case OH_SPD: {
                // Dark: transforms stochastically
                OHColor transform = dark_dist.sample(rng);
                do_reveal(idx);  // show it revealed as dark first
                if (transform == OH_SPP) {
                    // Dark → purple: refund the click
                    score   += oh_base_ev(OH_SPP);
                    is_free  = true;
                } else {
                    score += oh_base_ev(transform);
                }
                break;
            }

            case OH_SPB:
                // Blue: reveals 3 covered cells, gives its base value
                score += oh_base_ev(OH_SPB);
                do_reveal(idx);
                reveal_n_covered(3);
                break;

            case OH_SPT:
                // Teal: reveals 1 covered cell
                score += oh_base_ev(OH_SPT);
                do_reveal(idx);
                reveal_n_covered(1);
                break;

            case OH_CHEST:
                // Chest covered cell: high EV
                score += CHEST_EV;
                clicked_chest = true;
                do_reveal(idx);
                break;

            case OH_SPU: {
                // Plain covered cell: resolves to a random color
                OHColor resolved = dark_dist.sample(rng);  // use appearance dist as proxy
                slot_colors[idx] = resolved;
                color = resolved;
                goto handle_click;
            }

            default:
                // Flat: spG spY spL spO spR spW
                score += oh_base_ev(color);
                do_reveal(idx);
                break;
        }

        if (!is_free) --clicks_left;
    }

    return {score, clicked_chest};
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    setvbuf(stdout, nullptr, _IOLBF, 0);  // line-buffer stdout so progress streams through pipes
    std::string strategy_path;
    std::string dark_stats_path = std::string(REPO_ROOT) + "/boards/oh_dark_stats.json";
    uint64_t    n_games         = 100000;
    uint64_t    seed            = 42;
    int         n_threads       = 1;
#ifdef _OPENMP
    n_threads = omp_get_max_threads();
#endif

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--strategy") && i + 1 < argc)  strategy_path  = argv[++i];
        else if (!strcmp(argv[i], "--games")    && i + 1 < argc)  n_games    = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--seed")     && i + 1 < argc)  seed       = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--threads")  && i + 1 < argc)  n_threads  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dark-stats") && i + 1 < argc) dark_stats_path = argv[++i];
    }
    if (strategy_path.empty()) {
        fprintf(stderr, "Usage: evaluate_oh --strategy <path> [--games N] [--seed S] [--threads N]\n");
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

    std::vector<std::string> state_jsons(n_threads);
    for (int t = 0; t < n_threads; ++t)
        state_jsons[t] = bridges[t]->init_evaluation_run();

    std::atomic<uint64_t> done_count(0);
    ProgressReporter prog(n_games, 10000);

    // Release the GIL so OpenMP threads can each re-acquire it via PyGILState_Ensure
    PyThreadState* _tstate = PyEval_SaveThread();

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
        OHGameResult result = run_oh_game(board, dark_dist, *bridges[tid], state_jsons[tid], rng, game_seed);
        if (result.clicked_chest) ++chest_clicked_count[tid];
        ev_acc[tid].update(result.score);

        uint64_t d = done_count.fetch_add(1) + 1;
        if (d % prog.interval == 0)
            prog.report(d, ev_acc[tid].mean);
    }
    prog.done(ev_acc[0].mean);

    // Re-acquire the GIL on the main thread
    PyEval_RestoreThread(_tstate);

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
