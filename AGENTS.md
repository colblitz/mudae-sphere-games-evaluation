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
init_evaluation_run()
    Called once before all games.
    Use it to precompute board-independent data (lookup tables, opening books, etc.).
    The returned value is stored by the bridge as `run_state` and passed read-only
    to every init_game_payload call.  Never sent back to the harness again.

init_game_payload(meta, run_state)
    Called once before each game to reset per-game state.
    The returned value becomes `game_state` for that game's first next_click call.
    The bridge resets game state by calling this before every game — you do not need
    to manually clear anything between games.

next_click(board, meta, game_state)     → (row, col, new_game_state)
    Called on every click decision.
    `board` is the full 25-cell grid with all cells revealed so far this game.
    Return the cell to click and the updated game state for the next call.
```

**State model:**

```
Bridge owns two slots (never serialised to the harness):

  run_state   ← set once by init_evaluation_run().  Read-only.
  game_state  ← reset by init_game_payload() before each game.
                Updated by each next_click() return.

init_evaluation_run()
        │  (stored as run_state)
        ▼
init_game_payload(meta, run_state)   ← bridge supplies run_state from memory
        │  (stored as game_state)
        ▼
next_click(board, meta, game_state)  ──►  (row, col, new_game_state)
next_click(board, meta, new_game_state)  ──►  (row, col, ...)
        ...
        [next game: bridge calls init_game_payload again → game_state reset]
```

If your strategy is stateless, just return the same `game_state` unchanged and ignore `init_evaluation_run` / `init_game_payload`.

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
- `nextClick` returns a **plain object**: `return { row, col }` or `return { row, col, gameState }`  
  (`gameState` is optional; if present it is stored by the bridge for the next call)
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

- **Wrong return shape in JS.** `nextClick` must return `{ row, col }` or `{ row, col, gameState }`. Returning `{ row, col, state }` (wrong key name) will cause the bridge to silently drop your state.
- **Wrong return shape in Python.** `next_click` must return a 3-tuple `(row, col, state)`. Returning just `(row, col)` will cause an unpack error.
- **C++: returning a pointer to a local variable.** The `strategy_next_click` export must return a pointer that remains valid after the function returns. Use `thread_local static std::string buf` as shown in the templates.
- **C++: forgetting all five `extern "C"` exports.** The harness `dlopen`s your `.so` and resolves all five symbols. Missing any one will abort at load time.

**Structural errors:**

- **Not resetting per-game state in `init_game_payload`.** The bridge calls `init_game_payload` before every game to reset game state. For Python/JS, per-game data should be returned from `init_game_payload` (not carried on `self`/`this`), or explicitly reset there. For C++, reset per-game member variables inside `init_game_payload`. Failure to reset will leak state across games and produce wrong results (while appearing to work on game 1).
- **Treating `init_evaluation_run` output as mutable.** `run_state` is shared read-only across all games. Mutating it inside `next_click` will corrupt subsequent games. Per-game bookkeeping goes in `game_state` (returned from `init_game_payload`).

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
    // Store on `this` — or return it to receive it as evaluationRunState
    // in initGamePayload.  Both approaches work; `this` avoids any copying.
    this._lut = loadLut(filePath);
  }
}
```

### C++

```cpp
#include "../../interface/data.h"

void init_evaluation_run() override {
    std::string path = sphere::data::fetch(
        "https://...", "<hex sha256>", "oh_harvest_lut.bin.lzma"
    );
    load_lut(path);  // store result in a member variable
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
- **Mutating run-state data.** `run_state` is shared read-only across all games; treat it as immutable after `init_evaluation_run` returns. Load the file once and store the result in a way you won't modify it during play.
- **Forgetting to add a comment block.** Put a comment near the top of your strategy file documenting the external dependency (filename, size, URL) so contributors know what to expect before running.

See `strategies/oh/load_data.py` (and `.cpp`, `.js`) for a working end-to-end example.
