> **Note:** `kelinimo_expectimax_fast.cpp`, `svessinn_solver_fast.cpp`, and `zavex_heuristic_fast.cpp` are C++ ports of their original JS strategies with minor performance tweaks. They are not algorithmically identical to the JS originals — scores may differ slightly due to floating-point arithmetic differences and tiebreak ordering.

> **Theoretical ceiling:** A perfect game collects every cell on the board. Per-color perfect-game SP and the empirically-weighted ceiling (using mode frequencies from `data/trace_board_stats.json`, 1177 observed games; 9-color has 0 observed occurrences and receives a token weight of 1):
>
> | Mode | Perfect-game SP | Count | Rate | E[SP] per var-rare cell |
> |------|:--------------:|:-----:|:----:|:-----------------------:|
> | 6-color | ~811 SP | 870 | 73.85% | ~85 SP (spL/spD only) |
> | 7-color | ~1159 SP | 276 | 23.43% | ~135 SP |
> | 8-color | ~1706 SP | 31 | 2.63% | ~184 SP |
> | 9-color | ~2240 SP | 0 (token: 1) | ~0% | ~208 SP (all four always) |
>
> **Overall empirical ceiling: ~917 SP.** The expected SP per var-rare cell differs by mode because higher modes draw more rare slots, making spR and spW increasingly likely. 6-color can only draw spL or spD; 9-color always has all four (spL + spD + spR + spW = 830 SP across 8 cells = 207.5 SP/cell).
