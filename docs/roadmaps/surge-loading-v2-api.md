# Proposed loading v2 API: C and JSON parity

Status: **normative design proposal for review, not implemented** (2026-11-08).
All new types, symbols, CLI options and schema fields in this document are
proposed. Current Surge accepts loading version 1 only. Do not submit these
examples to production or advertise support before implementation/capability tests.

This is the shared interface specification for
[axle loads](../design-surge-axle-loads.md) and
[bounded reshuffling](../design-surge-reshuffling.md). Their physical/search rules
remain normative. The [review/validation plan](../design-surge-loading-next.md)
retains axle-first implementation and the independent historical validation track.

## 1. Version and capability negotiation

- Absent loading and existing version-1 loading/C functions remain unchanged.
  Version 2 is separate; no change to existing public struct layouts or enum values.
- V2 always requires `stacking`. Other known capabilities are `axle_loads` and
  `bounded_reshuffling`. They are independently selectable and can be combined.
  Adding a required capability never permits dropping another requirement.
- Capability discovery reports what the **running binary** implements, not what
  its headers/designs name. After complete API/HGS integration, axle-first builds advertise stacking/axles only;
  reshuffling requests then fail with NOT_IMPLEMENTED. No scalar/strict fallback.
- Unknown version: NOT_IMPLEMENTED. Unknown capability name/bit: INVALID_ARG.
  Known but unimplemented capability/model: NOT_IMPLEMENTED. Reject before solve.
- Proposed discovery surfaces: C function below; CLI `surge_solve --capabilities`
  emitting the same capability document; additive `loading_capabilities` on the
  existing HTTP `GET /api/v1/version` response. Missing discovery in older binaries
  is not support. Do not add a mandatory HTTP service for Cursus/native execution.
- A capability document has `schema_version:2`, `implemented_capabilities` (known
  string array), `axle_models` (`rigid_two_axle_v1` when implemented),
  `handling_models` (`fixed_slot_external_staging_v1` when implemented), and
  `limits` with the numeric constants below. Enum strings/arrays are canonical
  output, not numeric C enum ordinals. Discover only compiled/validated models.

## 2. Proposed public C declarations

This block is a complete proposed declaration set, not an installed header.
Include `surge.h` for existing opaque context, status, loading physical types and
cancellation callback. New fixed-layout names carry V2; future incompatible
changes require new names. New code must not append fields to V1 types.

