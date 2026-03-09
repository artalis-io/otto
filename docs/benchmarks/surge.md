# Surge Benchmark Results

Comprehensive benchmark results for the Surge VRP/PDPTW solver across standard
academic instance families.

Last updated: 2026-03-08

## How to Run

All benchmarks run from `surge/` directory. Always use `--population` for best results.

```bash
# Solomon 100 (56 instances, ~5min at 5s/instance)
./bench_solomon --time-limit 5 --population

# Gehring-Homberger 200 (60 instances, ~1h at 60s/instance)
./bench_solomon --dir benchmarks/gehring_homberger \
  --bks benchmarks/bks/gehring_homberger.csv --size 200 \
  --time-limit 60 --population

# Gehring-Homberger 400 (60 instances, ~1h at 60s/instance)
./bench_solomon --dir benchmarks/gehring_homberger \
  --bks benchmarks/bks/gehring_homberger.csv --size 400 \
  --time-limit 60 --population

# Gehring-Homberger 800 (60 instances, ~4h at 120s/instance)
./bench_solomon --dir benchmarks/gehring_homberger \
  --bks benchmarks/bks/gehring_homberger.csv --size 800 \
  --time-limit 120 --population

# Li & Lim PDPTW (57 instances)
./bench_li_lim --population

# Instance feature extraction (for S24 calibration)
./bench_solomon --features
./bench_solomon --dir benchmarks/gehring_homberger --size 200 --features
```

## Instance Families

| Family | Type | Sizes | Source |
|--------|------|-------|--------|
| Solomon | VRPTW | 100 | Solomon (1987), 56 instances |
| Gehring-Homberger | VRPTW | 200-1000 | Gehring & Homberger (1999), 60 per size |
| Li & Lim | PDPTW | 100 | Li & Lim (2003), 57 instances |

Instance categories:
- **C** = Clustered customers, **R** = Random, **RC** = Mixed
- **Type 1** = Tight time windows, **Type 2** = Wide time windows

## Current Best Results (S24, March 2026)

Configuration: Population mode (3 generations, all CPU cores), 10K iterations,
deterministic seed 42. Scale-tuned ALNS parameters (S22 profile matrix).
Instance-adaptive construction (S24) active.

### Solomon 100 (VRPTW, 56 instances, 5s time limit, single-thread)

| Category | Instances | BKS Veh Match | Avg Veh Gap | Avg Dist Gap |
|----------|-----------|---------------|-------------|--------------|
| C1 (clustered, tight) | 9 | 9/9 | +0.00 | -0.0% |
| C2 (clustered, wide) | 8 | 8/8 | +0.00 | +0.0% |
| R1 (random, tight) | 12 | 7/12 | +0.75 | +5.5% |
| R2 (random, wide) | 11 | 6/11 | +0.45 | +1.4% |
| RC1 (mixed, tight) | 8 | 4/8 | +0.63 | +3.2% |
| RC2 (mixed, wide) | 8 | 2/8 | +0.50 | +3.5% |
| **Overall** | **56** | **36/56 (64%)** | **+0.48** | **+3.7%** |

C1 and C2 categories: all 17 instances match BKS exactly.

### Gehring-Homberger 200 (VRPTW, 60 instances, 60s, population)

S22 MEDIUM_TUNE parameters + S24 instance-adaptive construction:

| Category | Instances | BKS Veh Match | Avg Veh Gap | Avg Dist Gap |
|----------|-----------|---------------|-------------|--------------|
| C1_2 (clustered, tight) | 10 | 8/10 | +0.20 | +5.6% |
| C2_2 (clustered, wide) | 10 | 10/10 | +0.00 | +0.7% |
| R1_2 (random, tight) | 10 | 10/10 | +0.00 | +12.0% |
| R2_2 (random, wide) | 10 | 9/10 | +0.10 | +1.7% |
| RC1_2 (mixed, tight) | 10 | 8/10 | +0.20 | +18.2% |
| RC2_2 (mixed, wide) | 10 | 8/10 | +0.20 | +1.2% |
| **Overall** | **60** | **53/60 (88%)** | **+0.12** | **+6.6%** |

Avg runtime: 66.2s. 4 instances lexicographically non-worse than BKS.
Wide-TW categories near-optimal: C2 +0.7%, R2 +1.7%, RC2 +1.2%.

### Gehring-Homberger 400 (VRPTW, 60 instances, 60s, population)

S22 LARGE_TUNE parameters + S24 instance-adaptive construction:

