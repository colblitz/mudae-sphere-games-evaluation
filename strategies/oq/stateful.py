"""Stateful example strategy for /sphere quest (oq).

Demonstrates how to use init_game_payload() and the threaded state payload to
maintain information across clicks within a single game.

WHY PER-GAME STATE?
-------------------
The state payload is threaded through every call within a game:

  init_game_payload(meta, evaluation_run_state) → initial game_state for this game
  next_click(revealed, meta, s0)               → (row, col, s1)
  next_click(revealed, meta, s1)               → (row, col, s2)
  ...

init_game_payload() is called once at the start of EACH game, making it the right
place to reset anything that should be fresh for every game.  Whatever it
returns becomes the state for that game's first next_click() call.

Contrast with init_evaluation_run(), which is called only ONCE before all games
begin — use that for cross-game global tables (see oc/global_state.py).

STRATEGY LOGIC
--------------
We track which rows and columns have already been clicked this game.
When choosing the next cell, we prefer cells whose row AND column are both
new (maximising board coverage).  If no such cell exists we fall back to
cells with at least one new axis, then to any unclicked cell.

The click history is accumulated in the state dict across calls within a
game and is reset to empty by init_game_payload() at the start of each new game.
"""

import random
from typing import Any

from interface.strategy import OQStrategy


class StatefulOQStrategy(OQStrategy):

    def init_game_payload(
        self,
        meta: dict[str, Any],
        evaluation_run_state: Any,
    ) -> Any:
        """Reset per-game tracking at the start of every game.

        Called once before the first next_click() of each game.
        evaluation_run_state is whatever init_evaluation_run() returned (None
        by default); we ignore it and return a fresh dict instead.

        Returns:
            A new game_state dict for this game — click history starts empty.
        """
        return {
            "clicked_rows": [],   # rows clicked so far this game
            "clicked_cols": [],   # cols clicked so far this game
            "click_count": 0,     # total clicks made this game
        }

    def next_click(
        self,
        revealed: list[dict[str, Any]],
        meta: dict[str, Any],
        game_state: Any,
    ) -> tuple[int, int, Any]:
        """Choose the next cell, preferring unexplored rows and columns.

        game_state carries the click history accumulated across all previous
        calls this game.  We update it and return the new version.
        """
        clicked_set = {(c["row"], c["col"]) for c in revealed}
        clicked_rows: set[int] = set(game_state["clicked_rows"])
        clicked_cols: set[int] = set(game_state["clicked_cols"])

        # Collect all unclicked cells, scored by how many new axes they cover
        new_both = []    # row AND col are new
        new_one  = []    # either row or col is new
        fallback = []    # neither row nor col is new

        for r in range(5):
            for c in range(5):
                if (r, c) in clicked_set:
                    continue
                new_r = r not in clicked_rows
                new_c = c not in clicked_cols
                if new_r and new_c:
                    new_both.append((r, c))
                elif new_r or new_c:
                    new_one.append((r, c))
                else:
                    fallback.append((r, c))

        candidates = new_both or new_one or fallback
        if not candidates:
            return 0, 0, game_state

        row, col = random.choice(candidates)

        # Update and return new game_state — this becomes game_state on the next call
        new_state = {
            "clicked_rows": game_state["clicked_rows"] + [row],
            "clicked_cols": game_state["clicked_cols"] + [col],
            "click_count": game_state["click_count"] + 1,
        }
        return row, col, new_state
