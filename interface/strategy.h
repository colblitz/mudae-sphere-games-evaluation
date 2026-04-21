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
 * Revealed cell format
 * --------------------
 * `revealed` is a vector of Cell structs, one per cell revealed so far.
 * Row and col are 0-indexed (0..4).  color is a Mudae emoji name such as
 * "spR", "spT", "spB".  The vector grows monotonically each call.
 *
 * Return value of next_click
 * --------------------------
 * Fill in the `out` ClickResult:
 *   out.row, out.col  — 0-indexed coordinates of the cell to click.
 *   out.state_json    — arbitrary JSON string; passed back as state_json on
 *                       the next call.  Use "{}" if stateless.
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
    std::string color;  // Mudae emoji name, e.g. "spR", "spT", "spB"
};

struct ClickResult {
    int         row        = 0;
    int         col        = 0;
    std::string state_json = "{}";  // Arbitrary JSON; threaded back each call
};

// ---------------------------------------------------------------------------
// Base class (all games share the same interface shape)
// ---------------------------------------------------------------------------

class StrategyBase {
public:
    virtual ~StrategyBase() = default;

    /**
     * Called once before the first click of each game.
     *
     * @param meta_json  JSON object string with game metadata (keys vary per
     *                   game — see subclass docs).
     * @param state_json Value returned by init_payload() or the previous
     *                   game's final next_click call.
     * @return           Updated state_json for the first next_click call.
     */
    virtual std::string init_run(const std::string& meta_json,
                                 const std::string& state_json) {
        (void)meta_json;
        return state_json;
    }

    /**
     * Return the initial state JSON before init_run is called.
     * Default: "{}".
     */
    virtual std::string init_payload() { return "{}"; }

    /**
     * Choose the next cell to click.
     *
     * @param revealed   All cells revealed so far.
     * @param meta_json  JSON object string with game-specific metadata.
     * @param state_json Value returned by the previous next_click (or
     *                   init_run for the first call).
     * @param out        Filled in with (row, col, next state_json).
     */
    virtual void next_click(const std::vector<Cell>& revealed,
                            const std::string&        meta_json,
                            const std::string&        state_json,
                            ClickResult&              out) = 0;
};

// ---------------------------------------------------------------------------
// oh — /sphere harvest
// ---------------------------------------------------------------------------

/**
 * Board setup: 10 cells are revealed at the start of every game; 15 start
 * covered (spU).  The 10 initially revealed cells are included in the
 * `revealed` vector on the first next_click call.
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
