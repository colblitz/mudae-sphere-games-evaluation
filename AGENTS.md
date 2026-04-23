# AGENTS.md — Guide for AI-Assisted Strategy Contributions

This repo is a competitive evaluation framework for four Mudae `/sphere` mini-games. You write a strategy (an algorithm that picks which cell to click next), run it through the evaluator, and your score lands on a leaderboard. **Read the README** for game rules, the full strategy interface spec, and the submission workflow. This file covers only what AI agents tend to get wrong when porting strategies into this codebase.

---

## What to Touch

| Path | Action |
|------|--------|
| `strategies/templates/<game>_template.<ext>` | Copy this to start a new strategy |
| `strategies/<game>/<your_strategy>.<ext>` | Where your strategy file lives |
| `interface/strategy.{py,h,js}` | Read-only — defines the ABCs/base classes |
| `interface/data.{py,h,js}` | Read-only — external data file helper |
| `data/<small_file>` | Commit small data files here with `git add -f` |

Do **not** modify `boards/`, `leaderboards/`, `scores/`, or `README.md` (the last is owned by `scripts/evaluate.py --commit`). Contributors shouldn't normally need to edit `harness/` or `scripts/` — if you think you've found a bug there, flag it rather than patching it unilaterally.

---

## The Interface in 30 Seconds

Every strategy implements three methods. Only `next_click` is required.

```
init_evaluation_run()                        → run_state
    Called once before all games.
    Use it to precompute board-independent data (lookup tables, opening books, etc.).
    Whatever you return becomes `run_state` in the next call.

init_game_payload(meta, run_state)           → game_state
    Called once before each game.
    Use it to reset per-game bookkeeping.
    Whatever you return becomes the first `state` in next_click.

next_click(revealed, meta, state)            → (row, col, new_state)
    Called on every click decision.
    `revealed` is the full list of all cells revealed so far this game (grows each turn).
    Whatever you return as `new_state` is passed back as `state` on the next call.
```

**State threading diagram:**

```
init_evaluation_run()
        │ run_state
        ▼
init_game_payload(meta, run_state)
        │ game_state
        ▼
next_click(revealed, meta, game_state)  ──► (row, col, state_1)
next_click(revealed, meta, state_1)     ──► (row, col, state_2)
next_click(revealed, meta, state_2)     ──► (row, col, state_3)
        ...
        [next game: init_game_payload called again]
```

If your strategy is stateless, just pass `state` through unchanged and ignore `init_evaluation_run` / `init_game_payload`.

---

## Language-Specific Checklists

### Python

- Import from `interface.strategy`, not a relative path: `from interface.strategy import OCStrategy`
- Method names are **snake_case**: `init_evaluation_run`, `init_game_payload`, `next_click`
- `next_click` returns a **3-tuple**: `return row, col, state`
- Place your file in `strategies/<game>/`, not in `interface/` or `strategies/templates/`

### JavaScript

- Require from `../../interface/strategy.js` (two levels up from `strategies/<game>/`)
- Method names are **camelCase**: `initEvaluationRun`, `initGamePayload`, `nextClick`
- `nextClick` returns a **plain object**: `return { row, col, gameState }`  
  (key is `gameState`, not `state`)
- **Call `register(new MyStrategy())` at the bottom of the file** — without this the process exits silently and the harness gets no output

### C++

C++ strategies require significant boilerplate (extern "C" exports, JSON parsing, static return buffers). Copy the appropriate template from `strategies/templates/` and implement only the `next_click` method body. Do not hand-write the exports from scratch.

---

## Common Mistakes

**Logic errors:**

- **Returning an already-revealed cell.** Build a set of `(row, col)` from `revealed` and exclude those. The harness behavior on duplicate clicks is not guaranteed.
- **Misreading `revealed`.** It contains *all* cells revealed so far this game, not just new ones from the last click. If you want only the delta, diff against your previous state.
- **Purple clicks are free in `oh` and `oq`.** A purple cell does not decrement `clicks_left`. If you see a purple, click it — it costs nothing. Do not skip it to "save" a click.
- **Treating `init_evaluation_run` output as per-game state.** `run_state` is shared across all games. Mutating it inside `next_click` will corrupt subsequent games. Per-game bookkeeping goes in `game_state` (returned from `init_game_payload`).
- **Assuming `game_seed` is always available in `meta`.** The `game_seed` key only exists for `oh`. Referencing it in `oc`, `oq`, or `ot` strategies will raise a `KeyError`/undefined.

**Interface errors:**

- **Wrong return shape in JS.** `nextClick` must return `{ row, col, gameState }`. Returning `{ row, col, state }` will cause the harness to lose your state silently.
- **Wrong return shape in Python.** `next_click` must return a 3-tuple `(row, col, state)`. Returning just `(row, col)` will cause an unpack error.
- **C++: returning a pointer to a local variable.** The `strategy_next_click` export must return a pointer that remains valid after the function returns. Use `static char buf[...]` or heap allocation.
- **C++: forgetting all five `extern "C"` exports.** The harness `dlopen`s your `.so` and resolves all five symbols. Missing any one will abort at load time.

**Structural errors:**

- **Putting state in instance variables instead of the state payload.** The harness creates one strategy instance for the whole run. If you store per-game data on `self` / `this` / member variables without resetting it in `init_game_payload`, it will leak across games and produce wrong results (while appearing to work on game 1).

