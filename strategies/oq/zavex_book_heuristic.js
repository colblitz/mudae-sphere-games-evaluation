/**
 * book_heuristic.js — Book + adaptive heuristic strategy for /sphere quest (oq).
 *
 * Port of the solver logic from https://orb-quest-book.pages.dev/
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
 * External data:
 *   data/oq_books.json  (~4.3 MB, committed with git add -f)
 *   Contains ALL_BOOKS and WR_BOOKS extracted from the page's embedded_data.js.
 *
 * State design:
 *   All precomputed data (world array, outcomes, constraint map, book tries)
 *   is stored on `this` in initEvaluationRun(). Both initEvaluationRun() and
 *   initGamePayload() return null so the harness never JSON-serialises the
 *   large data structures.
 *
 * Color mapping (harness → solver outcome codes):
 *   spP → 't'   purple mine (free click)
 *   spR → 'r'   red / 4th mine (costs 1 click, 150 SP)
 *   spB → '0'   0 purple neighbours
 *   spT → '1'   1 purple neighbour
 *   spG → '2'   2 purple neighbours
 *   spY → '3'   3 purple neighbours
 *   spO → '4'   4 purple neighbours
 */

"use strict";

const fs   = require("fs");
const path = require("path");
const { OQStrategy, register } = require("../../interface/strategy.js");

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const COLOR_TO_OUTCOME = {
  spP: "t",
  spR: "r",
  spB: "0",
  spT: "1",
  spG: "2",
  spY: "3",
  spO: "4",
};

// Harvest phase orb values by adjacency count (matches ORB_VALUES on the page)
const HARVEST_VALUE = [10, 20, 35, 55, 90];  // indices 0..4

// ---------------------------------------------------------------------------
// NEIGHBORS — 8-connected neighbour list per cell (matches page)
// ---------------------------------------------------------------------------
const NEIGHBORS = [];
for (let r = 0; r < 5; r++) {
  for (let c = 0; c < 5; c++) {
    const ns = [];
    for (let dr = -1; dr <= 1; dr++) {
      for (let dc = -1; dc <= 1; dc++) {
        if (!dr && !dc) continue;
        const nr = r + dr, nc = c + dc;
        if (nr >= 0 && nr < 5 && nc >= 0 && nc < 5) ns.push(nr * 5 + nc);
      }
    }
    NEIGHBORS.push(ns);
  }
}

// ---------------------------------------------------------------------------
// TRANSFORMS — D4 symmetry group on 5×5 grid (matches makeTransforms() on page)
// ---------------------------------------------------------------------------
function makeTransforms() {
  const transforms = [];
  for (let rot = 0; rot < 4; rot++) {
    for (let flip = 0; flip < 2; flip++) {
      const t = new Array(25);
      for (let r = 0; r < 5; r++) {
        for (let c = 0; c < 5; c++) {
          let nr = r, nc = c;
          for (let i = 0; i < rot; i++) { const tmp = nr; nr = nc; nc = 4 - tmp; }
          if (flip) nc = 4 - nc;
          t[r * 5 + c] = nr * 5 + nc;
        }
      }
      transforms.push(t);
    }
  }
  return transforms;
}
const TRANSFORMS = makeTransforms();

// ---------------------------------------------------------------------------
// World precomputation — all C(25,4)=12,650 four-mine layouts
// (matches ALL_WORLDS + WORLD_OUTCOMES + CONSTRAINT on the page)
// ---------------------------------------------------------------------------
function buildWorldData() {
  const allWorlds     = [];   // [wi] = [a,b,c,d]  mine cell indices
  const worldOutcomes = [];   // [wi] = Int8Array(25), -1=mine else adj count
  const constraint    = new Map(); // (cell*10 + outcome+1) → [worldIdx, ...]

  for (let a = 0; a < 25; a++)
    for (let b = a + 1; b < 25; b++)
      for (let c = b + 1; c < 25; c++)
        for (let d = c + 1; d < 25; d++) {
          const wi = allWorlds.length;
          allWorlds.push([a, b, c, d]);
          const oc    = new Int8Array(25);
          const mines = new Set([a, b, c, d]);
          for (let cell = 0; cell < 25; cell++) {
            if (mines.has(cell)) {
              oc[cell] = -1;
            } else {
              let adj = 0;
              for (const n of NEIGHBORS[cell]) if (mines.has(n)) adj++;
              oc[cell] = adj;
            }
          }
          worldOutcomes.push(oc);
          for (let cell = 0; cell < 25; cell++) {
            const key = cell * 10 + (oc[cell] + 1);
            if (!constraint.has(key)) constraint.set(key, []);
            constraint.get(key).push(wi);
          }
        }

  return { allWorlds, worldOutcomes, constraint };
}

