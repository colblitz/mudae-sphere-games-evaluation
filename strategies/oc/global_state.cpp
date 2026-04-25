/**
 * global_state.cpp — Global-state example strategy for /sphere chest (oc).
 *
 * Demonstrates how to use init_evaluation_run() to compute data ONCE before all
 * games in an evaluation run, then reuse it cheaply across every game.
 *
 * The payload here is a fixed cell visit order — a spiral from the center
 * outward.  Every game walks down this list and clicks the first cell not yet
 * revealed.
 *
 * WHY GLOBAL STATE?
 * -----------------
 * init_evaluation_run() is called exactly once per evaluation run (before any games
 * start).  Use it for anything that is:
 *   - Expensive to compute (search, optimisation, table building).
 *   - Identical for every game (board-independent precomputation).
 *   - Read-only during play (the lookup never changes mid-run).
 *
 * In C++, run-level results are stored in member variables set by
 * init_evaluation_run(); there is no return value threaded through the harness.
 * Per-game bookkeeping (if any) is reset in init_game_payload().
 *
 * Contrast with init_game_payload(), which is called once per game and is the
 * right place to reset per-game bookkeeping — see oq/stateful.cpp for that
 * pattern.
 *
 * STRATEGY LOGIC
 * --------------
 * The center cell (2,2) can never be red, so it is visited last.
 * All other cells are visited in a deterministic clockwise spiral starting
 * from the outermost ring inward.  The order is built once in
 * init_evaluation_run() and stored in a member variable; next_click() does a
 * simple linear scan.
 */

#include <cstring>
#include <string>
#include <vector>
#include <set>

#include "../../interface/strategy.h"

using namespace sphere;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct RC { int r, c; };

/** Build a center-outward spiral visit order over a 5×5 grid. */
static std::vector<RC> build_spiral_order() {
    std::vector<RC> order;
    std::set<int> seen;  // index = r*5+c

    auto add = [&](int r, int c) {
        if (r < 0 || r >= 5 || c < 0 || c >= 5) return;
        int idx = r * 5 + c;
        if (seen.count(idx)) return;
        seen.insert(idx);
        order.push_back({r, c});
    };

    // Shells by Chebyshev distance from center (2,2), starting at dist=1
    for (int dist = 1; dist <= 2; ++dist) {
        int rs = 2 - dist, re = 2 + dist;
        int cs = 2 - dist, ce = 2 + dist;
        // Top row
        for (int c = cs; c <= ce; ++c) add(rs, c);
        // Right col
        for (int r = rs + 1; r <= re; ++r) add(r, ce);
        // Bottom row
        for (int c = ce - 1; c >= cs; --c) add(re, c);
        // Left col
        for (int r = re - 1; r > rs; --r) add(r, cs);
    }

    // Center last (can never be red)
    add(2, 2);

    // Fill any remaining cells (belt-and-suspenders for a 5x5 grid)
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 5; ++c)
            add(r, c);

    return order;
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------

class GlobalStateOCStrategy : public OCStrategy {
public:
    // -----------------------------------------------------------------------
    // Global state: build visit order once, store in member variable
    // -----------------------------------------------------------------------

    /**
     * Called ONCE before all games.  Builds the visit order and stores it in
     * order_ so we pay the construction cost only once across the entire
     * evaluation run.
     *
     * In C++, run-level results live in member variables — there is no return
     * value threaded through the harness.  Treat order_ as read-only after
     * this call; never mutate it inside next_click().
     */
    void init_evaluation_run() override {
        order_ = build_spiral_order();
    }

    // init_game_payload() is intentionally omitted — no per-game reset needed.

    // -----------------------------------------------------------------------
    // Click decision: walk the pre-built spiral order
    // -----------------------------------------------------------------------

    void next_click(const std::vector<Cell>& board,
                    const std::string& /*meta_json*/,
                    ClickResult& out) override
    {
        bool clicked[25] = {};
        for (const Cell& c : board)
            if (c.clicked) clicked[c.row * 5 + c.col] = true;

        for (const RC& rc : order_) {
            if (!clicked[rc.r * 5 + rc.c]) {
                out.row = rc.r;
                out.col = rc.c;
                return;
            }
        }

        // Fallback: should never be reached on a valid board
        out.row = 0; out.col = 0;
    }

private:
    std::vector<RC> order_;  // built once in init_evaluation_run(); read-only thereafter
};

// ---------------------------------------------------------------------------
// C exports required by the harness — do not rename these functions
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new GlobalStateOCStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* inst) {
    static_cast<GlobalStateOCStrategy*>(inst)->init_evaluation_run();
}

extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {
    static_cast<GlobalStateOCStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<GlobalStateOCStrategy*>(inst);
    std::vector<Cell> board = parse_board_json(board_json);
    ClickResult out;
    s->next_click(board, meta_json ? meta_json : "{}", out);
    buf = "{\"row\":" + std::to_string(out.row) + ",\"col\":" + std::to_string(out.col) + "}";
    return buf.c_str();
}
