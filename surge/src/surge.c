#include "surge.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "arbor.h"
#include "sh_dist.h"

#define SG_UNASSIGNED_PENALTY 10000.0
#define SG_WORST_RANDOMNESS 4.0
#define SG_SHAW_RANDOMNESS 4.0
#define SG_ROUTE_CLUSTER_RANDOMNESS 2.5
#define SG_TIME_CLUSTER_RANDOMNESS 2.0
#define SG_PD_SHAW_RANDOMNESS 3.0
#define SG_OPERATOR_SEED_XOR 0x9E3779B97F4A7C15ULL
#define SG_DEMAND_TOLERANCE 1e-9
#define SG_NOISE_REGRET_SCALE 0.1
#define SG_CONSTRUCT_REGRET_K 3
#define SG_CONSTRUCT_DISTANCE_SECONDS 1800.0
#define SG_CONSTRUCT_FALLBACK_SECONDS 600.0
#define SG_ROUTE_OBJECTIVE_VEHICLE_WEIGHT 1000000.0
#define SG_ROUTE_OBJECTIVE_UNASSIGNED_WEIGHT 1000000000.0
#define SG_ROUTE_MAX_REGRET_K 4
#define SG_ROUTE_MAX_INTENSIFY_PASSES 4

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
} SGRouteStop;

typedef struct {
    SGBootstrapSolution base;

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
} SGRouteSolution;

typedef struct {
    double *remaining_capacity;
    double *remaining_time_seconds;
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
    uint8_t has_location;
    uint8_t has_time_window;
} SGDepotRecord;

typedef struct {
    uint32_t start_depot_id;
    uint32_t end_depot_id;
    int32_t shift_early;
    int32_t shift_late;
    double *capacity;
    double *initial_load;
    uint8_t has_depots;
    uint8_t has_shift_time_window;
    uint8_t has_capacity;
    uint8_t has_initial_load;
} SGVehicleRecord;

typedef struct {
    SGTaskType type;
    double x;
    double y;
    int32_t tw_early;
    int32_t tw_late;
    int32_t service_seconds;
    double *demand;
    uint8_t has_location;
    uint8_t has_time_window;
    uint8_t has_demand;
} SGTaskRecord;

typedef struct {
    SGRequestKind kind;
    uint32_t pickup_task_id;
    uint32_t delivery_task_id;
    uint8_t has_pickup_task;
    uint8_t has_delivery_task;
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
    SHRng *op_rng;
};

static double sg_vehicle_request_cost(const SGContext *ctx, uint32_t vehicle_id,
                                      uint32_t request_id, double noise_scale);
static int sg_request_emit_stops(const SGContext *ctx, uint32_t request_id,
                                 SGRouteStop *stops_out, uint32_t *stop_count_out);
static int sg_route_stop_sequence_feasible(const SGContext *ctx, uint32_t vehicle_id,
                                           const SGRouteStop *stops, uint32_t stop_count,
                                           double *distance_out);

static SGRequestHint sg_request_hint_default(void) {
    SGRequestHint hint;

    memset(&hint, 0, sizeof(hint));
    hint.priority = 50;
    return hint;
}

static SGRequestRecord sg_request_record_default(void) {
    SGRequestRecord record;

    memset(&record, 0, sizeof(record));
    record.kind = SG_REQUEST_KIND_UNBOUND;
    return record;
}

static int sg_task_type_valid(SGTaskType type) {
    return (type == SG_TASK_PICKUP || type == SG_TASK_DELIVERY || type == SG_TASK_SERVICE);
}

static void sg_vehicle_records_free(SGVehicleRecord *vehicles, uint32_t count) {
    uint32_t i;

    if (!vehicles) {
        return;
    }
    for (i = 0; i < count; i++) {
        free(vehicles[i].capacity);
        vehicles[i].capacity = NULL;
        free(vehicles[i].initial_load);
        vehicles[i].initial_load = NULL;
    }
    free(vehicles);
}

static void sg_task_records_free(SGTaskRecord *tasks, uint32_t count) {
    uint32_t i;

    if (!tasks) {
        return;
    }
    for (i = 0; i < count; i++) {
        free(tasks[i].demand);
        tasks[i].demand = NULL;
    }
    free(tasks);
}

static void sg_bootstrap_solution_reset(SGBootstrapSolution *sol) {
    if (!sol) {
        return;
    }
    free(sol->assigned_ids);
    free(sol->unassigned_ids);
    free(sol->assigned_flags);
    memset(sol, 0, sizeof(*sol));
}

static ARStatus sg_bootstrap_solution_init(SGBootstrapSolution *sol,
                                           uint32_t total_requests) {
    uint32_t i;

    if (!sol) {
        return AR_STATUS_INVALID_ARG;
    }

    memset(sol, 0, sizeof(*sol));
    sol->total_requests = total_requests;
    sol->num_unassigned = total_requests;

    if (total_requests == 0) {
        return AR_STATUS_OK;
    }

    sol->assigned_ids = (uint32_t *)malloc((size_t)total_requests * sizeof(uint32_t));
    sol->unassigned_ids = (uint32_t *)malloc((size_t)total_requests * sizeof(uint32_t));
    sol->assigned_flags = (uint8_t *)calloc((size_t)total_requests, sizeof(uint8_t));

    if (!sol->assigned_ids || !sol->unassigned_ids || !sol->assigned_flags) {
        sg_bootstrap_solution_reset(sol);
        return AR_STATUS_OUT_OF_MEMORY;
    }

    for (i = 0; i < total_requests; i++) {
        sol->unassigned_ids[i] = i;
    }

    return AR_STATUS_OK;
}

static int sg_find_id(const uint32_t *ids, uint32_t count, uint32_t id) {
    uint32_t i;

    if (!ids) {
        return -1;
    }

    for (i = 0; i < count; i++) {
        if (ids[i] == id) {
            return (int)i;
        }
    }

    return -1;
}

static ARStatus sg_bootstrap_assign_request(SGBootstrapSolution *sol, uint32_t id) {
    int idx;

    if (!sol || id >= sol->total_requests) {
        return AR_STATUS_INVALID_ARG;
    }

    if (sol->assigned_flags[id]) {
        return AR_STATUS_OK;
    }

    idx = sg_find_id(sol->unassigned_ids, sol->num_unassigned, id);
    if (idx < 0) {
        return AR_STATUS_INVALID_ARG;
    }

    sol->num_unassigned--;
    sol->unassigned_ids[(uint32_t)idx] = sol->unassigned_ids[sol->num_unassigned];

    sol->assigned_ids[sol->num_assigned++] = id;
    sol->assigned_flags[id] = 1;

    return AR_STATUS_OK;
}

static ARStatus sg_bootstrap_unassign_request(SGBootstrapSolution *sol, uint32_t id) {
    int idx;

    if (!sol || id >= sol->total_requests) {
        return AR_STATUS_INVALID_ARG;
    }

    if (!sol->assigned_flags[id]) {
        return AR_STATUS_OK;
    }

    idx = sg_find_id(sol->assigned_ids, sol->num_assigned, id);
    if (idx < 0) {
        return AR_STATUS_INVALID_ARG;
    }

    sol->num_assigned--;
    sol->assigned_ids[(uint32_t)idx] = sol->assigned_ids[sol->num_assigned];

    sol->unassigned_ids[sol->num_unassigned++] = id;
    sol->assigned_flags[id] = 0;

    return AR_STATUS_OK;
}

static void *sg_bootstrap_copy(const void *solution, void *user_ctx) {
    const SGBootstrapSolution *src = (const SGBootstrapSolution *)solution;
    SGBootstrapSolution *dst;
    (void)user_ctx;

    if (!src) {
        return NULL;
    }

    dst = (SGBootstrapSolution *)calloc(1, sizeof(*dst));
    if (!dst) {
        return NULL;
    }

    dst->total_requests = src->total_requests;
    dst->num_assigned = src->num_assigned;
    dst->num_unassigned = src->num_unassigned;

    if (src->total_requests > 0) {
        size_t count = (size_t)src->total_requests;
        dst->assigned_ids = (uint32_t *)malloc(count * sizeof(uint32_t));
        dst->unassigned_ids = (uint32_t *)malloc(count * sizeof(uint32_t));
        dst->assigned_flags = (uint8_t *)malloc(count * sizeof(uint8_t));

        if (!dst->assigned_ids || !dst->unassigned_ids || !dst->assigned_flags) {
            sg_bootstrap_solution_reset(dst);
            free(dst);
            return NULL;
        }

        memcpy(dst->assigned_ids, src->assigned_ids, count * sizeof(uint32_t));
        memcpy(dst->unassigned_ids, src->unassigned_ids, count * sizeof(uint32_t));
        memcpy(dst->assigned_flags, src->assigned_flags, count * sizeof(uint8_t));
    }

    return dst;
}

static void sg_bootstrap_free(void *solution, void *user_ctx) {
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    (void)user_ctx;

    if (!sol) {
        return;
    }

    sg_bootstrap_solution_reset(sol);
    free(sol);
}

static double sg_bootstrap_cost(const void *solution, void *user_ctx) {
    const SGBootstrapSolution *sol = (const SGBootstrapSolution *)solution;
    const SGContext *ctx = (const SGContext *)user_ctx;
    uint32_t i;
    double assigned_proxy = 0.0;

    if (ctx->num_vehicles == 0 && ctx->num_requests > 0) {
        return 1e9;
    }

    for (i = 0; i < sol->num_assigned; i++) {
        uint32_t request_id = sol->assigned_ids[i];
        uint32_t v;
        double best = INFINITY;

        for (v = 0; v < ctx->num_vehicles; v++) {
            double score = sg_vehicle_request_cost(ctx, v, request_id, 0.0);
            if (score < best) {
                best = score;
            }
        }

        if (!isfinite(best)) {
            best = (double)request_id + 1.0;
        }
        assigned_proxy += best;
    }

    return (double)sol->num_unassigned * SG_UNASSIGNED_PENALTY + assigned_proxy;
}

static int sg_bootstrap_size(const void *solution, void *user_ctx) {
    const SGBootstrapSolution *sol = (const SGBootstrapSolution *)solution;
    (void)user_ctx;
    return (int)sol->num_assigned;
}

static int sg_bootstrap_validate(const void *solution, void *user_ctx) {
    const SGBootstrapSolution *sol = (const SGBootstrapSolution *)solution;
    const SGContext *ctx = (const SGContext *)user_ctx;
    uint32_t i;
    uint32_t assigned_flags = 0;

    if (!sol || !ctx) {
        return 0;
    }
    if (sol->total_requests != ctx->num_requests) {
        return 0;
    }
    if (sol->num_assigned + sol->num_unassigned != sol->total_requests) {
        return 0;
    }
    if (sol->total_requests == 0) {
        return 1;
    }
    if (!sol->assigned_ids || !sol->unassigned_ids || !sol->assigned_flags) {
        return 0;
    }

    for (i = 0; i < sol->num_assigned; i++) {
        uint32_t id = sol->assigned_ids[i];
        if (id >= sol->total_requests || !sol->assigned_flags[id]) {
            return 0;
        }
    }

    for (i = 0; i < sol->num_unassigned; i++) {
        uint32_t id = sol->unassigned_ids[i];
        if (id >= sol->total_requests || sol->assigned_flags[id]) {
            return 0;
        }
    }

    for (i = 0; i < sol->total_requests; i++) {
        if (sol->assigned_flags[i]) {
            assigned_flags++;
        }
    }

    return assigned_flags == sol->num_assigned;
}

static int sg_get_assigned_count(void *solution, void *user_ctx) {
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    (void)user_ctx;
    return sol ? (int)sol->num_assigned : 0;
}

static uint32_t sg_get_assigned_element(void *solution, void *user_ctx, int index) {
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    (void)user_ctx;

    if (!sol || index < 0 || (uint32_t)index >= sol->num_assigned) {
        return UINT32_MAX;
    }
    return sol->assigned_ids[(uint32_t)index];
}

static int32_t sg_clamp_priority(int32_t priority) {
    if (priority < 0) {
        return 0;
    }
    if (priority > 100) {
        return 100;
    }
    return priority;
}

static int64_t sg_abs_i64(int64_t value) {
    return value < 0 ? -value : value;
}

static int sg_priority_policy_valid(SGPriorityRemovalPolicy policy) {
    return (policy == SG_PRIORITY_REMOVE_LOWER_FIRST ||
            policy == SG_PRIORITY_REMOVE_HIGHER_FIRST);
}

static int sg_demand_sign_convention_valid(SGDemandSignConvention convention) {
    return (convention == SG_DEMAND_PICKUP_POSITIVE_DELIVERY_NEGATIVE ||
            convention == SG_DEMAND_PICKUP_NEGATIVE_DELIVERY_POSITIVE);
}

static int sg_config_valid(const SGConfig *config) {
    if (!config) {
        return 0;
    }
    if (config->max_iterations <= 0 || config->max_time_seconds < 0) {
        return 0;
    }
    if (config->segment_size <= 0 || config->q_min < 0 || config->q_max < config->q_min) {
        return 0;
    }
    return sg_priority_policy_valid(config->priority_removal_policy);
}

static void sg_zone_matrix_clear(SGContext *ctx) {
    if (!ctx) {
        return;
    }

    free(ctx->zone_distance_matrix);
    ctx->zone_distance_matrix = NULL;
    ctx->zone_count = 0;
}

static int sg_zone_distance_lookup(const SGContext *ctx, uint32_t zone_a, uint32_t zone_b,
                                   double *distance) {
    size_t stride;

    if (!ctx || !distance || !ctx->zone_distance_matrix || ctx->zone_count == 0) {
        return 0;
    }
    if (zone_a >= ctx->zone_count || zone_b >= ctx->zone_count) {
        return 0;
    }

    stride = (size_t)ctx->zone_count;
    *distance = ctx->zone_distance_matrix[(size_t)zone_a * stride + (size_t)zone_b];
    return 1;
}

static double sg_zone_similarity(const SGContext *ctx, uint32_t zone_a, uint32_t zone_b) {
    double distance = 0.0;

    if (sg_zone_distance_lookup(ctx, zone_a, zone_b, &distance)) {
        return 1.0 / (1.0 + distance);
    }
    return zone_a == zone_b ? 1.0 : 0.0;
}

static double sg_priority_removal_score(const SGContext *ctx, int32_t priority) {
    int32_t clamped = sg_clamp_priority(priority);

    if (!ctx || ctx->config.priority_removal_policy == SG_PRIORITY_REMOVE_LOWER_FIRST) {
        return (double)(100 - clamped);
    }
    return (double)clamped;
}

static const SGRequestHint *sg_get_hint(const SGContext *ctx, uint32_t request_id) {
    if (!ctx || !ctx->request_hints || request_id >= ctx->num_requests) {
        return NULL;
    }
    return &ctx->request_hints[request_id];
}

static const SGRequestRecord *sg_get_request_record(const SGContext *ctx, uint32_t request_id) {
    if (!ctx || !ctx->requests || request_id >= ctx->num_requests) {
        return NULL;
    }
    return &ctx->requests[request_id];
}

static const SGTaskRecord *sg_get_task_record(const SGContext *ctx, uint32_t task_id) {
    if (!ctx || !ctx->tasks || task_id >= ctx->num_tasks) {
        return NULL;
    }
    return &ctx->tasks[task_id];
}

static double sg_euclid(double ax, double ay, double bx, double by) {
    double dx = ax - bx;
    double dy = ay - by;
    return sqrt(dx * dx + dy * dy);
}

static int sg_request_time_midpoint(const SGContext *ctx, uint32_t request_id, int64_t *midpoint) {
    const SGRequestHint *hint = sg_get_hint(ctx, request_id);
    const SGRequestRecord *request = sg_get_request_record(ctx, request_id);

    if (!midpoint) {
        return 0;
    }

    if (hint && hint->has_time_window) {
        *midpoint = ((int64_t)hint->tw_early + (int64_t)hint->tw_late) / 2;
        return 1;
    }
    if (!request) {
        return 0;
    }

    if (request->kind == SG_REQUEST_KIND_DELIVERY_ONLY && request->has_delivery_task) {
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (delivery && delivery->has_time_window) {
            *midpoint = ((int64_t)delivery->tw_early + (int64_t)delivery->tw_late) / 2;
            return 1;
        }
    } else if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY &&
               request->has_pickup_task && request->has_delivery_task) {
        const SGTaskRecord *pickup = sg_get_task_record(ctx, request->pickup_task_id);
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (pickup && delivery && pickup->has_time_window && delivery->has_time_window) {
            int64_t pick_mid = ((int64_t)pickup->tw_early + (int64_t)pickup->tw_late) / 2;
            int64_t drop_mid = ((int64_t)delivery->tw_early + (int64_t)delivery->tw_late) / 2;
            *midpoint = (pick_mid + drop_mid) / 2;
            return 1;
        }
    }

    return 0;
}

static int sg_request_tw_width(const SGContext *ctx, uint32_t request_id, int32_t *width_out) {
    const SGRequestHint *hint = sg_get_hint(ctx, request_id);
    const SGRequestRecord *request = sg_get_request_record(ctx, request_id);

    if (!width_out) {
        return 0;
    }

    if (hint && hint->has_time_window && hint->tw_late >= hint->tw_early) {
        *width_out = hint->tw_late - hint->tw_early;
        return 1;
    }
    if (!request) {
        return 0;
    }

    if (request->kind == SG_REQUEST_KIND_DELIVERY_ONLY && request->has_delivery_task) {
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (delivery && delivery->has_time_window && delivery->tw_late >= delivery->tw_early) {
            *width_out = delivery->tw_late - delivery->tw_early;
            return 1;
        }
    } else if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY &&
               request->has_pickup_task && request->has_delivery_task) {
        const SGTaskRecord *pickup = sg_get_task_record(ctx, request->pickup_task_id);
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (pickup && delivery && pickup->has_time_window && delivery->has_time_window) {
            int32_t early = pickup->tw_early < delivery->tw_early ? pickup->tw_early : delivery->tw_early;
            int32_t late = pickup->tw_late > delivery->tw_late ? pickup->tw_late : delivery->tw_late;
            if (late >= early) {
                *width_out = late - early;
                return 1;
            }
        }
    }

    return 0;
}

static int sg_request_time_window_bounds(const SGContext *ctx, uint32_t request_id,
                                         int32_t *early_out, int32_t *late_out) {
    const SGRequestHint *hint = sg_get_hint(ctx, request_id);
    const SGRequestRecord *request = sg_get_request_record(ctx, request_id);

    if (!early_out || !late_out) {
        return 0;
    }

    if (hint && hint->has_time_window && hint->tw_late >= hint->tw_early) {
        *early_out = hint->tw_early;
        *late_out = hint->tw_late;
        return 1;
    }
    if (!request) {
        return 0;
    }

    if (request->kind == SG_REQUEST_KIND_DELIVERY_ONLY && request->has_delivery_task) {
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (delivery && delivery->has_time_window && delivery->tw_late >= delivery->tw_early) {
            *early_out = delivery->tw_early;
            *late_out = delivery->tw_late;
            return 1;
        }
    } else if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY &&
               request->has_pickup_task && request->has_delivery_task) {
        const SGTaskRecord *pickup = sg_get_task_record(ctx, request->pickup_task_id);
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (pickup && delivery && pickup->has_time_window && delivery->has_time_window) {
            *early_out = pickup->tw_early < delivery->tw_early
                       ? pickup->tw_early
                       : delivery->tw_early;
            *late_out = pickup->tw_late > delivery->tw_late
                      ? pickup->tw_late
                      : delivery->tw_late;
            if (*late_out >= *early_out) {
                return 1;
            }
        }
    }

    return 0;
}

static int sg_request_centroid(const SGContext *ctx, uint32_t request_id, double *x, double *y) {
    const SGRequestRecord *request = sg_get_request_record(ctx, request_id);

    if (!x || !y || !request) {
        return 0;
    }

    if (request->kind == SG_REQUEST_KIND_DELIVERY_ONLY && request->has_delivery_task) {
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (delivery && delivery->has_location) {
            *x = delivery->x;
            *y = delivery->y;
            return 1;
        }
    } else if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY &&
               request->has_pickup_task && request->has_delivery_task) {
        const SGTaskRecord *pickup = sg_get_task_record(ctx, request->pickup_task_id);
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (pickup && delivery && pickup->has_location && delivery->has_location) {
            *x = (pickup->x + delivery->x) * 0.5;
            *y = (pickup->y + delivery->y) * 0.5;
            return 1;
        }
    }

    return 0;
}

static double sg_request_load_magnitude(const SGContext *ctx, uint32_t request_id) {
    const SGRequestRecord *request = sg_get_request_record(ctx, request_id);
    uint32_t d;
    double total = 0.0;

    if (!request) {
        return 0.0;
    }

    if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY &&
        request->has_pickup_task && request->has_delivery_task) {
        const SGTaskRecord *pickup = sg_get_task_record(ctx, request->pickup_task_id);
        if (pickup && pickup->has_demand && pickup->demand) {
            for (d = 0; d < ctx->dimension_count; d++) {
                total += fabs(pickup->demand[d]);
            }
        }
    } else if (request->kind == SG_REQUEST_KIND_DELIVERY_ONLY &&
               request->has_delivery_task) {
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (delivery && delivery->has_demand && delivery->demand) {
            for (d = 0; d < ctx->dimension_count; d++) {
                total += fabs(delivery->demand[d]);
            }
        }
    }

    return total;
}

static double sg_request_priority_score(const SGContext *ctx, uint32_t request_id) {
    const SGRequestHint *hint = sg_get_hint(ctx, request_id);
    if (!hint) {
        return 50.0;
    }
    return (double)sg_clamp_priority(hint->priority);
}

static SGRequestKind sg_request_kind(const SGContext *ctx, uint32_t request_id) {
    const SGRequestRecord *request = sg_get_request_record(ctx, request_id);
    return request ? request->kind : SG_REQUEST_KIND_UNBOUND;
}

static double sg_request_abs_demand_at_dim(const SGContext *ctx, uint32_t request_id, uint32_t dim) {
    const SGRequestRecord *request = sg_get_request_record(ctx, request_id);

    if (!ctx || !request || dim >= ctx->dimension_count) {
        return 0.0;
    }

    if (request->kind == SG_REQUEST_KIND_DELIVERY_ONLY && request->has_delivery_task) {
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (delivery && delivery->has_demand && delivery->demand) {
            return fabs(delivery->demand[dim]);
        }
    }

    if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY &&
        request->has_pickup_task && request->has_delivery_task) {
        const SGTaskRecord *pickup = sg_get_task_record(ctx, request->pickup_task_id);
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (pickup && pickup->has_demand && pickup->demand) {
            return fabs(pickup->demand[dim]);
        }
        if (delivery && delivery->has_demand && delivery->demand) {
            return fabs(delivery->demand[dim]);
        }
    }

    return 0.0;
}

