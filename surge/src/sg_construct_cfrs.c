#include "sg_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Vehicle count lower bound estimator
 * ============================================================================ */

uint32_t sg_estimate_min_vehicles(const SGContext *ctx) {
    uint32_t lb_cap = 0;
    uint32_t lb_tw = 0;
    uint32_t d, i;

    if (!ctx || ctx->num_requests == 0 || ctx->num_vehicles == 0)
        return 1;

    /* --- Bin packing bound: for each capacity dimension --- */
    for (d = 0; d < ctx->dimension_count; d++) {
        double total_demand = 0.0;
        double max_cap = 0.0;

        for (i = 0; i < ctx->num_requests; i++) {
            total_demand += sg_request_abs_demand_at_dim(ctx, i, d);
        }

        for (i = 0; i < ctx->num_vehicles; i++) {
            if (ctx->vehicles[i].has_capacity && ctx->vehicles[i].capacity) {
                double cap = ctx->vehicles[i].capacity[d];
                if (cap > max_cap) max_cap = cap;
            }
        }

        if (max_cap > SG_DEMAND_TOLERANCE) {
            uint32_t lb = (uint32_t)ceil(total_demand / max_cap);
            if (lb > lb_cap) lb_cap = lb;
        }
    }

    /* --- Time window conflict bound: greedy clique approximation --- */
    {
        uint32_t *order = (uint32_t *)malloc((size_t)ctx->num_requests * sizeof(uint32_t));
        if (order) {
            uint32_t n = 0;
            double total_service = 0.0;

            /* Collect requests that have time windows */
            for (i = 0; i < ctx->num_requests; i++) {
                int32_t early = 0, late = 0;
                if (sg_request_time_window_bounds(ctx, i, &early, &late))
                    order[n++] = i;
            }

            /* Accumulate service times for adjusted bound */
            for (i = 0; i < n; i++) {
                const SGRequestRecord *req = &ctx->requests[order[i]];
                if (req->has_delivery_task && req->delivery_task_id < ctx->num_tasks)
                    total_service += (double)ctx->tasks[req->delivery_task_id].service_seconds;
                if (req->has_pickup_task && req->pickup_task_id < ctx->num_tasks)
                    total_service += (double)ctx->tasks[req->pickup_task_id].service_seconds;
            }

            /* Sort by tw_early (insertion sort — small n in practice) */
            for (i = 1; i < n; i++) {
                uint32_t key = order[i];
                int32_t key_early = 0, key_late = 0;
                uint32_t j = i;
                (void)sg_request_time_window_bounds(ctx, key, &key_early, &key_late);

                while (j > 0) {
                    int32_t prev_early = 0, prev_late = 0;
                    (void)sg_request_time_window_bounds(ctx, order[j - 1], &prev_early, &prev_late);
                    if (prev_early <= key_early) break;
                    order[j] = order[j - 1];
                    j--;
                }
                order[j] = key;
            }

            /* Sweep: track min(tw_late) in current group */
            if (n > 0) {
                uint32_t groups = 1;
                int32_t group_min_late = 0;
                int32_t e0 = 0, l0 = 0;
                (void)sg_request_time_window_bounds(ctx, order[0], &e0, &l0);
                group_min_late = l0;

                for (i = 1; i < n; i++) {
                    int32_t early = 0, late = 0;
                    (void)sg_request_time_window_bounds(ctx, order[i], &early, &late);
                    if (early > group_min_late) {
                        groups++;
                        group_min_late = late;
                    } else {
                        if (late < group_min_late)
                            group_min_late = late;
                    }
                }
                lb_tw = groups;

                /* Service-adjusted TW bound: deduct avg service from late times.
                   This tightens the bound on instances where service times
                   consume significant TW slack (e.g., C1 with 10-min service). */
                if (n > 1) {
                    double avg_service = total_service / (double)n;
                    /* Compute average nearest-neighbor distance for travel estimate */
                    double total_nn_dist = 0.0;
                    uint32_t nn_count = 0;
                    for (i = 0; i < n; i++) {
                        uint32_t loc_i;
                        if (sg_request_representative_location(ctx, order[i], &loc_i)) {
                            double min_d = INFINITY;
                            uint32_t ji;
                            for (ji = 0; ji < n; ji++) {
                                uint32_t loc_j;
                                if (ji != i && sg_request_representative_location(ctx, order[ji], &loc_j)) {
                                    double dd = sg_travel_dist(ctx, loc_i, loc_j, SG_NO_VEHICLE);
                                    if (dd < min_d) min_d = dd;
                                }
                            }
                            if (min_d < INFINITY) {
                                total_nn_dist += min_d;
                                nn_count++;
                            }
                        }
                    }
                    {
                        double avg_nn = nn_count > 0 ? total_nn_dist / (double)nn_count : 0.0;
                        int32_t deduction = (int32_t)(avg_service + avg_nn);
                        if (deduction > 0) {
                            uint32_t groups2 = 1;
                            int32_t adj_late;
                            (void)sg_request_time_window_bounds(ctx, order[0], &e0, &l0);
                            adj_late = l0 - deduction;

                            for (i = 1; i < n; i++) {
                                int32_t early = 0, late = 0;
                                (void)sg_request_time_window_bounds(ctx, order[i], &early, &late);
                                if (early > adj_late) {
                                    groups2++;
                                    adj_late = late - deduction;
                                } else {
                                    int32_t candidate = late - deduction;
                                    if (candidate < adj_late)
                                        adj_late = candidate;
                                }
                            }
                            if (groups2 > lb_tw) lb_tw = groups2;
                        }
                    }
                }
            }
            free(order);
        }
    }

    {
        uint32_t lb = lb_cap > lb_tw ? lb_cap : lb_tw;
        if (lb < 1) lb = 1;
        if (lb > ctx->num_vehicles) lb = ctx->num_vehicles;
        return lb;
    }
}

