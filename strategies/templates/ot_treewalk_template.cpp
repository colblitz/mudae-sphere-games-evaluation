// sphere:stateless
/**
 * ot_treewalk_template.cpp — Template for tree-walk-compatible ot strategies — C++.
 *
 * Copy this file to strategies/ot/<your_name>.cpp, rename the class, and
 * fill in the TODO sections.
 *
 * USE THIS TEMPLATE INSTEAD OF ot_template.cpp IF:
 *   - Your strategy decides which cell to click by filtering a pre-enumerated
 *     set of possible board configurations down to those consistent with the
 *     cells revealed so far (the "board-filter" pattern), AND
 *   - You want the fast tree-walk evaluator (much faster than sequential for
 *     strategies that are deterministic given the same revealed board state).
 *
 * Use ot_template.cpp instead if your strategy is stateful (e.g. it uses
 * random numbers, or its choice depends on history beyond what is visible on
 * the board), or if you are writing in Python or JS (tree-walk is C++ only
 * due to the cost of serialising the board index list across language bridges).
 *
 * HOW THE TREE-WALK EVALUATOR USES THIS TEMPLATE
 * -----------------------------------------------
 * The tree-walk evaluator processes all boards simultaneously in a single DFS
 * over the game tree.  At each node it calls next_click once for the current
 * revealed state, then splits boards by the outcome color at the chosen cell
 * and recurses into each branch.  This eliminates redundant strategy calls on
 * shared game prefixes.
 *
 * The evaluator maintains the FULL-POPULATION surviving board index list
 * (full_sv) at every tree node — the set of all boards (across the entire
 * evaluation, not just the current thread's chunk) that are consistent with
 * the click history so far.  It passes this directly to
 * strategy_next_click_sv so the strategy never needs to recompute it.
 *
 * Parallelism: the board set is split into N chunks, one per thread.  Each
 * thread runs an independent DFS over its chunk and uses its own strategy
 * instance.  The thread's chunk is used only for probability weighting
 * (p_color = branch_size / chunk_size); full_sv is always the full-population
 * surviving set, independent of chunking.  All threads therefore pass the same
 * full_sv at any given tree node, so all threads make the same cell choice —
 * the correctness argument for chunked parallelism is preserved.
 *
 * THE sphere:stateless MARKER
 * ---------------------------
 * The marker on line 1 tells evaluate.py to route this strategy to the
 * tree-walk evaluator.  It carries a semantic contract:
 *
 *   The cell chosen by next_click is a deterministic function of (board, meta)
 *   — the same revealed pattern and game metadata always produce the same cell,
 *   regardless of how many prior calls were made or which thread is calling.
 *
 * For strategies using strategy_next_click_sv this extends to:
 *
 *   The cell chosen is a deterministic function of (board, meta, full_sv).
 *
 * full_sv is provided by the harness and is identical for all threads at any
 * given node, so the contract is preserved.
 *
 * DO NOT add sphere:stateless if your strategy calls rand() / std::mt19937 /
 * any other random source.  The tree walk would produce a deterministic but
 * incorrect EV by assuming the same cell is always chosen at each node.
 *
 * TWO EXECUTION PATHS
 * -------------------
 * This template exports two next_click entry points:
 *
 *   strategy_next_click     — standard entry point, called by the sequential
 *                             evaluator (evaluate_ot) and any harness that does
 *                             not recognise strategy_next_click_sv.  Rebuilds
 *                             the surviving board set from the board argument
 *                             on every call (or uses a delta cache — see
 *                             colblitz_v8_heuristics_stateless.cpp for the
 *                             pattern).
 *
 *   strategy_next_click_sv  — sv-aware entry point, called by the tree-walk
 *                             evaluator (evaluate_ot_treewalk) when this symbol
 *                             is present.  Receives the pre-filtered
 *                             full-population surviving board index list
 *                             directly from the harness — no filtering needed.
 *                             This is the primary fast path.
 *
 * The harness probes for strategy_next_click_sv via dlsym at load time and
 * uses it if found; otherwise it falls back to strategy_next_click.
 *
 * GAME OVERVIEW
 * -------------
 * Grid   : 5×5, all 25 cells start covered
 * Budget : 4 BLUE clicks (ship cells are FREE)
 * Goal   : find ship cells (free) while minimising wasted blue clicks
 *
 * Ships are contiguous horizontal or vertical segments; no overlap.
 * Blue (spB) cells are empty — clicking one uses 1 of your 4 blue budget.
 * Ship cells do not cost a click.
 *
 * Extra Chance rule:
 *   If blue click #4 is spent before ships_hit >= 5, the game continues.
 *   Each additional blue while ships_hit < 5 extends the game.
 *   Once ships_hit >= 5, the next blue ends the game immediately.
 *
 * Ship configs by n_colors:
 *   6: teal(4)+green(3)+yellow(3)+orange(2)+light(2)  → 14 ship, 11 blue
 *   7: + dark(2)   → 16 ship,  9 blue
 *   8: + red(2)    → 18 ship,  7 blue
 *   9: + white(2)  → 20 ship,  5 blue
 *
 * COLOR REFERENCE (ot)
 * --------------------
 *   spT   teal    ship of length 4
 *   spG   green   ship of length 3
 *   spY   yellow  ship of length 3
 *   spO   orange  ship of length 2 (always present)
 *   spL   light   ship of length 2 (6-color and above)
 *   spD   dark    ship of length 2 (7-color and above)
 *   spR   red     ship of length 2 (8-color and above)
 *   spW   white   ship of length 2 (9-color only)
 *   spB   blue    empty cell (costs 1 blue click)
 *
 * meta_json keys (ot):
 *   "n_colors"    int   ship color count (6, 7, 8, or 9)
 *   "ships_hit"   int   ship cells revealed so far
 *   "blues_used"  int   blue clicks spent so far
 *   "max_clicks"  int   base blue budget (always 4)
 *
 * EXTERNAL DATA FILES
 * -------------------
 * Board-filter strategies typically load a precomputed board corpus in
 * init_evaluation_run().  Because the tree-walk evaluator initialises one
 * bridge instance per thread (up to 20), you should load the data into a
 * process-wide static cache (guarded by std::call_once) so that decompression
 * runs exactly once regardless of thread count.  See
 * colblitz_v8_heuristics_stateless.cpp for a complete worked example.
 *
 * See also
 * --------
 * strategies/ot/colblitz_v8_heuristics_stateless.cpp — full board-filter
 *   strategy using this pattern (process-wide cache, sv-aware export,
 *   delta-cache fallback for sequential evaluator)
 * strategies/templates/ot_template.cpp — base template for stateful strategies
 *   or strategies that do not need the tree-walk evaluator
 */

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>

