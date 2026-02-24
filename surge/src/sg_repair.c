#include "sg_internal.h"

/* Flag bit to distinguish new-trip insertions from normal end-of-route insertions.
   OR'd into pos/pickup_pos/delivery_pos by rank_insertions, stripped by fill functions. */
#define SG_NEW_TRIP_BIT (1U << 31)

/* After a normal apply (which inserts with trip_start=0), mark the request as
   starting a new trip: set route_request_trip_start, set stop trip_start flag,
   and re-run timing/load to account for the depot return+reload+depart. */
static void sg_route_mark_new_trip(const SGContext *ctx, SGRouteSolution *sol,
                                    uint32_t request_id, uint32_t vehicle_id) {
    uint32_t req_pos;
    uint32_t stop_pos;
    double old_dist;

    if (!sol->route_request_trip_start) return;

    req_pos = sol->request_pos[request_id];
    sol->route_request_trip_start[(size_t)vehicle_id * sol->route_stride + req_pos] = 1;

    /* Set trip_start on the first stop of this request */
    if (ctx->requests[request_id].kind == SG_REQUEST_KIND_PICKUP_DELIVERY) {
        stop_pos = sol->request_pickup_stop_pos[request_id];
    } else {
        stop_pos = sol->request_delivery_stop_pos[request_id];
    }
    if (stop_pos < sol->route_stop_lengths[vehicle_id]) {
        SGRouteStop *stops = sg_route_vehicle_stop_ptr(sol, vehicle_id);
        stops[stop_pos].trip_start = 1;
    }

    /* Re-run timing and load to account for trip boundary */
    old_dist = sol->route_distance[vehicle_id];
    sg_route_update_timing(ctx, sol, vehicle_id);
    sg_route_update_load(ctx, sol, vehicle_id);
    sol->total_distance += (sol->route_distance[vehicle_id] - old_dist);
}

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

static void sg_rank_insert_candidate(
    double ranked_noisy[], double ranked_scores[], double ranked_distance[],
    uint32_t ranked_vehicle[], uint32_t ranked_pos[],
    uint32_t ranked_pickup_pos[], uint32_t ranked_delivery_pos[],
    int *ranked_count,
    double noisy, double score, double distance,
    uint32_t vehicle, uint32_t pos, uint32_t pickup_pos, uint32_t delivery_pos) {

    int insert_at = *ranked_count;
    int j;

    if (insert_at > SG_ROUTE_MAX_REGRET_K) {
        insert_at = SG_ROUTE_MAX_REGRET_K;
    }
    for (j = 0; j < *ranked_count && j < SG_ROUTE_MAX_REGRET_K; j++) {
        if (noisy < ranked_noisy[j]) {
            insert_at = j;
            break;
        }
    }

    if (insert_at < SG_ROUTE_MAX_REGRET_K) {
        int limit = *ranked_count < SG_ROUTE_MAX_REGRET_K
                    ? *ranked_count
                    : SG_ROUTE_MAX_REGRET_K - 1;
        for (j = limit; j > insert_at; j--) {
            ranked_noisy[j] = ranked_noisy[j - 1];
            ranked_scores[j] = ranked_scores[j - 1];
            ranked_distance[j] = ranked_distance[j - 1];
            ranked_vehicle[j] = ranked_vehicle[j - 1];
            ranked_pos[j] = ranked_pos[j - 1];
            ranked_pickup_pos[j] = ranked_pickup_pos[j - 1];
            ranked_delivery_pos[j] = ranked_delivery_pos[j - 1];
        }
        ranked_noisy[insert_at] = noisy;
        ranked_scores[insert_at] = score;
        ranked_distance[insert_at] = distance;
        ranked_vehicle[insert_at] = vehicle;
        ranked_pos[insert_at] = pos;
        ranked_pickup_pos[insert_at] = pickup_pos;
        ranked_delivery_pos[insert_at] = delivery_pos;
    }

    if (*ranked_count < SG_ROUTE_MAX_REGRET_K) {
        (*ranked_count)++;
    }
}

