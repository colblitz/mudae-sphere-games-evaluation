/**
 * colblitz_bellman.cpp — Optimal Bellman DP policy for /sphere chest (oc).
 *
 * Port of the cortana3 sphere_chest.py strategy (originally by cortana3/MagiBot).
 *
 * Algorithm
 * ---------
 * The primary strategy is an exact Bellman DP policy precomputed offline against
 * all 16,800 valid boards (chest_policy.bin.lzma, ~80% efficiency vs. theoretical
 * maximum).  At runtime next_click() performs an O(1) lookup:
 *
 *   1. Collect all revealed (clicked) cells from the board.
 *   2. For each, compute key_byte = cell_idx*6 + colour_int.
 *      colour_int: spR=0, spO=1, spY=2, spG=3, spT=4, spB=5
 *   3. Sort the bytes to form the lookup key.
 *   4. Look up in policy_ (std::unordered_map<std::string, uint8_t>).
 *   5. Divmod by 5 → (row, col).
 *
 * Falls back to Bayesian greedy EV (~72% efficiency) when the lookup misses.
 *
 * External data
 * -------------
 * File    : chest_policy.bin.lzma
 * Size    : ~1.1 MB compressed
 * SHA-256 : 0589f8182a1ab48af7458e519b9941e177404d3aab3a156c1d990f35829c2759
 * Source  : committed to data/chest_policy.bin.lzma in this repo
 *
 * Binary format (chest_policy.bin.lzma):
 *   lzma-compressed blob with 5 contiguous sections (key-lengths 0–4).
 *   Section header: 4-byte big-endian uint32 entry count.
 *   Each entry: <keylen bytes> <1 value byte>
 *     key byte  = cell_idx * 6 + colour_int  (0–149)
 *     value byte = best_cell_idx             (0–24)
 *   Keys within each section are sorted.
 *
 * Performance notes
 * -----------------
 * - Policy lookup: O(1) unordered_map, key ≤ 4 bytes (SSO fits in std::string).
 * - Bayesian fallback: fully inlined, no heap allocation per click.
 * - Zone tables: flat int8_t[25*25] array (cache-friendly).
 * - Bayesian base-pool precompute: one pass over surviving red candidates.
 * - All geometry built once in init_evaluation_run().
 */

#pragma GCC optimize("O3,unroll-loops")
#pragma GCC target("avx2,bmi,bmi2,popcnt")

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <lzma.h>

#include "../../interface/strategy.h"

using namespace sphere;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr int GRID      = 5;
static constexpr int N_CELLS   = 25;
static constexpr int N_COLOURS = 6;
static constexpr int N_CLICKS  = 5;  // sections in the policy file

// SP values per colour (indexed by colour_int 0..5)
static constexpr int SP_VALUES[N_COLOURS] = { 150, 90, 55, 35, 20, 10 };

// Colour integer encoding (must match precompute_chest_policy.py)
static constexpr int COL_R = 0;
static constexpr int COL_O = 1;
static constexpr int COL_Y = 2;
static constexpr int COL_G = 3;
static constexpr int COL_T = 4;
static constexpr int COL_B = 5;
static constexpr int COL_U = -1;  // unknown / covered

// Fixed colour quotas per game
static constexpr int QUOTA_O = 2;
static constexpr int QUOTA_Y = 3;
static constexpr int QUOTA_G = 4;

// Zone encoding
static constexpr int8_t Z_RED    = 0;
static constexpr int8_t Z_ORTH   = 1;
static constexpr int8_t Z_DIAG   = 2;
static constexpr int8_t Z_ROWCOL = 3;
static constexpr int8_t Z_NONE   = 4;

// ---------------------------------------------------------------------------
// Colour name → int helper
// ---------------------------------------------------------------------------

