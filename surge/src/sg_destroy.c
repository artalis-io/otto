#include "sg_internal.h"

ARStatus sg_unassign_removed_requests(SGBootstrapSolution *sol,
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

ARStatus sg_destroy_random(void *op_ctx, void *solution, int count,
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

ARStatus sg_destroy_worst(void *op_ctx, void *solution, int count,
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

ARStatus sg_destroy_shaw(void *op_ctx, void *solution, int count,
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

ARStatus sg_destroy_criticality_worst(void *op_ctx, void *solution, int count,
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

ARStatus sg_destroy_route_cluster(void *op_ctx, void *solution, int count,
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

ARStatus sg_destroy_time_cluster(void *op_ctx, void *solution, int count,
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

ARStatus sg_destroy_paired_shaw(void *op_ctx, void *solution, int count,
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

ARStatus sg_route_destroy_random(void *op_ctx, void *solution, int count,
                                 uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
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

    return sg_route_unassign_removed_requests(ctx, sol, removed_ids, *removed_count);
}

ARStatus sg_route_destroy_worst(void *op_ctx, void *solution, int count,
                                uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
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

    return sg_route_unassign_removed_requests(ctx, sol, removed_ids, *removed_count);
}

ARStatus sg_route_destroy_shaw(void *op_ctx, void *solution, int count,
                               uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
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

    return sg_route_unassign_removed_requests(ctx, sol, removed_ids, *removed_count);
}

ARStatus sg_route_destroy_criticality_worst(void *op_ctx, void *solution, int count,
                                            uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
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

    return sg_route_unassign_removed_requests(ctx, sol, removed_ids, *removed_count);
}

ARStatus sg_route_destroy_route_cluster(void *op_ctx, void *solution, int count,
                                        uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
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

    return sg_route_unassign_removed_requests(ctx, sol, removed_ids, *removed_count);
}

ARStatus sg_route_destroy_time_cluster(void *op_ctx, void *solution, int count,
                                       uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
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

    return sg_route_unassign_removed_requests(ctx, sol, removed_ids, *removed_count);
}

ARStatus sg_route_destroy_route_removal(void *op_ctx, void *solution, int count,
                                        uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    int target;
    int total_removed = 0;

    if (!ctx || !ctx->op_rng || !sol || !removed_count || count < 0) {
        return AR_STATUS_INVALID_ARG;
    }

    *removed_count = 0;
    if (count == 0 || sol->base.num_assigned == 0) {
        return AR_STATUS_OK;
    }
    if (!removed_ids) {
        return AR_STATUS_INVALID_ARG;
    }

    target = count;
    if ((uint32_t)target > sol->base.num_assigned) {
        target = (int)sol->base.num_assigned;
    }

    while (total_removed < target) {
        uint32_t selected_vehicle = UINT32_MAX;
        uint32_t seen_nonempty = 0;
        uint32_t route_len;
        uint32_t *route_snapshot;
        uint32_t v;
        int take;
        int i;
        ARStatus status;

        for (v = 0; v < sol->num_vehicles; v++) {
            if (sol->route_lengths[v] == 0) {
                continue;
            }
            seen_nonempty++;
            if (seen_nonempty == 1 ||
                sh_rng_int_range(ctx->op_rng, 0, (int)seen_nonempty - 1) == 0) {
                selected_vehicle = v;
            }
        }

        if (selected_vehicle == UINT32_MAX) {
            break;
        }

        route_len = sol->route_lengths[selected_vehicle];
        if (route_len == 0) {
            continue;
        }

        route_snapshot = (uint32_t *)malloc((size_t)route_len * sizeof(uint32_t));
        if (!route_snapshot) {
            return AR_STATUS_OUT_OF_MEMORY;
        }
        memcpy(route_snapshot, sg_route_vehicle_ptr_const(sol, selected_vehicle),
               (size_t)route_len * sizeof(uint32_t));

        take = target - total_removed;
        if ((uint32_t)take > route_len) {
            take = (int)route_len;
        }

        for (i = 0; i < take; i++) {
            int j = sh_rng_int_range(ctx->op_rng, i, (int)route_len - 1);
            uint32_t tmp = route_snapshot[i];
            route_snapshot[i] = route_snapshot[j];
            route_snapshot[j] = tmp;
            removed_ids[total_removed + i] = route_snapshot[i];
        }

        status = sg_route_unassign_removed_requests(ctx, sol, &removed_ids[total_removed], take);
        free(route_snapshot);
        if (status != AR_STATUS_OK) {
            return status;
        }

        total_removed += take;
    }

    *removed_count = total_removed;
    return AR_STATUS_OK;
}

ARStatus sg_route_destroy_time_window(void *op_ctx, void *solution, int count,
                                      uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    ARStatus status;

    if (!ctx || !ctx->op_rng || !sol || !removed_count || count < 0) {
        return AR_STATUS_INVALID_ARG;
    }

    status = ar_remove_related(ctx->op_rng, ctx, sol, count, removed_ids,
                               sg_get_assigned_count, sg_get_assigned_element,
                               sg_time_window_relatedness, SG_TIME_CLUSTER_RANDOMNESS,
                               NULL, removed_count);
    if (status != AR_STATUS_OK) {
        return status;
    }

    return sg_route_unassign_removed_requests(ctx, sol, removed_ids, *removed_count);
}

ARStatus sg_route_destroy_paired_shaw(void *op_ctx, void *solution, int count,
                                      uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
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

    return sg_route_unassign_removed_requests(ctx, sol, removed_ids, *removed_count);
}