```c
#include <stddef.h>
#include <stdint.h>
#include "surge.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SG_LOADING_V2_SCHEMA 2u
#define SG_LOADING_V2_MAX_UNITS 128u
#define SG_LOADING_V2_MAX_SLOTS 128u
#define SG_LOADING_V2_MAX_CLASSES 64u
#define SG_LOADING_V2_MAX_LAYERS 8u
#define SG_LOADING_V2_MAX_STOPS 100000u
#define SG_LOADING_V2_MAX_NODES 100000u
#define SG_LOADING_V2_MAX_FRONTIER 4096u
#define SG_LOADING_V2_MAX_STAGED 4u
#define SG_LOADING_V2_MAX_EXTRA_MOVES_STOP 8u
#define SG_LOADING_V2_MAX_EXTRA_MOVES_ROUND 64u
#define SG_LOADING_V2_MAX_OPERATIONS 320u
#define SG_LOADING_V2_MAX_AXLE_STATES 321u
#define SG_LOADING_V2_DEPOT UINT32_MAX

#define SG_LOADING_V2_STACKING UINT32_C(1)
#define SG_LOADING_V2_AXLES UINT32_C(2)
#define SG_LOADING_V2_RESHUFFLING UINT32_C(4)
#define SG_LOADING_V2_EQUIPMENT_FORKLIFT UINT32_C(1)
#define SG_LOADING_V2_EQUIPMENT_PALLET_JACK UINT32_C(2)
#define SG_LOADING_V2_EQUIPMENT_TAIL_LIFT UINT32_C(4)

typedef enum {
    SG_LOADING_V2_UNSPECIFIED = 0,
    SG_LOADING_V2_DENIED = 1,
    SG_LOADING_V2_ALLOWED = 2
} SGLoadingV2Permission;
typedef enum { SG_LOADING_V2_RIGID_TWO_AXLE = 1 } SGLoadingV2AxleModel;
typedef enum {
    SG_LOADING_V2_FIXED_SLOT_EXTERNAL_STAGING = 1
} SGLoadingV2HandlingModel;
typedef enum {
    SG_LOADING_V2_LOAD = 1, SG_LOADING_V2_DELIVER = 2,
    SG_LOADING_V2_STAGE = 3, SG_LOADING_V2_RESTORE = 4
} SGLoadingV2OperationKind;
typedef enum {
    SG_LOADING_V2_WITNESS = 1, SG_LOADING_V2_CAPACITY_BOUND,
    SG_LOADING_V2_EXHAUSTIVE_NO_WITNESS, SG_LOADING_V2_NODE_BUDGET,
    SG_LOADING_V2_FRONTIER_BUDGET, SG_LOADING_V2_INTERRUPTED,
    SG_LOADING_V2_UNSUPPORTED_SIZE, SG_LOADING_V2_SCHEDULE_REJECTED
} SGLoadingV2Reason;
typedef enum {
    SG_LOADING_V2_ERROR_NONE = 0, SG_LOADING_V2_INVALID_INPUT,
    SG_LOADING_V2_UNSUPPORTED_VERSION, SG_LOADING_V2_UNSUPPORTED_CAPABILITY,
    SG_LOADING_V2_UNSUPPORTED_MODEL, SG_LOADING_V2_INVALID_WITNESS,
    SG_LOADING_V2_WITNESS_REJECTED, SG_LOADING_V2_NO_RESULT,
    SG_LOADING_V2_ALLOCATION_FAILED, SG_LOADING_V2_INTERNAL_ERROR,
    SG_LOADING_V2_VALIDATION_UNKNOWN
} SGLoadingV2ErrorCode;

typedef struct { int32_t min, max; } SGLoadingV2SignedInterval;
typedef struct { uint64_t min, max; } SGLoadingV2MassInterval;
typedef struct { uint32_t numerator, denominator; } SGLoadingV2Fraction;
typedef struct { double start, end; } SGLoadingV2TimeInterval;

typedef struct {
    SGLoadingV2AxleModel model;
    const char *profile_sha256;
    uint32_t wheelbase_mm, footprint_length_mm;
    int32_t deck_min_mm, deck_max_mm;
    uint32_t slot_count;
    const int32_t *slot_centres_mm;
    SGLoadingV2MassInterval front_tare_g, rear_tare_g;
    SGLoadingV2MassInterval front_limits_g, rear_limits_g;
    uint64_t max_gross_g;
    const SGLoadingV2Fraction *min_front_fraction; /* NULL => explicit no rule */
} SGLoadingV2Axles;

typedef struct {
    SGLoadingV2HandlingModel model;
    const char *profile_sha256;
    SGLoadingV2Permission allowed;
    uint32_t equipment_mask;
    uint64_t max_unit_weight_g;
    uint32_t max_unit_height_mm, max_staged_units;
    uint32_t max_extra_moves_per_stop, max_extra_moves_per_round;
    uint32_t max_extra_sec_per_stop, max_extra_sec_per_round;
    uint32_t stage_sec, restore_sec;
    uint64_t stage_fee_units, restore_fee_units;
} SGLoadingV2Handling;

typedef struct {
    SGLoadingVehicle physical;
    const SGLoadingV2Axles *axles;       /* required iff AXLES selected */
    const SGLoadingV2Handling *handling; /* required iff RESHUFFLING selected */
} SGLoadingV2Vehicle;
typedef struct {
    SGLoadingUnit physical; /* physical.stop ignored; actual route supplies order */
    const SGLoadingV2SignedInterval *cg_offset_mm; /* required iff AXLES */
    SGLoadingV2Permission temporary_offload_allowed; /* explicit iff RESHUFFLING */
    uint32_t required_equipment_mask; /* zero when RESHUFFLING absent */
} SGLoadingV2Unit;

typedef struct {
    uint64_t max_weight_g;
    uint32_t max_height_mm;
    uint64_t allowed_classes; /* bit i = class i; zero permits no classes */
} SGLoadingV2StagingPosition;
typedef struct {
    const char *profile_sha256;
    SGLoadingV2Permission allowed;
    uint32_t equipment_available_mask, equipment_permitted_mask;
    uint32_t position_count;
    const SGLoadingV2StagingPosition *positions;
    const SGLoadingV2TimeInterval *handling_interval_sec; /* NULL => unrestricted */
    uint32_t max_extra_moves, max_extra_sec;
} SGLoadingV2StopPolicy;

typedef struct {
    uint32_t struct_size, schema_version, required_capabilities;
    uint32_t node_budget;
    uint32_t frontier_budget; /* 1..4096 iff RESHUFFLING, otherwise zero */
} SGLoadingV2Config;
typedef struct {
    uint32_t struct_size, schema_version, implemented_capabilities;
    uint32_t axle_models;     /* bit 0 = rigid_two_axle_v1 */
    uint32_t handling_models; /* bit 0 = fixed_slot_external_staging_v1 */
    uint32_t max_units, max_slots, max_classes, max_layers, max_stops;
    uint32_t max_nodes, max_frontier, max_staged;
    uint32_t max_extra_moves_stop, max_extra_moves_round;
    uint32_t max_operations, max_axle_states;
} SGLoadingV2Capabilities;
typedef struct {
    SGLoadingV2ErrorCode code;
    uint32_t vehicle_id, trip_index, operation_index; /* UINT32_MAX if absent */
    char path[192]; /* UTF-8 canonical JSON pointer; diagnostic, never authorization */
} SGLoadingV2Error;

typedef struct {
    uint32_t vehicle_id, trip_index, task_count;
    const uint32_t *task_ids; /* ordered delivery tasks in this ONE reload round */
    double departure_sec;    /* same clock/unit as the ordinary model */
} SGLoadingV2Round;
typedef struct {
    SGLoadingV2OperationKind kind;
    uint32_t stop_index; /* DEPOT for LOAD, actual stop ordinal otherwise */
    uint32_t request_id, unit_index, slot, layer;
    uint32_t staging_position; /* UINT32_MAX for LOAD/DELIVER */
} SGLoadingV2Operation;
typedef struct {
    uint32_t request_id, unit_index, slot, layer;
} SGLoadingV2Placement;
typedef struct { int64_t numerator; uint32_t denominator; } SGLoadingV2Rational;
typedef struct {
    uint32_t step; /* zero = empty depot; operation i completes step i+1 */
    SGLoadingV2Rational front_min_g, front_max_g, rear_min_g, rear_max_g;
    uint64_t gross_min_g, gross_max_g;
} SGLoadingV2AxleState;
typedef struct {
    uint32_t stop_index, task_id;
    double arrival_sec, service_start_sec, departure_sec, base_service_sec;
    uint32_t extra_service_sec, extra_moves;
    uint64_t extra_fee_units;
} SGLoadingV2StopEvidence;
typedef struct {
    uint32_t vehicle_id, trip_index;
    SGLoadingOutcome outcome;
    SGLoadingV2Reason reason;
    uint32_t nodes;
    uint32_t placement_count, operation_count, axle_state_count, stop_count;
    const SGLoadingV2Placement *placements;
    const SGLoadingV2Operation *operations;
    const SGLoadingV2AxleState *axle_states;
    const SGLoadingV2StopEvidence *stops;
    uint32_t extra_moves, extra_service_sec;
    uint64_t extra_fee_units;
} SGLoadingV2RoundEvidence;
typedef struct {
    uint32_t request_id, diagnostic_node_budget;
    uint32_t feasible_vehicles, infeasible_vehicles, unknown_vehicles;
    /* scope is always physical_singleton_not_route_insertability */
} SGLoadingV2Diagnostic;
typedef struct {
    uint32_t schema_version, required_capabilities, node_budget, frontier_budget;
} SGLoadingV2ReportInfo;
typedef enum {
    SG_LOADING_V2_FIND_WITNESSES = 1, SG_LOADING_V2_REPLAY_WITNESSES = 2
} SGLoadingV2PlanMode;
typedef struct SGLoadingV2Report SGLoadingV2Report;

SGStatus sg_loading_v2_capabilities(SGLoadingV2Capabilities *out);
SGStatus sg_loading_v2_set_config(SGContext *ctx, const SGLoadingV2Config *config);
SGStatus sg_loading_v2_set_vehicle(SGContext *ctx, uint32_t vehicle_id,
                                   const SGLoadingV2Vehicle *vehicle);
SGStatus sg_loading_v2_set_request(SGContext *ctx, uint32_t request_id,
                                   const SGLoadingV2Unit *units, uint32_t count);
SGStatus sg_loading_v2_set_stop_policy(SGContext *ctx, uint32_t task_id,
                                       const SGLoadingV2StopPolicy *policy);
SGStatus sg_loading_v2_reset(SGContext *ctx);
SGStatus sg_loading_v2_get_error(const SGContext *ctx, SGLoadingV2Error *out);
SGStatus sg_loading_v2_check_round(SGContext *ctx, const SGLoadingV2Round *round,
    SGLoadingShouldStop should_stop, void *user_data, SGLoadingV2Report **out);
SGStatus sg_loading_v2_validate_round_witness(SGContext *ctx,
    const SGLoadingV2Round *round, const SGLoadingV2ReportInfo *submitted_info,
    const SGLoadingV2RoundEvidence *witness);
SGStatus sg_loading_v2_validate_plan(SGContext *ctx,
    uint32_t route_count, const SGPlanRoute *routes,
    const uint8_t *const *trip_starts, SGLoadingV2PlanMode mode,
    const SGLoadingV2ReportInfo *submitted_info,
    uint32_t witness_count, const SGLoadingV2RoundEvidence *witnesses,
    SGLoadingV2Report **out);
SGStatus sg_loading_v2_solution_report(const SGContext *ctx, SGLoadingV2Report **out);
SGStatus sg_loading_v2_report_stats(const SGLoadingV2Report *report, SGStats *out);
SGStatus sg_loading_v2_report_info(const SGLoadingV2Report *report,
                                   SGLoadingV2ReportInfo *out);
SGStatus sg_loading_v2_report_view(const SGLoadingV2Report *report,
    uint32_t *round_count, const SGLoadingV2RoundEvidence **rounds,
    uint32_t *diagnostic_count, const SGLoadingV2Diagnostic **diagnostics);
void sg_loading_v2_report_free(SGLoadingV2Report *report);

#ifdef __cplusplus
}
#endif
```

