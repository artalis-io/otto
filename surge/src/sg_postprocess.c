#include "sg_internal.h"

static ARStatus sg_route_try_eliminate_vehicle(const SGContext *ctx,
                                               SGRouteSolution *sol,
                                               uint32_t vehicle_id) {
    uint32_t route_len;
    uint32_t *removed_requests = NULL;
    ARStatus status = AR_STATUS_OK;
    uint32_t i;

    if (!ctx || !sol || vehicle_id >= sol->num_vehicles) {
        return AR_STATUS_INVALID_ARG;
    }

    route_len = sol->route_lengths[vehicle_id];
    if (route_len == 0) {
        return AR_STATUS_OK;
    }

    removed_requests = (uint32_t *)malloc((size_t)route_len * sizeof(uint32_t));
    if (!removed_requests) {
        return AR_STATUS_OUT_OF_MEMORY;
    }
    memcpy(removed_requests, sg_route_vehicle_ptr_const(sol, vehicle_id),
           (size_t)route_len * sizeof(uint32_t));

    /* Insert tighter windows first to reduce dead-end reinsertion failures. */
    for (i = 1; i < route_len; i++) {
        uint32_t key = removed_requests[i];
        int32_t key_width = INT32_MAX;
        uint32_t j = i;
        (void)sg_request_tw_width(ctx, key, &key_width);

        while (j > 0) {
            uint32_t prev = removed_requests[j - 1];
            int32_t prev_width = INT32_MAX;
            (void)sg_request_tw_width(ctx, prev, &prev_width);
            if (prev_width < key_width ||
                (prev_width == key_width && prev <= key)) {
                break;
            }
            removed_requests[j] = prev;
            j--;
        }
        removed_requests[j] = key;
    }

    status = sg_route_unassign_removed_requests(ctx, sol, removed_requests, (int)route_len);
    if (status != AR_STATUS_OK) {
        free(removed_requests);
        return status;
    }

    for (i = 0; i < route_len; i++) {
        uint32_t request_id = removed_requests[i];
        uint32_t best_vehicle = UINT32_MAX;
        uint32_t best_pos = UINT32_MAX;
        uint32_t best_pickup_pos = UINT32_MAX;
        uint32_t best_delivery_pos = UINT32_MAX;
        double best_route_distance = 0.0;
        if (!sg_route_find_best_insertion_for_request(ctx, sol, request_id, vehicle_id,
                                                      &best_vehicle, &best_pos,
                                                      &best_pickup_pos, &best_delivery_pos,
                                                      &best_route_distance)) {
            free(removed_requests);
            return AR_STATUS_LIMIT;
        }
        if (best_pickup_pos != UINT32_MAX && best_delivery_pos != UINT32_MAX) {
            status = sg_route_apply_pd_insertion(ctx, sol, request_id, best_vehicle,
                                                  best_pickup_pos, best_delivery_pos,
                                                  best_route_distance);
        } else {
            status = sg_route_apply_insertion(ctx, sol, request_id, best_vehicle, best_pos,
                                              best_route_distance);
        }
        if (status != AR_STATUS_OK) {
            free(removed_requests);
            return status;
        }
    }

    free(removed_requests);
    return sol->route_lengths[vehicle_id] == 0 ? AR_STATUS_OK : AR_STATUS_LIMIT;
}

