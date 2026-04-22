/**
 * load_data.cpp — Color-priority strategy for /sphere harvest (oh) — C++.
 *
 * Demonstrates loading a data file via sphere::data::fetch().
 *
 * The strategy loads a JSON file containing color priority weights from data/.
 * On every click it picks the highest-value already-revealed cell that has not
 * yet been clicked, falling back to a random covered cell when no such cell
 * exists.  Purple cells are always clicked first (they are free).
 *
 * The data file is committed directly to data/ (it is tiny), so fetch() finds
 * it locally and skips any network request.  For a real large file (e.g. a
 * 14 GB lookup table) the same fetch() call would download it from the hosted
 * URL on first use and cache it in data/ for all subsequent runs.
 *
 * External data
 * -------------
 * File    : oh_example.json
 * Size    : < 1 KB
 * Source  : https://raw.githubusercontent.com/colblitz/mudae-sphere-games-evaluation/main/data/oh_example.json
 * SHA-256 : fdebc10f5140a1b4ae6643b4626660d86a34847ea8f6d30e617b2c63e7d008c8
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "../../interface/data.h"
#include "../../interface/strategy.h"

using namespace sphere;

// ---------------------------------------------------------------------------
// Data file configuration
// ---------------------------------------------------------------------------

static const char* DATA_URL    = "https://raw.githubusercontent.com/colblitz/mudae-sphere-games-evaluation/main/data/oh_example.json";
static const char* DATA_SHA256 = "fdebc10f5140a1b4ae6643b4626660d86a34847ea8f6d30e617b2c63e7d008c8";
static const char* DATA_FILE   = "oh_example.json";

// ---------------------------------------------------------------------------
// Minimal JSON helpers — extract the color_values object from oh_example.json
// ---------------------------------------------------------------------------

namespace {

// Parse "key": number pairs from a flat JSON object string.
std::map<std::string, int> parse_color_values(const std::string& json) {
    std::map<std::string, int> out;
    const char* p = json.c_str();
    // Find "color_values": { ... }
    const char* cv = strstr(p, "\"color_values\"");
    if (!cv) return out;
    const char* brace = strchr(cv, '{');
    if (!brace) return out;
    const char* end = strchr(brace, '}');
    if (!end) return out;
    // Scan key:value pairs inside the braces
    const char* q = brace + 1;
    while (q < end) {
        const char* ks = strchr(q, '"'); if (!ks || ks >= end) break;
        const char* ke = strchr(ks + 1, '"'); if (!ke || ke >= end) break;
        std::string key(ks + 1, ke - ks - 1);
        const char* colon = strchr(ke + 1, ':'); if (!colon || colon >= end) break;
        int val = atoi(colon + 1);
        out[key] = val;
        q = colon + 1;
        // Advance past digits
        while (q < end && (*q == ' ' || *q == '\t' || (*q >= '0' && *q <= '9'))) ++q;
    }
    return out;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

} // namespace

// ---------------------------------------------------------------------------
// Strategy
// ---------------------------------------------------------------------------

class LoadDataOHStrategy : public OHStrategy {
public:
    LoadDataOHStrategy() : rng_(static_cast<uint64_t>(std::time(nullptr))) {}

    // -----------------------------------------------------------------------
    // Global state: load data file once, share across all games
    // -----------------------------------------------------------------------

    /**
     * Called once before all games.  Loads color priority weights from
     * data/oh_example.json via sphere::data::fetch().
     *
     * fetch() checks whether the file is already present in data/ and its
     * SHA-256 matches.  If so it returns immediately — no download occurs.
     * If the file is absent or corrupted it downloads from DATA_URL.
     *
     * Returns a JSON string encoding the color_values map so the harness
     * can thread it through init_game_payload on every game.
     */
    std::string init_evaluation_run() override {
        std::string path = sphere::data::fetch(DATA_URL, DATA_SHA256, DATA_FILE);
        std::string json = read_file(path);
        color_values_ = parse_color_values(json);
        // Serialize to JSON for the harness state protocol.
        // (The harness re-passes this string to init_game_payload; we re-parse
        //  it there, or just rely on the member variable since there is one
        //  strategy instance for the entire run.)
        return "{}";
    }

    // -----------------------------------------------------------------------
    // Per-game state: seed RNG from game_seed for reproducibility
    // -----------------------------------------------------------------------

    std::string init_game_payload(const std::string& meta_json,
                                  const std::string& /*run_state_json*/) override {
        // Extract game_seed from meta_json: {"clicks_left":5,"max_clicks":5,"game_seed":N}
        uint64_t seed = static_cast<uint64_t>(std::time(nullptr));
        const char* gs = strstr(meta_json.c_str(), "\"game_seed\":");
        if (gs) seed = static_cast<uint64_t>(atoll(gs + 12));
        rng_.seed(seed);
        return "{}";
    }

    // -----------------------------------------------------------------------
    // Click decision: highest-value visible cell, or random covered cell
    // -----------------------------------------------------------------------

    void next_click(const std::vector<Cell>& revealed,
                    const std::string& /*meta_json*/,
                    const std::string& game_state_json,
                    ClickResult& out) override
    {
        bool clicked[25] = {};
        for (const Cell& c : revealed)
            clicked[c.row * 5 + c.col] = true;

        // Purples are free — click any visible purple immediately.
        std::vector<int> purples;
        for (const Cell& c : revealed)
            if (c.color == "spP") purples.push_back(c.row * 5 + c.col);
        if (!purples.empty()) {
            int chosen = purples[std::uniform_int_distribution<int>(0, (int)purples.size() - 1)(rng_)];
            out.row = chosen / 5; out.col = chosen % 5;
            out.game_state_json = game_state_json;
            return;
        }

        // Among revealed-but-unclicked cells, pick the highest-value one.
        int best_val = -1;
        std::vector<int> best_cells;
        for (const Cell& c : revealed) {
            int idx = c.row * 5 + c.col;
            if (clicked[idx]) continue;
            if (c.color == "spB" || c.color == "spT") continue; // info-only
            auto it = color_values_.find(c.color);
            int val = (it != color_values_.end()) ? it->second : 0;
            if (val > best_val) { best_val = val; best_cells.clear(); }
            if (val == best_val) best_cells.push_back(idx);
        }
        if (!best_cells.empty()) {
            int chosen = best_cells[std::uniform_int_distribution<int>(0, (int)best_cells.size() - 1)(rng_)];
            out.row = chosen / 5; out.col = chosen % 5;
            out.game_state_json = game_state_json;
            return;
        }

        // Fall back to a random covered cell.
        std::vector<int> unclicked;
        for (int i = 0; i < 25; ++i)
            if (!clicked[i]) unclicked.push_back(i);
        if (unclicked.empty()) { out.row = 0; out.col = 0; out.game_state_json = game_state_json; return; }
        int chosen = unclicked[std::uniform_int_distribution<int>(0, (int)unclicked.size() - 1)(rng_)];
        out.row = chosen / 5; out.col = chosen % 5;
        out.game_state_json = game_state_json;
    }

