#ifndef SURGE_SG_INTERNAL_H
#define SURGE_SG_INTERNAL_H

#include "surge.h"

#include <math.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arbor.h"
#include "sh_arena.h"
#include "sh_dist.h"
#include "sh_stepfunc.h"

/* Constants */
#define SG_UNASSIGNED_PENALTY 10000.0
#define SG_WORST_RANDOMNESS 4.0
#define SG_SHAW_RANDOMNESS 4.0
#define SG_ROUTE_CLUSTER_RANDOMNESS 2.5
#define SG_TIME_CLUSTER_RANDOMNESS 2.0
#define SG_PD_SHAW_RANDOMNESS 3.0
#define SG_ROUTE_SHAW_RANDOMNESS 4.0
#define SG_OPERATOR_SEED_XOR 0x9E3779B97F4A7C15ULL
#define SG_DEMAND_TOLERANCE 1e-9
#define SG_NOISE_REGRET_SCALE 0.1
#define SG_CONSTRUCT_REGRET_K 3
#define SG_CONSTRUCT_DISTANCE_SECONDS 1800.0
#define SG_CONSTRUCT_FALLBACK_SECONDS 600.0
#define SG_ROUTE_OBJECTIVE_VEHICLE_WEIGHT 1000000.0
#define SG_ROUTE_OBJECTIVE_UNASSIGNED_WEIGHT 1000000000.0
#define SG_DEPOT_CAPACITY_PENALTY 50000.0
#define SG_ROUTE_MAX_REGRET_K 4
#define SG_ROUTE_MAX_INTENSIFY_PASSES 8
#define SG_EJECTION_MAX_DEPTH 5
#define SG_EJECTION_BUDGET 50000
#define SG_STRING_L_MAX 10
#define SG_NO_VEHICLE UINT32_MAX

/* Internal types */
typedef struct {
    uint32_t total_requests;
    uint32_t num_assigned;
    uint32_t num_unassigned;

    uint32_t *assigned_ids;
    uint32_t *unassigned_ids;
    uint8_t *assigned_flags;
} SGBootstrapSolution;

typedef struct {
    uint32_t request_id;
    uint32_t task_id;
    uint8_t is_pickup;
    /* Cached timing (populated by sg_route_update_timing) */
    double arrival;        /* time cursor on arrival (after travel from prev) */
    double service_start;  /* max(arrival, tw_early) */
    double depart;         /* service_start + service_seconds */
    double latest_start;   /* latest feasible service start (backward pass) */
    double forward_slack;  /* latest_start - service_start */
    double work_since_break;  /* accumulated work since last break, at departure */
    /* Multi-trip */
    uint8_t trip_start;            /* 1 = vehicle returned to depot before this stop (new trip) */
    double  trip_depot_return;     /* Time vehicle arrived back at depot (only valid when trip_start=1) */
    double  trip_depot_depart;     /* Time vehicle departed depot for this trip (only valid when trip_start=1) */
} SGRouteStop;

typedef struct {
    uint32_t after_stop_index;  /* UINT32_MAX = before first stop */
    double start_time;
    double duration;
} SGRouteBreak;

typedef struct {
    /* sg_route_stop_sequence_feasible */
    double *timing;            /* 5 * stop_cap: service_start|depart|latest_start|forward_slack|seq_arrival */
    double *load_profile;      /* (stop_cap + 1) * dim_count, NULL if dim_count==0 */
    double *dim_scratch;       /* 2 * dim_count: [min_prefix|max_prefix], NULL if dim_count==0 */
    double *pickup_depart;     /* num_requests */
    uint8_t *pickup_seen;      /* num_requests */
    /* sg_route_sequence_feasible_distance */
    SGRouteStop *feas_stops;   /* stop_cap */
    /* Local search (2opt*, cross-exchange, or-opt) */
    uint32_t *candidate_a;     /* route_stride */
    uint32_t *candidate_b;     /* route_stride */
    /* sg_route_candidate_compat_ok */
    uint32_t *exclusion_counts; /* num_exclusion_groups, NULL if 0 */
    uint32_t stop_capacity;     /* = num_requests * 2 */
    SHArena *arena;
} SGScratchBuffers;

typedef struct {
    SGBootstrapSolution base;
    SHArena *arena;  /* NULL = legacy malloc, non-NULL = all arrays allocated from this arena */

    uint32_t num_vehicles;
    uint32_t route_stride;
    uint32_t stop_stride;
    uint32_t vehicles_used;
    double total_distance;

    uint32_t *route_lengths;
    uint32_t *route_requests;
    uint32_t *route_stop_lengths;
    SGRouteStop *route_stops;
    uint32_t *route_stop_prev;
    uint32_t *route_stop_next;
    uint32_t *request_vehicle;
    uint32_t *request_pos;
    uint32_t *request_pickup_stop_pos;
    uint32_t *request_delivery_stop_pos;
    double *route_distance;
    double *route_duration;
    double *route_waiting;
    double *route_overtime;
    double *route_tw_penalty;
    double *route_stop_load;   /* [vehicle * stop_stride * dim_count + stop * dim_count + d] */
    double *route_depot_depart;   /* [num_vehicles] — departure time from start depot */
    double *route_depot_return;   /* [num_vehicles] — return time at end depot (0.0 for open-end/empty) */
    uint64_t *route_commodities;       /* [num_vehicles] bitset of commodity IDs on route */
    uint32_t *route_exclusion_counts;  /* [num_vehicles * num_exclusion_groups] count per group per route */

    /* Break policy metrics */
    double *route_break_time;       /* [num_vehicles] total break time on route */
    uint32_t *route_break_count;    /* [num_vehicles] number of breaks on route */
    double *route_total_work;       /* [num_vehicles] total work time on route */
    SGRouteBreak *route_breaks;     /* [num_vehicles * break_stride] break position records */
    uint32_t break_stride;          /* = stop_stride (safe upper bound) */

    /* Multi-trip */
    uint32_t *route_trip_count;          /* [num_vehicles] */
    uint8_t  *route_request_trip_start;  /* [num_vehicles * route_stride] — parallels route_requests */
} SGRouteSolution;

