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
                             sg_route_removal_cost, SG_WORST_RANDOMNESS,
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

    ctx->active_solution = sol;
    status = ar_remove_related(ctx->op_rng, ctx, sol, count, removed_ids,
                               sg_get_assigned_count, sg_get_assigned_element,
                               sg_route_shaw_relatedness, SG_ROUTE_SHAW_RANDOMNESS,
                               NULL, removed_count);
    ctx->active_solution = NULL;
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

ARStatus sg_route_destroy_vehicle_target(void *op_ctx, void *solution, int count,
                                         uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    uint32_t target_vehicle = UINT32_MAX;
    uint32_t min_len = UINT32_MAX;
    uint32_t v;
    int total_removed = 0;
    int target;
    uint32_t i;

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

    /* Step 1: Find the non-empty vehicle with fewest requests (tie-break: lowest ID). */
    for (v = 0; v < sol->num_vehicles; v++) {
        uint32_t len = sol->route_lengths[v];
        if (len > 0 && (len < min_len || (len == min_len && v < target_vehicle))) {
            min_len = len;
            target_vehicle = v;
        }
    }
    if (target_vehicle == UINT32_MAX) {
        return AR_STATUS_OK;
    }

    /* Step 2: Remove ALL requests from the target vehicle. */
    {
        const uint32_t *route = sg_route_vehicle_ptr_const(sol, target_vehicle);
        uint32_t take = min_len;
        if ((int)take > target) {
            take = (uint32_t)target;
        }
        for (i = 0; i < take; i++) {
            removed_ids[total_removed++] = route[i];
        }
    }

    /* Step 3: Fill remaining quota with Shaw-related requests from other vehicles.
       These removals create insertion slots on destination vehicles. */
    if (total_removed < target && total_removed > 0) {
        ctx->active_solution = sol;
        for (i = 0; i < (uint32_t)total_removed && total_removed < target; i++) {
            uint32_t seed_id = removed_ids[i];
            uint32_t best_id = UINT32_MAX;
            double best_rel = INFINITY;
            uint32_t j;

            for (j = 0; j < sol->base.num_assigned; j++) {
                uint32_t cand = sol->base.assigned_ids[j];
                uint32_t k;
                int already_removed = 0;
                double rel;

                /* Skip requests on the target vehicle. */
                if (sol->request_vehicle[cand] == target_vehicle) {
                    continue;
                }
                /* Skip already-removed requests. */
                for (k = 0; k < (uint32_t)total_removed; k++) {
                    if (removed_ids[k] == cand) {
                        already_removed = 1;
                        break;
                    }
                }
                if (already_removed) {
                    continue;
                }

                rel = sg_route_shaw_relatedness(ctx, seed_id, cand);
                if (rel < best_rel) {
                    best_rel = rel;
                    best_id = cand;
                }
            }

            if (best_id != UINT32_MAX) {
                removed_ids[total_removed++] = best_id;
            }
        }
        ctx->active_solution = NULL;
    }

    *removed_count = total_removed;
    return sg_route_unassign_removed_requests(ctx, sol, removed_ids, *removed_count);
}

ARStatus sg_route_destroy_vehicle_empty(void *op_ctx, void *solution, int count,
                                        uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    uint32_t target_vehicle = UINT32_MAX;
    uint32_t v;
    int total_removed = 0;
    int target;
    uint32_t i;
    uint32_t num_nonempty = 0;
    uint32_t candidates[3];
    uint32_t cand_lens[3];
    uint32_t k_cands;

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

    /* Collect non-empty vehicles sorted by route length ascending (insertion sort). */
    {
        uint32_t sorted[256]; /* vehicle indices */
        uint32_t sorted_len[256];
        uint32_t n_sorted = 0;

        for (v = 0; v < sol->num_vehicles && n_sorted < 256; v++) {
            if (sol->route_lengths[v] == 0) continue;
            /* Insert in sorted order by length ascending, then vehicle id ascending. */
            {
                uint32_t j = n_sorted;
                while (j > 0 && (sol->route_lengths[v] < sorted_len[j - 1] ||
                       (sol->route_lengths[v] == sorted_len[j - 1] && v < sorted[j - 1]))) {
                    sorted[j] = sorted[j - 1];
                    sorted_len[j] = sorted_len[j - 1];
                    j--;
                }
                sorted[j] = v;
                sorted_len[j] = sol->route_lengths[v];
            }
            n_sorted++;
        }
        num_nonempty = n_sorted;
        if (num_nonempty == 0) {
            return AR_STATUS_OK;
        }

        /* Pick from bottom K candidates. */
        k_cands = num_nonempty < 3 ? num_nonempty : 3;
        for (i = 0; i < k_cands; i++) {
            candidates[i] = sorted[i];
            cand_lens[i] = sorted_len[i];
        }
    }

    {
        uint32_t pick = (uint32_t)(sh_rng_next_u64(ctx->op_rng) % k_cands);
        target_vehicle = candidates[pick];
    }

    /* Remove ALL requests from chosen vehicle. */
    {
        const uint32_t *route = sg_route_vehicle_ptr_const(sol, target_vehicle);
        uint32_t route_len = sol->route_lengths[target_vehicle];
        uint32_t take = route_len;
        if ((int)take > target) {
            take = (uint32_t)target;
        }
        for (i = 0; i < take; i++) {
            removed_ids[total_removed++] = route[i];
        }
    }

    /* Fill remaining quota with Shaw-related requests from other vehicles. */
    if (total_removed < target && total_removed > 0) {
        ctx->active_solution = sol;
        for (i = 0; i < (uint32_t)total_removed && total_removed < target; i++) {
            uint32_t seed_id = removed_ids[i];
            uint32_t best_id = UINT32_MAX;
            double best_rel = INFINITY;
            uint32_t j;

            for (j = 0; j < sol->base.num_assigned; j++) {
                uint32_t cand = sol->base.assigned_ids[j];
                uint32_t kk;
                int already_removed = 0;
                double rel;

                if (sol->request_vehicle[cand] == target_vehicle) {
                    continue;
                }
                for (kk = 0; kk < (uint32_t)total_removed; kk++) {
                    if (removed_ids[kk] == cand) {
                        already_removed = 1;
                        break;
                    }
                }
                if (already_removed) {
                    continue;
                }

                rel = sg_route_shaw_relatedness(ctx, seed_id, cand);
                if (rel < best_rel) {
                    best_rel = rel;
                    best_id = cand;
                }
            }

            if (best_id != UINT32_MAX) {
                removed_ids[total_removed++] = best_id;
            }
        }
        ctx->active_solution = NULL;
    }

    *removed_count = total_removed;
    return sg_route_unassign_removed_requests(ctx, sol, removed_ids, *removed_count);
}