## 3. C lifecycle, ownership and validation

1. Create the ordinary context/model (vehicles, tasks, requests, travel/time/cost
   data). Query capabilities; initialize capability/config `struct_size` to exact
   `sizeof` and `schema_version` to 2. Bad sizes are INVALID_ARG; known size with
   unsupported schema returns NOT_IMPLEMENTED. No future struct tail is guessed.
2. Set V2 config, then complete vehicle/unit declarations and stop policies.
   Every context vehicle/request needs one declaration; zero units means explicitly
   empty, not missing. Every request must have exactly one delivery task.
   Reshuffling additionally requires exactly one policy per delivery task.
   Duplicate IDs in JSON are invalid; replacing an existing record is an explicit
   typed setter operation. Missing or foreign IDs fail model validation.
3. Pointers are caller-owned for the duration of a setter, with valid arrays of
   the declared length. Setters synchronously deep-copy every nested input,
   including strings/intervals/positions. Caller may immediately free or change
   them after success. NULL is allowed for a zero-length array only; a non-NULL
   pointer with count zero is ignored. NULL nonempty arrays are invalid.
4. Setters validate local values before replacement. Failure leaves the previous
   model, saved result and caches unchanged, except last-error diagnostics.
   Cross-record completeness/ordinary model requirements are checked by existing
   `sg_validate_model` before solving/checking. Successful mutation invalidates
   saved results/caches. Do not mix V1 loading setters with active V2 declarations;
   return INVALID_ARG. Use a fresh context, or explicit V2 reset before switching.
5. A config's capability set cannot change after declarations exist without reset;
   budgets may change with cache/result invalidation. Reset removes V2 facts and
   disables V2 enforcement; it is a low-level explicit operation, not an application
   override of Cursus's durable source requirements. No NULL setter means deletion.
6. Reuse existing HGS/population solving and model validation entry points; V2
   must govern all mutation/finalization paths. Model/context mutation and report
   queries are serialized, as in V1. Existing cancellation is the permitted
   concurrent action; controlled checking also polls the supplied callback.
7. `check_round` searches the one specified round, including operation-dependent
   schedule against the ordinary model. It does not publish a solution or certify
   shift consistency with preceding/following rounds; full plan validation does.
   A successful call returns one owned report with one outcome, even UNKNOWN or
   INFEASIBLE. `solution_report` takes an immutable deep snapshot of all committed
   round evidence/diagnostics; no committed result returns ERROR/NO_RESULT.
