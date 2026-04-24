/**
 * colblitz_bellman.cpp — Bellman DP optimal policy strategy for /sphere harvest (oh) — C++.
 *
 * Ports cortana3/strategy/sphere_harvest.py and MagiBot/magibot/policy/harvest.py
 * to C++.  The strategy reads a precomputed dense uint8 policy table from disk
 * via pread() — one byte per lookup, zero RAM used for the array itself.
 *
 * Decision priority:
 *   1. Purple cells (spP) — free clicks, always take immediately.
 *   2. DP optimal policy — single pread() into the ~6 GB policy file.
 *   3. Greedy EV fallback — highest expected-sphere-value cell given remaining
 *      budget; used when the policy file is missing or the state is absent.
 *
 * Per-game state tracked by the strategy:
 *   chest_found — true once a (color="spU", clicked=true) cell appears on the
 *                 board.  That cell is the OH chest, which can only fire once.
 *                 Passed to the DP lookup so the policy can value covered cells
 *                 correctly on chest vs. non-chest boards.
 *
 * External data
 * -------------
 * File    : harvest_compact_base_20260423_143426.bin
 * Size    : ~62 KB
 * SHA-256 : 1ca124cfd18ab55a6b42c946b97e227767000ce385223434d272b1af79e2eff6
 * Source  : (commit to data/ directly — file is small)
 *
 * File    : harvest_policy_20260423_143426.bin
 * Size    : ~5.7 GB uncompressed
 * SHA-256 : 2e23b686d31ee8b125cdf135d02feeaffad4be73593125a96b77c4a832024384
 * Source  : (host externally — too large to commit; place in data/ manually)
 *
 * Both files are loaded from data/ relative to REPO_ROOT (injected by the
 * Makefile via -DREPO_ROOT=...).  If the policy file is absent the strategy
 * falls back to the greedy EV heuristic for every decision.
 *
 * COUPLING WARNING: The compact base enumeration order and dimension constants
 * below must match generate_harvest_policy_dense.cpp / precompute_harvest_policy.cpp
 * and the Python implementations exactly.  COMPACT_BASE_FORMAT_VERSION = 1.
 */

#pragma GCC optimize("O3,unroll-loops")
#pragma GCC optimize("fp-contract=off")   // disable FMA to match JS/Python IEEE 754 rounding

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "../../interface/strategy.h"

using namespace sphere;

// ---------------------------------------------------------------------------
// DP dimension constants — must match precompute_harvest_policy.cpp exactly
// ---------------------------------------------------------------------------

static constexpr int K_MAX         = 5;   // max clicks_left
static constexpr int N_FLAT_MAX    = 14;  // max visible flat cells in DP state
static constexpr int N_FLAT_VALS   = 6;   // spG spY spL spO spR spW
static constexpr int N_COV_MAX     = 15;
static constexpr int N_BLUE_MAX    = 24;
static constexpr int N_TEAL_MAX    = 25;
static constexpr int N_DARK_MAX    = 6;
static constexpr int BASE_CELL_CAP = 25;  // nc+nb+nt+nd <= this
static constexpr int HEADER_SIZE   = 33;  // bytes

// Expected N_BASE_COMPACT = 15,735 (validated at runtime)
// N_FLAT_TUPLES = 38,760 (validated at runtime)

// DP action codes (stored byte = action + 1; 0 = absent/unreachable)
static constexpr int ACTION_FLAT    = 0;
static constexpr int ACTION_BLUE    = 1;
static constexpr int ACTION_TEAL    = 2;
static constexpr int ACTION_DARK    = 3;
static constexpr int ACTION_COVERED = 4;

// ---------------------------------------------------------------------------
// Flat color index mapping (index 0..5 → spG spY spL spO spR spW)
// Ordered by EV ascending: 35 55 76 90 150 500
// ---------------------------------------------------------------------------