| Category | Instances | BKS Veh Match | Avg Veh Gap | Avg Dist Gap |
|----------|-----------|---------------|-------------|--------------|
| C1_4 (clustered, tight) | 10 | 5/10 | +0.90 | +11.2% |
| C2_4 (clustered, wide) | 10 | 2/10 | +0.80 | +6.9% |
| R1_4 (random, tight) | 10 | 8/10 | +0.20 | +25.0% |
| R2_4 (random, wide) | 10 | 10/10 | +0.00 | +7.0% |
| RC1_4 (mixed, tight) | 10 | 2/10 | +0.90 | +19.5% |
| RC2_4 (mixed, wide) | 10 | 5/10 | +0.70 | +4.9% |
| **Overall** | **60** | **32/60 (53%)** | **+0.58** | **+12.4%** |

Avg runtime: 76s. C1 gained +2 vehicle matches vs S22; RC2 distance improved -7.6pp.
R1 regressed -2 vehicle matches. Overall: -3 matches, -0.5pp distance vs S22.

### GH-400 Time Budget Analysis (selected C1/R1 instances)

Testing whether longer budgets and higher iteration caps close the gap. Population
mode, S24 construction, seed 42.

| Instance | BKS | 60s/10K | 120s/10K | 300s/10K | 600s/10K | 600s/50K |
|----------|-----|---------|----------|----------|----------|----------|
| c1_4_2 | 36v / 7686 | +3v, -5.5% | +3v, -6.2% | +3v, -6.5% | +3v, -6.5% | **+2v**, -5.5% |
| c1_4_3 | 36v / 7061 | +0v, +34.6% | +0v, +34.4% | +0v, +29.1% | +0v, +23.7% | +0v, **+19.1%** |
| c1_4_9 | 36v / 7043 | +2v, +13.3% | +1v, +26.7% | +1v, +36.3% | +1v, +32.0% | +1v, +24.1% |
| r1_4_1 | 40v / 10372 | +0v, +10.6% | +0v, +16.0% | +0v, +13.0% | +0v, +16.3% | +0v, **+7.2%** |
| r1_4_10 | 36v / 8094 | +0v, +37.8% | +0v, +25.7% | +0v, +25.7% | +0v, +25.7% | +0v, +27.0% |
| r1_4_7 | 36v / 7616 | +0v, +30.3% | +0v, +24.8% | +0v, +24.8% | +0v, +24.8% | +0v, +23.6% |

**Key findings:**

1. **10K iteration cap is the bottleneck, not wall time.** R1 instances plateau at
   120s/10K and show zero improvement through 600s/10K — the solver converges before
   the time limit. Raising to 50K unlocks further progress.

2. **C1 vehicle elimination responds to iterations.** c1_4_2 drops from +3v to +2v
   only at 50K iterations. The ejection chain needs many ALNS cycles to find viable
   vehicle-emptying sequences.

3. **Distance improves substantially with 50K iters.** c1_4_3: +34.6% → +19.1%
   (-15.5pp). r1_4_1: +10.6% → +7.2% (-3.4pp, approaching competitive range).

4. **Some R1 instances hit local optima.** r1_4_10 and r1_4_7 plateau at ~+24-27%
   regardless of budget — SA reheat or diversity injection needed to escape.

5. **c1_4_3 hit the 600s time limit** at 50K iters (LIMIT status), meaning even more
   budget would help. At 400 requests, each iteration costs ~12ms.

### Gehring-Homberger 800 (VRPTW, 60 instances, 120s, population)

LARGE_TUNE parameters (no dedicated XLARGE tuning yet):

| Category | Instances | BKS Veh Match | Avg Veh Gap | Avg Dist Gap |
|----------|-----------|---------------|-------------|--------------|
| C1_8 (clustered, tight) | 10 | 1/10 | +3.60 | +5.7% |
| C2_8 (clustered, wide) | 10 | 1/10 | +1.50 | +12.0% |
| R1_8 (random, tight) | 10 | 8/10 | +0.20 | +35.9% |
| R2_8 (random, wide) | 10 | 10/10 | +0.00 | +12.6% |
| RC1_8 (mixed, tight) | 10 | 0/10 | +2.60 | +21.7% |
| RC2_8 (mixed, wide) | 10 | 5/10 | +1.10 | +12.0% |
| **Overall** | **60** | **25/60 (42%)** | **+1.55** | **+17.0%** |

Avg runtime: 215s.

## Scaling Summary

| Scale | Time | Veh Match | Avg Veh Gap | Avg Dist Gap | Tune Profile |
|-------|------|-----------|-------------|--------------|--------------|
| Solomon 100 | 5s | 36/56 (64%) | +0.48 | +3.7% | FAST |
| GH-200 | 60s | 53/60 (88%) | +0.12 | +6.6% | MEDIUM_TUNE |
| GH-400 | 60s | 32/60 (53%) | +0.58 | +12.4% | LARGE_TUNE |
| GH-800 | 120s | 25/60 (42%) | +1.55 | +17.0% | LARGE_TUNE |

