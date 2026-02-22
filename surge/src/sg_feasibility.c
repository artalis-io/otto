#include "sg_internal.h"

int sg_route_update_timing(const SGContext *ctx, SGRouteSolution *sol, uint32_t vehicle_id) {
    const SGVehicleRecord *vehicle;
    const SGDepotRecord *start_depot;
    const SGDepotRecord *end_depot;
    SGRouteStop *stops;
    uint32_t stop_len;
    double time_cursor;
    double depot_depart;
    uint32_t prev_loc;
    double distance = 0.0;
    double waiting = 0.0;
    double tw_penalty = 0.0;
    double latest_next;
    uint32_t i;

    if (!ctx || !sol || vehicle_id >= sol->num_vehicles) {
        return 0;
    }

    stop_len = sol->route_stop_lengths[vehicle_id];
    if (stop_len == 0) {
        sol->route_distance[vehicle_id] = 0.0;
        if (sol->route_duration) {
            sol->route_duration[vehicle_id] = 0.0;
        }
        if (sol->route_waiting) {
            sol->route_waiting[vehicle_id] = 0.0;
        }
        if (sol->route_overtime) {
            sol->route_overtime[vehicle_id] = 0.0;
        }
        if (sol->route_tw_penalty) {
            sol->route_tw_penalty[vehicle_id] = 0.0;
        }
        return 1;
    }

    vehicle = &ctx->vehicles[vehicle_id];
    if (!vehicle->has_depots || vehicle->start_depot_id >= ctx->num_depots ||
        vehicle->end_depot_id >= ctx->num_depots) {
        return 0;
    }
    {
        double _sx, _sy, _ex, _ey;
        if (!sg_vehicle_start_end_locations(ctx, vehicle_id, &_sx, &_sy, &_ex, &_ey)) {
            return 0;
        }
    }

    start_depot = &ctx->depots[vehicle->start_depot_id];
    end_depot = &ctx->depots[vehicle->end_depot_id];
    stops = sg_route_vehicle_stop_ptr(sol, vehicle_id);

    /* Forward pass */
    time_cursor = vehicle->has_shift_time_window ? (double)vehicle->shift_early : 0.0;
    if (start_depot->has_time_window) {
        if (time_cursor < (double)start_depot->tw_early) {
            time_cursor = (double)start_depot->tw_early;
        }
    }
    depot_depart = time_cursor;

    prev_loc = vehicle->start_location_id;
    for (i = 0; i < stop_len; i++) {
        SGRouteStop *stop = &stops[i];
        uint32_t cur_loc = ctx->tasks[stop->task_id].location_id;
        const SGTaskRecord *task = &ctx->tasks[stop->task_id];
        double dist, dur;
        double start;

        sg_travel(ctx, prev_loc, cur_loc, vehicle_id, &dist, &dur);
        distance += dist;
        time_cursor += dur;
        stop->arrival = time_cursor;

        start = sg_task_snap_forward(task, time_cursor);
        stop->service_start = start;
        stop->depart = start + (double)task->service_seconds;
        waiting += start - stop->arrival;
        if (task->has_soft_time_window) {
            if (start < (double)task->soft_tw_early) {
                tw_penalty += task->tw_early_penalty * ((double)task->soft_tw_early - start);
            } else if (start > (double)task->soft_tw_late) {
                tw_penalty += task->tw_late_penalty * (start - (double)task->soft_tw_late);
            }
        }
        time_cursor = stop->depart;
        prev_loc = cur_loc;
    }

    /* Add travel to end depot (skip for open-end routes) */
    if (!vehicle->open_end) {
        double dist, dur;
        sg_travel(ctx, prev_loc, vehicle->end_location_id, vehicle_id, &dist, &dur);
        distance += dist;
        time_cursor += dur;
    }

    /* Compute route duration */
    if (sol->route_duration) {
        sol->route_duration[vehicle_id] = time_cursor - depot_depart;
    }
    if (sol->route_waiting) {
        sol->route_waiting[vehicle_id] = waiting;
    }
    if (sol->route_overtime) {
        double ot = 0.0;
        if (vehicle->has_shift_time_window && time_cursor > (double)vehicle->shift_late) {
            ot = time_cursor - (double)vehicle->shift_late;
        }
        sol->route_overtime[vehicle_id] = ot;
    }
    if (sol->route_tw_penalty) {
        sol->route_tw_penalty[vehicle_id] = tw_penalty;
    }

    /* Backward pass */
    latest_next = (vehicle->has_shift_time_window && !(vehicle->cost_per_overtime > 0.0))
        ? (double)vehicle->shift_late : INFINITY;
    if (!vehicle->open_end) {
        if (end_depot->has_time_window && latest_next > (double)end_depot->tw_late) {
            latest_next = (double)end_depot->tw_late;
        }
    }
    if (vehicle->max_duration_seconds > 0) {
        double max_dur_bound = depot_depart + (double)vehicle->max_duration_seconds;
        if (max_dur_bound < latest_next) {
            latest_next = max_dur_bound;
        }
    }
    for (i = stop_len; i > 0; i--) {
        uint32_t idx = i - 1U;
        SGRouteStop *stop = &stops[idx];
        const SGTaskRecord *task = &ctx->tasks[stop->task_id];
        uint32_t cur_loc = task->location_id;
        double travel_to_next;

        if (idx + 1U < stop_len) {
            uint32_t next_loc = ctx->tasks[stops[idx + 1U].task_id].location_id;
            travel_to_next = sg_travel_dur(ctx, cur_loc, next_loc, vehicle_id);
        } else if (vehicle->open_end) {
            travel_to_next = 0.0;
        } else {
            travel_to_next = sg_travel_dur(ctx, cur_loc, vehicle->end_location_id, vehicle_id);
        }

        stop->latest_start = latest_next - travel_to_next - (double)task->service_seconds;
        if (task->has_time_window) {
            stop->latest_start = sg_task_snap_backward(task, stop->latest_start);
        }
        stop->forward_slack = stop->latest_start - stop->service_start;
        latest_next = stop->latest_start;
    }

    sol->route_distance[vehicle_id] = distance;
    return 1;
}