static int sg_vehicle_start_end_locations(const SGContext *ctx, uint32_t vehicle_id,
                                          double *sx, double *sy, double *ex, double *ey) {
    const SGVehicleRecord *vehicle;
    const SGDepotRecord *start;
    const SGDepotRecord *end;

    if (!ctx || vehicle_id >= ctx->num_vehicles || !sx || !sy || !ex || !ey) {
        return 0;
    }

    vehicle = &ctx->vehicles[vehicle_id];
    if (!vehicle->has_depots || vehicle->start_depot_id >= ctx->num_depots ||
        vehicle->end_depot_id >= ctx->num_depots) {
        return 0;
    }

    start = &ctx->depots[vehicle->start_depot_id];
    end = &ctx->depots[vehicle->end_depot_id];
    if (!start->has_location || !end->has_location) {
        return 0;
    }

    *sx = start->x;
    *sy = start->y;
    *ex = end->x;
    *ey = end->y;
    return 1;
}

static int sg_route_sequence_feasible_distance(const SGContext *ctx, uint32_t vehicle_id,
                                               const uint32_t *request_ids, uint32_t request_count,
                                               double *distance_out, double *capacity_scratch) {
    SGRouteStop *stops = NULL;
    uint32_t stop_count = 0;
    uint32_t i;
    int ok = 0;

    (void)capacity_scratch;

    if (!ctx || !distance_out || vehicle_id >= ctx->num_vehicles ||
        (request_count > 0 && !request_ids)) {
        return 0;
    }

    if (request_count == 0) {
        *distance_out = 0.0;
        return 1;
    }

    if (ctx->num_requests > UINT32_MAX / 2U || request_count > UINT32_MAX / 2U) {
        return 0;
    }
    stops = (SGRouteStop *)malloc((size_t)request_count * 2U * sizeof(SGRouteStop));
    if (!stops) {
        return 0;
    }

    for (i = 0; i < request_count; i++) {
        SGRouteStop emitted[2];
        uint32_t emitted_count = 0;
        uint32_t e;
        if (!sg_request_emit_stops(ctx, request_ids[i], emitted, &emitted_count)) {
            goto done;
        }
        for (e = 0; e < emitted_count; e++) {
            stops[stop_count++] = emitted[e];
        }
    }

    ok = sg_route_stop_sequence_feasible(ctx, vehicle_id, stops, stop_count, distance_out);

done:
    free(stops);
    return ok;
}

static void sg_route_insert_request(uint32_t *route, uint32_t *route_len, uint32_t insert_pos,
                                    uint32_t request_id) {
    uint32_t len;

    if (!route || !route_len || insert_pos > *route_len) {
        return;
    }

    len = *route_len;
    if (insert_pos < len) {
        memmove(&route[insert_pos + 1], &route[insert_pos],
                (size_t)(len - insert_pos) * sizeof(uint32_t));
    }
    route[insert_pos] = request_id;
    *route_len = len + 1;
}

static int sg_compute_delivery_only_route_metrics(const SGContext *ctx,
                                                  const SGBootstrapSolution *sol,
                                                  uint32_t *vehicles_used_out,
                                                  double *total_distance_out) {
    uint32_t *remaining = NULL;
    uint32_t *routes = NULL;
    uint32_t *route_lens = NULL;
    uint32_t *candidate = NULL;
    double *route_distances = NULL;
    double *capacity_scratch = NULL;
    uint32_t remaining_count;
    size_t route_stride;
    uint32_t v;
    uint32_t used_vehicles = 0;
    double total_distance = 0.0;
    int ok = 0;

    if (!ctx || !sol || !vehicles_used_out || !total_distance_out ||
        ctx->num_vehicles == 0 || ctx->num_requests == 0) {
        return 0;
    }
    if (sol->num_assigned == 0) {
        *vehicles_used_out = 0;
        *total_distance_out = 0.0;
        return 1;
    }

    route_stride = (size_t)sol->num_assigned;
    if (route_stride == 0 || (size_t)ctx->num_vehicles > SIZE_MAX / route_stride) {
        return 0;
    }

    remaining = (uint32_t *)malloc((size_t)sol->num_assigned * sizeof(uint32_t));
    routes = (uint32_t *)malloc((size_t)ctx->num_vehicles * route_stride * sizeof(uint32_t));
    route_lens = (uint32_t *)calloc((size_t)ctx->num_vehicles, sizeof(uint32_t));
    route_distances = (double *)calloc((size_t)ctx->num_vehicles, sizeof(double));
    candidate = (uint32_t *)malloc((route_stride + 1U) * sizeof(uint32_t));
    capacity_scratch = (double *)malloc((size_t)ctx->dimension_count * sizeof(double));

    if (!remaining || !routes || !route_lens || !route_distances || !candidate ||
        !capacity_scratch) {
        goto done;
    }

    memcpy(remaining, sol->assigned_ids, (size_t)sol->num_assigned * sizeof(uint32_t));
    remaining_count = sol->num_assigned;

    while (remaining_count > 0) {
        int selected_idx = -1;
        uint32_t selected_vehicle = UINT32_MAX;
        uint32_t selected_pos = 0;
        uint32_t selected_request = UINT32_MAX;
        uint32_t selected_feasible_count = UINT32_MAX;
        double selected_route_distance = 0.0;
        double selected_delta = INFINITY;
        uint32_t ri;

        for (ri = 0; ri < remaining_count; ri++) {
            uint32_t request_id = remaining[ri];
            uint32_t req_feasible_count = 0;
            uint32_t req_best_vehicle = UINT32_MAX;
            uint32_t req_best_pos = 0;
            double req_best_route_distance = 0.0;
            double req_best_delta = INFINITY;

            for (v = 0; v < ctx->num_vehicles; v++) {
                uint32_t *route = &routes[(size_t)v * route_stride];
                uint32_t len = route_lens[v];
                uint32_t pos;

                for (pos = 0; pos <= len; pos++) {
                    double candidate_distance = 0.0;
                    double delta;

                    if (pos > 0) {
                        memcpy(candidate, route, (size_t)pos * sizeof(uint32_t));
                    }
                    candidate[pos] = request_id;
                    if (pos < len) {
                        memcpy(&candidate[pos + 1], &route[pos],
                               (size_t)(len - pos) * sizeof(uint32_t));
                    }

                    if (!sg_route_sequence_feasible_distance(ctx, v, candidate, len + 1,
                                                             &candidate_distance,
                                                             capacity_scratch)) {
                        continue;
                    }

                    req_feasible_count++;
                    delta = candidate_distance - route_distances[v];
                    if (delta < req_best_delta - 1e-9 ||
                        (fabs(delta - req_best_delta) <= 1e-9 &&
                         (v < req_best_vehicle ||
                          (v == req_best_vehicle && pos < req_best_pos)))) {
                        req_best_delta = delta;
                        req_best_vehicle = v;
                        req_best_pos = pos;
                        req_best_route_distance = candidate_distance;
                    }
                }
            }

            if (req_feasible_count == 0) {
                continue;
            }

            if (req_feasible_count < selected_feasible_count ||
                (req_feasible_count == selected_feasible_count &&
                 req_best_delta < selected_delta - 1e-9) ||
                (req_feasible_count == selected_feasible_count &&
                 fabs(req_best_delta - selected_delta) <= 1e-9 &&
                 request_id < selected_request)) {
                selected_idx = (int)ri;
                selected_request = request_id;
                selected_vehicle = req_best_vehicle;
                selected_pos = req_best_pos;
                selected_route_distance = req_best_route_distance;
                selected_feasible_count = req_feasible_count;
                selected_delta = req_best_delta;
            }
        }

        if (selected_idx < 0 || selected_vehicle == UINT32_MAX || selected_request == UINT32_MAX) {
            goto done;
        }

        {
            uint32_t *route = &routes[(size_t)selected_vehicle * route_stride];
            uint32_t len = route_lens[selected_vehicle];
            sg_route_insert_request(route, &len, selected_pos, selected_request);
            route_lens[selected_vehicle] = len;
            route_distances[selected_vehicle] = selected_route_distance;
        }

        remaining_count--;
        remaining[(uint32_t)selected_idx] = remaining[remaining_count];
    }

    for (v = 0; v < ctx->num_vehicles; v++) {
        if (route_lens[v] > 0) {
            used_vehicles++;
            total_distance += route_distances[v];
        }
    }

    *vehicles_used_out = used_vehicles;
    *total_distance_out = total_distance;
    ok = 1;

done:
    free(remaining);
    free(routes);
    free(route_lens);
    free(route_distances);
    free(candidate);
    free(capacity_scratch);
    return ok;
}

static void sg_compute_solution_route_metrics(const SGContext *ctx, const SGBootstrapSolution *sol,
                                              uint32_t *vehicles_used_out,
                                              double *total_distance_out) {
    uint32_t vehicles_used = 0;
    double total_distance = 0.0;
    uint32_t i;

    if (!vehicles_used_out || !total_distance_out) {
        return;
    }

    if (!ctx || !sol) {
        *vehicles_used_out = 0;
        *total_distance_out = 0.0;
        return;
    }

    if (sg_compute_delivery_only_route_metrics(ctx, sol, &vehicles_used, &total_distance)) {
        *vehicles_used_out = vehicles_used;
        *total_distance_out = total_distance;
        return;
    }

    if (ctx->num_vehicles > 0) {
        vehicles_used = sol->num_assigned < ctx->num_vehicles ? sol->num_assigned : ctx->num_vehicles;
    }
    for (i = 0; i < sol->num_assigned; i++) {
        uint32_t request_id = sol->assigned_ids[i];
        uint32_t v;
        double rx = 0.0;
        double ry = 0.0;
        double best_leg = INFINITY;

        if (!sg_request_centroid(ctx, request_id, &rx, &ry)) {
            continue;
        }

        for (v = 0; v < ctx->num_vehicles; v++) {
            double sx = 0.0;
            double sy = 0.0;
            double ex = 0.0;
            double ey = 0.0;
            double leg;

            if (!sg_vehicle_start_end_locations(ctx, v, &sx, &sy, &ex, &ey)) {
                continue;
            }
            leg = sg_euclid(sx, sy, rx, ry) + sg_euclid(rx, ry, ex, ey);
            if (leg < best_leg) {
                best_leg = leg;
            }
        }

        if (isfinite(best_leg)) {
            total_distance += best_leg;
        }
    }

    *vehicles_used_out = vehicles_used;
    *total_distance_out = total_distance;
}

static int sg_request_time_use_for_vehicle(const SGContext *ctx, uint32_t vehicle_id,
                                           uint32_t request_id, double *time_use_seconds) {
    const SGRequestRecord *request = sg_get_request_record(ctx, request_id);
    const SGVehicleRecord *vehicle;
    double sx = 0.0;
    double sy = 0.0;
    double ex = 0.0;
    double ey = 0.0;
    int has_depot_locations = 0;
    double shift_early;
    double shift_late;
    double time_factor = SG_CONSTRUCT_DISTANCE_SECONDS;
    double consumed;

    if (!ctx || !time_use_seconds || vehicle_id >= ctx->num_vehicles) {
        return 0;
    }
    vehicle = &ctx->vehicles[vehicle_id];

    shift_early = vehicle->has_shift_time_window ? (double)vehicle->shift_early : 0.0;
    shift_late = vehicle->has_shift_time_window ? (double)vehicle->shift_late : INFINITY;
    has_depot_locations = sg_vehicle_start_end_locations(ctx, vehicle_id, &sx, &sy, &ex, &ey);

    if (!request || request->kind == SG_REQUEST_KIND_UNBOUND) {
        *time_use_seconds = SG_CONSTRUCT_FALLBACK_SECONDS;
        return 1;
    }

    if (request->kind == SG_REQUEST_KIND_DELIVERY_ONLY && request->has_delivery_task) {
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        double d1;
        double d2;
        double arrival;
        double service_start;
        double finish;
        double wait = 0.0;

        if (!delivery || !delivery->has_location) {
            *time_use_seconds = SG_CONSTRUCT_FALLBACK_SECONDS;
            return 1;
        }

        d1 = has_depot_locations ? sg_euclid(sx, sy, delivery->x, delivery->y) : 1.0;
        d2 = has_depot_locations ? sg_euclid(delivery->x, delivery->y, ex, ey) : 1.0;
        arrival = shift_early + d1 * time_factor;
        service_start = arrival;

        if (delivery->has_time_window) {
            if (service_start < (double)delivery->tw_early) {
                wait = (double)delivery->tw_early - service_start;
                service_start = (double)delivery->tw_early;
            }
            if (service_start > (double)delivery->tw_late) {
                return 0;
            }
        }

        finish = service_start + (double)delivery->service_seconds + d2 * time_factor;
        if (finish > shift_late + 1e-9) {
            return 0;
        }

        consumed = d1 * time_factor + wait + (double)delivery->service_seconds + d2 * time_factor;
        if (!isfinite(consumed) || consumed < 0.0) {
            return 0;
        }
        *time_use_seconds = consumed;
        return 1;
    }

    if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY &&
        request->has_pickup_task && request->has_delivery_task) {
        const SGTaskRecord *pickup = sg_get_task_record(ctx, request->pickup_task_id);
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        double d_start_pick;
        double d_pick_drop;
        double d_drop_end;
        double arrive_pick;
        double start_pick;
        double leave_pick;
        double arrive_drop;
        double start_drop;
        double finish;
        double wait_pick = 0.0;
        double wait_drop = 0.0;

        if (!pickup || !delivery || !pickup->has_location || !delivery->has_location) {
            *time_use_seconds = SG_CONSTRUCT_FALLBACK_SECONDS * 2.0;
            return 1;
        }

        d_start_pick = has_depot_locations ? sg_euclid(sx, sy, pickup->x, pickup->y) : 1.0;
        d_pick_drop = sg_euclid(pickup->x, pickup->y, delivery->x, delivery->y);
        d_drop_end = has_depot_locations ? sg_euclid(delivery->x, delivery->y, ex, ey) : 1.0;

        arrive_pick = shift_early + d_start_pick * time_factor;
        start_pick = arrive_pick;
        if (pickup->has_time_window) {
            if (start_pick < (double)pickup->tw_early) {
                wait_pick = (double)pickup->tw_early - start_pick;
                start_pick = (double)pickup->tw_early;
            }
            if (start_pick > (double)pickup->tw_late) {
                return 0;
            }
        }
        leave_pick = start_pick + (double)pickup->service_seconds;
        arrive_drop = leave_pick + d_pick_drop * time_factor;
        start_drop = arrive_drop;
        if (delivery->has_time_window) {
            if (start_drop < (double)delivery->tw_early) {
                wait_drop = (double)delivery->tw_early - start_drop;
                start_drop = (double)delivery->tw_early;
            }
            if (start_drop > (double)delivery->tw_late) {
                return 0;
            }
        }

        finish = start_drop + (double)delivery->service_seconds + d_drop_end * time_factor;
        if (finish > shift_late + 1e-9) {
            return 0;
        }

        consumed = d_start_pick * time_factor + wait_pick + (double)pickup->service_seconds +
                   d_pick_drop * time_factor + wait_drop +
                   (double)delivery->service_seconds + d_drop_end * time_factor;
        if (!isfinite(consumed) || consumed < 0.0) {
            return 0;
        }
        *time_use_seconds = consumed;
        return 1;
    }

    *time_use_seconds = SG_CONSTRUCT_FALLBACK_SECONDS;
    return 1;
}

static ARStatus sg_construct_state_init(const SGContext *ctx, SGConstructState *state) {
    size_t total_caps;
    uint32_t v;
    uint32_t d;

    if (!ctx || !state) {
        return AR_STATUS_INVALID_ARG;
    }
    memset(state, 0, sizeof(*state));

    if (ctx->num_vehicles == 0 || ctx->dimension_count == 0) {
        return AR_STATUS_OK;
    }

    if ((size_t)ctx->num_vehicles > SIZE_MAX / (size_t)ctx->dimension_count) {
        return AR_STATUS_OUT_OF_MEMORY;
    }
    total_caps = (size_t)ctx->num_vehicles * (size_t)ctx->dimension_count;

    state->remaining_capacity = (double *)malloc(total_caps * sizeof(double));
    state->remaining_time_seconds = (double *)malloc((size_t)ctx->num_vehicles * sizeof(double));
    if (!state->remaining_capacity || !state->remaining_time_seconds) {
        free(state->remaining_capacity);
        free(state->remaining_time_seconds);
        memset(state, 0, sizeof(*state));
        return AR_STATUS_OUT_OF_MEMORY;
    }

    for (v = 0; v < ctx->num_vehicles; v++) {
        const SGVehicleRecord *vehicle = &ctx->vehicles[v];
        state->remaining_time_seconds[v] = vehicle->has_shift_time_window
                                           ? (double)(vehicle->shift_late - vehicle->shift_early)
                                           : INFINITY;

        for (d = 0; d < ctx->dimension_count; d++) {
            double cap = (vehicle->has_capacity && vehicle->capacity)
                         ? vehicle->capacity[d]
                         : INFINITY;
            double init = (vehicle->has_initial_load && vehicle->initial_load)
                          ? vehicle->initial_load[d] : 0.0;
            state->remaining_capacity[(size_t)v * (size_t)ctx->dimension_count + (size_t)d] = cap - init;
        }
    }

    return AR_STATUS_OK;
}

static void sg_construct_state_reset(SGConstructState *state) {
    if (!state) {
        return;
    }
    free(state->remaining_capacity);
    free(state->remaining_time_seconds);
    memset(state, 0, sizeof(*state));
}

static int sg_construct_eval_vehicle_request(const SGContext *ctx, const SGConstructState *state,
                                             uint32_t vehicle_id, uint32_t request_id,
                                             double *score_out, double *time_use_out) {
    uint32_t d;
    double time_use = 0.0;
    double score;

    if (!ctx || !state || vehicle_id >= ctx->num_vehicles || !score_out) {
        return 0;
    }
    if (!state->remaining_capacity || !state->remaining_time_seconds) {
        return 0;
    }

    if (!sg_request_time_use_for_vehicle(ctx, vehicle_id, request_id, &time_use)) {
        return 0;
    }
    if (time_use > state->remaining_time_seconds[vehicle_id] + 1e-9) {
        return 0;
    }

    for (d = 0; d < ctx->dimension_count; d++) {
        double req_demand = sg_request_abs_demand_at_dim(ctx, request_id, d);
        double remaining = state->remaining_capacity[(size_t)vehicle_id *
                                                     (size_t)ctx->dimension_count + (size_t)d];
        if (req_demand > remaining + SG_DEMAND_TOLERANCE) {
            return 0;
        }
    }

    score = sg_vehicle_request_cost(ctx, vehicle_id, request_id, 0.0);
    score += time_use / 3600.0;
    *score_out = score;
    if (time_use_out) {
        *time_use_out = time_use;
    }
    return 1;
}

static ARStatus sg_construct_assign_request(SGContext *ctx, SGConstructState *state,
                                            SGBootstrapSolution *sol, uint32_t request_id,
                                            uint32_t vehicle_id) {
    uint32_t d;
    double time_use = 0.0;
    double ignored = 0.0;
    ARStatus status;

    if (!ctx || !state || !sol || vehicle_id >= ctx->num_vehicles) {
        return AR_STATUS_INVALID_ARG;
    }
    if (!sg_construct_eval_vehicle_request(ctx, state, vehicle_id, request_id,
                                           &ignored, &time_use)) {
        return AR_STATUS_INVALID_ARG;
    }

    for (d = 0; d < ctx->dimension_count; d++) {
        size_t idx = (size_t)vehicle_id * (size_t)ctx->dimension_count + (size_t)d;
        state->remaining_capacity[idx] -= sg_request_abs_demand_at_dim(ctx, request_id, d);
        if (state->remaining_capacity[idx] < 0.0 &&
            state->remaining_capacity[idx] > -SG_DEMAND_TOLERANCE) {
            state->remaining_capacity[idx] = 0.0;
        }
    }
    state->remaining_time_seconds[vehicle_id] -= time_use;
    if (state->remaining_time_seconds[vehicle_id] < 0.0 &&
        state->remaining_time_seconds[vehicle_id] > -1e-9) {
        state->remaining_time_seconds[vehicle_id] = 0.0;
    }

    status = sg_bootstrap_assign_request(sol, request_id);
    return status;
}

static int sg_construct_select_regret_request(const SGContext *ctx, const SGConstructState *state,
                                              const SGBootstrapSolution *sol, int regret_k,
                                              uint32_t *request_id_out,
                                              uint32_t *vehicle_id_out) {
    uint32_t request_id;
    uint32_t best_request = UINT32_MAX;
    uint32_t best_vehicle = UINT32_MAX;
    double best_regret = -INFINITY;
    double best_first_cost = INFINITY;

    if (!ctx || !state || !sol || !request_id_out || !vehicle_id_out || regret_k < 1) {
        return 0;
    }

    for (request_id = 0; request_id < ctx->num_requests; request_id++) {
        uint32_t v;
        double ranked_costs[SG_CONSTRUCT_REGRET_K];
        uint32_t ranked_vehicles[SG_CONSTRUCT_REGRET_K];
        int ranked_count = 0;

        if (request_id >= sol->total_requests || sol->assigned_flags[request_id]) {
            continue;
        }

        for (v = 0; v < ctx->num_vehicles; v++) {
            double score = 0.0;
            if (sg_construct_eval_vehicle_request(ctx, state, v, request_id, &score, NULL)) {
                int insert_at = ranked_count;
                int j;

                if (insert_at > SG_CONSTRUCT_REGRET_K) {
                    insert_at = SG_CONSTRUCT_REGRET_K;
                }
                for (j = 0; j < ranked_count && j < SG_CONSTRUCT_REGRET_K; j++) {
                    if (score < ranked_costs[j]) {
                        insert_at = j;
                        break;
                    }
                }

                if (insert_at < SG_CONSTRUCT_REGRET_K) {
                    int limit = ranked_count < SG_CONSTRUCT_REGRET_K
                                ? ranked_count
                                : SG_CONSTRUCT_REGRET_K - 1;
                    for (j = limit; j > insert_at; j--) {
                        ranked_costs[j] = ranked_costs[j - 1];
                        ranked_vehicles[j] = ranked_vehicles[j - 1];
                    }
                    ranked_costs[insert_at] = score;
                    ranked_vehicles[insert_at] = v;
                }

                if (ranked_count < SG_CONSTRUCT_REGRET_K) {
                    ranked_count++;
                }
            }
        }

        if (ranked_count == 0) {
            continue;
        }

        {
            int k_index = regret_k - 1;
            double first_cost = ranked_costs[0];
            double kth_cost;
            double regret;
            if (k_index >= ranked_count) {
                k_index = ranked_count - 1;
            }
            kth_cost = ranked_costs[k_index];
            regret = kth_cost - first_cost;

            if (regret > best_regret ||
                (fabs(regret - best_regret) <= 1e-9 && first_cost < best_first_cost) ||
                (fabs(regret - best_regret) <= 1e-9 &&
                 fabs(first_cost - best_first_cost) <= 1e-9 &&
                 request_id < best_request)) {
                best_regret = regret;
                best_first_cost = first_cost;
                best_request = request_id;
                best_vehicle = ranked_vehicles[0];
            }
        }
    }

    if (best_request == UINT32_MAX || best_vehicle == UINT32_MAX) {
        return 0;
    }

    *request_id_out = best_request;
    *vehicle_id_out = best_vehicle;
    return 1;
}

