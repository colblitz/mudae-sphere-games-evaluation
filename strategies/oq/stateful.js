/**
 * stateful.js — Stateful example strategy for /sphere quest (oq).
 *
 * Demonstrates how to use initGamePayload() and the threaded state payload to
 * maintain information across clicks within a single game.
 *
 * WHY PER-GAME STATE?
 * -------------------
 * The game_state value is threaded through every call within a game:
 *
 *   initGamePayload(meta, evaluationRunState)             → initial state for this game
 *   nextClick(revealed, meta, s0)    → { row, col, gameState: s1 }
 *   nextClick(revealed, meta, s1)    → { row, col, gameState: s2 }
 *   ...
 *
 * initGamePayload() is called once at the start of EACH game, making it the right
 * place to reset anything that should be fresh for every game.  Whatever it
 * returns becomes the gameState for that game's first nextClick() call.
 *
 * Contrast with initEvaluationRun(), which is called only ONCE before all games
 * begin — use that for cross-game global tables (see oc/global_state.js).
 *
 * STRATEGY LOGIC
 * --------------
 * We track which rows and columns have already been clicked this game.
 * When choosing the next cell, we prefer cells whose row AND column are both
 * new (maximising board coverage).  If no such cell exists we fall back to
 * cells with at least one new axis, then to any unclicked cell.
 *
 * The click history is accumulated in the gameState object across calls within a
 * game and is reset to empty by initGamePayload() at the start of each new game.
 */

"use strict";

const { OQStrategy, register } = require("../../interface/strategy.js");

class StatefulOQStrategy extends OQStrategy {
  /**
   * Reset per-game tracking at the start of every game.
   *
   * Called once before the first nextClick() of each game.  The `evaluationRunState`
   * argument here is whatever initEvaluationRun() returned (null by default);
   * we ignore it and return a fresh object instead.
   *
   * @param {Object} meta   { clicks_left, max_clicks, purples_found }
   * @param {*}      evaluationRunState  Read-only value from initEvaluationRun() — ignored here
   * @returns {{ clickedRows: number[], clickedCols: number[], clickCount: number }}
   */
  initGamePayload(meta, evaluationRunState) {
    return {
      clickedRows: [],   // rows clicked so far this game
      clickedCols: [],   // cols clicked so far this game
      clickCount: 0,     // total clicks made this game
    };
  }

  /**
   * Choose the next cell, preferring unexplored rows and columns.
   *
   * The gameState object carries click history accumulated across all previous
   * calls this game.  We update it and return the new version.
   *
   * @param {Array<{row: number, col: number, color: string}>} revealed
   * @param {Object} meta   { clicks_left, max_clicks, purples_found }
   * @param {{ clickedRows: number[], clickedCols: number[], clickCount: number }} gameState
   * @returns {{ row: number, col: number, gameState: object }}
   */
  nextClick(revealed, meta, gameState) {
    const clickedSet = new Set(revealed.map(c => c.row * 5 + c.col));
    const rowSet = new Set(gameState.clickedRows);
    const colSet = new Set(gameState.clickedCols);

    const newBoth = [];   // row AND col are new
    const newOne  = [];   // either row or col is new
    const fallback = [];  // neither row nor col is new

    for (let r = 0; r < 5; r++) {
      for (let c = 0; c < 5; c++) {
        if (clickedSet.has(r * 5 + c)) continue;
        const newR = !rowSet.has(r);
        const newC = !colSet.has(c);
        if (newR && newC)      newBoth.push([r, c]);
        else if (newR || newC) newOne .push([r, c]);
        else                   fallback.push([r, c]);
      }
    }

    const candidates = newBoth.length ? newBoth : newOne.length ? newOne : fallback;
    if (!candidates.length) return { row: 0, col: 0, gameState };

    const [row, col] = candidates[Math.floor(Math.random() * candidates.length)];

    // Update and return the new state — becomes state on the next call
    const newState = {
      clickedRows: [...gameState.clickedRows, row],
      clickedCols: [...gameState.clickedCols, col],
      clickCount: gameState.clickCount + 1,
    };
    return { row, col, gameState: newState };
  }
}

register(new StatefulOQStrategy());
