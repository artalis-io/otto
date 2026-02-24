/*
 * Plan Validation — compute ETAs + check constraints on a fixed route
 *
 * Given a set of routes (vehicle_id + ordered task_ids), builds an
 * SGRouteSolution, runs timing/load updates, then collects violations.
 * After sg_validate_plan_impl() returns, all existing solution export
 * functions work via ctx->final_solution.
 */

#include "sg_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Helpers
 * ============================================================================ */

static int push_violation(SGContext *ctx, const SGViolation *v) {
    if (ctx->num_violations >= ctx->violations_capacity) {
        uint32_t new_cap = ctx->violations_capacity == 0 ? 16 : ctx->violations_capacity * 2;
        SGViolation *new_arr = (SGViolation *)realloc(ctx->violations,
                                                       (size_t)new_cap * sizeof(SGViolation));
        if (!new_arr) return 0;
        ctx->violations = new_arr;
        ctx->violations_capacity = new_cap;
    }
    ctx->violations[ctx->num_violations++] = *v;
    return 1;
}

/* Build reverse map: task_id → request_id. Returns heap array [num_tasks]. */
static uint32_t *build_task_to_request_map(const SGContext *ctx) {
    uint32_t *map;
    uint32_t r;

    map = (uint32_t *)malloc((size_t)ctx->num_tasks * sizeof(uint32_t));
    if (!map) return NULL;

    for (r = 0; r < ctx->num_tasks; r++) {
        map[r] = UINT32_MAX;
    }

    for (r = 0; r < ctx->num_requests; r++) {
        const SGRequestRecord *req = &ctx->requests[r];
        if (req->has_pickup_task && req->pickup_task_id < ctx->num_tasks) {
            map[req->pickup_task_id] = r;
        }
        if (req->has_delivery_task && req->delivery_task_id < ctx->num_tasks) {
            map[req->delivery_task_id] = r;
        }
    }

    return map;
}

/* ============================================================================
 * Violation Collection
 * ============================================================================ */