static ARStatus sg_construct_initial_solution(SGContext *ctx, SGBootstrapSolution *sol) {
    SGConstructState state;
    ARStatus status;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }

    status = sg_construct_state_init(ctx, &state);
    if (status != AR_STATUS_OK) {
        return status;
    }

    while (sol->num_unassigned > 0) {
        uint32_t request_id = UINT32_MAX;
        uint32_t vehicle_id = UINT32_MAX;
        if (!sg_construct_select_regret_request(ctx, &state, sol, SG_CONSTRUCT_REGRET_K,
                                                &request_id, &vehicle_id)) {
            break;
        }

        status = sg_construct_assign_request(ctx, &state, sol, request_id, vehicle_id);
        if (status != AR_STATUS_OK) {
            sg_construct_state_reset(&state);
            return status;
        }
    }

    sg_construct_state_reset(&state);
    return AR_STATUS_OK;
}

static double sg_zone_density_score(const SGContext *ctx,
                                    const SGBootstrapSolution *sol,
                                    uint32_t request_id) {
    const SGRequestHint *target_hint = sg_get_hint(ctx, request_id);
    uint32_t i;
    double density = 0.0;

    if (!target_hint || !target_hint->has_zone || !sol) {
        return 0.0;
    }

    for (i = 0; i < sol->num_assigned; i++) {
        const SGRequestHint *hint = sg_get_hint(ctx, sol->assigned_ids[i]);
        if (hint && hint->has_zone) {
            density += sg_zone_similarity(ctx, target_hint->zone_id, hint->zone_id);
        }
    }

    if (density <= 1.0) {
        return 0.0;
    }
    return (density - 1.0) * 0.25;
}

static double sg_bootstrap_removal_cost(void *ctx, void *solution, uint32_t element_id) {
    const SGContext *sg_ctx = (const SGContext *)ctx;
    const SGBootstrapSolution *sol = (const SGBootstrapSolution *)solution;
    const SGRequestHint *hint = sg_get_hint(sg_ctx, element_id);
    double score = 0.0;

    if (!hint) {
        return (double)element_id + 1.0;
    }

    /* Priority contribution is configurable to fit tenant policy. */
    score += sg_priority_removal_score(sg_ctx, hint->priority) * 0.5;

    /* Wider windows are easier to reinsert, so removal score increases. */
    if (hint->has_time_window && hint->tw_late > hint->tw_early) {
        int32_t width = hint->tw_late - hint->tw_early;
        score += ((double)width / 3600.0) * 0.5;
    }

    /* Over-represented zones are slightly preferred for removal. */
    score += sg_zone_density_score(sg_ctx, sol, element_id);

    /* Stable tie-breaker. */
    score += ((double)element_id + 1.0) * 0.0001;

    return score;
}

static double sg_bootstrap_relatedness(void *ctx, uint32_t a, uint32_t b) {
    const SGContext *sg_ctx = (const SGContext *)ctx;
    const SGRequestHint *ha = sg_get_hint(sg_ctx, a);
    const SGRequestHint *hb = sg_get_hint(sg_ctx, b);
    double score = 0.0;

    if (!ha || !hb) {
        uint32_t diff = (a > b) ? (a - b) : (b - a);
        return -(double)diff;
    }

    if (ha->has_zone && hb->has_zone) {
        double similarity = sg_zone_similarity(sg_ctx, ha->zone_id, hb->zone_id);
        score += 12.0 * similarity - 2.0;
    }

    if (ha->has_time_window && hb->has_time_window) {
        int64_t mid_a = ((int64_t)ha->tw_early + (int64_t)ha->tw_late) / 2;
        int64_t mid_b = ((int64_t)hb->tw_early + (int64_t)hb->tw_late) / 2;
        int64_t delta = sg_abs_i64(mid_a - mid_b);
        score -= ((double)delta / 3600.0);
    }

    score -= (double)sg_abs_i64((int64_t)ha->priority - (int64_t)hb->priority) / 20.0;
    score += 1.0 / (1.0 + (double)sg_abs_i64((int64_t)a - (int64_t)b));

    return score;
}

static double sg_route_cluster_relatedness(void *ctx, uint32_t a, uint32_t b) {
    const SGContext *sg_ctx = (const SGContext *)ctx;
    double score = 0.0;
    double ax, ay, bx, by;
    int64_t ta, tb;
    const SGRequestHint *ha = sg_get_hint(sg_ctx, a);
    const SGRequestHint *hb = sg_get_hint(sg_ctx, b);

    if (sg_request_centroid(sg_ctx, a, &ax, &ay) && sg_request_centroid(sg_ctx, b, &bx, &by)) {
        score -= sg_euclid(ax, ay, bx, by);
    }

    if (ha && hb && ha->has_zone && hb->has_zone) {
        score += 8.0 * sg_zone_similarity(sg_ctx, ha->zone_id, hb->zone_id);
    }

    if (sg_request_time_midpoint(sg_ctx, a, &ta) && sg_request_time_midpoint(sg_ctx, b, &tb)) {
        score -= (double)sg_abs_i64(ta - tb) / 3600.0;
    }

    if (sg_request_kind(sg_ctx, a) == sg_request_kind(sg_ctx, b)) {
        score += 1.0;
    }

    score += 1.0 / (1.0 + (double)sg_abs_i64((int64_t)a - (int64_t)b));
    return score;
}

static double sg_time_cluster_relatedness(void *ctx, uint32_t a, uint32_t b) {
    const SGContext *sg_ctx = (const SGContext *)ctx;
    int64_t ta, tb;
    const SGRequestHint *ha = sg_get_hint(sg_ctx, a);
    const SGRequestHint *hb = sg_get_hint(sg_ctx, b);
    double score = 0.0;

    if (sg_request_time_midpoint(sg_ctx, a, &ta) && sg_request_time_midpoint(sg_ctx, b, &tb)) {
        score -= (double)sg_abs_i64(ta - tb) / 1800.0;
    } else {
        score -= (double)sg_abs_i64((int64_t)a - (int64_t)b);
    }

    if (ha && hb && ha->has_zone && hb->has_zone) {
        score += 3.0 * sg_zone_similarity(sg_ctx, ha->zone_id, hb->zone_id);
    }

    score -= fabs(sg_request_priority_score(sg_ctx, a) -
                  sg_request_priority_score(sg_ctx, b)) / 30.0;
    return score;
}

static double sg_time_window_relatedness(void *ctx, uint32_t a, uint32_t b) {
    const SGContext *sg_ctx = (const SGContext *)ctx;
    int32_t early_a = 0;
    int32_t late_a = 0;
    int32_t early_b = 0;
    int32_t late_b = 0;
    int64_t mid_a = 0;
    int64_t mid_b = 0;
    double score = 0.0;

    if (sg_request_time_window_bounds(sg_ctx, a, &early_a, &late_a) &&
        sg_request_time_window_bounds(sg_ctx, b, &early_b, &late_b)) {
        int32_t overlap_start = early_a > early_b ? early_a : early_b;
        int32_t overlap_end = late_a < late_b ? late_a : late_b;
        int32_t span_start = early_a < early_b ? early_a : early_b;
        int32_t span_end = late_a > late_b ? late_a : late_b;
        double overlap = overlap_end > overlap_start
                       ? (double)(overlap_end - overlap_start)
                       : 0.0;
        double span = span_end > span_start
                    ? (double)(span_end - span_start)
                    : 1.0;
        double width_delta = fabs((double)(late_a - early_a) - (double)(late_b - early_b));
        double midpoint_delta = fabs((((double)early_a + (double)late_a) * 0.5) -
                                     (((double)early_b + (double)late_b) * 0.5));

        score += 16.0 * (overlap / span);
        score -= midpoint_delta / 1800.0;
        score -= width_delta / 3600.0;
    } else if (sg_request_time_midpoint(sg_ctx, a, &mid_a) &&
               sg_request_time_midpoint(sg_ctx, b, &mid_b)) {
        score -= (double)sg_abs_i64(mid_a - mid_b) / 1800.0;
    } else {
        score -= (double)sg_abs_i64((int64_t)a - (int64_t)b);
    }

    if (sg_request_kind(sg_ctx, a) == sg_request_kind(sg_ctx, b)) {
        score += 0.5;
    }
    score += 1.0 / (1.0 + (double)sg_abs_i64((int64_t)a - (int64_t)b));
    return score;
}

static double sg_pd_shaw_relatedness(void *ctx, uint32_t a, uint32_t b) {
    const SGContext *sg_ctx = (const SGContext *)ctx;
    SGRequestKind kind_a = sg_request_kind(sg_ctx, a);
    SGRequestKind kind_b = sg_request_kind(sg_ctx, b);
    double score = sg_bootstrap_relatedness(ctx, a, b);
    double load_a = sg_request_load_magnitude(sg_ctx, a);
    double load_b = sg_request_load_magnitude(sg_ctx, b);

    if (kind_a == SG_REQUEST_KIND_PICKUP_DELIVERY &&
        kind_b == SG_REQUEST_KIND_PICKUP_DELIVERY) {
        score += 4.0;
    } else if (kind_a != kind_b) {
        score -= 1.0;
    }

    score -= fabs(load_a - load_b) / 50.0;
    return score;
}

static double sg_criticality_removal_cost(void *ctx, void *solution, uint32_t element_id) {
    const SGContext *sg_ctx = (const SGContext *)ctx;
    const SGBootstrapSolution *sol = (const SGBootstrapSolution *)solution;
    double score = 0.0;
    int32_t tw_width = 0;

    score += (100.0 - sg_request_priority_score(sg_ctx, element_id)) * 0.7;

    if (sg_request_tw_width(sg_ctx, element_id, &tw_width)) {
        score += ((double)tw_width / 3600.0) * 0.8;
    } else {
        score += 1.0;
    }

    score += sg_zone_density_score(sg_ctx, sol, element_id);
    score += sg_request_load_magnitude(sg_ctx, element_id) * 0.05;
    score += ((double)element_id + 1.0) * 0.0001;
    return score;
}

static ARStatus sg_unassign_removed_requests(SGBootstrapSolution *sol,
                                             const uint32_t *removed_ids,
                                             int removed_count) {
    ARStatus status;
    int i;

    if (!sol || (removed_count > 0 && !removed_ids)) {
        return AR_STATUS_INVALID_ARG;
    }

    for (i = 0; i < removed_count; i++) {
        status = sg_bootstrap_unassign_request(sol, removed_ids[i]);
        if (status != AR_STATUS_OK) {
            return status;
        }
    }

    return AR_STATUS_OK;
}

static ARStatus sg_destroy_random(void *op_ctx, void *solution, int count,
                                  uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    ARStatus status;

    if (!ctx || !ctx->op_rng || !sol || !removed_count || count < 0) {
        return AR_STATUS_INVALID_ARG;
    }

    status = ar_remove_random(ctx->op_rng, sol, count, removed_ids,
                              sg_get_assigned_count, sg_get_assigned_element,
                              NULL, removed_count);
    if (status != AR_STATUS_OK) {
        return status;
    }

    return sg_unassign_removed_requests(sol, removed_ids, *removed_count);
}

static ARStatus sg_destroy_worst(void *op_ctx, void *solution, int count,
                                 uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    ARStatus status;

    if (!ctx || !ctx->op_rng || !sol || !removed_count || count < 0) {
        return AR_STATUS_INVALID_ARG;
    }

    status = ar_remove_worst(ctx->op_rng, ctx, sol, count, removed_ids,
                             sg_get_assigned_count, sg_get_assigned_element,
                             sg_bootstrap_removal_cost, SG_WORST_RANDOMNESS,
                             NULL, removed_count);
    if (status != AR_STATUS_OK) {
        return status;
    }

    return sg_unassign_removed_requests(sol, removed_ids, *removed_count);
}

static ARStatus sg_destroy_shaw(void *op_ctx, void *solution, int count,
                                uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    ARStatus status;

    if (!ctx || !ctx->op_rng || !sol || !removed_count || count < 0) {
        return AR_STATUS_INVALID_ARG;
    }

    status = ar_remove_related(ctx->op_rng, ctx, sol, count, removed_ids,
                               sg_get_assigned_count, sg_get_assigned_element,
                               sg_bootstrap_relatedness, SG_SHAW_RANDOMNESS,
                               NULL, removed_count);
    if (status != AR_STATUS_OK) {
        return status;
    }

    return sg_unassign_removed_requests(sol, removed_ids, *removed_count);
}

static ARStatus sg_destroy_criticality_worst(void *op_ctx, void *solution, int count,
                                             uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    ARStatus status;

    if (!ctx || !ctx->op_rng || !sol || !removed_count || count < 0) {
        return AR_STATUS_INVALID_ARG;
    }

    status = ar_remove_worst(ctx->op_rng, ctx, sol, count, removed_ids,
                             sg_get_assigned_count, sg_get_assigned_element,
                             sg_criticality_removal_cost, SG_WORST_RANDOMNESS,
                             NULL, removed_count);
    if (status != AR_STATUS_OK) {
        return status;
    }

    return sg_unassign_removed_requests(sol, removed_ids, *removed_count);
}

static ARStatus sg_destroy_route_cluster(void *op_ctx, void *solution, int count,
                                         uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    ARStatus status;

    if (!ctx || !ctx->op_rng || !sol || !removed_count || count < 0) {
        return AR_STATUS_INVALID_ARG;
    }

    status = ar_remove_related(ctx->op_rng, ctx, sol, count, removed_ids,
                               sg_get_assigned_count, sg_get_assigned_element,
                               sg_route_cluster_relatedness, SG_ROUTE_CLUSTER_RANDOMNESS,
                               NULL, removed_count);
    if (status != AR_STATUS_OK) {
        return status;
    }

    return sg_unassign_removed_requests(sol, removed_ids, *removed_count);
}

static ARStatus sg_destroy_time_cluster(void *op_ctx, void *solution, int count,
                                        uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    ARStatus status;

    if (!ctx || !ctx->op_rng || !sol || !removed_count || count < 0) {
        return AR_STATUS_INVALID_ARG;
    }

    status = ar_remove_related(ctx->op_rng, ctx, sol, count, removed_ids,
                               sg_get_assigned_count, sg_get_assigned_element,
                               sg_time_cluster_relatedness, SG_TIME_CLUSTER_RANDOMNESS,
                               NULL, removed_count);
    if (status != AR_STATUS_OK) {
        return status;
    }

    return sg_unassign_removed_requests(sol, removed_ids, *removed_count);
}

static ARStatus sg_destroy_paired_shaw(void *op_ctx, void *solution, int count,
                                       uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    ARStatus status;

    if (!ctx || !ctx->op_rng || !sol || !removed_count || count < 0) {
        return AR_STATUS_INVALID_ARG;
    }

    status = ar_remove_related(ctx->op_rng, ctx, sol, count, removed_ids,
                               sg_get_assigned_count, sg_get_assigned_element,
                               sg_pd_shaw_relatedness, SG_PD_SHAW_RANDOMNESS,
                               NULL, removed_count);
    if (status != AR_STATUS_OK) {
        return status;
    }

    return sg_unassign_removed_requests(sol, removed_ids, *removed_count);
}

static ARStatus sg_reinsert_removed_requests(SGBootstrapSolution *sol,
                                             const uint32_t *removed_ids,
                                             int removed_count) {
    ARStatus status;
    int i;

    if (!sol || removed_count < 0 || (removed_count > 0 && !removed_ids)) {
        return AR_STATUS_INVALID_ARG;
    }

    for (i = 0; i < removed_count; i++) {
        status = sg_bootstrap_assign_request(sol, removed_ids[i]);
        if (status != AR_STATUS_OK) {
            return status;
        }
    }

    return AR_STATUS_OK;
}

static double sg_vehicle_request_cost(const SGContext *ctx, uint32_t vehicle_id,
                                      uint32_t request_id, double noise_scale) {
    const SGVehicleRecord *vehicle;
    double x;
    double y;
    double cost = 0.0;
    int64_t midpoint;

    if (!ctx || vehicle_id >= ctx->num_vehicles || !ctx->vehicles) {
        return (double)request_id + 1.0;
    }
    vehicle = &ctx->vehicles[vehicle_id];

    if (sg_request_centroid(ctx, request_id, &x, &y)) {
        if (vehicle->has_depots && vehicle->start_depot_id < ctx->num_depots &&
            vehicle->end_depot_id < ctx->num_depots &&
            ctx->depots[vehicle->start_depot_id].has_location &&
            ctx->depots[vehicle->end_depot_id].has_location) {
            const SGDepotRecord *start = &ctx->depots[vehicle->start_depot_id];
            const SGDepotRecord *end = &ctx->depots[vehicle->end_depot_id];
            cost += sg_euclid(start->x, start->y, x, y);
            cost += sg_euclid(x, y, end->x, end->y);
        } else {
            cost += fabs(x) + fabs(y);
        }
    } else {
        cost += (double)((request_id % 17U) + 1U);
    }

    if (sg_request_time_midpoint(ctx, request_id, &midpoint) && vehicle->has_shift_time_window) {
        if (midpoint < vehicle->shift_early) {
            cost += (double)(vehicle->shift_early - midpoint) / 1800.0;
        } else if (midpoint > vehicle->shift_late) {
            cost += (double)(midpoint - vehicle->shift_late) / 1800.0;
        }
    }

    cost += sg_request_load_magnitude(ctx, request_id) * 0.05;
    cost += (100.0 - sg_request_priority_score(ctx, request_id)) * 0.01;

    if (noise_scale > 0.0 && ctx->op_rng) {
        double draw = sh_rng_uniform_range(ctx->op_rng, -noise_scale, noise_scale);
        cost *= (1.0 + draw);
    }

    if (!isfinite(cost) || cost < 0.0) {
        return (double)request_id + 1.0;
    }
    return cost;
}

static int sg_request_best_k_costs(const SGContext *ctx, uint32_t request_id, int k,
                                   double noise_scale, double *best_cost,
                                   double *kth_cost) {
    double *costs;
    uint32_t v;
    int i;
    int j;
    int count = 0;

    if (!best_cost || !kth_cost || k <= 0) {
        return 0;
    }

    if (!ctx || ctx->num_vehicles == 0) {
        *best_cost = sg_vehicle_request_cost(ctx, 0, request_id, noise_scale);
        *kth_cost = *best_cost + 1.0;
        return 1;
    }

    costs = (double *)malloc((size_t)ctx->num_vehicles * sizeof(double));
    if (!costs) {
        return 0;
    }

    for (v = 0; v < ctx->num_vehicles; v++) {
        costs[count++] = sg_vehicle_request_cost(ctx, v, request_id, noise_scale);
    }

    for (i = 0; i < count; i++) {
        for (j = i + 1; j < count; j++) {
            if (costs[j] < costs[i]) {
                double tmp = costs[i];
                costs[i] = costs[j];
                costs[j] = tmp;
            }
        }
    }

    *best_cost = costs[0];
    if (count >= k) {
        *kth_cost = costs[k - 1];
    } else {
        *kth_cost = costs[count - 1] + 1.0;
    }

    free(costs);
    return 1;
}

static uint32_t sg_select_extra_greedy(const SGContext *ctx, const SGBootstrapSolution *sol) {
    uint32_t i;
    uint32_t best_id = UINT32_MAX;
    double best_cost = INFINITY;

    if (!sol) {
        return UINT32_MAX;
    }

    for (i = 0; i < sol->num_unassigned; i++) {
        uint32_t id = sol->unassigned_ids[i];
        double cost;
        double ignored;
        if (!sg_request_best_k_costs(ctx, id, 1, 0.0, &cost, &ignored)) {
            continue;
        }
        if (cost < best_cost) {
            best_cost = cost;
            best_id = id;
        }
    }

    return best_id;
}

static uint32_t sg_select_extra_regret(const SGContext *ctx, const SGBootstrapSolution *sol,
                                       int regret_k, double noise_scale) {
    uint32_t i;
    uint32_t best_id = UINT32_MAX;
    double best_regret = -INFINITY;
    double best_first_cost = INFINITY;

    if (!sol || regret_k <= 1) {
        return UINT32_MAX;
    }

    for (i = 0; i < sol->num_unassigned; i++) {
        uint32_t id = sol->unassigned_ids[i];
        double first = 0.0;
        double kth = 0.0;
        double regret;
        if (!sg_request_best_k_costs(ctx, id, regret_k, noise_scale, &first, &kth)) {
            continue;
        }
        regret = kth - first;
        if (regret > best_regret ||
            (fabs(regret - best_regret) <= 1e-9 && first < best_first_cost)) {
            best_regret = regret;
            best_first_cost = first;
            best_id = id;
        }
    }

    return best_id;
}

static uint32_t sg_select_extra_pair_sync(const SGContext *ctx, const SGBootstrapSolution *sol) {
    uint32_t i;
    uint32_t best_id = UINT32_MAX;
    double best_cost = INFINITY;

    if (!sol) {
        return UINT32_MAX;
    }

    for (i = 0; i < sol->num_unassigned; i++) {
        uint32_t id = sol->unassigned_ids[i];
        if (sg_request_kind(ctx, id) == SG_REQUEST_KIND_PICKUP_DELIVERY) {
            double cost;
            double ignored;
            if (!sg_request_best_k_costs(ctx, id, 1, 0.0, &cost, &ignored)) {
                continue;
            }
            if (cost < best_cost) {
                best_cost = cost;
                best_id = id;
            }
        }
    }

    if (best_id != UINT32_MAX) {
        return best_id;
    }
    return sg_select_extra_greedy(ctx, sol);
}

static ARStatus sg_repair_with_selector(SGContext *ctx, SGBootstrapSolution *sol,
                                        const uint32_t *removed_ids, int removed_count,
                                        uint32_t (*selector)(const SGContext *,
                                                             const SGBootstrapSolution *)) {
    ARStatus status;
    uint32_t chosen;

    if (!ctx || !sol || !selector) {
        return AR_STATUS_INVALID_ARG;
    }

    status = sg_reinsert_removed_requests(sol, removed_ids, removed_count);
    if (status != AR_STATUS_OK) {
        return status;
    }

    if (sol->num_unassigned == 0) {
        return AR_STATUS_OK;
    }

    chosen = selector(ctx, sol);
    if (chosen == UINT32_MAX) {
        chosen = sol->unassigned_ids[0];
    }
    return sg_bootstrap_assign_request(sol, chosen);
}

static ARStatus sg_repair_greedy_insertion(void *op_ctx, void *solution,
                                           const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    return sg_repair_with_selector(ctx, sol, removed_ids, removed_count, sg_select_extra_greedy);
}

static ARStatus sg_repair_regret2(void *op_ctx, void *solution,
                                  const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    ARStatus status;
    uint32_t chosen;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }
    status = sg_reinsert_removed_requests(sol, removed_ids, removed_count);
    if (status != AR_STATUS_OK || sol->num_unassigned == 0) {
        return status;
    }
    chosen = sg_select_extra_regret(ctx, sol, 2, 0.0);
    if (chosen == UINT32_MAX) {
        chosen = sol->unassigned_ids[0];
    }
    return sg_bootstrap_assign_request(sol, chosen);
}

static ARStatus sg_repair_regret3(void *op_ctx, void *solution,
                                  const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    ARStatus status;
    uint32_t chosen;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }
    status = sg_reinsert_removed_requests(sol, removed_ids, removed_count);
    if (status != AR_STATUS_OK || sol->num_unassigned == 0) {
        return status;
    }
    chosen = sg_select_extra_regret(ctx, sol, 3, 0.0);
    if (chosen == UINT32_MAX) {
        chosen = sol->unassigned_ids[0];
    }
    return sg_bootstrap_assign_request(sol, chosen);
}