typedef struct {
    double *remaining_capacity;
    double *remaining_time_seconds;
    uint32_t *depot_vehicle_count;  /* [num_depots] — vehicles assigned to each depot during construction */
    uint64_t *construct_commodities;      /* [num_vehicles] */
    uint32_t *construct_exclusion_counts; /* [num_vehicles * num_exclusion_groups] */
} SGConstructState;

typedef struct {
    int32_t priority;
    uint32_t zone_id;
    int32_t tw_early;
    int32_t tw_late;
    uint8_t has_zone;
    uint8_t has_time_window;
} SGRequestHint;

typedef struct {
    double x;
    double y;
    int32_t tw_early;
    int32_t tw_late;
    uint32_t location_id;  /* UINT32_MAX if unset */
    uint8_t has_location;
    uint8_t has_time_window;
    uint32_t max_simultaneous;        /* 0 = unlimited (default). Max vehicles at depot at once. */
} SGDepotRecord;

typedef struct {
    double *distance_matrix;     /* [num_locations^2] or NULL (= use global) */
    double *duration_matrix;     /* [num_locations^2] or NULL (= use global) */
    uint32_t speed_profile_id;   /* 0 = none, 1..N = ctx->speed_profiles[id-1] */
    uint8_t has_distance_matrix;
    uint8_t has_duration_matrix;
    uint8_t has_speed_profile;
} SGTravelProfile;

typedef struct {
    uint32_t start_depot_id;
    uint32_t end_depot_id;
    uint32_t start_location_id;
    uint32_t end_location_id;
    int32_t shift_early;
    int32_t shift_late;
    double *capacity;
    uint64_t qualifications;
    uint8_t has_depots;
    uint8_t has_shift_time_window;
    uint8_t has_capacity;
    uint8_t open_end;
    int32_t max_duration_seconds;
    uint32_t max_tasks;              /* 0 = unlimited (default) */
    double   max_distance;           /* 0.0 = unlimited (default) */
    double fixed_cost;
    double cost_per_distance;
    double cost_per_duration;
    double cost_per_waiting;
    double cost_per_overtime;
    int32_t depot_loading_seconds;    /* 0 = instant. Time to load at start depot before departure. */
    int32_t depot_unloading_seconds;  /* 0 = instant. Time to unload at end depot after return. */

    /* Break policy */
    int32_t break_max_work_seconds;     /* 0 = disabled. Max continuous work before mandatory break. */
    int32_t break_duration_seconds;     /* Duration of each mandatory break. */
    int32_t max_total_work_seconds;     /* 0 = disabled. Max cumulative work (driving+service) per route. */
    uint8_t has_break_policy;           /* 1 if break cycle is active */

    /* Multi-trip */
    uint32_t max_trips;            /* 0 = unlimited, 1 = default (no multi-trip) */
    int32_t  trip_reload_seconds;  /* Depot service time between trips */
    uint8_t  has_multi_trip;       /* 1 if max_trips != 1 */

    /* Travel profile */
    uint32_t travel_profile_id;    /* 0 = use global, 1..N = ctx->travel_profiles[id-1] */
    uint8_t  has_travel_profile;
} SGVehicleRecord;

typedef struct {
    SGTaskType type;
    double x;
    double y;
    int32_t tw_early;
    int32_t tw_late;
    int32_t service_seconds;
    double *demand;
    uint32_t location_id;  /* UINT32_MAX if unset */
    uint8_t has_location;
    uint8_t has_time_window;
    uint8_t has_demand;
    int32_t soft_tw_early;
    int32_t soft_tw_late;
    double tw_early_penalty;
    double tw_late_penalty;
    uint8_t has_soft_time_window;
    uint8_t num_time_windows;       /* 0 = single TW via tw_early/tw_late; >=2 = disjunct */
    SGTimeWindow *time_windows;     /* NULL when num_time_windows <= 1; sorted array when >=2 */
} SGTaskRecord;

typedef struct {
    SGRequestKind kind;
    uint32_t pickup_task_id;
    uint32_t delivery_task_id;
    uint64_t required_qualifications;
    int32_t max_ride_time_seconds;
    uint64_t *allowed_vehicles;    /* NULL = all allowed; non-NULL = whitelist bitset */
    uint64_t *forbidden_vehicles;  /* NULL = none forbidden; non-NULL = bitset */
    uint16_t allowed_vc_words;     /* allocated uint64_t words for allowed_vehicles */
    uint16_t forbidden_vc_words;   /* allocated uint64_t words for forbidden_vehicles */
    uint8_t has_pickup_task;
    uint8_t has_delivery_task;
    uint8_t has_max_ride_time;
    uint32_t commodity_id;             /* 0 = none, 1..num_commodities = type */
    uint32_t *exclusion_group_ids;     /* NULL = no groups. Heap array. */
    uint16_t num_exclusion_groups;     /* count of groups this request belongs to */
    uint32_t setup_class_id;           /* 0 = none, 1..num_setup_classes */
    double unassigned_penalty;         /* Per-request drop penalty (0.0 = use global) */
    uint8_t has_unassigned_penalty;    /* 1 if per-request penalty is set */
} SGRequestRecord;

