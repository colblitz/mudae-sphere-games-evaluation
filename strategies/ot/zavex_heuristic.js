/**
 * heuristic.js — Heuristic strategy for /sphere trace (ot).
 *
 * Port of the solver logic from https://ouro-trace.zavex.workers.dev/
 *
 * Algorithm (mirrors getBestMove() on the page):
 *   1. Turn-0 opener: hardcoded from TURN0_PROBS (cell 8 for 6/7-ship games,
 *      cell 0 for 5-ship games).
 *   2. MCTS book lookup: if the current revealed state matches an entry in
 *      MCTS_BOOK, return that move.
 *   3. Per-cell probability computation:
 *      - Try exact joint enumeration (JS backtracker, budget=20000 boards).
 *      - Fall back to independence approximation if budget exceeded.
 *   4. Classify cells: certain-ship (P≥0.999), certain-blue (P(blue)≥0.999),
 *      or candidate (ambiguous).
 *   5. Decision tree (exact port):
 *      - Nothing ambiguous → harvest: best certain ship, else certain blue.
 *      - Fishing window closed (n≥5) AND certain ship exists → take it.
 *      - Danger zone (b+1≥4 AND n≥5) → min P(blue) among candidates.
 *      - Fishing mode (5-ship: n<5; 6-ship: b≥2 AND n≤3) →
 *          max(EV + LAMBDA_FISH * P(blue)).
 *      - Default → max(EV + LAMBDA_GINI * Gini-impurity).
 *
 * HV ship abstraction:
 *   Light, Dark, Red, Rainbow are all size-2 ships ("HV colors"). Until a
 *   slot is revealed the solver treats them abstractly as u1, u2, u3.
 *   The first HV color revealed in cell order binds to u1, second to u2, etc.
 *   This mirrors the getBindings() / toSolverReveals() logic on the page.
 *
 * n_colors → nShips mapping:
 *   n_colors 6 → 5 ships, 7 → 6 ships, 8 → 7 ships.
 *   9-color (8 ships) is not handled by the page's SHIPS_BY_COUNT; returns a
 *   random unclicked cell as a safe fallback.
 *
 * Color mapping (harness → solver):
 *   spT → "Teal",  spG → "Green",  spY → "Yellow", spO → "Orange",
 *   spL → "Light", spD → "Dark",   spR → "Red",    spW → "Rainbow",
 *   spB → "Blue"
 *
 * Game-end rule (exact from page comments):
 *   A blue click ends the game iff post-state has b ≥ 4 AND n ≥ 5.
 *   (b = blues used, n = ships hit.)
 *
 * Tuned constants (from page):
 *   LAMBDA_FISH = 300  (fishing bias, tuned via sweep_lambda_exact.py)
 *   LAMBDA_GINI = 250  (Gini tiebreaker, tuned via sweep_lambda_info.py)
 */

"use strict";

const { OTStrategy, register } = require("../../interface/strategy.js");

// ---------------------------------------------------------------------------
// Color mappings
// ---------------------------------------------------------------------------
const HARNESS_TO_COLOR = {
  spT: "Teal",
  spG: "Green",
  spY: "Yellow",
  spO: "Orange",
  spL: "Light",
  spD: "Dark",
  spR: "Red",
  spW: "Rainbow",
  spB: "Blue",
};

const BLUE    = "Blue";
const HV_COLORS = ["Light", "Dark", "Red", "Rainbow"];
const HV_GENERIC = new Set(HV_COLORS);

// ---------------------------------------------------------------------------
// Ship configurations by nShips (= n_colors - 1)
// (SHIPS_BY_COUNT on the page; only 5/6/7 defined — matches page exactly)
// ---------------------------------------------------------------------------
const SHIPS_BY_COUNT = {
  5: [["Teal",4],["Green",3],["Yellow",3],["Orange",2],["u1",2]],
  6: [["Teal",4],["Green",3],["Yellow",3],["Orange",2],["u1",2],["u2",2]],
  7: [["Teal",4],["Green",3],["Yellow",3],["Orange",2],["u1",2],["u2",2],["u3",2]],
};

// ---------------------------------------------------------------------------
// Cell values (VALUES on the page)
// Light rounded 75.9→76, Dark 104.5→105
// ---------------------------------------------------------------------------
const VALUES = {
  Teal:20, Green:35, Yellow:55, Orange:90,
  Light:76, Dark:105, Red:150, Rainbow:500,
  Blue:10,
};

const LAMBDA_FISH = 300;
const LAMBDA_GINI = 250;

// ---------------------------------------------------------------------------
// Ship placement bitmasks for sizes 2, 3, 4 on a 5×5 grid
// (genPlacements() on the page)
// ---------------------------------------------------------------------------
function genPlacements(size) {
  const pl = [];
  // Horizontal
  for (let r = 0; r < 5; r++)
    for (let c = 0; c <= 5 - size; c++) {
      let m = 0;
      for (let i = 0; i < size; i++) m |= 1 << (r * 5 + c + i);
      pl.push(m);
    }
  // Vertical
  for (let c = 0; c < 5; c++)
    for (let r = 0; r <= 5 - size; r++) {
      let m = 0;
      for (let i = 0; i < size; i++) m |= 1 << ((r + i) * 5 + c);
      pl.push(m);
    }
  return pl;
}
const PLACEMENTS = { 2: genPlacements(2), 3: genPlacements(3), 4: genPlacements(4) };