#include "../../interface/strategy.h"

using namespace sphere;

// ---------------------------------------------------------------------------
// TODO: board data types
//
// Define whatever structures you need to hold the precomputed board corpus
// loaded in init_evaluation_run().  The surviving board index list (sv) is
// a std::vector<int> of indices into that corpus.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Process-wide board cache (loaded once, shared across all thread instances)
//
// The tree-walk evaluator creates one strategy instance per thread.  Without
// this pattern, each instance would decompress the board files independently.
// std::call_once guarantees the load runs exactly once per process.
// ---------------------------------------------------------------------------

struct MyBoardCache {
    // TODO: add per-variant board arrays here
    // Example: std::vector<int32_t> boards[4]; // indexed by n_rare - 2
    bool loaded = false;
};

static MyBoardCache& board_cache() {
    static MyBoardCache cache;
    return cache;
}
static std::once_flag board_cache_flag;

static void load_board_cache() {
    MyBoardCache& c = board_cache();
    // TODO: load board data files here (once, for all thread instances)
    // Example:
    //   for (int n_colors = 6; n_colors <= 9; ++n_colors) { ... }
    c.loaded = true;
}

// ---------------------------------------------------------------------------
// Helper: read an int from a JSON string (minimal, no external deps)
// ---------------------------------------------------------------------------

static int json_get_int(const char* json, const char* key, int default_val) {
    const char* p = strstr(json, key);
    if (!p) return default_val;
    p += strlen(key);
    while (*p == ':' || *p == ' ') ++p;
    return atoi(p);
}

