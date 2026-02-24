#include "sg_internal.h"

/* Check that a candidate request array has no commodity conflicts or exclusion violations. */
static int sg_route_candidate_compat_ok(const SGContext *ctx,
                                         const uint32_t *requests, uint32_t len) {
    if (ctx->num_commodities > 0) {
        uint64_t bits = 0;
        uint32_t i;
        for (i = 0; i < len; i++) {
            uint32_t cid = ctx->requests[requests[i]].commodity_id;
            if (cid > 0) {
                if (ctx->commodity_conflicts[cid - 1] & bits) {
                    return 0;
                }
                bits |= (1ULL << (cid - 1));
            }
        }
    }
    if (ctx->num_exclusion_groups > 0) {
        uint32_t *counts;
        uint32_t i;
        int ok = 1;
        int use_scratch = 0;
        if (ctx->scratch.exclusion_counts) {
            use_scratch = 1;
            counts = ctx->scratch.exclusion_counts;
            memset(counts, 0, (size_t)ctx->num_exclusion_groups * sizeof(uint32_t));
        } else {
            counts = (uint32_t *)calloc(ctx->num_exclusion_groups, sizeof(uint32_t));
            if (!counts) return 1; /* conservative: assume OK if OOM */
        }
        for (i = 0; i < len && ok; i++) {
            const SGRequestRecord *req = &ctx->requests[requests[i]];
            uint16_t g;
            for (g = 0; g < req->num_exclusion_groups && ok; g++) {
                uint32_t gid = req->exclusion_group_ids[g];
                counts[gid]++;
                if (counts[gid] > 1) {
                    ok = 0;
                }
            }
        }
        if (!use_scratch) free(counts);
        return ok;
    }
    return 1;
}

/* Recompute route_commodities and route_exclusion_counts for a vehicle from its request list. */
static void sg_route_recompute_compat_tracking(const SGContext *ctx,
                                                SGRouteSolution *sol, uint32_t v) {
    const uint32_t *route = sg_route_vehicle_ptr_const(sol, v);
    uint32_t len = sol->route_lengths[v];
    uint32_t i;
    if (sol->route_commodities) {
        uint64_t bits = 0;
        for (i = 0; i < len; i++) {
            uint32_t cid = ctx->requests[route[i]].commodity_id;
            if (cid > 0) {
                bits |= (1ULL << (cid - 1));
            }
        }
        sol->route_commodities[v] = bits;
    }
    if (sol->route_exclusion_counts && ctx->num_exclusion_groups > 0) {
        memset(&sol->route_exclusion_counts[(size_t)v * ctx->num_exclusion_groups], 0,
               (size_t)ctx->num_exclusion_groups * sizeof(uint32_t));
        for (i = 0; i < len; i++) {
            const SGRequestRecord *req = &ctx->requests[route[i]];
            uint16_t g;
            for (g = 0; g < req->num_exclusion_groups; g++) {
                uint32_t gid = req->exclusion_group_ids[g];
                sol->route_exclusion_counts[(size_t)v * ctx->num_exclusion_groups + gid]++;
            }
        }
    }
}

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

            /* Skip if either request is frozen — re-insertion can't guarantee
             * the frozen request stays on its assigned vehicle */
            if (sg_request_is_frozen(ctx, req_a) || sg_request_is_frozen(ctx, req_b)) {
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
    int use_scratch = 0;

    if (!ctx || !sol || sol->num_vehicles < 2 || sol->route_stride == 0) {
        return 0;
    }

    if (ctx->scratch.candidate_a) {
        use_scratch = 1;
        candidate_a = ctx->scratch.candidate_a;
        candidate_b = ctx->scratch.candidate_b;
    } else {
        candidate_a = (uint32_t *)malloc((size_t)sol->route_stride * sizeof(uint32_t));
        candidate_b = (uint32_t *)malloc((size_t)sol->route_stride * sizeof(uint32_t));
        if (!candidate_a || !candidate_b) {
            free(candidate_a);
            free(candidate_b);
            return 0;
        }
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

                    /* Skip if any frozen request would change vehicle */
                    if (ctx->has_frozen) {
                        int frozen_conflict = 0;
                        uint32_t fi;
                        for (fi = cut_a; fi < len_a && !frozen_conflict; fi++) {
                            if (sg_request_is_frozen(ctx, route_a[fi])) frozen_conflict = 1;
                        }
                        for (fi = cut_b; fi < len_b && !frozen_conflict; fi++) {
                            if (sg_request_is_frozen(ctx, route_b[fi])) frozen_conflict = 1;
                        }
                        if (frozen_conflict) continue;
                    }

                    memcpy(candidate_a, route_a, (size_t)cut_a * sizeof(uint32_t));
                    memcpy(&candidate_a[cut_a], &route_b[cut_b],
                           (size_t)(len_b - cut_b) * sizeof(uint32_t));
                    memcpy(candidate_b, route_b, (size_t)cut_b * sizeof(uint32_t));
                    memcpy(&candidate_b[cut_b], &route_a[cut_a],
                           (size_t)(len_a - cut_a) * sizeof(uint32_t));

                    if (!sg_route_candidate_compat_ok(ctx, candidate_a, new_len_a) ||
                        !sg_route_candidate_compat_ok(ctx, candidate_b, new_len_b)) {
                        continue;
                    }
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
                        sg_route_recompute_compat_tracking(ctx, sol, va);
                        sg_route_recompute_compat_tracking(ctx, sol, vb);
                        sol->total_distance = new_total;
                        sg_route_solution_free(backup, NULL);
                        improved = 1;
                        break;
                    }
                }
            }
        }
    }

    if (!use_scratch) { free(candidate_a); free(candidate_b); }
    return improved;
}