/* ============================================================================
 * Shared helpers for CFRS heuristics
 * ============================================================================ */

/* Check all constraints for assigning a request to a vehicle */
static int sg_cfrs_vehicle_ok(const SGContext *ctx, const SGRouteSolution *sol,
                               uint32_t vehicle_id, uint32_t request_id) {
    if (!sg_vehicle_qualifies(ctx, vehicle_id, request_id)) return 0;
    if (!sg_vehicle_allowed_for_request(ctx, vehicle_id, request_id)) return 0;
    if (!sg_commodity_compatible(ctx, sol, vehicle_id, request_id)) return 0;
    if (!sg_compartment_compatible(ctx, vehicle_id, request_id)) return 0;
    if (!sg_exclusion_compatible(ctx, sol, vehicle_id, request_id)) return 0;
    return 1;
}

/* Select vehicle whose depot is closest to (cx, cy), respecting constraints.
   Ties broken by lower fixed_cost so CFRS heuristics respect vehicle costs. */
static uint32_t sg_cfrs_pick_vehicle(const SGContext *ctx, SGRouteSolution *sol,
                                      double cx, double cy, uint32_t request_id) {
    uint32_t best_v = UINT32_MAX;
    double best_dist = INFINITY;
    double best_cost = INFINITY;
    uint32_t v;

    for (v = 0; v < sol->num_vehicles; v++) {
        double vx, vy, ex, ey, d, fc;
        if (sol->route_stop_lengths[v] > 0) continue;  /* already used */
        if (!sg_cfrs_vehicle_ok(ctx, sol, v, request_id)) continue;

        sg_vehicle_start_end_locations(ctx, v, &vx, &vy, &ex, &ey);
        d = sg_euclid(vx, vy, cx, cy);
        fc = ctx->vehicles[v].fixed_cost;
        if (d < best_dist - 1e-9 ||
            (fabs(d - best_dist) < 1e-9 && fc < best_cost)) {
            best_dist = d;
            best_cost = fc;
            best_v = v;
        }
    }
    return best_v;
}

/* Route requests within a cluster by inserting in TW-width order (tightest first) */
static ARStatus sg_cfrs_route_cluster(SGContext *ctx, SGRouteSolution *sol,
                                       uint32_t vehicle_id,
                                       uint32_t *request_ids, uint32_t count) {
    uint32_t i;

    /* Sort by tw_width ascending (insertion sort) */
    for (i = 1; i < count; i++) {
        uint32_t key = request_ids[i];
        int32_t key_width = INT32_MAX;
        uint32_t j = i;
        (void)sg_request_tw_width(ctx, key, &key_width);

        while (j > 0) {
            int32_t prev_width = INT32_MAX;
            (void)sg_request_tw_width(ctx, request_ids[j - 1], &prev_width);
            if (prev_width <= key_width) break;
            request_ids[j] = request_ids[j - 1];
            j--;
        }
        request_ids[j] = key;
    }

    /* Insert each request at best position */
    for (i = 0; i < count; i++) {
        uint32_t rid = request_ids[i];
        int is_pd = (ctx->requests[rid].kind == SG_REQUEST_KIND_PICKUP_DELIVERY);

        if (sol->base.assigned_flags[rid]) continue;

        if (is_pd) {
            double score;
            uint32_t pp = UINT32_MAX, dp = UINT32_MAX;
            double dist = 0.0;
            if (sg_route_eval_pd_best_insertion_cached(ctx, sol, rid, vehicle_id,
                                                       &score, &pp, &dp, &dist)) {
                sg_route_apply_pd_insertion(ctx, sol, rid, vehicle_id, pp, dp, dist);
            }
        } else {
            uint32_t pos;
            uint32_t cur_len = sol->route_lengths[vehicle_id];
            double best_score = INFINITY;
            uint32_t best_pos = UINT32_MAX;
            double best_dist = 0.0;
            for (pos = 0; pos <= cur_len; pos++) {
                double score = 0.0, new_dist = 0.0;
                if (sg_route_eval_insertion_cached(ctx, sol, rid, vehicle_id,
                                                   pos, &score, &new_dist)) {
                    if (score < best_score) {
                        best_score = score;
                        best_pos = pos;
                        best_dist = new_dist;
                    }
                }
            }
            if (best_pos != UINT32_MAX) {
                sg_route_apply_insertion(ctx, sol, rid, vehicle_id, best_pos, best_dist);
            }
        }
    }
    return AR_STATUS_OK;
}

/* ============================================================================
 * Algorithm 1: Sweep CFRS
 * ============================================================================ */

