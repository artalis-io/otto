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

int sg_get_removable_count(void *solution, void *user_ctx) {
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    SGContext *ctx = (SGContext *)user_ctx;
    uint32_t i;
    int count;

    if (!sol) return 0;
    if (!ctx || !ctx->has_frozen || !ctx->request_locks) {
        return (int)sol->num_assigned;
    }

    count = 0;
    for (i = 0; i < sol->num_assigned; i++) {
        if (ctx->request_locks[sol->assigned_ids[i]] < SG_LOCK_FROZEN) {
            count++;
        }
    }
    return count;
}

uint32_t sg_get_removable_element(void *solution, void *user_ctx, int index) {
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    SGContext *ctx = (SGContext *)user_ctx;
    uint32_t i;
    int count;

    if (!sol || index < 0) return UINT32_MAX;
    if (!ctx || !ctx->has_frozen || !ctx->request_locks) {
        if ((uint32_t)index >= sol->num_assigned) return UINT32_MAX;
        return sol->assigned_ids[(uint32_t)index];
    }

    count = 0;
    for (i = 0; i < sol->num_assigned; i++) {
        if (ctx->request_locks[sol->assigned_ids[i]] < SG_LOCK_FROZEN) {
            if (count == index) return sol->assigned_ids[i];
            count++;
        }
    }
    return UINT32_MAX;
}

