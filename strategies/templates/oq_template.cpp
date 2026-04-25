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
 *   Special click rules:
 *   Purple (spP) : click is FREE (does not consume a click)
 *   Red (spR)    : appears after 3 purples clicked; costs 1 click (worth 150 SP)
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
 *   spR   red     150 SP; appears after 3 purples found; costs 1 click
 *   spB   blue    0 purple neighbours
 *   spT   teal    1 purple neighbour
 *   spG   green   2 purple neighbours
 *   spY   yellow  3 purple neighbours
 *   spO   orange  4 purple neighbours
 *
 * STATE MODEL
 * -----------
 * State lives in your class's member variables — not serialised.
 *
 *   init_evaluation_run()   — store read-only run-level data in members
 *   init_game_payload()     — reset per-game members; called before each game
 *   next_click()            — read/update member variables as needed
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
     * Called ONCE before all games.  Store board-independent precomputed
     * data in member variables here.
     */
    void init_evaluation_run() override {
        // TODO: precompute global data here, or delete this method
    }

    // -----------------------------------------------------------------------
    // Optional: per-game initialisation
    // -----------------------------------------------------------------------

    /**
     * Reset per-game member variables — called once before each game.
     *
     * @param meta_json  JSON: {"clicks_left":N,"max_clicks":7,"purples_found":0}
     */
    void init_game_payload(const std::string& meta_json) override {
        // TODO: reset per-game fields here, or delete this method
        (void)meta_json;
    }

    // -----------------------------------------------------------------------
    // Required: click decision
    // -----------------------------------------------------------------------

    /**
     * Choose the next cell to click.
     *
     * @param board      All 25 board cells (.row, .col, .color, .clicked).
     * @param meta_json  JSON: {"clicks_left":N,"max_clicks":7,"purples_found":K}
     * @param out        Fill in out.row and out.col.
     *
     * Tips:
     *   - Always click purple ("spP") cells immediately — they are free.
     *   - Always click red ("spR") when it appears — 150 SP for 1 click.
     *   - Each non-purple reveal gives a neighbour count.  Use it to
     *     eliminate or confirm candidate purple positions (like Minesweeper).
     *   - A "spO" (4 neighbours) cell means all 8 surrounding cells are
     *     purple — if spO is at position (r,c), all valid (r±1,c±1) are spP.
     *   - Do not return a (row, col) where board[row*5+col].clicked is true.
     */
    void next_click(const std::vector<Cell>& board,
                    const std::string& meta_json,
                    ClickResult& out) override
    {
        (void)meta_json;

        bool clicked[25] = {};
        std::vector<int> purples, reds, unclicked;

        for (const Cell& c : board) if (c.clicked) clicked[c.row * 5 + c.col] = true;
        for (const Cell& c : board) {
            if (c.color == "spP" && !c.clicked) purples.push_back(c.row * 5 + c.col);
            if (c.color == "spR" && !c.clicked) reds.push_back(c.row * 5 + c.col);
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
    }

private:
    std::mt19937_64 rng_;
};

// ---------------------------------------------------------------------------
// C exports required by the harness — do not rename these functions
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new MyOQStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* inst) {
    static_cast<MyOQStrategy*>(inst)->init_evaluation_run();
}

extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {
    static_cast<MyOQStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<MyOQStrategy*>(inst);

    std::vector<Cell> board = parse_board_json(board_json);
    ClickResult out;
    s->next_click(board, meta_json ? meta_json : "{}", out);
    buf = "{\"row\":" + std::to_string(out.row) + ",\"col\":" + std::to_string(out.col) + "}";
    return buf.c_str();
}
