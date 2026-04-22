/**
 * oh_template.js — Template strategy for /sphere harvest (oh) — JavaScript.
 *
 * Copy this file to strategies/oh/<your_name>.js, rename the class, and
 * fill in the TODO sections.  Delete any methods you don't need — only
 * nextClick() is required.
 *
 * GAME OVERVIEW
 * -------------
 * Grid     : 5×5
 * Revealed : 10 cells visible at game start; 15 start covered (spU)
 * Budget   : 5 clicks
 * Goal     : maximise total SP collected across your 5 clicks
 *
 * Special click rules:
 *   Purple (spP)  : click is FREE (does not consume a click)
 *   Dark (spD)    : transforms into another color on click;
 *                   if it becomes purple the click is also free
 *   Blue (spB)    : reveals 3 additional covered cells
 *   Teal (spT)    : reveals 1 additional covered cell
 *   ~50% of boards have one "chest" covered cell worth ~345 SP on average
 *
 * COLOR REFERENCE (oh)
 * --------------------
 *   spB   blue    reveals 3 covered cells
 *   spT   teal    reveals 1 covered cell
 *   spG   green   35 SP
 *   spY   yellow  55 SP
 *   spL   light   ~76 SP average
 *   spO   orange  90 SP
 *   spR   red     150 SP
 *   spW   white   ~300 SP
 *   spP   purple  ~5–12 SP, click is FREE
 *   spD   dark    ~104 SP average, transforms on click
 *   spU   covered (unrevealed; directly clickable)
 *
 * REVEALED CELL FORMAT
 * --------------------
 * `revealed` is an array of objects, one per cell revealed so far
 * (monotonically growing — every call includes all cells since game start):
 *   [{ row: number, col: number, color: string }, ...]
 * Row and col are 0-indexed (0..4).
 *
 * STATE PAYLOAD
 * -------------
 * The game_state value is threaded through every call within a game:
 *
 *   initEvaluationRun()             → initialState   (called once before all games)
 *   initGamePayload(meta, s0)         → s1             (called once per game)
 *   nextClick(revealed,meta,s1) → {row,col,gameState:s2}
 *   nextClick(revealed,meta,s2) → {row,col,gameState:s3}
 *   ...
 *
 * Use initEvaluationRun() for data computed ONCE and shared across all games
 * (lookup tables, precomputed weights, etc.).
 *
 * Use initGamePayload() to reset per-game bookkeeping at the start of each game.
 * The gameState it returns is passed to the first nextClick() call.
 *
 * If your strategy is stateless, omit both optional methods and return
 * `gameState` unchanged in nextClick().
 *
 * meta keys (oh):
 *   clicks_left  number  remaining budget (starts at 5)
 *   max_clicks   number  total budget (always 5)
 *   game_seed    number  per-game deterministic seed
 *
 * See also
 * --------
 * oh/random_clicks.js   — minimal stateless example
 * oc/global_state.js    — initEvaluationRun() (global state) example
 * oq/stateful.js        — initGamePayload() + state threading (per-game state) example
 */

"use strict";

const { OHStrategy, register } = require("../../interface/strategy.js");

// If your strategy needs a large precomputed file (lookup table, policy matrix,
// etc.), require interface/data.js and call fetchData() in initEvaluationRun().
// Small files (≤ ~80 MB compressed) can be committed directly to data/ and
// loaded by path instead.
//
// Uncomment and adapt the fetch example below if you need a large file:
//
//   const { fetch: fetchData } = require("../../interface/data.js");
//   const path = require("path");
//
//   // External data: <filename>
//   // Size: ~X MB compressed / ~Y GB uncompressed
//   // Hosted at: <url>
//   const LUT_URL    = "https://huggingface.co/datasets/org/repo/resolve/main/<filename>";
//   const LUT_SHA256 = "<hex sha256>";
//   const LUT_FILE   = "<filename>";

class MyOHStrategy extends OHStrategy {
  // -------------------------------------------------------------------------
  // Optional: global state (computed once, shared across ALL games)
  // -------------------------------------------------------------------------

  /**
   * Called ONCE before all games begin.
   *
   * Return anything that is board-independent and expensive to repeat.
   * The returned value is passed as `evaluationRunState` to every subsequent initGamePayload()
   * and nextClick() call.  Treat it as a read-only global table.
   *
   * @returns {*}  Any JSON-serialisable value.  Default: null.
   */
  // If using a large external file, make this method async:
  //   async initEvaluationRun() { ... }
  initEvaluationRun() {
    // TODO: replace with your global precomputation, or delete this method.
    //
    // Example (large external file via auto-download):
    //   async initEvaluationRun() {
    //     const filePath = await fetchData({ url: LUT_URL, sha256: LUT_SHA256, filename: LUT_FILE });
    //     return { lut: loadLut(filePath) };  // your own loader
    //   }
    //
    // Example (small committed file in data/):
    //   const DATA_DIR = path.join(__dirname, "..", "..", "data");
    //   const filePath = path.join(DATA_DIR, "oh_harvest_lut.bin.lzma");
    //   return { lut: loadLut(filePath) };
    return null;
  }

  // -------------------------------------------------------------------------
  // Optional: per-game initialisation
  // -------------------------------------------------------------------------

  /**
   * Called once before each game's first click.
   *
   * @param {Object} meta   { clicks_left, max_clicks, game_seed }
   * @param {*}      evaluationRunState  Read-only value from initEvaluationRun()
   * @returns {*}   Initial gameState for this game's first nextClick() call.
   *
   * Example: return { ...evaluationRunState, clicksMade: 0, seenColors: [] }
   */
  initGamePayload(meta, evaluationRunState) {
    // TODO: reset per-game fields here, or delete this method
    return evaluationRunState;
  }

  // -------------------------------------------------------------------------
  // Required: click decision
  // -------------------------------------------------------------------------

  /**
   * Choose the next cell to click.
   *
   * @param {Array<{row: number, col: number, color: string}>} revealed
   *   All cells revealed so far this game (grows monotonically).
   * @param {Object} meta
   *   { clicks_left: number, max_clicks: number, game_seed: number }
   * @param {*} gameState
   *   Value returned by the previous nextClick() (or initGamePayload() for the first
   *   call of the game).
   * @returns {{ row: number, col: number, gameState: * }}
   *   row, col    : 0-indexed coordinates of the cell to click.
   *   gameState   : updated gameState for the next call.  Return `gameState`
   *                 unchanged if nothing needs updating.
   *
   * Tips:
   *   - Purple cells ("spP") are free — click them immediately if visible.
   *   - Blue ("spB") and teal ("spT") reveal more cells, giving more info
   *     before spending remaining clicks on value cells.
   *   - Dark ("spD") transforms on click; average value ~104 SP.
   *   - Do not return a (row, col) already present in revealed.
   */
  nextClick(revealed, meta, gameState) {
    const clicked = new Set(revealed.map(c => c.row * 5 + c.col));

    // Prefer any visible purple (free click)
    const purples = revealed.filter(c => c.color === "spP");
    if (purples.length > 0) {
      const pick = purples[0];
      return { row: pick.row, col: pick.col, gameState };
    }

    // TODO: replace the random fallback with your click logic
    const unclicked = [];
    for (let r = 0; r < 5; r++)
      for (let c = 0; c < 5; c++)
        if (!clicked.has(r * 5 + c)) unclicked.push([r, c]);

    if (unclicked.length === 0) return { row: 0, col: 0, gameState };
    const [row, col] = unclicked[Math.floor(Math.random() * unclicked.length)];
    return { row, col, gameState };
  }
}

register(new MyOHStrategy());
