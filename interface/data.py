"""
interface/data.py — external data file helper for strategies.

Strategies that need large precomputed files (lookup tables, policy matrices,
etc.) call fetch() in init_evaluation_run() to get a local path.  The file is
downloaded once, verified against a SHA-256 hash, and cached in data/ for all
subsequent runs.

Usage
-----
::

    from interface.data import fetch

    class MyOHStrategy(OHStrategy):
        def init_evaluation_run(self):
            path = fetch(
                url="https://huggingface.co/datasets/org/repo/resolve/main/oh_harvest_lut.bin.lzma",
                sha256="abcdef1234...",   # hex digest of the file as hosted
                filename="oh_harvest_lut.bin.lzma",
            )
            lut = load_lut(path)          # your own loader
            return {"lut": lut}

Committed files (≤ ~80 MB compressed)
--------------------------------------
Small data files committed directly to data/ do not need this helper — just
load them by path:

::

    DATA_DIR = Path(__file__).resolve().parent.parent / "data"
    lut_path = DATA_DIR / "oh_harvest_lut.bin.lzma"

See also ``strategies/oh/load_data.py`` for a working end-to-end example that
calls fetch() and uses the returned path.

Hosting options for large files
--------------------------------
When a file is too large to commit (> ~80 MB compressed), host it externally
and pass the URL to fetch().  Options:

    Hugging Face Datasets  — recommended.  Free, no size limit, permanent
                             URLs.  https://huggingface.co/docs/datasets/
    Google Drive           — free (15 GB).  Use a direct URL:
                               https://drive.google.com/uc?export=download&id=<FILE_ID>
                             Note: files > ~100 MB trigger a virus-scan
                             interstitial that breaks plain urllib/curl.  For
                             large files prefer Hugging Face.
    Dropbox                — free (2 GB).  Change ?dl=0 → ?dl=1 in the share
                             link to get a direct download URL.
    GitHub (raw)           — free, files must be ≤ 100 MB (hard git limit).
                             URL format:
                               https://raw.githubusercontent.com/<org>/<repo>/main/data/<file>
    GitHub Releases        — free, up to 2 GB per file.  URL tied to a
                             release tag.
    Zenodo                 — free, up to 50 GB, DOI-backed permanent URLs.
                             https://zenodo.org

Notes
-----
- The cache directory is always ``<repo_root>/data/``.
- If the file is already present and the SHA-256 matches, no network request
  is made.  A hash mismatch triggers a fresh download (the old file is
  overwritten).
- Download progress is written to stderr so it does not interfere with the
  harness stdout protocol.
- No third-party libraries are required.
"""

from __future__ import annotations

import hashlib
import sys
import urllib.request
from pathlib import Path

# Resolve data/ relative to this file so it works regardless of CWD.
_DATA_DIR: Path = Path(__file__).resolve().parent.parent / "data"


def fetch(url: str, sha256: str, filename: str) -> Path:
    """Return the local path to *filename*, downloading from *url* if needed.

    Parameters
    ----------
    url:
        Direct download URL for the file.  Hugging Face Datasets, GitHub
        Releases, Zenodo, and S3 presigned URLs all work.
    sha256:
        Expected lowercase hex SHA-256 digest of the file.  Used both to
        skip re-downloads and to detect corrupt/truncated downloads.
    filename:
        Name of the file as it should appear in ``data/``.  Should match
        the basename of the hosted file to avoid confusion.

    Returns
    -------
    pathlib.Path
        Absolute path to the verified local copy of the file.

    Raises
    ------
    ValueError
        If the downloaded file's SHA-256 does not match *sha256*.
    urllib.error.URLError
        If the download fails (network error, 404, etc.).
    """
    _DATA_DIR.mkdir(exist_ok=True)
    dest = _DATA_DIR / filename

    if dest.exists():
        if _sha256(dest) == sha256.lower():
            return dest
        print(
            f"[data] {filename}: hash mismatch — re-downloading.",
            file=sys.stderr,
        )

    print(f"[data] Downloading {filename} ...", file=sys.stderr, flush=True)
    try:
        _download(url, dest)
    except Exception as exc:
        _fatal(
            f"Could not download {filename}.\n"
            f"  URL    : {url}\n"
            f"  Error  : {exc}\n"
            f"  If the file should be committed to this repo, restore it with:\n"
            f"    git checkout data/{filename}"
        )

    actual = _sha256(dest)
    if actual != sha256.lower():
        dest.unlink(missing_ok=True)
        _fatal(
            f"SHA-256 mismatch for {filename} — file removed.\n"
            f"  expected : {sha256.lower()}\n"
            f"  got      : {actual}\n"
            f"  Check that the url and sha256 are correct."
        )

    print(f"[data] {filename}: OK ({dest.stat().st_size:,} bytes)", file=sys.stderr)
    return dest


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _fatal(msg: str) -> None:
    """Print a clear error to stderr and hard-exit the process.

    A plain ``raise`` is not enough here: the harness Python bridge calls
    ``PyErr_Clear()`` on exceptions from ``init_evaluation_run``, silently
    substituting ``null`` state and letting the strategy crash later with a
    confusing error.  ``os._exit`` bypasses that and terminates the process
    immediately with a visible message.
    """
    import os
    print(f"\n[data] FATAL: {msg}\n", file=sys.stderr, flush=True)
    os._exit(1)

def _sha256(path: Path) -> str:
    """Return the lowercase hex SHA-256 digest of *path*."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _download(url: str, dest: Path) -> None:
    """Download *url* to *dest*, printing a progress bar to stderr."""
    tmp = dest.with_suffix(dest.suffix + ".part")

    def _reporthook(block_num: int, block_size: int, total_size: int) -> None:
        downloaded = block_num * block_size
        if total_size > 0:
            pct = min(100, downloaded * 100 // total_size)
            filled = pct // 2
            bar = "#" * filled + "-" * (50 - filled)
            mb_done = downloaded / (1 << 20)
            mb_total = total_size / (1 << 20)
            print(
                f"\r[data]   [{bar}] {pct:3d}%  {mb_done:.1f}/{mb_total:.1f} MB",
                end="",
                file=sys.stderr,
                flush=True,
            )
        else:
            mb_done = downloaded / (1 << 20)
            print(
                f"\r[data]   {mb_done:.1f} MB downloaded",
                end="",
                file=sys.stderr,
                flush=True,
            )

    try:
        urllib.request.urlretrieve(url, tmp, reporthook=_reporthook)
        print(file=sys.stderr)  # newline after progress bar
        tmp.replace(dest)
    except Exception:
        tmp.unlink(missing_ok=True)
        raise
