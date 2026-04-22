/**
 * random_clicks.js — Random baseline strategy for /sphere harvest (oh).
 *
 * Picks a random unrevealed, unclicked cell on every turn.
 * Prefers purple cells (they are free) if any are visible.
 *
 * This is the simplest possible strategy — no state, no inference.
 * See stateful.js (oq/) for per-game state usage, global_state.js (oc/)
 * for cross-game global state usage.
 */

"use strict";

const { OHStrategy, register } = require("../../interface/strategy.js");

class RandomOHStrategy extends OHStrategy {
  nextClick(revealed, meta, gameState) {
    const clicked = new Set(revealed.map(c => c.row * 5 + c.col));

    // Prefer any visible purple (free click)
    const purples = revealed.filter(c => c.color === "spP");
    if (purples.length > 0) {
      const pick = purples[Math.floor(Math.random() * purples.length)];
      return { row: pick.row, col: pick.col, gameState };
    }

    // Pick a random unclicked cell
    const unclicked = [];
    for (let r = 0; r < 5; r++)
      for (let c = 0; c < 5; c++)
        if (!clicked.has(r * 5 + c)) unclicked.push([r, c]);

    if (unclicked.length === 0) return { row: 0, col: 0, gameState };

    const [row, col] = unclicked[Math.floor(Math.random() * unclicked.length)];
    return { row, col, gameState };
  }
}

register(new RandomOHStrategy());
