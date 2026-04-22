/**
 * oq_template.cpp — Template strategy for /sphere quest (oq) — C++.
 *
 * Copy this file to strategies/oq/<your_name>.cpp, rename the class, and
 * fill in the TODO sections.  Delete any methods you don't need — only
 * next_click() is required.
 *
 * GAME OVERVIEW
 * -------------
 * Grid   : 5×5, all 25 cells start covered
 * Budget : 7 non-purple clicks
 * Goal   : find 3 of the 4 hidden purple spheres → the 4th converts to red
 *          (spR, 150 SP).  Collect red and spend remaining budget on
 *          high-value tiles.
 *
 * Special click rules:
 *   Purple (spP) : click is FREE (does not consume a click)
 *   Red (spR)    : appears after 3 purples clicked; click is also FREE
 *
 * Non-purple cells reveal the count of purple neighbours (orthogonal +
 * diagonal, capped at 4) as a color:
 *   spB = 0 purple neighbours
 *   spT = 1 purple neighbour
 *   spG = 2 purple neighbours
 *   spY = 3 purple neighbours
 *   spO = 4 purple neighbours
 *
 * COLOR REFERENCE (oq)
 * --------------------
 *   spP   purple  5 SP each; 4 hidden per board; click is FREE
 *   spR   red     150 SP; appears after 3 purples found; click is FREE
 *   spB   blue    0 purple neighbours
 *   spT   teal    1 purple neighbour
 *   spG   green   2 purple neighbours
 *   spY   yellow  3 purple neighbours
 *   spO   orange  4 purple neighbours
 *
 * STATE / JSON PROTOCOL
 * ---------------------
 * The harness serialises state as a JSON string threaded through every call:
 *
 *   init_evaluation_run()                 → game_state_json (once before all games)
 *   init_game_payload(meta_json, state0)    → state1     (once per game)
 *   next_click(revealed, meta, s1) → ClickResult{row, col, state2}
 *   ...
 *
 * meta_json keys (oq):
 *   "clicks_left"    int   remaining non-purple budget
 *   "max_clicks"     int   total non-purple budget (always 7)
 *   "purples_found"  int   purples clicked so far (0–3)
 *
 * See also
 * --------
 * oh/random_clicks.cpp   — minimal stateless example
 * oc/global_state.cpp    — init_evaluation_run() (global state) example
 * oq/stateful.cpp        — init_game_payload() + state threading (per-game state) example
 */

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <vector>

#include "../../interface/strategy.h"

using namespace sphere;

class MyOQStrategy : public OQStrategy {
public:
    MyOQStrategy() : rng_(static_cast<uint64_t>(std::time(nullptr))) {}

    // -----------------------------------------------------------------------
    // Optional: global state (computed once, shared across ALL games)
    // -----------------------------------------------------------------------

    /**
     * Return the initial state JSON — called ONCE before all games.
     *
     * Compute anything that is board-independent and expensive to repeat.
     * Default: "{}" (empty object).
     */
    std::string init_evaluation_run() override {
        // TODO: replace with your global precomputation, or delete this method
        return "{}";
    }

    // -----------------------------------------------------------------------
    // Optional: per-game initialisation
    // -----------------------------------------------------------------------

    /**
     * Set up per-game state — called once before each game's first click.
     *
     * @param meta_json   JSON: {"clicks_left":N,"max_clicks":7,"purples_found":0}
     * @param evaluation_run_state_json  Read-only value from init_evaluation_run().
     * @return            Initial game_state_json for this game's first next_click.
     */
    std::string init_game_payload(const std::string& meta_json,
                         const std::string& evaluation_run_state_json) override {
        // TODO: reset per-game fields here, or delete this method
        (void)meta_json;
        return game_state_json;
    }

    // -----------------------------------------------------------------------
    // Required: click decision
    // -----------------------------------------------------------------------

    /**
     * Choose the next cell to click.
     *
     * @param revealed    All cells revealed so far (.row, .col, .color).
     * @param meta_json   JSON: {"clicks_left":N,"max_clicks":7,"purples_found":K}
     * @param game_state_json  Value from the previous next_click (or init_game_payload).
     * @param out         Fill in out.row, out.col, out.game_state_json.
     *
     * Tips:
     *   - Always click purple ("spP") cells immediately — they are free.
     *   - Always click red ("spR") when it appears — it is also free.
     *   - Each non-purple reveal gives a neighbour count.  Use it to
     *     eliminate or confirm candidate purple positions (like Minesweeper).
     *   - A "spO" (4 neighbours) cell means all 8 surrounding cells are
     *     purple — if spO is at position (r,c), all valid (r±1,c±1) are spP.
     *   - Do not return a (row, col) already in revealed.
     */
    void next_click(const std::vector<Cell>& revealed,
                    const std::string& meta_json,
                    const std::string& game_state_json,
                    ClickResult& out) override
    {
        (void)meta_json;

        bool clicked[25] = {};
        std::vector<int> purples, reds, unclicked;

        for (const Cell& c : revealed) clicked[c.row * 5 + c.col] = true;
        for (const Cell& c : revealed) {
            if (c.color == "spP") purples.push_back(c.row * 5 + c.col);
            if (c.color == "spR") reds.push_back(c.row * 5 + c.col);
        }
        for (int i = 0; i < 25; ++i)
            if (!clicked[i]) unclicked.push_back(i);

        // Always collect free clicks first
        auto& priority = !purples.empty() ? purples
                       : !reds.empty()    ? reds
                                          : unclicked;

        // TODO: replace the random fallback with your click logic
        int chosen = priority.empty() ? 0
            : priority[std::uniform_int_distribution<int>(0, (int)priority.size() - 1)(rng_)];

        out.row = chosen / 5;
        out.col = chosen % 5;
        out.game_state_json = game_state_json;  // TODO: update state if needed
    }

private:
    std::mt19937_64 rng_;
};

// ---------------------------------------------------------------------------
// C exports required by the harness — do not rename these functions
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new MyOQStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" const char* strategy_init_evaluation_run(void* inst) {
    static std::string buf;
    buf = static_cast<MyOQStrategy*>(inst)->init_evaluation_run();
    return buf.c_str();
}

extern "C" const char* strategy_init_game_payload(void* inst,
                                          const char* meta_json,
                                          const char* game_state_json) {
    static std::string buf;
    buf = static_cast<MyOQStrategy*>(inst)->init_game_payload(
        meta_json  ? meta_json  : "{}",
        game_state_json ? game_state_json : "{}"
    );
    return buf.c_str();
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* revealed_json,
                                            const char* meta_json,
                                            const char* game_state_json)
{
    static std::string buf;
    auto* s = static_cast<MyOQStrategy*>(inst);

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
