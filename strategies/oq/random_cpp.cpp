/**
 * random_cpp.cpp — Random baseline strategy for /sphere quest (oq).
 * Picks a random unclicked cell on every turn.
 */

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <vector>

#include "../../interface/strategy.h"

using namespace sphere;

class RandomOQStrategy : public OQStrategy {
public:
    RandomOQStrategy() : rng_(static_cast<uint64_t>(std::time(nullptr))) {}

    void next_click(const std::vector<Cell>& revealed,
                    const std::string& /*meta_json*/,
                    const std::string& /*state_json*/,
                    ClickResult& out) override
    {
        bool clicked[25] = {};
        for (const Cell& c : revealed) clicked[c.row * 5 + c.col] = true;

        std::vector<int> unclicked;
        for (int i = 0; i < 25; ++i)
            if (!clicked[i]) unclicked.push_back(i);

        int chosen = unclicked.empty() ? 0
            : unclicked[std::uniform_int_distribution<int>(0, (int)unclicked.size() - 1)(rng_)];
        out.row = chosen / 5;
        out.col = chosen % 5;
        out.state_json = "{}";
    }

private:
    std::mt19937_64 rng_;
};

extern "C" sphere::StrategyBase* create_strategy()                        { return new RandomOQStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }
extern "C" const char* strategy_init_payload(void*)                        { return "{}"; }
extern "C" const char* strategy_init_run(void*, const char*, const char* s){ return s; }

extern "C" const char* strategy_next_click(void* inst,
                                            const char* revealed_json,
                                            const char* meta_json,
                                            const char* state_json)
{
    static char buf[64];
    auto* st = static_cast<RandomOQStrategy*>(inst);
    std::vector<Cell> revealed;
    const char* p = revealed_json;
    while ((p = strstr(p, "\"row\":")) != nullptr) {
        Cell c;
        c.row = static_cast<int8_t>(atoi(p + 6));
        const char* cp = strstr(p, "\"col\":"); if (cp) c.col = static_cast<int8_t>(atoi(cp + 6));
        const char* colp = strstr(p, "\"color\":\"");
        if (colp) { colp += 9; const char* e = strchr(colp, '"'); if (e) c.color = std::string(colp, e - colp); }
        revealed.push_back(c); p += 6;
    }
    ClickResult out;
    st->next_click(revealed, meta_json ? meta_json : "{}", state_json ? state_json : "{}", out);
    snprintf(buf, sizeof(buf), "{\"row\":%d,\"col\":%d,\"state\":{}}", out.row, out.col);
    return buf;
}