// ---------------------------------------------------------------------------
// Strategy class
// ---------------------------------------------------------------------------

class MyOTTreewalkStrategy : public OTStrategy {
public:

    // -----------------------------------------------------------------------
    // init_evaluation_run: load board data into the process-wide cache.
    //
    // Called once per bridge instance (N times for N threads), but the actual
    // file I/O runs only on the first call thanks to std::call_once.
    // -----------------------------------------------------------------------

    void init_evaluation_run() override {
        std::call_once(board_cache_flag, load_board_cache);
        // TODO: point any per-instance pointers at board_cache() data here
    }

    // init_game_payload is intentionally omitted.
    //
    // Stateless strategies carry no per-game state: the board argument to
    // next_click fully describes the game history.  The bridge calls
    // init_game_payload before every game to reset game_state, but since
    // this strategy does not use game_state the default no-op is sufficient.

    // -----------------------------------------------------------------------
    // score: given the surviving board index list and the unclicked cells,
    // return the best cell to click.
    //
    // This is separated from next_click / next_click_with_sv so that both
    // entry points share the same logic.
    // -----------------------------------------------------------------------

    int score(const std::vector<int>& sv,
              const std::vector<int>& unclicked,
              int ships_hit, int blues_used, int n_colors)
    {
        (void)ships_hit; (void)blues_used; (void)n_colors;

        if (sv.empty() || unclicked.empty()) return unclicked.empty() ? 0 : unclicked[0];

        // TODO: replace with your scoring logic.
        // sv contains the indices of all boards still consistent with the
        // current revealed state.  unclicked contains the indices (row*5+col)
        // of all cells not yet clicked.
        //
        // Typical pattern:
        //   for each cell in unclicked:
        //     count how many boards in sv have a ship at this cell
        //   return the cell with the highest ship probability

        return unclicked[0];  // placeholder: click first unclicked cell
    }

    // -----------------------------------------------------------------------
    // next_click: fallback entry point (sequential evaluator).
    //
    // Reconstructs the surviving board set by filtering the full corpus
    // against all clicked cells on the board.  Called by evaluate_ot and by
    // any harness that does not provide strategy_next_click_sv.
    // -----------------------------------------------------------------------

    void next_click(const std::vector<Cell>& board,
                    const std::string& meta_json,
                    ClickResult& out) override
    {
        int ships_hit  = json_get_int(meta_json.c_str(), "\"ships_hit\"",  0);
        int blues_used = json_get_int(meta_json.c_str(), "\"blues_used\"", 0);
        int n_colors   = json_get_int(meta_json.c_str(), "\"n_colors\"",   6);

        std::vector<int> unclicked;
        // TODO: also collect revealed cells to use for filtering
        for (const Cell& c : board)
            if (!c.clicked) unclicked.push_back(c.row * 5 + c.col);
        std::sort(unclicked.begin(), unclicked.end());

        if (unclicked.empty()) { out.row = 0; out.col = 0; return; }

        if (!board_cache().loaded) {
            out.row = unclicked[0] / 5;
            out.col = unclicked[0] % 5;
            return;
        }

        // TODO: build sv by starting from all boards and filtering out those
        // inconsistent with the clicked cells on the board.
        // For repeated sequential calls you may want a delta cache here —
        // see colblitz_v8_heuristics_stateless.cpp for the pattern.
        std::vector<int> sv;
        // sv.resize(n_total_boards); std::iota(sv.begin(), sv.end(), 0);
        // for each clicked cell: filter sv ...

        int chosen = score(sv, unclicked, ships_hit, blues_used, n_colors);
        out.row = chosen / 5;
        out.col = chosen % 5;
    }

    // -----------------------------------------------------------------------
    // next_click_with_sv: sv-aware entry point (tree-walk evaluator).
    //
    // Receives the pre-filtered full-population surviving board index list
    // directly from the harness.  No filtering is needed — just score and
    // return.
    //
    // sv_ptr / sv_len: the full-population surviving board indices for this
    //   tree node.  These are maintained by the harness independently of
    //   per-thread chunking, so they always reflect the correct board
    //   distribution for the current revealed state.
    //
    // DO NOT further filter sv based on per-instance state.  sv is already
    // correctly filtered; applying additional filters would give the strategy
    // a distorted view of the board distribution.
    // -----------------------------------------------------------------------

