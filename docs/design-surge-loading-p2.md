# Physical loading P2: native model and search integration

P2 builds on the standalone checker in `design-surge-loading.md`. No customer
stackability, height or geometry is inferred. Existing requests without a
`loading` declaration retain their behavior and output shape.

## Supported domain and fail-closed behavior

Hard loading requires version 1, mode `hard`, complete vehicle geometries and
explicit units for every request. Requests must be delivery-only; open starts
and explicit initial vehicle cargo declarations (including zero vectors) are
unsupported in v1; omit the declaration for an empty depot start. Empty unit lists are an
explicit declaration of no physical pallet units, not a default for missing data.
Class IDs are a common namespace supported by every declared vehicle. Zero
weight or stacking limits are literal limits, not “unlimited”. Callers must
reconcile units with their order-line data; Surge cannot infer that mapping from
unlabelled capacity dimensions.

FEASIBLE requires a validated arrangement. A proven capacity bound or exhaustive
search alone establishes INFEASIBLE. Node exhaustion, interrupted work and
rounds beyond the supported unit count remain UNKNOWN, which cannot satisfy a
hard constraint. No automatic scalar-capacity fallback is used. Ordinary scalar
capacity, GVW eligibility, qualifications, time windows and all existing model
constraints still apply independently. The final solution validator also enforces
must-serve assignments when construction could not place a committed/frozen
request; such a request cannot be silently dropped to claim success.

## Native input

The normal solve-request JSON has one optional top-level `loading` object:

```json
{
  "version": 1,
  "mode": "hard",
  "node_budget": 1000,
  "vehicles": [{
    "vehicle_id": 0,
    "access_model": "side_free",
    "slots": [{"lane": 0, "depth": 0}],
    "max_layers": 2,
    "clearance_mm": 2400,
    "max_stack_weight_g": 1000000,
    "payload_g": 10000000,
    "allowed_top": [[0]]
  }],
  "requests": [{
    "request_id": 0,
    "units": [{
      "height_mm": 1000,
      "weight_g": 500000,
      "stack_class": 0,
      "max_weight_above_g": 500000,
      "max_units_above": 1
    }]
  }]
}
```

This object is a fragment: the normal depots, vehicles, tasks and requests must
also be present. `vehicles` and `requests` each contain exactly one declaration
for every corresponding model ID. IDs cannot be duplicated. All numeric fields
are required, finite, nonnegative integers; integer widths are checked before
conversion. JSON gram values are limited to exact binary64 integers (2^53-1),
not rounded uint64 values. The node budget is in [0,100000]. Compatibility is an
array of permitted immediate-top class IDs for each bottom class. Access is
`side_free` or `rear_lifo`; lane/depth geometry has P1's conservative semantics.

The public C API copies inputs through `sg_vehicle_set_loading`,
`sg_request_set_loading` and `sg_set_hard_loading`. Stored unit stop ordinals
are ignored: each candidate's actual order determines its unloading sequence.
Model mutation invalidates the saved solution and its loading cache. As with
existing Surge contexts, concurrent mutation while solving is unsupported.

## Search and commit boundaries

- Ordered stop feasibility validates loading independently for each reload round.
- Cached insertion evaluation checks the complete candidate; apply repeats the
  loading guard before mutation. It cannot partially assign an invalid load.
- New-trip insertion carries the reload flag before checking/assigning, instead
  of treating the previous and new round as one physical load.
- Concat shortcuts do not authorize moves in hard-loading mode: ordered fallback
  validation is required. This covers reordering and cross-vehicle moves.
- Full solution validation enforces loading even when ALNS explores soft
  infeasibility. Final repair-or-reject and worker-to-master publication both
  require verified loading. Warm starts and SREX offspring re-enter guarded
  construction/repair; a claimed warm assignment is not a certificate.
- Worker clones share immutable model declarations, but each owns its cache.
  The cache key contains complete geometry, budget and ordered physical unit
  properties, not merely a hash. Reload boundaries are evaluated separately.
  Request/unit identities are remapped from the current round for every export.
- Search polls cancellation, including the master cancellation flag in workers.
  Interrupted UNKNOWN results are not cached as permanent infeasibility.

This implementation prioritizes correctness over the planned cheap-only hot
prefilter: bounded exact checking can run during insertion, with caching for
repeated candidates. Disabling concat shortcuts in hard mode also has a cost.
There is no zero-overhead claim for enabled loading and no country/workload
performance claim without actual physical input. Legacy mode skips these paths.
The checker is node-budget bounded; it does not independently poll the solver's
wall-clock budget. Cancellation is polled within packing search.

## Output and manual validation

Enabled responses add `loading.rounds`: vehicle, zero-based trip, outcome,
reason, explored node count and placements mapped to request ID/unit index.
Unknown/infeasible rounds have no placements. `sg_solution_get_loading` exposes
the same evidence to native callers.