struct SGContext {
    SGConfig config;
    SGDemandSignConvention demand_sign_convention;
    uint32_t dimension_count;
    uint32_t num_depots;
    uint32_t num_requests;
    uint32_t num_vehicles;
    uint32_t num_tasks;
    uint32_t zone_count;
    SGStats stats;

    SGDepotRecord *depots;
    SGRequestRecord *requests;
    SGRequestHint *request_hints;
    SGVehicleRecord *vehicles;
    SGTaskRecord *tasks;
    double *zone_distance_matrix;
    uint32_t num_locations;
    double *location_coords;           /* [num_locations * 2]: x,y pairs */
    double *travel_distance_matrix;    /* [num_locations * num_locations] row-major */
    double *travel_duration_matrix;    /* [num_locations * num_locations] row-major */
    SGTravelCallback travel_callback;
    void *travel_callback_data;
    SHRng *op_rng;
    void *active_solution;  /* Temporary: set during destroy ops needing route access */
    SGRouteSolution *final_solution;  /* Retained after solve for route/stop export */
    double unassigned_weight;
    _Atomic uint8_t travel_prepared;
    uint8_t avoid_new_vehicles;  /* Phase 1: skip empty vehicles in repair */
    uint8_t has_depot_capacity;  /* 1 if any depot has max_simultaneous > 0 */
    uint32_t num_commodities;          /* 0 = disabled */
    uint64_t *commodity_conflicts;     /* [num_commodities] bitmask per commodity */
    uint32_t num_exclusion_groups;     /* 0 = disabled */

    /* Sequence-dependent setup times */
    uint32_t num_setup_classes;         /* 0 = disabled */
    double *setup_time_matrix;          /* [num_setup_classes * num_setup_classes], seconds */

    /* Per-operator telemetry (populated after solve) */
    SGOperatorStats *destroy_op_stats;
    SGOperatorStats *repair_op_stats;
    uint32_t num_destroy_ops;
    uint32_t num_repair_ops;

    /* Error diagnostics */
    char last_error[256];

    /* Progress callback */
    SGProgressCallback progress_callback;
    void *progress_callback_data;
    _Atomic uint8_t cancel_requested;

    /* Pre-computed arena size for fast solution copy */
    size_t solution_arena_size;

    /* Pre-allocated scratch buffers (valid between sg_scratch_init/free) */
    SGScratchBuffers scratch;

    /* Warm start */
    uint32_t *initial_route_vehicle_ids;
    uint32_t *initial_route_request_ids;
    uint32_t *initial_route_lengths;
    uint32_t num_initial_routes;
    uint32_t total_initial_requests;

    /* Speed profiles (time-dependent duration multipliers) */
    SHStepFunc **speed_profiles;         /* [num_speed_profiles] array of pointers */
    uint32_t num_speed_profiles;
    uint32_t global_speed_profile_id;    /* 0 = none, 1..N */
    uint8_t has_speed_profiles;          /* fast-path flag */

    /* Travel profiles (per-vehicle-type matrices) */
    SGTravelProfile *travel_profiles;    /* [num_travel_profiles] */
    uint32_t num_travel_profiles;
    uint8_t has_travel_profiles;         /* fast-path flag */
};

/* sg_context.c */
void sg_set_error(SGContext *ctx, const char *fmt, ...);
void sg_clear_error(SGContext *ctx);
SGRequestHint sg_request_hint_default(void);
SGRequestRecord sg_request_record_default(void);
int sg_task_type_valid(SGTaskType type);
void sg_vehicle_records_free(SGVehicleRecord *vehicles, uint32_t count);
void sg_request_records_free(SGRequestRecord *requests, uint32_t count);
void sg_task_records_free(SGTaskRecord *tasks, uint32_t count);
int sg_config_valid(const SGConfig *config);
int sg_priority_policy_valid(SGPriorityRemovalPolicy policy);
int sg_demand_sign_convention_valid(SGDemandSignConvention convention);
void sg_zone_matrix_clear(SGContext *ctx);
int sg_task_ready_for_model(const SGTaskRecord *task);
SGStatus sg_prepare_travel(SGContext *ctx);
int sg_request_representative_location(const SGContext *ctx, uint32_t request_id,
                                        uint32_t *location_id_out);

/* Internal travel lookup functions (static inline) */

static inline void sg_resolve_matrices(const SGContext *ctx, uint32_t vehicle_id,
                                        const double **dist_out, const double **dur_out,
                                        uint32_t *sp_id_out) {
    *dist_out = ctx->travel_distance_matrix;
    *dur_out = ctx->travel_duration_matrix;
    *sp_id_out = ctx->global_speed_profile_id;
    if (ctx->has_travel_profiles && vehicle_id != SG_NO_VEHICLE) {
        uint32_t tp_id = ctx->vehicles[vehicle_id].travel_profile_id;
        if (tp_id > 0) {
            const SGTravelProfile *tp = &ctx->travel_profiles[tp_id - 1];
            if (tp->has_distance_matrix) *dist_out = tp->distance_matrix;
            if (tp->has_duration_matrix) *dur_out = tp->duration_matrix;
            if (tp->has_speed_profile) *sp_id_out = tp->speed_profile_id;
        }
    }
}

