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
 * BOARD CELL FORMAT
 * -----------------
 * `board` is always an array of exactly 25 objects, one per cell:
 *   [{ row: number, col: number, color: string, clicked: boolean }, ...]
 * Row and col are 0-indexed (0..4).  color="spU" = covered/unknown.
 * clicked=false = still interactable; clicked=true = disabled.
 *
 * After 3 purples are clicked the 4th appears with color="spR", clicked=false.
 * Click it — it costs 1 click but is worth 150 SP.
 *
 * STATE MODEL
 * -----------
 * State lives inside the Node process — it is never sent back to the harness.
 *
 *   initEvaluationRun()          → run state (once before all games)
 *   initGamePayload(meta, rs)    → game state (reset before each game)
 *   nextClick(board, meta, gs)   → { row, col [, gameState] }
 *
 * initEvaluationRun(): return read-only data shared across all games.
 * initGamePayload():   return fresh per-game state (bridge resets it each game).
 * nextClick():         receive current game state, return { row, col } and
 *                      optionally a new gameState to replace it.
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
   * The returned value is stored by the bridge and passed as `evaluationRunState`
   * to every initGamePayload() call.  Treat it as read-only.
   *
   * @returns {*}  Any value.  Default: null.
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
   * @param {Array<{row: number, col: number, color: string, clicked: boolean}>} board
   *   All 25 board cells.  After 3 purples are clicked, the 4th appears with
   *   color="spR", clicked=false.  Click it (costs 1 click, 150 SP).
   * @param {Object} meta
   *   { clicks_left: number, max_clicks: number, purples_found: number }
   * @param {*} gameState
   *   Value returned by the previous nextClick() (or initGamePayload() for first call).
   * @returns {{ row: number, col: number, gameState?: * }}
   *
   * Tips:
   *   - Click any cell with color="spR" and clicked=false — 150 SP for 1 click.
   *   - Click any cell with color="spP" and clicked=false immediately (free).
   *   - Each non-purple color gives a neighbour count for constraint inference.
   *   - Do not return a (row, col) where board[row*5+col].clicked is true.
   */
  nextClick(board, meta, gameState) {
    const clicked = new Set(board.filter(c => c.clicked).map(c => c.row * 5 + c.col));

    // Always click red when it appears — 150 SP for 1 click
    const reds = board.filter(c => c.color === "spR" && !c.clicked);
    if (reds.length > 0) return { row: reds[0].row, col: reds[0].col };

    // Always click purple immediately (free)
    const purples = board.filter(c => c.color === "spP" && !c.clicked);
    if (purples.length > 0) return { row: purples[0].row, col: purples[0].col };

    // TODO: replace the random fallback with your click logic
    const unclicked = [];
    for (let r = 0; r < 5; r++)
      for (let c = 0; c < 5; c++)
        if (!clicked.has(r * 5 + c)) unclicked.push([r, c]);

    if (unclicked.length === 0) return { row: 0, col: 0 };
    const [row, col] = unclicked[Math.floor(Math.random() * unclicked.length)];
    return { row, col };
  }
}

register(new MyOQStrategy());