// ---------------------------------------------------------------------------
// filterWorlds — intersect constraint sets for all revealed cells
// (exact port of filterWorlds() on the page)
// ---------------------------------------------------------------------------
function filterWorlds(cellStates, constraint) {
  let valid = null;
  for (let cell = 0; cell < 25; cell++) {
    const st = cellStates[cell];
    if (st === "?") continue;
    const outcome = (st === "t" || st === "r") ? -1 : parseInt(st, 10);
    const key = cell * 10 + (outcome + 1);
    const matching = constraint.get(key);
    if (!matching) return new Set();
    if (valid === null) {
      valid = new Set(matching);
    } else {
      const ms = new Set(matching);
      for (const w of valid) if (!ms.has(w)) valid.delete(w);
    }
  }
  // Nothing constrained yet → all 12,650 worlds valid
  if (valid === null) {
    valid = new Set();
    for (let i = 0; i < 12650; i++) valid.add(i);
  }
  return valid;
}

// ---------------------------------------------------------------------------
// heuristicAnalysis — adaptive threshold + entropy fallback
// (exact port of heuristicAnalysis() on the page)
// ---------------------------------------------------------------------------
function heuristicAnalysis(validWorlds, cellStates, worldOutcomes) {
  const total = validWorlds.size;
  if (total === 0) return { bestMove: -1 };
  const inv = 1.0 / total;
  const probs     = {};
  const entropies = {};

  for (let cell = 0; cell < 25; cell++) {
    if (cellStates[cell] !== "?") continue;
    const counts = new Map();
    let mc = 0;
    for (const w of validWorlds) {
      const oc = worldOutcomes[w][cell];
      counts.set(oc, (counts.get(oc) || 0) + 1);
      if (oc === -1) mc++;
    }
    probs[cell] = mc * inv;
    let ent = 0;
    for (const cnt of counts.values()) {
      const p = cnt * inv;
      if (p > 0) ent -= p * Math.log2(p);
    }
    entropies[cell] = ent;
  }

  // Count non-purple, non-red actual clicks (outcomes '0'..'4')
  let clicks = 0;
  for (let i = 0; i < 25; i++) {
    const s = cellStates[i];
    if (s !== "?" && s !== "t" && s !== "r") clicks++;
  }
  const clicksRemain = 7 - clicks;

  // Guaranteed purple (free)
  for (const c in probs) {
    if (probs[c] >= 0.9999)
      return { bestMove: parseInt(c, 10) };
  }

  // Last click: greedy max P(purple)
  if (clicksRemain <= 1) {
    let bm = -1, bp = 0;
    for (const c in probs) if (probs[c] > bp) { bp = probs[c]; bm = parseInt(c, 10); }
    return { bestMove: bm };
  }

  // Adaptive threshold
  const threshold = 0.06 + 0.06 * clicksRemain;
  let btc = -1, btp = 0;
  for (const c in probs) if (probs[c] > btp) { btp = probs[c]; btc = parseInt(c, 10); }
  if (btp > threshold) return { bestMove: btc };

  // Entropy fallback
  let bec = -1, bee = -1;
  for (const c in entropies) if (entropies[c] > bee) { bee = entropies[c]; bec = parseInt(c, 10); }
  return { bestMove: bec };
}

// ---------------------------------------------------------------------------
// walkBook — try every (book, transform) pair; return trie recommendation
// (exact port of walkBook() on the page; tries WR_BOOKS first, then ALL_BOOKS)
// ---------------------------------------------------------------------------
function walkBook(cellStates, wrBooks, allBooks) {
  // Count revealed cells
  let revealedCount = 0;
  for (let i = 0; i < 25; i++) if (cellStates[i] !== "?") revealedCount++;
  if (revealedCount === 0) return { bestMove: -1, onBook: false };

  const bookSets = [];
  if (wrBooks)  bookSets.push(wrBooks);
  if (allBooks) bookSets.push(allBooks);

  for (const books of bookSets) {
    for (const [, bk] of Object.entries(books)) {
      for (const transform of TRANSFORMS) {
        let node     = bk.root;
        let consumed = 0;
        let ok       = true;
        let safety   = 0;

        while (node && node.m !== undefined && safety++ < 25) {
          const actualCell = transform[node.m];
          const cs = cellStates[actualCell];
          if (cs === "?") break;           // trie wants this cell, not yet revealed
          if (node.c && node.c[cs]) {
            node = node.c[cs];
            consumed++;
          } else {
            ok = false;
            break;
          }
        }

        if (!ok || consumed !== revealedCount) continue;

        // Match — apply transform to get actual cell
        const bestMove = (node && node.m !== undefined) ? transform[node.m] : -1;
        return { bestMove, onBook: true };
      }
    }
  }

  return { bestMove: -1, onBook: false };
}

