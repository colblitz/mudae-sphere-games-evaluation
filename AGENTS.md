# AGENTS.md — Guide for AI-Assisted Strategy Contributions

This repo is a competitive evaluation framework for four Mudae `/sphere` mini-games. You write a strategy (an algorithm that picks which cell to click next), run it through the evaluator, and your score lands on a leaderboard. **Read the README** for game rules, the full strategy interface spec, and the submission workflow. This file covers only what AI agents tend to get wrong when porting strategies into this codebase.

---

## What to Touch

| Path | Action |
|------|--------|
| `strategies/templates/<game>_template.<ext>` | Copy this to start a new strategy |
| `strategies/<game>/<your_strategy>.<ext>` | Where your strategy file lives |
| `interface/strategy.{py,h,js}` | Read-only — defines the ABCs/base classes |

Do **not** modify anything in `harness/`, `boards/`, `scripts/`, or `leaderboards/`. Do not modify `README.md` directly — `scripts/evaluate.py --commit` owns it.

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
Makes two automatic git commits: one for the strategy source file, one for the leaderboard and README update. The leaderboard entry records the exact commit hash of the strategy that produced the score.

See the README for the full list of `evaluate.py` flags (`--games`, `--seed`, `--n-colors`, `--threads`, etc.).

---

## Naming Conventions

- **File:** `strategies/<game>/<descriptive_approach>.<ext>` — name it after what the strategy does, not who wrote it (e.g. `entropy_reduction.py`, `corner_first.js`, not `alice_strategy.py`).
- **Class:** name it after the approach, not a generic placeholder (e.g. `EntropyOCStrategy`, not `MyOCStrategy`).
- All three language implementations of the same strategy should share the same base filename (`my_approach.py`, `my_approach.cpp`, `my_approach.js`).
