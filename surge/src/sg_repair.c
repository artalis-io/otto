#include "sg_internal.h"

ARStatus sg_reinsert_removed_requests(SGBootstrapSolution *sol,
                                      const uint32_t *removed_ids,
                                      int removed_count) {
    ARStatus status;
    int i;

    if (!sol || removed_count < 0 || (removed_count > 0 && !removed_ids)) {
        return AR_STATUS_INVALID_ARG;
    }

    for (i = 0; i < removed_count; i++) {
        status = sg_bootstrap_assign_request(sol, removed_ids[i]);
        if (status != AR_STATUS_OK) {
            return status;
        }
    }

    return AR_STATUS_OK;
}

static uint32_t sg_select_extra_greedy(const SGContext *ctx, const SGBootstrapSolution *sol) {
    uint32_t i;
    uint32_t best_id = UINT32_MAX;
    double best_cost = INFINITY;

    if (!sol) {
        return UINT32_MAX;
    }

    for (i = 0; i < sol->num_unassigned; i++) {
        uint32_t id = sol->unassigned_ids[i];
        double cost;
        double ignored;
        if (!sg_request_best_k_costs(ctx, id, 1, 0.0, &cost, &ignored)) {
            continue;
        }
        if (cost < best_cost) {
            best_cost = cost;
            best_id = id;
        }
    }

    return best_id;
}

static uint32_t sg_select_extra_regret(const SGContext *ctx, const SGBootstrapSolution *sol,
                                       int regret_k, double noise_scale) {
    uint32_t i;
    uint32_t best_id = UINT32_MAX;
    double best_regret = -INFINITY;
    double best_first_cost = INFINITY;

    if (!sol || regret_k <= 1) {
        return UINT32_MAX;
    }

    for (i = 0; i < sol->num_unassigned; i++) {
        uint32_t id = sol->unassigned_ids[i];
        double first = 0.0;
        double kth = 0.0;
        double regret;
        if (!sg_request_best_k_costs(ctx, id, regret_k, noise_scale, &first, &kth)) {
            continue;
        }
        regret = kth - first;
        if (regret > best_regret ||
            (fabs(regret - best_regret) <= 1e-9 && first < best_first_cost)) {
            best_regret = regret;
            best_first_cost = first;
            best_id = id;
        }
    }

    return best_id;
}

static uint32_t sg_select_extra_pair_sync(const SGContext *ctx, const SGBootstrapSolution *sol) {
    uint32_t i;
    uint32_t best_id = UINT32_MAX;
    double best_cost = INFINITY;

    if (!sol) {
        return UINT32_MAX;
    }

    for (i = 0; i < sol->num_unassigned; i++) {
        uint32_t id = sol->unassigned_ids[i];
        if (sg_request_kind(ctx, id) == SG_REQUEST_KIND_PICKUP_DELIVERY) {
            double cost;
            double ignored;
            if (!sg_request_best_k_costs(ctx, id, 1, 0.0, &cost, &ignored)) {
                continue;
            }
            if (cost < best_cost) {
                best_cost = cost;
                best_id = id;
            }
        }
    }

    if (best_id != UINT32_MAX) {
        return best_id;
    }
    return sg_select_extra_greedy(ctx, sol);
}

static ARStatus sg_repair_with_selector(SGContext *ctx, SGBootstrapSolution *sol,
                                        const uint32_t *removed_ids, int removed_count,
                                        uint32_t (*selector)(const SGContext *,
                                                             const SGBootstrapSolution *)) {
    ARStatus status;
    uint32_t chosen;

    if (!ctx || !sol || !selector) {
        return AR_STATUS_INVALID_ARG;
    }

    status = sg_reinsert_removed_requests(sol, removed_ids, removed_count);
    if (status != AR_STATUS_OK) {
        return status;
    }

    if (sol->num_unassigned == 0) {
        return AR_STATUS_OK;
    }

    chosen = selector(ctx, sol);
    if (chosen == UINT32_MAX) {
        chosen = sol->unassigned_ids[0];
    }
    return sg_bootstrap_assign_request(sol, chosen);
}

ARStatus sg_repair_greedy_insertion(void *op_ctx, void *solution,
                                    const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    return sg_repair_with_selector(ctx, sol, removed_ids, removed_count, sg_select_extra_greedy);
}