static ARStatus sg_repair_regret4(void *op_ctx, void *solution,
                                  const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    ARStatus status;
    uint32_t chosen;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }
    status = sg_reinsert_removed_requests(sol, removed_ids, removed_count);
    if (status != AR_STATUS_OK || sol->num_unassigned == 0) {
        return status;
    }
    chosen = sg_select_extra_regret(ctx, sol, 4, 0.0);
    if (chosen == UINT32_MAX) {
        chosen = sol->unassigned_ids[0];
    }
    return sg_bootstrap_assign_request(sol, chosen);
}

static ARStatus sg_repair_noise_regret(void *op_ctx, void *solution,
                                       const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    ARStatus status;
    uint32_t chosen;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }
    status = sg_reinsert_removed_requests(sol, removed_ids, removed_count);
    if (status != AR_STATUS_OK || sol->num_unassigned == 0) {
        return status;
    }
    chosen = sg_select_extra_regret(ctx, sol, 3, SG_NOISE_REGRET_SCALE);
    if (chosen == UINT32_MAX) {
        chosen = sol->unassigned_ids[0];
    }
    return sg_bootstrap_assign_request(sol, chosen);
}

static ARStatus sg_repair_pair_sync(void *op_ctx, void *solution,
                                    const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    return sg_repair_with_selector(ctx, sol, removed_ids, removed_count, sg_select_extra_pair_sync);
}

static double sg_route_objective_cost(uint32_t unassigned, uint32_t vehicles_used,
                                      double total_distance) {
    return (double)unassigned * SG_ROUTE_OBJECTIVE_UNASSIGNED_WEIGHT +
           (double)vehicles_used * SG_ROUTE_OBJECTIVE_VEHICLE_WEIGHT +
           total_distance;
}

static uint32_t *sg_route_vehicle_ptr(SGRouteSolution *sol, uint32_t vehicle_id) {
    return sol->route_requests + (size_t)vehicle_id * (size_t)sol->route_stride;
}

static const uint32_t *sg_route_vehicle_ptr_const(const SGRouteSolution *sol, uint32_t vehicle_id) {
    return sol->route_requests + (size_t)vehicle_id * (size_t)sol->route_stride;
}

static SGRouteStop *sg_route_vehicle_stop_ptr(SGRouteSolution *sol, uint32_t vehicle_id) {
    return sol->route_stops + (size_t)vehicle_id * (size_t)sol->stop_stride;
}

static const SGRouteStop *sg_route_vehicle_stop_ptr_const(const SGRouteSolution *sol,
                                                          uint32_t vehicle_id) {
    return sol->route_stops + (size_t)vehicle_id * (size_t)sol->stop_stride;
}

static uint32_t *sg_route_vehicle_stop_prev_ptr(SGRouteSolution *sol, uint32_t vehicle_id) {
    return sol->route_stop_prev + (size_t)vehicle_id * (size_t)sol->stop_stride;
}

static uint32_t *sg_route_vehicle_stop_next_ptr(SGRouteSolution *sol, uint32_t vehicle_id) {
    return sol->route_stop_next + (size_t)vehicle_id * (size_t)sol->stop_stride;
}

static const uint32_t *sg_route_vehicle_stop_prev_ptr_const(const SGRouteSolution *sol,
                                                            uint32_t vehicle_id) {
    return sol->route_stop_prev + (size_t)vehicle_id * (size_t)sol->stop_stride;
}

static const uint32_t *sg_route_vehicle_stop_next_ptr_const(const SGRouteSolution *sol,
                                                            uint32_t vehicle_id) {
    return sol->route_stop_next + (size_t)vehicle_id * (size_t)sol->stop_stride;
}

static int sg_request_emit_stops(const SGContext *ctx, uint32_t request_id,
                                 SGRouteStop *stops_out, uint32_t *stop_count_out) {
    const SGRequestRecord *request;

    if (!ctx || request_id >= ctx->num_requests || !stops_out || !stop_count_out) {
        return 0;
    }

    request = sg_get_request_record(ctx, request_id);
    if (!request || request->kind == SG_REQUEST_KIND_UNBOUND) {
        return 0;
    }

    if (request->kind == SG_REQUEST_KIND_DELIVERY_ONLY) {
        if (!request->has_delivery_task || request->delivery_task_id >= ctx->num_tasks) {
            return 0;
        }
        stops_out[0].request_id = request_id;
        stops_out[0].task_id = request->delivery_task_id;
        stops_out[0].is_pickup = 0;
        *stop_count_out = 1;
        return 1;
    }

    if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY) {
        if (!request->has_pickup_task || !request->has_delivery_task ||
            request->pickup_task_id >= ctx->num_tasks ||
            request->delivery_task_id >= ctx->num_tasks) {
            return 0;
        }
        stops_out[0].request_id = request_id;
        stops_out[0].task_id = request->pickup_task_id;
        stops_out[0].is_pickup = 1;
        stops_out[1].request_id = request_id;
        stops_out[1].task_id = request->delivery_task_id;
        stops_out[1].is_pickup = 0;
        *stop_count_out = 2;
        return 1;
    }

    return 0;
}

static int sg_route_rebuild_vehicle_stop_state(const SGContext *ctx, SGRouteSolution *sol,
                                               uint32_t vehicle_id) {
    const uint32_t *route;
    SGRouteStop *stops;
    uint32_t *prev;
    uint32_t *next;
    uint32_t req_len;
    uint32_t stop_len = 0;
    uint32_t r;

    if (!ctx || !sol || vehicle_id >= sol->num_vehicles) {
        return 0;
    }
    if (!sol->route_stop_lengths || !sol->route_stops || !sol->route_stop_prev ||
        !sol->route_stop_next || !sol->request_pickup_stop_pos ||
        !sol->request_delivery_stop_pos) {
        return 0;
    }

    route = sg_route_vehicle_ptr_const(sol, vehicle_id);
    stops = sg_route_vehicle_stop_ptr(sol, vehicle_id);
    prev = sg_route_vehicle_stop_prev_ptr(sol, vehicle_id);
    next = sg_route_vehicle_stop_next_ptr(sol, vehicle_id);
    req_len = sol->route_lengths[vehicle_id];

    for (r = 0; r < sol->base.total_requests; r++) {
        if (sol->request_vehicle[r] == vehicle_id) {
            sol->request_pickup_stop_pos[r] = UINT32_MAX;
            sol->request_delivery_stop_pos[r] = UINT32_MAX;
        }
    }

    for (r = 0; r < req_len; r++) {
        SGRouteStop emitted[2];
        uint32_t emitted_count = 0;
        uint32_t e;
        uint32_t request_id = route[r];

        if (!sg_request_emit_stops(ctx, request_id, emitted, &emitted_count)) {
            return 0;
        }
        if (stop_len + emitted_count > sol->stop_stride) {
            return 0;
        }

        for (e = 0; e < emitted_count; e++) {
            stops[stop_len] = emitted[e];
            prev[stop_len] = stop_len > 0 ? stop_len - 1U : UINT32_MAX;
            next[stop_len] = UINT32_MAX;
            if (stop_len > 0) {
                next[stop_len - 1U] = stop_len;
            }
            if (emitted[e].is_pickup) {
                sol->request_pickup_stop_pos[request_id] = stop_len;
            } else {
                sol->request_delivery_stop_pos[request_id] = stop_len;
            }
            stop_len++;
        }
    }

    for (r = stop_len; r < sol->stop_stride; r++) {
        stops[r].request_id = UINT32_MAX;
        stops[r].task_id = UINT32_MAX;
        stops[r].is_pickup = 0;
        prev[r] = UINT32_MAX;
        next[r] = UINT32_MAX;
    }

    sol->route_stop_lengths[vehicle_id] = stop_len;
    return 1;
}

static int sg_route_stop_sequence_feasible(const SGContext *ctx, uint32_t vehicle_id,
                                           const SGRouteStop *stops, uint32_t stop_count,
                                           double *distance_out) {
    const SGVehicleRecord *vehicle;
    const SGDepotRecord *start_depot;
    const SGDepotRecord *end_depot;
    double sx = 0.0;
    double sy = 0.0;
    double ex = 0.0;
    double ey = 0.0;
    double *service_start = NULL;
    double *depart = NULL;
    double *latest_start = NULL;
    double *forward_slack = NULL;
    double *load_profile = NULL;
    double *pickup_depart = NULL;
    uint8_t *pickup_seen = NULL;
    double *min_prefix = NULL;
    double *max_prefix = NULL;
    double distance = 0.0;
    double time_cursor;
    double px;
    double py;
    double latest_next;
    uint32_t i;
    uint32_t d;
    int feasible = 0;

    if (!ctx || !distance_out || vehicle_id >= ctx->num_vehicles ||
        (stop_count > 0 && !stops)) {
        return 0;
    }

    *distance_out = 0.0;
    if (stop_count == 0) {
        return 1;
    }

    vehicle = &ctx->vehicles[vehicle_id];
    if (!vehicle->has_depots || vehicle->start_depot_id >= ctx->num_depots ||
        vehicle->end_depot_id >= ctx->num_depots) {
        return 0;
    }
    if (!sg_vehicle_start_end_locations(ctx, vehicle_id, &sx, &sy, &ex, &ey)) {
        return 0;
    }

    start_depot = &ctx->depots[vehicle->start_depot_id];
    end_depot = &ctx->depots[vehicle->end_depot_id];

    service_start = (double *)malloc((size_t)stop_count * sizeof(double));
    depart = (double *)malloc((size_t)stop_count * sizeof(double));
    latest_start = (double *)malloc((size_t)stop_count * sizeof(double));
    forward_slack = (double *)malloc((size_t)stop_count * sizeof(double));
    if (!service_start || !depart || !latest_start || !forward_slack) {
        goto done;
    }

    if (ctx->dimension_count > 0) {
        size_t load_count = ((size_t)stop_count + 1U) * (size_t)ctx->dimension_count;
        load_profile = (double *)malloc(load_count * sizeof(double));
        min_prefix = (double *)malloc((size_t)ctx->dimension_count * sizeof(double));
        max_prefix = (double *)malloc((size_t)ctx->dimension_count * sizeof(double));
        if (!load_profile || !min_prefix || !max_prefix) {
            goto done;
        }

        for (d = 0; d < ctx->dimension_count; d++) {
            load_profile[d] = 0.0;
            min_prefix[d] = 0.0;
            max_prefix[d] = 0.0;
        }

        for (i = 0; i < stop_count; i++) {
            const SGRouteStop *stop = &stops[i];
            const SGTaskRecord *task;
            if (stop->task_id >= ctx->num_tasks) {
                goto done;
            }
            task = &ctx->tasks[stop->task_id];
            if (!task->has_demand || !task->demand) {
                goto done;
            }

            for (d = 0; d < ctx->dimension_count; d++) {
                double prefix = load_profile[(size_t)i * (size_t)ctx->dimension_count + d] +
                                task->demand[d];
                load_profile[((size_t)i + 1U) * (size_t)ctx->dimension_count + d] = prefix;
                if (prefix < min_prefix[d]) {
                    min_prefix[d] = prefix;
                }
                if (prefix > max_prefix[d]) {
                    max_prefix[d] = prefix;
                }
            }
        }

        for (d = 0; d < ctx->dimension_count; d++) {
            double cap = (vehicle->has_capacity && vehicle->capacity)
                         ? vehicle->capacity[d]
                         : INFINITY;
            double initial_load = -min_prefix[d];
            if ((max_prefix[d] - min_prefix[d]) > cap + SG_DEMAND_TOLERANCE) {
                goto done;
            }
            for (i = 0; i <= stop_count; i++) {
                double load = initial_load +
                              load_profile[(size_t)i * (size_t)ctx->dimension_count + d];
                if (load < -SG_DEMAND_TOLERANCE || load > cap + SG_DEMAND_TOLERANCE) {
                    goto done;
                }
            }
        }
    }

    if (ctx->num_requests > 0) {
        pickup_depart = (double *)malloc((size_t)ctx->num_requests * sizeof(double));
        pickup_seen = (uint8_t *)calloc((size_t)ctx->num_requests, sizeof(uint8_t));
        if (!pickup_depart || !pickup_seen) {
            goto done;
        }
        for (i = 0; i < ctx->num_requests; i++) {
            pickup_depart[i] = 0.0;
        }
    }

    time_cursor = vehicle->has_shift_time_window ? (double)vehicle->shift_early : 0.0;
    if (start_depot->has_time_window) {
        if (time_cursor < (double)start_depot->tw_early) {
            time_cursor = (double)start_depot->tw_early;
        }
        if (time_cursor > (double)start_depot->tw_late + 1e-9) {
            goto done;
        }
    }

    px = sx;
    py = sy;
    for (i = 0; i < stop_count; i++) {
        const SGRouteStop *stop = &stops[i];
        const SGTaskRecord *task;
        const SGRequestRecord *request;
        double travel;
        double start;
        double ride_time = 0.0;
        double ride_limit = INFINITY;

        if (stop->request_id >= ctx->num_requests || stop->task_id >= ctx->num_tasks) {
            goto done;
        }
        task = &ctx->tasks[stop->task_id];
        request = &ctx->requests[stop->request_id];
        if (!task->has_location || !task->has_time_window) {
            goto done;
        }

        travel = sg_euclid(px, py, task->x, task->y);
        if (!isfinite(travel) || travel < 0.0) {
            goto done;
        }
        distance += travel;
        time_cursor += travel;
        start = time_cursor;
        if (start < (double)task->tw_early) {
            start = (double)task->tw_early;
        }
        if (start > (double)task->tw_late + 1e-9) {
            goto done;
        }

        if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY) {
            if (stop->is_pickup) {
                pickup_seen[stop->request_id] = 1;
            } else {
                const SGTaskRecord *pickup_task;
                const SGTaskRecord *drop_task;
                if (!request->has_pickup_task || !request->has_delivery_task ||
                    request->pickup_task_id >= ctx->num_tasks ||
                    request->delivery_task_id >= ctx->num_tasks) {
                    goto done;
                }
                if (!pickup_seen[stop->request_id]) {
                    goto done;
                }
                ride_time = start - pickup_depart[stop->request_id];
                if (ride_time < -1e-9) {
                    goto done;
                }
                pickup_task = &ctx->tasks[request->pickup_task_id];
                drop_task = &ctx->tasks[request->delivery_task_id];
                if (pickup_task->has_time_window && drop_task->has_time_window) {
                    ride_limit = (double)(drop_task->tw_late - pickup_task->tw_early);
                }
                if (isfinite(ride_limit) && ride_time > ride_limit + 1e-9) {
                    goto done;
                }
            }
        }

        service_start[i] = start;
        depart[i] = start + (double)task->service_seconds;
        if (!isfinite(depart[i])) {
            goto done;
        }
        if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY && stop->is_pickup) {
            pickup_depart[stop->request_id] = depart[i];
        }
        time_cursor = depart[i];
        px = task->x;
        py = task->y;
    }

    {
        double travel_to_end = sg_euclid(px, py, ex, ey);
        if (!isfinite(travel_to_end) || travel_to_end < 0.0) {
            goto done;
        }
        distance += travel_to_end;
        time_cursor += travel_to_end;
    }

    if (end_depot->has_time_window) {
        if (time_cursor < (double)end_depot->tw_early) {
            time_cursor = (double)end_depot->tw_early;
        }
        if (time_cursor > (double)end_depot->tw_late + 1e-9) {
            goto done;
        }
    }
    if (vehicle->has_shift_time_window && time_cursor > (double)vehicle->shift_late + 1e-9) {
        goto done;
    }

    latest_next = vehicle->has_shift_time_window ? (double)vehicle->shift_late : INFINITY;
    if (end_depot->has_time_window && latest_next > (double)end_depot->tw_late) {
        latest_next = (double)end_depot->tw_late;
    }
    for (i = stop_count; i > 0; i--) {
        uint32_t idx = i - 1U;
        const SGTaskRecord *task = &ctx->tasks[stops[idx].task_id];
        double nx;
        double ny;
        double travel_to_next;
        if (idx + 1U < stop_count) {
            const SGTaskRecord *next_task = &ctx->tasks[stops[idx + 1U].task_id];
            nx = next_task->x;
            ny = next_task->y;
        } else {
            nx = ex;
            ny = ey;
        }

        travel_to_next = sg_euclid(task->x, task->y, nx, ny);
        if (!isfinite(travel_to_next) || travel_to_next < 0.0) {
            goto done;
        }
        latest_start[idx] = latest_next - travel_to_next - (double)task->service_seconds;
        if (task->has_time_window && latest_start[idx] > (double)task->tw_late) {
            latest_start[idx] = (double)task->tw_late;
        }
        if (task->has_time_window && latest_start[idx] < (double)task->tw_early - 1e-9) {
            goto done;
        }

        forward_slack[idx] = latest_start[idx] - service_start[idx];
        if (forward_slack[idx] < -1e-9) {
            goto done;
        }
        latest_next = latest_start[idx];
    }

    *distance_out = distance;
    feasible = isfinite(distance) && distance >= 0.0;

done:
    free(service_start);
    free(depart);
    free(latest_start);
    free(forward_slack);
    free(load_profile);
    free(pickup_depart);
    free(pickup_seen);
    free(min_prefix);
    free(max_prefix);
    return feasible;
}

static void sg_route_solution_reset(SGRouteSolution *sol) {
    if (!sol) {
        return;
    }

    sg_bootstrap_solution_reset(&sol->base);
    free(sol->route_lengths);
    free(sol->route_requests);
    free(sol->route_stop_lengths);
    free(sol->route_stops);
    free(sol->route_stop_prev);
    free(sol->route_stop_next);
    free(sol->request_vehicle);
    free(sol->request_pos);
    free(sol->request_pickup_stop_pos);
    free(sol->request_delivery_stop_pos);
    free(sol->route_distance);
    sol->route_lengths = NULL;
    sol->route_requests = NULL;
    sol->route_stop_lengths = NULL;
    sol->route_stops = NULL;
    sol->route_stop_prev = NULL;
    sol->route_stop_next = NULL;
    sol->request_vehicle = NULL;
    sol->request_pos = NULL;
    sol->request_pickup_stop_pos = NULL;
    sol->request_delivery_stop_pos = NULL;
    sol->route_distance = NULL;
    sol->num_vehicles = 0;
    sol->route_stride = 0;
    sol->stop_stride = 0;
    sol->vehicles_used = 0;
    sol->total_distance = 0.0;
}

static ARStatus sg_route_solution_init(const SGContext *ctx, SGRouteSolution *sol) {
    ARStatus status;
    size_t route_capacity;
    size_t stop_capacity;
    uint32_t i;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }

    memset(sol, 0, sizeof(*sol));

    status = sg_bootstrap_solution_init(&sol->base, ctx->num_requests);
    if (status != AR_STATUS_OK) {
        return status;
    }

    sol->num_vehicles = ctx->num_vehicles;
    sol->route_stride = ctx->num_requests > 0 ? ctx->num_requests : 1;
    if (ctx->num_requests > UINT32_MAX / 2U) {
        sg_route_solution_reset(sol);
        return AR_STATUS_OUT_OF_MEMORY;
    }
    sol->stop_stride = ctx->num_requests > 0 ? ctx->num_requests * 2U : 1U;

    if (ctx->num_requests == 0 || ctx->num_vehicles == 0) {
        return AR_STATUS_OK;
    }

    if ((size_t)ctx->num_vehicles > SIZE_MAX / (size_t)sol->route_stride) {
        sg_route_solution_reset(sol);
        return AR_STATUS_OUT_OF_MEMORY;
    }
    route_capacity = (size_t)ctx->num_vehicles * (size_t)sol->route_stride;
    if ((size_t)ctx->num_vehicles > SIZE_MAX / (size_t)sol->stop_stride) {
        sg_route_solution_reset(sol);
        return AR_STATUS_OUT_OF_MEMORY;
    }
    stop_capacity = (size_t)ctx->num_vehicles * (size_t)sol->stop_stride;

    sol->route_lengths = (uint32_t *)calloc((size_t)ctx->num_vehicles, sizeof(uint32_t));
    sol->route_requests = (uint32_t *)calloc(route_capacity, sizeof(uint32_t));
    sol->route_stop_lengths = (uint32_t *)calloc((size_t)ctx->num_vehicles, sizeof(uint32_t));
    sol->route_stops = (SGRouteStop *)calloc(stop_capacity, sizeof(SGRouteStop));
    sol->route_stop_prev = (uint32_t *)malloc(stop_capacity * sizeof(uint32_t));
    sol->route_stop_next = (uint32_t *)malloc(stop_capacity * sizeof(uint32_t));
    sol->request_vehicle = (uint32_t *)malloc((size_t)ctx->num_requests * sizeof(uint32_t));
    sol->request_pos = (uint32_t *)malloc((size_t)ctx->num_requests * sizeof(uint32_t));
    sol->request_pickup_stop_pos = (uint32_t *)malloc((size_t)ctx->num_requests * sizeof(uint32_t));
    sol->request_delivery_stop_pos = (uint32_t *)malloc((size_t)ctx->num_requests * sizeof(uint32_t));
    sol->route_distance = (double *)calloc((size_t)ctx->num_vehicles, sizeof(double));

    if (!sol->route_lengths || !sol->route_requests || !sol->route_stop_lengths ||
        !sol->route_stops || !sol->route_stop_prev || !sol->route_stop_next ||
        !sol->request_vehicle || !sol->request_pos || !sol->request_pickup_stop_pos ||
        !sol->request_delivery_stop_pos || !sol->route_distance) {
        sg_route_solution_reset(sol);
        return AR_STATUS_OUT_OF_MEMORY;
    }

    for (i = 0; i < stop_capacity; i++) {
        sol->route_stop_prev[i] = UINT32_MAX;
        sol->route_stop_next[i] = UINT32_MAX;
        sol->route_stops[i].request_id = UINT32_MAX;
        sol->route_stops[i].task_id = UINT32_MAX;
        sol->route_stops[i].is_pickup = 0;
    }

    for (i = 0; i < ctx->num_requests; i++) {
        sol->request_vehicle[i] = UINT32_MAX;
        sol->request_pos[i] = UINT32_MAX;
        sol->request_pickup_stop_pos[i] = UINT32_MAX;
        sol->request_delivery_stop_pos[i] = UINT32_MAX;
    }

    return AR_STATUS_OK;
}

