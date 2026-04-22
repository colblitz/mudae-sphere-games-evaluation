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

/**
 * Return the local path to filename, downloading from url if needed.
 *
 * @param url      Direct download URL.
 * @param sha256   Expected lowercase hex SHA-256 of the file.
 * @param filename Basename for the cached file in data/.
 * @return         Absolute path to the verified local copy.
 * @throws std::runtime_error on download failure or hash mismatch.
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
    detail::download(url, dest);

    std::string actual = detail::file_sha256(dest);
    if (actual != sha256) {
        std::filesystem::remove(dest);
        throw std::runtime_error(
            "[data] SHA-256 mismatch for " + filename + "\n"
            "  expected: " + sha256 + "\n"
            "  got:      " + actual + "\n"
            "The file has been removed.  Check that the url and sha256 are correct."
        );
    }

    fprintf(stderr, "[data] %s: OK\n", filename.c_str());
    return dest.string();
}

} // namespace data
} // namespace sphere
