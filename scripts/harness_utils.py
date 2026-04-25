"""harness_utils.py — Shared utilities for evaluate.py and compare_strategies.py.

Provides:
  - _harness_source_mtime(source)  newest mtime across source + all headers
  - build_harness(game)            build (if stale) and return binary path
  - _COLOR_LETTER                  single-letter display codes for board colors
  - _cell_letter(color)            map a color string to a display letter
  - _render_board(cells, ...)      ASCII 5x5 board renderer
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path
from typing import Any

REPO_ROOT   = Path(__file__).parent.parent.resolve()
HARNESS_DIR = REPO_ROOT / "harness"


# ---------------------------------------------------------------------------
# Build helpers
# ---------------------------------------------------------------------------

def _harness_source_mtime(source: Path) -> float:
    """Return the newest mtime across the given source file and all headers
    in harness/common/, so that a header change triggers a rebuild."""
    mtimes = [source.stat().st_mtime]
    common_dir = HARNESS_DIR / "common"
    if common_dir.is_dir():
        for h in common_dir.iterdir():
            if h.suffix in (".h", ".hpp"):
                mtimes.append(h.stat().st_mtime)
    return max(mtimes)


def build_harness(game: str, *, exit_code: int = 1) -> Path:
    """Build (if stale) the evaluator binary for *game* and return its path.

    Staleness is checked against both the .cpp source and all headers in
    harness/common/.  Pass exit_code=2 when calling from compare_strategies.py
    to match that script's error-exit convention.
    """
    binary = HARNESS_DIR / f"evaluate_{game}"
    source = HARNESS_DIR / f"evaluate_{game}.cpp"
    if not source.exists():
        print(f"ERROR: harness source not found: {source}", file=sys.stderr)
        sys.exit(exit_code)
    if binary.exists() and binary.stat().st_mtime >= _harness_source_mtime(source):
        return binary  # up to date

    print(f"[build] Building {binary.name} ...")
    result = subprocess.run(
        ["make", f"build-{game}"],
        cwd=REPO_ROOT,
        capture_output=False,
    )
    if result.returncode != 0:
        print(f"ERROR: build failed for evaluate_{game}", file=sys.stderr)
        sys.exit(exit_code)
    return binary


# ---------------------------------------------------------------------------
# ASCII board rendering
# ---------------------------------------------------------------------------

# Single-letter display codes for each color
_COLOR_LETTER: dict[str, str] = {
    "spR": "R", "spO": "O", "spY": "Y", "spG": "G", "spT": "T",
    "spB": "B", "spP": "P", "spD": "D", "spL": "L", "spW": "W",
    "spU": "U", "chest": "C",
    "?": "?",
}


def _cell_letter(color: str) -> str:
    """Return a single display letter for a color string.

    Handles oh dark-transform notation like "spD→spP" (returns "D").
    """
    if color in _COLOR_LETTER:
        return _COLOR_LETTER[color]
    if "→" in color:
        return "D"
    return color[:1].upper() if color else "?"


def _render_board(
    cells: list[str],
    clicked_positions: set[tuple[int, int]] | None = None,
) -> str:
    """Render a 5×5 board from a 25-element cell list.

    cells[i] is the color string for cell i (row = i//5, col = i%5).
    Clicked cells are shown in lowercase when clicked_positions is provided.
    Returns a multi-line string.
    """
    header = "    0   1   2   3   4"
    rows = [header]
    for r in range(5):
        parts = [f"{r} "]
        for c in range(5):
            idx = r * 5 + c
            letter = _cell_letter(cells[idx])
            if clicked_positions and (r, c) in clicked_positions:
                letter = letter.lower()
            parts.append(f" {letter:>2} ")
        rows.append("".join(parts))
    return "\n".join(rows)