---

## Running and Submitting

**Iterate freely — no side effects:**
```bash
python scripts/evaluate.py --game oc --strategy strategies/oc/my_strategy.py
```
Prints EV, stdev, and game-specific rates. No files are written or committed.

**Record a result when satisfied:**
```bash
python scripts/evaluate.py --game oc --strategy strategies/oc/my_strategy.py --commit
```
Makes two automatic git commits:
1. `strategy: oc my_strategy.py` — commits the strategy file (skipped if already committed and unmodified).
2. `scores: oc my_strategy.py ev=78.43` — writes a scores artifact to `scores/<game>/<timestamp>_<commit>_<basename>.json` and commits it. If the result enters the top 5, the leaderboard and README are updated and included in the same commit.

The scores artifact records the timestamp, strategy commit hash, filename, all harness stats, and run parameters. It is always written on `--commit` regardless of leaderboard placement.

See the README for the full list of `evaluate.py` flags (`--games`, `--seed`, `--n-colors`, `--threads`, etc.).

---

## Naming Conventions

- **File:** `strategies/<game>/<descriptive_approach>.<ext>` — name it after what the strategy does, not who wrote it (e.g. `entropy_reduction.py`, `corner_first.js`, not `alice_strategy.py`).
- **Class:** name it after the approach, not a generic placeholder (e.g. `EntropyOCStrategy`, not `MyOCStrategy`).
- All three language implementations of the same strategy should share the same base filename (`my_approach.py`, `my_approach.cpp`, `my_approach.js`).

---

## External Data Files

If your strategy needs a large precomputed file (lookup table, policy matrix, etc.):

- **≤ ~80 MB compressed:** commit it to `data/` with `git add -f data/<filename>`, then load it by path in `init_evaluation_run`.
- **Larger:** host it externally (Hugging Face Datasets recommended — free, no size limit, permanent URLs) and use `interface/data.fetch()` to auto-download on first use.

The `data/` directory is gitignored by default. Files added with `git add -f` are tracked normally once committed — no further special handling needed.

### Python

```python
from interface.data import fetch

# External data: oh_harvest_lut.bin.lzma
# Size: ~66 MB compressed / ~14 GB uncompressed
# Hosted at: https://huggingface.co/datasets/org/repo/resolve/main/oh_harvest_lut.bin.lzma
_LUT_URL    = "https://huggingface.co/datasets/org/repo/resolve/main/oh_harvest_lut.bin.lzma"
_LUT_SHA256 = "<hex sha256>"

class MyOHStrategy(OHStrategy):
    def init_evaluation_run(self):
        lut_path = fetch(url=_LUT_URL, sha256=_LUT_SHA256, filename="oh_harvest_lut.bin.lzma")
        return {"lut": load_lut(lut_path)}
```

### JavaScript

```js
const { fetch: fetchData } = require("../../interface/data.js");

class MyOHStrategy extends OHStrategy {
  async initEvaluationRun() {
    const filePath = await fetchData({
      url: "https://...",
      sha256: "<hex sha256>",
      filename: "oh_harvest_lut.bin.lzma",
    });
    return { lut: loadLut(filePath) };
  }
}
```

### C++

```cpp
#include "../../interface/data.h"

std::string init_evaluation_run() override {
    std::string path = sphere::data::fetch(
        "https://...", "<hex sha256>", "oh_harvest_lut.bin.lzma"
    );
    load_lut(path);  // store result in a member variable
    return "{}";
}
```

### Hosting options for large files

When a file is too large to commit (> ~80 MB compressed), host it externally
and pass the URL to `fetch()`:

| Host | Size limit | Cost | URL stability |
|------|-----------|------|--------------|
| [Hugging Face Datasets](https://huggingface.co/docs/datasets/) | None | Free | Permanent — **recommended** |
| [Google Drive](https://drive.google.com) | 15 GB free | Free tier | Use `https://drive.google.com/uc?export=download&id=<FILE_ID>`. Files > ~100 MB trigger a virus-scan interstitial that breaks `urllib`/`curl` — prefer Hugging Face for large files. |
| [Dropbox](https://www.dropbox.com) | 2 GB free | Free tier | Change `?dl=0` → `?dl=1` in the share link for a direct download URL. |
| GitHub (raw) | 100 MB per file | Free | `https://raw.githubusercontent.com/<org>/<repo>/main/data/<file>` — permanent once committed. |
| GitHub Releases | 2 GB per file | Free | URL tied to a release tag. |
| [Zenodo](https://zenodo.org) | 50 GB | Free | DOI-backed, permanent. |

**Common mistakes:**

- **Hard-coding an absolute path.** Always use `fetch()` or `DATA_DIR` from `interface/data` — they resolve `data/` relative to the repo root regardless of where `evaluate.py` is run from.
- **Storing downloaded data in `run_state` and mutating it.** `run_state` is shared across all games; treat it as read-only after `init_evaluation_run` returns. Load the file once and store the result immutably.
- **Forgetting to add a comment block.** Put a comment near the top of your strategy file documenting the external dependency (filename, size, URL) so contributors know what to expect before running.

See `strategies/oh/load_data.py` (and `.cpp`, `.js`) for a working end-to-end example.
