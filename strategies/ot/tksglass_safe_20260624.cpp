// sphere:stateless
/**
 * tksglass_safe_20260624.cpp — tksglass OT Visualizer, Safe (default) mode.
 *
 * Source:  https://tksglass.github.io/OT/
 * Synced:  2026-06-24 (commit 68a48151803183bd)
 *
 * forcedStartMode = null (default).
 *   - 6/7-color games start in safe mode (prefer blue-finding) and switch
 *     to hunt mode once ships_hit >= 5.
 *   - 8/9-color games are aggressive from move 1 (getDefaultModeForRareCount
 *     returns "aggressive" for rareCount >= 4).
 *   - Opening: corners {0, 4, 20, 24} for 6/7c; cells {8, 12, 16} for 8/9c.
 *
 * All shared engine logic is in tksglass_20260624_engine.h.
 * For the aggressive variant see tksglass_aggressive_20260624.cpp.
 */

#include "tksglass_20260624_engine.h"

class TKSGlassSafe20260624OTStrategy : public OTStrategy {
public:
    void next_click(const std::vector<Cell>& board,
                    const std::string& meta_json,
                    ClickResult& out) override
    {
        tksglass_next_click(board, meta_json, out, /*forcedAggressive=*/false);
    }
};

TKSGLASS_EXPORTS(TKSGlassSafe20260624OTStrategy)
