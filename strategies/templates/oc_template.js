/**
 * oc_template.js — Template strategy for /sphere chest (oc) — JavaScript.
 *
 * Copy this file to strategies/oc/<your_name>.js, rename the class, and
 * fill in the TODO sections.  Delete any methods you don't need — only
 * nextClick() is required.
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
 * BOARD CELL FORMAT
 * -----------------
 * `board` is always an array of exactly 25 objects, one per cell:
 *   [{ row: number, col: number, color: string, clicked: boolean }, ...]
 * Row and col are 0-indexed (0..4).  color="spU" = covered/unknown.
 * clicked=false = still interactable; clicked=true = disabled.
 *
 * STATE PAYLOAD
 * -------------
 * The game_state value is threaded through every call within a game:
 *
 *   initEvaluationRun()               → initialState (called once before all games)
 *   initGamePayload(meta, s0)           → s1           (called once per game)
 *   nextClick(board,meta,s1) → {row,col,gameState:s2}
 *   ...
 *
 * Use initEvaluationRun() for data computed ONCE and shared across all games.
 * Use initGamePayload() to reset per-game bookkeeping at the start of each game.
 *
 * meta keys (oc):
 *   clicks_left  number  remaining budget
 *   max_clicks   number  total budget (always 5)
 *
 * See also
 * --------
 * oh/random_clicks.js   — minimal stateless example
 * oc/global_state.js    — initEvaluationRun() (global state) example
 * oq/stateful.js        — initGamePayload() + state threading (per-game state) example
 */

"use strict";

const { OCStrategy, register } = require("../../interface/strategy.js");

class MyOCStrategy extends OCStrategy {
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
   *
   * Example: pre-rank all 25 cells by expected SP under a uniform prior over
   * all 16,800 possible red positions, so nextClick() can do a fast scan.
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
   * @param {Object} meta   { clicks_left, max_clicks }
   * @param {*}      evaluationRunState  Read-only value from initEvaluationRun()
   * @returns {*}   Initial gameState for this game's first nextClick() call.
   *
   * Example: initialise a posterior distribution over possible red positions
   * (a Set of all 24 valid positions), to be pruned as colors are revealed.
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
   *   All 25 board cells.
   * @param {Object} meta
   *   { clicks_left: number, max_clicks: number }
   * @param {*} gameState
   *   Value returned by the previous nextClick() (or initGamePayload() for first call).
   * @returns {{ row: number, col: number, gameState: * }}
   *
   * Tips:
   *   - Each clicked color constrains where red can be.
   *   - Once red is found, all remaining colors are fully determined —
   *     greedily pick the highest-value unclicked cell (orange first, etc.).
   *   - Orange (90 SP) is always orthogonally adjacent to red.
   *   - Do not return a (row, col) where board[row*5+col].clicked is true.
   */
  nextClick(board, meta, gameState) {
    const clicked = new Set(board.filter(c => c.clicked).map(c => c.row * 5 + c.col));

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

register(new MyOCStrategy());