static inline void sg_travel(const SGContext *ctx, uint32_t from_loc, uint32_t to_loc,
                              uint32_t vehicle_id, double departure_time,
                              double *dist, double *dur) {
    if (ctx->travel_callback) {
        ctx->travel_callback(from_loc, to_loc, vehicle_id, departure_time,
                             dist, dur, ctx->travel_callback_data);
        return;
    }
    {
        const double *dm, *tm; uint32_t sp_id;
        sg_resolve_matrices(ctx, vehicle_id, &dm, &tm, &sp_id);
        size_t idx = (size_t)from_loc * ctx->num_locations + to_loc;
        *dist = dm[idx];
        *dur  = tm[idx];
        if (sp_id > 0)
            *dur *= sh_step_eval(ctx->speed_profiles[sp_id - 1], departure_time);
    }
}

/* Returns 1 if vehicle has all qualifications required by the request, 0 otherwise. */
static inline int sg_vehicle_qualifies(const SGContext *ctx,
                                        uint32_t vehicle_id, uint32_t request_id) {
    uint64_t required = ctx->requests[request_id].required_qualifications;
    if (required == 0) return 1;
    return (ctx->vehicles[vehicle_id].qualifications & required) == required;
}

/* Returns 1 if vehicle is allowed for the request, 0 otherwise. */
static inline int sg_vehicle_allowed_for_request(const SGContext *ctx,
                                                  uint32_t vehicle_id, uint32_t request_id) {
    const SGRequestRecord *req = &ctx->requests[request_id];
    uint32_t word = vehicle_id >> 6;
    uint64_t bit = 1ULL << (vehicle_id & 63);
    if (req->forbidden_vehicles &&
        word < req->forbidden_vc_words &&
        (req->forbidden_vehicles[word] & bit)) {
        return 0;
    }
    if (req->allowed_vehicles) {
        if (word >= req->allowed_vc_words || !(req->allowed_vehicles[word] & bit)) {
            return 0;
        }
    }
    return 1;
}

/* Returns 1 if request's commodity doesn't conflict with any commodity on the vehicle's route. */
static inline int sg_commodity_compatible(const SGContext *ctx, const SGRouteSolution *sol,
                                           uint32_t vehicle_id, uint32_t request_id) {
    uint32_t cid;
    uint64_t route_bits;
    if (ctx->num_commodities == 0) return 1;
    cid = ctx->requests[request_id].commodity_id;
    if (cid == 0) return 1;
    route_bits = sol->route_commodities[vehicle_id];
    if (route_bits == 0) return 1;
    return (ctx->commodity_conflicts[cid - 1] & route_bits) == 0;
}

/* Returns 1 if no exclusion group the request belongs to already has a member on the vehicle. */
static inline int sg_exclusion_compatible(const SGContext *ctx, const SGRouteSolution *sol,
                                           uint32_t vehicle_id, uint32_t request_id) {
    const SGRequestRecord *req;
    uint16_t g;
    if (ctx->num_exclusion_groups == 0) return 1;
    req = &ctx->requests[request_id];
    if (req->num_exclusion_groups == 0) return 1;
    for (g = 0; g < req->num_exclusion_groups; g++) {
        uint32_t gid = req->exclusion_group_ids[g];
        if (sol->route_exclusion_counts[(size_t)vehicle_id * ctx->num_exclusion_groups + gid] > 0) {
            return 0;
        }
    }
    return 1;
}

/* Construction-phase commodity compatibility using SGConstructState. */
static inline int sg_construct_commodity_compatible(const SGContext *ctx,
                                                     const SGConstructState *state,
                                                     uint32_t vehicle_id, uint32_t request_id) {
    uint32_t cid;
    uint64_t route_bits;
    if (ctx->num_commodities == 0) return 1;
    cid = ctx->requests[request_id].commodity_id;
    if (cid == 0) return 1;
    if (!state->construct_commodities) return 1;
    route_bits = state->construct_commodities[vehicle_id];
    if (route_bits == 0) return 1;
    return (ctx->commodity_conflicts[cid - 1] & route_bits) == 0;
}

/* Construction-phase exclusion compatibility using SGConstructState. */
static inline int sg_construct_exclusion_compatible(const SGContext *ctx,
                                                     const SGConstructState *state,
                                                     uint32_t vehicle_id, uint32_t request_id) {
    const SGRequestRecord *req;
    uint16_t g;
    if (ctx->num_exclusion_groups == 0) return 1;
    req = &ctx->requests[request_id];
    if (req->num_exclusion_groups == 0) return 1;
    if (!state->construct_exclusion_counts) return 1;
    for (g = 0; g < req->num_exclusion_groups; g++) {
        uint32_t gid = req->exclusion_group_ids[g];
        if (state->construct_exclusion_counts[(size_t)vehicle_id * ctx->num_exclusion_groups + gid] > 0) {
            return 0;
        }
    }
    return 1;
}