8. Initialize `*out=NULL`. Any non-OK function status leaves it NULL. Reports are
   library-owned allocations freed only by `report_free` (NULL-safe); borrowed
   array views remain valid until that free, independent of context destruction
   or subsequent mutation. Never `free` those arrays or cast away const.
   `report_view` allocates nothing and requires all output pointer arguments.
   `report_info` returns schema/capabilities/budget from that immutable snapshot,
   not a subsequently changed context. Report views may be read concurrently if
   lifetime is externally held; concurrent free is invalid.
9. `validate_round_witness` does no packing/search. It checks the submitted
   arrangement, operations, summaries and schedule against the supplied round and
   context. It returns OK only for a FEASIBLE witness. A rejected witness does not
   prove that no alternative load can work. Input is borrowed for the call.

`SGLoadingV2Permission.UNSPECIFIED` is invalid wherever permission is required.
It is required for unit temporary permission when reshuffling is absent; then
its equipment mask must be zero. Optional feature pointers must be NULL when
not selected, and complete when selected. JSON false maps to DENIED, not to
UNSPECIFIED. Zero numeric limits are always literal limits. Null fraction/time
pointers map to explicit JSON null; they never enable an otherwise denied policy.

`validate_plan` is the full-plan parity entry point: it computes all departure/
arrival times, reloads, shift effects and costs using the **submitted** witnesses,
not a separately optimized replacement. In FIND mode submitted_info/witnesses must
be NULL and witness_count zero; in REPLAY mode metadata is required and must match
the config, and the array exactly covers the fixed plan's rounds. Optional
trip_starts uses existing first-zero/one-flag-per-task semantics. Caller arrays are borrowed. Success atomically installs the fully validated
native solution snapshot (as in existing plan validation), and returns an owned
complete report. This does not publish Cursus/database state. Any failure leaves *out NULL and the previous native snapshot unchanged;
callers must not read ordinary solution getters after a failed call and treat
that old snapshot as the rejected plan. The JSON handler must return the failure,
not serialize an earlier snapshot. Tests must cover this explicitly. C function statuses are OK only
for fully accepted plans, LIMIT/VALIDATION_UNKNOWN for inconclusive checks, and
INFEASIBLE/WITNESS_REJECTED for the submitted plan's constraint failures. This is
not an infeasibility proof for the optimization problem or another route order.
Empty/partial plans still require the caller's separate order-coverage policy.
Full-plan and solution reports own an `SGStats` snapshot retrievable through
`report_stats`, including ordinary cost/distance/unassigned values computed using
the actual handling sequence. Those values match the normal outer JSON `stats`;
no application-side cost evaluator is substituted. A single-round check has no
whole-plan stats and report_stats returns ERROR without fabricated totals.
For validation-only snapshots, search iterations/elapsed time are zero; existing
ordinary route/stop getters expose the accepted native snapshot only after OK.

## 4. Normative JSON schema and mapping

[The proposed Draft-07 schema](surge-loading-v2-proposed.schema.json) validates
the **`loading` property** of the ordinary solve request. Its `definitions/result`
validates the result's `loading` property. `definitions/capabilities` and
`definitions/error_detail` specify discovery and structured error details. The schema identifier is a proposal,
not a hosted endpoint. Unknown V2 object fields are forbidden at every depth.
Version dispatch occurs before validating this version-specific schema.
Existing outer model fields retain the current `sg_api_build_model` contract.

| JSON field | C source | Meaning |
| --- | --- | --- |
| `version`, `mode` | config.schema_version=2, required hard enforcement | V2 has no soft or disabled mode |
| `required_capabilities` | config.required_capabilities | String set maps to bits 1/2/4; no duplicates |
| `node_budget` | config.node_budget | Required, 0..100000; zero yields UNKNOWN unless a sound negative bound applies |
| `frontier_budget` | config.frontier_budget | Required 1..4096 iff reshuffling, otherwise forbidden/zero in C |
| `vehicles[].vehicle_id` | vehicle setter argument | Existing model ID; full coverage, not an array ordinal substitution |
| Vehicle physical fields | vehicle.physical | V1 meanings; slot count/class count inferred from array lengths |
| `vehicles[].axles` | vehicle.axles | Required iff axles; model `rigid_two_axle_v1` maps to enum value 1 |
| `axles.slot_centres_mm` | axles.slot_count/slot_centres_mm | Length equals physical slot count, same index order |
| Tare/limit/CG `{min,max}` | interval structs | Endpoints required, ordered; no implicit point/default |
| `min_front_fraction` | optional fraction pointer | Required nullable field; null = no rule; `0<=p<=q`, q positive |
| `vehicles[].handling` | vehicle.handling | Required iff reshuffling; model fixed_slot_external_staging_v1 maps to enum 1; profile SHA, budgets, equipment, times and fees |
| `requests[].request_id,units` | request setter ID/array/count | Stable per-request `unit_index` is array position |
| Unit physical fields | unit.physical | Existing physical.stop is ignored; candidate order supplies actual stop |
| `cg_offset_mm` | unit.cg_offset_mm | Required iff axles, forbidden otherwise |
| Unit offload flag/equipment | permission enum/equipment mask | Both required iff reshuffling, forbidden otherwise |
| `stops[].task_id` | stop-policy setter argument | Delivery-task ID, not customer display ID or current route ordinal |
| Stop policy properties | SGLoadingV2StopPolicy | Required complete list iff reshuffling, forbidden otherwise |
| `profile_sha256` | const char pointer | Exactly 64 lowercase hex bytes plus C terminator; external immutable-profile identity |

