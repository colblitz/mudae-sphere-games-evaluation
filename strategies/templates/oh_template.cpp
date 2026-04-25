/**
 * oh_template.cpp — Template strategy for /sphere harvest (oh) — C++.
 *
 * Copy this file to strategies/oh/<your_name>.cpp, rename the class, and
 * fill in the TODO sections.  Delete any methods you don't need — only
 * next_click() is required.
 *
 * GAME OVERVIEW
 * -------------
 * Grid     : 5×5
 * Revealed : 10 cells visible at game start; 15 start covered (spU)
 * Budget   : 5 clicks
 * Goal     : maximise total SP collected across your 5 clicks
 *
 * Special click rules:
 *   Purple (spP)  : click is FREE (does not consume a click)
 *   Dark (spD)    : transforms into another color on click;
 *                   if it becomes purple the click is also free
 *   Blue (spB)    : reveals 3 additional covered cells
 *   Teal (spT)    : reveals 1 additional covered cell
 *   ~50% of boards have one "chest" covered cell worth ~345 SP on average
 *
 * COLOR REFERENCE (oh)
 * --------------------
 *   spB   blue    reveals 3 covered cells
 *   spT   teal    reveals 1 covered cell
 *   spG   green   35 SP
 *   spY   yellow  55 SP
 *   spL   light   ~76 SP average
 *   spO   orange  90 SP
 *   spR   red     150 SP
 *   spW   white   ~300 SP
 *   spP   purple  ~5–12 SP, click is FREE
 *   spD   dark    ~104 SP average, transforms on click
 *   spU   covered (unrevealed; directly clickable)
 *
 * STATE MODEL
 * -----------
 * State lives in your class's member variables — not serialised.
 *
 *   init_evaluation_run()   — store read-only run-level data in members
 *   init_game_payload()     — reset per-game members; called before each game
 *   next_click()            — read/update member variables as needed
 *
 * Use init_evaluation_run() for data computed ONCE and shared across all games.
 * Use init_game_payload() to reset per-game bookkeeping at the start of each game.
 *
 * meta_json keys (oh):
 *   "clicks_left"  int   remaining budget
 *   "max_clicks"   int   total budget (always 5)
 *   "game_seed"    int   per-game deterministic seed
 *
 * See also
 * --------
 * oh/load_data.cpp       — init_evaluation_run() loading a data file via interface/data.h
 * oc/global_state.cpp    — init_evaluation_run() (global state) example
 * oq/stateful.cpp        — init_game_payload() + state threading (per-game state) example
 */

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <vector>

#include "../../interface/strategy.h"

// If your strategy needs a large precomputed file (lookup table, policy matrix,
// etc.), include interface/data.h and call sphere::data::fetch() in
// init_evaluation_run().  Small files (≤ ~80 MB compressed) can be committed
// directly to data/ and loaded by path instead (DATA_DIR is set by the Makefile
// via -DREPO_ROOT=...).
//
// Uncomment and adapt the fetch example below if you need a large file:
//
//   #include "../../interface/data.h"
//
//   // External data: <filename>
//   // Size: ~X MB compressed / ~Y GB uncompressed
//   // Hosted at: <url>
//   static const char* LUT_URL    = "https://huggingface.co/datasets/org/repo/resolve/main/<filename>";
//   static const char* LUT_SHA256 = "<hex sha256>";
//   static const char* LUT_FILE   = "<filename>";

using namespace sphere;

class MyOHStrategy : public OHStrategy {
public:
    MyOHStrategy() : rng_(static_cast<uint64_t>(std::time(nullptr))) {}

    // -----------------------------------------------------------------------
    // Optional: global state (computed once, shared across ALL games)
    // -----------------------------------------------------------------------

    /**
     * Called ONCE before all games.  Store board-independent precomputed
     * data in member variables here.
     *
     * Example (large external file via auto-download):
     *   std::string path = sphere::data::fetch(LUT_URL, LUT_SHA256, LUT_FILE);
     *   load_lut(path);  // store result in a member variable
     *
     * Example (small committed file in data/):
     *   std::string path = std::string(REPO_ROOT) + "/data/oh_harvest_lut.bin.lzma";
     *   load_lut(path);
     */
    void init_evaluation_run() override {
        // TODO: precompute global data here, or delete this method
    }

    // -----------------------------------------------------------------------
    // Optional: per-game initialisation
    // -----------------------------------------------------------------------

    /**
     * Reset per-game member variables — called once before each game.
     *
     * @param meta_json  JSON: {"clicks_left":N,"max_clicks":5,"game_seed":K}
     */
    void init_game_payload(const std::string& meta_json) override {
        // TODO: reset per-game fields here, or delete this method
        (void)meta_json;
    }

    // -----------------------------------------------------------------------
    // Required: click decision
    // -----------------------------------------------------------------------

    /**
     * Choose the next cell to click.
     *
     * @param board      All 25 board cells (.row, .col, .color, .clicked).
     * @param meta_json  JSON: {"clicks_left":N,"max_clicks":5,"game_seed":K}
     * @param out        Fill in out.row and out.col.
     *
     * Tips:
     *   - Purple cells ("spP") are free; prioritise them.
     *   - Blue ("spB") and teal ("spT") reveal more cells, increasing info.
     *   - Do not return a (row, col) where board[row*5+col].clicked is true.
     */
    void next_click(const std::vector<Cell>& board,
                    const std::string& meta_json,
                    ClickResult& out) override
    {
        (void)meta_json;

        bool clicked[25] = {};
        std::vector<int> purples;
        std::vector<int> unclicked;

        for (const Cell& c : board)
            if (c.clicked) clicked[c.row * 5 + c.col] = true;

        for (const Cell& c : board)
            if (c.color == "spP" && !c.clicked) purples.push_back(c.row * 5 + c.col);

        for (int i = 0; i < 25; ++i)
            if (!clicked[i]) unclicked.push_back(i);

        // TODO: replace this random fallback with your click logic

        // Prefer any visible purple (free click)
        auto& pool = !purples.empty() ? purples : unclicked;
        int chosen = pool.empty() ? 0
            : pool[std::uniform_int_distribution<int>(0, (int)pool.size() - 1)(rng_)];

        out.row = chosen / 5;
        out.col = chosen % 5;
    }

private:
    std::mt19937_64 rng_;
};

// ---------------------------------------------------------------------------
// C exports required by the harness — do not rename these functions
// ---------------------------------------------------------------------------

extern "C" sphere::StrategyBase* create_strategy()                         { return new MyOHStrategy(); }
extern "C" void                  destroy_strategy(sphere::StrategyBase* s) { delete s; }

extern "C" void strategy_init_evaluation_run(void* inst) {
    static_cast<MyOHStrategy*>(inst)->init_evaluation_run();
}

extern "C" void strategy_init_game_payload(void* inst, const char* meta_json) {
    static_cast<MyOHStrategy*>(inst)->init_game_payload(
        meta_json ? meta_json : "{}");
}

extern "C" const char* strategy_next_click(void* inst,
                                            const char* board_json,
                                            const char* meta_json)
{
    thread_local static std::string buf;
    auto* s = static_cast<MyOHStrategy*>(inst);

    std::vector<Cell> board = parse_board_json(board_json);
    ClickResult out;
    s->next_click(board, meta_json ? meta_json : "{}", out);
    buf = "{\"row\":" + std::to_string(out.row) + ",\"col\":" + std::to_string(out.col) + "}";
    return buf.c_str();
}
