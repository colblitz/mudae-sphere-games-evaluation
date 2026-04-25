/**
 * stateful.cpp — Stateful example strategy for /sphere quest (oq).
 *
 * Demonstrates how to use init_game_payload() to reset per-game state and
 * maintain information across clicks within a single game using member
 * variables.
 *
 * WHY PER-GAME STATE?
 * -------------------
 * init_game_payload() is called once at the start of EACH game, making it the
 * right place to reset anything that should be fresh for every game.  In C++,
 * per-game state lives in member variables: reset them in init_game_payload()
 * and read/update them in next_click().
 *
 * Contrast with init_evaluation_run(), which is called only ONCE before all
 * games begin — use that for cross-game global tables (see oc/global_state.cpp).
 *
 * STRATEGY LOGIC
 * --------------
 * We track which rows and columns have already been clicked this game.
 * When choosing the next cell, we prefer cells whose row AND column are both
 * new (maximising board coverage).  If no such cell exists we fall back to
 * cells with at least one new axis, then to any unclicked cell.
 *
 * The click history is stored in row_mask_, col_mask_, and click_count_ member
 * variables, accumulated across calls within a game and reset by
 * init_game_payload() at the start of each new game.
 */

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <vector>

#include "../../interface/strategy.h"

using namespace sphere;

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------

class StatefulOQStrategy : public OQStrategy {
public:
    StatefulOQStrategy() : rng_(static_cast<uint64_t>(std::time(nullptr))) {}

    // -----------------------------------------------------------------------
    // Per-game reset
    // -----------------------------------------------------------------------

    /**
     * Reset per-game tracking at the start of every game.
     *
     * Called once before the first next_click() of each game.  Clears the
     * click-history bitmasks so that the new game starts fresh.
     */
    void init_game_payload(const std::string& /*meta_json*/) override {
        row_mask_    = 0;
        col_mask_    = 0;
        click_count_ = 0;
    }

    // -----------------------------------------------------------------------
    // Click decision: prefer unexplored rows and columns
    // -----------------------------------------------------------------------

    /**
     * Choose the next cell, preferring unexplored rows and columns.
     *
     * row_mask_ and col_mask_ track which rows/cols have been clicked this
     * game; next_click() updates them after choosing a cell.
     */
    void next_click(const std::vector<Cell>& board,
                    const std::string& /*meta_json*/,
                    ClickResult& out) override
    {
        // Build clicked set from board
        bool clicked[25] = {};
        for (const Cell& c : board)
            if (c.clicked) clicked[c.row * 5 + c.col] = true;

        // Classify unclicked cells
        std::vector<int> new_both, new_one, fallback;
        for (int r = 0; r < 5; ++r) {
            for (int c = 0; c < 5; ++c) {
                if (clicked[r * 5 + c]) continue;
                bool new_r = !(row_mask_ & (1 << r));
                bool new_c = !(col_mask_ & (1 << c));
                if (new_r && new_c)      new_both.push_back(r * 5 + c);
                else if (new_r || new_c) new_one .push_back(r * 5 + c);
                else                     fallback .push_back(r * 5 + c);
            }
        }

        auto& candidates = !new_both.empty() ? new_both
                         : !new_one .empty() ? new_one
                                             : fallback;
        if (candidates.empty()) { out.row = 0; out.col = 0; return; }

        int chosen = candidates[
            std::uniform_int_distribution<int>(0, (int)candidates.size() - 1)(rng_)
        ];
        out.row = chosen / 5;
        out.col = chosen % 5;

        // Update per-game state for the next call
        row_mask_   |= (1 << out.row);
        col_mask_   |= (1 << out.col);
        click_count_ += 1;
    }

private:
    std::mt19937_64 rng_;

    // Per-game state — reset by init_game_payload() before every game
    int row_mask_    = 0;
    int col_mask_    = 0;
    int click_count_ = 0;
};

// ---------------------------------------------------------------------------
// C exports required by the harness — do not rename these functions
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new StatefulOQStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* /*inst*/) {
    // No global precomputation needed for this strategy.
}

extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {
    static_cast<StatefulOQStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<StatefulOQStrategy*>(inst);
    std::vector<Cell> board = parse_board_json(board_json);
    ClickResult out;
    s->next_click(board, meta_json ? meta_json : "{}", out);
    buf = "{\"row\":" + std::to_string(out.row) + ",\"col\":" + std::to_string(out.col) + "}";
    return buf.c_str();
}