ARStatus sg_construct_sweep_cfrs(SGContext *ctx, SGRouteSolution *sol) {
    uint32_t n, k, i;
    double cx = 0.0, cy = 0.0;
    uint32_t *order = NULL;
    double *angles = NULL;

    if (!ctx || !sol) return AR_STATUS_INVALID_ARG;

    n = sol->base.num_unassigned;
    if (n == 0) return AR_STATUS_OK;

    /* Pre-place frozen requests */
    if (ctx->has_frozen) {
        for (i = 0; i < ctx->num_requests; i++) {
            if (!sg_request_is_frozen(ctx, i)) continue;
            if (sol->base.assigned_flags[i]) continue;
            uint32_t fv = sg_frozen_designated_vehicle(ctx, i);
            if (fv == SG_NO_VEHICLE || fv >= sol->num_vehicles) continue;
            int is_pd = (ctx->requests[i].kind == SG_REQUEST_KIND_PICKUP_DELIVERY);
            if (is_pd) {
                double score;
                uint32_t pp = UINT32_MAX, dp = UINT32_MAX;
                double dist = 0.0;
                if (sg_route_eval_pd_best_insertion_cached(ctx, sol, i, fv,
                                                           &score, &pp, &dp, &dist))
                    sg_route_apply_pd_insertion(ctx, sol, i, fv, pp, dp, dist);
            } else {
                double score = 0.0, dist = 0.0;
                if (sg_route_eval_insertion_cached(ctx, sol, i, fv, 0, &score, &dist))
                    sg_route_apply_insertion(ctx, sol, i, fv, 0, dist);
            }
        }
        n = sol->base.num_unassigned;
        if (n == 0) return AR_STATUS_OK;
    }

    /* Compute depot centroid */
    if (ctx->num_depots > 0) {
        for (i = 0; i < ctx->num_depots; i++) {
            cx += ctx->depots[i].x;
            cy += ctx->depots[i].y;
        }
        cx /= (double)ctx->num_depots;
        cy /= (double)ctx->num_depots;
    }

    /* Build order array and compute angles */
    order = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    angles = (double *)malloc((size_t)n * sizeof(double));
    if (!order || !angles) {
        free(order);
        free(angles);
        return AR_STATUS_OUT_OF_MEMORY;
    }

    memcpy(order, sol->base.unassigned_ids, (size_t)n * sizeof(uint32_t));
    for (i = 0; i < n; i++) {
        double rx = 0.0, ry = 0.0;
        sg_request_centroid(ctx, order[i], &rx, &ry);
        angles[i] = atan2(ry - cy, rx - cx);
    }

    /* Sort by angle (insertion sort) */
    for (i = 1; i < n; i++) {
        double key_angle = angles[i];
        uint32_t key_order = order[i];
        uint32_t j = i;
        while (j > 0 && angles[j - 1] > key_angle) {
            angles[j] = angles[j - 1];
            order[j] = order[j - 1];
            j--;
        }
        angles[j] = key_angle;
        order[j] = key_order;
    }

    /* Estimate target vehicle count */
    k = sg_estimate_min_vehicles(ctx);

    /* Cut clusters and route within each */
    {
        uint32_t *cluster = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
        if (!cluster) {
            free(order);
            free(angles);
            return AR_STATUS_OUT_OF_MEMORY;
        }

        {
            uint32_t cluster_size = 0;
            uint32_t clusters_created = 0;
            uint32_t target_per_cluster = (n + k - 1) / k;
            double cluster_cx = 0.0, cluster_cy = 0.0;

            for (i = 0; i < n; i++) {
                /* Check if we should start a new cluster */
                int start_new = 0;
                if (cluster_size == 0) {
                    start_new = 0; /* first request in first cluster */
                } else if (clusters_created < k - 1 && cluster_size >= target_per_cluster) {
                    start_new = 1;
                } else {
                    /* Check capacity overflow: would adding this request's demand
                       exceed average capacity for any dimension? */
                    uint32_t dd;
                    for (dd = 0; dd < ctx->dimension_count; dd++) {
                        double max_cap = 0.0;
                        uint32_t vi;
                        double cluster_demand = 0.0;
                        double req_demand;

                        for (vi = 0; vi < ctx->num_vehicles; vi++) {
                            if (ctx->vehicles[vi].has_capacity && ctx->vehicles[vi].capacity) {
                                if (ctx->vehicles[vi].capacity[dd] > max_cap)
                                    max_cap = ctx->vehicles[vi].capacity[dd];
                            }
                        }
                        req_demand = sg_request_abs_demand_at_dim(ctx, order[i], dd);
                        {
                            uint32_t ci;
                            for (ci = 0; ci < cluster_size; ci++)
                                cluster_demand += sg_request_abs_demand_at_dim(ctx, cluster[ci], dd);
                        }
                        if (max_cap > SG_DEMAND_TOLERANCE &&
                            cluster_demand + req_demand > max_cap) {
                            start_new = 1;
                            break;
                        }
                    }
                }

                if (start_new && cluster_size > 0) {
                    /* Route the current cluster */
                    cluster_cx /= (double)cluster_size;
                    cluster_cy /= (double)cluster_size;
                    uint32_t v = sg_cfrs_pick_vehicle(ctx, sol, cluster_cx, cluster_cy,
                                                      cluster[0]);
                    if (v != UINT32_MAX) {
                        sg_cfrs_route_cluster(ctx, sol, v, cluster, cluster_size);
                    }
                    clusters_created++;
                    cluster_size = 0;
                    cluster_cx = 0.0;
                    cluster_cy = 0.0;
                }

                cluster[cluster_size++] = order[i];
                {
                    double rx = 0.0, ry = 0.0;
                    sg_request_centroid(ctx, order[i], &rx, &ry);
                    cluster_cx += rx;
                    cluster_cy += ry;
                }
            }

            /* Route the last cluster */
            if (cluster_size > 0) {
                cluster_cx /= (double)cluster_size;
                cluster_cy /= (double)cluster_size;
                uint32_t v = sg_cfrs_pick_vehicle(ctx, sol, cluster_cx, cluster_cy,
                                                  cluster[0]);
                if (v != UINT32_MAX) {
                    sg_cfrs_route_cluster(ctx, sol, v, cluster, cluster_size);
                }
            }
        }
        free(cluster);
    }

    free(order);
    free(angles);

    /* Mop up any unassigned requests with regret-3 fill */
    if (sol->base.num_unassigned > 0)
        sg_route_repair_fill_regret(ctx, sol, 3, 0.0);

    return AR_STATUS_OK;
}

