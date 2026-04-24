/**
 * kelinimo_expectimax.js — 1-ply expectimax + greedy DP strategy for /sphere trace (ot).
 *
 * Algorithm (ported from solvespheres.kelinimo.workers.dev / ot.js):
 *
 *   1. Precompute all valid run-placement bitmasks for teal(4), green(3),
 *      yellow(3), and rare(2) ships, plus all non-overlapping rare-combo sets
 *      for each n_rare value (2–5), in initEvaluationRun().
 *
 *   2. Each call, rebuild `known` and `rareKnown` from `revealed`, then
 *      run the constraint engine (otComputeProbs) to compute per-cell
 *      marginal probabilities of being teal / green / yellow / rare / blue.
 *
 *   3. Score each unrevealed cell with a 1-ply expectimax lookahead followed
 *      by a greedy DP rollout (otScoreMove + otRolloutValue).  The score is
 *      expected total remaining points — no ad-hoc weights.
 *
 *   4. After grace ends, if any cell is guaranteed non-blue, promote the
 *      highest-EV certain cell to the top (mirrors the web solver).
 *
 * Color mapping (harness sp-codes → internal names):
 *   spT → teal    (run length 4)
 *   spG → green   (run length 3)
 *   spY → yellow  (run length 3)
 *   spO → orange  (run length 2, rare)
 *   spL → light   (run length 2, rare)
 *   spD → dark    (run length 2, rare)
 *   spR → red     (run length 2, rare)
 *   spW → rainbow (run length 2, rare)  ← web solver uses "rainbow" for spW
 *   spB → blue    (empty, costs a click)
 *
 * State: { nColors: number, known: object, rareKnown: object }
 *   Rebuilt from `revealed` each call; stored for reference only.
 */

"use strict";

const { OTStrategy, register } = require("../../interface/strategy.js");

// ---------------------------------------------------------------------------
// Constants (from ot.js)
// ---------------------------------------------------------------------------

const OT_RARE_TYPES_ARR = ["orange", "red", "light", "dark", "rainbow"];
const OT_RARE_TYPES_SET = new Set(OT_RARE_TYPES_ARR);

const OT_VALUES = {
  blue: 10, teal: 20, green: 35, yellow: 55,
  orange: 90, red: 150, rainbow: 500,
  light: 80,
  dark: 104,
};

const OT_RARE_EV_UNK =
  (OT_VALUES.orange + OT_VALUES.red + OT_VALUES.light + OT_VALUES.dark + OT_VALUES.rainbow) / 5;

// harness sp-code → internal color name
const SP_TO_COLOR = {
  spT: "teal", spG: "green", spY: "yellow", spB: "blue",
  spO: "orange", spL: "light", spD: "dark", spR: "red", spW: "rainbow",
};

// ---------------------------------------------------------------------------
// Bitmask helpers (5×5 grid, bit = row*5+col)
// ---------------------------------------------------------------------------

function otBit(r, c)  { return 1 << (r * 5 + c); }
function otCtz(v)     { return Math.clz32(v & -v) ^ 31; }

/**
 * Generate all horizontal and vertical run placements of length `len`
 * as 25-bit bitmasks.
 */
function otRunMasks(len) {
  const masks = [];
  for (let axis = 0; axis < 2; axis++) {         // 0=horizontal, 1=vertical
    for (let line = 0; line < 5; line++) {
      for (let start = 0; start <= 5 - len; start++) {
        let mask = 0;
        for (let i = 0; i < len; i++) {
          const r = axis === 0 ? line      : start + i;
          const c = axis === 0 ? start + i : line;
          mask |= otBit(r, c);
        }
        masks.push(mask);
      }
    }
  }
  return masks;
}

// ---------------------------------------------------------------------------
// Precompute rare combo combinations (runs once per nRare in initEvaluationRun)
// ---------------------------------------------------------------------------

/**
 * Enumerate all ways to place `N` non-overlapping rare runs (each of length 2).
 * Returns array of { combined: number, masks: number[] }.
 * Mirrors precomputeOtRareCombos from ot.js.
 */
