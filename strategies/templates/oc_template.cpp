/**
 * oc_template.cpp — Template strategy for /sphere chest (oc) — C++.
 *
 * Copy this file to strategies/oc/<your_name>.cpp, rename the class, and
 * fill in the TODO sections.  Delete any methods you don't need — only
 * next_click() is required.
 *
 * GAME OVERVIEW
 * -------------
 * Grid   : 5×5, all 25 cells start covered
 * Budget : 5 clicks
 * Goal   : maximise SP collected across 5 clicks
 *
 * One red sphere (spR, 150 SP) is hidden.  Its position determines every
 * other cell's color via fixed spatial zones (dr = row delta from red,
 * dc = col delta from red):
 *
 *   |dr|+|dc| == 1            → orange  (spO, 90 SP)  [2 cells]
 *   |dr| == |dc|, dr != 0     → yellow  (spY, 55 SP)  [3 cells]
 *   same row or col, not above → green   (spG, 35 SP)  [4 cells]
 *   residual orth/rowcol       → teal    (spT, 20 SP)
 *   no geometric relation      → blue    (spB, 10 SP)
 *
 * The center cell (2,2) can never be red.
 * Evaluation uses all 16,800 valid boards weighted equally.
 *
 * COLOR REFERENCE (oc)
 * --------------------
 *   spR   red     150 SP
 *   spO   orange   90 SP
 *   spY   yellow   55 SP
 *   spG   green    35 SP
 *   spT   teal     20 SP
 *   spB   blue     10 SP
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
 * meta_json keys (oc):
 *   "clicks_left"  int   remaining budget
 *   "max_clicks"   int   total budget (always 5)
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

class MyOCStrategy : public OCStrategy {
public:
    MyOCStrategy() : rng_(static_cast<uint64_t>(std::time(nullptr))) {}

    // -----------------------------------------------------------------------
    // Optional: global state (computed once, shared across ALL games)
    // -----------------------------------------------------------------------

    /**
     * Return the initial state JSON — called ONCE before all games.
     *
     * Compute anything that is board-independent and expensive to repeat.
     * The returned string is passed as state_json to init_game_payload() at the start
     * of every game.  Treat it as a read-only global lookup.
     *
     * Default: "{}" (empty object — no global state).
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
     * @param meta_json   JSON: {"clicks_left":N,"max_clicks":5}
     * @param state_json  Value returned by init_evaluation_run().
     * @return            Initial state_json for this game's first next_click.
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
     * @param meta_json   JSON: {"clicks_left":N,"max_clicks":5}
     * @param state_json  Value from the previous next_click (or init_game_payload).
     * @param out         Fill in out.row, out.col, out.state_json.
     *
     * Tips:
     *   - Each color reveal constrains where red can be.  Eliminate candidate
     *     red positions that are inconsistent with the revealed zone pattern.
     *   - Once red is found, all remaining colors are determined — greedily
     *     pick the highest-value unrevealed cell (orange, then yellow, etc.).
     *   - A "blue" (spB) reveal at position P means red is not in any of the
     *     geometric zones that would map P to orange/yellow/green/teal from red.
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

extern "C" sphere::StrategyBase* create_strategy()                         { return new MyOCStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" const char* strategy_init_evaluation_run(void* inst) {
    static std::string buf;
    buf = static_cast<MyOCStrategy*>(inst)->init_evaluation_run();
    return buf.c_str();
}

extern "C" const char* strategy_init_game_payload(void* inst,
                                          const char* meta_json,
                                          const char* state_json) {
    static std::string buf;
    buf = static_cast<MyOCStrategy*>(inst)->init_game_payload(
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
    auto* s = static_cast<MyOCStrategy*>(inst);

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
