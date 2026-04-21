/**
 * stats.h — Welford online mean/variance accumulator and per-game stat structs.
 */

#pragma once

#include <cmath>
#include <cstdint>

namespace sphere {

// ---------------------------------------------------------------------------
// Welford online mean + variance (numerically stable, single pass)
// ---------------------------------------------------------------------------

struct Welford {
    double   mean  = 0.0;
    double   M2    = 0.0;
    uint64_t count = 0;

    void update(double x) {
        ++count;
        double delta  = x - mean;
        mean         += delta / static_cast<double>(count);
        double delta2 = x - mean;
        M2           += delta * delta2;
    }

    double variance()  const { return count < 2 ? 0.0 : M2 / static_cast<double>(count - 1); }
    double stdev()     const { return std::sqrt(variance()); }
};

// ---------------------------------------------------------------------------
// Per-game stat result structs
// ---------------------------------------------------------------------------

struct OHResult {
    double   ev      = 0.0;
    double   stdev   = 0.0;
    double   oc_rate = 0.0;  // fraction of games containing a chest cell
    uint64_t n_games = 0;
};

struct OCResult {
    double   ev       = 0.0;
    double   stdev    = 0.0;
    double   red_rate = 0.0;  // fraction of boards where red was clicked
    uint64_t n_boards = 0;
};

struct OQResult {
    double   ev       = 0.0;
    double   stdev    = 0.0;
    double   red_rate = 0.0;  // fraction of boards where red was clicked
    uint64_t n_boards = 0;
};

struct OTVariantResult {
    int      n_colors       = 0;
    double   ev             = 0.0;
    double   stdev          = 0.0;
    double   avg_clicks     = 0.0;  // average total clicks per game (blue + free)
    double   perfect_rate   = 0.0;  // fraction of games where all ship cells were revealed
    double   all_ships_rate = 0.0;  // fraction of games where all ships were hit
    double   loss_5050_rate = 0.0;  // fraction of games lost on a ~50/50 blue decision
    uint64_t n_boards       = 0;
};

struct OTResult {
    OTVariantResult variants[4];  // indices 0..3 → n_colors 6..9
    double          aggregate_ev = 0.0;  // board-count-weighted average EV across variants
};

}  // namespace sphere
