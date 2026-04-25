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
const { fetchSync }            = require("../../interface/data.js");

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
   * data/oh_example.json via fetchSync().
   *
   * fetchSync() checks whether the file is already present in data/ and its
   * SHA-256 matches.  If so it returns immediately — no download occurs.
   * If the file is absent or corrupted it downloads from DATA_URL.
   *
   * Must be synchronous: the strategy.js protocol loop does not await
   * initEvaluationRun(), so an async version would return a Promise that
   * gets JSON-serialized as {} — the loaded data would be lost silently.
   *
   * Returns an object with the loaded colorValues table.
   */
  initEvaluationRun() {
    const filePath = fetchSync({ url: DATA_URL, sha256: DATA_SHA256, filename: DATA_FILE });
    const data = JSON.parse(fs.readFileSync(filePath, "utf8"));
    // Store on the instance rather than returning from initEvaluationRun so
    // that the large color-values table is never copied into the game-state
    // slot on every initGamePayload() call.
    this._colorValues = data["color_values"];
    return null;
  }

  // -------------------------------------------------------------------------
  // Per-game state: seed RNG from game_seed for reproducibility
  // -------------------------------------------------------------------------

  /**
   * Seed a per-game RNG on this instance and carry the run state through.
   *
   * The RNG is stored as an instance variable rather than in the state dict
   * because functions are not JSON-serializable — the harness serializes
   * gameState to JSON between calls, so any function in the state is silently
   * dropped.  The run state (colorValues) is JSON-safe and passes through
   * unchanged.
   */
  initGamePayload(meta, evaluationRunState) {  // eslint-disable-line no-unused-vars
    const seed = meta.game_seed ?? Date.now();
    this._rng = makeLcg(seed);
    return null;
  }

  // -------------------------------------------------------------------------
  // Click decision: highest-value visible cell, or random covered cell
  // -------------------------------------------------------------------------

  /**
   * @param {Array<{row: number, col: number, color: string, clicked: boolean}>} board
   * @param {Object} meta  { clicks_left, max_clicks, game_seed }
   * @param {*} gameState
   * @returns {{ row: number, col: number, gameState: * }}
   */
  nextClick(board, meta, gameState) {  // eslint-disable-line no-unused-vars
    const colorValues = this._colorValues;
    const rng = this._rng;
    const clicked = new Set(board.filter(c => c.clicked).map(c => c.row * 5 + c.col));

    // Purples are free — click any visible purple immediately.
    const purples = board.filter(c => c.color === "spP" && !c.clicked);
    if (purples.length > 0) {
      const pick = purples[Math.floor(rng() * purples.length)];
      return { row: pick.row, col: pick.col, gameState };
    }

    // Among revealed-but-unclicked cells, pick the highest-value one.
    const INFO_ONLY = new Set(["spU", "spB", "spT"]);
    const candidates = board.filter(
      c => !c.clicked && !INFO_ONLY.has(c.color)
    );
    if (candidates.length > 0) {
      const bestVal = Math.max(...candidates.map(c => colorValues[c.color] ?? 0));
      const best    = candidates.filter(c => (colorValues[c.color] ?? 0) === bestVal);
      const pick    = best[Math.floor(rng() * best.length)];
      return { row: pick.row, col: pick.col, gameState };
    }

    // Fall back to a random unclicked cell.
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
