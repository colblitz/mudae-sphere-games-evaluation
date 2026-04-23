/**
 * svessinn_solver.js — Faithful port of Svessinn's Trace solver for /sphere trace (ot).
 *
 * Source: https://github.com/Svessinn/Svessinn.github.io/blob/main/Mudae/Spheres/Trace/solver.html
 *
 * ALGORITHM OVERVIEW
 * ------------------
 * The board contains horizontal/vertical ship segments (fixed lengths per color)
 * plus blue (spB) empty cells.  Blue clicks cost 1 of the 4-click budget; ship
 * cell reveals are free.
 *
 * The solver runs a 4-pass deduction engine each turn, then applies phase logic:
 *
 * Pass 1 — Geometric certainty (intersection):
 *   For each ship color with remaining un-found segments, enumerate all valid
 *   placements consistent with already-revealed cells.  Any cell that appears
 *   in EVERY valid placement for that ship is certain to be that ship color.
 *   Iterate this to fixed-point (new certainties may unlock further certainties).
 *
 * Pass 2 — Ship weights:
 *   For each ship color, distribute weight 1/|validPlacements| equally across
 *   each cell of each valid placement (excluding already-revealed cells and
 *   cells whose color is already certain from Pass 1).
 *
 * Pass 3 — Rare ship identity weighting:
 *   For ships whose color is not yet identified (rare/optional ships whose
 *   presence is unknown), distribute a "?" weight across the valid size-2
 *   placements of unoccupied cells.  Also distribute individual rare-ship
 *   weights equally across candidate cells.
 *
 * Pass 4 — Blue normalisation:
 *   Compute the expected number of blue cells remaining and spread that mass
 *   across unresolved cells.  Normalise all heatmap entries to sum to 1.
 *
 * Phase logic:
 *   isHuntingBlue = ships_hit < 5
 *     → click the cell with the HIGHEST blue probability (safest non-ship click
 *       that avoids wasting budget on ships we haven't found yet).
 *   else
 *     → click the cell with the LOWEST blue probability (most likely to be a
 *       ship — extend the game by hitting ships before the budget runs out).
 *
 * Ship configuration by n_colors:
 *   Always present:  teal(4), green(3), yellow(3), orange(2), light(2)
 *   + 7-color:  dark(2)
 *   + 8-color:  red(2)
 *   + 9-color:  white(2)
 *   ("always" flags and lengths mirror ALL_COLORS in solver.html)
 *
 * State threading:
 *   initEvaluationRun()          → null  (no cross-game precomputation needed)
 *   initGamePayload(meta, _)     → { revealed: {} }   (flat index → color letter)
 *   nextClick(revealed, meta, s) → { row, col, gameState: s' }
 */

"use strict";

const { OTStrategy, register } = require("../../interface/strategy.js");

// ---------------------------------------------------------------------------
// Ship definitions (mirrors ALL_COLORS in solver.html)
// ---------------------------------------------------------------------------

const ALL_COLORS = {
  B: { name: "BLUE",    len: 0,  always: true  },   // blue = empty, handled separately
  T: { name: "TEAL",    len: 4,  always: true  },
  G: { name: "GREEN",   len: 3,  always: true  },
  Y: { name: "YELLOW",  len: 3,  always: true  },
  O: { name: "ORANGE",  len: 2,  always: true  },
  L: { name: "LIGHT",   len: 2,  always: true  },   // present from 6-color onward
  D: { name: "DARK",    len: 2,  always: false },
  R: { name: "RED",     len: 2,  always: false },
  W: { name: "RAINBOW", len: 2,  always: false },
};

// Color string mapping: harness "spX" → single letter used internally
const SP_TO_LETTER = {
  spB: "B", spT: "T", spG: "G", spY: "Y", spO: "O",
  spL: "L", spD: "D", spR: "R", spW: "W",
};

// ---------------------------------------------------------------------------
// Ship placement enumeration (mirrors getPossiblePlacements in solver.html)
// ---------------------------------------------------------------------------

/**
 * All valid horizontal and vertical placements for a ship of given length.
 * Returns an array of arrays, each inner array being the flat cell indices
 * covered by that placement.
 *
 * Mirrors getPossiblePlacements(len) in solver.html.
 */
function getPossiblePlacements(len) {
  const placements = [];
  // Horizontal
  for (let r = 0; r < 5; r++) {
    for (let c = 0; c <= 5 - len; c++) {
      const ship = [];
      for (let i = 0; i < len; i++) ship.push(r * 5 + (c + i));
      placements.push(ship);
    }
  }
  // Vertical
  for (let c = 0; c < 5; c++) {
    for (let r = 0; r <= 5 - len; r++) {
      const ship = [];
      for (let i = 0; i < len; i++) ship.push((r + i) * 5 + c);
      placements.push(ship);
    }
  }
  return placements;
}

// ---------------------------------------------------------------------------
// Active ship list for a given n_colors value
// ---------------------------------------------------------------------------

/**
 * Determine which ship colors are active given the current n_colors and
 * which rare colors have already been identified in the revealed map.
 *
 * Mirrors getActiveShips() in solver.html.
 */
