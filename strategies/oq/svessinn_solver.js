/**
 * svessinn_solver.js — Faithful port of Svessinn's Quest solver for /sphere quest (oq).
 *
 * Source: https://github.com/Svessinn/Svessinn.github.io/blob/main/Mudae/Spheres/Quest/solver.html
 *
 * ALGORITHM OVERVIEW
 * ------------------
 * The solver treats the board as a Minesweeper-style constraint problem.
 * There are exactly 4 purple spheres hidden in 25 cells.  All C(25,4) = 12,650
 * possible layouts are enumerated once (in initEvaluationRun) and filtered each
 * turn against the revealed clue cells.
 *
 * Non-purple cells reveal the count of purple neighbours (all 8 orthogonal +
 * diagonal neighbours, capped at 4) encoded as a color:
 *   spB = 0,  spT = 1,  spG = 2,  spY = 3,  spO = 4
 *
 * Purple (spP) and red (spR) cells appear as sphere finds; they constrain the
 * layout set directly (the revealed cell must be in the combo).
 *
 * Two phases:
 *
 * Phase 1 — fewer than 4 spheres found (spheresFound < 4):
 *   Score each unclicked cell as:
 *     percentPurpleProbability * 10 + number_of_unrevealed_neighbours
 *   Pick the highest score.  This blends finding purples quickly with
 *   gathering information from neighbour counts.
 *
 * Phase 2 — all 4 spheres found (spheresFound >= 4):
 *   For each unclicked cell, compute the expected reward by weighting
 *   REWARDS[k] by the fraction of remaining valid layouts that give it
 *   exactly k purple neighbours.  Pick the highest EV cell.
 *
 * REWARDS table (from solver.html):
 *   0 → 10 SP,  1 → 20 SP,  2 → 35 SP,  3 → 55 SP,  4 → 90 SP,
 *   P → 5 SP,   R → 150 SP
 *
 * Important notes:
 *   - Purple (spP) clicks are FREE — the harness does not decrement clicks_left.
 *   - Red (spR) appears after 3 purples and is also free.
 *   - The revealed list from the harness includes ALL revealed cells (purples,
 *     reds, and numbered clues).  We must handle all of them when filtering.
 *   - We never return a cell that is already in revealed.
 *
 * State threading:
 *   initEvaluationRun()          → { allCombos }   (12,650 combos, computed once)
 *   initGamePayload(meta, rs)    → { allCombos, revealed: [] }
 *   nextClick(revealed, meta, s) → { row, col, gameState: s' }
 */

"use strict";

const { OQStrategy, register } = require("../../interface/strategy.js");

// ---------------------------------------------------------------------------
// Reward table (from solver.html's REWARDS constant)
// ---------------------------------------------------------------------------

const REWARDS = { 0: 10, 1: 20, 2: 35, 3: 55, 4: 90, P: 5, R: 150 };

// ---------------------------------------------------------------------------
// Color string → internal value mapping
// The solver uses numeric neighbour counts (0-4) for clue cells, and "P"/"R"
// for sphere cells.
// ---------------------------------------------------------------------------

const COLOR_TO_VAL = {
  spB: 0, spT: 1, spG: 2, spY: 3, spO: 4,
  spP: "P", spR: "R",
};

// ---------------------------------------------------------------------------
// Geometry helper (direct port from solver.html)
// ---------------------------------------------------------------------------

const SIZE = 5;

/**
 * Return the flat indices of all 8 neighbours of cell idx (within 5×5 grid).
 * Mirrors getNeighbors() in solver.html.
 */
function getNeighbors(idx) {
  const n = [];
  const r = Math.floor(idx / SIZE);
  const c = idx % SIZE;
  for (let dr = -1; dr <= 1; dr++) {
    for (let dc = -1; dc <= 1; dc++) {
      if (dr === 0 && dc === 0) continue;
      const nr = r + dr, nc = c + dc;
      if (nr >= 0 && nr < SIZE && nc >= 0 && nc < SIZE) {
        n.push(nr * SIZE + nc);
      }
    }
  }
  return n;
}

// ---------------------------------------------------------------------------
// Combination generator (direct port from solver.html)
// ---------------------------------------------------------------------------

/**
 * Generate all combinations of arr taken k at a time.
 * Mirrors getCombinations() in solver.html.
 */
function getCombinations(arr, k) {
  const results = [];
  function helper(start, combo) {
    if (combo.length === k) { results.push([...combo]); return; }
    for (let i = start; i < arr.length; i++) helper(i + 1, [...combo, arr[i]]);
  }
  helper(0, []);
  return results;
}

// Pre-generate all 12,650 combos at module load time for fast access
// (also stored in run_state so it flows cleanly through the interface)
const ALL_COMBOS = getCombinations(Array.from({ length: 25 }, (_, i) => i), 4);

