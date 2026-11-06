# Surge physical loading: explicit data and staged integration

## Decision and scope

Implement P1 of `docs/roadmaps/surge.md` as a standalone, zero-dependency C
checker in Surge. Product and vehicle loading data are not yet available for
Gyermelyi. Synthetic fixtures are the acceptance input; no production data,
capacity, solver configuration, Cursus pin or deployed image is changed.

This is physical pallet stacking, distinct from pickup/delivery visit policies.
The initial domain is identical full floor footprints arranged into explicit
floor slots and vertical stacks. Heights are explicit: a partial pallet is not
necessarily half height or half footprint. Arbitrary footprints, orientation,
overhang, stability, axle distribution and reshuffling are not modeled. A
FEASIBLE certificate applies only to the declared model, not roadworthiness.

## Process alignment

The supplied transport-process document (2025-02-27), sections 2.3.5 and
2.5.1–2.5.3, supports product-specific stackability, vehicle floor capacity,
loading sequence and manual warehouse adjustment. Its 33/66 pallet example is
conditional on eligible products, not a blanket capacity multiplier. It does
not establish pallet heights, pairwise compatibility, access geometry or
reshuffle policy. Those remain required external inputs.

JDE extraction and WAMAS handoff belong in customer adapters, not Surge. The
adapter must preserve physical unit identity and product composition. Mixed
pallet properties cannot be guessed from an aggregate pallet number. It must
require validated rules or conservatively declare the input incomplete. Actual
measured weight must include the declared loading units; payload is separate
from gross vehicle access restrictions.

Loading is validated per truckload between reloads, never by summing all rounds
on one vehicle. Stop order edits invalidate the loading witness. Warehouse
changes require revalidation. Existing one-shot WAMAS transfer behavior demands
an explicit downstream revision/acknowledgement policy before integrating it.

## P1 C contract

`sg_loading_check(vehicle, units, count, node_budget, result)` accepts explicit
physical units. Integer millimeters and grams avoid tolerance ambiguity.
Each unit has a stop ordinal, stack class, height, weight, maximum total weight
above, and maximum number of units above. Zero limits prohibit stacking above
that unit, including zero-weight units. Caller maps article rules to these
limits; no defaults are inferred. Compatibility is directed and applies to the
immediate support contact. All underlying units enforce their cumulative
weight/count limits. A vehicle supplies floor slots, per-slot clearance and
weight limit (uniform in v1), payload, maximum layers and access model.

- SIDE_FREE: independent access to each stack; stop order is irrelevant.
- REAR_LIFO: independent lanes with explicit slot depth from the rear door;
  shallower slots unload before deeper occupied slots. Within a stack, upper
  units unload before lower units. Equal stop ordinals permit unloading together.
  This is a conservative discrete-lane model; no lateral maneuvers are inferred.

FEASIBLE includes placements in original unit order. An independent validator
checks every placement, support contact, cumulative load, height, unique and
contiguous layers, payload and access order. INFEASIBLE requires a mathematical
capacity bound or exhaustive search of this supported discrete domain. A node
budget exhausted by bounded deterministic backtracking yields UNKNOWN, never
INFEASIBLE. Invalid/missing/unsupported input returns INVALID_ARG and an UNKNOWN
result without a certificate. Limits: 128 units, 128 slots, 64 classes, 8 layers.
Zero search budget returns UNKNOWN (except a proven capacity rejection).
Every recursive state consumes one node; each state's finite work is bounded by
the input limits. No clock, randomness, global cache, allocation or I/O is used.
Caller-owned inputs/results make independent calls safe across worker threads.

Search explores unit ordering as well as slot placement: a fixed input order
cannot prove infeasibility because it could force an unsuitable bottom pallet.
The certificate validator is the authority for any positive result.

## P2 integration contract (not enabled by P1)

Add optional versioned loading inputs to the native request format and public C
context, carry them through context cloning and population workers, and preserve
current behavior when absent. Hard loading must fail closed on UNKNOWN or
incomplete input. A legacy scalar-only run remains explicitly unverified; it
cannot be described as physically loading-verified.

Use necessary bounds in hot insertion paths. Validate each round before
committing a candidate, covering construction, warm starts, repair, every local
search operator, population crossover, final export and manual validation.
Cache keys include ordered unit identities/properties, geometry, policy version,
rules and round boundaries. Never retain a witness after reordering or an input
revision. All existing constraints and HGS/population selection remain intact.

Do not merely post-filter a final result or substitute an empty plan. The search
needs a repair/reject path so it can find another physically feasible assignment.
Expose outcome/reason, bounded work, per-round witness and unassignment evidence.
Only fully verified candidates may publish when hard loading is selected.

## Acceptance

P1: deterministic feasible witnesses; directional classes; input-order reversal;
stack count/weight/height/payload limits; zero limits and zero weights; rear and
side access; independent lanes; invalid/floating/duplicate placements; overflow
boundaries; empty loads; zero/tiny budgets; realistic 66 units on 33 positions;
exhaustive small certificate enumeration compared with search; sanitizer checks.
Include the test in ordinary Surge CI, not a separate optional-only command.

P2: test all solver assignment paths, population/thread equivalence, each round
separately, hard UNKNOWN rejection, cache invalidation and manual reorder; then
real historical round fixtures once physical data exists. Historical route
acceptance alone cannot establish actual loading feasibility. No claim of
Gyermelyi loading parity until supplied physical data and warehouse validation.

## Implementation validation (2026-11-06)

P1 is implemented in `surge/include/sg_loading.h` and
`surge/src/sg_loading.c`, built into `libsurge.a`. `make -C surge test` includes
`test_loading`. The standalone public C API may also be compiled without any
Surge/Arbor runtime dependency.

Executed: 2,048 exhaustive class/access/stop combinations; 512 varied
height/weight/count cases against certificate enumeration; deterministic replay;
66-on-33 loading; malformed witnesses; cumulative top-load and count limits;
independent lanes; zero/tiny budgets; class 63; integer limits; eight concurrent
callers. Native macOS release and Linux ARM64 ASan+UBSan runs passed. The macOS
ASan runtime stalled during its loader initialization, before reaching main;
that run was stopped and is not a sanitizer pass. Linux ARM64 sanitizer coverage
was executed instead, in an isolated temporary source directory, without
changing containers or deployment configuration.

Existing regression suite: 484/484 Surge tests, 27/27 time-budget tests, 19/19
neighbour tests, 22/22 profile-matrix tests, and direct transport checks passed.
No real customer loading validation, solver coupling, JSON/HTTP/WASM loading
interface, Cursus integration or production rollout is claimed by this phase.
