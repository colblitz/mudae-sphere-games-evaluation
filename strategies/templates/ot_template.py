"""Template strategy for /sphere trace (ot) — Python.

Copy this file to strategies/ot/<your_name>.py, rename the class, and fill
in the TODO sections.  Delete any methods you don't need — only next_click()
is required.

GAME OVERVIEW
-------------
Grid   : 5×5, all 25 cells start covered
Budget : 4 BLUE clicks (ship cells are FREE)
Goal   : find ship cells (free) while avoiding spending blue clicks (costly)

Ships are contiguous horizontal or vertical segments.  Blue (spB) cells are
empty — clicking one uses up 1 of your 4 blue-click budget.  Ship cells do
not cost a click.

Extra Chance rule:
  If you click blue #4 before revealing at least 5 ship cells, the game
  continues.  Each additional blue while ships_hit < 5 extends the game.
  Once ships_hit >= 5, the next blue click ends the game.

Ship configurations by n_colors:
  6-color: teal(4), green(3), yellow(3), orange(2), light(2)
           → 14 ship cells, 11 blue cells
  7-color: + dark(2)   → 16 ship cells,  9 blue cells
  8-color: + red(2)    → 18 ship cells,  7 blue cells
  9-color: + white(2)  → 20 ship cells,  5 blue cells

COLOR REFERENCE (ot)
--------------------
spT   teal    ship of length 4
spG   green   ship of length 3
spY   yellow  ship of length 3
spO   orange  ship of length 2 (always present)
spL   light   ship of length 2 (6-color and above)
spD   dark    ship of length 2 (7-color and above)
spR   red     ship of length 2 (8-color and above)
spW   white   ship of length 2 (9-color only)
spB   blue    empty cell (costs 1 blue click)

BOARD CELL FORMAT
-----------------
board is always a list of exactly 25 dicts, one per cell:

    [{"row": int, "col": int, "color": str, "clicked": bool}, ...]

Row and col are 0-indexed (0..4).  color="spU" = covered/unknown.
clicked=False = still interactable; clicked=True = disabled.

META KEYS (ot)
--------------
n_colors    int   number of ship colors in this game (6, 7, 8, or 9)
ships_hit   int   number of ship cells revealed so far
blues_used  int   number of blue clicks spent so far
max_clicks  int   base blue click budget (always 4; Extra Chance may extend)

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

from interface.strategy import OTStrategy


class MyOTStrategy(OTStrategy):
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

        Example: for each n_colors variant, enumerate all valid ship placements
        and precompute the marginal probability that each cell is a ship cell.
        Use these priors as the starting point for per-game inference.
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
                   meta["n_colors"] tells you which ship configuration applies.
            evaluation_run_state: read-only value from init_evaluation_run().

        Returns:
            The initial game_state for this game's first next_click() call.

        Example: initialise the set of all valid ship placements for this
        game's n_colors, to be pruned as cells are revealed.
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
                "n_colors":   int,  # ship color count (6, 7, 8, or 9)
                "ships_hit":  int,  # ship cells clicked so far
                "blues_used": int,  # blue clicks spent so far
                "max_clicks": int,  # base blue budget (always 4)
            }
            game_state: value returned by the previous next_click() call, or by
                   init_game_payload() for the first call of the game.

        Returns:
            (row, col, game_state)
            row, col    : 0-indexed coordinates of the cell to click next.
            game_state  : updated state to pass into the next call.

        Tips:
            - Ship cells are FREE — clicking a ship cell does not cost a click.
            - Each clicked cell constrains where remaining ships can be.
            - A blue cell at (r,c) confirms no ship passes through it.
            - Prefer cells with high P(ship) to minimise wasted blue clicks.
            - Do not return a (row, col) where board[row*5+col]["clicked"] is True.
        """
        clicked = {(c["row"], c["col"]) for c in board if c["clicked"]}

        # TODO: replace with your click logic

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