int sg_route_update_load(const SGContext *ctx, SGRouteSolution *sol, uint32_t vehicle_id) {
    SGRouteStop *stops;
    uint32_t stop_len;
    double *load;
    size_t dim_count;
    size_t base_offset;
    uint32_t i, d;

    if (!ctx || !sol || vehicle_id >= sol->num_vehicles) {
        return 0;
    }
    dim_count = (size_t)ctx->dimension_count;
    if (dim_count == 0) {
        return 1;
    }
    if (!sol->route_stop_load) {
        return 0;
    }

    stop_len = sol->route_stop_lengths[vehicle_id];
    base_offset = (size_t)vehicle_id * ((size_t)sol->stop_stride + 1U) * dim_count;
    load = sol->route_stop_load + base_offset;

    /* Initialize first slot to zero */
    for (d = 0; d < dim_count; d++) {
        load[d] = 0.0;
    }

    stops = sg_route_vehicle_stop_ptr(sol, vehicle_id);
    for (i = 0; i < stop_len; i++) {
        const SGTaskRecord *task = &ctx->tasks[stops[i].task_id];
        for (d = 0; d < (uint32_t)dim_count; d++) {
            double prev_load = load[(size_t)i * dim_count + d];
            double demand = (task->has_demand && task->demand) ? task->demand[d] : 0.0;
            load[((size_t)i + 1U) * dim_count + d] = prev_load + demand;
        }
    }

    return 1;
}

