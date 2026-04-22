"""Global-state example strategy for /sphere chest (oc).

Demonstrates how to use init_evaluation_run() to compute data ONCE before all games
in an evaluation run, then reuse it cheaply across every game.

The payload here is a fixed cell visit order — a spiral from the center
outward.  Every game simply walks down this list and clicks the first cell
that hasn't been revealed yet.

Why global state?
-----------------
init_evaluation_run() is called exactly once per evaluation run (before any games
start).  The returned value is passed as ``state`` to init_game_payload() at the
start of every game, and from there flows through every next_click() call.

Use init_evaluation_run() for anything that is:
  - Expensive to compute (search, optimization, loading a table from disk).
  - Identical for every game (board-independent precomputation).
  - Read-only during play (the lookup table never changes mid-run).

Contrast with init_game_payload(), which is called once *per game* and is the right
place to reset per-game bookkeeping — see oq/stateful.py for that pattern.

Strategy logic
--------------
The center cell (2,2) can never be red, so we deprioritize it by putting it
last.  We visit all other cells in a deterministic spiral order so the first
few clicks cover a spread of positions regardless of the board layout.
The lookup table is built once; next_click() does a simple linear scan.
"""

from typing import Any

from interface.strategy import OCStrategy

# ---------------------------------------------------------------------------
# Cell visit order (built at module load time for clarity, but it could also
# be built lazily inside init_evaluation_run).
# ---------------------------------------------------------------------------

def _spiral_order() -> list[tuple[int, int]]:
    """Return all 25 cells in a center-outward spiral, (2,2) last."""
    # Manhattan-distance shells: 0 (center), 1, 2, 3, 4
    # Within each shell, iterate clockwise starting from top-left.
    visited = []
    seen = set()
    # Walk shells by increasing Chebyshev distance from center
    for dist in range(1, 4):
        r_start, r_end = 2 - dist, 2 + dist
        c_start, c_end = 2 - dist, 2 + dist
        # Top row left→right
        for c in range(max(0, c_start), min(5, c_end + 1)):
            r = r_start
            if 0 <= r < 5 and (r, c) not in seen:
                visited.append((r, c)); seen.add((r, c))
        # Right col top→bottom
        for r in range(max(0, r_start + 1), min(5, r_end + 1)):
            c = c_end
            if 0 <= c < 5 and (r, c) not in seen:
                visited.append((r, c)); seen.add((r, c))
        # Bottom row right→left
        for c in range(min(4, c_end - 1), max(-1, c_start - 1), -1):
            r = r_end
            if 0 <= r < 5 and (r, c) not in seen:
                visited.append((r, c)); seen.add((r, c))
        # Left col bottom→top
        for r in range(min(4, r_end - 1), max(-1, r_start), -1):
            c = c_start
            if 0 <= c < 5 and (r, c) not in seen:
                visited.append((r, c)); seen.add((r, c))
    # Add center last (it can never be red so it has the lowest priority)
    if (2, 2) not in seen:
        visited.append((2, 2))
    # Fill in any cells not yet covered (shouldn't happen on a 5x5 grid,
    # but keeps the list complete just in case)
    for r in range(5):
        for c in range(5):
            if (r, c) not in seen:
                visited.append((r, c)); seen.add((r, c))
    return visited


class GlobalStateOCStrategy(OCStrategy):

    def init_evaluation_run(self) -> Any:
        """Compute the visit order ONCE before all games begin.

        Returns a dict that will be passed as ``state`` to every subsequent
        init_game_payload() and next_click() call for the entire evaluation run.
        Treat it as read-only during play.
        """
        order = _spiral_order()
        return {"order": order}

    # init_game_payload() is intentionally omitted: the default implementation just
    # returns state unchanged, which is exactly what we want — the global
    # lookup table requires no per-game reset.

    def next_click(
        self,
        revealed: list[dict[str, Any]],
        meta: dict[str, Any],
        state: Any,
    ) -> tuple[int, int, Any]:
        """Pick the first cell in the precomputed order that isn't revealed yet."""
        clicked = {(c["row"], c["col"]) for c in revealed}
        order: list[tuple[int, int]] = state["order"]

        for row, col in order:
            if (row, col) not in clicked:
                return row, col, state  # state is unchanged — it's a shared table

        # Fallback: should never be reached on a valid board
        return 0, 0, state
