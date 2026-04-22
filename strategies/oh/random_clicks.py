"""Random baseline strategy for /sphere harvest (oh).

Picks a random unrevealed, unclicked cell on every turn.
Prefers purple cells (they are free) if any are visible.

This is the simplest possible strategy — no state, no inference.
See stateful.py (oq/) for per-game state usage, global_state.py (oc/)
for cross-game global state usage.
"""

import random
from typing import Any

from interface.strategy import OHStrategy


class RandomOHStrategy(OHStrategy):
    def __init__(self) -> None:
        self._rng = random.Random()

    def init_game_payload(
        self,
        meta: dict[str, Any],
        evaluation_run_state: Any,
    ) -> Any:
        # Seed a fresh local RNG from the per-game seed supplied by the harness.
        # This ensures identical click sequences across runs for the same seed.
        self._rng = random.Random(meta.get("game_seed"))
        return evaluation_run_state

    def next_click(
        self,
        revealed: list[dict[str, Any]],
        meta: dict[str, Any],
        game_state: Any,
    ) -> tuple[int, int, Any]:
        clicked = {(c["row"], c["col"]) for c in revealed}

        # Prefer any visible purple (free click)
        purples = [(c["row"], c["col"]) for c in revealed if c["color"] == "spP"]
        if purples:
            row, col = self._rng.choice(purples)
            return row, col, game_state

        # Pick a random unclicked cell
        unclicked = [
            (r, c)
            for r in range(5)
            for c in range(5)
            if (r, c) not in clicked
        ]
        if not unclicked:
            return 0, 0, game_state

        row, col = self._rng.choice(unclicked)
        return row, col, game_state  # game_state is None — stateless strategy