// ---------------------------------------------------------------------------
// TURN0_PROBS — precomputed per-cell color probabilities for an empty board
// (verbatim from the page, then remapped to slot names)
// ---------------------------------------------------------------------------
const TURN0_PROBS_RAW = {
  5: {total: 1194816, probs: [
    {Teal:0.140828,Light:0.061543,Orange:0.061543,Yellow:0.085884,Green:0.085884,Blue:0.564318},
    {Teal:0.181477,Green:0.111639,Orange:0.080238,Light:0.080238,Yellow:0.111639,Blue:0.434768},
    {Teal:0.168702,Green:0.151247,Yellow:0.151247,Light:0.073417,Orange:0.073417,Blue:0.381970},
    {Teal:0.181477,Orange:0.080238,Light:0.080238,Green:0.111639,Yellow:0.111639,Blue:0.434768},
    {Light:0.061543,Orange:0.061543,Yellow:0.085884,Green:0.085884,Teal:0.140828,Blue:0.564318},
    {Green:0.111639,Light:0.080238,Orange:0.080238,Yellow:0.111639,Teal:0.181477,Blue:0.434768},
    {Green:0.115050,Orange:0.092617,Light:0.092617,Yellow:0.115050,Teal:0.162596,Blue:0.422070},
    {Green:0.136647,Yellow:0.136647,Light:0.089523,Orange:0.089523,Teal:0.137045,Blue:0.410614},
    {Orange:0.092617,Light:0.092617,Yellow:0.115050,Green:0.115050,Teal:0.162596,Blue:0.422070},
    {Orange:0.080238,Light:0.080238,Yellow:0.111639,Green:0.111639,Teal:0.181477,Blue:0.434768},
    {Yellow:0.151247,Light:0.073417,Orange:0.073417,Green:0.151247,Teal:0.168702,Blue:0.381970},
    {Yellow:0.136647,Light:0.089523,Orange:0.089523,Green:0.136647,Teal:0.137045,Blue:0.410614},
    {Yellow:0.151575,Light:0.089694,Orange:0.089694,Green:0.151575,Teal:0.111495,Blue:0.405967},
    {Light:0.089523,Orange:0.089523,Yellow:0.136647,Green:0.136647,Teal:0.137045,Blue:0.410614},
    {Light:0.073417,Orange:0.073417,Yellow:0.151247,Green:0.151247,Teal:0.168702,Blue:0.381970},
    {Light:0.080238,Orange:0.080238,Yellow:0.111639,Green:0.111639,Teal:0.181477,Blue:0.434768},
    {Light:0.092617,Orange:0.092617,Yellow:0.115050,Green:0.115050,Teal:0.162596,Blue:0.422070},
    {Light:0.089523,Orange:0.089523,Yellow:0.136647,Green:0.136647,Teal:0.137045,Blue:0.410614},
    {Light:0.092617,Orange:0.092617,Yellow:0.115050,Green:0.115050,Teal:0.162596,Blue:0.422070},
    {Light:0.080238,Orange:0.080238,Yellow:0.111639,Green:0.111639,Teal:0.181477,Blue:0.434768},
    {Light:0.061543,Orange:0.061543,Yellow:0.085884,Green:0.085884,Teal:0.140828,Blue:0.564318},
    {Light:0.080238,Orange:0.080238,Yellow:0.111639,Green:0.111639,Teal:0.181477,Blue:0.434768},
    {Light:0.073417,Orange:0.073417,Yellow:0.151247,Green:0.151247,Teal:0.168702,Blue:0.381970},
    {Light:0.080238,Orange:0.080238,Yellow:0.111639,Green:0.111639,Teal:0.181477,Blue:0.434768},
    {Light:0.061543,Orange:0.061543,Yellow:0.085884,Green:0.085884,Teal:0.140828,Blue:0.564318},
  ]},
  6: {total: 11345760, probs: [
    {Teal:0.145630,Dark:0.064362,Light:0.064362,Orange:0.064362,Yellow:0.089414,Green:0.089414,Blue:0.482455},
    {Teal:0.184360,Green:0.111978,Orange:0.080664,Light:0.080664,Dark:0.080664,Yellow:0.111978,Blue:0.349692},
    {Teal:0.172539,Green:0.153337,Yellow:0.153337,Light:0.071763,Dark:0.071763,Orange:0.071763,Blue:0.305498},
    {Teal:0.184360,Orange:0.080664,Light:0.080664,Dark:0.080664,Green:0.111978,Yellow:0.111978,Blue:0.349692},
    {Dark:0.064362,Light:0.064362,Orange:0.064362,Yellow:0.089414,Green:0.089414,Teal:0.145630,Blue:0.482455},
    {Green:0.111978,Dark:0.080664,Light:0.080664,Orange:0.080664,Yellow:0.111978,Teal:0.184360,Blue:0.349692},
    {Green:0.112862,Orange:0.091593,Light:0.091593,Dark:0.091593,Yellow:0.112862,Teal:0.154922,Blue:0.344574},
    {Green:0.133547,Yellow:0.133547,Light:0.088672,Dark:0.088672,Orange:0.088672,Teal:0.131279,Blue:0.335609},
    {Orange:0.091593,Light:0.091593,Dark:0.091593,Yellow:0.112862,Green:0.112862,Teal:0.154922,Blue:0.344574},
    {Orange:0.080664,Light:0.080664,Dark:0.080664,Yellow:0.111978,Green:0.111978,Teal:0.184360,Blue:0.349692},
    {Yellow:0.153337,Dark:0.071763,Light:0.071763,Orange:0.071763,Green:0.153337,Teal:0.172539,Blue:0.305498},
    {Yellow:0.133547,Light:0.088672,Dark:0.088672,Orange:0.088672,Green:0.133547,Teal:0.131279,Blue:0.335609},
    {Yellow:0.147536,Dark:0.089126,Light:0.089126,Orange:0.089126,Green:0.147536,Teal:0.107636,Blue:0.329915},
    {Light:0.088672,Dark:0.088672,Orange:0.088672,Yellow:0.133547,Green:0.133547,Teal:0.131279,Blue:0.335609},
    {Light:0.071763,Dark:0.071763,Orange:0.071763,Yellow:0.153337,Green:0.153337,Teal:0.172539,Blue:0.305498},
    {Dark:0.080664,Light:0.080664,Orange:0.080664,Yellow:0.111978,Green:0.111978,Teal:0.184360,Blue:0.349692},
    {Dark:0.091593,Light:0.091593,Orange:0.091593,Yellow:0.112862,Green:0.112862,Teal:0.154922,Blue:0.344574},
    {Dark:0.088672,Light:0.088672,Orange:0.088672,Yellow:0.133547,Green:0.133547,Teal:0.131279,Blue:0.335609},
    {Dark:0.091593,Light:0.091593,Orange:0.091593,Yellow:0.112862,Green:0.112862,Teal:0.154922,Blue:0.344574},
    {Dark:0.080664,Light:0.080664,Orange:0.080664,Yellow:0.111978,Green:0.111978,Teal:0.184360,Blue:0.349692},
    {Dark:0.064362,Light:0.064362,Orange:0.064362,Yellow:0.089414,Green:0.089414,Teal:0.145630,Blue:0.482455},
    {Dark:0.080664,Light:0.080664,Orange:0.080664,Yellow:0.111978,Green:0.111978,Teal:0.184360,Blue:0.349692},
    {Dark:0.071763,Light:0.071763,Orange:0.071763,Yellow:0.153337,Green:0.153337,Teal:0.172539,Blue:0.305498},
    {Dark:0.080664,Light:0.080664,Orange:0.080664,Yellow:0.111978,Green:0.111978,Teal:0.184360,Blue:0.349692},
    {Dark:0.064362,Light:0.064362,Orange:0.064362,Yellow:0.089414,Green:0.089414,Teal:0.145630,Blue:0.482455},
  ]},
  7: {total: 73968768, probs: [
    {Teal:0.149505,Red:0.067624,Dark:0.067624,Light:0.067624,Orange:0.067624,Yellow:0.093475,Green:0.093475,Blue:0.393050},
    {Teal:0.186623,Green:0.111739,Orange:0.081055,Light:0.081055,Dark:0.081055,Red:0.081055,Yellow:0.111739,Blue:0.265679},
    {Teal:0.175765,Green:0.155269,Yellow:0.155269,Light:0.069742,Dark:0.069742,Red:0.069742,Orange:0.069742,Blue:0.234730},
    {Teal:0.186623,Orange:0.081055,Light:0.081055,Dark:0.081055,Red:0.081055,Green:0.111739,Yellow:0.111739,Blue:0.265679},
    {Red:0.067624,Dark:0.067624,Light:0.067624,Orange:0.067624,Yellow:0.093475,Green:0.093475,Teal:0.149505,Blue:0.393050},
    {Green:0.111739,Red:0.081055,Dark:0.081055,Light:0.081055,Orange:0.081055,Yellow:0.111739,Teal:0.186623,Blue:0.265679},
    {Green:0.111009,Orange:0.090570,Light:0.090570,Dark:0.090570,Red:0.090570,Yellow:0.111009,Teal:0.148471,Blue:0.267233},
    {Green:0.130750,Yellow:0.130750,Light:0.087890,Dark:0.087890,Red:0.087890,Orange:0.087890,Teal:0.126754,Blue:0.260184},
    {Orange:0.090570,Light:0.090570,Dark:0.090570,Red:0.090570,Yellow:0.111009,Green:0.111009,Teal:0.148471,Blue:0.267233},
    {Orange:0.081055,Light:0.081055,Dark:0.081055,Red:0.081055,Yellow:0.111739,Green:0.111739,Teal:0.186623,Blue:0.265679},
    {Yellow:0.155269,Red:0.069742,Dark:0.069742,Light:0.069742,Orange:0.069742,Green:0.155269,Teal:0.175765,Blue:0.234730},
    {Yellow:0.130750,Light:0.087890,Dark:0.087890,Red:0.087890,Orange:0.087890,Green:0.130750,Teal:0.126754,Blue:0.260184},
    {Yellow:0.144076,Dark:0.088257,Red:0.088257,Light:0.088257,Orange:0.088257,Green:0.144076,Teal:0.105037,Blue:0.253781},
    {Light:0.087890,Dark:0.087890,Red:0.087890,Orange:0.087890,Yellow:0.130750,Green:0.130750,Teal:0.126754,Blue:0.260184},
    {Light:0.069742,Dark:0.069742,Red:0.069742,Orange:0.069742,Yellow:0.155269,Green:0.155269,Teal:0.175765,Blue:0.234730},
    {Dark:0.081055,Red:0.081055,Light:0.081055,Orange:0.081055,Yellow:0.111739,Green:0.111739,Teal:0.186623,Blue:0.265679},
    {Dark:0.090570,Red:0.090570,Light:0.090570,Orange:0.090570,Yellow:0.111009,Green:0.111009,Teal:0.148471,Blue:0.267233},
    {Red:0.087890,Dark:0.087890,Light:0.087890,Orange:0.087890,Yellow:0.130750,Green:0.130750,Teal:0.126754,Blue:0.260184},
    {Red:0.090570,Dark:0.090570,Light:0.090570,Orange:0.090570,Yellow:0.111009,Green:0.111009,Teal:0.148471,Blue:0.267233},
    {Red:0.081055,Dark:0.081055,Light:0.081055,Orange:0.081055,Yellow:0.111739,Green:0.111739,Teal:0.186623,Blue:0.265679},
    {Red:0.067624,Dark:0.067624,Light:0.067624,Orange:0.067624,Yellow:0.093475,Green:0.093475,Teal:0.149505,Blue:0.393050},
    {Red:0.081055,Dark:0.081055,Light:0.081055,Orange:0.081055,Yellow:0.111739,Green:0.111739,Teal:0.186623,Blue:0.265679},
    {Red:0.069742,Dark:0.069742,Light:0.069742,Orange:0.069742,Yellow:0.155269,Green:0.155269,Teal:0.175765,Blue:0.234730},
    {Red:0.081055,Dark:0.081055,Light:0.081055,Orange:0.081055,Yellow:0.111739,Green:0.111739,Teal:0.186623,Blue:0.265679},
    {Red:0.067624,Dark:0.067624,Light:0.067624,Orange:0.067624,Yellow:0.093475,Green:0.093475,Teal:0.149505,Blue:0.393050},
  ]},
};

