# Loading extensions: design review and real-load validation

Status: **proposal for review, 2026-11-08**. Documentation only. Neither extension
is implemented or authorized for implementation by this document. No default,
customer input, Cursus pin, deployment or production resource changes.

## Decisions proposed

Design axle loads and bounded reshuffling together, review them, then implement
axle loads first. Continue validating existing stackability against real loads
in parallel; that work does not depend on either extension.

| Track | Deliverable | Gate |
| --- | --- | --- |
| A | [Axle-load design](design-surge-axle-loads.md): exact static checks throughout each round | Review model, numeric contract, witnesses and tests before implementation |
| B | [Reshuffling design](design-surge-reshuffling.md): explicit bounded handling sequence | Review now; implement after axle integration and customer need are established |
| C | Historical load validation described below | Starts as soon as verified evidence is available; never invent missing facts |

The initial axle implementation is deliberately narrower than the overall
product ambition: rigid vehicles with **two individual axles**, delivery-only,
closed depot rounds, aligned full-footprint pallet units. A three-axle rigid,
axle group or articulated combination needs a separately validated reaction/load
sharing model. Two support reactions alone do not establish individual axle
loads for those vehicles. Review this boundary against the actual pilot fleet
before choosing the first implementation cohort.

Both extensions must retain ordinary HGS/population search and all existing
constraints. No alternative optimizer, automatic constraint relaxation, inferred
vehicle facts or post-hoc filtering of the sole returned incumbent is proposed.
The supported intersection of constraints must have one replayable witness.

## Interface specification for review

Both extensions share a [precise C/JSON version-2 contract](roadmaps/surge-loading-v2-api.md),
with proposed C types/functions, strict JSON Schema, axle-only/reshuffling-only/
combined examples, full-plan witness replay, ownership/error/version semantics,
and authoritative C-versus-JSON parity tests. It supersedes vague interface naming
in the physical design narratives, without making the functions implemented.
Schema/compilation checks of proposed examples are not native execution tests.

## Existing baseline

- OTTO reference: `605691491b8199376dc989e440cce13b1fcd705c` (merged loading
  correctness, cancellation and hot-path work).
- [P1](design-surge-loading.md) and [P2](design-surge-loading-p2.md) provide discrete
  slots, support/height/weight rules, side access or strict rear unloading,
  bounded search, witness replay and integration with HGS mutations.
- [Performance protocol](surge-loading-benchmarks.md),
  [hot-path review](surge-loading-performance.md) and
  [native cancellation](surge-native-cancellation.md) remain binding references.
- Cursus PR #58, merged as `3705396beebb26d72003c14bb76d1911f80084eb`, adds
  default-off loading enforcement, immutable-source/certificate binding,
  fenced publication, manual invalidation and approval/Approved-export guards.
  Merging that code did not deploy or activate it.

Existing model/version 1 callers and absent-mode outputs must remain compatible.
The designs require future versioned declarations/certificates and explicit
capability checks. Existing Cursus schema-1 physical declarations do **not**
already contain usable axle geometry, mass centres or reshuffling permissions.

## Parallel historical validation

### Inputs and ownership

Customer adapters own JDE/Excel/WMS extraction and reconciliation. Surge owns
physical feasibility; Cursus owns admission, tenant isolation and release.
The supplied Gyermelyi process document (2025-02-27, sections 2.5.1-2.5.3)
establishes conditional stacking and warehouse sequencing, not actual unit
heights, support strengths, axle geometry or permission to stage goods at stops.
The 33/66 example is not a universal multiplier or default vehicle profile.

Keep private source files, photographs, measurements, routes, calibration records
and signed acceptance outside Git, images and public CI artifacts. Retain only
public synthetic regressions in the repositories. Tenant-owned immutable object
references, checksums, mapping revisions and provenance belong in Cursus.

### Evidence pack per truckload

Record source identity and conversion revision, date, vehicle configuration,
round boundaries, stop order, order lines and physical unit identities. Reconcile
unit counts, packed/tare-inclusive masses, product composition, actual heights,
footprints, directed support rules and vehicle clearance/payload. Aggregate or
fractional pallet quantities do not establish those facts. Explain measurement
uncertainty and obtain warehouse sign-off on rules and observations.

For an observed arrangement, retain slot/layer placements and unloading sequence
when available. A historical route alone proves neither a feasible arrangement
nor that the recorded loading complied with every declared rule. An accepted
manifest is not evidence that every order was actually delivered.