static int sg_route_try_or_opt_once(const SGContext *ctx, SGRouteSolution *sol) {
    uint32_t *candidate_src = NULL;
    uint32_t *candidate_dst = NULL;
    uint32_t va;
    int improved = 0;
    int use_scratch = 0;

    if (!ctx || !sol || sol->route_stride == 0) {
        return 0;
    }

    if (ctx->scratch.candidate_a) {
        use_scratch = 1;
        candidate_src = ctx->scratch.candidate_a;
        candidate_dst = ctx->scratch.candidate_b;
    } else {
        candidate_src = (uint32_t *)malloc((size_t)sol->route_stride * sizeof(uint32_t));
        candidate_dst = (uint32_t *)malloc((size_t)sol->route_stride * sizeof(uint32_t));
        if (!candidate_src || !candidate_dst) {
            free(candidate_src);
            free(candidate_dst);
            return 0;
        }
    }

    for (va = 0; va < sol->num_vehicles && !improved; va++) {
        uint32_t len_a = sol->route_lengths[va];
        const uint32_t *route_a;
        int k;

        if (len_a < 2) {
            continue;
        }
        route_a = sg_route_vehicle_ptr_const(sol, va);

        for (k = 3; k >= 1 && !improved; k--) {
            uint32_t start;

            if ((uint32_t)k > len_a) {
                continue;
            }

            for (start = 0; start + (uint32_t)k <= len_a && !improved; start++) {
                uint32_t src_len = len_a - (uint32_t)k;
                double src_dist = 0.0;
                uint32_t vb;

                /* Build source route (route_a without segment) */
                if (start > 0) {
                    memcpy(candidate_src, route_a,
                           (size_t)start * sizeof(uint32_t));
                }
                if (start + (uint32_t)k < len_a) {
                    memcpy(&candidate_src[start], &route_a[start + k],
                           (size_t)(len_a - start - (uint32_t)k) * sizeof(uint32_t));
                }

                /* Check source feasibility */
                if (src_len > 0) {
                    if (!sg_route_sequence_feasible_distance(
                            ctx, va, candidate_src, src_len, &src_dist, NULL)) {
                        continue;
                    }
                }

                /* Skip segment with frozen requests for cross-vehicle moves */
                {
                    int has_frozen_in_seg = 0;
                    if (ctx->has_frozen) {
                        uint32_t fi;
                        for (fi = start; fi < start + (uint32_t)k; fi++) {
                            if (sg_request_is_frozen(ctx, route_a[fi])) {
                                has_frozen_in_seg = 1;
                                break;
                            }
                        }
                    }

                /* Try each target vehicle */
                for (vb = 0; vb < sol->num_vehicles && !improved; vb++) {
                    uint32_t len_b;
                    const uint32_t *route_b_base;
                    uint32_t ins;

                    /* Don't create new routes on empty vehicles */
                    if (va != vb && sol->route_lengths[vb] == 0) {
                        continue;
                    }

                    /* Skip cross-vehicle if segment has frozen requests */
                    if (va != vb && has_frozen_in_seg) {
                        continue;
                    }

                    if (va == vb) {
                        len_b = src_len;
                        route_b_base = candidate_src;
                    } else {
                        len_b = sol->route_lengths[vb];
                        route_b_base = sg_route_vehicle_ptr_const(sol, vb);
                    }

                    for (ins = 0; ins <= len_b; ins++) {
                        uint32_t dst_len = len_b + (uint32_t)k;
                        double dst_dist = 0.0;
                        double new_total;

                        if (dst_len > sol->route_stride) {
                            continue;
                        }

                        /* Skip identity move (intra-route, same position) */
                        if (va == vb && ins == start) {
                            continue;
                        }

                        /* Build destination route */
                        if (ins > 0) {
                            memcpy(candidate_dst, route_b_base,
                                   (size_t)ins * sizeof(uint32_t));
                        }
                        memcpy(&candidate_dst[ins], &route_a[start],
                               (size_t)k * sizeof(uint32_t));
                        if (ins < len_b) {
                            memcpy(&candidate_dst[ins + k], &route_b_base[ins],
                                   (size_t)(len_b - ins) * sizeof(uint32_t));
                        }

                        if (va != vb &&
                            !sg_route_candidate_compat_ok(ctx, candidate_dst, dst_len)) {
                            continue;
                        }
                        if (!sg_route_sequence_feasible_distance(
                                ctx, vb, candidate_dst, dst_len, &dst_dist, NULL)) {
                            continue;
                        }

                        /* Compute improvement */
                        if (va == vb) {
                            new_total = sol->total_distance
                                        - sol->route_distance[va] + dst_dist;
                        } else {
                            new_total = sol->total_distance
                                        - sol->route_distance[va]
                                        - sol->route_distance[vb]
                                        + src_dist + dst_dist;
                        }

                        if (new_total >= sol->total_distance - 1e-9) {
                            continue;
                        }

                        /* Apply the move */
                        {
                            SGRouteSolution *backup =
                                (SGRouteSolution *)sg_route_solution_copy(sol, (void *)ctx);
                            uint32_t r;

                            if (!backup) {
                                continue;
                            }

                            if (va == vb) {
                                memcpy(sg_route_vehicle_ptr(sol, va), candidate_dst,
                                       (size_t)dst_len * sizeof(uint32_t));
                                sol->route_lengths[va] = dst_len;
                            } else {
                                memcpy(sg_route_vehicle_ptr(sol, va), candidate_src,
                                       (size_t)src_len * sizeof(uint32_t));
                                sol->route_lengths[va] = src_len;
                                memcpy(sg_route_vehicle_ptr(sol, vb), candidate_dst,
                                       (size_t)dst_len * sizeof(uint32_t));
                                sol->route_lengths[vb] = dst_len;
                            }

                            /* Update request-vehicle mapping */
                            for (r = 0; r < sol->base.total_requests; r++) {
                                if (sol->request_vehicle[r] == va ||
                                    (va != vb && sol->request_vehicle[r] == vb)) {
                                    sol->request_vehicle[r] = UINT32_MAX;
                                    sol->request_pos[r] = UINT32_MAX;
                                    sol->request_pickup_stop_pos[r] = UINT32_MAX;
                                    sol->request_delivery_stop_pos[r] = UINT32_MAX;
                                }
                            }

                            if (va == vb) {
                                for (r = 0; r < dst_len; r++) {
                                    sol->request_vehicle[candidate_dst[r]] = va;
                                    sol->request_pos[candidate_dst[r]] = r;
                                }
                            } else {
                                for (r = 0; r < src_len; r++) {
                                    sol->request_vehicle[candidate_src[r]] = va;
                                    sol->request_pos[candidate_src[r]] = r;
                                }
                                for (r = 0; r < dst_len; r++) {
                                    sol->request_vehicle[candidate_dst[r]] = vb;
                                    sol->request_pos[candidate_dst[r]] = r;
                                }
                            }

                            /* Rebuild stop state */
                            if (!sg_route_rebuild_vehicle_stop_state(ctx, sol, va) ||
                                (va != vb &&
                                 !sg_route_rebuild_vehicle_stop_state(ctx, sol, vb))) {
                                sg_route_restore_from_backup(sol, backup);
                                continue;
                            }

                            sg_route_update_timing(ctx, sol, va);
                            sg_route_update_load(ctx, sol, va);
                            if (va != vb) {
                                sg_route_update_timing(ctx, sol, vb);
                                sg_route_update_load(ctx, sol, vb);
                            }
                            sg_route_recompute_compat_tracking(ctx, sol, va);
                            if (va != vb) {
                                sg_route_recompute_compat_tracking(ctx, sol, vb);
                            }

                            /* Update distances */
                            if (va == vb) {
                                sol->route_distance[va] = dst_dist;
                            } else {
                                sol->route_distance[va] = src_dist;
                                sol->route_distance[vb] = dst_dist;
                            }
                            sol->total_distance = new_total;

                            /* Update vehicles_used */
                            {
                                uint32_t v, used = 0;
                                for (v = 0; v < sol->num_vehicles; v++) {
                                    if (sol->route_lengths[v] > 0) {
                                        used++;
                                    }
                                }
                                sol->vehicles_used = used;
                            }

                            sg_route_solution_free(backup, NULL);
                            improved = 1;
                            break;
                        }
                    }
                }
                } /* close has_frozen_in_seg block */
            }
        }
    }

    if (!use_scratch) { free(candidate_src); free(candidate_dst); }
    return improved;
}