function getActiveShips(nColors, revealedMap) {
  const common = Object.keys(ALL_COLORS).filter(k => ALL_COLORS[k].always);
  const userPlacedRares = [...new Set(Object.values(revealedMap))].filter(k => {
    return ALL_COLORS[k] && !ALL_COLORS[k].always;
  });
  const totalRaresAllowed = Math.max(0, nColors - 5);

  if (userPlacedRares.length < totalRaresAllowed) {
    const allRares = Object.keys(ALL_COLORS).filter(k => !ALL_COLORS[k].always);
    return [...common, ...allRares];
  }
  return [...common, ...userPlacedRares];
}

// ---------------------------------------------------------------------------
// 4-pass deduction engine (direct port of getDeductions() from solver.html)
// ---------------------------------------------------------------------------

/**
 * Run the 4-pass deduction engine and return:
 *   certain: { [idx]: colorLetter }  — cells deduced with 100% certainty
 *   heatmap: Array(25) of { [colorLetter]: weight }  — probability distributions
 *
 * @param {Object} revealedMap  { [flatIdx]: colorLetter }  (all revealed cells)
 * @param {number} nColors      6, 7, 8, or 9
 */
function getDeductions(revealedMap, nColors) {
  // virtualRevealed is extended during Pass 1 with certainties
  const virtualRevealed = { ...revealedMap };
  const heatmap = Array.from({ length: 25 }, () => ({}));
  const certain = {};
  let foundNewCertainty = true;

  const active = getActiveShips(nColors, revealedMap);
  const allPossibleRares = Object.keys(ALL_COLORS).filter(k => !ALL_COLORS[k].always);
  const totalRaresExpected = nColors - 5;
  const identifiedRares = [...new Set(Object.values(revealedMap))].filter(
    k => ALL_COLORS[k] && !ALL_COLORS[k].always
  );
  const missingRaresCount = totalRaresExpected - identifiedRares.length;

  // ---- PASS 1: INTERSECTION (GEOMETRIC CERTAINTY) ----
  while (foundNewCertainty) {
    foundNewCertainty = false;

    const currentCounts = {};
    for (const v of Object.values(virtualRevealed)) {
      currentCounts[v] = (currentCounts[v] || 0) + 1;
    }

    for (const color of active) {
      const ship = ALL_COLORS[color];
      if (!ship || ship.len === 0) continue; // skip blue

      const foundIdx = Object.keys(virtualRevealed)
        .filter(k => virtualRevealed[k] === color)
        .map(Number);

      // Ship fully found — skip
      if ((currentCounts[color] || 0) >= ship.len) continue;

      // Find valid placements: must include all already-found indices, must not
      // overlap with cells already assigned to a different color
      const validPos = getPossiblePlacements(ship.len).filter(pos => {
        if (!foundIdx.every(i => pos.includes(i))) return false;
        if (pos.some(i => virtualRevealed[i] !== undefined && virtualRevealed[i] !== color)) return false;
        return true;
      });

      if (validPos.length > 0) {
        // Intersection: cells present in ALL valid placements
        const intersection = validPos[0].filter(idx => validPos.every(p => p.includes(idx)));
        for (const idx of intersection) {
          if (virtualRevealed[idx] === undefined) {
            virtualRevealed[idx] = color;
            certain[idx] = color;
            heatmap[idx] = { [color]: 1 };
            foundNewCertainty = true;
          }
        }
      }
    }
  }

  // ---- PASS 2: SHIP WEIGHTS ----
  for (const color of active) {
    const ship = ALL_COLORS[color];
    if (!ship || ship.len === 0) continue;

    const currentCount = Object.values(virtualRevealed).filter(v => v === color).length;
    if (currentCount >= ship.len) continue;

    const foundIdx = Object.keys(virtualRevealed)
      .filter(k => virtualRevealed[k] === color)
      .map(Number);

    const validPos = getPossiblePlacements(ship.len).filter(pos => {
      if (!foundIdx.every(i => pos.includes(i))) return false;
      if (pos.some(i => virtualRevealed[i] !== undefined && virtualRevealed[i] !== color)) return false;
      return true;
    });

    if (validPos.length > 0) {
      const weight = 1 / validPos.length;
      for (const pos of validPos) {
        for (const idx of pos) {
          if (!revealedMap[idx] && !certain[idx]) {
            heatmap[idx][color] = (heatmap[idx][color] || 0) + weight;
          }
        }
      }
    }
  }

  // ---- PASS 3: IDENTITY-AWARE RARE LOGIC ----
  if (missingRaresCount > 0) {
    const unidentifiedRares = allPossibleRares.filter(r => !identifiedRares.includes(r));
    const validPos = getPossiblePlacements(2).filter(pos =>
      pos.every(idx => virtualRevealed[idx] === undefined)
    );

    if (validPos.length > 0) {
      const categoryWeight = missingRaresCount / validPos.length;
      for (const pos of validPos) {
        for (const idx of pos) {
          if (!revealedMap[idx] && !certain[idx]) {
            heatmap[idx]["?"] = (heatmap[idx]["?"] || 0) + categoryWeight;
            const individualWeight = categoryWeight / unidentifiedRares.length;
            for (const r of unidentifiedRares) {
              heatmap[idx][r] = (heatmap[idx][r] || 0) + individualWeight;
            }
          }
        }
      }
    }
  }

  // ---- PASS 4: BLUE NORMALISATION ----
  const totalShipSegments = 12 + totalRaresExpected * 2;  // always 12 + rares
  const totalBluesExpected = 25 - totalShipSegments;
  const bluesFound = Object.values(revealedMap).filter(v => v === "B").length;

  const unresolved = Array.from({ length: 25 }, (_, i) => i).filter(
    i => !revealedMap[i] && !certain[i]
  );
  const blueWeight = unresolved.length > 0
    ? Math.max(0, totalBluesExpected - bluesFound) / unresolved.length
    : 0;

  for (let i = 0; i < 25; i++) {
    if (revealedMap[i] || certain[i]) continue;
    heatmap[i]["B"] = blueWeight;
    const total = Object.values(heatmap[i]).reduce((a, b) => a + b, 0);
    if (total > 0) {
      for (const k of Object.keys(heatmap[i])) heatmap[i][k] /= total;
    } else {
      heatmap[i] = { B: 1 };
    }
  }

  return { certain, heatmap };
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------

class SvessinOTStrategy extends OTStrategy {
  /**
   * No cross-game precomputation needed for this solver.
   */
  initEvaluationRun() {
    return null;
  }

  /**
   * Reset per-game revealed map.
   */
  initGamePayload(meta, evaluationRunState) {
    return { revealedMap: {} };
  }

  /**
   * Choose the next cell using Svessinn's deduction engine + phase logic.
   *
   * @param {Array<{row, col, color}>} revealed  All cells revealed so far
   * @param {{ n_colors, ships_hit, blues_used, max_clicks }} meta
   * @param {{ revealedMap: Object }} gameState
   * @returns {{ row: number, col: number, gameState: object }}
   */
  nextClick(board, meta, gameState) {
    const { n_colors, ships_hit } = meta;

    // --- Rebuild revealedMap from clicked cells on the board ---
    const revealedMap = {};
    for (const cell of board.filter(c => c.clicked)) {
      const idx = cell.row * 5 + cell.col;
      const letter = SP_TO_LETTER[cell.color] || cell.color;
      revealedMap[idx] = letter;
    }

    // --- Run the deduction engine ---
    const { certain, heatmap } = getDeductions(revealedMap, n_colors);

    // --- Phase logic (mirrors render() in solver.html) ---
    // Phase: hunting blues if ships_hit < 5, else hunting ships
    const isHuntingBlue = ships_hit < 5;

    const available = Array.from({ length: 25 }, (_, i) => i).filter(
      i => !revealedMap[i]
    );

    // --- Check for certain non-blue cells: click them immediately ---
    // These are cells deduced to be ship segments with 100% confidence.
    // In solver.html this is presented as "SAFE" moves during the ship-hunting phase.
    // We prioritise certain cells (any non-blue certain cell is a free ship hit).
    for (const idx of available) {
      if (certain[idx] && certain[idx] !== "B") {
        const row = Math.floor(idx / 5);
        const col = idx % 5;
        return { row, col, gameState: { revealedMap } };
      }
    }

    // --- Also click any cell whose blue probability is effectively 0 ---
    // (heatmap[idx]["B"] is 0 or near-0, meaning it's almost certainly a ship)
    // This mirrors the solver.html logic of auto-clicking B >= 0.999 cells and
    // clicking certain cells directly.
    if (!isHuntingBlue) {
      for (const idx of available) {
        if (!certain[idx] && (heatmap[idx]["B"] || 0) < 0.001) {
          const row = Math.floor(idx / 5);
          const col = idx % 5;
          return { row, col, gameState: { revealedMap } };
        }
      }
    }

    // --- Pick best move based on phase ---
    // Hunting blues: highest blue probability → most likely blue → safe to click
    // Hunting ships: lowest blue probability → most likely ship → extend game
    let targetIdx = -1;

    if (available.length > 0) {
      if (isHuntingBlue) {
        // Find the cell with the highest blue probability
        let maxBlue = -1;
        for (const idx of available) {
          const blueProb = heatmap[idx]["B"] || 0;
          if (blueProb > maxBlue) { maxBlue = blueProb; targetIdx = idx; }
        }
      } else {
        // Find the cell with the lowest blue probability (most likely ship)
        let minBlue = Infinity;
        for (const idx of available) {
          const blueProb = heatmap[idx]["B"] || 0;
          if (blueProb < minBlue) { minBlue = blueProb; targetIdx = idx; }
        }
      }
    }

    // Fallback: click any available cell
    if (targetIdx === -1 && available.length > 0) {
      targetIdx = available[0];
    }

    // Ultimate fallback (should never be reached on a valid board)
    if (targetIdx === -1) return { row: 0, col: 0, gameState: { revealedMap } };

    const row = Math.floor(targetIdx / 5);
    const col = targetIdx % 5;
    return { row, col, gameState: { revealedMap } };
  }
}

register(new SvessinOTStrategy());
