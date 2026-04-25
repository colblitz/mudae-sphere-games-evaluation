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
// Weighted Welford online mean + variance (West 1979 / Chan et al.)
//
// Weights are arbitrary positive doubles; they need not sum to 1.
// Variance is the reliability-weights (frequency-weights) formula:
//   Var = M2 / (W - W2/W)   where W = sum of weights, W2 = sum of w^2.
// This equals the sample variance when all weights are 1.
// ---------------------------------------------------------------------------

struct WeightedWelford {
    double mean   = 0.0;
    double M2     = 0.0;
    double W      = 0.0;  // sum of weights
    double W2     = 0.0;  // sum of squared weights (for reliability-weights variance)
    uint64_t count = 0;   // number of observations (for parallel merge bookkeeping)

    void update(double x, double w) {
        ++count;
        double W_new  = W + w;
        double delta  = x - mean;
        mean         += delta * (w / W_new);
        double delta2 = x - mean;
        M2           += w * delta * delta2;
        W             = W_new;
        W2           += w * w;
    }

    // Population-weighted variance (reliability weights).
    // Returns 0 if fewer than 2 observations.
    double variance() const {
        if (count < 2 || W <= 0.0) return 0.0;
        double denom = W - W2 / W;
        return denom > 0.0 ? M2 / denom : 0.0;
    }
    double stdev() const { return std::sqrt(variance()); }

    // Parallel merge (Chan's formula for weighted accumulators).
    // Merges `other` into `*this`.
    void merge(const WeightedWelford& other) {
        if (other.count == 0) return;
        if (count == 0) { *this = other; return; }
        double W_new  = W + other.W;
        double delta  = other.mean - mean;
        mean          = (mean * W + other.mean * other.W) / W_new;
        M2           += other.M2 + delta * delta * (W * other.W / W_new);
        W             = W_new;
        W2           += other.W2;
        count        += other.count;
    }
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
    int      n_colors            = 0;
    double   ev                  = 0.0;
    double   stdev_ev            = 0.0;  // stdev of score
    double   avg_clicks          = 0.0;  // average total clicks per game (blue + free)
    double   stdev_clicks        = 0.0;  // stdev of total clicks
    double   avg_ship_clicks     = 0.0;  // average number of clicks that hit a ship cell
    double   stdev_ship_clicks   = 0.0;  // stdev of ship-cell clicks
    double   perfect_rate        = 0.0;  // fraction of games where all ship cells were revealed
    double   all_ships_rate      = 0.0;  // fraction of games where all ships were hit
    double   loss_5050_rate      = 0.0;  // fraction of games lost on a ~50/50 blue decision
    uint64_t n_boards            = 0;
    uint64_t total_strategy_calls = 0;  // treewalk only: total next_click invocations
};

struct OTResult {
    OTVariantResult variants[4];  // indices 0..3 → n_colors 6..9
    double          aggregate_ev = 0.0;  // board-count-weighted average EV across variants
};

}  // namespace sphere
