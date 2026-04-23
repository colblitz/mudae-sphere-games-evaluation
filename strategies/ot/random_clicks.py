"""Random baseline strategy for /sphere trace (ot).

Picks a random unclicked cell on every turn.
No constraint inference — does not use ship geometry.

This is the simplest possible strategy — no state, no inference.
See stateful.py (oq/) for per-game state usage, global_state.py (oc/)
for cross-game global state usage.
"""

import random
from typing import Any

from interface.strategy import OTStrategy


class RandomOTStrategy(OTStrategy):
    def next_click(
        self,
        board: list[dict[str, Any]],
        meta: dict[str, Any],
        game_state: Any,
    ) -> tuple[int, int, Any]:
        clicked = {(c["row"], c["col"]) for c in board if c["clicked"]}
        unclicked = [
            (r, c)
            for r in range(5)
            for c in range(5)
            if (r, c) not in clicked
        ]
        if not unclicked:
            return 0, 0, game_state
        row, col = random.choice(unclicked)
        return row, col, game_state  # game_state is None — stateless strategy
