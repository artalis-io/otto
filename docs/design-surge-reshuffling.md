# Surge bounded reshuffling: proposed operations and integration

Status: **design for review; not implemented**. Implementation follows axle loads,
not this documentation change. See the [review plan](design-surge-loading-next.md)
and [axle design](design-surge-axle-loads.md). No current loading constraint is
relaxed and no production/customer policy is enabled.

## Purpose and first supported domain

Permit a small, explicitly authorized number of temporary pallet removals to
reach a delivery, with a replayable handling sequence, bounded work, and realistic
schedule/cost consequences. This is not a penalty that permits physically blocked
unloading. A route without a valid operations witness remains unverified.

Keep the existing identical full-footprint slots, vertical stacks, delivery-only
closed depot rounds and rear-lane access model. Strict rear unloading remains
available and is the default. Side-free access does not imply permission to move
other customers' cargo. Reshuffling permission is the intersection of vehicle,
site, equipment and product declarations, scoped to the exact stop and revision.

First implementation permits a later-stop unit to be moved to an external floor
staging position, then returned to its **original floor slot** before departure.
Its layer may decrease after lower units were delivered; it must be explicitly
restacked on a valid contiguous support chain. No cross-slot repacking, rotations,
sliding through occupied lanes, temporary pallets in the aisle, stacking in the
staging area, exchanges between trucks, future-stop delivery, split units or
pickups are included. No unit teleports or falls through a removed support.

This intentionally limits search and makes instructions auditable. A load that
requires a wider operation is Unsupported/UNKNOWN as appropriate, not evidence
that every real-world handling procedure is impossible.

## Required policy facts

All permissions default to denied. An explicit prohibition is never overridden
by a cost term. Missing facts for a required capability block verification.

| Scope | Facts |
| --- | --- |
| Vehicle | Access model; explicit permission for temporary offload; suitable handling equipment; max unit handling mass/height; handling time and objective-unit pricing |
| Stop/site | Reviewed staging permission, available floor positions, position weight/height limits, compatible unit classes, equipment, permitted handling interval and policy revision |
| Product/unit | Temporary-offload permission and any protection/handling restrictions; packed physical facts retained from stacking |
| Route policy | Limits on simultaneously staged units, extra operations per stop/round, and extra service time per stop/round |
| Evidence | Owner/reviewer, source/configuration identities and validity; no permission inferred from customer name or historical delivery alone |

The staging surface is a declared set of independently accessible full-footprint
floor positions, one unit each; no staging stacks. If that abstraction does not
match the site, do not certify it with this version. Cold chain, food protection,
seals, weather exposure and operational restrictions require explicit compatible
policies; the solver does not discover them. An equipment limit is not a claim
that forklift reach, tipping stability or transient lifting forces are modeled.

Proposed initial implementation caps for review: at most four simultaneously
staged units, eight extra moves per stop and 64 per round, within the existing
128-unit round bound. Customer-selected limits may be stricter, including zero.
These are computation bounds, not recommended operating allowances. Configuring
above a supported cap is rejected, not silently truncated.

## Operations and accounting

A state contains each remaining unit exactly once, either in an onboard slot/
layer or a named staging position, plus a delivered set. Initial placements and
an independently checked depot-loading witness come from joint loading search.
The allowed transitions are:

1. **`deliver(unit)`**: remove an accessible topmost unit belonging to the current
   stop and mark it delivered. It is not counted as an extra reshuffle move.
2. **`stage(unit, position)`**: remove an accessible topmost later-stop unit to a
   compatible empty staging position. This counts as one extra move.
3. **`restore(unit, slot, layer)`**: move a staged unit back to its original floor
   slot at an explicitly valid layer and through an accessible path. This counts
   as one extra move; support, class, stack weight/height and all limits apply.

For rear access, the current nearer occupied slots in the same lane block a
farther slot; the unit must also be topmost. Restoration applies the reverse
access path, so restoring near pallets first may prevent restoring far pallets.
Do not model a whole stack as one move. The original floor slot is immutable
throughout the round, but exact layer positions are reconstructed after every
operation. A later unit can be a blocker repeatedly at different stops; each
physical stage/restore pair counts again.

Each operation atomically transfers one complete unit in the discrete model.
After every operation, independently verify inventory, support chains, height,
weight-on-top, vehicle payload, staging occupancy/limits and enabled axle rules.
Transient partially lifted states are outside the guarantee. If relevant rules
cannot be represented, the operation is not approved by this model.

At departure from each stop, its required units must all be delivered, every
other unit must be onboard and the staging area empty. No driving with cargo
left at a customer, omitted blockers, undelivered current cargo or an unsupported
stack is permitted. Each reload ends one round and starts a new empty-depot
round. Stop budgets reset; round budgets reset only at the actual reload boundary.
Driver/vehicle shift constraints still span all rounds.

