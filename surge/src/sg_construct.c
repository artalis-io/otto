#include "sg_internal.h"

static int sg_construct_eval_vehicle_request(const SGContext *ctx, const SGConstructState *state,
                                             uint32_t vehicle_id, uint32_t request_id,
                                             double *score_out, double *time_use_out) {
    uint32_t d;
    double time_use = 0.0;
    double score;

    if (!ctx || !state || vehicle_id >= ctx->num_vehicles || !score_out) {
        return 0;
    }
    if (!state->remaining_capacity || !state->remaining_time_seconds) {
        return 0;
    }
    if (request_id < ctx->num_requests && !sg_vehicle_qualifies(ctx, vehicle_id, request_id)) {
        return 0;
    }
    if (request_id < ctx->num_requests && !sg_vehicle_allowed_for_request(ctx, vehicle_id, request_id)) {
        return 0;
    }
    if (request_id < ctx->num_requests && !sg_construct_commodity_compatible(ctx, state, vehicle_id, request_id)) {
        return 0;
    }
    if (request_id < ctx->num_requests && !sg_construct_exclusion_compatible(ctx, state, vehicle_id, request_id)) {
        return 0;
    }

    if (!sg_request_time_use_for_vehicle(ctx, vehicle_id, request_id, &time_use)) {
        return 0;
    }
    if (time_use > state->remaining_time_seconds[vehicle_id] + 1e-9) {
        return 0;
    }

    for (d = 0; d < ctx->dimension_count; d++) {
        double req_demand = sg_request_abs_demand_at_dim(ctx, request_id, d);
        double remaining = state->remaining_capacity[(size_t)vehicle_id *
                                                     (size_t)ctx->dimension_count + (size_t)d];
        if (req_demand > remaining + SG_DEMAND_TOLERANCE) {
            return 0;
        }
    }

    score = sg_vehicle_request_cost(ctx, vehicle_id, request_id, 0.0);
    score += time_use / 3600.0;

    /* Soft penalty for vehicles at constrained depots */
    if (ctx->has_depot_capacity && state->depot_vehicle_count) {
        const SGVehicleRecord *vehicle = &ctx->vehicles[vehicle_id];
        if (vehicle->has_depots) {
            uint32_t start_depot = vehicle->start_depot_id;
            if (start_depot < ctx->num_depots &&
                ctx->depots[start_depot].max_simultaneous > 0) {
                uint32_t count = state->depot_vehicle_count[start_depot];
                uint32_t cap = ctx->depots[start_depot].max_simultaneous;
                if (count >= cap) {
                    score += SG_DEPOT_CAPACITY_PENALTY * (double)(count - cap + 1);
                }
            }
        }
    }

    *score_out = score;
    if (time_use_out) {
        *time_use_out = time_use;
    }
    return 1;
}

static ARStatus sg_construct_assign_request(SGContext *ctx, SGConstructState *state,
                                            SGBootstrapSolution *sol, uint32_t request_id,
                                            uint32_t vehicle_id) {
    uint32_t d;
    double time_use = 0.0;
    double ignored = 0.0;
    ARStatus status;

    if (!ctx || !state || !sol || vehicle_id >= ctx->num_vehicles) {
        return AR_STATUS_INVALID_ARG;
    }
    if (!sg_construct_eval_vehicle_request(ctx, state, vehicle_id, request_id,
                                           &ignored, &time_use)) {
        return AR_STATUS_INVALID_ARG;
    }

    for (d = 0; d < ctx->dimension_count; d++) {
        size_t idx = (size_t)vehicle_id * (size_t)ctx->dimension_count + (size_t)d;
        state->remaining_capacity[idx] -= sg_request_abs_demand_at_dim(ctx, request_id, d);
        if (state->remaining_capacity[idx] < 0.0 &&
            state->remaining_capacity[idx] > -SG_DEMAND_TOLERANCE) {
            state->remaining_capacity[idx] = 0.0;
        }
    }
    state->remaining_time_seconds[vehicle_id] -= time_use;
    if (state->remaining_time_seconds[vehicle_id] < 0.0 &&
        state->remaining_time_seconds[vehicle_id] > -1e-9) {
        state->remaining_time_seconds[vehicle_id] = 0.0;
    }

    /* Track depot vehicle counts for capacity awareness */
    if (state->depot_vehicle_count) {
        const SGVehicleRecord *vehicle = &ctx->vehicles[vehicle_id];
        if (vehicle->has_depots && vehicle->start_depot_id < ctx->num_depots) {
            state->depot_vehicle_count[vehicle->start_depot_id]++;
        }
    }

    /* Update commodity tracking */
    if (state->construct_commodities) {
        uint32_t cid = ctx->requests[request_id].commodity_id;
        if (cid > 0) {
            state->construct_commodities[vehicle_id] |= (1ULL << (cid - 1));
        }
    }
    /* Update exclusion tracking */
    if (state->construct_exclusion_counts) {
        const SGRequestRecord *req = &ctx->requests[request_id];
        uint16_t g;
        for (g = 0; g < req->num_exclusion_groups; g++) {
            uint32_t gid = req->exclusion_group_ids[g];
            state->construct_exclusion_counts[(size_t)vehicle_id * ctx->num_exclusion_groups + gid]++;
        }
    }

    status = sg_bootstrap_assign_request(sol, request_id);
    return status;
}