/* ============================================================================
 * Algorithm 2: K-Means with Time Windows
 * ============================================================================ */

ARStatus sg_construct_kmeans_tw(SGContext *ctx, SGRouteSolution *sol) {
    uint32_t n, k, i, iter;
    double *features = NULL;   /* [n * 3]: x_norm, y_norm, alpha * tw_norm */
    double *centroids = NULL;  /* [k * 3] */
    uint32_t *assign = NULL;   /* [n] cluster assignment */
    uint32_t *counts = NULL;   /* [k] cluster sizes */
    uint32_t *order = NULL;    /* copy of unassigned_ids */
    double x_min, x_max, y_min, y_max, tw_min, tw_max;
    double alpha = 0.3;
    int max_iters = 20;

    if (!ctx || !sol) return AR_STATUS_INVALID_ARG;

    n = sol->base.num_unassigned;
    if (n == 0) return AR_STATUS_OK;

    /* Pre-place frozen requests */
    if (ctx->has_frozen) {
        for (i = 0; i < ctx->num_requests; i++) {
            if (!sg_request_is_frozen(ctx, i)) continue;
            if (sol->base.assigned_flags[i]) continue;
            uint32_t fv = sg_frozen_designated_vehicle(ctx, i);
            if (fv == SG_NO_VEHICLE || fv >= sol->num_vehicles) continue;
            int is_pd = (ctx->requests[i].kind == SG_REQUEST_KIND_PICKUP_DELIVERY);
            if (is_pd) {
                double score;
                uint32_t pp = UINT32_MAX, dp = UINT32_MAX;
                double dist = 0.0;
                if (sg_route_eval_pd_best_insertion_cached(ctx, sol, i, fv,
                                                           &score, &pp, &dp, &dist))
                    sg_route_apply_pd_insertion(ctx, sol, i, fv, pp, dp, dist);
            } else {
                double score = 0.0, dist = 0.0;
                if (sg_route_eval_insertion_cached(ctx, sol, i, fv, 0, &score, &dist))
                    sg_route_apply_insertion(ctx, sol, i, fv, 0, dist);
            }
        }
        n = sol->base.num_unassigned;
        if (n == 0) return AR_STATUS_OK;
    }

    k = sg_estimate_min_vehicles(ctx);
    if (k > n) k = n;

    /* Allocate working arrays */
    order = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    features = (double *)calloc((size_t)n * 3, sizeof(double));
    centroids = (double *)calloc((size_t)k * 3, sizeof(double));
    assign = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    counts = (uint32_t *)calloc(k, sizeof(uint32_t));
    if (!order || !features || !centroids || !assign || !counts) {
        free(order); free(features); free(centroids); free(assign); free(counts);
        return AR_STATUS_OUT_OF_MEMORY;
    }

    memcpy(order, sol->base.unassigned_ids, (size_t)n * sizeof(uint32_t));

    /* Build feature vectors: find ranges for normalization */
    x_min = INFINITY; x_max = -INFINITY;
    y_min = INFINITY; y_max = -INFINITY;
    tw_min = INFINITY; tw_max = -INFINITY;

    for (i = 0; i < n; i++) {
        double rx = 0.0, ry = 0.0;
        int32_t early = 0, late = 0;
        double tw_center;

        sg_request_centroid(ctx, order[i], &rx, &ry);
        if (!sg_request_time_window_bounds(ctx, order[i], &early, &late)) {
            early = 0;
            late = 86400;  /* default full-day window */
        }
        tw_center = ((double)early + (double)late) * 0.5;

        if (rx < x_min) x_min = rx;
        if (rx > x_max) x_max = rx;
        if (ry < y_min) y_min = ry;
        if (ry > y_max) y_max = ry;
        if (tw_center < tw_min) tw_min = tw_center;
        if (tw_center > tw_max) tw_max = tw_center;
    }

    {
        double x_range = (x_max - x_min) > 1e-9 ? (x_max - x_min) : 1.0;
        double y_range = (y_max - y_min) > 1e-9 ? (y_max - y_min) : 1.0;
        double tw_range = (tw_max - tw_min) > 1e-9 ? (tw_max - tw_min) : 1.0;

        for (i = 0; i < n; i++) {
            double rx = 0.0, ry = 0.0;
            int32_t early = 0, late = 0;
            double tw_center;

            sg_request_centroid(ctx, order[i], &rx, &ry);
            if (!sg_request_time_window_bounds(ctx, order[i], &early, &late)) {
                early = 0;
                late = 86400;
            }
            tw_center = ((double)early + (double)late) * 0.5;

            features[i * 3 + 0] = (rx - x_min) / x_range;
            features[i * 3 + 1] = (ry - y_min) / y_range;
            features[i * 3 + 2] = alpha * (tw_center - tw_min) / tw_range;
        }
    }

    /* K-means++ initialization */
    {
        double *min_dist = (double *)malloc((size_t)n * sizeof(double));
        if (!min_dist) {
            free(order); free(features); free(centroids); free(assign); free(counts);
            return AR_STATUS_OUT_OF_MEMORY;
        }

        /* First centroid = first request (deterministic) */
        centroids[0] = features[0];
        centroids[1] = features[1];
        centroids[2] = features[2];

        for (i = 0; i < n; i++) {
            double dx = features[i * 3 + 0] - centroids[0];
            double dy = features[i * 3 + 1] - centroids[1];
            double dt = features[i * 3 + 2] - centroids[2];
            min_dist[i] = dx * dx + dy * dy + dt * dt;
        }

        for (uint32_t c = 1; c < k; c++) {
            /* Pick the point with maximum min-distance */
            uint32_t best_idx = 0;
            double best_d = -1.0;
            for (i = 0; i < n; i++) {
                if (min_dist[i] > best_d) {
                    best_d = min_dist[i];
                    best_idx = i;
                }
            }

            centroids[c * 3 + 0] = features[best_idx * 3 + 0];
            centroids[c * 3 + 1] = features[best_idx * 3 + 1];
            centroids[c * 3 + 2] = features[best_idx * 3 + 2];

            /* Update min distances */
            for (i = 0; i < n; i++) {
                double dx = features[i * 3 + 0] - centroids[c * 3 + 0];
                double dy = features[i * 3 + 1] - centroids[c * 3 + 1];
                double dt = features[i * 3 + 2] - centroids[c * 3 + 2];
                double d2 = dx * dx + dy * dy + dt * dt;
                if (d2 < min_dist[i]) min_dist[i] = d2;
            }
        }
        free(min_dist);
    }

    /* K-means iteration */
    for (iter = 0; iter < (uint32_t)max_iters; iter++) {
        int converged = 1;

        /* Assign each request to nearest centroid */
        for (i = 0; i < n; i++) {
            uint32_t best_c = 0;
            double best_d = INFINITY;
            uint32_t c;

            for (c = 0; c < k; c++) {
                double dx = features[i * 3 + 0] - centroids[c * 3 + 0];
                double dy = features[i * 3 + 1] - centroids[c * 3 + 1];
                double dt = features[i * 3 + 2] - centroids[c * 3 + 2];
                double d2 = dx * dx + dy * dy + dt * dt;
                if (d2 < best_d) {
                    best_d = d2;
                    best_c = c;
                }
            }

            if (iter == 0 || assign[i] != best_c) {
                assign[i] = best_c;
                converged = 0;
            }
        }

        if (converged) break;

        /* Recompute centroids */
        memset(centroids, 0, (size_t)k * 3 * sizeof(double));
        memset(counts, 0, (size_t)k * sizeof(uint32_t));

        for (i = 0; i < n; i++) {
            uint32_t c = assign[i];
            centroids[c * 3 + 0] += features[i * 3 + 0];
            centroids[c * 3 + 1] += features[i * 3 + 1];
            centroids[c * 3 + 2] += features[i * 3 + 2];
            counts[c]++;
        }

        for (uint32_t c = 0; c < k; c++) {
            if (counts[c] > 0) {
                centroids[c * 3 + 0] /= (double)counts[c];
                centroids[c * 3 + 1] /= (double)counts[c];
                centroids[c * 3 + 2] /= (double)counts[c];
            }
        }
    }

    /* Assign clusters to vehicles and route within each */
    {
        /* Rebuild counts for cluster sizes */
        memset(counts, 0, (size_t)k * sizeof(uint32_t));
        for (i = 0; i < n; i++)
            counts[assign[i]]++;

        for (uint32_t c = 0; c < k; c++) {
            uint32_t *cluster_reqs;
            uint32_t ci = 0;
            double ccx = 0.0, ccy = 0.0;
            uint32_t v;

            if (counts[c] == 0) continue;

            cluster_reqs = (uint32_t *)malloc((size_t)counts[c] * sizeof(uint32_t));
            if (!cluster_reqs) continue;

            for (i = 0; i < n; i++) {
                if (assign[i] == c) {
                    double rx = 0.0, ry = 0.0;
                    cluster_reqs[ci++] = order[i];
                    sg_request_centroid(ctx, order[i], &rx, &ry);
                    ccx += rx;
                    ccy += ry;
                }
            }
            ccx /= (double)ci;
            ccy /= (double)ci;

            v = sg_cfrs_pick_vehicle(ctx, sol, ccx, ccy, cluster_reqs[0]);
            if (v != UINT32_MAX) {
                sg_cfrs_route_cluster(ctx, sol, v, cluster_reqs, ci);
            }
            free(cluster_reqs);
        }
    }

    free(order);
    free(features);
    free(centroids);
    free(assign);
    free(counts);

    /* Mop up any unassigned requests */
    if (sol->base.num_unassigned > 0)
        sg_route_repair_fill_regret(ctx, sol, 3, 0.0);

    return AR_STATUS_OK;
}

