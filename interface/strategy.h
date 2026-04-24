/**
 * strategy.h — C++ strategy interface for Mudae sphere mini-game evaluation.
 *
 * To submit a C++ strategy, create a .cpp file in strategies/<game>/ that:
 *   1. Includes this header: #include "../../interface/strategy.h"
 *   2. Defines a class inheriting from the appropriate base (OHStrategy,
 *      OCStrategy, OQStrategy, or OTStrategy).
 *   3. Exports a factory function with C linkage so the harness can load it
 *      via dlopen:
 *
 *        extern "C" StrategyBase* create_strategy() {
 *            return new MyStrategy();
 *        }
 *        extern "C" void destroy_strategy(StrategyBase* s) { delete s; }
 *
 * The harness compiles the strategy file as a shared library automatically;
 * you do not need to compile it yourself.
 *
 * Board cell format
 * -----------------
 * `board` is always a vector of exactly 25 Cell structs (all cells on the
 * 5x5 grid).  Row and col are 0-indexed (0..4).
 *
 * Each cell carries:
 *   color    — Mudae emoji name, e.g. "spR", "spT", "spB".
 *              "spU" means the cell is covered/unknown.
 *   clicked  — false = cell has NOT yet been clicked (still interactable).
 *              true  = cell HAS been clicked (disabled, do not re-click).
 *
 * Cell state combinations:
 *   color="spU",   clicked=false  → normal uninteracted covered cell
 *   color="spU",   clicked=true   → oh chest cell (remains visually covered)
 *   color=<real>,  clicked=false  → passively revealed (oh blue/teal, or oq spR auto-reveal)
 *   color=<real>,  clicked=true   → normally clicked and disabled
 *
 * Return value of next_click
 * --------------------------
 * Fill in the `out` ClickResult:
 *   out.row, out.col  — 0-indexed coordinates of the cell to click.
 *
 * Per-game state lives in your class's member variables.  Reset it in
 * init_game_payload(); it will be cleared correctly between games.
 *
 * Do NOT return a (row, col) where board[row*5+col].clicked is true.
 *
 * For the full color reference and game rules see interface/strategy.py.
 */

#pragma once

#include <string>
#include <vector>

namespace sphere {

// ---------------------------------------------------------------------------
// Shared data structures
// ---------------------------------------------------------------------------

struct Cell {
    int         row;
    int         col;
    std::string color   = "spU";  // Mudae emoji name; "spU" = covered/unknown
    bool        clicked = false;  // true = cell has been clicked (disabled)
};

struct ClickResult {
    int row = 0;
    int col = 0;
};

// ---------------------------------------------------------------------------
// Base class (all games share the same interface shape)
// ---------------------------------------------------------------------------

class StrategyBase {
public:
    virtual ~StrategyBase() = default;

    /**
     * Called once before the evaluation run begins.
     *
     * Override to compute data shared across all games — lookup tables,
     * precomputed weights, etc.  Store the results in member variables.
     *
     * Do not store game-specific information here.  Each game must be played
     * independently; sharing board history between games produces unfair results.
     *
     * Default: no-op.
     */
    virtual void init_evaluation_run() {}

    /**
     * Called once before the first click of each game.
     *
     * Override to reset per-game member variables so each game starts clean.
     * The bridge calls this before every game.
     *
     * @param meta_json  JSON object with game metadata (initial state, e.g. clicks_left).
     */
    virtual void init_game_payload(const std::string& meta_json) { (void)meta_json; }

    /**
     * Choose the next cell to click.
     *
     * @param board      All 25 board cells.  color="spU" means covered/unknown;
     *                   clicked=false means still interactable.
     * @param meta_json  JSON object with game-specific metadata.
     * @param out        Fill in out.row and out.col.
     *                   Do not return a cell where board[row*5+col].clicked is true.
     */
    virtual void next_click(const std::vector<Cell>& board,
                            const std::string&        meta_json,
                            ClickResult&              out) = 0;
};

// ---------------------------------------------------------------------------
// oh — /sphere harvest
// ---------------------------------------------------------------------------

/**
 * Board setup: 10 cells start with their real color visible (clicked=false);
 * 15 cells start as (color="spU", clicked=false).
 *
 * Game metadata JSON keys:
 *   "clicks_left"  int  remaining click budget (5 at start; decreases by 1
 *                       per non-purple, non-dark->purple click).
 *   "max_clicks"   int  total click budget (always 5).
 *
 * Colors: spB spT spG spY spL spO spR spW spP spD spU
 * See interface/strategy.py for full color reference and rules.
 */
class OHStrategy : public StrategyBase {};

// ---------------------------------------------------------------------------
// oc — /sphere chest
// ---------------------------------------------------------------------------

/**
 * Game metadata JSON keys:
 *   "clicks_left"  int  remaining click budget.
 *   "max_clicks"   int  total click budget (always 5).
 *
 * Colors: spR spO spY spG spT spB
 * Note on board distribution: all 16,800 valid boards are weighted equally.
 * In reality Mudae may not use a uniform distribution.
 */
class OCStrategy : public StrategyBase {};

// ---------------------------------------------------------------------------
// oq — /sphere quest
// ---------------------------------------------------------------------------

/**
 * Game metadata JSON keys:
 *   "clicks_left"    int  remaining non-purple click budget.
 *   "max_clicks"     int  total non-purple click budget (always 7).
 *   "purples_found"  int  purple cells clicked so far.
 *
 * Colors: spP spB spT spG spY spO spR
 *
 * After 3 purples are clicked the 4th purple is auto-revealed as spR
 * (color="spR", clicked=false) in the board on the next call.  Click it —
 * it costs 1 click but is worth 150 SP.
 */
class OQStrategy : public StrategyBase {};

// ---------------------------------------------------------------------------
// ot — /sphere trace
// ---------------------------------------------------------------------------

/**
 * Game metadata JSON keys:
 *   "n_colors"    int  number of colors in this game (6, 7, 8, or 9).
 *   "ships_hit"   int  ship cells revealed so far.
 *   "blues_used"  int  blue clicks spent so far.
 *   "max_clicks"  int  base blue click budget (always 4; Extra Chance extends).
 *
 * Colors: spT spG spY spO spL spD spR spW spB
 */
class OTStrategy : public StrategyBase {};

}  // namespace sphere
