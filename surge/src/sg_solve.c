#include "sg_internal.h"

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

static ARStatus sg_route_construct_tw_sorted(SGContext *ctx, SGRouteSolution *sol) {
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
            double sx, sy, ex, ey;
            uint32_t ui;

            if (sol->base.num_unassigned == 0) {
                break;
            }

            (void)sg_vehicle_start_end_locations(ctx, seed_vehicle, &sx, &sy, &ex, &ey);

            for (ui = 0; ui < sol->base.num_unassigned; ui++) {
                uint32_t rid = sol->base.unassigned_ids[ui];
                int is_pd = (ctx->requests[rid].kind == SG_REQUEST_KIND_PICKUP_DELIVERY);
                double old_dist = sol->route_distance[seed_vehicle];
                double c1, depot_dist, c2;
                double rx, ry;

                if (is_pd) {
                    double score;
                    uint32_t pp = UINT32_MAX, dp = UINT32_MAX;
                    double new_dist = 0.0;
                    double px, py, ddx, ddy;
                    if (!sg_route_eval_pd_best_insertion_cached(ctx, sol, rid,
                                                                 seed_vehicle, &score,
                                                                 &pp, &dp, &new_dist)) {
                        continue;
                    }
                    c1 = new_dist - old_dist;
                    px = ctx->tasks[ctx->requests[rid].pickup_task_id].x;
                    py = ctx->tasks[ctx->requests[rid].pickup_task_id].y;
                    ddx = ctx->tasks[ctx->requests[rid].delivery_task_id].x;
                    ddy = ctx->tasks[ctx->requests[rid].delivery_task_id].y;
                    depot_dist = sg_euclid(sx, sy, px, py) + sg_euclid(px, py, ddx, ddy) + sg_euclid(ddx, ddy, ex, ey);
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
                    (void)sg_request_centroid(ctx, rid, &rx, &ry);
                    depot_dist = sg_euclid(sx, sy, rx, ry) + sg_euclid(rx, ry, ex, ey);
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

static ARStatus sg_route_construct_initial_solution(SGContext *ctx, SGRouteSolution *sol) {
    SGRouteSolution alt;
    ARStatus status;

    /* Attempt 1: regret-3 */
    status = sg_route_repair_fill_regret(ctx, sol, 3, 0.0);
    if (status != AR_STATUS_OK) return status;

    /* Attempt 2: TW-sorted greedy */
    status = sg_route_solution_init(ctx, &alt);
    if (status != AR_STATUS_OK) return AR_STATUS_OK;

    status = sg_route_construct_tw_sorted(ctx, &alt);
    if (status != AR_STATUS_OK) {
        sg_route_solution_reset(&alt);
        return AR_STATUS_OK;
    }

    /* Keep whichever is better (lexicographic: unassigned, vehicles, distance) */
    if (alt.base.num_unassigned < sol->base.num_unassigned ||
        (alt.base.num_unassigned == sol->base.num_unassigned &&
         (alt.vehicles_used < sol->vehicles_used ||
          (alt.vehicles_used == sol->vehicles_used &&
           alt.total_distance < sol->total_distance - 1e-9)))) {
        sg_route_solution_reset(sol);
        *sol = alt;
    } else {
        sg_route_solution_reset(&alt);
    }

    /* Attempt 3: Solomon I1 */
    {
        SGRouteSolution i1;
        status = sg_route_solution_init(ctx, &i1);
        if (status == AR_STATUS_OK) {
            status = sg_route_construct_solomon_i1(ctx, &i1);
            if (status == AR_STATUS_OK &&
                (i1.base.num_unassigned < sol->base.num_unassigned ||
                 (i1.base.num_unassigned == sol->base.num_unassigned &&
                  (i1.vehicles_used < sol->vehicles_used ||
                   (i1.vehicles_used == sol->vehicles_used &&
                    i1.total_distance < sol->total_distance - 1e-9))))) {
                sg_route_solution_reset(sol);
                *sol = i1;
            } else {
                sg_route_solution_reset(&i1);
            }
        }
    }

    return AR_STATUS_OK;
}

static ARALNSContext *sg_create_route_alns(SGContext *ctx, ARALNSParams *params,
                                            ARSolutionOps *ops,
                                            double vehicle_target_weight,
                                            double vehicle_empty_weight) {
    ARALNSContext *alns;

    ops->copy = sg_route_solution_copy;
    ops->free = sg_route_solution_free;
    ops->cost = sg_route_solution_cost;
    ops->size = sg_route_solution_size;
    ops->validate = sg_route_solution_validate;
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

    if (!ctx) {
        return SG_STATUS_INVALID_ARG;
    }

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

    total_iters = ctx->config.max_iterations;
    phase1_iters = total_iters * 3 / 5;  /* 60% */
    if (phase1_iters < 500) phase1_iters = 500;
    if (phase1_iters > total_iters) phase1_iters = total_iters;
    phase2_iters = total_iters - phase1_iters;

    /* ---- Phase 1: Vehicle minimization ---- */
    ar_alns_params_default(&params);
    params.max_iterations = phase1_iters;
    params.max_time_seconds = ctx->config.max_time_seconds;
    params.segment_size = ctx->config.segment_size;
    sg_adaptive_q_bounds((int)ctx->num_requests, ctx->config.q_min, ctx->config.q_max,
                         &params.q_min, &params.q_max);
    params.target_cost = 0.0;
    params.restart_threshold = phase1_iters / 4;
    /* Calibrate from total cost (includes vehicle penalty ~1M per vehicle).
       This makes temperature ~1000x hotter so SA accepts distance-worsening
       moves that reduce vehicle count. */
    ar_alns_calibrate_sa(&params, sg_route_solution_cost(&initial, ctx), phase1_iters);

    alns = sg_create_route_alns(ctx, &params, &ops, 3.0, 2.0);
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

    ctx->avoid_new_vehicles = 1;
    ar_status = ar_alns_solve(alns, &initial, (void **)&p1_best);
    ctx->avoid_new_vehicles = 0;
    if (ar_status != AR_STATUS_OK && ar_status != AR_STATUS_LIMIT) {
        sg_route_solution_reset(&initial);
        ar_alns_free(alns);
        return SG_STATUS_ERROR;
    }
    ar_alns_get_stats(alns, &ar_stats);
    total_alns_iters += ar_stats.iterations;
    ar_alns_free(alns);
    alns = NULL;

    /* ---- Phase 2: Distance polishing ---- */
    if (phase2_iters > 0) {
        SGRouteSolution *p2_initial = p1_best ? p1_best : &initial;

        ar_alns_params_default(&params);
        params.max_iterations = phase2_iters;
        params.max_time_seconds = ctx->config.max_time_seconds;
        params.segment_size = ctx->config.segment_size;
        sg_adaptive_q_bounds((int)ctx->num_requests, ctx->config.q_min, ctx->config.q_max,
                             &params.q_min, &params.q_max);
        params.target_cost = 0.0;
        params.restart_threshold = phase2_iters / 4;
        /* Calibrate from distance — standard behavior for polishing. */
        ar_alns_calibrate_sa(&params,
                              p2_initial->total_distance > 0.0
                                  ? p2_initial->total_distance
                                  : sg_route_solution_cost(p2_initial, ctx),
                              phase2_iters);

        alns = sg_create_route_alns(ctx, &params, &ops, 1.5, 1.0);
        if (!alns) {
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

        ar_status = ar_alns_solve(alns, p2_initial, (void **)&p2_best);
        if (ar_status != AR_STATUS_OK && ar_status != AR_STATUS_LIMIT) {
            sg_route_solution_reset(&initial);
            sg_route_solution_free(p1_best, NULL);
            ar_alns_free(alns);
            return SG_STATUS_ERROR;
        }
        ar_alns_get_stats(alns, &ar_stats);
        total_alns_iters += ar_stats.iterations;
        ar_alns_free(alns);
        alns = NULL;
    }

    /* Determine best solution across phases. */
    {
        SGRouteSolution *best = p2_best ? p2_best : p1_best;
        if (best) {
            (void)sg_route_postprocess_reduce_vehicles(ctx, best);
            (void)sg_route_postprocess_ejection_reduce(ctx, best);
            (void)sg_route_postprocess_intensify(ctx, best);
            (void)sg_route_postprocess_polish_distance(ctx, best);
        } else {
            (void)sg_route_postprocess_reduce_vehicles(ctx, &initial);
            (void)sg_route_postprocess_ejection_reduce(ctx, &initial);
            (void)sg_route_postprocess_intensify(ctx, &initial);
            (void)sg_route_postprocess_polish_distance(ctx, &initial);
        }

        final_sol = best ? best : &initial;
        ctx->stats.iterations = total_alns_iters;
        ctx->stats.unassigned = final_sol->base.num_unassigned;
        ctx->stats.vehicles_used = final_sol->vehicles_used;
        ctx->stats.total_distance = final_sol->total_distance;
        ctx->stats.total_cost = sg_route_solution_cost(final_sol, ctx);
    }

    sg_route_solution_reset(&initial);
    sg_route_solution_free(p1_best, NULL);
    sg_route_solution_free(p2_best, NULL);

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