Array/set ordering: vehicle/request/stop records match by model ID. Slot and unit
arrays are ordered. Class lists and capability lists are mathematical sets;
canonical output orders capability bits 1,2,4 and class IDs ascending.
Setters and parsing normalize equivalent logical values before cache comparison;
never authorize reuse from raw struct padding, pointer values or ignored stop
ordinals. Normalize signed zero in time values without rounding any nonzero time. JSON duplicate
object keys, duplicate IDs/list members, booleans-as-numbers, non-finite/fractional
integers, numeric strings, wrong nulls and signed/unsigned overflow are rejected,
not coerced. Signed millimetres really accept negative coordinates; reusing V1's
nonnegative-number parser would be incorrect. C validates equivalent ranges,
known enum/mask bits, pointer presence and length before copying.

Normative semantic checks beyond JSON Schema: intervals ordered; wheelbase/deck/
footprints/CG/access geometry consistent; class IDs in the shared namespace;
all model IDs exactly covered; p<=q and checked rational arithmetic; each request
one delivery task; required metadata present; prohibited cross-feature fields
rejected; and no source rules inferred from absence. Both interfaces run the same
local/model validators after parsing, not parallel divergent implementations.

Physical numerical bounds are as specified in the axle design: V2 mass quantities
<=10^9 g, signed coordinates +/-10^6 mm, existing unit/slot/class/layer bounds,
max units above 127. Rational numerator/denominator inputs <=10^6 with denominator
positive. These are protocol bounds, not vehicle allowances. V1 larger accepted
numeric values are not silently narrowed when translating: reject unrepresentable
V2 conversion. Each added fee <=10^9 native objective units, each stage/restore
<=86400 seconds, per-stop/round extra seconds <=INT32_MAX. Masks contain only the
three named equipment bits. Known vehicle profiles must certify the handling
limits for every permitted equipment combination.

Timestamps/service values remain finite double seconds in the ordinary model's
clock, range 0..10^12; no truncation of fractional travel/service times or invented
clock/timezone conversion. Integers for added handling remain exact. A reshuffling
permission interval must conservatively contain the **whole effective service**
when extra moves occur in first V2, not merely departure. A narrower per-operation
permission schedule is outside this first contract. No staging wait outside that
interval is silently lost; the evaluator may wait and must propagate it.

Equipment availability is `(vehicle_equipment | site_available) & site_permitted`.
Every unit's required mask must be a subset. Permission must be ALLOWED on the
vehicle, site and staged unit; all staging/physical/time limits also apply. Fees
are additional handling fees, explicitly excluding duration charges already in
the ordinary model. Time costs use ordinary coefficients exactly once.

The V2 model has at most 100000 vehicles, requests and delivery stops. Zero-unit
stops remain supported: the 128-unit packing bound is **not** a 128-stop bound.
Reports include ordinary schedule entries for those stops but no fictional unit
moves. Transport body/memory limits still apply independently and fail as resource
errors; they do not prove infeasibility. Output/scratch sizes are checked before
allocation and remain bounded independently of iteration count.

## 5. Results and certificates

The normal solution gets `loading.version=2`, `mode=hard`, the actual required
capabilities and configured node budget, round evidence and scoped singleton
diagnostics. A one-round C report serializes to a one-element rounds array;
solution reports use the same shape for all committed rounds. A raw native report
is not a Cursus approval: Cursus must additionally bind admitted source hashes,
full native-request/executable identity, scenario/revision and durable requirements.
JSON parsing or C report construction never creates tenant authorization.

| JSON | C mapping / invariant |
| --- | --- |
| `rounds[].outcome` | Existing SGLoadingOutcome; strings feasible/infeasible/unknown |
| `reason` | V2Reason in declaration order: witness/capacity_bound/exhaustive_no_witness/node_budget/frontier_budget/interrupted/unsupported_size/schedule_rejected |
| `node_budget,frontier_budget` | Immutable report metadata; frontier required iff reshuffling, otherwise absent/zero in C |
| `vehicle_id,trip_index,nodes` | Round fields; nodes <= configured combined budget |
| `placements` | Initial departure placements by request_id/unit_index/slot/layer |
| `operations` | Complete LOAD/DELIVER/STAGE/RESTORE sequence, not just temporary moves |
| `operation.stop_index` | null maps to DEPOT for LOAD only; otherwise ordinal in round.task_ids |
| `operation.slot,layer` | Destination for LOAD/RESTORE, current source for DELIVER/STAGE |
| `operation.staging_position` | Required only for STAGE/RESTORE; absent maps to UINT32_MAX otherwise |
| `axle_states` | Required after every operation plus initial empty state iff axles selected; [] otherwise |
| State reaction bounds | Rational C numerators -> canonical decimal **strings**, denominators -> integer L |
| `stops` | StopEvidence, complete order including zero-unit stops; inferred effective_service=base+extra |
| `extra_moves,extra_service_sec,extra_fee_units` | Round totals; independently equal sum of stop contributions |
| `unassigned_checks` | Diagnostic views; fixed physical_singleton_not_route_insertability scope |

Every feasible witness has exactly one initial placement/load and one delivery
for each unit, plus matching legal temporary moves. All depot LOADs precede stop
operations. Stop indices never move backwards; restore all blockers before leaving
a stop. Source/target positions and inventory must agree with replay. Preserve
per-request unit order in placements, with requests ordered by actual stop order;
sequence order is otherwise explicitly represented by operations. Stable IDs are
not compacted independently for placements, operations and diagnostics. Cursus's
adapter must restore all of them using one mapping.

At most 128 units mean at most 256 ordinary load/deliver operations plus 64 extra
moves: 320 operations, and 321 axle states. Axle state `step=0` is the empty depot;
operation array element i completes step i+1. Every rational denominator equals
the positive wheelbase; numerators must fit signed int64, with no leading plus,
leading zero, exponent, -0, NaN or infinity. Units are grams; the numerator is a
scaled gram-mm value. Never serialize it as an imprecise JSON number. Exact interval
extrema and summaries are recomputed from source facts, not trusted in the input.

