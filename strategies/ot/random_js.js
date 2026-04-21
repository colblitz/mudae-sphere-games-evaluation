/**
 * random_js.js — Random baseline strategy for /sphere trace (ot).
 * Picks a random unclicked cell on every turn.
 */

"use strict";

const { OTStrategy, register } = require("../../interface/strategy.js");

class RandomOTStrategy extends OTStrategy {
  nextClick(revealed, meta, state) {
    const clicked = new Set(revealed.map(c => c.row * 5 + c.col));
    const unclicked = [];
    for (let r = 0; r < 5; r++)
      for (let c = 0; c < 5; c++)
        if (!clicked.has(r * 5 + c)) unclicked.push([r, c]);
    if (unclicked.length === 0) return { row: 0, col: 0, state };
    const [row, col] = unclicked[Math.floor(Math.random() * unclicked.length)];
    return { row, col, state };
  }
}

register(new RandomOTStrategy());