// Remap TURN0_PROBS from canonical HV names to slot names (u1, u2, u3)
// (matches renameTurn0ToSlots() on the page)
const SLOT_MAP = {
  5: {Light:"u1"},
  6: {Light:"u1", Dark:"u2"},
  7: {Light:"u1", Dark:"u2", Red:"u3"},
};
const TURN0_PROBS = {};
for (const n of [5, 6, 7]) {
  const map = SLOT_MAP[n];
  TURN0_PROBS[n] = {
    total: TURN0_PROBS_RAW[n].total,
    probs: TURN0_PROBS_RAW[n].probs.map(d => {
      const out = {};
      for (const [k, v] of Object.entries(d)) out[map[k] || k] = v;
      return out;
    }),
  };
}

// ---------------------------------------------------------------------------
// MCTS_BOOK — verbatim from https://ouro-trace.zavex.workers.dev/mcts_book.js
// Keys: nShips → { "cell,Color[;cell,Color]": recommendedCell }
// HV colors (Light/Dark/Red/Rainbow) normalized to u1/u2/u3 in cell order.
// ---------------------------------------------------------------------------
const MCTS_BOOK = {
  5: {
    "0,Blue": 1,
    "0,Blue;11,Yellow": 21,
    "0,Blue;12,Yellow": 14,
    "0,Green": 4,
    "0,Green;12,Orange": 17,
    "0,Green;4,Orange": 9,
    "0,Green;4,Yellow": 3,
    "0,Orange": 4,
    "0,Orange;12,Teal": 1,
    "0,Orange;12,Yellow": 5,
    "0,Orange;4,Yellow": 1,
    "0,Teal": 4,
    "0,Teal;11,Green": 6,
    "0,Teal;11,Orange": 6,
    "0,Teal;11,Yellow": 13,
    "0,Teal;11,u1": 12,
    "0,Teal;12,Green": 7,
    "0,Teal;12,Orange": 7,
    "0,Teal;12,Yellow": 17,
    "0,Teal;7,Green": 13,
    "0,Teal;7,Orange": 8,
    "0,Teal;7,Yellow": 17,
    "0,Yellow": 4,
    "0,u1": 4,
    "0,u1;12,Teal": 8,
  },
  6: {
    "3,Blue;8,Orange": 7,
    "3,Orange;8,Orange": 12,
    "3,Teal;8,Orange": 11,
    "3,u1;8,Orange": 2,
    "5,Green;8,Teal": 15,
    "5,Teal;8,Teal": 16,
    "5,Yellow;8,Teal": 7,
    "5,u1;8,Teal": 23,
    "6,Blue;8,Green": 16,
    "6,Green;8,Yellow": 18,
    "6,Yellow;8,Green": 18,
    "6,Yellow;8,Yellow": 2,
    "7,Blue;8,Orange": 3,
    "7,Blue;8,Yellow": 17,
    "7,Orange;8,Orange": 14,
    "7,Teal;8,Orange": 9,
    "7,Teal;8,Yellow": 22,
    "7,Teal;8,u1": 9,
    "7,Yellow;8,Yellow": 9,
    "7,u1;8,u1": 11,
    "7,u1;8,u2": 9,
    "8,Blue": 18,
    "8,Blue;11,Blue": 18,
    "8,Blue;11,Green": 21,
    "8,Blue;11,Teal": 18,
    "8,Blue;12,Yellow": 10,
    "8,Blue;17,Blue": 6,
    "8,Blue;17,Green": 15,
    "8,Blue;17,Teal": 15,
    "8,Blue;17,Yellow": 15,
    "8,Blue;18,Green": 6,
    "8,Blue;18,Teal": 6,
    "8,Blue;18,u1": 19,
    "8,Green": 11,
    "8,Green;11,Blue": 22,
    "8,Green;11,Yellow": 18,
    "8,Green;11,u1": 18,
    "8,Green;12,Blue": 15,
    "8,Green;12,Teal": 10,
    "8,Green;18,Blue": 16,
    "8,Green;18,Green": 11,
    "8,Green;18,Yellow": 11,
    "8,Orange": 3,
    "8,Orange;12,Blue": 3,
    "8,Orange;12,Green": 17,
    "8,Orange;12,Teal": 10,
    "8,Orange;12,u1": 13,
    "8,Orange;13,Teal": 3,
    "8,Orange;13,Yellow": 3,
    "8,Teal": 5,
    "8,Teal;11,Blue": 5,
    "8,Teal;11,Green": 13,
    "8,Teal;11,u1": 16,
    "8,Teal;12,Blue": 23,
    "8,Teal;17,Blue": 23,
    "8,Teal;17,Orange": 18,
    "8,Teal;17,Yellow": 7,
    "8,Teal;17,u1": 18,
    "8,Yellow": 6,
    "8,Yellow;18,Blue": 3,
    "8,Yellow;18,Green": 11,
    "8,Yellow;18,Orange": 11,
    "8,Yellow;18,Yellow": 2,
    "8,Yellow;18,u1": 17,
    "8,u1": 9,
    "8,u1;12,Blue": 9,
    "8,u1;12,Orange": 13,
    "8,u1;12,u2": 7,
    "8,u1;13,Blue": 16,
    "8,u1;13,Green": 11,
    "8,u1;13,Orange": 3,
    "8,u1;13,Teal": 3,
    "8,u1;13,Yellow": 7,
    "8,u1;9,Blue": 13,
    "8,u1;9,u1": 2,
    "8,u1;9,u2": 14,
  },
  7: {
    "2,Blue;8,Blue": 6,
    "2,Green;8,Blue": 6,
    "2,Orange;8,Blue": 11,
    "2,Teal;8,Blue": 6,
    "2,u1;8,Blue": 12,
    "6,Blue;8,Green": 2,
    "6,Green;8,Green": 17,
    "6,Teal;8,Green": 21,
    "6,Yellow;8,Yellow": 16,
    "6,u1;8,Green": 7,
    "6,u1;8,Yellow": 11,
    "7,Blue;8,Orange": 11,
    "7,Blue;8,u1": 3,
    "7,Green;8,u1": 5,
    "7,Orange;8,Orange": 16,
    "7,Teal;8,u1": 3,
    "7,Yellow;8,Orange": 17,
    "7,u1;8,u1": 18,
    "7,u1;8,u2": 13,
    "8,Blue": 2,
    "8,Blue;11,Blue": 12,
    "8,Blue;11,Green": 1,
    "8,Blue;11,Orange": 18,
    "8,Blue;11,u1": 16,
    "8,Blue;12,Green": 6,
    "8,Blue;12,Orange": 18,
    "8,Blue;12,u1": 11,
    "8,Blue;17,Blue": 6,
    "8,Blue;17,Green": 15,
    "8,Blue;17,Yellow": 15,
    "8,Blue;17,u1": 6,
    "8,Green": 6,
    "8,Green;12,Teal": 6,
    "8,Green;12,Yellow": 18,
    "8,Green;12,u1": 17,
    "8,Green;18,Green": 16,
    "8,Green;18,Orange": 13,
    "8,Green;18,u1": 13,
    "8,Orange": 13,
    "8,Orange;12,Blue": 9,
    "8,Orange;12,Green": 13,
    "8,Teal": 12,
    "8,Teal;11,Blue": 5,
    "8,Teal;11,Orange": 18,
    "8,Teal;11,u1": 16,
    "8,Teal;12,Green": 6,
    "8,Teal;17,Blue": 23,
    "8,Teal;17,Green": 16,
    "8,Yellow": 16,
    "8,Yellow;12,Teal": 18,
    "8,Yellow;12,u1": 18,
    "8,Yellow;16,Blue": 18,
    "8,Yellow;16,u1": 17,
    "8,Yellow;18,Yellow": 16,
    "8,Yellow;18,u1": 17,
    "8,u1": 7,
    "8,u1;11,Green": 3,
    "8,u1;11,u2": 12,
    "8,u1;12,Blue": 9,
    "8,u1;12,Green": 7,
    "8,u1;12,Orange": 7,
    "8,u1;12,Yellow": 13,
    "8,u1;12,u2": 7,
    "8,u1;13,Blue": 17,
    "8,u1;13,Green": 17,
    "8,u1;13,Orange": 12,
    "8,u1;13,Teal": 17,
    "8,u1;13,Yellow": 23,
    "8,u1;13,u1": 11,
    "8,u1;13,u2": 12,
  },
};