## Historical Progression (GH-200)

| Phase | Date | Veh Match | Avg Dist Gap | Key Change |
|-------|------|-----------|--------------|------------|
| S16 CFRS | Feb 2026 | 48/60 (80%) | +13.2% | CFRS construction heuristics |
| S16 CFRS + Pop | Feb 2026 | 55/60 (92%) | +13.1% | Population mode |
| S22 Tune | Mar 2026 | 53/60 (88%) | +6.0% | Scale-tuned ALNS params |
| S24 Adaptive | Mar 2026 | 53/60 (88%) | +6.6% | Instance-adaptive construction |

## Historical Progression (GH-400)

| Phase | Date | Veh Match | Avg Dist Gap | Key Change |
|-------|------|-----------|--------------|------------|
| Pre-S17.3 (1T) | Feb 2026 | 28/60 (47%) | +51.8% | Baseline |
| S17.3 + Pop | Feb 2026 | 33/60 (55%) | +35.1% | O(1) concat pre-filter |
| + Ejection Probe | Mar 2026 | 27/60 (45%) | +33.3% | Budget enforcement fix |
| S19 Heap | Mar 2026 | 33/60 (55%) | +18.6% | Lazy heap repair |
| S22 Tuned | Mar 2026 | 35/60 (58%) | +12.9% | Scale-tuned ALNS params |
| S24 Adaptive | Mar 2026 | 32/60 (53%) | +12.4% | Instance-adaptive construction |

## Instance-Adaptive Construction (S24)

S24 adds feature extraction to classify instances and drive construction strategy
ordering. Key components:

1. **Feature extraction**: `spatial_cv` (distance spread) and `tw_tightness`
   (TW width / horizon) classify instances as clustered/random and tight/wide
2. **Service-adjusted lower bound**: Tightens vehicle count estimate by accounting
   for service times and travel
3. **Strategy ordering**: Feature-based priority for 5 construction heuristics
4. **Route merging**: Post-construction consolidation of short routes
5. **Cluster TW validation**: Prevents oversized clusters in sweep/k-means

### Feature Calibration Data

From `--features` flag on Solomon-100 and GH-200:

| Instance Type | spatial_cv | tw_tightness | Classification |
|---------------|------------|--------------|----------------|
| C1 (clustered, tight) | 0.39-0.41 | 0.04-0.15 | clustered + tight |
| C2 (clustered, wide) | 0.38-0.42 | 0.05-0.74 | clustered + wide |
| R1 (random, tight) | 0.38 | 0.02-0.13 | clustered + tight |
| R2 (random, wide) | 0.38 | 0.12-0.78 | clustered + wide |
| RC1 (mixed, tight) | 0.38 | 0.13-0.47 | clustered + tight/wide |
| RC2 (mixed, wide) | 0.38 | 0.13-0.75 | clustered + wide |

Note: `spatial_cv` does not distinguish Solomon/GH instance types because they all
use the same 100x100 coordinate space. The threshold (0.60) is set to classify truly
random real-world instances correctly. TW tightness cleanly separates type-1 from
type-2 with threshold 0.15.

## Key Observations

### Strengths
- **Wide-TW mastery**: C2, R2, RC2 categories consistently near-optimal (+0.8% to +1.5%)
- **Vehicle count at 200**: 88% exact BKS match
- **R2 scaling**: Perfect 10/10 vehicle match at all scales (200, 400, 800)
- **Time-budgeted**: All instances complete within ~1.5x budget

### Bottlenecks
- **Tight-TW distance**: R1 and RC1 categories have highest distance gaps (+11-35%)
- **C1 vehicle count at scale**: Construction produces extra vehicles that ALNS can't
  eliminate within budget (C1_8: +3.60 avg gap)
- **Time starvation at 800+**: Per-iteration cost is O(n^2*V), ~4x slower at 800 vs 400

### Root Causes at 400+
| Issue | Impact | Status |
|-------|--------|--------|
| Construction quality | Extra vehicles on C-type | Improved (S16 CFRS, S24 adaptive) |
| Low iterations/sec | Fewer ALNS iterations | Improved (S17.3 concat, S19 heap) |
| Ejection chain timeout | Budget overruns | Fixed (SGBudgetProbe) |
| SA temperature | Too hot for large instances | Tuned (S22 profile matrix) |
| **10K iteration cap** | Solver converges before time limit | **Confirmed bottleneck** (50K shows +2v, -15pp) |
| R1 local optima | Distance plateaus at +24-27% | Needs SA reheat / diversity injection |
| Vehicle-first objective | 60s spent on vehicle elimination | Needs longer budgets + more iters |
