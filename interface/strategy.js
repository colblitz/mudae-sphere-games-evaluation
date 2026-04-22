/**
 * strategy.js — JavaScript strategy interface for Mudae sphere mini-game evaluation.
 *
 * To submit a JavaScript strategy, create a .js file in strategies/<game>/
 * that exports a class extending the appropriate base class below and calls
 * register() at the bottom.
 *
 * The harness runs your file as a Node.js child process and communicates via
 * newline-delimited JSON on stdin/stdout.  You do not need to handle the
 * protocol yourself — just extend the base class and export it.
 *
 * Example (strategies/oc/my_strategy.js):
 *
 *   const { OCStrategy, register } = require("../../interface/strategy.js");
 *
 *   class MyOCStrategy extends OCStrategy {
 *     nextClick(revealed, meta, state) {
 *       // Pick a random unrevealed cell
 *       const clicked = new Set(revealed.map(c => c.row * 5 + c.col));
 *       const unclicked = [];
 *       for (let r = 0; r < 5; r++)
 *         for (let c = 0; c < 5; c++)
 *           if (!clicked.has(r * 5 + c)) unclicked.push([r, c]);
 *       const [row, col] = unclicked[Math.floor(Math.random() * unclicked.length)];
 *       return { row, col, state };
 *     }
 *   }
 *
 *   register(new MyOCStrategy());
 *
 * Revealed cell format
 * --------------------
 * `revealed` is an array of objects, one per cell revealed so far:
 *   [{ row: number, col: number, color: string }, ...]
 * Row and col are 0-indexed (0..4).  The array grows monotonically.
 *
 * Return value of nextClick
 * -------------------------
 * Return an object: { row, col, state }
 *   row, col  — 0-indexed coordinates of the cell to click.
 *   state     — any JSON-serializable value; passed back on the next call.
 *               Use null if stateless.
 *
 * For the full color reference and game rules see interface/strategy.py.
 */

"use strict";

// ---------------------------------------------------------------------------
// Base classes
// ---------------------------------------------------------------------------

class StrategyBase {
  /**
   * Called once before the evaluation run begins.
   *
   * Override to compute data shared across all games — lookup tables,
   * precomputed weights, etc.  The returned value is passed as
   * `evaluationRunState` to every initGamePayload call.
   *
   * Do not store game-specific information here.  Each game must be played
   * independently; sharing board history between games produces unfair results.
   *
   * Default: null.
   * @returns {*}
   */
  initEvaluationRun() {
    return null;
  }

  /**
   * Called once before the first click of each game.
   *
   * Override to set up fresh per-game state.  The returned value becomes
   * `gameState` for that game's first nextClick call.
   *
   * @param {Object} meta               Game metadata (keys vary per game).
   * @param {*}      evaluationRunState Read-only value from initEvaluationRun().
   *                                    Do not mutate — shared across all games.
   * @returns {*}    Initial gameState for this game's first nextClick call.
   */
  initGamePayload(meta, evaluationRunState) {
    return evaluationRunState;
  }

  /**
   * Choose the next cell to click.
   *
   * @param {Array<{row: number, col: number, color: string}>} revealed
   *   All cells revealed so far.
   * @param {Object} meta       Game-specific metadata (see subclass docs).
   * @param {*}      gameState  Value returned by the previous nextClick (or
   *                            initGamePayload for the first call of the game).
   * @returns {{ row: number, col: number, gameState: * }}
   */
  nextClick(revealed, meta, gameState) {  // eslint-disable-line no-unused-vars
    throw new Error("nextClick() must be implemented");
  }
}

// ---------------------------------------------------------------------------
// oh — /sphere harvest
// ---------------------------------------------------------------------------

/**
 * Game metadata keys:
 *   clicks_left  number  remaining click budget (5 at start).
 *   max_clicks   number  total click budget (always 5).
 *
 * Colors: spB spT spG spY spL spO spR spW spP spD spU
 */
class OHStrategy extends StrategyBase {}

// ---------------------------------------------------------------------------
// oc — /sphere chest
// ---------------------------------------------------------------------------

/**
 * Game metadata keys:
 *   clicks_left  number  remaining click budget.
 *   max_clicks   number  total click budget (always 5).
 *
 * Colors: spR spO spY spG spT spB
 */
class OCStrategy extends StrategyBase {}

// ---------------------------------------------------------------------------
// oq — /sphere quest
// ---------------------------------------------------------------------------

/**
 * Game metadata keys:
 *   clicks_left    number  remaining non-purple click budget.
 *   max_clicks     number  total non-purple click budget (always 7).
 *   purples_found  number  purple cells clicked so far.
 *
 * Colors: spP spB spT spG spY spO spR
 */
class OQStrategy extends StrategyBase {}

// ---------------------------------------------------------------------------
// ot — /sphere trace
// ---------------------------------------------------------------------------

/**
 * Game metadata keys:
 *   n_colors    number  number of colors in this game (6, 7, 8, or 9).
 *   ships_hit   number  ship cells revealed so far.
 *   blues_used  number  blue clicks spent so far.
 *   max_clicks  number  base blue click budget (always 4).
 *
 * Colors: spT spG spY spO spL spD spR spW spB
 */
class OTStrategy extends StrategyBase {}

// ---------------------------------------------------------------------------
// Protocol handler — do not modify
// ---------------------------------------------------------------------------

let _strategy = null;

/**
 * Register a strategy instance.  Call this once at the bottom of your file.
 * @param {StrategyBase} instance
 */
function register(instance) {
  _strategy = instance;
}

// JSON protocol loop (driven by the harness).
// Starts unconditionally so the loop is active when strategy files are run as
// the Node entry point (e.g. `node strategies/oq/stateful.js`).  The old
// `require.main === module` guard caused the loop to never start in that case
// because `module` inside strategy.js refers to strategy.js's own module
// object, which is never `require.main` when strategy.js is require()'d from
// a user strategy file.
{
  const readline = require("readline");
  const rl = readline.createInterface({ input: process.stdin, crlfDelay: Infinity });

  rl.on("line", (line) => {
    if (!line.trim()) return;
    let msg;
    try {
      msg = JSON.parse(line);
    } catch (e) {
      process.stdout.write(JSON.stringify({ error: "parse_error" }) + "\n");
      return;
    }

    if (!_strategy) {
      process.stdout.write(JSON.stringify({ error: "no_strategy_registered" }) + "\n");
      return;
    }

    try {
      let result;
      if (msg.method === "init_evaluation_run") {
        result = { value: _strategy.initEvaluationRun() };
      } else if (msg.method === "init_game_payload") {
        result = { value: _strategy.initGamePayload(msg.meta, msg.evaluationRunState) };
      } else if (msg.method === "next_click") {
        const { row, col, gameState } = _strategy.nextClick(msg.revealed, msg.meta, msg.gameState);
        result = { row, col, gameState };
      } else {
        result = { error: "unknown_method" };
      }
      process.stdout.write(JSON.stringify(result) + "\n");
    } catch (e) {
      process.stdout.write(JSON.stringify({ error: e.message }) + "\n");
    }
  });
}

module.exports = { StrategyBase, OHStrategy, OCStrategy, OQStrategy, OTStrategy, register };