// ---------------------------------------------------------------------------
// Helper: is a color name an HV slot name (u1/u2/u3)?
// ---------------------------------------------------------------------------
function isSlotName(s) { return /^u\d+$/.test(s); }

// ---------------------------------------------------------------------------
// getBindings — map first HV color seen → u1, second → u2, etc.
// (port of getBindings() on the page)
// ---------------------------------------------------------------------------
function getBindings(revealed) {
  const slotToColor = {};
  const colorToSlot = {};
  let next = 1;
  for (const col of Object.values(revealed)) {
    if (!HV_GENERIC.has(col) || col in colorToSlot) continue;
    const slot = "u" + next;
    slotToColor[slot] = col;
    colorToSlot[col]  = slot;
    next++;
  }
  return { slotToColor, colorToSlot };
}

// ---------------------------------------------------------------------------
// toSolverReveals — translate user-facing HV colors to slot names
// (port of toSolverReveals() on the page)
// ---------------------------------------------------------------------------
function toSolverReveals(revealed, colorToSlot) {
  const out = {};
  for (const [cell, col] of Object.entries(revealed)) {
    out[cell] = HV_GENERIC.has(col) ? colorToSlot[col] : col;
  }
  return out;
}

// ---------------------------------------------------------------------------
// translateProbs — slot names → user-facing color labels
// (port of translateProbs() on the page)
// ---------------------------------------------------------------------------
function translateProbs(probs, slotToColor) {
  return probs.map(d => {
    const out = {};
    for (const [k, v] of Object.entries(d)) out[slotToColor[k] || k] = v;
    return out;
  });
}