static int sg_route_try_exchange_once(const SGContext *ctx, SGRouteSolution *sol) {
    uint32_t count;
    uint32_t *requests = NULL;
    uint32_t i;
    uint32_t j;
    int improved = 0;

    if (!ctx || !sol) {
        return 0;
    }
    count = sol->base.num_assigned;
    if (count < 2) {
        return 0;
    }

    requests = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
    if (!requests) {
        return 0;
    }
    memcpy(requests, sol->base.assigned_ids, (size_t)count * sizeof(uint32_t));

    for (i = 0; i < count && !improved; i++) {
        uint32_t req_a = requests[i];
        if (req_a >= sol->base.total_requests || !sol->base.assigned_flags[req_a]) {
            continue;
        }

        for (j = i + 1; j < count; j++) {
            uint32_t req_b = requests[j];
            uint32_t ids[2];
            uint32_t vehicle_a;
            uint32_t vehicle_b;
            uint32_t best_vehicle_a = UINT32_MAX;
            uint32_t best_pos_a = UINT32_MAX;
            uint32_t best_vehicle_b = UINT32_MAX;
            uint32_t best_pos_b = UINT32_MAX;
            double best_dist_a = 0.0;
            double best_dist_b = 0.0;
            double before_cost;
            SGRouteSolution *backup;
            ARStatus status;

            if (req_b >= sol->base.total_requests || !sol->base.assigned_flags[req_b]) {
                continue;
            }

            vehicle_a = sol->request_vehicle[req_a];
            vehicle_b = sol->request_vehicle[req_b];
            if (vehicle_a >= sol->num_vehicles || vehicle_b >= sol->num_vehicles) {
                continue;
            }

            backup = (SGRouteSolution *)sg_route_solution_copy(sol, (void *)ctx);
            if (!backup) {
                continue;
            }
            before_cost = sg_route_solution_cost(backup, (void *)ctx);

            ids[0] = req_a;
            ids[1] = req_b;
            status = sg_route_unassign_removed_requests(ctx, sol, ids, 2);
            if (status != AR_STATUS_OK) {
                sg_route_restore_from_backup(sol, backup);
                continue;
            }

            {
                uint32_t pickup_pos_a = UINT32_MAX, delivery_pos_a = UINT32_MAX;
                uint32_t pickup_pos_b = UINT32_MAX, delivery_pos_b = UINT32_MAX;

                if (!sg_route_find_best_insertion_no_new_vehicle(ctx, sol, req_a, vehicle_b,
                                                                 &best_vehicle_a, &best_pos_a,
                                                                 &pickup_pos_a, &delivery_pos_a,
                                                                 &best_dist_a)) {
                    sg_route_restore_from_backup(sol, backup);
                    continue;
                }
                if (pickup_pos_a != UINT32_MAX && delivery_pos_a != UINT32_MAX) {
                    status = sg_route_apply_pd_insertion(ctx, sol, req_a, best_vehicle_a,
                                                         pickup_pos_a, delivery_pos_a, best_dist_a);
                } else {
                    status = sg_route_apply_insertion(ctx, sol, req_a, best_vehicle_a,
                                                      best_pos_a, best_dist_a);
                }
                if (status != AR_STATUS_OK) {
                    sg_route_restore_from_backup(sol, backup);
                    continue;
                }

                if (!sg_route_find_best_insertion_no_new_vehicle(ctx, sol, req_b, vehicle_a,
                                                                 &best_vehicle_b, &best_pos_b,
                                                                 &pickup_pos_b, &delivery_pos_b,
                                                                 &best_dist_b)) {
                    sg_route_restore_from_backup(sol, backup);
                    continue;
                }
                if (pickup_pos_b != UINT32_MAX && delivery_pos_b != UINT32_MAX) {
                    status = sg_route_apply_pd_insertion(ctx, sol, req_b, best_vehicle_b,
                                                         pickup_pos_b, delivery_pos_b, best_dist_b);
                } else {
                    status = sg_route_apply_insertion(ctx, sol, req_b, best_vehicle_b,
                                                      best_pos_b, best_dist_b);
                }
            }
            if (status != AR_STATUS_OK) {
                sg_route_restore_from_backup(sol, backup);
                continue;
            }

            if (sol->base.num_unassigned == 0 &&
                sg_route_solution_cost(sol, (void *)ctx) < before_cost - 1e-9) {
                sg_route_solution_free(backup, NULL);
                improved = 1;
                break;
            }

            sg_route_restore_from_backup(sol, backup);
        }
    }

    free(requests);
    return improved;
}

