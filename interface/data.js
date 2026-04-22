/**
 * interface/data.js — external data file helper for JavaScript strategies.
 *
 * Strategies that need large precomputed files call fetch() in
 * initEvaluationRun() to get the local path.  The file is downloaded once,
 * verified against a SHA-256 hash, and cached in data/ for all subsequent
 * runs.
 *
 * Usage
 * -----
 *
 *   const { fetch: fetchData } = require("../../interface/data.js");
 *
 *   class MyOHStrategy extends OHStrategy {
 *     async initEvaluationRun() {
 *       const filePath = await fetchData({
 *         url: "https://huggingface.co/datasets/org/repo/resolve/main/oh_harvest_lut.bin.lzma",
 *         sha256: "abcdef1234...",        // hex SHA-256 of the hosted file
 *         filename: "oh_harvest_lut.bin.lzma",
 *       });
 *       return { lut: loadLut(filePath) };  // your own loader
 *     }
 *   }
 *
 * Committed files (≤ ~80 MB compressed)
 * ---------------------------------------
 * Small data files committed directly to data/ do not need this helper:
 *
 *   const path = require("path");
 *   const DATA_DIR = path.join(__dirname, "..", "data");
 *   const lutPath = path.join(DATA_DIR, "oh_harvest_lut.bin.lzma");
 *
 * Notes
 * -----
 * - Uses only Node.js built-in modules (https, fs, crypto, path) — no npm.
 * - If the file is already present and the SHA-256 matches, no download occurs.
 * - A hash mismatch after download rejects the promise with an Error.
 * - Download progress is written to stderr so it does not interfere with the
 *   harness stdout protocol.
 * - Redirects (HTTP 301/302) are followed automatically.
 */

"use strict";

const crypto = require("crypto");
const fs     = require("fs");
const https  = require("https");
const http   = require("http");
const path   = require("path");

// Resolve data/ relative to this file so it works regardless of CWD.
const DATA_DIR = path.join(__dirname, "..", "data");

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * Return the local path to filename, downloading from url if needed.
 *
 * @param {Object} options
 * @param {string} options.url      Direct download URL.
 * @param {string} options.sha256   Expected lowercase hex SHA-256 of the file.
 * @param {string} options.filename Basename for the cached file in data/.
 * @returns {Promise<string>}       Absolute path to the verified local copy.
 */
async function fetch({ url, sha256, filename }) {
  fs.mkdirSync(DATA_DIR, { recursive: true });
  const dest = path.join(DATA_DIR, filename);

  if (fs.existsSync(dest)) {
    const actual = await fileSha256(dest);
    if (actual === sha256.toLowerCase()) return dest;
    process.stderr.write(`[data] ${filename}: hash mismatch — re-downloading.\n`);
  }

  process.stderr.write(`[data] Downloading ${filename} ...\n`);
  await download(url, dest);

  const actual = await fileSha256(dest);
  if (actual !== sha256.toLowerCase()) {
    fs.unlinkSync(dest);
    throw new Error(
      `[data] SHA-256 mismatch for ${filename}\n` +
      `  expected: ${sha256.toLowerCase()}\n` +
      `  got:      ${actual}\n` +
      "The file has been removed.  Check that the url and sha256 are correct."
    );
  }

  const size = fs.statSync(dest).size;
  process.stderr.write(`[data] ${filename}: OK (${size.toLocaleString()} bytes)\n`);
  return dest;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

function fileSha256(filePath) {
  return new Promise((resolve, reject) => {
    const hash   = crypto.createHash("sha256");
    const stream = fs.createReadStream(filePath);
    stream.on("data", chunk => hash.update(chunk));
    stream.on("end",  ()    => resolve(hash.digest("hex")));
    stream.on("error", reject);
  });
}

/**
 * Download url → dest, following redirects and showing progress on stderr.
 */
function download(url, dest) {
  return new Promise((resolve, reject) => {
    const tmp = dest + ".part";

    function doRequest(currentUrl) {
      const mod = currentUrl.startsWith("https") ? https : http;
      mod.get(currentUrl, res => {
        // Follow redirects
        if (res.statusCode === 301 || res.statusCode === 302 || res.statusCode === 307) {
          const location = res.headers["location"];
          if (!location) return reject(new Error(`[data] Redirect with no Location header from ${currentUrl}`));
          res.resume();
          doRequest(location);
          return;
        }
        if (res.statusCode !== 200) {
          res.resume();
          return reject(new Error(`[data] HTTP ${res.statusCode} from ${currentUrl}`));
        }

        const total    = parseInt(res.headers["content-length"] || "0", 10);
        let downloaded = 0;
        let lastPct    = -1;

        const out = fs.createWriteStream(tmp);
        res.on("data", chunk => {
          downloaded += chunk.length;
          if (total > 0) {
            const pct = Math.min(100, Math.floor(downloaded * 100 / total));
            if (pct !== lastPct) {
              lastPct = pct;
              const filled = Math.floor(pct / 2);
              const bar    = "#".repeat(filled) + "-".repeat(50 - filled);
              const mbDone = (downloaded / (1 << 20)).toFixed(1);
              const mbTot  = (total      / (1 << 20)).toFixed(1);
              process.stderr.write(`\r[data]   [${bar}] ${String(pct).padStart(3)}%  ${mbDone}/${mbTot} MB`);
            }
          } else {
            const mbDone = (downloaded / (1 << 20)).toFixed(1);
            process.stderr.write(`\r[data]   ${mbDone} MB downloaded`);
          }
        });
        res.pipe(out);
        out.on("finish", () => {
          process.stderr.write("\n");
          fs.renameSync(tmp, dest);
          resolve();
        });
        out.on("error", err => { fs.unlinkSync(tmp); reject(err); });
        res.on("error", err => { fs.unlinkSync(tmp); reject(err); });
      }).on("error", err => {
        if (fs.existsSync(tmp)) fs.unlinkSync(tmp);
        reject(new Error(`[data] Network error: ${err.message}`));
      });
    }

    doRequest(url);
  });
}

module.exports = { fetch };
