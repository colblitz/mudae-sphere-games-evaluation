#!/usr/bin/env python3
"""evaluate.py — Top-level evaluation runner.

Builds the harness binary if needed, runs the evaluation, and prints the
result.  After every run the script checks whether the score would change
the leaderboard and prompts whether to commit the result.

Normal usage — iterate freely, results printed:
  python scripts/evaluate.py --game oc --strategy strategies/oc/my_strategy.py

  After the run completes you will be asked whether to commit.  Answering
  yes makes two commits:
  1. "strategy: oc my_strategy.py" — commits the strategy file so it has a
     stable hash.  Skipped if the file is already committed and unmodified.
  2. "scores: oc my_strategy.py ev=78.43" — writes a scores artifact to
     scores/<game>/<timestamp>_<commit>_<basename>.json and updates
     leaderboards/<game>.json and README.md if the result enters the top 5.
     All changed files are bundled into this single commit.

Trace mode — inspect individual games:
  python scripts/evaluate.py --game oc --strategy strategies/oc/my_strategy.py --trace --n 20

  Randomly samples N games, prints the starting board (ASCII 5×5) and the
  sequence of moves for each game.  No files are written and no commit
  prompt is shown.  For ot, a specific --n-colors value is required.

Flags
-----
  --game            one of: oh oc oq ot                     (required)
  --strategy        path to the strategy file (.py/.cpp/.js) (required)
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
import platform
import re
import subprocess
import sys
import time
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
# Stateless detection
# ---------------------------------------------------------------------------

def is_strategy_stateless(path: Path) -> bool:
    """Return True if the strategy file contains 'sphere:stateless' in its first 50 lines.

    Strategies opt into the tree-walk evaluator by placing this marker anywhere
    in a comment within the first 50 lines of their source file, e.g.:
        # sphere:stateless      (Python)
        // sphere:stateless     (C++ / JS)
    """
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for i, line in enumerate(f):
                if i >= 50:
                    break
                if "sphere:stateless" in line:
                    return True
    except OSError:
        pass
    return False


# ---------------------------------------------------------------------------
# CPU model detection
# ---------------------------------------------------------------------------

def get_cpu_model() -> str:
    """Return a human-readable CPU model string (e.g. 'AMD Ryzen 9 7950X').

    Tries platform-specific sources in order; falls back to platform.processor()
    if none succeed.  Never raises — always returns a non-empty string.
    """
    system = platform.system()
    try:
        if system == "Linux":
            cpu_info = Path("/proc/cpuinfo").read_text(errors="replace")
            for line in cpu_info.splitlines():
                if line.startswith("model name"):
                    _, _, value = line.partition(":")
                    name = value.strip()
                    if name:
                        return name
        elif system == "Darwin":
            r = subprocess.run(
                ["sysctl", "-n", "machdep.cpu.brand_string"],
                capture_output=True, text=True, timeout=5,
            )
            if r.returncode == 0:
                name = r.stdout.strip()
                if name:
                    return name
        elif system == "Windows":
            r = subprocess.run(
                ["wmic", "cpu", "get", "name", "/value"],
                capture_output=True, text=True, timeout=10,
            )
            if r.returncode == 0:
                for line in r.stdout.splitlines():
                    if line.lower().startswith("name="):
                        name = line.split("=", 1)[1].strip()
                        if name:
                            return name
    except Exception:
        pass
    # Fallback
    name = platform.processor().strip()
    return name if name else "unknown"


# ---------------------------------------------------------------------------
# Build harness
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


def build_harness(game: str) -> Path:
    binary = HARNESS_DIR / f"evaluate_{game}"
    source = HARNESS_DIR / f"evaluate_{game}.cpp"
    if not source.exists():
        print(f"ERROR: harness source not found: {source}", file=sys.stderr)
        sys.exit(1)
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
        sys.exit(1)
    return binary


def build_harness_treewalk() -> Path:
    """Build (if stale) and return the path to evaluate_ot_treewalk."""
    binary = HARNESS_DIR / "evaluate_ot_treewalk"
    source = HARNESS_DIR / "evaluate_ot_treewalk.cpp"
    if not source.exists():
        print(f"ERROR: harness source not found: {source}", file=sys.stderr)
        sys.exit(1)
    if binary.exists() and binary.stat().st_mtime >= _harness_source_mtime(source):
        return binary  # up to date

    print("[build] Building evaluate_ot_treewalk ...")
    result = subprocess.run(
        ["make", "build-ot-treewalk"],
        cwd=REPO_ROOT,
        capture_output=False,
    )
    if result.returncode != 0:
        print("ERROR: build failed for evaluate_ot_treewalk", file=sys.stderr)
        sys.exit(1)
    return binary


# ---------------------------------------------------------------------------
# Run harness
# ---------------------------------------------------------------------------

def run_harness(game: str, strategy: str, extra_args: list[str],
                binary_override: Path | None = None) -> tuple[dict[str, Any], float]:
    binary = binary_override if binary_override is not None else build_harness(game)
    cmd = [str(binary), "--strategy", strategy] + extra_args

    # Add boards-dir default
    if "--boards-dir" not in " ".join(extra_args):
        cmd += ["--boards-dir", str(REPO_ROOT / "boards")]

    print(f"[run] {' '.join(cmd)}")
    t0 = time.perf_counter()
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
    harness_elapsed = time.perf_counter() - t0
    if proc.returncode != 0:
        print(f"ERROR: harness exited with code {proc.returncode}", file=sys.stderr)
        sys.exit(1)
    if result_json is None:
        print("ERROR: harness did not emit RESULT_JSON", file=sys.stderr)
        sys.exit(1)
    return result_json, harness_elapsed


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
                "n_games", "n_boards", "aggregate_ev", "seed",
                "games_per_cpu_s", "harness_elapsed_s", "cpu_model"):
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
        "| Rank | Strategy | EV | Stdev | OC Rate | Games/CPU-s | CPU | Commit | Date |",
        "|------|----------|----|-------|---------|-------------|-----|--------|------|",
    ]
    for i, e in enumerate(top5, 1):
        fname = Path(e.get("filename", "")).name
        gps = _fmt_f(e.get("games_per_cpu_s"), 0) if e.get("games_per_cpu_s") is not None else "—"
        cpu = e.get("cpu_model", "—")
        lines.append(
            f"| {i} | `{fname}` | {_fmt_f(e.get('ev'))} | {_fmt_f(e.get('stdev'))} "
            f"| {_fmt_pct(e.get('oc_rate', '—'))} | {gps} | {cpu} | `{e.get('commit','?')}` | {e.get('date','?')} |"
        )
    return "\n".join(lines)


def render_oc_oq_table(top5: list[dict], game: str) -> str:
    label = "Red Rate"
    lines = [
        f"| Rank | Strategy | EV | Stdev | {label} | Games/CPU-s | CPU | Commit | Date |",
        f"|------|----------|----|-------|{'-------' if len(label) < 9 else '-' * len(label)}|-------------|-----|--------|------|",
    ]
    for i, e in enumerate(top5, 1):
        fname = Path(e.get("filename", "")).name
        gps = _fmt_f(e.get("games_per_cpu_s"), 0) if e.get("games_per_cpu_s") is not None else "—"
        cpu = e.get("cpu_model", "—")
        lines.append(
            f"| {i} | `{fname}` | {_fmt_f(e.get('ev'))} | {_fmt_f(e.get('stdev'))} "
            f"| {_fmt_pct(e.get('red_rate', '—'))} | {gps} | {cpu} | `{e.get('commit','?')}` | {e.get('date','?')} |"
        )
    return "\n".join(lines)


def render_ot_tables(lb: dict[str, Any]) -> str:
    sections = []

    # Aggregate
    sections.append("**Aggregate (board-count weighted EV across all variants)**\n")
    agg_lines = [
        "| Rank | Strategy | Agg EV | Games/CPU-s | CPU | Commit | Date |",
        "|------|----------|--------|-------------|-----|--------|------|",
    ]
    for i, e in enumerate(lb.get("top5", []), 1):
        fname = Path(e.get("filename", "")).name
        gps = _fmt_f(e.get("games_per_cpu_s"), 0) if e.get("games_per_cpu_s") is not None else "—"
        cpu = e.get("cpu_model", "—")
        agg_lines.append(
            f"| {i} | `{fname}` | {_fmt_f(e.get('ev'))} | {gps} "
            f"| {cpu} | `{e.get('commit','?')}` | {e.get('date','?')} |"
        )
    sections.append("\n".join(agg_lines))

    # Per variant (collapsible)
    variant_parts = []
    for nc in [6, 7, 8, 9]:
        key = str(nc)
        vdata = lb.get("by_variant", {}).get(key, {})
        top5 = vdata.get("top5", [])
        v_lines = [
            f"**{nc}-color variant**\n",
            "| Rank | Strategy | EV | Stdev EV | Perfect% | All Ships% | 50/50 Loss% | Avg Clicks | Stdev Clicks | Avg Ship Clicks | Stdev Ship Clicks | Games/CPU-s | CPU | Commit | Date |",
            "|------|----------|----|----------|----------|------------|-------------|------------|--------------|-----------------|-------------------|-------------|-----|--------|------|",
        ]
        for i, e in enumerate(top5, 1):
            fname = Path(e.get("filename", "")).name
            gps = _fmt_f(e.get("games_per_cpu_s"), 0) if e.get("games_per_cpu_s") is not None else "—"
            cpu = e.get("cpu_model", "—")
            v_lines.append(
                f"| {i} | `{fname}` | {_fmt_f(e.get('ev'))} | {_fmt_f(e.get('stdev_ev','—'))} "
                f"| {_fmt_pct(e.get('perfect_rate','—'))} | {_fmt_pct(e.get('all_ships_rate','—'))} "
                f"| {_fmt_pct(e.get('loss_5050_rate','—'))} | {_fmt_f(e.get('avg_clicks','—'))} "
                f"| {_fmt_f(e.get('stdev_clicks','—'))} | {_fmt_f(e.get('avg_ship_clicks','—'))} "
                f"| {_fmt_f(e.get('stdev_ship_clicks','—'))} | {gps} "
                f"| {cpu} | `{e.get('commit','?')}` | {e.get('date','?')} |"
            )
        variant_parts.append("\n".join(v_lines))

    variants_block = "\n\n".join(variant_parts)
    sections.append(
        f"\n<details>\n<summary>Per-color variant breakdown</summary>\n\n"
        f"{variants_block}\n\n"
        f"</details>"
    )

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

        # Actual board (full true layout, unaffected by what was revealed)
        actual_board: list[str] = t.get("actual_board", ["?"] * 25)
        print("\nActual board:")
        print(_render_board(actual_board))

        # Moves
        moves: list[dict[str, Any]] = t.get("moves", [])
        if not moves:
            print("\n(no moves recorded)")
        else:
            print("\nMoves:")
            clicked: set[tuple[int, int]] = set()
            revealed_board: list[str] = list(actual_board)
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
                revealed_board[row * 5 + col] = color

            # Final board: actual board with clicked cells in lowercase
            print("\nClicks (lowercase = clicked):")
            print(_render_board(actual_board, clicked))

    print(f"\n{'='*60}\n")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    global make_entry  # noqa: PLW0603
    parser = argparse.ArgumentParser(description="Evaluate a sphere mini-game strategy.")
    parser.add_argument("--game",            required=True, choices=GAMES)
    parser.add_argument("--strategy",        required=True)
    parser.add_argument("--games",           type=int, default=1000000,
                        help="(oh) number of Monte Carlo games  default: 1000000")
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
    parser.add_argument("--yes",             action="store_true",
                        help="automatically commit the result without prompting")
    parser.add_argument("--treewalk",        action="store_true",
                        help="(ot) force the tree-walk evaluator regardless of sphere:stateless marker")
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
    t_start = time.perf_counter()

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
    # Tree-walk routing (ot non-trace mode only)
    # ------------------------------------------------------------------
    # Determine whether to use the tree-walk evaluator.
    # Conditions: game is ot, not trace mode, and either --treewalk was passed
    # or the strategy file contains the 'sphere:stateless' marker.
    use_treewalk = False
    if args.game == "ot" and not args.trace:
        if args.treewalk:
            use_treewalk = True
            print("[info] --treewalk flag set — using tree-walk evaluator")
        elif is_strategy_stateless(strategy_path):
            use_treewalk = True
            print("[info] Detected sphere:stateless marker — using tree-walk evaluator")

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
        t0_trace = time.perf_counter()
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
        trace_elapsed = time.perf_counter() - t0_trace
        if proc.returncode != 0:
            print(f"ERROR: harness exited with code {proc.returncode}", file=sys.stderr)
            sys.exit(1)
        if trace_json is None:
            print("ERROR: harness did not emit TRACE_JSON", file=sys.stderr)
            sys.exit(1)
        render_trace(args.game, trace_json)
        total_elapsed = time.perf_counter() - t_start
        n_traced = len(trace_json)
        n_threads_trace = args.threads if args.threads is not None else os.cpu_count() or 1
        cpu_s_trace = trace_elapsed * n_threads_trace
        print(f"--- Timing ---")
        print(f"  Harness run:   {trace_elapsed:.2f} s  wall  ×{n_threads_trace} threads"
              f"  →  {cpu_s_trace:.2f} CPU-s  ({n_traced / cpu_s_trace:,.0f} games/CPU-s)")
        print(f"  Total elapsed: {total_elapsed:.2f} s")
        return

    # ------------------------------------------------------------------
    # Run evaluation
    # ------------------------------------------------------------------
    if use_treewalk:
        treewalk_binary = build_harness_treewalk()
        result, harness_elapsed = run_harness(args.game, strategy_abs, extra,
                                              binary_override=treewalk_binary)
    else:
        result, harness_elapsed = run_harness(args.game, strategy_abs, extra)

    print("\n--- Result ---")
    print(json.dumps(result, indent=2))

    # ------------------------------------------------------------------
    # Timing summary
    # ------------------------------------------------------------------
    total_elapsed = time.perf_counter() - t_start
    # Determine game count: oh uses n_games, board-based games use n_boards.
    # For ot with multiple variants, sum n_boards across variants if present.
    n_games_run: int = 0
    if "n_games" in result:
        n_games_run = result["n_games"]
    elif "variants" in result:
        n_games_run = sum(v.get("n_boards", 0) for v in result["variants"])
    elif "n_boards" in result:
        n_games_run = result["n_boards"]
    n_threads_run = args.threads if args.threads is not None else os.cpu_count() or 1
    print("\n--- Timing ---")
    if n_games_run:
        cpu_seconds = harness_elapsed * n_threads_run
        games_per_cpu_s = n_games_run / cpu_seconds
        print(f"  Harness run:   {harness_elapsed:.2f} s  wall  ×{n_threads_run} threads"
              f"  →  {cpu_seconds:.2f} CPU-s  ({games_per_cpu_s:,.0f} games/CPU-s,  {n_games_run:,} games)")
    else:
        print(f"  Harness run:   {harness_elapsed:.2f} s")
    print(f"  Total elapsed: {total_elapsed:.2f} s")

    # ------------------------------------------------------------------
    # Dry-run leaderboard check — would this result change the top 5?
    # ------------------------------------------------------------------
    lb_dry = load_leaderboard(args.game)
    _would_change = False
    if args.game == "ot":
        # Check aggregate top5
        agg_entry_dry = make_entry(result, args.strategy)
        agg_entry_dry["ev"] = result.get("aggregate_ev", 0.0)
        _, _would_change = update_leaderboard_top5(lb_dry.get("top5", []), agg_entry_dry)
        if not _would_change:
            for vr in result.get("variants", []):
                nc = vr["n_colors"]
                key = str(nc)
                vdata = lb_dry.get("by_variant", {}).get(key, {})
                v_entry_dry = {**make_entry(result, args.strategy), **vr}
                v_entry_dry["ev"] = vr.get("ev", 0.0)
                _, vch = update_leaderboard_top5(vdata.get("top5", []), v_entry_dry)
                if vch:
                    _would_change = True
                    break
    else:
        entry_dry = make_entry(result, args.strategy)
        _, _would_change = update_leaderboard_top5(lb_dry.get("top5", []), entry_dry)

    if _would_change:
        print("\n*** This result would enter the top 5 leaderboard! ***")

    # ------------------------------------------------------------------
    # Prompt — always ask whether to commit after every run (skip if --yes)
    # ------------------------------------------------------------------
    if args.yes:
        print("\nCommit this result? [y/N] y  (--yes)")
    else:
        try:
            answer = input("\nCommit this result? [y/N] ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            # Non-interactive environment or user pressed Ctrl-C — skip commit.
            print()
            return

        if answer != "y":
            return

    strat_short = strategy_path.name

    # ------------------------------------------------------------------
    # Commit 1 — the strategy file
    # ------------------------------------------------------------------
    strategy_commit_hash: str | None = None
    if git_is_clean(strategy_path.resolve()):
        # File is already committed and unmodified — use current HEAD hash
        strategy_commit_hash = git_short_hash()
        print(f"[commit] Strategy file already committed (using HEAD {strategy_commit_hash})")
    else:
        msg1 = f"strategy: {args.game} {strat_short}"
        print(f"[commit 1/2] {msg1}")
        strategy_commit_hash = git_commit(msg1, [strategy_path.resolve()])
        print(f"[commit 1/2] Done — {strategy_commit_hash}")

    commit_for_entry = strategy_commit_hash or git_short_hash()

    # Collect run parameters to embed in the artifact alongside harness stats.
    run_params: dict[str, Any] = {}
    if args.game == "oh":
        run_params["n_games"] = args.games
        run_params["seed"] = args.seed
    if args.game == "ot":
        run_params["n_colors"] = args.n_colors
    run_params["harness_elapsed_s"] = round(harness_elapsed, 3)
    run_params["n_threads"] = n_threads_run
    if n_games_run:
        cpu_s = harness_elapsed * n_threads_run
        run_params["games_per_cpu_s"] = round(n_games_run / cpu_s, 1)
    run_params["cpu_model"] = get_cpu_model()

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
        for key in ("games_per_cpu_s", "harness_elapsed_s", "n_threads", "cpu_model"):
            if key in run_params:
                entry[key] = run_params[key]
        return entry
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
