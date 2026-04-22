/**
 * oq_template.js — Template strategy for /sphere quest (oq) — JavaScript.
 *
 * Copy this file to strategies/oq/<your_name>.js, rename the class, and
 * fill in the TODO sections.  Delete any methods you don't need — only
 * nextClick() is required.
 *
 * GAME OVERVIEW
 * -------------
 * Grid   : 5×5, all 25 cells start covered
 * Budget : 7 non-purple clicks
 * Goal   : find 3 of the 4 hidden purple spheres → the 4th converts to red
 *          (spR, 150 SP).  Collect red and spend remaining budget on tiles.
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
 * REVEALED CELL FORMAT
 * --------------------
 * `revealed` is an array of objects, one per cell revealed so far:
 *   [{ row: number, col: number, color: string }, ...]
 * Row and col are 0-indexed (0..4).  Grows monotonically each call.
 *
 * STATE PAYLOAD
 * -------------
 * The game_state value is threaded through every call within a game:
 *
 *   initEvaluationRun()               → initialState (called once before all games)
 *   initGamePayload(meta, s0)           → s1           (called once per game)
 *   nextClick(revealed,meta,s1) → {row,col,gameState:s2}
 *   ...
 *
 * Use initEvaluationRun() for data computed ONCE and shared across all games.
 * Use initGamePayload() to reset per-game bookkeeping at the start of each game.
 *
 * meta keys (oq):
 *   clicks_left    number  remaining non-purple budget
 *   max_clicks     number  total non-purple budget (always 7)
 *   purples_found  number  purples clicked so far (0–3)
 *
 * See also
 * --------
 * oh/random_clicks.js   — minimal stateless example
 * oc/global_state.js    — initEvaluationRun() (global state) example
 * oq/stateful.js        — initGamePayload() + state threading (per-game state) example
 */

"use strict";

const { OQStrategy, register } = require("../../interface/strategy.js");

class MyOQStrategy extends OQStrategy {
  // -------------------------------------------------------------------------
  // Optional: global state (computed once, shared across ALL games)
  // -------------------------------------------------------------------------

  /**
   * Called ONCE before all games begin.
   *
   * Return anything that is board-independent and expensive to repeat.
   * The returned value is passed as `evaluationRunState` to every subsequent initGamePayload()
   * and nextClick() call.  Treat it as a read-only global table.
   *
   * @returns {*}  Any JSON-serialisable value.  Default: null.
   */
  initEvaluationRun() {
    // TODO: replace with your global precomputation, or delete this method
    return null;
  }

  // -------------------------------------------------------------------------
  // Optional: per-game initialisation
  // -------------------------------------------------------------------------

  /**
   * Called once before each game's first click.
   *
   * @param {Object} meta   { clicks_left, max_clicks, purples_found }
   * @param {*}      evaluationRunState  Read-only value from initEvaluationRun()
   * @returns {*}   Initial gameState for this game's first nextClick() call.
   *
   * Example: initialise a constraint model — the set of all 12,650 possible
   * purple layouts, to be pruned as neighbor-count colors are revealed.
   */
  initGamePayload(meta, evaluationRunState) {
    // TODO: reset per-game fields here, or delete this method
    return evaluationRunState;
  }

  // -------------------------------------------------------------------------
  // Required: click decision
  // -------------------------------------------------------------------------

  /**
   * Choose the next cell to click.
   *
   * @param {Array<{row: number, col: number, color: string}>} revealed
   *   All cells revealed so far this game (grows monotonically).
   * @param {Object} meta
   *   { clicks_left: number, max_clicks: number, purples_found: number }
   * @param {*} gameState
   *   Value returned by the previous nextClick() (or initGamePayload() for first call).
   * @returns {{ row: number, col: number, gameState: * }}
   *
   * Tips:
   *   - Always click purple ("spP") cells immediately — they are free.
   *   - Always click red ("spR") when it appears — also free.
   *   - Each non-purple color gives a neighbour count you can use for
   *     constraint inference (like Minesweeper).
   *   - A cell colored "spO" (4 neighbours) means ALL 8 surrounding cells
   *     are purple — very strong constraint.
   *   - Do not return a (row, col) already in revealed.
   */
  nextClick(revealed, meta, gameState) {
    const clicked = new Set(revealed.map(c => c.row * 5 + c.col));

    // Always collect free clicks first
    const purples = revealed.filter(c => c.color === "spP");
    if (purples.length > 0) return { row: purples[0].row, col: purples[0].col, gameState };

    const reds = revealed.filter(c => c.color === "spR");
    if (reds.length > 0) return { row: reds[0].row, col: reds[0].col, gameState };

    // TODO: replace the random fallback with your click logic
    const unclicked = [];
    for (let r = 0; r < 5; r++)
      for (let c = 0; c < 5; c++)
        if (!clicked.has(r * 5 + c)) unclicked.push([r, c]);

    if (unclicked.length === 0) return { row: 0, col: 0, gameState };
    const [row, col] = unclicked[Math.floor(Math.random() * unclicked.length)];
    return { row, col, gameState };
  }
}

register(new MyOQStrategy());
