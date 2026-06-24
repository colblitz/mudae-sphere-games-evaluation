// sphere:stateless
/**
 * tksglass_aggressive_20260624.cpp — tksglass OT Visualizer, Aggressive mode.
 *
 * Source:  https://tksglass.github.io/OT/
 * Synced:  2026-06-24 (commit 68a48151803183bd)
 *
 * forcedStartMode = "aggressive".
 *   - Hunt mode active from move 1 for all color counts (ships_hit check
 *     is bypassed; mode = "aggressive" → huntMode = true always).
 *   - Opening: center-adjacent {7, 11, 13, 17} for 6/7c; cells {8, 12, 16}
 *     for 8/9c (use89ColorOpening takes precedence, unchanged from safe).
 *
 * All shared engine logic is in tksglass_20260624_engine.h.
 * For the safe/default variant see tksglass_safe_20260624.cpp.
 */

#include "tksglass_20260624_engine.h"

class TKSGlassAggressive20260624OTStrategy : public OTStrategy {
public:
    void next_click(const std::vector<Cell>& board,
                    const std::string& meta_json,
                    ClickResult& out) override
    {
        tksglass_next_click(board, meta_json, out, /*forcedAggressive=*/true);
    }
};

TKSGLASS_EXPORTS(TKSGlassAggressive20260624OTStrategy)
