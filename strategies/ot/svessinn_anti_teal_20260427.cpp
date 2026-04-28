// sphere:stateless
/**
 * svessinn_anti_teal_20260427.cpp — Anti-Teal strategy.
 *
 * JS mode: "anti-teal" — option value "anti-teal", display "Anti-Teal (Avoid Teal)"
 * Source:  https://github.com/Svessinn/Svessinn.github.io/blob/main/Mudae/Spheres/Trace/solver.html
 *
 * Selection logic (JS case "anti-teal"):
 *
 *   tealFound   = any T revealed on board
 *   greenFound  = any G revealed on board
 *   yellowFound = any Y revealed on board
 *
 *   Per cell:
 *     blueProb   = heatmap[i]["B"]
 *     tealProb   = heatmap[i]["T"]
 *     greenProb  = heatmap[i]["G"]
 *     yellowProb = heatmap[i]["Y"]
 *     shipSum    = sum of all non-B heatmap weights
 *
 *   if (!tealFound):
 *     score = isHuntingBlue ? blueProb - tealProb*10 : shipSum
 *   else if (!greenFound || !yellowFound):
 *     score = isHuntingBlue ? blueProb - tealProb*10 - greenProb*10 - yellowProb*10 : shipSum
 *   else:
 *     score = isHuntingBlue ? blueProb : shipSum
 *
 *   bestIdx = max(score)
 *
 * Rationale: before teal is located, heavily penalise cells where teal might be
 * (minimising teal exposure during the blue-hunt phase). Once teal is found,
 * also deprioritise green/yellow until they are located too. After all three
 * ships are found, fall back to pure blueProb maximisation while hunting blues.
 * In the ship-hunting phase (shipsHit >= 5) all branches reduce to max(shipSum).
 *
 * All engine logic is in svessinn_solver_v2_engine.h.
 */

#include "svessinn_solver_v2_engine.h"

class SvessinnAntiTealOTStrategy : public OTStrategy {
public:
    void init_game_payload(const std::string&) override {}

    void next_click(const std::vector<Cell>& board,
                    const std::string& meta_json,
                    ClickResult& out) override
    {
        SvessinnV2State st = svessinn_v2_parse(board, meta_json);

        // Click any geometrically-certain non-blue cell immediately.
        if (svessinn_v2_certain_early_return(st, out)) return;

        // Determine which large ships have been located yet.
        bool tealFound   = false, greenFound = false, yellowFound = false;
        for (int i = 0; i < 25; ++i) {
            switch (st.revealedArr[i]) {
                case 'T': tealFound   = true; break;
                case 'G': greenFound  = true; break;
                case 'Y': yellowFound = true; break;
                default:  break;
            }
        }

        // JS case "anti-teal": score depends on which ships have been found.
        int      targetIdx = -1;
        double   best      = -std::numeric_limits<double>::infinity();
        uint32_t bits      = st.availMask;

        while (bits) {
            int    idx       = __builtin_ctz(bits); bits &= bits - 1;
            const CellHeat& h = st.dr.heatmap[idx];
            double blueProb   = h.get('B');
            double tealProb   = h.get('T');
            double greenProb  = h.get('G');
            double yellowProb = h.get('Y');
            double shipSum    = h.shipSum();

            double score;
            if (!tealFound) {
                score = st.isHuntingBlue
                    ? blueProb - tealProb * 10.0
                    : shipSum;
            } else if (!greenFound || !yellowFound) {
                score = st.isHuntingBlue
                    ? blueProb - tealProb * 10.0 - greenProb * 10.0 - yellowProb * 10.0
                    : shipSum;
            } else {
                score = st.isHuntingBlue ? blueProb : shipSum;
            }

            if (score > best) { best = score; targetIdx = idx; }
        }

        if (targetIdx == -1) svessinn_v2_fallback(st, out);
        else { out.row = targetIdx / 5; out.col = targetIdx % 5; }
    }
};

SVESSINN_V2_EXPORTS(SvessinnAntiTealOTStrategy)