// ---------------------------------------------------------------------------
// expectedHVValue — mean value of HV colors not yet revealed
// (port of expectedHVValue() on the page)
// ---------------------------------------------------------------------------
function expectedHVValue(revealed) {
  const revealedSet = new Set(Object.values(revealed));
  const unrevealed  = HV_COLORS.filter(c => !revealedSet.has(c));
  if (unrevealed.length === 0) return 0;
  return unrevealed.reduce((s, c) => s + VALUES[c], 0) / unrevealed.length;
}

// ---------------------------------------------------------------------------
// filterPlacements — valid placement masks per ship given current reveals
// (port of filterPlacements() on the page)
// ---------------------------------------------------------------------------
function filterPlacements(revealed, ships) {
  const revealSelf  = {};
  const revealOther = {};
  for (const [s] of ships) { revealSelf[s] = 0; revealOther[s] = 0; }

  for (const [cellStr, color] of Object.entries(revealed)) {
    const bit = 1 << (+cellStr);
    if (color === BLUE) {
      for (const [s] of ships) revealOther[s] |= bit;
    } else {
      if (color in revealSelf) revealSelf[color] |= bit;
      for (const [s] of ships) if (s !== color) revealOther[s] |= bit;
    }
  }

  const valid = {};
  for (const [ship, size] of ships) {
    const must = revealSelf[ship], avoid = revealOther[ship];
    const out  = [];
    for (const p of PLACEMENTS[size]) {
      if ((p & must) === must && (p & avoid) === 0) out.push(p);
    }
    valid[ship] = out;
  }
  return valid;
}