function computeRareCombos(rareMasks, N) {
  const combos = [];
  (function recurse(start, current, combined) {
    if (current.length === N) {
      combos.push({ combined, masks: current.slice() });
      return;
    }
    for (let i = start; i < rareMasks.length; i++) {
      const m = rareMasks[i];
      if (!(combined & m)) {
        current.push(m);
        recurse(i + 1, current, combined | m);
        current.pop();
      }
    }
  })(0, [], 0);
  return combos;
}

// ---------------------------------------------------------------------------
// Rare combo compatibility check (mirrors isOtRareComboCompatible)
// ---------------------------------------------------------------------------

function isOtRareComboCompatible(combo, rareKnown) {
  const colorToRun = {};
  for (const [color, bits] of Object.entries(rareKnown)) {
    if (!bits) continue;
    let matched = null;
    for (const m of combo.masks) {
      if ((m & bits) === bits) { matched = m; break; }
    }
    if (matched === null) return false;
    colorToRun[color] = matched;
  }
  const used = Object.values(colorToRun);
  return new Set(used).size === used.length;
}

// ---------------------------------------------------------------------------
// Core probability engine (mirrors otComputeProbs)
// ---------------------------------------------------------------------------

/**
 * Compute per-cell marginal probabilities given current observations.
 *
 * `known`     : { "r,c_str_or_idx": colorName } — but we pass it as a plain
 *               object keyed by cellIdx (number).
 * `nRare`     : number of rare runs (= n_colors - 4)
 * `rareKnown` : { colorName: bitmask_of_confirmed_cells }
 * `runMasks`  : { teal, green, yellow, rare } precomputed arrays
 * `rareCombos`: precomputed combos for this nRare
 *
 * Returns { probs: Array<{teal,green,yellow,rare,blue}>, totalConfigs } or null.
 */
function otComputeProbs(knownByIdx, nRare, rareKnown, runMasks, rareCombos) {
  let knownTeal = 0, knownGreen = 0, knownYellow = 0, knownBlue = 0, knownRareAny = 0;

  for (const [idxStr, color] of Object.entries(knownByIdx)) {
    const bit = 1 << Number(idxStr);
    if      (color === "blue")   knownBlue    |= bit;
    else if (color === "teal")   knownTeal    |= bit;
    else if (color === "green")  knownGreen   |= bit;
    else if (color === "yellow") knownYellow  |= bit;
    else                         knownRareAny |= bit;
  }

  const compatT = runMasks.teal.filter(m =>
    (m & knownTeal) === knownTeal &&
    !(m & (knownBlue | knownGreen | knownYellow | knownRareAny)));
  const compatG = runMasks.green.filter(m =>
    (m & knownGreen) === knownGreen &&
    !(m & (knownBlue | knownTeal | knownYellow | knownRareAny)));
  const compatY = runMasks.yellow.filter(m =>
    (m & knownYellow) === knownYellow &&
    !(m & (knownBlue | knownTeal | knownGreen | knownRareAny)));
  const compatRareCombos = rareCombos.filter(combo =>
    (combo.combined & knownRareAny) === knownRareAny &&
    !(combo.combined & knownBlue)   &&
    !(combo.combined & knownTeal)   &&
    !(combo.combined & knownGreen)  &&
    !(combo.combined & knownYellow) &&
    isOtRareComboCompatible(combo, rareKnown)
  );

  if (!compatT.length || !compatG.length || !compatY.length || !compatRareCombos.length)
    return null;

  // Enumerate valid TGY triples (pre-filter for non-overlap)
  const tgyT = [], tgyG = [], tgyY = [], tgyMask = [];
  for (const t of compatT) {
    for (const g of compatG) {
      if (t & g) continue;
      const tg = t | g;
      for (const y of compatY) {
        if (tg & y) continue;
        tgyT.push(t); tgyG.push(g); tgyY.push(y); tgyMask.push(tg | y);
      }
    }
  }

  if (tgyT.length === 0) return null;

  const rcLen      = compatRareCombos.length;
  const rcCombined = compatRareCombos.map(c => c.combined);

  // Accumulate per-cell counts
  const cellTeal   = new Float64Array(25);
  const cellGreen  = new Float64Array(25);
  const cellYellow = new Float64Array(25);
  const cellRare   = new Float64Array(25);
  let totalConfigs = 0;

  for (let i = 0; i < tgyT.length; i++) {
    const tgy = tgyMask[i];
    const ti  = tgyT[i];
    const gi  = tgyG[i];
    const yi  = tgyY[i];
    for (let j = 0; j < rcLen; j++) {
      const rcm = rcCombined[j];
      if (tgy & rcm) continue;
      totalConfigs++;
      for (let b = ti;  b; b &= b - 1) cellTeal  [otCtz(b)]++;
      for (let b = gi;  b; b &= b - 1) cellGreen [otCtz(b)]++;
      for (let b = yi;  b; b &= b - 1) cellYellow[otCtz(b)]++;
      for (let b = rcm; b; b &= b - 1) cellRare  [otCtz(b)]++;
    }
  }

  if (totalConfigs === 0) return null;

  const probs = [];
  for (let i = 0; i < 25; i++) {
    const t = cellTeal[i]   / totalConfigs;
    const g = cellGreen[i]  / totalConfigs;
    const y = cellYellow[i] / totalConfigs;
    const r = cellRare[i]   / totalConfigs;
    probs.push({ teal: t, green: g, yellow: y, rare: r, blue: 1 - t - g - y - r });
  }
  return { probs, totalConfigs };
}

