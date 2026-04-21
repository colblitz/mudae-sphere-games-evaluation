"""Random baseline strategy for /sphere trace (ot).

Picks a random unclicked cell on every turn.
No constraint inference — does not use ship geometry.
"""

import random
from typing import Any

from interface.strategy import OTStrategy


class RandomOTStrategy(OTStrategy):
    def next_click(
        self,
        revealed: list[dict[str, Any]],
        meta: dict[str, Any],
        state: Any,
    ) -> tuple[int, int, Any]:
        clicked = {(c["row"], c["col"]) for c in revealed}
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
