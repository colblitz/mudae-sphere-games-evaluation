/**
 * adaptive_ev.js — Bayesian adaptive-EV strategy for /sphere chest (oc).
 *
 * Algorithm (ported from solvespheres.kelinimo.workers.dev / sphere.js):
 *
 *   1. Maintain a set of candidate red positions (all 24 non-center cells
 *      initially).  After each reveal, eliminate any candidate that is
 *      inconsistent with the observed color given that candidate as red.
 *
 *   2. For each unrevealed cell, compute expected value and information
 *      entropy over the current candidate set.
 *
 *   3. Pick the cell that maximises a weighted combination of EV and entropy,
 *      with adaptive weights based on how many clicks have been made and how
 *      many candidates remain:
 *        - Default:  infoW=0.65, pointsW=0.35
 *        - Early (< 2 clicks, > 12 candidates): infoW=0.80, pointsW=0.20
 *        - Late  (<= 5 candidates):             infoW=0.40, pointsW=0.60
 *
 * Color mapping (harness sp-codes → web solver color names):
 *   spR → red   (150 SP)
 *   spO → orange ( 90 SP)
 *   spY → yellow ( 55 SP)
 *   spG → green  ( 35 SP)
 *   spT → teal   ( 20 SP)
 *   spB → blue   ( 10 SP)
 *
 * State: { possibleRed: number[] }  — array of candidate red cell indices
 *   (index = row*5+col).  Rebuilt into a Set each call for fast lookup.
 *   Stored as a plain array so it survives JSON serialisation by the harness.
 */

"use strict";

const { OCStrategy, register } = require("../../interface/strategy.js");

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const GRID = 5;
const LOG2_6 = Math.log2(6);

// harness sp-code → internal color name
const SP_TO_COLOR = {
  spR: "red", spO: "orange", spY: "yellow",
  spG: "green", spT: "teal", spB: "blue",
};

const SPHERE_SCORES = { red: 150, orange: 90, yellow: 55, green: 35, teal: 20, blue: 10 };

// All 25 cell indices 0..24
const ALL_CELLS = Array.from({ length: 25 }, (_, i) => i);

// Center cell index (2,2) — can never be red
const CENTER = 2 * 5 + 2; // 12

// ---------------------------------------------------------------------------
// Geometry helpers (operating on flat cell indices)
// ---------------------------------------------------------------------------

// Precompute all geometry sets once at module load — these never change.

function _r(idx) { return (idx / 5) | 0; }
function _c(idx) { return idx % 5; }

// Orthogonal neighbors of `idx` (|dr|+|dc|==1)
function buildOrthNeighbors() {
  const result = new Array(25);
  for (let i = 0; i < 25; i++) {
    const r = _r(i), c = _c(i);
    const nb = [];
    for (const [dr, dc] of [[-1,0],[1,0],[0,-1],[0,1]]) {
      const nr = r+dr, nc = c+dc;
      if (nr >= 0 && nr < GRID && nc >= 0 && nc < GRID) nb.push(nr*5+nc);
    }
    result[i] = nb;
  }
  return result;
}

// Diagonal ray positions from `idx` (|dr|==|dc|, dr!=0) — full rays, not just 1 step
function buildDiagSets() {
  const result = new Array(25);
  for (let i = 0; i < 25; i++) {
    const r = _r(i), c = _c(i);
    const s = new Set();
    for (const [dr, dc] of [[-1,-1],[-1,1],[1,-1],[1,1]]) {
      let nr = r+dr, nc = c+dc;
      while (nr >= 0 && nr < GRID && nc >= 0 && nc < GRID) {
        s.add(nr*5+nc);
        nr += dr; nc += dc;
      }
    }
    result[i] = s;
  }
  return result;
}

// Same-row-or-col positions (not including self, not orthogonally adjacent — but
// note: the web solver's getRowColPositions includes ALL same-row/col cells
// except self, so adjacents are in both orth AND rowcol sets; the consistency
// check handles priority via the ordered if/else chain)
function buildRowColSets() {
  const result = new Array(25);
  for (let i = 0; i < 25; i++) {
    const r = _r(i), c = _c(i);
    const s = new Set();
    for (let rr = 0; rr < GRID; rr++) if (rr !== r) s.add(rr*5+c);
    for (let cc = 0; cc < GRID; cc++) if (cc !== c) s.add(r*5+cc);
    result[i] = s;
  }
  return result;
}

