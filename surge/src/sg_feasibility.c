#include "sg_internal.h"

int sg_route_stop_sequence_feasible(const SGContext *ctx, uint32_t vehicle_id,
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

int sg_route_sequence_feasible_distance(const SGContext *ctx, uint32_t vehicle_id,
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

void sg_route_insert_request(uint32_t *route, uint32_t *route_len, uint32_t insert_pos,
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

int sg_route_eval_insertion(const SGContext *ctx, const SGRouteSolution *sol,
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

ARStatus sg_route_apply_insertion(const SGContext *ctx, SGRouteSolution *sol,
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

ARStatus sg_route_unassign_request(const SGContext *ctx, SGRouteSolution *sol,
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

ARStatus sg_route_unassign_removed_requests(const SGContext *ctx, SGRouteSolution *sol,
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
