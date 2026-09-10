# NETLIB Performance Baseline — Ralph vs GLPK

**Captured:** 2026-09-10 at commit `f57253e9` on Apple M1 Max, Darwin 25.6.0 arm64  
**Settings:** presolve=1, method=auto(2), detect_special=0, hard_cap=30s; GLPK via glpsol

> ⚠️ **Read this first.** The ralph-benchmark tool routes 18 numerically-hard problems through the EXTERNAL GLPK provider (solve_path='external_glpk_oop'); for those rows the 'ralph_*' numbers are GLPK's, not native Ralph's. Only the 66 solve_path='native' rows reflect Ralph's own solver. See the native_only_summary and docs/roadmaps/ralph-vs-glop.md for the honest picture.

The `ralph-benchmark` tool opts the hard tail into the external GLPK backend (`*_EXTERNAL` algorithm + `lp_external_provider=GLPK`); the default library LP path is internal-first and does **not** auto-route to external. So these rows are a *tool* artifact, not the library silently wrapping GLPK — but a 'Ralph vs GLPK' table must separate them, which earlier versions of this file did not.

## Native-only summary (the 66 rows Ralph actually solved itself)

| Metric | Value |
|--------|-------|
| Problems (native) | 66 |
| Objective matches GLPK | 66/66 |
| Identical iteration count to GLPK | 0/66 (≈0 → genuinely independent solver) |
| Iteration ratio Ralph/GLPK (geomean / median) | 0.9674 / 1.1687 |
| Wall-time ratio Ralph/GLPK (geomean / median) | 0.7692 / 0.7191 |
| Wall-time ratio, native GLPK≥50ms subset (geomean) | 1.1424 |

## Hard tail (18 externally-routed) — native reality

Of the 18 externally-routed problems, native Ralph within a 15s cap: 1 solved (pilot.we, 4.2s); 2 report ITERATION_LIMIT non-convergence (25fv47, pilot) -- it does NOT return a false optimum; 15 exceed 15s (e.g. 80bau3b ~172-742s vs GLPK 0.6s). This tail is where GLPK is far ahead and why the benchmark offloads it.

Externally routed: `25fv47`, `80bau3b`, `cycle`, `d2q06c`, `fit1p`, `fit2p`, `greenbea`, `greenbeb`, `maros-r7`, `perold`, `pilot`, `pilot.ja`, `pilot.we`, `pilot4`, `pilot87`, `pilotnov`, `sierra`, `woodw`

## All-84 summary (MIXED native + external — do not read as pure Ralph)

| Metric | Value |
|--------|-------|
| Objective matches GLPK | 84/84 |
| Iteration ratio (geomean / median) | 0.9743 / 1.0 |
| Wall-time ratio (geomean / median) | 0.8165 / 0.9947 |

## Per-problem (sorted by nnz) — `path` column: native vs external_glpk_oop