static inline double sg_travel_dist(const SGContext *ctx, uint32_t from_loc, uint32_t to_loc,
                                     uint32_t vehicle_id) {
    if (ctx->travel_callback) {
        double d, t;
        ctx->travel_callback(from_loc, to_loc, vehicle_id, 0.0, &d, &t,
                             ctx->travel_callback_data);
        return d;
    }
    {
        const double *dist_matrix = ctx->travel_distance_matrix;
        if (ctx->has_travel_profiles && vehicle_id != SG_NO_VEHICLE) {
            uint32_t tp_id = ctx->vehicles[vehicle_id].travel_profile_id;
            if (tp_id > 0 && ctx->travel_profiles[tp_id - 1].has_distance_matrix)
                dist_matrix = ctx->travel_profiles[tp_id - 1].distance_matrix;
        }
        return dist_matrix[(size_t)from_loc * ctx->num_locations + to_loc];
    }
}

static inline double sg_travel_dur(const SGContext *ctx, uint32_t from_loc, uint32_t to_loc,
                                    uint32_t vehicle_id, double departure_time) {
    if (ctx->travel_callback) {
        double d, t;
        ctx->travel_callback(from_loc, to_loc, vehicle_id, departure_time, &d, &t,
                             ctx->travel_callback_data);
        return t;
    }
    {
        const double *dur_matrix = ctx->travel_duration_matrix;
        uint32_t sp_id = ctx->global_speed_profile_id;
        if (ctx->has_travel_profiles && vehicle_id != SG_NO_VEHICLE) {
            uint32_t tp_id = ctx->vehicles[vehicle_id].travel_profile_id;
            if (tp_id > 0) {
                const SGTravelProfile *tp = &ctx->travel_profiles[tp_id - 1];
                if (tp->has_duration_matrix) dur_matrix = tp->duration_matrix;
                if (tp->has_speed_profile) sp_id = tp->speed_profile_id;
            }
        }
        double dur = dur_matrix[(size_t)from_loc * ctx->num_locations + to_loc];
        if (sp_id > 0)
            dur *= sh_step_eval(ctx->speed_profiles[sp_id - 1], departure_time);
        return dur;
    }
}

/* Disjunct TW helpers.
   n <= 1 fast path compiles to identical operations as current inline code. */

static inline double sg_task_snap_forward(const SGTaskRecord *task, double arrival) {
    uint8_t n = task->num_time_windows;
    if (n <= 1) {
        return arrival < (double)task->tw_early ? (double)task->tw_early : arrival;
    }
    {
        uint8_t i;
        const SGTimeWindow *w = task->time_windows;
        for (i = 0; i < n; i++) {
            if (arrival <= (double)w[i].late + 1e-9) {
                return arrival < (double)w[i].early ? (double)w[i].early : arrival;
            }
        }
        /* Past all windows — return arrival; caller's outer-bound check will reject. */
        return arrival;
    }
}

static inline double sg_task_snap_backward(const SGTaskRecord *task, double latest) {
    uint8_t n = task->num_time_windows;
    if (n <= 1) {
        return latest > (double)task->tw_late ? (double)task->tw_late : latest;
    }
    {
        uint8_t i;
        const SGTimeWindow *w = task->time_windows;
        for (i = n; i > 0; i--) {
            if (latest >= (double)w[i - 1].early - 1e-9) {
                return latest > (double)w[i - 1].late ? (double)w[i - 1].late : latest;
            }
        }
        /* Before all windows — return latest; caller's outer-bound check will reject. */
        return latest;
    }
}

/* Returns setup time (seconds) for transition between two consecutive stops.
   prev_request_id = UINT32_MAX means depot → no setup class → 0. */
static inline double sg_setup_time_between(const SGContext *ctx,
                                            uint32_t prev_request_id,
                                            uint32_t cur_request_id) {
    uint32_t pc, cc;
    if (ctx->num_setup_classes == 0) return 0.0;
    pc = (prev_request_id < ctx->num_requests)
         ? ctx->requests[prev_request_id].setup_class_id : 0;
    cc = (cur_request_id < ctx->num_requests)
         ? ctx->requests[cur_request_id].setup_class_id : 0;
    if (pc == 0 || cc == 0) return 0.0;
    return ctx->setup_time_matrix[(size_t)(pc - 1) * ctx->num_setup_classes + (cc - 1)];
}

int sg_request_pd_demands_valid(const SGTaskRecord *pickup, const SGTaskRecord *delivery,
                                uint32_t dimension_count, SGDemandSignConvention convention);
int sg_delivery_task_demand_valid(const SGTaskRecord *delivery, uint32_t dimension_count,
                                  SGDemandSignConvention convention);
uint32_t sg_count_unbound_requests(const SGContext *ctx);

