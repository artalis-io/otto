#include "sg_internal.h"

/* Apply tune parameter overrides to ALNS params (rewards, reaction, segment_size) */
static void sg_apply_tune_to_alns(const SGContext *ctx, ARALNSParams *params) {
    if (!ctx->tune_params) return;
    const SGTuneParams *tp = ctx->tune_params;
    if (tp->reaction_factor != SG_TUNE_SENTINEL_D) params->reaction_factor = tp->reaction_factor;
    if (tp->reward_best != SG_TUNE_SENTINEL_D) params->reward_best = tp->reward_best;
    if (tp->reward_better != SG_TUNE_SENTINEL_D) params->reward_better = tp->reward_better;
    if (tp->reward_accepted != SG_TUNE_SENTINEL_D) params->reward_accepted = tp->reward_accepted;
    if (tp->segment_size != SG_TUNE_SENTINEL_I) params->segment_size = tp->segment_size;
}

/* Write a convergence entry to ring buffer and/or fire callback */
static void sg_record_convergence(SGContext *ctx, const SGConvergenceEntry *entry) {
    if (ctx->convergence_buffer) {
        ctx->convergence_buffer[ctx->convergence_write_pos] = *entry;
        ctx->convergence_write_pos = (ctx->convergence_write_pos + 1) % ctx->convergence_capacity;
        ctx->convergence_count++;
    }
    if (ctx->convergence_callback) {
        ctx->convergence_callback(entry, ctx->convergence_callback_data);
    }
}

/* Record start of a phase */
static void sg_phase_start(SGContext *ctx, SGSolvePhase phase,
                           double cost, uint32_t vehicles, uint32_t unassigned) {
    ctx->current_phase = phase;
    if (ctx->num_phase_stats < 5) {
        SGPhaseStats *ps = &ctx->phase_stats[ctx->num_phase_stats];
        memset(ps, 0, sizeof(*ps));
        ps->phase = phase;
        ps->start_cost = cost;
        ps->start_vehicles = vehicles;
        ps->start_unassigned = unassigned;
        ps->elapsed_seconds = sg_monotonic_seconds() - ctx->solve_start_time;
    }
}

/* Record end of a phase */
static void sg_phase_end(SGContext *ctx, int64_t iterations,
                         double cost, uint32_t vehicles, uint32_t unassigned) {
    if (ctx->num_phase_stats < 5) {
        SGPhaseStats *ps = &ctx->phase_stats[ctx->num_phase_stats];
        ps->iterations = iterations;
        ps->end_cost = cost;
        ps->end_vehicles = vehicles;
        ps->end_unassigned = unassigned;
        ps->elapsed_seconds = (sg_monotonic_seconds() - ctx->solve_start_time) - ps->elapsed_seconds;
        ctx->num_phase_stats++;
    }
}

/* Progress callback forwarder: Arbor → Surge */
static int sg_progress_forwarder(int64_t iteration, double best_cost,
                                  double elapsed_seconds, void *user_data) {
    SGContext *ctx = (SGContext *)user_data;
    if (ctx->cancel_requested) return 1;
    /* Adaptive penalty self-adjustment at segment boundaries */
    if (ctx->penalty.enabled && ctx->penalty.update) {
        ctx->penalty.update(&ctx->penalty);
        /* Invalidate insertion cache: penalty weights changed → cached scores stale */
        if (ctx->scratch.insertion_cache)
            ctx->scratch.insertion_cache->penalty_gen++;
    }

    /* Build enriched progress stats */
    {
        SGStats snap;
        memset(&snap, 0, sizeof(snap));
        snap.iterations = iteration;
        snap.total_cost = best_cost;
        snap.elapsed_seconds = elapsed_seconds;
        snap.phase = ctx->current_phase;

        /* Record convergence sample at segment boundary */
        if (ctx->convergence_buffer || ctx->convergence_callback) {
            SGConvergenceEntry entry;
            entry.iteration = iteration;
            entry.cost = best_cost;
            entry.elapsed_seconds = elapsed_seconds;
            entry.vehicles_used = 0;
            entry.unassigned = 0;
            entry.total_distance = 0.0;
            entry.phase = ctx->current_phase;
            entry.is_new_best = 0;
            sg_record_convergence(ctx, &entry);
        }

        if (ctx->progress_callback) {
            if (ctx->progress_callback(&snap, ctx->progress_callback_data)) {
                ctx->cancel_requested = 1;
                return 1;
            }
        }
    }
    return 0;
}

/* Instrumented is_better wrapper: records convergence on improvement */
static int sg_instrumented_is_better(const void *candidate, const void *current_best,
                                      void *user_ctx) {
    SGContext *ctx = (SGContext *)user_ctx;
    int better = sg_route_solution_is_better(candidate, current_best, user_ctx);
    if (better && (ctx->convergence_buffer || ctx->convergence_callback)) {
        const SGRouteSolution *sol = (const SGRouteSolution *)candidate;
        SGConvergenceEntry entry;
        entry.iteration = 0;  /* approximate — arbor doesn't expose iter in is_better */
        entry.cost = sg_route_solution_cost(candidate, user_ctx);
        entry.elapsed_seconds = sg_monotonic_seconds() - ctx->solve_start_time;
        entry.vehicles_used = sol->vehicles_used;
        entry.unassigned = sol->base.num_unassigned;
        entry.total_distance = sol->total_distance;
        entry.phase = ctx->current_phase;
        entry.is_new_best = 1;
        sg_record_convergence(ctx, &entry);
    }
    return better;
}

