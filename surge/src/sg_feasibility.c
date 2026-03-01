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
    double work_since_break = 0.0;
    double total_work = 0.0;
    double total_break_time = 0.0;
    uint32_t break_count = 0;
    SGRouteBreak *breaks = NULL;
    uint32_t break_stride = 0;
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
        if (sol->route_depot_depart) {
            sol->route_depot_depart[vehicle_id] = 0.0;
        }
        if (sol->route_depot_return) {
            sol->route_depot_return[vehicle_id] = 0.0;
        }
        if (sol->route_break_time) sol->route_break_time[vehicle_id] = 0.0;
        if (sol->route_break_count) sol->route_break_count[vehicle_id] = 0;
        if (sol->route_total_work) sol->route_total_work[vehicle_id] = 0.0;
        if (sol->route_violations) {
            memset(sol->route_violations + (size_t)vehicle_id * SG_PENALTY_COUNT,
                   0, SG_PENALTY_COUNT * sizeof(double));
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

    if (sol->route_breaks && sol->break_stride > 0) {
        breaks = sol->route_breaks + (size_t)vehicle_id * sol->break_stride;
        break_stride = sol->break_stride;
    }

    /* Forward pass */
    time_cursor = vehicle->has_shift_time_window ? (double)vehicle->shift_early : 0.0;
    if (!vehicle->open_start && start_depot->has_time_window) {
        if (time_cursor < (double)start_depot->tw_early) {
            time_cursor = (double)start_depot->tw_early;
        }
    }
    depot_depart = time_cursor;
    if (sol->route_depot_depart) {
        sol->route_depot_depart[vehicle_id] = depot_depart;
    }

    prev_loc = vehicle->start_location_id;
    {
    uint32_t prev_request_id = UINT32_MAX;
    for (i = 0; i < stop_len; i++) {
        SGRouteStop *stop = &stops[i];
        uint32_t cur_loc = ctx->tasks[stop->task_id].location_id;
        const SGTaskRecord *task = &ctx->tasks[stop->task_id];
        double dist, dur;
        double start;
        double setup;

        /* Multi-trip: return to depot, reload, depart for new trip */
        if (stop->trip_start && i > 0) {
            double ret_dist, ret_dur;
            sg_travel(ctx, prev_loc, vehicle->end_location_id, vehicle_id, time_cursor, &ret_dist, &ret_dur);
            distance += ret_dist;

            /* Track return driving as work and inject breaks */
            work_since_break += ret_dur;
            total_work += ret_dur;
            if (vehicle->has_break_policy) {
                double mw = (double)vehicle->break_max_work_seconds;
                double bd = (double)vehicle->break_duration_seconds;
                while (work_since_break > mw + 1e-9) {
                    work_since_break -= mw;
                    if (breaks && break_count < break_stride) {
                        breaks[break_count].after_stop_index = (i > 0) ? i - 1U : UINT32_MAX;
                        breaks[break_count].start_time = time_cursor + ret_dur - work_since_break;
                        breaks[break_count].duration = bd;
                    }
                    time_cursor += bd;
                    total_break_time += bd;
                    break_count++;
                }
            }
            time_cursor += ret_dur;

            stop->trip_depot_return = time_cursor;

            /* Reload at depot — NOT work, resets break counter */
            time_cursor += (double)vehicle->trip_reload_seconds;
            work_since_break = 0.0;

            /* Snap to depot TW if needed */
            if (start_depot->has_time_window && time_cursor < (double)start_depot->tw_early) {
                time_cursor = (double)start_depot->tw_early;
            }

            stop->trip_depot_depart = time_cursor;

            /* Depart from start depot for new trip */
            prev_loc = vehicle->start_location_id;
            prev_request_id = UINT32_MAX;  /* Reset setup class chain */
        }

        /* Open start: skip first leg from start depot to first stop */
        if (vehicle->open_start && i == 0 && !(stop->trip_start && i > 0)) {
            dist = 0.0;
            dur = 0.0;
        } else {
            sg_travel(ctx, prev_loc, cur_loc, vehicle_id, time_cursor, &dist, &dur);
        }
        distance += dist;

        /* Track driving as work and inject breaks during travel */
        work_since_break += dur;
        total_work += dur;
        if (vehicle->has_break_policy) {
            double mw = (double)vehicle->break_max_work_seconds;
            double bd = (double)vehicle->break_duration_seconds;
            while (work_since_break > mw + 1e-9) {
                work_since_break -= mw;
                if (breaks && break_count < break_stride) {
                    breaks[break_count].after_stop_index = (i > 0) ? i - 1U : UINT32_MAX;
                    breaks[break_count].start_time = time_cursor + dur - work_since_break;
                    breaks[break_count].duration = bd;
                }
                time_cursor += bd;
                total_break_time += bd;
                break_count++;
            }
        }

        time_cursor += dur;
        stop->arrival = time_cursor;

        setup = sg_setup_time_between(ctx, prev_request_id, stop->request_id);
        work_since_break += setup;
        total_work += setup;

        start = sg_task_snap_forward(task, time_cursor + setup);
        stop->service_start = start;
        stop->depart = start + (double)task->service_seconds;
        waiting += start - stop->arrival;

        work_since_break += (double)task->service_seconds;
        total_work += (double)task->service_seconds;
        stop->work_since_break = work_since_break;

        if (task->has_soft_time_window) {
            if (start < (double)task->soft_tw_early) {
                tw_penalty += task->tw_early_penalty * ((double)task->soft_tw_early - start);
            } else if (start > (double)task->soft_tw_late) {
                tw_penalty += task->tw_late_penalty * (start - (double)task->soft_tw_late);
            }
        }
        time_cursor = stop->depart;
        prev_request_id = stop->request_id;
        prev_loc = cur_loc;
    }
    } /* end prev_request_id scope */

    /* Count trips */
    if (sol->route_trip_count) {
        uint32_t trip_count = (stop_len > 0) ? 1 : 0;
        for (i = 1; i < stop_len; i++) {
            if (stops[i].trip_start) trip_count++;
        }
        sol->route_trip_count[vehicle_id] = trip_count;
    }

    /* Add travel to end depot (skip for open-end routes) */
    if (!vehicle->open_end) {
        double dist, dur;
        sg_travel(ctx, prev_loc, vehicle->end_location_id, vehicle_id, time_cursor, &dist, &dur);
        distance += dist;

        /* Track return driving as work and inject breaks */
        work_since_break += dur;
        total_work += dur;
        if (vehicle->has_break_policy) {
            double mw = (double)vehicle->break_max_work_seconds;
            double bd = (double)vehicle->break_duration_seconds;
            while (work_since_break > mw + 1e-9) {
                work_since_break -= mw;
                if (breaks && break_count < break_stride) {
                    breaks[break_count].after_stop_index = stop_len > 0 ? stop_len - 1U : UINT32_MAX;
                    breaks[break_count].start_time = time_cursor + dur - work_since_break;
                    breaks[break_count].duration = bd;
                }
                time_cursor += bd;
                total_break_time += bd;
                break_count++;
            }
        }

        time_cursor += dur;
    }
    if (sol->route_depot_return) {
        sol->route_depot_return[vehicle_id] = vehicle->open_end ? 0.0 : time_cursor;
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

    /* Store break metrics */
    if (sol->route_break_time) sol->route_break_time[vehicle_id] = total_break_time;
    if (sol->route_break_count) sol->route_break_count[vehicle_id] = break_count;
    if (sol->route_total_work) sol->route_total_work[vehicle_id] = total_work;

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
        double setup_to_next;
        double break_to_next = 0.0;

        if (idx + 1U < stop_len) {
            if (stops[idx + 1U].trip_start) {
                /* Compound path: stop → end_depot + reload + start_depot → next_stop */
                uint32_t next_loc = ctx->tasks[stops[idx + 1U].task_id].location_id;
                travel_to_next = sg_travel_dur(ctx, cur_loc, vehicle->end_location_id, vehicle_id, stops[idx].depart)
                               + (double)vehicle->trip_reload_seconds
                               + sg_travel_dur(ctx, vehicle->start_location_id, next_loc, vehicle_id, stops[idx].depart);
                break_to_next = 0.0;  /* Depot visit resets break tracking */
            } else {
                uint32_t next_loc = ctx->tasks[stops[idx + 1U].task_id].location_id;
                travel_to_next = sg_travel_dur(ctx, cur_loc, next_loc, vehicle_id, stops[idx].depart);
                if (vehicle->has_break_policy) {
                    break_to_next = stops[idx + 1U].arrival - stop->depart - travel_to_next;
                    if (break_to_next < 0.0) break_to_next = 0.0;
                }
            }
        } else if (vehicle->open_end) {
            travel_to_next = 0.0;
        } else {
            travel_to_next = sg_travel_dur(ctx, cur_loc, vehicle->end_location_id, vehicle_id, stops[idx].depart);
            if (vehicle->has_break_policy && sol->route_depot_return) {
                break_to_next = sol->route_depot_return[vehicle_id] - stop->depart - travel_to_next;
                if (break_to_next < 0.0) break_to_next = 0.0;
            }
        }

        setup_to_next = (idx + 1U < stop_len && !stops[idx + 1U].trip_start)
            ? sg_setup_time_between(ctx, stop->request_id, stops[idx + 1U].request_id)
            : 0.0;
        stop->latest_start = latest_next - travel_to_next - setup_to_next - break_to_next - (double)task->service_seconds;
        if (task->has_time_window) {
            stop->latest_start = sg_task_snap_backward(task, stop->latest_start);
        }
        if (!stop->is_pickup) {
            const SGRequestRecord *req = &ctx->requests[stop->request_id];
            if (req->kind == SG_REQUEST_KIND_PICKUP_DELIVERY && req->has_max_ride_time) {
                uint32_t pp = sol->request_pickup_stop_pos[stop->request_id];
                if (pp < stop_len) {
                    double rb = stops[pp].depart + (double)req->max_ride_time_seconds;
                    if (rb < stop->latest_start) {
                        stop->latest_start = rb;
                    }
                }
            }
        }
        stop->forward_slack = stop->latest_start - stop->service_start;
        latest_next = stop->latest_start;
    }

    sol->route_distance[vehicle_id] = distance;

    /* Compute per-route constraint violations for infeasible-space exploration */
    if (sol->route_violations) {
        double *rv = sol->route_violations + (size_t)vehicle_id * SG_PENALTY_COUNT;
        int vk;
        for (vk = 0; vk < SG_PENALTY_COUNT; vk++) rv[vk] = 0.0;

        /* Time warp: per-stop TW violation */
        for (i = 0; i < stop_len; i++) {
            const SGTaskRecord *task = &ctx->tasks[stops[i].task_id];
            if (task->has_time_window && stops[i].service_start > (double)task->tw_late + 1e-9) {
                rv[SG_PENALTY_TIME_WARP] += stops[i].service_start - (double)task->tw_late;
            }
        }
        /* End depot TW violation */
        if (!vehicle->open_end && end_depot->has_time_window && stop_len > 0) {
            double ret = sol->route_depot_return ? sol->route_depot_return[vehicle_id] : time_cursor;
            if (ret > (double)end_depot->tw_late + 1e-9) {
                rv[SG_PENALTY_TIME_WARP] += ret - (double)end_depot->tw_late;
            }
        }
        /* Shift TW violation (hard shift only — soft shifts use overtime cost) */
        if (vehicle->has_shift_time_window && !(vehicle->cost_per_overtime > 0.0) && stop_len > 0) {
            if (time_cursor > (double)vehicle->shift_late + 1e-9) {
                rv[SG_PENALTY_TIME_WARP] += time_cursor - (double)vehicle->shift_late;
            }
        }
        /* Duration violation */
        if (vehicle->max_duration_seconds > 0 && sol->route_duration) {
            double dur = sol->route_duration[vehicle_id];
            if (dur > (double)vehicle->max_duration_seconds + 1e-9) {
                rv[SG_PENALTY_DURATION] += dur - (double)vehicle->max_duration_seconds;
            }
        }
        /* Ride time violation (PD pairs) */
        for (i = 0; i < stop_len; i++) {
            if (!stops[i].is_pickup) {
                const SGRequestRecord *req = &ctx->requests[stops[i].request_id];
                if (req->kind == SG_REQUEST_KIND_PICKUP_DELIVERY) {
                    uint32_t pp = sol->request_pickup_stop_pos[stops[i].request_id];
                    if (pp < stop_len) {
                        double ride = stops[i].service_start - stops[pp].depart;
                        double limit = INFINITY;
                        if (req->has_max_ride_time) {
                            limit = (double)req->max_ride_time_seconds;
                        } else {
                            const SGTaskRecord *pt = &ctx->tasks[req->pickup_task_id];
                            const SGTaskRecord *dt = &ctx->tasks[req->delivery_task_id];
                            if (pt->has_time_window && dt->has_time_window) {
                                limit = (double)(dt->tw_late - pt->tw_early);
                            }
                        }
                        if (isfinite(limit) && ride > limit + 1e-9) {
                            rv[SG_PENALTY_RIDE_TIME] += ride - limit;
                        }
                    }
                }
            }
        }
        /* Total work violation */
        if (vehicle->max_total_work_seconds > 0 && sol->route_total_work) {
            double tw = sol->route_total_work[vehicle_id];
            if (tw > (double)vehicle->max_total_work_seconds + 1e-9) {
                rv[SG_PENALTY_TOTAL_WORK] += tw - (double)vehicle->max_total_work_seconds;
            }
        }
        /* Distance violation */
        if (vehicle->max_distance > 0.0 && distance > vehicle->max_distance + 1e-9) {
            rv[SG_PENALTY_DISTANCE] += distance - vehicle->max_distance;
        }

        /* Recompute solution-level violation sums */
        {
            int sk;
            for (sk = 0; sk < SG_PENALTY_COUNT; sk++) sol->violations[sk] = 0.0;
            for (i = 0; i < sol->num_vehicles; i++) {
                const double *vrv = sol->route_violations + (size_t)i * SG_PENALTY_COUNT;
                for (sk = 0; sk < SG_PENALTY_COUNT; sk++) sol->violations[sk] += vrv[sk];
            }
        }
    }

    /* Rebuild concatenation-based capacity segment summaries (O(L)).
       These enable O(1) capacity checks in sg_route_eval_insertion_cached. */
    sg_route_build_cap_segments(ctx, sol, vehicle_id);

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

    /* Initialize first slot to zero (prefix-sum convention;
       initial_load offset applied during feasibility checks) */
    for (d = 0; d < dim_count; d++) {
        load[d] = 0.0;
    }

    stops = sg_route_vehicle_stop_ptr(sol, vehicle_id);
    for (i = 0; i < stop_len; i++) {
        const SGTaskRecord *task = &ctx->tasks[stops[i].task_id];
        for (d = 0; d < (uint32_t)dim_count; d++) {
            double prev_load = stops[i].trip_start
                ? 0.0  /* Capacity reset at depot reload */
                : load[(size_t)i * dim_count + d];
            double demand = (task->has_demand && task->demand) ? task->demand[d] : 0.0;
            load[((size_t)i + 1U) * dim_count + d] = prev_load + demand;
        }
    }

    /* Compute capacity violation for infeasible-space exploration */
    if (sol->route_violations) {
        const SGVehicleRecord *vehicle = &ctx->vehicles[vehicle_id];
        double cap_excess = 0.0;

        /* Per-trip: compute min/max prefix sums, check against capacity */
        {
            uint32_t trip_start = 0;
            uint32_t s;
            for (s = 0; s <= stop_len; s++) {
                int trip_end = (s == stop_len) || (s > 0 && stops[s].trip_start);
                if (trip_end && s > trip_start) {
                    /* Evaluate trip [trip_start, s) */
                    uint32_t scan_start = (trip_start > 0) ? trip_start + 1 : 1;
                    for (d = 0; d < (uint32_t)dim_count; d++) {
                        double cap = (vehicle->has_capacity && vehicle->capacity)
                                     ? vehicle->capacity[d] : INFINITY;
                        double pmin = 0.0, pmax = 0.0;
                        uint32_t ps;
                        if (!isfinite(cap)) continue;
                        for (ps = scan_start; ps <= s; ps++) {
                            double val = load[(size_t)ps * dim_count + d];
                            if (val < pmin) pmin = val;
                            if (val > pmax) pmax = val;
                        }
                        /* For first trip with initial_load, check fixed-start bounds */
                        if (trip_start == 0 &&
                            vehicle->has_initial_load && vehicle->initial_load) {
                            double il = vehicle->initial_load[d];
                            double lo = il + pmin;
                            double hi = il + pmax;
                            if (lo < -SG_DEMAND_TOLERANCE) {
                                cap_excess += -lo;
                            }
                            if (hi > cap + SG_DEMAND_TOLERANCE) {
                                cap_excess += hi - cap;
                            }
                        } else if ((pmax - pmin) > cap + SG_DEMAND_TOLERANCE) {
                            cap_excess += (pmax - pmin) - cap;
                        }
                    }
                }
                if (s < stop_len && stops[s].trip_start && s > 0) {
                    trip_start = s;
                }
            }
        }

        sol->route_violations[(size_t)vehicle_id * SG_PENALTY_COUNT + SG_PENALTY_CAPACITY] = cap_excess;

        /* Recompute solution-level capacity violation sum */
        {
            double total_cap = 0.0;
            uint32_t v;
            for (v = 0; v < sol->num_vehicles; v++) {
                total_cap += sol->route_violations[(size_t)v * SG_PENALTY_COUNT + SG_PENALTY_CAPACITY];
            }
            sol->violations[SG_PENALTY_CAPACITY] = total_cap;
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
    double *seq_arrival = NULL;
    uint32_t *pd_stack = NULL;
    uint8_t *prec_completed = NULL;
    double seq_depot_return = 0.0;
    double distance = 0.0;
    double time_cursor;
    uint32_t prev_loc;
    double latest_next;
    uint32_t i;
    uint32_t d;
    int feasible = 0;
    int use_scratch = 0;

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

    if (ctx->scratch.timing && stop_count <= ctx->scratch.stop_capacity) {
        uint32_t scap = ctx->scratch.stop_capacity;
        use_scratch = 1;
        service_start = ctx->scratch.timing;
        depart = service_start + scap;
        latest_start = depart + scap;
        forward_slack = latest_start + scap;
        seq_arrival = forward_slack + scap;
        if (ctx->dimension_count > 0) {
            load_profile = ctx->scratch.load_profile;
            min_prefix = ctx->scratch.dim_scratch;
            max_prefix = min_prefix + ctx->dimension_count;
        }
        if (ctx->num_requests > 0) {
            pickup_depart = ctx->scratch.pickup_depart;
            pickup_seen = ctx->scratch.pickup_seen;
            memset(pickup_seen, 0, (size_t)ctx->num_requests);
        }
    }

    if (!use_scratch) {
        service_start = (double *)malloc((size_t)stop_count * sizeof(double));
        depart = (double *)malloc((size_t)stop_count * sizeof(double));
        latest_start = (double *)malloc((size_t)stop_count * sizeof(double));
        forward_slack = (double *)malloc((size_t)stop_count * sizeof(double));
        seq_arrival = (double *)malloc((size_t)stop_count * sizeof(double));
        if (!service_start || !depart || !latest_start || !forward_slack || !seq_arrival) {
            goto done;
        }
    }

    if (ctx->dimension_count > 0) {
        if (!use_scratch) {
            size_t load_count;
            if ((size_t)stop_count + 1U > SIZE_MAX / (size_t)ctx->dimension_count) {
                goto done;
            }
            load_count = ((size_t)stop_count + 1U) * (size_t)ctx->dimension_count;
            load_profile = (double *)malloc(load_count * sizeof(double));
            min_prefix = (double *)malloc((size_t)ctx->dimension_count * sizeof(double));
            max_prefix = (double *)malloc((size_t)ctx->dimension_count * sizeof(double));
            if (!load_profile || !min_prefix || !max_prefix) {
                goto done;
            }
        }

        /* Check capacity per trip segment (capacity resets at trip boundaries) */
        {
            uint32_t trip_start_idx = 0;
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

                /* At trip boundary, check previous trip and reset */
                if (stop->trip_start && i > 0) {
                    uint8_t fixed_start = (trip_start_idx == 0 &&
                                           vehicle->has_initial_load && vehicle->initial_load);
                    for (d = 0; d < ctx->dimension_count; d++) {
                        double cap = (vehicle->has_capacity && vehicle->capacity)
                                     ? vehicle->capacity[d] : INFINITY;
                        if (fixed_start) {
                            double il = vehicle->initial_load[d];
                            if (il + min_prefix[d] < -SG_DEMAND_TOLERANCE ||
                                il + max_prefix[d] > cap + SG_DEMAND_TOLERANCE) {
                                goto done;
                            }
                        } else {
                            if ((max_prefix[d] - min_prefix[d]) > cap + SG_DEMAND_TOLERANCE) {
                                goto done;
                            }
                        }
                    }
                    /* Reset for new trip */
                    trip_start_idx = i;
                    for (d = 0; d < ctx->dimension_count; d++) {
                        load_profile[(size_t)i * (size_t)ctx->dimension_count + d] = 0.0;
                        min_prefix[d] = 0.0;
                        max_prefix[d] = 0.0;
                    }
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

            /* Check final trip segment */
            {
                uint8_t fixed_start = (trip_start_idx == 0 &&
                                       vehicle->has_initial_load && vehicle->initial_load);
                for (d = 0; d < ctx->dimension_count; d++) {
                    double cap = (vehicle->has_capacity && vehicle->capacity)
                                 ? vehicle->capacity[d]
                                 : INFINITY;
                    if (fixed_start) {
                        double il = vehicle->initial_load[d];
                        if (il + min_prefix[d] < -SG_DEMAND_TOLERANCE ||
                            il + max_prefix[d] > cap + SG_DEMAND_TOLERANCE) {
                            goto done;
                        }
                    } else {
                        if ((max_prefix[d] - min_prefix[d]) > cap + SG_DEMAND_TOLERANCE) {
                            goto done;
                        }
                    }
                }
            }
        }
    }

    /* Per-compartment capacity check (layered on top of vehicle overall capacity) */
    if (ctx->has_compartments && vehicle->num_compartments > 0 && ctx->dimension_count > 0) {
        size_t dim_count = (size_t)ctx->dimension_count;
        size_t comp_stride = (size_t)SG_MAX_COMPARTMENTS_PER_VEHICLE * dim_count;
        double *comp_load, *comp_min, *comp_max;
        uint8_t comp_stack = (comp_stride * 3 * sizeof(double)) <= 1152;

        /* Compartment compatibility: vehicle must have the request's compartment type */
        for (i = 0; i < stop_count; i++) {
            uint32_t ct = ctx->requests[stops[i].request_id].compartment_type;
            if (ct > 0 && sg_vehicle_find_compartment(vehicle, ct) < 0)
                goto done;
        }

        if (comp_stack || (use_scratch && ctx->scratch.compartment_load)) {
            if (use_scratch && ctx->scratch.compartment_load) {
                comp_load = ctx->scratch.compartment_load;
                comp_min = ctx->scratch.compartment_min_prefix;
                comp_max = ctx->scratch.compartment_max_prefix;
            } else {
                comp_load = (double *)malloc(comp_stride * 3 * sizeof(double));
                if (!comp_load) goto done;
                comp_min = comp_load + comp_stride;
                comp_max = comp_min + comp_stride;
            }
        } else {
            comp_load = (double *)malloc(comp_stride * 3 * sizeof(double));
            if (!comp_load) goto done;
            comp_min = comp_load + comp_stride;
            comp_max = comp_min + comp_stride;
        }

        memset(comp_load, 0, comp_stride * sizeof(double));
        memset(comp_min, 0, comp_stride * sizeof(double));
        memset(comp_max, 0, comp_stride * sizeof(double));

        for (i = 0; i < stop_count; i++) {
            const SGRouteStop *stop = &stops[i];
            const SGTaskRecord *task = &ctx->tasks[stop->task_id];
            uint32_t ct = ctx->requests[stop->request_id].compartment_type;

            /* At trip boundary, check previous trip and reset */
            if (stop->trip_start && i > 0) {
                uint8_t ci;
                for (ci = 0; ci < vehicle->num_compartments; ci++) {
                    if (!vehicle->compartments[ci].capacity) continue;
                    for (d = 0; d < ctx->dimension_count; d++) {
                        size_t idx = (size_t)ci * dim_count + d;
                        double cap = vehicle->compartments[ci].capacity[d];
                        if ((comp_max[idx] - comp_min[idx]) > cap + SG_DEMAND_TOLERANCE) {
                            if (!(use_scratch && ctx->scratch.compartment_load) && comp_load) {
                                free(comp_load);
                            }
                            goto done;
                        }
                    }
                }
                memset(comp_load, 0, comp_stride * sizeof(double));
                memset(comp_min, 0, comp_stride * sizeof(double));
                memset(comp_max, 0, comp_stride * sizeof(double));
            }

            if (ct > 0) {
                int ci = sg_vehicle_find_compartment(vehicle, ct);
                for (d = 0; d < ctx->dimension_count; d++) {
                    size_t idx = (size_t)ci * dim_count + d;
                    double val = comp_load[idx] + task->demand[d];
                    comp_load[idx] = val;
                    if (val < comp_min[idx]) comp_min[idx] = val;
                    if (val > comp_max[idx]) comp_max[idx] = val;
                }
            }
        }

        /* Check final trip segment */
        {
            uint8_t ci;
            for (ci = 0; ci < vehicle->num_compartments; ci++) {
                if (!vehicle->compartments[ci].capacity) continue;
                for (d = 0; d < ctx->dimension_count; d++) {
                    size_t idx = (size_t)ci * dim_count + d;
                    double cap = vehicle->compartments[ci].capacity[d];
                    if ((comp_max[idx] - comp_min[idx]) > cap + SG_DEMAND_TOLERANCE) {
                        if (!(use_scratch && ctx->scratch.compartment_load) && comp_load) {
                            free(comp_load);
                        }
                        goto done;
                    }
                }
            }
        }

        if (!(use_scratch && ctx->scratch.compartment_load)) {
            free(comp_load);
        }
    }

    if (ctx->num_requests > 0 && !use_scratch) {
        pickup_depart = (double *)malloc((size_t)ctx->num_requests * sizeof(double));
        pickup_seen = (uint8_t *)calloc((size_t)ctx->num_requests, sizeof(uint8_t));
        if (!pickup_depart || !pickup_seen) {
            goto done;
        }
        for (i = 0; i < ctx->num_requests; i++) {
            pickup_depart[i] = 0.0;
        }
    }

    /* LIFO/FIFO PD policy: stack/queue for ordering check */
    uint32_t pd_stack_top = 0;     /* LIFO: stack pointer; FIFO: enqueue pointer */
    uint32_t pd_queue_front = 0;   /* FIFO: dequeue pointer */
    uint8_t seen_pd_pickup = 0;    /* Backhaul: set when first PD pickup encountered */

    if (ctx->has_pd_policy && ctx->vehicles[vehicle_id].pd_policy != SG_PD_POLICY_NONE &&
        stop_count > 0) {
        pd_stack = (uint32_t *)malloc((size_t)stop_count * sizeof(uint32_t));
        if (!pd_stack) {
            goto done;
        }
    }

    if (ctx->has_precedence && stop_count > 0) {
        /* Allocate 2 * num_requests: first half = completed, second half = on_route */
        prec_completed = (uint8_t *)calloc((size_t)ctx->num_requests * 2, sizeof(uint8_t));
        if (!prec_completed) {
            goto done;
        }
        /* Build on_route set (second half of the buffer) */
        {
            uint8_t *on_route = prec_completed + ctx->num_requests;
            uint32_t si;
            for (si = 0; si < stop_count; si++) {
                if (stops[si].request_id < ctx->num_requests)
                    on_route[stops[si].request_id] = 1;
            }
        }
    }

    {
    double seq_work_since_break = 0.0;
    double seq_total_work = 0.0;
    double seq_total_break_time = 0.0;
    (void)seq_total_break_time;

    time_cursor = vehicle->has_shift_time_window ? (double)vehicle->shift_early : 0.0;
    if (!vehicle->open_start && start_depot->has_time_window) {
        if (time_cursor < (double)start_depot->tw_early) {
            time_cursor = (double)start_depot->tw_early;
        }
        if (time_cursor > (double)start_depot->tw_late + 1e-9) {
            goto done;
        }
    }

    prev_loc = vehicle->start_location_id;
    {
    uint32_t prev_request_id_seq = UINT32_MAX;
    for (i = 0; i < stop_count; i++) {
        const SGRouteStop *stop = &stops[i];
        const SGTaskRecord *task;
        const SGRequestRecord *request;
        double dist, dur;
        double start;
        double ride_time = 0.0;
        double ride_limit = INFINITY;
        uint32_t cur_loc;
        double setup;

        if (stop->request_id >= ctx->num_requests || stop->task_id >= ctx->num_tasks) {
            goto done;
        }
        task = &ctx->tasks[stop->task_id];
        request = &ctx->requests[stop->request_id];
        if (!task->has_location || !task->has_time_window) {
            goto done;
        }

        cur_loc = task->location_id;

        /* Trip boundary: return to depot, reload, depart */
        if (stop->trip_start && i > 0) {
            double ret_dist, ret_dur;
            sg_travel(ctx, prev_loc, vehicle->end_location_id, vehicle_id, time_cursor, &ret_dist, &ret_dur);
            if (!isfinite(ret_dist) || ret_dist < 0.0) {
                goto done;
            }
            distance += ret_dist;
            seq_work_since_break += ret_dur;
            seq_total_work += ret_dur;
            if (vehicle->has_break_policy) {
                double mw = (double)vehicle->break_max_work_seconds;
                double bd = (double)vehicle->break_duration_seconds;
                while (seq_work_since_break > mw + 1e-9) {
                    seq_work_since_break -= mw;
                    time_cursor += bd;
                    seq_total_break_time += bd;
                }
            }
            time_cursor += ret_dur;
            /* Reload at depot (not work, resets break counter) */
            time_cursor += (double)vehicle->trip_reload_seconds;
            seq_work_since_break = 0.0;
            /* Snap to depot TW */
            if (start_depot->has_time_window && time_cursor < (double)start_depot->tw_early) {
                time_cursor = (double)start_depot->tw_early;
            }
            prev_loc = vehicle->start_location_id;
            prev_request_id_seq = UINT32_MAX;
        }

        /* Open start: skip first leg from start depot to first stop */
        if (vehicle->open_start && i == 0 && !(stop->trip_start && i > 0)) {
            dist = 0.0;
            dur = 0.0;
        } else {
            sg_travel(ctx, prev_loc, cur_loc, vehicle_id, time_cursor, &dist, &dur);
            if (!isfinite(dist) || dist < 0.0) {
                goto done;
            }
        }
        distance += dist;

        /* Track driving as work and inject breaks */
        seq_work_since_break += dur;
        seq_total_work += dur;
        if (vehicle->has_break_policy) {
            double mw = (double)vehicle->break_max_work_seconds;
            double bd = (double)vehicle->break_duration_seconds;
            while (seq_work_since_break > mw + 1e-9) {
                seq_work_since_break -= mw;
                time_cursor += bd;
                seq_total_break_time += bd;
            }
        }

        time_cursor += dur;
        seq_arrival[i] = time_cursor;
        setup = sg_setup_time_between(ctx, prev_request_id_seq, stop->request_id);
        seq_work_since_break += setup;
        seq_total_work += setup;
        start = sg_task_snap_forward(task, time_cursor + setup);
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

        /* LIFO/FIFO PD policy check */
        if (pd_stack && request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY) {
            if (stop->is_pickup) {
                pd_stack[pd_stack_top++] = stop->request_id;
            } else {
                if (vehicle->pd_policy == SG_PD_POLICY_LIFO) {
                    if (pd_stack_top == 0 || pd_stack[pd_stack_top - 1] != stop->request_id) {
                        goto done;
                    }
                    pd_stack_top--;
                } else { /* FIFO */
                    if (pd_queue_front >= pd_stack_top || pd_stack[pd_queue_front] != stop->request_id) {
                        goto done;
                    }
                    pd_queue_front++;
                }
            }
        }

        /* Backhaul check: no delivery-only stop after a PD pickup */
        if (ctx->has_backhaul && vehicle->backhaul) {
            if (stop->is_pickup && request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY) {
                seen_pd_pickup = 1;
            } else if (!stop->is_pickup && request->kind == SG_REQUEST_KIND_DELIVERY_ONLY) {
                if (seen_pd_pickup) {
                    goto done;
                }
            }
        }

        /* Precedence check: if this is the first stop of a request with predecessors,
           all predecessors on this vehicle must be in the completed set */
        if (prec_completed) {
            uint8_t *on_route = prec_completed + ctx->num_requests;
            int is_first = stop->is_pickup ||
                           (request->kind == SG_REQUEST_KIND_DELIVERY_ONLY);
            if (is_first) {
                uint16_t pb;
                for (pb = 0; pb < request->num_prec_before; pb++) {
                    uint32_t pred = request->precedence_before[pb];
                    /* Only enforce if predecessor is on this route */
                    if (on_route[pred] && !prec_completed[pred]) goto done;
                }
            }
            /* Mark completion: last stop of request (delivery for PD, delivery for D-only) */
            if (!stop->is_pickup) {
                prec_completed[stop->request_id] = 1;
            }
        }

        service_start[i] = start;
        depart[i] = start + (double)task->service_seconds;
        if (!isfinite(depart[i])) {
            goto done;
        }

        seq_work_since_break += (double)task->service_seconds;
        seq_total_work += (double)task->service_seconds;

        if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY && stop->is_pickup) {
            pickup_depart[stop->request_id] = depart[i];
        }
        time_cursor = depart[i];
        prev_request_id_seq = stop->request_id;
        prev_loc = cur_loc;
    }
    } /* end prev_request_id_seq scope */

    if (!vehicle->open_end) {
        double dist, dur;
        sg_travel(ctx, prev_loc, vehicle->end_location_id, vehicle_id, time_cursor, &dist, &dur);
        if (!isfinite(dist) || dist < 0.0) {
            goto done;
        }
        distance += dist;

        /* Track return driving as work and inject breaks */
        seq_work_since_break += dur;
        seq_total_work += dur;
        if (vehicle->has_break_policy) {
            double mw = (double)vehicle->break_max_work_seconds;
            double bd = (double)vehicle->break_duration_seconds;
            while (seq_work_since_break > mw + 1e-9) {
                seq_work_since_break -= mw;
                time_cursor += bd;
                seq_total_break_time += bd;
            }
        }

        time_cursor += dur;
        seq_depot_return = time_cursor;

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
        if (!vehicle->open_start && start_depot->has_time_window && depot_depart_t < (double)start_depot->tw_early) {
            depot_depart_t = (double)start_depot->tw_early;
        }
        if (vehicle->max_duration_seconds > 0 &&
            (time_cursor - depot_depart_t) > (double)vehicle->max_duration_seconds + 1e-9) {
            goto done;
        }
    }

    /* Max total work check */
    if (vehicle->max_total_work_seconds > 0 &&
        seq_total_work > (double)vehicle->max_total_work_seconds + 1e-9) {
        goto done;
    }

    /* Max trips check */
    if (vehicle->has_multi_trip) {
        uint32_t trip_count = (stop_count > 0) ? 1 : 0;
        for (i = 1; i < stop_count; i++) {
            if (stops[i].trip_start) trip_count++;
        }
        if (vehicle->max_trips > 0 && trip_count > vehicle->max_trips) {
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
        if (!vehicle->open_start && start_depot->has_time_window && depot_depart_t < (double)start_depot->tw_early) {
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
        double setup_to_next_seq;
        double break_to_next_seq = 0.0;

        if (idx + 1U < stop_count) {
            if (stops[idx + 1U].trip_start) {
                /* Compound path: stop → end_depot + reload + start_depot → next_stop */
                uint32_t next_loc = ctx->tasks[stops[idx + 1U].task_id].location_id;
                travel_to_next = sg_travel_dur(ctx, cur_loc, vehicle->end_location_id, vehicle_id, depart[idx])
                               + (double)vehicle->trip_reload_seconds
                               + sg_travel_dur(ctx, vehicle->start_location_id, next_loc, vehicle_id, depart[idx]);
                break_to_next_seq = 0.0;
            } else {
                uint32_t next_loc = ctx->tasks[stops[idx + 1U].task_id].location_id;
                travel_to_next = sg_travel_dur(ctx, cur_loc, next_loc, vehicle_id, depart[idx]);
                if (vehicle->has_break_policy) {
                    break_to_next_seq = seq_arrival[idx + 1U] - depart[idx] - travel_to_next;
                    if (break_to_next_seq < 0.0) break_to_next_seq = 0.0;
                }
            }
        } else if (vehicle->open_end) {
            travel_to_next = 0.0;
        } else {
            travel_to_next = sg_travel_dur(ctx, cur_loc, vehicle->end_location_id, vehicle_id, depart[idx]);
            if (vehicle->has_break_policy) {
                break_to_next_seq = seq_depot_return - depart[idx] - travel_to_next;
                if (break_to_next_seq < 0.0) break_to_next_seq = 0.0;
            }
        }

        if (!isfinite(travel_to_next) || travel_to_next < 0.0) {
            goto done;
        }
        setup_to_next_seq = (idx + 1U < stop_count && !stops[idx + 1U].trip_start)
            ? sg_setup_time_between(ctx, stops[idx].request_id, stops[idx + 1U].request_id)
            : 0.0;
        latest_start[idx] = latest_next - travel_to_next - setup_to_next_seq - break_to_next_seq - (double)task->service_seconds;
        if (task->has_time_window) {
            latest_start[idx] = sg_task_snap_backward(task, latest_start[idx]);
        }
        if (!stops[idx].is_pickup) {
            const SGRequestRecord *req = &ctx->requests[stops[idx].request_id];
            if (req->kind == SG_REQUEST_KIND_PICKUP_DELIVERY && req->has_max_ride_time
                && pickup_seen[stops[idx].request_id]) {
                double rb = pickup_depart[stops[idx].request_id]
                             + (double)req->max_ride_time_seconds;
                if (rb < latest_start[idx]) {
                    latest_start[idx] = rb;
                }
            }
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

    } /* end seq_work_since_break scope */

done:
    free(pd_stack);
    free(prec_completed);
    if (!use_scratch) {
        free(service_start);
        free(depart);
        free(latest_start);
        free(forward_slack);
        free(load_profile);
        free(pickup_depart);
        free(pickup_seen);
        free(min_prefix);
        free(max_prefix);
        free(seq_arrival);
    }
    return feasible;
}

int sg_route_sequence_feasible_distance(const SGContext *ctx, uint32_t vehicle_id,
                                        const uint32_t *request_ids, uint32_t request_count,
                                        double *distance_out, double *capacity_scratch) {
    SGRouteStop *stops = NULL;
    uint32_t stop_count = 0;
    uint32_t i;
    int ok = 0;
    int use_scratch = 0;

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

    if (ctx->scratch.feas_stops && request_count * 2U <= ctx->scratch.stop_capacity) {
        use_scratch = 1;
        stops = ctx->scratch.feas_stops;
    } else {
        stops = (SGRouteStop *)malloc((size_t)request_count * 2U * sizeof(SGRouteStop));
        if (!stops) {
            return 0;
        }
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

    if (ok) {
        const SGVehicleRecord *vehicle = &ctx->vehicles[vehicle_id];
        if (vehicle->max_tasks > 0 && request_count > vehicle->max_tasks) {
            ok = 0;
        }
        if (ok && vehicle->max_distance > 0.0 && *distance_out > vehicle->max_distance + 1e-9) {
            ok = 0;
        }
    }

done:
    if (!use_scratch) free(stops);
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
    double ins_violations[SG_PENALTY_COUNT];
    uint8_t pen_enabled;

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
    if (!sg_commodity_compatible(ctx, sol, vehicle_id, request_id)) {
        return 0;
    }
    if (!sg_exclusion_compatible(ctx, sol, vehicle_id, request_id)) {
        return 0;
    }
    if (!sg_compartment_compatible(ctx, vehicle_id, request_id)) {
        return 0;
    }

    pen_enabled = ctx->penalty.enabled;
    memset(ins_violations, 0, sizeof(ins_violations));

    vehicle = &ctx->vehicles[vehicle_id];

    /* Max tasks: route_lengths counts requests = tasks */
    if (vehicle->max_tasks > 0 && sol->route_lengths[vehicle_id] >= vehicle->max_tasks) {
        return 0;
    }

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

    /* Backhaul: D-only request must be inserted before any PD pickup */
    if (ctx->has_backhaul && vehicle->backhaul &&
        ctx->requests[request_id].kind == SG_REQUEST_KIND_DELIVERY_ONLY && stop_len > 0) {
        /* Find stop-level position for new D-only stop at request position 'pos' */
        uint32_t stop_pos = 0, r;
        for (r = 0; r < pos && r < route_len; r++) {
            stop_pos += (ctx->requests[route[r]].kind == SG_REQUEST_KIND_PICKUP_DELIVERY) ? 2 : 1;
        }
        /* Find first PD pickup stop */
        {
            uint32_t k;
            for (k = 0; k < stop_len; k++) {
                if (stops[k].is_pickup &&
                    ctx->requests[stops[k].request_id].kind == SG_REQUEST_KIND_PICKUP_DELIVERY) {
                    if (stop_pos > k) return 0;
                    break;
                }
            }
        }
    }

    /* Precedence: check that request-level position 'pos' respects ordering constraints */
    if (ctx->has_precedence) {
        const SGRequestRecord *req = &ctx->requests[request_id];
        uint16_t p;
        for (p = 0; p < req->num_prec_before; p++) {
            uint32_t pred = req->precedence_before[p];
            if (sol->request_vehicle[pred] != vehicle_id) continue;
            /* Predecessor must be at a request position < pos */
            if (sol->request_pos[pred] >= pos) return 0;
        }
        for (p = 0; p < req->num_prec_after; p++) {
            uint32_t succ = req->precedence_after[p];
            if (sol->request_vehicle[succ] != vehicle_id) continue;
            /* Successor must be at a request position >= pos (will shift to pos+1) */
            if (sol->request_pos[succ] < pos) return 0;
        }
    }

    /* Emit new stops for the request */
    if (!sg_request_emit_stops(ctx, request_id, new_stops, &new_stop_count)) {
        return 0;
    }

    /* Determine preceding timing state */
    if (pos == 0) {
        const SGDepotRecord *start_depot = &ctx->depots[vehicle->start_depot_id];
        prev_depart = vehicle->has_shift_time_window ? (double)vehicle->shift_early : 0.0;
        if (!vehicle->open_start && start_depot->has_time_window && prev_depart < (double)start_depot->tw_early) {
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
        uint32_t prev_req_id = (prev_stop_idx != UINT32_MAX)
            ? stops[prev_stop_idx].request_id : UINT32_MAX;
        double ins_work_since_break = (prev_stop_idx != UINT32_MAX)
            ? stops[prev_stop_idx].work_since_break : 0.0;
        double ins_break_time = 0.0;
        (void)ins_break_time;

        for (ns = 0; ns < new_stop_count; ns++) {
            const SGTaskRecord *task = &ctx->tasks[new_stops[ns].task_id];
            uint32_t task_loc = task->location_id;
            double travel = (vehicle->open_start && pos == 0 && ns == 0)
                ? 0.0
                : sg_travel_dur(ctx, c_loc, task_loc, vehicle_id, cursor);
            double arrival_t;
            double setup = sg_setup_time_between(ctx, prev_req_id, new_stops[ns].request_id);
            double start_t;

            /* Track work and inject breaks during travel */
            ins_work_since_break += travel;
            if (vehicle->has_break_policy) {
                double mw = (double)vehicle->break_max_work_seconds;
                double bd = (double)vehicle->break_duration_seconds;
                while (ins_work_since_break > mw + 1e-9) {
                    ins_work_since_break -= mw;
                    cursor += bd;
                    ins_break_time += bd;
                }
            }

            arrival_t = cursor + travel;
            ins_work_since_break += setup;
            start_t = sg_task_snap_forward(task, arrival_t + setup);
            if (start_t > (double)task->tw_late + 1e-9) {
                if (!pen_enabled) return 0;
                ins_violations[SG_PENALTY_TIME_WARP] += start_t - (double)task->tw_late;
                start_t = (double)task->tw_late; /* warp: pretend on-time for downstream */
            }

            new_stops[ns].arrival = arrival_t;
            new_stops[ns].service_start = start_t;
            new_stops[ns].depart = start_t + (double)task->service_seconds;
            ins_work_since_break += (double)task->service_seconds;

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
                        if (!pen_enabled) return 0;
                        ins_violations[SG_PENALTY_RIDE_TIME] += ride_time - ride_limit;
                    }
                }
            }

            cursor = new_stops[ns].depart;
            prev_req_id = new_stops[ns].request_id;
            c_loc = task_loc;
        }

        /* Max total work check */
        if (vehicle->max_total_work_seconds > 0 && sol->route_total_work) {
            double new_work_delta = 0.0;
            uint32_t last_new_loc;
            /* Travel + service through new stops */
            for (ns = 0; ns < new_stop_count; ns++) {
                const SGTaskRecord *t = &ctx->tasks[new_stops[ns].task_id];
                uint32_t t_loc = t->location_id;
                uint32_t p_loc = (ns == 0) ? prev_loc : ctx->tasks[new_stops[ns - 1].task_id].location_id;
                double seg_travel = sg_travel_dur(ctx, p_loc, t_loc, vehicle_id, 0.0);
                double seg_setup = sg_setup_time_between(ctx,
                    (ns == 0) ? ((prev_stop_idx != UINT32_MAX) ? stops[prev_stop_idx].request_id : UINT32_MAX)
                              : new_stops[ns - 1].request_id,
                    new_stops[ns].request_id);
                new_work_delta += seg_travel + seg_setup + (double)t->service_seconds;
            }
            last_new_loc = ctx->tasks[new_stops[new_stop_count - 1].task_id].location_id;
            /* Account for link change: old prev→next replaced by prev→...→new→next */
            if (next_stop_idx != UINT32_MAX) {
                uint32_t next_loc = ctx->tasks[stops[next_stop_idx].task_id].location_id;
                new_work_delta += sg_travel_dur(ctx, last_new_loc, next_loc, vehicle_id, 0.0);
                if (!(vehicle->open_start && pos == 0)) {
                    new_work_delta -= sg_travel_dur(ctx, prev_loc, next_loc, vehicle_id, 0.0);
                }
            } else if (!vehicle->open_end) {
                new_work_delta += sg_travel_dur(ctx, last_new_loc, vehicle->end_location_id, vehicle_id, 0.0);
                if (!(vehicle->open_start && pos == 0)) {
                    new_work_delta -= sg_travel_dur(ctx, prev_loc, vehicle->end_location_id, vehicle_id, 0.0);
                }
            }
            if (sol->route_total_work[vehicle_id] + new_work_delta >
                (double)vehicle->max_total_work_seconds + 1e-9) {
                if (!pen_enabled) return 0;
                ins_violations[SG_PENALTY_TOTAL_WORK] +=
                    (sol->route_total_work[vehicle_id] + new_work_delta) -
                    (double)vehicle->max_total_work_seconds;
            }
        }

        /* Check push on next existing stop (O(1)) */
        if (next_stop_idx != UINT32_MAX) {
            if (stops[next_stop_idx].trip_start) {
                /* Trip boundary: new stop is at end of current trip.
                   Check depot return + reload + travel to next stop. */
                const SGDepotRecord *sd = &ctx->depots[vehicle->start_depot_id];
                double ret_travel = sg_travel_dur(ctx, c_loc, vehicle->end_location_id, vehicle_id, cursor);
                double ret_brk = 0.0;
                double arr_depot;
                double reload_depart;
                uint32_t next_loc = ctx->tasks[stops[next_stop_idx].task_id].location_id;
                double arr_next;

                if (vehicle->has_break_policy) {
                    double wsb = ins_work_since_break + ret_travel;
                    double mw = (double)vehicle->break_max_work_seconds;
                    double bd = (double)vehicle->break_duration_seconds;
                    while (wsb > mw + 1e-9) {
                        wsb -= mw;
                        ret_brk += bd;
                    }
                }
                arr_depot = cursor + ret_brk + ret_travel;
                if (end_depot->has_time_window && arr_depot > (double)end_depot->tw_late + 1e-9) {
                    if (!pen_enabled) return 0;
                    ins_violations[SG_PENALTY_TIME_WARP] += arr_depot - (double)end_depot->tw_late;
                }
                reload_depart = arr_depot + (double)vehicle->trip_reload_seconds;
                if (sd->has_time_window && reload_depart < (double)sd->tw_early) {
                    reload_depart = (double)sd->tw_early;
                }
                arr_next = reload_depart + sg_travel_dur(ctx, vehicle->start_location_id, next_loc, vehicle_id, reload_depart);
                /* No setup across trip boundary */
                if (arr_next > stops[next_stop_idx].latest_start + 1e-9) {
                    if (!pen_enabled) return 0;
                    ins_violations[SG_PENALTY_TIME_WARP] += arr_next - stops[next_stop_idx].latest_start;
                }
            } else {
                uint32_t next_loc = ctx->tasks[stops[next_stop_idx].task_id].location_id;
                double setup_at_next = sg_setup_time_between(ctx, prev_req_id, stops[next_stop_idx].request_id);
                double travel_to_next_stop = sg_travel_dur(ctx, c_loc, next_loc, vehicle_id, cursor);
                double next_break_time = 0.0;

                /* Account for breaks during travel to next existing stop */
                if (vehicle->has_break_policy) {
                    double wsb = ins_work_since_break + travel_to_next_stop;
                    double mw = (double)vehicle->break_max_work_seconds;
                    double bd = (double)vehicle->break_duration_seconds;
                    while (wsb > mw + 1e-9) {
                        wsb -= mw;
                        next_break_time += bd;
                    }
                }

                {
                double new_arrival_at_next = cursor + next_break_time + travel_to_next_stop;
                if (new_arrival_at_next + setup_at_next > stops[next_stop_idx].latest_start + 1e-9) {
                    if (!pen_enabled) return 0;
                    ins_violations[SG_PENALTY_TIME_WARP] +=
                        (new_arrival_at_next + setup_at_next) - stops[next_stop_idx].latest_start;
                }
                }
            }
        } else {
            /* At end of route: check return to end depot (or shift/duration for open-end) */
            if (vehicle->open_end) {
                if (vehicle->has_shift_time_window && cursor > (double)vehicle->shift_late + 1e-9
                    && !(vehicle->cost_per_overtime > 0.0)) {
                    if (!pen_enabled) return 0;
                    ins_violations[SG_PENALTY_TIME_WARP] += cursor - (double)vehicle->shift_late;
                }
                if (vehicle->max_duration_seconds > 0) {
                    const SGDepotRecord *sd = &ctx->depots[vehicle->start_depot_id];
                    double dd = vehicle->has_shift_time_window ? (double)vehicle->shift_early : 0.0;
                    if (!vehicle->open_start && sd->has_time_window && dd < (double)sd->tw_early) {
                        dd = (double)sd->tw_early;
                    }
                    if ((cursor - dd) > (double)vehicle->max_duration_seconds + 1e-9) {
                        if (!pen_enabled) return 0;
                        ins_violations[SG_PENALTY_DURATION] += (cursor - dd) - (double)vehicle->max_duration_seconds;
                    }
                }
            } else {
                double return_travel = sg_travel_dur(ctx, c_loc, vehicle->end_location_id, vehicle_id, cursor);
                double return_brk = 0.0;
                double arrival_at_end;
                if (vehicle->has_break_policy) {
                    double wsb = ins_work_since_break + return_travel;
                    double mw = (double)vehicle->break_max_work_seconds;
                    double bd = (double)vehicle->break_duration_seconds;
                    while (wsb > mw + 1e-9) {
                        wsb -= mw;
                        return_brk += bd;
                    }
                }
                arrival_at_end = cursor + return_brk + return_travel;
                if (end_depot->has_time_window && arrival_at_end > (double)end_depot->tw_late + 1e-9) {
                    if (!pen_enabled) return 0;
                    ins_violations[SG_PENALTY_TIME_WARP] += arrival_at_end - (double)end_depot->tw_late;
                }
                if (vehicle->has_shift_time_window && arrival_at_end > (double)vehicle->shift_late + 1e-9
                    && !(vehicle->cost_per_overtime > 0.0)) {
                    if (!pen_enabled) return 0;
                    ins_violations[SG_PENALTY_TIME_WARP] += arrival_at_end - (double)vehicle->shift_late;
                }
                if (vehicle->max_duration_seconds > 0) {
                    const SGDepotRecord *sd = &ctx->depots[vehicle->start_depot_id];
                    double dd = vehicle->has_shift_time_window ? (double)vehicle->shift_early : 0.0;
                    if (sd->has_time_window && dd < (double)sd->tw_early) {
                        dd = (double)sd->tw_early;
                    }
                    if ((arrival_at_end - dd) > (double)vehicle->max_duration_seconds + 1e-9) {
                        if (!pen_enabled) return 0;
                        ins_violations[SG_PENALTY_DURATION] += (arrival_at_end - dd) - (double)vehicle->max_duration_seconds;
                    }
                }
            }
        }
    }

    /* Capacity check (signed-load model: max_prefix - min_prefix <= capacity) */
    if (ctx->dimension_count > 0 && sol->route_stop_load) {
        uint32_t insert_stop_pos;

        /* Find where in the stop sequence the insertion occurs */
        if (next_stop_idx != UINT32_MAX) {
            insert_stop_pos = next_stop_idx;
        } else {
            insert_stop_pos = stop_len;
        }

        if (sol->route_seg_cap_prefix_delta) {
            /* O(1) capacity check via concatenation-based segment summaries */
            double concat_violation = 0.0;
            int concat_ok = sg_route_check_capacity_concat(
                ctx, sol, vehicle_id, insert_stop_pos,
                new_stops, new_stop_count, pen_enabled, &concat_violation);

#ifdef SG_CONCAT_VERIFY
            /* Verification: run both O(1) and O(L) paths, assert agreement */
            {
                size_t dim_count = (size_t)ctx->dimension_count;
                size_t load_base = (size_t)vehicle_id * ((size_t)sol->stop_stride + 1U) * dim_count;
                const double *load = sol->route_stop_load + load_base;
                double scan_violation = 0.0;
                int scan_ok = 1;
                uint32_t d;
                uint32_t cap_trip_first = 0;
                uint32_t cap_trip_end = stop_len;
                if (vehicle->has_multi_trip && stop_len > 0) {
                    if (insert_stop_pos < stop_len && stops[insert_stop_pos].trip_start) {
                        cap_trip_end = insert_stop_pos;
                    } else {
                        uint32_t s;
                        for (s = insert_stop_pos + 1; s < stop_len; s++) {
                            if (stops[s].trip_start) { cap_trip_end = s; break; }
                        }
                    }
                    {
                        uint32_t s;
                        for (s = cap_trip_end; s > 0; s--) {
                            if (stops[s - 1].trip_start && s - 1 < cap_trip_end) {
                                cap_trip_first = s - 1;
                                break;
                            }
                        }
                    }
                }
                for (d = 0; d < ctx->dimension_count; d++) {
                    double cap = (vehicle->has_capacity && vehicle->capacity)
                                 ? vehicle->capacity[d] : INFINITY;
                    double added = 0.0;
                    double hyp_min = 0.0, hyp_max = 0.0;
                    double load_at_insert;
                    uint32_t s;
                    uint32_t scan_start = (cap_trip_first > 0) ? cap_trip_first + 1 : 1;
                    for (ns = 0; ns < new_stop_count; ns++) {
                        const SGTaskRecord *task = &ctx->tasks[new_stops[ns].task_id];
                        double demand = (task->has_demand && task->demand) ? task->demand[d] : 0.0;
                        added += demand;
                    }
                    for (s = scan_start; s <= insert_stop_pos; s++) {
                        double val = load[(size_t)s * dim_count + d];
                        if (val < hyp_min) hyp_min = val;
                        if (val > hyp_max) hyp_max = val;
                    }
                    load_at_insert = (insert_stop_pos >= scan_start)
                        ? load[(size_t)insert_stop_pos * dim_count + d] : 0.0;
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
                    for (s = insert_stop_pos + 1; s <= cap_trip_end; s++) {
                        double val = load[(size_t)s * dim_count + d] + added;
                        if (val < hyp_min) hyp_min = val;
                        if (val > hyp_max) hyp_max = val;
                    }
                    if (cap_trip_first == 0 &&
                        vehicle->has_initial_load && vehicle->initial_load) {
                        double il = vehicle->initial_load[d];
                        double excess = 0.0;
                        if (il + hyp_min < -SG_DEMAND_TOLERANCE) excess += -(il + hyp_min);
                        if (il + hyp_max > cap + SG_DEMAND_TOLERANCE) excess += (il + hyp_max) - cap;
                        if (excess > 0.0) { scan_ok = 0; scan_violation += excess; }
                    } else if ((hyp_max - hyp_min) > cap + SG_DEMAND_TOLERANCE) {
                        scan_ok = 0;
                        scan_violation += (hyp_max - hyp_min) - cap;
                    }
                }
                /* Both paths must agree on feasibility */
                if (concat_ok != scan_ok) {
                    fprintf(stderr, "SG_CONCAT_VERIFY FAIL: concat_ok=%d scan_ok=%d "
                            "v=%u pos=%u stop_len=%u concat_viol=%.6f scan_viol=%.6f\n",
                            concat_ok, scan_ok, vehicle_id, insert_stop_pos,
                            stop_len, concat_violation, scan_violation);
                    abort();
                }
            }
#endif /* SG_CONCAT_VERIFY */

            if (!concat_ok) {
                if (!pen_enabled) return 0;
                ins_violations[SG_PENALTY_CAPACITY] += concat_violation;
            }
        } else {
            /* Fallback: O(L) capacity scan (for routes without concat segments) */
            size_t dim_count = (size_t)ctx->dimension_count;
            size_t load_base = (size_t)vehicle_id * ((size_t)sol->stop_stride + 1U) * dim_count;
            const double *load = sol->route_stop_load + load_base;
            uint32_t d;
            uint32_t cap_trip_first = 0;
            uint32_t cap_trip_end = stop_len;
            if (vehicle->has_multi_trip && stop_len > 0) {
                if (insert_stop_pos < stop_len && stops[insert_stop_pos].trip_start) {
                    cap_trip_end = insert_stop_pos;
                } else {
                    uint32_t s;
                    for (s = insert_stop_pos + 1; s < stop_len; s++) {
                        if (stops[s].trip_start) { cap_trip_end = s; break; }
                    }
                }
                {
                    uint32_t s;
                    for (s = cap_trip_end; s > 0; s--) {
                        if (stops[s - 1].trip_start && s - 1 < cap_trip_end) {
                            cap_trip_first = s - 1;
                            break;
                        }
                    }
                }
            }
            for (d = 0; d < ctx->dimension_count; d++) {
                double cap = (vehicle->has_capacity && vehicle->capacity)
                             ? vehicle->capacity[d] : INFINITY;
                double added = 0.0;
                double hyp_min = 0.0;
                double hyp_max = 0.0;
                double load_at_insert;
                uint32_t s;
                uint32_t scan_start = (cap_trip_first > 0) ? cap_trip_first + 1 : 1;
                for (ns = 0; ns < new_stop_count; ns++) {
                    const SGTaskRecord *task = &ctx->tasks[new_stops[ns].task_id];
                    double demand = (task->has_demand && task->demand) ? task->demand[d] : 0.0;
                    added += demand;
                }
                for (s = scan_start; s <= insert_stop_pos; s++) {
                    double val = load[(size_t)s * dim_count + d];
                    if (val < hyp_min) hyp_min = val;
                    if (val > hyp_max) hyp_max = val;
                }
                load_at_insert = (insert_stop_pos >= scan_start)
                    ? load[(size_t)insert_stop_pos * dim_count + d] : 0.0;
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
                for (s = insert_stop_pos + 1; s <= cap_trip_end; s++) {
                    double val = load[(size_t)s * dim_count + d] + added;
                    if (val < hyp_min) hyp_min = val;
                    if (val > hyp_max) hyp_max = val;
                }
                if (cap_trip_first == 0 &&
                    vehicle->has_initial_load && vehicle->initial_load) {
                    double il = vehicle->initial_load[d];
                    double excess = 0.0;
                    if (il + hyp_min < -SG_DEMAND_TOLERANCE) {
                        excess += -(il + hyp_min);
                    }
                    if (il + hyp_max > cap + SG_DEMAND_TOLERANCE) {
                        excess += (il + hyp_max) - cap;
                    }
                    if (excess > 0.0) {
                        if (!pen_enabled) return 0;
                        ins_violations[SG_PENALTY_CAPACITY] += excess;
                    }
                } else if ((hyp_max - hyp_min) > cap + SG_DEMAND_TOLERANCE) {
                    if (!pen_enabled) return 0;
                    ins_violations[SG_PENALTY_CAPACITY] += (hyp_max - hyp_min) - cap;
                }
            }
        }
    }

    /* Compartment capacity check for insertion (single affected compartment) */
    if (ctx->has_compartments && vehicle->num_compartments > 0 &&
        ctx->dimension_count > 0 && sol->route_stop_load) {
        uint32_t ct = ctx->requests[request_id].compartment_type;
        if (ct > 0) {
            int ci = sg_vehicle_find_compartment(vehicle, ct);
            /* ci guaranteed valid by sg_compartment_compatible above */
            if (ci >= 0 && vehicle->compartments[ci].capacity) {
                uint32_t insert_stop_pos = (next_stop_idx != UINT32_MAX) ? next_stop_idx : stop_len;

                /* Same trip boundaries as overall capacity check */
                uint32_t comp_trip_first = 0;
                uint32_t comp_trip_end = stop_len;
                if (vehicle->has_multi_trip && stop_len > 0) {
                    if (insert_stop_pos < stop_len && stops[insert_stop_pos].trip_start) {
                        comp_trip_end = insert_stop_pos;
                    } else {
                        uint32_t s;
                        for (s = insert_stop_pos + 1; s < stop_len; s++) {
                            if (stops[s].trip_start) { comp_trip_end = s; break; }
                        }
                    }
                    {
                        uint32_t s;
                        for (s = comp_trip_end; s > 0; s--) {
                            if (stops[s - 1].trip_start && s - 1 < comp_trip_end) {
                                comp_trip_first = s - 1;
                                break;
                            }
                        }
                    }
                }

                {
                uint32_t cd;
                for (cd = 0; cd < ctx->dimension_count; cd++) {
                    double cap = vehicle->compartments[ci].capacity[cd];
                    double added = 0.0;
                    double hyp_min = 0.0;
                    double hyp_max = 0.0;
                    uint32_t s;

                    for (ns = 0; ns < new_stop_count; ns++) {
                        const SGTaskRecord *task = &ctx->tasks[new_stops[ns].task_id];
                        double demand = (task->has_demand && task->demand) ? task->demand[cd] : 0.0;
                        added += demand;
                    }

                    /* Upstream: only stops in same compartment */
                    {
                        double comp_prefix = 0.0;
                        for (s = comp_trip_first; s < insert_stop_pos; s++) {
                            uint32_t sc = ctx->requests[stops[s].request_id].compartment_type;
                            if (sc == ct) {
                                const SGTaskRecord *t = &ctx->tasks[stops[s].task_id];
                                comp_prefix += (t->has_demand && t->demand) ? t->demand[cd] : 0.0;
                                if (comp_prefix < hyp_min) hyp_min = comp_prefix;
                                if (comp_prefix > hyp_max) hyp_max = comp_prefix;
                            }
                        }

                        /* New stop(s) */
                        {
                            double partial = comp_prefix;
                            for (ns = 0; ns < new_stop_count; ns++) {
                                const SGTaskRecord *task = &ctx->tasks[new_stops[ns].task_id];
                                double demand = (task->has_demand && task->demand) ? task->demand[cd] : 0.0;
                                partial += demand;
                                if (partial < hyp_min) hyp_min = partial;
                                if (partial > hyp_max) hyp_max = partial;
                            }
                        }

                        /* Downstream: only stops in same compartment, shifted by added */
                        for (s = insert_stop_pos; s < comp_trip_end; s++) {
                            uint32_t sc = ctx->requests[stops[s].request_id].compartment_type;
                            if (sc == ct) {
                                const SGTaskRecord *t = &ctx->tasks[stops[s].task_id];
                                comp_prefix += (t->has_demand && t->demand) ? t->demand[cd] : 0.0;
                                {
                                    double val = comp_prefix + added;
                                    if (val < hyp_min) hyp_min = val;
                                    if (val > hyp_max) hyp_max = val;
                                }
                            }
                        }
                    }

                    if ((hyp_max - hyp_min) > cap + SG_DEMAND_TOLERANCE) {
                        if (!pen_enabled) return 0;
                        ins_violations[SG_PENALTY_CAPACITY] += (hyp_max - hyp_min) - cap;
                    }
                }
                }
            }
        }
    }

    /* Compute distance delta */
    {
        uint32_t last_new_loc = ctx->tasks[new_stops[new_stop_count - 1].task_id].location_id;
        uint32_t first_new_loc = ctx->tasks[new_stops[0].task_id].location_id;
        double prev_to_first = (vehicle->open_start && pos == 0)
            ? 0.0
            : sg_travel_dist(ctx, prev_loc, first_new_loc, vehicle_id);

        if (next_stop_idx != UINT32_MAX) {
            if (stops[next_stop_idx].trip_start) {
                /* Trip boundary: old = prev→depot, new = prev→new→depot
                   (start_depot→next part is unchanged and cancels out) */
                old_segment = (vehicle->open_start && pos == 0)
                    ? 0.0
                    : sg_travel_dist(ctx, prev_loc, vehicle->end_location_id, vehicle_id);
                new_segment = prev_to_first;
                if (new_stop_count == 2) {
                    new_segment += sg_travel_dist(ctx, first_new_loc, last_new_loc, vehicle_id);
                }
                new_segment += sg_travel_dist(ctx, last_new_loc, vehicle->end_location_id, vehicle_id);
            } else {
                uint32_t next_loc = ctx->tasks[stops[next_stop_idx].task_id].location_id;
                old_segment = (vehicle->open_start && pos == 0)
                    ? 0.0
                    : sg_travel_dist(ctx, prev_loc, next_loc, vehicle_id);
                new_segment = prev_to_first;
                if (new_stop_count == 2) {
                    new_segment += sg_travel_dist(ctx, first_new_loc, last_new_loc, vehicle_id);
                }
                new_segment += sg_travel_dist(ctx, last_new_loc, next_loc, vehicle_id);
            }
        } else {
            if (vehicle->open_end) {
                old_segment = 0.0;
                new_segment = prev_to_first;
                if (new_stop_count == 2) {
                    new_segment += sg_travel_dist(ctx, first_new_loc, last_new_loc, vehicle_id);
                }
            } else {
                old_segment = (vehicle->open_start && pos == 0)
                    ? 0.0
                    : sg_travel_dist(ctx, prev_loc, vehicle->end_location_id, vehicle_id);
                new_segment = prev_to_first;
                if (new_stop_count == 2) {
                    new_segment += sg_travel_dist(ctx, first_new_loc, last_new_loc, vehicle_id);
                }
                new_segment += sg_travel_dist(ctx, last_new_loc, vehicle->end_location_id, vehicle_id);
            }
        }
    }

    delta = new_segment - old_segment;
    new_route_distance = sol->route_distance[vehicle_id] + delta;

    if (vehicle->max_distance > 0.0 && new_route_distance > vehicle->max_distance + 1e-9) {
        if (!pen_enabled) return 0;
        ins_violations[SG_PENALTY_DISTANCE] += new_route_distance - vehicle->max_distance;
    }

    score = (route_len == 0 ? vehicle->fixed_cost : 0.0) +
            vehicle->cost_per_distance * (new_route_distance - sol->route_distance[vehicle_id]);

    /* Duration-aware scoring: add duration delta when cost_per_duration > 0 */
    if (vehicle->cost_per_duration > 0.0 && sol->route_duration) {
        double duration_delta;
        if (next_stop_idx != UINT32_MAX) {
            uint32_t next_loc = ctx->tasks[stops[next_stop_idx].task_id].location_id;
            double new_start_next = new_stops[new_stop_count - 1].depart +
                sg_travel_dur(ctx, ctx->tasks[new_stops[new_stop_count - 1].task_id].location_id,
                              next_loc, vehicle_id, new_stops[new_stop_count - 1].depart);
            duration_delta = new_start_next - stops[next_stop_idx].arrival;
            if (duration_delta < 0.0) duration_delta = 0.0;
        } else {
            duration_delta = new_stops[new_stop_count - 1].depart - prev_depart;
        }
        score += vehicle->cost_per_duration * duration_delta;
    }

    /* Add penalty cost for infeasible insertion */
    if (pen_enabled) {
        int k;
        for (k = 0; k < SG_PENALTY_COUNT; k++)
            score += ctx->penalty.weight[k] * ins_violations[k];
    }

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
    uint8_t pen_enabled;

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
    if (!sg_commodity_compatible(ctx, sol, vehicle_id, request_id)) {
        return 0;
    }
    if (!sg_exclusion_compatible(ctx, sol, vehicle_id, request_id)) {
        return 0;
    }
    if (!sg_compartment_compatible(ctx, vehicle_id, request_id)) {
        return 0;
    }

    /* Max tasks: route_lengths counts requests = tasks */
    if (ctx->vehicles[vehicle_id].max_tasks > 0 &&
        sol->route_lengths[vehicle_id] >= ctx->vehicles[vehicle_id].max_tasks) {
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
    if (!vehicle->open_start && start_depot->has_time_window && depot_depart < (double)start_depot->tw_early) {
        depot_depart = (double)start_depot->tw_early;
    }

    pen_enabled = ctx->penalty.enabled;

    /* Ride time limit */
    if (request->has_max_ride_time) {
        ride_limit = (double)request->max_ride_time_seconds;
    } else {
        ride_limit = (double)(delivery_task->tw_late - pickup_task->tw_early);
    }

    /* --- LIFO/FIFO precomputation --- */
    int32_t *pd_open_depth = NULL;          /* [stop_len + 1] for LIFO */
    uint32_t *pd_max_del_before = NULL;     /* [stop_len + 2] for FIFO: max delivery pos of earlier pickups */
    uint32_t *pd_min_del_after = NULL;      /* [stop_len + 2] for FIFO: min delivery pos of later pickups */
    uint32_t pd_backhaul_last_donly = 0;    /* Backhaul: last D-only stop position + 1 */

    if (ctx->has_pd_policy && vehicle->pd_policy != SG_PD_POLICY_NONE && stop_len > 0) {
        uint32_t k;
        if (vehicle->pd_policy == SG_PD_POLICY_LIFO) {
            pd_open_depth = (int32_t *)malloc(((size_t)stop_len + 1) * sizeof(int32_t));
            if (!pd_open_depth) return 0;
            {
                int32_t depth = 0;
                pd_open_depth[0] = 0;
                for (k = 0; k < stop_len; k++) {
                    if (stops[k].is_pickup &&
                        ctx->requests[stops[k].request_id].kind == SG_REQUEST_KIND_PICKUP_DELIVERY) {
                        depth++;
                    } else if (!stops[k].is_pickup &&
                               ctx->requests[stops[k].request_id].kind == SG_REQUEST_KIND_PICKUP_DELIVERY) {
                        depth--;
                    }
                    pd_open_depth[k + 1] = depth;
                }
            }
        } else { /* FIFO */
            /* Build pickup-order delivery position map */
            pd_max_del_before = (uint32_t *)malloc(((size_t)stop_len + 2) * sizeof(uint32_t));
            pd_min_del_after = (uint32_t *)malloc(((size_t)stop_len + 2) * sizeof(uint32_t));
            if (!pd_max_del_before || !pd_min_del_after) {
                free(pd_max_del_before);
                free(pd_min_del_after);
                pd_max_del_before = NULL;
                pd_min_del_after = NULL;
                return 0;
            }
            /* max_del_before[i] = max delivery stop position for PD pairs whose pickup pos < i */
            {
                uint32_t max_del = 0;
                pd_max_del_before[0] = 0;
                for (k = 0; k < stop_len; k++) {
                    if (stops[k].is_pickup &&
                        ctx->requests[stops[k].request_id].kind == SG_REQUEST_KIND_PICKUP_DELIVERY) {
                        /* Find this pair's delivery position */
                        uint32_t del_pos = sol->request_delivery_stop_pos[stops[k].request_id];
                        if (del_pos > max_del) max_del = del_pos;
                    }
                    pd_max_del_before[k + 1] = max_del;
                }
                pd_max_del_before[stop_len + 1] = max_del;
            }
            /* min_del_after[i] = min delivery stop position for PD pairs whose pickup pos >= i
               Sentinel: stop_len + 1 means no such pair exists (no constraint). */
            {
                uint32_t min_del = stop_len + 1;
                pd_min_del_after[stop_len + 1] = stop_len + 1;
                pd_min_del_after[stop_len] = stop_len + 1;
                for (k = stop_len; k > 0; k--) {
                    if (stops[k - 1].is_pickup &&
                        ctx->requests[stops[k - 1].request_id].kind == SG_REQUEST_KIND_PICKUP_DELIVERY) {
                        uint32_t del_pos = sol->request_delivery_stop_pos[stops[k - 1].request_id];
                        if (del_pos < min_del) min_del = del_pos;
                    }
                    pd_min_del_after[k - 1] = min_del;
                }
            }
        }
    }

    /* --- Backhaul precomputation --- */
    if (ctx->has_backhaul && vehicle->backhaul && stop_len > 0) {
        uint32_t k;
        for (k = 0; k < stop_len; k++) {
            if (!stops[k].is_pickup &&
                ctx->requests[stops[k].request_id].kind == SG_REQUEST_KIND_DELIVERY_ONLY) {
                pd_backhaul_last_donly = k + 1;
            }
        }
    }

    /* --- Precedence precomputation --- */
    int32_t prec_pickup_earliest = 0;
    int32_t prec_delivery_latest = (int32_t)(stop_len + 1);

    if (ctx->has_precedence) {
        uint16_t pp;
        for (pp = 0; pp < request->num_prec_before; pp++) {
            uint32_t pred = request->precedence_before[pp];
            if (sol->request_vehicle[pred] != vehicle_id) continue;
            /* Predecessor's last stop must be before our pickup */
            uint32_t last = sol->request_delivery_stop_pos[pred];
            if ((int32_t)(last + 1) > prec_pickup_earliest)
                prec_pickup_earliest = (int32_t)(last + 1);
        }
        for (pp = 0; pp < request->num_prec_after; pp++) {
            uint32_t succ = request->precedence_after[pp];
            if (sol->request_vehicle[succ] != vehicle_id) continue;
            /* Successor's first stop must be after our delivery */
            uint32_t first_stop = (ctx->requests[succ].kind == SG_REQUEST_KIND_PICKUP_DELIVERY)
                ? sol->request_pickup_stop_pos[succ]
                : sol->request_delivery_stop_pos[succ];
            if ((int32_t)first_stop < prec_delivery_latest)
                prec_delivery_latest = (int32_t)first_stop;
        }
        if (prec_pickup_earliest > prec_delivery_latest) {
            free(pd_open_depth);
            free(pd_max_del_before);
            free(pd_min_del_after);
            return 0;
        }
    }

    /* For each pickup position i = 0..stop_len */
    for (i = 0; i <= stop_len; i++) {
        double prev_depart;
        uint32_t prev_loc;
        double p_travel, p_arrival, p_start, p_depart;
        double p_wsb, p_break_time;
        uint32_t j;

        /* Precedence: pickup position must be >= prec_pickup_earliest */
        if (ctx->has_precedence && (int32_t)i < prec_pickup_earliest) {
            continue;
        }

        /* Backhaul: PD pickup must be after all D-only stops */
        if (ctx->has_backhaul && vehicle->backhaul && i < pd_backhaul_last_donly) {
            continue;
        }

        /* A. Compute pickup timing */
        if (i == 0) {
            prev_depart = depot_depart;
            prev_loc = vehicle->start_location_id;
        } else {
            prev_depart = stops[i - 1].depart;
            prev_loc = ctx->tasks[stops[i - 1].task_id].location_id;
        }

        {
        uint32_t prev_req_pd = (i > 0) ? stops[i - 1].request_id : UINT32_MAX;
        double p_setup = sg_setup_time_between(ctx, prev_req_pd, request_id);
        double p_tw_penalty = 0.0;
        int break_after_j_loop = 0;
        p_travel = (vehicle->open_start && i == 0)
            ? 0.0
            : sg_travel_dur(ctx, prev_loc, pickup_loc, vehicle_id, prev_depart);
        p_wsb = (i > 0) ? stops[i - 1].work_since_break : 0.0;
        p_break_time = 0.0;
        p_wsb += p_travel;
        if (vehicle->has_break_policy) {
            double mw = (double)vehicle->break_max_work_seconds;
            double bd = (double)vehicle->break_duration_seconds;
            while (p_wsb > mw + 1e-9) {
                p_wsb -= mw;
                p_break_time += bd;
            }
        }
        p_arrival = prev_depart + p_break_time + p_travel;
        p_wsb += p_setup;
        p_start = sg_task_snap_forward(pickup_task, p_arrival + p_setup);
        if (p_start > (double)pickup_task->tw_late + 1e-9) {
            if (!pen_enabled) break; /* Later i only arrives later at pickup */
            p_tw_penalty = p_start - (double)pickup_task->tw_late;
            p_start = (double)pickup_task->tw_late; /* warp */
            break_after_j_loop = 1;
        }
        p_depart = p_start + (double)pickup_task->service_seconds;
        p_wsb += (double)pickup_task->service_seconds;

        /* B. Propagate push and check delivery positions */
        {
            /* push[k] for k = i..stop_len-1:
               how much stop[k] would be delayed by inserting pickup before it */
            double push_k = 0.0;

            for (j = i + 1; j <= stop_len + 1; j++) {
                double d_prev_depart;
                uint32_t d_prev_loc;
                double d_travel, d_arrival, d_start, d_depart;
                double d_wsb, d_break_time;
                double ride_time;
                double delta;
                double new_route_distance, score;
                double pd_viol[SG_PENALTY_COUNT];
                int break_j = 0;

                memset(pd_viol, 0, sizeof(pd_viol));

                /* PD must be in same trip: break if a trip boundary crossed */
                if (vehicle->has_multi_trip && j >= i + 2 && j - 2 < stop_len &&
                    stops[j - 2].trip_start) {
                    break;
                }

                /* LIFO: between our pickup at position i and delivery at position j,
                   all existing PD pairs must be complete (opened and closed).
                   pd_open_depth[k] = depth before stop[k]. Delivery at j means
                   it goes after stop[j-2], so depth at insertion point = depth[j-1].
                   For adjacent (j==i+1), depth[i]==depth[i] always true. */
                if (pd_open_depth && vehicle->pd_policy == SG_PD_POLICY_LIFO) {
                    uint32_t depth_idx = (j - 1 <= stop_len) ? j - 1 : stop_len;
                    if (pd_open_depth[depth_idx] != pd_open_depth[i]) {
                        goto next_j;
                    }
                }

                /* FIFO: our delivery must come strictly after deliveries of pairs
                   picked up before position i, and at or before deliveries of
                   pairs picked up at or after position i.
                   j-1 is our delivery's effective position in the pre-insertion
                   stop array (delivery goes between stop[j-2] and stop[j-1]). */
                if (pd_max_del_before && vehicle->pd_policy == SG_PD_POLICY_FIFO) {
                    uint32_t eff_del = (j - 1 <= stop_len) ? j - 1 : stop_len;
                    /* Our delivery must be after max delivery of earlier pickups */
                    if (i > 0 && pd_max_del_before[i] >= eff_del) {
                        goto next_j;
                    }
                    /* Our delivery must be at or before min delivery of later pickups */
                    if (pd_min_del_after[i] < eff_del) {
                        goto next_j;
                    }
                }

                /* Precedence: delivery must be before all successors' first stops */
                if (ctx->has_precedence && (int32_t)(j - 1) >= prec_delivery_latest) {
                    break;  /* Later j only makes it worse */
                }

                /* -- Try delivery at position j -- */

                /* Delivery arrival */
                if (j == i + 1) {
                    /* Delivery immediately after pickup */
                    d_prev_depart = p_depart;
                    d_prev_loc = pickup_loc;
                    d_wsb = p_wsb;
                } else {
                    /* Delivery after stop[j-1] (which has been pushed) */
                    d_prev_depart = stops[j - 2].depart + push_k;
                    d_prev_loc = ctx->tasks[stops[j - 2].task_id].location_id;
                    d_wsb = stops[j - 2].work_since_break;
                }

                {
                uint32_t d_prev_req = (j == i + 1) ? request_id
                    : stops[j - 2].request_id;
                double d_setup = sg_setup_time_between(ctx, d_prev_req, request_id);
                d_travel = sg_travel_dur(ctx, d_prev_loc, delivery_loc, vehicle_id, d_prev_depart);
                d_wsb += d_travel;
                d_break_time = 0.0;
                if (vehicle->has_break_policy) {
                    double mw = (double)vehicle->break_max_work_seconds;
                    double bd = (double)vehicle->break_duration_seconds;
                    while (d_wsb > mw + 1e-9) {
                        d_wsb -= mw;
                        d_break_time += bd;
                    }
                }
                d_arrival = d_prev_depart + d_break_time + d_travel;
                d_wsb += d_setup;
                d_start = sg_task_snap_forward(delivery_task, d_arrival + d_setup);
                if (d_start > (double)delivery_task->tw_late + 1e-9) {
                    if (!pen_enabled) break; /* Later j only makes it worse */
                    pd_viol[SG_PENALTY_TIME_WARP] += d_start - (double)delivery_task->tw_late;
                    d_start = (double)delivery_task->tw_late; /* warp */
                    break_j = 1;
                }

                /* Ride time check */
                ride_time = d_start - p_depart;
                if (isfinite(ride_limit) && ride_time > ride_limit + 1e-9) {
                    if (!pen_enabled) break; /* Later j is worse */
                    pd_viol[SG_PENALTY_RIDE_TIME] += ride_time - ride_limit;
                    break_j = 1;
                }

                d_depart = d_start + (double)delivery_task->service_seconds;
                d_wsb += (double)delivery_task->service_seconds;

                /* Check push on stop after delivery */
                if (j <= stop_len) {
                    if (stops[j - 1].trip_start) {
                        /* Trip boundary: delivery is at end of current trip.
                           Check depot return + reload + travel to next. */
                        double ret_travel_d = sg_travel_dur(ctx, delivery_loc, vehicle->end_location_id, vehicle_id, d_depart);
                        double ret_brk_d = 0.0;
                        double arr_depot_d;
                        double reload_dep_d;
                        uint32_t next_loc_d = ctx->tasks[stops[j - 1].task_id].location_id;
                        double arr_next_d;
                        if (vehicle->has_break_policy) {
                            double wsb = d_wsb + ret_travel_d;
                            double mw = (double)vehicle->break_max_work_seconds;
                            double bd = (double)vehicle->break_duration_seconds;
                            while (wsb > mw + 1e-9) {
                                wsb -= mw;
                                ret_brk_d += bd;
                            }
                        }
                        arr_depot_d = d_depart + ret_brk_d + ret_travel_d;
                        if (end_depot->has_time_window && arr_depot_d > (double)end_depot->tw_late + 1e-9) {
                            if (!pen_enabled) goto next_j;
                            pd_viol[SG_PENALTY_TIME_WARP] += arr_depot_d - (double)end_depot->tw_late;
                        }
                        reload_dep_d = arr_depot_d + (double)vehicle->trip_reload_seconds;
                        if (start_depot->has_time_window && reload_dep_d < (double)start_depot->tw_early) {
                            reload_dep_d = (double)start_depot->tw_early;
                        }
                        arr_next_d = reload_dep_d + sg_travel_dur(ctx, vehicle->start_location_id, next_loc_d, vehicle_id, reload_dep_d);
                        if (arr_next_d > stops[j - 1].latest_start + 1e-9) {
                            if (!pen_enabled) goto next_j;
                            pd_viol[SG_PENALTY_TIME_WARP] += arr_next_d - stops[j - 1].latest_start;
                        }
                    } else {
                    /* There is a stop[j-1] in the original array at index j-1.
                       After inserting pickup at i, original stop at index j-1
                       becomes the stop after delivery. */
                    uint32_t next_loc = ctx->tasks[stops[j - 1].task_id].location_id;
                    double setup_after_d = sg_setup_time_between(ctx, request_id, stops[j - 1].request_id);
                    double travel_d_to_next = sg_travel_dur(ctx, delivery_loc, next_loc, vehicle_id, d_depart);
                    double next_brk_d = 0.0;
                    double new_arrival_next;
                    if (vehicle->has_break_policy) {
                        double wsb = d_wsb + travel_d_to_next;
                        double mw = (double)vehicle->break_max_work_seconds;
                        double bd = (double)vehicle->break_duration_seconds;
                        while (wsb > mw + 1e-9) {
                            wsb -= mw;
                            next_brk_d += bd;
                        }
                    }
                    new_arrival_next = d_depart + next_brk_d + travel_d_to_next;
                    /* The pushed latest_start of stop[j-1] */
                    if (new_arrival_next + setup_after_d > stops[j - 1].latest_start + 1e-9) {
                        if (!pen_enabled) goto next_j;
                        pd_viol[SG_PENALTY_TIME_WARP] += (new_arrival_next + setup_after_d) - stops[j - 1].latest_start;
                    }
                    } /* end else (non-trip-boundary push check) */
                } else {
                    /* j == stop_len + 1: delivery at end of route */
                    if (vehicle->open_end) {
                        if (vehicle->has_shift_time_window &&
                            d_depart > (double)vehicle->shift_late + 1e-9
                            && !(vehicle->cost_per_overtime > 0.0)) {
                            if (!pen_enabled) goto next_j;
                            pd_viol[SG_PENALTY_TIME_WARP] += d_depart - (double)vehicle->shift_late;
                        }
                        if (vehicle->max_duration_seconds > 0 &&
                            (d_depart - depot_depart) > (double)vehicle->max_duration_seconds + 1e-9) {
                            if (!pen_enabled) goto next_j;
                            pd_viol[SG_PENALTY_DURATION] += (d_depart - depot_depart) - (double)vehicle->max_duration_seconds;
                        }
                    } else {
                        double ret_travel_pd = sg_travel_dur(ctx, delivery_loc, vehicle->end_location_id, vehicle_id, d_depart);
                        double ret_brk_pd = 0.0;
                        double arrival_at_end;
                        if (vehicle->has_break_policy) {
                            double wsb = d_wsb + ret_travel_pd;
                            double mw = (double)vehicle->break_max_work_seconds;
                            double bd = (double)vehicle->break_duration_seconds;
                            while (wsb > mw + 1e-9) {
                                wsb -= mw;
                                ret_brk_pd += bd;
                            }
                        }
                        arrival_at_end = d_depart + ret_brk_pd + ret_travel_pd;
                        if (end_depot->has_time_window &&
                            arrival_at_end > (double)end_depot->tw_late + 1e-9) {
                            if (!pen_enabled) goto next_j;
                            pd_viol[SG_PENALTY_TIME_WARP] += arrival_at_end - (double)end_depot->tw_late;
                        }
                        if (vehicle->has_shift_time_window &&
                            arrival_at_end > (double)vehicle->shift_late + 1e-9
                            && !(vehicle->cost_per_overtime > 0.0)) {
                            if (!pen_enabled) goto next_j;
                            pd_viol[SG_PENALTY_TIME_WARP] += arrival_at_end - (double)vehicle->shift_late;
                        }
                        if (vehicle->max_duration_seconds > 0 &&
                            (arrival_at_end - depot_depart) > (double)vehicle->max_duration_seconds + 1e-9) {
                            if (!pen_enabled) goto next_j;
                            pd_viol[SG_PENALTY_DURATION] += (arrival_at_end - depot_depart) - (double)vehicle->max_duration_seconds;
                        }
                    }
                }

                /* Capacity check: between pickup (at i) and delivery (at j),
                   load increases by pickup demand. Check within trip only. */
                if (ctx->dimension_count > 0 && sol->route_stop_load) {
                    size_t dim_count = (size_t)ctx->dimension_count;
                    size_t load_base = (size_t)vehicle_id *
                        ((size_t)sol->stop_stride + 1U) * dim_count;
                    const double *load = sol->route_stop_load + load_base;
                    uint32_t d;
                    int cap_ok = 1;

                    /* Find trip boundaries for capacity scan */
                    uint32_t pd_trip_first = 0;
                    uint32_t pd_trip_end = stop_len;
                    if (vehicle->has_multi_trip && stop_len > 0) {
                        uint32_t s;
                        for (s = i; s > 0; s--) {
                            if (stops[s].trip_start) { pd_trip_first = s; break; }
                        }
                        for (s = (pd_trip_first > 0 ? pd_trip_first + 1 : 1); s < stop_len; s++) {
                            if (stops[s].trip_start && s >= i) { pd_trip_end = s; break; }
                        }
                    }

                    for (d = 0; d < ctx->dimension_count && cap_ok; d++) {
                        double cap = (vehicle->has_capacity && vehicle->capacity)
                                     ? vehicle->capacity[d] : INFINITY;
                        double pickup_demand = (pickup_task->has_demand && pickup_task->demand)
                                               ? pickup_task->demand[d] : 0.0;
                        double delivery_demand = (delivery_task->has_demand && delivery_task->demand)
                                                 ? delivery_task->demand[d] : 0.0;
                        uint32_t s;
                        double hyp_min = 0.0, hyp_max = 0.0;  /* Trip initial load */
                        uint32_t scan_start = (pd_trip_first > 0) ? pd_trip_first + 1 : 1;

                        /* Before pickup insertion point within trip */
                        for (s = scan_start; s <= i; s++) {
                            double val = load[s * dim_count + d];
                            if (val < hyp_min) hyp_min = val;
                            if (val > hyp_max) hyp_max = val;
                        }
                        /* After pickup: load[i] + pickup_demand */
                        {
                            double base = (i >= scan_start) ? load[i * dim_count + d] : 0.0;
                            double val = base + pickup_demand;
                            if (val < hyp_min) hyp_min = val;
                            if (val > hyp_max) hyp_max = val;
                        }
                        /* Between pickup and delivery: shifted by pickup_demand */
                        for (s = i + 1; s <= j - 1 && s <= pd_trip_end; s++) {
                            double val = load[s * dim_count + d] + pickup_demand;
                            if (val < hyp_min) hyp_min = val;
                            if (val > hyp_max) hyp_max = val;
                        }
                        /* After delivery: load[j-1] + pickup + delivery */
                        {
                            uint32_t load_idx = j - 1 <= pd_trip_end ? j - 1 : pd_trip_end;
                            double val = load[load_idx * dim_count + d] + pickup_demand + delivery_demand;
                            if (val < hyp_min) hyp_min = val;
                            if (val > hyp_max) hyp_max = val;
                        }
                        /* After delivery within trip: shifted by pickup + delivery */
                        for (s = j; s <= pd_trip_end; s++) {
                            double val = load[s * dim_count + d] + pickup_demand + delivery_demand;
                            if (val < hyp_min) hyp_min = val;
                            if (val > hyp_max) hyp_max = val;
                        }

                        if ((hyp_max - hyp_min) > cap + SG_DEMAND_TOLERANCE) {
                            cap_ok = 0;
                            if (pen_enabled)
                                pd_viol[SG_PENALTY_CAPACITY] += (hyp_max - hyp_min) - cap;
                        }
                    }
                    if (!cap_ok) {
                        if (!pen_enabled) goto next_j;
                    }
                }

                /* Compartment capacity check for PD insertion */
                if (ctx->has_compartments && vehicle->num_compartments > 0 &&
                    ctx->dimension_count > 0 && sol->route_stop_load) {
                    uint32_t ct = ctx->requests[request_id].compartment_type;
                    if (ct > 0) {
                        int ci = sg_vehicle_find_compartment(vehicle, ct);
                        if (ci >= 0 && vehicle->compartments[ci].capacity) {
                            uint32_t d;
                            int comp_ok = 1;
                            for (d = 0; d < ctx->dimension_count && comp_ok; d++) {
                                double cap = vehicle->compartments[ci].capacity[d];
                                double pickup_dem = (pickup_task->has_demand && pickup_task->demand)
                                                    ? pickup_task->demand[d] : 0.0;
                                double delivery_dem = (delivery_task->has_demand && delivery_task->demand)
                                                      ? delivery_task->demand[d] : 0.0;
                                double comp_prefix = 0.0;
                                double hyp_min = 0.0, hyp_max = 0.0;
                                uint32_t s;

                                /* Before pickup: compartment-filtered prefix sums */
                                for (s = 0; s < i; s++) {
                                    if (ctx->requests[stops[s].request_id].compartment_type == ct) {
                                        const SGTaskRecord *t = &ctx->tasks[stops[s].task_id];
                                        comp_prefix += (t->has_demand && t->demand) ? t->demand[d] : 0.0;
                                        if (comp_prefix < hyp_min) hyp_min = comp_prefix;
                                        if (comp_prefix > hyp_max) hyp_max = comp_prefix;
                                    }
                                }
                                /* After pickup */
                                {
                                    double val = comp_prefix + pickup_dem;
                                    if (val < hyp_min) hyp_min = val;
                                    if (val > hyp_max) hyp_max = val;
                                }
                                /* Between pickup and delivery */
                                {
                                    double shifted = comp_prefix + pickup_dem;
                                    for (s = i; s < j - 1 && s < stop_len; s++) {
                                        if (ctx->requests[stops[s].request_id].compartment_type == ct) {
                                            const SGTaskRecord *t = &ctx->tasks[stops[s].task_id];
                                            shifted += (t->has_demand && t->demand) ? t->demand[d] : 0.0;
                                            if (shifted < hyp_min) hyp_min = shifted;
                                            if (shifted > hyp_max) hyp_max = shifted;
                                        }
                                    }
                                    /* After delivery */
                                    {
                                        double val2 = shifted + delivery_dem;
                                        if (val2 < hyp_min) hyp_min = val2;
                                        if (val2 > hyp_max) hyp_max = val2;
                                    }
                                    /* After delivery within route */
                                    {
                                        double after_del = shifted + delivery_dem;
                                        for (s = (j > 0 ? j - 1 : 0); s < stop_len; s++) {
                                            if (ctx->requests[stops[s].request_id].compartment_type == ct) {
                                                const SGTaskRecord *t = &ctx->tasks[stops[s].task_id];
                                                after_del += (t->has_demand && t->demand) ? t->demand[d] : 0.0;
                                                if (after_del < hyp_min) hyp_min = after_del;
                                                if (after_del > hyp_max) hyp_max = after_del;
                                            }
                                        }
                                    }
                                }

                                if ((hyp_max - hyp_min) > cap + SG_DEMAND_TOLERANCE) {
                                    comp_ok = 0;
                                    if (pen_enabled)
                                        pd_viol[SG_PENALTY_CAPACITY] += (hyp_max - hyp_min) - cap;
                                }
                            }
                            if (!comp_ok && !pen_enabled) goto next_j;
                        }
                    }
                }

                /* Distance delta */
                {
                    double old_seg_pickup, new_seg_pickup;
                    double old_seg_delivery, new_seg_delivery;
                    int skip_prev_leg = (vehicle->open_start && i == 0);

                    /* Pickup segment: prev_of_i -> stop[i] becomes prev_of_i -> pickup -> ... */
                    if (j == i + 1) {
                        /* Adjacent: prev -> pickup -> delivery -> stop[i] (or end/depot) */
                        double prev_to_pickup = skip_prev_leg
                            ? 0.0
                            : sg_travel_dist(ctx, prev_loc, pickup_loc, vehicle_id);
                        if (i < stop_len) {
                            if (stops[i].trip_start) {
                                /* Trip boundary after delivery: go to depot */
                                uint32_t next_loc = vehicle->end_location_id;
                                old_seg_pickup = skip_prev_leg
                                    ? 0.0
                                    : sg_travel_dist(ctx, prev_loc, next_loc, vehicle_id);
                                new_seg_pickup = prev_to_pickup +
                                                  sg_travel_dist(ctx, pickup_loc, delivery_loc, vehicle_id) +
                                                  sg_travel_dist(ctx, delivery_loc, next_loc, vehicle_id);
                            } else {
                                uint32_t next_loc = ctx->tasks[stops[i].task_id].location_id;
                                old_seg_pickup = skip_prev_leg
                                    ? 0.0
                                    : sg_travel_dist(ctx, prev_loc, next_loc, vehicle_id);
                                new_seg_pickup = prev_to_pickup +
                                                  sg_travel_dist(ctx, pickup_loc, delivery_loc, vehicle_id) +
                                                  sg_travel_dist(ctx, delivery_loc, next_loc, vehicle_id);
                            }
                        } else if (vehicle->open_end) {
                            old_seg_pickup = 0.0;
                            new_seg_pickup = prev_to_pickup +
                                              sg_travel_dist(ctx, pickup_loc, delivery_loc, vehicle_id);
                        } else {
                            uint32_t next_loc = vehicle->end_location_id;
                            old_seg_pickup = skip_prev_leg
                                ? 0.0
                                : sg_travel_dist(ctx, prev_loc, next_loc, vehicle_id);
                            new_seg_pickup = prev_to_pickup +
                                              sg_travel_dist(ctx, pickup_loc, delivery_loc, vehicle_id) +
                                              sg_travel_dist(ctx, delivery_loc, next_loc, vehicle_id);
                        }
                        delta = new_seg_pickup - old_seg_pickup;
                    } else {
                        /* Non-adjacent: two separate segment changes */
                        uint32_t stop_i_loc;
                        uint32_t stop_jm1_loc; /* stop at j-2 in original */

                        /* Pickup segment: prev -> stop[i] becomes prev -> pickup -> stop[i] */
                        stop_i_loc = ctx->tasks[stops[i].task_id].location_id;
                        if (skip_prev_leg) {
                            old_seg_pickup = 0.0;
                            new_seg_pickup = sg_travel_dist(ctx, pickup_loc, stop_i_loc, vehicle_id);
                        } else {
                            old_seg_pickup = sg_travel_dist(ctx, prev_loc, stop_i_loc, vehicle_id);
                            new_seg_pickup = sg_travel_dist(ctx, prev_loc, pickup_loc, vehicle_id) +
                                              sg_travel_dist(ctx, pickup_loc, stop_i_loc, vehicle_id);
                        }

                        /* Delivery segment: stop[j-2] -> stop[j-1] becomes
                           stop[j-2] -> delivery -> stop[j-1] (or depot at trip boundary) */
                        stop_jm1_loc = ctx->tasks[stops[j - 2].task_id].location_id;
                        if (j - 1 < stop_len) {
                            if (stops[j - 1].trip_start) {
                                /* Trip boundary: delivery → depot */
                                uint32_t after_d_loc = vehicle->end_location_id;
                                old_seg_delivery = sg_travel_dist(ctx, stop_jm1_loc, after_d_loc, vehicle_id);
                                new_seg_delivery = sg_travel_dist(ctx, stop_jm1_loc, delivery_loc, vehicle_id) +
                                                    sg_travel_dist(ctx, delivery_loc, after_d_loc, vehicle_id);
                            } else {
                                uint32_t after_d_loc = ctx->tasks[stops[j - 1].task_id].location_id;
                                old_seg_delivery = sg_travel_dist(ctx, stop_jm1_loc, after_d_loc, vehicle_id);
                                new_seg_delivery = sg_travel_dist(ctx, stop_jm1_loc, delivery_loc, vehicle_id) +
                                                    sg_travel_dist(ctx, delivery_loc, after_d_loc, vehicle_id);
                            }
                        } else if (vehicle->open_end) {
                            old_seg_delivery = 0.0;
                            new_seg_delivery = sg_travel_dist(ctx, stop_jm1_loc, delivery_loc, vehicle_id);
                        } else {
                            uint32_t after_d_loc = vehicle->end_location_id;
                            old_seg_delivery = sg_travel_dist(ctx, stop_jm1_loc, after_d_loc, vehicle_id);
                            new_seg_delivery = sg_travel_dist(ctx, stop_jm1_loc, delivery_loc, vehicle_id) +
                                                sg_travel_dist(ctx, delivery_loc, after_d_loc, vehicle_id);
                        }
                        delta = (new_seg_pickup - old_seg_pickup) +
                                (new_seg_delivery - old_seg_delivery);
                    }
                }

                new_route_distance = sol->route_distance[vehicle_id] + delta;

                if (vehicle->max_distance > 0.0 && new_route_distance > vehicle->max_distance + 1e-9) {
                    if (!pen_enabled) goto next_j;
                    pd_viol[SG_PENALTY_DISTANCE] += new_route_distance - vehicle->max_distance;
                }

                score = (stop_len == 0 ? vehicle->fixed_cost : 0.0) +
                        vehicle->cost_per_distance * delta;

                /* Duration-aware scoring for PD insertion */
                if (vehicle->cost_per_duration > 0.0 && sol->route_duration) {
                    double dur_delta;
                    if (j <= stop_len) {
                        double push_at_j = d_depart +
                            sg_travel_dur(ctx, delivery_loc,
                                          ctx->tasks[stops[j - 1].task_id].location_id,
                                          vehicle_id, d_depart)
                            - stops[j - 1].arrival;
                        dur_delta = push_at_j > 0.0 ? push_at_j : 0.0;
                    } else {
                        dur_delta = d_depart - (stop_len > 0 ? stops[stop_len - 1].depart : depot_depart);
                        if (!vehicle->open_end) {
                            dur_delta += sg_travel_dur(ctx, delivery_loc,
                                                       vehicle->end_location_id, vehicle_id, d_depart);
                            if (stop_len > 0) {
                                dur_delta -= sg_travel_dur(ctx,
                                    ctx->tasks[stops[stop_len - 1].task_id].location_id,
                                    vehicle->end_location_id, vehicle_id, stops[stop_len - 1].depart);
                            }
                        }
                    }
                    score += vehicle->cost_per_duration * dur_delta;
                }

                /* Ride-time penalty: penalize excess ride time above direct travel.
                   Use the larger of distance/duration cost as weight so the penalty
                   is meaningful under any cost model. */
                if (request->has_max_ride_time) {
                    double direct_travel = sg_travel_dur(ctx, pickup_loc, delivery_loc, vehicle_id, p_depart);
                    double excess = ride_time - direct_travel;
                    if (excess > 0.0) {
                        double w = vehicle->cost_per_distance > vehicle->cost_per_duration
                                   ? vehicle->cost_per_distance : vehicle->cost_per_duration;
                        score += w * excess;
                    }
                }

                /* Add penalty cost for infeasible PD insertion */
                if (pen_enabled) {
                    int k;
                    score += ctx->penalty.weight[SG_PENALTY_TIME_WARP] * p_tw_penalty;
                    for (k = 0; k < SG_PENALTY_COUNT; k++)
                        score += ctx->penalty.weight[k] * pd_viol[k];
                }

                if (score < best_score) {
                    best_score = score;
                    best_i = i;
                    best_j = j;
                    best_dist = new_route_distance;
                    found = 1;
                }
                if (break_j) break;
                } /* end d_setup scope */

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
                        double travel_p_to_i = sg_travel_dur(ctx, pickup_loc, stop_i_loc, vehicle_id, p_depart);
                        double push_brk = 0.0;
                        double new_arr;
                        if (vehicle->has_break_policy) {
                            double wsb = p_wsb + travel_p_to_i;
                            double mw = (double)vehicle->break_max_work_seconds;
                            double bd = (double)vehicle->break_duration_seconds;
                            while (wsb > mw + 1e-9) {
                                wsb -= mw;
                                push_brk += bd;
                            }
                        }
                        new_arr = p_depart + push_brk + travel_p_to_i;
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
        if (break_after_j_loop) break;
        } /* end prev_req_pd scope */
    } /* end for i */

    free(pd_open_depth);
    free(pd_max_del_before);
    free(pd_min_del_after);

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

    /* Shift route_request_trip_start alongside route_requests */
    if (sol->route_request_trip_start) {
        uint8_t *ts = sol->route_request_trip_start + (size_t)vehicle_id * sol->route_stride;
        if (pos < old_len) {
            memmove(&ts[pos + 1], &ts[pos], (size_t)(old_len - pos) * sizeof(uint8_t));
        }
        ts[pos] = 0;  /* Default: not a new trip start. Caller sets if needed. */
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

    /* Update commodity tracking */
    if (sol->route_commodities) {
        uint32_t cid = ctx->requests[request_id].commodity_id;
        if (cid > 0) {
            sol->route_commodities[vehicle_id] |= (1ULL << (cid - 1));
        }
    }
    /* Update exclusion tracking */
    if (sol->route_exclusion_counts) {
        const SGRequestRecord *req = &ctx->requests[request_id];
        uint16_t g;
        for (g = 0; g < req->num_exclusion_groups; g++) {
            uint32_t gid = req->exclusion_group_ids[g];
            sol->route_exclusion_counts[(size_t)vehicle_id * ctx->num_exclusion_groups + gid]++;
        }
    }

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

    /* Shift route_request_trip_start alongside route_requests */
    if (sol->route_request_trip_start) {
        uint8_t *ts = sol->route_request_trip_start + (size_t)vehicle_id * sol->route_stride;
        if (insert_req_pos < old_len) {
            memmove(&ts[insert_req_pos + 1], &ts[insert_req_pos],
                    (size_t)(old_len - insert_req_pos) * sizeof(uint8_t));
        }
        ts[insert_req_pos] = 0;
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

    /* Update commodity tracking */
    if (sol->route_commodities) {
        uint32_t cid = ctx->requests[request_id].commodity_id;
        if (cid > 0) {
            sol->route_commodities[vehicle_id] |= (1ULL << (cid - 1));
        }
    }
    /* Update exclusion tracking */
    if (sol->route_exclusion_counts) {
        const SGRequestRecord *req = &ctx->requests[request_id];
        uint16_t g;
        for (g = 0; g < req->num_exclusion_groups; g++) {
            uint32_t gid = req->exclusion_group_ids[g];
            sol->route_exclusion_counts[(size_t)vehicle_id * ctx->num_exclusion_groups + gid]++;
        }
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

    /* Shift route_request_trip_start alongside route_requests */
    if (sol->route_request_trip_start) {
        uint8_t *ts = sol->route_request_trip_start + (size_t)vehicle_id * sol->route_stride;
        /* If this request starts a trip, transfer to next request */
        if (ts[pos] && pos + 1 < old_len) {
            ts[pos + 1] = 1;
        }
        if (pos < old_len - 1) {
            memmove(&ts[pos], &ts[pos + 1], (size_t)(old_len - pos - 1) * sizeof(uint8_t));
        }
        ts[old_len - 1] = 0;
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

    /* Recompute commodity tracking from scratch for this vehicle */
    if (sol->route_commodities) {
        uint64_t bits = 0;
        const uint32_t *cur_route = sg_route_vehicle_ptr_const(sol, vehicle_id);
        uint32_t cur_len = sol->route_lengths[vehicle_id];
        uint32_t ri;
        for (ri = 0; ri < cur_len; ri++) {
            uint32_t cid = ctx->requests[cur_route[ri]].commodity_id;
            if (cid > 0) {
                bits |= (1ULL << (cid - 1));
            }
        }
        sol->route_commodities[vehicle_id] = bits;
    }
    /* Decrement exclusion counts */
    if (sol->route_exclusion_counts) {
        const SGRequestRecord *req = &ctx->requests[request_id];
        uint16_t g;
        for (g = 0; g < req->num_exclusion_groups; g++) {
            uint32_t gid = req->exclusion_group_ids[g];
            size_t idx = (size_t)vehicle_id * ctx->num_exclusion_groups + gid;
            if (sol->route_exclusion_counts[idx] > 0) {
                sol->route_exclusion_counts[idx]--;
            }
        }
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
