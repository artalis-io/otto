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

typedef struct {
    uint32_t total_requests;
    uint32_t num_assigned;
    uint32_t num_unassigned;

    uint32_t *assigned_ids;
    uint32_t *unassigned_ids;
    uint8_t *assigned_flags;
} SGBootstrapSolution;

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
    uint8_t has_depots;
    uint8_t has_shift_time_window;
    uint8_t has_capacity;
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

    if (ctx->num_vehicles == 0 && ctx->num_requests > 0) {
        return 1e9;
    }

    return (double)sol->num_unassigned * SG_UNASSIGNED_PENALTY;
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
            state->remaining_capacity[(size_t)v * (size_t)ctx->dimension_count + (size_t)d] = cap;
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

uint32_t sg_get_unassigned(const SGContext *ctx) {
    if (!ctx) {
        return 0;
    }
    return ctx->stats.unassigned;
}

void sg_get_stats(const SGContext *ctx, SGStats *stats) {
    if (!ctx || !stats) {
        return;
    }
    *stats = ctx->stats;
}
