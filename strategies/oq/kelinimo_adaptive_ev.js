/**
 * kelinimo_adaptive_ev.js — Adaptive-EV strategy for /sphere quest (oq).
 *
 * Algorithm (ported from solvespheres.kelinimo.workers.dev / purple.js):
 *
 *   1. All 12,650 possible purple configurations (C(25,4)) are generated once
 *      at module load time as 25-bit bitmasks (ALL_PURPLE_CONFIGS).  This
 *      mirrors how main.js populates ALL_PURPLE_CONFIGS on DOMContentLoaded.
 *
 *   2. Each nextClick call, filter ALL_PURPLE_CONFIGS to only those consistent
 *      with all revealed cells so far.  A config is consistent when:
 *        - Every revealed "spP"/"spR" cell is in the purple set.
 *        - Every revealed neighbor-count cell has the correct 8-neighbor
 *          purple count given the config.
 *
 *   3. For each unrevealed cell, compute EV, purple probability, and
 *      information entropy over the remaining configs.
 *
 *   4. Pick the cell maximising a weighted combination of those three signals,
 *      with five adaptive stages (default / lastclick / early / mid / late)
 *      whose weights are tuned constants copied verbatim from the source.
 *
 * Color mapping (harness sp-codes → internal color names):
 *   spP → purple   (5 SP, free click)
 *   spR → red      (150 SP, costs 1 click)
 *   spB → blue     (0 purple neighbours)
 *   spT → teal     (1 purple neighbour)
 *   spG → green    (2 purple neighbours)
 *   spY → yellow   (3 purple neighbours)
 *   spO → orange   (4+ purple neighbours)
 *
 * State: null — the strategy is stateless; ALL_PURPLE_CONFIGS is a module-level
 *   constant and configs are re-filtered from scratch each call using `revealed`.
 *   This avoids sending the large config array through the harness JSON pipe.
 */

"use strict";

const { OQStrategy, register } = require("../../interface/strategy.js");

// ---------------------------------------------------------------------------
// Constants (verbatim from purple.js / utils.js)
// ---------------------------------------------------------------------------

const GRID        = 5;
const NUM_PURPLES = 4;
const LOG2_6      = Math.log2(6);

const PURPLE_SCORES = {
  purple: 5, red: 150, orange: 90, yellow: 55, green: 35, teal: 20, blue: 10,
};

// Adjacency colors: index = number of purple 8-neighbors (capped at 4)
const ADJACENCY_COLORS = { 0: "blue", 1: "teal", 2: "green", 3: "yellow", 4: "orange" };

// harness sp-code → internal color name
const SP_TO_COLOR = {
  spP: "purple", spR: "red",
  spB: "blue", spT: "teal", spG: "green", spY: "yellow", spO: "orange",
};

// Adaptive weights (verbatim from purple.js ADAPTIVE_WEIGHTS)
const ADAPTIVE_WEIGHTS = {
  info_default:    0.5474764665078234,
  elim_default:    0.07181511395388832,
  purple_default:  0.3807084195382882,

  info_lastclick:    0.3742029842110276,
  elim_lastclick:    0.1723606896679593,
  purple_lastclick:  0.45343632612101314,

  info_early:    0.1603304880219415,
  elim_early:    0.3337655646958492,
  purple_early:  0.5059039472822092,

  info_mid:    0.23663079212601287,
  elim_mid:    0.10330795500844421,
  purple_mid:  0.6600612528655428,

  info_late:    0.38317530997055566,
  elim_late:    0.26838785970274326,
  purple_late:  0.34843683032670103,
};

// Adaptive thresholds (verbatim from purple.js ADAPTIVE_THRESHOLDS)
const ADAPTIVE_THRESHOLDS = {
  lastclick_threshold: 5,
  purples_early:       3,
  configs_mid:         276,
  configs_late:        1189,
};

// ---------------------------------------------------------------------------
// Precomputed topology (mirrors utils.js NEIGHBORS)
// ---------------------------------------------------------------------------

// 8-way neighbours for each cell index (row*5+col)
const NEIGHBORS = (() => {
  const result = new Array(25);
  for (let i = 0; i < 25; i++) {
    const r = (i / 5) | 0, c = i % 5;
    const nb = [];
    for (let dr = -1; dr <= 1; dr++) {
      for (let dc = -1; dc <= 1; dc++) {
        if (!dr && !dc) continue;
        const nr = r + dr, nc = c + dc;
        if (nr >= 0 && nr < GRID && nc >= 0 && nc < GRID) nb.push(nr * 5 + nc);
      }
    }
    result[i] = nb;
  }
  return result;
})();

// ---------------------------------------------------------------------------
// Module-level config generation (mirrors main.js DOMContentLoaded block)
// ---------------------------------------------------------------------------

/**
 * Generate all C(25,4) = 12,650 purple configurations as 25-bit bitmasks.
 * Bit i set iff cell i is purple.
 * Called once at module load time, exactly as main.js does on page load.
 */
const ALL_PURPLE_CONFIGS = (() => {
  const configs = [];
  for (let a = 0; a < 25; a++)
    for (let b = a + 1; b < 25; b++)
      for (let c = b + 1; c < 25; c++)
        for (let d = c + 1; d < 25; d++)
          configs.push((1 << a) | (1 << b) | (1 << c) | (1 << d));
  return configs;
})();

// ---------------------------------------------------------------------------
// Core logic (mirroring purple.js)
// ---------------------------------------------------------------------------

/**
 * Return the color name cell `cellIdx` would show if `configMask` is the
 * purple layout.  Mirrors getPurpleColorForPosition from purple.js.
 */