int sg_route_rank_insertions_for_request(SGContext *ctx, const SGRouteSolution *sol,
                                         uint32_t request_id, int regret_k,
                                         double noise_scale, double *best_score_out,
                                         double *kth_score_out,
                                         uint32_t *best_vehicle_out,
                                         uint32_t *best_pos_out,
                                         uint32_t *best_pickup_stop_pos_out,
                                         uint32_t *best_delivery_stop_pos_out,
                                         double *best_route_distance_out) {
    double ranked_scores[SG_ROUTE_MAX_REGRET_K];
    double ranked_noisy[SG_ROUTE_MAX_REGRET_K];
    double ranked_distance[SG_ROUTE_MAX_REGRET_K];
    uint32_t ranked_vehicle[SG_ROUTE_MAX_REGRET_K];
    uint32_t ranked_pos[SG_ROUTE_MAX_REGRET_K];
    uint32_t ranked_pickup_pos[SG_ROUTE_MAX_REGRET_K];
    uint32_t ranked_delivery_pos[SG_ROUTE_MAX_REGRET_K];
    int ranked_count = 0;
    uint32_t v;
    uint32_t frozen_designated;
    int ok = 0;
    int is_pd;

    if (!ctx || !sol || !best_score_out || !kth_score_out || !best_vehicle_out ||
        !best_pos_out || !best_pickup_stop_pos_out || !best_delivery_stop_pos_out ||
        !best_route_distance_out || request_id >= sol->base.total_requests ||
        regret_k < 1 || regret_k > SG_ROUTE_MAX_REGRET_K) {
        return 0;
    }

    is_pd = (ctx->requests[request_id].kind == SG_REQUEST_KIND_PICKUP_DELIVERY);
    frozen_designated = sg_frozen_designated_vehicle(ctx, request_id);

    for (v = 0; v < sol->num_vehicles; v++) {
        if (frozen_designated != SG_NO_VEHICLE && v != frozen_designated) continue;
        if (ctx->avoid_new_vehicles && sol->route_lengths[v] == 0) {
            continue;
        }
        if (ctx->vehicles[v].max_tasks > 0 && sol->route_lengths[v] >= ctx->vehicles[v].max_tasks) {
            continue;
        }
        if (is_pd) {
            /* O(L²) stop-level evaluation for PD requests */
            double score = 0.0;
            uint32_t pickup_pos = UINT32_MAX;
            uint32_t delivery_pos = UINT32_MAX;
            double new_route_distance = 0.0;
            double noisy;

            if (!sg_route_eval_pd_best_insertion_cached(ctx, sol, request_id, v,
                                                         &score, &pickup_pos,
                                                         &delivery_pos,
                                                         &new_route_distance)) {
                continue;
            }

            noisy = score;
            if (noise_scale > 0.0 && ctx->op_rng) {
                noisy *= (1.0 + sh_rng_uniform_range(ctx->op_rng, -noise_scale, noise_scale));
            }

            sg_rank_insert_candidate(ranked_noisy, ranked_scores, ranked_distance,
                                     ranked_vehicle, ranked_pos,
                                     ranked_pickup_pos, ranked_delivery_pos,
                                     &ranked_count,
                                     noisy, score, new_route_distance,
                                     v, UINT32_MAX, pickup_pos, delivery_pos);
        } else {
            /* O(L) request-level evaluation for delivery-only */
            uint32_t len = sol->route_lengths[v];
            uint32_t pos;
            for (pos = 0; pos <= len; pos++) {
                double score = 0.0;
                double noisy = 0.0;
                double new_route_distance = 0.0;

                if (!sg_route_eval_insertion_cached(ctx, sol, request_id, v, pos,
                                                    &score, &new_route_distance)) {
                    continue;
                }

                noisy = score;
                if (noise_scale > 0.0 && ctx->op_rng) {
                    noisy *= (1.0 + sh_rng_uniform_range(ctx->op_rng, -noise_scale, noise_scale));
                }

                sg_rank_insert_candidate(ranked_noisy, ranked_scores, ranked_distance,
                                         ranked_vehicle, ranked_pos,
                                         ranked_pickup_pos, ranked_delivery_pos,
                                         &ranked_count,
                                         noisy, score, new_route_distance,
                                         v, pos, UINT32_MAX, UINT32_MAX);
            }
        }

        /* New-trip evaluation: try inserting as a new trip at end of route */
        if (ctx->vehicles[v].has_multi_trip && sol->route_lengths[v] > 0) {
            const SGVehicleRecord *vehicle = &ctx->vehicles[v];
            uint32_t tc = sol->route_trip_count ? sol->route_trip_count[v] : 1;
            if (vehicle->max_trips == 0 || tc < vehicle->max_trips) {
                /* Evaluate new trip: depot → new_stop(s) → depot */
                SGRouteStop new_stops[2];
                uint32_t new_stop_count = 0;
                if (sg_request_emit_stops(ctx, request_id, new_stops, &new_stop_count)) {
                    const SGDepotRecord *sd = &ctx->depots[vehicle->start_depot_id];
                    const SGDepotRecord *ed = &ctx->depots[vehicle->end_depot_id];
                    double dist_delta = 0.0;
                    double trip_time;
                    double depot_dep;
                    double cursor;
                    int time_ok = 1;
                    uint32_t ns;

                    /* Distance: start_depot → stops → end_depot */
                    {
                        uint32_t ploc = vehicle->start_location_id;
                        for (ns = 0; ns < new_stop_count; ns++) {
                            uint32_t sloc = ctx->tasks[new_stops[ns].task_id].location_id;
                            dist_delta += sg_travel_dist(ctx, ploc, sloc, v);
                            ploc = sloc;
                        }
                        if (!vehicle->open_end) {
                            dist_delta += sg_travel_dist(ctx, ploc, vehicle->end_location_id, v);
                        }
                    }

                    /* Timing: last stop → depot return + reload → depot TW snap → stops */
                    {
                        uint32_t slen = sol->route_stop_lengths[v];
                        const SGRouteStop *stops_v = sg_route_vehicle_stop_ptr_const(sol, v);
                        double last_depart = (slen > 0) ? stops_v[slen - 1].depart : 0.0;
                        uint32_t last_loc = (slen > 0)
                            ? ctx->tasks[stops_v[slen - 1].task_id].location_id
                            : vehicle->start_location_id;
                        double ret_dur = sg_travel_dur(ctx, last_loc, vehicle->end_location_id, v, 0.0);
                        double arr_depot = last_depart + ret_dur;
                        depot_dep = arr_depot + (double)vehicle->trip_reload_seconds;
                        if (sd->has_time_window && depot_dep < (double)sd->tw_early) {
                            depot_dep = (double)sd->tw_early;
                        }
                    }

                    cursor = depot_dep;
                    {
                        uint32_t ploc = vehicle->start_location_id;
                        for (ns = 0; ns < new_stop_count; ns++) {
                            const SGTaskRecord *task = &ctx->tasks[new_stops[ns].task_id];
                            uint32_t sloc = task->location_id;
                            double dur = sg_travel_dur(ctx, ploc, sloc, v, 0.0);
                            double arr = cursor + dur;
                            double start = sg_task_snap_forward(task, arr);
                            if (start > (double)task->tw_late + 1e-9) {
                                time_ok = 0;
                                break;
                            }
                            cursor = start + (double)task->service_seconds;
                            ploc = sloc;
                        }
                    }

                    /* Check depot return and shift TW */
                    if (time_ok && !vehicle->open_end) {
                        double ret_dur = sg_travel_dur(ctx,
                            ctx->tasks[new_stops[new_stop_count - 1].task_id].location_id,
                            vehicle->end_location_id, v, 0.0);
                        trip_time = cursor + ret_dur;
                        if (ed->has_time_window && trip_time > (double)ed->tw_late + 1e-9) {
                            time_ok = 0;
                        }
                        if (vehicle->has_shift_time_window &&
                            trip_time > (double)vehicle->shift_late + 1e-9 &&
                            !(vehicle->cost_per_overtime > 0.0)) {
                            time_ok = 0;
                        }
                    }

                    if (time_ok) {
                        double score = vehicle->cost_per_distance * dist_delta;
                        double noisy = score;
                        double new_route_dist = sol->route_distance[v] + dist_delta;
                        uint32_t req_pos = sol->route_lengths[v];
                        if (noise_scale > 0.0 && ctx->op_rng) {
                            noisy *= (1.0 + sh_rng_uniform_range(ctx->op_rng, -noise_scale, noise_scale));
                        }
                        if (is_pd) {
                            /* PD uses stop-level positions; SG_NEW_TRIP_BIT marks new trip */
                            uint32_t spos = sol->route_stop_lengths[v];
                            sg_rank_insert_candidate(ranked_noisy, ranked_scores, ranked_distance,
                                                     ranked_vehicle, ranked_pos,
                                                     ranked_pickup_pos, ranked_delivery_pos,
                                                     &ranked_count,
                                                     noisy, score, new_route_dist,
                                                     v, UINT32_MAX,
                                                     spos | SG_NEW_TRIP_BIT,
                                                     (spos + 1) | SG_NEW_TRIP_BIT);
                        } else {
                            sg_rank_insert_candidate(ranked_noisy, ranked_scores, ranked_distance,
                                                     ranked_vehicle, ranked_pos,
                                                     ranked_pickup_pos, ranked_delivery_pos,
                                                     &ranked_count,
                                                     noisy, score, new_route_dist,
                                                     v, req_pos | SG_NEW_TRIP_BIT,
                                                     UINT32_MAX, UINT32_MAX);
                        }
                    }
                }
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
        *best_pickup_stop_pos_out = ranked_pickup_pos[0];
        *best_delivery_stop_pos_out = ranked_delivery_pos[0];
        *best_route_distance_out = ranked_distance[0];
        ok = 1;
    }

    return ok;
}

static uint32_t sg_route_select_request_greedy(SGContext *ctx, const SGRouteSolution *sol,
                                               double noise_scale, uint32_t *best_vehicle_out,
                                               uint32_t *best_pos_out,
                                               uint32_t *best_pickup_pos_out,
                                               uint32_t *best_delivery_pos_out,
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
        uint32_t pickup_pos = UINT32_MAX;
        uint32_t delivery_pos = UINT32_MAX;
        double route_distance = 0.0;
        if (!sg_route_rank_insertions_for_request(ctx, sol, request_id, 1, noise_scale,
                                                  &first_score, &kth_score,
                                                  &vehicle_id, &pos,
                                                  &pickup_pos, &delivery_pos,
                                                  &route_distance)) {
            continue;
        }

        if (first_score < best_score ||
            (fabs(first_score - best_score) <= 1e-9 && request_id < best_request)) {
            best_score = first_score;
            best_request = request_id;
            *best_vehicle_out = vehicle_id;
            *best_pos_out = pos;
            *best_pickup_pos_out = pickup_pos;
            *best_delivery_pos_out = delivery_pos;
            *best_route_distance_out = route_distance;
        }
    }

    return best_request;
}

static uint32_t sg_route_select_request_regret(SGContext *ctx, const SGRouteSolution *sol,
                                               int regret_k, double noise_scale,
                                               uint32_t *best_vehicle_out,
                                               uint32_t *best_pos_out,
                                               uint32_t *best_pickup_pos_out,
                                               uint32_t *best_delivery_pos_out,
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
        uint32_t pickup_pos = UINT32_MAX;
        uint32_t delivery_pos = UINT32_MAX;
        double route_distance = 0.0;
        if (!sg_route_rank_insertions_for_request(ctx, sol, request_id, regret_k, noise_scale,
                                                  &first_score, &kth_score,
                                                  &vehicle_id, &pos,
                                                  &pickup_pos, &delivery_pos,
                                                  &route_distance)) {
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
            *best_pickup_pos_out = pickup_pos;
            *best_delivery_pos_out = delivery_pos;
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
        uint32_t pickup_pos = UINT32_MAX;
        uint32_t delivery_pos = UINT32_MAX;
        double route_distance = 0.0;
        int is_new_trip = 0;
        uint32_t request_id = sg_route_select_request_greedy(ctx, sol, noise_scale,
                                                             &vehicle_id, &pos,
                                                             &pickup_pos, &delivery_pos,
                                                             &route_distance);
        if (request_id == UINT32_MAX) {
            break;
        }
        if (pickup_pos != UINT32_MAX && delivery_pos != UINT32_MAX) {
            if (pickup_pos & SG_NEW_TRIP_BIT) {
                is_new_trip = 1;
                pickup_pos &= ~SG_NEW_TRIP_BIT;
                delivery_pos &= ~SG_NEW_TRIP_BIT;
            }
            if (sg_route_apply_pd_insertion(ctx, sol, request_id, vehicle_id,
                                            pickup_pos, delivery_pos,
                                            route_distance) != AR_STATUS_OK) {
                return AR_STATUS_ERROR;
            }
        } else {
            if (pos & SG_NEW_TRIP_BIT) {
                is_new_trip = 1;
                pos &= ~SG_NEW_TRIP_BIT;
            }
            if (sg_route_apply_insertion(ctx, sol, request_id, vehicle_id, pos,
                                         route_distance) != AR_STATUS_OK) {
                return AR_STATUS_ERROR;
            }
        }
        if (is_new_trip) {
            sg_route_mark_new_trip(ctx, sol, request_id, vehicle_id);
        }
    }
    return AR_STATUS_OK;
}

ARStatus sg_route_repair_fill_regret(SGContext *ctx, SGRouteSolution *sol,
                                     int regret_k, double noise_scale) {
    while (sol->base.num_unassigned > 0) {
        uint32_t vehicle_id = UINT32_MAX;
        uint32_t pos = UINT32_MAX;
        uint32_t pickup_pos = UINT32_MAX;
        uint32_t delivery_pos = UINT32_MAX;
        double route_distance = 0.0;
        int is_new_trip = 0;
        uint32_t request_id = sg_route_select_request_regret(ctx, sol, regret_k, noise_scale,
                                                             &vehicle_id, &pos,
                                                             &pickup_pos, &delivery_pos,
                                                             &route_distance);
        if (request_id == UINT32_MAX) {
            break;
        }
        if (pickup_pos != UINT32_MAX && delivery_pos != UINT32_MAX) {
            if (pickup_pos & SG_NEW_TRIP_BIT) {
                is_new_trip = 1;
                pickup_pos &= ~SG_NEW_TRIP_BIT;
                delivery_pos &= ~SG_NEW_TRIP_BIT;
            }
            if (sg_route_apply_pd_insertion(ctx, sol, request_id, vehicle_id,
                                            pickup_pos, delivery_pos,
                                            route_distance) != AR_STATUS_OK) {
                return AR_STATUS_ERROR;
            }
        } else {
            if (pos & SG_NEW_TRIP_BIT) {
                is_new_trip = 1;
                pos &= ~SG_NEW_TRIP_BIT;
            }
            if (sg_route_apply_insertion(ctx, sol, request_id, vehicle_id, pos,
                                         route_distance) != AR_STATUS_OK) {
                return AR_STATUS_ERROR;
            }
        }
        if (is_new_trip) {
            sg_route_mark_new_trip(ctx, sol, request_id, vehicle_id);
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
