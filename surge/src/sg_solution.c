#include "sg_internal.h"

void sg_bootstrap_solution_reset(SGBootstrapSolution *sol) {
    if (!sol) {
        return;
    }
    free(sol->assigned_ids);
    free(sol->unassigned_ids);
    free(sol->assigned_flags);
    memset(sol, 0, sizeof(*sol));
}

ARStatus sg_bootstrap_solution_init(SGBootstrapSolution *sol,
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

int sg_find_id(const uint32_t *ids, uint32_t count, uint32_t id) {
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

ARStatus sg_bootstrap_assign_request(SGBootstrapSolution *sol, uint32_t id) {
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

ARStatus sg_bootstrap_unassign_request(SGBootstrapSolution *sol, uint32_t id) {
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

void *sg_bootstrap_copy(const void *solution, void *user_ctx) {
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

void sg_bootstrap_free(void *solution, void *user_ctx) {
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    (void)user_ctx;

    if (!sol) {
        return;
    }

    sg_bootstrap_solution_reset(sol);
    free(sol);
}

double sg_bootstrap_cost(const void *solution, void *user_ctx) {
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

int sg_bootstrap_size(const void *solution, void *user_ctx) {
    const SGBootstrapSolution *sol = (const SGBootstrapSolution *)solution;
    (void)user_ctx;
    return (int)sol->num_assigned;
}

int sg_bootstrap_validate(const void *solution, void *user_ctx) {
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

int sg_get_assigned_count(void *solution, void *user_ctx) {
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    (void)user_ctx;
    return sol ? (int)sol->num_assigned : 0;
}

uint32_t sg_get_assigned_element(void *solution, void *user_ctx, int index) {
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    (void)user_ctx;

    if (!sol || index < 0 || (uint32_t)index >= sol->num_assigned) {
        return UINT32_MAX;
    }
    return sol->assigned_ids[(uint32_t)index];
}

double sg_route_objective_cost(uint32_t unassigned, uint32_t vehicles_used,
                               double total_distance) {
    return (double)unassigned * SG_ROUTE_OBJECTIVE_UNASSIGNED_WEIGHT +
           (double)vehicles_used * SG_ROUTE_OBJECTIVE_VEHICLE_WEIGHT +
           total_distance;
}

uint32_t *sg_route_vehicle_ptr(SGRouteSolution *sol, uint32_t vehicle_id) {
    return sol->route_requests + (size_t)vehicle_id * (size_t)sol->route_stride;
}

const uint32_t *sg_route_vehicle_ptr_const(const SGRouteSolution *sol, uint32_t vehicle_id) {
    return sol->route_requests + (size_t)vehicle_id * (size_t)sol->route_stride;
}

SGRouteStop *sg_route_vehicle_stop_ptr(SGRouteSolution *sol, uint32_t vehicle_id) {
    return sol->route_stops + (size_t)vehicle_id * (size_t)sol->stop_stride;
}

const SGRouteStop *sg_route_vehicle_stop_ptr_const(const SGRouteSolution *sol,
                                                    uint32_t vehicle_id) {
    return sol->route_stops + (size_t)vehicle_id * (size_t)sol->stop_stride;
}

uint32_t *sg_route_vehicle_stop_prev_ptr(SGRouteSolution *sol, uint32_t vehicle_id) {
    return sol->route_stop_prev + (size_t)vehicle_id * (size_t)sol->stop_stride;
}

uint32_t *sg_route_vehicle_stop_next_ptr(SGRouteSolution *sol, uint32_t vehicle_id) {
    return sol->route_stop_next + (size_t)vehicle_id * (size_t)sol->stop_stride;
}

const uint32_t *sg_route_vehicle_stop_prev_ptr_const(const SGRouteSolution *sol,
                                                      uint32_t vehicle_id) {
    return sol->route_stop_prev + (size_t)vehicle_id * (size_t)sol->stop_stride;
}

const uint32_t *sg_route_vehicle_stop_next_ptr_const(const SGRouteSolution *sol,
                                                      uint32_t vehicle_id) {
    return sol->route_stop_next + (size_t)vehicle_id * (size_t)sol->stop_stride;
}

int sg_request_emit_stops(const SGContext *ctx, uint32_t request_id,
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

int sg_route_rebuild_vehicle_stop_state(const SGContext *ctx, SGRouteSolution *sol,
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
        stops[r].arrival = 0.0;
        stops[r].service_start = 0.0;
        stops[r].depart = 0.0;
        stops[r].latest_start = 0.0;
        stops[r].forward_slack = 0.0;
        prev[r] = UINT32_MAX;
        next[r] = UINT32_MAX;
    }

    sol->route_stop_lengths[vehicle_id] = stop_len;
    return 1;
}

int sg_route_splice_stop(const SGContext *ctx, SGRouteSolution *sol,
                         uint32_t vehicle_id, uint32_t at,
                         const SGRouteStop *stop) {
    SGRouteStop *stops;
    uint32_t *prev;
    uint32_t *next;
    uint32_t stop_len;
    uint32_t r;

    if (!ctx || !sol || !stop || vehicle_id >= sol->num_vehicles) {
        return 0;
    }

    stops = sg_route_vehicle_stop_ptr(sol, vehicle_id);
    prev = sg_route_vehicle_stop_prev_ptr(sol, vehicle_id);
    next = sg_route_vehicle_stop_next_ptr(sol, vehicle_id);
    stop_len = sol->route_stop_lengths[vehicle_id];

    if (at > stop_len || stop_len + 1 > sol->stop_stride) {
        return 0;
    }

    /* Shift stops right */
    if (at < stop_len) {
        memmove(&stops[at + 1], &stops[at],
                (size_t)(stop_len - at) * sizeof(SGRouteStop));
        memmove(&prev[at + 1], &prev[at],
                (size_t)(stop_len - at) * sizeof(uint32_t));
        memmove(&next[at + 1], &next[at],
                (size_t)(stop_len - at) * sizeof(uint32_t));
    }

    /* Update position tracking for all requests on this vehicle whose
       stop positions >= at (they shifted right by 1) */
    for (r = 0; r < sol->base.total_requests; r++) {
        if (sol->request_vehicle[r] != vehicle_id) {
            continue;
        }
        if (sol->request_pickup_stop_pos[r] != UINT32_MAX &&
            sol->request_pickup_stop_pos[r] >= at) {
            sol->request_pickup_stop_pos[r]++;
        }
        if (sol->request_delivery_stop_pos[r] != UINT32_MAX &&
            sol->request_delivery_stop_pos[r] >= at) {
            sol->request_delivery_stop_pos[r]++;
        }
    }

    /* Place the new stop */
    stops[at] = *stop;

    /* Set position tracking for the new stop */
    if (stop->is_pickup) {
        sol->request_pickup_stop_pos[stop->request_id] = at;
    } else {
        sol->request_delivery_stop_pos[stop->request_id] = at;
    }

    /* Rebuild prev/next linked list (memmove invalidates shifted entries) */
    stop_len++;
    {
        uint32_t k;
        for (k = 0; k < stop_len; k++) {
            prev[k] = k > 0 ? k - 1U : UINT32_MAX;
            next[k] = k + 1U < stop_len ? k + 1U : UINT32_MAX;
        }
    }

    sol->route_stop_lengths[vehicle_id] = stop_len;
    return 1;
}

int sg_route_excise_stop(const SGContext *ctx, SGRouteSolution *sol,
                         uint32_t vehicle_id, uint32_t at) {
    SGRouteStop *stops;
    uint32_t *prev;
    uint32_t *next;
    uint32_t stop_len;
    uint32_t request_id;
    uint8_t is_pickup;
    uint32_t r;

    if (!ctx || !sol || vehicle_id >= sol->num_vehicles) {
        return 0;
    }

    stops = sg_route_vehicle_stop_ptr(sol, vehicle_id);
    prev = sg_route_vehicle_stop_prev_ptr(sol, vehicle_id);
    next = sg_route_vehicle_stop_next_ptr(sol, vehicle_id);
    stop_len = sol->route_stop_lengths[vehicle_id];

    if (at >= stop_len) {
        return 0;
    }

    /* Clear position tracking for the removed stop */
    request_id = stops[at].request_id;
    is_pickup = stops[at].is_pickup;
    if (is_pickup) {
        sol->request_pickup_stop_pos[request_id] = UINT32_MAX;
    } else {
        sol->request_delivery_stop_pos[request_id] = UINT32_MAX;
    }

    /* Shift stops left */
    if (at + 1U < stop_len) {
        memmove(&stops[at], &stops[at + 1U],
                (size_t)(stop_len - at - 1U) * sizeof(SGRouteStop));
        memmove(&prev[at], &prev[at + 1U],
                (size_t)(stop_len - at - 1U) * sizeof(uint32_t));
        memmove(&next[at], &next[at + 1U],
                (size_t)(stop_len - at - 1U) * sizeof(uint32_t));
    }

    stop_len--;

    /* Update position tracking for all requests on this vehicle whose
       stop positions > at (they shifted left by 1) */
    for (r = 0; r < sol->base.total_requests; r++) {
        if (sol->request_vehicle[r] != vehicle_id) {
            continue;
        }
        if (sol->request_pickup_stop_pos[r] != UINT32_MAX &&
            sol->request_pickup_stop_pos[r] > at) {
            sol->request_pickup_stop_pos[r]--;
        }
        if (sol->request_delivery_stop_pos[r] != UINT32_MAX &&
            sol->request_delivery_stop_pos[r] > at) {
            sol->request_delivery_stop_pos[r]--;
        }
    }

    /* Rebuild prev/next linked list (memmove invalidates shifted entries) */
    {
        uint32_t k;
        for (k = 0; k < stop_len; k++) {
            prev[k] = k > 0 ? k - 1U : UINT32_MAX;
            next[k] = k + 1U < stop_len ? k + 1U : UINT32_MAX;
        }
    }

    /* Clear the now-unused last slot */
    if (stop_len < sol->stop_stride) {
        stops[stop_len].request_id = UINT32_MAX;
        stops[stop_len].task_id = UINT32_MAX;
        stops[stop_len].is_pickup = 0;
        prev[stop_len] = UINT32_MAX;
        next[stop_len] = UINT32_MAX;
    }

    sol->route_stop_lengths[vehicle_id] = stop_len;
    return 1;
}

void sg_route_solution_reset(SGRouteSolution *sol) {
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
    free(sol->route_duration);
    free(sol->route_stop_load);
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
    sol->route_duration = NULL;
    sol->route_stop_load = NULL;
    sol->num_vehicles = 0;
    sol->route_stride = 0;
    sol->stop_stride = 0;
    sol->vehicles_used = 0;
    sol->total_distance = 0.0;
}

ARStatus sg_route_solution_init(const SGContext *ctx, SGRouteSolution *sol) {
    ARStatus status;
    size_t route_capacity;
    size_t stop_capacity;
    uint32_t i;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }

    /* Ensure travel infrastructure is prepared (idempotent). Cast away const
       because prepare_travel computes derived data, not changing the model. */
    if (!((SGContext *)ctx)->travel_prepared) {
        SGStatus prep = sg_prepare_travel((SGContext *)ctx);
        if (prep != SG_STATUS_OK) {
            return AR_STATUS_ERROR;
        }
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
    sol->route_duration = (double *)calloc((size_t)ctx->num_vehicles, sizeof(double));

    if (ctx->dimension_count > 0) {
        /* +1 per vehicle because load is a prefix sum: entry i holds cumulative
           load AFTER stop i, so we need stop_stride + 1 slots per vehicle. */
        size_t load_size = (size_t)ctx->num_vehicles * ((size_t)sol->stop_stride + 1U) *
                           (size_t)ctx->dimension_count;
        sol->route_stop_load = (double *)calloc(load_size, sizeof(double));
    }

    if (!sol->route_lengths || !sol->route_requests || !sol->route_stop_lengths ||
        !sol->route_stops || !sol->route_stop_prev || !sol->route_stop_next ||
        !sol->request_vehicle || !sol->request_pos || !sol->request_pickup_stop_pos ||
        !sol->request_delivery_stop_pos || !sol->route_distance || !sol->route_duration ||
        (ctx->dimension_count > 0 && !sol->route_stop_load)) {
        sg_route_solution_reset(sol);
        return AR_STATUS_OUT_OF_MEMORY;
    }

    for (i = 0; i < stop_capacity; i++) {
        sol->route_stop_prev[i] = UINT32_MAX;
        sol->route_stop_next[i] = UINT32_MAX;
        sol->route_stops[i].request_id = UINT32_MAX;
        sol->route_stops[i].task_id = UINT32_MAX;
        sol->route_stops[i].is_pickup = 0;
        sol->route_stops[i].arrival = 0.0;
        sol->route_stops[i].service_start = 0.0;
        sol->route_stops[i].depart = 0.0;
        sol->route_stops[i].latest_start = 0.0;
        sol->route_stops[i].forward_slack = 0.0;
    }

    for (i = 0; i < ctx->num_requests; i++) {
        sol->request_vehicle[i] = UINT32_MAX;
        sol->request_pos[i] = UINT32_MAX;
        sol->request_pickup_stop_pos[i] = UINT32_MAX;
        sol->request_delivery_stop_pos[i] = UINT32_MAX;
    }

    return AR_STATUS_OK;
}

void *sg_route_solution_copy(const void *solution, void *user_ctx) {
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
        if (src->route_duration && dst->route_duration) {
            memcpy(dst->route_duration, src->route_duration,
                   (size_t)src->num_vehicles * sizeof(double));
        }
        if (src->route_stop_load && dst->route_stop_load && ctx->dimension_count > 0) {
            size_t load_size = (size_t)src->num_vehicles * ((size_t)src->stop_stride + 1U) *
                               (size_t)ctx->dimension_count;
            memcpy(dst->route_stop_load, src->route_stop_load, load_size * sizeof(double));
        }
    }

    return dst;
}

void sg_route_solution_free(void *solution, void *user_ctx) {
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    (void)user_ctx;

    if (!sol) {
        return;
    }

    sg_route_solution_reset(sol);
    free(sol);
}

int sg_route_solution_validate(const void *solution, void *user_ctx) {
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
         !sol->route_distance || !sol->route_duration)) {
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

double sg_route_solution_cost(const void *solution, void *user_ctx) {
    const SGRouteSolution *sol = (const SGRouteSolution *)solution;
    const SGContext *ctx = (const SGContext *)user_ctx;
    double cost;
    uint32_t v;

    if (!sol) {
        return INFINITY;
    }

    cost = (double)sol->base.num_unassigned * ctx->unassigned_weight;
    for (v = 0; v < sol->num_vehicles; v++) {
        if (sol->route_stop_lengths[v] > 0) {
            const SGVehicleRecord *vehicle = &ctx->vehicles[v];
            cost += vehicle->fixed_cost;
            cost += vehicle->cost_per_distance * sol->route_distance[v];
            if (sol->route_duration) {
                cost += vehicle->cost_per_duration * sol->route_duration[v];
            }
        }
    }
    return cost;
}

int sg_route_solution_size(const void *solution, void *user_ctx) {
    const SGRouteSolution *sol = (const SGRouteSolution *)solution;
    (void)user_ctx;
    return sol ? (int)sol->base.num_assigned : 0;
}