function getPurpleColorForCell(cellIdx, configMask) {
  if (configMask & (1 << cellIdx)) return "purple";
  let count = 0;
  for (const nb of NEIGHBORS[cellIdx]) {
    if (configMask & (1 << nb)) count++;
  }
  return ADJACENCY_COLORS[Math.min(count, 4)];
}

/**
 * Is `configMask` consistent with all known observations?
 * `known` is an array of { idx, color } objects.
 * Mirrors isPurpleConfigConsistent from purple.js.
 */
function isConfigConsistent(configMask, known) {
  for (const { idx, color } of known) {
    if (color === "purple" || color === "red") {
      if (!(configMask & (1 << idx))) return false;
    } else {
      if (getPurpleColorForCell(idx, configMask) !== color) return false;
    }
  }
  return true;
}

/**
 * Filter ALL_PURPLE_CONFIGS to only those consistent with `known`.
 * Mirrors generatePurpleConfigs from purple.js.
 */
function generatePurpleConfigs(known) {
  if (known.length === 0) return ALL_PURPLE_CONFIGS;
  return ALL_PURPLE_CONFIGS.filter(cfg => isConfigConsistent(cfg, known));
}

/**
 * Compute per-cell stats for a single cell over the current config set.
 * Returns { groups, ev, purpleProb, avgRemaining, elimPower, entropy }
 * Mirrors getPurpleCellData from purple.js.
 */
function getPurpleCellData(cellIdx, configs, purplesFound) {
  const groups = {};
  for (const cfg of configs) {
    const color = getPurpleColorForCell(cellIdx, cfg);
    if (!groups[color]) groups[color] = [];
    groups[color].push(cfg);
  }

  const n = configs.length;
  let ev = 0, purpleProb = 0, avgRemaining = 0, entropy = 0;

  for (const [color, cfgs] of Object.entries(groups)) {
    const prob  = cfgs.length / n;
    const score = color === "purple"
      ? (purplesFound >= 2 ? PURPLE_SCORES.red : PURPLE_SCORES.purple) + 15
      : (PURPLE_SCORES[color] || 0);

    ev           += prob * score;
    if (color === "purple") purpleProb = prob;
    avgRemaining += prob * cfgs.length;
    entropy      -= prob * Math.log2(prob);
  }

  return { groups, ev, purpleProb, avgRemaining, elimPower: n - avgRemaining, entropy };
}

/**
 * Rank all unrevealed cells using the 5-stage adaptive weights.
 * Mirrors getPurpleBestMovesAdaptive from purple.js.
 */
function getPurpleBestMovesAdaptive(clickedSet, configs, purplesFound, clicksMade) {
  const w = ADAPTIVE_WEIGHTS;
  const t = ADAPTIVE_THRESHOLDS;

  let stage;
  if      (clicksMade >= t.lastclick_threshold) stage = "lastclick";
  else if (purplesFound >= t.purples_early)     stage = "early";
  else if (configs.length <= t.configs_mid)     stage = "mid";
  else if (configs.length <= t.configs_late)    stage = "late";
  else                                          stage = "default";

  const infoW   = w[`info_${stage}`];
  const elimW   = w[`elim_${stage}`];
  const purpleW = w[`purple_${stage}`];

  const moves = [];
  for (let cellIdx = 0; cellIdx < 25; cellIdx++) {
    if (clickedSet.has(cellIdx)) continue;
    const { ev, purpleProb, avgRemaining, elimPower, entropy } =
      getPurpleCellData(cellIdx, configs, purplesFound);

    const combined = (
      (ev / (PURPLE_SCORES.red + 20)) * elimW +
      (entropy / LOG2_6)              * infoW +
      purpleProb                      * purpleW
    ) * 1000;

    moves.push({ cellIdx, ev, purpleProb, avgRemaining, elimPower, entropy, combined });
  }

  moves.sort((a, b) => b.combined - a.combined);
  return moves;
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------

class KelimoAdaptiveEVOQStrategy extends OQStrategy {
  // initEvaluationRun and initGamePayload intentionally omitted:
  // ALL_PURPLE_CONFIGS is a module-level constant, so no per-run or per-game
  // state is needed.  gameState is null throughout.

  /**
   * Choose the next cell using the Bayesian adaptive-EV approach.
   *
   * Filters ALL_PURPLE_CONFIGS from scratch against the full `revealed` list
   * each call — authoritative and avoids large state serialization through
   * the harness JSON pipe.
   */
  nextClick(board, meta, gameState) {
    const clickedSet = new Set(board.filter(c => c.clicked).map(c => c.row * 5 + c.col));

    // Build known observations from clicked cells
    const known = [];
    for (const cell of board.filter(c => c.clicked)) {
      const color = SP_TO_COLOR[cell.color] || cell.color;
      known.push({ idx: cell.row * 5 + cell.col, color });
    }

    // Count purples found and non-purple clicks made
    let purplesFound = 0, clicksMade = 0;
    for (const { color } of known) {
      if (color === "purple" || color === "red") purplesFound++;
      else                                       clicksMade++;
    }

    // Filter configs from the full module-level set each call
    const configs = generatePurpleConfigs(known);

    if (configs.length === 0) {
      for (let i = 0; i < 25; i++) {
        if (!clickedSet.has(i))
          return { row: (i / 5) | 0, col: i % 5, gameState };
      }
      return { row: 0, col: 0, gameState };
    }

    const moves = getPurpleBestMovesAdaptive(clickedSet, configs, purplesFound, clicksMade);

    if (moves.length === 0) {
      return { row: 0, col: 0, gameState };
    }

    const best = moves[0];
    return {
      row: (best.cellIdx / 5) | 0,
      col: best.cellIdx % 5,
      gameState,
    };
  }
}

register(new KelimoAdaptiveEVOQStrategy());
