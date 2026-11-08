# Surge axle loads: proposed model and integration

Status: **design for review; not implemented**. Read the
[review plan](design-surge-loading-next.md) for sequencing and parallel real-load
validation. This document authorizes no code, deployment or customer activation.

## Scope and physical contract

First release: a rigid vehicle with two individual axles on level ground,
quasi-static vertical loads, known wheelbase and verified tare distribution;
delivery-only closed depot rounds; the existing aligned full-footprint slots
and vertical stacks. No pickups, trailers, initial external cargo, movable ballast,
mid-route fuel stops or onboard repositioning. Fuel/driver/equipment variations
must fit a verified tare envelope for the entire round.

A rear tandem/tridem is not an individual axle. Suspension load sharing cannot
be inferred by dividing a group reaction equally. Articulated combinations also
require kingpin/coupling reactions and tractor axle distribution. Those models
are future work, with their own calibration and oracle fixtures. Required axle
checks for an unsupported vehicle are an explicit input/capability error, not a
scalar-payload fallback or evidence that its orders are impossible to serve.

This checks the declared static model. Braking/cornering, slopes, suspension
transients, lateral wheel loads, restraint, centre-of-gravity height/stability,
bridges and jurisdiction-specific rule discovery remain outside the guarantee.
Limits are supplied as verified operating/manufacturer constraints with source,
configuration, validity interval and units. No legal limits are hardcoded or
inferred from nominal truck tonnage. Gross vehicle access restrictions and
ordinary payload remain separate existing hard constraints.

## Required facts

Use integer grams and signed integer millimetres. Coordinate origin is the front
axle, positive toward the vehicle rear; rear axle is at `L > 0`. Load space may
extend outside the axle span. A negative cargo reaction increment is physically
possible; never clamp it to zero.

| Input | Required meaning |
| --- | --- |
| Vehicle model/configuration identity | Exact chassis/body/axle configuration and verified source revision |
| `wheelbase_mm`, deck extent | Measured support spacing and usable longitudinal range in this frame |
| Metric slot centres and common footprint length | Actual longitudinal positions, not ordinal lane/depth numbers |
| Front/rear tare reaction intervals | Empty vehicle including the declared operating envelope; no double-counted fuel/driver/equipment |
| Axle min/max loads; vehicle max gross | Applicable manufacturer/operating limits, all literal; explicit zero is not unlimited |
| Optional front minimum load fraction | Explicit rational numerator/denominator if required; no presumed steering percentage |
| Unit mass and CG-offset interval | Packed unit including pallet/packaging; longitudinal mass-centre bounds relative to its slot centre |
| Calibration and uncertainty metadata | Measurement method, configuration, reviewer, tolerances and validity |

Every declared unit, including a zero-mass unit, needs explicit CG bounds.
A symmetric unit may have `[0,0]` only when confirmed, not because its contents
are unknown. Stacking aligned units preserves their longitudinal coordinates;
rotations, overhang and unequal footprints are not introduced by this extension.

Validate deck/footprint containment, non-overlapping slots in each lane and
consistency of metric positions with rear-door access ordering. Different lanes
may share a longitudinal coordinate. Geometry and CG bounds must use the same
frame and fit the supported footprint. Partial pallets are not automatically
shorter, lighter by a ratio, or half-height.

Tare and CG intervals define a conservative box of possible values. They may
include unrealizable correlated combinations, causing rejection of some usable
loads; they must include every plausible actual configuration. Do not narrow
intervals just to obtain a pass. Out-of-envelope measurements invalidate the
profile. Exact point facts are intervals of zero width.

## Static equations and exact comparisons

For a remaining onboard unit set `S`, let `w_i` be unit mass, `x_i` its longitudinal
mass centre, `T_f/T_r` tare axle reactions, and `M = sum(w_i * x_i)`:

```
rear  = T_r + M / L
front = T_f + sum(w_i) - M / L
front + rear = T_f + T_r + sum(w_i)
```

The implementation compares scaled rational numerators, never rounded display
values. For fixed tare, `rear_num = T_r * L + M` and
`front_num = T_f * L + sum(w_i) * L - M`; denominator is `L`.

Use interval endpoints for worst-case absolute bounds: maximum rear uses maximum
rear tare and maximum moment; maximum front uses maximum front tare and minimum
moment; minima use the opposite endpoints. Check gross with maximum total tare
and cargo. An optional front-fraction requirement compares
`front >= p/q * gross`; evaluate its linear expression at the relevant tare/CG
corners, not independent rounded front/gross estimates. Declare `0 <= p <= q`
and `q > 0` explicitly. There is no floating epsilon or implicit allowance over a
limit. A customer safety margin is an explicit stricter bound in the profile.

