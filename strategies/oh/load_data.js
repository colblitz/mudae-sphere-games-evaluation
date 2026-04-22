/**
 * load_data.js — Color-priority strategy for /sphere harvest (oh) — JavaScript.
 *
 * Demonstrates loading a data file via interface/data.js fetch().
 *
 * The strategy loads a JSON file containing color priority weights from data/.
 * On every click it picks the highest-value already-revealed cell that has not
 * yet been clicked, falling back to a random covered cell when no such cell
 * exists.  Purple cells are always clicked first (they are free).
 *
 * The data file is committed directly to data/ (it is tiny), so fetch() finds
 * it locally and skips any network request.  For a real large file (e.g. a
 * 14 GB lookup table) the same fetch() call would download it from the hosted
 * URL on first use and cache it in data/ for all subsequent runs.
 *
 * External data
 * -------------
 * File    : oh_example.json
 * Size    : < 1 KB
 * Source  : https://raw.githubusercontent.com/colblitz/mudae-sphere-games-evaluation/main/data/oh_example.json
 * SHA-256 : fdebc10f5140a1b4ae6643b4626660d86a34847ea8f6d30e617b2c63e7d008c8
 */

"use strict";

const fs = require("fs");

const { OHStrategy, register } = require("../../interface/strategy.js");
const { fetch: fetchData }     = require("../../interface/data.js");

// ---------------------------------------------------------------------------
// Data file configuration
// ---------------------------------------------------------------------------

const DATA_URL    = "https://raw.githubusercontent.com/colblitz/mudae-sphere-games-evaluation/main/data/oh_example.json";
const DATA_SHA256 = "fdebc10f5140a1b4ae6643b4626660d86a34847ea8f6d30e617b2c63e7d008c8";
const DATA_FILE   = "oh_example.json";

class LoadDataOHStrategy extends OHStrategy {
  // -------------------------------------------------------------------------
  // Global state: load data file once, share across all games
  // -------------------------------------------------------------------------

  /**
   * Called once before all games.  Loads color priority weights from
   * data/oh_example.json via fetchData().
   *
   * fetchData() checks whether the file is already present in data/ and its
   * SHA-256 matches.  If so it returns immediately — no download occurs.
   * If the file is absent or corrupted it downloads from DATA_URL.
   *
   * Returns an object with the loaded colorValues table.
   */
  async initEvaluationRun() {
    const filePath = await fetchData({ url: DATA_URL, sha256: DATA_SHA256, filename: DATA_FILE });
    const data = JSON.parse(fs.readFileSync(filePath, "utf8"));
    return { colorValues: data["color_values"] };
  }

  // -------------------------------------------------------------------------
  // Per-game state: seed RNG from game_seed for reproducibility
  // -------------------------------------------------------------------------

  /**
   * Seed a per-game RNG and carry the run state through.
   * JS does not have a seedable built-in RNG, so we use a simple LCG.
   */
  initGamePayload(meta, evaluationRunState) {
    const seed = meta.game_seed ?? Date.now();
    return { ...evaluationRunState, rng: makeLcg(seed) };
  }

  // -------------------------------------------------------------------------
  // Click decision: highest-value visible cell, or random covered cell
  // -------------------------------------------------------------------------

  /**
   * @param {Array<{row: number, col: number, color: string}>} revealed
   * @param {Object} meta  { clicks_left, max_clicks, game_seed }
   * @param {*} gameState
   * @returns {{ row: number, col: number, gameState: * }}
   */
  nextClick(revealed, meta, gameState) {
    const { colorValues, rng } = gameState;
    const clicked = new Set(revealed.map(c => c.row * 5 + c.col));

    // Purples are free — click any visible purple immediately.
    const purples = revealed.filter(c => c.color === "spP");
    if (purples.length > 0) {
      const pick = purples[Math.floor(rng() * purples.length)];
      return { row: pick.row, col: pick.col, gameState };
    }

    // Among revealed-but-unclicked cells, pick the highest-value one.
    const INFO_ONLY = new Set(["spB", "spT"]);
    const candidates = revealed.filter(
      c => !clicked.has(c.row * 5 + c.col) && !INFO_ONLY.has(c.color)
    );
    if (candidates.length > 0) {
      const bestVal = Math.max(...candidates.map(c => colorValues[c.color] ?? 0));
      const best    = candidates.filter(c => (colorValues[c.color] ?? 0) === bestVal);
      const pick    = best[Math.floor(rng() * best.length)];
      return { row: pick.row, col: pick.col, gameState };
    }

    // Fall back to a random covered cell.
    const unclicked = [];
    for (let r = 0; r < 5; r++)
      for (let c = 0; c < 5; c++)
        if (!clicked.has(r * 5 + c)) unclicked.push([r, c]);
    if (unclicked.length === 0) return { row: 0, col: 0, gameState };
    const [row, col] = unclicked[Math.floor(rng() * unclicked.length)];
    return { row, col, gameState };
  }
}

// ---------------------------------------------------------------------------
// Minimal seedable LCG — reproducible per-game RNG (no npm required)
// ---------------------------------------------------------------------------

function makeLcg(seed) {
  // Knuth's multiplicative LCG (mod 2^32), returns floats in [0, 1).
  let s = seed >>> 0;
  return function () {
    s = Math.imul(1664525, s) + 1013904223 >>> 0;
    return s / 0x100000000;
  };
}

register(new LoadDataOHStrategy());