// ---------------------------------------------------------------------------
// Conditioned rare EV (mirrors otComputeRareEv)
// ---------------------------------------------------------------------------

function otComputeRareEv(rareKnown, nRare) {
  const knownTypes   = Object.keys(rareKnown).filter(k => rareKnown[k]);
  const unknownSlots = nRare - knownTypes.length;

  if (unknownSlots <= 0)
    return knownTypes.reduce((s, t) => s + OT_VALUES[t], 0) / Math.max(knownTypes.length, 1);
  if (knownTypes.length === 0)
    return OT_RARE_EV_UNK;

  const unknownTypes = OT_RARE_TYPES_ARR.filter(t => !knownTypes.includes(t));
  const unknownAvg   = unknownTypes.reduce((s, t) => s + OT_VALUES[t], 0) / unknownTypes.length;
  const knownAvg     = knownTypes.reduce((s, t)   => s + OT_VALUES[t], 0) / knownTypes.length;
  return (knownAvg * knownTypes.length + unknownAvg * unknownSlots) / nRare;
}

// ---------------------------------------------------------------------------
// Greedy DP rollout (mirrors otRolloutValue)
// ---------------------------------------------------------------------------

const _DP_K = 4;  // paid blues dimension: 0..3
const _DP_G = 6;  // colored dimension: 0..5 (5 = sentinel "grace over")

/**
 * Expected total remaining points from the greedy rollout policy.
 *
 * `sortedCells` : cell indices sorted by P(blue) ascending (safest first)
 * `probs25`     : per-cell probability objects from otComputeProbs
 * `pb0`         : paid blues used so far (before this rollout)
 * `C0`          : colored cells found so far (before this rollout)
 * `rareEv`      : expected points for a rare cell
 */