| problem | path | nnz | R iters | G iters | it R/G | R ms | G ms | t R/G | obj match |
|---------|------|----:|--------:|--------:|-------:|-----:|-----:|------:|:---------:|
| afiro | native | 83 | 12 | 10 | 1.20 | 0.7 | 13.3 | 0.05 | yes |
| sc50b | native | 118 | 31 | 30 | 1.03 | 0.9 | 15.9 | 0.06 | yes |
| sc50a | native | 130 | 33 | 26 | 1.27 | 1.0 | 15.8 | 0.06 | yes |
| sc105 | native | 280 | 62 | 51 | 1.22 | 1.6 | 16.0 | 0.10 | yes |
| kb2 | native | 286 | 70 | 39 | 1.79 | 1.3 | 13.4 | 0.09 | yes |
| adlittle | native | 383 | 132 | 86 | 1.53 | 2.1 | 13.7 | 0.15 | yes |
| scagr7 | native | 420 | 54 | 84 | 0.64 | 3.2 | 16.1 | 0.20 | yes |
| stocfor1 | native | 447 | 44 | 52 | 0.85 | 2.2 | 11.8 | 0.19 | yes |
| blend | native | 491 | 67 | 40 | 1.68 | 2.1 | 11.3 | 0.19 | yes |
| sc205 | native | 551 | 142 | 117 | 1.21 | 4.2 | 11.2 | 0.37 | yes |
| recipe | native | 663 | 37 | 15 | 2.47 | 1.8 | 16.0 | 0.11 | yes |
| share2b | native | 694 | 52 | 73 | 0.71 | 2.3 | 15.5 | 0.15 | yes |
| lotfi | native | 1078 | 126 | 103 | 1.22 | 4.0 | 13.9 | 0.29 | yes |
| share1b | native | 1151 | 75 | 172 | 0.44 | 5.9 | 12.8 | 0.46 | yes |
| scorpion | native | 1426 | 51 | 159 | 0.32 | 11.9 | 12.8 | 0.93 | yes |
| bore3d | native | 1429 | 45 | 31 | 1.45 | 18.2 | 14.4 | 1.26 | yes |
| scagr25 | native | 1554 | 319 | 260 | 1.23 | 25.2 | 14.7 | 1.72 | yes |
| sctap1 | native | 1692 | 55 | 167 | 0.33 | 10.6 | 16.8 | 0.63 | yes |
| capri | native | 1767 | 98 | 207 | 0.47 | 133.4 | 12.3 | 10.85 | yes |
| brandy | native | 2148 | 148 | 210 | 0.70 | 22.2 | 13.7 | 1.62 | yes |
| israel | native | 2269 | 168 | 111 | 1.51 | 5.2 | 14.7 | 0.35 | yes |
| finnis | native | 2310 | 328 | 379 | 0.87 | 19.6 | 27.5 | 0.71 | yes |
| scsd1 | native | 2388 | 178 | 83 | 2.14 | 6.0 | 15.3 | 0.39 | yes |
| etamacro | native | 2409 | 460 | 452 | 1.02 | 38.2 | 25.9 | 1.47 | yes |
| agg | native | 2410 | 52 | 76 | 0.68 | 6.8 | 12.9 | 0.53 | yes |
| bandm | native | 2494 | 273 | 240 | 1.14 | 32.0 | 14.7 | 2.17 | yes |
| e226 | native | 2578 | 329 | 237 | 1.39 | 11.6 | 15.0 | 0.77 | yes |
| scfxm1 | native | 2589 | 162 | 222 | 0.73 | 18.6 | 17.4 | 1.07 | yes |
| grow7 | native | 2612 | 191 | 169 | 1.13 | 6.8 | 15.2 | 0.45 | yes |
| standata | native | 3031 | 37 | 128 | 0.29 | 8.5 | 17.4 | 0.49 | yes |
| beaconfd | native | 3375 | 40 | 14 | 2.86 | 7.1 | 15.5 | 0.46 | yes |
| shell | native | 3556 | 163 | 422 | 0.39 | 21.6 | 33.0 | 0.65 | yes |
| standmps | native | 3679 | 109 | 259 | 0.42 | 18.2 | 14.7 | 1.24 | yes |
| stair | native | 3856 | 74 | 434 | 0.17 | 80.3 | 30.1 | 2.67 | yes |
| degen2 | native | 3978 | 447 | 443 | 1.01 | 109.3 | 27.7 | 3.94 | yes |
| agg2 | native | 4284 | 112 | 201 | 0.56 | 9.9 | 16.4 | 0.60 | yes |
| agg3 | native | 4300 | 166 | 189 | 0.88 | 11.7 | 16.5 | 0.71 | yes |
| scsd6 | native | 4316 | 469 | 162 | 2.90 | 13.7 | 19.0 | 0.72 | yes |
| ship04s | native | 4352 | 84 | 224 | 0.38 | 15.4 | 28.3 | 0.54 | yes |
| seba | native | 4367 | 22 | 258 | 0.09 | 29.3 | 28.8 | 1.02 | yes |
| forplan | native | 4564 | 84 | 118 | 0.71 | 14.2 | 16.6 | 0.85 | yes |
| bnl1 | native | 5121 | 350 | 605 | 0.58 | 227.5 | 42.3 | 5.38 | yes |
| pilot4 | **ext-glpk** | 5141 | 523 | 523 | 1.00 | 49.7 | 49.2 | 1.01 | yes |
| scfxm2 | native | 5183 | 451 | 441 | 1.02 | 61.8 | 32.3 | 1.91 | yes |
| grow15 | native | 5620 | 523 | 380 | 1.38 | 40.7 | 30.0 | 1.35 | yes |
| perold | **ext-glpk** | 6018 | 947 | 947 | 1.00 | 79.7 | 73.2 | 1.09 | yes |
| fffff800 | native | 6227 | 900 | 227 | 3.96 | 41.5 | 29.8 | 1.39 | yes |
| ship04l | native | 6332 | 115 | 335 | 0.34 | 21.8 | 29.8 | 0.73 | yes |
| sctap2 | native | 6714 | 99 | 381 | 0.26 | 411.7 | 34.2 | 12.05 | yes |
| ganges | native | 6912 | 1480 | 733 | 2.02 | 49.5 | 42.4 | 1.17 | yes |
| ship08s | native | 7114 | 675 | 326 | 2.07 | 22.3 | 32.0 | 0.70 | yes |
| sierra | **ext-glpk** | 7302 | 998 | 998 | 1.00 | 61.9 | 65.6 | 0.94 | yes |
| scfxm3 | native | 7777 | 568 | 686 | 0.83 | 139.0 | 48.5 | 2.87 | yes |
| ship12s | native | 8178 | 1126 | 365 | 3.08 | 36.0 | 32.0 | 1.12 | yes |
| grow22 | native | 8252 | 851 | 519 | 1.64 | 86.0 | 54.5 | 1.58 | yes |
| stocfor2 | native | 8343 | 3039 | 873 | 3.48 | 465.4 | 88.0 | 5.29 | yes |
| scsd8 | native | 8584 | 1370 | 472 | 2.90 | 65.6 | 47.6 | 1.38 | yes |
| sctap3 | native | 8874 | 154 | 629 | 0.24 | 552.9 | 51.5 | 10.73 | yes |
| pilot.we | **ext-glpk** | 9126 | 1287 | 1287 | 1.00 | 121.5 | 107.7 | 1.13 | yes |
| maros | native | 9614 | 859 | 708 | 1.21 | 652.3 | 47.1 | 13.85 | yes |
| fit1p | **ext-glpk** | 9868 | 390 | 390 | 1.00 | 40.9 | 42.0 | 0.97 | yes |
| 25fv47 | **ext-glpk** | 10400 | 1592 | 1592 | 1.00 | 154.3 | 155.5 | 0.99 | yes |
| czprob | native | 10669 | 1349 | 850 | 1.59 | 142.1 | 57.6 | 2.47 | yes |
| ship08l | native | 12802 | 722 | 539 | 1.34 | 29.0 | 53.5 | 0.54 | yes |
| pilotnov | **ext-glpk** | 13057 | 851 | 851 | 1.00 | 90.2 | 96.2 | 0.94 | yes |
| fit1d | native | 13404 | 1624 | 519 | 3.13 | 15.6 | 52.4 | 0.30 | yes |
| nesm | native | 13680 | 3282 | 2582 | 1.27 | 273.3 | 203.2 | 1.35 | yes |
| bnl2 | native | 13999 | 3407 | 2217 | 1.54 | 537.7 | 279.6 | 1.92 | yes |
| pilot.ja | **ext-glpk** | 14698 | 1413 | 1413 | 1.00 | 156.1 | 151.4 | 1.03 | yes |
| ship12l | native | 16170 | 1184 | 713 | 1.66 | 46.5 | 66.7 | 0.70 | yes |
| cycle | **ext-glpk** | 20720 | 542 | 542 | 1.00 | 84.0 | 74.4 | 1.13 | yes |
| 80bau3b | **ext-glpk** | 21002 | 5591 | 5591 | 1.00 | 625.7 | 609.1 | 1.03 | yes |
| degen3 | native | 24646 | 4591 | 1946 | 2.36 | 1433.5 | 243.2 | 5.89 | yes |
| greenbea | **ext-glpk** | 30877 | 3137 | 3137 | 1.00 | 442.7 | 444.0 | 1.00 | yes |
| greenbeb | **ext-glpk** | 30877 | 2143 | 2143 | 1.00 | 322.4 | 318.7 | 1.01 | yes |
| d2q06c | **ext-glpk** | 32417 | 5546 | 5546 | 1.00 | 1314.4 | 1301.9 | 1.01 | yes |
| woodw | **ext-glpk** | 37474 | 928 | 928 | 1.00 | 126.3 | 121.7 | 1.04 | yes |
| d6cube | native | 37704 | 523 | 8767 | 0.06 | 273.9 | 1563.8 | 0.18 | yes |
| pilot | **ext-glpk** | 43167 | 5359 | 5359 | 1.00 | 1579.6 | 1575.7 | 1.00 | yes |
| fit2p | **ext-glpk** | 50284 | 6603 | 6603 | 1.00 | 1294.5 | 1291.3 | 1.00 | yes |
| wood1p | native | 70215 | 229 | 320 | 0.72 | 74.1 | 146.4 | 0.51 | yes |
| pilot87 | **ext-glpk** | 73152 | 7582 | 7582 | 1.00 | 3964.4 | 3968.9 | 1.00 | yes |
| fit2d | native | 129018 | 29620 | 5665 | 5.23 | 408.8 | 2436.7 | 0.17 | yes |
| maros-r7 | **ext-glpk** | 144848 | 5821 | 5821 | 1.00 | 1812.1 | 1811.9 | 1.00 | yes |
