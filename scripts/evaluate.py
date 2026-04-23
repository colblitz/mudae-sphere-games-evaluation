#!/usr/bin/env python3
"""evaluate.py — Top-level evaluation runner.

Builds the harness binary if needed, runs the evaluation, updates the
leaderboard and README if the result enters the top 5, and optionally
creates a pair of git commits to record the strategy and its scores.

Normal usage — iterate freely, results printed only, no files changed:
  python scripts/evaluate.py --game oc --strategy strategies/oc/my_strategy.py

When you're happy with your strategy and want to record it:
  python scripts/evaluate.py --game oc --strategy strategies/oc/my_strategy.py --commit

The --commit flow makes two commits:
  1. "strategy: oc my_strategy.py" — commits the strategy file so it has a
     stable hash.  Skipped if the file is already committed and unmodified.
  2. "scores: oc my_strategy.py ev=78.43" — runs evaluation using that hash,
     writes a scores artifact to scores/<game>/<timestamp>_<commit>_<basename>.json,
     and updates leaderboards/<game>.json and README.md if the result enters
     the top 5.  All changed files are bundled into this single commit.

Trace mode — inspect individual games:
  python scripts/evaluate.py --game oc --strategy strategies/oc/my_strategy.py --trace --n 20

  Randomly samples N games, prints the starting board (ASCII 5×5) and the
  sequence of moves for each game.  No files are written.  --commit is
  incompatible with --trace.  For ot, a specific --n-colors value is required.

Flags
-----
  --game            one of: oh oc oq ot                     (required)
  --strategy        path to the strategy file (.py/.cpp/.js) (required)
  --commit          two commits: strategy file + scoring artifacts
  --games N         (oh) number of Monte Carlo games         default: 100000
  --seed S          RNG seed (evaluation and trace mode)     default: 42
  --n-colors X      (ot) 6|7|8|9|all                        default: all
  --threads N       number of parallel threads               default: all cores
  --boards-dir      override boards directory
  --trace           enable trace mode (print per-game boards and moves)
  --n N             (trace) number of games to sample        default: 20
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from datetime import date, datetime
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).parent.parent.resolve()
LEADERBOARD_DIR = REPO_ROOT / "leaderboards"
SCORES_DIR = REPO_ROOT / "scores"
HARNESS_DIR = REPO_ROOT / "harness"
README_PATH = REPO_ROOT / "README.md"

LEADERBOARD_SENTINEL_START = "<!-- LEADERBOARD_START -->"
LEADERBOARD_SENTINEL_END = "<!-- LEADERBOARD_END -->"

GAMES = ("oh", "oc", "oq", "ot")


# ---------------------------------------------------------------------------
# Build harness
# ---------------------------------------------------------------------------

def build_harness(game: str) -> Path:
    binary = HARNESS_DIR / f"evaluate_{game}"
    source = HARNESS_DIR / f"evaluate_{game}.cpp"
    if not source.exists():
        print(f"ERROR: harness source not found: {source}", file=sys.stderr)
        sys.exit(1)
    if binary.exists() and binary.stat().st_mtime >= source.stat().st_mtime:
        return binary  # up to date

    print(f"[build] Building {binary.name} ...")
    result = subprocess.run(
        ["make", f"build-{game}"],
        cwd=REPO_ROOT,
        capture_output=False,
    )
    if result.returncode != 0:
        print(f"ERROR: build failed for evaluate_{game}", file=sys.stderr)
        sys.exit(1)
    return binary


# ---------------------------------------------------------------------------
# Run harness
# ---------------------------------------------------------------------------

def run_harness(game: str, strategy: str, extra_args: list[str]) -> dict[str, Any]:
    binary = build_harness(game)
    cmd = [str(binary), "--strategy", strategy] + extra_args

    # Add boards-dir default
    if "--boards-dir" not in " ".join(extra_args):
        cmd += ["--boards-dir", str(REPO_ROOT / "boards")]

    print(f"[run] {' '.join(cmd)}")
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=None, text=True,
                            bufsize=1)

    # Stream output line-by-line as it arrives
    result_json: dict[str, Any] | None = None
    assert proc.stdout is not None
    for line in proc.stdout:
        line = line.rstrip("\n")
        print(line, flush=True)
        if line.startswith("RESULT_JSON:"):
            try:
                result_json = json.loads(line[len("RESULT_JSON:"):].strip())
            except json.JSONDecodeError as e:
                print(f"ERROR: could not parse RESULT_JSON: {e}", file=sys.stderr)
                sys.exit(1)

    proc.wait()
    if proc.returncode != 0:
        print(f"ERROR: harness exited with code {proc.returncode}", file=sys.stderr)
        sys.exit(1)
    if result_json is None:
        print("ERROR: harness did not emit RESULT_JSON", file=sys.stderr)
        sys.exit(1)
    return result_json


# ---------------------------------------------------------------------------
# Git helpers
# ---------------------------------------------------------------------------

def git_short_hash() -> str:
    try:
        r = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=REPO_ROOT, capture_output=True, text=True
        )
        return r.stdout.strip() if r.returncode == 0 else "unknown"
    except Exception:
        return "unknown"


def git_is_clean(path: Path) -> bool:
    """Return True if the given file has no uncommitted changes."""
    r = subprocess.run(
        ["git", "status", "--porcelain", str(path.relative_to(REPO_ROOT))],
        cwd=REPO_ROOT, capture_output=True, text=True
    )
    return r.returncode == 0 and r.stdout.strip() == ""


def git_commit(message: str, files: list[Path]) -> str:
    """Stage files and create a commit. Returns the new short hash."""
    paths = [str(f.relative_to(REPO_ROOT)) for f in files]
    subprocess.run(["git", "add"] + paths, cwd=REPO_ROOT, check=True)
    subprocess.run(["git", "commit", "-m", message], cwd=REPO_ROOT, check=True)
    return git_short_hash()


# ---------------------------------------------------------------------------
# Leaderboard logic
# ---------------------------------------------------------------------------

def load_leaderboard(game: str) -> dict[str, Any]:
    path = LEADERBOARD_DIR / f"{game}.json"
    if path.exists():
        return json.loads(path.read_text())
    # Default empty leaderboard
    lb: dict[str, Any] = {"game": game, "metric": "ev", "updated": "", "top5": []}
    if game == "ot":
        lb["by_variant"] = {str(nc): {"n_colors": nc, "top5": []} for nc in [6, 7, 8, 9]}
    return lb


def save_leaderboard(game: str, lb: dict[str, Any]) -> None:
    path = LEADERBOARD_DIR / f"{game}.json"
    path.write_text(json.dumps(lb, indent=2) + "\n")


def write_scores_artifact(
    game: str,
    result: dict[str, Any],
    strategy_path: str,
    commit_hash: str,
    extra_params: dict[str, Any],
) -> Path:
    """Write a scores artifact JSON to scores/<game>/<timestamp>_<commit>_<basename>.json.

    The artifact records everything needed to reproduce and audit the run:
    timestamp, commit hash, filename, all harness stats, and run parameters.
    Returns the path of the written file.
    """
    now = datetime.now()
    timestamp = now.strftime("%Y%m%d_%H%M%S")
    basename = Path(strategy_path).stem  # filename without extension
    artifact_name = f"{timestamp}_{commit_hash}_{basename}.json"

    out_dir = SCORES_DIR / game
    out_dir.mkdir(parents=True, exist_ok=True)
    artifact_path = out_dir / artifact_name

    artifact: dict[str, Any] = {
        "timestamp": now.isoformat(timespec="seconds"),
        "commit": commit_hash,
        "filename": strategy_path,
        "game": game,
    }
    # Merge all harness result fields (ev, stdev, red_rate, variants, etc.)
    artifact.update(result)
    # Merge run parameters (seed, n_games, n_colors, etc.) — these override
    # nothing in result since they use different keys, but we keep result fields
    # authoritative by updating params last only for keys not already present.
    for k, v in extra_params.items():
        if k not in artifact:
            artifact[k] = v

    artifact_path.write_text(json.dumps(artifact, indent=2) + "\n")
    print(f"[scores] Written {artifact_path.relative_to(REPO_ROOT)}")
    return artifact_path


def make_entry(result: dict[str, Any], strategy_path: str) -> dict[str, Any]:
    """Build a leaderboard entry from a harness result."""
    entry: dict[str, Any] = {
        "filename": strategy_path,
        "commit": git_short_hash(),
        "date": str(date.today()),
    }
    # Copy all numeric stats
    for key in ("ev", "stdev", "stdev_ev", "red_rate", "oc_rate",
                "avg_clicks", "stdev_clicks", "avg_ship_clicks", "stdev_ship_clicks",
                "perfect_rate", "all_ships_rate", "loss_5050_rate",
                "n_games", "n_boards", "aggregate_ev", "seed"):
        if key in result:
            entry[key] = result[key]
    return entry


def update_leaderboard_top5(top5: list[dict], new_entry: dict) -> tuple[list[dict], bool]:
    """Insert new_entry into top5 (sorted by ev desc), keep top 5. Returns (new_list, changed)."""
    # Remove any existing entry for the same filename
    filtered = [e for e in top5 if e.get("filename") != new_entry["filename"]]
    filtered.append(new_entry)
    filtered.sort(key=lambda e: e.get("ev", 0.0), reverse=True)
    new_top5 = filtered[:5]
    changed = new_top5 != top5
    return new_top5, changed


def update_leaderboard(game: str, result: dict[str, Any], strategy_path: str) -> bool:
    """Update leaderboard JSON for the given game. Returns True if leaderboard changed."""
    lb = load_leaderboard(game)
    lb["updated"] = str(date.today())
    changed = False

    if game == "ot":
        # Update aggregate top5
        agg_entry = make_entry(result, strategy_path)
        agg_entry["ev"] = result.get("aggregate_ev", 0.0)
        new_agg, ch = update_leaderboard_top5(lb.get("top5", []), agg_entry)
        if ch:
            lb["top5"] = new_agg
            changed = True

        # Update per-variant top5
        for vr in result.get("variants", []):
            nc = vr["n_colors"]
            key = str(nc)
            if "by_variant" not in lb:
                lb["by_variant"] = {}
            if key not in lb["by_variant"]:
                lb["by_variant"][key] = {"n_colors": nc, "top5": []}
            v_entry = {**make_entry(result, strategy_path), **vr}
            v_entry["ev"] = vr.get("ev", 0.0)
            new_v, vch = update_leaderboard_top5(lb["by_variant"][key]["top5"], v_entry)
            if vch:
                lb["by_variant"][key]["top5"] = new_v
                changed = True
    else:
        entry = make_entry(result, strategy_path)
        new_top5, ch = update_leaderboard_top5(lb.get("top5", []), entry)
        if ch:
            lb["top5"] = new_top5
            changed = True

    save_leaderboard(game, lb)
    return changed


# ---------------------------------------------------------------------------
# README leaderboard table rendering
# ---------------------------------------------------------------------------

def _fmt_pct(v: Any) -> str:
    if isinstance(v, float):
        return f"{v * 100:.1f}%"
    return str(v)


def _fmt_f(v: Any, d: int = 2) -> str:
    if isinstance(v, (int, float)):
        return f"{v:.{d}f}"
    return str(v) if v is not None else "—"


def render_oh_table(top5: list[dict]) -> str:
    lines = [
        "| Rank | Strategy | EV | Stdev | OC Rate | Commit | Date |",
        "|------|----------|----|-------|---------|--------|------|",
    ]
    for i, e in enumerate(top5, 1):
        fname = Path(e.get("filename", "")).name
        lines.append(
            f"| {i} | `{fname}` | {_fmt_f(e.get('ev'))} | {_fmt_f(e.get('stdev'))} "
            f"| {_fmt_pct(e.get('oc_rate', '—'))} | `{e.get('commit','?')}` | {e.get('date','?')} |"
        )
    return "\n".join(lines)


def render_oc_oq_table(top5: list[dict], game: str) -> str:
    label = "Red Rate"
    lines = [
        f"| Rank | Strategy | EV | Stdev | {label} | Commit | Date |",
        f"|------|----------|----|-------|{'-------' if len(label) < 9 else '-' * len(label)}|--------|------|",
    ]
    for i, e in enumerate(top5, 1):
        fname = Path(e.get("filename", "")).name
        lines.append(
            f"| {i} | `{fname}` | {_fmt_f(e.get('ev'))} | {_fmt_f(e.get('stdev'))} "
            f"| {_fmt_pct(e.get('red_rate', '—'))} | `{e.get('commit','?')}` | {e.get('date','?')} |"
        )
    return "\n".join(lines)


def render_ot_tables(lb: dict[str, Any]) -> str:
    sections = []

    # Aggregate
    sections.append("**Aggregate (board-count weighted EV across all variants)**\n")
    agg_lines = [
        "| Rank | Strategy | Agg EV | Commit | Date |",
        "|------|----------|--------|--------|------|",
    ]
    for i, e in enumerate(lb.get("top5", []), 1):
        fname = Path(e.get("filename", "")).name
        agg_lines.append(
            f"| {i} | `{fname}` | {_fmt_f(e.get('ev'))} "
            f"| `{e.get('commit','?')}` | {e.get('date','?')} |"
        )
    sections.append("\n".join(agg_lines))

    # Per variant
    for nc in [6, 7, 8, 9]:
        key = str(nc)
        vdata = lb.get("by_variant", {}).get(key, {})
        top5 = vdata.get("top5", [])
        sections.append(f"\n**{nc}-color variant**\n")
        v_lines = [
            "| Rank | Strategy | EV | Stdev EV | Perfect% | All Ships% | 50/50 Loss% | Avg Clicks | Stdev Clicks | Avg Ship Clicks | Stdev Ship Clicks | Commit | Date |",
            "|------|----------|----|----------|----------|------------|-------------|------------|--------------|-----------------|-------------------|--------|------|",
        ]
        for i, e in enumerate(top5, 1):
            fname = Path(e.get("filename", "")).name
            v_lines.append(
                f"| {i} | `{fname}` | {_fmt_f(e.get('ev'))} | {_fmt_f(e.get('stdev_ev','—'))} "
                f"| {_fmt_pct(e.get('perfect_rate','—'))} | {_fmt_pct(e.get('all_ships_rate','—'))} "
                f"| {_fmt_pct(e.get('loss_5050_rate','—'))} | {_fmt_f(e.get('avg_clicks','—'))} "
                f"| {_fmt_f(e.get('stdev_clicks','—'))} | {_fmt_f(e.get('avg_ship_clicks','—'))} "
                f"| {_fmt_f(e.get('stdev_ship_clicks','—'))} "
                f"| `{e.get('commit','?')}` | {e.get('date','?')} |"
            )
        sections.append("\n".join(v_lines))

    return "\n".join(sections)


def render_leaderboard_section() -> str:
    parts = []
    for game in GAMES:
        lb = load_leaderboard(game)
        game_labels = {
            "oh": "/sphere harvest (oh)",
            "oc": "/sphere chest (oc)",
            "oq": "/sphere quest (oq)",
            "ot": "/sphere trace (ot)",
        }
        parts.append(f"### {game_labels[game]}\n")
        if game == "oh":
            parts.append(render_oh_table(lb.get("top5", [])))
        elif game in ("oc", "oq"):
            parts.append(render_oc_oq_table(lb.get("top5", []), game))
        elif game == "ot":
            parts.append(render_ot_tables(lb))
        parts.append("")
    return "\n".join(parts)


def update_readme() -> None:
    if not README_PATH.exists():
        return
    content = README_PATH.read_text()
    new_section = (
        LEADERBOARD_SENTINEL_START + "\n"
        + render_leaderboard_section()
        + "\n" + LEADERBOARD_SENTINEL_END
    )
    pattern = re.compile(
        re.escape(LEADERBOARD_SENTINEL_START) + r".*?" + re.escape(LEADERBOARD_SENTINEL_END),
        re.DOTALL,
    )
    if pattern.search(content):
        new_content = pattern.sub(new_section, content)
    else:
        # Append if sentinels not present yet
        new_content = content + "\n" + new_section + "\n"
    README_PATH.write_text(new_content)
    print("[readme] Updated leaderboard tables in README.md")


# ---------------------------------------------------------------------------
# Trace rendering
# ---------------------------------------------------------------------------

# Single-letter color codes for ASCII board display
_COLOR_LETTER: dict[str, str] = {
    "spR": "R", "spO": "O", "spY": "Y", "spG": "G", "spT": "T",
    "spB": "B", "spP": "P", "spD": "D", "spL": "L", "spW": "W",
    "spU": "U", "chest": "C",
    "?": "?",
}

def _cell_letter(color: str) -> str:
    """Return a single display letter for a color string (handles spD→spX variants)."""
    if color in _COLOR_LETTER:
        return _COLOR_LETTER[color]
    # Handle oh dark-transform notation like "spD→spP"
    if "→" in color:
        return "D"
    return color[:1].upper() if color else "?"


def _render_board(cells: list[str], clicked_positions: set[tuple[int, int]] | None = None) -> str:
    """Render a 5×5 board from a 25-element cell list.

    cells[i] is the color string for cell i (row = i//5, col = i%5).
    Clicked cells are shown in lowercase.
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


def _meta_str(meta: dict[str, Any], game: str) -> str:
    """Format meta dict as a compact key=val string, game-appropriate fields only."""
    parts = []
    if game in ("oc", "oh", "oq") and "clicks_left" in meta:
        parts.append(f"clicks_left={meta['clicks_left']}")
    if game == "oq" and "purples_found" in meta:
        parts.append(f"purples={meta['purples_found']}")
    if game == "ot":
        if "ships_hit" in meta:
            parts.append(f"ships_hit={meta['ships_hit']}")
        if "blues_used" in meta:
            parts.append(f"blues_used={meta['blues_used']}")
    return "  ".join(parts)


def render_trace(game: str, traces: list[dict[str, Any]]) -> None:
    """Print a human-readable trace of sampled games to stdout."""
    total = len(traces)
    print(f"\n{'='*60}")
    print(f"  Trace: {game}  ({total} game{'s' if total != 1 else ''})")
    print(f"{'='*60}")

    for gi, t in enumerate(traces, 1):
        score = t.get("score", 0.0)

        # Game header
        if game == "oh":
            chest_tag = "  [has chest]" if t.get("has_chest") else ""
            seed_tag = f"  seed={t.get('game_seed', '?')}"
            print(f"\n--- Game {gi}/{total}{chest_tag}{seed_tag}  score={score:.0f} SP ---")
        elif game == "ot":
            nc = t.get("n_colors", "?")
            assign = [c for c in t.get("color_assignment", []) if c]
            assign_tag = f"  rare={assign}" if assign else ""
            print(f"\n--- Game {gi}/{total}  n_colors={nc}{assign_tag}  score={score:.0f} SP ---")
        else:
            board_idx = t.get("board_index", "?")
            print(f"\n--- Game {gi}/{total}  board #{board_idx}  score={score:.0f} SP ---")

        # Initial board
        initial_board: list[str] = t.get("initial_board", ["?"] * 25)
        print("\nInitial board:")
        print(_render_board(initial_board))

        # Moves
        moves: list[dict[str, Any]] = t.get("moves", [])
        if not moves:
            print("\n(no moves recorded)")
        else:
            print("\nMoves:")
            clicked: set[tuple[int, int]] = set()
            for m in moves:
                mn    = m.get("move_num", "?")
                row   = m.get("row", 0)
                col   = m.get("col", 0)
                color = m.get("color", "?")
                delta = m.get("sp_delta", 0.0)
                total_score = m.get("running_score", 0.0)
                meta  = m.get("meta", {})
                free  = m.get("free", False)

                letter = _cell_letter(color)
                meta_s = _meta_str(meta, game)
                free_tag = "  [free]" if free else ""
                delta_sign = "+" if delta >= 0 else ""
                print(f"  #{mn:>2}  ({row},{col}) → {letter:<8}  "
                      f"{delta_sign}{delta:.0f} SP  [total {total_score:.0f}]"
                      f"  {meta_s}{free_tag}")
                clicked.add((row, col))

            # Final board state (initial board with clicked cells lower-cased)
            print("\nFinal board (clicked cells in lowercase):")
            print(_render_board(initial_board, clicked))

    print(f"\n{'='*60}\n")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(description="Evaluate a sphere mini-game strategy.")
    parser.add_argument("--game",            required=True, choices=GAMES)
    parser.add_argument("--strategy",        required=True)
    parser.add_argument("--commit",          action="store_true",
                        help="Make two commits: one for the strategy file, one for scores")
    parser.add_argument("--games",           type=int, default=100000,
                        help="(oh) number of Monte Carlo games  default: 100000")
    parser.add_argument("--seed",            type=int, default=42,
                        help="RNG seed (evaluation and trace mode)  default: 42")
    parser.add_argument("--n-colors",        default="all",
                        help="(ot) 6|7|8|9|all  default: all")
    parser.add_argument("--threads",         type=int,
                        help="number of parallel threads  default: all cores")
    parser.add_argument("--boards-dir",      help="override boards directory")
    parser.add_argument("--trace",           action="store_true",
                        help="Trace mode: print per-game boards and moves instead of aggregate stats")
    parser.add_argument("--n",               type=int, default=20,
                        help="(trace) number of games to sample  default: 20")
    args = parser.parse_args()

    strategy_path = Path(args.strategy)
    if not strategy_path.exists():
        # Try resolving as a bare filename within strategies/<game>/
        candidate = REPO_ROOT / "strategies" / args.game / strategy_path.name
        if candidate.exists():
            strategy_path = candidate
        else:
            print(f"ERROR: strategy file not found: {args.strategy}", file=sys.stderr)
            sys.exit(1)
    strategy_abs = str(strategy_path.resolve())

    # --trace is incompatible with --commit
    if args.trace and args.commit:
        print("ERROR: --trace and --commit are incompatible.", file=sys.stderr)
        sys.exit(1)

    # --trace for ot requires a specific n-colors value
    if args.trace and args.game == "ot" and args.n_colors == "all":
        print("ERROR: --trace for ot requires a specific --n-colors value (6, 7, 8, or 9).",
              file=sys.stderr)
        sys.exit(1)

    # Build harness extra args
    extra: list[str] = []
    if args.game == "oh":
        extra += ["--games", str(args.games)]
        extra += ["--seed", str(args.seed)]
    if args.game == "ot":
        extra += ["--n-colors", args.n_colors]
    if args.threads is not None:
        extra += ["--threads", str(args.threads)]
    if args.boards_dir:
        extra += ["--boards-dir", args.boards_dir]

    # ------------------------------------------------------------------
    # TRACE MODE
    # ------------------------------------------------------------------
    if args.trace:
        trace_extra = extra.copy()
        trace_extra += ["--trace", str(args.n)]
        # Seed is forwarded for all games (oc/oq/ot use it for board sampling;
        # oh already gets it via --seed above for oh, add it for others)
        if args.game != "oh":
            trace_extra += ["--seed", str(args.seed)]
        binary = build_harness(args.game)
        cmd = [str(binary), "--strategy", strategy_abs] + trace_extra
        if "--boards-dir" not in " ".join(trace_extra):
            cmd += ["--boards-dir", str(REPO_ROOT / "boards")]
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
                    sys.exit(1)
            else:
                print(line, flush=True)
        proc.wait()
        if proc.returncode != 0:
            print(f"ERROR: harness exited with code {proc.returncode}", file=sys.stderr)
            sys.exit(1)
        if trace_json is None:
            print("ERROR: harness did not emit TRACE_JSON", file=sys.stderr)
            sys.exit(1)
        render_trace(args.game, trace_json)
        return

    # ------------------------------------------------------------------
    # --commit: commit 1 — the strategy file
    # ------------------------------------------------------------------
    strategy_commit_hash: str | None = None
    if args.commit:
        if git_is_clean(strategy_path.resolve()):
            # File is already committed and unmodified — use current HEAD hash
            strategy_commit_hash = git_short_hash()
            print(f"[commit] Strategy file already committed (using HEAD {strategy_commit_hash})")
        else:
            strat_short = strategy_path.name
            msg1 = f"strategy: {args.game} {strat_short}"
            print(f"[commit 1/2] {msg1}")
            strategy_commit_hash = git_commit(msg1, [strategy_path.resolve()])
            print(f"[commit 1/2] Done — {strategy_commit_hash}")

    # ------------------------------------------------------------------
    # Run evaluation
    # ------------------------------------------------------------------
    result = run_harness(args.game, strategy_abs, extra)

    print("\n--- Result ---")
    print(json.dumps(result, indent=2))

    if not args.commit:
        # Normal run: print only, no file changes.
        return

    # ------------------------------------------------------------------
    # --commit: write scores artifact, update leaderboard + README, commit 2
    # ------------------------------------------------------------------
    commit_for_entry = strategy_commit_hash or git_short_hash()

    # Collect run parameters to embed in the artifact alongside harness stats.
    run_params: dict[str, Any] = {}
    if args.game == "oh":
        run_params["n_games"] = args.games
        run_params["seed"] = args.seed
    if args.game == "ot":
        run_params["n_colors"] = args.n_colors

    artifact_path = write_scores_artifact(
        args.game, result, args.strategy, commit_for_entry, run_params
    )

    # Use the strategy commit hash in the leaderboard entry (not the scores commit).
    def _make_entry_with_hash(res: dict[str, Any], spath: str) -> dict[str, Any]:
        entry: dict[str, Any] = {
            "filename": spath,
            "commit": commit_for_entry,
            "date": str(date.today()),
        }
        for key in ("ev", "stdev", "stdev_ev", "red_rate", "oc_rate",
                    "avg_clicks", "stdev_clicks", "avg_ship_clicks", "stdev_ship_clicks",
                    "perfect_rate", "all_ships_rate", "loss_5050_rate",
                    "n_games", "n_boards", "aggregate_ev", "seed"):
            if key in res:
                entry[key] = res[key]
        return entry

    global make_entry  # noqa: PLW0603
    _original_make_entry = make_entry
    make_entry = _make_entry_with_hash  # type: ignore[assignment]

    lb_changed = update_leaderboard(args.game, result, args.strategy)

    make_entry = _original_make_entry  # restore

    if lb_changed:
        print(f"[leaderboard] Updated leaderboards/{args.game}.json (entered top 5)")
        update_readme()
    else:
        print("[leaderboard] Result did not enter top 5 — leaderboard unchanged.")

    ev = result.get("ev") or result.get("aggregate_ev", 0.0)
    strat_short = strategy_path.name
    lb_note = " (leaderboard updated)" if lb_changed else ""
    msg2 = f"scores: {args.game} {strat_short} ev={ev:.2f}{lb_note}"

    # Always commit the scores artifact; also commit leaderboard+README if changed.
    commit2_files: list[Path] = [artifact_path]
    if lb_changed:
        commit2_files += [LEADERBOARD_DIR / f"{args.game}.json", README_PATH]
    scores_hash = git_commit(msg2, commit2_files)
    print(f"[commit 2/2] {msg2}  ({scores_hash})")


if __name__ == "__main__":
    main()
