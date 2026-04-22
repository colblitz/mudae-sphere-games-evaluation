# Mudae Sphere Games — Strategy Evaluation Framework

A testing and evaluation framework for the four Mudae `/sphere` mini-games:
**harvest (oh)**, **chest (oc)**, **quest (oq)**, and **trace (ot)**.

Submit a strategy, run the evaluator, and see how it compares against others on the leaderboard.

---

## Leaderboards

The top 5 strategies per game, ranked by expected value (EV). Updated automatically by `scripts/evaluate.py --commit`.

<!-- LEADERBOARD_START -->
### /sphere harvest (oh)

| Rank | Strategy | EV | Stdev | OC Rate | Commit | Date |
|------|----------|----|-------|---------|--------|------|

### /sphere chest (oc)

| Rank | Strategy | EV | Stdev | Red Rate | Commit | Date |
|------|----------|----|-------|----------|--------|------|

### /sphere quest (oq)

| Rank | Strategy | EV | Stdev | Red Rate | Commit | Date |
|------|----------|----|-------|----------|--------|------|

### /sphere trace (ot)

**Aggregate (board-count weighted EV across all variants)**

| Rank | Strategy | Agg EV | Commit | Date |
|------|----------|--------|--------|------|

**6-color variant**

| Rank | Strategy | EV | Stdev | Perfect% | All Ships% | 50/50 Loss% | Avg Clicks | Commit | Date |
|------|----------|----|-------|----------|------------|-------------|------------|--------|------|

**7-color variant**

| Rank | Strategy | EV | Stdev | Perfect% | All Ships% | 50/50 Loss% | Avg Clicks | Commit | Date |
|------|----------|----|-------|----------|------------|-------------|------------|--------|------|

**8-color variant**

| Rank | Strategy | EV | Stdev | Perfect% | All Ships% | 50/50 Loss% | Avg Clicks | Commit | Date |
|------|----------|----|-------|----------|------------|-------------|------------|--------|------|

**9-color variant**

| Rank | Strategy | EV | Stdev | Perfect% | All Ships% | 50/50 Loss% | Avg Clicks | Commit | Date |
|------|----------|----|-------|----------|------------|-------------|------------|--------|------|

<!-- LEADERBOARD_END -->

---

## Game Rules

### /sphere harvest (oh)

- **Grid:** 5×5, 25 cells. **10 cells are revealed at the start of every game**; the other 15 start covered (`spU`). The 10 initial reveals are passed to your strategy before the first click decision.
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

- **Grid:** 5×5, all 25 cells start covered.
- **Non-purple click budget:** 7 clicks.
- **4 purple spheres (`spP`)** are hidden. Clicking a purple is **free**.
- **Non-purple cells** reveal the count of purple neighbors (0–4) as a color:
  `spB`=0, `spT`=1, `spG`=2, `spY`=3, `spO`=4 purple neighbors.
- **Goal:** click 3 purples → the 4th converts to red (`spR`, 150 SP, free click). Spend remaining budget greedily on highest-value derivable tiles.

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

---

## Strategy Interface

All strategies implement three methods:

```
init_payload()                              → initial state
init_run(meta, state)                       → updated state  [optional]
next_click(revealed, meta, state)           → (row, col, next_state)
```

- **`revealed`:** list of all cells revealed so far, each as `{row, col, color}`. Row and col are 0-indexed (0–4). Grows monotonically — every call includes all reveals since the start of the game.
- **`meta`:** game-specific metadata dict (see per-game docs below).
- **`state`:** whatever you returned from the previous `next_click` call (or `init_run`). Use `None`/`null`/`{}` if stateless.
- **Return:** `(row, col, next_state)` — the cell to click and your updated state.

The harness calls `init_payload()` once before the first game, then for each game calls `init_run()` once, then `next_click()` on every click decision. `init_run` and `init_payload` are optional — default implementations are provided.

### Per-game metadata keys