FEASIBLE requires reason witness and complete independently accepted evidence.
INFEASIBLE requires a sound capacity bound/exhaustive supported search; checking
only the first arrangement is not exhaustive. UNKNOWN covers interrupted/budget/
frontier/unsupported round-size cases. `schedule_rejected` is a scoped rejection
reason, not a proof all alternative sequences fail; exhaustive route infeasibility
uses exhaustive_no_witness. Non-feasible results contain empty evidence arrays and
zero totals; no partial witness can be mistaken for a result to publish. A round
with zero units still checks tare and schedule; with axles it has state zero and
no unit operations. No assigned rounds is not proof of complete order service.

Singleton diagnostic counts sum to the eligible model vehicle count; one row per
unassigned request, no duplicate IDs. Their budget is separately capped at 128.
They test physical capability in isolation, not insertion into a current route or
schedule. Support can be non-monotone; singleton failure does not prove a request
cannot be served in a combined load. Unknown counts stay explicit and cannot
be converted into a universal unassigned reason or a positive certificate.

## 6. Solve versus fixed-plan/witness validation

Ordinary JSON solve: send the existing full native request with V2 `loading`.
C constructs the same base model and declarations, validates it, then calls the
existing population solver. No new optimizer/HTTP service is needed.

The existing HTTP JSON `POST /api/v1/solve` supports a `plan` validation mode.
Extend that mode for V2 through the same native model and round checks. Proposed
optional `loading_witnesses` is a V2 result-shaped object, accepted **only** with
`plan`. If omitted, search for feasible joint witnesses for the fixed task/trip
order under the configured budgets. If supplied, independently replay those exact
witnesses without finding a replacement; require exact complete route/trip coverage,
matching capabilities/budget and no unexpected unassigned diagnostic rows. Failed
submitted evidence is not replaced by another packing and reported as valid.

C parity is the proposed `sg_loading_v2_validate_plan`, integrating ordinary
constraints and V2 witness-derived scheduling in one full-plan evaluation.
Using the existing validator to find another witness and then checking each
submitted round in isolation is insufficient: its handling durations may differ.
The single-round search/replay functions above have only one-round scope. First trip flag must be zero; reload boundaries split
witnesses identically. The proposed CLI V2 dispatch must honor `plan` and
`loading_witnesses` just like the HTTP handler; current CLI direct-solve behavior
must not silently ignore them. This is an implementation requirement, not current
CLI support. JSON witness validation still checks ordinary schedule/constraints.

`status:OK`, HTTP 200, CLI exit zero, or a successful C function status means the
operation ran, **not** physical feasibility, complete service or business approval.
Callers must inspect physical outcomes/evidence, ordinary violations and their
normal solve-status policy (including LIMIT/incumbent handling).

## 7. Status and error parity

| Situation | C | V2 JSON / HTTP | CLI V2 |
| --- | --- | --- | --- |
| Malformed/missing/duplicate/range/unknown field or capability name | INVALID_ARG, INVALID_INPUT | status INVALID_ARG + loading_error, 400 | exit 2 |
| Unknown version / known unimplemented capability or model | NOT_IMPLEMENTED + specific error | status NOT_IMPLEMENTED + loading_error, 422 | exit 3 |
| Valid search completed | OK + report outcome | normal processing response with outcome, 200 | exit 0 even if UNKNOWN/INFEASIBLE |
| Malformed supplied witness | INVALID_ARG, INVALID_WITNESS | validation error, 400 | exit 2 |
| Full-plan validation remains inconclusive | LIMIT, VALIDATION_UNKNOWN; NULL report | status LIMIT + loading_error, 200; no accepted witness | exit 0; not a passing validation |
| Well-formed witness violates declared constraints | INFEASIBLE, WITNESS_REJECTED (this witness only) | validation rejection with violations, 200 | exit 0; never a passing witness |
| Allocation/internal failure | OUT_OF_MEMORY or ERROR | corresponding status + loading_error, 500 | exit 1 |
| No saved result queried | ERROR, NO_RESULT; NULL output | no result/certificate is fabricated | not a solve outcome |

JSON loading errors have `{code,path,message}`, nested under `loading_error` with
status above. Code strings are error enum names in lowercase without the prefix.
Paths are JSON pointers, e.g. `/loading/vehicles/0/axles/wheelbase_mm`; unknown or
context-only locations use empty string. Error text is descriptive and not a
machine contract or a dump of private customer input. C error path is UTF-8, NUL
terminated and may truncate at a codepoint boundary; classification never relies
on it. C error IDs use UINT32_MAX when inapplicable. `get_error` copies the current
structured error without clearing it; no error is NONE with empty path and
absent IDs. Successful mutators/checks clear prior errors; read-only error/report
queries do not alter context state. Existing last-error text
remains context-owned until another operation/destruction.

The CLI statuses in this table apply to the proposed V2 dispatch only; no promise
to change V1 exit behavior. HTTP queue/rate/body limits retain existing 429/413
and transport handling, not a fake INFEASIBLE result. Cancellation during bounded
checking returns OK with UNKNOWN/interrupted and no certificate. Existing solve
cancellation/status policy remains applicable; no stale or interrupted worker
may publish through Cursus regardless of transport status.

## 8. Examples and C construction correspondence

[Discovery example](loading-v2-examples/capabilities.json) shows all implemented
capabilities only for a future binary supporting both extensions. The axle-only
implementation must omit reshuffling and its handling model. The
[error example](loading-v2-examples/invalid-input.error.json) illustrates the
INVALID_ARG/400 shape and its C error-code/path counterpart.

The three `.request.json` files below are complete synthetic native model inputs;
only their loading blocks are governed by this schema. They are **future requests**,
not executable against current Surge. Result files show possible valid evidence,
not the exact arrangement HGS is guaranteed to select. A solver may find a strict
zero-move alternative to the reshuffling example. Use the supplied witness with
fixed-plan validation to exercise its specific operations. Coordinates are zero,
travel is zero and ordinary service is 60 seconds in these examples.

