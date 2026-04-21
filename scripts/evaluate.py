#!/usr/bin/env python3
"""evaluate.py — Top-level evaluation runner.

Builds the harness binary if needed, runs the evaluation, updates the
leaderboard and README if the result enters the top 5, and optionally
creates a pair of git commits to record the strategy and its scores.

Normal usage (iterate freely, no commits):
  python scripts/evaluate.py --game oc --strategy strategies/oc/my_strategy.py

When you're happy with your strategy and want to record it:
  python scripts/evaluate.py --game oc --strategy strategies/oc/my_strategy.py --commit

The --commit flow makes two commits:
  1. "strategy: oc my_strategy.py" — commits the strategy file itself, so it
     gets a stable commit hash.
  2. "scores: oc my_strategy.py ev=78.43" — runs evaluation using that hash,
     updates leaderboards/<game>.json and README.md with the correct commit
     reference, and commits those artifacts.

This ensures the score artifact always points to the exact committed version
of the strategy that produced it.

Flags
-----
  --game         one of: oh oc oq ot                     (required)
  --strategy     path to the strategy file (.py/.cpp/.js) (required)
  --commit       make two commits: strategy file + scoring artifacts
  --games N      (oh) number of Monte Carlo games         default: 100000
  --seed S       (oh) RNG seed
  --n-colors X   (ot) 6|7|8|9|all                        default: all
  --threads N    (ot) number of parallel threads          default: all cores
  --boards-dir   override boards directory
  --no-leaderboard  skip leaderboard/README update (just print results)
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from datetime import date
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).parent.parent.resolve()
LEADERBOARD_DIR = REPO_ROOT / "leaderboards"
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
    proc = subprocess.run(cmd, capture_output=False, text=True,
                          stdout=subprocess.PIPE, stderr=None)

    # Find RESULT_JSON line
    result_json: dict[str, Any] | None = None
    for line in proc.stdout.splitlines():
        print(line)  # stream progress to terminal
        if line.startswith("RESULT_JSON:"):
            try:
                result_json = json.loads(line[len("RESULT_JSON:"):].strip())
            except json.JSONDecodeError as e:
                print(f"ERROR: could not parse RESULT_JSON: {e}", file=sys.stderr)
                sys.exit(1)

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


def make_entry(result: dict[str, Any], strategy_path: str) -> dict[str, Any]:
    """Build a leaderboard entry from a harness result."""
    entry: dict[str, Any] = {
        "filename": strategy_path,
        "commit": git_short_hash(),
        "date": str(date.today()),
    }
    # Copy all numeric stats
    for key in ("ev", "stdev", "red_rate", "oc_rate",
                "avg_clicks", "perfect_rate", "all_ships_rate", "loss_5050_rate",
                "n_games", "n_boards", "aggregate_ev"):
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
            "| Rank | Strategy | EV | Stdev | Perfect% | All Ships% | 50/50 Loss% | Avg Clicks | Commit | Date |",
            "|------|----------|----|-------|----------|------------|-------------|------------|--------|------|",
        ]
        for i, e in enumerate(top5, 1):
            fname = Path(e.get("filename", "")).name
            v_lines.append(
                f"| {i} | `{fname}` | {_fmt_f(e.get('ev'))} | {_fmt_f(e.get('stdev'))} "
                f"| {_fmt_pct(e.get('perfect_rate','—'))} | {_fmt_pct(e.get('all_ships_rate','—'))} "
                f"| {_fmt_pct(e.get('loss_5050_rate','—'))} | {_fmt_f(e.get('avg_clicks','—'))} "
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
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(description="Evaluate a sphere mini-game strategy.")
    parser.add_argument("--game",            required=True, choices=GAMES)
    parser.add_argument("--strategy",        required=True)
    parser.add_argument("--commit",          action="store_true",
                        help="Make two commits: one for the strategy file, one for scores")
    parser.add_argument("--no-leaderboard",  action="store_true",
                        help="Skip leaderboard/README update (just print results)")
    parser.add_argument("--games",           type=int, default=100000,
                        help="(oh) number of Monte Carlo games  default: 100000")
    parser.add_argument("--seed",            type=int, help="(oh) RNG seed")
    parser.add_argument("--n-colors",        default="all",
                        help="(ot) 6|7|8|9|all  default: all")
    parser.add_argument("--threads",         type=int,
                        help="(ot) number of parallel threads  default: all cores")
    parser.add_argument("--boards-dir",      help="override boards directory")
    args = parser.parse_args()

    strategy_path = Path(args.strategy)
    if not strategy_path.exists():
        print(f"ERROR: strategy file not found: {args.strategy}", file=sys.stderr)
        sys.exit(1)
    strategy_abs = str(strategy_path.resolve())

    # Build harness extra args
    extra: list[str] = []
    if args.game == "oh":
        extra += ["--games", str(args.games)]
        if args.seed is not None:
            extra += ["--seed", str(args.seed)]
    if args.game == "ot":
        extra += ["--n-colors", args.n_colors]
        if args.threads is not None:
            extra += ["--threads", str(args.threads)]
    if args.boards_dir:
        extra += ["--boards-dir", args.boards_dir]

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

    if args.no_leaderboard:
        return

    # ------------------------------------------------------------------
    # Update leaderboard and README
    # ------------------------------------------------------------------
    # Use the strategy commit hash in the leaderboard entry if we just
    # committed it; otherwise use whatever HEAD currently is.
    commit_for_entry = strategy_commit_hash or git_short_hash()

    # Temporarily monkey-patch git_short_hash so make_entry picks up the
    # right hash without needing to thread it through every call.
    import scripts.evaluate as _self  # noqa: F401 — we're already in this module
    _orig_hash = git_short_hash.__code__

    # Simpler: just pass commit_for_entry directly into make_entry
    def _make_entry_with_hash(result: dict[str, Any], spath: str) -> dict[str, Any]:
        entry: dict[str, Any] = {
            "filename": spath,
            "commit": commit_for_entry,
            "date": str(date.today()),
        }
        for key in ("ev", "stdev", "red_rate", "oc_rate",
                    "avg_clicks", "perfect_rate", "all_ships_rate", "loss_5050_rate",
                    "n_games", "n_boards", "aggregate_ev"):
            if key in result:
                entry[key] = result[key]
        return entry

    # Patch make_entry for this run
    global make_entry  # noqa: PLW0603
    _original_make_entry = make_entry
    make_entry = _make_entry_with_hash  # type: ignore[assignment]

    lb_changed = update_leaderboard(args.game, result, args.strategy)

    make_entry = _original_make_entry  # restore

    if lb_changed:
        print(f"[leaderboard] Updated leaderboards/{args.game}.json (entered top 5)")
    else:
        print("[leaderboard] Result did not enter top 5 — leaderboard unchanged.")

    update_readme()

    # ------------------------------------------------------------------
    # --commit: commit 2 — scoring artifacts
    # ------------------------------------------------------------------
    if args.commit:
        ev = result.get("ev") or result.get("aggregate_ev", 0.0)
        strat_short = strategy_path.name
        lb_note = " (leaderboard updated)" if lb_changed else ""
        msg2 = f"scores: {args.game} {strat_short} ev={ev:.2f}{lb_note}"
        artifact_files = [LEADERBOARD_DIR / f"{args.game}.json", README_PATH]
        scores_hash = git_commit(msg2, artifact_files)
        print(f"[commit 2/2] {msg2}  ({scores_hash})")


if __name__ == "__main__":
    main()