Select representative two-high/non-stackable/mixed loads, reload rounds, rear
and side access, near-capacity loads and known operational failures. Include
unusual products and manually corrected loads, rather than only easy successes.
Where possible pair driver/warehouse observations with source records. Never
create an unsafe physical experiment to obtain a negative example.

### Validation ladder

1. **Reconcile data:** no unexplained missing/duplicate units or mass/count
   differences. Mark incomplete evidence unassessed; do not fill guessed defaults.
2. **Replay observed arrangements:** use the independent validator, not search
   success, to check recorded operations against the declared model.
3. **Search the same fixed round:** compare the search witness with independent
   replay. UNKNOWN is inconclusive; distinguish a false rejection, bad mapping,
   incorrect historical record, unsupported operation and genuinely invalid load.
   An alternative feasible arrangement does not prove the observed one was valid.
4. **Optimize in isolation:** paired HGS seeds, current constraints and comparable
   budgets; preserve orders, GVW, tail-lift rules, exceptions and tariffs. Report
   coverage before distance/cost, plus fleet/trips, solve time, RSS, throughput,
   UNKNOWN count and all certificates. Do not promote acceptance-test data.
5. **Warehouse review:** a load planner checks the witness/instructions. Explain
   divergences; sign off the supported scope, not a generic roadworthiness claim.
6. **Separate activation review:** only after data and performance gates pass.
   Build one pinned capable image, certify it, stage on both replicas and keep
   enforcement off until explicitly approved. Preserve production plans and data.

A private validation report should list total observed rounds, evidence-complete
rounds, independently accepted/rejected/UNKNOWN rounds, discrepancies and their
resolutions, unsupported vehicle/product classes and named sign-offs. No fixed
sample count is a substitute for covering the pilot fleet's distinct rules.

This track can finish a validated pilot of current stackability while axle loads
remain in development. It must label axle loads and reshuffling **not assessed**.
Those features cannot retrospectively certify older plans.

## Shared engineering and release gates

- A versioned required-capabilities list distinguishes stacking, axle loads and
  reshuffling. Required unsupported/incomplete input is rejected before search;
  optional absent features remain explicitly unassessed. Never advertise an
  overall pass for an unassessed required dimension.
- FEASIBLE requires independent replay; only sound bounds or exhaustive supported
  search prove INFEASIBLE. Budget exhaustion and cancellation are UNKNOWN.
- Cache keys bind the full immutable descriptors, ordered identities, remaining
  load states, policies, schema/validator versions and budgets. Hashes index
  entries; exact equality authorizes reuse. Worker-local storage stays bounded.
- Follow every mutation boundary in P2, including reloads, retained/manual work,
  crossover, all local moves, final worker transfer and manual validation. Native
  execution stays outside database transactions; authoritative publication stays
  in the existing fenced transaction.
- Cursus must reject old/unsupported certificate versions for new required
  capabilities; use a capability-aware immutable manifest binding. All replicas
  and workers must support the requirements before activation. Durable dataset/
  scenario requirements survive edits, copies, reimports and flag changes. Reject
  older capability-unaware rollback images; restoring an old database over new
  writes is not a rollback method.
- Reviewer/Planner/Admin permissions and source privacy remain unchanged. Drafts
  may be saved with explicit blockers; approval, release and Approved export must
  recheck current requirements under the existing authoritative lock.

Future test work spans native exact oracles/sanitizers/fuzzing, real CLI HGS,
ARM64/AMD64, two-server PostgreSQL/object-store concurrency and Playwright EN/HU.
Tests must prove failure at authoritative boundaries, not merely disabled buttons.
Neither design permits unsafe production drills or automatic activation.

## Review decisions to record

1. Accept the two-individual-axle rigid pilot boundary, or require a wider initial
   vehicle model based on verified fleet data before implementation starts.
2. Accept robust mass-centre/tare intervals and exact, conservative comparison
   semantics; agree which manufacturer/operating limits the pilot must supply.
3. Accept fixed-slot, unit-at-a-time reshuffling and explicit staging/equipment
   permissions. Wider repacking and uncertain handling remain unsupported.
4. Approve the performance gates in the two designs **before** measurements.
5. Select pilot rounds/data owner and the warehouse reviewer; explicitly separate
   the implementation review from customer activation approval.

Implementation sequence after design approval: standalone axle validator and
oracle; joint axle/stack search; native/HGS integration; Cursus witnesses and
workflow tests; isolated real-data validation. Reshuffling follows as a separate
implementation milestone. Production activation requires separate authorization.
