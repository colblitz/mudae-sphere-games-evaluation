"""Abstract base classes for Mudae sphere mini-game strategies.

Each game has its own ABC.  To submit a strategy, create a file in
``strategies/<game>/`` that subclasses the appropriate ABC and implements
``next_click``.  ``init_evaluation_run`` and ``init_game_payload`` are optional overrides.

The harness calls ``init_evaluation_run`` once before all games begin, then
``init_game_payload`` once before each game, and then ``next_click`` on every
click decision, threading the returned game state payload back in.

Revealed cell format
--------------------
``revealed`` is always a list of dicts, one per cell revealed so far::

    [{"row": int, "col": int, "color": str}, ...]

Row and col are 0-indexed (0..4).  Color is a Mudae emoji name, e.g.
``"spR"``, ``"spT"``, ``"spB"``.  The list grows monotonically — every call
includes all cells revealed since the start of the game, not just new ones.

Return value of next_click
--------------------------
A 3-tuple ``(row, col, game_state)``:

- ``row``, ``col``: 0-indexed coordinates of the cell to click next.
- ``game_state``: any Python object; passed back in as ``game_state`` on the
  next call.  Use ``None`` if your strategy is stateless.

Color reference
---------------
oh / harvest:
    spB  blue    (reveals 3 covered cells)
    spT  teal    (reveals 1 covered cell)
    spG  green   35 SP
    spY  yellow  55 SP
    spL  light   ~76 SP average (breaks down into components)
    spO  orange  90 SP
    spR  red     150 SP
    spW  white   ~300 SP
    spP  purple  ~5–12 SP, click is FREE (does not cost a click)
    spD  dark    ~104 SP average (transforms into another color)
    spU  covered (unrevealed; directly clickable)

oc / chest:
    spR  red     150 SP  (one per board; position determines all other colors)
    spO  orange  90 SP   (2 per board, orthogonally adjacent to red)
    spY  yellow  55 SP   (3 per board, diagonally adjacent to red)
    spG  green   35 SP   (4 per board, same row/col as red but not adjacent)
    spT  teal    20 SP   (orthogonal/rowcol residual)
    spB  blue    10 SP   (no spatial relationship to red)

oq / quest:
    spP  purple  5 SP each; 4 hidden per board; click 3 to convert 4th to red
    spR  red     150 SP  (appears after 3 purples clicked; one click to collect)
    spB  blue    0 purple neighbours
    spT  teal    1 purple neighbour
    spG  green   2 purple neighbours
    spY  yellow  3 purple neighbours
    spO  orange  4 purple neighbours

ot / trace:
    spT  teal    ship of length 4
    spG  green   ship of length 3
    spY  yellow  ship of length 3
    spO  orange  ship of length 2 (always present)
    spL  light   ship of length 2 (variable rare; 6-color only)
    spD  dark    ship of length 2 (variable rare; 7-color+)
    spR  red     ship of length 2 (variable rare; 8-color+)
    spW  white   ship of length 2 (variable rare; 9-color only)
    spB  blue    empty cell (clicking costs 1 blue from your budget)
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from typing import Any


# ---------------------------------------------------------------------------
# oh — /sphere harvest
# ---------------------------------------------------------------------------


class OHStrategy(ABC):
    """Strategy interface for /sphere harvest (oh).

    Game rules summary:
    - 5×5 grid; 10 cells are revealed at the start of every game, 15 start
      covered (spU).  The 10 initially revealed cells are already in
      ``revealed`` on the first ``next_click`` call.
    - Click budget: 5 clicks.
    - Blue (spB) reveals 3 additional covered cells; teal (spT) reveals 1.
    - Purple (spP) clicks are FREE (do not cost a click).
    - Dark (spD) transforms into another color when clicked; if it transforms
      into purple the click is also refunded (free).
    - ~50% of boards have one "chest" covered cell worth ~345 SP on average.
    - Goal: maximise total SP collected across your 5 clicks.

    Game metadata keys:
        clicks_left (int): remaining click budget (5 at start, decreases by 1
            per non-purple, non-dark→purple click).
        max_clicks (int): total click budget for this game (always 5).
        game_seed (int): per-game deterministic seed derived from the harness
            master seed and game index.  Passed in the ``meta`` given to
            ``init_game_payload`` so strategies can seed their own RNG and produce
            identical results across runs for the same master seed.
    """

    def init_evaluation_run(self) -> Any:
        """Called once before the evaluation run begins.

        Override to compute data shared across all games — lookup tables,
        precomputed weights, loaded models, etc.  The returned value is passed
        as ``evaluation_run_state`` to every ``init_game_payload`` call.

        Do not store game-specific information here.  Each game must be played
        independently; sharing board history between games produces unfair results.

        Default: returns ``None``.
        """
        return None

    def init_game_payload(self, meta: dict[str, Any], evaluation_run_state: Any) -> Any:
        """Called once before the first click of each game.

        Override to set up fresh per-game state.  The returned value becomes
        the ``game_state`` for that game's first ``next_click`` call.

        Args:
            meta: game metadata (same keys as passed to ``next_click``).
            evaluation_run_state: read-only value returned by ``init_evaluation_run``.
                Do not mutate this — it is shared across all games.

        Returns:
            Initial ``game_state`` for this game's first ``next_click`` call.

        Default: returns ``evaluation_run_state`` unchanged.
        """
        return evaluation_run_state

    @abstractmethod
    def next_click(
        self,
        revealed: list[dict[str, Any]],
        meta: dict[str, Any],
        game_state: Any,
    ) -> tuple[int, int, Any]:
        """Choose the next cell to click.

        Args:
            revealed: all cells revealed so far, each as
                ``{"row": int, "col": int, "color": str}``.
            meta: ``{"clicks_left": int, "max_clicks": int}``.
            game_state: value returned by the previous ``next_click`` call
                (or ``init_game_payload`` for the first call of the game).

        Returns:
            ``(row, col, game_state)`` — coordinates of the cell to click
            (0-indexed) and the updated game state to pass into the next call.
        """
        ...


# ---------------------------------------------------------------------------
# oc — /sphere chest
# ---------------------------------------------------------------------------


class OCStrategy(ABC):
    """Strategy interface for /sphere chest (oc).

    Game rules summary:
    - 5×5 grid; all 25 cells start covered.
    - Click budget: 5 clicks.
    - One red sphere (spR, 150 SP) is hidden; its position determines the
      color of every other cell via fixed spatial zones:
        orth     (|dr|+|dc|==1)        → orange (spO, 90)  [2 cells]
        diag     (|dr|==|dc|, dr!=0)   → yellow (spY, 55)  [3 cells]
        rowcol   (same row/col, not orth/diag) → green (spG, 35) [4 cells]
        none     (no geometric relation) → blue  (spB, 10)  [remaining]
        residual orth/rowcol            → teal  (spT, 20)
    - Center cell (2,2) can never be red.
    - Goal: maximise SP collected across 5 clicks.

    Note on board distribution: all 16,800 valid boards are weighted equally
    in evaluation.  In reality Mudae does not necessarily use a uniform
    distribution; equal weighting is used here for standardized comparison.

    Game metadata keys:
        clicks_left (int): remaining click budget.
        max_clicks (int): total click budget (always 5).
    """

    def init_evaluation_run(self) -> Any:
        """Called once before the evaluation run begins. Default: None.

        Do not store game-specific information here — each game must be played
        independently; sharing board history between games produces unfair results.
        """
        return None

    def init_game_payload(self, meta: dict[str, Any], evaluation_run_state: Any) -> Any:
        """Called once before each game. Returns initial game_state. Default: evaluation_run_state."""
        return evaluation_run_state

    @abstractmethod
    def next_click(
        self,
        revealed: list[dict[str, Any]],
        meta: dict[str, Any],
        game_state: Any,
    ) -> tuple[int, int, Any]:
        """Choose the next cell to click.

        Args:
            revealed: all cells revealed so far.
            meta: ``{"clicks_left": int, "max_clicks": int}``.
            game_state: value from the previous call (or init_game_payload).

        Returns:
            ``(row, col, game_state)``.
        """
        ...


# ---------------------------------------------------------------------------
# oq — /sphere quest
# ---------------------------------------------------------------------------


class OQStrategy(ABC):
    """Strategy interface for /sphere quest (oq).

    Game rules summary:
    - 5×5 grid; all 25 cells start covered.
    - Click budget: 7 non-purple clicks.
    - 4 purple spheres (spP) are hidden among the cells.
    - Clicking a purple is FREE (does not cost a click).
    - Non-purple cells reveal the count of purple neighbours (0–4) as a color:
        spB=0, spT=1, spG=2, spY=3, spO=4
    - Goal: click 3 of the 4 purples → the 4th converts to red (spR, 150 SP).
      Then spend remaining budget greedily on highest-value derivable tiles.
    - Clicking red does NOT cost a click.

    Game metadata keys:
        clicks_left (int): remaining non-purple click budget.
        max_clicks (int): total non-purple click budget (always 7).
        purples_found (int): number of purple cells clicked so far.
    """

    def init_evaluation_run(self) -> Any:
        """Called once before the evaluation run begins. Default: None.

        Do not store game-specific information here — each game must be played
        independently; sharing board history between games produces unfair results.
        """
        return None

    def init_game_payload(self, meta: dict[str, Any], evaluation_run_state: Any) -> Any:
        """Called once before each game. Returns initial game_state. Default: evaluation_run_state."""
        return evaluation_run_state

    @abstractmethod
    def next_click(
        self,
        revealed: list[dict[str, Any]],
        meta: dict[str, Any],
        game_state: Any,
    ) -> tuple[int, int, Any]:
        """Choose the next cell to click.

        Args:
            revealed: all cells revealed so far.
            meta: ``{"clicks_left": int, "max_clicks": int,
                      "purples_found": int}``.
            game_state: value from the previous call (or init_game_payload).

        Returns:
            ``(row, col, game_state)``.
        """
        ...


# ---------------------------------------------------------------------------
# ot — /sphere trace
# ---------------------------------------------------------------------------


class OTStrategy(ABC):
    """Strategy interface for /sphere trace (ot).

    Game rules summary:
    - 5×5 grid; all 25 cells start covered.
    - Click budget: 4 BLUE clicks.  Ship cells are FREE (do not cost a click).
    - Ships (contiguous horizontal or vertical segments, no overlap):
        6-color: teal(4), green(3), yellow(3), orange(2), light(2)
                 → 11 blue cells, 14 ship cells
        7-color: teal(4), green(3), yellow(3), orange(2), light(2), dark(2)
                 → 9 blue cells, 16 ship cells
        8-color: teal(4), green(3), yellow(3), orange(2), light(2), dark(2), red(2)
                 → 7 blue cells, 18 ship cells
        9-color: all above + white(2)
                 → 5 blue cells, 20 ship cells
    - Extra Chance: if you click blue #4 before hitting 5 ship cells, the game
      continues.  Each additional blue while ships_hit < 5 extends the game.
      After the 5th ship hit, Extra Chance shuts off — the next blue ends the game.
    - Perfect game: find all blues via constraint inference, then collect all
      remaining ship cells for free.

    Game metadata keys:
        n_colors (int): number of colors (6, 7, 8, or 9).
        ships_hit (int): number of ship cells revealed so far.
        blues_used (int): number of blue clicks spent so far.
        max_clicks (int): base blue click budget (always 4; Extra Chance may extend).
    """

    def init_evaluation_run(self) -> Any:
        """Called once before the evaluation run begins. Default: None.

        Do not store game-specific information here — each game must be played
        independently; sharing board history between games produces unfair results.
        """
        return None

    def init_game_payload(self, meta: dict[str, Any], evaluation_run_state: Any) -> Any:
        """Called once before each game. Returns initial game_state. Default: evaluation_run_state."""
        return evaluation_run_state

    @abstractmethod
    def next_click(
        self,
        revealed: list[dict[str, Any]],
        meta: dict[str, Any],
        game_state: Any,
    ) -> tuple[int, int, Any]:
        """Choose the next cell to click.

        Args:
            revealed: all cells revealed so far.
            meta: ``{"n_colors": int, "ships_hit": int,
                      "blues_used": int, "max_clicks": int}``.
            game_state: value from the previous call (or init_game_payload).

        Returns:
            ``(row, col, game_state)``.
        """
        ...
