/**
 * svessinn_solver.js — Faithful port of Svessinn's Chest solver for /sphere chest (oc).
 *
 * Source: https://github.com/Svessinn/Svessinn.github.io/blob/main/Mudae/Spheres/Chest/solver.html
 *
 * ALGORITHM OVERVIEW
 * ------------------
 * The solver maintains a set of possible red positions (possibleRedIndices) and
 * uses two phases:
 *
 * Phase 1 — Red location unknown (possibleRedIndices.length > 1):
 *   For each unclicked cell, compute a histogram of which color it would reveal
 *   across all remaining possible red positions.  Score = sum of count² (lower
 *   is better — more evenly distributed outcomes give more information).
 *   Pick the cell with the lowest score (maximum entropy reduction).
 *
 * Phase 2 — Red location known (possibleRedIndices.length === 1):
 *   Click the red cell first (the UI marks it "RED HERE" as the primary
 *   recommendation).  Then greedily collect high-value cells in priority order:
 *   orange (2 needed) → yellow (3 needed) → green (4 needed).  Click the first
 *   available cell for the highest-priority uncollected color type.
 *
 * Color geometry (all relative to the red cell at (rr, rc)):
 *   R  — the red cell itself
 *   O  — orthogonally adjacent: |dr|+|dc| == 1
 *   Y  — diagonal: dr == dc && dr > 0
 *   G  — same row or column (not orange): dr==0||dc==0, not the red cell
 *   T  — remaining axial/diagonal (filled by Teal in the game)
 *   B  — no geometric relation to red
 *
 * State threading:
 *   initEvaluationRun()          → null  (no global state needed)
 *   initGamePayload(meta, _)     → { possibleRedIndices, revealedMap }
 *   nextClick(revealed, meta, s) → { row, col, gameState: s' }
 */

"use strict";

const { OCStrategy, register } = require("../../interface/strategy.js");

// All valid red positions: indices 0..24 except center (index 12 = row 2, col 2)
const ALL_POSSIBLE_REDS = Array.from({ length: 25 }, (_, i) => i).filter(i => i !== 12);

// How many of each color the board contains (Svessinn's LIMITS table)
const LIMITS = { R: 1, O: 2, Y: 3, G: 4 };

// ---------------------------------------------------------------------------
// Geometry helpers (direct port from solver.html)
// ---------------------------------------------------------------------------

/**
 * Given that red is at redIdx, what color would targetIdx reveal?
 * Mirrors getExpectedColor() in solver.html.
 */
function getExpectedColor(targetIdx, redIdx) {
  if (targetIdx === redIdx) return "R";
  const rr = Math.floor(redIdx   / 5), rc = redIdx   % 5;
  const tr = Math.floor(targetIdx / 5), tc = targetIdx % 5;
  const dr = Math.abs(rr - tr), dc = Math.abs(rc - tc);

  // Orange: orthogonally adjacent (distance 1 on one axis, 0 on the other)
  if ((dr === 1 && dc === 0) || (dr === 0 && dc === 1)) return "O";
  // Yellow: strictly diagonal
  if (dr === dc && dr > 0) return "Y";
  // Green: same row or column (but not the orange adjacency above)
  if (dr === 0 || dc === 0) return "G";
  // Everything else is Blue (no geometric relation)
  return "B";
}

/**
 * Is it geometrically possible that targetIdx has color foundColor when red is at redIdx?
 * Mirrors checkPossibility() in solver.html.
 */
function checkPossibility(targetIdx, foundColor, redIdx) {
  const rr = Math.floor(redIdx   / 5), rc = redIdx   % 5;
  const tr = Math.floor(targetIdx / 5), tc = targetIdx % 5;
  const dr = Math.abs(rr - tr), dc = Math.abs(rc - tc);

  if (foundColor === "R") return targetIdx === redIdx;
  if (foundColor === "O") return (dr === 1 && dc === 0) || (dr === 0 && dc === 1);
  if (foundColor === "Y") return dr === dc && dr > 0;
  // Green: same row or column (dr==0 or dc==0), but not the red cell itself
  if (foundColor === "G") return (dr === 0 || dc === 0) && targetIdx !== redIdx;
  if (foundColor === "T") return dr === dc || dr === 0 || dc === 0;
  if (foundColor === "B") return !(dr === 0 || dc === 0 || dr === dc);
  return true;
}

// ---------------------------------------------------------------------------
// Color string mapping (harness uses "spX" strings; solver uses single letters)
// ---------------------------------------------------------------------------

const COLOR_TO_LETTER = {
  spR: "R", spO: "O", spY: "Y", spG: "G", spT: "T", spB: "B",
};

// ---------------------------------------------------------------------------
// Core solver functions (direct port from solver.html)
// ---------------------------------------------------------------------------

/**
 * Recompute possibleRedIndices given the current revealedMap.
 * Mirrors recalculatePossibilities() in solver.html.
 *
 * @param {Object} revealedMap  { [idx]: letter }
 * @returns {number[]}  Remaining candidate red indices
 */