int sg_route_solution_is_better(const void *candidate, const void *current_best,
                                 void *user_ctx) {
    const SGRouteSolution *cand = (const SGRouteSolution *)candidate;
    const SGRouteSolution *best = (const SGRouteSolution *)current_best;
    const SGContext *ctx = (const SGContext *)user_ctx;
    if (!cand || !best) return 0;

    /* Feasible beats infeasible, regardless of cost or mode */
    {
        int cf = sg_solution_is_feasible(cand);
        int bf = sg_solution_is_feasible(best);
        if (cf && !bf) return 1;
        if (!cf && bf) return 0;
        if (!cf && !bf) {
            /* Both infeasible: prefer less total violation */
            double cv = sg_solution_total_violation(cand);
            double bv = sg_solution_total_violation(best);
            if (cv < bv - 1e-9) return 1;
            if (cv > bv + 1e-9) return 0;
        }
    }

    /* Lexicographic: unassigned -> vehicles_used -> total_distance */
    if (!ctx || ctx->config.lexicographic_objective) {
        if (cand->base.num_unassigned < best->base.num_unassigned) return 1;
        if (cand->base.num_unassigned > best->base.num_unassigned) return 0;
        if (cand->vehicles_used < best->vehicles_used) return 1;
        if (cand->vehicles_used > best->vehicles_used) return 0;
        return cand->total_distance < best->total_distance - 1e-9;
    }

    /* Non-lexicographic: use cost comparison */
    {
        double cc = sg_route_solution_cost(cand, user_ctx);
        double bc = sg_route_solution_cost(best, user_ctx);
        return cc < bc - 1e-9;
    }
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
        memset(&stops_out[0], 0, sizeof(SGRouteStop));
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
        memset(&stops_out[0], 0, 2 * sizeof(SGRouteStop));
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
        uint8_t req_trip_start = 0;

        if (!sg_request_emit_stops(ctx, request_id, emitted, &emitted_count)) {
            return 0;
        }
        if (stop_len + emitted_count > sol->stop_stride) {
            return 0;
        }

        /* Check if this request starts a new trip */
        if (sol->route_request_trip_start) {
            req_trip_start = sol->route_request_trip_start[
                (size_t)vehicle_id * sol->route_stride + r];
        }

        for (e = 0; e < emitted_count; e++) {
            stops[stop_len] = emitted[e];
            /* Set trip_start on the first stop of the request */
            stops[stop_len].trip_start = (e == 0) ? req_trip_start : 0;
            stops[stop_len].trip_depot_return = 0.0;
            stops[stop_len].trip_depot_depart = 0.0;
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
        stops[r].work_since_break = 0.0;
        stops[r].trip_start = 0;
        stops[r].trip_depot_return = 0.0;
        stops[r].trip_depot_depart = 0.0;
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

    /* Transfer trip_start flag to next stop if needed */
    if (stops[at].trip_start && at + 1U < stop_len && !stops[at + 1U].trip_start) {
        stops[at + 1U].trip_start = 1;
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
        stops[stop_len].trip_start = 0;
        stops[stop_len].trip_depot_return = 0.0;
        stops[stop_len].trip_depot_depart = 0.0;
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

    if (sol->arena) {
        /* All arrays (including bootstrap) are in the arena — single free */
        sh_arena_free(sol->arena);
    } else {
        /* Legacy path: standalone bootstrap or partially-initialized solution */
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
        free(sol->route_waiting);
        free(sol->route_overtime);
        free(sol->route_tw_penalty);
        free(sol->route_stop_load);
        free(sol->route_depot_depart);
        free(sol->route_depot_return);
        free(sol->route_commodities);
        free(sol->route_exclusion_counts);
        free(sol->route_break_time);
        free(sol->route_break_count);
        free(sol->route_total_work);
        free(sol->route_breaks);
        free(sol->route_trip_count);
        free(sol->route_request_trip_start);
    }
    memset(sol, 0, sizeof(*sol));
}

ARStatus sg_route_solution_init(const SGContext *ctx, SGRouteSolution *sol) {
    size_t route_capacity;
    size_t stop_capacity;
    size_t total;
    uint32_t num_req;
    uint32_t num_veh;
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

    num_req = ctx->num_requests;
    num_veh = ctx->num_vehicles;

    sol->num_vehicles = num_veh;
    sol->route_stride = num_req > 0 ? num_req : 1;
    if (num_req > UINT32_MAX / 2U) {
        return AR_STATUS_OUT_OF_MEMORY;
    }
    sol->stop_stride = num_req > 0 ? num_req * 2U : 1U;

    if (num_req == 0 || num_veh == 0) {
        /* No route data needed. Bootstrap-only (malloc) or empty. */
        ARStatus status = sg_bootstrap_solution_init(&sol->base, num_req);
        if (status != AR_STATUS_OK) {
            memset(sol, 0, sizeof(*sol));
        }
        return status;
    }

    /* --- Arena path: num_req > 0 && num_veh > 0 --- */

    if ((size_t)num_veh > SIZE_MAX / (size_t)sol->route_stride) {
        memset(sol, 0, sizeof(*sol));
        return AR_STATUS_OUT_OF_MEMORY;
    }
    route_capacity = (size_t)num_veh * (size_t)sol->route_stride;
    if ((size_t)num_veh > SIZE_MAX / (size_t)sol->stop_stride) {
        memset(sol, 0, sizeof(*sol));
        return AR_STATUS_OUT_OF_MEMORY;
    }
    stop_capacity = (size_t)num_veh * (size_t)sol->stop_stride;
    sol->break_stride = sol->stop_stride;

    /* Compute total arena size (each allocation aligned to 8 bytes) */
    #define ALIGN8(x) (((x) + 7U) & ~(size_t)7U)
    total = 0;
    /* Bootstrap arrays */
    total += ALIGN8((size_t)num_req * sizeof(uint32_t));   /* assigned_ids */
    total += ALIGN8((size_t)num_req * sizeof(uint32_t));   /* unassigned_ids */
    total += ALIGN8((size_t)num_req * sizeof(uint8_t));    /* assigned_flags */
    /* Per-vehicle uint32_t arrays (4) */
    total += 4 * ALIGN8((size_t)num_veh * sizeof(uint32_t));
    /* Route requests + trip start */
    total += ALIGN8(route_capacity * sizeof(uint32_t));
    total += ALIGN8(route_capacity * sizeof(uint8_t));
    /* Stops + prev/next */
    total += ALIGN8(stop_capacity * sizeof(SGRouteStop));
    total += 2 * ALIGN8(stop_capacity * sizeof(uint32_t));
    /* Per-request arrays (4) */
    total += 4 * ALIGN8((size_t)num_req * sizeof(uint32_t));
    /* Per-vehicle doubles (9) */
    total += 9 * ALIGN8((size_t)num_veh * sizeof(double));
    /* Conditional arrays */
    if (ctx->dimension_count > 0) {
        total += ALIGN8((size_t)num_veh * ((size_t)sol->stop_stride + 1U) *
                         (size_t)ctx->dimension_count * sizeof(double));
    }
    if (ctx->num_commodities > 0) {
        total += ALIGN8((size_t)num_veh * sizeof(uint64_t));
    }
    if (ctx->num_exclusion_groups > 0) {
        total += ALIGN8((size_t)num_veh * (size_t)ctx->num_exclusion_groups * sizeof(uint32_t));
    }
    /* Breaks */
    total += ALIGN8((size_t)num_veh * (size_t)sol->break_stride * sizeof(SGRouteBreak));
    /* Penalty violations (per-route per-constraint-type) */
    total += ALIGN8((size_t)num_veh * SG_PENALTY_COUNT * sizeof(double));
    #undef ALIGN8

    /* Cache arena size for fast copy path */
    ((SGContext *)ctx)->solution_arena_size = total;

    /* Create arena — single malloc for all arrays */
    sol->arena = sh_arena_create(total);
    if (!sol->arena) {
        memset(sol, 0, sizeof(*sol));
        return AR_STATUS_OUT_OF_MEMORY;
    }

    /* Bootstrap arrays (from arena, not standalone init) */
    sol->base.total_requests = num_req;
    sol->base.num_unassigned = num_req;
    sol->base.assigned_ids = (uint32_t *)sh_arena_calloc(sol->arena, (size_t)num_req, sizeof(uint32_t));
    sol->base.unassigned_ids = (uint32_t *)sh_arena_calloc(sol->arena, (size_t)num_req, sizeof(uint32_t));
    sol->base.assigned_flags = (uint8_t *)sh_arena_calloc(sol->arena, (size_t)num_req, sizeof(uint8_t));

    /* Per-vehicle uint32_t arrays */
    sol->route_lengths = (uint32_t *)sh_arena_calloc(sol->arena, (size_t)num_veh, sizeof(uint32_t));
    sol->route_stop_lengths = (uint32_t *)sh_arena_calloc(sol->arena, (size_t)num_veh, sizeof(uint32_t));
    sol->route_break_count = (uint32_t *)sh_arena_calloc(sol->arena, (size_t)num_veh, sizeof(uint32_t));
    sol->route_trip_count = (uint32_t *)sh_arena_calloc(sol->arena, (size_t)num_veh, sizeof(uint32_t));

    /* Route-level arrays */
    sol->route_requests = (uint32_t *)sh_arena_calloc(sol->arena, route_capacity, sizeof(uint32_t));
    sol->route_request_trip_start = (uint8_t *)sh_arena_calloc(sol->arena, route_capacity, sizeof(uint8_t));

    /* Stop-level arrays */
    sol->route_stops = (SGRouteStop *)sh_arena_calloc(sol->arena, stop_capacity, sizeof(SGRouteStop));
    sol->route_stop_prev = (uint32_t *)sh_arena_alloc(sol->arena, stop_capacity * sizeof(uint32_t));
    sol->route_stop_next = (uint32_t *)sh_arena_alloc(sol->arena, stop_capacity * sizeof(uint32_t));

    /* Per-request arrays */
    sol->request_vehicle = (uint32_t *)sh_arena_alloc(sol->arena, (size_t)num_req * sizeof(uint32_t));
    sol->request_pos = (uint32_t *)sh_arena_alloc(sol->arena, (size_t)num_req * sizeof(uint32_t));
    sol->request_pickup_stop_pos = (uint32_t *)sh_arena_alloc(sol->arena, (size_t)num_req * sizeof(uint32_t));
    sol->request_delivery_stop_pos = (uint32_t *)sh_arena_alloc(sol->arena, (size_t)num_req * sizeof(uint32_t));

    /* Per-vehicle doubles */
    sol->route_distance = (double *)sh_arena_calloc(sol->arena, (size_t)num_veh, sizeof(double));
    sol->route_duration = (double *)sh_arena_calloc(sol->arena, (size_t)num_veh, sizeof(double));
    sol->route_waiting = (double *)sh_arena_calloc(sol->arena, (size_t)num_veh, sizeof(double));
    sol->route_overtime = (double *)sh_arena_calloc(sol->arena, (size_t)num_veh, sizeof(double));
    sol->route_tw_penalty = (double *)sh_arena_calloc(sol->arena, (size_t)num_veh, sizeof(double));
    sol->route_depot_depart = (double *)sh_arena_calloc(sol->arena, (size_t)num_veh, sizeof(double));
    sol->route_depot_return = (double *)sh_arena_calloc(sol->arena, (size_t)num_veh, sizeof(double));
    sol->route_break_time = (double *)sh_arena_calloc(sol->arena, (size_t)num_veh, sizeof(double));
    sol->route_total_work = (double *)sh_arena_calloc(sol->arena, (size_t)num_veh, sizeof(double));

    /* Conditional arrays */
    if (ctx->dimension_count > 0) {
        size_t load_size = (size_t)num_veh * ((size_t)sol->stop_stride + 1U) *
                           (size_t)ctx->dimension_count;
        sol->route_stop_load = (double *)sh_arena_calloc(sol->arena, load_size, sizeof(double));
    }
    if (ctx->num_commodities > 0) {
        sol->route_commodities = (uint64_t *)sh_arena_calloc(sol->arena, (size_t)num_veh, sizeof(uint64_t));
    }
    if (ctx->num_exclusion_groups > 0) {
        sol->route_exclusion_counts = (uint32_t *)sh_arena_calloc(sol->arena,
            (size_t)num_veh * (size_t)ctx->num_exclusion_groups, sizeof(uint32_t));
    }

    /* Breaks */
    sol->route_breaks = (SGRouteBreak *)sh_arena_calloc(sol->arena,
        (size_t)num_veh * (size_t)sol->break_stride, sizeof(SGRouteBreak));

    /* Penalty violations */
    sol->route_violations = (double *)sh_arena_calloc(sol->arena,
        (size_t)num_veh * SG_PENALTY_COUNT, sizeof(double));

    /* Verify all allocations succeeded */
    if (!sol->base.assigned_ids || !sol->base.unassigned_ids || !sol->base.assigned_flags ||
        !sol->route_lengths || !sol->route_stop_lengths || !sol->route_break_count ||
        !sol->route_trip_count || !sol->route_requests || !sol->route_request_trip_start ||
        !sol->route_stops || !sol->route_stop_prev || !sol->route_stop_next ||
        !sol->request_vehicle || !sol->request_pos ||
        !sol->request_pickup_stop_pos || !sol->request_delivery_stop_pos ||
        !sol->route_distance || !sol->route_duration || !sol->route_waiting ||
        !sol->route_overtime || !sol->route_tw_penalty ||
        !sol->route_depot_depart || !sol->route_depot_return ||
        !sol->route_break_time || !sol->route_total_work || !sol->route_breaks ||
        !sol->route_violations ||
        (ctx->dimension_count > 0 && !sol->route_stop_load) ||
        (ctx->num_commodities > 0 && !sol->route_commodities) ||
        (ctx->num_exclusion_groups > 0 && !sol->route_exclusion_counts)) {
        sh_arena_free(sol->arena);
        memset(sol, 0, sizeof(*sol));
        return AR_STATUS_OUT_OF_MEMORY;
    }

    /* Initialize bootstrap unassigned_ids */
    for (i = 0; i < num_req; i++) {
        sol->base.unassigned_ids[i] = i;
    }

    /* Initialize stop state (non-zero fields only; rest is zero from calloc) */
    for (i = 0; i < stop_capacity; i++) {
        sol->route_stop_prev[i] = UINT32_MAX;
        sol->route_stop_next[i] = UINT32_MAX;
        sol->route_stops[i].request_id = UINT32_MAX;
        sol->route_stops[i].task_id = UINT32_MAX;
    }

    /* Initialize per-request tracking */
    for (i = 0; i < num_req; i++) {
        sol->request_vehicle[i] = UINT32_MAX;
        sol->request_pos[i] = UINT32_MAX;
        sol->request_pickup_stop_pos[i] = UINT32_MAX;
        sol->request_delivery_stop_pos[i] = UINT32_MAX;
    }

    return AR_STATUS_OK;
}

static ARStatus sg_route_solution_init_for_copy(const SGContext *ctx, SGRouteSolution *sol) {
    uint32_t num_req = ctx->num_requests;
    uint32_t num_veh = ctx->num_vehicles;
    size_t route_capacity, stop_capacity;

    memset(sol, 0, sizeof(*sol));
    sol->num_vehicles = num_veh;
    sol->route_stride = num_req > 0 ? num_req : 1;
    sol->stop_stride = num_req > 0 ? num_req * 2U : 1U;
    sol->break_stride = sol->stop_stride;
    sol->base.total_requests = num_req;
    route_capacity = (size_t)num_veh * (size_t)sol->route_stride;
    stop_capacity = (size_t)num_veh * (size_t)sol->stop_stride;

    sol->arena = sh_arena_create(ctx->solution_arena_size);
    if (!sol->arena) {
        memset(sol, 0, sizeof(*sol));
        return AR_STATUS_OUT_OF_MEMORY;
    }

    /* Allocate all arrays in EXACT same order as init (layout must match).
       Uses sh_arena_alloc — no zeroing, no init loops.
       Content will be overwritten by memcpy of source arena buffer. */

    /* Bootstrap arrays */
    sol->base.assigned_ids = (uint32_t *)sh_arena_alloc(sol->arena, (size_t)num_req * sizeof(uint32_t));
    sol->base.unassigned_ids = (uint32_t *)sh_arena_alloc(sol->arena, (size_t)num_req * sizeof(uint32_t));
    sol->base.assigned_flags = (uint8_t *)sh_arena_alloc(sol->arena, (size_t)num_req * sizeof(uint8_t));

    /* Per-vehicle uint32_t arrays */
    sol->route_lengths = (uint32_t *)sh_arena_alloc(sol->arena, (size_t)num_veh * sizeof(uint32_t));
    sol->route_stop_lengths = (uint32_t *)sh_arena_alloc(sol->arena, (size_t)num_veh * sizeof(uint32_t));
    sol->route_break_count = (uint32_t *)sh_arena_alloc(sol->arena, (size_t)num_veh * sizeof(uint32_t));
    sol->route_trip_count = (uint32_t *)sh_arena_alloc(sol->arena, (size_t)num_veh * sizeof(uint32_t));

    /* Route-level arrays */
    sol->route_requests = (uint32_t *)sh_arena_alloc(sol->arena, route_capacity * sizeof(uint32_t));
    sol->route_request_trip_start = (uint8_t *)sh_arena_alloc(sol->arena, route_capacity * sizeof(uint8_t));

    /* Stop-level arrays */
    sol->route_stops = (SGRouteStop *)sh_arena_alloc(sol->arena, stop_capacity * sizeof(SGRouteStop));
    sol->route_stop_prev = (uint32_t *)sh_arena_alloc(sol->arena, stop_capacity * sizeof(uint32_t));
    sol->route_stop_next = (uint32_t *)sh_arena_alloc(sol->arena, stop_capacity * sizeof(uint32_t));

    /* Per-request arrays */
    sol->request_vehicle = (uint32_t *)sh_arena_alloc(sol->arena, (size_t)num_req * sizeof(uint32_t));
    sol->request_pos = (uint32_t *)sh_arena_alloc(sol->arena, (size_t)num_req * sizeof(uint32_t));
    sol->request_pickup_stop_pos = (uint32_t *)sh_arena_alloc(sol->arena, (size_t)num_req * sizeof(uint32_t));
    sol->request_delivery_stop_pos = (uint32_t *)sh_arena_alloc(sol->arena, (size_t)num_req * sizeof(uint32_t));

    /* Per-vehicle doubles */
    sol->route_distance = (double *)sh_arena_alloc(sol->arena, (size_t)num_veh * sizeof(double));
    sol->route_duration = (double *)sh_arena_alloc(sol->arena, (size_t)num_veh * sizeof(double));
    sol->route_waiting = (double *)sh_arena_alloc(sol->arena, (size_t)num_veh * sizeof(double));
    sol->route_overtime = (double *)sh_arena_alloc(sol->arena, (size_t)num_veh * sizeof(double));
    sol->route_tw_penalty = (double *)sh_arena_alloc(sol->arena, (size_t)num_veh * sizeof(double));
    sol->route_depot_depart = (double *)sh_arena_alloc(sol->arena, (size_t)num_veh * sizeof(double));
    sol->route_depot_return = (double *)sh_arena_alloc(sol->arena, (size_t)num_veh * sizeof(double));
    sol->route_break_time = (double *)sh_arena_alloc(sol->arena, (size_t)num_veh * sizeof(double));
    sol->route_total_work = (double *)sh_arena_alloc(sol->arena, (size_t)num_veh * sizeof(double));

    /* Conditional arrays */
    if (ctx->dimension_count > 0) {
        size_t load_size = (size_t)num_veh * ((size_t)sol->stop_stride + 1U) *
                           (size_t)ctx->dimension_count;
        sol->route_stop_load = (double *)sh_arena_alloc(sol->arena, load_size * sizeof(double));
    }
    if (ctx->num_commodities > 0) {
        sol->route_commodities = (uint64_t *)sh_arena_alloc(sol->arena, (size_t)num_veh * sizeof(uint64_t));
    }
    if (ctx->num_exclusion_groups > 0) {
        sol->route_exclusion_counts = (uint32_t *)sh_arena_alloc(sol->arena,
            (size_t)num_veh * (size_t)ctx->num_exclusion_groups * sizeof(uint32_t));
    }

    /* Breaks */
    sol->route_breaks = (SGRouteBreak *)sh_arena_alloc(sol->arena,
        (size_t)num_veh * (size_t)sol->break_stride * sizeof(SGRouteBreak));

    /* Penalty violations */
    sol->route_violations = (double *)sh_arena_alloc(sol->arena,
        (size_t)num_veh * SG_PENALTY_COUNT * sizeof(double));

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

    if (src->arena && ctx->solution_arena_size > 0) {
        /* Fast path: single arena alloc + single memcpy */
        if (sg_route_solution_init_for_copy(ctx, dst) != AR_STATUS_OK) {
            free(dst);
            return NULL;
        }
        memcpy(dst->arena->buffer, src->arena->buffer, sh_arena_used(src->arena));
        dst->base.num_assigned = src->base.num_assigned;
        dst->base.num_unassigned = src->base.num_unassigned;
        dst->vehicles_used = src->vehicles_used;
        dst->total_distance = src->total_distance;
        memcpy(dst->violations, src->violations, sizeof(dst->violations));
    } else {
        /* Legacy path (non-arena solutions) */
        if (sg_route_solution_init(ctx, dst) != AR_STATUS_OK) {
            free(dst);
            return NULL;
        }

        dst->base.num_assigned = src->base.num_assigned;
        dst->base.num_unassigned = src->base.num_unassigned;
        dst->vehicles_used = src->vehicles_used;
        dst->total_distance = src->total_distance;
        memcpy(dst->violations, src->violations, sizeof(dst->violations));

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
            if (src->route_waiting && dst->route_waiting) {
                memcpy(dst->route_waiting, src->route_waiting,
                       (size_t)src->num_vehicles * sizeof(double));
            }
            if (src->route_overtime && dst->route_overtime) {
                memcpy(dst->route_overtime, src->route_overtime,
                       (size_t)src->num_vehicles * sizeof(double));
            }
            if (src->route_tw_penalty && dst->route_tw_penalty) {
                memcpy(dst->route_tw_penalty, src->route_tw_penalty,
                       (size_t)src->num_vehicles * sizeof(double));
            }
            if (src->route_depot_depart && dst->route_depot_depart) {
                memcpy(dst->route_depot_depart, src->route_depot_depart,
                       (size_t)src->num_vehicles * sizeof(double));
            }
            if (src->route_depot_return && dst->route_depot_return) {
                memcpy(dst->route_depot_return, src->route_depot_return,
                       (size_t)src->num_vehicles * sizeof(double));
            }
            if (src->route_stop_load && dst->route_stop_load && ctx->dimension_count > 0) {
                size_t load_size = (size_t)src->num_vehicles * ((size_t)src->stop_stride + 1U) *
                                   (size_t)ctx->dimension_count;
                memcpy(dst->route_stop_load, src->route_stop_load, load_size * sizeof(double));
            }
            if (src->route_commodities && dst->route_commodities) {
                memcpy(dst->route_commodities, src->route_commodities,
                       (size_t)src->num_vehicles * sizeof(uint64_t));
            }
            if (src->route_exclusion_counts && dst->route_exclusion_counts &&
                ctx->num_exclusion_groups > 0) {
                memcpy(dst->route_exclusion_counts, src->route_exclusion_counts,
                       (size_t)src->num_vehicles * (size_t)ctx->num_exclusion_groups * sizeof(uint32_t));
            }
            if (src->route_break_time && dst->route_break_time) {
                memcpy(dst->route_break_time, src->route_break_time,
                       (size_t)src->num_vehicles * sizeof(double));
            }
            if (src->route_break_count && dst->route_break_count) {
                memcpy(dst->route_break_count, src->route_break_count,
                       (size_t)src->num_vehicles * sizeof(uint32_t));
            }
            if (src->route_total_work && dst->route_total_work) {
                memcpy(dst->route_total_work, src->route_total_work,
                       (size_t)src->num_vehicles * sizeof(double));
            }
            if (src->route_breaks && dst->route_breaks && src->break_stride > 0) {
                memcpy(dst->route_breaks, src->route_breaks,
                       (size_t)src->num_vehicles * (size_t)src->break_stride * sizeof(SGRouteBreak));
            }
            if (src->route_trip_count && dst->route_trip_count) {
                memcpy(dst->route_trip_count, src->route_trip_count,
                       (size_t)src->num_vehicles * sizeof(uint32_t));
            }
            if (src->route_request_trip_start && dst->route_request_trip_start) {
                memcpy(dst->route_request_trip_start, src->route_request_trip_start,
                       (size_t)src->num_vehicles * (size_t)src->route_stride * sizeof(uint8_t));
            }
            if (src->route_violations && dst->route_violations) {
                memcpy(dst->route_violations, src->route_violations,
                       (size_t)src->num_vehicles * SG_PENALTY_COUNT * sizeof(double));
            }
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
         !sol->route_distance || !sol->route_duration || !sol->route_waiting ||
         !sol->route_overtime || !sol->route_tw_penalty ||
         !sol->route_depot_depart || !sol->route_depot_return)) {
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
            if (!ctx->penalty.enabled) {
                if (!sg_route_stop_sequence_feasible(ctx, v, stops, stop_len, &recomputed_distance)) {
                    goto done;
                }
            } else {
                /* When penalty is enabled, recompute distance only (skip feasibility) */
                uint32_t ss;
                uint32_t rloc = ctx->vehicles[v].start_location_id;
                recomputed_distance = 0.0;
                for (ss = 0; ss < stop_len; ss++) {
                    uint32_t cloc = ctx->tasks[stops[ss].task_id].location_id;
                    recomputed_distance += sg_travel_dist(ctx, rloc, cloc, v);
                    rloc = cloc;
                }
                if (!ctx->vehicles[v].open_end) {
                    recomputed_distance += sg_travel_dist(ctx, rloc, ctx->vehicles[v].end_location_id, v);
                }
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

    if (route_assigned != sol->base.num_assigned) {
        goto done;
    }
    if (computed_vehicles != sol->vehicles_used) {
        goto done;
    }
    if (fabs(computed_distance - sol->total_distance) > 1e-6) {
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

    /* Validate frozen requests are on their designated vehicle */
    if (ctx->has_frozen && ctx->frozen_vehicle_map) {
        for (r = 0; r < sol->base.total_requests; r++) {
            if (!sol->base.assigned_flags[r]) continue;
            if (ctx->frozen_vehicle_map[r] != SG_NO_VEHICLE &&
                sol->request_vehicle[r] != ctx->frozen_vehicle_map[r])
                goto done;
        }
    }

    /* Validate commodity and exclusion tracking */
    if (ctx->num_commodities > 0 && sol->route_commodities) {
        for (v = 0; v < sol->num_vehicles; v++) {
            const uint32_t *route = sg_route_vehicle_ptr_const(sol, v);
            uint32_t len = sol->route_lengths[v];
            uint64_t recomputed = 0;
            for (r = 0; r < len; r++) {
                uint32_t cid = ctx->requests[route[r]].commodity_id;
                if (cid > 0) {
                    recomputed |= (1ULL << (cid - 1));
                }
            }
            if (sol->route_commodities[v] != recomputed) {
                goto done;
            }
            /* Check for actual conflicts */
            for (r = 0; r < len; r++) {
                uint32_t cid = ctx->requests[route[r]].commodity_id;
                if (cid > 0 && (ctx->commodity_conflicts[cid - 1] & recomputed & ~(1ULL << (cid - 1)))) {
                    goto done;
                }
            }
        }
    }

    if (ctx->num_exclusion_groups > 0 && sol->route_exclusion_counts) {
        for (v = 0; v < sol->num_vehicles; v++) {
            const uint32_t *route = sg_route_vehicle_ptr_const(sol, v);
            uint32_t len = sol->route_lengths[v];
            uint32_t g;
            for (g = 0; g < ctx->num_exclusion_groups; g++) {
                uint32_t recount = 0;
                for (r = 0; r < len; r++) {
                    const SGRequestRecord *req = &ctx->requests[route[r]];
                    uint16_t gi;
                    for (gi = 0; gi < req->num_exclusion_groups; gi++) {
                        if (req->exclusion_group_ids[gi] == g) {
                            recount++;
                        }
                    }
                }
                if (sol->route_exclusion_counts[(size_t)v * ctx->num_exclusion_groups + g] != recount) {
                    goto done;
                }
                if (recount > 1) {
                    goto done;
                }
            }
        }
    }

    ok = 1;

done:
    free(seen_assigned);
    return ok;
}

/* Depot dock capacity: sweep-line overlap penalty */
static double sg_compute_depot_overlap_penalty(const SGContext *ctx,
                                                const SGRouteSolution *sol) {
    typedef struct { double time; int delta; } DepotEvent;
    double total_penalty = 0.0;
    uint32_t depot_id;
    DepotEvent *events = NULL;
    uint32_t events_cap = 0;

    for (depot_id = 0; depot_id < ctx->num_depots; depot_id++) {
        uint32_t max_sim = ctx->depots[depot_id].max_simultaneous;
        uint32_t event_count = 0;
        uint32_t v;
        uint32_t i, j;
        int running, max_running;

        if (max_sim == 0) {
            continue;
        }

        /* Count events needed for this depot */
        for (v = 0; v < sol->num_vehicles; v++) {
            const SGVehicleRecord *vehicle = &ctx->vehicles[v];
            if (!vehicle->has_depots) continue;
            if (sol->route_stop_lengths[v] == 0) continue;

            if (!vehicle->open_start && vehicle->start_depot_id == depot_id && vehicle->depot_loading_seconds > 0) {
                event_count += 2;
            }
            if (!vehicle->open_end && vehicle->end_depot_id == depot_id &&
                vehicle->depot_unloading_seconds > 0) {
                event_count += 2;
            }

            /* Inter-trip reload events */
            if (vehicle->has_multi_trip) {
                const SGRouteStop *stops = &sol->route_stops[(size_t)v * sol->stop_stride];
                uint32_t slen = sol->route_stop_lengths[v];
                uint32_t s;
                for (s = 1; s < slen; s++) {
                    if (!stops[s].trip_start) continue;
                    if (vehicle->end_depot_id == depot_id && vehicle->depot_unloading_seconds > 0) {
                        event_count += 2;
                    }
                    if (vehicle->start_depot_id == depot_id && vehicle->depot_loading_seconds > 0) {
                        event_count += 2;
                    }
                }
            }
        }

        if (event_count == 0) {
            continue;
        }

        /* Ensure events array is large enough */
        if (event_count > events_cap) {
            DepotEvent *new_events = (DepotEvent *)realloc(events,
                (size_t)event_count * sizeof(DepotEvent));
            if (!new_events) {
                continue;  /* Skip this depot on OOM */
            }
            events = new_events;
            events_cap = event_count;
        }

        /* Build events */
        event_count = 0;
        for (v = 0; v < sol->num_vehicles; v++) {
            const SGVehicleRecord *vehicle = &ctx->vehicles[v];
            if (!vehicle->has_depots) continue;
            if (sol->route_stop_lengths[v] == 0) continue;

            /* Start depot: occupancy [depart - loading_seconds, depart) */
            if (!vehicle->open_start && vehicle->start_depot_id == depot_id && vehicle->depot_loading_seconds > 0) {
                double depart = sol->route_depot_depart[v];
                double occupy_start = depart - (double)vehicle->depot_loading_seconds;
                events[event_count].time = occupy_start;
                events[event_count].delta = +1;
                event_count++;
                events[event_count].time = depart;
                events[event_count].delta = -1;
                event_count++;
            }

            /* End depot: occupancy [return, return + unloading_seconds) */
            if (!vehicle->open_end && vehicle->end_depot_id == depot_id &&
                vehicle->depot_unloading_seconds > 0) {
                double ret = sol->route_depot_return[v];
                if (ret > 0.0) {
                    events[event_count].time = ret;
                    events[event_count].delta = +1;
                    event_count++;
                    events[event_count].time = ret + (double)vehicle->depot_unloading_seconds;
                    events[event_count].delta = -1;
                    event_count++;
                }
            }

            /* Inter-trip reload events */
            if (vehicle->has_multi_trip) {
                const SGRouteStop *stops = &sol->route_stops[(size_t)v * sol->stop_stride];
                uint32_t slen = sol->route_stop_lengths[v];
                uint32_t s;
                for (s = 1; s < slen; s++) {
                    if (!stops[s].trip_start) continue;
                    double ret = stops[s].trip_depot_return;
                    double dep = stops[s].trip_depot_depart;

                    /* Unloading at end depot: [return, return + unloading_seconds) */
                    if (vehicle->end_depot_id == depot_id && vehicle->depot_unloading_seconds > 0) {
                        events[event_count].time = ret;
                        events[event_count].delta = +1;
                        event_count++;
                        events[event_count].time = ret + (double)vehicle->depot_unloading_seconds;
                        events[event_count].delta = -1;
                        event_count++;
                    }
                    /* Loading at start depot: [depart - loading_seconds, depart) */
                    if (vehicle->start_depot_id == depot_id && vehicle->depot_loading_seconds > 0) {
                        events[event_count].time = dep - (double)vehicle->depot_loading_seconds;
                        events[event_count].delta = +1;
                        event_count++;
                        events[event_count].time = dep;
                        events[event_count].delta = -1;
                        event_count++;
                    }
                }
            }
        }

        if (event_count == 0) {
            continue;
        }

        /* Sort events by time. Ties: departures (-1) before arrivals (+1)
           so that a vehicle leaving a dock before another arrives doesn't count as overlap. */
        for (i = 1; i < event_count; i++) {
            DepotEvent key = events[i];
            j = i;
            while (j > 0 && (events[j - 1].time > key.time ||
                              (events[j - 1].time == key.time &&
                               events[j - 1].delta > key.delta))) {
                events[j] = events[j - 1];
                j--;
            }
            events[j] = key;
        }

        /* Sweep to find max concurrent occupancy */
        running = 0;
        max_running = 0;
        for (i = 0; i < event_count; i++) {
            running += events[i].delta;
            if (running > max_running) {
                max_running = running;
            }
        }

        if ((uint32_t)max_running > max_sim) {
            total_penalty += (double)((uint32_t)max_running - max_sim) * SG_DEPOT_CAPACITY_PENALTY;
        }
    }

    free(events);
    return total_penalty;
}

double sg_route_solution_cost(const void *solution, void *user_ctx) {
    const SGRouteSolution *sol = (const SGRouteSolution *)solution;
    const SGContext *ctx = (const SGContext *)user_ctx;
    double cost = 0.0;
    uint32_t v;

    if (!sol) {
        return INFINITY;
    }

    /* Per-request unassigned penalty */
    {
        uint32_t u;
        for (u = 0; u < sol->base.num_unassigned; u++) {
            uint32_t rid = sol->base.unassigned_ids[u];
            /* COMMITTED/FROZEN requests get effectively infinite penalty */
            if (ctx->has_committed && ctx->request_locks &&
                rid < ctx->num_requests &&
                ctx->request_locks[rid] >= SG_LOCK_COMMITTED) {
                cost += 1e12;
                continue;
            }
            if (rid < ctx->num_requests && ctx->requests[rid].has_unassigned_penalty) {
                cost += ctx->requests[rid].unassigned_penalty;
            } else {
                cost += ctx->unassigned_weight;
            }
        }
    }
    for (v = 0; v < sol->num_vehicles; v++) {
        if (sol->route_stop_lengths[v] > 0) {
            const SGVehicleRecord *vehicle = &ctx->vehicles[v];
            cost += vehicle->fixed_cost;
            cost += vehicle->cost_per_distance * sol->route_distance[v];
            if (sol->route_duration) {
                cost += vehicle->cost_per_duration * sol->route_duration[v];
            }
            if (sol->route_waiting) {
                cost += vehicle->cost_per_waiting * sol->route_waiting[v];
            }
            if (sol->route_overtime) {
                cost += vehicle->cost_per_overtime * sol->route_overtime[v];
            }
            if (sol->route_tw_penalty) {
                cost += sol->route_tw_penalty[v];
            }
        }
    }
    if (ctx->has_depot_capacity) {
        cost += sg_compute_depot_overlap_penalty(ctx, sol);
    }
    /* Infeasible-space penalty terms */
    if (ctx->penalty.enabled) {
        int k;
        for (k = 0; k < SG_PENALTY_COUNT; k++)
            cost += ctx->penalty.weight[k] * sol->violations[k];
    }
    /* Span balancing penalty */
    if (ctx->span_cost_duration != 0.0 || ctx->span_cost_distance != 0.0) {
        double min_dur = INFINITY, max_dur = -INFINITY;
        double min_dist = INFINITY, max_dist = -INFINITY;
        uint32_t active = 0;
        for (v = 0; v < sol->num_vehicles; v++) {
            if (sol->route_stop_lengths[v] > 0) {
                active++;
                if (ctx->span_cost_duration != 0.0 && sol->route_duration) {
                    double d = sol->route_duration[v];
                    if (d < min_dur) min_dur = d;
                    if (d > max_dur) max_dur = d;
                }
                if (ctx->span_cost_distance != 0.0) {
                    double d = sol->route_distance[v];
                    if (d < min_dist) min_dist = d;
                    if (d > max_dist) max_dist = d;
                }
            }
        }
        if (active >= 2) {
            if (ctx->span_cost_duration != 0.0 && max_dur > min_dur)
                cost += ctx->span_cost_duration * (max_dur - min_dur);
            if (ctx->span_cost_distance != 0.0 && max_dist > min_dist)
                cost += ctx->span_cost_distance * (max_dist - min_dist);
        }
    }
    return cost;
}

int sg_route_solution_size(const void *solution, void *user_ctx) {
    const SGRouteSolution *sol = (const SGRouteSolution *)solution;
    (void)user_ctx;
    return sol ? (int)sol->base.num_assigned : 0;
}

void sg_scratch_init(SGContext *ctx) {
    SGScratchBuffers *s;
    uint32_t num_req, num_veh, stop_cap, route_stride;
    size_t total;
    SHArena *arena;

    if (!ctx) return;
    s = &ctx->scratch;
    memset(s, 0, sizeof(*s));

    num_req = ctx->num_requests;
    num_veh = ctx->num_vehicles;
    if (num_req == 0 || num_veh == 0) return;

    stop_cap = num_req * 2U;
    route_stride = num_req;
    s->stop_capacity = stop_cap;

    #define ALIGN8(x) (((x) + 7U) & ~(size_t)7U)
    total = 0;
    /* timing: 5 arrays of stop_cap doubles */
    total += ALIGN8((size_t)stop_cap * 5U * sizeof(double));
    /* load_profile */
    if (ctx->dimension_count > 0) {
        total += ALIGN8(((size_t)stop_cap + 1U) * (size_t)ctx->dimension_count * sizeof(double));
        /* dim_scratch: 2 * dim_count doubles */
        total += ALIGN8((size_t)ctx->dimension_count * 2U * sizeof(double));
    }
    /* pickup_depart + pickup_seen */
    total += ALIGN8((size_t)num_req * sizeof(double));
    total += ALIGN8((size_t)num_req * sizeof(uint8_t));
    /* feas_stops */
    total += ALIGN8((size_t)stop_cap * sizeof(SGRouteStop));
    /* candidate_a, candidate_b */
    total += 2U * ALIGN8((size_t)route_stride * sizeof(uint32_t));
    /* exclusion_counts */
    if (ctx->num_exclusion_groups > 0) {
        total += ALIGN8((size_t)ctx->num_exclusion_groups * sizeof(uint32_t));
    }
    #undef ALIGN8

    arena = sh_arena_create(total);
    if (!arena) {
        memset(s, 0, sizeof(*s));
        return;
    }
    s->arena = arena;

    s->timing = (double *)sh_arena_alloc(arena, (size_t)stop_cap * 5U * sizeof(double));
    if (ctx->dimension_count > 0) {
        s->load_profile = (double *)sh_arena_alloc(arena,
            ((size_t)stop_cap + 1U) * (size_t)ctx->dimension_count * sizeof(double));
        s->dim_scratch = (double *)sh_arena_alloc(arena,
            (size_t)ctx->dimension_count * 2U * sizeof(double));
    }
    s->pickup_depart = (double *)sh_arena_alloc(arena, (size_t)num_req * sizeof(double));
    s->pickup_seen = (uint8_t *)sh_arena_alloc(arena, (size_t)num_req * sizeof(uint8_t));
    s->feas_stops = (SGRouteStop *)sh_arena_alloc(arena, (size_t)stop_cap * sizeof(SGRouteStop));
    s->candidate_a = (uint32_t *)sh_arena_alloc(arena, (size_t)route_stride * sizeof(uint32_t));
    s->candidate_b = (uint32_t *)sh_arena_alloc(arena, (size_t)route_stride * sizeof(uint32_t));
    if (ctx->num_exclusion_groups > 0) {
        s->exclusion_counts = (uint32_t *)sh_arena_alloc(arena,
            (size_t)ctx->num_exclusion_groups * sizeof(uint32_t));
    }
}

void sg_scratch_free(SGContext *ctx) {
    if (!ctx) return;
    if (ctx->scratch.arena) {
        sh_arena_free(ctx->scratch.arena);
    }
    memset(&ctx->scratch, 0, sizeof(ctx->scratch));
}
