"""Random baseline strategy for /sphere chest (oc).

Picks a random unclicked cell on every turn.
"""

import random
from typing import Any

from interface.strategy import OCStrategy


class RandomOCStrategy(OCStrategy):
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
