// sphere:stateless
/**
 * svessinn_blue_focus_20260427.cpp — Standard (Blue Focus) strategy.
 *
 * JS mode: "standard" — option value "standard", display "Standard (Blue Focus)"
 * Source:  https://github.com/Svessinn/Svessinn.github.io/blob/main/Mudae/Spheres/Trace/solver.html
 *
 * Selection logic (JS case "standard"):
 *   finalScore = isHuntingBlue ? -shipSum : shipSum
 *   bestIdx    = max(finalScore)
 *
 *   Phase "hunting blue" (shipsHit < 5 and blues not exhausted):
 *     → minimise shipSum  (prefer cells least likely to hold a ship)
 *   Phase "hunting ships" (shipsHit >= 5, or all blues found):
 *     → maximise shipSum  (prefer cells most likely to hold a ship)
 *
 * All engine logic is in svessinn_solver_v2_engine.h.
 */

#include "svessinn_solver_v2_engine.h"

class SvessinnBlueFocusOTStrategy : public OTStrategy {
public:
    void init_game_payload(const std::string&) override {}

    void next_click(const std::vector<Cell>& board,
                    const std::string& meta_json,
                    ClickResult& out) override
    {
        SvessinnV2State st = svessinn_v2_parse(board, meta_json);

        // Click any geometrically-certain non-blue cell immediately.
        if (svessinn_v2_certain_early_return(st, out)) return;

        // JS case "standard": finalScore = isHuntingBlue ? -shipSum : shipSum
        // max(finalScore) → min(shipSum) when hunting blue, max(shipSum) when hunting ships.
        int    targetIdx = -1;
        uint32_t bits    = st.availMask;

        if (st.isHuntingBlue) {
            double best = std::numeric_limits<double>::infinity();
            while (bits) {
                int    idx     = __builtin_ctz(bits); bits &= bits - 1;
                double score   = st.dr.heatmap[idx].shipSum();
                if (score < best) { best = score; targetIdx = idx; }
            }
        } else {
            double best = -1.0;
            while (bits) {
                int    idx     = __builtin_ctz(bits); bits &= bits - 1;
                double score   = st.dr.heatmap[idx].shipSum();
                if (score > best) { best = score; targetIdx = idx; }
            }
        }

        if (targetIdx == -1) svessinn_v2_fallback(st, out);
        else { out.row = targetIdx / 5; out.col = targetIdx % 5; }
    }
};

SVESSINN_V2_EXPORTS(SvessinnBlueFocusOTStrategy)
