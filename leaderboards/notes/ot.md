> **Note:** `kelinimo_expectimax_fast.cpp`, `svessinn_solver_fast.cpp`, and `zavex_heuristic_fast.cpp` are C++ ports of their original JS strategies with minor performance tweaks. They are not algorithmically identical to the JS originals — scores may differ slightly due to floating-point arithmetic differences and tiebreak ordering.

> **Theoretical ceiling:** A perfect game collects every cell on the board. Per-color perfect-game SP and the empirically-weighted ceiling (using mode frequencies from `data/trace_board_stats.json`, 1177 observed games; 9-color has 0 observed occurrences and receives a token weight of 1):
>
> | Mode | Perfect-game SP | Count | Rate |
> |------|:--------------:|:-----:|:----:|
> | 6-color | ~858 SP | 870 | 73.85% |
> | 7-color | ~1055 SP | 276 | 23.43% |
> | 8-color | ~1253 SP | 31 | 2.63% |
> | 9-color | ~1450 SP | 0 (token: 1) | ~0% |
>
> **Overall empirical ceiling: ~915 SP.** Per-color values assume ~109 SP/cell for var-rare ship cells (empirical EV weighted across spL/spD/spR/spW by observed appearance rates in the same dataset).