ARStatus sg_construct_state_init(const SGContext *ctx, SGConstructState *state) {
    size_t total_caps;
    uint32_t v;
    uint32_t d;

    if (!ctx || !state) {
        return AR_STATUS_INVALID_ARG;
    }
    memset(state, 0, sizeof(*state));

    if (ctx->num_vehicles == 0 || ctx->dimension_count == 0) {
        return AR_STATUS_OK;
    }

    if ((size_t)ctx->num_vehicles > SIZE_MAX / (size_t)ctx->dimension_count) {
        return AR_STATUS_OUT_OF_MEMORY;
    }
    total_caps = (size_t)ctx->num_vehicles * (size_t)ctx->dimension_count;

    state->remaining_capacity = (double *)malloc(total_caps * sizeof(double));
    state->remaining_time_seconds = (double *)malloc((size_t)ctx->num_vehicles * sizeof(double));
    if (ctx->has_depot_capacity && ctx->num_depots > 0) {
        state->depot_vehicle_count = (uint32_t *)calloc((size_t)ctx->num_depots, sizeof(uint32_t));
    }
    if (ctx->num_commodities > 0) {
        state->construct_commodities = (uint64_t *)calloc((size_t)ctx->num_vehicles, sizeof(uint64_t));
    }
    if (ctx->num_exclusion_groups > 0) {
        state->construct_exclusion_counts = (uint32_t *)calloc(
            (size_t)ctx->num_vehicles * (size_t)ctx->num_exclusion_groups, sizeof(uint32_t));
    }

    if (!state->remaining_capacity || !state->remaining_time_seconds ||
        (ctx->has_depot_capacity && ctx->num_depots > 0 && !state->depot_vehicle_count) ||
        (ctx->num_commodities > 0 && !state->construct_commodities) ||
        (ctx->num_exclusion_groups > 0 && !state->construct_exclusion_counts)) {
        free(state->remaining_capacity);
        free(state->remaining_time_seconds);
        free(state->depot_vehicle_count);
        free(state->construct_commodities);
        free(state->construct_exclusion_counts);
        memset(state, 0, sizeof(*state));
        return AR_STATUS_OUT_OF_MEMORY;
    }

    for (v = 0; v < ctx->num_vehicles; v++) {
        const SGVehicleRecord *vehicle = &ctx->vehicles[v];
        {
            double shift_span = vehicle->has_shift_time_window
                                ? (double)(vehicle->shift_late - vehicle->shift_early)
                                : INFINITY;
            if (vehicle->max_duration_seconds > 0 && (double)vehicle->max_duration_seconds < shift_span) {
                state->remaining_time_seconds[v] = (double)vehicle->max_duration_seconds;
            } else {
                state->remaining_time_seconds[v] = shift_span;
            }
        }

        for (d = 0; d < ctx->dimension_count; d++) {
            double cap = (vehicle->has_capacity && vehicle->capacity)
                         ? vehicle->capacity[d]
                         : INFINITY;
            state->remaining_capacity[(size_t)v * (size_t)ctx->dimension_count + (size_t)d] = cap;
        }
    }

    return AR_STATUS_OK;
}

