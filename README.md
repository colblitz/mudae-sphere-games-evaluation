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
   - **Commit 2** — `scores: oc my_strategy.py ev=78.43` — runs evaluation, writes a scores artifact to `scores/oc/<timestamp>_<commit>_<basename>.json`, and commits it. If the result enters the top 5, `leaderboards/oc.json` and `README.md` are also updated and included in the same commit.

   The scores artifact records the timestamp, strategy commit hash, filename, all harness stats, and run parameters. It is always written on `--commit` regardless of leaderboard placement.

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

The top 5 strategies per game, ranked by expected value (EV). Updated automatically by `scripts/evaluate.py --commit`.

<!-- LEADERBOARD_START -->
### /sphere harvest (oh)

| Rank | Strategy | EV | Stdev | OC Rate | Commit | Date |
|------|----------|----|-------|---------|--------|------|
| 1 | `random_clicks.py` | 117.63 | 224.46 | 4.1% | `e5b8664` | 2026-04-21 |

### /sphere chest (oc)

| Rank | Strategy | EV | Stdev | Red Rate | Commit | Date |
|------|----------|----|-------|-------|--------|------|
| 1 | `kelinimo_adaptive_ev.js` | 328.48 | 58.10 | 99.9% | `d1b0cd9` | 2026-04-23 |
| 2 | `global_state.cpp` | 64.50 | 53.10 | 11.1% | `312108f` | 2026-04-21 |

### /sphere quest (oq)

| Rank | Strategy | EV | Stdev | Red Rate | Commit | Date |
|------|----------|----|-------|-------|--------|------|
| 1 | `stateful.js` | 13.67 | 7.52 | 0.0% | `575db95` | 2026-04-21 |

### /sphere trace (ot)

**Aggregate (board-count weighted EV across all variants)**

| Rank | Strategy | Agg EV | Commit | Date |
|------|----------|--------|--------|------|
| 1 | `random_clicks.cpp` | 688.37 | `9cf69e2` | 2026-04-21 |

<details>
<summary>Per-color variant breakdown</summary>

**6-color variant**

| Rank | Strategy | EV | Stdev EV | Perfect% | All Ships% | 50/50 Loss% | Avg Clicks | Stdev Clicks | Avg Ship Clicks | Stdev Ship Clicks | Commit | Date |
|------|----------|----|----------|----------|------------|-------------|------------|--------------|-----------------|-------------------|--------|------|
| 1 | `random_clicks.cpp` | 270.39 | — | 0.0% | 16.0% | 95.7% | 10.69 | — | — | — | `9cf69e2` | 2026-04-21 |

**7-color variant**

| Rank | Strategy | EV | Stdev EV | Perfect% | All Ships% | 50/50 Loss% | Avg Clicks | Stdev Clicks | Avg Ship Clicks | Stdev Ship Clicks | Commit | Date |
|------|----------|----|----------|----------|------------|-------------|------------|--------------|-----------------|-------------------|--------|------|
| 1 | `random_clicks.cpp` | 349.70 | — | 0.0% | 11.8% | 84.8% | 10.75 | — | — | — | `9cf69e2` | 2026-04-21 |

**8-color variant**

| Rank | Strategy | EV | Stdev EV | Perfect% | All Ships% | 50/50 Loss% | Avg Clicks | Stdev Clicks | Avg Ship Clicks | Stdev Ship Clicks | Commit | Date |
|------|----------|----|----------|----------|------------|-------------|------------|--------------|-----------------|-------------------|--------|------|
| 1 | `random_clicks.cpp` | 500.88 | — | 0.0% | 14.0% | 44.3% | 11.74 | — | — | — | `9cf69e2` | 2026-04-21 |

**9-color variant**

| Rank | Strategy | EV | Stdev EV | Perfect% | All Ships% | 50/50 Loss% | Avg Clicks | Stdev Clicks | Avg Ship Clicks | Stdev Ship Clicks | Commit | Date |
|------|----------|----|----------|----------|------------|-------------|------------|--------------|-----------------|-------------------|--------|------|
| 1 | `random_clicks.cpp` | 1069.13 | — | 0.1% | 20.9% | 9.9% | 13.86 | — | — | — | `9cf69e2` | 2026-04-21 |

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

`interface/data` provides a `fetch()` function in all three languages. Call it in `init_evaluation_run` (or `initEvaluationRun` for JS/C++). The file is downloaded once to `data/`, its SHA-256 is verified, and it is reused on every subsequent run. If the file is already present with a matching hash, no download occurs.

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
const { fetch: fetchData } = require("../../interface/data.js");

class MyOHStrategy extends OHStrategy {
  async initEvaluationRun() {
    const filePath = await fetchData({
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