    void next_click_with_sv(const std::vector<Cell>& board,
                             const std::string& meta_json,
                             const int* sv_ptr, int sv_len,
                             ClickResult& out)
    {
        int ships_hit  = json_get_int(meta_json.c_str(), "\"ships_hit\"",  0);
        int blues_used = json_get_int(meta_json.c_str(), "\"blues_used\"", 0);
        int n_colors   = json_get_int(meta_json.c_str(), "\"n_colors\"",   6);

        std::vector<int> unclicked;
        for (const Cell& c : board)
            if (!c.clicked) unclicked.push_back(c.row * 5 + c.col);
        std::sort(unclicked.begin(), unclicked.end());

        if (unclicked.empty()) { out.row = 0; out.col = 0; return; }

        if (sv_len == 0) {
            out.row = unclicked[0] / 5;
            out.col = unclicked[0] % 5;
            return;
        }

        // Use sv directly — harness has already filtered it.
        std::vector<int> sv(sv_ptr, sv_ptr + sv_len);

        int chosen = score(sv, unclicked, ships_hit, blues_used, n_colors);
        out.row = chosen / 5;
        out.col = chosen % 5;
    }
};

// ---------------------------------------------------------------------------
// Shared board JSON parser used by both C exports below
// ---------------------------------------------------------------------------

static std::vector<Cell> parse_board_json(const char* board_json) {
    std::vector<Cell> brd;
    brd.reserve(25);
    const char* p = board_json;
    while ((p = strstr(p, "\"row\":")) != nullptr) {
        Cell c;
        c.row = atoi(p + 6);
        const char* cp = strstr(p, "\"col\":"); if (cp) c.col = atoi(cp + 6);
        const char* colp = strstr(p, "\"color\":\"");
        if (colp) { colp += 9; const char* e = strchr(colp, '"'); if (e) c.color = std::string(colp, e - colp); }
        const char* clkp = strstr(p, "\"clicked\":"); if (clkp) { clkp += 10; while (*clkp == ' ') ++clkp; c.clicked = (strncmp(clkp, "true", 4) == 0); }
        brd.push_back(c); p += 6;
    }
    return brd;
}

// ---------------------------------------------------------------------------
// C exports required by the harness — do not rename these functions
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new MyOTTreewalkStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* inst) {
    static_cast<MyOTTreewalkStrategy*>(inst)->init_evaluation_run();
}

extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {
    static_cast<MyOTTreewalkStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
}

// Standard entry point — used by the sequential evaluator and any harness
// that does not provide strategy_next_click_sv.
extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<MyOTTreewalkStrategy*>(inst);
    std::vector<Cell> brd = parse_board_json(board_json ? board_json : "[]");
    ClickResult out;
    s->next_click(brd, meta_json ? meta_json : "{}", out);
    buf = "{\"row\":" + std::to_string(out.row) + ",\"col\":" + std::to_string(out.col) + "}";
    return buf.c_str();
}

// sv-aware entry point — used by the tree-walk evaluator when this symbol is
// present.  Receives the pre-filtered full-population surviving board index
// list so the strategy can skip its own filtering step entirely.
// The harness detects this symbol via dlsym at load time; if absent it falls
// back to strategy_next_click above.
extern "C" const char* strategy_next_click_sv(void* inst,
                                               const char* board_json,
                                               const char* meta_json,
                                               const int*  sv_ptr,
                                               int         sv_len)
{
    thread_local static std::string buf;
    auto* s = static_cast<MyOTTreewalkStrategy*>(inst);
    std::vector<Cell> brd = parse_board_json(board_json ? board_json : "[]");
    ClickResult out;
    s->next_click_with_sv(brd, meta_json ? meta_json : "{}", sv_ptr, sv_len, out);
    buf = "{\"row\":" + std::to_string(out.row) + ",\"col\":" + std::to_string(out.col) + "}";
    return buf.c_str();
}