/* sg_solution.c */
void sg_bootstrap_solution_reset(SGBootstrapSolution *sol);
ARStatus sg_bootstrap_solution_init(SGBootstrapSolution *sol, uint32_t total_requests);
int sg_find_id(const uint32_t *ids, uint32_t count, uint32_t id);
ARStatus sg_bootstrap_assign_request(SGBootstrapSolution *sol, uint32_t id);
ARStatus sg_bootstrap_unassign_request(SGBootstrapSolution *sol, uint32_t id);
void *sg_bootstrap_copy(const void *solution, void *user_ctx);
void sg_bootstrap_free(void *solution, void *user_ctx);
double sg_bootstrap_cost(const void *solution, void *user_ctx);
int sg_bootstrap_size(const void *solution, void *user_ctx);
int sg_bootstrap_validate(const void *solution, void *user_ctx);
int sg_get_assigned_count(void *solution, void *user_ctx);
uint32_t sg_get_assigned_element(void *solution, void *user_ctx, int index);
void sg_route_solution_reset(SGRouteSolution *sol);
ARStatus sg_route_solution_init(const SGContext *ctx, SGRouteSolution *sol);
void *sg_route_solution_copy(const void *solution, void *user_ctx);
void sg_route_solution_free(void *solution, void *user_ctx);
int sg_route_solution_validate(const void *solution, void *user_ctx);
double sg_route_solution_cost(const void *solution, void *user_ctx);
int sg_route_solution_size(const void *solution, void *user_ctx);
uint32_t *sg_route_vehicle_ptr(SGRouteSolution *sol, uint32_t vehicle_id);
const uint32_t *sg_route_vehicle_ptr_const(const SGRouteSolution *sol, uint32_t vehicle_id);
SGRouteStop *sg_route_vehicle_stop_ptr(SGRouteSolution *sol, uint32_t vehicle_id);
const SGRouteStop *sg_route_vehicle_stop_ptr_const(const SGRouteSolution *sol,
                                                    uint32_t vehicle_id);
uint32_t *sg_route_vehicle_stop_prev_ptr(SGRouteSolution *sol, uint32_t vehicle_id);
uint32_t *sg_route_vehicle_stop_next_ptr(SGRouteSolution *sol, uint32_t vehicle_id);
const uint32_t *sg_route_vehicle_stop_prev_ptr_const(const SGRouteSolution *sol,
                                                      uint32_t vehicle_id);
const uint32_t *sg_route_vehicle_stop_next_ptr_const(const SGRouteSolution *sol,
                                                      uint32_t vehicle_id);
int sg_request_emit_stops(const SGContext *ctx, uint32_t request_id,
                          SGRouteStop *stops_out, uint32_t *stop_count_out);
int sg_route_rebuild_vehicle_stop_state(const SGContext *ctx, SGRouteSolution *sol,
                                        uint32_t vehicle_id);
int sg_route_splice_stop(const SGContext *ctx, SGRouteSolution *sol,
                         uint32_t vehicle_id, uint32_t at,
                         const SGRouteStop *stop);
int sg_route_excise_stop(const SGContext *ctx, SGRouteSolution *sol,
                         uint32_t vehicle_id, uint32_t at);
double sg_route_objective_cost(uint32_t unassigned, uint32_t vehicles_used,
                               double total_distance);
int sg_route_solution_is_better(const void *candidate, const void *current_best,
                                 void *user_ctx);
void sg_scratch_init(SGContext *ctx);
void sg_scratch_free(SGContext *ctx);

/* sg_cost.c */
double sg_euclid(double ax, double ay, double bx, double by);
int32_t sg_clamp_priority(int32_t priority);
int64_t sg_abs_i64(int64_t value);
const SGRequestHint *sg_get_hint(const SGContext *ctx, uint32_t request_id);
const SGRequestRecord *sg_get_request_record(const SGContext *ctx, uint32_t request_id);
const SGTaskRecord *sg_get_task_record(const SGContext *ctx, uint32_t task_id);
int sg_request_time_midpoint(const SGContext *ctx, uint32_t request_id, int64_t *midpoint);
int sg_request_tw_width(const SGContext *ctx, uint32_t request_id, int32_t *width_out);
int sg_request_time_window_bounds(const SGContext *ctx, uint32_t request_id,
                                  int32_t *early_out, int32_t *late_out);
int sg_request_centroid(const SGContext *ctx, uint32_t request_id, double *x, double *y);
double sg_request_load_magnitude(const SGContext *ctx, uint32_t request_id);
double sg_request_priority_score(const SGContext *ctx, uint32_t request_id);
SGRequestKind sg_request_kind(const SGContext *ctx, uint32_t request_id);
double sg_request_abs_demand_at_dim(const SGContext *ctx, uint32_t request_id, uint32_t dim);
int sg_vehicle_start_end_locations(const SGContext *ctx, uint32_t vehicle_id,
                                   double *sx, double *sy, double *ex, double *ey);
double sg_vehicle_request_cost(const SGContext *ctx, uint32_t vehicle_id,
                               uint32_t request_id, double noise_scale);
int sg_request_best_k_costs(const SGContext *ctx, uint32_t request_id, int k,
                            double noise_scale, double *best_cost, double *kth_cost);
int sg_zone_distance_lookup(const SGContext *ctx, uint32_t zone_a, uint32_t zone_b,
                            double *distance);
double sg_zone_similarity(const SGContext *ctx, uint32_t zone_a, uint32_t zone_b);
double sg_priority_removal_score(const SGContext *ctx, int32_t priority);
double sg_zone_density_score(const SGContext *ctx, const SGBootstrapSolution *sol,
                             uint32_t request_id);
double sg_bootstrap_removal_cost(void *ctx, void *solution, uint32_t element_id);
double sg_route_removal_cost(void *ctx, void *solution, uint32_t element_id);
double sg_bootstrap_relatedness(void *ctx, uint32_t a, uint32_t b);
double sg_route_cluster_relatedness(void *ctx, uint32_t a, uint32_t b);
double sg_time_cluster_relatedness(void *ctx, uint32_t a, uint32_t b);
double sg_time_window_relatedness(void *ctx, uint32_t a, uint32_t b);
double sg_pd_shaw_relatedness(void *ctx, uint32_t a, uint32_t b);
double sg_route_shaw_relatedness(void *ctx, uint32_t a, uint32_t b);
double sg_criticality_removal_cost(void *ctx, void *solution, uint32_t element_id);
int sg_request_time_use_for_vehicle(const SGContext *ctx, uint32_t vehicle_id,
                                    uint32_t request_id, double *time_use_seconds);