// Union = self ∪ same-row ∪ same-col ∪ diagonals
function buildUnionSets(diagSets, rowColSets) {
  const result = new Array(25);
  for (let i = 0; i < 25; i++) {
    const s = new Set([i]);
    for (const x of rowColSets[i]) s.add(x);
    for (const x of diagSets[i])   s.add(x);
    result[i] = s;
  }
  return result;
}

const ORTH_NEIGHBORS = buildOrthNeighbors();
const DIAG_SETS      = buildDiagSets();
const ROW_COL_SETS   = buildRowColSets();
const UNION_SETS     = buildUnionSets(DIAG_SETS, ROW_COL_SETS);

// ---------------------------------------------------------------------------
// Core logic (mirroring sphere.js)
// ---------------------------------------------------------------------------

/**
 * Return the color name that cell `target` would show if red is at `redIdx`.
 */
function getSphereColorAt(target, redIdx) {
  if (target === redIdx)                           return "red";
  if (ORTH_NEIGHBORS[redIdx].includes(target))     return "orange";
  if (DIAG_SETS[redIdx].has(target))               return "yellow";
  if (ROW_COL_SETS[redIdx].has(target))            return "green";
  if (UNION_SETS[redIdx].has(target))              return "teal";
  return "blue";
}

/**
 * Is observation (obsIdx, obsColor) consistent with red being at redIdx?
 * Mirrors isSphereObservationConsistent from sphere.js exactly.
 */
function isConsistent(obsIdx, obsColor, redIdx) {
  if (obsIdx === redIdx) return obsColor === "red";

  const isOrth   = ORTH_NEIGHBORS[redIdx].includes(obsIdx);
  const isDiag   = DIAG_SETS[redIdx].has(obsIdx);
  const isRowCol = ROW_COL_SETS[redIdx].has(obsIdx);
  const isUnion  = UNION_SETS[redIdx].has(obsIdx);

  if (obsColor === "red")    return false;
  if (obsColor === "orange") return isOrth;
  if (obsColor === "yellow") return isDiag;
  if (obsColor === "green")  return isRowCol;
  if (obsColor === "teal")   return isUnion;
  if (obsColor === "blue")   return !isUnion;
  return false;
}

/**
 * Filter the possible-red set given a new observation.
 * Returns a new array of remaining candidate indices.
 */
function filterRedCandidates(possibleRed, obsIdx, obsColor) {
  const out = [];
  for (const redIdx of possibleRed) {
    if (obsColor !== "red" && redIdx === obsIdx) continue; // obsIdx can't be red
    if (isConsistent(obsIdx, obsColor, redIdx))  out.push(redIdx);
  }
  return out;
}

/**
 * Compute stats for clicking cell `cellIdx` given the current candidate set.
 * Returns { ev, entropy, avgRemaining, elimPower }
 * Mirrors getSphereCellStats from sphere.js.
 */
function getSphereCellStats(cellIdx, possibleRed) {
  const n = possibleRed.length;
  const counts = {};
  for (const redIdx of possibleRed) {
    const color = getSphereColorAt(cellIdx, redIdx);
    counts[color] = (counts[color] || 0) + 1;
  }

  let ev = 0, avgRemaining = 0, entropy = 0;
  for (const [color, count] of Object.entries(counts)) {
    const prob    = count / n;
    ev           += prob * (SPHERE_SCORES[color] || 0);
    avgRemaining += prob * count;
    entropy      -= prob * Math.log2(prob);
  }
  return { ev, entropy, avgRemaining, elimPower: n - avgRemaining };
}

/**
 * Rank all unrevealed cells by a weighted EV+entropy score.
 * Adaptive weights mirror getSphereBestMoves from sphere.js.
 */