static int sg_route_try_2opt_star_once(const SGContext *ctx, SGRouteSolution *sol) {
    uint32_t *candidate_a = NULL;
    uint32_t *candidate_b = NULL;
    uint32_t va;
    int improved = 0;

    if (!ctx || !sol || sol->num_vehicles < 2 || sol->route_stride == 0) {
        return 0;
    }

    candidate_a = (uint32_t *)malloc((size_t)sol->route_stride * sizeof(uint32_t));
    candidate_b = (uint32_t *)malloc((size_t)sol->route_stride * sizeof(uint32_t));
    if (!candidate_a || !candidate_b) {
        free(candidate_a);
        free(candidate_b);
        return 0;
    }

    for (va = 0; va < sol->num_vehicles && !improved; va++) {
        uint32_t vb;
        uint32_t len_a = sol->route_lengths[va];
        if (len_a < 2) {
            continue;
        }

        for (vb = va + 1; vb < sol->num_vehicles; vb++) {
            uint32_t len_b = sol->route_lengths[vb];
            const uint32_t *route_a;
            const uint32_t *route_b;
            uint32_t cut_a;
            if (len_b < 2) {
                continue;
            }

            route_a = sg_route_vehicle_ptr_const(sol, va);
            route_b = sg_route_vehicle_ptr_const(sol, vb);

            for (cut_a = 1; cut_a < len_a && !improved; cut_a++) {
                uint32_t cut_b;
                for (cut_b = 1; cut_b < len_b; cut_b++) {
                    uint32_t new_len_a = cut_a + (len_b - cut_b);
                    uint32_t new_len_b = cut_b + (len_a - cut_a);
                    double new_dist_a = 0.0;
                    double new_dist_b = 0.0;
                    double new_total;
                    uint32_t r;

                    if (new_len_a > sol->route_stride || new_len_b > sol->route_stride) {
                        continue;
                    }

                    memcpy(candidate_a, route_a, (size_t)cut_a * sizeof(uint32_t));
                    memcpy(&candidate_a[cut_a], &route_b[cut_b],
                           (size_t)(len_b - cut_b) * sizeof(uint32_t));
                    memcpy(candidate_b, route_b, (size_t)cut_b * sizeof(uint32_t));
                    memcpy(&candidate_b[cut_b], &route_a[cut_a],
                           (size_t)(len_a - cut_a) * sizeof(uint32_t));

                    if (!sg_route_sequence_feasible_distance(ctx, va, candidate_a, new_len_a,
                                                             &new_dist_a, NULL) ||
                        !sg_route_sequence_feasible_distance(ctx, vb, candidate_b, new_len_b,
                                                             &new_dist_b, NULL)) {
                        continue;
                    }

                    new_total = sol->total_distance - sol->route_distance[va] -
                                sol->route_distance[vb] + new_dist_a + new_dist_b;
                    if (new_total >= sol->total_distance - 1e-9) {
                        continue;
                    }

                    {
                        SGRouteSolution *backup =
                            (SGRouteSolution *)sg_route_solution_copy(sol, (void *)ctx);
                        if (!backup) {
                            continue;
                        }

                        memcpy(sg_route_vehicle_ptr(sol, va), candidate_a,
                               (size_t)new_len_a * sizeof(uint32_t));
                        memcpy(sg_route_vehicle_ptr(sol, vb), candidate_b,
                               (size_t)new_len_b * sizeof(uint32_t));
                        sol->route_lengths[va] = new_len_a;
                        sol->route_lengths[vb] = new_len_b;

                        for (r = 0; r < sol->base.total_requests; r++) {
                            if (sol->request_vehicle[r] == va || sol->request_vehicle[r] == vb) {
                                sol->request_vehicle[r] = UINT32_MAX;
                                sol->request_pos[r] = UINT32_MAX;
                                sol->request_pickup_stop_pos[r] = UINT32_MAX;
                                sol->request_delivery_stop_pos[r] = UINT32_MAX;
                            }
                        }

                        for (r = 0; r < new_len_a; r++) {
                            uint32_t req = candidate_a[r];
                            sol->request_vehicle[req] = va;
                            sol->request_pos[req] = r;
                        }
                        for (r = 0; r < new_len_b; r++) {
                            uint32_t req = candidate_b[r];
                            sol->request_vehicle[req] = vb;
                            sol->request_pos[req] = r;
                        }

                        if (!sg_route_rebuild_vehicle_stop_state(ctx, sol, va) ||
                            !sg_route_rebuild_vehicle_stop_state(ctx, sol, vb)) {
                            sg_route_restore_from_backup(sol, backup);
                            continue;
                        }

                        sg_route_update_timing(ctx, sol, va);
                        sg_route_update_timing(ctx, sol, vb);
                        sg_route_update_load(ctx, sol, va);
                        sg_route_update_load(ctx, sol, vb);
                        sol->total_distance = new_total;
                        sg_route_solution_free(backup, NULL);
                        improved = 1;
                        break;
                    }
                }
            }
        }
    }

    free(candidate_a);
    free(candidate_b);
    return improved;
}