// ---------------------------------------------------------------------------
// popcountCells helper
// ---------------------------------------------------------------------------
function popcountCells(mask, cb) {
  while (mask) {
    const lsb = mask & -mask;
    cb(Math.log2(lsb));
    mask ^= lsb;
  }
}

// ---------------------------------------------------------------------------
// perCellProbs — independence approximation (fallback)
// (port of perCellProbs() on the page)
// ---------------------------------------------------------------------------
function perCellProbs(revealed, ships) {
  const valid = filterPlacements(revealed, ships);
  const pShipPerCell = {};
  for (const [ship] of ships) {
    const n   = valid[ship].length;
    const cnt = new Array(25).fill(0);
    if (n === 0) { pShipPerCell[ship] = cnt; continue; }
    for (const p of valid[ship]) popcountCells(p, c => cnt[c]++);
    pShipPerCell[ship] = cnt.map(x => x / n);
  }
  const out = [];
  for (let c = 0; c < 25; c++) {
    if (String(c) in revealed) { out.push({}); continue; }
    let pBlue = 1.0, shipSum = 0;
    for (const [ship] of ships) {
      const ps = pShipPerCell[ship][c];
      pBlue   *= (1 - ps);
      shipSum += ps;
    }
    const total = shipSum + pBlue;
    const d = {};
    if (total <= 0) { d[BLUE] = 1; out.push(d); continue; }
    const scale = 1 / total;
    for (const [ship] of ships) {
      const v = pShipPerCell[ship][c] * scale;
      if (v > 1e-9) d[ship] = v;
    }
    d[BLUE] = pBlue * scale;
    out.push(d);
  }
  return out;
}

