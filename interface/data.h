/**
 * interface/data.h — external data file helper for C++ strategies.
 *
 * Strategies that need large precomputed files call sphere::data::fetch() in
 * strategy_init_evaluation_run() to get the local path.  The file is
 * downloaded once, verified against a SHA-256 hash, and cached in data/ for
 * all subsequent runs.
 *
 * Usage
 * -----
 *
 *   #include "../../interface/data.h"
 *
 *   std::string init_evaluation_run() override {
 *       std::string path = sphere::data::fetch(
 *           "https://huggingface.co/datasets/org/repo/resolve/main/oh_harvest_lut.bin.lzma",
 *           "abcdef1234...",          // hex SHA-256 of the hosted file
 *           "oh_harvest_lut.bin.lzma"
 *       );
 *       load_lut(path);               // your own loader
 *       return "{}";
 *   }
 *
 * Committed files (≤ ~80 MB compressed)
 * ---------------------------------------
 * Small data files committed directly to data/ do not need this helper:
 *
 *   // DATA_DIR is injected by the Makefile at compile time via -DREPO_ROOT=...
 *   std::string path = std::string(REPO_ROOT) + "/data/oh_harvest_lut.bin.lzma";
 *
 * See also strategies/oh/load_data.cpp for a working end-to-end example that
 * calls fetch() and uses the returned path.
 *
 * Hosting options for large files
 * --------------------------------
 * When a file is too large to commit (> ~80 MB compressed), host it externally
 * and pass the URL to fetch().  Options:
 *
 *   Hugging Face Datasets  — recommended.  Free, no size limit, permanent
 *                            URLs.  https://huggingface.co/docs/datasets/
 *   Google Drive           — free (15 GB).  Use a direct URL:
 *                              https://drive.google.com/uc?export=download&id=<FILE_ID>
 *                            Note: files > ~100 MB trigger a virus-scan
 *                            interstitial that breaks curl/wget.  For large
 *                            files prefer Hugging Face.
 *   Dropbox                — free (2 GB).  Change ?dl=0 → ?dl=1 in the share
 *                            link to get a direct download URL.
 *   GitHub (raw)           — free, files must be ≤ 100 MB (hard git limit).
 *                            URL: https://raw.githubusercontent.com/<org>/<repo>/main/data/<file>
 *   GitHub Releases        — free, up to 2 GB per file.  URL tied to a release tag.
 *   Zenodo                 — free, up to 50 GB, DOI-backed permanent URLs.
 *                            https://zenodo.org
 *
 * Notes
 * -----
 * - Uses curl(1) or wget(1) (whichever is on PATH) to perform the download.
 *   No libcurl dependency is required.
 * - SHA-256 is verified via sha256sum(1) (Linux) or shasum -a 256 (macOS).
 * - Progress is printed to stderr so it does not interfere with the harness
 *   stdout protocol.
 * - If the file is already present and the hash matches, no download occurs.
 * - A hash mismatch after download throws std::runtime_error.
 */

#pragma once

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace sphere {
namespace data {

namespace detail {

// ---------------------------------------------------------------------------
// Portable path to <repo>/data/
// ---------------------------------------------------------------------------

inline std::filesystem::path data_dir() {
    // REPO_ROOT is injected by the Makefile: -DREPO_ROOT=\"/abs/path\"
    // Fall back to a relative path as a best-effort for manual compilation.
#ifdef REPO_ROOT
    return std::filesystem::path(REPO_ROOT) / "data";
#else
    // Walk up from this header's location at runtime is not possible in C++
    // without platform-specific tricks.  Use CWD-relative path as fallback.
    return std::filesystem::current_path() / "data";
#endif
}

// ---------------------------------------------------------------------------
// Run a shell command and capture stdout (single line expected)
// ---------------------------------------------------------------------------

inline std::string shell_capture(const std::string& cmd) {
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return result;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe.get()))
        result += buf;
    // strip trailing whitespace
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' ||
                                result.back() == ' '))
        result.pop_back();
    return result;
}