void sg_route_restore_from_backup(SGRouteSolution *sol, SGRouteSolution *backup) {
    if (!sol || !backup) {
        return;
    }
    sg_route_solution_reset(sol);
    *sol = *backup;
    free(backup);
}

int sg_route_find_best_insertion_for_request(const SGContext *ctx,
                                             const SGRouteSolution *sol,
                                             uint32_t request_id,
                                             uint32_t forbidden_vehicle,
                                             uint32_t *best_vehicle_out,
                                             uint32_t *best_pos_out,
                                             uint32_t *best_pickup_pos_out,
                                             uint32_t *best_delivery_pos_out,
                                             double *best_route_distance_out) {
    double best_score = INFINITY;
    uint32_t v;
    int found = 0;
    int is_pd;

    if (!ctx || !sol || !best_vehicle_out || !best_pos_out ||
        !best_pickup_pos_out || !best_delivery_pos_out ||
        !best_route_distance_out || request_id >= sol->base.total_requests) {
        return 0;
    }

    is_pd = (ctx->requests[request_id].kind == SG_REQUEST_KIND_PICKUP_DELIVERY);

    for (v = 0; v < sol->num_vehicles; v++) {
        if (v == forbidden_vehicle) {
            continue;
        }

        if (is_pd) {
            double score = 0.0;
            uint32_t pickup_pos = UINT32_MAX;
            uint32_t delivery_pos = UINT32_MAX;
            double new_route_distance = 0.0;

            if (!sg_route_eval_pd_best_insertion_cached(ctx, sol, request_id, v,
                                                         &score, &pickup_pos,
                                                         &delivery_pos,
                                                         &new_route_distance)) {
                continue;
            }

            if (!found || score < best_score ||
                (fabs(score - best_score) <= 1e-9 && v < *best_vehicle_out)) {
                found = 1;
                best_score = score;
                *best_vehicle_out = v;
                *best_pos_out = UINT32_MAX;
                *best_pickup_pos_out = pickup_pos;
                *best_delivery_pos_out = delivery_pos;
                *best_route_distance_out = new_route_distance;
            }
        } else {
            uint32_t len = sol->route_lengths[v];
            uint32_t pos;
            for (pos = 0; pos <= len; pos++) {
                double score = 0.0;
                double new_route_distance = 0.0;
                if (!sg_route_eval_insertion_cached(ctx, sol, request_id, v, pos,
                                                    &score, &new_route_distance)) {
                    continue;
                }

                if (!found || score < best_score ||
                    (fabs(score - best_score) <= 1e-9 && v < *best_vehicle_out) ||
                    (fabs(score - best_score) <= 1e-9 && v == *best_vehicle_out &&
                     pos < *best_pos_out)) {
                    found = 1;
                    best_score = score;
                    *best_vehicle_out = v;
                    *best_pos_out = pos;
                    *best_pickup_pos_out = UINT32_MAX;
                    *best_delivery_pos_out = UINT32_MAX;
                    *best_route_distance_out = new_route_distance;
                }
            }
        }
    }

    return found;
}

