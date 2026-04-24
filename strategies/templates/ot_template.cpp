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
 * STATE MODEL
 * -----------
 * State lives in your class's member variables — not serialised.
 *
 *   init_evaluation_run()   — store read-only run-level data in members
 *   init_game_payload()     — reset per-game members; called before each game
 *   next_click()            — read/update member variables as needed
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
     * Called ONCE before all games.  Store board-independent precomputed
     * data in member variables here.
     *
     * Example: enumerate all valid ship placements for each n_colors variant
     * and store the per-cell marginal probabilities of being a ship cell.
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
     * @param meta_json  JSON: {"n_colors":N,"ships_hit":0,"blues_used":0,"max_clicks":4}
     *
     * Tip: read "n_colors" from meta_json to select the right precomputed
     * tables for this variant.
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
     * @param meta_json  JSON: {"n_colors":N,"ships_hit":K,"blues_used":B,"max_clicks":4}
     * @param out        Fill in out.row and out.col.
     *
     * Tips:
     *   - Ship cells are FREE — clicking a ship cell does not cost a click.
     *   - Each clicked cell constrains where remaining ships can be.
     *   - A blue cell at (r,c) confirms no ship passes through it.
     *   - Prefer cells with high P(ship) to minimise wasted blue clicks.
     *   - Ship lengths: spT=4, spG=3, spY=3, spO/spL/spD/spR/spW=2
     *   - Do not return a (row, col) where board[row*5+col].clicked is true.
     */
    void next_click(const std::vector<Cell>& board,
                    const std::string& meta_json,
                    ClickResult& out) override
    {
        (void)meta_json;

        bool clicked[25] = {};
        std::vector<int> unclicked;

        for (const Cell& c : board) if (c.clicked) clicked[c.row * 5 + c.col] = true;
        for (int i = 0; i < 25; ++i)
            if (!clicked[i]) unclicked.push_back(i);

        // TODO: replace this random fallback with your click logic
        int chosen = unclicked.empty() ? 0
            : unclicked[std::uniform_int_distribution<int>(0, (int)unclicked.size() - 1)(rng_)];

        out.row = chosen / 5;
        out.col = chosen % 5;
    }

private:
    std::mt19937_64 rng_;
};

// ---------------------------------------------------------------------------
// C exports required by the harness — do not rename these functions
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new MyOTStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* inst) {
    static_cast<MyOTStrategy*>(inst)->init_evaluation_run();
}

extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {
    static_cast<MyOTStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<MyOTStrategy*>(inst);

    std::vector<Cell> board;
    const char* p = board_json;
    while ((p = strstr(p, "\"row\":")) != nullptr) {
        Cell c;
        c.row = atoi(p + 6);
        const char* cp = strstr(p, "\"col\":"); if (cp) c.col = atoi(cp + 6);
        const char* colp = strstr(p, "\"color\":\"");
        if (colp) { colp += 9; const char* e = strchr(colp, '"'); if (e) c.color = std::string(colp, e - colp); }
        const char* clkp = strstr(p, "\"clicked\":"); if (clkp) { clkp += 10; while (*clkp==' ') ++clkp; c.clicked = (strncmp(clkp,"true",4)==0); }
        board.push_back(c); p += 6;
    }

    ClickResult out;
    s->next_click(board, meta_json ? meta_json : "{}", out);
    buf = "{\"row\":" + std::to_string(out.row) + ",\"col\":" + std::to_string(out.col) + "}";
    return buf.c_str();
}