function otRolloutValue(sortedCells, probs25, pb0, C0, rareEv) {
  const budget = 4 - pb0;
  if (budget <= 0) return 0;

  const graceNeed = Math.max(0, 5 - C0);
  const G = graceNeed;

  // dpA[k * _DP_G + c] = probability of being in state (k paid blues, c extra colored)
  const dpA = new Float64Array(_DP_K * _DP_G);
  const dpB = new Float64Array(_DP_K * _DP_G);
  dpA[0] = 1.0;

  let ev    = 0;
  let alive = false;

  for (const idx of sortedCells) {
    const pr = probs25[idx];
    const pB = pr.blue;
    const pC = 1.0 - pB;

    const cVal = pC > 1e-14
      ? (pr.teal * OT_VALUES.teal + pr.green * OT_VALUES.green +
         pr.yellow * OT_VALUES.yellow + pr.rare * rareEv) / pC
      : 0;

    dpB.fill(0);
    alive = false;

    for (let k = 0; k < budget; k++) {
      for (let c = 0; c <= G; c++) {
        const p = dpA[k * _DP_G + c];
        if (p < 1e-14) continue;

        const graceOn = (pb0 + k >= 3) && (c < G);

        // Colored outcome
        if (pC > 1e-14) {
          ev += p * pC * cVal;
          const nc = (c < G) ? c + 1 : G;
          dpB[k * _DP_G + nc] += p * pC;
          alive = true;
        }

        // Blue outcome
        if (pB > 1e-14) {
          ev += p * pB * OT_VALUES.blue;
          if (graceOn) {
            dpB[k * _DP_G + c] += p * pB;
            alive = true;
          } else {
            const nk = k + 1;
            if (nk < budget) {
              dpB[nk * _DP_G + c] += p * pB;
              alive = true;
            }
          }
        }
      }
    }

    if (!alive) break;
    for (let i = 0; i < _DP_K * _DP_G; i++) dpA[i] = dpB[i];
  }

  return ev;
}

// ---------------------------------------------------------------------------
// 1-ply expectimax move scorer (mirrors otScoreMove)
// ---------------------------------------------------------------------------

/**
 * Expected total remaining points if we click `cellIdx` next, then play greedy.
 *
 * PERF DEVIATION from solvespheres.kelinimo.workers.dev ot.js:
 * The original builds and sorts `remaining` inside this function on each call,
 * doing O(N log N) work per candidate cell (~20 times per nextClick).  Here,
 * the caller (nextClick) pre-sorts all unrevealed cells once and passes
 * `sortedUnrevealed` in; we filter out `cellIdx` in O(N).  Results are
 * identical because removing one element from a sorted array preserves order.
 */
function otScoreMove(cellIdx, sortedUnrevealed, probs25, knownByIdx, pb0, C0, rareEv) {
  const pr = probs25[cellIdx];
  const pB = pr.blue;
  const pC = 1.0 - pB;
  const blueIsFree = (pb0 >= 3) && (C0 < 5);

  // Remaining cells: pre-sorted by P(blue) ascending, with cellIdx excluded.
  const remaining = sortedUnrevealed.filter(i => i !== cellIdx);

  let ev = 0;

  if (pC > 1e-10) {
    const cVal = (pr.teal * OT_VALUES.teal + pr.green * OT_VALUES.green +
                  pr.yellow * OT_VALUES.yellow + pr.rare * rareEv) / pC;
    const fut  = otRolloutValue(remaining, probs25, pb0, C0 + 1, rareEv);
    ev += pC * (cVal + fut);
  }

  if (pB > 1e-10) {
    if (blueIsFree) {
      const fut = otRolloutValue(remaining, probs25, pb0, C0, rareEv);
      ev += pB * (OT_VALUES.blue + fut);
    } else {
      const nb = pb0 + 1;
      if (nb >= 4) {
        ev += pB * OT_VALUES.blue;
      } else {
        const fut = otRolloutValue(remaining, probs25, nb, C0, rareEv);
        ev += pB * (OT_VALUES.blue + fut);
      }
    }
  }

  return ev;
}

// ---------------------------------------------------------------------------
// Helper: derive effective paid blues used (mirrors getEffectiveClicksUsed)
// ---------------------------------------------------------------------------