Use checked signed arithmetic throughout, including subtracting overhang moments
and multiplying rational terms. Proposed basic caps: at most the existing 128
units, individual masses/tare/limits at most `10^9` grams, coordinate magnitude at
most `10^6` mm, and positive wheelbase at most `10^6` mm. These are numerical
bounds, not vehicle limits. Basic moments fit signed 64-bit under these caps.
For rational fraction rules, reduce terms and prevalidate worst-case arithmetic
bounds; reject a descriptor if its declared products cannot be represented.
Do not silently cast through binary64, wrap, saturate or weaken a comparison.
The standalone C implementation stays portable and dependency-free; any wider
integer helper requires its own independent boundary/oracle tests. Cursus replay
uses exact integer arithmetic (for example BigInt), and matches native semantics.

### Required counterexample

Synthetic two-axle vehicle: `L=4000 mm`, tare `front=3000 kg`, `rear=2000 kg`,
rear maximum `6500 kg`, side-free access and two unstacked floor positions. Unit A is `1000 kg` at `x=-2000 mm`; B is `4000 kg` at
`x=5000 mm`. Choose all other bounds to accommodate these synthetic facts.
Departure reactions are `front=3500 kg`, `rear=6500 kg`, total `10000 kg`.
After A is delivered, rear reaction becomes `7000 kg`: it fails although payload
has decreased. Reversing those deliveries avoids this particular violation.
A departure-only checker must fail its regression test on this case.

## States and joint feasibility

A round starts with an empty vehicle at the depot, is loaded, travels through
stops and returns empty. No inference that depot reload means every round's
initial placement is identical. Each round gets its own placement and certificate.

Check empty tare, complete departure load, every arriving/departing road load
and each discrete pallet removal in the unloading witness, including the final
empty state. Axle maxima, minima, fraction and gross rules apply to these states.
Checking after an entire multi-pallet delivery alone can miss a violating
intermediate removal; a passing result includes a valid unit-removal order.
Transient lifted/part-supported pallet forces are not simulated. Known operating
procedures requiring a wider dynamics model are unsupported, not implicitly safe.

For depot loading, include a feasible bottom-up/access-compatible placement
sequence with the same static checks after each completed placement. A sequence
can be the reverse of a fully replayed unload sequence in this delivery-only,
fixed-placement domain, provided access and support checks also pass in reverse.
No axle claim is made for continuous forklift transfer forces.

The checker must search for a **joint** arrangement satisfying stacking, access
and axle bounds at every required state. Taking the first stacking-feasible
arrangement and rejecting the route if its axles fail loses valid alternatives.
Unit ordering within a delivery can matter; include it in bounded search.
A failed partial packing is not, by itself, a sound pruning rule: a later pallet
can counterbalance it. Only prune with a proven necessary bound or a replayed
required operation state; exhaustive negative claims must cover all supported
placements and handling orders. Weight-on-top and support-bridge cases remain
non-monotone too.

No sound proof before node/time/cancellation limits means UNKNOWN. Return an
explicit reason (budget, unsupported data, interrupted, proven axle bound or
exhausted supported search). Do not label every unassigned request an axle
failure: singleton checks and failed candidate diagnostics have limited scope.

## Proposed native and certificate boundaries

The [shared C/JSON v2 API specification](roadmaps/surge-loading-v2-api.md)
defines the exact proposed declarations, schema, examples, ownership, status
mapping and required parity tests. These are review contracts, not current API
support. Its full-plan replay must propagate the submitted handling witness
through the actual schedule; independent successful partial checks are insufficient.

Add a future `loading` schema version 2 with explicit `required_capabilities`,
versioned axle descriptors and per-unit CG declarations. This is a proposal, not
an accepted request example. Version 1 remains byte-shape compatible without new
features; old readers must reject required version-2 capabilities. Use versioned
sibling C descriptors/functions rather than silently changing public struct ABI.
Immutable setters deep-copy, validate before replacement and invalidate saved
solutions/caches. Worker clones share immutable facts, not mutable proof buffers.

A round witness contains stable request/unit and vehicle identities, slot/layer
placements, depot loading and stop unloading order, and exact interval reaction
bounds for each state (rational strings where needed). State inventory is
reconstructed by the validator; supplied reaction totals are redundant evidence,
not trusted inputs. Bind source/profile hashes, supported model and validator
versions, budgets, route order, round boundaries and executable identity.

The independent validator recomputes reactions from source facts and operations,
checks inventory/stack/access/axle constraints and rejects malformed, omitted,
duplicated or overflowing states. Its mathematical oracle in tests must not call
the production reaction helper. A witness proves one arrangement, not optimality
or full search completeness.