ARStatus sg_repair_regret2(void *op_ctx, void *solution,
                           const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    ARStatus status;
    uint32_t chosen;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }
    status = sg_reinsert_removed_requests(sol, removed_ids, removed_count);
    if (status != AR_STATUS_OK || sol->num_unassigned == 0) {
        return status;
    }
    chosen = sg_select_extra_regret(ctx, sol, 2, 0.0);
    if (chosen == UINT32_MAX) {
        chosen = sol->unassigned_ids[0];
    }
    return sg_bootstrap_assign_request(sol, chosen);
}

ARStatus sg_repair_regret3(void *op_ctx, void *solution,
                           const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    ARStatus status;
    uint32_t chosen;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }
    status = sg_reinsert_removed_requests(sol, removed_ids, removed_count);
    if (status != AR_STATUS_OK || sol->num_unassigned == 0) {
        return status;
    }
    chosen = sg_select_extra_regret(ctx, sol, 3, 0.0);
    if (chosen == UINT32_MAX) {
        chosen = sol->unassigned_ids[0];
    }
    return sg_bootstrap_assign_request(sol, chosen);
}

ARStatus sg_repair_regret4(void *op_ctx, void *solution,
                           const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    ARStatus status;
    uint32_t chosen;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }
    status = sg_reinsert_removed_requests(sol, removed_ids, removed_count);
    if (status != AR_STATUS_OK || sol->num_unassigned == 0) {
        return status;
    }
    chosen = sg_select_extra_regret(ctx, sol, 4, 0.0);
    if (chosen == UINT32_MAX) {
        chosen = sol->unassigned_ids[0];
    }
    return sg_bootstrap_assign_request(sol, chosen);
}

ARStatus sg_repair_noise_regret(void *op_ctx, void *solution,
                                const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    ARStatus status;
    uint32_t chosen;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }
    status = sg_reinsert_removed_requests(sol, removed_ids, removed_count);
    if (status != AR_STATUS_OK || sol->num_unassigned == 0) {
        return status;
    }
    chosen = sg_select_extra_regret(ctx, sol, 3, SG_NOISE_REGRET_SCALE);
    if (chosen == UINT32_MAX) {
        chosen = sol->unassigned_ids[0];
    }
    return sg_bootstrap_assign_request(sol, chosen);
}

ARStatus sg_repair_pair_sync(void *op_ctx, void *solution,
                             const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGBootstrapSolution *sol = (SGBootstrapSolution *)solution;
    return sg_repair_with_selector(ctx, sol, removed_ids, removed_count, sg_select_extra_pair_sync);
}