static inline int colour_int_from_name(const std::string& name) {
    // Handle "sp" alias for red (Mudae bare red sphere)
    if (name.size() == 2 && name[0] == 's' && name[1] == 'p') return COL_R;
    if (name.size() < 3 || name[0] != 's' || name[1] != 'p') return COL_U;
    switch (name[2]) {
        case 'R': return COL_R;
        case 'O': return COL_O;
        case 'Y': return COL_Y;
        case 'G': return COL_G;
        case 'T': return COL_T;
        case 'B': return COL_B;
        default:  return COL_U;
    }
}

// ---------------------------------------------------------------------------
// Policy loader (liblzma)
// ---------------------------------------------------------------------------

static size_t load_policy_from_file(
    const std::string& path,
    std::unordered_map<std::string, uint8_t>& out)
{
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        fprintf(stderr, "[colblitz_bellman] Cannot open policy file: %s\n", path.c_str());
        return 0;
    }
    fseek(fp, 0, SEEK_END);
    long csz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    std::vector<uint8_t> compressed(static_cast<size_t>(csz));
    if (fread(compressed.data(), 1, compressed.size(), fp) != compressed.size()) {
        fclose(fp);
        fprintf(stderr, "[colblitz_bellman] Short read: %s\n", path.c_str());
        return 0;
    }
    fclose(fp);

    // Decompress — upper bound 128 MB (policy is ~6 MB decompressed)
    constexpr size_t MAX_DECOMP = 128 * 1024 * 1024;
    std::vector<uint8_t> raw(MAX_DECOMP);
    size_t in_pos = 0, out_pos = 0;
    uint64_t memlimit = UINT64_MAX;
    lzma_ret ret = lzma_stream_buffer_decode(
        &memlimit, 0, nullptr,
        compressed.data(), &in_pos, compressed.size(),
        raw.data(), &out_pos, MAX_DECOMP);
    if (ret != LZMA_OK) {
        fprintf(stderr, "[colblitz_bellman] LZMA decode error: %d\n", (int)ret);
        return 0;
    }

    const uint8_t* p   = raw.data();
    const uint8_t* end = raw.data() + out_pos;
    size_t total = 0;

    out.reserve(1 << 20);  // ~1M entries, avoids rehashing

    for (int keylen = 0; keylen < N_CLICKS; ++keylen) {
        if (p + 4 > end) break;
        uint32_t count = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
                       | (uint32_t(p[2]) <<  8) |  uint32_t(p[3]);
        p += 4;
        int entry_size = keylen + 1;
        if (p + (size_t)count * entry_size > end) break;

        if (keylen == 0) {
            if (count > 0) { out[std::string()] = p[0]; ++total; }
            p += count * entry_size;
        } else {
            for (uint32_t i = 0; i < count; ++i) {
                out[std::string(reinterpret_cast<const char*>(p), keylen)] = p[keylen];
                p += entry_size;
                ++total;
            }
        }
    }
    return total;
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------

class ColblitzBellmanOCStrategy : public OCStrategy {
public:

    void init_evaluation_run() override {
        build_geometry();
        load_policy_table();
    }

    void init_game_payload(const std::string& /*meta_json*/) override {
        // Stateless — nothing to reset between games.
    }

    void next_click(const std::vector<Cell>& board,
                    const std::string& /*meta_json*/,
                    ClickResult& out) override
    {
        // Parse board into flat arrays for O(1) access
        bool    clicked[N_CELLS]  = {};
        int     col_int[N_CELLS];
        for (int i = 0; i < N_CELLS; ++i) col_int[i] = COL_U;

        uint8_t key_bytes[4];
        int     key_len = 0;

        for (const Cell& c : board) {
            int idx = c.row * GRID + c.col;
            if (c.clicked) {
                clicked[idx] = true;
                int ci = colour_int_from_name(c.color);
                col_int[idx] = ci;
                if (ci >= 0 && key_len < 4) {
                    key_bytes[key_len++] = static_cast<uint8_t>(idx * N_COLOURS + ci);
                }
            }
        }

        // Sort key bytes for order-independent lookup
        std::sort(key_bytes, key_bytes + key_len);

        // Policy O(1) lookup
        if (!policy_.empty()) {
            const std::string key(reinterpret_cast<char*>(key_bytes), key_len);
            auto it = policy_.find(key);
            if (it != policy_.end()) {
                out.row = it->second / GRID;
                out.col = it->second % GRID;
                return;
            }
        }

        // Bayesian greedy fallback
        bayesian_fallback(clicked, col_int, out);
    }

private:
    // -----------------------------------------------------------------------
    // Types
    // -----------------------------------------------------------------------