| Case | Request | Result loading evidence |
| --- | --- | --- |
| Axles only | [request](loading-v2-examples/axles.request.json) | [witness](loading-v2-examples/axles.result-loading.json) |
| Reshuffling only | [request](loading-v2-examples/reshuffling.request.json) | [witness](loading-v2-examples/reshuffling.result-loading.json) |
| Combined | [request](loading-v2-examples/combined.request.json) | [witness](loading-v2-examples/combined.result-loading.json) |

C construction is field-for-field from the mapping table. For axle-only, configure
STACKING|AXLES, node_budget=10000, frontier_budget=0; create one base vehicle, one
delivery task/request; set the physical vehicle and the exact axle descriptor in
axles.request.json; provide its one unit with cg_offset=[-50,50], permission
UNSPECIFIED/equipment zero; then validate and solve/check round vehicle=0,
trip=0, task_ids=[0], departure=0. Stop policy setters are invalid in this mode.

For reshuffling-only, configure STACKING|RESHUFFLING, node_budget=10000,
frontier_budget=256; create the two delivery tasks/requests, one rear-access
vehicle and handling descriptor; pass NULL axle/CG pointers; unit 0 is DENIED
and unit 1 ALLOWED with required forklift mask. Set task-0 ALLOWED staging policy
and explicit task-1 DENIED policy. Check round task_ids=[0,1]. Combined is the
same construction plus AXLES, the given axle descriptor and both CG=[0,0].
No setting may default permission to ALLOWED or invent a missing profile hash.
[Combined fixed-plan replay input](loading-v2-examples/combined.validate.json)
adds the ordinary plan/trip flags and the exact combined witness. Its C counterpart
calls validate_plan in REPLAY mode with routes vehicle 0/tasks [0,1], trip flags
[0,0], matching report metadata and the one supplied round. Expected result after
implementation is OK only if all ordinary and joint physical/schedule checks pass.

The reshuffling witness loads A then B into the same slot, stages B, delivers A,
restores B at layer zero, then delivers B. Extra service is 60 seconds and extra
fees are 5 native units. Stop departures are 120 and 180 seconds. Axle-only shows
reaction intervals for an uncertain CG; combined has seven reaction states, one
before and one after each of its six operations.

These profile hashes are synthetic identifiers, not measurement evidence. All
source/executable/scenario binding remains required in the later Cursus certificate
version. A native profile hash alone cannot authenticate the supplied facts.

### Concrete typed construction examples

These proposed functions configure the same loading values as the JSON examples.
The caller has already added the matching ordinary vehicle/tasks/requests to a
fresh context. They demonstrate the public ownership boundary, not implemented
symbols: all nested data below is local and must be deep-copied by the setters.
Failure at any step requires the caller to discard that partially configured
context; individual setter failure does not undo previous successful setters.

```c
SGStatus example_configure_axles_v2(SGContext *ctx) {
    SGLoadingV2Config config = { sizeof(config), 2,
        SG_LOADING_V2_STACKING | SG_LOADING_V2_AXLES, 10000, 0 };
    SGStatus st = sg_loading_v2_set_config(ctx, &config);
    if (st != SG_STATUS_OK) return st;
    const int32_t centres[] = {2000};
    SGLoadingV2Axles axles = {
        .model = SG_LOADING_V2_RIGID_TWO_AXLE,
        .profile_sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        .wheelbase_mm = 4000, .footprint_length_mm = 1200,
        .deck_min_mm = 0, .deck_max_mm = 8000,
        .slot_count = 1, .slot_centres_mm = centres,
        .front_tare_g = {3000000,3000000}, .rear_tare_g = {2000000,2000000},
        .front_limits_g = {0,6000000}, .rear_limits_g = {0,6500000},
        .max_gross_g = 12000000, .min_front_fraction = NULL
    };
    SGLoadingV2Vehicle vehicle = {0};
    vehicle.physical.slot_count = 1; vehicle.physical.max_layers = 2;
    vehicle.physical.clearance_mm = 2400; vehicle.physical.class_count = 1;
    vehicle.physical.max_stack_weight_g = 1000000;
    vehicle.physical.payload_g = 10000000;
    vehicle.physical.access = SG_LOADING_SIDE_FREE;
    vehicle.physical.allowed_top[0] = UINT64_C(1);
    vehicle.axles = &axles;
    st = sg_loading_v2_set_vehicle(ctx, 0, &vehicle);
    if (st != SG_STATUS_OK) return st;
    const SGLoadingV2SignedInterval cg = {-50,50};
    SGLoadingV2Unit unit = {0};
    unit.physical.height_mm = 1000; unit.physical.weight_g = 500000;
    unit.physical.max_weight_above_g = 500000; unit.physical.max_units_above = 1;
    unit.cg_offset_mm = &cg;
    unit.temporary_offload_allowed = SG_LOADING_V2_UNSPECIFIED;
    st = sg_loading_v2_set_request(ctx, 0, &unit, 1);
    return st == SG_STATUS_OK ? sg_validate_model(ctx) : st;
}

SGStatus example_configure_reshuffling_v2(SGContext *ctx) {
    SGLoadingV2Config config = { sizeof(config), 2,
        SG_LOADING_V2_STACKING | SG_LOADING_V2_RESHUFFLING, 10000, 256 };
    SGStatus st = sg_loading_v2_set_config(ctx, &config);
    if (st != SG_STATUS_OK) return st;
    SGLoadingV2Handling handling = {
        .model = SG_LOADING_V2_FIXED_SLOT_EXTERNAL_STAGING,
        .profile_sha256 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        .allowed = SG_LOADING_V2_ALLOWED, .equipment_mask = 0,
        .max_unit_weight_g = 1000000, .max_unit_height_mm = 2400,
        .max_staged_units = 1, .max_extra_moves_per_stop = 2,
        .max_extra_moves_per_round = 2, .max_extra_sec_per_stop = 60,
        .max_extra_sec_per_round = 60, .stage_sec = 30, .restore_sec = 30,
        .stage_fee_units = 2, .restore_fee_units = 3
    };
    SGLoadingV2Vehicle vehicle = {0};
    vehicle.physical.slot_count = 1; vehicle.physical.max_layers = 2;
    vehicle.physical.clearance_mm = 2400; vehicle.physical.class_count = 1;
    vehicle.physical.max_stack_weight_g = 1000000;
    vehicle.physical.payload_g = 10000000;
    vehicle.physical.access = SG_LOADING_REAR_LIFO;
    vehicle.physical.allowed_top[0] = UINT64_C(1);
    vehicle.handling = &handling;
    st = sg_loading_v2_set_vehicle(ctx, 0, &vehicle);
    if (st != SG_STATUS_OK) return st;
    for (uint32_t i = 0; i < 2; i++) {
        SGLoadingV2Unit unit = {0};
        unit.physical.height_mm = 1000; unit.physical.weight_g = 500000;
        unit.physical.max_weight_above_g = 500000;
        unit.physical.max_units_above = 1;
        unit.temporary_offload_allowed = i ? SG_LOADING_V2_ALLOWED : SG_LOADING_V2_DENIED;
        unit.required_equipment_mask = i ? SG_LOADING_V2_EQUIPMENT_FORKLIFT : 0;
        st = sg_loading_v2_set_request(ctx, i, &unit, 1);
        if (st != SG_STATUS_OK) return st;
    }
    const SGLoadingV2StagingPosition position = {1000000,2400,UINT64_C(1)};
    SGLoadingV2StopPolicy site = {
        .profile_sha256 = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
        .allowed = SG_LOADING_V2_ALLOWED,
        .equipment_available_mask = SG_LOADING_V2_EQUIPMENT_FORKLIFT,
        .equipment_permitted_mask = SG_LOADING_V2_EQUIPMENT_FORKLIFT,
        .position_count = 1, .positions = &position,
        .handling_interval_sec = NULL, .max_extra_moves = 2, .max_extra_sec = 60
    };
    st = sg_loading_v2_set_stop_policy(ctx, 0, &site);
    if (st != SG_STATUS_OK) return st;
    const SGLoadingV2StopPolicy denied = {
        .profile_sha256 = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
        .allowed = SG_LOADING_V2_DENIED
    };
    st = sg_loading_v2_set_stop_policy(ctx, 1, &denied);
    return st == SG_STATUS_OK ? sg_validate_model(ctx) : st;
}
```

