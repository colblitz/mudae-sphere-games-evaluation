/**
 * global_state.cpp — Global-state example strategy for /sphere chest (oc).
 *
 * Demonstrates how to use init_evaluation_run() to compute data ONCE before all
 * games in an evaluation run, then reuse it cheaply across every game.
 *
 * The payload here is a fixed cell visit order — a spiral from the center
 * outward — serialised as a JSON array of [row, col] pairs.  Every game
 * walks down this list and clicks the first cell not yet revealed.
 *
 * WHY GLOBAL STATE?
 * -----------------
 * init_evaluation_run() is called exactly once per evaluation run (before any games
 * start).  Its return value (a JSON string) is passed as game_state_json to
 * init_game_payload() at the start of every game, and from there flows through every
 * next_click() call unchanged.
 *
 * Use init_evaluation_run() for anything that is:
 *   - Expensive to compute (search, optimisation, table building).
 *   - Identical for every game (board-independent precomputation).
 *   - Read-only during play (the lookup never changes mid-run).
 *
 * Contrast with init_game_payload(), which is called once per game and is the right
 * place to reset per-game bookkeeping — see oq/stateful.cpp for that pattern.
 *
 * STRATEGY LOGIC
 * --------------
 * The center cell (2,2) can never be red, so it is visited last.
 * All other cells are visited in a deterministic clockwise spiral starting
 * from the outermost ring inward.  The order is built once in init_evaluation_run()
 * and stored as a JSON array; next_click() does a simple linear scan.
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

/** Serialise the visit order to a JSON string: [[r,c],...] */
static std::string order_to_json(const std::vector<RC>& order) {
    std::string s = "[";
    for (size_t i = 0; i < order.size(); ++i) {
        if (i) s += ",";
        s += "[" + std::to_string(order[i].r) + "," + std::to_string(order[i].c) + "]";
    }
    s += "]";
    return s;
}

/** Parse [[r,c],...] from a JSON string. */
static std::vector<RC> order_from_json(const std::string& json) {
    std::vector<RC> order;
    const char* p = json.c_str();
    // Each pair starts with '['
    while ((p = strchr(p, '[')) != nullptr) {
        ++p;
        if (*p == '[') {  // inner array
            ++p;
            RC rc;
            rc.r = atoi(p);
            const char* comma = strchr(p, ',');
            if (!comma) break;
            rc.c = atoi(comma + 1);
            order.push_back(rc);
        }
    }
    return order;
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------

class GlobalStateOCStrategy : public OCStrategy {
public:
    /**
     * Called ONCE before all games.  Build the visit order here so we pay
     * the construction cost only once across the entire evaluation run.
     *
     * Returns a JSON string that the harness will pass as game_state_json to every
     * subsequent init_game_payload() and next_click() call.
     */
    std::string init_evaluation_run() override {
        auto order = build_spiral_order();
        // Wrap in an object so game_state_json is always a JSON object (not a bare array)
        return "{\"order\":" + order_to_json(order) + "}";
    }

    // init_game_payload() is intentionally omitted: the default returns game_state_json
    // unchanged, which is exactly what we want — no per-game reset needed.

    void next_click(const std::vector<Cell>& board,
                    const std::string& /*meta_json*/,
                    const std::string& game_state_json,
                    ClickResult& out) override
    {
        // Reconstruct the visit order from the global state.
        // In a real high-performance strategy you'd cache this in a member
        // variable parsed once from init_evaluation_run's return; kept simple here
        // to illustrate game_state_json usage clearly.
        bool clicked[25] = {};
        for (const Cell& c : board)
            if (c.clicked) clicked[c.row * 5 + c.col] = true;

        // Extract the "order" array from the state JSON
        auto pos = game_state_json.find("\"order\":");
        std::string order_json = (pos != std::string::npos)
            ? game_state_json.substr(pos + 8)
            : "[]";
        auto order = order_from_json(order_json);

        for (const RC& rc : order) {
            if (!clicked[rc.r * 5 + rc.c]) {
                out.row = rc.r;
                out.col = rc.c;
                out.game_state_json = game_state_json;  // unchanged — shared read-only table
                return;
            }
        }

        // Fallback: should never be reached on a valid board
        out.row = 0; out.col = 0;
        out.game_state_json = game_state_json;
    }
};

// ---------------------------------------------------------------------------
// C exports required by the harness
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new GlobalStateOCStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" const char* strategy_init_evaluation_run(void* inst) {
    thread_local static std::string buf;
    buf = static_cast<GlobalStateOCStrategy*>(inst)->init_evaluation_run();
    return buf.c_str();
}

extern "C" const char* strategy_init_game_payload(void*, const char*, const char* evaluation_run_state) {
    // No per-game reset needed — pass evaluation_run_state through unchanged.
    return evaluation_run_state;
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json,
                                            const char* game_state_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<GlobalStateOCStrategy*>(inst);

    std::vector<Cell> board;
    const char* p = board_json;
    while ((p = strstr(p, "\"row\":")) != nullptr) {
        Cell c;
        c.row = atoi(p + 6);
        const char* cp = strstr(p, "\"col\":"); if (cp) c.col = atoi(cp + 6);
        const char* colp = strstr(p, "\"color\":\"");
        if (colp) { colp += 9; const char* e = strchr(colp, '"'); if (e) c.color = std::string(colp, e - colp); }
        const char* clkp = strstr(p, "\"clicked\":"); if (clkp) { clkp += 10; while (*clkp==' ') ++clkp; c.clicked = (strncmp(clkp,"true",4)==0); }
        board.push_back(c); p += 6;
    }

    ClickResult out;
    s->next_click(board, meta_json ? meta_json : "{}", game_state_json ? game_state_json : "{}", out);
    buf = "{\"row\":" + std::to_string(out.row) +
          ",\"col\":" + std::to_string(out.col) +
          ",\"state\":" + out.game_state_json + "}";
    return buf.c_str();
}