static void *sg_route_solution_copy(const void *solution, void *user_ctx) {
    const SGRouteSolution *src = (const SGRouteSolution *)solution;
    const SGContext *ctx = (const SGContext *)user_ctx;
    SGRouteSolution *dst;

    if (!src || !ctx) {
        return NULL;
    }

    dst = (SGRouteSolution *)calloc(1, sizeof(*dst));
    if (!dst) {
        return NULL;
    }

    if (sg_route_solution_init(ctx, dst) != AR_STATUS_OK) {
        free(dst);
        return NULL;
    }

    dst->base.num_assigned = src->base.num_assigned;
    dst->base.num_unassigned = src->base.num_unassigned;
    dst->vehicles_used = src->vehicles_used;
    dst->total_distance = src->total_distance;

    if (src->base.total_requests > 0) {
        size_t req_count = (size_t)src->base.total_requests;
        memcpy(dst->base.assigned_ids, src->base.assigned_ids, req_count * sizeof(uint32_t));
        memcpy(dst->base.unassigned_ids, src->base.unassigned_ids, req_count * sizeof(uint32_t));
        memcpy(dst->base.assigned_flags, src->base.assigned_flags, req_count * sizeof(uint8_t));
        memcpy(dst->request_vehicle, src->request_vehicle, req_count * sizeof(uint32_t));
        memcpy(dst->request_pos, src->request_pos, req_count * sizeof(uint32_t));
        memcpy(dst->request_pickup_stop_pos, src->request_pickup_stop_pos,
               req_count * sizeof(uint32_t));
        memcpy(dst->request_delivery_stop_pos, src->request_delivery_stop_pos,
               req_count * sizeof(uint32_t));
    }

    if (src->num_vehicles > 0) {
        size_t route_count = (size_t)src->num_vehicles * (size_t)src->route_stride;
        size_t stop_count = (size_t)src->num_vehicles * (size_t)src->stop_stride;
        memcpy(dst->route_lengths, src->route_lengths,
               (size_t)src->num_vehicles * sizeof(uint32_t));
        memcpy(dst->route_requests, src->route_requests, route_count * sizeof(uint32_t));
        memcpy(dst->route_stop_lengths, src->route_stop_lengths,
               (size_t)src->num_vehicles * sizeof(uint32_t));
        memcpy(dst->route_stops, src->route_stops, stop_count * sizeof(SGRouteStop));
        memcpy(dst->route_stop_prev, src->route_stop_prev, stop_count * sizeof(uint32_t));
        memcpy(dst->route_stop_next, src->route_stop_next, stop_count * sizeof(uint32_t));
        memcpy(dst->route_distance, src->route_distance,
               (size_t)src->num_vehicles * sizeof(double));
    }

    return dst;
}

static void sg_route_solution_free(void *solution, void *user_ctx) {
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    (void)user_ctx;

    if (!sol) {
        return;
    }

    sg_route_solution_reset(sol);
    free(sol);
}

static int sg_route_solution_validate(const void *solution, void *user_ctx) {
    const SGRouteSolution *sol = (const SGRouteSolution *)solution;
    const SGContext *ctx = (const SGContext *)user_ctx;
    uint32_t route_assigned = 0;
    uint32_t computed_vehicles = 0;
    double computed_distance = 0.0;
    uint8_t *seen_assigned = NULL;
    uint32_t v;
    uint32_t r;
    int ok = 0;

    if (!sol || !ctx) {
        return 0;
    }
    if (!sg_bootstrap_validate(&sol->base, user_ctx)) {
        return 0;
    }
    if (sol->num_vehicles != ctx->num_vehicles) {
        return 0;
    }
    if (ctx->num_requests > 0 &&
        (sol->route_stride < ctx->num_requests ||
         sol->stop_stride < ctx->num_requests * 2U)) {
        return 0;
    }
    if ((ctx->num_requests > 0 || ctx->num_vehicles > 0) &&
        (!sol->request_vehicle || !sol->request_pos || !sol->route_lengths ||
         !sol->route_requests || !sol->route_stop_lengths || !sol->route_stops ||
         !sol->route_stop_prev || !sol->route_stop_next ||
         !sol->request_pickup_stop_pos || !sol->request_delivery_stop_pos ||
         !sol->route_distance)) {
        return 0;
    }

    if (ctx->num_requests > 0) {
        seen_assigned = (uint8_t *)calloc((size_t)ctx->num_requests, sizeof(uint8_t));
        if (!seen_assigned) {
            return 0;
        }
    }

    for (v = 0; v < sol->num_vehicles; v++) {
        const uint32_t *route = sg_route_vehicle_ptr_const(sol, v);
        const SGRouteStop *stops = sg_route_vehicle_stop_ptr_const(sol, v);
        const uint32_t *prev = sg_route_vehicle_stop_prev_ptr_const(sol, v);
        const uint32_t *next = sg_route_vehicle_stop_next_ptr_const(sol, v);
        uint32_t len = sol->route_lengths[v];
        uint32_t stop_len = sol->route_stop_lengths[v];
        double recomputed_distance = 0.0;
        uint32_t s;

        if (len > sol->base.total_requests) {
            goto done;
        }
        if (stop_len > sol->stop_stride) {
            goto done;
        }

        for (r = 0; r < len; r++) {
            uint32_t request_id = route[r];
            if (request_id >= sol->base.total_requests ||
                !sol->base.assigned_flags[request_id]) {
                goto done;
            }
            if (sol->request_vehicle[request_id] != v || sol->request_pos[request_id] != r) {
                goto done;
            }
            if (seen_assigned[request_id]) {
                goto done;
            }
            seen_assigned[request_id] = 1;
            route_assigned++;
        }

        for (s = 0; s < stop_len; s++) {
            const SGRouteStop *stop = &stops[s];
            if (stop->request_id >= sol->base.total_requests ||
                !sol->base.assigned_flags[stop->request_id] ||
                stop->task_id >= ctx->num_tasks) {
                goto done;
            }
            if ((s == 0 && prev[s] != UINT32_MAX) ||
                (s > 0 && prev[s] != s - 1U) ||
                (s + 1U < stop_len && next[s] != s + 1U) ||
                (s + 1U == stop_len && next[s] != UINT32_MAX)) {
                goto done;
            }
        }

        if (stop_len > 0) {
            if (!sg_route_stop_sequence_feasible(ctx, v, stops, stop_len, &recomputed_distance)) {
                goto done;
            }
            computed_vehicles++;
            computed_distance += recomputed_distance;
        } else {
            recomputed_distance = 0.0;
        }

        if (fabs(recomputed_distance - sol->route_distance[v]) > 1e-6) {
            goto done;
        }
    }

    if (route_assigned != sol->base.num_assigned ||
        computed_vehicles != sol->vehicles_used ||
        fabs(computed_distance - sol->total_distance) > 1e-6) {
        goto done;
    }

    for (r = 0; r < sol->base.total_requests; r++) {
        const SGRequestRecord *request = &ctx->requests[r];
        if (sol->base.assigned_flags[r]) {
            uint32_t vehicle_id = sol->request_vehicle[r];
            const SGRouteStop *stops;
            uint32_t stop_len;
            if (sol->request_vehicle[r] >= sol->num_vehicles ||
                sol->request_pos[r] == UINT32_MAX ||
                sol->request_delivery_stop_pos[r] == UINT32_MAX) {
                goto done;
            }
            stops = sg_route_vehicle_stop_ptr_const(sol, vehicle_id);
            stop_len = sol->route_stop_lengths[vehicle_id];
            if (sol->request_delivery_stop_pos[r] >= stop_len) {
                goto done;
            }
            if (stops[sol->request_delivery_stop_pos[r]].request_id != r ||
                stops[sol->request_delivery_stop_pos[r]].is_pickup) {
                goto done;
            }
            if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY) {
                if (sol->request_pickup_stop_pos[r] == UINT32_MAX ||
                    sol->request_pickup_stop_pos[r] >= sol->request_delivery_stop_pos[r]) {
                    goto done;
                }
                if (sol->request_pickup_stop_pos[r] >= stop_len) {
                    goto done;
                }
                if (stops[sol->request_pickup_stop_pos[r]].request_id != r ||
                    !stops[sol->request_pickup_stop_pos[r]].is_pickup) {
                    goto done;
                }
            } else if (sol->request_pickup_stop_pos[r] != UINT32_MAX) {
                goto done;
            }
        } else {
            if (sol->request_vehicle[r] != UINT32_MAX || sol->request_pos[r] != UINT32_MAX ||
                sol->request_pickup_stop_pos[r] != UINT32_MAX ||
                sol->request_delivery_stop_pos[r] != UINT32_MAX) {
                goto done;
            }
        }
    }

    ok = 1;

done:
    free(seen_assigned);
    return ok;
}

static double sg_route_solution_cost(const void *solution, void *user_ctx) {
    const SGRouteSolution *sol = (const SGRouteSolution *)solution;
    (void)user_ctx;

    if (!sol) {
        return INFINITY;
    }
    return sg_route_objective_cost(sol->base.num_unassigned, sol->vehicles_used,
                                   sol->total_distance);
}

static int sg_route_solution_size(const void *solution, void *user_ctx) {
    const SGRouteSolution *sol = (const SGRouteSolution *)solution;
    (void)user_ctx;
    return sol ? (int)sol->base.num_assigned : 0;
}

static int sg_route_eval_insertion(const SGContext *ctx, const SGRouteSolution *sol,
                                   uint32_t request_id, uint32_t vehicle_id, uint32_t pos,
                                   uint32_t *candidate_route, double *capacity_scratch,
                                   double *score_out, double *new_route_distance_out) {
    const uint32_t *route;
    uint32_t len;
    double new_distance = 0.0;
    double score;

    if (!ctx || !sol || !candidate_route || !score_out ||
        !new_route_distance_out || vehicle_id >= sol->num_vehicles ||
        request_id >= sol->base.total_requests) {
        return 0;
    }
    (void)capacity_scratch;

    route = sg_route_vehicle_ptr_const(sol, vehicle_id);
    len = sol->route_lengths[vehicle_id];
    if (pos > len || len >= sol->route_stride) {
        return 0;
    }

    if (pos > 0) {
        memcpy(candidate_route, route, (size_t)pos * sizeof(uint32_t));
    }
    candidate_route[pos] = request_id;
    if (pos < len) {
        memcpy(&candidate_route[pos + 1], &route[pos],
               (size_t)(len - pos) * sizeof(uint32_t));
    }

    if (!sg_route_sequence_feasible_distance(ctx, vehicle_id, candidate_route, len + 1,
                                             &new_distance, capacity_scratch)) {
        return 0;
    }

    score = (len == 0 ? SG_ROUTE_OBJECTIVE_VEHICLE_WEIGHT : 0.0) +
            (new_distance - sol->route_distance[vehicle_id]);
    *score_out = score;
    *new_route_distance_out = new_distance;
    return 1;
}

static ARStatus sg_route_apply_insertion(const SGContext *ctx, SGRouteSolution *sol,
                                         uint32_t request_id, uint32_t vehicle_id,
                                         uint32_t pos, double new_route_distance) {
    uint32_t *route;
    uint32_t old_len;
    double old_distance;
    uint32_t r;
    ARStatus status;

    if (!ctx || !sol || request_id >= sol->base.total_requests ||
        vehicle_id >= sol->num_vehicles) {
        return AR_STATUS_INVALID_ARG;
    }
    if (sol->base.assigned_flags[request_id]) {
        return AR_STATUS_INVALID_ARG;
    }

    route = sg_route_vehicle_ptr(sol, vehicle_id);
    old_len = sol->route_lengths[vehicle_id];
    old_distance = sol->route_distance[vehicle_id];
    if (pos > old_len || old_len >= sol->route_stride) {
        return AR_STATUS_INVALID_ARG;
    }

    if (pos < old_len) {
        memmove(&route[pos + 1], &route[pos], (size_t)(old_len - pos) * sizeof(uint32_t));
    }
    route[pos] = request_id;
    sol->route_lengths[vehicle_id] = old_len + 1;

    sol->request_vehicle[request_id] = vehicle_id;
    sol->request_pos[request_id] = pos;
    for (r = pos + 1; r < sol->route_lengths[vehicle_id]; r++) {
        sol->request_pos[route[r]] = r;
    }

    sol->route_distance[vehicle_id] = new_route_distance;
    sol->total_distance += (new_route_distance - old_distance);
    if (old_len == 0) {
        sol->vehicles_used++;
    }

    status = sg_bootstrap_assign_request(&sol->base, request_id);
    if (status != AR_STATUS_OK) {
        return status;
    }

    if (!sg_route_rebuild_vehicle_stop_state(ctx, sol, vehicle_id)) {
        return AR_STATUS_ERROR;
    }

    return AR_STATUS_OK;
}

static ARStatus sg_route_unassign_request(const SGContext *ctx, SGRouteSolution *sol,
                                          uint32_t request_id, double *capacity_scratch) {
    uint32_t vehicle_id;
    uint32_t pos;
    uint32_t *route;
    uint32_t old_len;
    double old_distance;
    double new_distance = 0.0;
    uint32_t r;
    ARStatus status;

    if (!ctx || !sol || request_id >= sol->base.total_requests) {
        return AR_STATUS_INVALID_ARG;
    }
    (void)capacity_scratch;
    if (!sol->base.assigned_flags[request_id]) {
        return AR_STATUS_OK;
    }

    vehicle_id = sol->request_vehicle[request_id];
    pos = sol->request_pos[request_id];
    if (vehicle_id >= sol->num_vehicles) {
        return AR_STATUS_INVALID_ARG;
    }

    route = sg_route_vehicle_ptr(sol, vehicle_id);
    old_len = sol->route_lengths[vehicle_id];
    old_distance = sol->route_distance[vehicle_id];
    if (pos >= old_len) {
        return AR_STATUS_INVALID_ARG;
    }

    if (pos < old_len - 1) {
        memmove(&route[pos], &route[pos + 1], (size_t)(old_len - pos - 1) * sizeof(uint32_t));
    }
    sol->route_lengths[vehicle_id] = old_len - 1;

    for (r = pos; r < sol->route_lengths[vehicle_id]; r++) {
        sol->request_pos[route[r]] = r;
    }
    if (!sg_route_rebuild_vehicle_stop_state(ctx, sol, vehicle_id)) {
        return AR_STATUS_ERROR;
    }

    if (sol->route_stop_lengths[vehicle_id] > 0) {
        if (!sg_route_stop_sequence_feasible(ctx, vehicle_id,
                                             sg_route_vehicle_stop_ptr_const(sol, vehicle_id),
                                             sol->route_stop_lengths[vehicle_id],
                                             &new_distance)) {
            return AR_STATUS_INVALID_ARG;
        }
    } else {
        new_distance = 0.0;
    }

    sol->route_distance[vehicle_id] = new_distance;
    sol->total_distance += (new_distance - old_distance);
    if (old_len > 0 && sol->route_lengths[vehicle_id] == 0) {
        sol->vehicles_used--;
    }

    sol->request_vehicle[request_id] = UINT32_MAX;
    sol->request_pos[request_id] = UINT32_MAX;
    sol->request_pickup_stop_pos[request_id] = UINT32_MAX;
    sol->request_delivery_stop_pos[request_id] = UINT32_MAX;

    status = sg_bootstrap_unassign_request(&sol->base, request_id);
    return status;
}

static ARStatus sg_route_unassign_removed_requests(const SGContext *ctx, SGRouteSolution *sol,
                                                   const uint32_t *removed_ids, int removed_count) {
    int i;
    ARStatus status = AR_STATUS_OK;

    if (!ctx || !sol || removed_count < 0 || (removed_count > 0 && !removed_ids)) {
        return AR_STATUS_INVALID_ARG;
    }

    for (i = 0; i < removed_count; i++) {
        status = sg_route_unassign_request(ctx, sol, removed_ids[i], NULL);
        if (status != AR_STATUS_OK) {
            break;
        }
    }

    return status;
}

static int sg_route_rank_insertions_for_request(SGContext *ctx, const SGRouteSolution *sol,
                                                uint32_t request_id, int regret_k,
                                                double noise_scale, double *best_score_out,
                                                double *kth_score_out,
                                                uint32_t *best_vehicle_out,
                                                uint32_t *best_pos_out,
                                                double *best_route_distance_out) {
    double ranked_scores[SG_ROUTE_MAX_REGRET_K];
    double ranked_noisy[SG_ROUTE_MAX_REGRET_K];
    double ranked_distance[SG_ROUTE_MAX_REGRET_K];
    uint32_t ranked_vehicle[SG_ROUTE_MAX_REGRET_K];
    uint32_t ranked_pos[SG_ROUTE_MAX_REGRET_K];
    uint32_t *candidate_route = NULL;
    double *capacity_scratch = NULL;
    int ranked_count = 0;
    uint32_t v;
    int ok = 0;

    if (!ctx || !sol || !best_score_out || !kth_score_out || !best_vehicle_out ||
        !best_pos_out || !best_route_distance_out || request_id >= sol->base.total_requests ||
        regret_k < 1 || regret_k > SG_ROUTE_MAX_REGRET_K) {
        return 0;
    }

    candidate_route = (uint32_t *)malloc((size_t)(sol->route_stride + 1U) * sizeof(uint32_t));
    if (!candidate_route) {
        return 0;
    }
    if (ctx->dimension_count > 0) {
        capacity_scratch = (double *)malloc((size_t)ctx->dimension_count * sizeof(double));
        if (!capacity_scratch) {
            free(candidate_route);
            return 0;
        }
    }

    for (v = 0; v < sol->num_vehicles; v++) {
        uint32_t len = sol->route_lengths[v];
        uint32_t pos;
        for (pos = 0; pos <= len; pos++) {
            double score = 0.0;
            double noisy = 0.0;
            double new_route_distance = 0.0;
            int insert_at = ranked_count;
            int j;

            if (!sg_route_eval_insertion(ctx, sol, request_id, v, pos,
                                         candidate_route, capacity_scratch,
                                         &score, &new_route_distance)) {
                continue;
            }

            noisy = score;
            if (noise_scale > 0.0 && ctx->op_rng) {
                noisy *= (1.0 + sh_rng_uniform_range(ctx->op_rng, -noise_scale, noise_scale));
            }

            if (insert_at > SG_ROUTE_MAX_REGRET_K) {
                insert_at = SG_ROUTE_MAX_REGRET_K;
            }
            for (j = 0; j < ranked_count && j < SG_ROUTE_MAX_REGRET_K; j++) {
                if (noisy < ranked_noisy[j]) {
                    insert_at = j;
                    break;
                }
            }

            if (insert_at < SG_ROUTE_MAX_REGRET_K) {
                int limit = ranked_count < SG_ROUTE_MAX_REGRET_K
                            ? ranked_count
                            : SG_ROUTE_MAX_REGRET_K - 1;
                for (j = limit; j > insert_at; j--) {
                    ranked_noisy[j] = ranked_noisy[j - 1];
                    ranked_scores[j] = ranked_scores[j - 1];
                    ranked_distance[j] = ranked_distance[j - 1];
                    ranked_vehicle[j] = ranked_vehicle[j - 1];
                    ranked_pos[j] = ranked_pos[j - 1];
                }
                ranked_noisy[insert_at] = noisy;
                ranked_scores[insert_at] = score;
                ranked_distance[insert_at] = new_route_distance;
                ranked_vehicle[insert_at] = v;
                ranked_pos[insert_at] = pos;
            }

            if (ranked_count < SG_ROUTE_MAX_REGRET_K) {
                ranked_count++;
            }
        }
    }

    if (ranked_count > 0) {
        int k_index = regret_k - 1;
        if (k_index >= ranked_count) {
            k_index = ranked_count - 1;
        }

        *best_score_out = ranked_scores[0];
        *kth_score_out = ranked_scores[k_index];
        *best_vehicle_out = ranked_vehicle[0];
        *best_pos_out = ranked_pos[0];
        *best_route_distance_out = ranked_distance[0];
        ok = 1;
    }

    free(capacity_scratch);
    free(candidate_route);
    return ok;
}

static uint32_t sg_route_select_request_greedy(SGContext *ctx, const SGRouteSolution *sol,
                                               double noise_scale, uint32_t *best_vehicle_out,
                                               uint32_t *best_pos_out,
                                               double *best_route_distance_out) {
    uint32_t best_request = UINT32_MAX;
    double best_score = INFINITY;
    uint32_t i;

    if (!ctx || !sol || !best_vehicle_out || !best_pos_out || !best_route_distance_out) {
        return UINT32_MAX;
    }

    for (i = 0; i < sol->base.num_unassigned; i++) {
        uint32_t request_id = sol->base.unassigned_ids[i];
        double first_score = 0.0;
        double kth_score = 0.0;
        uint32_t vehicle_id = UINT32_MAX;
        uint32_t pos = UINT32_MAX;
        double route_distance = 0.0;
        if (!sg_route_rank_insertions_for_request(ctx, sol, request_id, 1, noise_scale,
                                                  &first_score, &kth_score,
                                                  &vehicle_id, &pos, &route_distance)) {
            continue;
        }

        if (first_score < best_score ||
            (fabs(first_score - best_score) <= 1e-9 && request_id < best_request)) {
            best_score = first_score;
            best_request = request_id;
            *best_vehicle_out = vehicle_id;
            *best_pos_out = pos;
            *best_route_distance_out = route_distance;
        }
    }

    return best_request;
}

static uint32_t sg_route_select_request_regret(SGContext *ctx, const SGRouteSolution *sol,
                                               int regret_k, double noise_scale,
                                               uint32_t *best_vehicle_out,
                                               uint32_t *best_pos_out,
                                               double *best_route_distance_out) {
    uint32_t best_request = UINT32_MAX;
    double best_regret = -INFINITY;
    double best_first_score = INFINITY;
    uint32_t i;

    if (!ctx || !sol || !best_vehicle_out || !best_pos_out || !best_route_distance_out ||
        regret_k < 1 || regret_k > SG_ROUTE_MAX_REGRET_K) {
        return UINT32_MAX;
    }

    for (i = 0; i < sol->base.num_unassigned; i++) {
        uint32_t request_id = sol->base.unassigned_ids[i];
        double first_score = 0.0;
        double kth_score = 0.0;
        double regret = 0.0;
        uint32_t vehicle_id = UINT32_MAX;
        uint32_t pos = UINT32_MAX;
        double route_distance = 0.0;
        if (!sg_route_rank_insertions_for_request(ctx, sol, request_id, regret_k, noise_scale,
                                                  &first_score, &kth_score,
                                                  &vehicle_id, &pos, &route_distance)) {
            continue;
        }

        regret = kth_score - first_score;
        if (regret > best_regret ||
            (fabs(regret - best_regret) <= 1e-9 && first_score < best_first_score) ||
            (fabs(regret - best_regret) <= 1e-9 &&
             fabs(first_score - best_first_score) <= 1e-9 &&
             request_id < best_request)) {
            best_regret = regret;
            best_first_score = first_score;
            best_request = request_id;
            *best_vehicle_out = vehicle_id;
            *best_pos_out = pos;
            *best_route_distance_out = route_distance;
        }
    }

    return best_request;
}

static ARStatus sg_route_repair_fill_greedy(SGContext *ctx, SGRouteSolution *sol,
                                            double noise_scale) {
    while (sol->base.num_unassigned > 0) {
        uint32_t vehicle_id = UINT32_MAX;
        uint32_t pos = UINT32_MAX;
        double route_distance = 0.0;
        uint32_t request_id = sg_route_select_request_greedy(ctx, sol, noise_scale,
                                                             &vehicle_id, &pos, &route_distance);
        if (request_id == UINT32_MAX) {
            break;
        }
        if (sg_route_apply_insertion(ctx, sol, request_id, vehicle_id, pos,
                                     route_distance) != AR_STATUS_OK) {
            return AR_STATUS_ERROR;
        }
    }
    return AR_STATUS_OK;
}

