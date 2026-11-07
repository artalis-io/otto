# Loading: HGS quality and performance regression protocol

Hard loading remains opt-in. Passing packing correctness tests is insufficient
evidence for activation: exact checks during insertion and disabled concat
shortcuts can reduce useful search throughput. This suite measures that cost
without changing deployed binaries, Cursus pins, customer inputs or constraints.

## Comparisons and budgets

Use the pre-loading main revision `ae414dc1387209fced770607a8f30a91a005187f`
and merged P2 `99de6cade6a118569249da1dda785f8145fa9616`. Compile the **same**
`surge/benchmarks/bench_loading_driver.c` against both libraries, with identical
release compiler/flags. Record compiler version, architecture, source revisions,
executable SHA-256 and input fingerprints. Different architectures do not imply
identical binaries or timing.

The adapter calls `sg_solve_population`, never substitutes single-run ALNS. It
rechecks the complete solution with soft penalties disabled before emitting a
result. It adds no instrumentation to the production library. The runner invokes
one child at a time and alternates AB/BA ordering by seed (three-way hard-mode
comparisons reverse the entire ordering). Default paired seeds: 42, 31415, 2718.
Three seeds are an initial regression screen, not a statistical guarantee.

- **Disabled parity:** old and current binaries, identical loading-absent input.
- **Nonbinding:** current absent versus hard loading, with one floor position
  per unit permitted by scalar capacity. Physical constraints provably cannot
  exclude a scalar-feasible route. No customer properties are inferred.
- **Binding:** public 66-order fixtures with four physical cells per truck,
  enough vehicles for complete service, and side-free/rear-LIFO access.
- **Small exact:** six orders, exhaustive physical placement and routing/route
  partition enumeration. The Python reference calls no production checker or
  certificate validator. Its deliberately narrow supported domain is checked.
- **Scalar-equivalent binding reference:** `--equivalent-only` compares the
  old solver with scalar capacity four against hard loading with four physical
  cells and the original scalar capacity six/eleven. For these explicitly
  identical public units, side-free and rear access both permit every ordered
  subset of up to four units. The feasible routing sets are identical, unlike
  comparing a physically restricted plan against unconstrained capacity eleven.
  Use the same 0.75 throughput review target and ordinary quality gates. This
  supplemental comparison is declared separately before its measurements.
- **Manual changes:** physically overloaded/reordered warm input, committed and
  frozen assignments, HGS repair and reload-round replay.
- **Mixed/support-bridge exact cases:** `--adversarial-small` tests two six-order
  inputs with directed stack classes and rear unloading. In the bridge case,
  two class-0 units cannot share one slot until a class-1 support unit is added.
  This non-monotone feasibility case exposes search-quality issues that uniform
  pallet fixtures cannot. All quality gaps are measured against the independent
  exact reference, not the less-constrained old plan.

Run equal **wall budgets** and equal **requested iteration budgets**. A
300-iteration HGS budget is split across three generations **per worker**; the
reported count is aggregate executed work, not necessarily exactly 8×300.
Phase minimums can exceed the requested count; early termination and
postprocessing can differ. Compare actual counts as well as requested budgets.
Iteration-budget runs set the
solver wall limit to zero and use an external watchdog; a watchdog expiry is a
failed/incomplete run, never a usable incumbent or performance pass.

Record served/unassigned, vehicles, trips, cost, distance, aggregate iterations,
iterations/second, actual wall time, child CPU seconds and child peak RSS.
Cost is the native model objective in the input's units, not automatically an
application tariff calculation or Hungarian forints. Do not label objective
deltas as business savings without separately applying the application's tariffs.
`wait4` provides per-child resource use on native Linux/macOS; Windows runner
measurement is explicitly unsupported. The synthetic checker independently
replays distance, scalar capacity, complete order coverage and every returned
physical witness, including unloading order and reload boundaries.

## Review gates

`surge/benchmarks/loading_thresholds.json` was fixed before the long-run results:
no paired service loss; no median vehicle increase; at most 5% median cost or
distance increase; disabled throughput ratio ≥0.90; nonbinding ratio ≥0.75;
small exact cases zero vehicle gap and ≤5% distance gap; cancellation target 2s.
These are review/activation gates, not promises that optional hard mode meets
them. Do not silently relax thresholds after seeing results. Distance/cost
comparisons are withheld when coverage differs: less service is not savings.
Binding-case fleet counts must be judged against physical feasibility/exact
references, not unconstrained fleet counts.

CI runs small correctness/harness regressions, not noisy host-dependent speed
assertions. Long measurements belong on an otherwise quiet measured host.
`loading_assess.py` reports each gate, sample count and paired min/median/max;
it exits nonzero for invalid, incomplete or regressed comparisons.

## Build and run

From a current checkout, set `baseline_root` to an external baseline checkout,
`benchmark_output` to an external results directory, and `dayonepath`/`daytwopath`
to existing private native requests. These are operator shell variables, not
application settings. A C toolchain and Python 3 must already be available:

```sh
make -C surge bench_loading_driver bench_loading_probe
make -C "$baseline_root/surge" lib
cc -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE -DSG_HAS_THREADS \
  -I"$baseline_root/surge/include" -I"$baseline_root/arbor/include" \
  -I"$baseline_root/shared/include" surge/benchmarks/bench_loading_driver.c \
  "$baseline_root/surge/libsurge.a" "$baseline_root/arbor/libarbor.a" \
  "$baseline_root/shared/libshared.a" -lm -pthread \
  -o "$baseline_root/bench_loading_driver"
make -C surge loading-benchmark-test

# Public representative cases; benchmark_output must be outside any Git checkout.
python3 surge/benchmarks/loading_quality.py \
  --baseline "$baseline_root/bench_loading_driver" \
  --current surge/bench_loading_driver --output "$benchmark_output/synthetic" \
  --threads 8 --seconds 30 --iterations 300 --seeds 42 31415 2718 \
  --budgets wall iterations \
  --baseline-revision ae414dc1387209fced770607a8f30a91a005187f \
  --current-revision "$(git rev-parse HEAD)"
python3 surge/benchmarks/loading_assess.py "$benchmark_output/synthetic/report.json"

# Small exact references, same public fixtures and independent validation.
python3 surge/benchmarks/loading_quality.py \
  --baseline "$baseline_root/bench_loading_driver" \
  --current surge/bench_loading_driver --output "$benchmark_output/exact" \
  --orders 6 --threads 2 --seconds 3 --iterations 60

# Private solver-ready native request JSONs, NEVER Excel/raw data or an image.
# This preserves every field except explicit benchmark budget/seed/HGS config.
python3 surge/benchmarks/loading_quality.py \
  --baseline "$baseline_root/bench_loading_driver" \
  --current surge/bench_loading_driver --output "$benchmark_output/private-wall600" \
  --external-only --input day1="$dayonepath" --input day2="$daytwopath" \
  --threads 8 --seconds 600 --seeds 42 31415 2718 --budgets wall --watchdog 900
```

Private inputs must not contain a native `loading` declaration. They measure
legacy parity only; real Gyermelyi loading quality still requires verified unit
and vehicle geometry. Original files are read-only inputs, not edited or
overwritten. Generated requests, complete results and stderr remain in a
permission-restricted external directory; do not upload them as public CI
artifacts. The runner refuses Git-checkout output paths, uses hashed artifact
directories, atomic completion records and binary/input identities for resume.
An interrupted or invalid record cannot become a passing result on resume.

To reproduce timing, use the same machine, compiler, threads, source revisions,
inputs and budgets. Avoid competing benchmark jobs. On a shared Spark run one
eight-thread child at a time at reduced scheduling priority, check available
memory/CPU first, and do not touch production containers or ports.

## Packing, cache and cancellation probes

`surge/bench_loading_probe` reports repeated exact-key cache queries versus
cold queries with explicit invalidation, plus a combinatorial no-support case
that reaches its 1,000-node budget 100 times. It checks UNKNOWN rather than
false infeasibility, and cancellation polling after 100 polls. This isolates
packing behavior. It **does not measure the cache hit rate or total UNKNOWN
frequency inside HGS**; native insertion-cache statistics are not physical-cache
statistics. Published certificate node counts likewise are not total search
work. Do not make those claims from these counters.

The adapter optionally accepts a third argument, cancellation delay in ms:
`surge/bench_loading_driver request.json 100`. A watcher calls `sg_cancel` while
HGS runs; the output records whether cancellation was requested and actual
return latency. The external runner kills/reaps its own process group on
watchdog expiry. A bounded CI test checks termination and publication integrity;
the separate 2s latency target must be assessed from actual measurements.
Exploratory controller measurements exceeded the external ten-second watchdog
even with loading absent, on both the pre-loading baseline and P2. Those are
failed measurements, not evidence of bounded cancellation. Keep this limitation
separate from packing cancellation and assess it on the otherwise quiet native
benchmark host before promising a latency bound.

## Boundary issue discovered by this pass

Frozen incoming plans were cleared during worker cloning, causing valid manual
warm starts to fail in both the pre-loading baseline and P2. The follow-up fix
borrows incoming routes in parallel workers and generation zero. Frozen-plan
later generations use complete elite parents: SREX may omit singleton fragments,
so it cannot supply authoritative frozen assignments. Population ALNS and elite
selection remain enabled. New C regression coverage runs parallel/population,
hard/disabled loading, committed work and frozen vehicle preservation under the
normal platform/sanitizer suites. Measurements of the merged P2 executable are
identified separately from this follow-up source; they are not silently relabeled.

Do not deploy or enable hard loading based on this protocol alone. Review actual
measurements, cancellation limits, customer-data readiness and any failed gates.