static constexpr int FLAT_IDX_spG = 0;
static constexpr int FLAT_IDX_spY = 1;
static constexpr int FLAT_IDX_spL = 2;
static constexpr int FLAT_IDX_spO = 3;
static constexpr int FLAT_IDX_spR = 4;
static constexpr int FLAT_IDX_spW = 5;

static inline int color_to_flat_idx(const std::string& c) {
    if (c == "spG") return FLAT_IDX_spG;
    if (c == "spY") return FLAT_IDX_spY;
    if (c == "spL") return FLAT_IDX_spL;
    if (c == "spO") return FLAT_IDX_spO;
    if (c == "spR") return FLAT_IDX_spR;
    if (c == "spW") return FLAT_IDX_spW;
    return -1;
}

static const char* flat_idx_to_color(int idx) {
    switch (idx) {
        case 0: return "spG";
        case 1: return "spY";
        case 2: return "spL";
        case 3: return "spO";
        case 4: return "spR";
        case 5: return "spW";
        default: return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Greedy EV constants (from cortana3/strategy/sphere_harvest.py)
// ---------------------------------------------------------------------------

static constexpr double DEFAULT_VALUES[] = {
    // indexed by flat idx for fast access; others looked up by name
    35.0,  // spG [0]
    55.0,  // spY [1]
    76.03, // spL [2]
    90.0,  // spO [3]
    150.0, // spR [4]
    300.0, // spW [5]
};

static inline double ev_of_color(const std::string& color) {
    int fi = color_to_flat_idx(color);
    if (fi >= 0) return DEFAULT_VALUES[fi];
    if (color == "spB") return 10.0;
    if (color == "spT") return 20.0;
    if (color == "spD") return 104.30;
    if (color == "spP") return 5.0;
    return 0.0;
}

// DEFAULT_APPEARANCE_RATES (from 12425 games, excluding spU)
// Used for ev_of_covered_button() in the greedy fallback
static constexpr double APP_RATE_spB = 0.5453;
static constexpr double APP_RATE_spT = 0.2346;
static constexpr double APP_RATE_spG = 0.0786;
static constexpr double APP_RATE_spP = 0.0393;
static constexpr double APP_RATE_spL = 0.0296;
static constexpr double APP_RATE_spY = 0.0259;
static constexpr double APP_RATE_spD = 0.0145;
static constexpr double APP_RATE_spO = 0.0097;
static constexpr double APP_RATE_spR = 0.0023;
static constexpr double APP_RATE_spW = 0.0004;

// Precomputed: Σ rate(c) × ev(c) over all non-covered colors
// = 0.5453*10 + 0.2346*20 + 0.0786*35 + 0.0259*55 + 0.0296*76.03
//   + 0.0097*90 + 0.0023*150 + 0.0004*300 + 0.0393*5 + 0.0145*104.30
// (computed at compile time would require constexpr math; computed once at init instead)
static double g_ev_covered = 0.0;  // set in init_evaluation_run

static constexpr double REVEAL_BONUS_WEIGHT = 0.7;

// ---------------------------------------------------------------------------
// Flat-tuple index: encode sorted multiset as uint64_t key
//
// A sorted multiset of length L drawn from {0..5} with L <= 14 can be encoded
// as a base-7 number with a length prefix.  We pack:
//   bits [63:60] = length (0..14, 4 bits)
//   bits [59:0]  = up to 14 digits in base 7, each 3 bits, LSB = first element
// This gives a unique 64-bit key for each flat tuple.
// ---------------------------------------------------------------------------

static inline uint64_t encode_flat_tuple(const int* vals, int len) {
    uint64_t key = static_cast<uint64_t>(len) << 60;
    for (int i = 0; i < len; ++i)
        key |= static_cast<uint64_t>(vals[i]) << (i * 3);
    return key;
}

// ---------------------------------------------------------------------------
// Compact-base index: flat 4D array [nc][nb][nt][nd]
// Dimensions: [16][25][26][7] = 72,800 entries × 4 bytes = ~284 KB
// Unreachable states (sum > BASE_CELL_CAP) are stored as -1.
// ---------------------------------------------------------------------------

static constexpr int CB_DIM_NC = N_COV_MAX  + 1;  // 16
static constexpr int CB_DIM_NB = N_BLUE_MAX + 1;  // 25
static constexpr int CB_DIM_NT = N_TEAL_MAX + 1;  // 26
static constexpr int CB_DIM_ND = N_DARK_MAX + 1;  // 7

static inline int cb_idx(int nc, int nb, int nt, int nd) {
    return ((nc * CB_DIM_NB + nb) * CB_DIM_NT + nt) * CB_DIM_ND + nd;
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------

class ColblitzBellmanOHStrategy : public OHStrategy {
public:
    ColblitzBellmanOHStrategy() {
        std::fill(compact_idx_.begin(), compact_idx_.end(), -1);
    }

    ~ColblitzBellmanOHStrategy() {
        if (policy_fd_ >= 0) ::close(policy_fd_);
    }

    // -----------------------------------------------------------------------
    // init_evaluation_run: build index tables, validate and open policy files
    // -----------------------------------------------------------------------

    void init_evaluation_run() override {
        build_ft_index();
        build_compact_index();
        compute_ev_covered();

        std::string data_dir = std::string(REPO_ROOT) + "/data";
        std::string compact_path = data_dir + "/harvest_compact_base_20260423_143426.bin";
        std::string policy_path  = data_dir + "/harvest_policy_20260423_143426.bin";

        if (!validate_and_open(compact_path, policy_path)) {
            fprintf(stderr,
                "[colblitz_bellman] WARNING: failed to open DP policy files; "
                "using greedy EV fallback for all decisions.\n");
        } else {
            fprintf(stderr,
                "[colblitz_bellman] DP policy loaded. n_flat_tuples=%d n_base_compact=%d\n",
                n_flat_tuples_, n_base_compact_);
        }
    }

    // -----------------------------------------------------------------------
    // init_game_payload: reset per-game state
    // -----------------------------------------------------------------------

    void init_game_payload(const std::string& /*meta_json*/) override {
        chest_found_ = false;
    }

    // -----------------------------------------------------------------------
    // next_click: DP lookup with greedy fallback
    // -----------------------------------------------------------------------

    void next_click(const std::vector<Cell>& board,
                    const std::string& meta_json,
                    ClickResult& out) override
    {
        // --- Check for chest (color="spU", clicked=true) ---
        if (!chest_found_) {
            for (const Cell& c : board) {
                if (c.clicked && c.color == "spU") {
                    chest_found_ = true;
                    break;
                }
            }
        }

        // --- Priority 1: click any purple (free) — pick top-left first ---
        {
            int best = -1;
            for (const Cell& c : board) {
                if (!c.clicked && c.color == "spP") {
                    int pos = c.row * 5 + c.col;
                    if (best < 0 || pos < best) best = pos;
                }
            }
            if (best >= 0) {
                out.row = best / 5;
                out.col = best % 5;
                return;
            }
        }

        // --- Parse clicks_left from meta_json ---
        int clicks_left = 5;
        {
            const char* p = strstr(meta_json.c_str(), "\"clicks_left\":");
            if (p) clicks_left = atoi(p + 14);
        }
        if (clicks_left <= 0) {
            // No budget — return any unclicked cell to avoid crashing harness
            fallback_any(board, out);
            return;
        }

        // --- Build state counts from board ---
        int n_cov = 0, n_blue = 0, n_teal = 0, n_dark = 0;
        int flat_vals[N_FLAT_MAX];
        int n_flat = 0;

        for (const Cell& c : board) {
            if (c.clicked) continue;
            const std::string& col = c.color;
            if (col == "spP") continue;      // already handled above
            if (col == "spU") { ++n_cov; continue; }
            if (col == "spB") { ++n_blue; continue; }
            if (col == "spT") { ++n_teal; continue; }
            if (col == "spD") { ++n_dark; continue; }
            int fi = color_to_flat_idx(col);
            if (fi >= 0 && n_flat < N_FLAT_MAX)
                flat_vals[n_flat++] = fi;
        }

        // Sort flat indices ascending (required for tuple key)
        std::sort(flat_vals, flat_vals + n_flat);

        // --- Priority 2: DP policy lookup ---
        int action = dp_lookup(clicks_left, n_cov, n_blue, n_teal, n_dark,
                               flat_vals, n_flat, chest_found_);

        if (action >= 0) {
            bool found = action_to_click(action, board, flat_vals, n_flat, out);
            if (found) return;
            // Policy said an action but no matching cell — fall through to greedy
        }

        // --- Priority 3: greedy EV fallback ---
        greedy_click(board, clicks_left, n_cov, out);
    }

private:
    // ---- Policy state ----
    int policy_fd_      = -1;
    int n_base_compact_ = 0;
    int n_flat_tuples_  = 0;

    // Flat array: compact_idx_[cb_idx(nc,nb,nt,nd)] = compact base index (-1 if unreachable)
    std::array<int, CB_DIM_NC * CB_DIM_NB * CB_DIM_NT * CB_DIM_ND> compact_idx_;

    // flat-tuple key → flat-tuple index
    std::unordered_map<uint64_t, int> ft_to_idx_;

    // Per-game state
    bool chest_found_ = false;

    // ---- Build flat-tuple index ----
    // Must match Python's _build_flat_tuples() DFS order exactly.
    void build_ft_index() {
        ft_to_idx_.clear();
        ft_to_idx_.reserve(50000);  // ~38760 entries
        int vals[N_FLAT_MAX];
        build_ft_dfs(0, vals, 0);
        n_flat_tuples_ = static_cast<int>(ft_to_idx_.size());
    }

    void build_ft_dfs(int min_val, int* vals, int depth) {
        // Register this tuple (length = depth)
        uint64_t key = encode_flat_tuple(vals, depth);
        ft_to_idx_.emplace(key, static_cast<int>(ft_to_idx_.size()));
        if (depth >= N_FLAT_MAX) return;
        for (int v = min_val; v < N_FLAT_VALS; ++v) {
            vals[depth] = v;
            build_ft_dfs(v, vals, depth + 1);
        }
    }

    // ---- Build compact-base index ----
    // Must match generate_harvest_policy_dense.cpp build_compact_base() exactly.
    void build_compact_index() {
        std::fill(compact_idx_.begin(), compact_idx_.end(), -1);
        int i = 0;
        for (int nc = 0; nc <= N_COV_MAX; ++nc)
            for (int nb = 0; nb <= N_BLUE_MAX; ++nb)
                for (int nt = 0; nt <= N_TEAL_MAX; ++nt)
                    for (int nd = 0; nd <= N_DARK_MAX; ++nd)
                        if (nc + nb + nt + nd <= BASE_CELL_CAP)
                            compact_idx_[cb_idx(nc, nb, nt, nd)] = i++;
        n_base_compact_ = i;
    }

    // ---- Precompute ev_covered ----
    void compute_ev_covered() {
        g_ev_covered =
            APP_RATE_spB * 10.0
          + APP_RATE_spT * 20.0
          + APP_RATE_spG * 35.0
          + APP_RATE_spY * 55.0
          + APP_RATE_spL * 76.03
          + APP_RATE_spO * 90.0
          + APP_RATE_spR * 150.0
          + APP_RATE_spW * 300.0
          + APP_RATE_spP * 5.0
          + APP_RATE_spD * 104.30;
    }

    // ---- Validate headers and open policy fd ----
    bool validate_and_open(const std::string& compact_path, const std::string& policy_path) {
        // --- Step 1: Read compact base file ---
        FILE* cbf = fopen(compact_path.c_str(), "rb");
        if (!cbf) {
            fprintf(stderr, "[colblitz_bellman] cannot open compact base: %s\n", compact_path.c_str());
            return false;
        }
        uint8_t cb_hdr[HEADER_SIZE];
        if (fread(cb_hdr, 1, HEADER_SIZE, cbf) < (size_t)HEADER_SIZE) {
            fclose(cbf); return false;
        }
        fclose(cbf);

        if (memcmp(cb_hdr, "HCBI", 4) != 0) {
            fprintf(stderr, "[colblitz_bellman] bad magic in compact base file\n"); return false;
        }
        uint32_t cb_version;
        memcpy(&cb_version, cb_hdr + 4, 4);
        if (cb_version != 1) {
            fprintf(stderr, "[colblitz_bellman] unsupported compact base version %u\n", cb_version); return false;
        }
        char cb_ts[17] = {};
        memcpy(cb_ts, cb_hdr + 8, 16);

        uint32_t file_n_base;
        memcpy(&file_n_base, cb_hdr + 24, 4);
        if ((int)file_n_base != n_base_compact_) {
            fprintf(stderr, "[colblitz_bellman] N_BASE_COMPACT mismatch: file=%u local=%d\n",
                    file_n_base, n_base_compact_);
            return false;
        }

        // Validate dims: [N_COV_MAX, N_BLUE_MAX, N_TEAL_MAX, N_DARK_MAX, CAP]
        static const uint8_t expected_dims[5] = {
            N_COV_MAX, N_BLUE_MAX, N_TEAL_MAX, N_DARK_MAX, BASE_CELL_CAP
        };
        if (memcmp(cb_hdr + 28, expected_dims, 5) != 0) {
            fprintf(stderr, "[colblitz_bellman] compact base dimension mismatch\n"); return false;
        }

        // --- Step 2: Validate policy file header ---
        int fd = open(policy_path.c_str(), O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "[colblitz_bellman] cannot open policy file: %s\n", policy_path.c_str());
            return false;
        }

        // Get file size
        struct stat st;
        if (fstat(fd, &st) != 0) { ::close(fd); return false; }
        off_t file_size = st.st_size;
        off_t expected_size = (off_t)HEADER_SIZE
            + (off_t)2 * K_MAX * n_flat_tuples_ * n_base_compact_;
        if (file_size != expected_size) {
            fprintf(stderr,
                "[colblitz_bellman] policy size mismatch: got %lld expected %lld\n",
                (long long)file_size, (long long)expected_size);
            ::close(fd); return false;
        }

        uint8_t p_hdr[HEADER_SIZE];
        if (pread(fd, p_hdr, HEADER_SIZE, 0) < HEADER_SIZE) {
            ::close(fd); return false;
        }
        if (memcmp(p_hdr, "HARV", 4) != 0) {
            fprintf(stderr, "[colblitz_bellman] bad magic in policy file\n");
            ::close(fd); return false;
        }
        uint32_t p_version;
        memcpy(&p_version, p_hdr + 4, 4);
        if (p_version != 2) {
            fprintf(stderr, "[colblitz_bellman] unsupported policy version %u\n", p_version);
            ::close(fd); return false;
        }
        char p_ts[17] = {};
        memcpy(p_ts, p_hdr + 8, 16);
        if (strcmp(cb_ts, p_ts) != 0) {
            fprintf(stderr, "[colblitz_bellman] timestamp mismatch: compact=%s policy=%s\n", cb_ts, p_ts);
            ::close(fd); return false;
        }
        uint32_t p_n_base;
        memcpy(&p_n_base, p_hdr + 24, 4);
        if ((int)p_n_base != n_base_compact_) {
            fprintf(stderr, "[colblitz_bellman] policy N_BASE_COMPACT mismatch\n");
            ::close(fd); return false;
        }
        if (memcmp(p_hdr + 28, expected_dims, 5) != 0) {
            fprintf(stderr, "[colblitz_bellman] policy dimension mismatch\n");
            ::close(fd); return false;
        }

        policy_fd_ = fd;
        return true;
    }

    // ---- DP lookup: returns action code 0..4, or -1 if miss/unavailable ----
    int dp_lookup(int clicks_left,
                  int n_cov, int n_blue, int n_teal, int n_dark,
                  const int* flat_vals, int n_flat,
                  bool chest_found) const
    {
        if (policy_fd_ < 0) return -1;
        if (clicks_left < 1 || clicks_left > K_MAX) return -1;

        // Flat-tuple lookup
        uint64_t key = encode_flat_tuple(flat_vals, n_flat);
        auto it = ft_to_idx_.find(key);
        if (it == ft_to_idx_.end()) return -1;
        int ft_idx = it->second;

        // Clamp counts
        int nc = std::min(n_cov,  N_COV_MAX);
        int nb = std::min(n_blue, N_BLUE_MAX);
        int nt = std::min(n_teal, N_TEAL_MAX);
        int nd = std::min(n_dark, N_DARK_MAX);

        int compact_base = compact_idx_[cb_idx(nc, nb, nt, nd)];
        if (compact_base < 0) return -1;  // unreachable state

        int cf = chest_found ? 1 : 0;
        int n_ft_base = n_flat_tuples_ * n_base_compact_;
        off_t offset = (off_t)HEADER_SIZE
            + (off_t)cf       * (K_MAX * n_ft_base)
            + (off_t)(clicks_left - 1) * n_ft_base
            + (off_t)ft_idx   * n_base_compact_
            + (off_t)compact_base;

        uint8_t raw = 0;
        if (pread(policy_fd_, &raw, 1, offset) != 1) return -1;
        if (raw == 0) return -1;  // absent/unreachable state
        return static_cast<int>(raw) - 1;  // 1-indexed storage → action code 0..4
    }

    // ---- Translate action code to board click (top-left tie-break) ----
    // Returns true if a valid cell was found.
    bool action_to_click(int action,
                         const std::vector<Cell>& board,
                         const int* flat_vals, int n_flat,
                         ClickResult& out) const
    {
        if (action == ACTION_COVERED) {
            // Pick top-left unclicked covered cell
            int best = -1;
            for (const Cell& c : board) {
                if (!c.clicked && c.color == "spU") {
                    int pos = c.row * 5 + c.col;
                    if (best < 0 || pos < best) best = pos;
                }
            }
            if (best >= 0) { out.row = best / 5; out.col = best % 5; return true; }
            return false;
        }

        if (action == ACTION_BLUE) {
            return find_color_click("spB", board, out);
        }
        if (action == ACTION_TEAL) {
            return find_color_click("spT", board, out);
        }
        if (action == ACTION_DARK) {
            return find_color_click("spD", board, out);
        }
        if (action == ACTION_FLAT) {
            // Pick visible flat cell with highest EV (highest flat index)
            // Ties broken by grid position (top-left first)
            if (n_flat == 0) return false;
            int best_fi = -1;
            int best_pos = INT32_MAX;
            for (const Cell& c : board) {
                if (c.clicked) continue;
                int fi = color_to_flat_idx(c.color);
                if (fi < 0) continue;
                int pos = c.row * 5 + c.col;
                if (fi > best_fi || (fi == best_fi && pos < best_pos)) {
                    best_fi  = fi;
                    best_pos = pos;
                }
            }
            if (best_fi >= 0) {
                out.row = best_pos / 5;
                out.col = best_pos % 5;
                return true;
            }
            return false;
        }
        return false;
    }

    // ---- Helper: find first unclicked cell of given color (top-left) ----
    static bool find_color_click(const char* target_color,
                                  const std::vector<Cell>& board,
                                  ClickResult& out)
    {
        int best = -1;
        for (const Cell& c : board) {
            if (!c.clicked && c.color == target_color) {
                int pos = c.row * 5 + c.col;
                if (best < 0 || pos < best) best = pos;
            }
        }
        if (best >= 0) { out.row = best / 5; out.col = best % 5; return true; }
        return false;
    }

    // ---- Greedy EV fallback ----
    //
    // EV formula:
    //   ev_click(color) = ev(color) + REVEAL_BONUS_WEIGHT
    //                     × min(clicks_after, min(reveal_count, n_covered))
    //                     × ev_covered
    //
    // for blue (reveal_count=3) and teal (reveal_count=1); 0 for others.
    // Picks highest EV non-purple unclicked cell; tie-breaks by top-left.
    void greedy_click(const std::vector<Cell>& board,
                      int clicks_left, int n_cov,
                      ClickResult& out) const
    {
        double best_ev  = -1e30;
        int    best_pos = -1;
        int clicks_after = clicks_left - 1;

        for (const Cell& c : board) {
            if (c.clicked) continue;
            const std::string& col = c.color;
            if (col == "spP") continue;  // purples handled before this path

            double ev;
            if (col == "spU") {
                // Covered cell: EV = ev_covered (no reveal bonus — clicking covered doesn't reveal others)
                ev = g_ev_covered;
            } else {
                ev = ev_of_color(col);
                int reveal_count = 0;
                if (col == "spB") reveal_count = 3;
                else if (col == "spT") reveal_count = 1;
                if (reveal_count > 0 && clicks_after > 0 && n_cov > 0) {
                    int exploitable = std::min(clicks_after, std::min(reveal_count, n_cov));
                    ev += REVEAL_BONUS_WEIGHT * exploitable * g_ev_covered;
                }
            }

            int pos = c.row * 5 + c.col;
            if (ev > best_ev || (ev == best_ev && pos < best_pos)) {
                best_ev  = ev;
                best_pos = pos;
            }
        }

        if (best_pos >= 0) {
            out.row = best_pos / 5;
            out.col = best_pos % 5;
        } else {
            fallback_any(board, out);
        }
    }

    // ---- Last-resort: click any unclicked cell ----
    static void fallback_any(const std::vector<Cell>& board, ClickResult& out) {
        for (const Cell& c : board) {
            if (!c.clicked) {
                out.row = c.row;
                out.col = c.col;
                return;
            }
        }
        out.row = 0; out.col = 0;
    }
};

// ---------------------------------------------------------------------------
// Inline board JSON parser (shared by all C++ strategies in this repo)
// ---------------------------------------------------------------------------

static std::vector<Cell> parse_board(const char* board_json) {
    std::vector<Cell> board;
    board.reserve(25);
    const char* p = board_json;
    while ((p = strstr(p, "\"row\":")) != nullptr) {
        Cell c;
        c.row = atoi(p + 6);
        const char* cp = strstr(p, "\"col\":"); if (cp) c.col = atoi(cp + 6);
        const char* colp = strstr(p, "\"color\":\"");
        if (colp) {
            colp += 9;
            const char* e = strchr(colp, '"');
            if (e) c.color = std::string(colp, e - colp);
        }
        const char* clkp = strstr(p, "\"clicked\":");
        if (clkp) {
            clkp += 10;
            while (*clkp == ' ') ++clkp;
            c.clicked = (strncmp(clkp, "true", 4) == 0);
        }
        board.push_back(c);
        p += 6;
    }
    return board;
}

// ---------------------------------------------------------------------------
// C exports required by the harness
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new ColblitzBellmanOHStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* inst) {
    static_cast<ColblitzBellmanOHStrategy*>(inst)->init_evaluation_run();
}

extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {
    static_cast<ColblitzBellmanOHStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<ColblitzBellmanOHStrategy*>(inst);

    std::vector<Cell> board = parse_board(board_json ? board_json : "[]");

    ClickResult out;
    s->next_click(board, meta_json ? meta_json : "{}", out);
    buf = "{\"row\":" + std::to_string(out.row) +
          ",\"col\":" + std::to_string(out.col) + "}";
    return buf.c_str();
}