// ---------------------------------------------------------------------------
// getOpeningMove — best first click on an empty board
// Page WR-mode default: Inner Corner (canon=6, cells [6,8,16,18]).
// ---------------------------------------------------------------------------
function getOpeningMove(wrBooks, allBooks) {
  const books = wrBooks || allBooks;
  if (!books) return 6;
  if (books["6"] && books["6"].root) return books["6"].root.m;
  return 6;
}

// ---------------------------------------------------------------------------
// locateMineCandidates — cells that are mines in at least one remaining world
// (port of locateMineCandidates() on the page)
// ---------------------------------------------------------------------------
function locateMineCandidates(cellStates, constraint, allWorlds) {
  const validWorlds  = filterWorlds(cellStates, constraint);
  const knownTargets = new Set();
  for (let i = 0; i < 25; i++) if (cellStates[i] === "t") knownTargets.add(i);

  const candidates = new Set();
  for (const wi of validWorlds) {
    for (const m of allWorlds[wi]) {
      if (!knownTargets.has(m)) { candidates.add(m); break; }
    }
  }
  return candidates;
}

// ---------------------------------------------------------------------------
// harvestRanking — rank unclicked non-mine cells by adjacency-based value
// (port of harvestRanking() on the page)
// ---------------------------------------------------------------------------
function harvestRanking(cellStates, mines) {
  const ranking = [];
  for (let i = 0; i < 25; i++) {
    if (cellStates[i] !== "?") continue;
    let adj = 0;
    for (const n of NEIGHBORS[i]) if (mines.has(n)) adj++;
    ranking.push({ cell: i, value: HARVEST_VALUE[adj] });
  }
  ranking.sort((a, b) => b.value - a.value || a.cell - b.cell);
  return ranking;
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------
class BookHeuristicOQStrategy extends OQStrategy {

  /**
   * Called once before all games.
   * Stores precomputed world data and book data on `this` — never returned
   * through the state payload so it is never JSON-serialised.
   */
  initEvaluationRun() {
    // World precomputation
    const { allWorlds, worldOutcomes, constraint } = buildWorldData();
    this._allWorlds     = allWorlds;
    this._worldOutcomes = worldOutcomes;
    this._constraint    = constraint;

    // Load book data
    const dataPath = path.join(__dirname, "..", "..", "data", "oq_books.json");
    this._allBooks = null;
    this._wrBooks  = null;
    try {
      const raw    = fs.readFileSync(dataPath, "utf8");
      const parsed = JSON.parse(raw);
      this._allBooks = parsed.ALL_BOOKS || null;
      this._wrBooks  = parsed.WR_BOOKS  || null;
    } catch (e) {
      process.stderr.write(
        `[oq/book_heuristic] Warning: could not load oq_books.json: ${e.message}\n` +
        `[oq/book_heuristic] Falling back to heuristic-only mode.\n`
      );
    }

    // Return null — we don't use the state payload for large data
    return null;
  }

  /**
   * Called once per game. Returns null — no per-game state needed since
   * the full revealed list is passed to nextClick each call.
   */
  initGamePayload(meta, evaluationRunState) {
    return null;
  }

  /**
   * Choose the next cell to click.
   *
   * @param {Array<{row,col,color}>} revealed  All cells revealed so far.
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

    // Empty board → opening move
    if (revealedSet.size === 0) {
      const opener = getOpeningMove(this._wrBooks, this._allBooks);
      const r = pick(opener);
      if (r) return r;
    }

    // Book walk (WR_BOOKS first, ALL_BOOKS fallback)
    if (this._allBooks || this._wrBooks) {
      const { bestMove, onBook } = walkBook(
        cellStates, this._wrBooks, this._allBooks
      );
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

register(new BookHeuristicOQStrategy());
