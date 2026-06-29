/**
 * zavex_wr.js — WR-optimal book + adaptive heuristic strategy for /sphere quest (oq).
 *
 * Port of the solver logic from https://orb-quest-book.pages.dev/ in WR mode.
 *
 * Algorithm:
 *   PLAYING phase:
 *     1. Walk the WR-optimal opening book (WR_BOOKS) using D4 symmetry (8
 *        transforms). If the revealed cells match any (book, transform) pair,
 *        follow the trie recommendation.
 *     2. Off-book fallback — adaptive heuristic:
 *          - Filter all 12,650 possible 4-purple layouts to those consistent
 *            with revealed outcomes.
 *          - 100% certain purple → click it immediately (free).
 *          - Last non-purple click → max P(purple).
 *          - Adaptive threshold = 0.06 + 0.06 * clicks_remaining:
 *              if max P(purple) > threshold → click it,
 *              else → click max-entropy cell.
 *   BONUS_LOCATE phase (3 purples found, red not yet appeared):
 *     - Identify the 4th mine by constraint inference; click it (free).
 *   BONUS_HARVEST phase (red appeared, clicks remain):
 *     - Click highest-value unrevealed non-mine cell each turn.
 *       Value = adjacency to all 4 mines: 0adj=10, 1=20, 2=35, 3=55, 4=90 SP.
 *
 * Opening selection (empty board):
 *   Matches the live source WR-mode behavior: sort WR_BOOKS openers by meta.wr
 *   descending, pick index 1 (inner corner). The live source deliberately skips
 *   the #1 WR opener in favor of inner corner as a risk/reward tradeoff.
 *
 * External data:
 *   data/oq_books_20260628_221107.json  (~3.8 MB, committed with git add -f)
 *   Contains ALL_BOOKS and WR_BOOKS extracted from the page's embedded_data.js.
 *
 * State design:
 *   All precomputed data (world array, outcomes, constraint map, book trie)
 *   is stored on `this` in initEvaluationRun(). Both initEvaluationRun() and
 *   initGamePayload() return null so the harness never JSON-serialises the
 *   large data structures.
 */

"use strict";

const fs   = require("fs");
const path = require("path");
const { OQStrategy, register } = require("../../interface/strategy.js");
const {
  COLOR_TO_OUTCOME,
  buildWorldData,
  filterWorlds,
  heuristicAnalysis,
  walkBook,
  locateMineCandidates,
  harvestRanking,
} = require("./zavex_core.js");

const DATA_FILE = "oq_books_20260628_221107.json";

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------
class WROQStrategy extends OQStrategy {

  /**
   * Called once before all games.
   * Stores precomputed world data and WR book trie on `this`.
   */
  initEvaluationRun() {
    // World precomputation
    const { allWorlds, worldOutcomes, constraint } = buildWorldData();
    this._allWorlds     = allWorlds;
    this._worldOutcomes = worldOutcomes;
    this._constraint    = constraint;

    // Load WR_BOOKS from the timestamped data file
    const dataPath = path.join(__dirname, "..", "..", "data", DATA_FILE);
    this._books = null;
    try {
      const raw    = fs.readFileSync(dataPath, "utf8");
      const parsed = JSON.parse(raw);
      this._books = parsed.WR_BOOKS || null;
    } catch (e) {
      process.stderr.write(
        `[oq/zavex_wr] Warning: could not load ${DATA_FILE}: ${e.message}\n` +
        `[oq/zavex_wr] Falling back to heuristic-only mode.\n`
      );
    }

    // Precompute WR-mode opening: sort canons by meta.wr descending, pick index 1.
    // Matches the live source behavior (inner corner = canon 6 with current data).
    this._opener = 6;  // default fallback
    if (this._books) {
      const sorted = Object.values(this._books)
        .sort((a, b) => b.meta.wr - a.meta.wr);
      if (sorted.length > 1) {
        this._opener = sorted[1].root.m;
      } else if (sorted.length === 1) {
        this._opener = sorted[0].root.m;
      }
    }

    return null;
  }

