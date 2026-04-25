/**
 * gap22_backtrack_solver.js — Backtracking constraint solver for /sphere quest (oq).
 *
 * Ported from: https://gap22.github.io/oq-solver/
 *
 * Algorithm:
 *
 *   1. Parse the current board into:
 *        knownNumbers  — Map(cellIdx → neighborPurpleCount) for all clicked
 *                        non-purple cells
 *        knownMines    — Set of cellIdx for all clicked purple/red cells
 *        available     — array of unclicked cell indices (candidates for the
 *                        remaining hidden purples)
 *
 *   2. Run a backtracking search over `available` to enumerate all ways to
 *      place the remaining (4 − knownMines.size) purples.  Early pruning:
 *      if placing a purple at position p would push any adjacent constraint
 *      cell above its expected count, skip that branch immediately.
 *      Track validMineCount[i] = number of valid scenarios where cell i is
 *      purple.
 *
 *   3. For each unclicked cell i compute:
 *        prob   = validMineCount[i] / validScenarios
 *        points = expected adjacency score
 *               = Σ_{n ∈ neighbours(i)} [ 1 if n is a known mine,
 *                                         else validMineCount[n]/validScenarios ]
 *      (The `points` field mirrors the original solver's `expectedPoints`
 *       heuristic, which estimates how informative / high-value a non-purple
 *       click would be.)
 *
 *   4. Decision priority (verbatim from gap22's recommendation engine):
 *        a. Certain purple (prob === 1.0) — click it.
 *           (4th certain purple at prob===1.0 is the future-red; clicking it
 *            as purple is free and triggers the red reveal.)
 *        b. All 4 purples are located (knownMines.size + certainCount >= 4):
 *           harvest mode — sort unclicked non-certain cells by points desc,
 *           click the highest.
 *        c. Otherwise — sort by prob desc, break ties by points desc.
 *
 * This strategy is stateless: no gameState is threaded through calls.
 * The neighbour table is precomputed once in initEvaluationRun().
 *
 * Color mapping (harness sp-codes):
 *   spP → purple (5 SP, free)   spR → red (150 SP, costs 1 click)
 *   spB → 0 nb   spT → 1 nb   spG → 2 nb   spY → 3 nb   spO → 4 nb
 */

"use strict";

const { OQStrategy, register } = require("../../interface/strategy.js");

const ROWS        = 5;
const COLS        = 5;
const TOTAL_CELLS = ROWS * COLS;
const TOTAL_PURPLES = 4;

// Map harness neighbor-count color codes to integer counts
const SP_TO_NB_COUNT = { spB: 0, spT: 1, spG: 2, spY: 3, spO: 4 };

// ---------------------------------------------------------------------------
// 8-way neighbour table (computed once at module load as a fallback, but also
// returned by initEvaluationRun so it can be cached on run state).
// ---------------------------------------------------------------------------
function buildNeighborTable() {
  const table = new Array(TOTAL_CELLS);
  for (let i = 0; i < TOTAL_CELLS; i++) {
    const r = (i / COLS) | 0;
    const c = i % COLS;
    const nb = [];
    for (let dr = -1; dr <= 1; dr++) {
      for (let dc = -1; dc <= 1; dc++) {
        if (dr === 0 && dc === 0) continue;
        const nr = r + dr, nc = c + dc;
        if (nr >= 0 && nr < ROWS && nc >= 0 && nc < COLS) {
          nb.push(nr * COLS + nc);
        }
      }
    }
    table[i] = nb;
  }
  return table;
}

const MODULE_NEIGHBORS = buildNeighborTable();

// ---------------------------------------------------------------------------
// Core backtracking solver (mirrors gap22's calculateProbabilities / solve)
// ---------------------------------------------------------------------------

/**
 * Run the backtracking search and return { validScenarios, validMineCount }.
 *
 * @param {Map<number,number>} knownNumbers  cellIdx → expected nb-purple count
 * @param {Set<number>}        knownMines    cell indices that are confirmed purple
 * @param {number[]}           available     unclicked, unoccupied cell indices
 * @param {number[][]}         neighbors     precomputed 8-way neighbour table
 */
