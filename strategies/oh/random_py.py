"""Random baseline strategy for /sphere harvest (oh).

Picks a random unrevealed, unclicked cell on every turn.
Prefers purple cells (they are free) if any are visible.
"""

import random
from typing import Any

from interface.strategy import OHStrategy


class RandomOHStrategy(OHStrategy):
    def next_click(
        self,
        revealed: list[dict[str, Any]],
        meta: dict[str, Any],
        state: Any,
    ) -> tuple[int, int, Any]:
        clicked = {(c["row"], c["col"]) for c in revealed}

        # Prefer any visible purple (free click)
        purples = [(c["row"], c["col"]) for c in revealed if c["color"] == "spP"]
        if purples:
            row, col = random.choice(purples)
            return row, col, state

        # Pick a random unclicked cell
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