int sg_route_find_best_insertion_no_new_vehicle(const SGContext *ctx,
                                                const SGRouteSolution *sol,
                                                uint32_t request_id,
                                                uint32_t empty_route_ok_vehicle,
                                                uint32_t *best_vehicle_out,
                                                uint32_t *best_pos_out,
                                                uint32_t *best_pickup_pos_out,
                                                uint32_t *best_delivery_pos_out,
                                                double *best_route_distance_out) {
    double best_score = INFINITY;
    uint32_t v;
    int found = 0;
    int is_pd;

    if (!ctx || !sol || !best_vehicle_out || !best_pos_out ||
        !best_pickup_pos_out || !best_delivery_pos_out ||
        !best_route_distance_out || request_id >= sol->base.total_requests) {
        return 0;
    }

    is_pd = (ctx->requests[request_id].kind == SG_REQUEST_KIND_PICKUP_DELIVERY);

    for (v = 0; v < sol->num_vehicles; v++) {
        uint32_t len = sol->route_lengths[v];
        if (len == 0 && v != empty_route_ok_vehicle) {
            continue;
        }

        if (is_pd) {
            double score = 0.0;
            uint32_t pickup_pos = UINT32_MAX;
            uint32_t delivery_pos = UINT32_MAX;
            double new_route_distance = 0.0;

            if (!sg_route_eval_pd_best_insertion_cached(ctx, sol, request_id, v,
                                                         &score, &pickup_pos,
                                                         &delivery_pos,
                                                         &new_route_distance)) {
                continue;
            }

            if (!found || score < best_score ||
                (fabs(score - best_score) <= 1e-9 && v < *best_vehicle_out)) {
                found = 1;
                best_score = score;
                *best_vehicle_out = v;
                *best_pos_out = UINT32_MAX;
                *best_pickup_pos_out = pickup_pos;
                *best_delivery_pos_out = delivery_pos;
                *best_route_distance_out = new_route_distance;
            }
        } else {
            uint32_t pos;
            for (pos = 0; pos <= len; pos++) {
                double score = 0.0;
                double new_route_distance = 0.0;
                if (!sg_route_eval_insertion_cached(ctx, sol, request_id, v, pos,
                                                    &score, &new_route_distance)) {
                    continue;
                }

                if (!found || score < best_score ||
                    (fabs(score - best_score) <= 1e-9 && v < *best_vehicle_out) ||
                    (fabs(score - best_score) <= 1e-9 && v == *best_vehicle_out &&
                     pos < *best_pos_out)) {
                    found = 1;
                    best_score = score;
                    *best_vehicle_out = v;
                    *best_pos_out = pos;
                    *best_pickup_pos_out = UINT32_MAX;
                    *best_delivery_pos_out = UINT32_MAX;
                    *best_route_distance_out = new_route_distance;
                }
            }
        }
    }

    return found;
}

ARStatus sg_route_postprocess_reduce_vehicles(const SGContext *ctx,
                                              SGRouteSolution *sol) {
    uint8_t *tried = NULL;
    int improved = 1;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }

    if (sol->num_vehicles > 0) {
        tried = (uint8_t *)malloc((size_t)sol->num_vehicles * sizeof(uint8_t));
        if (!tried) {
            return AR_STATUS_OUT_OF_MEMORY;
        }
    }

    while (improved) {
        uint32_t attempts = 0;
        improved = 0;
        if (tried && sol->num_vehicles > 0) {
            memset(tried, 0, (size_t)sol->num_vehicles * sizeof(uint8_t));
        }

        while (attempts < sol->num_vehicles) {
            uint32_t selected_vehicle = UINT32_MAX;
            uint32_t selected_len = UINT32_MAX;
            uint32_t v;
            SGRouteSolution *backup;
            double before_cost;
            double max_distance_after;
            ARStatus status;

            for (v = 0; v < sol->num_vehicles; v++) {
                uint32_t len = sol->route_lengths[v];
                if ((tried && tried[v]) || len == 0) {
                    continue;
                }
                if (len < selected_len ||
                    (len == selected_len && (selected_vehicle == UINT32_MAX || v < selected_vehicle))) {
                    selected_len = len;
                    selected_vehicle = v;
                }
            }

            if (selected_vehicle == UINT32_MAX) {
                break;
            }
            if (tried) {
                tried[selected_vehicle] = 1;
            }
            attempts++;

            backup = (SGRouteSolution *)sg_route_solution_copy(sol, (void *)ctx);
            if (!backup) {
                free(tried);
                return AR_STATUS_OUT_OF_MEMORY;
            }
            before_cost = sg_route_solution_cost(backup, (void *)ctx);
            max_distance_after = backup->total_distance * 1.12 + 50.0;

            status = sg_route_try_eliminate_vehicle(ctx, sol, selected_vehicle);
            if (status == AR_STATUS_OK &&
                sol->vehicles_used + 1 == backup->vehicles_used &&
                sol->total_distance <= max_distance_after &&
                sg_route_solution_cost(sol, (void *)ctx) < before_cost - 1e-9) {
                sg_route_solution_free(backup, NULL);
                improved = 1;
                break;
            }

            sg_route_restore_from_backup(sol, backup);
        }
    }

    free(tried);
    return AR_STATUS_OK;
}