void sg_compute_solution_route_metrics(const SGContext *ctx, const SGBootstrapSolution *sol,
                                       uint32_t *vehicles_used_out, double *total_distance_out);

/* sg_feasibility.c */
int sg_route_update_timing(const SGContext *ctx, SGRouteSolution *sol, uint32_t vehicle_id);
int sg_route_update_load(const SGContext *ctx, SGRouteSolution *sol, uint32_t vehicle_id);
int sg_route_eval_insertion_cached(const SGContext *ctx, const SGRouteSolution *sol,
                                   uint32_t request_id, uint32_t vehicle_id,
                                   uint32_t pos, double *score_out,
                                   double *new_route_distance_out);
int sg_route_eval_pd_best_insertion_cached(
    const SGContext *ctx, const SGRouteSolution *sol,
    uint32_t request_id, uint32_t vehicle_id,
    double *best_score_out, uint32_t *best_pickup_pos_out,
    uint32_t *best_delivery_pos_out, double *best_route_distance_out);
int sg_route_stop_sequence_feasible(const SGContext *ctx, uint32_t vehicle_id,
                                    const SGRouteStop *stops, uint32_t stop_count,
                                    double *distance_out);
int sg_route_sequence_feasible_distance(const SGContext *ctx, uint32_t vehicle_id,
                                        const uint32_t *request_ids, uint32_t request_count,
                                        double *distance_out, double *capacity_scratch);
int sg_route_eval_insertion(const SGContext *ctx, const SGRouteSolution *sol,
                            uint32_t request_id, uint32_t vehicle_id, uint32_t pos,
                            uint32_t *candidate_route, double *capacity_scratch,
                            double *score_out, double *new_route_distance_out);
ARStatus sg_route_apply_insertion(const SGContext *ctx, SGRouteSolution *sol,
                                  uint32_t request_id, uint32_t vehicle_id,
                                  uint32_t pos, double new_route_distance);
ARStatus sg_route_apply_pd_insertion(const SGContext *ctx, SGRouteSolution *sol,
                                     uint32_t request_id, uint32_t vehicle_id,
                                     uint32_t pickup_stop_pos, uint32_t delivery_stop_pos,
                                     double new_route_distance);
ARStatus sg_route_unassign_request(const SGContext *ctx, SGRouteSolution *sol,
                                   uint32_t request_id, double *capacity_scratch);
ARStatus sg_route_unassign_removed_requests(const SGContext *ctx, SGRouteSolution *sol,
                                            const uint32_t *removed_ids, int removed_count);
void sg_route_insert_request(uint32_t *route, uint32_t *route_len, uint32_t insert_pos,
                             uint32_t request_id);

/* sg_construct.c */
ARStatus sg_construct_state_init(const SGContext *ctx, SGConstructState *state);
void sg_construct_state_reset(SGConstructState *state);
int sg_construct_select_regret_request(const SGContext *ctx, const SGConstructState *state,
                                       const SGBootstrapSolution *sol, int regret_k,
                                       uint32_t *request_id_out, uint32_t *vehicle_id_out);
ARStatus sg_construct_initial_solution(SGContext *ctx, SGBootstrapSolution *sol);

/* sg_destroy.c */
ARStatus sg_unassign_removed_requests(SGBootstrapSolution *sol,
                                      const uint32_t *removed_ids, int removed_count);
ARStatus sg_destroy_random(void *op_ctx, void *solution, int count,
                           uint32_t *removed_ids, int *removed_count);
ARStatus sg_destroy_worst(void *op_ctx, void *solution, int count,
                          uint32_t *removed_ids, int *removed_count);
ARStatus sg_destroy_shaw(void *op_ctx, void *solution, int count,
                         uint32_t *removed_ids, int *removed_count);
ARStatus sg_destroy_criticality_worst(void *op_ctx, void *solution, int count,
                                      uint32_t *removed_ids, int *removed_count);
ARStatus sg_destroy_route_cluster(void *op_ctx, void *solution, int count,
                                  uint32_t *removed_ids, int *removed_count);
ARStatus sg_destroy_time_cluster(void *op_ctx, void *solution, int count,
                                 uint32_t *removed_ids, int *removed_count);
ARStatus sg_destroy_paired_shaw(void *op_ctx, void *solution, int count,
                                uint32_t *removed_ids, int *removed_count);
ARStatus sg_route_destroy_random(void *op_ctx, void *solution, int count,
                                 uint32_t *removed_ids, int *removed_count);
ARStatus sg_route_destroy_worst(void *op_ctx, void *solution, int count,
                                uint32_t *removed_ids, int *removed_count);
ARStatus sg_route_destroy_shaw(void *op_ctx, void *solution, int count,
                               uint32_t *removed_ids, int *removed_count);
ARStatus sg_route_destroy_criticality_worst(void *op_ctx, void *solution, int count,
                                            uint32_t *removed_ids, int *removed_count);
ARStatus sg_route_destroy_route_cluster(void *op_ctx, void *solution, int count,
                                        uint32_t *removed_ids, int *removed_count);
