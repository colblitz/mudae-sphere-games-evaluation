/**
 * stateful.cpp — Stateful example strategy for /sphere quest (oq).
 *
 * Demonstrates how to use init_game_payload() and the threaded game_state_json payload to
 * maintain information across clicks within a single game.
 *
 * WHY PER-GAME STATE?
 * -------------------
 * The game_state_json string is threaded through every call within a game:
 *
 *   init_game_payload(meta_json, game_state_json)           → new game_state_json for this game
 *   next_click(revealed, meta_json, state0)   → ClickResult{row, col, state1}
 *   next_click(revealed, meta_json, state1)   → ClickResult{row, col, state2}
 *   ...
 *
 * init_game_payload() is called once at the start of EACH game, making it the right
 * place to reset anything that should be fresh for every game.  Whatever JSON
 * string it returns becomes game_state_json for that game's first next_click() call.
 *
 * Contrast with init_evaluation_run(), which is called only ONCE before all games
 * begin — use that for cross-game global tables (see oc/global_state.cpp).
 *
 * STRATEGY LOGIC
 * --------------
 * We track which rows and columns have already been clicked this game.
 * When choosing the next cell, we prefer cells whose row AND column are both
 * new (maximising board coverage).  If no such cell exists we fall back to
 * cells with at least one new axis, then to any unclicked cell.
 *
 * The click history is serialised into game_state_json as two compact bit-masks
 * (one for rows, one for cols) and a click count, accumulated across calls
 * within a game and reset by init_game_payload() at the start of each new game.
 */

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <vector>

#include "../../interface/strategy.h"

using namespace sphere;

// ---------------------------------------------------------------------------
// Tiny JSON helpers (sufficient for our simple state format)
// ---------------------------------------------------------------------------

/** Extract an integer field from a flat JSON object string. */
static int json_get_int(const std::string& json, const char* key, int default_val = 0) {
    std::string needle = std::string("\"") + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return default_val;
    return atoi(json.c_str() + pos + needle.size());
}

/** Build the per-game state JSON string from its components. */
static std::string make_state(int row_mask, int col_mask, int click_count) {
    return "{\"row_mask\":" + std::to_string(row_mask) +
           ",\"col_mask\":" + std::to_string(col_mask) +
           ",\"click_count\":" + std::to_string(click_count) + "}";
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------

class StatefulOQStrategy : public OQStrategy {
public:
    StatefulOQStrategy() : rng_(static_cast<uint64_t>(std::time(nullptr))) {}

    /**
     * Reset per-game tracking at the start of every game.
     *
     * Called once before the first next_click() of each game.  Returns a
     * fresh game_state_json string — click history starts empty.
     *
     * The incoming evaluation_run_state_json here is whatever init_evaluation_run() returned ("{}"),
     * but we discard it and return a fresh state instead.
     */
    std::string init_game_payload(const std::string& /*meta_json*/,
                         const std::string& /*evaluation_run_state_json*/) override {
        return make_state(0, 0, 0);  // row_mask=0, col_mask=0, click_count=0
    }

    /**
     * Choose the next cell, preferring unexplored rows and columns.
     *
     * game_state_json carries the click history from previous calls this game.
     * We update it and write the new version into out.game_state_json.
     */
    void next_click(const std::vector<Cell>& revealed,
                    const std::string& /*meta_json*/,
                    const std::string& game_state_json,
                    ClickResult& out) override
    {
        // Unpack state
        int row_mask   = json_get_int(game_state_json, "row_mask");
        int col_mask   = json_get_int(game_state_json, "col_mask");
        int click_count = json_get_int(game_state_json, "click_count");

        // Build clicked set
        bool clicked[25] = {};
        for (const Cell& c : revealed)
            clicked[c.row * 5 + c.col] = true;

        // Classify unclicked cells
        std::vector<int> new_both, new_one, fallback;
        for (int r = 0; r < 5; ++r) {
            for (int c = 0; c < 5; ++c) {
                if (clicked[r * 5 + c]) continue;
                bool new_r = !(row_mask & (1 << r));
                bool new_c = !(col_mask & (1 << c));
                if (new_r && new_c)      new_both.push_back(r * 5 + c);
                else if (new_r || new_c) new_one .push_back(r * 5 + c);
                else                     fallback .push_back(r * 5 + c);
            }
        }

        auto& candidates = !new_both.empty() ? new_both
                         : !new_one .empty() ? new_one
                                             : fallback;
        if (candidates.empty()) {
            out.row = 0; out.col = 0;
            out.game_state_json = game_state_json;
            return;
        }

        int chosen = candidates[
            std::uniform_int_distribution<int>(0, (int)candidates.size() - 1)(rng_)
        ];
        out.row = chosen / 5;
        out.col = chosen % 5;

        // Update state — this becomes game_state_json on the next call
        out.game_state_json = make_state(
            row_mask   | (1 << out.row),
            col_mask   | (1 << out.col),
            click_count + 1
        );
    }

private:
    std::mt19937_64 rng_;
};

// ---------------------------------------------------------------------------
// C exports required by the harness
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new StatefulOQStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" const char* strategy_init_evaluation_run(void*) { return "{}"; }

extern "C" const char* strategy_init_game_payload(void* inst,
                                          const char* meta_json,
                                          const char* game_state_json)
{
    static std::string buf;
    buf = static_cast<StatefulOQStrategy*>(inst)->init_game_payload(
        meta_json   ? meta_json   : "{}",
        game_state_json  ? game_state_json  : "{}"
    );
    return buf.c_str();
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* revealed_json,
                                            const char* meta_json,
                                            const char* game_state_json)
{
    static std::string buf;
    auto* s = static_cast<StatefulOQStrategy*>(inst);

    std::vector<Cell> revealed;
    const char* p = revealed_json;
    while ((p = strstr(p, "\"row\":")) != nullptr) {
        Cell c;
        c.row = atoi(p + 6);
        const char* cp = strstr(p, "\"col\":"); if (cp) c.col = atoi(cp + 6);
        const char* colp = strstr(p, "\"color\":\"");
        if (colp) { colp += 9; const char* e = strchr(colp, '"'); if (e) c.color = std::string(colp, e - colp); }
        revealed.push_back(c); p += 6;
    }

    ClickResult out;
    s->next_click(revealed, meta_json ? meta_json : "{}", game_state_json ? game_state_json : "{}", out);
    buf = "{\"row\":" + std::to_string(out.row) +
          ",\"col\":" + std::to_string(out.col) +
          ",\"state\":" + out.game_state_json + "}";
    return buf.c_str();
}
