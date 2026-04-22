/**
 * global_state.js — Global-state example strategy for /sphere chest (oc).
 *
 * Demonstrates how to use initEvaluationRun() to compute data ONCE before all
 * games in an evaluation run, then reuse it cheaply across every game.
 *
 * The payload here is a fixed cell visit order — a spiral from the center
 * outward.  Every game simply walks down this list and clicks the first cell
 * that hasn't been revealed yet.
 *
 * WHY GLOBAL STATE?
 * -----------------
 * initEvaluationRun() is called exactly once per evaluation run (before any games
 * start).  The returned value is passed as `state` to initGamePayload() at the start
 * of every game, and from there flows through every nextClick() call.
 *
 * Use initEvaluationRun() for anything that is:
 *   - Expensive to compute (search, optimisation, loading a table).
 *   - Identical for every game (board-independent precomputation).
 *   - Read-only during play (the lookup table never changes mid-run).
 *
 * Contrast with initGamePayload(), which is called once per game and is the right
 * place to reset per-game bookkeeping — see oq/stateful.js for that pattern.
 *
 * STRATEGY LOGIC
 * --------------
 * The center cell (2,2) can never be red, so it is visited last.
 * All other cells are visited in a deterministic clockwise spiral from the
 * outermost ring inward.  The order is built once in initEvaluationRun() and stored
 * in the state object; nextClick() does a simple linear scan.
 */

"use strict";

const { OCStrategy, register } = require("../../interface/strategy.js");

// ---------------------------------------------------------------------------
// Build the spiral visit order (runs once at module load time)
// ---------------------------------------------------------------------------

function buildSpiralOrder() {
  const seen = new Set();
  const order = [];

  const add = (r, c) => {
    if (r < 0 || r >= 5 || c < 0 || c >= 5) return;
    const idx = r * 5 + c;
    if (seen.has(idx)) return;
    seen.add(idx);
    order.push([r, c]);
  };

  // Shells by Chebyshev distance from center (2,2), starting at dist=1
  for (let dist = 1; dist <= 2; dist++) {
    const rs = 2 - dist, re = 2 + dist;
    const cs = 2 - dist, ce = 2 + dist;
    // Top row left→right
    for (let c = cs; c <= ce; c++) add(rs, c);
    // Right col top→bottom
    for (let r = rs + 1; r <= re; r++) add(r, ce);
    // Bottom row right→left
    for (let c = ce - 1; c >= cs; c--) add(re, c);
    // Left col bottom→top
    for (let r = re - 1; r > rs; r--) add(r, cs);
  }

  // Center last (can never be red)
  add(2, 2);

  // Fill any remaining cells (belt-and-suspenders)
  for (let r = 0; r < 5; r++)
    for (let c = 0; c < 5; c++)
      add(r, c);

  return order;
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------

class GlobalStateOCStrategy extends OCStrategy {
  /**
   * Called ONCE before all games.  Build the visit order here so we pay
   * the construction cost only once across the entire evaluation run.
   *
   * The returned value is passed as `state` to every subsequent initGamePayload()
   * and nextClick() call — treat it as a read-only global table.
   *
   * @returns {{ order: Array<[number, number]> }}
   */
  initEvaluationRun() {
    return { order: buildSpiralOrder() };
  }

  // initGamePayload() is intentionally omitted: the default returns state unchanged,
  // which is exactly what we want — no per-game reset needed.

  /**
   * Pick the first cell in the precomputed order that isn't revealed yet.
   *
   * @param {Array<{row: number, col: number, color: string}>} revealed
   * @param {Object} meta  { clicks_left, max_clicks }
   * @param {{ order: Array<[number, number]> }} state  Global visit order
   * @returns {{ row: number, col: number, state: object }}
   */
  nextClick(revealed, meta, state) {
    const clicked = new Set(revealed.map(c => c.row * 5 + c.col));

    for (const [row, col] of state.order) {
      if (!clicked.has(row * 5 + col)) {
        return { row, col, state };  // state is unchanged — shared read-only table
      }
    }

    // Fallback: should never be reached on a valid board
    return { row: 0, col: 0, state };
  }
}

register(new GlobalStateOCStrategy());