Example: A is below B, with B due later. Stage B; deliver A; restore B to the
same slot at layer zero. That is **two** extra moves and one staging position.
Returning B to its former layer one above an empty floor is invalid. Removing B
may change axle reactions: all intermediate states must satisfy required axle
rules, even if the restored departure arrangement would pass.

## Time, objective and scheduling

Reshuffling is a hard operational feasibility capability with priced handling;
axle, support, permissions and budget violations never become soft penalties.

Extra handling time is derived from the actual stage/restore operations using
explicit vehicle/equipment rules and unit properties. The first version may use
fixed per-stage/per-restore seconds within a verified equipment class; values
must be supplied, not guessed. The existing stop service duration must explicitly
cover ordinary delivery only, excluding these temporary moves, to avoid counting
the same work twice. If source service times already include reshuffling and
cannot be separated, the admission mapping is unresolved and must not fabricate
an incremental duration.

Compute `effective_service = base_service + extra_handling`. Handling occurs at
the customer during service, not while driving or invisibly in depot reload time.
Propagate that duration through downstream arrivals, existing time-window
semantics, waiting, shifts/breaks, round returns, subsequent trips, depot hours
and overtime rules. A customer's latest service-start constraint does not become
a latest-completion constraint unless explicitly declared. A staging-permission
interval, when supplied, must cover the actual handling sequence; waiting for
permission counts as waiting and cannot be discarded from the schedule.

The native route evaluator must consume per-stop handling duration from a valid
witness **before** accepting a candidate. Never mutate a shared task's base
service time, add duration only at JSON export, or let a cached packing result
certify a changed schedule. This requires a joint loading/operations/schedule
contract, not only another boolean in the existing feasibility helper.

Price extra work in explicit native objective units; application adapters map
verified tariffs/currencies without assuming native units are HUF. If duration
already has a configured cost coefficient, do not double-charge it with an
unlabelled second time cost. Any additional handling fee is separate and explicit.
Export distance, driving time, base service, extra service, extra move count,
waiting and cost components consistently; KPIs must reconcile to the exact same
immutable witness. An unassigned-order penalty cannot buy a physically invalid
sequence. Ordinary legacy objectives remain unchanged when reshuffling is absent.

The evaluator may need multiple feasible witness alternatives: a cheap sequence
can be too slow for a downstream window, while a different handling pattern is
usable. Keep a bounded frontier over relevant remaining budgets, physical state,
schedule and cost; prune only with proved dominance for the same remaining
problem. If a frontier cap discards an unproved alternative, preserve the
incomplete-search flag: a valid found witness may still be FEASIBLE, but absence
of one is UNKNOWN, not INFEASIBLE or a globally minimum handling-cost claim.

## Search, proof and interaction with axles

Use bounded deterministic search over placements and operation sequences, with
a combined per-round work budget in addition to subproblem counters. Otherwise
running a full reshuffle search for every packing candidate multiplies bounds
into impractical work. Proposed combined budget: 10,000 nodes by default, with the existing maximum
100,000 nodes per round. Every attempted placement/handling transition and
frontier expansion must be charged consistently; exact counter charging requires
review and benchmark.
Poll cancellation inside both loops and frontier/replay processing. Bound scratch,
frontier and witness memory independently of solve time; no unbounded global cache.

First try the strict zero-extra-move witness, then bounded alternatives when
permitted. Failure of the first packing does not prove all packings need too
many moves. A valid strict witness has zero reshuffle cost; do not force moves
merely because the policy is enabled. Depot loading has its own witness; reversing
an entire multi-stop trace with staging is not a valid substitute.

Jointly enforce the active axle model at every completed operation. Returning a
blocker cannot excuse an axle violation while it is off the truck. Axles absent
remain explicitly Unassessed, never silently enabled from guessed tare/geometry.
Selecting axle checks for an unsupported vehicle blocks that capability.
A rearrangement valid for stacking and a different arrangement valid for axles
are not a common feasibility certificate.

Only sound bounds/exhaustive search of the supported operations establish
INFEASIBLE. Node/frontier exhaustion is UNKNOWN. A witness is not proof of minimum
handling cost or best route. Diagnostics distinguish staging/permission limits,
access, support, axle failure, time limits and search exhaustion with explicit
scope; do not infer a universal unassigned reason from one attempted sequence.

## Interfaces, caches and authoritative publication

The [shared C/JSON v2 API specification](roadmaps/surge-loading-v2-api.md)
defines the exact proposed declarations, schema, examples, ownership, status
mapping and required parity tests. These are review contracts, not current API
support. Its full-plan replay must propagate the submitted handling witness
through the actual schedule; independent successful partial checks are insufficient.