int sg_route_rank_insertions_for_request(SGContext *ctx, const SGRouteSolution *sol,
                                         uint32_t request_id, int regret_k,
                                         double noise_scale, double *best_score_out,
                                         double *kth_score_out,
                                         uint32_t *best_vehicle_out,
                                         uint32_t *best_pos_out,
                                         double *best_route_distance_out) {
    double ranked_scores[SG_ROUTE_MAX_REGRET_K];
    double ranked_noisy[SG_ROUTE_MAX_REGRET_K];
    double ranked_distance[SG_ROUTE_MAX_REGRET_K];
    uint32_t ranked_vehicle[SG_ROUTE_MAX_REGRET_K];
    uint32_t ranked_pos[SG_ROUTE_MAX_REGRET_K];
    int ranked_count = 0;
    uint32_t v;
    int ok = 0;

    if (!ctx || !sol || !best_score_out || !kth_score_out || !best_vehicle_out ||
        !best_pos_out || !best_route_distance_out || request_id >= sol->base.total_requests ||
        regret_k < 1 || regret_k > SG_ROUTE_MAX_REGRET_K) {
        return 0;
    }

    for (v = 0; v < sol->num_vehicles; v++) {
        uint32_t len = sol->route_lengths[v];
        uint32_t pos;
        for (pos = 0; pos <= len; pos++) {
            double score = 0.0;
            double noisy = 0.0;
            double new_route_distance = 0.0;
            int insert_at = ranked_count;
            int j;

            if (!sg_route_eval_insertion_cached(ctx, sol, request_id, v, pos,
                                                &score, &new_route_distance)) {
                continue;
            }

            noisy = score;
            if (noise_scale > 0.0 && ctx->op_rng) {
                noisy *= (1.0 + sh_rng_uniform_range(ctx->op_rng, -noise_scale, noise_scale));
            }

            if (insert_at > SG_ROUTE_MAX_REGRET_K) {
                insert_at = SG_ROUTE_MAX_REGRET_K;
            }
            for (j = 0; j < ranked_count && j < SG_ROUTE_MAX_REGRET_K; j++) {
                if (noisy < ranked_noisy[j]) {
                    insert_at = j;
                    break;
                }
            }

            if (insert_at < SG_ROUTE_MAX_REGRET_K) {
                int limit = ranked_count < SG_ROUTE_MAX_REGRET_K
                            ? ranked_count
                            : SG_ROUTE_MAX_REGRET_K - 1;
                for (j = limit; j > insert_at; j--) {
                    ranked_noisy[j] = ranked_noisy[j - 1];
                    ranked_scores[j] = ranked_scores[j - 1];
                    ranked_distance[j] = ranked_distance[j - 1];
                    ranked_vehicle[j] = ranked_vehicle[j - 1];
                    ranked_pos[j] = ranked_pos[j - 1];
                }
                ranked_noisy[insert_at] = noisy;
                ranked_scores[insert_at] = score;
                ranked_distance[insert_at] = new_route_distance;
                ranked_vehicle[insert_at] = v;
                ranked_pos[insert_at] = pos;
            }

            if (ranked_count < SG_ROUTE_MAX_REGRET_K) {
                ranked_count++;
            }
        }
    }

    if (ranked_count > 0) {
        int k_index = regret_k - 1;
        if (k_index >= ranked_count) {
            k_index = ranked_count - 1;
        }

        *best_score_out = ranked_scores[0];
        *kth_score_out = ranked_scores[k_index];
        *best_vehicle_out = ranked_vehicle[0];
        *best_pos_out = ranked_pos[0];
        *best_route_distance_out = ranked_distance[0];
        ok = 1;
    }

    return ok;
}

static uint32_t sg_route_select_request_greedy(SGContext *ctx, const SGRouteSolution *sol,
                                               double noise_scale, uint32_t *best_vehicle_out,
                                               uint32_t *best_pos_out,
                                               double *best_route_distance_out) {
    uint32_t best_request = UINT32_MAX;
    double best_score = INFINITY;
    uint32_t i;

    if (!ctx || !sol || !best_vehicle_out || !best_pos_out || !best_route_distance_out) {
        return UINT32_MAX;
    }

    for (i = 0; i < sol->base.num_unassigned; i++) {
        uint32_t request_id = sol->base.unassigned_ids[i];
        double first_score = 0.0;
        double kth_score = 0.0;
        uint32_t vehicle_id = UINT32_MAX;
        uint32_t pos = UINT32_MAX;
        double route_distance = 0.0;
        if (!sg_route_rank_insertions_for_request(ctx, sol, request_id, 1, noise_scale,
                                                  &first_score, &kth_score,
                                                  &vehicle_id, &pos, &route_distance)) {
            continue;
        }

        if (first_score < best_score ||
            (fabs(first_score - best_score) <= 1e-9 && request_id < best_request)) {
            best_score = first_score;
            best_request = request_id;
            *best_vehicle_out = vehicle_id;
            *best_pos_out = pos;
            *best_route_distance_out = route_distance;
        }
    }

    return best_request;
}

