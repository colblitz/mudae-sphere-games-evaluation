"use strict";

/**
 * zavex_core.js — Shared logic for the zavex oq book+heuristic strategy family.
 *
 * Exported by zavex_book_heuristic.js (deprecated), zavex_wr.js, and zavex_ev.js.
 * Do not require this file directly from outside strategies/oq/.
 *
 * Port of the solver logic from https://orb-quest-book.pages.dev/
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
// walkBook — try every (book, transform) pair; return trie recommendation.
// (exact port of walkBook() on the page)
//
// books: a single book set object (ALL_BOOKS or WR_BOOKS).  Pass null/undefined
//        to skip.  The deprecated zavex_book_heuristic.js passes two sets by
//        calling this twice; the wr/ev variants pass exactly one set each.
// ---------------------------------------------------------------------------
function walkBook(cellStates, books) {
  if (!books) return { bestMove: -1, onBook: false };

  // Count revealed cells
  let revealedCount = 0;
  for (let i = 0; i < 25; i++) if (cellStates[i] !== "?") revealedCount++;
  if (revealedCount === 0) return { bestMove: -1, onBook: false };

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

  return { bestMove: -1, onBook: false };
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
// Module exports
// ---------------------------------------------------------------------------
module.exports = {
  COLOR_TO_OUTCOME,
  HARVEST_VALUE,
  NEIGHBORS,
  TRANSFORMS,
  buildWorldData,
  filterWorlds,
  heuristicAnalysis,
  walkBook,
  locateMineCandidates,
  harvestRanking,
};