  /**
   * Called once per game. Returns null — no per-game state needed.
   */
  initGamePayload(meta, evaluationRunState) {
    return null;
  }

  /**
   * Choose the next cell to click.
   *
   * @param {Array<{row,col,color,clicked}>} board  All 25 cells with current state.
   * @param {{clicks_left,max_clicks,purples_found}} meta
   * @param {*} gameState  null (unused)
   * @returns {{ row, col, gameState }}
   */
  nextClick(board, meta, gameState) {
    const revealedSet = new Set(board.filter(c => c.clicked).map(c => c.row * 5 + c.col));

    // Build cellStates[25] from clicked cells
    const cellStates = new Array(25).fill("?");
    for (const cell of board.filter(c => c.clicked)) {
      const idx     = cell.row * 5 + cell.col;
      const outcome = COLOR_TO_OUTCOME[cell.color];
      if (outcome !== undefined) cellStates[idx] = outcome;
    }

    // Helper: safe cell picker — never re-clicks a clicked cell
    const pick = (cellIdx) => {
      if (cellIdx >= 0 && !revealedSet.has(cellIdx))
        return { row: Math.floor(cellIdx / 5), col: cellIdx % 5, gameState: null };
      return null;
    };

    // ------------------------------------------------------------------
    // Auto-reveal: if spR is visible (not yet clicked), click it immediately
    // (harness sets color="spR", clicked=false after 3rd purple is clicked)
    // ------------------------------------------------------------------
    for (const cell of board) {
      if (cell.color === "spR" && !cell.clicked)
        return { row: cell.row, col: cell.col, gameState: null };
    }

    const purplesFound = meta.purples_found;
    // hasRed: spR has actually been clicked (not just revealed)
    const hasRed = board.some(c => c.color === "spR" && c.clicked);

    // ------------------------------------------------------------------
    // BONUS_HARVEST: red appeared and was clicked, spend remaining clicks on best orbs
    // ------------------------------------------------------------------
    if (purplesFound >= 3 && hasRed) {
      const mines = new Set();
      for (let i = 0; i < 25; i++) {
        if (cellStates[i] === "t" || cellStates[i] === "r") mines.add(i);
      }
      // Try to deduce 4th mine if not explicitly clicked
      if (mines.size === 3) {
        const cands = locateMineCandidates(
          cellStates, this._constraint, this._allWorlds
        );
        if (cands.size === 1) mines.add([...cands][0]);
      }
      const ranking = harvestRanking(cellStates, mines);
      for (const { cell } of ranking) {
        const r = pick(cell);
        if (r) return r;
      }
      return this._randomUnclicked(board);
    }

    // ------------------------------------------------------------------
    // PLAYING phase
    // ------------------------------------------------------------------

    // Empty board → WR-mode recommended opener (index 1 by meta.wr, per live source)
    if (revealedSet.size === 0) {
      const r = pick(this._opener);
      if (r) return r;
    }

    // Book walk — WR_BOOKS only (single book set, matching live source WR mode)
    if (this._books) {
      const { bestMove, onBook } = walkBook(cellStates, this._books);
      if (onBook && bestMove >= 0) {
        const r = pick(bestMove);
        if (r) return r;
      }
    }

    // Off-book: adaptive heuristic
    const validWorlds = filterWorlds(cellStates, this._constraint);
    const { bestMove } = heuristicAnalysis(
      validWorlds, cellStates, this._worldOutcomes
    );
    if (bestMove >= 0) {
      const r = pick(bestMove);
      if (r) return r;
    }

    return this._randomUnclicked(board);
  }

  /** Fallback: first unclicked cell in row-major order */
  _randomUnclicked(board) {
    const clicked = new Set(board.filter(c => c.clicked).map(c => c.row * 5 + c.col));
    for (let r = 0; r < 5; r++)
      for (let c = 0; c < 5; c++)
        if (!clicked.has(r * 5 + c))
          return { row: r, col: c, gameState: null };
    return { row: 0, col: 0, gameState: null };
  }
}

register(new WROQStrategy());