inline int shell_run(const std::string& cmd) {
    return std::system(cmd.c_str());
}

// ---------------------------------------------------------------------------
// SHA-256 of a local file via system tool
// ---------------------------------------------------------------------------

inline std::string file_sha256(const std::filesystem::path& path) {
    std::string p = path.string();
    // Try sha256sum (Linux/coreutils), fall back to shasum -a 256 (macOS)
    std::string out = shell_capture("sha256sum \"" + p + "\" 2>/dev/null");
    if (out.empty())
        out = shell_capture("shasum -a 256 \"" + p + "\" 2>/dev/null");
    // Both tools print "<hash>  <filename>" — take the first token
    auto sp = out.find(' ');
    return (sp == std::string::npos) ? out : out.substr(0, sp);
}

// ---------------------------------------------------------------------------
// Download url → dest using curl or wget
// ---------------------------------------------------------------------------

inline void download(const std::string& url, const std::filesystem::path& dest) {
    std::string tmp = dest.string() + ".part";
    int rc = shell_run("curl -fL --progress-bar -o \"" + tmp + "\" \"" + url + "\" 2>&1");
    if (rc != 0)
        rc = shell_run("wget -q --show-progress -O \"" + tmp + "\" \"" + url + "\" 2>&1");
    if (rc != 0) {
        std::remove(tmp.c_str());
        throw std::runtime_error("[data] Download failed for: " + url +
                                 "\nMake sure curl or wget is installed and the URL is reachable.");
    }
    std::filesystem::rename(tmp, dest);
}

} // namespace detail

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Fatal error: print message and terminate immediately.
//
// A plain throw is not sufficient for the Python bridge — the harness calls
// PyErr_Clear() on exceptions from init_evaluation_run, silently substituting
// null state and letting the strategy crash later with a confusing error.
// std::exit(1) terminates the whole process before that can happen, and the
// message is visible on stderr.  C++ and JS strategies throw/reject normally
// since their bridges do not swallow errors.
// ---------------------------------------------------------------------------

[[noreturn]] inline void fatal(const std::string& msg) {
    fprintf(stderr, "\n[data] FATAL: %s\n\n", msg.c_str());
    std::exit(1);
}

/**
 * Return the local path to filename, downloading from url if needed.
 *
 * @param url      Direct download URL.
 * @param sha256   Expected lowercase hex SHA-256 of the file.
 * @param filename Basename for the cached file in data/.
 * @return         Absolute path to the verified local copy.
 * Terminates the process on download failure or hash mismatch.
 */
inline std::string fetch(const std::string& url,
                         const std::string& sha256,
                         const std::string& filename)
{
    auto dir  = detail::data_dir();
    std::filesystem::create_directories(dir);
    auto dest = dir / filename;

    if (std::filesystem::exists(dest)) {
        std::string actual = detail::file_sha256(dest);
        if (actual == sha256) return dest.string();
        fprintf(stderr, "[data] %s: hash mismatch — re-downloading.\n", filename.c_str());
    }

    fprintf(stderr, "[data] Downloading %s ...\n", filename.c_str());
    try {
        detail::download(url, dest);
    } catch (const std::exception& e) {
        fatal(
            "Could not download " + filename + ".\n"
            "  URL    : " + url + "\n"
            "  Error  : " + e.what() + "\n"
            "  If the file should be committed to this repo, restore it with:\n"
            "    git checkout data/" + filename
        );
    }

    std::string actual = detail::file_sha256(dest);
    if (actual != sha256) {
        std::filesystem::remove(dest);
        fatal(
            "SHA-256 mismatch for " + filename + " — file removed.\n"
            "  expected : " + sha256 + "\n"
            "  got      : " + actual + "\n"
            "  Check that the url and sha256 are correct."
        );
    }

    fprintf(stderr, "[data] %s: OK\n", filename.c_str());
    return dest.string();
}

} // namespace data
} // namespace sphere