function recalculatePossibilities(revealedMap) {
  const entries = Object.entries(revealedMap); // [[idx, letter], ...]

  // If we already know where red is, lock to it
  const knownRed = entries.find(([, color]) => color === "R");
  if (knownRed) {
    return [parseInt(knownRed[0])];
  }

  // Filter the full candidate set
  const occupied = entries.map(([k]) => parseInt(k));
  let filtered = ALL_POSSIBLE_REDS.filter(idx => !occupied.includes(idx));

  filtered = filtered.filter(redIdx => {
    for (const [idx, color] of entries) {
      if (!checkPossibility(parseInt(idx), color, redIdx)) return false;
    }
    return true;
  });

  return filtered;
}

/**
 * Choose the best cell to click next.
 * Mirrors calculateBestMove() in solver.html.
 *
 * @param {Object}   revealedMap         { [idx]: letter }
 * @param {number[]} possibleRedIndices  Current candidate red positions
 * @returns {{ idx: number, phase: string }|null}
 */
function calculateBestMove(revealedMap, possibleRedIndices) {
  const clickCount = Object.keys(revealedMap).length;
  if (clickCount >= 5) return null;

  // --- PHASE 1: Red not yet found — seek it via entropy minimisation ---
  if (possibleRedIndices.length > 1) {
    let bestScore = Infinity, bestIdx = -1;

    for (let i = 0; i < 25; i++) {
      if (revealedMap[i] !== undefined) continue; // already revealed

      // Count how many reds would produce each color for cell i
      const outcomes = { B: 0, T: 0, G: 0, Y: 0, O: 0, R: 0 };
      for (const rIdx of possibleRedIndices) {
        outcomes[getExpectedColor(i, rIdx)]++;
      }

      // Sum of squares (lower = more spread = more informative)
      let score = 0;
      for (const count of Object.values(outcomes)) {
        if (count > 0) score += count * count;
      }

      if (bestIdx === -1 || score < bestScore) {
        bestScore = score;
        bestIdx = i;
      }
    }

    return { idx: bestIdx, phase: "seek_red" };
  }

  // --- PHASE 2: Red is known — click red first, then collect O → Y → G ---
  if (possibleRedIndices.length === 1) {
    const redIdx = possibleRedIndices[0];

    // The UI marks the red cell "RED HERE" as the primary recommendation once
    // the location is known.  Click it first before collecting other colors.
    if (revealedMap[redIdx] === undefined) {
      return { idx: redIdx, phase: "collect_red" };
    }

    // Red already clicked — collect O → Y → G in priority order
    const counts = {};
    for (const color of Object.values(revealedMap)) {
      counts[color] = (counts[color] || 0) + 1;
    }

    const searchOrder = [
      { type: "O", limit: LIMITS.O },
      { type: "Y", limit: LIMITS.Y },
      { type: "G", limit: LIMITS.G },
    ];

    for (const goal of searchOrder) {
      if ((counts[goal.type] || 0) < goal.limit) {
        // Find unrevealed cells of this color relative to the known red
        for (let i = 0; i < 25; i++) {
          if (revealedMap[i] !== undefined) continue;
          if (i !== redIdx && checkPossibility(i, goal.type, redIdx)) {
            return { idx: i, phase: "collect", type: goal.type };
          }
        }
      }
    }
  }

  return null;
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------

class SvessinOCStrategy extends OCStrategy {
  /**
   * No global precomputation needed — the geometry is computed on the fly.
   */
  initEvaluationRun() {
    return null;
  }

  /**
   * Reset per-game state: fresh possibility set and empty revealed map.
   */
  initGamePayload(meta, evaluationRunState) {
    return {
      possibleRedIndices: [...ALL_POSSIBLE_REDS],
      revealedMap: {},          // { [flatIdx]: colorLetter }
    };
  }

  /**
   * Pick the next cell using Svessinn's two-phase solver logic.
   *
   * @param {Array<{row, col, color}>} revealed
   * @param {Object} meta  { clicks_left, max_clicks }
   * @param {{ possibleRedIndices: number[], revealedMap: Object }} gameState
   * @returns {{ row: number, col: number, gameState: object }}
   */
  nextClick(revealed, meta, gameState) {
    // --- Sync revealedMap with the full revealed list from the harness ---
    // (revealed grows monotonically; we rebuild from scratch each call to be safe)
    const revealedMap = {};
    for (const cell of revealed) {
      const idx = cell.row * 5 + cell.col;
      const letter = COLOR_TO_LETTER[cell.color] || cell.color;
      revealedMap[idx] = letter;
    }

    // Recompute the candidate red positions from current knowledge
    const possibleRedIndices = recalculatePossibilities(revealedMap);

    // Ask the solver for the best move
    const move = calculateBestMove(revealedMap, possibleRedIndices);

    let targetIdx;
    if (move !== null) {
      targetIdx = move.idx;
    } else {
      // Fallback: click any unclicked cell (should rarely/never be reached)
      const clicked = new Set(Object.keys(revealedMap).map(Number));
      for (let i = 0; i < 25; i++) {
        if (!clicked.has(i)) { targetIdx = i; break; }
      }
    }

    const row = Math.floor(targetIdx / 5);
    const col = targetIdx % 5;

    const newState = { possibleRedIndices, revealedMap };
    return { row, col, gameState: newState };
  }
}

register(new SvessinOCStrategy());
