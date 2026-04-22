"""Template strategy for /sphere harvest (oh) — Python.

Copy this file to strategies/oh/<your_name>.py, rename the class, and fill
in the TODO sections.  Delete any methods you don't need — only next_click()
is required.

GAME OVERVIEW
-------------
Grid     : 5×5
Revealed : 10 cells visible at game start; 15 start covered (spU)
Budget   : 5 clicks
Goal     : maximise total SP collected across your 5 clicks

Special click rules:
  - Purple (spP)    : click is FREE (does not consume a click)
  - Dark (spD)      : transforms into another color when clicked;
                      if it becomes purple the click is also free
  - Blue (spB)      : reveals 3 additional covered cells
  - Teal (spT)      : reveals 1 additional covered cell
  - ~50% of boards have one "chest" covered cell worth ~345 SP on average

COLOR REFERENCE
---------------
spB   blue    reveals 3 covered cells
spT   teal    reveals 1 covered cell
spG   green   35 SP
spY   yellow  55 SP
spL   light   ~76 SP average
spO   orange  90 SP
spR   red     150 SP
spW   white   ~300 SP
spP   purple  ~5–12 SP, click is FREE
spD   dark    ~104 SP average, transforms on click
spU   covered (unrevealed; directly clickable)

REVEALED CELL FORMAT
--------------------
revealed is a list of dicts, one per cell revealed so far (monotonically
growing — every call includes all cells revealed since game start):

    [{"row": int, "col": int, "color": str}, ...]

Row and col are 0-indexed (0..4).

META KEYS (oh)
--------------
clicks_left  int   remaining click budget (starts at 5)
max_clicks   int   total click budget (always 5)
game_seed    int   per-game deterministic seed; use to seed your own RNG
                   if you want reproducible results across harness runs

STATE PAYLOAD
-------------
game_state is any Python object you choose.  The harness threads it through every
call within a game:

  init_evaluation_run()        → initial_state        (called once before all games)
  init_game_payload(meta, s0)    → s1                   (called once per game)
  next_click(..., s1)   → (row, col, s2)
  next_click(..., s2)   → (row, col, s3)
  ...

Use init_evaluation_run() for data computed ONCE and shared across all games
(lookup tables, precomputed weights, etc.).

Use init_game_payload() to reset per-game bookkeeping at the start of each game.
The game_state returned by init_game_payload() is passed as `game_state` to the first
next_click() call of that game.

If your strategy is stateless, leave init_evaluation_run() and init_game_payload() out and
return `game_state` unchanged from next_click().

See also
--------
oh/load_data.py       — init_evaluation_run() loading a data file via interface.data.fetch()
oc/global_state.py    — init_evaluation_run() (global state) example
oq/stateful.py        — init_game_payload() + state threading (per-game state) example
"""

import random
from pathlib import Path
from typing import Any

from interface.strategy import OHStrategy

# If your strategy needs a large precomputed file (lookup table, policy matrix,
# etc.), use interface.data.fetch() in init_evaluation_run() to download and
# cache it automatically.  Small files (≤ ~80 MB compressed) can be committed
# directly to data/ and loaded by path instead:
#
#   DATA_DIR = Path(__file__).resolve().parent.parent.parent / "data"
#
# Uncomment and adapt the fetch example below if you need a large file:
#
#   from interface.data import fetch
#
#   # External data: <filename>
#   # Size: ~X MB compressed / ~Y GB uncompressed
#   # Hosted at: <url>
#   _LUT_URL    = "https://huggingface.co/datasets/org/repo/resolve/main/<filename>"
#   _LUT_SHA256 = "<hex sha256>"
#   _LUT_FILE   = "<filename>"


class MyOHStrategy(OHStrategy):
    # -----------------------------------------------------------------------
    # Optional: global state (computed once, shared across ALL games)
    # -----------------------------------------------------------------------

    def init_evaluation_run(self) -> Any:
        """Called once before the evaluation run begins.

        Compute anything that is board-independent and expensive to repeat:
        lookup tables, precomputed orderings, loaded model weights, etc.

        The returned value is passed as ``evaluation_run_state`` to init_game_payload() at the start
        of every game.  Treat it as read-only during play.

        Returns:
            Any Python object.  Default: None.

        Example (large external file via auto-download):
            lut_path = fetch(url=_LUT_URL, sha256=_LUT_SHA256, filename=_LUT_FILE)
            lut = load_lut(lut_path)   # your own loader
            return {"lut": lut}

        Example (small committed file in data/):
            lut_path = DATA_DIR / "oh_harvest_lut.bin.lzma"
            lut = load_lut(lut_path)
            return {"lut": lut}
        """
        # TODO: replace with your global precomputation, or delete this method
        return None

    # -----------------------------------------------------------------------
    # Optional: per-game initialisation
    # -----------------------------------------------------------------------

    def init_game_payload(
        self,
        meta: dict[str, Any],
        evaluation_run_state: Any,
    ) -> Any:
        """Called once before each game's first click. Set up fresh per-game state.

        Args:
            meta:  game metadata (same keys as next_click).
            evaluation_run_state: read-only value from init_evaluation_run().
                         Do not mutate — sharing state between games is unfair.

        Returns:
            The initial game_state for this game's first next_click() call.
            Typically a fresh dict with per-game bookkeeping fields.

        Example:
            return {**evaluation_run_state, "clicks_made": 0, "seen_colors": []}
        """
        # TODO: reset per-game fields here, or delete this method
        return evaluation_run_state

    # -----------------------------------------------------------------------
    # Required: click decision
    # -----------------------------------------------------------------------

    def next_click(
        self,
        revealed: list[dict[str, Any]],
        meta: dict[str, Any],
        game_state: Any,
    ) -> tuple[int, int, Any]:
        """Choose the next cell to click.

        Args:
            revealed: all cells revealed so far this game, each as
                      {"row": int, "col": int, "color": str}.
                      Includes cells visible from game start plus all
                      cells uncovered by previous clicks.
            meta: {
                "clicks_left": int,   # remaining budget (may be > original
                                      # if purples/darks gave free clicks)
                "max_clicks":  int,   # total budget (always 5)
                "game_seed":   int,   # per-game seed for reproducibility
            }
            game_state: value returned by the previous next_click() call, or by
                   init_game_payload() for the first call of the game.

        Returns:
            (row, col, game_state)
            row, col    : 0-indexed coordinates of the cell to click next.
            game_state  : updated state to pass into the next call.
                          Return `game_state` unchanged if nothing needs updating.

        Tips:
            - Purple cells (spP) are free; click them first if visible.
            - Blue (spB) and teal (spT) cells reveal more cells, giving more
              information before spending remaining clicks.
            - Dark (spD) cells transform when clicked — you don't know the
              outcome in advance, but their average value is ~104 SP.
            - All cells in `revealed` have already been interacted with;
              do not return a (row, col) that is already in revealed.
        """
        clicked = {(c["row"], c["col"]) for c in revealed}

        # TODO: replace with your click logic

        # Minimal fallback: click a random unclicked cell
        unclicked = [
            (r, c)
            for r in range(5)
            for c in range(5)
            if (r, c) not in clicked
        ]
        if not unclicked:
            return 0, 0, game_state
        row, col = random.choice(unclicked)
        return row, col, game_state