    struct RevCell { uint8_t idx; uint8_t col; };

    struct BasePools {
        int n_orth, n_diag, n_rowcol;
        int q_O, q_Y, q_G;
    };

    // Compact list of cell indices for a (red, zone) pair
    struct ZoneCells {
        uint8_t count;
        uint8_t cells[24];
    };

    // -----------------------------------------------------------------------
    // Member data
    // -----------------------------------------------------------------------

    std::unordered_map<std::string, uint8_t> policy_;

    // zone_map_[cell_idx * N_CELLS + red_idx] → zone
    int8_t zone_map_[N_CELLS * N_CELLS];

    // zone_cells_[red_idx][zone] — cells in that zone (excluding red itself)
    ZoneCells zone_cells_[N_CELLS][5];

    // -----------------------------------------------------------------------
    // Geometry builder (called once)
    // -----------------------------------------------------------------------

    void build_geometry() {
        memset(zone_cells_, 0, sizeof(zone_cells_));

        for (int ri = 0; ri < N_CELLS; ++ri) {
            int rr = ri / GRID, rc_col = ri % GRID;
            for (int ci = 0; ci < N_CELLS; ++ci) {
                int r = ci / GRID, c = ci % GRID;
                int8_t z;
                if (ci == ri) {
                    z = Z_RED;
                } else {
                    int dr = r - rr; if (dr < 0) dr = -dr;
                    int dc = c - rc_col; if (dc < 0) dc = -dc;
                    if (dr + dc == 1)           z = Z_ORTH;
                    else if (dr == dc)          z = Z_DIAG;
                    else if (r == rr || c == rc_col) z = Z_ROWCOL;
                    else                        z = Z_NONE;
                }
                zone_map_[ci * N_CELLS + ri] = z;
                if (z != Z_RED) {
                    ZoneCells& zc = zone_cells_[ri][z];
                    zc.cells[zc.count++] = static_cast<uint8_t>(ci);
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Policy loader
    // -----------------------------------------------------------------------

    void load_policy_table() {
#ifdef REPO_ROOT
        std::string path = std::string(REPO_ROOT) + "/data/chest_policy.bin.lzma";
#else
        // Resolve relative to this source file location (fallback)
        std::string path = std::string(__FILE__);
        size_t p1 = path.rfind('/');
        if (p1 != std::string::npos) {
            size_t p2 = path.rfind('/', p1 - 1);
            if (p2 != std::string::npos)
                path = path.substr(0, p2) + "/data/chest_policy.bin.lzma";
        }
#endif
        size_t n = load_policy_from_file(path, policy_);
        if (n == 0) {
            fprintf(stderr,
                "[colblitz_bellman] WARNING: policy table empty or missing at %s\n"
                "[colblitz_bellman] Falling back to Bayesian greedy (~72%% efficiency)\n",
                path.c_str());
        } else {
            fprintf(stderr,
                "[colblitz_bellman] Loaded %zu policy states.\n", n);
        }
    }

    // -----------------------------------------------------------------------
    // Bayesian greedy fallback
    // -----------------------------------------------------------------------

    void bayesian_fallback(const bool    clicked[N_CELLS],
                           const int     col_int[N_CELLS],
                           ClickResult&  out) const
    {
        // Build revealed list
        RevCell revealed[N_CELLS];
        int n_revealed = 0;

        for (int i = 0; i < N_CELLS; ++i) {
            if (clicked[i] && col_int[i] >= 0)
                revealed[n_revealed++] = { (uint8_t)i, (uint8_t)col_int[i] };
        }

        // Bayesian posterior over red candidates (all cells except center=12)
        double weights[N_CELLS] = {};
        double total_w = 0.0;

        for (int ri = 0; ri < N_CELLS; ++ri) {
            if (ri == 12) continue;  // center (2,2) can never be red
            double w = 1.0;
            for (int j = 0; j < n_revealed && w > 0.0; ++j)
                w *= cell_likelihood(revealed[j].idx, revealed[j].col, ri, revealed, n_revealed);
            weights[ri] = w;
            total_w += w;
        }

        if (total_w <= 0.0) {
            // All candidates eliminated — pick first unclicked
            for (int i = 0; i < N_CELLS; ++i)
                if (!clicked[i]) { out.row = i / GRID; out.col = i % GRID; return; }
            out.row = out.col = 0;
            return;
        }

        for (int ri = 0; ri < N_CELLS; ++ri)
            weights[ri] /= total_w;

        // Precompute base pools for each surviving red candidate
        BasePools bp[N_CELLS];
        for (int ri = 0; ri < N_CELLS; ++ri) {
            if (weights[ri] > 0.0)
                bp[ri] = compute_base_pools(ri, revealed, n_revealed);
        }

        // Find best unclicked cell by expected SP value
        double best_ev  = -1e18;
        int    best_idx = -1;

        for (int i = 0; i < N_CELLS; ++i) {
            if (clicked[i]) continue;

            // Fast path: if all surviving candidates see this cell as Z_NONE,
            // EV is just SP_VALUES[COL_B].
            bool all_none = true;
            for (int ri = 0; ri < N_CELLS && all_none; ++ri) {
                if (weights[ri] > 0.0 && zone_map_[i * N_CELLS + ri] != Z_NONE)
                    all_none = false;
            }

            double ev = all_none ? (double)SP_VALUES[COL_B] : 0.0;
            if (!all_none) {
                for (int ri = 0; ri < N_CELLS; ++ri) {
                    if (weights[ri] > 0.0)
                        ev += weights[ri] * ev_given_red(i, ri, bp[ri]);
                }
            }

            // Tiebreak: lower cell index wins (matches Python's -(row*5+col) key)
            if (ev > best_ev || (ev == best_ev && i < best_idx)) {
                best_ev  = ev;
                best_idx = i;
            }
        }

        if (best_idx >= 0) { out.row = best_idx / GRID; out.col = best_idx % GRID; }
        else               { out.row = 0; out.col = 0; }
    }

    // -----------------------------------------------------------------------
    // Compute base pool counts for a red candidate
    // -----------------------------------------------------------------------

    BasePools compute_base_pools(int ri,
                                 const RevCell* revealed,
                                 int n_revealed) const
    {
        int n_orth = 0, n_diag = 0, n_rowcol = 0;
        int q_O = QUOTA_O, q_Y = QUOTA_Y, q_G = QUOTA_G;

        // Count unrevealed cells in each zone
        for (int8_t z = Z_ORTH; z <= Z_ROWCOL; ++z) {
            const ZoneCells& zc = zone_cells_[ri][z];
            for (int k = 0; k < zc.count; ++k) {
                uint8_t ci = zc.cells[k];
                bool is_rev = false;
                for (int j = 0; j < n_revealed; ++j)
                    if (revealed[j].idx == ci) { is_rev = true; break; }
                if (!is_rev) {
                    if      (z == Z_ORTH)   ++n_orth;
                    else if (z == Z_DIAG)   ++n_diag;
                    else                    ++n_rowcol;
                }
            }
        }

        // Subtract revealed colour quotas
        for (int j = 0; j < n_revealed; ++j) {
            int8_t z = zone_map_[revealed[j].idx * N_CELLS + ri];
            int    col = revealed[j].col;
            if (z == Z_ORTH   && col == COL_O) --q_O;
            if (z == Z_DIAG   && col == COL_Y) --q_Y;
            if ((z == Z_ORTH || z == Z_ROWCOL) && col == COL_G) --q_G;
        }

        return { n_orth, n_diag, n_rowcol, q_O, q_Y, q_G };
    }

    // -----------------------------------------------------------------------
    // Expected SP value for clicking cell_idx given red=ri
    // -----------------------------------------------------------------------

    double ev_given_red(int cell_idx, int ri, const BasePools& base) const {
        int8_t z = zone_map_[cell_idx * N_CELLS + ri];

        if (z == Z_RED)  return SP_VALUES[COL_R];
        if (z == Z_NONE) return SP_VALUES[COL_B];

        // Exclude this cell from its zone pool
        int n_orth   = base.n_orth   - (z == Z_ORTH   ? 1 : 0);
        int n_diag   = base.n_diag   - (z == Z_DIAG   ? 1 : 0);
        int n_rowcol = base.n_rowcol - (z == Z_ROWCOL ? 1 : 0);

        if (z == Z_DIAG) {
            if (n_diag <= 0) return SP_VALUES[COL_T];
            double p_y = clamp01((double)base.q_Y / n_diag);
            return p_y * SP_VALUES[COL_Y] + (1.0 - p_y) * SP_VALUES[COL_T];
        }

        if (z == Z_ROWCOL) {
            int n_gp = n_orth + n_rowcol;
            if (n_gp <= 0) return SP_VALUES[COL_T];
            double p_g = clamp01((double)base.q_G / n_gp);
            return p_g * SP_VALUES[COL_G] + (1.0 - p_g) * SP_VALUES[COL_T];
        }

        // Z_ORTH
        // n_orth is base.n_orth - 1 (this cell excluded from pool).
        // Use (n_orth + 1) as denominator so this cell competes equally with
        // the other orth cells for the orange quota.
        int n_orth_total = n_orth + 1;
        if (n_orth_total <= 0) return SP_VALUES[COL_T];
        double p_o  = clamp01((double)base.q_O / n_orth_total);
        int    n_gp = n_orth + n_rowcol;
        double p_gc = (n_gp > 0) ? clamp01((double)base.q_G / n_gp) : 0.0;
        double p_g  = (1.0 - p_o) * p_gc;
        double p_t  = std::max(0.0, 1.0 - p_o - p_g);
        return p_o * SP_VALUES[COL_O]
             + p_g * SP_VALUES[COL_G]
             + p_t * SP_VALUES[COL_T];
    }

    // -----------------------------------------------------------------------
    // P(colour | cell, red, revealed) — used for Bayesian posterior update
    // -----------------------------------------------------------------------

    double cell_likelihood(int ci, int col, int ri,
                           const RevCell* revealed,
                           int n_revealed) const
    {
        int8_t z = zone_map_[ci * N_CELLS + ri];

        if (z == Z_RED)  return (col == COL_R) ? 1.0 : 0.0;
        if (z == Z_NONE) return (col == COL_B) ? 1.0 : 0.0;
        if (col == COL_R || col == COL_B) return 0.0;

        // Pool counts excluding ci
        int n_orth = 0, n_diag = 0, n_rowcol = 0;
        int q_O = QUOTA_O, q_Y = QUOTA_Y, q_G = QUOTA_G;

        for (int8_t zz = Z_ORTH; zz <= Z_ROWCOL; ++zz) {
            const ZoneCells& zc = zone_cells_[ri][zz];
            for (int k = 0; k < zc.count; ++k) {
                uint8_t other = zc.cells[k];
                if (other == (uint8_t)ci) continue;  // exclude ci
                bool is_rev = false;
                for (int j = 0; j < n_revealed; ++j)
                    if (revealed[j].idx == other) { is_rev = true; break; }
                if (!is_rev) {
                    if      (zz == Z_ORTH)   ++n_orth;
                    else if (zz == Z_DIAG)   ++n_diag;
                    else                     ++n_rowcol;
                }
            }
        }

        for (int j = 0; j < n_revealed; ++j) {
            if (revealed[j].idx == (uint8_t)ci) continue;
            int8_t oz = zone_map_[revealed[j].idx * N_CELLS + ri];
            int    oc = revealed[j].col;
            if (oz == Z_ORTH   && oc == COL_O) --q_O;
            if (oz == Z_DIAG   && oc == COL_Y) --q_Y;
            if ((oz == Z_ORTH || oz == Z_ROWCOL) && oc == COL_G) --q_G;
        }

        if (z == Z_DIAG) {
            if (n_diag <= 0) return 0.0;
            double p_y = clamp01((double)q_Y / n_diag);
            return (col == COL_Y) ? p_y : ((col == COL_T) ? 1.0 - p_y : 0.0);
        }

        if (z == Z_ROWCOL) {
            int n_gp = n_orth + n_rowcol;
            if (n_gp <= 0) return 0.0;
            double p_g = clamp01((double)q_G / n_gp);
            return (col == COL_G) ? p_g : ((col == COL_T) ? 1.0 - p_g : 0.0);
        }

        // Z_ORTH
        int n_orth_total = n_orth + 1;
        if (n_orth_total <= 0) return 0.0;
        double p_o  = clamp01((double)q_O / n_orth_total);
        int    n_gp = n_orth + n_rowcol;
        double p_gc = (n_gp > 0) ? clamp01((double)q_G / n_gp) : 0.0;
        double p_g  = (1.0 - p_o) * p_gc;
        double p_t  = std::max(0.0, 1.0 - p_o - p_g);

        if (col == COL_O) return p_o;
        if (col == COL_G) return p_g;
        if (col == COL_T) return p_t;
        return 0.0;
    }

    static inline double clamp01(double x) {
        return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
    }
};

// ---------------------------------------------------------------------------
// C exports — modern ABI (void-returning inits, state in member variables)
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new ColblitzBellmanOCStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* inst) {
    static_cast<ColblitzBellmanOCStrategy*>(inst)->init_evaluation_run();
}

extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {
    static_cast<ColblitzBellmanOCStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<ColblitzBellmanOCStrategy*>(inst);

    std::vector<Cell> board;
    board.reserve(25);
    // Walk object by object: find each '{' that starts a cell dict.
    const char* p = board_json;
    while ((p = strchr(p, '{')) != nullptr) {
        // Find the matching '}' to bound sub-searches to this object.
        const char* obj_end = strchr(p + 1, '}');
        if (!obj_end) break;

        Cell c;
        const char* rp = strstr(p + 1, "\"row\":");
        if (!rp || rp >= obj_end) { p = obj_end + 1; continue; }
        c.row = atoi(rp + 6);

        const char* cp2 = strstr(rp + 1, "\"col\":");
        if (cp2 && cp2 < obj_end) c.col = atoi(cp2 + 6);

        const char* colp = strstr(p + 1, "\"color\":\"");
        if (colp && colp < obj_end) {
            colp += 9;
            const char* e = strchr(colp, '"');
            if (e && e < obj_end) c.color = std::string(colp, e - colp);
        }

        const char* clkp = strstr(p + 1, "\"clicked\":");
        if (clkp && clkp < obj_end) {
            clkp += 10;
            while (*clkp == ' ') ++clkp;
            c.clicked = (strncmp(clkp, "true", 4) == 0);
        }

        board.push_back(c);
        p = obj_end + 1;
    }

    ClickResult out;
    s->next_click(board, meta_json ? meta_json : "{}", out);
    buf = "{\"row\":" + std::to_string(out.row) + ",\"col\":" + std::to_string(out.col) + "}";
    return buf.c_str();
}