| Game | `meta` keys |
|------|-------------|
| oh | `clicks_left` (int), `max_clicks` (int, always 5) |
| oc | `clicks_left` (int), `max_clicks` (int, always 5) |
| oq | `clicks_left` (int), `max_clicks` (int, always 7), `purples_found` (int) |
| ot | `n_colors` (int), `ships_hit` (int), `blues_used` (int), `max_clicks` (int, always 4) |

### Python example

```python
# strategies/oc/my_strategy.py
import random
from interface.strategy import OCStrategy

class MyOCStrategy(OCStrategy):
    def next_click(self, revealed, meta, state):
        clicked = {(c["row"], c["col"]) for c in revealed}
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
    void next_click(const std::vector<Cell>& revealed,
                    const std::string& meta_json,
                    const std::string& state_json,
                    ClickResult& out) override {
        bool clicked[25] = {};
        for (const Cell& c : revealed) clicked[c.row * 5 + c.col] = true;
        for (int i = 0; i < 25; ++i) {
            if (!clicked[i]) { out.row = i/5; out.col = i%5; return; }
        }
    }
};

extern "C" sphere::StrategyBase* create_strategy() { return new MyOCStrategy(); }
extern "C" void destroy_strategy(sphere::StrategyBase* s) { delete s; }
extern "C" const char* strategy_init_payload(void*)                         { return "{}"; }
extern "C" const char* strategy_init_run(void*, const char*, const char* s) { return s; }
extern "C" const char* strategy_next_click(void* inst,
    const char* revealed_json, const char* meta_json, const char* state_json)
{
    // Parse revealed_json, call next_click, return JSON result
    // See strategies/oc/random_cpp.cpp for a complete example
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
  nextClick(revealed, meta, state) {
    const clicked = new Set(revealed.map(c => c.row * 5 + c.col));
    for (let r = 0; r < 5; r++)
      for (let c = 0; c < 5; c++)
        if (!clicked.has(r * 5 + c)) return { row: r, col: c, state };
    return { row: 0, col: 0, state };
  }
}

register(new MyOCStrategy());
```

> See the `strategies/` directory for complete working examples in all three languages.

---

## Submitting a Strategy

1. Create a file in `strategies/<game>/` (e.g. `strategies/oc/my_strategy.py`).
2. Subclass the appropriate ABC from `interface/strategy.py` (Python), inherit from the base class in `interface/strategy.h` (C++), or extend the class in `interface/strategy.js` (JS).
3. Implement `next_click`. Optionally implement `init_run` and `init_payload`.
4. Iterate freely — run the evaluator as many times as you like. Results are printed only; no files are changed.
   ```bash
   python scripts/evaluate.py --game oc --strategy strategies/oc/my_strategy.py
   ```
5. When you're satisfied with your strategy, record it with `--commit`:
   ```bash
   python scripts/evaluate.py --game oc --strategy strategies/oc/my_strategy.py --commit
   ```
   This makes **two commits** automatically:
   - **Commit 1** — `strategy: oc my_strategy.py` — commits the strategy file so it has a stable hash. Skipped if the file is already committed and unmodified.
   - **Commit 2** — `scores: oc my_strategy.py ev=78.43` — runs evaluation, always updates `README.md`, updates `leaderboards/oc.json` with the commit hash from step 1 if the score enters the top 5, and commits those artifacts.

   The score artifact always references the exact committed version of the strategy that produced it.

The evaluator builds the harness binary automatically if it is not already built or is out of date. For C++ strategies, it also compiles your `.cpp` to a `.so` automatically.

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

### evaluate.py flags

```
--game            oh | oc | oq | ot                      (required)
--strategy        path to strategy file                  (required)
--commit          two commits: strategy file + scoring artifacts
--games N         (oh) number of Monte Carlo games        default: 100000
--seed S          (oh) RNG seed                          default: 42
--n-colors X      (ot) 6|7|8|9|all                       default: all
--threads N       parallel threads                        default: all cores
--boards-dir      override boards directory
--no-leaderboard  (with --commit) skip leaderboard/README updates
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
