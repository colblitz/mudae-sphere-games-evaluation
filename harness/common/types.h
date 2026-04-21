/**
 * types.h — Shared types used across all harness evaluators.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sphere {

// ---------------------------------------------------------------------------
// Grid
// ---------------------------------------------------------------------------

static constexpr int GRID_SIZE = 5;
static constexpr int N_CELLS   = 25;

static inline int rc_to_idx(int row, int col) { return row * GRID_SIZE + col; }
static inline int idx_to_row(int idx)          { return idx / GRID_SIZE; }
static inline int idx_to_col(int idx)          { return idx % GRID_SIZE; }

// ---------------------------------------------------------------------------
// Revealed cell
// ---------------------------------------------------------------------------

struct Cell {
    int8_t      row;
    int8_t      col;
    std::string color;  // Mudae emoji name
};

// ---------------------------------------------------------------------------
// Click decision returned by a strategy
// ---------------------------------------------------------------------------

struct Click {
    int8_t row = 0;
    int8_t col = 0;
};

}  // namespace sphere