function getEffectiveClicksUsed(graceActive, blueClicks) {
  if (graceActive) return Math.min(blueClicks, 3);
  return Math.min(blueClicks, 4);
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------

class KelimoExpectimaxOTStrategy extends OTStrategy {
  /**
   * Precompute run masks and rare combos for all four n_colors variants (6–9).
   * n_rare = n_colors - 4  (so nRare in [2,3,4,5]).
   * Stored on `this` — never serialised or sent back to the harness.
   */
  initEvaluationRun() {
    const tealMasks   = otRunMasks(4);
    const greenMasks  = otRunMasks(3);
    const yellowMasks = otRunMasks(3);
    const rareMasks   = otRunMasks(2);

    const rareCombos = {};
    for (const N of [2, 3, 4, 5]) {
      rareCombos[N] = computeRareCombos(rareMasks, N);
    }

    this._runMasks   = { teal: tealMasks, green: greenMasks, yellow: yellowMasks, rare: rareMasks };
    this._rareCombos = rareCombos;
  }

  // initGamePayload not needed — no per-game state to reset.

  /**
   * Choose the next cell using 1-ply expectimax + greedy DP rollout.
   */
  nextClick(board, meta) {
    const runMasks   = this._runMasks;
    const rareCombos = this._rareCombos;

    const nColors = meta.n_colors;
    const nRare   = nColors - 4;  // number of rare run slots

    // Build knownByIdx (cellIdx → color) and rareKnown (colorName → bitmask)
    // from clicked cells — mirrors how the web solver's cycleOtColor
    // populates otState.known and otState.rareKnown.
    const knownByIdx = {};
    const rareKnown  = {};

    for (const cell of board.filter(c => c.clicked)) {
      const idx   = cell.row * 5 + cell.col;
      const color = SP_TO_COLOR[cell.color] || cell.color;
      knownByIdx[idx] = color;
      if (OT_RARE_TYPES_SET.has(color)) {
        rareKnown[color] = (rareKnown[color] || 0) | (1 << idx);
      }
    }

    const clickedSet = new Set(board.filter(c => c.clicked).map(c => c.row * 5 + c.col));

    // Compute probabilities
    const result = otComputeProbs(knownByIdx, nRare, rareKnown, runMasks, rareCombos[nRare]);

    // If the constraint engine finds no valid configs (shouldn't happen on a
    // valid board), fall back to clicking the first unrevealed cell.
    if (!result) {
      for (let i = 0; i < 25; i++) {
        if (!clickedSet.has(i))
          return { row: (i / 5) | 0, col: i % 5 };
      }
      return { row: 0, col: 0 };
    }

    const { probs } = result;

    // Derive game-phase values from meta (mirrors analyzeOt)
    const blueClicks   = meta.blues_used;
    const coloredFound = meta.ships_hit;
    const graceActive  = coloredFound < 5;
    const effectiveClicksUsed = getEffectiveClicksUsed(graceActive, blueClicks);

    const rareEv = otComputeRareEv(rareKnown, nRare);

    // Score all unrevealed cells
    const unrevealed = [];
    for (let i = 0; i < 25; i++) {
      if (clickedSet.has(i)) continue;
      unrevealed.push(i);
    }

    // Pre-sort once by P(blue) ascending — passed into otScoreMove so it
    // doesn't re-sort inside each of the ~20 per-cell calls.
    const sortedUnrevealed = [...unrevealed].sort((a, b) => probs[a].blue - probs[b].blue);

    const moves = unrevealed.map(cellIdx => {
      const score = otScoreMove(
        cellIdx, sortedUnrevealed, probs, knownByIdx,
        effectiveClicksUsed, coloredFound, rareEv
      );
      return { cellIdx, ev: score, probs: probs[cellIdx] };
    });

    moves.sort((a, b) => b.ev - a.ev);

    // After grace ends: if any cell is guaranteed non-blue, promote the
    // highest-EV certain cell to the front (mirrors the web solver's
    // post-sort promotion logic in analyzeOt).
    if (!graceActive) {
      const certainMoves = moves.filter(m => m.probs.blue <= 0.0001);
      if (certainMoves.length > 0) {
        certainMoves.sort((a, b) => b.ev - a.ev);
        const topCertain = certainMoves[0];
        const targetIdx  = moves.findIndex(m => m.cellIdx === topCertain.cellIdx);
        moves.splice(targetIdx, 1);
        moves.unshift(topCertain);
      }
    }

    if (moves.length === 0) {
      return { row: 0, col: 0 };
    }

    const best = moves[0];
    return {
      row: (best.cellIdx / 5) | 0,
      col: best.cellIdx % 5,
    };
  }
}

register(new KelimoExpectimaxOTStrategy());
