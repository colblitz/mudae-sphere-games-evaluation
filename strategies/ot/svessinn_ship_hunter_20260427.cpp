// sphere:stateless
/**
 * svessinn_ship_hunter_20260427.cpp — Aggressive (Ship Hunter) strategy.
 *
 * JS mode: "ship-hunter" — option value "ship-hunter", display "Aggressive (Ship Hunter)"
 * Source:  https://github.com/Svessinn/Svessinn.github.io/blob/main/Mudae/Spheres/Trace/solver.html
 *
 * Selection logic (JS case "ship-hunter"):
 *   finalScore = shipSum   (phase-independent — always maximise ship density)
 *   bestIdx    = max(finalScore)
 *
 * Unlike the standard strategy this mode ignores the blue-hunting phase entirely
 * and always targets the cell with the highest combined ship probability. This
 * aggressively locates ships early but accepts a higher risk of hitting a blue
 * during the first five clicks.
 *
 * All engine logic is in svessinn_solver_v2_engine.h.
 */

#include "svessinn_solver_v2_engine.h"

class SvessinnShipHunterOTStrategy : public OTStrategy {
public:
    void init_game_payload(const std::string&) override {}

    void next_click(const std::vector<Cell>& board,
                    const std::string& meta_json,
                    ClickResult& out) override
    {
        SvessinnV2State st = svessinn_v2_parse(board, meta_json);

        // Click any geometrically-certain non-blue cell immediately.
        if (svessinn_v2_certain_early_return(st, out)) return;

        // JS case "ship-hunter": finalScore = shipSum regardless of phase.
        int      targetIdx = -1;
        double   best      = -1.0;
        uint32_t bits      = st.availMask;

        while (bits) {
            int    idx   = __builtin_ctz(bits); bits &= bits - 1;
            double score = st.dr.heatmap[idx].shipSum();
            if (score > best) { best = score; targetIdx = idx; }
        }

        if (targetIdx == -1) svessinn_v2_fallback(st, out);
        else { out.row = targetIdx / 5; out.col = targetIdx % 5; }
    }
};

SVESSINN_V2_EXPORTS(SvessinnShipHunterOTStrategy)