static void collect_violations(SGContext *ctx, const SGRouteSolution *sol) {
    uint32_t v, i, r, d;
    SGViolation viol;
    uint32_t dim_count = ctx->dimension_count;

    for (v = 0; v < sol->num_vehicles; v++) {
        uint32_t stop_len = sol->route_stop_lengths[v];
        const SGRouteStop *stops = sg_route_vehicle_stop_ptr_const(sol, v);
        const SGVehicleRecord *veh = &ctx->vehicles[v];

        if (stop_len == 0) continue;

        /* Per-stop checks */
        for (i = 0; i < stop_len; i++) {
            const SGRouteStop *s = &stops[i];
            uint32_t task_id = s->task_id;
            const SGTaskRecord *task = &ctx->tasks[task_id];

            /* Hard TW violation */
            if (s->service_start > (double)task->tw_late + 1e-9) {
                memset(&viol, 0, sizeof(viol));
                viol.type = SG_VIOLATION_HARD_TW;
                viol.vehicle_id = v;
                viol.stop_index = i;
                viol.request_id = s->request_id;
                viol.task_id = task_id;
                viol.actual = s->service_start;
                viol.limit = (double)task->tw_late;
                push_violation(ctx, &viol);
            }

            /* Capacity violation (check after this stop) */
            if (sol->route_stop_load && dim_count > 0) {
                size_t base_offset = (size_t)v * ((size_t)sol->stop_stride + 1U) *
                                     (size_t)dim_count;
                const double *load = sol->route_stop_load + base_offset;
                for (d = 0; d < dim_count; d++) {
                    double load_after = load[((size_t)i + 1U) * dim_count + d];
                    double cap = (veh->has_capacity && veh->capacity)
                                 ? veh->capacity[d] : 0.0;
                    if (cap > 0.0 && load_after > cap + 1e-9) {
                        memset(&viol, 0, sizeof(viol));
                        viol.type = SG_VIOLATION_CAPACITY;
                        viol.vehicle_id = v;
                        viol.stop_index = i;
                        viol.request_id = s->request_id;
                        viol.task_id = task_id;
                        viol.actual = load_after;
                        viol.limit = cap;
                        push_violation(ctx, &viol);
                    }
                }
            }
        }

        /* Compartment capacity violation */
        if (ctx->has_compartments && veh->num_compartments > 0 && dim_count > 0) {
            double comp_load[SG_MAX_COMPARTMENTS_PER_VEHICLE * 6];
            memset(comp_load, 0, sizeof(comp_load));
            for (i = 0; i < stop_len; i++) {
                const SGRouteStop *s = &stops[i];
                const SGTaskRecord *task = &ctx->tasks[s->task_id];
                uint32_t ct = ctx->requests[s->request_id].compartment_type;
                if (ct == 0) continue;
                {
                    int ci = sg_vehicle_find_compartment(veh, ct);
                    if (ci < 0) {
                        memset(&viol, 0, sizeof(viol));
                        viol.type = SG_VIOLATION_COMPARTMENT_CAPACITY;
                        viol.vehicle_id = v;
                        viol.stop_index = i;
                        viol.request_id = s->request_id;
                        viol.task_id = s->task_id;
                        viol.actual = 0.0;
                        viol.limit = 0.0;
                        push_violation(ctx, &viol);
                        continue;
                    }
                    for (d = 0; d < dim_count; d++) {
                        double demand = (task->has_demand && task->demand) ? task->demand[d] : 0.0;
                        comp_load[(size_t)ci * dim_count + d] += demand;
                        {
                            double cap = veh->compartments[ci].capacity
                                         ? veh->compartments[ci].capacity[d] : 0.0;
                            if (cap > 0.0 &&
                                comp_load[(size_t)ci * dim_count + d] > cap + 1e-9) {
                                memset(&viol, 0, sizeof(viol));
                                viol.type = SG_VIOLATION_COMPARTMENT_CAPACITY;
                                viol.vehicle_id = v;
                                viol.stop_index = i;
                                viol.request_id = s->request_id;
                                viol.task_id = s->task_id;
                                viol.actual = comp_load[(size_t)ci * dim_count + d];
                                viol.limit = cap;
                                push_violation(ctx, &viol);
                            }
                        }
                    }
                }
            }
        }

        /* Forbidden vehicle / qualification checks (per request on this route) */
        {
            uint8_t *seen_request = (uint8_t *)calloc((size_t)ctx->num_requests, sizeof(uint8_t));
            if (seen_request) {
                for (i = 0; i < stop_len; i++) {
                    uint32_t req_id = stops[i].request_id;
                    if (req_id < ctx->num_requests && !seen_request[req_id]) {
                        seen_request[req_id] = 1;

                        /* Forbidden vehicle */
                        if (!sg_vehicle_allowed_for_request(ctx, v, req_id)) {
                            memset(&viol, 0, sizeof(viol));
                            viol.type = SG_VIOLATION_FORBIDDEN_VEHICLE;
                            viol.vehicle_id = v;
                            viol.stop_index = UINT32_MAX;
                            viol.request_id = req_id;
                            viol.task_id = UINT32_MAX;
                            viol.actual = 0.0;
                            viol.limit = 0.0;
                            push_violation(ctx, &viol);
                        }

                        /* Qualification */
                        if (!sg_vehicle_qualifies(ctx, v, req_id)) {
                            memset(&viol, 0, sizeof(viol));
                            viol.type = SG_VIOLATION_QUALIFICATION;
                            viol.vehicle_id = v;
                            viol.stop_index = UINT32_MAX;
                            viol.request_id = req_id;
                            viol.task_id = UINT32_MAX;
                            viol.actual = 0.0;
                            viol.limit = 0.0;
                            push_violation(ctx, &viol);
                        }
                    }
                }
                free(seen_request);
            }
        }

        /* PD order check: delivery before its pickup */
        for (r = 0; r < ctx->num_requests; r++) {
            const SGRequestRecord *req = &ctx->requests[r];
            if (req->kind != SG_REQUEST_KIND_PICKUP_DELIVERY) continue;
            if (sol->request_vehicle[r] != v) continue;
            if (sol->request_pickup_stop_pos[r] >= sol->request_delivery_stop_pos[r]) {
                memset(&viol, 0, sizeof(viol));
                viol.type = SG_VIOLATION_PD_ORDER;
                viol.vehicle_id = v;
                viol.stop_index = sol->request_delivery_stop_pos[r];
                viol.request_id = r;
                viol.task_id = UINT32_MAX;
                viol.actual = (double)sol->request_pickup_stop_pos[r];
                viol.limit = (double)sol->request_delivery_stop_pos[r];
                push_violation(ctx, &viol);
            }
        }

        /* PD policy (LIFO/FIFO) check */
        if (ctx->has_pd_policy && veh->pd_policy != SG_PD_POLICY_NONE) {
            /* Use a stack/queue to verify ordering of PD pairs */
            uint32_t *pd_buf = (uint32_t *)malloc((size_t)stop_len * sizeof(uint32_t));
            if (pd_buf) {
                uint32_t top = 0, front = 0;
                for (i = 0; i < stop_len; i++) {
                    const SGRouteStop *s = &stops[i];
                    const SGRequestRecord *req = &ctx->requests[s->request_id];
                    if (req->kind != SG_REQUEST_KIND_PICKUP_DELIVERY) continue;
                    if (s->is_pickup) {
                        pd_buf[top++] = s->request_id;
                    } else {
                        int violation = 0;
                        if (veh->pd_policy == SG_PD_POLICY_LIFO) {
                            violation = (top == 0 || pd_buf[top - 1] != s->request_id);
                            if (!violation) top--;
                        } else { /* FIFO */
                            violation = (front >= top || pd_buf[front] != s->request_id);
                            if (!violation) front++;
                        }
                        if (violation) {
                            memset(&viol, 0, sizeof(viol));
                            viol.type = SG_VIOLATION_PD_POLICY;
                            viol.vehicle_id = v;
                            viol.stop_index = i;
                            viol.request_id = s->request_id;
                            viol.task_id = s->task_id;
                            viol.actual = (double)veh->pd_policy;
                            viol.limit = 0.0;
                            push_violation(ctx, &viol);
                        }
                    }
                }
                free(pd_buf);
            }
        }

        /* Backhaul check: no D-only stop after PD pickup */
        if (ctx->has_backhaul && veh->backhaul) {
            uint8_t saw_pd_pickup = 0;
            for (i = 0; i < stop_len; i++) {
                const SGRouteStop *s = &stops[i];
                const SGRequestRecord *req = &ctx->requests[s->request_id];
                if (s->is_pickup && req->kind == SG_REQUEST_KIND_PICKUP_DELIVERY) {
                    saw_pd_pickup = 1;
                } else if (!s->is_pickup && req->kind == SG_REQUEST_KIND_DELIVERY_ONLY) {
                    if (saw_pd_pickup) {
                        memset(&viol, 0, sizeof(viol));
                        viol.type = SG_VIOLATION_BACKHAUL;
                        viol.vehicle_id = v;
                        viol.stop_index = i;
                        viol.request_id = s->request_id;
                        viol.task_id = s->task_id;
                        viol.actual = 0.0;
                        viol.limit = 0.0;
                        push_violation(ctx, &viol);
                    }
                }
            }
        }

        /* Ride time check */
        for (r = 0; r < ctx->num_requests; r++) {
            const SGRequestRecord *req = &ctx->requests[r];
            if (req->kind != SG_REQUEST_KIND_PICKUP_DELIVERY) continue;
            if (!req->has_max_ride_time) continue;
            if (sol->request_vehicle[r] != v) continue;
            {
                uint32_t pp = sol->request_pickup_stop_pos[r];
                uint32_t dp = sol->request_delivery_stop_pos[r];
                if (pp < stop_len && dp < stop_len && pp < dp) {
                    double ride = stops[dp].arrival - stops[pp].depart;
                    double max_ride = (double)req->max_ride_time_seconds;
                    if (ride > max_ride + 1e-9) {
                        memset(&viol, 0, sizeof(viol));
                        viol.type = SG_VIOLATION_RIDE_TIME;
                        viol.vehicle_id = v;
                        viol.stop_index = dp;
                        viol.request_id = r;
                        viol.task_id = UINT32_MAX;
                        viol.actual = ride;
                        viol.limit = max_ride;
                        push_violation(ctx, &viol);
                    }
                }
            }
        }

        /* Route-level: max duration */
        if (veh->max_duration_seconds > 0 && sol->route_duration) {
            double dur = sol->route_duration[v];
            double max_dur = (double)veh->max_duration_seconds;
            if (dur > max_dur + 1e-9) {
                memset(&viol, 0, sizeof(viol));
                viol.type = SG_VIOLATION_MAX_DURATION;
                viol.vehicle_id = v;
                viol.stop_index = UINT32_MAX;
                viol.request_id = UINT32_MAX;
                viol.task_id = UINT32_MAX;
                viol.actual = dur;
                viol.limit = max_dur;
                push_violation(ctx, &viol);
            }
        }

        /* Route-level: max distance */
        if (veh->max_distance > 0.0) {
            double dist = sol->route_distance[v];
            if (dist > veh->max_distance + 1e-9) {
                memset(&viol, 0, sizeof(viol));
                viol.type = SG_VIOLATION_MAX_DISTANCE;
                viol.vehicle_id = v;
                viol.stop_index = UINT32_MAX;
                viol.request_id = UINT32_MAX;
                viol.task_id = UINT32_MAX;
                viol.actual = dist;
                viol.limit = veh->max_distance;
                push_violation(ctx, &viol);
            }
        }

        /* Route-level: max tasks */
        if (veh->max_tasks > 0 && stop_len > veh->max_tasks) {
            memset(&viol, 0, sizeof(viol));
            viol.type = SG_VIOLATION_MAX_TASKS;
            viol.vehicle_id = v;
            viol.stop_index = UINT32_MAX;
            viol.request_id = UINT32_MAX;
            viol.task_id = UINT32_MAX;
            viol.actual = (double)stop_len;
            viol.limit = (double)veh->max_tasks;
            push_violation(ctx, &viol);
        }
    }
}