static void sg_copy_operator_stats(SGContext *ctx, const ARALNSContext *alns) {
    int nd = ar_alns_destroy_count(alns);
    int nr = ar_alns_repair_count(alns);
    int i;

    if (nd > 0) {
        /* Aggregate: find matching name or append */
        for (i = 0; i < nd; i++) {
            ARALNSOperatorStats as;
            uint32_t j;
            int found = 0;
            if (ar_alns_get_destroy_stats(alns, i, &as) != AR_STATUS_OK) continue;
            for (j = 0; j < ctx->num_destroy_ops; j++) {
                if (strcmp(ctx->destroy_op_stats[j].name, as.name) == 0) {
                    ctx->destroy_op_stats[j].selected += as.selected;
                    ctx->destroy_op_stats[j].accepted += as.accepted;
                    ctx->destroy_op_stats[j].improvements += as.improvements;
                    ctx->destroy_op_stats[j].total_seconds += as.total_seconds;
                    ctx->destroy_op_stats[j].weight = as.weight;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                SGOperatorStats *new_arr = (SGOperatorStats *)realloc(
                    ctx->destroy_op_stats,
                    (size_t)(ctx->num_destroy_ops + 1) * sizeof(SGOperatorStats));
                if (new_arr) {
                    SGOperatorStats *s = &new_arr[ctx->num_destroy_ops];
                    memcpy(s->name, as.name, 32);
                    s->weight = as.weight;
                    s->selected = as.selected;
                    s->accepted = as.accepted;
                    s->improvements = as.improvements;
                    s->total_seconds = as.total_seconds;
                    ctx->destroy_op_stats = new_arr;
                    ctx->num_destroy_ops++;
                }
            }
        }
    }

    if (nr > 0) {
        for (i = 0; i < nr; i++) {
            ARALNSOperatorStats as;
            uint32_t j;
            int found = 0;
            if (ar_alns_get_repair_stats(alns, i, &as) != AR_STATUS_OK) continue;
            for (j = 0; j < ctx->num_repair_ops; j++) {
                if (strcmp(ctx->repair_op_stats[j].name, as.name) == 0) {
                    ctx->repair_op_stats[j].selected += as.selected;
                    ctx->repair_op_stats[j].accepted += as.accepted;
                    ctx->repair_op_stats[j].improvements += as.improvements;
                    ctx->repair_op_stats[j].total_seconds += as.total_seconds;
                    ctx->repair_op_stats[j].weight = as.weight;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                SGOperatorStats *new_arr = (SGOperatorStats *)realloc(
                    ctx->repair_op_stats,
                    (size_t)(ctx->num_repair_ops + 1) * sizeof(SGOperatorStats));
                if (new_arr) {
                    SGOperatorStats *s = &new_arr[ctx->num_repair_ops];
                    memcpy(s->name, as.name, 32);
                    s->weight = as.weight;
                    s->selected = as.selected;
                    s->accepted = as.accepted;
                    s->improvements = as.improvements;
                    s->total_seconds = as.total_seconds;
                    ctx->repair_op_stats = new_arr;
                    ctx->num_repair_ops++;
                }
            }
        }
    }
}

/* Compute representative route-level cost scale from initial solution.
 * Uses median of per-route objective costs (fixed + variable components)
 * for robustness against skewed vehicle cost distributions.
 *
 * Invariant: penalties must remain on the same order of magnitude as the
 * instance's objective scale. If penalty magnitudes are too small, infeasible
 * solutions dominate. If too large, search dynamics freeze and selection
 * pressure becomes numerically distorted. */
static double sg_compute_cost_scale(const SGContext *ctx,
                                    const SGRouteSolution *sol)
{
    double *costs = ctx->scratch.cost_scale_buf;
    int heap = 0;
    uint32_t n = 0, v;

    if (!costs) {
        costs = (double *)malloc((size_t)sol->num_vehicles * sizeof(double));
        if (!costs) return 1.0;
        heap = 1;
    }

    for (v = 0; v < sol->num_vehicles; v++) {
        if (sol->route_stop_lengths[v] == 0) continue;
        const SGVehicleRecord *veh = &ctx->vehicles[v];
        double rc = veh->fixed_cost;
        rc += veh->cost_per_distance * sol->route_distance[v];
        if (sol->route_duration)
            rc += veh->cost_per_duration * sol->route_duration[v];
        if (sol->route_waiting)
            rc += veh->cost_per_waiting * sol->route_waiting[v];
        if (sol->route_overtime)
            rc += veh->cost_per_overtime * sol->route_overtime[v];
        if (rc > 1e-9)
            costs[n++] = rc;
    }

    if (n == 0) { if (heap) free(costs); return 1.0; }

    /* Insertion sort for median (n is small, typically < 100) */
    for (uint32_t i = 1; i < n; i++) {
        double key = costs[i];
        uint32_t j = i;
        while (j > 0 && costs[j - 1] > key) {
            costs[j] = costs[j - 1];
            j--;
        }
        costs[j] = key;
    }

    double median = (n % 2 == 1) ? costs[n / 2]
                                  : (costs[n / 2 - 1] + costs[n / 2]) / 2.0;
    if (heap) free(costs);

    /* Clamp to safe numeric range */
    if (median != median || median <= 0.0) return 1.0;  /* NaN or non-positive */
    if (median < 1.0) median = 1.0;
    if (median > 1e12) median = 1e12;
    return median;
}

void sg_adaptive_q_bounds(int num_requests, int config_q_min, int config_q_max,
                          int *q_min_out, int *q_max_out) {
    int adaptive_min = num_requests / 20;
    int adaptive_max = num_requests / 4;
    *q_min_out = config_q_min > adaptive_min ? config_q_min : adaptive_min;
    *q_max_out = config_q_max > adaptive_max ? config_q_max : adaptive_max;
    if (*q_min_out > *q_max_out) {
        *q_min_out = *q_max_out;
    }
}

int sg_route_solver_eligible(const SGContext *ctx) {
    uint32_t i;

    if (!ctx || ctx->num_requests == 0 || ctx->num_vehicles == 0) {
        return 0;
    }

    for (i = 0; i < ctx->num_requests; i++) {
        const SGRequestRecord *request = &ctx->requests[i];
        if (request->kind == SG_REQUEST_KIND_DELIVERY_ONLY) {
            if (!request->has_delivery_task || request->delivery_task_id >= ctx->num_tasks ||
                (ctx->tasks[request->delivery_task_id].type != SG_TASK_DELIVERY &&
                 ctx->tasks[request->delivery_task_id].type != SG_TASK_SERVICE)) {
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

ARStatus sg_route_construct_tw_sorted(SGContext *ctx, SGRouteSolution *sol) {
    uint32_t *order = NULL;
    uint32_t n, i;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }

    n = sol->base.num_unassigned;
    if (n == 0) {
        return AR_STATUS_OK;
    }

    order = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    if (!order) {
        return AR_STATUS_OUT_OF_MEMORY;
    }
    memcpy(order, sol->base.unassigned_ids, (size_t)n * sizeof(uint32_t));

    /* Insertion sort by (tw_width asc, tw_early asc, request_id asc).
       For LC1 instances (all TWs width 55), secondary sort by tw_early
       creates a temporal sweep building routes in natural progression. */
    for (i = 1; i < n; i++) {
        uint32_t key = order[i];
        int32_t key_width = INT32_MAX;
        int32_t key_early = INT32_MAX;
        int32_t key_late = 0;
        uint32_t j = i;
        (void)sg_request_tw_width(ctx, key, &key_width);
        (void)sg_request_time_window_bounds(ctx, key, &key_early, &key_late);

        while (j > 0) {
            uint32_t prev = order[j - 1];
            int32_t prev_width = INT32_MAX;
            int32_t prev_early = INT32_MAX;
            int32_t prev_late = 0;
            (void)sg_request_tw_width(ctx, prev, &prev_width);
            (void)sg_request_time_window_bounds(ctx, prev, &prev_early, &prev_late);
            if (prev_width < key_width ||
                (prev_width == key_width && prev_early < key_early) ||
                (prev_width == key_width && prev_early == key_early && prev <= key)) {
                break;
            }
            order[j] = prev;
            j--;
        }
        order[j] = key;
    }

    /* Insert in sorted order using cheapest-position greedy */
    for (i = 0; i < n; i++) {
        uint32_t request_id = order[i];
        double best_score = 0.0;
        double kth_score = 0.0;
        uint32_t vehicle_id = UINT32_MAX;
        uint32_t pos = UINT32_MAX;
        uint32_t pickup_pos = UINT32_MAX;
        uint32_t delivery_pos = UINT32_MAX;
        double route_distance = 0.0;

        if (!sg_route_rank_insertions_for_request(ctx, sol, request_id, 1, 0.0,
                                                   &best_score, &kth_score,
                                                   &vehicle_id, &pos,
                                                   &pickup_pos, &delivery_pos,
                                                   &route_distance)) {
            continue;
        }
        if (pickup_pos != UINT32_MAX && delivery_pos != UINT32_MAX) {
            if (sg_route_apply_pd_insertion(ctx, sol, request_id, vehicle_id,
                                            pickup_pos, delivery_pos,
                                            route_distance) != AR_STATUS_OK) {
                free(order);
                return AR_STATUS_ERROR;
            }
        } else {
            if (sg_route_apply_insertion(ctx, sol, request_id, vehicle_id, pos,
                                         route_distance) != AR_STATUS_OK) {
                free(order);
                return AR_STATUS_ERROR;
            }
        }
    }

    free(order);
    return AR_STATUS_OK;
}

ARStatus sg_route_construct_solomon_i1(SGContext *ctx, SGRouteSolution *sol) {
    uint32_t n;
    double lambda = 1.0;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }

    n = sol->base.num_unassigned;
    if (n == 0) {
        return AR_STATUS_OK;
    }

    for (;;) {
        uint32_t seed_vehicle = UINT32_MAX;
        uint32_t seed_request = UINT32_MAX;
        int32_t seed_early = INT32_MAX;
        int32_t seed_width = INT32_MAX;
        uint32_t i;

        /* Find next empty vehicle. */
        for (i = 0; i < sol->num_vehicles; i++) {
            if (sol->route_lengths[i] == 0) {
                seed_vehicle = i;
                break;
            }
        }
        if (seed_vehicle == UINT32_MAX) {
            break;  /* No empty vehicles left. */
        }
        if (sol->base.num_unassigned == 0) {
            break;  /* All requests assigned. */
        }

        /* Seed selection: unrouted request with earliest TW. */
        for (i = 0; i < sol->base.num_unassigned; i++) {
            uint32_t rid = sol->base.unassigned_ids[i];
            int32_t r_early = INT32_MAX, r_late = 0;
            int32_t r_width = INT32_MAX;
            (void)sg_request_time_window_bounds(ctx, rid, &r_early, &r_late);
            (void)sg_request_tw_width(ctx, rid, &r_width);
            if (r_early < seed_early ||
                (r_early == seed_early && r_width < seed_width) ||
                (r_early == seed_early && r_width == seed_width && rid < seed_request)) {
                seed_early = r_early;
                seed_width = r_width;
                seed_request = rid;
            }
        }
        if (seed_request == UINT32_MAX) {
            break;
        }

        /* Insert seed into vehicle. */
        {
            int is_pd = (ctx->requests[seed_request].kind == SG_REQUEST_KIND_PICKUP_DELIVERY);
            if (is_pd) {
                double score;
                uint32_t pp = UINT32_MAX, dp = UINT32_MAX;
                double dist = 0.0;
                if (!sg_route_eval_pd_best_insertion_cached(ctx, sol, seed_request,
                                                             seed_vehicle, &score,
                                                             &pp, &dp, &dist)) {
                    break;  /* Seed doesn't fit — can't continue. */
                }
                if (sg_route_apply_pd_insertion(ctx, sol, seed_request, seed_vehicle,
                                                 pp, dp, dist) != AR_STATUS_OK) {
                    break;
                }
            } else {
                double score = 0.0, dist = 0.0;
                if (!sg_route_eval_insertion_cached(ctx, sol, seed_request, seed_vehicle,
                                                     0, &score, &dist)) {
                    break;
                }
                if (sg_route_apply_insertion(ctx, sol, seed_request, seed_vehicle,
                                              0, dist) != AR_STATUS_OK) {
                    break;
                }
            }
        }

        /* Fill loop: repeatedly insert best c2-scoring request into current vehicle. */
        for (;;) {
            uint32_t best_req = UINT32_MAX;
            double best_c2 = -INFINITY;
            uint32_t best_pos = UINT32_MAX;
            uint32_t best_pp = UINT32_MAX, best_dp = UINT32_MAX;
            double best_dist = 0.0;
            uint32_t v_start_loc, v_end_loc;
            uint32_t ui;

            if (sol->base.num_unassigned == 0) {
                break;
            }

            v_start_loc = ctx->vehicles[seed_vehicle].start_location_id;
            v_end_loc = ctx->vehicles[seed_vehicle].end_location_id;

            for (ui = 0; ui < sol->base.num_unassigned; ui++) {
                uint32_t rid = sol->base.unassigned_ids[ui];
                int is_pd = (ctx->requests[rid].kind == SG_REQUEST_KIND_PICKUP_DELIVERY);
                double old_dist = sol->route_distance[seed_vehicle];
                double c1, depot_dist, c2;

                if (is_pd) {
                    double score;
                    uint32_t pp = UINT32_MAX, dp = UINT32_MAX;
                    double new_dist = 0.0;
                    if (!sg_route_eval_pd_best_insertion_cached(ctx, sol, rid,
                                                                 seed_vehicle, &score,
                                                                 &pp, &dp, &new_dist)) {
                        continue;
                    }
                    c1 = new_dist - old_dist;
                    {
                        uint32_t p_loc = ctx->tasks[ctx->requests[rid].pickup_task_id].location_id;
                        uint32_t d_loc = ctx->tasks[ctx->requests[rid].delivery_task_id].location_id;
                        depot_dist = ctx->vehicles[seed_vehicle].open_start
                            ? 0.0
                            : sg_travel_dist(ctx, v_start_loc, p_loc, seed_vehicle);
                        depot_dist += sg_travel_dist(ctx, p_loc, d_loc, seed_vehicle);
                        if (!ctx->vehicles[seed_vehicle].open_end) {
                            depot_dist += sg_travel_dist(ctx, d_loc, v_end_loc, seed_vehicle);
                        }
                    }
                    c2 = lambda * depot_dist - c1;
                    if (c2 > best_c2) {
                        best_c2 = c2;
                        best_req = rid;
                        best_pos = UINT32_MAX;
                        best_pp = pp;
                        best_dp = dp;
                        best_dist = new_dist;
                    }
                } else {
                    uint32_t pos;
                    uint32_t cur_len = sol->route_lengths[seed_vehicle];
                    double best_local_score = INFINITY;
                    uint32_t best_local_pos = UINT32_MAX;
                    double best_local_dist = 0.0;

                    for (pos = 0; pos <= cur_len; pos++) {
                        double score = 0.0, new_dist = 0.0;
                        if (sg_route_eval_insertion_cached(ctx, sol, rid, seed_vehicle,
                                                            pos, &score, &new_dist)) {
                            if (score < best_local_score) {
                                best_local_score = score;
                                best_local_pos = pos;
                                best_local_dist = new_dist;
                            }
                        }
                    }
                    if (best_local_pos == UINT32_MAX) {
                        continue;
                    }
                    c1 = best_local_dist - old_dist;
                    {
                        uint32_t rep_loc;
                        if (sg_request_representative_location(ctx, rid, &rep_loc)) {
                            depot_dist = ctx->vehicles[seed_vehicle].open_start
                                ? 0.0
                                : sg_travel_dist(ctx, v_start_loc, rep_loc, seed_vehicle);
                            if (!ctx->vehicles[seed_vehicle].open_end) {
                                depot_dist += sg_travel_dist(ctx, rep_loc, v_end_loc, seed_vehicle);
                            }
                        } else {
                            depot_dist = 0.0;
                        }
                    }
                    c2 = lambda * depot_dist - c1;
                    if (c2 > best_c2) {
                        best_c2 = c2;
                        best_req = rid;
                        best_pos = best_local_pos;
                        best_pp = UINT32_MAX;
                        best_dp = UINT32_MAX;
                        best_dist = best_local_dist;
                    }
                }
            }

            if (best_req == UINT32_MAX) {
                break;  /* No feasible insertions — close route. */
            }

            /* Apply best insertion. */
            {
                ARStatus s;
                if (best_pp != UINT32_MAX && best_dp != UINT32_MAX) {
                    s = sg_route_apply_pd_insertion(ctx, sol, best_req, seed_vehicle,
                                                     best_pp, best_dp, best_dist);
                } else {
                    s = sg_route_apply_insertion(ctx, sol, best_req, seed_vehicle,
                                                  best_pos, best_dist);
                }
                if (s != AR_STATUS_OK) {
                    break;
                }
            }
        }
    }

    return AR_STATUS_OK;
}

static void sg_build_frozen_vehicle_map(SGContext *ctx) {
    uint32_t r, offset, i;

    if (!ctx->has_frozen || ctx->num_initial_routes == 0) return;

    ctx->frozen_vehicle_map = (uint32_t *)malloc((size_t)ctx->num_requests * sizeof(uint32_t));
    if (!ctx->frozen_vehicle_map) return;

    for (i = 0; i < ctx->num_requests; i++)
        ctx->frozen_vehicle_map[i] = SG_NO_VEHICLE;

    offset = 0;
    for (r = 0; r < ctx->num_initial_routes; r++) {
        uint32_t vid = ctx->initial_route_vehicle_ids[r];
        uint32_t len = ctx->initial_route_lengths[r];
        uint32_t j;
        if (vid < ctx->num_vehicles) {
            for (j = 0; j < len; j++) {
                uint32_t rid = ctx->initial_route_request_ids[offset + j];
                if (rid < ctx->num_requests && sg_request_is_frozen(ctx, rid))
                    ctx->frozen_vehicle_map[rid] = vid;
            }
        }
        offset += len;
    }
}

static ARStatus sg_route_construct_from_warm_start(SGContext *ctx, SGRouteSolution *sol) {
    uint32_t r, offset;
    int pass, num_passes;

    if (!ctx || !sol || ctx->num_initial_routes == 0) {
        return AR_STATUS_ERROR;
    }

    /* Two-pass when frozen requests exist: pass 0 inserts frozen first (priority
       on their designated vehicle), pass 1 inserts the rest.  Single pass otherwise. */
    num_passes = ctx->has_frozen ? 2 : 1;

    for (pass = 0; pass < num_passes; pass++) {
        offset = 0;
        for (r = 0; r < ctx->num_initial_routes; r++) {
            uint32_t vid = ctx->initial_route_vehicle_ids[r];
            uint32_t len = ctx->initial_route_lengths[r];
            uint32_t j;

            if (vid >= ctx->num_vehicles) {
                offset += len;
                continue;
            }

            for (j = 0; j < len; j++) {
                uint32_t rid = ctx->initial_route_request_ids[offset + j];
                double best_score = 0.0;
                uint32_t best_pos = UINT32_MAX;
                uint32_t best_pickup_pos = UINT32_MAX;
                uint32_t best_delivery_pos = UINT32_MAX;
                double best_route_distance = 0.0;
                int is_pd;
                int frozen;

                if (rid >= ctx->num_requests) continue;
                if (sol->base.assigned_flags[rid]) continue;

                frozen = sg_request_is_frozen(ctx, rid);
                if (num_passes == 2) {
                    if (pass == 0 && !frozen) continue;   /* pass 0: frozen only */
                    if (pass == 1 && frozen) continue;    /* pass 1: non-frozen only */
                }

                is_pd = (ctx->requests[rid].kind == SG_REQUEST_KIND_PICKUP_DELIVERY);
                if (is_pd) {
                    if (sg_route_eval_pd_best_insertion_cached(ctx, sol, rid, vid,
                            &best_score, &best_pickup_pos, &best_delivery_pos,
                            &best_route_distance)) {
                        sg_route_apply_pd_insertion(ctx, sol, rid, vid,
                                                    best_pickup_pos, best_delivery_pos,
                                                    best_route_distance);
                    }
                } else {
                    uint32_t pos;
                    uint32_t cur_len = sol->route_lengths[vid];
                    double best_local_score = INFINITY;
                    for (pos = 0; pos <= cur_len; pos++) {
                        double score = 0.0, new_dist = 0.0;
                        if (sg_route_eval_insertion_cached(ctx, sol, rid, vid,
                                                            pos, &score, &new_dist)) {
                            if (score < best_local_score) {
                                best_local_score = score;
                                best_pos = pos;
                                best_route_distance = new_dist;
                            }
                        }
                    }
                    if (best_pos != UINT32_MAX) {
                        sg_route_apply_insertion(ctx, sol, rid, vid,
                                                 best_pos, best_route_distance);
                    }
                }
            }
            offset += len;
        }
    }

    return AR_STATUS_OK;
}

/* Try a candidate solution against the current best; keep the lexicographic winner.
   Returns 1 if candidate was better (sol updated), 0 otherwise. */
static int sg_try_keep_better(SGContext *ctx, SGRouteSolution *sol, SGRouteSolution *candidate) {
    (void)ctx;
    if (candidate->base.num_unassigned < sol->base.num_unassigned ||
        (candidate->base.num_unassigned == sol->base.num_unassigned &&
         (candidate->vehicles_used < sol->vehicles_used ||
          (candidate->vehicles_used == sol->vehicles_used &&
           candidate->total_distance < sol->total_distance - 1e-9)))) {
        sg_route_solution_reset(sol);
        *sol = *candidate;
        return 1;
    }
    sg_route_solution_reset(candidate);
    return 0;
}

static ARStatus sg_route_construct_initial_solution(SGContext *ctx, SGRouteSolution *sol) {
    ARStatus status;

    /* Population mode: single method for per-thread diversity */
    if (ctx->construct_method < SG_CONSTRUCT_COUNT) {
        return sg_construct_by_method(ctx, sol, ctx->construct_method);
    }

    /* Default multi-trial: try all methods, keep lexicographic best */

    /* Attempt 1: regret-3 */
    status = sg_route_repair_fill_regret(ctx, sol, 3, 0.0);
    if (status != AR_STATUS_OK) return status;

    /* Attempt 2: TW-sorted greedy */
    {
        SGRouteSolution alt;
        status = sg_route_solution_init(ctx, &alt);
        if (status == AR_STATUS_OK) {
            status = sg_route_construct_tw_sorted(ctx, &alt);
            if (status != AR_STATUS_OK) {
                sg_route_solution_reset(&alt);
            } else {
                sg_try_keep_better(ctx, sol, &alt);
            }
        }
    }

    /* Attempt 3: Solomon I1 */
    {
        SGRouteSolution i1;
        status = sg_route_solution_init(ctx, &i1);
        if (status == AR_STATUS_OK) {
            status = sg_route_construct_solomon_i1(ctx, &i1);
            if (status != AR_STATUS_OK) {
                sg_route_solution_reset(&i1);
            } else {
                sg_try_keep_better(ctx, sol, &i1);
            }
        }
    }

    /* Attempt 4: Sweep CFRS */
    {
        SGRouteSolution sweep;
        status = sg_route_solution_init(ctx, &sweep);
        if (status == AR_STATUS_OK) {
            status = sg_construct_sweep_cfrs(ctx, &sweep);
            if (status != AR_STATUS_OK) {
                sg_route_solution_reset(&sweep);
            } else {
                sg_try_keep_better(ctx, sol, &sweep);
            }
        }
    }

    /* Attempt 5: K-means TW */
    {
        SGRouteSolution km;
        status = sg_route_solution_init(ctx, &km);
        if (status == AR_STATUS_OK) {
            status = sg_construct_kmeans_tw(ctx, &km);
            if (status != AR_STATUS_OK) {
                sg_route_solution_reset(&km);
            } else {
                sg_try_keep_better(ctx, sol, &km);
            }
        }
    }

    return AR_STATUS_OK;
}

/* Cost wrapper that records violations for adaptive penalty adjustment.
   Used as ALNS ops.cost so recording happens exactly once per candidate. */
static double sg_route_solution_cost_record(const void *solution, void *user_ctx) {
    SGContext *ctx = (SGContext *)user_ctx;
    double cost = sg_route_solution_cost(solution, user_ctx);
    if (ctx->penalty.enabled && ctx->penalty.record) {
        const SGRouteSolution *sol = (const SGRouteSolution *)solution;
        ctx->penalty.record(&ctx->penalty, sol->violations);
    }
    return cost;
}

static ARALNSContext *sg_create_route_alns(SGContext *ctx, ARALNSParams *params,
                                            ARSolutionOps *ops,
                                            double vehicle_target_weight,
                                            double vehicle_empty_weight) {
    ARALNSContext *alns;

    ops->copy = sg_route_solution_copy;
    ops->free = sg_route_solution_free;
    ops->cost = sg_route_solution_cost_record;
    ops->size = sg_route_solution_size;
    ops->validate = sg_route_solution_validate;
    ops->is_better = sg_instrumented_is_better;
    ops->user_ctx = ctx;

    alns = ar_alns_create(params, ops, ctx);
    if (!alns) {
        return NULL;
    }

    if (ar_alns_add_destroy(alns, "random", sg_route_destroy_random, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_destroy(alns, "criticality-worst", sg_route_destroy_criticality_worst, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_destroy(alns, "route-removal", sg_route_destroy_route_removal, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_destroy(alns, "time-window-removal", sg_route_destroy_time_window, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_destroy(alns, "route-cluster", sg_route_destroy_route_cluster, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_destroy(alns, "time-cluster", sg_route_destroy_time_cluster, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_destroy(alns, "paired-shaw", sg_route_destroy_paired_shaw, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_destroy(alns, "vehicle-target", sg_route_destroy_vehicle_target, ctx, vehicle_target_weight) != AR_STATUS_OK ||
        ar_alns_add_destroy(alns, "vehicle-empty", sg_route_destroy_vehicle_empty, ctx, vehicle_empty_weight) != AR_STATUS_OK ||
        ar_alns_add_destroy(alns, "worst", sg_route_destroy_worst, ctx, 0.5) != AR_STATUS_OK ||
        ar_alns_add_destroy(alns, "shaw", sg_route_destroy_shaw, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_destroy(alns, "string", sg_route_destroy_string, ctx, 2.0) != AR_STATUS_OK ||
        ar_alns_add_destroy(alns, "string-split", sg_route_destroy_string_split, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_destroy(alns, "vehicle-worst-cost", sg_route_destroy_vehicle_worst_cost, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_repair(alns, "greedy-insert", sg_route_repair_greedy, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_repair(alns, "regret-2", sg_route_repair_regret2, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_repair(alns, "regret-3", sg_route_repair_regret3, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_repair(alns, "regret-4", sg_route_repair_regret4, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_repair(alns, "noise-regret", sg_route_repair_noise_regret, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_repair(alns, "pair-sync", sg_route_repair_pair_sync, ctx, 1.0) != AR_STATUS_OK ||
        ar_alns_add_repair(alns, "bootstrap-repair", sg_route_repair_greedy, ctx, 0.5) != AR_STATUS_OK) {
        ar_alns_free(alns);
        return NULL;
    }

    return alns;
}

static SGStatus sg_solve_route_model(SGContext *ctx) {
    ARALNSParams params;
    ARSolutionOps ops;
    ARALNSContext *alns = NULL;
    SGRouteSolution initial;
    SGRouteSolution *p1_best = NULL;
    SGRouteSolution *p2_best = NULL;
    ARStatus ar_status;
    ARALNSStats ar_stats;
    ARStatus init_status;
    const SGRouteSolution *final_sol;
    int total_iters;
    int phase1_iters;
    int phase2_iters;
    int64_t total_alns_iters = 0;
    int solution_valid = 1;

    if (!ctx) {
        return SG_STATUS_INVALID_ARG;
    }

    /* Initialize instrumentation */
    ctx->solve_start_time = sg_monotonic_seconds();
    sg_time_budget_init(&ctx->time_budget, ctx->solve_start_time,
                        (double)ctx->config.max_time_seconds);
    ctx->current_phase = SG_PHASE_CONSTRUCTION;
    ctx->convergence_count = 0;
    ctx->convergence_write_pos = 0;
    ctx->num_phase_stats = 0;
    memset(&ctx->penalty_snapshot, 0, sizeof(ctx->penalty_snapshot));

    /* Reset operator telemetry for this solve */
    free(ctx->destroy_op_stats);
    ctx->destroy_op_stats = NULL;
    ctx->num_destroy_ops = 0;
    free(ctx->repair_op_stats);
    ctx->repair_op_stats = NULL;
    ctx->num_repair_ops = 0;

    init_status = sg_route_solution_init(ctx, &initial);
    if (init_status != AR_STATUS_OK) {
        return SG_STATUS_OUT_OF_MEMORY;
    }

    sg_scratch_init(ctx);
    sg_build_frozen_vehicle_map(ctx);
    sg_neighbor_init(&ctx->neighbor_index, ctx,
                     (uint32_t)sg_tune_i(ctx,
                         ctx->tune_params ? ctx->tune_params->neighbor_k : SG_TUNE_SENTINEL_I,
                         30));

    sg_phase_start(ctx, SG_PHASE_CONSTRUCTION, 0.0, 0, ctx->num_requests);
    if (ctx->num_initial_routes > 0) {
        init_status = sg_route_construct_from_warm_start(ctx, &initial);
        /* Fill any remaining unassigned requests via standard construction */
        if (init_status == AR_STATUS_OK && initial.base.num_unassigned > 0) {
            (void)sg_route_repair_fill_regret(ctx, &initial, 3, 0.0);
        }
    } else {
        init_status = sg_route_construct_initial_solution(ctx, &initial);
    }
    sg_phase_end(ctx, 0, sg_route_solution_cost(&initial, ctx),
                 initial.vehicles_used, initial.base.num_unassigned);
    /* Record construction result as first convergence entry */
    if (init_status == AR_STATUS_OK &&
        (ctx->convergence_buffer || ctx->convergence_callback)) {
        SGConvergenceEntry entry;
        entry.iteration = 0;
        entry.cost = sg_route_solution_cost(&initial, ctx);
        entry.elapsed_seconds = sg_monotonic_seconds() - ctx->solve_start_time;
        entry.vehicles_used = initial.vehicles_used;
        entry.unassigned = initial.base.num_unassigned;
        entry.total_distance = initial.total_distance;
        entry.phase = SG_PHASE_CONSTRUCTION;
        entry.is_new_best = 1;
        sg_record_convergence(ctx, &entry);
    }
    if (init_status != AR_STATUS_OK) {
        free(ctx->frozen_vehicle_map); ctx->frozen_vehicle_map = NULL;
        sg_scratch_free(ctx);
        sg_route_solution_reset(&initial);
        return init_status == AR_STATUS_OUT_OF_MEMORY ? SG_STATUS_OUT_OF_MEMORY
                                                      : SG_STATUS_ERROR;
    }
    total_iters = ctx->config.max_iterations;

    /* Skip vehicle minimization phase when no vehicle has positive fixed cost —
       there is no incentive to reduce the fleet. */
    {
        int has_fixed = 0;
        uint32_t vi;
        for (vi = 0; vi < ctx->num_vehicles; vi++) {
            if (ctx->vehicles[vi].fixed_cost > 1e-9) {
                has_fixed = 1;
                break;
            }
        }
        if (has_fixed) {
            double p1_frac = sg_tune_d(ctx,
                ctx->tune_params ? ctx->tune_params->phase1_fraction : SG_TUNE_SENTINEL_D,
                0.60);
            phase1_iters = (int)(total_iters * p1_frac);
            if (phase1_iters < 500) phase1_iters = 500;
            if (phase1_iters > total_iters) phase1_iters = total_iters;
        } else {
            phase1_iters = 0;
        }
    }
    phase2_iters = total_iters - phase1_iters;

    /* ---- Phase 1: Vehicle minimization ---- */
    if (phase1_iters > 0) {
        sg_phase_start(ctx, SG_PHASE_1_VEHICLE_MIN,
                       sg_route_solution_cost(&initial, ctx),
                       initial.vehicles_used, initial.base.num_unassigned);
        ar_alns_params_default(&params);
        params.max_iterations = phase1_iters;
        {
            double p1_now = sg_monotonic_seconds();
            if (sg_time_budget_expired(&ctx->time_budget, p1_now)) goto skip_phase1;
            double p1_time = sg_time_budget_phase(&ctx->time_budget, p1_now, 0.55, 5.0);
            params.max_time_seconds = (p1_time == DBL_MAX)
                ? ctx->config.max_time_seconds
                : (int)ceil(p1_time);
        }
        params.segment_size = ctx->config.segment_size;
        sg_adaptive_q_bounds((int)ctx->num_requests, ctx->config.q_min, ctx->config.q_max,
                             &params.q_min, &params.q_max);
        params.target_cost = 0.0;
        params.restart_threshold = phase1_iters / 4;
        /* Calibrate from total cost (includes vehicle penalty ~1M per vehicle).
           This makes temperature ~1000x hotter so SA accepts distance-worsening
           moves that reduce vehicle count. */
        ar_alns_calibrate_sa(&params, sg_route_solution_cost(&initial, ctx), phase1_iters);
        /* Slower cooling for vehicle minimization: decay to final_temp_ratio of T0.
           Vehicle-reducing moves need high temperature; Phase 2 handles distance. */
        {
            double p1_final = sg_tune_d(ctx,
                ctx->tune_params ? ctx->tune_params->p1_final_temp_ratio : SG_TUNE_SENTINEL_D,
                0.05);
            params.cooling_rate = exp(log(p1_final) / (double)phase1_iters);
        }
        /* Override initial temperature if sa_accept_pct is tuned.
           Default calibrate_sa uses 0.05; tuner found 0.074 gives better results.
           Profiles set this via tune_params; non-profiled solves use calibrate_sa default. */
        if (ctx->tune_params && ctx->tune_params->sa_accept_pct != SG_TUNE_SENTINEL_D) {
            double abs_cost = fabs(sg_route_solution_cost(&initial, ctx));
            if (abs_cost < 1e-12) abs_cost = 1.0;
            params.initial_temp = ctx->tune_params->sa_accept_pct * abs_cost / 0.693147180559945;
        }
        if (ctx->config.accept_type != SG_ACCEPT_SA) {
            params.accept_type = (ARAcceptType)ctx->config.accept_type;
            if (params.accept_type == AR_ACCEPT_RRT) {
                double ic = sg_route_solution_cost(&initial, ctx);
                params.threshold = 0.05 * fabs(ic);
            }
        }
        if (ctx->config.adaptive_q) {
            params.adaptive_q = 1;
        }

        /* Enable infeasible-space exploration with progressive schedule:
           Start aggressive (25% infeasibility target) to allow deeper infeasible-space
           exploration when vehicle cuts are most likely, linearly tighten to 15%
           over Phase 1 to avoid returning infeasible solutions.
           cost_scale derived from median per-route cost of initial solution. */
        {
            double cost_scale = sg_compute_cost_scale(ctx, &initial);
            int seg_sz = ctx->tune_params
                ? sg_tune_i(ctx, ctx->tune_params->segment_size, ctx->config.segment_size)
                : ctx->config.segment_size;
            int p1_segs = phase1_iters / seg_sz;
            double pen_ts = sg_tune_d(ctx, ctx->tune_params ? ctx->tune_params->pen_target_start : SG_TUNE_SENTINEL_D, 0.25);
            double pen_te = sg_tune_d(ctx, ctx->tune_params ? ctx->tune_params->pen_target_end : SG_TUNE_SENTINEL_D, 0.15);
            double pen_tol = sg_tune_d(ctx, ctx->tune_params ? ctx->tune_params->pen_tolerance : SG_TUNE_SENTINEL_D, 0.08);
            double pen_inc = sg_tune_d(ctx, ctx->tune_params ? ctx->tune_params->pen_increase : SG_TUNE_SENTINEL_D, 1.3);
            double pen_dec = sg_tune_d(ctx, ctx->tune_params ? ctx->tune_params->pen_decrease : SG_TUNE_SENTINEL_D, 0.80);
            sg_penalty_init_progressive(&ctx->penalty, pen_ts, pen_te, pen_tol, pen_inc, pen_dec,
                                        cost_scale, p1_segs > 0 ? (uint32_t)p1_segs : 1);
        }

        sg_apply_tune_to_alns(ctx, &params);
        alns = sg_create_route_alns(ctx, &params, &ops, 3.0, 2.0);
        if (!alns) {
            sg_penalty_free(&ctx->penalty);
            free(ctx->frozen_vehicle_map); ctx->frozen_vehicle_map = NULL;
            sg_scratch_free(ctx);
            sg_route_solution_reset(&initial);
            return SG_STATUS_OUT_OF_MEMORY;
        }

        if (ctx->config.deterministic) {
            ar_alns_set_seed(alns, ctx->config.seed);
            sh_rng_seed(ctx->op_rng, ctx->config.seed ^ SG_OPERATOR_SEED_XOR);
        } else {
            sh_rng_seed_time(ctx->op_rng);
        }
        if (ctx->progress_callback || ctx->cancel_requested ||
            ctx->convergence_buffer || ctx->convergence_callback) {
            ar_alns_set_progress_callback(alns, sg_progress_forwarder, ctx);
        }

        ctx->avoid_new_vehicles = 1;
        ctx->ejection_in_repair = 1;
        ar_status = ar_alns_solve(alns, &initial, (void **)&p1_best);
        ctx->ejection_in_repair = 0;
        ctx->avoid_new_vehicles = 0;
        if (ar_status != AR_STATUS_OK && ar_status != AR_STATUS_LIMIT) {
            sg_penalty_free(&ctx->penalty);
            free(ctx->frozen_vehicle_map); ctx->frozen_vehicle_map = NULL;
            sg_scratch_free(ctx);
            sg_route_solution_reset(&initial);
            ar_alns_free(alns);
            return SG_STATUS_ERROR;
        }
        ar_alns_get_stats(alns, &ar_stats);
        total_alns_iters += ar_stats.iterations;
        sg_copy_operator_stats(ctx, alns);
        sg_phase_end(ctx, ar_stats.iterations,
                     p1_best ? sg_route_solution_cost(p1_best, ctx) : sg_route_solution_cost(&initial, ctx),
                     p1_best ? p1_best->vehicles_used : initial.vehicles_used,
                     p1_best ? p1_best->base.num_unassigned : initial.base.num_unassigned);
        ar_alns_free(alns);
        alns = NULL;
    }
skip_phase1:

    /* Disable penalty for postprocessing between phases (must be strict) */
    ctx->penalty.enabled = 0;

    /* ---- Ejection pulse: exploit Phase 1's loose routes to eliminate vehicles ---- */
    if (p1_best && !sg_time_budget_expired(&ctx->time_budget, sg_monotonic_seconds())) {
        (void)sg_route_postprocess_ejection_reduce(ctx, p1_best);
        if (!sg_time_budget_expired(&ctx->time_budget, sg_monotonic_seconds())) {
            (void)sg_route_postprocess_reduce_vehicles_relaxed(ctx, p1_best, 1.20);
        }
        if (!sg_time_budget_expired(&ctx->time_budget, sg_monotonic_seconds())) {
            (void)sg_route_postprocess_intensify(ctx, p1_best);
        }
    }

    /* ---- Phase 1.5: Vehicle crunch — short focused ALNS with vehicle-reducing operators ---- */
    if (sg_time_budget_expired(&ctx->time_budget, sg_monotonic_seconds())) goto skip_phase15;
    if (p1_best && p1_best->vehicles_used > 1) {
        int p15_iters = sg_tune_i(ctx,
            ctx->tune_params ? ctx->tune_params->phase15_iters : SG_TUNE_SENTINEL_I, 500);
        double cost_scale;

        sg_phase_start(ctx, SG_PHASE_1_5_CRUNCH,
                       sg_route_solution_cost(p1_best, ctx),
                       p1_best->vehicles_used, p1_best->base.num_unassigned);

        /* Phase 1.5 runs as additional budget — do NOT subtract from Phase 2.
           Reducing Phase 2 budget degrades search quality on frozen-request models. */

        cost_scale = sg_compute_cost_scale(ctx, p1_best);
        {
            double p15_tgt = sg_tune_d(ctx, ctx->tune_params ? ctx->tune_params->pen_p15_target : SG_TUNE_SENTINEL_D, 0.20);
            double p15_tol = sg_tune_d(ctx, ctx->tune_params ? ctx->tune_params->pen_p15_tolerance : SG_TUNE_SENTINEL_D, 0.05);
            double p15_inc = sg_tune_d(ctx, ctx->tune_params ? ctx->tune_params->pen_p15_increase : SG_TUNE_SENTINEL_D, 1.2);
            double p15_dec = sg_tune_d(ctx, ctx->tune_params ? ctx->tune_params->pen_p15_decrease : SG_TUNE_SENTINEL_D, 0.85);
            sg_penalty_init_adaptive(&ctx->penalty, p15_tgt, p15_tol, p15_inc, p15_dec, cost_scale);
        }

        {
            ARALNSParams p15_params;
            ARSolutionOps p15_ops;
            ARALNSContext *p15_alns;
            SGRouteSolution *p15_best = NULL;
            ARStatus p15_status;
            ARALNSStats p15_stats;

            ar_alns_params_default(&p15_params);
            p15_params.max_iterations = p15_iters;
            {
                int p15_time = sg_time_budget_remaining_int(&ctx->time_budget, sg_monotonic_seconds());
                p15_params.max_time_seconds = (p15_time > 0) ? p15_time : ctx->config.max_time_seconds;
            }
            p15_params.segment_size = ctx->config.segment_size;
            sg_adaptive_q_bounds((int)ctx->num_requests, ctx->config.q_min, ctx->config.q_max,
                                 &p15_params.q_min, &p15_params.q_max);
            p15_params.target_cost = 0.0;
            p15_params.restart_threshold = p15_iters / 2;
            ar_alns_calibrate_sa(&p15_params, sg_route_solution_cost(p1_best, ctx), p15_iters);
            p15_params.cooling_rate = exp(log(0.05) / (double)p15_iters);

            p15_ops.copy = sg_route_solution_copy;
            p15_ops.free = sg_route_solution_free;
            p15_ops.cost = sg_route_solution_cost_record;
            p15_ops.size = sg_route_solution_size;
            p15_ops.validate = sg_route_solution_validate;
            p15_ops.is_better = sg_instrumented_is_better;
            p15_ops.user_ctx = ctx;

            p15_alns = ar_alns_create(&p15_params, &p15_ops, ctx);
            if (p15_alns) {
                /* Register only vehicle-reducing operators */
                int reg_ok = 1;
                if (ar_alns_add_destroy(p15_alns, "vehicle-target", sg_route_destroy_vehicle_target, ctx, 3.0) != AR_STATUS_OK) reg_ok = 0;
                if (reg_ok && ar_alns_add_destroy(p15_alns, "vehicle-empty", sg_route_destroy_vehicle_empty, ctx, 2.0) != AR_STATUS_OK) reg_ok = 0;
                if (reg_ok && ar_alns_add_destroy(p15_alns, "route-removal", sg_route_destroy_route_removal, ctx, 1.0) != AR_STATUS_OK) reg_ok = 0;
                if (reg_ok && ar_alns_add_repair(p15_alns, "regret-3", sg_route_repair_regret3, ctx, 1.0) != AR_STATUS_OK) reg_ok = 0;
                if (reg_ok && ar_alns_add_repair(p15_alns, "greedy-insert", sg_route_repair_greedy, ctx, 1.0) != AR_STATUS_OK) reg_ok = 0;

                if (reg_ok) {
                    if (ctx->config.deterministic) {
                        ar_alns_set_seed(p15_alns, ctx->config.seed + 2);
                        sh_rng_seed(ctx->op_rng, (ctx->config.seed + 2) ^ SG_OPERATOR_SEED_XOR);
                    } else {
                        sh_rng_seed_time(ctx->op_rng);
                    }
                    if (ctx->progress_callback || ctx->cancel_requested ||
                        ctx->convergence_buffer || ctx->convergence_callback) {
                        ar_alns_set_progress_callback(p15_alns, sg_progress_forwarder, ctx);
                    }

                    ctx->avoid_new_vehicles = 1;
                    p15_status = ar_alns_solve(p15_alns, p1_best, (void **)&p15_best);
                    ctx->avoid_new_vehicles = 0;

                    if ((p15_status == AR_STATUS_OK || p15_status == AR_STATUS_LIMIT) && p15_best) {
                        ar_alns_get_stats(p15_alns, &p15_stats);
                        total_alns_iters += p15_stats.iterations;
                        sg_copy_operator_stats(ctx, p15_alns);

                        if (sg_route_solution_is_better(p15_best, p1_best, ctx)) {
                            sg_route_solution_free(p1_best, NULL);
                            p1_best = p15_best;
                            p15_best = NULL;
                        }
                    }
                    sg_route_solution_free(p15_best, NULL);
                }
                ar_alns_free(p15_alns);
            }
        }

        sg_phase_end(ctx, 0,  /* p1.5 iters already counted in total */
                     sg_route_solution_cost(p1_best, ctx),
                     p1_best->vehicles_used, p1_best->base.num_unassigned);
        ctx->penalty.enabled = 0;
    }
skip_phase15:

    /* ---- Phase 2: Distance polishing ---- */
    if (sg_time_budget_expired(&ctx->time_budget, sg_monotonic_seconds())) goto skip_phase2;
    if (phase2_iters > 0) {
        SGRouteSolution *p2_initial = p1_best ? p1_best : &initial;

        sg_phase_start(ctx, SG_PHASE_2_POLISH,
                       sg_route_solution_cost(p2_initial, ctx),
                       p2_initial->vehicles_used, p2_initial->base.num_unassigned);

        /* Phase 2 runs strict (no infeasible exploration) — penalty hurts distance
           quality without meaningful vehicle reduction at this stage. */
        ctx->penalty.enabled = 0;

        ar_alns_params_default(&params);
        params.max_iterations = phase2_iters;
        {
            int p2_time = sg_time_budget_remaining_int(&ctx->time_budget, sg_monotonic_seconds());
            params.max_time_seconds = (p2_time > 0) ? p2_time : ctx->config.max_time_seconds;
        }
        params.segment_size = ctx->config.segment_size;
        sg_adaptive_q_bounds((int)ctx->num_requests, ctx->config.q_min, ctx->config.q_max,
                             &params.q_min, &params.q_max);
        params.target_cost = 0.0;
        params.restart_threshold = phase2_iters / 4;
        /* Calibrate from variable cost (distance + duration), excluding fixed
           costs and unassigned penalty.  This works for any cost model:
           standard VRP (cost ≈ distance), DARP (cost ≈ duration), or mixed. */
        {
            double p2_cal = p2_initial->total_distance;
            if (p2_initial->route_duration) {
                uint32_t dv;
                for (dv = 0; dv < p2_initial->num_vehicles; dv++) {
                    p2_cal += p2_initial->route_duration[dv];
                }
            }
            if (p2_cal < 1e-9) {
                p2_cal = sg_route_solution_cost(p2_initial, ctx);
            }
            ar_alns_calibrate_sa(&params, p2_cal, phase2_iters);
            /* Override Phase 2 cooling and SA acceptance if tuned via profiles.
               Tuner found: p2_final_temp_ratio=0.0001, sa_accept_pct=0.074. */
            if (ctx->tune_params) {
                if (ctx->tune_params->p2_final_temp_ratio != SG_TUNE_SENTINEL_D) {
                    params.cooling_rate = exp(log(ctx->tune_params->p2_final_temp_ratio) / (double)phase2_iters);
                }
                if (ctx->tune_params->sa_accept_pct != SG_TUNE_SENTINEL_D &&
                    fabs(p2_cal) > 1e-12) {
                    params.initial_temp = ctx->tune_params->sa_accept_pct * fabs(p2_cal) / 0.693147180559945;
                }
            }
        }
        if (ctx->config.accept_type != SG_ACCEPT_SA) {
            params.accept_type = (ARAcceptType)ctx->config.accept_type;
            if (params.accept_type == AR_ACCEPT_RRT) {
                double ic = p2_initial->total_distance > 0.0
                            ? p2_initial->total_distance
                            : sg_route_solution_cost(p2_initial, ctx);
                params.threshold = 0.05 * fabs(ic);
            }
        }
        if (ctx->config.adaptive_q) {
            params.adaptive_q = 1;
        }

        sg_apply_tune_to_alns(ctx, &params);
        alns = sg_create_route_alns(ctx, &params, &ops, 1.5, 1.0);
        if (!alns) {
            sg_penalty_free(&ctx->penalty);
            free(ctx->frozen_vehicle_map); ctx->frozen_vehicle_map = NULL;
            sg_scratch_free(ctx);
            sg_route_solution_reset(&initial);
            sg_route_solution_free(p1_best, NULL);
            return SG_STATUS_OUT_OF_MEMORY;
        }

        if (ctx->config.deterministic) {
            ar_alns_set_seed(alns, ctx->config.seed + 1);
            sh_rng_seed(ctx->op_rng, (ctx->config.seed + 1) ^ SG_OPERATOR_SEED_XOR);
        } else {
            sh_rng_seed_time(ctx->op_rng);
        }
        if (ctx->progress_callback || ctx->cancel_requested ||
            ctx->convergence_buffer || ctx->convergence_callback) {
            ar_alns_set_progress_callback(alns, sg_progress_forwarder, ctx);
        }

        ar_status = ar_alns_solve(alns, p2_initial, (void **)&p2_best);
        if (ar_status != AR_STATUS_OK && ar_status != AR_STATUS_LIMIT) {
            sg_penalty_free(&ctx->penalty);
            free(ctx->frozen_vehicle_map); ctx->frozen_vehicle_map = NULL;
            sg_scratch_free(ctx);
            sg_route_solution_reset(&initial);
            sg_route_solution_free(p1_best, NULL);
            ar_alns_free(alns);
            return SG_STATUS_ERROR;
        }
        ar_alns_get_stats(alns, &ar_stats);
        total_alns_iters += ar_stats.iterations;
        sg_copy_operator_stats(ctx, alns);
        sg_phase_end(ctx, ar_stats.iterations,
                     p2_best ? sg_route_solution_cost(p2_best, ctx) : sg_route_solution_cost(p2_initial, ctx),
                     p2_best ? p2_best->vehicles_used : p2_initial->vehicles_used,
                     p2_best ? p2_best->base.num_unassigned : p2_initial->base.num_unassigned);
        ar_alns_free(alns);
        alns = NULL;
    }
skip_phase2:

    /* Disable infeasible-space exploration for postprocessing (must be strict) */
    ctx->penalty.enabled = 0;

    /* Determine best solution across phases. */
    {
        SGRouteSolution *best;
        if (p2_best && p1_best) {
            best = sg_route_solution_is_better(p1_best, p2_best, ctx) ? p1_best : p2_best;
        } else {
            best = p2_best ? p2_best : p1_best;
        }

        /* Postprocess phase */
        {
            SGRouteSolution *pp_sol = best ? best : &initial;
            sg_phase_start(ctx, SG_PHASE_POSTPROCESS,
                           sg_route_solution_cost(pp_sol, ctx),
                           pp_sol->vehicles_used, pp_sol->base.num_unassigned);
        }
        if (best) {
            if (!sg_time_budget_expired(&ctx->time_budget, sg_monotonic_seconds()))
                (void)sg_route_postprocess_reduce_vehicles(ctx, best);
            if (!sg_time_budget_expired(&ctx->time_budget, sg_monotonic_seconds()))
                (void)sg_route_postprocess_ejection_reduce(ctx, best);
            if (!sg_time_budget_expired(&ctx->time_budget, sg_monotonic_seconds()))
                (void)sg_route_postprocess_intensify(ctx, best);
            if (!sg_time_budget_expired(&ctx->time_budget, sg_monotonic_seconds()) &&
                (ctx->num_requests <= 200 || phase2_iters == 0))
                (void)sg_route_postprocess_polish_distance(ctx, best);
        } else {
            if (!sg_time_budget_expired(&ctx->time_budget, sg_monotonic_seconds()))
                (void)sg_route_postprocess_reduce_vehicles(ctx, &initial);
            if (!sg_time_budget_expired(&ctx->time_budget, sg_monotonic_seconds()))
                (void)sg_route_postprocess_ejection_reduce(ctx, &initial);
            if (!sg_time_budget_expired(&ctx->time_budget, sg_monotonic_seconds()))
                (void)sg_route_postprocess_intensify(ctx, &initial);
            if (!sg_time_budget_expired(&ctx->time_budget, sg_monotonic_seconds()) &&
                (ctx->num_requests <= 200 || phase2_iters == 0))
                (void)sg_route_postprocess_polish_distance(ctx, &initial);
        }

        final_sol = best ? best : &initial;

        if (!sg_route_solution_validate(final_sol, (void *)ctx)) {
            /* Phase 1 infeasible-space exploration may have produced an
               infeasible best.  Fall back to the initial solution if valid. */
            if (final_sol != &initial &&
                sg_route_solution_validate(&initial, (void *)ctx)) {
                final_sol = &initial;
            } else {
                solution_valid = 0;
            }
        }

        /* End postprocess phase */
        sg_phase_end(ctx, 0,
                     sg_route_solution_cost(final_sol, ctx),
                     final_sol->vehicles_used, final_sol->base.num_unassigned);

        ctx->stats.iterations = total_alns_iters;
        ctx->stats.unassigned = final_sol->base.num_unassigned;
        ctx->stats.vehicles_used = final_sol->vehicles_used;
        ctx->stats.total_distance = final_sol->total_distance;
        ctx->stats.total_cost = sg_route_solution_cost(final_sol, ctx);
        ctx->stats.elapsed_seconds = sg_monotonic_seconds() - ctx->solve_start_time;
        ctx->stats.phase = ctx->current_phase;
        {
            double tw = 0.0;
            uint32_t wv;
            if (final_sol->route_waiting) {
                for (wv = 0; wv < final_sol->num_vehicles; wv++) {
                    tw += final_sol->route_waiting[wv];
                }
            }
            ctx->stats.total_waiting = tw;
        }
        {
            double tot = 0.0;
            uint32_t ov;
            if (final_sol->route_overtime) {
                for (ov = 0; ov < final_sol->num_vehicles; ov++) {
                    tot += final_sol->route_overtime[ov];
                }
            }
            ctx->stats.total_overtime = tot;
        }
        {
            double ttp = 0.0;
            uint32_t tv;
            if (final_sol->route_tw_penalty) {
                for (tv = 0; tv < final_sol->num_vehicles; tv++) {
                    ttp += final_sol->route_tw_penalty[tv];
                }
            }
            ctx->stats.total_tw_penalty = ttp;
        }
        /* Span stats (unconditional) */
        {
            double min_dur = INFINITY, max_dur = -INFINITY;
            double min_dist = INFINITY, max_dist = -INFINITY;
            uint32_t sv, s_active = 0;
            for (sv = 0; sv < final_sol->num_vehicles; sv++) {
                if (final_sol->route_stop_lengths[sv] > 0) {
                    s_active++;
                    if (final_sol->route_duration) {
                        double d = final_sol->route_duration[sv];
                        if (d < min_dur) min_dur = d;
                        if (d > max_dur) max_dur = d;
                    }
                    {
                        double dd = final_sol->route_distance[sv];
                        if (dd < min_dist) min_dist = dd;
                        if (dd > max_dist) max_dist = dd;
                    }
                }
            }
            ctx->stats.duration_span = (s_active >= 2 && max_dur > min_dur) ? (max_dur - min_dur) : 0.0;
            ctx->stats.distance_span = (s_active >= 2 && max_dist > min_dist) ? (max_dist - min_dist) : 0.0;
        }

        /* Copy insertion cache stats */
        if (ctx->scratch.insertion_cache) {
            SGInsertionCache *ic = ctx->scratch.insertion_cache;
            ctx->stats.insertion_cache_hits = ic->total_hits + ic->hits;
            ctx->stats.insertion_cache_misses = ic->total_misses + ic->misses;
        }

        /* Retain final solution for route/stop export */
        if (ctx->final_solution) {
            sg_route_solution_free(ctx->final_solution, NULL);
        }
        ctx->final_solution = (SGRouteSolution *)sg_route_solution_copy(final_sol, (void *)ctx);
    }

    /* Capture penalty weights before freeing */
    {
        int pi;
        for (pi = 0; pi < SG_PENALTY_COUNT; pi++) {
            ctx->penalty_snapshot.weight[pi] = ctx->penalty.weight[pi];
        }
    }
    sg_penalty_free(&ctx->penalty);
    free(ctx->frozen_vehicle_map); ctx->frozen_vehicle_map = NULL;
    sg_neighbor_free(&ctx->neighbor_index);
    sg_scratch_free(ctx);
    sg_route_solution_reset(&initial);
    sg_route_solution_free(p1_best, NULL);
    sg_route_solution_free(p2_best, NULL);

    if (!solution_valid) {
        return SG_STATUS_ERROR;
    }
    if (ar_status == AR_STATUS_LIMIT) {
        return SG_STATUS_LIMIT;
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
    ctx->cancel_requested = 0;
    sg_clear_error(ctx);
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
    /* Resolve profile × scale matrix if a profile was set */
    if (ctx->active_profile < SG_PROFILE_COUNT && !ctx->profile_applied) {
        SGScale scale = ctx->active_scale;
        if (scale >= SG_SCALE_COUNT) {
            scale = sg_scale_from_count(ctx->num_requests);
        }
        sg_profile_matrix_apply(ctx, ctx->active_profile, scale);
        ctx->profile_applied = 1;
    }
    {
        SGStatus prep_status = sg_prepare_travel(ctx);
        if (prep_status != SG_STATUS_OK) {
            return prep_status;
        }
    }
    if (sg_route_solver_eligible(ctx)) {
        return sg_solve_route_model(ctx);
    }

    /* Reset operator telemetry for this solve */
    free(ctx->destroy_op_stats);
    ctx->destroy_op_stats = NULL;
    ctx->num_destroy_ops = 0;
    free(ctx->repair_op_stats);
    ctx->repair_op_stats = NULL;
    ctx->num_repair_ops = 0;

    init_status = sg_bootstrap_solution_init(&initial, ctx->num_requests);
    if (init_status != AR_STATUS_OK) {
        return SG_STATUS_OUT_OF_MEMORY;
    }

    init_status = sg_construct_initial_solution(ctx, &initial);
    if (init_status != AR_STATUS_OK) {
        sg_bootstrap_solution_reset(&initial);
        return init_status == AR_STATUS_OUT_OF_MEMORY ? SG_STATUS_OUT_OF_MEMORY
                                                      : SG_STATUS_ERROR;
    }

    ar_alns_params_default(&params);
    params.max_iterations = ctx->config.max_iterations;
    params.max_time_seconds = ctx->config.max_time_seconds;
    params.segment_size = ctx->config.segment_size;
    sg_adaptive_q_bounds((int)ctx->num_requests, ctx->config.q_min, ctx->config.q_max,
                         &params.q_min, &params.q_max);
    params.target_cost = 0.0;
    params.restart_threshold = params.max_iterations / 4;
    ar_alns_calibrate_sa(&params, sg_bootstrap_cost(&initial, ctx),
                          params.max_iterations);

    ops.copy = sg_bootstrap_copy;
    ops.free = sg_bootstrap_free;
    ops.cost = sg_bootstrap_cost;
    ops.size = sg_bootstrap_size;
    ops.validate = sg_bootstrap_validate;
    ops.is_better = NULL;
    ops.user_ctx = ctx;

    alns = ar_alns_create(&params, &ops, ctx);
    if (!alns) {
        sg_bootstrap_solution_reset(&initial);
        return SG_STATUS_OUT_OF_MEMORY;
    }

    if (ctx->config.deterministic) {
        ar_alns_set_seed(alns, ctx->config.seed);
        sh_rng_seed(ctx->op_rng, ctx->config.seed ^ SG_OPERATOR_SEED_XOR);
    } else {
        sh_rng_seed_time(ctx->op_rng);
    }
    if (ctx->progress_callback || ctx->cancel_requested ||
        ctx->convergence_buffer || ctx->convergence_callback) {
        ar_alns_set_progress_callback(alns, sg_progress_forwarder, ctx);
    }

    ar_status = ar_alns_add_destroy(alns, "random", sg_destroy_random, ctx, 1.0);
    if (ar_status != AR_STATUS_OK) {
        sg_bootstrap_solution_reset(&initial);
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_destroy(alns, "criticality-worst",
                                    sg_destroy_criticality_worst, ctx, 1.0);
    if (ar_status != AR_STATUS_OK) {
        sg_bootstrap_solution_reset(&initial);
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_destroy(alns, "route-cluster",
                                    sg_destroy_route_cluster, ctx, 1.0);
    if (ar_status != AR_STATUS_OK) {
        sg_bootstrap_solution_reset(&initial);
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_destroy(alns, "time-cluster",
                                    sg_destroy_time_cluster, ctx, 1.0);
    if (ar_status != AR_STATUS_OK) {
        sg_bootstrap_solution_reset(&initial);
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_destroy(alns, "paired-shaw",
                                    sg_destroy_paired_shaw, ctx, 1.0);
    if (ar_status != AR_STATUS_OK) {
        sg_bootstrap_solution_reset(&initial);
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_destroy(alns, "worst", sg_destroy_worst, ctx, 0.5);
    if (ar_status != AR_STATUS_OK) {
        sg_bootstrap_solution_reset(&initial);
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_destroy(alns, "shaw", sg_destroy_shaw, ctx, 0.5);
    if (ar_status != AR_STATUS_OK) {
        sg_bootstrap_solution_reset(&initial);
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_repair(alns, "greedy-insert", sg_repair_greedy_insertion, ctx, 1.0);
    if (ar_status != AR_STATUS_OK) {
        sg_bootstrap_solution_reset(&initial);
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_repair(alns, "regret-2", sg_repair_regret2, ctx, 1.0);
    if (ar_status != AR_STATUS_OK) {
        sg_bootstrap_solution_reset(&initial);
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_repair(alns, "regret-3", sg_repair_regret3, ctx, 1.0);
    if (ar_status != AR_STATUS_OK) {
        sg_bootstrap_solution_reset(&initial);
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_repair(alns, "regret-4", sg_repair_regret4, ctx, 1.0);
    if (ar_status != AR_STATUS_OK) {
        sg_bootstrap_solution_reset(&initial);
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_repair(alns, "noise-regret", sg_repair_noise_regret, ctx, 1.0);
    if (ar_status != AR_STATUS_OK) {
        sg_bootstrap_solution_reset(&initial);
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_repair(alns, "pair-sync", sg_repair_pair_sync, ctx, 1.0);
    if (ar_status != AR_STATUS_OK) {
        sg_bootstrap_solution_reset(&initial);
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_add_repair(alns, "bootstrap-repair", sg_repair_greedy_insertion, ctx, 0.5);
    if (ar_status != AR_STATUS_OK) {
        sg_bootstrap_solution_reset(&initial);
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_status = ar_alns_solve(alns, &initial, (void **)&best);
    if (ar_status != AR_STATUS_OK && ar_status != AR_STATUS_LIMIT) {
        sg_bootstrap_solution_reset(&initial);
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }

    ar_alns_get_stats(alns, &ar_stats);
    sg_copy_operator_stats(ctx, alns);
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
