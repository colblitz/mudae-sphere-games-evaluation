# Mudae Sphere Games — Strategy Evaluation Framework

A testing and evaluation framework for the four Mudae `/sphere` mini-games:
**harvest (oh)**, **chest (oc)**, **quest (oq)**, and **trace (ot)**.

Submit a strategy, run the evaluator, and see how it compares against others on the leaderboard.

---

## Table of Contents

- [Submitting a Strategy](#submitting-a-strategy)
- [Strategy Interface](#strategy-interface)
- [Leaderboards](#leaderboards)
- [Building](#building)
- [Board Data](#board-data)
- [External Data Files](#external-data-files)
- [Game Rules](#game-rules)

---

## Submitting a Strategy

1. Create a file in `strategies/<game>/` (e.g. `strategies/oc/my_strategy.py`).
2. Subclass the appropriate ABC from `interface/strategy.py` (Python), inherit from the base class in `interface/strategy.h` (C++), or extend the class in `interface/strategy.js` (JS).
3. Implement `next_click`. Optionally implement `init_evaluation_run` and `init_game_payload`.
4. Iterate freely — run the evaluator as many times as you like. After each run, the script prints EV and stats then asks:
   ```bash
   python scripts/evaluate.py --game oc --strategy strategies/oc/my_strategy.py
   ```
   ```
   Commit this result? [y/N]
   ```
5. Answer `y` to record the result. This makes **two commits** automatically:
   - **Commit 1** — `strategy: oc my_strategy.py` — commits the strategy file so it has a stable hash. Skipped if the file is already committed and unmodified.
   - **Commit 2** — `scores: oc my_strategy.py ev=78.43` — writes a scores artifact to `scores/oc/<timestamp>_<commit>_<basename>.json` and commits it. If the result enters the top 5, `leaderboards/oc.json` and `README.md` are also updated and included in the same commit.

   The scores artifact records the timestamp, strategy commit hash, filename, all harness stats, and run parameters. It is always written when you commit regardless of leaderboard placement.

   Use `--yes` to skip the interactive prompt (e.g. in CI).

The evaluator builds the harness binary automatically if it is not already built or is out of date. For C++ strategies, it also compiles your `.cpp` to a `.so` automatically.

---

## Strategy Interface

All strategies implement three methods:

```
init_evaluation_run()                       → run_state       [optional]
init_game_payload(meta, run_state)          → game_state      [optional]
next_click(board, meta, state)              → (row, col, next_state)
```

- **`board`:** fixed array of exactly **25 cells**, one per grid position. Each cell is `{row, col, color, clicked}`:
  - `color` — Mudae emoji name (e.g. `"spR"`, `"spT"`). `"spU"` means the cell is covered/unknown.
  - `clicked` — `false` = cell is still interactable; `true` = cell has been clicked (disabled, do not re-click).
  - Common combinations: `{color:"spU", clicked:false}` = normal covered cell; `{color:"spR", clicked:false}` = passively revealed (e.g. oq 4th-purple auto-reveal); `{color:"spP", clicked:true}` = purple already clicked.
- **`meta`:** game-specific metadata dict (see per-game docs below).
- **`state`:** whatever you returned from the previous `next_click` call (or `init_game_payload`). Use `None`/`null`/`{}` if stateless.
- **Return:** `(row, col, next_state)` — the cell to click and your updated state. Do **not** return a cell where `board[row*5+col].clicked` is `true`.

The harness calls `init_evaluation_run()` once before all games begin (use it for expensive board-independent precomputation), then `init_game_payload()` once per game (use it to reset per-game state), then `next_click()` on every click decision. Both init methods are optional — default implementations are provided.

### Per-game metadata keys

| Game | `meta` keys |
|------|-------------|
| oh | `clicks_left` (int), `max_clicks` (int, always 5), `game_seed` (int) |
| oc | `clicks_left` (int), `max_clicks` (int, always 5) |
| oq | `clicks_left` (int), `max_clicks` (int, always 7), `purples_found` (int) |
| ot | `n_colors` (int), `ships_hit` (int), `blues_used` (int), `max_clicks` (int, always 4) |

### Python example

```python
# strategies/oc/my_strategy.py
import random
from interface.strategy import OCStrategy

class MyOCStrategy(OCStrategy):
    def next_click(self, board, meta, state):
        clicked = {(c["row"], c["col"]) for c in board if c["clicked"]}
        unclicked = [(r, c) for r in range(5) for c in range(5)
                     if (r, c) not in clicked]
        row, col = random.choice(unclicked)
        return row, col, state
```

### C++ example

```cpp
// strategies/oc/my_strategy.cpp
#include "../../interface/strategy.h"
using namespace sphere;

class MyOCStrategy : public OCStrategy {
public:
    void next_click(const std::vector<Cell>& board,
                    const std::string& meta_json,
                    const std::string& state_json,
                    ClickResult& out) override {
        for (const Cell& c : board) {
            if (!c.clicked) { out.row = c.row; out.col = c.col; return; }
        }
    }
};

extern "C" sphere::StrategyBase* create_strategy() { return new MyOCStrategy(); }
extern "C" void destroy_strategy(sphere::StrategyBase* s) { delete s; }
extern "C" const char* strategy_init_evaluation_run(void*)                                { return "{}"; }
extern "C" const char* strategy_init_game_payload(void*, const char*, const char* s)      { return s; }
extern "C" const char* strategy_next_click(void* inst,
    const char* board_json, const char* meta_json, const char* state_json)
{
    // Parse board_json, call next_click, return JSON result
    // See strategies/oc/global_state.cpp for a complete example
    static char buf[64];
    ClickResult out;
    // ... (parsing omitted for brevity)
    snprintf(buf, sizeof(buf), "{\"row\":%d,\"col\":%d,\"state\":{}}", out.row, out.col);
    return buf;
}
```

### JavaScript example

```js
// strategies/oc/my_strategy.js
const { OCStrategy, register } = require("../../interface/strategy.js");

class MyOCStrategy extends OCStrategy {
  nextClick(board, meta, gameState) {
    const clicked = new Set(board.filter(c => c.clicked).map(c => c.row * 5 + c.col));
    for (let r = 0; r < 5; r++)
      for (let c = 0; c < 5; c++)
        if (!clicked.has(r * 5 + c)) return { row: r, col: c, gameState };
    return { row: 0, col: 0, gameState };
  }
}

register(new MyOCStrategy());
```

> See the `strategies/` directory for complete working examples in all three languages.

---

## Leaderboards

The top 5 strategies per game, ranked by expected value (EV). Updated automatically when you commit a run via the post-run prompt.

<!-- LEADERBOARD_START -->
### /sphere harvest (oh)

| Rank | Strategy | EV | Stdev | OC Rate | Commit | Date |
|------|----------|----|-------|---------|--------|------|
| 1 | `colblitz_bellman.cpp` | 721.90 | 222.50 | 1.00% | `8f4e3b5` | 2026-04-25 |
| 2 | `colblitz_bellman.cpp` | 721.90 | 222.50 | 1.00% | `8f4e3b5` | 2026-04-25 |
| 3 | `random_clicks.py` | 117.63 | 224.46 | 4.09% | `e5b8664` | 2026-04-21 |

<details>
<summary>Performance</summary>

| Strategy | Games/CPU-s | Setup CPU-s | Harness wall-s | Threads | CPU |
|----------|-------------|-------------|----------------|---------|-----|
| `colblitz_bellman.cpp` | 18298 | — | 2.7 | 20 | 13th Gen Intel(R) Core(TM) i7-13700K |
| `colblitz_bellman.cpp` | 18514 | 0.02 | 2.7 | 20 | 13th Gen Intel(R) Core(TM) i7-13700K |
| `random_clicks.py` | — | — | — | — | — |

</details>

### /sphere chest (oc)

| Rank | Strategy | EV | Stdev | Red Rate | Commit | Date |
|------|----------|----|-------|-------|--------|------|
| 1 | `colblitz_bellman.cpp` | 344.73 | 57.97 | 99.98% | `8f4e3b5` | 2026-04-25 |
| 2 | `colblitz_bellman.cpp` | 344.73 | 57.97 | 99.98% | `8f4e3b5` | 2026-04-25 |
| 3 | `kelinimo_adaptive_ev.js` | 332.34 | 61.17 | 99.95% | `98cc571` | 2026-04-25 |
| 4 | `kelinimo_adaptive_ev.js` | 332.34 | 61.17 | 99.95% | `98cc571` | 2026-04-25 |
| 5 | `svessinn_solver.js` | 317.39 | 72.49 | 98.82% | `fb02619` | 2026-04-25 |

<details>
<summary>Performance</summary>

| Strategy | Games/CPU-s | Setup CPU-s | Harness wall-s | Threads | CPU |
|----------|-------------|-------------|----------------|---------|-----|
| `colblitz_bellman.cpp` | 87 | — | 9.7 | 20 | 13th Gen Intel(R) Core(TM) i7-13700K |
| `colblitz_bellman.cpp` | 225 | 5.95 | 9.7 | 20 | 13th Gen Intel(R) Core(TM) i7-13700K |
| `kelinimo_adaptive_ev.js` | 1266 | — | 0.7 | 20 | 13th Gen Intel(R) Core(TM) i7-13700K |
| `kelinimo_adaptive_ev.js` | 1463 | 0.04 | 0.6 | 20 | 13th Gen Intel(R) Core(TM) i7-13700K |
| `svessinn_solver.js` | 1376 | — | 0.6 | 20 | 13th Gen Intel(R) Core(TM) i7-13700K |

</details>

### /sphere quest (oq)

| Rank | Strategy | EV | Stdev | Red Rate | Commit | Date |
|------|----------|----|-------|-------|--------|------|
| 1 | `zavex_book_heuristic.js` | 350.86 | 44.04 | 98.96% | `da54712` | 2026-04-23 |
| 2 | `kelinimo_adaptive_ev.js` | 348.03 | 50.85 | 96.20% | `7ac88d8` | 2026-04-23 |
| 3 | `gap22_backtrack_solver.js` | 346.59 | 58.72 | 92.66% | `daf7d9b` | 2026-04-25 |
| 4 | `colblitz_mixed_gini.cpp` | 342.56 | 52.18 | 95.35% | `37b0f7f` | 2026-04-25 |
| 5 | `colblitz_mixed_gini.cpp` | 342.56 | 52.18 | 95.35% | `37b0f7f` | 2026-04-25 |

<details>
<summary>Performance</summary>

| Strategy | Games/CPU-s | Setup CPU-s | Harness wall-s | Threads | CPU |
|----------|-------------|-------------|----------------|---------|-----|
| `zavex_book_heuristic.js` | — | — | 3.1 | — | — |
| `kelinimo_adaptive_ev.js` | — | — | 13.4 | — | — |
| `gap22_backtrack_solver.js` | 262 | 0.04 | 2.5 | 20 | 13th Gen Intel(R) Core(TM) i7-13700K |
| `colblitz_mixed_gini.cpp` | 675 | — | 0.9 | 20 | 13th Gen Intel(R) Core(TM) i7-13700K |
| `colblitz_mixed_gini.cpp` | 678 | 0.00 | 0.9 | 20 | 13th Gen Intel(R) Core(TM) i7-13700K |

</details>

### /sphere trace (ot)

> **Note:** `kelinimo_expectimax_fast.cpp`, `svessinn_solver_fast.cpp`, and `zavex_heuristic_fast.cpp` are C++ ports of their original JS strategies with minor performance tweaks. They are not algorithmically identical to the JS originals — scores may differ slightly due to floating-point arithmetic differences and tiebreak ordering.

> **Note:** `zavex_heuristic_fast.cpp` does not appear in the aggregate leaderboard due to its low 9-color EV, but has competitive scores in the 6, 7, and 8-color variants.

**Aggregate (board-count weighted EV across all variants)**

| Rank | Strategy | Agg EV | Commit | Date |
|------|----------|--------|--------|------|
| 1 | `colblitz_v8_heuristics_stateless.cpp` | 1579.05 | `14d4ef4` | 2026-04-25 |
| 2 | `kelinimo_expectimax_fast.cpp` | 1513.58 | `c09e6cd` | 2026-04-25 |
| 3 | `tksglass_5.cpp` | 1318.03 | `25f4aef` | 2026-04-25 |
| 4 | `tksglass_4.cpp` | 1267.93 | `25f4aef` | 2026-04-25 |
| 5 | `svessinn_solver_fast.cpp` | 1259.21 | `c09e6cd` | 2026-04-25 |

<details>
<summary>Per-color variant breakdown</summary>

**6-color variant**

| Rank | Strategy | EV | Stdev EV | Perfect% | All Ships% | 50/50 Loss% | Avg Clicks | Stdev Clicks | Avg Ship Clicks | Stdev Ship Clicks | Commit | Date |
|------|----------|----|----------|----------|------------|-------------|------------|--------------|-----------------|-------------------|--------|------|
| 1 | `colblitz_v8_heuristics_stateless.cpp` | 768.62 | 184.65 | 59.02% | 91.84% | 33.26% | 21.39 | 0.00 | 13.03 | 0.00 | `14d4ef4` | 2026-04-25 |
| 2 | `zavex_heuristic_fast.cpp` | 751.17 | 197.86 | 53.27% | 94.75% | 37.10% | 21.21 | 0.00 | 12.60 | 0.00 | `c09e6cd` | 2026-04-25 |
| 3 | `kelinimo_expectimax_fast.cpp` | 696.37 | 193.69 | 33.88% | 69.72% | 50.73% | 17.33 | 0.00 | 11.78 | 0.00 | `c09e6cd` | 2026-04-25 |
| 4 | `tksglass_5.cpp` | 633.20 | 197.98 | 23.39% | 57.15% | 62.37% | 17.76 | 0.00 | 11.47 | 0.00 | `25f4aef` | 2026-04-25 |
| 5 | `svessinn_solver_fast.cpp` | 557.04 | 212.46 | 11.71% | 45.79% | 60.64% | 16.62 | 0.00 | 10.07 | 0.00 | `c09e6cd` | 2026-04-25 |

**7-color variant**

| Rank | Strategy | EV | Stdev EV | Perfect% | All Ships% | 50/50 Loss% | Avg Clicks | Stdev Clicks | Avg Ship Clicks | Stdev Ship Clicks | Commit | Date |
|------|----------|----|----------|----------|------------|-------------|------------|--------------|-----------------|-------------------|--------|------|
| 1 | `colblitz_v8_heuristics_stateless.cpp` | 949.44 | 269.94 | 43.09% | 77.80% | 44.36% | 20.17 | 0.00 | 14.50 | 0.00 | `14d4ef4` | 2026-04-25 |
| 2 | `zavex_heuristic_fast.cpp` | 892.43 | 277.54 | 33.62% | 66.40% | 47.75% | 18.87 | 0.00 | 13.85 | 0.00 | `c09e6cd` | 2026-04-25 |
| 3 | `kelinimo_expectimax_fast.cpp` | 880.71 | 283.57 | 27.38% | 64.87% | 53.10% | 17.81 | 0.00 | 13.15 | 0.00 | `c09e6cd` | 2026-04-25 |
| 4 | `tksglass_5.cpp` | 761.58 | 271.27 | 14.55% | 45.07% | 66.78% | 17.43 | 0.00 | 12.65 | 0.00 | `25f4aef` | 2026-04-25 |
| 5 | `tksglass_4.cpp` | 702.14 | 269.02 | 9.62% | 36.34% | 69.31% | 16.34 | 0.00 | 11.95 | 0.00 | `25f4aef` | 2026-04-25 |

**8-color variant**

| Rank | Strategy | EV | Stdev EV | Perfect% | All Ships% | 50/50 Loss% | Avg Clicks | Stdev Clicks | Avg Ship Clicks | Stdev Ship Clicks | Commit | Date |
|------|----------|----|----------|----------|------------|-------------|------------|--------------|-----------------|-------------------|--------|------|
| 1 | `colblitz_v8_heuristics_stateless.cpp` | 1341.55 | 357.51 | 53.21% | 87.12% | 37.22% | 21.36 | 0.00 | 16.90 | 0.00 | `14d4ef4` | 2026-04-25 |
| 2 | `kelinimo_expectimax_fast.cpp` | 1262.98 | 387.49 | 36.40% | 77.73% | 42.17% | 19.96 | 0.00 | 15.73 | 0.00 | `c09e6cd` | 2026-04-25 |
| 3 | `zavex_heuristic_fast.cpp` | 1258.96 | 394.50 | 39.78% | 81.81% | 39.92% | 20.03 | 0.00 | 15.98 | 0.00 | `c09e6cd` | 2026-04-25 |
| 4 | `tksglass_5.cpp` | 1100.47 | 387.49 | 19.75% | 59.70% | 54.98% | 19.42 | 0.00 | 15.02 | 0.00 | `25f4aef` | 2026-04-25 |
| 5 | `tksglass_4.cpp` | 1052.72 | 392.50 | 15.77% | 54.84% | 56.31% | 18.84 | 0.00 | 14.60 | 0.00 | `25f4aef` | 2026-04-25 |

**9-color variant**

| Rank | Strategy | EV | Stdev EV | Perfect% | All Ships% | 50/50 Loss% | Avg Clicks | Stdev Clicks | Avg Ship Clicks | Stdev Ship Clicks | Commit | Date |
|------|----------|----|----------|----------|------------|-------------|------------|--------------|-----------------|-------------------|--------|------|
| 1 | `colblitz_v8_heuristics_stateless.cpp` | 2179.95 | 150.42 | 79.11% | 98.76% | 17.61% | 23.74 | 0.00 | 19.68 | 0.00 | `14d4ef4` | 2026-04-25 |
| 2 | `kelinimo_expectimax_fast.cpp` | 2129.61 | 252.00 | 65.84% | 96.06% | 21.62% | 23.27 | 0.00 | 19.23 | 0.00 | `c09e6cd` | 2026-04-25 |
| 3 | `svessinn_solver_fast.cpp` | 1890.85 | 526.54 | 52.03% | 84.40% | 30.41% | 22.12 | 0.00 | 18.00 | 0.00 | `c09e6cd` | 2026-04-25 |
| 4 | `tksglass_5.cpp` | 1853.67 | 462.95 | 35.60% | 81.72% | 40.65% | 22.18 | 0.00 | 18.02 | 0.00 | `25f4aef` | 2026-04-25 |
| 5 | `tksglass_4.cpp` | 1809.17 | 492.41 | 31.92% | 79.38% | 41.91% | 21.86 | 0.00 | 17.77 | 0.00 | `25f4aef` | 2026-04-25 |

</details>

<details>
<summary>Performance</summary>

| Strategy | Games/CPU-s | Setup CPU-s | Harness wall-s | Threads | CPU |
|----------|-------------|-------------|----------------|---------|-----|
| `colblitz_v8_heuristics_stateless.cpp` | 1354 | 2.22 | 1170.5 | 20 | 13th Gen Intel(R) Core(TM) i7-13700K |
| `kelinimo_expectimax_fast.cpp` | 134 | 130.30 | 11953.8 | 20 | 13th Gen Intel(R) Core(TM) i7-13700K |
| `tksglass_5.cpp` | 1596 | 0.00 | 990.7 | 20 | 13th Gen Intel(R) Core(TM) i7-13700K |
| `tksglass_4.cpp` | 1796 | 0.00 | 880.2 | 20 | 13th Gen Intel(R) Core(TM) i7-13700K |
| `svessinn_solver_fast.cpp` | 2656 | 0.00 | 595.4 | 20 | 13th Gen Intel(R) Core(TM) i7-13700K |

</details>

<!-- LEADERBOARD_END -->

---

## Building

### Prerequisites

| Dependency | Version | Purpose |
|------------|---------|---------|
| `g++` | GCC 11+ | Compiling harness and C++ strategies |
| `liblzma-dev` | any | Board file decompression |
| Python | 3.11+ | Python strategy bridge (pybind11 via Python C API) |
| Python headers | matching Python version | `python3-dev` / `python3.11-dev` |
| Node.js | 16+ | JavaScript strategy bridge |
| OpenMP | optional | Parallel ot evaluation (`-fopenmp`) |

Install on Debian/Ubuntu:
```bash
sudo apt install g++ liblzma-dev python3.11-dev nodejs
```

### Makefile targets

```bash
make build-harness     # Build all four evaluate_* binaries
make build-oh          # Build only evaluate_oh
make build-oc          # Build only evaluate_oc
make build-oq          # Build only evaluate_oq
make build-ot          # Build only evaluate_ot
make generate-boards   # Rebuild board files from scratch
make clean             # Remove compiled binaries and strategy .so files
```

### compare_strategies.py

Compares two strategy implementations move-by-move to verify they produce identical decisions (useful when porting a strategy from JS to C++):

```bash
python scripts/compare_strategies.py \
    --game ot \
    --strategy-a strategies/ot/kelinimo_expectimax.js \
    --strategy-b strategies/ot/kelinimo_expectimax.cpp \
    --n-colors 6 --seed 42 --n 200
```

Runs both strategies in trace mode with the same seed and diffs move sequences game-by-game. Exits 0 if all games match, 1 if any mismatch. Use `--verbose` to print every game's moves, `--show-board` to print the board on mismatches.

### evaluate.py flags

```
--game            oh | oc | oq | ot                      (required)
--strategy        path to strategy file                  (required)
--games N         (oh) number of Monte Carlo games        default: 1000000
--seed S          RNG seed (evaluation and trace mode)   default: 42
--n-colors X      (ot) 6|7|8|9|all                       default: all
--threads N       parallel threads                        default: all cores
--boards-dir      override boards directory
--yes             auto-accept the post-run commit prompt (non-interactive)
--trace           print per-game boards and move sequences
--n N             (trace) number of games to sample       default: 20
```

---

## Board Data

Board files are committed to `boards/` as lzma-compressed binary. See `scripts/generate_boards.cpp` for the generation logic.

| File | Boards | Format |
|------|--------|--------|
| `oc_boards.bin.lzma` | 16,800 | 25 ASCII bytes per board (`R O Y G T B`) |
| `oq_boards.bin.lzma` | 12,650 | 4-byte little-endian `uint32` bitmask (bit `i` = cell `i` is purple) |
| `ot_boards_2.bin.lzma` | ~1.2M | `(3+2)×4` bytes: `[teal, green, yellow, spo, var0]` int32 masks |
| `ot_boards_3.bin.lzma` | ~5.7M | `(3+3)×4` bytes |
| `ot_boards_4.bin.lzma` | ~12.3M | `(3+4)×4` bytes |
| `ot_boards_5.bin.lzma` | ~12.4M | `(3+5)×4` bytes |

To generate human-readable text versions locally (not committed):
```bash
scripts/generate_boards --text
# Writes boards/oc_boards.txt, oq_boards.txt, ot_boards_N.txt
```

oh does not have a board file — boards are generated stochastically during evaluation using the appearance distribution in `boards/oh_dark_stats.json`.

---

## External Data Files

Some strategies require large precomputed files (lookup tables, policy matrices, etc.) that cannot reasonably be committed to the repository. These live in `data/` and are either committed directly (if small enough) or downloaded automatically by the strategy on first run.

### Directory layout

The `data/` directory is gitignored by default. Two patterns apply:

| Situation | Approach |
|-----------|----------|
| File ≤ ~80 MB compressed | Commit to `data/` with `git add -f data/<filename>` |
| File > ~80 MB compressed | Host externally; auto-download via `interface/data` helper |

Files added with `git add -f` are tracked normally once committed — no further `.gitignore` exceptions needed.

### Auto-download helper

`interface/data` provides a `fetch()` function in Python and C++, and both `fetch()` (async) and `fetchSync()` (synchronous) in JavaScript. Call it in `init_evaluation_run` / `initEvaluationRun`. For JavaScript, always use `fetchSync` — the protocol loop does not `await` `initEvaluationRun`. The file is downloaded once to `data/`, its SHA-256 is verified, and it is reused on every subsequent run. If the file is already present with a matching hash, no download occurs.

See `strategies/oh/load_data.py` (and `.cpp`, `.js`) for a complete working example.

**Python:**

```python
from interface.data import fetch

# External data: oh_example.json
# Size: < 1 KB
# Source: https://raw.githubusercontent.com/colblitz/mudae-sphere-games-evaluation/main/data/oh_example.json
_DATA_URL    = "https://raw.githubusercontent.com/..."
_DATA_SHA256 = "<hex sha256>"

class MyOHStrategy(OHStrategy):
    def init_evaluation_run(self):
        path = fetch(url=_DATA_URL, sha256=_DATA_SHA256, filename="my_lut.bin.lzma")
        return {"lut": load_lut(path)}
```

**JavaScript:**

```js
const { fetchSync } = require("../../interface/data.js");

class MyOHStrategy extends OHStrategy {
  initEvaluationRun() {
    // Use fetchSync (not async fetch) — the protocol loop does not await initEvaluationRun.
    const filePath = fetchSync({
      url: "https://...",
      sha256: "<hex sha256>",
      filename: "my_lut.bin.lzma",
    });
    return { lut: loadLut(filePath) };
  }
}
```

**C++:**

```cpp
#include "../../interface/data.h"

std::string init_evaluation_run() override {
    std::string path = sphere::data::fetch(
        "https://...", "<hex sha256>", "my_lut.bin.lzma"
    );
    load_lut(path);  // store result in a member variable
    return "{}";
}
```

### Hosting large files

| Host | Size limit | Cost | URL stability |
|------|-----------|------|--------------|
| [Hugging Face Datasets](https://huggingface.co/docs/datasets/) | None | Free | Permanent — recommended for large files |
| [Google Drive](https://drive.google.com) | 15 GB free | Free tier | Use `https://drive.google.com/uc?export=download&id=<FILE_ID>`. Files > ~100 MB trigger a virus-scan interstitial that breaks `urllib`/`curl` — prefer Hugging Face for large files. |
| [Dropbox](https://www.dropbox.com) | 2 GB free | Free tier | Change `?dl=0` → `?dl=1` in the share link for a direct download URL. |
| GitHub (raw) | 100 MB per file | Free | `https://raw.githubusercontent.com/<org>/<repo>/main/data/<file>` — permanent once committed. |
| GitHub Releases | 2 GB per file | Free | URL tied to a release tag. |
| [Zenodo](https://zenodo.org) | 50 GB | Free | DOI-backed, permanent. |

---

## Game Rules

### /sphere harvest (oh)

- **Grid:** 5×5, 25 cells. **10 cells start with their real color visible** (`clicked=false`); the other 15 start covered (`color="spU"`, `clicked=false`). All 25 cells are present in the board on the first `next_click` call.
- **Click budget:** 5 clicks.
- **Blue (`spB`):** reveals 3 random covered cells when clicked. Worth ~10 SP.
- **Teal (`spT`):** reveals 1 random covered cell. Worth ~20 SP.
- **Purple (`spP`):** click is **free** (does not consume a click). Worth ~5–12 SP.
- **Dark (`spD`):** transforms into another color when clicked (~104 SP average). If it transforms into purple, the click is also **refunded**.
- **Light (`spL`):** flat value ~76 SP average.
- **Covered (`spU`):** clicking a covered cell resolves it stochastically to a random color.
- **Chest boards (~50% of games):** one covered cell is a "chest" worth ~345 SP on average.
- **Goal:** maximize total SP across your 5 clicks.

**Evaluation:** 100,000 Monte Carlo games. Stats: `ev`, `stdev`, `oc_rate` (fraction of games with a chest cell).

### /sphere chest (oc)

- **Grid:** 5×5, all 25 cells start covered.
- **Click budget:** 5 clicks.
- **One red sphere (`spR`, 150 SP)** is hidden at a non-center position. Its location determines every other cell's color via fixed spatial zones:

  | Zone | Relation to red | Color | SP | Count |
  |------|----------------|-------|----|-------|
  | orth | orthogonally adjacent (`|dr|+|dc|==1`) | orange `spO` | 90 | 2 |
  | diag | diagonally adjacent (`|dr|==|dc|, dr≠0`) | yellow `spY` | 55 | 3 |
  | rowcol | same row/col, not orth/diag | green `spG` | 35 | 4 |
  | none | no geometric relation | blue `spB` | 10 | varies |
  | residual | orth/rowcol remainder | teal `spT` | 20 | varies |

- **Goal:** maximize SP across 5 clicks.

> **Note on board distribution:** All 16,800 valid boards are weighted equally in evaluation. In practice, Mudae may not use a uniform distribution. Equal weighting is used here for standardized, reproducible comparison.

**Evaluation:** exhaustive over all 16,800 boards. Stats: `ev`, `stdev`, `red_rate` (fraction of boards where red was clicked).

### /sphere quest (oq)

- **Grid:** 5×5, all 25 cells start covered (`color="spU"`, `clicked=false`).
- **Non-purple click budget:** 7 clicks.
- **4 purple spheres (`spP`)** are hidden. Clicking a purple is **free**.
- **Non-purple cells** reveal the count of purple neighbors (0–4) as a color:
  `spB`=0, `spT`=1, `spG`=2, `spY`=3, `spO`=4 purple neighbors.
- **Auto-reveal:** after clicking the 3rd purple, the harness automatically reveals the 4th purple as `spR` (`color="spR"`, `clicked=false`) in the board passed to the next `next_click` call. Clicking it costs 1 click and is worth 150 SP.
- **Goal:** click 3 purples → click the auto-revealed red (`spR`, 150 SP). Spend remaining budget greedily on highest-value derivable tiles.

**Evaluation:** exhaustive over all C(25,4) = 12,650 boards. Stats: `ev`, `stdev`, `red_rate`.

### /sphere trace (ot)

- **Grid:** 5×5, all 25 cells start covered.
- **Blue click budget:** 4. Clicking a **ship cell is free**.
- Ships are contiguous horizontal or vertical segments, no overlap:

  | Variant | Ships | Blue cells | Ship cells |
  |---------|-------|------------|------------|
  | 6-color | teal(4) green(3) yellow(3) orange(2) light(2) | 11 | 14 |
  | 7-color | + dark(2) | 9 | 16 |
  | 8-color | + red(2) | 7 | 18 |
  | 9-color | + white(2) | 5 | 20 |

- **Extra Chance:** if you click blue #4 before hitting 5 ship cells, the game continues. Each additional blue while `ships_hit < 5` extends the game. After the 5th ship hit, Extra Chance shuts off — the next blue ends the game.
- **Perfect game:** find all blues via constraint inference, then collect all remaining ship cells for free.
- **Ship values:** `spT`=20, `spG`=35, `spY`=55, `spO`=90, `spL`=76, `spD`=104, `spR`=150, `spW`=500.

**Evaluation:** exhaustive over all boards per n_colors variant. Stats per variant and aggregated: `ev`, `stdev`, `avg_clicks`, `perfect_rate`, `all_ships_rate`, `loss_5050_rate` (fraction of games lost on a ~50/50 blue decision).
