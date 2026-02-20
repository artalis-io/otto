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

    ar_alns_params_default(&params);
    params.max_iterations = ctx->config.max_iterations;
    params.max_time_seconds = ctx->config.max_time_seconds;
    params.segment_size = ctx->config.segment_size;
    sg_adaptive_q_bounds((int)ctx->num_requests, ctx->config.q_min, ctx->config.q_max,
                         &params.q_min, &params.q_max);
    params.target_cost = 0.0;
    /* Calibrate SA temperature from distance, not total cost. The cost
       function includes large vehicle/unassigned penalties (~1e6/1e9) that
       would make the temperature absurdly hot. Distance deltas are the
       typical move magnitude during the improvement phase. */
    ar_alns_calibrate_sa(&params,
                          initial.total_distance > 0.0
                              ? initial.total_distance
                              : sg_route_solution_cost(&initial, ctx),
                          params.max_iterations);

    ops.copy = sg_route_solution_copy;
    ops.free = sg_route_solution_free;
    ops.cost = sg_route_solution_cost;
    ops.size = sg_route_solution_size;
    ops.validate = sg_route_solution_validate;
    ops.user_ctx = ctx;

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
        ar_alns_add_destroy(alns, "shaw", sg_route_destroy_shaw, ctx, 1.0) != AR_STATUS_OK ||
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
