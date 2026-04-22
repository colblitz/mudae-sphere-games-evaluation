/**
 * ot_template.cpp — Template strategy for /sphere trace (ot) — C++.
 *
 * Copy this file to strategies/ot/<your_name>.cpp, rename the class, and
 * fill in the TODO sections.  Delete any methods you don't need — only
 * next_click() is required.
 *
 * GAME OVERVIEW
 * -------------
 * Grid   : 5×5, all 25 cells start covered
 * Budget : 4 BLUE clicks (ship cells are FREE)
 * Goal   : find ship cells (free) while minimising wasted blue clicks
 *
 * Ships are contiguous horizontal or vertical segments; no overlap.
 * Blue (spB) cells are empty — clicking one uses 1 of your 4 blue budget.
 * Ship cells do not cost a click.
 *
 * Extra Chance rule:
 *   If blue click #4 is spent before ships_hit >= 5, the game continues.
 *   Each additional blue while ships_hit < 5 extends the game.
 *   Once ships_hit >= 5, the next blue ends the game immediately.
 *
 * Ship configs by n_colors:
 *   6: teal(4)+green(3)+yellow(3)+orange(2)+light(2)  → 14 ship, 11 blue
 *   7: + dark(2)   → 16 ship,  9 blue
 *   8: + red(2)    → 18 ship,  7 blue
 *   9: + white(2)  → 20 ship,  5 blue
 *
 * COLOR REFERENCE (ot)
 * --------------------
 *   spT   teal    ship of length 4
 *   spG   green   ship of length 3
 *   spY   yellow  ship of length 3
 *   spO   orange  ship of length 2 (always present)
 *   spL   light   ship of length 2 (6-color and above)
 *   spD   dark    ship of length 2 (7-color and above)
 *   spR   red     ship of length 2 (8-color and above)
 *   spW   white   ship of length 2 (9-color only)
 *   spB   blue    empty cell (costs 1 blue click)
 *
 * STATE / JSON PROTOCOL
 * ---------------------
 * The harness serialises state as a JSON string threaded through every call:
 *
 *   init_evaluation_run()                 → state_json (once before all games)
 *   init_game_payload(meta_json, state0)    → state1     (once per game)
 *   next_click(revealed, meta, s1) → ClickResult{row, col, state2}
 *   ...
 *
 * meta_json keys (ot):
 *   "n_colors"    int   ship color count (6, 7, 8, or 9)
 *   "ships_hit"   int   ship cells revealed so far
 *   "blues_used"  int   blue clicks spent so far
 *   "max_clicks"  int   base blue budget (always 4)
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

class MyOTStrategy : public OTStrategy {
public:
    MyOTStrategy() : rng_(static_cast<uint64_t>(std::time(nullptr))) {}

    // -----------------------------------------------------------------------
    // Optional: global state (computed once, shared across ALL games)
    // -----------------------------------------------------------------------

    /**
     * Return the initial state JSON — called ONCE before all games.
     *
     * Compute anything that is board-independent and expensive to repeat.
     * Default: "{}" (empty object).
     *
     * Example: for each n_colors variant, enumerate all valid ship placements
     * and store per-cell marginal probabilities of being a ship cell.
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
     * @param meta_json   JSON: {"n_colors":N,"ships_hit":0,"blues_used":0,"max_clicks":4}
     * @param state_json  Value returned by init_evaluation_run().
     * @return            Initial state_json for this game's first next_click.
     *
     * Tip: use meta_json's "n_colors" to select the right precomputed
     * placement set from init_evaluation_run()'s global table.
     */
    std::string init_game_payload(const std::string& meta_json,
                         const std::string& state_json) override {
        // TODO: reset per-game fields here, or delete this method
        (void)meta_json;
        return state_json;
    }

    // -----------------------------------------------------------------------
    // Required: click decision
    // -----------------------------------------------------------------------

    /**
     * Choose the next cell to click.
     *
     * @param revealed    All cells revealed so far (.row, .col, .color).
     * @param meta_json   JSON: {"n_colors":N,"ships_hit":K,"blues_used":B,"max_clicks":4}
     * @param state_json  Value from the previous next_click (or init_game_payload).
     * @param out         Fill in out.row, out.col, out.state_json.
     *
     * Tips:
     *   - Revealed ship cells constrain where the rest of each ship must be
     *     (ships are contiguous horizontal or vertical segments of fixed length).
     *   - A revealed blue at (r,c) means no ship passes through that cell —
     *     eliminates any placement that includes (r,c).
     *   - After hitting one end of a ship, the continuation cells are known
     *     (constrained by direction and remaining ship length).
     *   - Prefer cells with high probability of being ship cells to minimise
     *     wasted blue clicks.
     *   - Do not return a (row, col) already in revealed.
     */
    void next_click(const std::vector<Cell>& revealed,
                    const std::string& meta_json,
                    const std::string& state_json,
                    ClickResult& out) override
    {
        (void)meta_json;

        bool clicked[25] = {};
        std::vector<int> unclicked;

        for (const Cell& c : revealed) clicked[c.row * 5 + c.col] = true;
        for (int i = 0; i < 25; ++i)
            if (!clicked[i]) unclicked.push_back(i);

        // TODO: replace this random fallback with your click logic
        int chosen = unclicked.empty() ? 0
            : unclicked[std::uniform_int_distribution<int>(0, (int)unclicked.size() - 1)(rng_)];

        out.row = chosen / 5;
        out.col = chosen % 5;
        out.state_json = state_json;  // TODO: update state if needed
    }

private:
    std::mt19937_64 rng_;
};

// ---------------------------------------------------------------------------
// C exports required by the harness — do not rename these functions
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new MyOTStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" const char* strategy_init_evaluation_run(void* inst) {
    static std::string buf;
    buf = static_cast<MyOTStrategy*>(inst)->init_evaluation_run();
    return buf.c_str();
}

extern "C" const char* strategy_init_game_payload(void* inst,
                                          const char* meta_json,
                                          const char* state_json) {
    static std::string buf;
    buf = static_cast<MyOTStrategy*>(inst)->init_game_payload(
        meta_json  ? meta_json  : "{}",
        state_json ? state_json : "{}"
    );
    return buf.c_str();
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* revealed_json,
                                            const char* meta_json,
                                            const char* state_json)
{
    static std::string buf;
    auto* s = static_cast<MyOTStrategy*>(inst);

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
    s->next_click(revealed, meta_json ? meta_json : "{}", state_json ? state_json : "{}", out);
    buf = "{\"row\":" + std::to_string(out.row) +
          ",\"col\":" + std::to_string(out.col) +
          ",\"state\":" + out.state_json + "}";
    return buf.c_str();
}
