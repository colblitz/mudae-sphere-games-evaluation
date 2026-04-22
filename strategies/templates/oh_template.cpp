/**
 * oh_template.cpp — Template strategy for /sphere harvest (oh) — C++.
 *
 * Copy this file to strategies/oh/<your_name>.cpp, rename the class, and
 * fill in the TODO sections.  Delete any methods you don't need — only
 * next_click() is required.
 *
 * GAME OVERVIEW
 * -------------
 * Grid     : 5×5
 * Revealed : 10 cells visible at game start; 15 start covered (spU)
 * Budget   : 5 clicks
 * Goal     : maximise total SP collected across your 5 clicks
 *
 * Special click rules:
 *   Purple (spP)  : click is FREE (does not consume a click)
 *   Dark (spD)    : transforms into another color on click;
 *                   if it becomes purple the click is also free
 *   Blue (spB)    : reveals 3 additional covered cells
 *   Teal (spT)    : reveals 1 additional covered cell
 *   ~50% of boards have one "chest" covered cell worth ~345 SP on average
 *
 * COLOR REFERENCE (oh)
 * --------------------
 *   spB   blue    reveals 3 covered cells
 *   spT   teal    reveals 1 covered cell
 *   spG   green   35 SP
 *   spY   yellow  55 SP
 *   spL   light   ~76 SP average
 *   spO   orange  90 SP
 *   spR   red     150 SP
 *   spW   white   ~300 SP
 *   spP   purple  ~5–12 SP, click is FREE
 *   spD   dark    ~104 SP average, transforms on click
 *   spU   covered (unrevealed; directly clickable)
 *
 * STATE / JSON PROTOCOL
 * ---------------------
 * The harness serialises state as a JSON string threaded through every call:
 *
 *   init_evaluation_run()                 → game_state_json (once before all games)
 *   init_game_payload(meta_json, state0)    → state1     (once per game)
 *   next_click(revealed, meta, s1) → ClickResult{row, col, state2}
 *   next_click(revealed, meta, s2) → ClickResult{row, col, state3}
 *   ...
 *
 * Use init_evaluation_run() for data computed ONCE and shared across all games.
 * Use init_game_payload() to reset per-game bookkeeping at the start of each game.
 *
 * meta_json keys (oh):
 *   "clicks_left"  int   remaining budget
 *   "max_clicks"   int   total budget (always 5)
 *   "game_seed"    int   per-game deterministic seed
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

class MyOHStrategy : public OHStrategy {
public:
    MyOHStrategy() : rng_(static_cast<uint64_t>(std::time(nullptr))) {}

    // -----------------------------------------------------------------------
    // Optional: global state (computed once, shared across ALL games)
    // -----------------------------------------------------------------------

    /**
     * Return the initial state JSON — called ONCE before all games.
     *
     * Compute anything that is board-independent and expensive to repeat.
     * The returned string is passed as evaluation_run_state_json to init_game_payload() at the start
     * of every game.  Keep it as small as possible (it is parsed each call).
     *
     * Default: "{}" (empty object — no global state).
     *
     * Example: build a JSON array of cells in visit priority order so that
     * next_click() can do a fast linear scan instead of recomputing.
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
     * @param meta_json   JSON object with game metadata (same keys as
     *                    next_click's meta_json parameter).
     * @param evaluation_run_state_json  Read-only value from init_evaluation_run() (or the previous
     *                    game's state if you forwarded it — avoid mutation).
     * @return            Initial game_state_json for this game's first next_click.
     *
     * Example: return a fresh JSON object that merges the global priority
     * table with per-game counters: {"order":[...],"click_count":0}
     */
    std::string init_game_payload(const std::string& meta_json,
                         const std::string& evaluation_run_state_json) override {
        // TODO: reset per-game fields here, or delete this method
        (void)meta_json;
        return game_state_json;  // pass global state through unchanged
    }

    // -----------------------------------------------------------------------
    // Required: click decision
    // -----------------------------------------------------------------------

    /**
     * Choose the next cell to click.
     *
     * @param revealed    All cells revealed so far, each with .row, .col,
     *                    .color (a Mudae emoji name such as "spR").
     *                    The vector grows monotonically across calls.
     * @param meta_json   JSON: {"clicks_left":N,"max_clicks":5,"game_seed":K}
     * @param game_state_json  Value returned by the previous next_click (or
     *                    init_game_payload for the first call of the game).
     * @param out         Fill in out.row, out.col, and out.game_state_json.
     *                    out.game_state_json is passed back as game_state_json next call.
     *
     * Tips:
     *   - Purple cells ("spP") are free; prioritise them.
     *   - Blue ("spB") and teal ("spT") reveal more cells, increasing info.
     *   - Do not return a (row, col) that is already in revealed.
     */
    void next_click(const std::vector<Cell>& revealed,
                    const std::string& meta_json,
                    const std::string& game_state_json,
                    ClickResult& out) override
    {
        (void)meta_json;

        bool clicked[25] = {};
        std::vector<int> purples;
        std::vector<int> unclicked;

        for (const Cell& c : revealed)
            clicked[c.row * 5 + c.col] = true;

        for (const Cell& c : revealed)
            if (c.color == "spP") purples.push_back(c.row * 5 + c.col);

        for (int i = 0; i < 25; ++i)
            if (!clicked[i]) unclicked.push_back(i);

        // TODO: replace this random fallback with your click logic

        // Prefer any visible purple (free click)
        auto& pool = !purples.empty() ? purples : unclicked;
        int chosen = pool.empty() ? 0
            : pool[std::uniform_int_distribution<int>(0, (int)pool.size() - 1)(rng_)];

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

extern "C" sphere::StrategyBase* create_strategy()                         { return new MyOHStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" const char* strategy_init_evaluation_run(void* inst) {
    static std::string buf;
    buf = static_cast<MyOHStrategy*>(inst)->init_evaluation_run();
    return buf.c_str();
}

extern "C" const char* strategy_init_game_payload(void* inst,
                                          const char* meta_json,
                                          const char* game_state_json) {
    static std::string buf;
    buf = static_cast<MyOHStrategy*>(inst)->init_game_payload(
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
    auto* s = static_cast<MyOHStrategy*>(inst);

    // Minimal JSON parsing — extract each {"row":R,"col":C,"color":"spX"} entry
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