static uint32_t sg_route_select_request_regret(SGContext *ctx, const SGRouteSolution *sol,
                                               int regret_k, double noise_scale,
                                               uint32_t *best_vehicle_out,
                                               uint32_t *best_pos_out,
                                               double *best_route_distance_out) {
    uint32_t best_request = UINT32_MAX;
    double best_regret = -INFINITY;
    double best_first_score = INFINITY;
    uint32_t i;

    if (!ctx || !sol || !best_vehicle_out || !best_pos_out || !best_route_distance_out ||
        regret_k < 1 || regret_k > SG_ROUTE_MAX_REGRET_K) {
        return UINT32_MAX;
    }

    for (i = 0; i < sol->base.num_unassigned; i++) {
        uint32_t request_id = sol->base.unassigned_ids[i];
        double first_score = 0.0;
        double kth_score = 0.0;
        double regret = 0.0;
        uint32_t vehicle_id = UINT32_MAX;
        uint32_t pos = UINT32_MAX;
        double route_distance = 0.0;
        if (!sg_route_rank_insertions_for_request(ctx, sol, request_id, regret_k, noise_scale,
                                                  &first_score, &kth_score,
                                                  &vehicle_id, &pos, &route_distance)) {
            continue;
        }

        regret = kth_score - first_score;
        if (regret > best_regret ||
            (fabs(regret - best_regret) <= 1e-9 && first_score < best_first_score) ||
            (fabs(regret - best_regret) <= 1e-9 &&
             fabs(first_score - best_first_score) <= 1e-9 &&
             request_id < best_request)) {
            best_regret = regret;
            best_first_score = first_score;
            best_request = request_id;
            *best_vehicle_out = vehicle_id;
            *best_pos_out = pos;
            *best_route_distance_out = route_distance;
        }
    }

    return best_request;
}

ARStatus sg_route_repair_fill_greedy(SGContext *ctx, SGRouteSolution *sol,
                                     double noise_scale) {
    while (sol->base.num_unassigned > 0) {
        uint32_t vehicle_id = UINT32_MAX;
        uint32_t pos = UINT32_MAX;
        double route_distance = 0.0;
        uint32_t request_id = sg_route_select_request_greedy(ctx, sol, noise_scale,
                                                             &vehicle_id, &pos, &route_distance);
        if (request_id == UINT32_MAX) {
            break;
        }
        if (sg_route_apply_insertion(ctx, sol, request_id, vehicle_id, pos,
                                     route_distance) != AR_STATUS_OK) {
            return AR_STATUS_ERROR;
        }
    }
    return AR_STATUS_OK;
}

ARStatus sg_route_repair_fill_regret(SGContext *ctx, SGRouteSolution *sol,
                                     int regret_k, double noise_scale) {
    while (sol->base.num_unassigned > 0) {
        uint32_t vehicle_id = UINT32_MAX;
        uint32_t pos = UINT32_MAX;
        double route_distance = 0.0;
        uint32_t request_id = sg_route_select_request_regret(ctx, sol, regret_k, noise_scale,
                                                             &vehicle_id, &pos, &route_distance);
        if (request_id == UINT32_MAX) {
            break;
        }
        if (sg_route_apply_insertion(ctx, sol, request_id, vehicle_id, pos,
                                     route_distance) != AR_STATUS_OK) {
            return AR_STATUS_ERROR;
        }
    }
    return AR_STATUS_OK;
}

ARStatus sg_route_repair_greedy(void *op_ctx, void *solution,
                                const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    (void)removed_ids;
    (void)removed_count;
    return sg_route_repair_fill_greedy(ctx, sol, 0.0);
}

ARStatus sg_route_repair_regret2(void *op_ctx, void *solution,
                                 const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    (void)removed_ids;
    (void)removed_count;
    return sg_route_repair_fill_regret(ctx, sol, 2, 0.0);
}

ARStatus sg_route_repair_regret3(void *op_ctx, void *solution,
                                 const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    (void)removed_ids;
    (void)removed_count;
    return sg_route_repair_fill_regret(ctx, sol, 3, 0.0);
}

ARStatus sg_route_repair_regret4(void *op_ctx, void *solution,
                                 const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    (void)removed_ids;
    (void)removed_count;
    return sg_route_repair_fill_regret(ctx, sol, 4, 0.0);
}

ARStatus sg_route_repair_noise_regret(void *op_ctx, void *solution,
                                      const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    (void)removed_ids;
    (void)removed_count;
    return sg_route_repair_fill_regret(ctx, sol, 3, SG_NOISE_REGRET_SCALE);
}

ARStatus sg_route_repair_pair_sync(void *op_ctx, void *solution,
                                   const uint32_t *removed_ids, int removed_count) {
    SGContext *ctx = (SGContext *)op_ctx;
    SGRouteSolution *sol = (SGRouteSolution *)solution;
    (void)removed_ids;
    (void)removed_count;
    return sg_route_repair_fill_greedy(ctx, sol, 0.0);
}
