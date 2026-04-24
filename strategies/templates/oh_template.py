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

BOARD CELL FORMAT
-----------------
board is always a list of exactly 25 dicts, one per cell:

    [{"row": int, "col": int, "color": str, "clicked": bool}, ...]

Row and col are 0-indexed (0..4).  color="spU" = covered/unknown.
clicked=False = still interactable; clicked=True = disabled.

META KEYS (oh)
--------------
clicks_left  int   remaining click budget (starts at 5)
max_clicks   int   total click budget (always 5)
game_seed    int   per-game deterministic seed; use to seed your own RNG
                   if you want reproducible results across harness runs

STATE MODEL
-----------
State lives inside the bridge — it is never serialised back to the harness.

  init_evaluation_run()          → run state (once before all games)
  init_game_payload(meta, rs)    → game state (reset before each game)
  next_click(board, meta, gs)    → (row, col, new_gs)

init_evaluation_run():  return read-only data shared across all games.
init_game_payload():    return fresh per-game state; the bridge resets it
                        before every game by calling this.
next_click():           receive current game state, return updated state.

If your strategy is stateless, omit all three optional methods and return
`game_state` unchanged from next_click().

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

        The returned value is stored by the bridge and passed as ``evaluation_run_state``
        to every init_game_payload() call.  Treat it as read-only.

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
        board: list[dict[str, Any]],
        meta: dict[str, Any],
        game_state: Any,
    ) -> tuple[int, int, Any]:
        """Choose the next cell to click.

        Args:
            board: all 25 board cells, each as
                   {"row": int, "col": int, "color": str, "clicked": bool}.
                   color="spU" = covered/unknown.  clicked=False = interactable.
                   10 cells start with their real color visible (clicked=False);
                   15 start as (color="spU", clicked=False).
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
            - Purple cells (spP) with clicked=False are free; click them first.
            - Blue (spB) and teal (spT) with clicked=False reveal more cells.
            - Dark (spD) cells transform when clicked; average value ~104 SP.
            - Do not return a (row, col) where board[row*5+col]["clicked"] is True.
        """
        clicked = {(c["row"], c["col"]) for c in board if c["clicked"]}

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