## HGS, performance and application integration

Reuse P2's read-only candidate views, ordinary cheap constraint prefilters,
worker-local bounded caches and guarded apply/final validation. Keep all
construction, repair, retained/frozen warm starts, crossover, reorders, cross-route
moves, reloads and master-result transfer covered. Never allow a concat scalar
summary to authorize a physical move. Geometry, CG/tare intervals, limits,
ordered units and budgets are part of exact cache equality.

For a fixed placement, reactions are linear in remaining weight/moment. Maintain
per-stop aggregates and extrema to avoid a full layout copy per insertion;
validate against full replay before enabling any fast summary. A nonbinding
axle profile may reuse the current packing result only with a sound proof that
all supported placements and intermediate states satisfy every axle rule.
Physical bounds are independent of HGS's soft-penalty exploration. Unverified
incumbents cannot be selected as authoritative results. Cancellation remains
polled inside search and replay loops, with bounded scratch/certificate sizes.

Cursus adoption is a later implementation step: version-2 immutable physical
sources, capability-aware image compatibility, per-state proof replay, durable
requirements and fenced completion/approval/export. No network I/O or native
search while holding the authoritative database lock. Manual editing preserves a
saveable draft but invalidates the certificate; Replan or a separately implemented
native validation path must create fresh evidence. UI shows worst axle margins,
the stop/state responsible and uncertainty assumptions, scoped as static-model
checks. Unsupported/missing facts show Unassessed/Blocked, never Passed.

## Acceptance before activation

| Area | Required evidence |
| --- | --- |
| Arithmetic | Independent rational/statics oracle; conservation, tare-only, overhang, zero mass, min/max/fraction exact boundaries, intervals and overflow rejection |
| State sequence | Departure-pass/unload-fail example; multi-unit intermediate violations; valid alternative placements/removal orders; depot loading; reload resets |
| Joint search | Exhaustive small placement/order enumeration with stacking/support/access; no false FEASIBLE/INFEASIBLE; UNKNOWN at small budgets |
| Integration | All P2 mutation paths, single/multiple workers, HGS warm/crossover/manual plans, cache invalidation/collisions, allocation failures and fuzzed schema/witnesses |
| Application | Two real replicas; stale worker/source/profile/revision cannot publish or approve; draft versus Approved export; EN/HU Playwright and role/permission regressions |
| Field validation | Configuration-specific measurements and weighbridge/axle-scale observations with documented tolerances, operator review and discrepancy resolution |

Performance gates are proposed for review before any runs. Keep the existing
absent-loading baseline gates unchanged. For stacking enabled with axles absent,
require >=0.90 median iteration throughput versus the current stacking engine.
For added but provably nonbinding axle constraints, target >=0.85 versus that
same stacking baseline, with no paired service loss, no median vehicle increase
and <=5% median distance/cost increase at equal service. Report combined throughput
relative to loading-absent too; individually acceptable incremental overheads do
not automatically pass the existing >=0.75 combined nonbinding gate. Do not
weaken or silently compound away existing gates.

Use at least the existing three paired seeds, HGS with eight threads, wall and
requested-iteration budgets, a 600-second control, native ARM64 and AMD64, peak
RSS/CPU, cancellation <=2 seconds, and independent final replay. Binding cases
compare against exact/same-feasible-set references; unavoidable physical service
loss is reported, not hidden as savings. Profile a worst supported 128-unit
round and one fixture exceeding normal scratch/certificate sizes; exceedance
must fail cleanly. Proposed RSS headroom is <=20% over stacking-only on the same
fixtures; report absolute bytes and per-worker/cache storage. Failed quality,
latency or resource gates block activation rather than trigger threshold edits.

## Implementation sequence after review

A1: standalone versioned reaction/operation validator plus independent oracle.
A2: joint bounded placement/unload search and complete witnesses. A3: native
schema/C setters and every HGS mutation boundary, profiling and performance review.
A4: Cursus immutable facts, durable capabilities, publication/approval/export and
Playwright. A5: isolated customer replay and calibration, then a separate pilot
activation decision. No reshuffling implementation is bundled into A1-A5.

## References and limits of attribution

Krebs and Ehmke, *Axle Weights in Combined Vehicle Routing and Container Loading
Problems* (2020 working paper; 2021 journal article), describes static models for
rigid/articulated vehicles and the importance of intermediate loading states:
[author-hosted paper](https://www.fww.ovgu.de/fww_media/femm/femm_2020/2020_03-p-11326.pdf).
This proposal deliberately starts with a narrower two-individual-axle domain.
It does not claim the paper proves Surge's implementation or supplies customer
geometry, applicable limits or suspension calibration.