private:
    std::mt19937_64 rng_;
    std::map<std::string, int> color_values_;
};

// ---------------------------------------------------------------------------
// C exports required by the harness — do not rename these functions
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new LoadDataOHStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" const char* strategy_init_evaluation_run(void* inst) {
    thread_local static std::string buf;
    buf = static_cast<LoadDataOHStrategy*>(inst)->init_evaluation_run();
    return buf.c_str();
}

extern "C" const char* strategy_init_game_payload(void* inst,
                                                   const char* meta_json,
                                                   const char* game_state_json) {
    thread_local static std::string buf;
    buf = static_cast<LoadDataOHStrategy*>(inst)->init_game_payload(
        meta_json        ? meta_json        : "{}",
        game_state_json  ? game_state_json  : "{}"
    );
    return buf.c_str();
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* revealed_json,
                                            const char* meta_json,
                                            const char* game_state_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<LoadDataOHStrategy*>(inst);

    std::vector<Cell> revealed;
    const char* p = revealed_json;
    while ((p = strstr(p, "\"row\":")) != nullptr) {
        Cell c;
        c.row = atoi(p + 6);
        const char* cp   = strstr(p, "\"col\":");   if (cp)   c.col   = atoi(cp + 6);
        const char* colp = strstr(p, "\"color\":\""); if (colp) {
            colp += 9;
            const char* e = strchr(colp, '"');
            if (e) c.color = std::string(colp, e - colp);
        }
        revealed.push_back(c);
        p += 6;
    }

    ClickResult out;
    s->next_click(revealed, meta_json ? meta_json : "{}", game_state_json ? game_state_json : "{}", out);
    buf = "{\"row\":" + std::to_string(out.row) +
          ",\"col\":" + std::to_string(out.col) +
          ",\"state\":" + out.game_state_json + "}";
    return buf.c_str();
}
