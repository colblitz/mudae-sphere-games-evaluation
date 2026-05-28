// sphere:stateless
#pragma GCC optimize("fp-contract=off")

/**
 * zavex_heuristic_fast_higgs.cpp — Higgs variant of zavex_heuristic_fast.
 *
 * Forces the four corners (cells 0, 4, 20, 24 — top-left, top-right,
 * bottom-left, bottom-right) as the first four clicks, then delegates all
 * subsequent decisions to FastHeuristicOTStrategy.
 *
 * Implementation: thin wrapper via #include of the base file (with
 * HIGGS_DELEGATE defined to suppress the base extern "C" exports).
 */

#define HIGGS_DELEGATE
#include "zavex_heuristic_fast.cpp"

// ---------------------------------------------------------------------------
// Higgs wrapper
// ---------------------------------------------------------------------------

static constexpr int HIGGS_CORNERS[4] = {0, 4, 20, 24};

class FastHeuristicHiggsOTStrategy : public OTStrategy {
    FastHeuristicOTStrategy base_;

    // Returns the corner cell to play if we are still in the opening (fewer
    // than 4 cells clicked), or -1 if the opening is complete.
    static int higgsCorner(uint32_t clickedMask) {
        if (__builtin_popcount(clickedMask) >= 4) return -1;
        for (int corner : HIGGS_CORNERS)
            if (!((clickedMask >> corner) & 1)) return corner;
        return -1;
    }

public:
    void next_click(const std::vector<Cell>& board,
                    const std::string& meta_json,
                    ClickResult& out) override
    {
        uint32_t clickedMask = 0;
        for (const Cell& c : board)
            if (c.clicked) clickedMask |= 1u << (c.row * 5 + c.col);

        int corner = higgsCorner(clickedMask);
        if (corner >= 0) {
            out.row = corner / 5;
            out.col = corner % 5;
            return;
        }
        base_.next_click(board, meta_json, out);
    }
};

// ---------------------------------------------------------------------------
// C exports required by the harness
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new FastHeuristicHiggsOTStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* inst) {
    static_cast<FastHeuristicHiggsOTStrategy*>(inst)->init_evaluation_run();
}

extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {
    static_cast<FastHeuristicHiggsOTStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<FastHeuristicHiggsOTStrategy*>(inst);

    std::vector<Cell> board;
    board.reserve(25);
    const char* p = board_json;
    while ((p = strstr(p, "\"row\":")) != nullptr) {
        Cell c;
        c.row = atoi(p + 6);
        const char* cp   = strstr(p, "\"col\":");    if (cp)   c.col = atoi(cp + 6);
        const char* colp = strstr(p, "\"color\":\"");
        if (colp) { colp += 9; const char* e = strchr(colp, '"'); if (e) c.color = std::string(colp, e - colp); }
        const char* clkp = strstr(p, "\"clicked\":"); if (clkp) { clkp += 10; while (*clkp == ' ') ++clkp; c.clicked = (strncmp(clkp, "true", 4) == 0); }
        board.push_back(c); p += 6;
    }

    ClickResult result;
    s->next_click(board, meta_json ? meta_json : "{}", result);
    buf = "{\"row\":" + std::to_string(result.row) + ",\"col\":" + std::to_string(result.col) + "}";
    return buf.c_str();
}