static ARStatus sg_route_repair_fill_regret(SGContext *ctx, SGRouteSolution *sol,
                                            int regret_k, double noise_scale) {
    while (sol->base.num_unassigned > 0) {
        uint32_t vehicle_id = UINT32_MAX;
        uint32_t pos = UINT32_MAX;
        double route_distance = 0.0;
        uint32_t request_id = sg_route_select_request_regret(ctx, sol, regret_k, noise_scale,
                                                             &vehicle_id, &pos, &route_distance);
        if (request_id == UINT32_MAX) {
            break;
        }
        if (sg_route_apply_insertion(ctx, sol, request_id, vehicle_id, pos,
                                     route_distance) != AR_STATUS_OK) {
            return AR_STATUS_ERROR;
        }
    }
    return AR_STATUS_OK;
}

static ARStatus sg_route_destroy_random(void *op_ctx, void *solution, int count,
                                        uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    ARStatus status;

    if (!ctx || !ctx->op_rng || !sol || !removed_count || count < 0) {
        return AR_STATUS_INVALID_ARG;
    }

    status = ar_remove_random(ctx->op_rng, sol, count, removed_ids,
                              sg_get_assigned_count, sg_get_assigned_element,
                              NULL, removed_count);
    if (status != AR_STATUS_OK) {
        return status;
    }

    return sg_route_unassign_removed_requests(ctx, sol, removed_ids, *removed_count);
}

static ARStatus sg_route_destroy_worst(void *op_ctx, void *solution, int count,
                                       uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    ARStatus status;

    if (!ctx || !ctx->op_rng || !sol || !removed_count || count < 0) {
        return AR_STATUS_INVALID_ARG;
    }

    status = ar_remove_worst(ctx->op_rng, ctx, sol, count, removed_ids,
                             sg_get_assigned_count, sg_get_assigned_element,
                             sg_bootstrap_removal_cost, SG_WORST_RANDOMNESS,
                             NULL, removed_count);
    if (status != AR_STATUS_OK) {
        return status;
    }

    return sg_route_unassign_removed_requests(ctx, sol, removed_ids, *removed_count);
}

static ARStatus sg_route_destroy_shaw(void *op_ctx, void *solution, int count,
                                      uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    ARStatus status;

    if (!ctx || !ctx->op_rng || !sol || !removed_count || count < 0) {
        return AR_STATUS_INVALID_ARG;
    }

    status = ar_remove_related(ctx->op_rng, ctx, sol, count, removed_ids,
                               sg_get_assigned_count, sg_get_assigned_element,
                               sg_bootstrap_relatedness, SG_SHAW_RANDOMNESS,
                               NULL, removed_count);
    if (status != AR_STATUS_OK) {
        return status;
    }

    return sg_route_unassign_removed_requests(ctx, sol, removed_ids, *removed_count);
}

static ARStatus sg_route_destroy_criticality_worst(void *op_ctx, void *solution, int count,
                                                   uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    ARStatus status;

    if (!ctx || !ctx->op_rng || !sol || !removed_count || count < 0) {
        return AR_STATUS_INVALID_ARG;
    }

    status = ar_remove_worst(ctx->op_rng, ctx, sol, count, removed_ids,
                             sg_get_assigned_count, sg_get_assigned_element,
                             sg_criticality_removal_cost, SG_WORST_RANDOMNESS,
                             NULL, removed_count);
    if (status != AR_STATUS_OK) {
        return status;
    }

    return sg_route_unassign_removed_requests(ctx, sol, removed_ids, *removed_count);
}

static ARStatus sg_route_destroy_route_cluster(void *op_ctx, void *solution, int count,
                                               uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    ARStatus status;

    if (!ctx || !ctx->op_rng || !sol || !removed_count || count < 0) {
        return AR_STATUS_INVALID_ARG;
    }

    status = ar_remove_related(ctx->op_rng, ctx, sol, count, removed_ids,
                               sg_get_assigned_count, sg_get_assigned_element,
                               sg_route_cluster_relatedness, SG_ROUTE_CLUSTER_RANDOMNESS,
                               NULL, removed_count);
    if (status != AR_STATUS_OK) {
        return status;
    }

    return sg_route_unassign_removed_requests(ctx, sol, removed_ids, *removed_count);
}

static ARStatus sg_route_destroy_time_cluster(void *op_ctx, void *solution, int count,
                                              uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    ARStatus status;

    if (!ctx || !ctx->op_rng || !sol || !removed_count || count < 0) {
        return AR_STATUS_INVALID_ARG;
    }

    status = ar_remove_related(ctx->op_rng, ctx, sol, count, removed_ids,
                               sg_get_assigned_count, sg_get_assigned_element,
                               sg_time_cluster_relatedness, SG_TIME_CLUSTER_RANDOMNESS,
                               NULL, removed_count);
    if (status != AR_STATUS_OK) {
        return status;
    }

    return sg_route_unassign_removed_requests(ctx, sol, removed_ids, *removed_count);
}

static ARStatus sg_route_destroy_route_removal(void *op_ctx, void *solution, int count,
                                               uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    int target;
    int total_removed = 0;

    if (!ctx || !ctx->op_rng || !sol || !removed_count || count < 0) {
        return AR_STATUS_INVALID_ARG;
    }

    *removed_count = 0;
    if (count == 0 || sol->base.num_assigned == 0) {
        return AR_STATUS_OK;
    }
    if (!removed_ids) {
        return AR_STATUS_INVALID_ARG;
    }

    target = count;
    if ((uint32_t)target > sol->base.num_assigned) {
        target = (int)sol->base.num_assigned;
    }

    while (total_removed < target) {
        uint32_t selected_vehicle = UINT32_MAX;
        uint32_t seen_nonempty = 0;
        uint32_t route_len;
        uint32_t *route_snapshot;
        uint32_t v;
        int take;
        int i;
        ARStatus status;

        for (v = 0; v < sol->num_vehicles; v++) {
            if (sol->route_lengths[v] == 0) {
                continue;
            }
            seen_nonempty++;
            if (seen_nonempty == 1 ||
                sh_rng_int_range(ctx->op_rng, 0, (int)seen_nonempty - 1) == 0) {
                selected_vehicle = v;
            }
        }

        if (selected_vehicle == UINT32_MAX) {
            break;
        }

        route_len = sol->route_lengths[selected_vehicle];
        if (route_len == 0) {
            continue;
        }

        route_snapshot = (uint32_t *)malloc((size_t)route_len * sizeof(uint32_t));
        if (!route_snapshot) {
            return AR_STATUS_OUT_OF_MEMORY;
        }
        memcpy(route_snapshot, sg_route_vehicle_ptr_const(sol, selected_vehicle),
               (size_t)route_len * sizeof(uint32_t));

        take = target - total_removed;
        if ((uint32_t)take > route_len) {
            take = (int)route_len;
        }

        for (i = 0; i < take; i++) {
            int j = sh_rng_int_range(ctx->op_rng, i, (int)route_len - 1);
            uint32_t tmp = route_snapshot[i];
            route_snapshot[i] = route_snapshot[j];
            route_snapshot[j] = tmp;
            removed_ids[total_removed + i] = route_snapshot[i];
        }

        status = sg_route_unassign_removed_requests(ctx, sol, &removed_ids[total_removed], take);
        free(route_snapshot);
        if (status != AR_STATUS_OK) {
            return status;
        }

        total_removed += take;
    }

    *removed_count = total_removed;
    return AR_STATUS_OK;
}

static ARStatus sg_route_destroy_time_window(void *op_ctx, void *solution, int count,
                                             uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    ARStatus status;

    if (!ctx || !ctx->op_rng || !sol || !removed_count || count < 0) {
        return AR_STATUS_INVALID_ARG;
    }

    status = ar_remove_related(ctx->op_rng, ctx, sol, count, removed_ids,
                               sg_get_assigned_count, sg_get_assigned_element,
                               sg_time_window_relatedness, SG_TIME_CLUSTER_RANDOMNESS,
                               NULL, removed_count);
    if (status != AR_STATUS_OK) {
        return status;
    }

    return sg_route_unassign_removed_requests(ctx, sol, removed_ids, *removed_count);
}

static ARStatus sg_route_destroy_paired_shaw(void *op_ctx, void *solution, int count,
                                             uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    ARStatus status;

    if (!ctx || !ctx->op_rng || !sol || !removed_count || count < 0) {
        return AR_STATUS_INVALID_ARG;
    }

    status = ar_remove_related(ctx->op_rng, ctx, sol, count, removed_ids,
                               sg_get_assigned_count, sg_get_assigned_element,
                               sg_pd_shaw_relatedness, SG_PD_SHAW_RANDOMNESS,
                               NULL, removed_count);
    if (status != AR_STATUS_OK) {
        return status;
    }

    return sg_route_unassign_removed_requests(ctx, sol, removed_ids, *removed_count);
}

static ARStatus sg_route_repair_greedy(void *op_ctx, void *solution,
                                       const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    (void)removed_ids;
    (void)removed_count;
    return sg_route_repair_fill_greedy(ctx, sol, 0.0);
}

static ARStatus sg_route_repair_regret2(void *op_ctx, void *solution,
                                        const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    (void)removed_ids;
    (void)removed_count;
    return sg_route_repair_fill_regret(ctx, sol, 2, 0.0);
}

static ARStatus sg_route_repair_regret3(void *op_ctx, void *solution,
                                        const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    (void)removed_ids;
    (void)removed_count;
    return sg_route_repair_fill_regret(ctx, sol, 3, 0.0);
}

static ARStatus sg_route_repair_regret4(void *op_ctx, void *solution,
                                        const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    (void)removed_ids;
    (void)removed_count;
    return sg_route_repair_fill_regret(ctx, sol, 4, 0.0);
}

static ARStatus sg_route_repair_noise_regret(void *op_ctx, void *solution,
                                             const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    (void)removed_ids;
    (void)removed_count;
    return sg_route_repair_fill_regret(ctx, sol, 3, SG_NOISE_REGRET_SCALE);
}

static ARStatus sg_route_repair_pair_sync(void *op_ctx, void *solution,
                                          const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    (void)removed_ids;
    (void)removed_count;
    return sg_route_repair_fill_greedy(ctx, sol, 0.0);
}

static int sg_route_find_best_insertion_for_request(const SGContext *ctx,
                                                    const SGRouteSolution *sol,
                                                    uint32_t request_id,
                                                    uint32_t forbidden_vehicle,
                                                    uint32_t *best_vehicle_out,
                                                    uint32_t *best_pos_out,
                                                    double *best_route_distance_out) {
    uint32_t *candidate_route = NULL;
    double *capacity_scratch = NULL;
    size_t scratch_count;
    double best_score = INFINITY;
    uint32_t v;
    int found = 0;

    if (!ctx || !sol || !best_vehicle_out || !best_pos_out || !best_route_distance_out ||
        request_id >= sol->base.total_requests) {
        return 0;
    }

    candidate_route = (uint32_t *)malloc((size_t)(sol->route_stride + 1U) * sizeof(uint32_t));
    if (!candidate_route) {
        return 0;
    }
    scratch_count = ctx->dimension_count > 0 ? (size_t)ctx->dimension_count : 1U;
    capacity_scratch = (double *)malloc(scratch_count * sizeof(double));
    if (!capacity_scratch) {
        free(candidate_route);
        return 0;
    }

    for (v = 0; v < sol->num_vehicles; v++) {
        uint32_t len;
        uint32_t pos;
        if (v == forbidden_vehicle) {
            continue;
        }

        len = sol->route_lengths[v];
        for (pos = 0; pos <= len; pos++) {
            double score = 0.0;
            double new_route_distance = 0.0;
            if (!sg_route_eval_insertion(ctx, sol, request_id, v, pos,
                                         candidate_route, capacity_scratch,
                                         &score, &new_route_distance)) {
                continue;
            }

            if (!found || score < best_score ||
                (fabs(score - best_score) <= 1e-9 && v < *best_vehicle_out) ||
                (fabs(score - best_score) <= 1e-9 && v == *best_vehicle_out &&
                 pos < *best_pos_out)) {
                found = 1;
                best_score = score;
                *best_vehicle_out = v;
                *best_pos_out = pos;
                *best_route_distance_out = new_route_distance;
            }
        }
    }

    free(capacity_scratch);
    free(candidate_route);
    return found;
}

static int sg_route_find_best_insertion_no_new_vehicle(const SGContext *ctx,
                                                       const SGRouteSolution *sol,
                                                       uint32_t request_id,
                                                       uint32_t empty_route_ok_vehicle,
                                                       uint32_t *best_vehicle_out,
                                                       uint32_t *best_pos_out,
                                                       double *best_route_distance_out) {
    uint32_t *candidate_route = NULL;
    double *capacity_scratch = NULL;
    size_t scratch_count;
    double best_score = INFINITY;
    uint32_t v;
    int found = 0;

    if (!ctx || !sol || !best_vehicle_out || !best_pos_out || !best_route_distance_out ||
        request_id >= sol->base.total_requests) {
        return 0;
    }

    candidate_route = (uint32_t *)malloc((size_t)(sol->route_stride + 1U) * sizeof(uint32_t));
    if (!candidate_route) {
        return 0;
    }
    scratch_count = ctx->dimension_count > 0 ? (size_t)ctx->dimension_count : 1U;
    capacity_scratch = (double *)malloc(scratch_count * sizeof(double));
    if (!capacity_scratch) {
        free(candidate_route);
        return 0;
    }

    for (v = 0; v < sol->num_vehicles; v++) {
        uint32_t len;
        uint32_t pos;

        len = sol->route_lengths[v];
        if (len == 0 && v != empty_route_ok_vehicle) {
            continue;
        }

        for (pos = 0; pos <= len; pos++) {
            double score = 0.0;
            double new_route_distance = 0.0;
            if (!sg_route_eval_insertion(ctx, sol, request_id, v, pos,
                                         candidate_route, capacity_scratch,
                                         &score, &new_route_distance)) {
                continue;
            }

            if (!found || score < best_score ||
                (fabs(score - best_score) <= 1e-9 && v < *best_vehicle_out) ||
                (fabs(score - best_score) <= 1e-9 && v == *best_vehicle_out &&
                 pos < *best_pos_out)) {
                found = 1;
                best_score = score;
                *best_vehicle_out = v;
                *best_pos_out = pos;
                *best_route_distance_out = new_route_distance;
            }
        }
    }

    free(capacity_scratch);
    free(candidate_route);
    return found;
}

static void sg_route_restore_from_backup(SGRouteSolution *sol, SGRouteSolution *backup) {
    if (!sol || !backup) {
        return;
    }
    sg_route_solution_reset(sol);
    *sol = *backup;
    free(backup);
}

static ARStatus sg_route_try_eliminate_vehicle(const SGContext *ctx,
                                               SGRouteSolution *sol,
                                               uint32_t vehicle_id) {
    uint32_t route_len;
    uint32_t *removed_requests = NULL;
    ARStatus status = AR_STATUS_OK;
    uint32_t i;

    if (!ctx || !sol || vehicle_id >= sol->num_vehicles) {
        return AR_STATUS_INVALID_ARG;
    }

    route_len = sol->route_lengths[vehicle_id];
    if (route_len == 0) {
        return AR_STATUS_OK;
    }

    removed_requests = (uint32_t *)malloc((size_t)route_len * sizeof(uint32_t));
    if (!removed_requests) {
        return AR_STATUS_OUT_OF_MEMORY;
    }
    memcpy(removed_requests, sg_route_vehicle_ptr_const(sol, vehicle_id),
           (size_t)route_len * sizeof(uint32_t));

    /* Insert tighter windows first to reduce dead-end reinsertion failures. */
    for (i = 1; i < route_len; i++) {
        uint32_t key = removed_requests[i];
        int32_t key_width = INT32_MAX;
        uint32_t j = i;
        (void)sg_request_tw_width(ctx, key, &key_width);

        while (j > 0) {
            uint32_t prev = removed_requests[j - 1];
            int32_t prev_width = INT32_MAX;
            (void)sg_request_tw_width(ctx, prev, &prev_width);
            if (prev_width < key_width ||
                (prev_width == key_width && prev <= key)) {
                break;
            }
            removed_requests[j] = prev;
            j--;
        }
        removed_requests[j] = key;
    }

    status = sg_route_unassign_removed_requests(ctx, sol, removed_requests, (int)route_len);
    if (status != AR_STATUS_OK) {
        free(removed_requests);
        return status;
    }

    for (i = 0; i < route_len; i++) {
        uint32_t request_id = removed_requests[i];
        uint32_t best_vehicle = UINT32_MAX;
        uint32_t best_pos = UINT32_MAX;
        double best_route_distance = 0.0;
        if (!sg_route_find_best_insertion_for_request(ctx, sol, request_id, vehicle_id,
                                                      &best_vehicle, &best_pos,
                                                      &best_route_distance)) {
            free(removed_requests);
            return AR_STATUS_LIMIT;
        }
        status = sg_route_apply_insertion(ctx, sol, request_id, best_vehicle, best_pos,
                                          best_route_distance);
        if (status != AR_STATUS_OK) {
            free(removed_requests);
            return status;
        }
    }

    free(removed_requests);
    return sol->route_lengths[vehicle_id] == 0 ? AR_STATUS_OK : AR_STATUS_LIMIT;
}

static ARStatus sg_route_postprocess_reduce_vehicles(const SGContext *ctx,
                                                     SGRouteSolution *sol) {
    uint8_t *tried = NULL;
    int improved = 1;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }

    if (sol->num_vehicles > 0) {
        tried = (uint8_t *)malloc((size_t)sol->num_vehicles * sizeof(uint8_t));
        if (!tried) {
            return AR_STATUS_OUT_OF_MEMORY;
        }
    }

    while (improved) {
        uint32_t attempts = 0;
        improved = 0;
        if (tried && sol->num_vehicles > 0) {
            memset(tried, 0, (size_t)sol->num_vehicles * sizeof(uint8_t));
        }

        while (attempts < sol->num_vehicles) {
            uint32_t selected_vehicle = UINT32_MAX;
            uint32_t selected_len = UINT32_MAX;
            uint32_t v;
            SGRouteSolution *backup;
            double before_cost;
            double max_distance_after;
            ARStatus status;

            for (v = 0; v < sol->num_vehicles; v++) {
                uint32_t len = sol->route_lengths[v];
                if ((tried && tried[v]) || len == 0) {
                    continue;
                }
                if (len < selected_len ||
                    (len == selected_len && (selected_vehicle == UINT32_MAX || v < selected_vehicle))) {
                    selected_len = len;
                    selected_vehicle = v;
                }
            }

            if (selected_vehicle == UINT32_MAX) {
                break;
            }
            if (tried) {
                tried[selected_vehicle] = 1;
            }
            attempts++;

            backup = (SGRouteSolution *)sg_route_solution_copy(sol, (void *)ctx);
            if (!backup) {
                free(tried);
                return AR_STATUS_OUT_OF_MEMORY;
            }
            before_cost = sg_route_solution_cost(backup, (void *)ctx);
            max_distance_after = backup->total_distance * 1.12 + 50.0;

            status = sg_route_try_eliminate_vehicle(ctx, sol, selected_vehicle);
            if (status == AR_STATUS_OK &&
                sol->vehicles_used + 1 == backup->vehicles_used &&
                sol->total_distance <= max_distance_after &&
                sg_route_solution_cost(sol, (void *)ctx) < before_cost - 1e-9) {
                sg_route_solution_free(backup, NULL);
                improved = 1;
                break;
            }

            sg_route_restore_from_backup(sol, backup);
        }
    }

    free(tried);
    return AR_STATUS_OK;
}

static ARStatus sg_route_postprocess_polish_distance(const SGContext *ctx,
                                                     SGRouteSolution *sol) {
    uint32_t max_vehicles;
    uint32_t pass;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }
    if (sol->base.num_assigned == 0) {
        return AR_STATUS_OK;
    }

    max_vehicles = sol->vehicles_used;
    for (pass = 0; pass < 3; pass++) {
        uint32_t count = sol->base.num_assigned;
        uint32_t *requests;
        uint32_t i;
        int improved = 0;

        if (count == 0) {
            break;
        }
        requests = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
        if (!requests) {
            return AR_STATUS_OUT_OF_MEMORY;
        }
        memcpy(requests, sol->base.assigned_ids, (size_t)count * sizeof(uint32_t));

        for (i = 0; i < count; i++) {
            uint32_t request_id = requests[i];
            uint32_t original_vehicle;
            SGRouteSolution *backup;
            double before_cost;
            ARStatus status;
            uint32_t best_vehicle = UINT32_MAX;
            uint32_t best_pos = UINT32_MAX;
            double best_route_distance = 0.0;

            if (request_id >= sol->base.total_requests || !sol->base.assigned_flags[request_id]) {
                continue;
            }

            original_vehicle = sol->request_vehicle[request_id];
            backup = (SGRouteSolution *)sg_route_solution_copy(sol, (void *)ctx);
            if (!backup) {
                free(requests);
                return AR_STATUS_OUT_OF_MEMORY;
            }
            before_cost = sg_route_solution_cost(backup, (void *)ctx);

            status = sg_route_unassign_removed_requests(ctx, sol, &request_id, 1);
            if (status != AR_STATUS_OK) {
                sg_route_restore_from_backup(sol, backup);
                continue;
            }

            if (!sg_route_find_best_insertion_no_new_vehicle(ctx, sol, request_id,
                                                             original_vehicle,
                                                             &best_vehicle, &best_pos,
                                                             &best_route_distance)) {
                sg_route_restore_from_backup(sol, backup);
                continue;
            }

            status = sg_route_apply_insertion(ctx, sol, request_id, best_vehicle, best_pos,
                                              best_route_distance);
            if (status != AR_STATUS_OK) {
                sg_route_restore_from_backup(sol, backup);
                continue;
            }

            if (sol->base.num_unassigned != 0 ||
                sol->vehicles_used > max_vehicles ||
                sg_route_solution_cost(sol, (void *)ctx) >= before_cost - 1e-9) {
                sg_route_restore_from_backup(sol, backup);
                continue;
            }

            if (sol->vehicles_used < max_vehicles) {
                max_vehicles = sol->vehicles_used;
            }
            sg_route_solution_free(backup, NULL);
            improved = 1;
        }

        free(requests);
        if (!improved) {
            break;
        }
    }

    return AR_STATUS_OK;
}

