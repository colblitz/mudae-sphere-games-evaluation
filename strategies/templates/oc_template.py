"""Template strategy for /sphere chest (oc) — Python.

Copy this file to strategies/oc/<your_name>.py, rename the class, and fill
in the TODO sections.  Delete any methods you don't need — only next_click()
is required.

GAME OVERVIEW
-------------
Grid   : 5×5, all 25 cells start covered
Budget : 5 clicks
Goal   : maximise SP collected across 5 clicks

One red sphere (spR, 150 SP) is hidden.  Its position determines every other
cell's color via fixed spatial zones relative to red (dr = row delta from
red, dc = col delta from red):

  |dr|+|dc| == 1            → orange  (spO, 90 SP)   [2 cells]
  |dr| == |dc|, dr != 0     → yellow  (spY, 55 SP)   [3 cells]
  same row or col, not above → green   (spG, 35 SP)   [4 cells]
  residual orth/rowcol       → teal    (spT, 20 SP)
  no geometric relation      → blue    (spB, 10 SP)

The center cell (2,2) can never be red.
Evaluation uses all 16,800 valid boards weighted equally.

COLOR REFERENCE (oc)
--------------------
spR   red     150 SP
spO   orange   90 SP
spY   yellow   55 SP
spG   green    35 SP
spT   teal     20 SP
spB   blue     10 SP

BOARD CELL FORMAT
-----------------
board is always a list of exactly 25 dicts, one per cell:

    [{"row": int, "col": int, "color": str, "clicked": bool}, ...]

Row and col are 0-indexed (0..4).  color="spU" = covered/unknown.
clicked=False = still interactable; clicked=True = disabled.

META KEYS (oc)
--------------
clicks_left  int   remaining click budget
max_clicks   int   total click budget (always 5)

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
oh/random_clicks.py   — minimal stateless example
oc/global_state.py    — init_evaluation_run() (global state) example
oq/stateful.py        — init_game_payload() + state threading (per-game state) example
"""

import random
from typing import Any

from interface.strategy import OCStrategy


class MyOCStrategy(OCStrategy):
    # -----------------------------------------------------------------------
    # Optional: global state (computed once, shared across ALL games)
    # -----------------------------------------------------------------------

    def init_evaluation_run(self) -> Any:
        """Called once before the evaluation run begins.

        Compute anything that is board-independent and expensive to repeat.
        The returned value is stored by the bridge and passed as ``evaluation_run_state``
        to every init_game_payload() call.  Treat it as read-only.

        Returns:
            Any Python object.  Default: None.

        Example: pre-rank all 25 cells by expected SP under a uniform red
        prior, so next_click() can do a fast O(25) scan instead of
        recomputing probabilities from scratch each call.
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

        Returns:
            The initial game_state for this game's first next_click() call.

        Example: merge the global lookup table with a fresh per-game
        posterior over possible red positions, then update the posterior
        as each color is revealed.
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
            meta: {
                "clicks_left": int,   # remaining budget
                "max_clicks":  int,   # total budget (always 5)
            }
            game_state: value returned by the previous next_click() call, or by
                   init_game_payload() for the first call of the game.

        Returns:
            (row, col, game_state)
            row, col    : 0-indexed coordinates of the cell to click next.
            game_state  : updated state to pass into the next call.

        Tips:
            - Each clicked color constrains where the red sphere can be.
            - Once red is found, the remaining colors are fully determined
              and you can greedily pick the highest-value unclicked cell.
            - Orange (90 SP) and yellow (55 SP) are always adjacent to red.
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
