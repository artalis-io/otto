# Native loading V2: rigid two-axle loads

This implementation adds an opt-in native capability alongside loading V1.
It does not enable physical loading in Cursus or change deployed binaries.
The [approved contract](roadmaps/surge-loading-v2-api.md) describes the complete
V2 surface; bounded reshuffling remains unavailable and is rejected explicitly.

## Scope and facts

Supported capabilities are `stacking` and `axle_loads`, using
`rigid_two_axle_v1`. Axle loads require stacking so one common witness satisfies
support, access and reactions. Articulated vehicles, axle groups, dynamic axle
loads and temporary offloading are unsupported. No vehicle geometry, axle limit,
CG, customer exception or statutory default is inferred.

Inputs use integer grams and millimetres, signed longitudinal coordinates and CG
intervals, explicit tare/load intervals, floor-slot centres, common footprint and
an optional rational minimum front-load fraction. Each vehicle and unit needs
complete facts for the chosen capabilities. `min_front_fraction: null` explicitly
means no fraction rule. Numeric zero limits restrict the model; they do not mean
unlimited. Intervals express conservative uncertainty, not measured calibration.
Profile SHA-256 fields are identifiers, not native authentication of measurements.

Every empty/loading/departure/unloading state must pass exact interval reaction,
gross, support and access checks. V2 side access still requires topmost unit
removal; V1 retains its previous abstract stacking semantics. Search considers
alternative packings and same-stop unload sequences under one bounded node
budget. Budget exhaustion or cancellation is UNKNOWN, never infeasibility proof.

## C and JSON interfaces

`surge/include/sg_loading_v2.h` provides separate V2 records and owned reports.
Query capabilities, configure V2, declare all vehicles/requests, then use existing
native HGS or the fixed-plan/round APIs. Setters copy input arrays. Reports own
their arrays and remain readable after context mutation/free; release each caller
reference with `sg_loading_v2_report_free`. Report views are borrowed until that
reference is released. Context access remains single-owner; only `sg_cancel` is
safe concurrently, as in the existing solver API. Worker clones have independent
mutable loading caches and share immutable model facts.

`sg_loading_v2_validate_plan` stages ordinary validation and physical evidence
before replacing the accepted snapshot. FIND searches; REPLAY validates the
supplied placements, operations, exact reaction summaries and stop schedule
without substituting a different witness. A failed call preserves the last
accepted snapshot. A one-round report contains no whole-plan statistics.
Ordinary `sg_validate_plan[_with_trips]` also enforces V2 when selected.

From the repository root:

```sh
make -C surge surge_solve
surge/surge_solve --capabilities
surge/surge_solve docs/roadmaps/loading-v2-examples/axles.request.json > /tmp/axles-result.json
make -C surge test axle-oracle-test loading-v2-cli-test
```

The CLI and HTTP solve handler share V2 decoding, HGS selection, plan validation
and serialization. `GET /api/v1/version` also returns `loading_capabilities`.
For fixed plans, supply `plan[].task_ids` and optional `trip_starts`; adding
`loading_witnesses` selects exact replay. Its metadata must match the request,
and its `unassigned_checks` must be empty: diagnostics are recomputed locally
rather than accepted as caller evidence. Search node counts are bounded work
metadata, not part of an optimality or minimal-search proof.

Unknown/duplicate loading fields, incomplete facts and malformed witnesses fail
closed. Exact reaction numerators are decimal strings; denominators are the
wheelbase in millimetres. No double conversion is used for reaction arithmetic.
Malformed/unsupported requests return HTTP 400; rejected/unknown fixed plans 422;
internal/allocation failures 500. The V2 CLI exits nonzero on these errors.
A 200/exit-zero result does not establish full order coverage or business approval:
inspect status, all rounds, unassigned orders and ordinary constraints.

## Validation and activation boundary

See [implementation evidence](surge-axle-implementation.md). Tests are synthetic;
no real Gyermelyi axle or stacking calibration is claimed. Existing V1 tests and
quality/cancellation harnesses remain in use. Physical constraints can change
feasible service and must not be judged as performance-only objective changes.

Cursus integration, independent BigInt replay at its fenced publication boundary,
capability-pinned workers, sparse-ID remapping, approval/export invalidation and
customer pilot evidence remain separate required work before product activation.
Reshuffling is still design-only. No production rollout is included here.