ARStatus sg_route_postprocess_polish_distance(const SGContext *ctx,
                                              SGRouteSolution *sol) {
    uint32_t max_vehicles;
    uint32_t pass;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }
    if (sol->base.num_assigned == 0) {
        return AR_STATUS_OK;
    }

    max_vehicles = sol->vehicles_used;
    for (pass = 0; pass < 3; pass++) {
        uint32_t count = sol->base.num_assigned;
        uint32_t *requests;
        uint32_t i;
        int improved = 0;

        if (count == 0) {
            break;
        }
        requests = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
        if (!requests) {
            return AR_STATUS_OUT_OF_MEMORY;
        }
        memcpy(requests, sol->base.assigned_ids, (size_t)count * sizeof(uint32_t));

        for (i = 0; i < count; i++) {
            uint32_t request_id = requests[i];
            uint32_t original_vehicle;
            SGRouteSolution *backup;
            double before_cost;
            ARStatus status;
            uint32_t best_vehicle = UINT32_MAX;
            uint32_t best_pos = UINT32_MAX;
            uint32_t best_pickup_pos = UINT32_MAX;
            uint32_t best_delivery_pos = UINT32_MAX;
            double best_route_distance = 0.0;

            if (request_id >= sol->base.total_requests || !sol->base.assigned_flags[request_id]) {
                continue;
            }

            original_vehicle = sol->request_vehicle[request_id];
            backup = (SGRouteSolution *)sg_route_solution_copy(sol, (void *)ctx);
            if (!backup) {
                free(requests);
                return AR_STATUS_OUT_OF_MEMORY;
            }
            before_cost = sg_route_solution_cost(backup, (void *)ctx);

            status = sg_route_unassign_removed_requests(ctx, sol, &request_id, 1);
            if (status != AR_STATUS_OK) {
                sg_route_restore_from_backup(sol, backup);
                continue;
            }

            if (!sg_route_find_best_insertion_no_new_vehicle(ctx, sol, request_id,
                                                             original_vehicle,
                                                             &best_vehicle, &best_pos,
                                                             &best_pickup_pos, &best_delivery_pos,
                                                             &best_route_distance)) {
                sg_route_restore_from_backup(sol, backup);
                continue;
            }

            if (best_pickup_pos != UINT32_MAX && best_delivery_pos != UINT32_MAX) {
                status = sg_route_apply_pd_insertion(ctx, sol, request_id, best_vehicle,
                                                      best_pickup_pos, best_delivery_pos,
                                                      best_route_distance);
            } else {
                status = sg_route_apply_insertion(ctx, sol, request_id, best_vehicle, best_pos,
                                                  best_route_distance);
            }
            if (status != AR_STATUS_OK) {
                sg_route_restore_from_backup(sol, backup);
                continue;
            }

            if (sol->base.num_unassigned != 0 ||
                sol->vehicles_used > max_vehicles ||
                sg_route_solution_cost(sol, (void *)ctx) >= before_cost - 1e-9) {
                sg_route_restore_from_backup(sol, backup);
                continue;
            }

            if (sol->vehicles_used < max_vehicles) {
                max_vehicles = sol->vehicles_used;
            }
            sg_route_solution_free(backup, NULL);
            improved = 1;
        }

        free(requests);
        if (!improved) {
            break;
        }
    }

    return AR_STATUS_OK;
}

ARStatus sg_route_postprocess_intensify(const SGContext *ctx, SGRouteSolution *sol) {
    uint32_t pass;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }

    for (pass = 0; pass < SG_ROUTE_MAX_INTENSIFY_PASSES; pass++) {
        int improved = 0;
        if (sg_route_try_exchange_once(ctx, sol)) {
            improved = 1;
        }
        if (sg_route_try_2opt_star_once(ctx, sol)) {
            improved = 1;
        }
        if (!improved) {
            break;
        }
    }

    return AR_STATUS_OK;
}
