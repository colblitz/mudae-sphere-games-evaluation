"""Color-priority strategy for /sphere harvest (oh) — Python.

Demonstrates loading a data file via interface.data.fetch().

The strategy loads a JSON file containing color priority weights from data/.
On every click it picks the highest-value already-revealed cell that has not
yet been clicked, falling back to a random covered cell when no such cell
exists.  Purple cells are always clicked first (they are free).

The data file is committed directly to data/ (it is tiny), so fetch() finds
it locally and skips any network request.  For a real large file (e.g. a
14 GB lookup table) the same fetch() call would download it from the hosted
URL on first use and cache it in data/ for all subsequent runs.

External data
-------------
File    : oh_example.json
Size    : < 1 KB
Source  : https://raw.githubusercontent.com/colblitz/mudae-sphere-games-evaluation/main/data/oh_example.json
SHA-256 : fdebc10f5140a1b4ae6643b4626660d86a34847ea8f6d30e617b2c63e7d008c8
"""

import json
import random
from typing import Any

from interface.data import fetch
from interface.strategy import OHStrategy

# ---------------------------------------------------------------------------
# Data file configuration
# ---------------------------------------------------------------------------

_DATA_URL    = "https://raw.githubusercontent.com/colblitz/mudae-sphere-games-evaluation/main/data/oh_example.json"
_DATA_SHA256 = "fdebc10f5140a1b4ae6643b4626660d86a34847ea8f6d30e617b2c63e7d008c8"
_DATA_FILE   = "oh_example.json"


class LoadDataOHStrategy(OHStrategy):
    """OH strategy that loads color priority weights from a data file.

    init_evaluation_run loads the weights once before any games begin.
    next_click uses them to rank visible cells, clicking the highest-value
    unclicked cell first.
    """

    # -----------------------------------------------------------------------
    # Global state: load data file once, share across all games
    # -----------------------------------------------------------------------

    def init_evaluation_run(self) -> Any:
        """Load color priority weights from data/oh_example.json.

        fetch() checks whether the file is already present in data/ and its
        SHA-256 matches.  If so it returns immediately — no download occurs.
        If the file is absent or corrupted it downloads from _DATA_URL.

        Returns a dict with the loaded color_values table.
        """
        path = fetch(url=_DATA_URL, sha256=_DATA_SHA256, filename=_DATA_FILE)
        with open(path) as f:
            data = json.load(f)
        return {"color_values": data["color_values"]}

    # -----------------------------------------------------------------------
    # Per-game state: seed RNG from game_seed for reproducibility
    # -----------------------------------------------------------------------

    def init_game_payload(
        self,
        meta: dict[str, Any],
        evaluation_run_state: Any,
    ) -> Any:
        """Seed a fresh per-game RNG on self and carry the run state through.

        The RNG is stored as an instance variable rather than in the state
        dict because random.Random is not JSON-serializable — the harness
        serializes state to JSON between calls.  The run state (color_values)
        is JSON-safe and passes through unchanged.
        """
        self._rng = random.Random(meta.get("game_seed"))
        return evaluation_run_state

    # -----------------------------------------------------------------------
    # Click decision: highest-value visible cell, or random covered cell
    # -----------------------------------------------------------------------

    def next_click(
        self,
        revealed: list[dict[str, Any]],
        meta: dict[str, Any],
        game_state: Any,
    ) -> tuple[int, int, Any]:
        color_values: dict[str, int] = game_state["color_values"]
        rng: random.Random = self._rng

        clicked = {(c["row"], c["col"]) for c in revealed}

        # Purples are free — click any visible purple immediately.
        purples = [(c["row"], c["col"]) for c in revealed if c["color"] == "spP"]
        if purples:
            row, col = rng.choice(purples)
            return row, col, game_state

        # Among all revealed-but-unclicked cells, pick the highest-value one.
        # (Revealed cells have a known color; covered cells do not.)
        candidates = [
            (color_values.get(c["color"], 0), c["row"], c["col"])
            for c in revealed
            if (c["row"], c["col"]) not in clicked
            and c["color"] not in ("spB", "spT")  # skip info-only cells for now
        ]
        if candidates:
            best_value = max(v for v, _, _ in candidates)
            best = [(r, c) for v, r, c in candidates if v == best_value]
            row, col = rng.choice(best)
            return row, col, game_state

        # Fall back to a random covered (spU) cell.
        unclicked = [
            (r, c)
            for r in range(5)
            for c in range(5)
            if (r, c) not in clicked
        ]
        if not unclicked:
            return 0, 0, game_state
        row, col = rng.choice(unclicked)
        return row, col, game_state
