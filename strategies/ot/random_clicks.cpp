/**
 * random_clicks.cpp — Random baseline strategy for /sphere trace (ot).
 *
 * Picks a random unclicked cell on every turn.
 * No constraint inference — does not use ship geometry.
 *
 * This is the simplest possible strategy — no state, no inference.
 * See oq/stateful.cpp for per-game state usage, oc/global_state.cpp
 * for cross-game global state usage.
 */

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <vector>

#include "../../interface/strategy.h"

using namespace sphere;

class RandomOTStrategy : public OTStrategy {
public:
    RandomOTStrategy() : rng_(static_cast<uint64_t>(std::time(nullptr))) {}

    void next_click(const std::vector<Cell>& board,
                    const std::string& /*meta_json*/,
                    ClickResult& out) override
    {
        bool clicked[25] = {};
        for (const Cell& c : board) if (c.clicked) clicked[c.row * 5 + c.col] = true;

        std::vector<int> unclicked;
        for (int i = 0; i < 25; ++i)
            if (!clicked[i]) unclicked.push_back(i);

        int chosen = unclicked.empty() ? 0
            : unclicked[std::uniform_int_distribution<int>(0, (int)unclicked.size() - 1)(rng_)];
        out.row = chosen / 5;
        out.col = chosen % 5;
    }

private:
    std::mt19937_64 rng_;
};

// ---------------------------------------------------------------------------
// C exports required by the harness — do not rename these functions
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new RandomOTStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* /*inst*/) {}

extern "C" void strategy_init_game_payload(void* /*inst*/, const char* /*meta_json*/) {}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<RandomOTStrategy*>(inst);
    std::vector<Cell> board = parse_board_json(board_json);
    ClickResult out;
    s->next_click(board, meta_json ? meta_json : "{}", out);
    buf = "{\"row\":" + std::to_string(out.row) + ",\"col\":" + std::to_string(out.col) + "}";
    return buf.c_str();
}