ARStatus sg_route_destroy_string(void *op_ctx, void *solution, int count,
                                 uint32_t *removed_ids, int *removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    int target;
    int total_removed = 0;
    uint8_t visited_stack[256];
    uint8_t *visited;
    int visited_heap = 0;

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

    /* Vehicle visited tracking */
    if (sol->num_vehicles <= 256) {
        visited = visited_stack;
    } else {
        visited = (uint8_t *)calloc(sol->num_vehicles, 1);
        if (!visited) {
            return AR_STATUS_OUT_OF_MEMORY;
        }
        visited_heap = 1;
    }
    if (!visited_heap) {
        memset(visited, 0, sol->num_vehicles);
    }

    /* Step 1: Pick random seed request */
    {
        uint32_t seed_idx = (uint32_t)sh_rng_int_range(ctx->op_rng, 0,
                                (int)sol->base.num_assigned - 1);
        uint32_t seed_id = sol->base.assigned_ids[seed_idx];
        uint32_t seed_vehicle = sol->request_vehicle[seed_id];
        uint32_t route_len = sol->route_lengths[seed_vehicle];
        uint32_t center_pos = sol->request_pos[seed_id];
        int L = sh_rng_int_range(ctx->op_rng, 1, SG_STRING_L_MAX);
        int take;
        uint32_t start, end;
        const uint32_t *route;
        int i;

        if (L > target) L = target;
        if ((uint32_t)L > route_len) L = (int)route_len;
        take = L;

        start = (center_pos >= (uint32_t)(take / 2))
                ? center_pos - (uint32_t)(take / 2) : 0;
        end = start + (uint32_t)take;
        if (end > route_len) {
            end = route_len;
            start = end - (uint32_t)take;
        }

        /* Snapshot substring before unassign */
        route = sg_route_vehicle_ptr_const(sol, seed_vehicle);
        for (i = 0; i < take; i++) {
            removed_ids[total_removed + i] = route[start + (uint32_t)i];
        }
        {
            ARStatus status = sg_route_unassign_removed_requests(
                ctx, sol, &removed_ids[total_removed], take);
            if (status != AR_STATUS_OK) {
                if (visited_heap) free(visited);
                return status;
            }
        }
        total_removed += take;
        visited[seed_vehicle] = 1;
    }

    /* Step 2: Cross-route loop */
    while (total_removed < target) {
        uint32_t best_id = UINT32_MAX;
        uint32_t best_vehicle = UINT32_MAX;
        double best_dist = INFINITY;
        uint32_t j;

        /* Find nearest unremoved request on a non-visited vehicle */
        for (j = 0; j < sol->base.num_assigned; j++) {
            uint32_t cand = sol->base.assigned_ids[j];
            uint32_t cand_v = sol->request_vehicle[cand];
            double cx, cy;
            double min_d = INFINITY;
            int r;

            if (visited[cand_v]) continue;

            sg_request_centroid(ctx, cand, &cx, &cy);
            for (r = 0; r < total_removed; r++) {
                double rx, ry, d;
                sg_request_centroid(ctx, removed_ids[r], &rx, &ry);
                d = sg_euclid(rx, ry, cx, cy);
                if (d < min_d) min_d = d;
            }

            if (min_d < best_dist) {
                best_dist = min_d;
                best_id = cand;
                best_vehicle = cand_v;
            }
        }

        if (best_id == UINT32_MAX) break;

        /* Extract string from best_vehicle centered on best_id */
        {
            uint32_t route_len = sol->route_lengths[best_vehicle];
            uint32_t center_pos = sol->request_pos[best_id];
            int remaining = target - total_removed;
            int L = sh_rng_int_range(ctx->op_rng, 1, SG_STRING_L_MAX);
            int take;
            uint32_t start, end;
            const uint32_t *route;
            int i;
            ARStatus status;

            if (L > remaining) L = remaining;
            if ((uint32_t)L > route_len) L = (int)route_len;
            take = L;

            start = (center_pos >= (uint32_t)(take / 2))
                    ? center_pos - (uint32_t)(take / 2) : 0;
            end = start + (uint32_t)take;
            if (end > route_len) {
                end = route_len;
                start = end - (uint32_t)take;
            }

            route = sg_route_vehicle_ptr_const(sol, best_vehicle);
            for (i = 0; i < take; i++) {
                removed_ids[total_removed + i] = route[start + (uint32_t)i];
            }

            status = sg_route_unassign_removed_requests(
                ctx, sol, &removed_ids[total_removed], take);
            if (status != AR_STATUS_OK) {
                if (visited_heap) free(visited);
                return status;
            }
            total_removed += take;
            visited[best_vehicle] = 1;
        }
    }

    if (visited_heap) free(visited);
    *removed_count = total_removed;
    return AR_STATUS_OK;
}