/* ============================================================================
 * Instance feature extraction
 * ============================================================================ */

void sg_compute_instance_features(const SGContext *ctx, SGInstanceFeatures *out) {
    uint32_t i, n;
    double cx = 0.0, cy = 0.0;
    double sum_dist = 0.0, sum_dist2 = 0.0;
    double sum_tw_width = 0.0;
    uint32_t tw_count = 0;
    int32_t horizon_early = INT32_MAX, horizon_late = INT32_MIN;

    memset(out, 0, sizeof(*out));
    if (!ctx || ctx->num_requests == 0) return;

    n = ctx->num_requests;

    /* Compute global centroid of all request locations */
    for (i = 0; i < n; i++) {
        double rx = 0.0, ry = 0.0;
        sg_request_centroid(ctx, i, &rx, &ry);
        cx += rx;
        cy += ry;
    }
    cx /= (double)n;
    cy /= (double)n;

    /* Compute spatial statistics and TW metrics */
    for (i = 0; i < n; i++) {
        double rx = 0.0, ry = 0.0;
        int32_t early = 0, late = 0;
        double d;

        sg_request_centroid(ctx, i, &rx, &ry);
        d = sg_euclid(rx, ry, cx, cy);
        sum_dist += d;
        sum_dist2 += d * d;

        if (sg_request_time_window_bounds(ctx, i, &early, &late)) {
            sum_tw_width += (double)(late - early);
            tw_count++;
        }
    }

    /* spatial_cv = stddev / mean of distances from centroid */
    {
        double mean = sum_dist / (double)n;
        double variance = sum_dist2 / (double)n - mean * mean;
        if (variance < 0.0) variance = 0.0;
        out->spatial_cv = (mean > 1e-9) ? sqrt(variance) / mean : 0.0;
    }

    /* tw_tightness = avg(tw_width) / planning_horizon */
    /* Planning horizon = max(shift_late) - min(shift_early) across vehicles */
    for (i = 0; i < ctx->num_vehicles; i++) {
        if (ctx->vehicles[i].has_shift_time_window) {
            if (ctx->vehicles[i].shift_early < horizon_early)
                horizon_early = ctx->vehicles[i].shift_early;
            if (ctx->vehicles[i].shift_late > horizon_late)
                horizon_late = ctx->vehicles[i].shift_late;
        }
    }
    if (horizon_early == INT32_MAX || horizon_late == INT32_MIN || horizon_late <= horizon_early) {
        /* Fallback: use depot TWs or default 24h */
        horizon_early = 0;
        horizon_late = 86400;
        for (i = 0; i < ctx->num_depots; i++) {
            if (ctx->depots[i].has_time_window) {
                if (ctx->depots[i].tw_early < horizon_early)
                    horizon_early = ctx->depots[i].tw_early;
                if (ctx->depots[i].tw_late > horizon_late)
                    horizon_late = ctx->depots[i].tw_late;
            }
        }
    }
    {
        double horizon = (double)(horizon_late - horizon_early);
        double avg_tw = tw_count > 0 ? sum_tw_width / (double)tw_count : horizon;
        out->tw_tightness = (horizon > 1e-9) ? avg_tw / horizon : 1.0;
    }

    out->is_clustered = (out->spatial_cv < SG_CLUSTER_THRESHOLD) ? 1 : 0;
    out->is_tight_tw = (out->tw_tightness < SG_TIGHT_TW_THRESHOLD) ? 1 : 0;
}