static int sg_route_try_exchange_once(const SGContext *ctx, SGRouteSolution *sol) {
    uint32_t count;
    uint32_t *requests = NULL;
    uint32_t i;
    uint32_t j;
    int improved = 0;

    if (!ctx || !sol) {
        return 0;
    }
    count = sol->base.num_assigned;
    if (count < 2) {
        return 0;
    }

    requests = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
    if (!requests) {
        return 0;
    }
    memcpy(requests, sol->base.assigned_ids, (size_t)count * sizeof(uint32_t));

    for (i = 0; i < count && !improved; i++) {
        uint32_t req_a = requests[i];
        if (req_a >= sol->base.total_requests || !sol->base.assigned_flags[req_a]) {
            continue;
        }

        for (j = i + 1; j < count; j++) {
            uint32_t req_b = requests[j];
            uint32_t ids[2];
            uint32_t vehicle_a;
            uint32_t vehicle_b;
            uint32_t best_vehicle_a = UINT32_MAX;
            uint32_t best_pos_a = UINT32_MAX;
            uint32_t best_vehicle_b = UINT32_MAX;
            uint32_t best_pos_b = UINT32_MAX;
            double best_dist_a = 0.0;
            double best_dist_b = 0.0;
            double before_cost;
            SGRouteSolution *backup;
            ARStatus status;

            if (req_b >= sol->base.total_requests || !sol->base.assigned_flags[req_b]) {
                continue;
            }

            vehicle_a = sol->request_vehicle[req_a];
            vehicle_b = sol->request_vehicle[req_b];
            if (vehicle_a >= sol->num_vehicles || vehicle_b >= sol->num_vehicles) {
                continue;
            }

            backup = (SGRouteSolution *)sg_route_solution_copy(sol, (void *)ctx);
            if (!backup) {
                continue;
            }
            before_cost = sg_route_solution_cost(backup, (void *)ctx);

            ids[0] = req_a;
            ids[1] = req_b;
            status = sg_route_unassign_removed_requests(ctx, sol, ids, 2);
            if (status != AR_STATUS_OK) {
                sg_route_restore_from_backup(sol, backup);
                continue;
            }

            if (!sg_route_find_best_insertion_no_new_vehicle(ctx, sol, req_a, vehicle_b,
                                                             &best_vehicle_a, &best_pos_a,
                                                             &best_dist_a)) {
                sg_route_restore_from_backup(sol, backup);
                continue;
            }
            status = sg_route_apply_insertion(ctx, sol, req_a, best_vehicle_a, best_pos_a,
                                              best_dist_a);
            if (status != AR_STATUS_OK) {
                sg_route_restore_from_backup(sol, backup);
                continue;
            }

            if (!sg_route_find_best_insertion_no_new_vehicle(ctx, sol, req_b, vehicle_a,
                                                             &best_vehicle_b, &best_pos_b,
                                                             &best_dist_b)) {
                sg_route_restore_from_backup(sol, backup);
                continue;
            }
            status = sg_route_apply_insertion(ctx, sol, req_b, best_vehicle_b, best_pos_b,
                                              best_dist_b);
            if (status != AR_STATUS_OK) {
                sg_route_restore_from_backup(sol, backup);
                continue;
            }

            if (sol->base.num_unassigned == 0 &&
                sg_route_solution_cost(sol, (void *)ctx) < before_cost - 1e-9) {
                sg_route_solution_free(backup, NULL);
                improved = 1;
                break;
            }

            sg_route_restore_from_backup(sol, backup);
        }
    }

    free(requests);
    return improved;
}

static int sg_route_try_2opt_star_once(const SGContext *ctx, SGRouteSolution *sol) {
    uint32_t *candidate_a = NULL;
    uint32_t *candidate_b = NULL;
    uint32_t va;
    int improved = 0;

    if (!ctx || !sol || sol->num_vehicles < 2 || sol->route_stride == 0) {
        return 0;
    }

    candidate_a = (uint32_t *)malloc((size_t)sol->route_stride * sizeof(uint32_t));
    candidate_b = (uint32_t *)malloc((size_t)sol->route_stride * sizeof(uint32_t));
    if (!candidate_a || !candidate_b) {
        free(candidate_a);
        free(candidate_b);
        return 0;
    }

    for (va = 0; va < sol->num_vehicles && !improved; va++) {
        uint32_t vb;
        uint32_t len_a = sol->route_lengths[va];
        if (len_a < 2) {
            continue;
        }

        for (vb = va + 1; vb < sol->num_vehicles; vb++) {
            uint32_t len_b = sol->route_lengths[vb];
            const uint32_t *route_a;
            const uint32_t *route_b;
            uint32_t cut_a;
            if (len_b < 2) {
                continue;
            }

            route_a = sg_route_vehicle_ptr_const(sol, va);
            route_b = sg_route_vehicle_ptr_const(sol, vb);

            for (cut_a = 1; cut_a < len_a && !improved; cut_a++) {
                uint32_t cut_b;
                for (cut_b = 1; cut_b < len_b; cut_b++) {
                    uint32_t new_len_a = cut_a + (len_b - cut_b);
                    uint32_t new_len_b = cut_b + (len_a - cut_a);
                    double new_dist_a = 0.0;
                    double new_dist_b = 0.0;
                    double new_total;
                    uint32_t r;

                    if (new_len_a > sol->route_stride || new_len_b > sol->route_stride) {
                        continue;
                    }

                    memcpy(candidate_a, route_a, (size_t)cut_a * sizeof(uint32_t));
                    memcpy(&candidate_a[cut_a], &route_b[cut_b],
                           (size_t)(len_b - cut_b) * sizeof(uint32_t));
                    memcpy(candidate_b, route_b, (size_t)cut_b * sizeof(uint32_t));
                    memcpy(&candidate_b[cut_b], &route_a[cut_a],
                           (size_t)(len_a - cut_a) * sizeof(uint32_t));

                    if (!sg_route_sequence_feasible_distance(ctx, va, candidate_a, new_len_a,
                                                             &new_dist_a, NULL) ||
                        !sg_route_sequence_feasible_distance(ctx, vb, candidate_b, new_len_b,
                                                             &new_dist_b, NULL)) {
                        continue;
                    }

                    new_total = sol->total_distance - sol->route_distance[va] -
                                sol->route_distance[vb] + new_dist_a + new_dist_b;
                    if (new_total >= sol->total_distance - 1e-9) {
                        continue;
                    }

                    {
                        SGRouteSolution *backup =
                            (SGRouteSolution *)sg_route_solution_copy(sol, (void *)ctx);
                        if (!backup) {
                            continue;
                        }

                        memcpy(sg_route_vehicle_ptr(sol, va), candidate_a,
                               (size_t)new_len_a * sizeof(uint32_t));
                        memcpy(sg_route_vehicle_ptr(sol, vb), candidate_b,
                               (size_t)new_len_b * sizeof(uint32_t));
                        sol->route_lengths[va] = new_len_a;
                        sol->route_lengths[vb] = new_len_b;

                        for (r = 0; r < sol->base.total_requests; r++) {
                            if (sol->request_vehicle[r] == va || sol->request_vehicle[r] == vb) {
                                sol->request_vehicle[r] = UINT32_MAX;
                                sol->request_pos[r] = UINT32_MAX;
                                sol->request_pickup_stop_pos[r] = UINT32_MAX;
                                sol->request_delivery_stop_pos[r] = UINT32_MAX;
                            }
                        }

                        for (r = 0; r < new_len_a; r++) {
                            uint32_t req = candidate_a[r];
                            sol->request_vehicle[req] = va;
                            sol->request_pos[req] = r;
                        }
                        for (r = 0; r < new_len_b; r++) {
                            uint32_t req = candidate_b[r];
                            sol->request_vehicle[req] = vb;
                            sol->request_pos[req] = r;
                        }

                        if (!sg_route_rebuild_vehicle_stop_state(ctx, sol, va) ||
                            !sg_route_rebuild_vehicle_stop_state(ctx, sol, vb)) {
                            sg_route_restore_from_backup(sol, backup);
                            continue;
                        }

                        sol->route_distance[va] = new_dist_a;
                        sol->route_distance[vb] = new_dist_b;
                        sol->total_distance = new_total;
                        sg_route_solution_free(backup, NULL);
                        improved = 1;
                        break;
                    }
                }
            }
        }
    }

    free(candidate_a);
    free(candidate_b);
    return improved;
}

static ARStatus sg_route_postprocess_intensify(const SGContext *ctx, SGRouteSolution *sol) {
    uint32_t pass;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }

    for (pass = 0; pass < SG_ROUTE_MAX_INTENSIFY_PASSES; pass++) {
        int improved = 0;
        if (sg_route_try_exchange_once(ctx, sol)) {
            improved = 1;
        }
        if (sg_route_try_2opt_star_once(ctx, sol)) {
            improved = 1;
        }
        if (!improved) {
            break;
        }
    }

    return AR_STATUS_OK;
}

static int sg_route_solver_eligible(const SGContext *ctx) {
    uint32_t i;

    if (!ctx || ctx->num_requests == 0 || ctx->num_vehicles == 0) {
        return 0;
    }

    for (i = 0; i < ctx->num_requests; i++) {
        const SGRequestRecord *request = &ctx->requests[i];
        if (request->kind == SG_REQUEST_KIND_DELIVERY_ONLY) {
            if (!request->has_delivery_task || request->delivery_task_id >= ctx->num_tasks ||
                ctx->tasks[request->delivery_task_id].type != SG_TASK_DELIVERY) {
                return 0;
            }
            continue;
        }

        if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY) {
            if (!request->has_pickup_task || !request->has_delivery_task ||
                request->pickup_task_id >= ctx->num_tasks ||
                request->delivery_task_id >= ctx->num_tasks) {
                return 0;
            }
            if (ctx->tasks[request->pickup_task_id].type != SG_TASK_PICKUP ||
                ctx->tasks[request->delivery_task_id].type != SG_TASK_DELIVERY) {
                return 0;
            }
            continue;
        }

        if (request->kind == SG_REQUEST_KIND_UNBOUND) {
            return 0;
        }

        if (!request->has_delivery_task || request->delivery_task_id >= ctx->num_tasks) {
            return 0;
        }
    }

    return 1;
}

static ARStatus sg_route_construct_initial_solution(SGContext *ctx, SGRouteSolution *sol) {
    return sg_route_repair_fill_regret(ctx, sol, 3, 0.0);
}