void sg_construct_state_reset(SGConstructState *state) {
    if (!state) {
        return;
    }
    free(state->remaining_capacity);
    free(state->remaining_time_seconds);
    free(state->depot_vehicle_count);
    free(state->construct_commodities);
    free(state->construct_exclusion_counts);
    memset(state, 0, sizeof(*state));
}

int sg_construct_select_regret_request(const SGContext *ctx, const SGConstructState *state,
                                       const SGBootstrapSolution *sol, int regret_k,
                                       uint32_t *request_id_out,
                                       uint32_t *vehicle_id_out) {
    uint32_t request_id;
    uint32_t best_request = UINT32_MAX;
    uint32_t best_vehicle = UINT32_MAX;
    double best_regret = -INFINITY;
    double best_first_cost = INFINITY;

    if (!ctx || !state || !sol || !request_id_out || !vehicle_id_out || regret_k < 1) {
        return 0;
    }

    for (request_id = 0; request_id < ctx->num_requests; request_id++) {
        uint32_t v;
        double ranked_costs[SG_CONSTRUCT_REGRET_K];
        uint32_t ranked_vehicles[SG_CONSTRUCT_REGRET_K];
        int ranked_count = 0;

        if (request_id >= sol->total_requests || sol->assigned_flags[request_id]) {
            continue;
        }

        for (v = 0; v < ctx->num_vehicles; v++) {
            double score = 0.0;
            if (sg_construct_eval_vehicle_request(ctx, state, v, request_id, &score, NULL)) {
                int insert_at = ranked_count;
                int j;

                if (insert_at > SG_CONSTRUCT_REGRET_K) {
                    insert_at = SG_CONSTRUCT_REGRET_K;
                }
                for (j = 0; j < ranked_count && j < SG_CONSTRUCT_REGRET_K; j++) {
                    if (score < ranked_costs[j]) {
                        insert_at = j;
                        break;
                    }
                }

                if (insert_at < SG_CONSTRUCT_REGRET_K) {
                    int limit = ranked_count < SG_CONSTRUCT_REGRET_K
                                ? ranked_count
                                : SG_CONSTRUCT_REGRET_K - 1;
                    for (j = limit; j > insert_at; j--) {
                        ranked_costs[j] = ranked_costs[j - 1];
                        ranked_vehicles[j] = ranked_vehicles[j - 1];
                    }
                    ranked_costs[insert_at] = score;
                    ranked_vehicles[insert_at] = v;
                }

                if (ranked_count < SG_CONSTRUCT_REGRET_K) {
                    ranked_count++;
                }
            }
        }

        if (ranked_count == 0) {
            continue;
        }

        {
            int k_index = regret_k - 1;
            double first_cost = ranked_costs[0];
            double kth_cost;
            double regret;
            if (k_index >= ranked_count) {
                k_index = ranked_count - 1;
            }
            kth_cost = ranked_costs[k_index];
            regret = kth_cost - first_cost;

            if (regret > best_regret ||
                (fabs(regret - best_regret) <= 1e-9 && first_cost < best_first_cost) ||
                (fabs(regret - best_regret) <= 1e-9 &&
                 fabs(first_cost - best_first_cost) <= 1e-9 &&
                 request_id < best_request)) {
                best_regret = regret;
                best_first_cost = first_cost;
                best_request = request_id;
                best_vehicle = ranked_vehicles[0];
            }
        }
    }

    if (best_request == UINT32_MAX || best_vehicle == UINT32_MAX) {
        return 0;
    }

    *request_id_out = best_request;
    *vehicle_id_out = best_vehicle;
    return 1;
}

ARStatus sg_construct_initial_solution(SGContext *ctx, SGBootstrapSolution *sol) {
    SGConstructState state;
    ARStatus status;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }

    status = sg_construct_state_init(ctx, &state);
    if (status != AR_STATUS_OK) {
        return status;
    }

    while (sol->num_unassigned > 0) {
        uint32_t request_id = UINT32_MAX;
        uint32_t vehicle_id = UINT32_MAX;
        if (!sg_construct_select_regret_request(ctx, &state, sol, SG_CONSTRUCT_REGRET_K,
                                                &request_id, &vehicle_id)) {
            break;
        }

        status = sg_construct_assign_request(ctx, &state, sol, request_id, vehicle_id);
        if (status != AR_STATUS_OK) {
            sg_construct_state_reset(&state);
            return status;
        }
    }

    sg_construct_state_reset(&state);
    return AR_STATUS_OK;
}