// ---------------------------------------------------------------------------
// exactPerCellProbs — backtracking joint enumeration with budget cap
// (port of exactPerCellProbs() on the page)
// ---------------------------------------------------------------------------
function exactPerCellProbs(revealed, ships, budget) {
  const valid = filterPlacements(revealed, ships);
  for (const [s] of ships) if (valid[s].length === 0) return { probs: null, total: 0 };

  const nShips = ships.length;
  const order  = ships.map((_, i) => i)
    .sort((a, b) => valid[ships[a][0]].length - valid[ships[b][0]].length);
  const placementLists = order.map(i => valid[ships[i][0]]);
  const shipNames      = order.map(i => ships[i][0]);

  const assignment = new Array(nShips);
  const counts     = Array.from({ length: 25 }, () => ({}));
  let total = 0, aborted = false;

  function backtrack(idx, occ) {
    if (aborted) return;
    if (idx === nShips) {
      total++;
      if (total > budget) { aborted = true; return; }
      for (let i = 0; i < nShips; i++) {
        let m = assignment[i];
        const name = shipNames[i];
        while (m) {
          const lsb = m & -m;
          const c   = Math.log2(lsb);
          counts[c][name] = (counts[c][name] || 0) + 1;
          m ^= lsb;
        }
      }
      return;
    }
    const list = placementLists[idx];
    for (let j = 0; j < list.length; j++) {
      const p = list[j];
      if (p & occ) continue;
      assignment[idx] = p;
      backtrack(idx + 1, occ | p);
      if (aborted) return;
    }
  }
  backtrack(0, 0);
  if (aborted || total === 0) return { probs: null, total };

  const out = [];
  for (let c = 0; c < 25; c++) {
    if (String(c) in revealed) { out.push({}); continue; }
    const d = {};
    let shipTotal = 0;
    for (const [color, cnt] of Object.entries(counts[c])) {
      d[color]   = cnt / total;
      shipTotal += d[color];
    }
    d[BLUE] = Math.max(0, 1 - shipTotal);
    out.push(d);
  }
  return { probs: out, total };
}

// ---------------------------------------------------------------------------
// maybeExactProbs — turn-0 fast path + exact backtracker + independence fallback
// (port of maybeExactProbs() on the page; WASM path omitted)
//
// NOTE: The original page uses a WASM C solver for exact enumeration, which
// handles millions of boards in ~7ms. Without WASM we can only run the JS
// backtracker cheaply when the board count is genuinely small.
// We use budget=5000 with a tight upper-bound pre-check (50*budget = 250K).
// States with more valid boards fall back to the independence approximation.
// ---------------------------------------------------------------------------
function maybeExactProbs(revealed, ships, budget) {
  budget = budget || 5000;

  // Turn-0 fast path (exact precomputed probs from TURN0_PROBS)
  if (Object.keys(revealed).length === 0) {
    const n = ships.length;
    if (TURN0_PROBS[n]) return { probs: TURN0_PROBS[n].probs, total: TURN0_PROBS[n].total, exact: true };
  }

  // Upper-bound check: only attempt exact backtrack when board count is small
  const valid = filterPlacements(revealed, ships);
  let upper = 1;
  for (const [s] of ships) {
    upper *= Math.max(1, valid[s].length);
    if (upper > 50 * budget) return { probs: perCellProbs(revealed, ships), total: null, exact: false };
  }

  const { probs, total } = exactPerCellProbs(revealed, ships, budget);
  if (probs === null) return { probs: perCellProbs(revealed, ships), total: null, exact: false };
  return { probs, total, exact: true };
}

// ---------------------------------------------------------------------------
// cellEV — expected value of clicking a cell
// (port of cellEV() on the page)
// ---------------------------------------------------------------------------
function cellEV(d, vals) {
  vals = vals || VALUES;
  let s = 0;
  for (const [col, p] of Object.entries(d)) s += p * (vals[col] || 0);
  return s;
}

// ---------------------------------------------------------------------------
// giniImpurity — 1 − Σ P(outcome)²
// (port of giniImpurity() on the page)
// ---------------------------------------------------------------------------
function giniImpurity(d) {
  let s = 0;
  for (const p of Object.values(d)) s += p * p;
  return 1 - s;
}

// ---------------------------------------------------------------------------
// classify — split unrevealed cells into certain-blue, certain-ship, candidates
// (port of classify() on the page)
// ---------------------------------------------------------------------------
function classify(probs, revealed) {
  const cb = [], cs = [], cand = [];
  for (let c = 0; c < 25; c++) {
    if (String(c) in revealed) continue;
    const d  = probs[c];
    const pb = d[BLUE] || 0;
    let maxShipP = 0;
    for (const [col, p] of Object.entries(d)) if (col !== BLUE && p > maxShipP) maxShipP = p;
    if (pb > 0.999)      cb.push(c);
    else if (maxShipP > 0.999) cs.push(c);
    else                 cand.push(c);
  }
  return { cb, cs, cand };
}

// ---------------------------------------------------------------------------
// harvestPick — pick best certain ship (by value), else first certain blue
// (port of harvestPick() on the page)
// ---------------------------------------------------------------------------
function harvestPick(cs, cb, probs, vals) {
  vals = vals || VALUES;
  if (cs.length) {
    let best = cs[0], bestV = -Infinity;
    for (const c of cs) {
      let maxCol = BLUE, maxP = 0;
      for (const [col, p] of Object.entries(probs[c])) if (p > maxP) { maxP = p; maxCol = col; }
      const v = vals[maxCol] || 0;
      if (v > bestV) { bestV = v; best = c; }
    }
    return best;
  }
  if (cb.length) return cb[0];
  return null;
}

// ---------------------------------------------------------------------------
// pickBest — argmax over candidates by scoring function
// (port of pickBest() on the page)
// ---------------------------------------------------------------------------
function pickBest(cand, scoreFn) {
  let best = cand[0], bestScore = -Infinity;
  for (const c of cand) {
    const s = scoreFn(c);
    if (s > bestScore) { bestScore = s; best = c; }
  }
  return best;
}

