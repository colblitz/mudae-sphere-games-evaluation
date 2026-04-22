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

REVEALED CELL FORMAT
--------------------
revealed is a list of dicts, one per cell revealed so far (monotonically
growing — every call includes all cells revealed since game start):

    [{"row": int, "col": int, "color": str}, ...]

Row and col are 0-indexed (0..4).

META KEYS (oc)
--------------
clicks_left  int   remaining click budget
max_clicks   int   total click budget (always 5)

STATE PAYLOAD
-------------
state is any Python object you choose.  The harness threads it through every
call within a game:

  init_evaluation_run()        → initial_state        (called once before all games)
  init_game_payload(meta, s0)    → s1                   (called once per game)
  next_click(..., s1)   → (row, col, s2)
  next_click(..., s2)   → (row, col, s3)
  ...

Use init_evaluation_run() for data computed ONCE and shared across all games
(lookup tables, precomputed weights, etc.).

Use init_game_payload() to reset per-game bookkeeping at the start of each game.

If your strategy is stateless, leave both optional methods out and return
`state` unchanged from next_click().

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
        """Return the initial state payload — called ONCE before all games.

        Compute anything that is board-independent and expensive to repeat.
        The returned value is passed as ``state`` to init_game_payload() at the start
        of every game.  Treat it as read-only during play.

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
        state: Any,
    ) -> Any:
        """Set up per-game state — called once before each game's first click.

        Args:
            meta:  game metadata (same keys as next_click).
            state: value returned by init_evaluation_run().

        Returns:
            The initial state for this game's first next_click() call.

        Example: merge the global lookup table with a fresh per-game
        posterior over possible red positions, then update the posterior
        as each color is revealed.
        """
        # TODO: reset per-game fields here, or delete this method
        return state

    # -----------------------------------------------------------------------
    # Required: click decision
    # -----------------------------------------------------------------------

    def next_click(
        self,
        revealed: list[dict[str, Any]],
        meta: dict[str, Any],
        state: Any,
    ) -> tuple[int, int, Any]:
        """Choose the next cell to click.

        Args:
            revealed: all cells revealed so far this game, each as
                      {"row": int, "col": int, "color": str}.
            meta: {
                "clicks_left": int,   # remaining budget
                "max_clicks":  int,   # total budget (always 5)
            }
            state: value returned by the previous next_click() call, or by
                   init_game_payload() for the first call of the game.

        Returns:
            (row, col, next_state)
            row, col    : 0-indexed coordinates of the cell to click next.
            next_state  : updated state to pass into the next call.

        Tips:
            - Each revealed color constrains where the red sphere can be.
              Any cell that would violate the revealed pattern given a
              candidate red position can be eliminated.
            - Once red is found, the remaining colors are fully determined
              and you can greedily pick the highest-value unrevealed cell.
            - Orange (90 SP) and yellow (55 SP) are always adjacent to red;
              after finding red you know exactly where they are.
            - Do not return a (row, col) that is already in revealed.
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
            return 0, 0, state
        row, col = random.choice(unclicked)
        return row, col, state
