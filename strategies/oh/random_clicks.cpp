/**
 * random_clicks.cpp — Random baseline strategy for /sphere harvest (oh).
 *
 * Picks a random unrevealed, unclicked cell on every turn.
 * Prefers purple cells (they are free) if any are visible.
 *
 * This is the simplest possible strategy — no state, no inference.
 * See stateful.cpp (oq/) for per-game state usage, global_state.cpp (oc/)
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

class RandomOHStrategy : public OHStrategy {
public:
    RandomOHStrategy() : rng_(static_cast<uint64_t>(std::time(nullptr))) {}

    void next_click(const std::vector<Cell>& revealed,
                    const std::string& /*meta_json*/,
                    const std::string& /*state_json*/,
                    ClickResult& out) override
    {
        bool clicked[25] = {};
        std::vector<int> purples;
        std::vector<int> unclicked;

        for (const Cell& c : revealed)
            clicked[c.row * 5 + c.col] = true;

        for (const Cell& c : revealed)
            if (c.color == "spP") purples.push_back(c.row * 5 + c.col);

        for (int i = 0; i < 25; ++i)
            if (!clicked[i]) unclicked.push_back(i);

        int chosen = -1;
        if (!purples.empty()) {
            chosen = purples[std::uniform_int_distribution<int>(0, (int)purples.size() - 1)(rng_)];
        } else if (!unclicked.empty()) {
            chosen = unclicked[std::uniform_int_distribution<int>(0, (int)unclicked.size() - 1)(rng_)];
        }

        out.row = (chosen >= 0) ? chosen / 5 : 0;
        out.col = (chosen >= 0) ? chosen % 5 : 0;
        out.state_json = "{}";
    }

private:
    std::mt19937_64 rng_;
};

// ---------------------------------------------------------------------------
// C exports required by the harness
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()               { return new RandomOHStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" const char* strategy_init_evaluation_run(void*) { return "{}"; }
extern "C" const char* strategy_init_game_payload(void*, const char*, const char* state) { return state; }

extern "C" const char* strategy_next_click(void* inst,
                                            const char* revealed_json,
                                            const char* meta_json,
                                            const char* state_json)
{
    // Minimal JSON parsing: extract "row" and "col" fields
    static char buf[64];
    auto* s = static_cast<RandomOHStrategy*>(inst);

    // Build a minimal revealed vector from JSON array
    // Format: [{"row":R,"col":C,"color":"spX"}, ...]
    std::vector<Cell> revealed;
    const char* p = revealed_json;
    while ((p = strstr(p, "\"row\":")) != nullptr) {
        Cell c;
        c.row = static_cast<int8_t>(atoi(p + 6));
        const char* cp = strstr(p, "\"col\":");
        if (cp) c.col = static_cast<int8_t>(atoi(cp + 6));
        const char* colp = strstr(p, "\"color\":\"");
        if (colp) {
            colp += 9;
            const char* end = strchr(colp, '"');
            if (end) c.color = std::string(colp, end - colp);
        }
        revealed.push_back(c);
        p += 6;
    }

    ClickResult out;
    s->next_click(revealed, meta_json ? meta_json : "{}", state_json ? state_json : "{}", out);
    snprintf(buf, sizeof(buf), "{\"row\":%d,\"col\":%d,\"state\":{}}", out.row, out.col);
    return buf;
}