Use the common future loading schema version 2 and explicit required-capabilities
contract. Do not reinterpret version-1 `rear_lifo` as allowing reshuffling. Add a
separate versioned handling policy/access declaration while retaining its strict
base access geometry. Use versioned sibling C descriptors/functions;
the precise proposed names/shapes are recorded in that specification but are not
implemented public API promises.

Cache exact immutable unit/geometry/policy identities, ordered stop/round
boundaries, axle descriptors, work budgets and handling rules. Physical operation
frontiers may be reused only with exact equality. Schedule-dependent choice must
also match the relevant arrival/window/shift context, or be re-evaluated against
the current schedule. Interrupted/incomplete answers cannot be reused as proof
of impossibility. Cache equality must not depend on a hash alone.

A round certificate contains initial placements, depot-loading witness, ordered
per-stop operations, stable unit identities, explicit staging positions and
restore destinations, time/cost accounting, enabled axle state evidence, budget
counters, source/policy/validator versions and exact route/scenario identity.
Independent replay reconstructs all inventories, placements, access, support,
reactions, duration and costs; precomputed summaries cannot authorize a move.
Output size is bounded by units and permitted moves, not optimization iterations.

Integrate through all P2 mutation paths: construction/repair, retained/frozen
assignments, crossover, reorders, cross-route moves, reloads, final worker transfer
and manual validation. Keep HGS/population search, existing scalar constraints and
strict-mode behavior. A retained route does not grant handling permissions.

Cursus must bind immutable site/product permissions and tariff revisions as well
as loading facts. Materialize operation-dependent schedule/KPIs before fenced
publication. Current certificates are invalid after stop order, assignment,
vehicle, site permissions, time windows, handling rules or tariff changes.
A manual edit may be saved as an unverified draft but cannot inherit approval.
Native validation or Replan must produce a fresh joint witness.

In the existing Constraints panel, show handling required/verified/unverified,
extra moves and time, site permission source and any blocker. A route/stop detail
can disclose the ordered unit-specific instructions without another top-level
workspace. Reviewers can inspect; permissions for editing/approval remain unchanged.
Draft and Approved exports retain the same sequence and counters. Actual warehouse/
driver changes require revalidation and downstream revision acknowledgement,
not a client-side edit of the release certificate. Loading-specific operations
should not force a speculative WMS platform or driver app into the core engine.

## Acceptance and performance gates

- Independent state-machine oracle exhaustively enumerates small placement and
  stage/deliver/restore cases without production helpers. Check valid witnesses
  and reject wrong stop/identity, duplicate/lost units, floating restoration,
  blocked paths, excess staging/moves/time, prohibited products and missing rules.
- Test a one-blocker/two-move case, stacked/multiple blockers, repeated blockers,
  zero budgets, permission intersections, staging windows, no valid restoration,
  axle-invalid intermediate states and valid alternatives. Verify the narrow
  model never claims general cross-slot reshuffling impossibility.
- Recompute actual downstream time windows, waits, shift/break/return/reload and
  costs. Include a shortest-distance candidate rendered infeasible by handling,
  a feasible alternative sequence, and double-counting regressions.
- Extend randomized/exact packing and routing oracles, CLI HGS tests, sanitizers,
  allocation-failure/fuzz tests, cache collisions/invalidation and native cancellation.
- Two real Cursus replicas must reject stale-worker publication, permission/tariff
  changes, forged counters, missing operations and stale Approved exports.
  Playwright EN/HU covers manual invalidation, role restrictions, reload, instruction
  inspection and reconciliation of served/vehicles/trips/distance/time/cost KPIs.

Before measurements, retain existing gates and propose >=0.90 median throughput
for absent or explicitly zero-budget reshuffling versus the same axle/stacking
baseline; >=0.80 for enabled nonbinding permissions versus that baseline. Also
report combined overhead versus loading absent and enforce the existing combined
nonbinding >=0.75 gate. No paired service loss or median vehicle increase and
<=5% median distance/cost increase apply only to equal-feasible-set controls.
Binding reshuffling may improve coverage; compare quality with exact references
and report added time/cost rather than calling lower service savings.

Use the existing three paired seeds, eight-thread HGS, wall and requested-iteration
budgets, a 600-second nonbinding run, ARM64/AMD64, absolute/per-worker RSS and CPU,
cancellation <=2 seconds and complete independent replay. Proposed RSS headroom
is <=20% relative to the same enabled axle baseline on equal fixtures; report
combined absolute memory too. Frontier/move caps and worst supported loads must
be exercised. Failed gates block activation, not trigger retuning after results.
All thresholds here are review proposals, not achieved measurements.

Warehouse-observed permitted reshuffles and staging conditions are prerequisites
for customer activation. Model-level correctness does not establish equipment
safety or operational permission at an unreviewed site.
