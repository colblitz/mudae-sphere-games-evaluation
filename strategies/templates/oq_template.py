"""Template strategy for /sphere quest (oq) — Python.

Copy this file to strategies/oq/<your_name>.py, rename the class, and fill
in the TODO sections.  Delete any methods you don't need — only next_click()
is required.

GAME OVERVIEW
-------------
Grid   : 5×5, all 25 cells start covered
Budget : 7 non-purple clicks
Goal   : find 3 of the 4 hidden purple spheres → the 4th converts to red
         (spR, 150 SP).  Collect red and spend remaining budget on high-value
         tiles.

Special click rules:
  - Purple (spP) : click is FREE (does not consume a click)
  - Red (spR)    : appears after 3 purples clicked; costs 1 click (worth 150 SP)

Non-purple cells reveal the count of purple neighbours (orthogonal +
diagonal, capped at 4) as a color:

  spB = 0 purple neighbours
  spT = 1 purple neighbour
  spG = 2 purple neighbours
  spY = 3 purple neighbours
  spO = 4 purple neighbours

This neighbor-count information can be used to narrow down where purples are
(similar to Minesweeper constraint inference).

COLOR REFERENCE (oq)
--------------------
spP   purple  5 SP each; 4 hidden per board; click is FREE
spR   red     150 SP; appears after 3 purples found; click is FREE
spB   blue    0 purple neighbours
spT   teal    1 purple neighbour
spG   green   2 purple neighbours
spY   yellow  3 purple neighbours
spO   orange  4 purple neighbours

BOARD CELL FORMAT
-----------------
board is always a list of exactly 25 dicts, one per cell:

    [{"row": int, "col": int, "color": str, "clicked": bool}, ...]

Row and col are 0-indexed (0..4).  color="spU" = covered/unknown.
clicked=False = still interactable; clicked=True = disabled.

After 3 purples are clicked the 4th purple appears with color="spR",
clicked=False in the board on the next call.  Click it — costs 1 click,
worth 150 SP.

META KEYS (oq)
--------------
clicks_left    int   remaining non-purple click budget
max_clicks     int   total non-purple click budget (always 7)
purples_found  int   number of purple cells clicked so far

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

If your strategy is stateless, leave both optional methods out and return
`game_state` unchanged from next_click().

See also
--------
oh/random_clicks.py   — minimal stateless example
oc/global_state.py    — init_evaluation_run() (global state) example
oq/stateful.py        — init_game_payload() + state threading (per-game state) example
"""

import random
from typing import Any

from interface.strategy import OQStrategy


class MyOQStrategy(OQStrategy):
    # -----------------------------------------------------------------------
    # Optional: global state (computed once, shared across ALL games)
    # -----------------------------------------------------------------------

    def init_evaluation_run(self) -> Any:
        """Called once before the evaluation run begins.

        Compute anything that is board-independent and expensive to repeat.
        The returned value is passed as ``evaluation_run_state`` to init_game_payload() at the start
        of every game.  Treat it as read-only during play.

        Returns:
            Any Python object.  Default: None.

        Example: precompute for each of the 12,650 possible purple layouts
        the optimal first-click order, then look it up at runtime.
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

        Example: initialise a constraint model — a set of all 12,650 possible
        purple layouts, narrowed down each turn as colors are revealed.
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
                   After 3 purples are clicked, the 4th appears with
                   color="spR", clicked=False — click it (costs 1 click, 150 SP).
            meta: {
                "clicks_left":   int,  # remaining non-purple budget
                "max_clicks":    int,  # total non-purple budget (always 7)
                "purples_found": int,  # purples clicked so far (0–3)
            }
            game_state: value returned by the previous next_click() call, or by
                   init_game_payload() for the first call of the game.

        Returns:
            (row, col, game_state)
            row, col    : 0-indexed coordinates of the cell to click next.
            game_state  : updated state to pass into the next call.

        Tips:
            - Click any cell with color="spR" and clicked=False — 150 SP for 1 click.
            - Click any cell with color="spP" and clicked=False immediately (free).
            - Each non-purple reveal gives a neighbor count — use for inference.
            - Do not return a (row, col) where board[row*5+col]["clicked"] is True.
        """
        clicked = {(c["row"], c["col"]) for c in board if c["clicked"]}

        # Always click red when it appears — 150 SP for 1 click
        reds = [(c["row"], c["col"]) for c in board if c["color"] == "spR" and not c["clicked"]]
        if reds:
            row, col = reds[0]
            return row, col, game_state

        # Always click purple immediately (free)
        purples = [(c["row"], c["col"]) for c in board if c["color"] == "spP" and not c["clicked"]]
        if purples:
            row, col = purples[0]
            return row, col, game_state

        # TODO: replace the random fallback with your click logic

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
