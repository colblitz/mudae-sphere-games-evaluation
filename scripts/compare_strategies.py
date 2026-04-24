#!/usr/bin/env python3
"""compare_strategies.py — Compare two strategy implementations move-by-move.

Runs both strategies in --trace mode with identical parameters (same seed,
same n-colors, same N) and diffs the move sequences game-by-game.  The
comparison is strict: any difference in chosen (row, col) is a mismatch,
even if caused by float epsilon.

Exit code:
    0   all games fully matched
    1   at least one game had a mismatch
    2   usage error or run failure

Usage
-----
    python scripts/compare_strategies.py \\
        --game ot \\
        --strategy-a strategies/ot/kelinimo_expectimax.js \\
        --strategy-b strategies/ot/kelinimo_expectimax.cpp \\
        --n-colors 6 \\
        --seed 42 \\
        --n 200

Flags
-----
    --game         one of: oh oc oq ot        (required)
    --strategy-a   path to first strategy     (required)
    --strategy-b   path to second strategy    (required)
    --n-colors     (ot only) 6|7|8|9          required for ot
    --seed         RNG seed                   default: 42
    --n            number of games            default: 100
    --threads      parallel threads           default: all cores
    --boards-dir   override boards directory
    --verbose      print every game's move sequence (not just mismatches)
    --show-board   also print the actual board for each mismatched game
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).parent.parent.resolve()
HARNESS_DIR = REPO_ROOT / "harness"
GAMES = ("oh", "oc", "oq", "ot")


# ---------------------------------------------------------------------------
# Harness build (reuses the same logic as evaluate.py)
# ---------------------------------------------------------------------------

def build_harness(game: str) -> Path:
    binary = HARNESS_DIR / f"evaluate_{game}"
    source = HARNESS_DIR / f"evaluate_{game}.cpp"
    if not source.exists():
        print(f"ERROR: harness source not found: {source}", file=sys.stderr)
        sys.exit(2)
    if binary.exists() and binary.stat().st_mtime >= source.stat().st_mtime:
        return binary
    print(f"[build] Building {binary.name} ...")
    result = subprocess.run(["make", f"build-{game}"], cwd=REPO_ROOT, capture_output=False)
    if result.returncode != 0:
        print(f"ERROR: build failed for evaluate_{game}", file=sys.stderr)
        sys.exit(2)
    return binary


# ---------------------------------------------------------------------------
# Run strategy in trace mode and return parsed TRACE_JSON
# ---------------------------------------------------------------------------

def run_trace(
    game: str,
    strategy: str,
    seed: int,
    n: int,
    n_colors: str | None,
    threads: int | None,
    boards_dir: str | None,
) -> list[dict[str, Any]]:
    binary = build_harness(game)
    cmd = [str(binary), "--strategy", str(Path(strategy).resolve()),
           "--trace", str(n)]

    if game != "oh":
        cmd += ["--seed", str(seed)]
    if game == "ot" and n_colors:
        cmd += ["--n-colors", n_colors]
    if threads is not None:
        cmd += ["--threads", str(threads)]
    if boards_dir:
        cmd += ["--boards-dir", boards_dir]
    else:
        cmd += ["--boards-dir", str(REPO_ROOT / "boards")]

    # oh needs --seed and --games
    if game == "oh":
        cmd += ["--seed", str(seed), "--games", "100000"]

    print(f"[run] {' '.join(cmd)}")

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=None, text=True, bufsize=1)
    trace_json: list[Any] | None = None
    assert proc.stdout is not None
    for line in proc.stdout:
        line = line.rstrip("\n")
        if line.startswith("TRACE_JSON:"):
            try:
                trace_json = json.loads(line[len("TRACE_JSON:"):].strip())
            except json.JSONDecodeError as e:
                print(f"ERROR: could not parse TRACE_JSON: {e}", file=sys.stderr)
                sys.exit(2)
        else:
            # Print harness progress lines but suppress them for cleaner output
            pass

    proc.wait()
    if proc.returncode != 0:
        print(f"ERROR: harness exited with code {proc.returncode}", file=sys.stderr)
        sys.exit(2)
    if trace_json is None:
        print("ERROR: harness did not emit TRACE_JSON", file=sys.stderr)
        sys.exit(2)
    return trace_json


# ---------------------------------------------------------------------------
# Move sequence extraction
# ---------------------------------------------------------------------------

def moves_of(game_obj: dict[str, Any]) -> list[tuple[int, int]]:
    """Extract the (row, col) click sequence from a trace game object.

    Only non-free moves are included in the comparison, matching what the
    strategy actually chose (free ship-cell reveals have row/col but the
    strategy did not select them — the harness auto-reveals them).
    The comparison is over all moves regardless of free-flag, since any
    divergence in the sequence matters.
    """
    return [(m["row"], m["col"]) for m in game_obj.get("moves", [])]


# ---------------------------------------------------------------------------
# Color codes for ASCII board rendering
# ---------------------------------------------------------------------------

_COLOR_LETTER = {
    "spR": "R", "spO": "O", "spY": "Y", "spG": "G", "spT": "T",
    "spB": "B", "spP": "P", "spD": "D", "spL": "L", "spW": "W",
    "spU": "U", "chest": "C", "?": "?",
}

def _cell_letter(color: str) -> str:
    if color in _COLOR_LETTER:
        return _COLOR_LETTER[color]
    return color[:1].upper() if color else "?"

def _render_board(cells: list[str]) -> str:
    header = "    0   1   2   3   4"
    rows = [header]
    for r in range(5):
        parts = [f"{r} "]
        for c in range(5):
            parts.append(f" {_cell_letter(cells[r * 5 + c]):>2} ")
        rows.append("".join(parts))
    return "\n".join(rows)


# ---------------------------------------------------------------------------
# Main comparison logic
# ---------------------------------------------------------------------------

def compare(
    game: str,
    traces_a: list[dict[str, Any]],
    traces_b: list[dict[str, Any]],
    label_a: str,
    label_b: str,
    verbose: bool = False,
    show_board: bool = False,
) -> int:
    """Compare two trace lists game-by-game.

    Returns the number of mismatched games.
    """
    n = len(traces_a)
    if len(traces_b) != n:
        print(f"WARNING: trace lengths differ: {label_a}={n}, {label_b}={len(traces_b)}")
        n = min(n, len(traces_b))

    mismatches = 0
    match_count = 0

    for gi in range(n):
        ga = traces_a[gi]
        gb = traces_b[gi]
        seq_a = moves_of(ga)
        seq_b = moves_of(gb)

        first_diff: int | None = None
        max_len = max(len(seq_a), len(seq_b))

        for ti in range(max_len):
            move_a = seq_a[ti] if ti < len(seq_a) else None
            move_b = seq_b[ti] if ti < len(seq_b) else None
            if move_a != move_b:
                first_diff = ti
                break

        game_matched = (first_diff is None and len(seq_a) == len(seq_b))

        if game_matched:
            match_count += 1
            if verbose:
                score_a = ga.get("score", "?")
                print(f"  game {gi+1:>4}: OK   len={len(seq_a)}  score={score_a}")
        else:
            mismatches += 1
            score_a = ga.get("score", "?")
            score_b = gb.get("score", "?")

            # Game header
            if game == "ot":
                nc = ga.get("n_colors", "?")
                assign = [c for c in ga.get("color_assignment", []) if c]
                hdr = f"n_colors={nc}  rare={assign}" if assign else f"n_colors={nc}"
            elif game in ("oc", "oq"):
                hdr = f"board #{ga.get('board_index', '?')}"
            else:
                hdr = f"seed={ga.get('game_seed', '?')}"

            print(f"\n  game {gi+1:>4}: MISMATCH  {hdr}  "
                  f"score_a={score_a}  score_b={score_b}")

            if show_board:
                actual = ga.get("actual_board", ["?"] * 25)
                print("  Actual board:")
                for line in _render_board(actual).splitlines():
                    print(f"    {line}")

            # Show the first divergence and a few surrounding moves
            print(f"  First divergence at turn {first_diff + 1 if first_diff is not None else '(length diff)'}:")
            show_range = range(
                max(0, (first_diff or 0) - 1),
                min(max_len, (first_diff or 0) + 4)
            )
            for ti in show_range:
                move_a = seq_a[ti] if ti < len(seq_a) else None
                move_b = seq_b[ti] if ti < len(seq_b) else None
                flag = " <<<" if ti == first_diff else ""
                print(f"    turn {ti+1:>3}: {label_a}={move_a}  {label_b}={move_b}{flag}")

            if len(seq_a) != len(seq_b):
                print(f"  Sequence lengths differ: {label_a}={len(seq_a)}  {label_b}={len(seq_b)}")

    return mismatches


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Compare two strategy implementations move-by-move using trace mode."
    )
    parser.add_argument("--game",        required=True, choices=GAMES)
    parser.add_argument("--strategy-a",  required=True, dest="strategy_a",
                        help="Path to first strategy (reference)")
    parser.add_argument("--strategy-b",  required=True, dest="strategy_b",
                        help="Path to second strategy (comparison target)")
    parser.add_argument("--n-colors",    dest="n_colors", default=None,
                        help="(ot only) 6|7|8|9")
    parser.add_argument("--seed",        type=int, default=42)
    parser.add_argument("--n",           type=int, default=100,
                        help="number of games to trace  default: 100")
    parser.add_argument("--threads",     type=int, default=None)
    parser.add_argument("--boards-dir",  default=None)
    parser.add_argument("--verbose",     action="store_true",
                        help="print all games, not just mismatches")
    parser.add_argument("--show-board",  action="store_true",
                        help="print the actual board for each mismatched game")
    args = parser.parse_args()

    # Validate ot requires n-colors
    if args.game == "ot" and not args.n_colors:
        print("ERROR: --n-colors is required for --game ot (use 6, 7, 8, or 9)",
              file=sys.stderr)
        sys.exit(2)

    # Validate strategy files exist
    for path_str, label in [(args.strategy_a, "--strategy-a"), (args.strategy_b, "--strategy-b")]:
        p = Path(path_str)
        if not p.exists():
            candidate = REPO_ROOT / "strategies" / args.game / p.name
            if candidate.exists():
                pass  # harness will resolve it
            else:
                print(f"ERROR: {label} file not found: {path_str}", file=sys.stderr)
                sys.exit(2)

    label_a = Path(args.strategy_a).name
    label_b = Path(args.strategy_b).name

    print(f"\nComparing strategies for game={args.game}  seed={args.seed}  n={args.n}"
          + (f"  n_colors={args.n_colors}" if args.n_colors else ""))
    print(f"  A: {args.strategy_a}")
    print(f"  B: {args.strategy_b}")
    print()

    # Run both strategies
    print(f"--- Running strategy A ({label_a}) ---")
    traces_a = run_trace(
        args.game, args.strategy_a,
        args.seed, args.n, args.n_colors,
        args.threads, args.boards_dir,
    )
    print(f"  got {len(traces_a)} games\n")

    print(f"--- Running strategy B ({label_b}) ---")
    traces_b = run_trace(
        args.game, args.strategy_b,
        args.seed, args.n, args.n_colors,
        args.threads, args.boards_dir,
    )
    print(f"  got {len(traces_b)} games\n")

    # Compare
    print("--- Comparing move sequences ---")
    mismatches = compare(
        args.game, traces_a, traces_b,
        label_a, label_b,
        verbose=args.verbose,
        show_board=args.show_board,
    )

    n_compared = min(len(traces_a), len(traces_b))
    matched = n_compared - mismatches
    print(f"\n{'='*60}")
    print(f"Result: {matched}/{n_compared} games fully matched")
    if mismatches == 0:
        print("All games identical.")
    else:
        print(f"{mismatches} game(s) had at least one divergent move.")
    print(f"{'='*60}\n")

    sys.exit(0 if mismatches == 0 else 1)


if __name__ == "__main__":
    main()
