# Native cooperative cancellation

`sg_cancel` may be called from another thread while `sg_solve`,
`sg_solve_parallel`, or `sg_solve_population` is running. The context must remain
alive until solve returns. Do not concurrently modify its model or configuration.
Workers join before the enclosing solve releases their contexts or returns.
A subsequent solve resets the request and may reuse the context.

Workers read their own and their parent's atomic cancellation flags directly.
Cancellation no longer depends on a progress callback reaching a segment
boundary. Arbor has a separate cheap per-iteration cancellation callback;
progress reporting, penalty updates and operator weight segments retain their
existing schedule. Without cancellation, these checks do not consume RNG or
change iteration budgets, constraints or search decisions.

Construction, insertion/repair and merge loops stop at complete move boundaries.
Existing postprocessing deadline probes also observe cancellation, including
when the wall-time limit is unlimited. Recursive ejection checks cancellation
even when invoked without a deadline probe. Expensive postprocessing entrypoints
skip new work after a request. Physical packing retains its bounded polling and
UNKNOWN semantics; interrupted checks never become evidence of infeasibility.

Authoritative result validation remains strict. A cancelled route solve returns
`SG_STATUS_LIMIT` with a verified incumbent, or `SG_STATUS_ERROR` with no route
result when one cannot be verified. Error cancellation clears a previous route
result on a reused context, so callers cannot mistake stale work for this solve's
incumbent. Callers must inspect both status and result availability. In hard
loading mode cancellation may interrupt certificate validation and yield no
incumbent even if a route was constructed; do not publish it unverified.

This is cooperative cancellation, not a universal real-time deadline. A blocking
external travel callback, allocation or OS scheduling delay cannot be preempted
by this flag. Cursus subprocess termination is a separate application lifecycle
mechanism. The performance review target remains two seconds on the measured
representative corpus; do not infer a hard bound for arbitrary instances.

## Regression checks

- Arbor tests cancel before the second iteration with a huge progress segment,
  verify `AR_STOP_CANCELLED`, retain the valid best, and clear/reuse the callback.
- Surge tests cancel after construction, assert zero ALNS iterations, verify
  postprocessing respects cancellation, validate the incumbent and reuse the
  context. Parent cancellation is tested with an unlimited deadline. Failed
  cancellation is checked for stale-result removal.
- The native benchmark harness checks single, parallel and population solves,
  loading absent and hard, eight workers, unlimited wall time and a huge progress
  segment. A watcher calls `sg_cancel`; subprocess timeout catches hangs and
  validation rejects unsafe publication. Portable CI allows five seconds after
  the request; separate native measurements apply the two-second target.
- The benchmark adapter accepts an optional fourth argument after delay:
  `single`, `parallel` or `population` (default). For example:
  `surge/bench_loading_driver request.json 100 population`.

Run `make -C arbor test`, `make -C surge test loading-benchmark-test`, and the
normal sanitizer suites. The quality protocol remains
[surge-loading-benchmarks.md](surge-loading-benchmarks.md); ordinary uncancelled
paired iteration-budget runs should preserve solution quality as well as latency.