`loading.unassigned_checks` reports bounded *singleton* physical-loading checks
against vehicles (feasible/infeasible/unknown counts). It explicitly does not
claim route insertability or attribute every unassigned order to loading. Its
diagnostic budget is at most 128 nodes, separate from authoritative route checks.

Directed support can be non-monotone: another request may provide a compatible
intermediate layer, making a previously infeasible singleton load fit. Thus a
singleton failure does not prove infeasibility in every combined load. Tests
cover that bridge case and multi-unit request identity mapping. As a heuristic,
construction can still miss jointly feasible groups lacking an individually
feasible seed; no completeness guarantee is claimed.

Manual validation accepts optional per-task `trip_starts` flags in each plan
route, e.g. `[0,1]` to begin the second round at task 1. Native callers use
`sg_validate_plan_with_trips`. Without flags a route is one round. Invalid flags
are rejected; duplicate vehicle entries are rejected in hard mode. Loading
failures are `loading` / `loading_unknown` violations. Existing validation status
`OK` means the validation request executed; callers must inspect violations and
per-round evidence, not use that status as approval. Changes after warehouse
handoff still require downstream revision coordination outside Surge.

## Acceptance

Run `make -C surge test` and `make -C surge loading-cli-test`. Test fixtures are
synthetic. The latter executes the actual native CLI with HGS enabled and checks
optional-mode parity, stacking, UNKNOWN, reloads and malformed input. Integration
coverage also exercises native C setters, manual validation/reordering, cache
invalidation, cancellation, insertion-before-mutation and population results.

No Cursus pin, deployed solver, customer dataset or production configuration is
updated by P2. Physical correctness for Gyermelyi remains unverified until its
product/pallet/vehicle data and real loading evidence are supplied.

## Executed validation

- Native macOS release: full aggregate target passed, including 492 core tests,
  transport, 27 timing, 19 neighbour, 22 profile-matrix cases and the standalone
  loading checker (2,560 exhaustive/varied oracle comparisons plus edge cases).
- Actual CLI with HGS enabled: stackable versus non-stackable loads, absent-mode
  parity, UNKNOWN rejection, reloads and 22 invalid-input cases passed.
- Native Linux ARM64: the eight P2 integration cases passed in an isolated
  ASan+UBSan `-O1` executable, including its leak check; the actual CLI input
  tests also passed under sanitizers. No container or deployment was changed.
- Warning-as-error source lint, documentation symbols, test reachability and
  sanitizer-reachability checks passed.
- A complete default ARM64 sanitizer target is **not** a pass: its existing
  FAST-profile test fails its strict OK assertion after a five-second limit,
  also reproduced on P1. A separate broad core sanitizer investigation was
  stopped after exceeding 600 seconds; it is not counted as passing. The
  suspected break test passed in isolation on both P1 and P2. CI remains a
  separate gate; none of these partial native checks replaces full CI.

A synthetic 66-unit/33-position two-worker HGS smoke run served all units using
one vehicle and identical distance in both modes. Controller release timings
were 0.561 seconds legacy and 1.926 seconds hard loading. This single small
fixture demonstrates added cost, not a realistic fleet benchmark or performance
promise. Real physical-data admission, warehouse validation and deployment
remain outside this implementation.

## Final boundary review

The review follows authoritative mutations rather than only public setters:

| Path | Guard |
| --- | --- |
| Construction and repair insertion | Full ordered candidate check, repeated before assignment mutation |
| Reload/new round | Explicit boundary enters the check before insertion; trip quotas remain enforced |
| Reorder and cross-route postprocessing | Ordered sequence validation; concat summaries cannot authorize hard-loading moves |
| Destroy/eject intermediate state | May temporarily lack support units; intermediate layouts are not certificates or publishable results |
| Warm starts and population crossover | Re-enter guarded construction/repair; full final solution validation still required |
| Single-worker completion | Final repair-or-reject validates physical and ordinary hard constraints |
| Parallel/HGS completion | Full solution validation after transferring the selected worker result to the main context |
| Manual plan validation | Explicit trip boundaries, per-round checks, UNKNOWN recorded as a violation |
| Model mutation/cancellation | Saved result/cache invalidation; interrupted checks are not cached as proof |
| JSON output | Loading witnesses are recomputed for the returned order and mapped to original request/unit identities |

The standalone suite additionally compares 4,096 seeded randomized instances
against a separate enumerator that implements the specification without calling
the production checker, fit logic or certificate validator. Small-budget checks
cannot report false infeasibility. This supplements the original 2,048 exhaustive
and 512 varied-geometry cases rather than replacing their edge/concurrency tests.

Profile tests accept the documented time-limit status only alongside a fully
assigned, validated incumbent. Instrumented ARM execution need not complete an
optimization profile before its five-second wall-clock budget to satisfy that
contract. Runtime behavior and profile budgets are unchanged.
