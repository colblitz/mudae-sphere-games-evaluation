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

REVEALED CELL FORMAT
--------------------
revealed is a list of dicts, one per cell revealed so far (monotonically
growing — every call includes all cells revealed since game start):

    [{"row": int, "col": int, "color": str}, ...]

Row and col are 0-indexed (0..4).

META KEYS (ot)
--------------
n_colors    int   number of ship colors in this game (6, 7, 8, or 9)
ships_hit   int   number of ship cells revealed so far
blues_used  int   number of blue clicks spent so far
max_clicks  int   base blue click budget (always 4; Extra Chance may extend)

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

from interface.strategy import OTStrategy


class MyOTStrategy(OTStrategy):
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
        revealed: list[dict[str, Any]],
        meta: dict[str, Any],
        game_state: Any,
    ) -> tuple[int, int, Any]:
        """Choose the next cell to click.

        Args:
            revealed: all cells revealed so far this game, each as
                      {"row": int, "col": int, "color": str}.
            meta: {
                "n_colors":   int,  # ship color count (6, 7, 8, or 9)
                "ships_hit":  int,  # ship cells revealed so far
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
            - Ship cells are FREE — clicking a revealed ship cell costs nothing.
              After a ship cell is revealed, click it immediately before spending
              another blue click.
            - Each revealed cell (ship or blue) constrains where remaining ships
              can be.  Use revealed ship segments to infer the orientation and
              extent of each ship.
            - A revealed blue cell confirms that row/col position is empty —
              no ship passes through it.
            - Prefer cells with high probability of being a ship cell to
              minimise wasted blue clicks.
            - Do not return a (row, col) that is already in revealed.
        """
        clicked = {(c["row"], c["col"]) for c in revealed}

        # Always collect any already-revealed ship cell that hasn't been
        # "collected" yet — ship clicks are free and should never be skipped.
        # (In practice the harness auto-collects contiguous ship cells when
        #  a new segment is hit, but your strategy can also choose them
        #  explicitly to ensure contiguous chains are followed.)

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