static SGStatus sg_solve_route_model(SGContext *ctx) {
    ARALNSParams params;
    ARSolutionOps ops;
    ARALNSContext *alns = NULL;
    SGRouteSolution initial;
    SGRouteSolution *best = NULL;
    ARStatus ar_status;
    ARALNSStats ar_stats;
    ARStatus init_status;
    const SGRouteSolution *final_sol;

    if (!ctx) {
        return SG_STATUS_INVALID_ARG;
    }

    ar_alns_params_default(&params);
    params.max_iterations = ctx->config.max_iterations;
    params.max_time_seconds = ctx->config.max_time_seconds;
    params.segment_size = ctx->config.segment_size;
    params.q_min = ctx->config.q_min;
    params.q_max = ctx->config.q_max;
    params.target_cost = 0.0;
    params.accept_type = AR_ACCEPT_IMPROVING;

    ops.copy = sg_route_solution_copy;
    ops.free = sg_route_solution_free;
    ops.cost = sg_route_solution_cost;
    ops.size = sg_route_solution_size;
    ops.validate = sg_route_solution_validate;
    ops.user_ctx = ctx;

    init_status = sg_route_solution_init(ctx, &initial);
    if (init_status != AR_STATUS_OK) {
        return SG_STATUS_OUT_OF_MEMORY;
    }

    init_status = sg_route_construct_initial_solution(ctx, &initial);
    if (init_status != AR_STATUS_OK) {
            sg_route_solution_reset(&initial);
        return init_status == AR_STATUS_OUT_OF_MEMORY ? SG_STATUS_OUT_OF_MEMORY
                                                      : SG_STATUS_ERROR;
    }

    alns = ar_alns_create(&params, &ops, ctx);
    if (!alns) {
        sg_route_solution_reset(&initial);
        return SG_STATUS_OUT_OF_MEMORY;
    }

    if (ctx->config.deterministic) {
        ar_alns_set_seed(alns, ctx->config.seed);
        sh_rng_seed(ctx->op_rng, ctx->config.seed ^ SG_OPERATOR_SEED_XOR);
    } else {
        sh_rng_seed_time(ctx->op_rng);
    }

    if (ar_alns_add_destroy(alns, "random", sg_route_destroy_random, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_destroy(alns, "criticality-worst", sg_route_destroy_criticality_worst, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_destroy(alns, "route-removal", sg_route_destroy_route_removal, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_destroy(alns, "time-window-removal", sg_route_destroy_time_window, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_destroy(alns, "route-cluster", sg_route_destroy_route_cluster, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_destroy(alns, "time-cluster", sg_route_destroy_time_cluster, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_destroy(alns, "paired-shaw", sg_route_destroy_paired_shaw, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_destroy(alns, "worst", sg_route_destroy_worst, ctx, 0.5) != AR_STATUS_OK ||
        ar_alns_add_destroy(alns, "shaw", sg_route_destroy_shaw, ctx, 0.5) != AR_STATUS_OK ||
        ar_alns_add_repair(alns, "greedy-insert", sg_route_repair_greedy, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_repair(alns, "regret-2", sg_route_repair_regret2, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_repair(alns, "regret-3", sg_route_repair_regret3, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_repair(alns, "regret-4", sg_route_repair_regret4, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_repair(alns, "noise-regret", sg_route_repair_noise_regret, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_repair(alns, "pair-sync", sg_route_repair_pair_sync, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_repair(alns, "bootstrap-repair", sg_route_repair_greedy, ctx, 0.5) != AR_STATUS_OK) {
        sg_route_solution_reset(&initial);
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_solve(alns, &initial, (void **)&best);
    if (ar_status != AR_STATUS_OK && ar_status != AR_STATUS_LIMIT) {
        sg_route_solution_reset(&initial);
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    if (best) {
        (void)sg_route_postprocess_reduce_vehicles(ctx, best);
        (void)sg_route_postprocess_intensify(ctx, best);
        (void)sg_route_postprocess_polish_distance(ctx, best);
    } else {
        (void)sg_route_postprocess_reduce_vehicles(ctx, &initial);
        (void)sg_route_postprocess_intensify(ctx, &initial);
        (void)sg_route_postprocess_polish_distance(ctx, &initial);
    }

    final_sol = best ? best : &initial;
    ar_alns_get_stats(alns, &ar_stats);
    ctx->stats.iterations = ar_stats.iterations;
    ctx->stats.unassigned = final_sol->base.num_unassigned;
    ctx->stats.vehicles_used = final_sol->vehicles_used;
    ctx->stats.total_distance = final_sol->total_distance;
    ctx->stats.total_cost = sg_route_solution_cost(final_sol, ctx);

    sg_route_solution_reset(&initial);
    sg_route_solution_free(best, NULL);
    ar_alns_free(alns);

    if (ar_status == AR_STATUS_LIMIT) {
        return SG_STATUS_LIMIT;
    }
    return SG_STATUS_OK;
}

const char *sg_version(void) {
    return "0.1.0-dev";
}

void sg_config_default(SGConfig *config) {
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->max_iterations = 1000;
    config->max_time_seconds = 0;
    config->segment_size = 100;
    config->q_min = 4;
    config->q_max = 20;
    config->seed = 0xDEADBEEF;
    config->deterministic = true;
    config->require_bound_requests_at_solve = true;
    config->priority_removal_policy = SG_PRIORITY_REMOVE_LOWER_FIRST;
}

SGContext *sg_create(void) {
    SGContext *ctx = (SGContext *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }

    ctx->op_rng = sh_rng_create_default();
    if (!ctx->op_rng) {
        free(ctx);
        return NULL;
    }

    sg_config_default(&ctx->config);
    ctx->demand_sign_convention = SG_DEMAND_PICKUP_POSITIVE_DELIVERY_NEGATIVE;
    ctx->dimension_count = 1;
    return ctx;
}

void sg_free(SGContext *ctx) {
    if (!ctx) {
        return;
    }

    free(ctx->depots);
    ctx->depots = NULL;
    ctx->num_depots = 0;

    free(ctx->requests);
    ctx->requests = NULL;

    free(ctx->request_hints);
    ctx->request_hints = NULL;
    ctx->num_requests = 0;

    sg_vehicle_records_free(ctx->vehicles, ctx->num_vehicles);
    ctx->vehicles = NULL;
    ctx->num_vehicles = 0;

    sg_task_records_free(ctx->tasks, ctx->num_tasks);
    ctx->tasks = NULL;
    ctx->num_tasks = 0;

    sg_zone_matrix_clear(ctx);

    sh_rng_free(ctx->op_rng);
    ctx->op_rng = NULL;
    free(ctx);
}

SGStatus sg_set_config(SGContext *ctx, const SGConfig *config) {
    if (!ctx || !sg_config_valid(config)) {
        return SG_STATUS_INVALID_ARG;
    }
    ctx->config = *config;
    return SG_STATUS_OK;
}

SGStatus sg_set_require_bound_requests_at_solve(SGContext *ctx, bool require_bound) {
    if (!ctx) {
        return SG_STATUS_INVALID_ARG;
    }

    ctx->config.require_bound_requests_at_solve = require_bound;
    return SG_STATUS_OK;
}

bool sg_get_require_bound_requests_at_solve(const SGContext *ctx) {
    if (!ctx) {
        return true;
    }
    return ctx->config.require_bound_requests_at_solve;
}

SGStatus sg_set_demand_sign_convention(SGContext *ctx, SGDemandSignConvention convention) {
    if (!ctx || !sg_demand_sign_convention_valid(convention)) {
        return SG_STATUS_INVALID_ARG;
    }

    ctx->demand_sign_convention = convention;
    return SG_STATUS_OK;
}

SGDemandSignConvention sg_get_demand_sign_convention(const SGContext *ctx) {
    if (!ctx) {
        return SG_DEMAND_PICKUP_POSITIVE_DELIVERY_NEGATIVE;
    }
    return ctx->demand_sign_convention;
}

SGStatus sg_set_dimension_count(SGContext *ctx, uint32_t dimension_count) {
    if (!ctx || dimension_count == 0) {
        return SG_STATUS_INVALID_ARG;
    }
    if (ctx->num_vehicles > 0 || ctx->num_tasks > 0) {
        return SG_STATUS_INVALID_ARG;
    }

    ctx->dimension_count = dimension_count;
    return SG_STATUS_OK;
}

uint32_t sg_get_dimension_count(const SGContext *ctx) {
    if (!ctx) {
        return 0;
    }
    return ctx->dimension_count;
}

uint32_t sg_add_depot(SGContext *ctx) {
    SGDepotRecord *new_depots;
    size_t next_count;
    uint32_t id;

    if (!ctx) {
        return UINT32_MAX;
    }

    id = ctx->num_depots;
    next_count = (size_t)ctx->num_depots + 1;
    if (next_count > SIZE_MAX / sizeof(*ctx->depots)) {
        return UINT32_MAX;
    }

    new_depots = (SGDepotRecord *)realloc(ctx->depots, next_count * sizeof(*ctx->depots));
    if (!new_depots) {
        return UINT32_MAX;
    }

    ctx->depots = new_depots;
    memset(&ctx->depots[id], 0, sizeof(ctx->depots[id]));
    ctx->num_depots++;
    return id;
}

SGStatus sg_depot_set_location(SGContext *ctx, uint32_t depot_id, double x, double y) {
    SGDepotRecord *depot;

    if (!ctx || depot_id >= ctx->num_depots || !isfinite(x) || !isfinite(y)) {
        return SG_STATUS_INVALID_ARG;
    }

    depot = &ctx->depots[depot_id];
    depot->x = x;
    depot->y = y;
    depot->has_location = 1;
    return SG_STATUS_OK;
}

SGStatus sg_depot_set_time_window(SGContext *ctx, uint32_t depot_id, int32_t early,
                                  int32_t late) {
    SGDepotRecord *depot;

    if (!ctx || depot_id >= ctx->num_depots || late < early) {
        return SG_STATUS_INVALID_ARG;
    }

    depot = &ctx->depots[depot_id];
    depot->tw_early = early;
    depot->tw_late = late;
    depot->has_time_window = 1;
    return SG_STATUS_OK;
}

uint32_t sg_add_request(SGContext *ctx) {
    SGRequestHint *new_hints;
    SGRequestRecord *new_requests;
    size_t next_count;
    uint32_t id;

    if (!ctx) {
        return UINT32_MAX;
    }

    id = ctx->num_requests;
    next_count = (size_t)ctx->num_requests + 1;
    if (next_count > SIZE_MAX / sizeof(*ctx->request_hints) ||
        next_count > SIZE_MAX / sizeof(*ctx->requests)) {
        return UINT32_MAX;
    }

    new_hints = (SGRequestHint *)realloc(ctx->request_hints,
                                         next_count * sizeof(*ctx->request_hints));
    if (!new_hints) {
        return UINT32_MAX;
    }
    ctx->request_hints = new_hints;

    new_requests = (SGRequestRecord *)realloc(ctx->requests,
                                              next_count * sizeof(*ctx->requests));
    if (!new_requests) {
        return UINT32_MAX;
    }
    ctx->requests = new_requests;

    ctx->request_hints[id] = sg_request_hint_default();
    ctx->requests[id] = sg_request_record_default();
    ctx->num_requests++;
    return id;
}

uint32_t sg_add_vehicle(SGContext *ctx) {
    SGVehicleRecord *new_vehicles;
    SGVehicleRecord *vehicle;
    size_t next_count;
    uint32_t id;

    if (!ctx || ctx->dimension_count == 0) {
        return UINT32_MAX;
    }

    id = ctx->num_vehicles;
    next_count = (size_t)ctx->num_vehicles + 1;
    if (next_count > SIZE_MAX / sizeof(*ctx->vehicles) ||
        (size_t)ctx->dimension_count > SIZE_MAX / sizeof(double)) {
        return UINT32_MAX;
    }

    new_vehicles = (SGVehicleRecord *)realloc(ctx->vehicles,
                                              next_count * sizeof(*ctx->vehicles));
    if (!new_vehicles) {
        return UINT32_MAX;
    }
    ctx->vehicles = new_vehicles;

    vehicle = &ctx->vehicles[id];
    memset(vehicle, 0, sizeof(*vehicle));
    vehicle->capacity = (double *)calloc((size_t)ctx->dimension_count, sizeof(double));
    if (!vehicle->capacity) {
        return UINT32_MAX;
    }

    ctx->num_vehicles++;
    return id;
}

uint32_t sg_add_task(SGContext *ctx, SGTaskType type) {
    SGTaskRecord *new_tasks;
    SGTaskRecord *task;
    size_t next_count;
    uint32_t id;

    if (!ctx || !sg_task_type_valid(type) || ctx->dimension_count == 0) {
        return UINT32_MAX;
    }

    id = ctx->num_tasks;
    next_count = (size_t)ctx->num_tasks + 1;
    if (next_count > SIZE_MAX / sizeof(*ctx->tasks) ||
        (size_t)ctx->dimension_count > SIZE_MAX / sizeof(double)) {
        return UINT32_MAX;
    }

    new_tasks = (SGTaskRecord *)realloc(ctx->tasks, next_count * sizeof(*ctx->tasks));
    if (!new_tasks) {
        return UINT32_MAX;
    }
    ctx->tasks = new_tasks;

    task = &ctx->tasks[id];
    memset(task, 0, sizeof(*task));
    task->type = type;
    task->demand = (double *)calloc((size_t)ctx->dimension_count, sizeof(double));
    if (!task->demand) {
        return UINT32_MAX;
    }

    ctx->num_tasks++;
    return id;
}

SGStatus sg_vehicle_set_depots(SGContext *ctx, uint32_t vehicle_id, uint32_t start_depot_id,
                               uint32_t end_depot_id) {
    SGVehicleRecord *vehicle;

    if (!ctx || vehicle_id >= ctx->num_vehicles || start_depot_id >= ctx->num_depots ||
        end_depot_id >= ctx->num_depots) {
        return SG_STATUS_INVALID_ARG;
    }

    vehicle = &ctx->vehicles[vehicle_id];
    vehicle->start_depot_id = start_depot_id;
    vehicle->end_depot_id = end_depot_id;
    vehicle->has_depots = 1;
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_shift_time_window(SGContext *ctx, uint32_t vehicle_id, int32_t early,
                                          int32_t late) {
    SGVehicleRecord *vehicle;

    if (!ctx || vehicle_id >= ctx->num_vehicles || late < early) {
        return SG_STATUS_INVALID_ARG;
    }

    vehicle = &ctx->vehicles[vehicle_id];
    vehicle->shift_early = early;
    vehicle->shift_late = late;
    vehicle->has_shift_time_window = 1;
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_capacity(SGContext *ctx, uint32_t vehicle_id, const double *capacity,
                                 uint32_t capacity_count) {
    SGVehicleRecord *vehicle;
    uint32_t i;

    if (!ctx || vehicle_id >= ctx->num_vehicles || !capacity ||
        capacity_count != ctx->dimension_count || !ctx->vehicles[vehicle_id].capacity) {
        return SG_STATUS_INVALID_ARG;
    }

    vehicle = &ctx->vehicles[vehicle_id];
    for (i = 0; i < capacity_count; i++) {
        if (!isfinite(capacity[i]) || capacity[i] < 0.0) {
            return SG_STATUS_INVALID_ARG;
        }
    }
    memcpy(vehicle->capacity, capacity, (size_t)capacity_count * sizeof(double));
    vehicle->has_capacity = 1;
    return SG_STATUS_OK;
}

SGStatus sg_task_set_location(SGContext *ctx, uint32_t task_id, double x, double y) {
    SGTaskRecord *task;

    if (!ctx || task_id >= ctx->num_tasks || !isfinite(x) || !isfinite(y)) {
        return SG_STATUS_INVALID_ARG;
    }

    task = &ctx->tasks[task_id];
    task->x = x;
    task->y = y;
    task->has_location = 1;
    return SG_STATUS_OK;
}

SGStatus sg_task_set_time_window(SGContext *ctx, uint32_t task_id, int32_t early, int32_t late) {
    SGTaskRecord *task;

    if (!ctx || task_id >= ctx->num_tasks || late < early) {
        return SG_STATUS_INVALID_ARG;
    }

    task = &ctx->tasks[task_id];
    task->tw_early = early;
    task->tw_late = late;
    task->has_time_window = 1;
    return SG_STATUS_OK;
}

SGStatus sg_task_set_service_seconds(SGContext *ctx, uint32_t task_id, int32_t service_seconds) {
    SGTaskRecord *task;

    if (!ctx || task_id >= ctx->num_tasks || service_seconds < 0) {
        return SG_STATUS_INVALID_ARG;
    }

    task = &ctx->tasks[task_id];
    task->service_seconds = service_seconds;
    return SG_STATUS_OK;
}

SGStatus sg_task_set_demand(SGContext *ctx, uint32_t task_id, const double *demand,
                            uint32_t demand_count) {
    SGTaskRecord *task;
    uint32_t i;

    if (!ctx || task_id >= ctx->num_tasks || !demand || demand_count != ctx->dimension_count ||
        !ctx->tasks[task_id].demand) {
        return SG_STATUS_INVALID_ARG;
    }

    task = &ctx->tasks[task_id];
    for (i = 0; i < demand_count; i++) {
        if (!isfinite(demand[i])) {
            return SG_STATUS_INVALID_ARG;
        }
    }

    memcpy(task->demand, demand, (size_t)demand_count * sizeof(double));
    task->has_demand = 1;
    return SG_STATUS_OK;
}

SGStatus sg_request_bind_delivery_task(SGContext *ctx, uint32_t request_id,
                                       uint32_t delivery_task_id) {
    SGRequestRecord *request;
    SGTaskRecord *delivery;

    if (!ctx || request_id >= ctx->num_requests || delivery_task_id >= ctx->num_tasks) {
        return SG_STATUS_INVALID_ARG;
    }

    request = &ctx->requests[request_id];
    delivery = &ctx->tasks[delivery_task_id];
    if (delivery->type != SG_TASK_DELIVERY) {
        return SG_STATUS_INVALID_ARG;
    }

    request->kind = SG_REQUEST_KIND_DELIVERY_ONLY;
    request->delivery_task_id = delivery_task_id;
    request->has_delivery_task = 1;
    request->pickup_task_id = 0;
    request->has_pickup_task = 0;
    return SG_STATUS_OK;
}

SGStatus sg_request_bind_pickup_delivery_tasks(SGContext *ctx, uint32_t request_id,
                                               uint32_t pickup_task_id,
                                               uint32_t delivery_task_id) {
    SGRequestRecord *request;
    SGTaskRecord *pickup;
    SGTaskRecord *delivery;

    if (!ctx || request_id >= ctx->num_requests || pickup_task_id >= ctx->num_tasks ||
        delivery_task_id >= ctx->num_tasks || pickup_task_id == delivery_task_id) {
        return SG_STATUS_INVALID_ARG;
    }

    request = &ctx->requests[request_id];
    pickup = &ctx->tasks[pickup_task_id];
    delivery = &ctx->tasks[delivery_task_id];
    if (pickup->type != SG_TASK_PICKUP || delivery->type != SG_TASK_DELIVERY) {
        return SG_STATUS_INVALID_ARG;
    }

    request->kind = SG_REQUEST_KIND_PICKUP_DELIVERY;
    request->pickup_task_id = pickup_task_id;
    request->delivery_task_id = delivery_task_id;
    request->has_pickup_task = 1;
    request->has_delivery_task = 1;
    return SG_STATUS_OK;
}

SGStatus sg_request_set_priority_hint(SGContext *ctx, uint32_t request_id, int32_t priority) {
    if (!ctx || request_id >= ctx->num_requests || !ctx->request_hints) {
        return SG_STATUS_INVALID_ARG;
    }

    ctx->request_hints[request_id].priority = sg_clamp_priority(priority);
    return SG_STATUS_OK;
}

SGStatus sg_request_set_time_window_hint(SGContext *ctx, uint32_t request_id,
                                         int32_t early, int32_t late) {
    if (!ctx || request_id >= ctx->num_requests || !ctx->request_hints || late < early) {
        return SG_STATUS_INVALID_ARG;
    }

    ctx->request_hints[request_id].tw_early = early;
    ctx->request_hints[request_id].tw_late = late;
    ctx->request_hints[request_id].has_time_window = 1;
    return SG_STATUS_OK;
}

SGStatus sg_request_set_zone_hint(SGContext *ctx, uint32_t request_id, uint32_t zone_id) {
    if (!ctx || request_id >= ctx->num_requests || !ctx->request_hints) {
        return SG_STATUS_INVALID_ARG;
    }

    ctx->request_hints[request_id].zone_id = zone_id;
    ctx->request_hints[request_id].has_zone = 1;
    return SG_STATUS_OK;
}

SGStatus sg_set_priority_removal_policy(SGContext *ctx, SGPriorityRemovalPolicy policy) {
    if (!ctx || !sg_priority_policy_valid(policy)) {
        return SG_STATUS_INVALID_ARG;
    }

    ctx->config.priority_removal_policy = policy;
    return SG_STATUS_OK;
}

SGStatus sg_set_zone_distance_matrix(SGContext *ctx, uint32_t zone_count,
                                     const double *matrix_row_major) {
    double *copy = NULL;
    size_t i;
    size_t j;
    size_t n;
    size_t total;

    if (!ctx) {
        return SG_STATUS_INVALID_ARG;
    }

    if (zone_count == 0) {
        if (matrix_row_major != NULL) {
            return SG_STATUS_INVALID_ARG;
        }
        sg_zone_matrix_clear(ctx);
        return SG_STATUS_OK;
    }

    if (!matrix_row_major) {
        return SG_STATUS_INVALID_ARG;
    }

    n = (size_t)zone_count;
    if (n > SIZE_MAX / n) {
        return SG_STATUS_INVALID_ARG;
    }
    total = n * n;
    if (total > SIZE_MAX / sizeof(double)) {
        return SG_STATUS_INVALID_ARG;
    }

    copy = (double *)malloc(total * sizeof(double));
    if (!copy) {
        return SG_STATUS_OUT_OF_MEMORY;
    }

    for (i = 0; i < total; i++) {
        double value = matrix_row_major[i];
        if (!isfinite(value) || value < 0.0) {
            free(copy);
            return SG_STATUS_INVALID_ARG;
        }
        copy[i] = value;
    }

    for (i = 0; i < n; i++) {
        copy[i * n + i] = 0.0;
    }

    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            double a = copy[i * n + j];
            double b = copy[j * n + i];
            if (fabs(a - b) > 1e-9) {
                free(copy);
                return SG_STATUS_INVALID_ARG;
            }
        }
    }

    sg_zone_matrix_clear(ctx);
    ctx->zone_distance_matrix = copy;
    ctx->zone_count = zone_count;
    return SG_STATUS_OK;
}

SGStatus sg_clear_zone_distance_matrix(SGContext *ctx) {
    if (!ctx) {
        return SG_STATUS_INVALID_ARG;
    }

    sg_zone_matrix_clear(ctx);
    return SG_STATUS_OK;
}

static int sg_task_ready_for_model(const SGTaskRecord *task) {
    return task && task->has_location && task->has_time_window && task->service_seconds >= 0;
}

static int sg_request_pd_demands_valid(const SGTaskRecord *pickup, const SGTaskRecord *delivery,
                                       uint32_t dimension_count,
                                       SGDemandSignConvention convention) {
    uint32_t dim;
    int has_non_zero_flow = 0;

    if (!pickup || !delivery || !pickup->demand || !delivery->demand || !pickup->has_demand ||
        !delivery->has_demand) {
        return 0;
    }

    for (dim = 0; dim < dimension_count; dim++) {
        double pick = pickup->demand[dim];
        double drop = delivery->demand[dim];

        if (!isfinite(pick) || !isfinite(drop)) {
            return 0;
        }
        if (convention == SG_DEMAND_PICKUP_POSITIVE_DELIVERY_NEGATIVE) {
            if (pick < -SG_DEMAND_TOLERANCE || drop > SG_DEMAND_TOLERANCE) {
                return 0;
            }
        } else {
            if (pick > SG_DEMAND_TOLERANCE || drop < -SG_DEMAND_TOLERANCE) {
                return 0;
            }
        }
        if (fabs(pick + drop) > SG_DEMAND_TOLERANCE) {
            return 0;
        }
        if (fabs(pick) > SG_DEMAND_TOLERANCE || fabs(drop) > SG_DEMAND_TOLERANCE) {
            has_non_zero_flow = 1;
        }
    }

    return has_non_zero_flow;
}

static int sg_delivery_task_demand_valid(const SGTaskRecord *delivery, uint32_t dimension_count,
                                         SGDemandSignConvention convention) {
    uint32_t dim;
    int has_non_zero = 0;

    if (!delivery || !delivery->demand || !delivery->has_demand) {
        return 0;
    }

    for (dim = 0; dim < dimension_count; dim++) {
        double value = delivery->demand[dim];
        if (!isfinite(value)) {
            return 0;
        }
        if (convention == SG_DEMAND_PICKUP_POSITIVE_DELIVERY_NEGATIVE) {
            if (value > SG_DEMAND_TOLERANCE) {
                return 0;
            }
        } else {
            if (value < -SG_DEMAND_TOLERANCE) {
                return 0;
            }
        }
        if (fabs(value) > SG_DEMAND_TOLERANCE) {
            has_non_zero = 1;
        }
    }

    return has_non_zero;
}

static uint32_t sg_count_unbound_requests(const SGContext *ctx) {
    uint32_t i;
    uint32_t count = 0;

    if (!ctx || ctx->num_requests == 0) {
        return 0;
    }
    if (!ctx->requests) {
        return ctx->num_requests;
    }

    for (i = 0; i < ctx->num_requests; i++) {
        if (ctx->requests[i].kind == SG_REQUEST_KIND_UNBOUND) {
            count++;
        }
    }
    return count;
}

SGStatus sg_validate_model(const SGContext *ctx) {
    uint32_t i;
    uint32_t d;

    if (!ctx || ctx->dimension_count == 0) {
        return SG_STATUS_INVALID_ARG;
    }

    if ((ctx->num_depots > 0 && !ctx->depots) ||
        (ctx->num_vehicles > 0 && !ctx->vehicles) ||
        (ctx->num_tasks > 0 && !ctx->tasks) ||
        (ctx->num_requests > 0 && (!ctx->requests || !ctx->request_hints))) {
        return SG_STATUS_INFEASIBLE;
    }

    for (i = 0; i < ctx->num_depots; i++) {
        const SGDepotRecord *depot = &ctx->depots[i];
        if (!depot->has_location) {
            return SG_STATUS_INFEASIBLE;
        }
        if (depot->has_time_window && depot->tw_late < depot->tw_early) {
            return SG_STATUS_INFEASIBLE;
        }
    }

    for (i = 0; i < ctx->num_vehicles; i++) {
        const SGVehicleRecord *vehicle = &ctx->vehicles[i];

        if (!vehicle->capacity) {
            return SG_STATUS_INFEASIBLE;
        }
        if (ctx->num_depots > 0 && !vehicle->has_depots) {
            return SG_STATUS_INFEASIBLE;
        }
        if (vehicle->has_depots &&
            (vehicle->start_depot_id >= ctx->num_depots || vehicle->end_depot_id >= ctx->num_depots)) {
            return SG_STATUS_INFEASIBLE;
        }
        if (vehicle->has_shift_time_window && vehicle->shift_late < vehicle->shift_early) {
            return SG_STATUS_INFEASIBLE;
        }

        for (d = 0; d < ctx->dimension_count; d++) {
            double cap = vehicle->capacity[d];
            if (!isfinite(cap) || cap < 0.0) {
                return SG_STATUS_INFEASIBLE;
            }
        }
    }

    for (i = 0; i < ctx->num_tasks; i++) {
        const SGTaskRecord *task = &ctx->tasks[i];

        if (!sg_task_ready_for_model(task)) {
            return SG_STATUS_INFEASIBLE;
        }
        if (task->tw_late < task->tw_early) {
            return SG_STATUS_INFEASIBLE;
        }
        if (!task->demand) {
            return SG_STATUS_INFEASIBLE;
        }
        if (task->type != SG_TASK_SERVICE && !task->has_demand) {
            return SG_STATUS_INFEASIBLE;
        }
        if (task->has_demand) {
            for (d = 0; d < ctx->dimension_count; d++) {
                if (!isfinite(task->demand[d])) {
                    return SG_STATUS_INFEASIBLE;
                }
            }
        }
    }

    for (i = 0; i < ctx->num_requests; i++) {
        const SGRequestRecord *request = &ctx->requests[i];

        if (request->kind == SG_REQUEST_KIND_UNBOUND) {
            continue;
        }

        if (request->kind == SG_REQUEST_KIND_DELIVERY_ONLY) {
            const SGTaskRecord *delivery;
            if (!request->has_delivery_task || request->delivery_task_id >= ctx->num_tasks) {
                return SG_STATUS_INFEASIBLE;
            }
            delivery = &ctx->tasks[request->delivery_task_id];
            if (delivery->type != SG_TASK_DELIVERY) {
                return SG_STATUS_INFEASIBLE;
            }
            if (!sg_delivery_task_demand_valid(delivery, ctx->dimension_count,
                                               ctx->demand_sign_convention)) {
                return SG_STATUS_INFEASIBLE;
            }
            continue;
        }

        if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY) {
            const SGTaskRecord *pickup;
            const SGTaskRecord *delivery;

            if (!request->has_pickup_task || !request->has_delivery_task ||
                request->pickup_task_id >= ctx->num_tasks ||
                request->delivery_task_id >= ctx->num_tasks ||
                request->pickup_task_id == request->delivery_task_id) {
                return SG_STATUS_INFEASIBLE;
            }

            pickup = &ctx->tasks[request->pickup_task_id];
            delivery = &ctx->tasks[request->delivery_task_id];
            if (pickup->type != SG_TASK_PICKUP || delivery->type != SG_TASK_DELIVERY) {
                return SG_STATUS_INFEASIBLE;
            }
            if (pickup->tw_early > delivery->tw_late) {
                return SG_STATUS_INFEASIBLE;
            }
            if (!sg_request_pd_demands_valid(pickup, delivery, ctx->dimension_count,
                                             ctx->demand_sign_convention)) {
                return SG_STATUS_INFEASIBLE;
            }
            continue;
        }

        return SG_STATUS_INFEASIBLE;
    }

    return SG_STATUS_OK;
}

SGStatus sg_solve(SGContext *ctx) {
    ARALNSParams params;
    ARSolutionOps ops;
    ARALNSContext *alns = NULL;
    SGBootstrapSolution initial;
    SGBootstrapSolution *best = NULL;
    ARStatus ar_status;
    ARALNSStats ar_stats;
    ARStatus init_status;

    if (!ctx) {
        return SG_STATUS_INVALID_ARG;
    }
    if (ctx->config.require_bound_requests_at_solve &&
        sg_count_unbound_requests(ctx) > 0) {
        return SG_STATUS_INFEASIBLE;
    }
    if ((ctx->num_depots > 0 || ctx->num_tasks > 0) &&
        sg_validate_model(ctx) != SG_STATUS_OK) {
        return SG_STATUS_INFEASIBLE;
    }
    if (ctx->num_requests > 0 && ctx->num_vehicles == 0) {
        return SG_STATUS_INFEASIBLE;
    }
    if (sg_route_solver_eligible(ctx)) {
        return sg_solve_route_model(ctx);
    }

    ar_alns_params_default(&params);
    params.max_iterations = ctx->config.max_iterations;
    params.max_time_seconds = ctx->config.max_time_seconds;
    params.segment_size = ctx->config.segment_size;
    params.q_min = ctx->config.q_min;
    params.q_max = ctx->config.q_max;
    params.target_cost = 0.0;
    params.accept_type = AR_ACCEPT_IMPROVING;

    ops.copy = sg_bootstrap_copy;
    ops.free = sg_bootstrap_free;
    ops.cost = sg_bootstrap_cost;
    ops.size = sg_bootstrap_size;
    ops.validate = sg_bootstrap_validate;
    ops.user_ctx = ctx;

    alns = ar_alns_create(&params, &ops, ctx);
    if (!alns) {
        return SG_STATUS_OUT_OF_MEMORY;
    }

    if (ctx->config.deterministic) {
        ar_alns_set_seed(alns, ctx->config.seed);
        sh_rng_seed(ctx->op_rng, ctx->config.seed ^ SG_OPERATOR_SEED_XOR);
    } else {
        sh_rng_seed_time(ctx->op_rng);
    }

    ar_status = ar_alns_add_destroy(alns, "random", sg_destroy_random, ctx, 1.0);
    if (ar_status != AR_STATUS_OK) {
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_destroy(alns, "criticality-worst",
                                    sg_destroy_criticality_worst, ctx, 1.0);
    if (ar_status != AR_STATUS_OK) {
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_destroy(alns, "route-cluster",
                                    sg_destroy_route_cluster, ctx, 1.0);
    if (ar_status != AR_STATUS_OK) {
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_destroy(alns, "time-cluster",
                                    sg_destroy_time_cluster, ctx, 1.0);
    if (ar_status != AR_STATUS_OK) {
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_destroy(alns, "paired-shaw",
                                    sg_destroy_paired_shaw, ctx, 1.0);
    if (ar_status != AR_STATUS_OK) {
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_destroy(alns, "worst", sg_destroy_worst, ctx, 0.5);
    if (ar_status != AR_STATUS_OK) {
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_destroy(alns, "shaw", sg_destroy_shaw, ctx, 0.5);
    if (ar_status != AR_STATUS_OK) {
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_repair(alns, "greedy-insert", sg_repair_greedy_insertion, ctx, 1.0);
    if (ar_status != AR_STATUS_OK) {
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_repair(alns, "regret-2", sg_repair_regret2, ctx, 1.0);
    if (ar_status != AR_STATUS_OK) {
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_repair(alns, "regret-3", sg_repair_regret3, ctx, 1.0);
    if (ar_status != AR_STATUS_OK) {
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_repair(alns, "regret-4", sg_repair_regret4, ctx, 1.0);
    if (ar_status != AR_STATUS_OK) {
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_repair(alns, "noise-regret", sg_repair_noise_regret, ctx, 1.0);
    if (ar_status != AR_STATUS_OK) {
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_repair(alns, "pair-sync", sg_repair_pair_sync, ctx, 1.0);
    if (ar_status != AR_STATUS_OK) {
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_repair(alns, "bootstrap-repair", sg_repair_greedy_insertion, ctx, 0.5);
    if (ar_status != AR_STATUS_OK) {
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    init_status = sg_bootstrap_solution_init(&initial, ctx->num_requests);
    if (init_status != AR_STATUS_OK) {
        ar_alns_free(alns);
        return SG_STATUS_OUT_OF_MEMORY;
    }

    init_status = sg_construct_initial_solution(ctx, &initial);
    if (init_status != AR_STATUS_OK) {
        sg_bootstrap_solution_reset(&initial);
        ar_alns_free(alns);
        return init_status == AR_STATUS_OUT_OF_MEMORY ? SG_STATUS_OUT_OF_MEMORY
                                                      : SG_STATUS_ERROR;
    }

    ar_status = ar_alns_solve(alns, &initial, (void **)&best);
    if (ar_status != AR_STATUS_OK && ar_status != AR_STATUS_LIMIT) {
        sg_bootstrap_solution_reset(&initial);
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_alns_get_stats(alns, &ar_stats);
    ctx->stats.iterations = ar_stats.iterations;
    ctx->stats.unassigned = best ? best->num_unassigned : initial.num_unassigned;
    ctx->stats.total_cost = best ? sg_bootstrap_cost(best, ctx)
                                 : sg_bootstrap_cost(&initial, ctx);
    sg_compute_solution_route_metrics(ctx, best ? best : &initial,
                                      &ctx->stats.vehicles_used,
                                      &ctx->stats.total_distance);

    sg_bootstrap_solution_reset(&initial);
    sg_bootstrap_free(best, NULL);
    ar_alns_free(alns);

    if (ar_status == AR_STATUS_LIMIT) {
        return SG_STATUS_LIMIT;
    }

    return SG_STATUS_OK;
}

double sg_get_total_cost(const SGContext *ctx) {
    if (!ctx) {
        return 0.0;
    }
    return ctx->stats.total_cost;
}

double sg_get_total_distance(const SGContext *ctx) {
    if (!ctx) {
        return 0.0;
    }
    return ctx->stats.total_distance;
}

uint32_t sg_get_unassigned(const SGContext *ctx) {
    if (!ctx) {
        return 0;
    }
    return ctx->stats.unassigned;
}

uint32_t sg_get_used_vehicle_count(const SGContext *ctx) {
    if (!ctx) {
        return 0;
    }
    return ctx->stats.vehicles_used;
}

uint32_t sg_get_request_count(const SGContext *ctx) {
    if (!ctx) {
        return 0;
    }
    return ctx->num_requests;
}

void sg_get_stats(const SGContext *ctx, SGStats *stats) {
    if (!ctx || !stats) {
        return;
    }
    *stats = ctx->stats;
}
