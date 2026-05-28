// sphere:stateless
#pragma GCC optimize("O3")
#pragma GCC optimize("fp-contract=off")

/**
 * colblitz_v10_bdp_cb_ct_higgs.cpp — Higgs variant of colblitz_v10_bdp_cb_ct.
 *
 * Forces the four corners (cells 0, 4, 20, 24 — top-left, top-right,
 * bottom-left, bottom-right) as the first four clicks, then delegates all
 * subsequent decisions to ColblitzV10BdpCbCtOTStrategy.
 *
 * Implementation: thin wrapper via #include of the base file (with
 * HIGGS_DELEGATE defined to suppress the base extern "C" exports).
 */

#define HIGGS_DELEGATE
#include "colblitz_v10_bdp_cb_ct.cpp"

// ---------------------------------------------------------------------------
// Higgs wrapper
// ---------------------------------------------------------------------------

static constexpr int HIGGS_CORNERS[4] = {0, 4, 20, 24};

class ColblitzV10BdpCbCtHiggsOTStrategy : public OTStrategy {
    ColblitzV10BdpCbCtOTStrategy base_;

    // Returns the corner cell to play if we are still in the opening (fewer
    // than 4 cells clicked), or -1 if the opening is complete.
    static int higgsCorner(uint32_t clickedMask) {
        if (__builtin_popcount(clickedMask) >= 4) return -1;
        for (int corner : HIGGS_CORNERS)
            if (!((clickedMask >> corner) & 1)) return corner;
        return -1;
    }

    static uint32_t buildClickedMask(const std::vector<Cell>& board) {
        uint32_t mask = 0;
        for (const Cell& c : board)
            if (c.clicked) mask |= 1u << (c.row * 5 + c.col);
        return mask;
    }

public:
    void init_evaluation_run() override { base_.init_evaluation_run(); }

    void next_click(const std::vector<Cell>& board,
                    const std::string& meta_json,
                    ClickResult& out) override
    {
        int corner = higgsCorner(buildClickedMask(board));
        if (corner >= 0) {
            out.row = corner / 5;
            out.col = corner % 5;
            return;
        }
        base_.next_click(board, meta_json, out);
    }

    void next_click_with_sv(const std::vector<Cell>& board,
                             const std::string& meta_json,
                             const int* sv_ptr, int sv_len,
                             ClickResult& out)
    {
        int corner = higgsCorner(buildClickedMask(board));
        if (corner >= 0) {
            out.row = corner / 5;
            out.col = corner % 5;
            return;
        }
        base_.next_click_with_sv(board, meta_json, sv_ptr, sv_len, out);
    }
};

// ---------------------------------------------------------------------------
// C exports required by the harness
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new ColblitzV10BdpCbCtHiggsOTStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* inst) {
    static_cast<ColblitzV10BdpCbCtHiggsOTStrategy*>(inst)->init_evaluation_run();
}

extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {
    static_cast<ColblitzV10BdpCbCtHiggsOTStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<ColblitzV10BdpCbCtHiggsOTStrategy*>(inst);
    std::vector<Cell> brd = parse_board_json(board_json ? board_json : "[]");
    ClickResult out;
    s->next_click(brd, meta_json ? meta_json : "{}", out);
    buf = "{\"row\":" + std::to_string(out.row) + ",\"col\":" + std::to_string(out.col) + "}";
    return buf.c_str();
}

extern "C" const char* strategy_next_click_sv(void* inst,
                                               const char* board_json,
                                               const char* meta_json,
                                               const int*  sv_ptr,
                                               int         sv_len)
{
    thread_local static std::string buf;
    auto* s = static_cast<ColblitzV10BdpCbCtHiggsOTStrategy*>(inst);
    std::vector<Cell> brd = parse_board_json(board_json ? board_json : "[]");
    ClickResult out;
    s->next_click_with_sv(brd, meta_json ? meta_json : "{}", sv_ptr, sv_len, out);
    buf = "{\"row\":" + std::to_string(out.row) + ",\"col\":" + std::to_string(out.col) + "}";
    return buf.c_str();
}