function getSphereBestMoves(clickedSet, possibleRed) {
  const clicksMade = clickedSet.size;
  const n = possibleRed.length;

  let infoW = 0.65, pointsW = 0.35;
  if      (clicksMade < 2 && n > 12) { infoW = 0.8;  pointsW = 0.2; }
  else if (n <= 5)                   { infoW = 0.4;  pointsW = 0.6; }

  const moves = [];
  for (const cellIdx of ALL_CELLS) {
    if (clickedSet.has(cellIdx)) continue;
    const { ev, entropy, avgRemaining, elimPower } = getSphereCellStats(cellIdx, possibleRed);

    const combined = (
      (ev / SPHERE_SCORES.red) * pointsW +
      (entropy / LOG2_6)       * infoW
    ) * 1000;

    moves.push({ cellIdx, ev, entropy, avgRemaining, elimPower, combined });
  }

  moves.sort((a, b) => b.combined - a.combined);
  return moves;
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------

class KelimoAdaptiveEVOCStrategy extends OCStrategy {
  /**
   * No cross-game precomputation needed — geometry is precomputed at module
   * load time.  Return null so initGamePayload gets null as evaluationRunState.
   */
  initEvaluationRun() {
    return null;
  }

  /**
   * Reset per-game state: start with all 24 non-center cells as candidates.
   */
  initGamePayload(meta, evaluationRunState) {
    // All cells except center (2,2) are valid red positions initially
    const possibleRed = ALL_CELLS.filter(i => i !== CENTER);
    return { possibleRed };
  }

  /**
   * Choose the next cell using the Bayesian adaptive-EV approach.
   *
   * gameState.possibleRed is the current array of candidate red cell indices.
   * We update it from `revealed` (which is authoritative and grows each call),
   * then pick the highest-ranked unrevealed cell.
   */
  nextClick(board, meta, gameState) {
    // Rebuild possibleRed from scratch each call using clicked cells.
    let possibleRed = ALL_CELLS.filter(i => i !== CENTER);
    for (const cell of board.filter(c => c.clicked)) {
      const obsIdx   = cell.row * 5 + cell.col;
      const obsColor = SP_TO_COLOR[cell.color] || cell.color;
      possibleRed = filterRedCandidates(possibleRed, obsIdx, obsColor);
    }

    const clickedSet = new Set(board.filter(c => c.clicked).map(c => c.row * 5 + c.col));

    // If red is already found (clicked), click highest-value unrevealed cell greedily
    const redCell = board.find(c => c.color === "spR" && c.clicked);
    if (redCell) {
      const redIdx = redCell.row * 5 + redCell.col;
      // Sort unrevealed cells by their known color value (all colors are now
      // determined since red position is known)
      const unrevealedWithValues = [];
      for (const cellIdx of ALL_CELLS) {
        if (clickedSet.has(cellIdx)) continue;
        const color = getSphereColorAt(cellIdx, redIdx);
        unrevealedWithValues.push({ cellIdx, value: SPHERE_SCORES[color] || 0 });
      }
      unrevealedWithValues.sort((a, b) => b.value - a.value);
      if (unrevealedWithValues.length > 0) {
        const best = unrevealedWithValues[0];
        const row = _r(best.cellIdx), col = _c(best.cellIdx);
        return { row, col, gameState: { possibleRed } };
      }
    }

    // If no candidates remain (shouldn't happen on a valid board) fall back
    if (possibleRed.length === 0) {
      for (const cellIdx of ALL_CELLS) {
        if (!clickedSet.has(cellIdx)) {
          return { row: _r(cellIdx), col: _c(cellIdx), gameState: { possibleRed } };
        }
      }
      return { row: 0, col: 0, gameState: { possibleRed } };
    }

    // If exactly one candidate remains, click it directly (it's red)
    if (possibleRed.length === 1) {
      const redIdx = possibleRed[0];
      if (!clickedSet.has(redIdx)) {
        return { row: _r(redIdx), col: _c(redIdx), gameState: { possibleRed } };
      }
    }

    const moves = getSphereBestMoves(clickedSet, possibleRed);
    if (moves.length === 0) {
      return { row: 0, col: 0, gameState: { possibleRed } };
    }

    const best = moves[0];
    return {
      row: _r(best.cellIdx),
      col: _c(best.cellIdx),
      gameState: { possibleRed },
    };
  }
}

register(new KelimoAdaptiveEVOCStrategy());