/* ============================================================================
 * Solution Building
 * ============================================================================ */

SGStatus sg_validate_plan_impl(SGContext *ctx, uint32_t num_routes,
                               const SGPlanRoute *routes) {
    SGRouteSolution *sol = NULL;
    uint32_t *task_to_request = NULL;
    uint8_t *task_assigned = NULL;
    SGViolation viol;
    uint32_t ri, si, v;
    double total_distance = 0.0;
    uint32_t vehicles_used = 0;

    if (!ctx || (num_routes > 0 && !routes)) {
        return SG_STATUS_INVALID_ARG;
    }

    /* Clear previous violations */
    ctx->num_violations = 0;

    /* Prepare travel infrastructure */
    {
        SGStatus prep = sg_prepare_travel(ctx);
        if (prep != SG_STATUS_OK) {
            sg_set_error(ctx, "failed to prepare travel data");
            return SG_STATUS_ERROR;
        }
    }

    /* Build task→request map */
    task_to_request = build_task_to_request_map(ctx);
    if (!task_to_request && ctx->num_tasks > 0) {
        sg_set_error(ctx, "out of memory for task map");
        return SG_STATUS_OUT_OF_MEMORY;
    }

    /* Track which tasks have been assigned (duplicate check) */
    task_assigned = (uint8_t *)calloc((size_t)ctx->num_tasks, sizeof(uint8_t));
    if (!task_assigned && ctx->num_tasks > 0) {
        free(task_to_request);
        sg_set_error(ctx, "out of memory");
        return SG_STATUS_OUT_OF_MEMORY;
    }

    /* Input validation: check task IDs, vehicle IDs, duplicates */
    for (ri = 0; ri < num_routes; ri++) {
        const SGPlanRoute *pr = &routes[ri];
        if (pr->vehicle_id >= ctx->num_vehicles) {
            free(task_to_request);
            free(task_assigned);
            sg_set_error(ctx, "plan route %u: vehicle_id %u out of range",
                         ri, pr->vehicle_id);
            return SG_STATUS_INVALID_ARG;
        }
        for (si = 0; si < pr->task_count; si++) {
            uint32_t tid = pr->task_ids[si];
            if (tid >= ctx->num_tasks) {
                memset(&viol, 0, sizeof(viol));
                viol.type = SG_VIOLATION_UNKNOWN_TASK;
                viol.vehicle_id = pr->vehicle_id;
                viol.stop_index = si;
                viol.request_id = UINT32_MAX;
                viol.task_id = tid;
                push_violation(ctx, &viol);
                continue;
            }
            if (task_to_request[tid] == UINT32_MAX) {
                memset(&viol, 0, sizeof(viol));
                viol.type = SG_VIOLATION_UNKNOWN_TASK;
                viol.vehicle_id = pr->vehicle_id;
                viol.stop_index = si;
                viol.request_id = UINT32_MAX;
                viol.task_id = tid;
                push_violation(ctx, &viol);
                continue;
            }
            if (task_assigned[tid]) {
                memset(&viol, 0, sizeof(viol));
                viol.type = SG_VIOLATION_DUPLICATE_TASK;
                viol.vehicle_id = pr->vehicle_id;
                viol.stop_index = si;
                viol.request_id = task_to_request[tid];
                viol.task_id = tid;
                push_violation(ctx, &viol);
                continue;
            }
            task_assigned[tid] = 1;
        }
    }

    /* Allocate solution */
    sol = (SGRouteSolution *)calloc(1, sizeof(SGRouteSolution));
    if (!sol) {
        free(task_to_request);
        free(task_assigned);
        sg_set_error(ctx, "out of memory");
        return SG_STATUS_OUT_OF_MEMORY;
    }

    {
        ARStatus init_status = sg_route_solution_init(ctx, sol);
        if (init_status != AR_STATUS_OK) {
            free(sol);
            free(task_to_request);
            free(task_assigned);
            sg_set_error(ctx, "failed to init solution");
            return SG_STATUS_OUT_OF_MEMORY;
        }
    }

    /* Populate stops from plan routes */
    for (ri = 0; ri < num_routes; ri++) {
        const SGPlanRoute *pr = &routes[ri];
        uint32_t vid = pr->vehicle_id;
        SGRouteStop *stops = sg_route_vehicle_stop_ptr(sol, vid);
        uint32_t *prev_arr = sg_route_vehicle_stop_prev_ptr(sol, vid);
        uint32_t *next_arr = sg_route_vehicle_stop_next_ptr(sol, vid);
        uint32_t stop_len = 0;
        uint32_t req_len = 0;
        uint32_t *route_reqs = sg_route_vehicle_ptr(sol, vid);

        for (si = 0; si < pr->task_count; si++) {
            uint32_t tid = pr->task_ids[si];
            uint32_t req_id;
            const SGTaskRecord *task;
            uint8_t is_pickup;

            /* Skip tasks we flagged as invalid */
            if (tid >= ctx->num_tasks || task_to_request[tid] == UINT32_MAX ||
                !task_assigned[tid]) {
                continue;
            }

            req_id = task_to_request[tid];
            task = &ctx->tasks[tid];
            is_pickup = (task->type == SG_TASK_PICKUP) ? 1 : 0;

            if (stop_len >= sol->stop_stride) break;

            memset(&stops[stop_len], 0, sizeof(SGRouteStop));
            stops[stop_len].request_id = req_id;
            stops[stop_len].task_id = tid;
            stops[stop_len].is_pickup = is_pickup;

            prev_arr[stop_len] = stop_len > 0 ? stop_len - 1U : UINT32_MAX;
            next_arr[stop_len] = UINT32_MAX;
            if (stop_len > 0) {
                next_arr[stop_len - 1U] = stop_len;
            }

            /* Track stop positions */
            if (is_pickup) {
                sol->request_pickup_stop_pos[req_id] = stop_len;
            } else {
                sol->request_delivery_stop_pos[req_id] = stop_len;
            }

            sol->request_vehicle[req_id] = vid;

            stop_len++;
        }

        sol->route_stop_lengths[vid] = stop_len;

        /* Build route_requests (one entry per unique request) */
        {
            uint8_t *seen = (uint8_t *)calloc((size_t)ctx->num_requests, sizeof(uint8_t));
            if (seen) {
                for (si = 0; si < stop_len; si++) {
                    uint32_t req_id = stops[si].request_id;
                    if (req_id < ctx->num_requests && !seen[req_id]) {
                        seen[req_id] = 1;
                        if (req_len < sol->route_stride) {
                            route_reqs[req_len] = req_id;
                            sol->request_pos[req_id] = req_len;
                            req_len++;
                        }
                    }
                }
                free(seen);
            }
        }
        sol->route_lengths[vid] = req_len;

        /* Mark requests as assigned */
        for (si = 0; si < stop_len; si++) {
            uint32_t req_id = stops[si].request_id;
            if (req_id < ctx->num_requests && !sol->base.assigned_flags[req_id]) {
                sg_bootstrap_assign_request(&sol->base, req_id);
            }
        }
    }

    /* Clear unused stop slots */
    for (v = 0; v < sol->num_vehicles; v++) {
        SGRouteStop *stops = sg_route_vehicle_stop_ptr(sol, v);
        uint32_t stop_len = sol->route_stop_lengths[v];
        uint32_t s;
        for (s = stop_len; s < sol->stop_stride; s++) {
            memset(&stops[s], 0, sizeof(SGRouteStop));
            stops[s].request_id = UINT32_MAX;
            stops[s].task_id = UINT32_MAX;
        }
    }

    /* Run timing and load updates for each vehicle with stops */
    for (v = 0; v < sol->num_vehicles; v++) {
        if (sol->route_stop_lengths[v] > 0) {
            sg_route_update_timing(ctx, sol, v);
            sg_route_update_load(ctx, sol, v);
        }
    }

    /* Compute aggregates */
    for (v = 0; v < sol->num_vehicles; v++) {
        if (sol->route_stop_lengths[v] > 0) {
            vehicles_used++;
            total_distance += sol->route_distance[v];
        }
    }
    sol->vehicles_used = vehicles_used;
    sol->total_distance = total_distance;

    /* Collect constraint violations */
    collect_violations(ctx, sol);

    /* Lock violation checks */
    if (ctx->request_locks) {
        for (ri = 0; ri < ctx->num_requests; ri++) {
            uint8_t lock = ctx->request_locks[ri];
            if (lock == SG_LOCK_FROZEN) {
                /* Check frozen request is on the correct vehicle from initial_routes */
                if (sol->request_vehicle[ri] != SG_NO_VEHICLE) {
                    /* Find which vehicle it was assigned to in initial_routes */
                    uint32_t expected_vehicle = SG_NO_VEHICLE;
                    uint32_t ir_offset = 0, ir;
                    for (ir = 0; ir < ctx->num_initial_routes; ir++) {
                        uint32_t j;
                        for (j = 0; j < ctx->initial_route_lengths[ir]; j++) {
                            if (ctx->initial_route_request_ids[ir_offset + j] == ri) {
                                expected_vehicle = ctx->initial_route_vehicle_ids[ir];
                                break;
                            }
                        }
                        ir_offset += ctx->initial_route_lengths[ir];
                        if (expected_vehicle != SG_NO_VEHICLE) break;
                    }
                    if (expected_vehicle != SG_NO_VEHICLE &&
                        sol->request_vehicle[ri] != expected_vehicle) {
                        SGViolation viol;
                        memset(&viol, 0, sizeof(viol));
                        viol.type = SG_VIOLATION_FROZEN_ASSIGNMENT;
                        viol.vehicle_id = sol->request_vehicle[ri];
                        viol.request_id = ri;
                        viol.actual = (double)sol->request_vehicle[ri];
                        viol.limit = (double)expected_vehicle;
                        push_violation(ctx, &viol);
                    }
                }
            }
            if (lock >= SG_LOCK_COMMITTED) {
                /* Check committed/frozen request is assigned */
                if (!sol->base.assigned_flags[ri]) {
                    SGViolation viol;
                    memset(&viol, 0, sizeof(viol));
                    viol.type = SG_VIOLATION_COMMITTED_UNASSIGNED;
                    viol.vehicle_id = UINT32_MAX;
                    viol.request_id = ri;
                    push_violation(ctx, &viol);
                }
            }
        }
    }

    /* Store as ctx->final_solution */
    if (ctx->final_solution) {
        sg_route_solution_free(ctx->final_solution, NULL);
    }
    ctx->final_solution = sol;

    /* Populate stats */
    ctx->stats.iterations = 0;
    ctx->stats.unassigned = sol->base.num_unassigned;
    ctx->stats.vehicles_used = vehicles_used;
    ctx->stats.total_distance = total_distance;
    ctx->stats.total_cost = sg_route_solution_cost(sol, ctx);
    {
        double tw = 0.0, tot_ot = 0.0, ttp = 0.0;
        for (v = 0; v < sol->num_vehicles; v++) {
            if (sol->route_waiting) tw += sol->route_waiting[v];
            if (sol->route_overtime) tot_ot += sol->route_overtime[v];
            if (sol->route_tw_penalty) ttp += sol->route_tw_penalty[v];
        }
        ctx->stats.total_waiting = tw;
        ctx->stats.total_overtime = tot_ot;
        ctx->stats.total_tw_penalty = ttp;
    }

    free(task_to_request);
    free(task_assigned);

    return SG_STATUS_OK;
}