ARStatus sg_route_destroy_time_cluster(void *op_ctx, void *solution, int count,
                                       uint32_t *removed_ids, int *removed_count);
ARStatus sg_route_destroy_route_removal(void *op_ctx, void *solution, int count,
                                        uint32_t *removed_ids, int *removed_count);
ARStatus sg_route_destroy_time_window(void *op_ctx, void *solution, int count,
                                      uint32_t *removed_ids, int *removed_count);
ARStatus sg_route_destroy_paired_shaw(void *op_ctx, void *solution, int count,
                                      uint32_t *removed_ids, int *removed_count);
ARStatus sg_route_destroy_vehicle_target(void *op_ctx, void *solution, int count,
                                         uint32_t *removed_ids, int *removed_count);
ARStatus sg_route_destroy_vehicle_empty(void *op_ctx, void *solution, int count,
                                        uint32_t *removed_ids, int *removed_count);
ARStatus sg_route_destroy_string(void *op_ctx, void *solution, int count,
                                 uint32_t *removed_ids, int *removed_count);

/* sg_repair.c */
ARStatus sg_reinsert_removed_requests(SGBootstrapSolution *sol,
                                      const uint32_t *removed_ids, int removed_count);
ARStatus sg_repair_greedy_insertion(void *op_ctx, void *solution,
                                    const uint32_t *removed_ids, int removed_count);
ARStatus sg_repair_regret2(void *op_ctx, void *solution,
                           const uint32_t *removed_ids, int removed_count);
ARStatus sg_repair_regret3(void *op_ctx, void *solution,
                           const uint32_t *removed_ids, int removed_count);
ARStatus sg_repair_regret4(void *op_ctx, void *solution,
                           const uint32_t *removed_ids, int removed_count);
ARStatus sg_repair_noise_regret(void *op_ctx, void *solution,
                                const uint32_t *removed_ids, int removed_count);
ARStatus sg_repair_pair_sync(void *op_ctx, void *solution,
                             const uint32_t *removed_ids, int removed_count);
ARStatus sg_route_repair_greedy(void *op_ctx, void *solution,
                                const uint32_t *removed_ids, int removed_count);
ARStatus sg_route_repair_regret2(void *op_ctx, void *solution,
                                 const uint32_t *removed_ids, int removed_count);
ARStatus sg_route_repair_regret3(void *op_ctx, void *solution,
                                 const uint32_t *removed_ids, int removed_count);
ARStatus sg_route_repair_regret4(void *op_ctx, void *solution,
                                 const uint32_t *removed_ids, int removed_count);
ARStatus sg_route_repair_noise_regret(void *op_ctx, void *solution,
                                      const uint32_t *removed_ids, int removed_count);
ARStatus sg_route_repair_pair_sync(void *op_ctx, void *solution,
                                   const uint32_t *removed_ids, int removed_count);
ARStatus sg_route_repair_fill_greedy(SGContext *ctx, SGRouteSolution *sol, double noise_scale);
ARStatus sg_route_repair_fill_regret(SGContext *ctx, SGRouteSolution *sol,
                                     int regret_k, double noise_scale);
int sg_route_rank_insertions_for_request(SGContext *ctx, const SGRouteSolution *sol,
                                         uint32_t request_id, int regret_k,
                                         double noise_scale, double *best_score_out,
                                         double *kth_score_out, uint32_t *best_vehicle_out,
                                         uint32_t *best_pos_out,
                                         uint32_t *best_pickup_stop_pos_out,
                                         uint32_t *best_delivery_stop_pos_out,
                                         double *best_route_distance_out);

/* sg_postprocess.c */
ARStatus sg_route_postprocess_reduce_vehicles(const SGContext *ctx, SGRouteSolution *sol);
ARStatus sg_route_postprocess_ejection_reduce(const SGContext *ctx, SGRouteSolution *sol);
ARStatus sg_route_postprocess_polish_distance(const SGContext *ctx, SGRouteSolution *sol);
int sg_route_try_pd_reorder_once(const SGContext *ctx, SGRouteSolution *sol);
ARStatus sg_route_postprocess_intensify(const SGContext *ctx, SGRouteSolution *sol);
int sg_route_find_best_insertion_for_request(const SGContext *ctx, const SGRouteSolution *sol,
                                             uint32_t request_id, uint32_t forbidden_vehicle,
                                             uint32_t *best_vehicle_out,
                                             uint32_t *best_pos_out,
                                             uint32_t *best_pickup_pos_out,
                                             uint32_t *best_delivery_pos_out,
                                             double *best_route_distance_out);
int sg_route_find_best_insertion_no_new_vehicle(const SGContext *ctx, const SGRouteSolution *sol,
                                                uint32_t request_id,
                                                uint32_t empty_route_ok_vehicle,
                                                uint32_t *best_vehicle_out,
                                                uint32_t *best_pos_out,
                                                uint32_t *best_pickup_pos_out,
                                                uint32_t *best_delivery_pos_out,
                                                double *best_route_distance_out);
void sg_route_restore_from_backup(SGRouteSolution *sol, SGRouteSolution *backup);

/* sg_solve.c */
void sg_adaptive_q_bounds(int num_requests, int config_q_min, int config_q_max,
                          int *q_min_out, int *q_max_out);
int sg_route_solver_eligible(const SGContext *ctx);
ARStatus sg_route_construct_solomon_i1(SGContext *ctx, SGRouteSolution *sol);

#endif /* SURGE_SG_INTERNAL_H */
