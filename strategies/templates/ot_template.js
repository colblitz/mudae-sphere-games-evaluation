/**
 * ot_template.js — Template strategy for /sphere trace (ot) — JavaScript.
 *
 * Copy this file to strategies/ot/<your_name>.js, rename the class, and
 * fill in the TODO sections.  Delete any methods you don't need — only
 * nextClick() is required.
 *
 * GAME OVERVIEW
 * -------------
 * Grid   : 5×5, all 25 cells start covered
 * Budget : 4 BLUE clicks (ship cells are FREE)
 * Goal   : find ship cells (free) while minimising wasted blue clicks
 *
 * Ships are contiguous horizontal or vertical segments; no overlap.
 * Blue (spB) cells are empty — clicking one costs 1 of your 4 blue budget.
 * Ship cells do not cost a click.
 *
 * Extra Chance rule:
 *   If blue #4 is spent before ships_hit >= 5, the game continues.
 *   Each additional blue while ships_hit < 5 extends the game.
 *   Once ships_hit >= 5, the next blue click ends the game immediately.
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
 * meta keys (ot):
 *   n_colors    number  ship color count (6, 7, 8, or 9)
 *   ships_hit   number  ship cells revealed so far
 *   blues_used  number  blue clicks spent so far
 *   max_clicks  number  base blue budget (always 4)
 *
 * See also
 * --------
 * oh/random_clicks.js   — minimal stateless example
 * oc/global_state.js    — initEvaluationRun() (global state) example
 * oq/stateful.js        — initGamePayload() + state threading (per-game state) example
 */

"use strict";

const { OTStrategy, register } = require("../../interface/strategy.js");

class MyOTStrategy extends OTStrategy {
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
   * Example: for each n_colors variant, enumerate all valid ship placements
   * and store per-cell marginal probabilities of being a ship cell.
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
   * @param {Object} meta   { n_colors, ships_hit, blues_used, max_clicks }
   * @param {*}      evaluationRunState  Read-only value from initEvaluationRun()
   * @returns {*}   Initial gameState for this game's first nextClick() call.
   *
   * Tip: use meta.n_colors to select the right precomputed prior from the
   * global table returned by initEvaluationRun().
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
   *   { n_colors: number, ships_hit: number, blues_used: number, max_clicks: number }
   * @param {*} gameState
   *   Value returned by the previous nextClick() (or initGamePayload() for first call).
   * @returns {{ row: number, col: number, gameState: * }}
   *
   * Tips:
   *   - Clicked ship cells constrain remaining ship placements (ships are
   *     contiguous horizontal or vertical segments of fixed per-color length).
   *   - A clicked blue at (r,c) eliminates all placements passing through it.
   *   - After hitting one cell of a ship, adjacent cells along the same axis
   *     are candidate continuations.
   *   - Prefer cells with high P(ship) to avoid wasting blue clicks.
   *   - Ship lengths: spT=4, spG=3, spY=3, spO/spL/spD/spR/spW=2
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

register(new MyOTStrategy());
