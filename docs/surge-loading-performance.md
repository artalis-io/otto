# Physical-loading hot-path review

Hard loading remains opt-in. The quality/cancellation review discovered that
nonbinding physical checks could roughly halve useful HGS iteration throughput.
This pass retains the same exact packing rules, node budgets and independent
certificate validation. It changes how often data is copied and repeated checks
are performed, not what constitutes a feasible load.

## Profiling

Build `make -C surge bench_loading_driver_profile` and run it with the same
request JSON accepted by `surge/bench_loading_driver`. The separate instrumented
executable emits a top-level `loading_profile` object with round queries, cache
hits, checker calls/time, and insertion calls/time. Counters are worker-local and
aggregated only after completion; no shared atomic timing counters contend in
the hot path. Normal builds contain neither these counters nor their clock calls.
Never use the instrumented executable for release throughput gates. Insertion
time includes nested round/checker time; do not add those measurements together.
Empty rounds contribute round queries without a checker call or cache hit.

An initial controller sample and instrumented 66-order HGS run found copying,
zeroing, allocation/free and comparison ahead of packing search. The nonbinding
cache already hit approximately 97% of the time. An isolated warm-cache microtest
would have missed the dominant cost: constructing each candidate and copying
full witnesses at every boolean feasibility check. Actual measurements belong
in the external review report; these observations are not an ARM64 speed claim.
Linux perf sampling may be disallowed by the host's security policy. Do not
change host observability permissions for this benchmark; use the separate
counter executable or an available controller sampler instead.

## Implementation boundaries

- An insertion is a read-only view over existing stops plus one synthetic stop.
  It does not allocate/copy a whole candidate route. Its trip_start semantics
  match materialization, including insertion before the first stop and reload
  boundaries. Stop count is not confused with the 128-unit packing bound;
  zero-unit requests remain supported.
- Boolean cache hits return the outcome without copying a placement array.
  Certificate APIs and publication still return/check the complete witness.
  Unit scratch storage is initialized only for populated units, not its full
  maximum capacity on every query.
- A 32-entry worker-local cache supplements the last-hit fast path. Hashes only
  select a slot; ordered units and node budget must match exactly before reuse.
  Geometry matches by immutable owned-descriptor identity or exact descriptor
  bytes. Public setters deep-copy and invalidate the cache before mutation; a
  caller changing its original descriptor cannot change the owned facts. Collisions evict; they never authorize another load. Every cache
  is bounded independently of solve duration and freed with its context/worker.
- Interrupted queries remain UNKNOWN and never enter the cache. Model setters
  retain invalidation. UNKNOWN budget results are keyed by the exact budget.
- Ordinary insertion constraints are checked before physical packing, avoiding
  packing work for already-rejected candidates. Hard loading is still checked
  before accepting the insertion and again at the apply boundary.
- Scalar distance prefilters may reject non-improving local-search moves, as
  they do without hard loading. Every surviving candidate still takes the full
  ordered physical validation path before acceptance. A direct regression shows
  that a scalar-feasible reversal can remain physically invalid and is rejected.
  These prefilters never certify physical feasibility. Search heuristics and
  loading facts are unchanged; no fractional-capacity approximation is added.

## Regression requirements

Run `make -C surge test loading-benchmark-test`; the latter also compiles and
checks the separate profiling build. Direct tests compare insertion views with
materialized candidates across insertion positions, access models and reload
flags, including zero-unit routes longer than 128 stops. Cache tests alternate
more keys than available slots, geometry, feasible/infeasible answers and hot
certificate reads, comparing with the pure checker and independent validator.
Existing randomized exhaustive packing oracles, mixed-class/support-bridge HGS
references, manual/frozen regressions and cancellation checks remain required.

Use the unmodified gates in [surge-loading-benchmarks.md](surge-loading-benchmarks.md)
and [surge-native-cancellation.md](surge-native-cancellation.md): paired seeds,
wall and requested-iteration budgets, representative/exact/equivalent fixtures,
and real native cancellation. Include a 600-second nonbinding comparison. Keep
raw results, executable/source identities and resource measurements outside Git.
Do not relax failed gates or claim real customer loading validation without
verified physical loading facts. This work does not deploy or activate loading.