static int sg_route_try_cross_exchange_once(const SGContext *ctx, SGRouteSolution *sol) {
    uint32_t *candidate_a = NULL;
    uint32_t *candidate_b = NULL;
    uint32_t va;
    int improved = 0;
    int use_scratch = 0;

    if (!ctx || !sol || sol->num_vehicles < 2 || sol->route_stride == 0) {
        return 0;
    }

    if (ctx->scratch.candidate_a) {
        use_scratch = 1;
        candidate_a = ctx->scratch.candidate_a;
        candidate_b = ctx->scratch.candidate_b;
    } else {
        candidate_a = (uint32_t *)malloc((size_t)sol->route_stride * sizeof(uint32_t));
        candidate_b = (uint32_t *)malloc((size_t)sol->route_stride * sizeof(uint32_t));
        if (!candidate_a || !candidate_b) {
            free(candidate_a);
            free(candidate_b);
            return 0;
        }
    }

    for (va = 0; va < sol->num_vehicles && !improved; va++) {
        uint32_t vb;
        uint32_t len_a = sol->route_lengths[va];
        if (len_a == 0) {
            continue;
        }

        for (vb = va + 1; vb < sol->num_vehicles && !improved; vb++) {
            uint32_t len_b = sol->route_lengths[vb];
            const uint32_t *route_a;
            const uint32_t *route_b;
            int sa;

            if (len_b == 0) {
                continue;
            }

            route_a = sg_route_vehicle_ptr_const(sol, va);
            route_b = sg_route_vehicle_ptr_const(sol, vb);

            for (sa = 1; sa <= 3 && !improved; sa++) {
                int sb;
                if ((uint32_t)sa > len_a) {
                    break;
                }

                for (sb = 1; sb <= 3 && !improved; sb++) {
                    uint32_t ia;
                    if ((uint32_t)sb > len_b) {
                        break;
                    }

                    for (ia = 0; ia + (uint32_t)sa <= len_a && !improved; ia++) {
                        uint32_t ib;

                        /* Skip if any frozen request in route_a segment */
                        if (ctx->has_frozen) {
                            int frozen_a = 0;
                            uint32_t fi;
                            for (fi = ia; fi < ia + (uint32_t)sa; fi++) {
                                if (sg_request_is_frozen(ctx, route_a[fi])) { frozen_a = 1; break; }
                            }
                            if (frozen_a) continue;
                        }

                        for (ib = 0; ib + (uint32_t)sb <= len_b; ib++) {
                            uint32_t new_len_a = len_a - (uint32_t)sa + (uint32_t)sb;
                            uint32_t new_len_b = len_b - (uint32_t)sb + (uint32_t)sa;
                            double new_dist_a = 0.0;
                            double new_dist_b = 0.0;
                            double new_total;

                            /* Skip identity swap (same size at same positions on same route
                               is impossible since va < vb, but skip sa==sb same content). */
                            if (sa == sb && ia == ib && va == vb) {
                                continue;
                            }

                            /* Skip if any frozen request in route_b segment */
                            if (ctx->has_frozen) {
                                int frozen_b = 0;
                                uint32_t fi;
                                for (fi = ib; fi < ib + (uint32_t)sb; fi++) {
                                    if (sg_request_is_frozen(ctx, route_b[fi])) { frozen_b = 1; break; }
                                }
                                if (frozen_b) continue;
                            }

                            if (new_len_a > sol->route_stride ||
                                new_len_b > sol->route_stride) {
                                continue;
                            }

                            /* Build candidate A: A[0..ia-1] + B[ib..ib+sb-1] + A[ia+sa..end] */
                            if (ia > 0) {
                                memcpy(candidate_a, route_a,
                                       (size_t)ia * sizeof(uint32_t));
                            }
                            memcpy(&candidate_a[ia], &route_b[ib],
                                   (size_t)sb * sizeof(uint32_t));
                            if (ia + (uint32_t)sa < len_a) {
                                memcpy(&candidate_a[ia + (uint32_t)sb],
                                       &route_a[ia + (uint32_t)sa],
                                       (size_t)(len_a - ia - (uint32_t)sa) * sizeof(uint32_t));
                            }

                            /* Build candidate B: B[0..ib-1] + A[ia..ia+sa-1] + B[ib+sb..end] */
                            if (ib > 0) {
                                memcpy(candidate_b, route_b,
                                       (size_t)ib * sizeof(uint32_t));
                            }
                            memcpy(&candidate_b[ib], &route_a[ia],
                                   (size_t)sa * sizeof(uint32_t));
                            if (ib + (uint32_t)sb < len_b) {
                                memcpy(&candidate_b[ib + (uint32_t)sa],
                                       &route_b[ib + (uint32_t)sb],
                                       (size_t)(len_b - ib - (uint32_t)sb) * sizeof(uint32_t));
                            }

                            if (!sg_route_candidate_compat_ok(ctx, candidate_a, new_len_a) ||
                                !sg_route_candidate_compat_ok(ctx, candidate_b, new_len_b)) {
                                continue;
                            }
                            if (!sg_route_sequence_feasible_distance(
                                    ctx, va, candidate_a, new_len_a,
                                    &new_dist_a, NULL) ||
                                !sg_route_sequence_feasible_distance(
                                    ctx, vb, candidate_b, new_len_b,
                                    &new_dist_b, NULL)) {
                                continue;
                            }

                            new_total = sol->total_distance
                                        - sol->route_distance[va]
                                        - sol->route_distance[vb]
                                        + new_dist_a + new_dist_b;
                            if (new_total >= sol->total_distance - 1e-9) {
                                continue;
                            }

                            /* Apply the move */
                            {
                                SGRouteSolution *backup =
                                    (SGRouteSolution *)sg_route_solution_copy(sol, (void *)ctx);
                                uint32_t r;

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
                                    if (sol->request_vehicle[r] == va ||
                                        sol->request_vehicle[r] == vb) {
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
                                sg_route_recompute_compat_tracking(ctx, sol, va);
                                sg_route_recompute_compat_tracking(ctx, sol, vb);
                                sol->route_distance[va] = new_dist_a;
                                sol->route_distance[vb] = new_dist_b;
                                sol->total_distance = new_total;

                                sg_route_solution_free(backup, NULL);
                                improved = 1;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    if (!use_scratch) { free(candidate_a); free(candidate_b); }
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

static ARStatus sg_route_try_eliminate_two_vehicles(const SGContext *ctx,
                                                     SGRouteSolution *sol,
                                                     uint32_t vehicle_a,
                                                     uint32_t vehicle_b) {
    uint32_t len_a, len_b, total_len;
    uint32_t *removed = NULL;
    ARStatus status = AR_STATUS_OK;
    uint32_t i;

    if (!ctx || !sol || vehicle_a >= sol->num_vehicles ||
        vehicle_b >= sol->num_vehicles || vehicle_a == vehicle_b) {
        return AR_STATUS_INVALID_ARG;
    }

    len_a = sol->route_lengths[vehicle_a];
    len_b = sol->route_lengths[vehicle_b];
    total_len = len_a + len_b;
    if (total_len == 0) {
        return AR_STATUS_OK;
    }

    removed = (uint32_t *)malloc((size_t)total_len * sizeof(uint32_t));
    if (!removed) {
        return AR_STATUS_OUT_OF_MEMORY;
    }
    memcpy(removed, sg_route_vehicle_ptr_const(sol, vehicle_a),
           (size_t)len_a * sizeof(uint32_t));
    memcpy(removed + len_a, sg_route_vehicle_ptr_const(sol, vehicle_b),
           (size_t)len_b * sizeof(uint32_t));

    /* Sort by TW width ascending, then TW early ascending, then request ID. */
    for (i = 1; i < total_len; i++) {
        uint32_t key = removed[i];
        int32_t key_width = INT32_MAX;
        int32_t key_early = INT32_MAX;
        int32_t key_late = 0;
        uint32_t j = i;
        (void)sg_request_tw_width(ctx, key, &key_width);
        (void)sg_request_time_window_bounds(ctx, key, &key_early, &key_late);

        while (j > 0) {
            uint32_t prev = removed[j - 1];
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
            removed[j] = prev;
            j--;
        }
        removed[j] = key;
    }

    status = sg_route_unassign_removed_requests(ctx, sol, removed, (int)total_len);
    if (status != AR_STATUS_OK) {
        free(removed);
        return status;
    }

    for (i = 0; i < total_len; i++) {
        uint32_t request_id = removed[i];
        uint32_t best_vehicle = UINT32_MAX;
        uint32_t best_pos = UINT32_MAX;
        uint32_t best_pickup_pos = UINT32_MAX;
        uint32_t best_delivery_pos = UINT32_MAX;
        double best_route_distance = 0.0;
        if (!sg_route_find_best_insertion_no_new_vehicle(ctx, sol, request_id, UINT32_MAX,
                                                          &best_vehicle, &best_pos,
                                                          &best_pickup_pos, &best_delivery_pos,
                                                          &best_route_distance)) {
            free(removed);
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
            free(removed);
            return status;
        }
    }

    free(removed);
    return (sol->route_lengths[vehicle_a] == 0 && sol->route_lengths[vehicle_b] == 0)
           ? AR_STATUS_OK : AR_STATUS_LIMIT;
}

ARStatus sg_route_postprocess_reduce_vehicles(const SGContext *ctx,
                                              SGRouteSolution *sol) {
    uint8_t *tried = NULL;
    int improved = 1;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }

    /* Skip vehicle reduction when no vehicle has positive fixed cost —
       there is no cost incentive to consolidate routes. */
    {
        int has_fixed = 0;
        uint32_t vi;
        for (vi = 0; vi < ctx->num_vehicles; vi++) {
            if (ctx->vehicles[vi].fixed_cost > 1e-9) {
                has_fixed = 1;
                break;
            }
        }
        if (!has_fixed) {
            return AR_STATUS_OK;
        }
    }

    if (sol->num_vehicles > 0) {
        tried = (uint8_t *)malloc((size_t)sol->num_vehicles * sizeof(uint8_t));
        if (!tried) {
            return AR_STATUS_OUT_OF_MEMORY;
        }
    }

    /* Phase 1: pair elimination — remove two vehicles simultaneously. */
    improved = 1;
    while (improved) {
        improved = 0;
        if (tried && sol->num_vehicles > 0) {
            memset(tried, 0, (size_t)sol->num_vehicles * sizeof(uint8_t));
        }

        for (;;) {
            uint32_t v1 = UINT32_MAX, v2 = UINT32_MAX;
            uint32_t len1 = UINT32_MAX, len2 = UINT32_MAX;
            uint32_t v;
            SGRouteSolution *backup;
            double before_cost;
            double max_distance_after;
            ARStatus status;

            /* Find two smallest non-empty, non-tried vehicles (skip frozen). */
            for (v = 0; v < sol->num_vehicles; v++) {
                uint32_t len = sol->route_lengths[v];
                if ((tried && tried[v]) || len == 0) {
                    continue;
                }
                /* Never try to eliminate a vehicle that has frozen requests */
                if (ctx->has_frozen) {
                    const uint32_t *rv = sg_route_vehicle_ptr_const(sol, v);
                    uint32_t fi;
                    int has_frozen_req = 0;
                    for (fi = 0; fi < len; fi++) {
                        if (sg_request_is_frozen(ctx, rv[fi])) { has_frozen_req = 1; break; }
                    }
                    if (has_frozen_req) continue;
                }
                if (len < len1 || (len == len1 && (v1 == UINT32_MAX || v < v1))) {
                    v2 = v1; len2 = len1;
                    v1 = v;  len1 = len;
                } else if (len < len2 || (len == len2 && (v2 == UINT32_MAX || v < v2))) {
                    v2 = v;  len2 = len;
                }
            }

            if (v1 == UINT32_MAX || v2 == UINT32_MAX) {
                break;
            }

            backup = (SGRouteSolution *)sg_route_solution_copy(sol, (void *)ctx);
            if (!backup) {
                free(tried);
                return AR_STATUS_OUT_OF_MEMORY;
            }
            before_cost = sg_route_solution_cost(backup, (void *)ctx);
            max_distance_after = backup->total_distance * 1.12 + 50.0;

            status = sg_route_try_eliminate_two_vehicles(ctx, sol, v1, v2);
            if (status == AR_STATUS_OK &&
                sol->vehicles_used + 2 == backup->vehicles_used &&
                sol->total_distance <= max_distance_after &&
                sg_route_solution_cost(sol, (void *)ctx) < before_cost - 1e-9) {
                sg_route_solution_free(backup, NULL);
                improved = 1;
                break;
            }

            sg_route_restore_from_backup(sol, backup);
            if (tried) {
                tried[v1] = 1;
            }
        }
    }

    /* Phase 2: single-vehicle elimination (existing logic). */
    improved = 1;
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
                /* Never try to eliminate a vehicle that has frozen requests */
                if (ctx->has_frozen) {
                    const uint32_t *rv = sg_route_vehicle_ptr_const(sol, v);
                    uint32_t fi;
                    int has_frozen_req = 0;
                    for (fi = 0; fi < len; fi++) {
                        if (sg_request_is_frozen(ctx, rv[fi])) { has_frozen_req = 1; break; }
                    }
                    if (has_frozen_req) continue;
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

static int sg_try_place_with_ejection(const SGContext *ctx, SGRouteSolution *sol,
                                       uint32_t req, int depth, uint32_t target_v,
                                       uint8_t *chain_visited, int *budget) {
    /* Step 1: Direct insertion (no new vehicle). */
    {
        uint32_t best_v = UINT32_MAX, best_pos = UINT32_MAX;
        uint32_t best_pp = UINT32_MAX, best_dp = UINT32_MAX;
        double best_dist = 0.0;

        if (sg_route_find_best_insertion_no_new_vehicle(ctx, sol, req, UINT32_MAX,
                                                        &best_v, &best_pos,
                                                        &best_pp, &best_dp,
                                                        &best_dist)) {
            ARStatus s;
            if (best_pp != UINT32_MAX && best_dp != UINT32_MAX) {
                s = sg_route_apply_pd_insertion(ctx, sol, req, best_v,
                                                best_pp, best_dp, best_dist);
            } else {
                s = sg_route_apply_insertion(ctx, sol, req, best_v,
                                             best_pos, best_dist);
            }
            if (s == AR_STATUS_OK) {
                return 1;
            }
        }
    }

    /* Step 2: If no depth remaining or budget exhausted, give up. */
    if (depth <= 0 || (budget && *budget <= 0)) {
        return 0;
    }

    /* Step 3: Ejection scan — try ejecting one request from each vehicle. */
    {
        int32_t req_width = INT32_MAX;
        uint32_t vp;
        (void)sg_request_tw_width(ctx, req, &req_width);

        for (vp = 0; vp < sol->num_vehicles; vp++) {
            uint32_t vp_len = sol->route_lengths[vp];
            uint32_t *vp_requests_snapshot = NULL;
            uint32_t si;

            if (vp_len == 0 || vp == target_v) {
                continue;
            }

            /* Snapshot the route since unassignment modifies it. */
            vp_requests_snapshot = (uint32_t *)malloc((size_t)vp_len * sizeof(uint32_t));
            if (!vp_requests_snapshot) {
                continue;
            }
            memcpy(vp_requests_snapshot, sg_route_vehicle_ptr_const(sol, vp),
                   (size_t)vp_len * sizeof(uint32_t));

            for (si = 0; si < vp_len; si++) {
                uint32_t eject_req = vp_requests_snapshot[si];
                int32_t eject_width = INT32_MAX;
                SGRouteSolution *chain_backup;
                ARStatus cs;
                int r_fits = 0;

                if (chain_visited[eject_req]) {
                    continue;
                }

                /* Budget accounting: decrement and bail if exhausted. */
                if (budget) {
                    (*budget)--;
                    if (*budget <= 0) {
                        free(vp_requests_snapshot);
                        return 0;
                    }
                }

                /* Pruning: skip if ejected request has tighter TW (harder to reinsert). */
                (void)sg_request_tw_width(ctx, eject_req, &eject_width);
                if (eject_width < req_width) {
                    continue;
                }

                chain_backup = (SGRouteSolution *)sg_route_solution_copy(sol, (void *)ctx);
                if (!chain_backup) {
                    continue;
                }

                /* Eject request from V'. */
                cs = sg_route_unassign_removed_requests(ctx, sol, &eject_req, 1);
                if (cs != AR_STATUS_OK) {
                    sg_route_restore_from_backup(sol, chain_backup);
                    continue;
                }

                /* Try inserting req into V'. */
                {
                    int is_pd = (ctx->requests[req].kind == SG_REQUEST_KIND_PICKUP_DELIVERY);
                    if (is_pd) {
                        double score;
                        uint32_t ins_pp = UINT32_MAX, ins_dp = UINT32_MAX;
                        double ins_dist = 0.0;
                        if (sg_route_eval_pd_best_insertion_cached(
                                ctx, sol, req, vp, &score,
                                &ins_pp, &ins_dp, &ins_dist)) {
                            cs = sg_route_apply_pd_insertion(
                                ctx, sol, req, vp, ins_pp, ins_dp, ins_dist);
                            if (cs == AR_STATUS_OK) {
                                r_fits = 1;
                            }
                        }
                    } else {
                        uint32_t p;
                        uint32_t cur_len = sol->route_lengths[vp];
                        double best_sc = INFINITY;
                        uint32_t ins_pos = UINT32_MAX;
                        double ins_dist = 0.0;
                        for (p = 0; p <= cur_len; p++) {
                            double sc = 0.0, nd = 0.0;
                            if (sg_route_eval_insertion_cached(
                                    ctx, sol, req, vp, p, &sc, &nd)) {
                                if (sc < best_sc) {
                                    best_sc = sc;
                                    ins_pos = p;
                                    ins_dist = nd;
                                }
                            }
                        }
                        if (ins_pos != UINT32_MAX) {
                            cs = sg_route_apply_insertion(
                                ctx, sol, req, vp, ins_pos, ins_dist);
                            if (cs == AR_STATUS_OK) {
                                r_fits = 1;
                            }
                        }
                    }
                }

                if (r_fits) {
                    /* Recursively place the ejected request. */
                    chain_visited[eject_req] = 1;
                    if (sg_try_place_with_ejection(ctx, sol, eject_req, depth - 1,
                                                    target_v, chain_visited, budget)) {
                        chain_visited[eject_req] = 0;
                        sg_route_solution_free(chain_backup, NULL);
                        free(vp_requests_snapshot);
                        return 1;
                    }
                    chain_visited[eject_req] = 0;
                }

                /* Chain failed — restore. */
                sg_route_restore_from_backup(sol, chain_backup);
            }

            free(vp_requests_snapshot);
        }
    }

    return 0;
}

ARStatus sg_route_postprocess_ejection_reduce(const SGContext *ctx, SGRouteSolution *sol) {
    int restarted;
    uint8_t *chain_visited = NULL;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }

    /* Skip ejection-based reduction when no vehicle has positive fixed cost. */
    {
        int has_fixed = 0;
        uint32_t vi;
        for (vi = 0; vi < ctx->num_vehicles; vi++) {
            if (ctx->vehicles[vi].fixed_cost > 1e-9) {
                has_fixed = 1;
                break;
            }
        }
        if (!has_fixed) {
            return AR_STATUS_OK;
        }
    }

    if (sol->base.total_requests > 0) {
        chain_visited = (uint8_t *)malloc((size_t)sol->base.total_requests * sizeof(uint8_t));
        if (!chain_visited) {
            return AR_STATUS_OUT_OF_MEMORY;
        }
    }

    restarted = 1;
    while (restarted) {
        uint32_t *vehicle_order = NULL;
        uint32_t num_nonempty = 0;
        uint32_t v, vi;

        restarted = 0;

        /* Build list of non-empty vehicles sorted by route length ascending. */
        vehicle_order = (uint32_t *)malloc((size_t)sol->num_vehicles * sizeof(uint32_t));
        if (!vehicle_order) {
            free(chain_visited);
            return AR_STATUS_OUT_OF_MEMORY;
        }
        for (v = 0; v < sol->num_vehicles; v++) {
            if (sol->route_lengths[v] == 0) continue;
            /* Skip vehicles with frozen requests */
            if (ctx->has_frozen) {
                const uint32_t *rv = sg_route_vehicle_ptr_const(sol, v);
                uint32_t fi;
                int has_frozen_req = 0;
                for (fi = 0; fi < sol->route_lengths[v]; fi++) {
                    if (sg_request_is_frozen(ctx, rv[fi])) { has_frozen_req = 1; break; }
                }
                if (has_frozen_req) continue;
            }
            vehicle_order[num_nonempty++] = v;
        }
        /* Insertion sort by route_length ascending, then vehicle ID ascending. */
        for (vi = 1; vi < num_nonempty; vi++) {
            uint32_t key = vehicle_order[vi];
            uint32_t key_len = sol->route_lengths[key];
            uint32_t j = vi;
            while (j > 0) {
                uint32_t prev = vehicle_order[j - 1];
                uint32_t prev_len = sol->route_lengths[prev];
                if (prev_len < key_len || (prev_len == key_len && prev <= key)) {
                    break;
                }
                vehicle_order[j] = prev;
                j--;
            }
            vehicle_order[j] = key;
        }

        for (vi = 0; vi < num_nonempty && !restarted; vi++) {
            uint32_t target_v = vehicle_order[vi];
            uint32_t route_len = sol->route_lengths[target_v];
            uint32_t *requests = NULL;
            SGRouteSolution *backup = NULL;
            double before_cost;
            uint32_t ri;
            int all_placed = 1;

            if (route_len == 0) {
                continue;
            }

            /* Collect and sort requests by TW width ascending (tightest first). */
            requests = (uint32_t *)malloc((size_t)route_len * sizeof(uint32_t));
            if (!requests) {
                free(vehicle_order);
                free(chain_visited);
                return AR_STATUS_OUT_OF_MEMORY;
            }
            memcpy(requests, sg_route_vehicle_ptr_const(sol, target_v),
                   (size_t)route_len * sizeof(uint32_t));

            for (ri = 1; ri < route_len; ri++) {
                uint32_t key = requests[ri];
                int32_t key_width = INT32_MAX;
                uint32_t j = ri;
                (void)sg_request_tw_width(ctx, key, &key_width);
                while (j > 0) {
                    uint32_t prev = requests[j - 1];
                    int32_t prev_width = INT32_MAX;
                    (void)sg_request_tw_width(ctx, prev, &prev_width);
                    if (prev_width < key_width ||
                        (prev_width == key_width && prev <= key)) {
                        break;
                    }
                    requests[j] = prev;
                    j--;
                }
                requests[j] = key;
            }

            backup = (SGRouteSolution *)sg_route_solution_copy(sol, (void *)ctx);
            if (!backup) {
                free(requests);
                free(vehicle_order);
                free(chain_visited);
                return AR_STATUS_OUT_OF_MEMORY;
            }
            before_cost = sg_route_solution_cost(backup, (void *)ctx);

            /* Unassign all requests from target vehicle. */
            {
                ARStatus status = sg_route_unassign_removed_requests(ctx, sol, requests,
                                                                      (int)route_len);
                if (status != AR_STATUS_OK) {
                    sg_route_restore_from_backup(sol, backup);
                    free(requests);
                    continue;
                }
            }

            /* Try to place each request via direct insertion or ejection chain. */
            {
                int ejection_budget = SG_EJECTION_BUDGET;
            for (ri = 0; ri < route_len; ri++) {
                uint32_t req = requests[ri];

                if (chain_visited) {
                    memset(chain_visited, 0,
                           (size_t)sol->base.total_requests * sizeof(uint8_t));
                    chain_visited[req] = 1;
                }

                if (!sg_try_place_with_ejection(ctx, sol, req, SG_EJECTION_MAX_DEPTH,
                                                 target_v, chain_visited,
                                                 &ejection_budget)) {
                    all_placed = 0;
                    break;
                }

                if (chain_visited) {
                    chain_visited[req] = 0;
                }
            }
            }

            if (all_placed && sol->route_lengths[target_v] == 0 &&
                sg_route_solution_cost(sol, (void *)ctx) < before_cost - 1e-9) {
                /* Vehicle eliminated. */
                sg_route_solution_free(backup, NULL);
                restarted = 1;
            } else {
                sg_route_restore_from_backup(sol, backup);
            }

            free(requests);
        }

        free(vehicle_order);
    }

    free(chain_visited);
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
            /* Never relocate frozen requests — they must stay on their vehicle */
            if (sg_request_is_frozen(ctx, request_id)) {
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

int sg_route_try_pd_reorder_once(const SGContext *ctx, SGRouteSolution *sol) {
    uint32_t v;
    int improved = 0;

    if (!ctx || !sol) {
        return 0;
    }

    for (v = 0; v < sol->num_vehicles && !improved; v++) {
        uint32_t stop_len = sol->route_stop_lengths[v];
        uint32_t i;

        if (stop_len < 4) {
            continue;
        }

        for (i = 0; i + 1 < stop_len && !improved; i++) {
            SGRouteStop *stops = sg_route_vehicle_stop_ptr(sol, v);
            SGRouteStop tmp;
            uint32_t req_i, req_i1;
            uint32_t pickup_pos_i, pickup_pos_i1;
            SGRouteSolution *backup;
            double before_cost;
            double new_dist = 0.0;

            /* Only swap adjacent deliveries from different requests */
            if (stops[i].is_pickup || stops[i + 1].is_pickup) {
                continue;
            }
            req_i = stops[i].request_id;
            req_i1 = stops[i + 1].request_id;
            if (req_i == req_i1) {
                continue;
            }

            /* Check PD precedence: both pickups must be before position i */
            pickup_pos_i = sol->request_pickup_stop_pos[req_i];
            pickup_pos_i1 = sol->request_pickup_stop_pos[req_i1];
            if (pickup_pos_i >= i || pickup_pos_i1 >= i) {
                continue;
            }

            backup = (SGRouteSolution *)sg_route_solution_copy(sol, (void *)ctx);
            if (!backup) {
                continue;
            }
            before_cost = sg_route_solution_cost(backup, (void *)ctx);

            /* Swap the two delivery stops */
            tmp = stops[i];
            stops[i] = stops[i + 1];
            stops[i + 1] = tmp;

            /* Check feasibility of swapped sequence */
            if (!sg_route_stop_sequence_feasible(ctx, v, stops, stop_len, &new_dist)) {
                sg_route_restore_from_backup(sol, backup);
                continue;
            }

            /* Apply: update indices, timing, and distances */
            sol->request_delivery_stop_pos[req_i] = i + 1;
            sol->request_delivery_stop_pos[req_i1] = i;
            {
                double old_dist_v = sol->route_distance[v];
                sg_route_update_timing(ctx, sol, v);
                sol->total_distance = sol->total_distance - old_dist_v + sol->route_distance[v];
            }

            if (sg_route_solution_cost(sol, (void *)ctx) < before_cost - 1e-9) {
                sg_route_solution_free(backup, NULL);
                improved = 1;
            } else {
                sg_route_restore_from_backup(sol, backup);
            }
        }
    }

    return improved;
}

ARStatus sg_route_postprocess_intensify(const SGContext *ctx, SGRouteSolution *sol) {
    uint32_t pass;

    if (!ctx || !sol) {
        return AR_STATUS_INVALID_ARG;
    }

    for (pass = 0; pass < SG_ROUTE_MAX_INTENSIFY_PASSES; pass++) {
        int improved = 0;
        if (sg_route_try_or_opt_once(ctx, sol)) {
            improved = 1;
        }
        if (sg_route_try_exchange_once(ctx, sol)) {
            improved = 1;
        }
        if (sg_route_try_2opt_star_once(ctx, sol)) {
            improved = 1;
        }
        if (sg_route_try_cross_exchange_once(ctx, sol)) {
            improved = 1;
        }
        if (sg_route_try_pd_reorder_once(ctx, sol)) {
            improved = 1;
        }
        if (!improved) {
            break;
        }
    }

    return AR_STATUS_OK;
}