function solve(knownNumbers, knownMines, available, neighbors) {
  const minesToPlace = TOTAL_PURPLES - knownMines.size;

  let validScenarios = 0;
  const validMineCount = new Array(TOTAL_CELLS).fill(0);

  // minesAroundConstraint[i] = how many known mines are adjacent to cell i
  // (pre-seeded with the already-clicked purples/reds)
  const minesAround = new Array(TOTAL_CELLS).fill(0);
  for (const mine of knownMines) {
    for (const nb of neighbors[mine]) {
      minesAround[nb]++;
    }
  }

  // Collect active constraints into an array for O(k) final validation
  const constraints = [];
  for (const [pos, expected] of knownNumbers.entries()) {
    constraints.push({ pos, expected });
  }

  const currentCombo = [];

  function bt(idx, placed) {
    if (placed === TOTAL_PURPLES) {
      // Verify all constraints are exactly satisfied
      for (const { pos, expected } of constraints) {
        if (minesAround[pos] !== expected) return;
      }
      validScenarios++;
      for (const mine of currentCombo) {
        validMineCount[mine]++;
      }
      return;
    }

    if (idx >= available.length) return;

    const pos = available[idx];

    // --- Try placing a mine here ---
    let canPlace = true;
    for (const nb of neighbors[pos]) {
      if (knownNumbers.has(nb) && minesAround[nb] + 1 > knownNumbers.get(nb)) {
        canPlace = false;
        break;
      }
    }

    if (canPlace) {
      for (const nb of neighbors[pos]) minesAround[nb]++;
      currentCombo.push(pos);

      bt(idx + 1, placed + 1);

      currentCombo.pop();
      for (const nb of neighbors[pos]) minesAround[nb]--;
    }

    // --- Skip this cell ---
    bt(idx + 1, placed);
  }

  bt(0, knownMines.size);

  return { validScenarios, validMineCount };
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------

class Gap22BacktrackSolverOQStrategy extends OQStrategy {
  /**
   * Precompute the 8-way neighbour table once.  Returned as run state and
   * passed to every initGamePayload call (read-only).
   */
  initEvaluationRun() {
    return { neighbors: buildNeighborTable() };
  }

  /**
   * Choose the next cell to click.
   *
   * Priority order:
   *   1. Unclicked red (spR)   → click immediately (150 SP, 1 click)
   *   2. Unclicked purple (spP) → click immediately (free)
   *   3. Run backtracking solver → apply gap22 recommendation logic
   */
  nextClick(board, meta, gameState) {
    const neighbors = (gameState && gameState.neighbors)
      ? gameState.neighbors
      : MODULE_NEIGHBORS;

    // 1. Red takes absolute priority
    for (const cell of board) {
      if (cell.color === "spR" && !cell.clicked) {
        return { row: cell.row, col: cell.col };
      }
    }

    // 2. Visible unclicked purple is free — always take it
    for (const cell of board) {
      if (cell.color === "spP" && !cell.clicked) {
        return { row: cell.row, col: cell.col };
      }
    }

    // 3. Parse board state
    const knownNumbers   = new Map();   // cellIdx → nb-purple count
    const knownMines     = new Set();   // cellIdx of clicked purples/reds
    const occupiedIdx    = new Set();   // all clicked cells
    const available      = [];          // unclicked cell indices

    for (const cell of board) {
      const idx = cell.row * COLS + cell.col;
      if (cell.clicked) {
        occupiedIdx.add(idx);
        if (cell.color === "spP" || cell.color === "spR") {
          knownMines.add(idx);
        } else if (SP_TO_NB_COUNT[cell.color] !== undefined) {
          knownNumbers.set(idx, SP_TO_NB_COUNT[cell.color]);
        }
      }
    }

    for (let i = 0; i < TOTAL_CELLS; i++) {
      if (!occupiedIdx.has(i)) available.push(i);
    }

    // If all purples already clicked and red clicked (full harvest)
    // or no cells left, just pick any unclicked cell
    if (available.length === 0) {
      return { row: 0, col: 0 };
    }

    // If we already have all 4 purples clicked — harvest phase (no solver needed)
    if (knownMines.size >= TOTAL_PURPLES) {
      // Score by expected adjacency: number of purple neighbours
      let bestIdx = available[0], bestPoints = -1;
      for (const i of available) {
        let points = 0;
        for (const nb of neighbors[i]) {
          if (knownMines.has(nb)) points++;
        }
        if (points > bestPoints) { bestPoints = points; bestIdx = i; }
      }
      return { row: (bestIdx / COLS) | 0, col: bestIdx % COLS };
    }

    // 4. Run backtracking solver
    const { validScenarios, validMineCount } = solve(
      knownNumbers, knownMines, available, neighbors
    );

    // Degenerate / inconsistent board state — fall back to first available cell
    if (validScenarios === 0) {
      return { row: (available[0] / COLS) | 0, col: available[0] % COLS };
    }

    // 5. Build cell stats (prob, expectedPoints) for unclicked cells
    const cellStats = [];
    let totalCertainPurples = knownMines.size;

    for (const i of available) {
      const prob = validMineCount[i] / validScenarios;
      if (prob === 1) totalCertainPurples++;

      // expectedPoints mirrors gap22: expected number of purple neighbours
      // (a proxy for the neighbour-count value of clicking this cell)
      let points = 0;
      for (const nb of neighbors[i]) {
        if (knownMines.has(nb)) {
          points += 1;
        } else if (!occupiedIdx.has(nb)) {
          points += validMineCount[nb] / validScenarios;
        }
        // occupied non-mine nb contributes 0
      }

      cellStats.push({ idx: i, prob, points });
    }

    // 6. Apply gap22 recommendation logic
    // a. Certain purple (prob===1) — always click it first
    const certainPurple = cellStats.find(c => c.prob === 1);
    if (certainPurple) {
      return {
        row: (certainPurple.idx / COLS) | 0,
        col:  certainPurple.idx % COLS,
      };
    }

    // b. All 4 purples located → harvest mode: sort by expectedPoints desc
    if (totalCertainPurples >= TOTAL_PURPLES) {
      cellStats.sort((a, b) => b.points - a.points);
      const best = cellStats[0];
      return { row: (best.idx / COLS) | 0, col: best.idx % COLS };
    }

    // c. General case: sort by prob desc, tiebreak by points desc
    // Uses exact equality for prob comparison, matching gap22's sort comparator.
    cellStats.sort((a, b) => {
      if (b.prob !== a.prob) return b.prob - a.prob;
      return b.points - a.points;
    });

    const best = cellStats[0];
    return { row: (best.idx / COLS) | 0, col: best.idx % COLS };
  }
}

register(new Gap22BacktrackSolverOQStrategy());