For either configured model, query the runtime's capability mask before these
calls. Then use existing HGS/population solving and `solution_report`, or supply
`SGLoadingV2Round` to `check_round`/`validate_round_witness`. Always free an owned
report through `sg_loading_v2_report_free` after inspecting its outcome/evidence.
The combined example uses the same records plus axle pointers/CG and all three
required bits; it must not run as two independent checkers choosing different loads.

## 9. Authoritative parity and regression suite required before implementation release

1. Build each example as a C model and via JSON; compare normalized owned facts,
   discovered capabilities, fixed-round outcome and independently replayed witness.
   Do not require heuristic placements to be bit-identical across architectures.
2. Submit each result witness to both replay interfaces. Compare all reaction
   extrema, state inventories, service/move/fee totals, ordinary violations and
   error classification. JSON -> C -> JSON preserves exact signed coordinates,
   permission/null distinction, unit IDs and rational numerators beyond 2^53.
3. Axle suite: negative CG/deck coordinates, point/interval tare, p/q edges,
   departure-pass/unload-fail, depot loading, valid alternative ordering and
   overflowing checked products. No float narrowing or clipping negative moments.
4. Reshuffling suite: zero/denied/UNSPECIFIED permissions, missing stop profiles,
   equipment union/intersection, literal zero limits, restoration compaction,
   blocked access, staging windows, repeated blockers, extra duration propagated
   to all following trips, native cost reconciliation and no double counting.
5. Strict malformed input suite: duplicate JSON keys/IDs/class/capability members,
   unknown fields/versions/models/masks, numbers-as-strings, null/fraction/negative
   unsigned values, unrepresentable V2 data and mismatched feature descriptors.
   Both interfaces reject; verify model/setter rollback and error OOM paths.
6. Ownership/ABI: mutate/free source buffers after setters, retain report after
   context mutation/free, NULL/zero counts, invalid struct sizes, reset/mixed V1/V2,
   allocation-failure snapshots and concurrent worker/cancel behavior. C/C++ header
   compilation and native ARM64/AMD64 sanitizer/API/CLI parity are mandatory.
7. Preserve zero-unit routes longer than 128 stops, all reload boundaries, retained/
   frozen moves and HGS worker transfer. UNKNOWN and no-result outputs must never
   carry a publishable partial witness. Cache collision/budget changes cannot
   cross-contaminate model versions or capability sets.
8. JSON plan mode and CLI V2 must never ignore submitted witnesses or trip flags.
   Test a witness valid for a different route/profile and a malformed one despite
   an alternative feasible arrangement. Ensure no fallback authorizes either.
9. Two-server Cursus integration/Playwright: all request/placement/operation/state/
   diagnostic IDs use the same sparse-ID mapping; mismatched capability images,
   source/policy/scenario changes, stale lease publication, manual edits and old
   Approved exports fail at the API boundary. Draft inspection stays available.
10. Run the unchanged quality/performance/cancellation gates and the proposed
    extension gates only after review. Schema/example checks are design checks,
    not evidence of an implemented native API or validated customer loads.