// ---------------------------------------------------------------------------
// Core solver logic (direct port of calculate() from solver.html)
// ---------------------------------------------------------------------------

/**
 * Filter ALL_COMBOS against the current revealed list and compute per-cell
 * purple probabilities.
 *
 * revealedList: [{ idx, val }] where val is 0-4 (clue) or "P"/"R" (sphere)
 *
 * Mirrors calculate() in solver.html.
 */
function calculate(revealedList, allCombos) {
  // Filter combos to those consistent with all revealed clues
  const filtered = allCombos.filter(combo => {
    return revealedList.every(rev => {
      if (rev.val === "P" || rev.val === "R") {
        // Sphere cell — must be in this combo
        return combo.includes(rev.idx);
      }
      // Clue cell — must not be a purple, and neighbour count must match
      return (
        !combo.includes(rev.idx) &&
        getNeighbors(rev.idx).filter(nIdx => combo.includes(nIdx)).length === rev.val
      );
    });
  });

  // Per-cell purple probability (count of layouts containing each cell)
  const probs = Array(25).fill(0);
  for (const combo of filtered) {
    for (const idx of combo) probs[idx]++;
  }
  const percentProbs = probs.map(p => (filtered.length ? (p / filtered.length) * 100 : 0));

  // Count spheres found
  const purplesFound = revealedList.filter(r => r.val === "P").length;
  const redFound     = revealedList.some(r => r.val === "R");
  const spheresFound = purplesFound + (redFound ? 1 : 0);

  // Best cell index
  const revealedIdxSet = new Set(revealedList.map(r => r.idx));
  let bestIdx = -1, maxVal = -1;

  for (let i = 0; i < 25; i++) {
    if (revealedIdxSet.has(i)) continue;

    if (spheresFound < 4) {
      // Phase 1: blend purple probability with information value
      const score = percentProbs[i] * 10 +
        getNeighbors(i).filter(nIdx => !revealedIdxSet.has(nIdx)).length;
      if (score > maxVal) { maxVal = score; bestIdx = i; }
    } else {
      // Phase 2: maximise expected reward from neighbour-count distribution
      let ev = 0;
      for (let opt = 0; opt <= 4; opt++) {
        const count = filtered.filter(combo =>
          getNeighbors(i).filter(nIdx => combo.includes(nIdx)).length === opt
        ).length;
        ev += (count / filtered.length) * REWARDS[opt];
      }
      if (ev > maxVal) { maxVal = ev; bestIdx = i; }
    }
  }

  return { percentProbs, validCount: filtered.length, bestIdx, spheresFound, filteredLayouts: filtered };
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------

class SvessinOQStrategy extends OQStrategy {
  /**
   * Pre-generate the 12,650 combos once for the whole evaluation run.
   * Stored as module-level ALL_COMBOS; we return null here since the combos
   * don't need to be serialized through the state channel.
   */
  initEvaluationRun() {
    return null;
  }

  /**
   * Reset the per-game revealed list at the start of each game.
   * allCombos is the module-level constant — not threaded through state.
   */
  initGamePayload(meta, evaluationRunState) {
    return { revealedList: [] };
  }

  /**
   * Choose the next cell using Svessinn's two-phase constraint solver.
   *
   * @param {Array<{row, col, color}>} revealed  All cells revealed so far this game
   * @param {Object} meta  { clicks_left, max_clicks, purples_found }
   * @param {{ revealedList: Array }} gameState
   * @returns {{ row: number, col: number, gameState: object }}
   */
  nextClick(board, meta, gameState) {
    // --- Rebuild revealedList from clicked cells on the board ---
    // We rebuild from scratch each call.
    const revealedList = [];
    for (const cell of board.filter(c => c.clicked)) {
      const idx = cell.row * 5 + cell.col;
      const val = COLOR_TO_VAL[cell.color];
      if (val === undefined) continue; // unknown color — skip
      revealedList.push({ idx, val });
    }

    const revealedIdxSet = new Set(revealedList.map(r => r.idx));

    // --- Run the solver using the module-level combo list ---
    const { bestIdx } = calculate(revealedList, ALL_COMBOS);

    let targetIdx = bestIdx;

    // Fallback if solver returns -1 (no valid move — shouldn't normally happen)
    if (targetIdx === -1) {
      for (let i = 0; i < 25; i++) {
        if (!revealedIdxSet.has(i)) { targetIdx = i; break; }
      }
    }

    const row = Math.floor(targetIdx / 5);
    const col = targetIdx % 5;

    // Only thread the small revealedList through state, not the 12,650 combos
    return { row, col, gameState: { revealedList } };
  }
}

register(new SvessinOQStrategy());
