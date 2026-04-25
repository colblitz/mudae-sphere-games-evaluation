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
// Board cell
// ---------------------------------------------------------------------------
//
// NOTE: interface/strategy.h also defines sphere::Cell for use by strategy
// authors.  That definition uses `int` for row/col (not int8_t) so that
// strategies do not need to cast when doing arithmetic.  The two definitions
// are binary-compatible (int8_t widened to int at the ABI boundary), but they
// are intentionally separate: harness internals use this compact form;
// strategies use the interface form.

struct Cell {
    int8_t      row;
    int8_t      col;
    std::string color   = "spU";  // Mudae emoji name; "spU" = covered/unknown
    bool        clicked = false;  // true = cell has been clicked (disabled)
};

// ---------------------------------------------------------------------------
// Click decision returned by a strategy
// ---------------------------------------------------------------------------

struct Click {
    int8_t row = 0;
    int8_t col = 0;
};

}  // namespace sphere