/* ============================================================================
 * TW-aware cluster validation
 * ============================================================================ */

uint32_t sg_cluster_tw_check(const SGContext *ctx, const uint32_t *requests,
                              uint32_t count, uint32_t vehicle_id) {
    uint32_t i;
    double cursor;
    int32_t shift_late = INT32_MAX;

    if (!ctx || !requests || count == 0) return count;

    /* Get vehicle shift end */
    if (vehicle_id < ctx->num_vehicles && ctx->vehicles[vehicle_id].has_shift_time_window)
        shift_late = ctx->vehicles[vehicle_id].shift_late;

    /* Requests should already be sorted by tw_early ascending.
       Track time cursor: cursor = max(cursor+travel, tw_early) + service */
    {
        int32_t early = 0, late = 0;
        const SGRequestRecord *req;
        uint32_t loc_id;

        if (!sg_request_time_window_bounds(ctx, requests[0], &early, &late))
            return count;  /* no TW info → can't validate */
        cursor = (double)early;

        req = &ctx->requests[requests[0]];
        if (req->has_delivery_task && req->delivery_task_id < ctx->num_tasks)
            cursor += (double)ctx->tasks[req->delivery_task_id].service_seconds;
        if (req->has_pickup_task && req->pickup_task_id < ctx->num_tasks)
            cursor += (double)ctx->tasks[req->pickup_task_id].service_seconds;
    }

    for (i = 1; i < count; i++) {
        int32_t early = 0, late = 0;
        double travel = 0.0;

        if (!sg_request_time_window_bounds(ctx, requests[i], &early, &late))
            continue;

        /* Estimate travel from previous to current request */
        {
            uint32_t prev_loc, cur_loc;
            if (sg_request_representative_location(ctx, requests[i - 1], &prev_loc) &&
                sg_request_representative_location(ctx, requests[i], &cur_loc)) {
                travel = sg_travel_dur(ctx, prev_loc, cur_loc, vehicle_id, cursor);
            }
        }

        cursor += travel;
        if (cursor < (double)early)
            cursor = (double)early;

        /* Check feasibility */
        if (cursor > (double)late || cursor > (double)shift_late)
            return i;

        /* Add service time */
        {
            const SGRequestRecord *req = &ctx->requests[requests[i]];
            if (req->has_delivery_task && req->delivery_task_id < ctx->num_tasks)
                cursor += (double)ctx->tasks[req->delivery_task_id].service_seconds;
            if (req->has_pickup_task && req->pickup_task_id < ctx->num_tasks)
                cursor += (double)ctx->tasks[req->pickup_task_id].service_seconds;
        }
    }

    return count;  /* all fit */
}

