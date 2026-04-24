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
 * STATE MODEL
 * -----------
 * State lives in your class's member variables — not serialised.
 *
 *   init_evaluation_run()   — store read-only run-level data in members
 *   init_game_payload()     — reset per-game members; called before each game
 *   next_click()            — read/update member variables as needed
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
     * @param meta_json   JSON: {"clicks_left":N,"max_clicks":5}
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
     * @param meta_json  JSON: {"clicks_left":N,"max_clicks":5}
     * @param out        Fill in out.row and out.col.
     *
     * Tips:
     *   - Each color reveal constrains where red can be.  Eliminate candidate
     *     red positions that are inconsistent with the revealed zone pattern.
     *   - Once red is found, all remaining colors are determined — greedily
     *     pick the highest-value unrevealed cell (orange, then yellow, etc.).
     *   - A "blue" (spB) reveal at position P means red is not in any of the
     *     geometric zones that would map P to orange/yellow/green/teal from red.
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

extern "C" sphere::StrategyBase* create_strategy()                         { return new MyOCStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* inst) {
    static_cast<MyOCStrategy*>(inst)->init_evaluation_run();
}

extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {
    static_cast<MyOCStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<MyOCStrategy*>(inst);

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