// ---------------------------------------------------------------------------
// tryBook — MCTS book lookup
// (port of tryBook() on the page)
// ---------------------------------------------------------------------------
function tryBook(revealed, nShips) {
  const book = MCTS_BOOK[nShips];
  if (!book) return null;
  const entries = Object.entries(revealed)
    .map(([c, col]) => [parseInt(c, 10), col])
    .sort((a, b) => a[0] - b[0]);
  // Normalize HV colors to u1/u2/... in cell order
  let nextU = 1;
  const hvMap = {};
  const translated = entries.map(([c, col]) => {
    if (HV_GENERIC.has(col)) {
      if (!(col in hvMap)) hvMap[col] = "u" + (nextU++);
      return [c, hvMap[col]];
    }
    return [c, col];
  });
  const key = translated.map(([c, col]) => `${c},${col}`).join(";");
  const v = book[key];
  return v !== undefined ? v : null;
}

// ---------------------------------------------------------------------------
// getBestMove — main decision function
// (port of getBestMove() on the page; WASM endgame solver omitted)
// ---------------------------------------------------------------------------
function getBestMove(revealed, b, n, nShips) {
  const ships = SHIPS_BY_COUNT[nShips];
  if (!ships) return null;  // unsupported nShips

  // Turn-0 fast path
  if (Object.keys(revealed).length === 0) {
    // 6/7-ship: inner corner (cell 8). 5-ship: corner (cell 0).
    return nShips === 5 ? 0 : 8;
  }

  // MCTS book
  const bk = tryBook(revealed, nShips);
  if (bk !== null) return bk;

  const { slotToColor, colorToSlot } = getBindings(revealed);
  const solverRevealed = toSolverReveals(revealed, colorToSlot);
  const { probs: slotProbs } = maybeExactProbs(solverRevealed, ships);

  // EV value table — bound slots use real color values, unbound use mean HV value
  const unboundV = expectedHVValue(revealed);
  const vals     = Object.assign({}, VALUES);
  for (const [slot, col] of Object.entries(slotToColor)) vals[slot] = VALUES[col];
  for (const [shipName] of ships) {
    if (isSlotName(shipName) && !(shipName in slotToColor)) vals[shipName] = unboundV;
  }

  const { cb, cs, cand } = classify(slotProbs, solverRevealed);

  // Everything determined → harvest
  if (cand.length === 0) return harvestPick(cs, cb, slotProbs, vals);

  // Fishing window closed AND certain ship available → take it
  if (n >= 5 && cs.length > 0) return harvestPick(cs, cb, slotProbs, vals);

  // Danger zone: next blue ends the game → min P(blue)
  if (b + 1 >= 4 && n >= 5) return pickBest(cand, c => -(slotProbs[c][BLUE] || 0));

  // Fishing mode
  const shouldFish = (nShips === 5 && n < 5)
                  || (nShips === 6 && b >= 2 && n <= 3);
  if (shouldFish) {
    return pickBest(cand, c => cellEV(slotProbs[c], vals) + LAMBDA_FISH * (slotProbs[c][BLUE] || 0));
  }

  // Default: max EV + Gini tiebreaker
  return pickBest(cand, c => cellEV(slotProbs[c], vals) + LAMBDA_GINI * giniImpurity(slotProbs[c]));
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------
class HeuristicOTStrategy extends OTStrategy {

  /**
   * initEvaluationRun — nothing to precompute (all data is module-level).
   */
  initEvaluationRun() {
    return null;
  }

  /**
   * initGamePayload — stateless: we rebuild everything from `revealed` each
   * turn, so there is no per-game state to initialise.
   */
  initGamePayload(meta, evaluationRunState) {
    return null;
  }

  /**
   * nextClick — choose the best cell to click.
   *
   * @param {Array<{row,col,color}>} revealed  All cells revealed so far.
   * @param {{n_colors,ships_hit,blues_used,max_clicks}} meta
   * @param {{revealed:{},b:number,n:number}} gameState
   * @returns {{ row, col, gameState }}
   */
  nextClick(board, meta, gameState) {
    const revealedSet = new Set(board.filter(c => c.clicked).map(c => c.row * 5 + c.col));

    // Rebuild the revealed dict from clicked cells
    const revDict = {};
    for (const cell of board.filter(c => c.clicked)) {
      const idx   = cell.row * 5 + cell.col;
      const color = HARNESS_TO_COLOR[cell.color];
      if (color !== undefined) revDict[String(idx)] = color;
    }

    const b = meta.blues_used;
    const n = meta.ships_hit;
    const nShips = meta.n_colors - 1;  // 6-color = 5 ships, etc.

    // 9-color (8 ships) is not in SHIPS_BY_COUNT — random fallback
    if (!SHIPS_BY_COUNT[nShips]) {
      for (let r = 0; r < 5; r++)
        for (let c = 0; c < 5; c++)
          if (!revealedSet.has(r * 5 + c))
            return { row: r, col: c, gameState: null };
      return { row: 0, col: 0, gameState: null };
    }

    const move = getBestMove(revDict, b, n, nShips);

    if (move !== null && move >= 0 && !revealedSet.has(move)) {
      return { row: Math.floor(move / 5), col: move % 5, gameState: null };
    }

    // Fallback: first unrevealed cell
    for (let r = 0; r < 5; r++)
      for (let c = 0; c < 5; c++)
        if (!revealedSet.has(r * 5 + c))
          return { row: r, col: c, gameState: null };

    return { row: 0, col: 0, gameState: null };
  }
}

register(new HeuristicOTStrategy());