/* ============================================================================
 * Post-construction route merging
 * ============================================================================ */

ARStatus sg_construct_try_merge_routes(SGContext *ctx, SGRouteSolution *sol) {
    uint32_t max_merges = 5;
    uint32_t merge_count = 0;
    uint32_t v, num_v;

    if (!ctx || !sol) return AR_STATUS_INVALID_ARG;

    num_v = sol->num_vehicles;
    if (num_v < 2) return AR_STATUS_OK;

    while (merge_count < max_merges) {
        /* Find shortest non-empty route */
        uint32_t best_small = UINT32_MAX;
        uint32_t best_small_len = UINT32_MAX;
        double avg_len = 0.0;
        uint32_t active_count = 0;
        int merged = 0;

        for (v = 0; v < num_v; v++) {
            if (sol->route_stop_lengths[v] > 0) {
                avg_len += (double)sol->route_lengths[v];
                active_count++;
            }
        }
        if (active_count < 2) break;
        avg_len /= (double)active_count;

        /* Find shortest route that's <= avg/2 */
        for (v = 0; v < num_v; v++) {
            uint32_t len = sol->route_lengths[v];
            if (len == 0 || len > (uint32_t)(avg_len / 2.0 + 0.5)) continue;
            if (len < best_small_len) {
                best_small_len = len;
                best_small = v;
            }
        }
        if (best_small == UINT32_MAX) break;

        /* Try merging small route into each other route */
        {
            const uint32_t *small_reqs = sg_route_vehicle_ptr_const(sol, best_small);
            uint32_t small_len = sol->route_lengths[best_small];
            uint32_t other;

            for (other = 0; other < num_v; other++) {
                uint32_t *merged_seq = NULL;
                uint32_t merged_len;
                double merged_dist;
                uint32_t ri, all_ok;

                if (other == best_small || sol->route_lengths[other] == 0) continue;

                /* Check constraint compatibility for all requests from small route */
                all_ok = 1;
                for (ri = 0; ri < small_len; ri++) {
                    if (!sg_cfrs_vehicle_ok(ctx, sol, other, small_reqs[ri])) {
                        all_ok = 0;
                        break;
                    }
                }
                if (!all_ok) continue;

                /* Build merged sequence: other's requests + small's requests */
                {
                    const uint32_t *other_reqs = sg_route_vehicle_ptr_const(sol, other);
                    uint32_t other_len = sol->route_lengths[other];
                    merged_len = other_len + small_len;
                    merged_seq = (uint32_t *)malloc((size_t)merged_len * sizeof(uint32_t));
                    if (!merged_seq) continue;
                    memcpy(merged_seq, other_reqs, (size_t)other_len * sizeof(uint32_t));
                    memcpy(merged_seq + other_len, small_reqs,
                           (size_t)small_len * sizeof(uint32_t));
                }

                /* Check if merged sequence is feasible */
                if (sg_route_sequence_feasible_distance(ctx, other, merged_seq,
                                                         merged_len, &merged_dist, NULL)) {
                    /* Feasible! Unassign small route's requests and rebuild other */
                    for (ri = 0; ri < small_len; ri++) {
                        sg_route_unassign_request(ctx, sol, small_reqs[ri], NULL);
                    }
                    /* Re-read other route (may have shifted after unassign) and
                       rebuild with merged sequence via best-position insertion */
                    {
                        uint32_t si;
                        for (si = 0; si < small_len; si++) {
                            uint32_t rid = merged_seq[sol->route_lengths[other] + si];
                            int is_pd = (ctx->requests[rid].kind == SG_REQUEST_KIND_PICKUP_DELIVERY);

                            if (sol->base.assigned_flags[rid]) continue;

                            if (is_pd) {
                                double score;
                                uint32_t pp = UINT32_MAX, dp = UINT32_MAX;
                                double dist = 0.0;
                                if (sg_route_eval_pd_best_insertion_cached(ctx, sol, rid, other,
                                                                           &score, &pp, &dp, &dist)) {
                                    sg_route_apply_pd_insertion(ctx, sol, rid, other, pp, dp, dist);
                                }
                            } else {
                                uint32_t pos;
                                uint32_t cur_len = sol->route_lengths[other];
                                double best_score = INFINITY;
                                uint32_t best_pos = UINT32_MAX;
                                double best_d = 0.0;
                                for (pos = 0; pos <= cur_len; pos++) {
                                    double score = 0.0, new_dist = 0.0;
                                    if (sg_route_eval_insertion_cached(ctx, sol, rid, other,
                                                                       pos, &score, &new_dist)) {
                                        if (score < best_score) {
                                            best_score = score;
                                            best_pos = pos;
                                            best_d = new_dist;
                                        }
                                    }
                                }
                                if (best_pos != UINT32_MAX) {
                                    sg_route_apply_insertion(ctx, sol, rid, other, best_pos, best_d);
                                }
                            }
                        }
                    }
                    free(merged_seq);
                    merged = 1;
                    merge_count++;
                    break;
                }
                free(merged_seq);
            }
        }
        if (!merged) break;
    }

    /* Recount vehicles used */
    {
        uint32_t used = 0;
        double total_dist = 0.0;
        for (v = 0; v < num_v; v++) {
            if (sol->route_stop_lengths[v] > 0) {
                used++;
                total_dist += sol->route_distance[v];
            }
        }
        sol->vehicles_used = used;
        sol->total_distance = total_dist;
    }

    return AR_STATUS_OK;
}

