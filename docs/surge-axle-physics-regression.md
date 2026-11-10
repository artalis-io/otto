# Synthetic axle-physics regression coverage

This suite tests the supported `rigid_two_axle_v1` implementation with public,
synthetic inputs. It does not establish accuracy for real vehicles. Customer
activation still needs measured geometry, tare, CG uncertainty and axle limits.
Neither Cursus's OTTO pin nor any deployment or enforcement setting changes here.

## Coverage audit

| Existing coverage | Gap | Change |
| --- | --- | --- |
| 4,291 independent Fraction state/boundary cases | Units in a state share one longitudinal slot position | Retain unchanged; add 2,498 multi-position and metamorphic state checks |
| 512 exhaustive two-slot, up-to-three-unit joint searches | Unit CG offsets are zero; tare is exact | Retain those exact seeded cases; add 256 uncertain-CG/tare searches and independently replay every positive witness |
| 67 native JSON cases, including 12 HGS models and malformed/forged proofs | Few named, analytically specified multi-load boundary/unloading examples | Add 21 named cases: 14 feasible and 7 infeasible, plus 14 public-witness replay round trips |
| C API, OOM, cancellation, sparse-ID/cache, stack/access and route validation | Already covered | Reuse existing suites and JSON witness replayer; do not duplicate them |
| Paired nonbinding V1/V2 HGS measurement tool | Ratios were reported but existing thresholds did not affect its exit status | Use the existing release assessor and unchanged thresholds; add deterministic failure-path tests |

Production solver/search code is unchanged. The C bridge in
`surge/tests/axle_oracle_probe.c` only copies test fields into native inputs and
returns results; it is not linked into production libraries.

## Independent physics and named examples

`surge/tests/test_axle_physics.py` enumerates independent tare and per-unit CG
endpoint combinations using Python `Fraction`. It does not call a native
validator or search routine to calculate expected forces. Random generation is
seeded with `20261110`; failures include the case, profile, unit facts and seed.

The multi-position checks exercise mass permutation/splitting, zero-mass addition,
equivalent global CG descriptions, longitudinal scale invariance, mass scaling,
front/rear reflection, one-gram axle/gross boundaries, front-share thresholds and
128-unit signed-moment cancellation beyond floating-point integer precision.

The named corpus covers empty/full cargo, front/rear bias and overhang,
independent tare and CG uncertainty, literal zero limits, non-integral gram
reactions, an overload smaller than one gram, gross/fraction/minimum-axle boundary
pairs, and counterweight removal. A synthetic 1 t load ahead of the front axle
and 4 t load behind the rear axle is safe together, but removing the front load
first overloads the rear axle. Reversing the delivery order is safe; a same-stop
unload must choose the heavy unit first. These cases test every published load
and unload state through the existing independent JSON witness replayer.

The exhaustive search oracle remains small enough to prove both feasible and
infeasible outcomes. Increasing ordinary random search size is not a substitute
for exhaustive comparison or negative cases.

## Commands and CI

```sh
make -C shared lib
make -C arbor lib
make -C surge surge_solve
make -C surge axle-oracle-test axle-physics-test loading-v2-cli-test
make -C surge loading-benchmark-test
make -C surge test
```

The Linux Surge CI job runs all three oracle/JSON targets. The normal benchmark
harness target runs the new assessor regression tests. Timing thresholds are not
sampled on noisy shared CI runners; their fail-closed behavior is tested there
with deterministic reports. Native C tests retain their existing Windows and
sanitizer coverage. The Python shared-library bridge requires a POSIX compiler.

For actual performance evidence, use the existing **benchmark adapter**, not the
ordinary CLI, and keep measurements outside Git:

```sh
make -C surge bench_loading_driver
python3 surge/benchmarks/axle_nonbinding.py \
  --binary surge/bench_loading_driver \
  --output /external/evidence/axle-physics-screen \
  --orders 66 --seeds 42 31415 2718 --seconds 3 --iterations 50
```

This alternates V1/V2 order, uses actual two-thread HGS and collects CPU, RSS,
wall-time and input/binary hashes. Every V2 result is independently replayed.
The fixture has one floor position per scalar-feasible unit and deliberately
nonbinding axles; comparing stacked V1 and V2 domains as if they were identical
would be invalid. The existing equivalent-feasible-set gates require three
paired seeds, zero paired service loss, no median fleet increase, at most 5%
median distance/cost increase and at least 0.75 median throughput ratio. The tool
now exits nonzero on an invalid/incomplete result or failed gate; no thresholds
were relaxed. Lower service is never reported as efficiency savings.

A short controller screen is regression evidence, not a quiet-host capacity
certificate or real-load calibration. Real geometry, multi-axle groups,
articulated vehicles, suspension dynamics and temporary reshuffling remain
outside this model. Missing measurements need not block unrelated product work.

## Executed validation: 9 November 2026

On the controller's native macOS ARM64 build:

- All 2,498 new multi-position/metamorphic comparisons and 35 named native JSON
  checks passed, alongside the unchanged 4,291 Fraction cases and 67 JSON checks.
- The expanded exhaustive oracle passed 768 cases (257 feasible, 511 infeasible).
- Complete `make -C surge test` passed: 501/501 main tests plus the existing
  transport, time, neighbour, profile, loading, V2, allocation and cancellation
  suites. Native V2 cancellation returned in 0 ms in this run.
- All 16 existing benchmark-harness tests and seven new review-gate tests passed.
- The correct benchmark adapter completed 12 valid 66-order HGS runs, covering
  three seeds under wall and iteration budgets. Every run served all 66 orders
  with six vehicles; paired distance and cost were identical. Median V2/V1
  throughput was 1.001 at equal wall time and 0.899 at equal iterations, passing
  the unchanged 0.75 review floor. The latter records real V2 overhead; it is not
  a claim that axle checking is free.

Evidence is outside Git at
`data/otto-axle-physics-2026-11-09/adapter-screen/report.json` in the controller
workspace. An earlier trial used `surge_solve` instead of the required benchmark
adapter; its invalid output was interrupted and is retained separately under
`nonbinding/`. It is not passing performance evidence. Final performance runs
were serialized after the correctness workloads. Other-platform CI and measured
real loads are separate evidence; neither is claimed by this local run.
