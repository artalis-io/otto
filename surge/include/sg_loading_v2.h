#ifndef SG_LOADING_V2_H
#define SG_LOADING_V2_H
/* Versioned axle/loading API. Reshuffling capability is not implemented. */
#include "sg_loading.h"
#include "sg_types.h"
#include <stddef.h>
#include <stdint.h>
typedef struct SGContext SGContext;

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
typedef enum { SG_LOADING_V2_FIXED_SLOT_EXTERNAL_STAGING = 1 } SGLoadingV2HandlingModel;
typedef enum {
    SG_LOADING_V2_LOAD = 1,
    SG_LOADING_V2_DELIVER = 2,
    SG_LOADING_V2_STAGE = 3,
    SG_LOADING_V2_RESTORE = 4
} SGLoadingV2OperationKind;
typedef enum {
    SG_LOADING_V2_WITNESS = 1,
    SG_LOADING_V2_CAPACITY_BOUND,
    SG_LOADING_V2_EXHAUSTIVE_NO_WITNESS,
    SG_LOADING_V2_NODE_BUDGET,
    SG_LOADING_V2_FRONTIER_BUDGET,
    SG_LOADING_V2_INTERRUPTED,
    SG_LOADING_V2_UNSUPPORTED_SIZE,
    SG_LOADING_V2_SCHEDULE_REJECTED
} SGLoadingV2Reason;
typedef enum {
    SG_LOADING_V2_ERROR_NONE = 0,
    SG_LOADING_V2_INVALID_INPUT,
    SG_LOADING_V2_UNSUPPORTED_VERSION,
    SG_LOADING_V2_UNSUPPORTED_CAPABILITY,
    SG_LOADING_V2_UNSUPPORTED_MODEL,
    SG_LOADING_V2_INVALID_WITNESS,
    SG_LOADING_V2_WITNESS_REJECTED,
    SG_LOADING_V2_NO_RESULT,
    SG_LOADING_V2_ALLOCATION_FAILED,
    SG_LOADING_V2_INTERNAL_ERROR,
    SG_LOADING_V2_VALIDATION_UNKNOWN
} SGLoadingV2ErrorCode;

typedef struct {
    int32_t min, max;
} SGLoadingV2SignedInterval;
typedef struct {
    uint64_t min, max;
} SGLoadingV2MassInterval;
typedef struct {
    uint32_t numerator, denominator;
} SGLoadingV2Fraction;
typedef struct {
    double start, end;
} SGLoadingV2TimeInterval;

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
    const SGLoadingV2SignedInterval *cg_offset_mm;   /* required iff AXLES */
    SGLoadingV2Permission temporary_offload_allowed; /* explicit iff RESHUFFLING */
    uint32_t required_equipment_mask;                /* zero when RESHUFFLING absent */
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
    char path[192]; /* UTF-8 canonical JSON pointer; diagnostic, never
                       authorization */
} SGLoadingV2Error;

typedef struct {
    uint32_t vehicle_id, trip_index, task_count;
    const uint32_t *task_ids; /* ordered delivery tasks in this ONE reload round */
    double departure_sec;     /* same clock/unit as the ordinary model */
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
typedef struct {
    int64_t numerator;
    uint32_t denominator;
} SGLoadingV2Rational;
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
    SG_LOADING_V2_FIND_WITNESSES = 1,
    SG_LOADING_V2_REPLAY_WITNESSES = 2
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
                                   SGLoadingShouldStop should_stop, void *user_data,
                                   SGLoadingV2Report **out);
SGStatus sg_loading_v2_validate_round_witness(SGContext *ctx, const SGLoadingV2Round *round,
                                              const SGLoadingV2ReportInfo *submitted_info,
                                              const SGLoadingV2RoundEvidence *witness);
SGStatus
sg_loading_v2_validate_plan(SGContext *ctx, uint32_t route_count, const SGPlanRoute *routes,
                            const uint8_t *const *trip_starts, SGLoadingV2PlanMode mode,
                            const SGLoadingV2ReportInfo *submitted_info, uint32_t witness_count,
                            const SGLoadingV2RoundEvidence *witnesses, SGLoadingV2Report **out);
SGStatus sg_loading_v2_solution_report(const SGContext *ctx, SGLoadingV2Report **out);
SGStatus sg_loading_v2_report_stats(const SGLoadingV2Report *report, SGStats *out);
SGStatus sg_loading_v2_report_info(const SGLoadingV2Report *report, SGLoadingV2ReportInfo *out);
SGStatus sg_loading_v2_report_view(const SGLoadingV2Report *report, uint32_t *round_count,
                                   const SGLoadingV2RoundEvidence **rounds,
                                   uint32_t *diagnostic_count,
                                   const SGLoadingV2Diagnostic **diagnostics);
void sg_loading_v2_report_free(SGLoadingV2Report *report);

#ifdef __cplusplus
}
#endif

#endif