/* ============================================================================
 * Feature-based strategy ordering
 * ============================================================================ */

void sg_feature_strategy_order(const SGInstanceFeatures *f,
                                SGConstructMethod order[SG_CONSTRUCT_COUNT]) {
    if (!f) {
        /* Default order */
        order[0] = SG_CONSTRUCT_REGRET3;
        order[1] = SG_CONSTRUCT_TW_SORTED;
        order[2] = SG_CONSTRUCT_SOLOMON_I1;
        order[3] = SG_CONSTRUCT_SWEEP_CFRS;
        order[4] = SG_CONSTRUCT_KMEANS_TW;
        return;
    }

    if (f->is_clustered && f->is_tight_tw) {
        /* C1-type: I1 packs routes tightly, sweep benefits from better k */
        order[0] = SG_CONSTRUCT_SOLOMON_I1;
        order[1] = SG_CONSTRUCT_SWEEP_CFRS;
        order[2] = SG_CONSTRUCT_KMEANS_TW;
        order[3] = SG_CONSTRUCT_TW_SORTED;
        order[4] = SG_CONSTRUCT_REGRET3;
    } else if (f->is_clustered && !f->is_tight_tw) {
        /* C2-type: K-means TW dimension helps group temporally compatible */
        order[0] = SG_CONSTRUCT_KMEANS_TW;
        order[1] = SG_CONSTRUCT_SWEEP_CFRS;
        order[2] = SG_CONSTRUCT_SOLOMON_I1;
        order[3] = SG_CONSTRUCT_REGRET3;
        order[4] = SG_CONSTRUCT_TW_SORTED;
    } else if (!f->is_clustered && f->is_tight_tw) {
        /* R1-type: TW-sorted inserts tightest first, respecting temporal */
        order[0] = SG_CONSTRUCT_TW_SORTED;
        order[1] = SG_CONSTRUCT_REGRET3;
        order[2] = SG_CONSTRUCT_SOLOMON_I1;
        order[3] = SG_CONSTRUCT_KMEANS_TW;
        order[4] = SG_CONSTRUCT_SWEEP_CFRS;
    } else {
        /* R2-type (random + wide TW): Regret-3 exploits slack well */
        order[0] = SG_CONSTRUCT_REGRET3;
        order[1] = SG_CONSTRUCT_TW_SORTED;
        order[2] = SG_CONSTRUCT_SOLOMON_I1;
        order[3] = SG_CONSTRUCT_SWEEP_CFRS;
        order[4] = SG_CONSTRUCT_KMEANS_TW;
    }
}

/* ============================================================================
 * Dispatch table
 * ============================================================================ */

ARStatus sg_construct_by_method(SGContext *ctx, SGRouteSolution *sol, SGConstructMethod method) {
    if (!ctx || !sol) return AR_STATUS_INVALID_ARG;

    switch (method) {
    case SG_CONSTRUCT_REGRET3:
        return sg_route_repair_fill_regret(ctx, sol, 3, 0.0);
    case SG_CONSTRUCT_TW_SORTED:
        return sg_route_construct_tw_sorted(ctx, sol);
    case SG_CONSTRUCT_SOLOMON_I1:
        return sg_route_construct_solomon_i1(ctx, sol);
    case SG_CONSTRUCT_SWEEP_CFRS:
        return sg_construct_sweep_cfrs(ctx, sol);
    case SG_CONSTRUCT_KMEANS_TW:
        return sg_construct_kmeans_tw(ctx, sol);
    default:
        return sg_route_repair_fill_regret(ctx, sol, 3, 0.0);
    }
}