int sg_route_stop_sequence_feasible(const SGContext *ctx, uint32_t vehicle_id,
                                    const SGRouteStop *stops, uint32_t stop_count,
                                    double *distance_out) {
    const SGVehicleRecord *vehicle;
    const SGDepotRecord *start_depot;
    const SGDepotRecord *end_depot;
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
    uint32_t prev_loc;
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
    {
        double _sx, _sy, _ex, _ey;
        if (!sg_vehicle_start_end_locations(ctx, vehicle_id, &_sx, &_sy, &_ex, &_ey)) {
            return 0;
        }
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

    prev_loc = vehicle->start_location_id;
    for (i = 0; i < stop_count; i++) {
        const SGRouteStop *stop = &stops[i];
        const SGTaskRecord *task;
        const SGRequestRecord *request;
        double dist, dur;
        double start;
        double ride_time = 0.0;
        double ride_limit = INFINITY;
        uint32_t cur_loc;

        if (stop->request_id >= ctx->num_requests || stop->task_id >= ctx->num_tasks) {
            goto done;
        }
        task = &ctx->tasks[stop->task_id];
        request = &ctx->requests[stop->request_id];
        if (!task->has_location || !task->has_time_window) {
            goto done;
        }

        cur_loc = task->location_id;
        sg_travel(ctx, prev_loc, cur_loc, vehicle_id, &dist, &dur);
        if (!isfinite(dist) || dist < 0.0) {
            goto done;
        }
        distance += dist;
        time_cursor += dur;
        start = sg_task_snap_forward(task, time_cursor);
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
                if (request->has_max_ride_time) {
                    ride_limit = (double)request->max_ride_time_seconds;
                } else if (pickup_task->has_time_window && drop_task->has_time_window) {
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
        prev_loc = cur_loc;
    }

    if (!vehicle->open_end) {
        double dist, dur;
        sg_travel(ctx, prev_loc, vehicle->end_location_id, vehicle_id, &dist, &dur);
        if (!isfinite(dist) || dist < 0.0) {
            goto done;
        }
        distance += dist;
        time_cursor += dur;

        if (end_depot->has_time_window) {
            if (time_cursor < (double)end_depot->tw_early) {
                time_cursor = (double)end_depot->tw_early;
            }
            if (time_cursor > (double)end_depot->tw_late + 1e-9) {
                goto done;
            }
        }
    }
    if (vehicle->has_shift_time_window && time_cursor > (double)vehicle->shift_late + 1e-9
        && !(vehicle->cost_per_overtime > 0.0)) {
        goto done;
    }

    /* Max duration check */
    {
        double depot_depart_t = vehicle->has_shift_time_window ? (double)vehicle->shift_early : 0.0;
        if (start_depot->has_time_window && depot_depart_t < (double)start_depot->tw_early) {
            depot_depart_t = (double)start_depot->tw_early;
        }
        if (vehicle->max_duration_seconds > 0 &&
            (time_cursor - depot_depart_t) > (double)vehicle->max_duration_seconds + 1e-9) {
            goto done;
        }
    }

    latest_next = (vehicle->has_shift_time_window && !(vehicle->cost_per_overtime > 0.0))
        ? (double)vehicle->shift_late : INFINITY;
    if (!vehicle->open_end) {
        if (end_depot->has_time_window && latest_next > (double)end_depot->tw_late) {
            latest_next = (double)end_depot->tw_late;
        }
    }
    if (vehicle->max_duration_seconds > 0) {
        double depot_depart_t = vehicle->has_shift_time_window ? (double)vehicle->shift_early : 0.0;
        double max_dur_bound;
        if (start_depot->has_time_window && depot_depart_t < (double)start_depot->tw_early) {
            depot_depart_t = (double)start_depot->tw_early;
        }
        max_dur_bound = depot_depart_t + (double)vehicle->max_duration_seconds;
        if (max_dur_bound < latest_next) {
            latest_next = max_dur_bound;
        }
    }
    for (i = stop_count; i > 0; i--) {
        uint32_t idx = i - 1U;
        const SGTaskRecord *task = &ctx->tasks[stops[idx].task_id];
        uint32_t cur_loc = task->location_id;
        double travel_to_next;

        if (idx + 1U < stop_count) {
            uint32_t next_loc = ctx->tasks[stops[idx + 1U].task_id].location_id;
            travel_to_next = sg_travel_dur(ctx, cur_loc, next_loc, vehicle_id);
        } else if (vehicle->open_end) {
            travel_to_next = 0.0;
        } else {
            travel_to_next = sg_travel_dur(ctx, cur_loc, vehicle->end_location_id, vehicle_id);
        }

        if (!isfinite(travel_to_next) || travel_to_next < 0.0) {
            goto done;
        }
        latest_start[idx] = latest_next - travel_to_next - (double)task->service_seconds;
        if (task->has_time_window) {
            latest_start[idx] = sg_task_snap_backward(task, latest_start[idx]);
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

    score = (len == 0 ? ctx->vehicles[vehicle_id].fixed_cost : 0.0) +
            ctx->vehicles[vehicle_id].cost_per_distance * (new_distance - sol->route_distance[vehicle_id]);
    *score_out = score;
    *new_route_distance_out = new_distance;
    return 1;
}

int sg_route_eval_insertion_cached(const SGContext *ctx, const SGRouteSolution *sol,
                                   uint32_t request_id, uint32_t vehicle_id,
                                   uint32_t pos, double *score_out,
                                   double *new_route_distance_out) {
    const SGVehicleRecord *vehicle;
    const SGDepotRecord *end_depot;
    const uint32_t *route;
    const SGRouteStop *stops;
    uint32_t route_len;
    uint32_t stop_len;
    SGRouteStop new_stops[2];
    uint32_t new_stop_count = 0;
    double prev_depart;
    uint32_t prev_loc;
    uint32_t next_stop_idx;
    uint32_t prev_stop_idx;
    double old_segment, new_segment, delta, new_route_distance;
    double score;
    uint32_t ns;

    if (!ctx || !sol || !score_out || !new_route_distance_out ||
        vehicle_id >= sol->num_vehicles || request_id >= sol->base.total_requests) {
        return 0;
    }

    if (!sg_vehicle_qualifies(ctx, vehicle_id, request_id)) {
        return 0;
    }
    if (!sg_vehicle_allowed_for_request(ctx, vehicle_id, request_id)) {
        return 0;
    }

    vehicle = &ctx->vehicles[vehicle_id];
    if (!vehicle->has_depots || vehicle->start_depot_id >= ctx->num_depots ||
        vehicle->end_depot_id >= ctx->num_depots) {
        return 0;
    }
    {
        double _sx, _sy, _ex, _ey;
        if (!sg_vehicle_start_end_locations(ctx, vehicle_id, &_sx, &_sy, &_ex, &_ey)) {
            return 0;
        }
    }
    end_depot = &ctx->depots[vehicle->end_depot_id];

    route = sg_route_vehicle_ptr_const(sol, vehicle_id);
    stops = sg_route_vehicle_stop_ptr_const(sol, vehicle_id);
    route_len = sol->route_lengths[vehicle_id];
    stop_len = sol->route_stop_lengths[vehicle_id];

    if (pos > route_len || route_len >= sol->route_stride) {
        return 0;
    }

    /* Emit new stops for the request */
    if (!sg_request_emit_stops(ctx, request_id, new_stops, &new_stop_count)) {
        return 0;
    }

    /* Determine preceding timing state */
    if (pos == 0) {
        const SGDepotRecord *start_depot = &ctx->depots[vehicle->start_depot_id];
        prev_depart = vehicle->has_shift_time_window ? (double)vehicle->shift_early : 0.0;
        if (start_depot->has_time_window && prev_depart < (double)start_depot->tw_early) {
            prev_depart = (double)start_depot->tw_early;
        }
        prev_loc = vehicle->start_location_id;
        prev_stop_idx = UINT32_MAX;
    } else {
        /* Previous request's last stop */
        uint32_t prev_request = route[pos - 1];
        prev_stop_idx = sol->request_delivery_stop_pos[prev_request];
        if (prev_stop_idx >= stop_len) {
            return 0;
        }
        prev_depart = stops[prev_stop_idx].depart;
        prev_loc = ctx->tasks[stops[prev_stop_idx].task_id].location_id;
    }

    /* Determine next stop index */
    if (pos < route_len) {
        uint32_t next_request = route[pos];
        const SGRequestRecord *next_req_rec = &ctx->requests[next_request];
        if (next_req_rec->kind == SG_REQUEST_KIND_PICKUP_DELIVERY) {
            next_stop_idx = sol->request_pickup_stop_pos[next_request];
        } else {
            next_stop_idx = sol->request_delivery_stop_pos[next_request];
        }
        if (next_stop_idx >= stop_len) {
            return 0;
        }
    } else {
        next_stop_idx = UINT32_MAX;
    }

    /* Time check for new stop(s) */
    {
        double cursor = prev_depart;
        uint32_t c_loc = prev_loc;
        double pickup_depart_time = 0.0;

        for (ns = 0; ns < new_stop_count; ns++) {
            const SGTaskRecord *task = &ctx->tasks[new_stops[ns].task_id];
            uint32_t task_loc = task->location_id;
            double travel = sg_travel_dur(ctx, c_loc, task_loc, vehicle_id);
            double arrival_t = cursor + travel;
            double start_t = sg_task_snap_forward(task, arrival_t);
            if (start_t > (double)task->tw_late + 1e-9) {
                return 0;
            }

            new_stops[ns].arrival = arrival_t;
            new_stops[ns].service_start = start_t;
            new_stops[ns].depart = start_t + (double)task->service_seconds;

            if (new_stops[ns].is_pickup) {
                pickup_depart_time = new_stops[ns].depart;
            } else {
                /* Check PD ride time if this is a PD delivery */
                const SGRequestRecord *req_rec = &ctx->requests[new_stops[ns].request_id];
                if (req_rec->kind == SG_REQUEST_KIND_PICKUP_DELIVERY) {
                    double ride_time = start_t - pickup_depart_time;
                    double ride_limit = INFINITY;
                    const SGTaskRecord *pickup_task = &ctx->tasks[req_rec->pickup_task_id];
                    const SGTaskRecord *drop_task = &ctx->tasks[req_rec->delivery_task_id];
                    if (pickup_task->has_time_window && drop_task->has_time_window) {
                        ride_limit = (double)(drop_task->tw_late - pickup_task->tw_early);
                    }
                    if (isfinite(ride_limit) && ride_time > ride_limit + 1e-9) {
                        return 0;
                    }
                }
            }

            cursor = new_stops[ns].depart;
            c_loc = task_loc;
        }

        /* Check push on next existing stop (O(1)) */
        if (next_stop_idx != UINT32_MAX) {
            uint32_t next_loc = ctx->tasks[stops[next_stop_idx].task_id].location_id;
            double new_arrival_at_next = cursor + sg_travel_dur(ctx, c_loc, next_loc, vehicle_id);
            if (new_arrival_at_next > stops[next_stop_idx].latest_start + 1e-9) {
                return 0;
            }
        } else {
            /* At end of route: check return to end depot (or shift/duration for open-end) */
            if (vehicle->open_end) {
                if (vehicle->has_shift_time_window && cursor > (double)vehicle->shift_late + 1e-9
                    && !(vehicle->cost_per_overtime > 0.0)) {
                    return 0;
                }
                if (vehicle->max_duration_seconds > 0) {
                    const SGDepotRecord *sd = &ctx->depots[vehicle->start_depot_id];
                    double dd = vehicle->has_shift_time_window ? (double)vehicle->shift_early : 0.0;
                    if (sd->has_time_window && dd < (double)sd->tw_early) {
                        dd = (double)sd->tw_early;
                    }
                    if ((cursor - dd) > (double)vehicle->max_duration_seconds + 1e-9) {
                        return 0;
                    }
                }
            } else {
                double arrival_at_end = cursor + sg_travel_dur(ctx, c_loc, vehicle->end_location_id, vehicle_id);
                if (end_depot->has_time_window && arrival_at_end > (double)end_depot->tw_late + 1e-9) {
                    return 0;
                }
                if (vehicle->has_shift_time_window && arrival_at_end > (double)vehicle->shift_late + 1e-9
                    && !(vehicle->cost_per_overtime > 0.0)) {
                    return 0;
                }
                if (vehicle->max_duration_seconds > 0) {
                    const SGDepotRecord *sd = &ctx->depots[vehicle->start_depot_id];
                    double dd = vehicle->has_shift_time_window ? (double)vehicle->shift_early : 0.0;
                    if (sd->has_time_window && dd < (double)sd->tw_early) {
                        dd = (double)sd->tw_early;
                    }
                    if ((arrival_at_end - dd) > (double)vehicle->max_duration_seconds + 1e-9) {
                        return 0;
                    }
                }
            }
        }
    }

    /* Capacity check (signed-load model: max_prefix - min_prefix <= capacity) */
    if (ctx->dimension_count > 0 && sol->route_stop_load) {
        size_t dim_count = (size_t)ctx->dimension_count;
        size_t load_base = (size_t)vehicle_id * ((size_t)sol->stop_stride + 1U) * dim_count;
        const double *load = sol->route_stop_load + load_base;
        uint32_t insert_stop_pos;
        uint32_t d;

        /* Find where in the stop sequence the insertion occurs */
        if (next_stop_idx != UINT32_MAX) {
            insert_stop_pos = next_stop_idx;
        } else {
            insert_stop_pos = stop_len;
        }

        for (d = 0; d < ctx->dimension_count; d++) {
            double cap = (vehicle->has_capacity && vehicle->capacity)
                         ? vehicle->capacity[d] : INFINITY;
            double added = 0.0;
            double hyp_min = INFINITY;
            double hyp_max = -INFINITY;
            double load_at_insert;
            uint32_t s;

            /* Compute total demand of new stops */
            for (ns = 0; ns < new_stop_count; ns++) {
                const SGTaskRecord *task = &ctx->tasks[new_stops[ns].task_id];
                double demand = (task->has_demand && task->demand) ? task->demand[d] : 0.0;
                added += demand;
            }

            /* Upstream prefix sums (unchanged) */
            for (s = 0; s <= insert_stop_pos; s++) {
                double val = load[(size_t)s * dim_count + d];
                if (val < hyp_min) hyp_min = val;
                if (val > hyp_max) hyp_max = val;
            }

            /* New stop prefix sums */
            load_at_insert = load[(size_t)insert_stop_pos * dim_count + d];
            {
                double partial = 0.0;
                for (ns = 0; ns < new_stop_count; ns++) {
                    const SGTaskRecord *task = &ctx->tasks[new_stops[ns].task_id];
                    double demand = (task->has_demand && task->demand) ? task->demand[d] : 0.0;
                    partial += demand;
                    {
                        double val = load_at_insert + partial;
                        if (val < hyp_min) hyp_min = val;
                        if (val > hyp_max) hyp_max = val;
                    }
                }
            }

            /* Downstream prefix sums (shifted by added) */
            for (s = insert_stop_pos + 1; s <= stop_len; s++) {
                double val = load[(size_t)s * dim_count + d] + added;
                if (val < hyp_min) hyp_min = val;
                if (val > hyp_max) hyp_max = val;
            }

            if ((hyp_max - hyp_min) > cap + SG_DEMAND_TOLERANCE) {
                return 0;
            }
        }
    }

    /* Compute distance delta */
    {
        uint32_t last_new_loc = ctx->tasks[new_stops[new_stop_count - 1].task_id].location_id;
        uint32_t first_new_loc = ctx->tasks[new_stops[0].task_id].location_id;

        if (next_stop_idx != UINT32_MAX) {
            uint32_t next_loc = ctx->tasks[stops[next_stop_idx].task_id].location_id;
            old_segment = sg_travel_dist(ctx, prev_loc, next_loc);
            new_segment = sg_travel_dist(ctx, prev_loc, first_new_loc);
            if (new_stop_count == 2) {
                new_segment += sg_travel_dist(ctx, first_new_loc, last_new_loc);
            }
            new_segment += sg_travel_dist(ctx, last_new_loc, next_loc);
        } else {
            if (vehicle->open_end) {
                old_segment = 0.0;
                new_segment = sg_travel_dist(ctx, prev_loc, first_new_loc);
                if (new_stop_count == 2) {
                    new_segment += sg_travel_dist(ctx, first_new_loc, last_new_loc);
                }
            } else {
                old_segment = sg_travel_dist(ctx, prev_loc, vehicle->end_location_id);
                new_segment = sg_travel_dist(ctx, prev_loc, first_new_loc);
                if (new_stop_count == 2) {
                    new_segment += sg_travel_dist(ctx, first_new_loc, last_new_loc);
                }
                new_segment += sg_travel_dist(ctx, last_new_loc, vehicle->end_location_id);
            }
        }
    }

    delta = new_segment - old_segment;
    new_route_distance = sol->route_distance[vehicle_id] + delta;

    score = (route_len == 0 ? vehicle->fixed_cost : 0.0) +
            vehicle->cost_per_distance * (new_route_distance - sol->route_distance[vehicle_id]);
    *score_out = score;
    *new_route_distance_out = new_route_distance;
    return 1;
}

int sg_route_eval_pd_best_insertion_cached(
    const SGContext *ctx, const SGRouteSolution *sol,
    uint32_t request_id, uint32_t vehicle_id,
    double *best_score_out, uint32_t *best_pickup_pos_out,
    uint32_t *best_delivery_pos_out, double *best_route_distance_out) {

    const SGVehicleRecord *vehicle;
    const SGDepotRecord *start_depot;
    const SGDepotRecord *end_depot;
    const SGRequestRecord *request;
    const SGTaskRecord *pickup_task;
    const SGTaskRecord *delivery_task;
    const SGRouteStop *stops;
    uint32_t stop_len;
    uint32_t pickup_loc;
    uint32_t delivery_loc;
    double best_score = INFINITY;
    uint32_t best_i = UINT32_MAX, best_j = UINT32_MAX;
    double best_dist = 0.0;
    double depot_depart;
    double ride_limit;
    uint32_t i;
    int found = 0;

    if (!ctx || !sol || !best_score_out || !best_pickup_pos_out ||
        !best_delivery_pos_out || !best_route_distance_out ||
        vehicle_id >= sol->num_vehicles || request_id >= sol->base.total_requests) {
        return 0;
    }

    if (!sg_vehicle_qualifies(ctx, vehicle_id, request_id)) {
        return 0;
    }
    if (!sg_vehicle_allowed_for_request(ctx, vehicle_id, request_id)) {
        return 0;
    }

    request = &ctx->requests[request_id];
    if (request->kind != SG_REQUEST_KIND_PICKUP_DELIVERY) {
        return 0;
    }
    if (!request->has_pickup_task || !request->has_delivery_task ||
        request->pickup_task_id >= ctx->num_tasks ||
        request->delivery_task_id >= ctx->num_tasks) {
        return 0;
    }

    pickup_task = &ctx->tasks[request->pickup_task_id];
    delivery_task = &ctx->tasks[request->delivery_task_id];
    if (!pickup_task->has_location || !pickup_task->has_time_window ||
        !delivery_task->has_location || !delivery_task->has_time_window) {
        return 0;
    }

    pickup_loc = pickup_task->location_id;
    delivery_loc = delivery_task->location_id;

    vehicle = &ctx->vehicles[vehicle_id];
    if (!vehicle->has_depots || vehicle->start_depot_id >= ctx->num_depots ||
        vehicle->end_depot_id >= ctx->num_depots) {
        return 0;
    }
    {
        double _sx, _sy, _ex, _ey;
        if (!sg_vehicle_start_end_locations(ctx, vehicle_id, &_sx, &_sy, &_ex, &_ey)) {
            return 0;
        }
    }

    start_depot = &ctx->depots[vehicle->start_depot_id];
    end_depot = &ctx->depots[vehicle->end_depot_id];
    stops = sg_route_vehicle_stop_ptr_const(sol, vehicle_id);
    stop_len = sol->route_stop_lengths[vehicle_id];

    depot_depart = vehicle->has_shift_time_window ? (double)vehicle->shift_early : 0.0;
    if (start_depot->has_time_window && depot_depart < (double)start_depot->tw_early) {
        depot_depart = (double)start_depot->tw_early;
    }

    /* Ride time limit */
    if (request->has_max_ride_time) {
        ride_limit = (double)request->max_ride_time_seconds;
    } else {
        ride_limit = (double)(delivery_task->tw_late - pickup_task->tw_early);
    }

    /* For each pickup position i = 0..stop_len */
    for (i = 0; i <= stop_len; i++) {
        double prev_depart;
        uint32_t prev_loc;
        double p_travel, p_arrival, p_start, p_depart;
        uint32_t j;

        /* A. Compute pickup timing */
        if (i == 0) {
            prev_depart = depot_depart;
            prev_loc = vehicle->start_location_id;
        } else {
            prev_depart = stops[i - 1].depart;
            prev_loc = ctx->tasks[stops[i - 1].task_id].location_id;
        }

        p_travel = sg_travel_dur(ctx, prev_loc, pickup_loc, vehicle_id);
        p_arrival = prev_depart + p_travel;
        p_start = sg_task_snap_forward(pickup_task, p_arrival);
        if (p_start > (double)pickup_task->tw_late + 1e-9) {
            break; /* Later i only arrives later at pickup */
        }
        p_depart = p_start + (double)pickup_task->service_seconds;

        /* B. Propagate push and check delivery positions */
        {
            /* push[k] for k = i..stop_len-1:
               how much stop[k] would be delayed by inserting pickup before it */
            double push_k = 0.0;

            for (j = i + 1; j <= stop_len + 1; j++) {
                double d_prev_depart;
                uint32_t d_prev_loc;
                double d_travel, d_arrival, d_start, d_depart;
                double ride_time;
                double delta;
                double new_route_distance, score;

                /* -- Try delivery at position j -- */

                /* Delivery arrival */
                if (j == i + 1) {
                    /* Delivery immediately after pickup */
                    d_prev_depart = p_depart;
                    d_prev_loc = pickup_loc;
                } else {
                    /* Delivery after stop[j-1] (which has been pushed) */
                    d_prev_depart = stops[j - 2].depart + push_k;
                    d_prev_loc = ctx->tasks[stops[j - 2].task_id].location_id;
                }

                d_travel = sg_travel_dur(ctx, d_prev_loc, delivery_loc, vehicle_id);
                d_arrival = d_prev_depart + d_travel;
                d_start = sg_task_snap_forward(delivery_task, d_arrival);
                if (d_start > (double)delivery_task->tw_late + 1e-9) {
                    break; /* Later j only makes it worse */
                }

                /* Ride time check */
                ride_time = d_start - p_depart;
                if (isfinite(ride_limit) && ride_time > ride_limit + 1e-9) {
                    break; /* Later j is worse */
                }

                d_depart = d_start + (double)delivery_task->service_seconds;

                /* Check push on stop after delivery */
                if (j <= stop_len) {
                    /* There is a stop[j-1] in the original array at index j-1.
                       After inserting pickup at i, original stop at index j-1
                       becomes the stop after delivery. */
                    uint32_t next_loc = ctx->tasks[stops[j - 1].task_id].location_id;
                    double new_arrival_next = d_depart +
                        sg_travel_dur(ctx, delivery_loc, next_loc, vehicle_id);
                    /* The pushed latest_start of stop[j-1] */
                    if (new_arrival_next > stops[j - 1].latest_start + 1e-9) {
                        /* But maybe further j could still work if this one fails
                           due to delivery distance. Actually no - further j means
                           the delivery is further away and arrival is later.
                           However, push propagation for the stop right after delivery
                           depends on delivery placement, not just monotonic.
                           Be conservative: continue to try next j. */
                        goto next_j;
                    }
                } else {
                    /* j == stop_len + 1: delivery at end of route */
                    if (vehicle->open_end) {
                        if (vehicle->has_shift_time_window &&
                            d_depart > (double)vehicle->shift_late + 1e-9
                            && !(vehicle->cost_per_overtime > 0.0)) {
                            goto next_j;
                        }
                        if (vehicle->max_duration_seconds > 0 &&
                            (d_depart - depot_depart) > (double)vehicle->max_duration_seconds + 1e-9) {
                            goto next_j;
                        }
                    } else {
                        double arrival_at_end = d_depart +
                            sg_travel_dur(ctx, delivery_loc, vehicle->end_location_id, vehicle_id);
                        if (end_depot->has_time_window &&
                            arrival_at_end > (double)end_depot->tw_late + 1e-9) {
                            goto next_j;
                        }
                        if (vehicle->has_shift_time_window &&
                            arrival_at_end > (double)vehicle->shift_late + 1e-9
                            && !(vehicle->cost_per_overtime > 0.0)) {
                            goto next_j;
                        }
                        if (vehicle->max_duration_seconds > 0 &&
                            (arrival_at_end - depot_depart) > (double)vehicle->max_duration_seconds + 1e-9) {
                            goto next_j;
                        }
                    }
                }

                /* Capacity check: between pickup (at i) and delivery (at j),
                   load increases by pickup demand. Check all stops in [i, j-1). */
                if (ctx->dimension_count > 0 && sol->route_stop_load) {
                    size_t dim_count = (size_t)ctx->dimension_count;
                    size_t load_base = (size_t)vehicle_id *
                        ((size_t)sol->stop_stride + 1U) * dim_count;
                    const double *load = sol->route_stop_load + load_base;
                    uint32_t d;
                    int cap_ok = 1;

                    for (d = 0; d < ctx->dimension_count && cap_ok; d++) {
                        double cap = (vehicle->has_capacity && vehicle->capacity)
                                     ? vehicle->capacity[d] : INFINITY;
                        double pickup_demand = (pickup_task->has_demand && pickup_task->demand)
                                               ? pickup_task->demand[d] : 0.0;
                        double delivery_demand = (delivery_task->has_demand && delivery_task->demand)
                                                 ? delivery_task->demand[d] : 0.0;
                        /* Check prefix sums: from position i to j-1 in original stop array,
                           load is increased by pickup_demand. After j-1, it's increased by
                           pickup_demand + delivery_demand (which should be ~0 for PD). */
                        uint32_t s;
                        double hyp_min = INFINITY, hyp_max = -INFINITY;

                        /* Before pickup insertion point: unchanged */
                        for (s = 0; s <= i; s++) {
                            double val = load[s * dim_count + d];
                            if (val < hyp_min) hyp_min = val;
                            if (val > hyp_max) hyp_max = val;
                        }
                        /* After pickup: load[i] + pickup_demand */
                        {
                            double val = load[i * dim_count + d] + pickup_demand;
                            if (val < hyp_min) hyp_min = val;
                            if (val > hyp_max) hyp_max = val;
                        }
                        /* Between pickup and delivery: original loads shifted by pickup_demand */
                        for (s = i + 1; s <= j - 1 && s <= stop_len; s++) {
                            double val = load[s * dim_count + d] + pickup_demand;
                            if (val < hyp_min) hyp_min = val;
                            if (val > hyp_max) hyp_max = val;
                        }
                        /* After delivery: load[j-1] + pickup + delivery */
                        {
                            uint32_t load_idx = j - 1 <= stop_len ? j - 1 : stop_len;
                            double val = load[load_idx * dim_count + d] + pickup_demand + delivery_demand;
                            if (val < hyp_min) hyp_min = val;
                            if (val > hyp_max) hyp_max = val;
                        }
                        /* After delivery insertion: original loads shifted by pickup + delivery */
                        for (s = j; s <= stop_len; s++) {
                            double val = load[s * dim_count + d] + pickup_demand + delivery_demand;
                            if (val < hyp_min) hyp_min = val;
                            if (val > hyp_max) hyp_max = val;
                        }

                        if ((hyp_max - hyp_min) > cap + SG_DEMAND_TOLERANCE) {
                            cap_ok = 0;
                        }
                    }
                    if (!cap_ok) {
                        goto next_j;
                    }
                }

                /* Distance delta */
                {
                    double old_seg_pickup, new_seg_pickup;
                    double old_seg_delivery, new_seg_delivery;

                    /* Pickup segment: prev_of_i -> stop[i] becomes prev_of_i -> pickup -> ... */
                    if (j == i + 1) {
                        /* Adjacent: prev -> pickup -> delivery -> stop[i] (or end) */
                        if (i < stop_len) {
                            uint32_t next_loc = ctx->tasks[stops[i].task_id].location_id;
                            old_seg_pickup = sg_travel_dist(ctx, prev_loc, next_loc);
                            new_seg_pickup = sg_travel_dist(ctx, prev_loc, pickup_loc) +
                                              sg_travel_dist(ctx, pickup_loc, delivery_loc) +
                                              sg_travel_dist(ctx, delivery_loc, next_loc);
                        } else if (vehicle->open_end) {
                            old_seg_pickup = 0.0;
                            new_seg_pickup = sg_travel_dist(ctx, prev_loc, pickup_loc) +
                                              sg_travel_dist(ctx, pickup_loc, delivery_loc);
                        } else {
                            uint32_t next_loc = vehicle->end_location_id;
                            old_seg_pickup = sg_travel_dist(ctx, prev_loc, next_loc);
                            new_seg_pickup = sg_travel_dist(ctx, prev_loc, pickup_loc) +
                                              sg_travel_dist(ctx, pickup_loc, delivery_loc) +
                                              sg_travel_dist(ctx, delivery_loc, next_loc);
                        }
                        delta = new_seg_pickup - old_seg_pickup;
                    } else {
                        /* Non-adjacent: two separate segment changes */
                        uint32_t stop_i_loc;
                        uint32_t stop_jm1_loc; /* stop at j-2 in original */

                        /* Pickup segment: prev -> stop[i] becomes prev -> pickup -> stop[i] */
                        stop_i_loc = ctx->tasks[stops[i].task_id].location_id;
                        old_seg_pickup = sg_travel_dist(ctx, prev_loc, stop_i_loc);
                        new_seg_pickup = sg_travel_dist(ctx, prev_loc, pickup_loc) +
                                          sg_travel_dist(ctx, pickup_loc, stop_i_loc);

                        /* Delivery segment: stop[j-2] -> stop[j-1] becomes
                           stop[j-2] -> delivery -> stop[j-1] */
                        stop_jm1_loc = ctx->tasks[stops[j - 2].task_id].location_id;
                        if (j - 1 < stop_len) {
                            uint32_t after_d_loc = ctx->tasks[stops[j - 1].task_id].location_id;
                            old_seg_delivery = sg_travel_dist(ctx, stop_jm1_loc, after_d_loc);
                            new_seg_delivery = sg_travel_dist(ctx, stop_jm1_loc, delivery_loc) +
                                                sg_travel_dist(ctx, delivery_loc, after_d_loc);
                        } else if (vehicle->open_end) {
                            old_seg_delivery = 0.0;
                            new_seg_delivery = sg_travel_dist(ctx, stop_jm1_loc, delivery_loc);
                        } else {
                            uint32_t after_d_loc = vehicle->end_location_id;
                            old_seg_delivery = sg_travel_dist(ctx, stop_jm1_loc, after_d_loc);
                            new_seg_delivery = sg_travel_dist(ctx, stop_jm1_loc, delivery_loc) +
                                                sg_travel_dist(ctx, delivery_loc, after_d_loc);
                        }
                        delta = (new_seg_pickup - old_seg_pickup) +
                                (new_seg_delivery - old_seg_delivery);
                    }
                }

                new_route_distance = sol->route_distance[vehicle_id] + delta;
                score = (stop_len == 0 ? vehicle->fixed_cost : 0.0) +
                        vehicle->cost_per_distance * delta;

                if (score < best_score) {
                    best_score = score;
                    best_i = i;
                    best_j = j;
                    best_dist = new_route_distance;
                    found = 1;
                }

                next_j:

                /* Update push propagation for next j iteration:
                   push on stop[j-1] (original index) for the next inner loop step. */
                if (j - 1 < stop_len) {
                    /* When j was i+1, we haven't started tracking push yet.
                       push_k tracks push on stop[j-1] in the original array. */
                    if (j == i + 1) {
                        /* First stop after pickup: push = how much pickup delays it */
                        double orig_arrival = stops[i].arrival;
                        uint32_t stop_i_loc = ctx->tasks[stops[i].task_id].location_id;
                        double new_arr = p_depart +
                            sg_travel_dur(ctx, pickup_loc, stop_i_loc, vehicle_id);
                        push_k = new_arr - orig_arrival;
                        if (push_k < 0.0) push_k = 0.0;
                    } else {
                        /* Propagate: push on stop[j-1] from push on stop[j-2] */
                        double wait = stops[j - 1].service_start - stops[j - 1].arrival;
                        push_k = push_k - wait;
                        if (push_k < 0.0) push_k = 0.0;
                    }
                    /* Check if push exceeds forward slack - if so, no more
                       delivery positions past here can work */
                    if (push_k > stops[j - 1].forward_slack + 1e-9) {
                        break;
                    }
                }
            } /* end for j */
        }
    } /* end for i */

    if (found) {
        *best_score_out = best_score;
        *best_pickup_pos_out = best_i;
        *best_delivery_pos_out = best_j;
        *best_route_distance_out = best_dist;
    }
    return found;
}

ARStatus sg_route_apply_insertion(const SGContext *ctx, SGRouteSolution *sol,
                                  uint32_t request_id, uint32_t vehicle_id,
                                  uint32_t pos, double new_route_distance) {
    uint32_t *route;
    uint32_t old_len;
    double old_distance;
    uint32_t r;
    ARStatus status;

    (void)new_route_distance;

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

    if (old_len == 0) {
        sol->vehicles_used++;
    }

    status = sg_bootstrap_assign_request(&sol->base, request_id);
    if (status != AR_STATUS_OK) {
        return status;
    }

    /* Use direct stop splice instead of rebuild (preserves non-adjacent PD stops) */
    {
        SGRouteStop emitted_stops[2];
        uint32_t emitted_count = 0;
        uint32_t stop_insert_pos;

        if (!sg_request_emit_stops(ctx, request_id, emitted_stops, &emitted_count)) {
            return AR_STATUS_ERROR;
        }

        /* Compute stop-level insertion position from request-level position */
        if (pos == 0) {
            stop_insert_pos = 0;
        } else {
            /* Insert after the last stop of the preceding request */
            uint32_t prev_request = route[pos - 1];
            stop_insert_pos = sol->request_delivery_stop_pos[prev_request];
            if (stop_insert_pos == UINT32_MAX) {
                return AR_STATUS_ERROR;
            }
            stop_insert_pos++;
        }

        if (emitted_count == 1) {
            /* Delivery-only */
            if (!sg_route_splice_stop(ctx, sol, vehicle_id, stop_insert_pos, &emitted_stops[0])) {
                return AR_STATUS_ERROR;
            }
        } else if (emitted_count == 2) {
            /* PD: insert pickup then delivery adjacently */
            if (!sg_route_splice_stop(ctx, sol, vehicle_id, stop_insert_pos, &emitted_stops[0])) {
                return AR_STATUS_ERROR;
            }
            if (!sg_route_splice_stop(ctx, sol, vehicle_id, stop_insert_pos + 1, &emitted_stops[1])) {
                return AR_STATUS_ERROR;
            }
        } else {
            return AR_STATUS_ERROR;
        }
    }

    sg_route_update_timing(ctx, sol, vehicle_id);
    sg_route_update_load(ctx, sol, vehicle_id);
    sol->total_distance += (sol->route_distance[vehicle_id] - old_distance);

    return AR_STATUS_OK;
}

ARStatus sg_route_apply_pd_insertion(const SGContext *ctx, SGRouteSolution *sol,
                                     uint32_t request_id, uint32_t vehicle_id,
                                     uint32_t pickup_stop_pos, uint32_t delivery_stop_pos,
                                     double new_route_distance) {
    uint32_t *route;
    uint32_t old_len;
    double old_distance;
    uint32_t insert_req_pos;
    uint32_t r;
    ARStatus status;
    SGRouteStop emitted[2];
    uint32_t emitted_count = 0;
    uint32_t adj_delivery_pos;

    (void)new_route_distance;

    if (!ctx || !sol || request_id >= sol->base.total_requests ||
        vehicle_id >= sol->num_vehicles) {
        return AR_STATUS_INVALID_ARG;
    }
    if (sol->base.assigned_flags[request_id]) {
        return AR_STATUS_INVALID_ARG;
    }
    if (ctx->requests[request_id].kind != SG_REQUEST_KIND_PICKUP_DELIVERY) {
        return AR_STATUS_INVALID_ARG;
    }

    if (!sg_request_emit_stops(ctx, request_id, emitted, &emitted_count)) {
        return AR_STATUS_ERROR;
    }
    if (emitted_count != 2) {
        return AR_STATUS_ERROR;
    }

    route = sg_route_vehicle_ptr(sol, vehicle_id);
    old_len = sol->route_lengths[vehicle_id];
    old_distance = sol->route_distance[vehicle_id];

    /* Determine request-level insertion position:
       Find which request position corresponds to pickup_stop_pos.
       The new request goes at the position such that its pickup is at pickup_stop_pos. */
    if (pickup_stop_pos == 0) {
        insert_req_pos = 0;
    } else {
        /* Find the request whose last stop is immediately before pickup_stop_pos */
        uint32_t stop_len = sol->route_stop_lengths[vehicle_id];
        const SGRouteStop *stops = sg_route_vehicle_stop_ptr_const(sol, vehicle_id);
        insert_req_pos = 0;
        if (pickup_stop_pos <= stop_len) {
            /* Walk through requests to find insertion point */
            uint32_t prev_stop = pickup_stop_pos - 1;
            if (prev_stop < stop_len) {
                uint32_t prev_req = stops[prev_stop].request_id;
                if (prev_req < sol->base.total_requests) {
                    insert_req_pos = sol->request_pos[prev_req] + 1;
                }
            }
        } else {
            insert_req_pos = old_len;
        }
    }

    if (insert_req_pos > old_len || old_len >= sol->route_stride) {
        return AR_STATUS_INVALID_ARG;
    }

    /* Insert request into route_requests */
    if (insert_req_pos < old_len) {
        memmove(&route[insert_req_pos + 1], &route[insert_req_pos],
                (size_t)(old_len - insert_req_pos) * sizeof(uint32_t));
    }
    route[insert_req_pos] = request_id;
    sol->route_lengths[vehicle_id] = old_len + 1;

    sol->request_vehicle[request_id] = vehicle_id;
    sol->request_pos[request_id] = insert_req_pos;
    for (r = insert_req_pos + 1; r < sol->route_lengths[vehicle_id]; r++) {
        sol->request_pos[route[r]] = r;
    }

    if (old_len == 0) {
        sol->vehicles_used++;
    }

    status = sg_bootstrap_assign_request(&sol->base, request_id);
    if (status != AR_STATUS_OK) {
        return status;
    }

    /* Splice pickup stop */
    if (!sg_route_splice_stop(ctx, sol, vehicle_id, pickup_stop_pos, &emitted[0])) {
        return AR_STATUS_ERROR;
    }

    /* delivery_stop_pos is already in terms of the combined array (with pickup present),
       so no adjustment needed — pickup splice already shifted indices. */
    adj_delivery_pos = delivery_stop_pos;

    /* Splice delivery stop */
    if (!sg_route_splice_stop(ctx, sol, vehicle_id, adj_delivery_pos, &emitted[1])) {
        return AR_STATUS_ERROR;
    }

    sg_route_update_timing(ctx, sol, vehicle_id);
    sg_route_update_load(ctx, sol, vehicle_id);
    sol->total_distance += (sol->route_distance[vehicle_id] - old_distance);

    return AR_STATUS_OK;
}

ARStatus sg_route_unassign_request(const SGContext *ctx, SGRouteSolution *sol,
                                   uint32_t request_id, double *capacity_scratch) {
    uint32_t vehicle_id;
    uint32_t pos;
    uint32_t *route;
    uint32_t old_len;
    double old_distance;
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

    /* Remove stops directly instead of rebuilding (preserves non-adjacent PD) */
    {
        const SGRequestRecord *req_rec = &ctx->requests[request_id];
        uint32_t p_pos = sol->request_pickup_stop_pos[request_id];
        uint32_t d_pos = sol->request_delivery_stop_pos[request_id];

        if (req_rec->kind == SG_REQUEST_KIND_PICKUP_DELIVERY) {
            /* Remove higher index first to avoid shifting the other */
            if (d_pos != UINT32_MAX && p_pos != UINT32_MAX) {
                if (d_pos > p_pos) {
                    sg_route_excise_stop(ctx, sol, vehicle_id, d_pos);
                    sg_route_excise_stop(ctx, sol, vehicle_id, p_pos);
                } else {
                    sg_route_excise_stop(ctx, sol, vehicle_id, p_pos);
                    sg_route_excise_stop(ctx, sol, vehicle_id, d_pos);
                }
            } else if (p_pos != UINT32_MAX) {
                sg_route_excise_stop(ctx, sol, vehicle_id, p_pos);
            } else if (d_pos != UINT32_MAX) {
                sg_route_excise_stop(ctx, sol, vehicle_id, d_pos);
            }
        } else {
            /* Delivery-only: just remove the delivery stop */
            if (d_pos != UINT32_MAX) {
                sg_route_excise_stop(ctx, sol, vehicle_id, d_pos);
            }
        }
    }

    sg_route_update_timing(ctx, sol, vehicle_id);
    sg_route_update_load(ctx, sol, vehicle_id);
    sol->total_distance += (sol->route_distance[vehicle_id] - old_distance);
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
