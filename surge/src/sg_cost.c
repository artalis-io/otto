#include "sg_internal.h"

double sg_euclid(double ax, double ay, double bx, double by) {
    double dx = ax - bx;
    double dy = ay - by;
    return sqrt(dx * dx + dy * dy);
}

int32_t sg_clamp_priority(int32_t priority) {
    if (priority < 0) {
        return 0;
    }
    if (priority > 100) {
        return 100;
    }
    return priority;
}

int64_t sg_abs_i64(int64_t value) {
    return value < 0 ? -value : value;
}

const SGRequestHint *sg_get_hint(const SGContext *ctx, uint32_t request_id) {
    if (!ctx || !ctx->request_hints || request_id >= ctx->num_requests) {
        return NULL;
    }
    return &ctx->request_hints[request_id];
}

const SGRequestRecord *sg_get_request_record(const SGContext *ctx, uint32_t request_id) {
    if (!ctx || !ctx->requests || request_id >= ctx->num_requests) {
        return NULL;
    }
    return &ctx->requests[request_id];
}

const SGTaskRecord *sg_get_task_record(const SGContext *ctx, uint32_t task_id) {
    if (!ctx || !ctx->tasks || task_id >= ctx->num_tasks) {
        return NULL;
    }
    return &ctx->tasks[task_id];
}

int sg_zone_distance_lookup(const SGContext *ctx, uint32_t zone_a, uint32_t zone_b,
                            double *distance) {
    size_t stride;

    if (!ctx || !distance || !ctx->zone_distance_matrix || ctx->zone_count == 0) {
        return 0;
    }
    if (zone_a >= ctx->zone_count || zone_b >= ctx->zone_count) {
        return 0;
    }

    stride = (size_t)ctx->zone_count;
    *distance = ctx->zone_distance_matrix[(size_t)zone_a * stride + (size_t)zone_b];
    return 1;
}

double sg_zone_similarity(const SGContext *ctx, uint32_t zone_a, uint32_t zone_b) {
    double distance = 0.0;

    if (sg_zone_distance_lookup(ctx, zone_a, zone_b, &distance)) {
        return 1.0 / (1.0 + distance);
    }
    return zone_a == zone_b ? 1.0 : 0.0;
}

double sg_priority_removal_score(const SGContext *ctx, int32_t priority) {
    int32_t clamped = sg_clamp_priority(priority);

    if (!ctx || ctx->config.priority_removal_policy == SG_PRIORITY_REMOVE_LOWER_FIRST) {
        return (double)(100 - clamped);
    }
    return (double)clamped;
}

int sg_request_time_midpoint(const SGContext *ctx, uint32_t request_id, int64_t *midpoint) {
    const SGRequestHint *hint = sg_get_hint(ctx, request_id);
    const SGRequestRecord *request = sg_get_request_record(ctx, request_id);

    if (!midpoint) {
        return 0;
    }

    if (hint && hint->has_time_window) {
        *midpoint = ((int64_t)hint->tw_early + (int64_t)hint->tw_late) / 2;
        return 1;
    }
    if (!request) {
        return 0;
    }

    if (request->kind == SG_REQUEST_KIND_DELIVERY_ONLY && request->has_delivery_task) {
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (delivery && delivery->has_time_window) {
            *midpoint = ((int64_t)delivery->tw_early + (int64_t)delivery->tw_late) / 2;
            return 1;
        }
    } else if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY &&
               request->has_pickup_task && request->has_delivery_task) {
        const SGTaskRecord *pickup = sg_get_task_record(ctx, request->pickup_task_id);
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (pickup && delivery && pickup->has_time_window && delivery->has_time_window) {
            int64_t pick_mid = ((int64_t)pickup->tw_early + (int64_t)pickup->tw_late) / 2;
            int64_t drop_mid = ((int64_t)delivery->tw_early + (int64_t)delivery->tw_late) / 2;
            *midpoint = (pick_mid + drop_mid) / 2;
            return 1;
        }
    }

    return 0;
}

int sg_request_tw_width(const SGContext *ctx, uint32_t request_id, int32_t *width_out) {
    const SGRequestHint *hint = sg_get_hint(ctx, request_id);
    const SGRequestRecord *request = sg_get_request_record(ctx, request_id);

    if (!width_out) {
        return 0;
    }

    if (hint && hint->has_time_window && hint->tw_late >= hint->tw_early) {
        *width_out = hint->tw_late - hint->tw_early;
        return 1;
    }
    if (!request) {
        return 0;
    }

    if (request->kind == SG_REQUEST_KIND_DELIVERY_ONLY && request->has_delivery_task) {
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (delivery && delivery->has_time_window && delivery->tw_late >= delivery->tw_early) {
            *width_out = delivery->tw_late - delivery->tw_early;
            return 1;
        }
    } else if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY &&
               request->has_pickup_task && request->has_delivery_task) {
        const SGTaskRecord *pickup = sg_get_task_record(ctx, request->pickup_task_id);
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (pickup && delivery && pickup->has_time_window && delivery->has_time_window) {
            int32_t early = pickup->tw_early < delivery->tw_early ? pickup->tw_early : delivery->tw_early;
            int32_t late = pickup->tw_late > delivery->tw_late ? pickup->tw_late : delivery->tw_late;
            if (late >= early) {
                *width_out = late - early;
                return 1;
            }
        }
    }

    return 0;
}

int sg_request_time_window_bounds(const SGContext *ctx, uint32_t request_id,
                                  int32_t *early_out, int32_t *late_out) {
    const SGRequestHint *hint = sg_get_hint(ctx, request_id);
    const SGRequestRecord *request = sg_get_request_record(ctx, request_id);

    if (!early_out || !late_out) {
        return 0;
    }

    if (hint && hint->has_time_window && hint->tw_late >= hint->tw_early) {
        *early_out = hint->tw_early;
        *late_out = hint->tw_late;
        return 1;
    }
    if (!request) {
        return 0;
    }

    if (request->kind == SG_REQUEST_KIND_DELIVERY_ONLY && request->has_delivery_task) {
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (delivery && delivery->has_time_window && delivery->tw_late >= delivery->tw_early) {
            *early_out = delivery->tw_early;
            *late_out = delivery->tw_late;
            return 1;
        }
    } else if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY &&
               request->has_pickup_task && request->has_delivery_task) {
        const SGTaskRecord *pickup = sg_get_task_record(ctx, request->pickup_task_id);
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (pickup && delivery && pickup->has_time_window && delivery->has_time_window) {
            *early_out = pickup->tw_early < delivery->tw_early
                       ? pickup->tw_early
                       : delivery->tw_early;
            *late_out = pickup->tw_late > delivery->tw_late
                      ? pickup->tw_late
                      : delivery->tw_late;
            if (*late_out >= *early_out) {
                return 1;
            }
        }
    }

    return 0;
}

int sg_request_centroid(const SGContext *ctx, uint32_t request_id, double *x, double *y) {
    const SGRequestRecord *request = sg_get_request_record(ctx, request_id);

    if (!x || !y || !request) {
        return 0;
    }

    if (request->kind == SG_REQUEST_KIND_DELIVERY_ONLY && request->has_delivery_task) {
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (delivery && delivery->has_location) {
            *x = delivery->x;
            *y = delivery->y;
            return 1;
        }
    } else if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY &&
               request->has_pickup_task && request->has_delivery_task) {
        const SGTaskRecord *pickup = sg_get_task_record(ctx, request->pickup_task_id);
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (pickup && delivery && pickup->has_location && delivery->has_location) {
            *x = (pickup->x + delivery->x) * 0.5;
            *y = (pickup->y + delivery->y) * 0.5;
            return 1;
        }
    }

    return 0;
}

int sg_request_representative_location(const SGContext *ctx, uint32_t request_id,
                                        uint32_t *location_id_out) {
    const SGRequestRecord *request = sg_get_request_record(ctx, request_id);

    if (!location_id_out || !request) {
        return 0;
    }

    if (request->kind == SG_REQUEST_KIND_DELIVERY_ONLY && request->has_delivery_task) {
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (delivery && delivery->location_id != UINT32_MAX) {
            *location_id_out = delivery->location_id;
            return 1;
        }
    } else if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY &&
               request->has_delivery_task) {
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (delivery && delivery->location_id != UINT32_MAX) {
            *location_id_out = delivery->location_id;
            return 1;
        }
    }

    return 0;
}

double sg_request_load_magnitude(const SGContext *ctx, uint32_t request_id) {
    const SGRequestRecord *request = sg_get_request_record(ctx, request_id);
    uint32_t d;
    double total = 0.0;

    if (!request) {
        return 0.0;
    }

    if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY &&
        request->has_pickup_task && request->has_delivery_task) {
        const SGTaskRecord *pickup = sg_get_task_record(ctx, request->pickup_task_id);
        if (pickup && pickup->has_demand && pickup->demand) {
            for (d = 0; d < ctx->dimension_count; d++) {
                total += fabs(pickup->demand[d]);
            }
        }
    } else if (request->kind == SG_REQUEST_KIND_DELIVERY_ONLY &&
               request->has_delivery_task) {
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (delivery && delivery->has_demand && delivery->demand) {
            for (d = 0; d < ctx->dimension_count; d++) {
                total += fabs(delivery->demand[d]);
            }
        }
    }

    return total;
}

double sg_request_priority_score(const SGContext *ctx, uint32_t request_id) {
    const SGRequestHint *hint = sg_get_hint(ctx, request_id);
    if (!hint) {
        return 50.0;
    }
    return (double)sg_clamp_priority(hint->priority);
}

SGRequestKind sg_request_kind(const SGContext *ctx, uint32_t request_id) {
    const SGRequestRecord *request = sg_get_request_record(ctx, request_id);
    return request ? request->kind : SG_REQUEST_KIND_UNBOUND;
}

double sg_request_abs_demand_at_dim(const SGContext *ctx, uint32_t request_id, uint32_t dim) {
    const SGRequestRecord *request = sg_get_request_record(ctx, request_id);

    if (!ctx || !request || dim >= ctx->dimension_count) {
        return 0.0;
    }

    if (request->kind == SG_REQUEST_KIND_DELIVERY_ONLY && request->has_delivery_task) {
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (delivery && delivery->has_demand && delivery->demand) {
            return fabs(delivery->demand[dim]);
        }
    }

    if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY &&
        request->has_pickup_task && request->has_delivery_task) {
        const SGTaskRecord *pickup = sg_get_task_record(ctx, request->pickup_task_id);
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        if (pickup && pickup->has_demand && pickup->demand) {
            return fabs(pickup->demand[dim]);
        }
        if (delivery && delivery->has_demand && delivery->demand) {
            return fabs(delivery->demand[dim]);
        }
    }

    return 0.0;
}

int sg_vehicle_start_end_locations(const SGContext *ctx, uint32_t vehicle_id,
                                   double *sx, double *sy, double *ex, double *ey) {
    const SGVehicleRecord *vehicle;
    const SGDepotRecord *start;
    const SGDepotRecord *end;

    if (!ctx || vehicle_id >= ctx->num_vehicles || !sx || !sy || !ex || !ey) {
        return 0;
    }

    vehicle = &ctx->vehicles[vehicle_id];
    if (!vehicle->has_depots || vehicle->start_depot_id >= ctx->num_depots ||
        vehicle->end_depot_id >= ctx->num_depots) {
        return 0;
    }

    start = &ctx->depots[vehicle->start_depot_id];
    end = &ctx->depots[vehicle->end_depot_id];
    if (!start->has_location || !end->has_location) {
        return 0;
    }

    *sx = start->x;
    *sy = start->y;
    *ex = end->x;
    *ey = end->y;
    return 1;
}

double sg_vehicle_request_cost(const SGContext *ctx, uint32_t vehicle_id,
                               uint32_t request_id, double noise_scale) {
    const SGVehicleRecord *vehicle;
    double cost = 0.0;
    int64_t midpoint;

    if (!ctx || vehicle_id >= ctx->num_vehicles || !ctx->vehicles) {
        return (double)request_id + 1.0;
    }
    if (request_id < ctx->num_requests && !sg_vehicle_qualifies(ctx, vehicle_id, request_id)) {
        return INFINITY;
    }
    if (request_id < ctx->num_requests && !sg_vehicle_allowed_for_request(ctx, vehicle_id, request_id)) {
        return INFINITY;
    }
    vehicle = &ctx->vehicles[vehicle_id];

    {
        uint32_t rep_loc;
        if (sg_request_representative_location(ctx, request_id, &rep_loc) &&
            vehicle->start_location_id != UINT32_MAX &&
            vehicle->end_location_id != UINT32_MAX) {
            double dist = 0.0;
            if (!vehicle->open_start) {
                dist += sg_travel_dist(ctx, vehicle->start_location_id, rep_loc, vehicle_id);
            }
            if (!vehicle->open_end) {
                dist += sg_travel_dist(ctx, rep_loc, vehicle->end_location_id, vehicle_id);
            }
            cost += vehicle->cost_per_distance * dist;
        } else {
            cost += (double)((request_id % 17U) + 1U);
        }
    }

    if (sg_request_time_midpoint(ctx, request_id, &midpoint) && vehicle->has_shift_time_window) {
        if (midpoint < vehicle->shift_early) {
            cost += (double)(vehicle->shift_early - midpoint) / 1800.0;
        } else if (midpoint > vehicle->shift_late) {
            cost += (double)(midpoint - vehicle->shift_late) / 1800.0;
        }
    }

    cost += sg_request_load_magnitude(ctx, request_id) * 0.05;
    cost += (100.0 - sg_request_priority_score(ctx, request_id)) * 0.01;

    if (noise_scale > 0.0 && ctx->op_rng) {
        double draw = sh_rng_uniform_range(ctx->op_rng, -noise_scale, noise_scale);
        cost *= (1.0 + draw);
    }

    if (!isfinite(cost) || cost < 0.0) {
        return (double)request_id + 1.0;
    }
    return cost;
}

int sg_request_best_k_costs(const SGContext *ctx, uint32_t request_id, int k,
                            double noise_scale, double *best_cost,
                            double *kth_cost) {
    double *costs;
    uint32_t v;
    int i;
    int j;
    int count = 0;

    if (!best_cost || !kth_cost || k <= 0) {
        return 0;
    }

    if (!ctx || ctx->num_vehicles == 0) {
        *best_cost = sg_vehicle_request_cost(ctx, 0, request_id, noise_scale);
        *kth_cost = *best_cost + 1.0;
        return 1;
    }

    costs = (double *)malloc((size_t)ctx->num_vehicles * sizeof(double));
    if (!costs) {
        return 0;
    }

    for (v = 0; v < ctx->num_vehicles; v++) {
        costs[count++] = sg_vehicle_request_cost(ctx, v, request_id, noise_scale);
    }

    for (i = 0; i < count; i++) {
        for (j = i + 1; j < count; j++) {
            if (costs[j] < costs[i]) {
                double tmp = costs[i];
                costs[i] = costs[j];
                costs[j] = tmp;
            }
        }
    }

    *best_cost = costs[0];
    if (count >= k) {
        *kth_cost = costs[k - 1];
    } else {
        *kth_cost = costs[count - 1] + 1.0;
    }

    free(costs);
    return 1;
}

double sg_zone_density_score(const SGContext *ctx,
                             const SGBootstrapSolution *sol,
                             uint32_t request_id) {
    const SGRequestHint *target_hint = sg_get_hint(ctx, request_id);
    uint32_t i;
    double density = 0.0;

    if (!target_hint || !target_hint->has_zone || !sol) {
        return 0.0;
    }

    for (i = 0; i < sol->num_assigned; i++) {
        const SGRequestHint *hint = sg_get_hint(ctx, sol->assigned_ids[i]);
        if (hint && hint->has_zone) {
            density += sg_zone_similarity(ctx, target_hint->zone_id, hint->zone_id);
        }
    }

    if (density <= 1.0) {
        return 0.0;
    }
    return (density - 1.0) * 0.25;
}

double sg_bootstrap_removal_cost(void *ctx, void *solution, uint32_t element_id) {
    const SGContext *sg_ctx = (const SGContext *)ctx;
    const SGBootstrapSolution *sol = (const SGBootstrapSolution *)solution;
    const SGRequestHint *hint = sg_get_hint(sg_ctx, element_id);
    double score = 0.0;

    if (!hint) {
        return (double)element_id + 1.0;
    }

    /* Priority contribution is configurable to fit tenant policy. */
    score += sg_priority_removal_score(sg_ctx, hint->priority) * 0.5;

    /* Wider windows are easier to reinsert, so removal score increases. */
    if (hint->has_time_window && hint->tw_late > hint->tw_early) {
        int32_t width = hint->tw_late - hint->tw_early;
        score += ((double)width / 3600.0) * 0.5;
    }

    /* Over-represented zones are slightly preferred for removal. */
    score += sg_zone_density_score(sg_ctx, sol, element_id);

    /* Stable tie-breaker. */
    score += ((double)element_id + 1.0) * 0.0001;

    return score;
}

double sg_bootstrap_relatedness(void *ctx, uint32_t a, uint32_t b) {
    const SGContext *sg_ctx = (const SGContext *)ctx;
    const SGRequestHint *ha = sg_get_hint(sg_ctx, a);
    const SGRequestHint *hb = sg_get_hint(sg_ctx, b);
    double score = 0.0;

    if (!ha || !hb) {
        uint32_t diff = (a > b) ? (a - b) : (b - a);
        return -(double)diff;
    }

    if (ha->has_zone && hb->has_zone) {
        double similarity = sg_zone_similarity(sg_ctx, ha->zone_id, hb->zone_id);
        score += 12.0 * similarity - 2.0;
    }

    if (ha->has_time_window && hb->has_time_window) {
        int64_t mid_a = ((int64_t)ha->tw_early + (int64_t)ha->tw_late) / 2;
        int64_t mid_b = ((int64_t)hb->tw_early + (int64_t)hb->tw_late) / 2;
        int64_t delta = sg_abs_i64(mid_a - mid_b);
        score -= ((double)delta / 3600.0);
    }

    score -= (double)sg_abs_i64((int64_t)ha->priority - (int64_t)hb->priority) / 20.0;
    score += 1.0 / (1.0 + (double)sg_abs_i64((int64_t)a - (int64_t)b));

    return score;
}

double sg_route_cluster_relatedness(void *ctx, uint32_t a, uint32_t b) {
    const SGContext *sg_ctx = (const SGContext *)ctx;
    double score = 0.0;
    int64_t ta, tb;
    const SGRequestHint *ha = sg_get_hint(sg_ctx, a);
    const SGRequestHint *hb = sg_get_hint(sg_ctx, b);

    {
        uint32_t loc_a, loc_b;
        if (sg_request_representative_location(sg_ctx, a, &loc_a) &&
            sg_request_representative_location(sg_ctx, b, &loc_b)) {
            score -= sg_travel_dist(sg_ctx, loc_a, loc_b, SG_NO_VEHICLE);
        }
    }

    if (ha && hb && ha->has_zone && hb->has_zone) {
        score += 8.0 * sg_zone_similarity(sg_ctx, ha->zone_id, hb->zone_id);
    }

    if (sg_request_time_midpoint(sg_ctx, a, &ta) && sg_request_time_midpoint(sg_ctx, b, &tb)) {
        score -= (double)sg_abs_i64(ta - tb) / 3600.0;
    }

    if (sg_request_kind(sg_ctx, a) == sg_request_kind(sg_ctx, b)) {
        score += 1.0;
    }

    score += 1.0 / (1.0 + (double)sg_abs_i64((int64_t)a - (int64_t)b));
    return score;
}

double sg_time_cluster_relatedness(void *ctx, uint32_t a, uint32_t b) {
    const SGContext *sg_ctx = (const SGContext *)ctx;
    int64_t ta, tb;
    const SGRequestHint *ha = sg_get_hint(sg_ctx, a);
    const SGRequestHint *hb = sg_get_hint(sg_ctx, b);
    double score = 0.0;

    if (sg_request_time_midpoint(sg_ctx, a, &ta) && sg_request_time_midpoint(sg_ctx, b, &tb)) {
        score -= (double)sg_abs_i64(ta - tb) / 1800.0;
    } else {
        score -= (double)sg_abs_i64((int64_t)a - (int64_t)b);
    }

    if (ha && hb && ha->has_zone && hb->has_zone) {
        score += 3.0 * sg_zone_similarity(sg_ctx, ha->zone_id, hb->zone_id);
    }

    score -= fabs(sg_request_priority_score(sg_ctx, a) -
                  sg_request_priority_score(sg_ctx, b)) / 30.0;
    return score;
}

double sg_time_window_relatedness(void *ctx, uint32_t a, uint32_t b) {
    const SGContext *sg_ctx = (const SGContext *)ctx;
    int32_t early_a = 0;
    int32_t late_a = 0;
    int32_t early_b = 0;
    int32_t late_b = 0;
    int64_t mid_a = 0;
    int64_t mid_b = 0;
    double score = 0.0;

    if (sg_request_time_window_bounds(sg_ctx, a, &early_a, &late_a) &&
        sg_request_time_window_bounds(sg_ctx, b, &early_b, &late_b)) {
        int32_t overlap_start = early_a > early_b ? early_a : early_b;
        int32_t overlap_end = late_a < late_b ? late_a : late_b;
        int32_t span_start = early_a < early_b ? early_a : early_b;
        int32_t span_end = late_a > late_b ? late_a : late_b;
        double overlap = overlap_end > overlap_start
                       ? (double)(overlap_end - overlap_start)
                       : 0.0;
        double span = span_end > span_start
                    ? (double)(span_end - span_start)
                    : 1.0;
        double width_delta = fabs((double)(late_a - early_a) - (double)(late_b - early_b));
        double midpoint_delta = fabs((((double)early_a + (double)late_a) * 0.5) -
                                     (((double)early_b + (double)late_b) * 0.5));

        score += 16.0 * (overlap / span);
        score -= midpoint_delta / 1800.0;
        score -= width_delta / 3600.0;
    } else if (sg_request_time_midpoint(sg_ctx, a, &mid_a) &&
               sg_request_time_midpoint(sg_ctx, b, &mid_b)) {
        score -= (double)sg_abs_i64(mid_a - mid_b) / 1800.0;
    } else {
        score -= (double)sg_abs_i64((int64_t)a - (int64_t)b);
    }

    if (sg_request_kind(sg_ctx, a) == sg_request_kind(sg_ctx, b)) {
        score += 0.5;
    }
    score += 1.0 / (1.0 + (double)sg_abs_i64((int64_t)a - (int64_t)b));
    return score;
}

double sg_pd_shaw_relatedness(void *ctx, uint32_t a, uint32_t b) {
    const SGContext *sg_ctx = (const SGContext *)ctx;
    SGRequestKind kind_a = sg_request_kind(sg_ctx, a);
    SGRequestKind kind_b = sg_request_kind(sg_ctx, b);
    double score = sg_bootstrap_relatedness(ctx, a, b);
    double load_a = sg_request_load_magnitude(sg_ctx, a);
    double load_b = sg_request_load_magnitude(sg_ctx, b);

    if (kind_a == SG_REQUEST_KIND_PICKUP_DELIVERY &&
        kind_b == SG_REQUEST_KIND_PICKUP_DELIVERY) {
        score += 4.0;
    } else if (kind_a != kind_b) {
        score -= 1.0;
    }

    score -= fabs(load_a - load_b) / 50.0;
    return score;
}

double sg_route_shaw_relatedness(void *ctx, uint32_t a, uint32_t b) {
    const SGContext *sg_ctx = (const SGContext *)ctx;
    const SGRouteSolution *sol = (const SGRouteSolution *)sg_ctx->active_solution;
    int32_t early_a, late_a, early_b, late_b;
    SGRequestKind kind_a, kind_b;
    double score = 0.0;

    /* Spatial distance term */
    {
        uint32_t loc_a, loc_b;
        if (sg_request_representative_location(sg_ctx, a, &loc_a) &&
            sg_request_representative_location(sg_ctx, b, &loc_b)) {
            score -= sg_travel_dist(sg_ctx, loc_a, loc_b, SG_NO_VEHICLE) / 40.0;
        }
    }

    /* TW overlap term */
    if (sg_request_time_window_bounds(sg_ctx, a, &early_a, &late_a) &&
        sg_request_time_window_bounds(sg_ctx, b, &early_b, &late_b)) {
        int32_t overlap_start = early_a > early_b ? early_a : early_b;
        int32_t overlap_end = late_a < late_b ? late_a : late_b;
        int32_t span_start = early_a < early_b ? early_a : early_b;
        int32_t span_end = late_a > late_b ? late_a : late_b;
        double overlap = overlap_end > overlap_start
                       ? (double)(overlap_end - overlap_start) : 0.0;
        double span = span_end > span_start
                    ? (double)(span_end - span_start) : 1.0;
        score += 3.0 * (overlap / span);
    }

    /* Load similarity term */
    score -= fabs(sg_request_load_magnitude(sg_ctx, a) -
                  sg_request_load_magnitude(sg_ctx, b)) / 100.0;

    /* Co-route bonus */
    if (sol && sol->request_vehicle) {
        uint32_t va = sol->request_vehicle[a];
        uint32_t vb = sol->request_vehicle[b];
        if (va != UINT32_MAX && vb != UINT32_MAX && va == vb) {
            score += 5.0;
        }
    }

    /* PD kind bonus */
    kind_a = sg_request_kind(sg_ctx, a);
    kind_b = sg_request_kind(sg_ctx, b);
    if (kind_a == SG_REQUEST_KIND_PICKUP_DELIVERY &&
        kind_b == SG_REQUEST_KIND_PICKUP_DELIVERY) {
        score += 2.0;
    } else if (kind_a != kind_b) {
        score -= 0.5;
    }

    /* Deterministic tie-breaker */
    score += 1.0 / (1.0 + (double)sg_abs_i64((int64_t)a - (int64_t)b));

    return score;
}

double sg_route_removal_cost(void *ctx, void *solution, uint32_t element_id) {
    const SGContext *sg_ctx = (const SGContext *)ctx;
    const SGRouteSolution *sol = (const SGRouteSolution *)solution;
    const SGRequestRecord *request;
    const SGVehicleRecord *vehicle;
    uint32_t vehicle_id;
    const SGRouteStop *stops;
    const uint32_t *prev_arr;
    const uint32_t *next_arr;
    uint32_t start_loc, end_loc;
    double saving = 0.0;

    if (!sg_ctx || !sol || element_id >= sg_ctx->num_requests) {
        return 0.0;
    }

    vehicle_id = sol->request_vehicle[element_id];
    if (vehicle_id == UINT32_MAX || vehicle_id >= sg_ctx->num_vehicles) {
        return 0.0;
    }

    request = sg_get_request_record(sg_ctx, element_id);
    if (!request) {
        return 0.0;
    }

    vehicle = &sg_ctx->vehicles[vehicle_id];
    if (vehicle->start_location_id == UINT32_MAX || vehicle->end_location_id == UINT32_MAX) {
        return 0.0;
    }
    start_loc = vehicle->start_location_id;
    end_loc = vehicle->end_location_id;

    stops = sg_route_vehicle_stop_ptr_const(sol, vehicle_id);
    prev_arr = sg_route_vehicle_stop_prev_ptr_const(sol, vehicle_id);
    next_arr = sg_route_vehicle_stop_next_ptr_const(sol, vehicle_id);

    if (request->kind == SG_REQUEST_KIND_DELIVERY_ONLY) {
        uint32_t d_pos = sol->request_delivery_stop_pos[element_id];
        uint32_t p = prev_arr[d_pos];
        uint32_t n = next_arr[d_pos];
        uint32_t d_loc = sg_ctx->tasks[stops[d_pos].task_id].location_id;
        uint32_t p_loc;
        int first_stop = (p == UINT32_MAX);

        p_loc = first_stop ? start_loc : sg_ctx->tasks[stops[p].task_id].location_id;

        if (n == UINT32_MAX && vehicle->open_end) {
            /* Last stop on open-end route: saving is just the leg to this stop */
            saving = (first_stop && vehicle->open_start) ? 0.0
                   : sg_travel_dist(sg_ctx, p_loc, d_loc, vehicle_id);
        } else {
            uint32_t n_loc = (n == UINT32_MAX) ? end_loc : sg_ctx->tasks[stops[n].task_id].location_id;
            double leg_in = (first_stop && vehicle->open_start) ? 0.0
                          : sg_travel_dist(sg_ctx, p_loc, d_loc, vehicle_id);
            double leg_out = sg_travel_dist(sg_ctx, d_loc, n_loc, vehicle_id);
            double direct = (first_stop && vehicle->open_start) ? 0.0
                           : sg_travel_dist(sg_ctx, p_loc, n_loc, vehicle_id);
            saving = leg_in + leg_out - direct;
        }
    } else if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY) {
        uint32_t p_pos = sol->request_pickup_stop_pos[element_id];
        uint32_t d_pos = sol->request_delivery_stop_pos[element_id];
        uint32_t pick_loc = sg_ctx->tasks[stops[p_pos].task_id].location_id;
        uint32_t del_loc = sg_ctx->tasks[stops[d_pos].task_id].location_id;

        if (d_pos == p_pos + 1) {
            /* Adjacent: remove both as one segment */
            uint32_t pp = prev_arr[p_pos];
            uint32_t nd = next_arr[d_pos];
            uint32_t pp_loc;
            int first_stop = (pp == UINT32_MAX);

            pp_loc = first_stop ? start_loc : sg_ctx->tasks[stops[pp].task_id].location_id;

            if (nd == UINT32_MAX && vehicle->open_end) {
                saving = ((first_stop && vehicle->open_start) ? 0.0
                       : sg_travel_dist(sg_ctx, pp_loc, pick_loc, vehicle_id))
                       + sg_travel_dist(sg_ctx, pick_loc, del_loc, vehicle_id);
            } else {
                uint32_t nd_loc = (nd == UINT32_MAX) ? end_loc : sg_ctx->tasks[stops[nd].task_id].location_id;
                saving = ((first_stop && vehicle->open_start) ? 0.0
                       : sg_travel_dist(sg_ctx, pp_loc, pick_loc, vehicle_id))
                       + sg_travel_dist(sg_ctx, pick_loc, del_loc, vehicle_id)
                       + sg_travel_dist(sg_ctx, del_loc, nd_loc, vehicle_id)
                       - ((first_stop && vehicle->open_start) ? 0.0
                       : sg_travel_dist(sg_ctx, pp_loc, nd_loc, vehicle_id));
            }
        } else {
            /* Non-adjacent: sum independent savings */
            uint32_t pp = prev_arr[p_pos];
            uint32_t np = next_arr[p_pos];
            uint32_t pd = prev_arr[d_pos];
            uint32_t nd = next_arr[d_pos];
            uint32_t pp_loc, np_loc, pd_loc;
            int first_stop = (pp == UINT32_MAX);

            pp_loc = first_stop ? start_loc : sg_ctx->tasks[stops[pp].task_id].location_id;
            np_loc = sg_ctx->tasks[stops[np].task_id].location_id;
            pd_loc = sg_ctx->tasks[stops[pd].task_id].location_id;

            if (first_stop && vehicle->open_start) {
                saving = sg_travel_dist(sg_ctx, pick_loc, np_loc, vehicle_id);
            } else {
                saving = sg_travel_dist(sg_ctx, pp_loc, pick_loc, vehicle_id)
                       + sg_travel_dist(sg_ctx, pick_loc, np_loc, vehicle_id)
                       - sg_travel_dist(sg_ctx, pp_loc, np_loc, vehicle_id);
            }

            if (nd == UINT32_MAX && vehicle->open_end) {
                saving += sg_travel_dist(sg_ctx, pd_loc, del_loc, vehicle_id);
            } else {
                uint32_t nd_loc = (nd == UINT32_MAX) ? end_loc : sg_ctx->tasks[stops[nd].task_id].location_id;
                saving += sg_travel_dist(sg_ctx, pd_loc, del_loc, vehicle_id)
                        + sg_travel_dist(sg_ctx, del_loc, nd_loc, vehicle_id)
                        - sg_travel_dist(sg_ctx, pd_loc, nd_loc, vehicle_id);
            }
        }
    }

    saving += ((double)element_id + 1.0) * 0.0001;
    return saving;
}

double sg_criticality_removal_cost(void *ctx, void *solution, uint32_t element_id) {
    const SGContext *sg_ctx = (const SGContext *)ctx;
    const SGBootstrapSolution *sol = (const SGBootstrapSolution *)solution;
    double score = 0.0;
    int32_t tw_width = 0;

    score += (100.0 - sg_request_priority_score(sg_ctx, element_id)) * 0.7;

    if (sg_request_tw_width(sg_ctx, element_id, &tw_width)) {
        score += ((double)tw_width / 3600.0) * 0.8;
    } else {
        score += 1.0;
    }

    score += sg_zone_density_score(sg_ctx, sol, element_id);
    score += sg_request_load_magnitude(sg_ctx, element_id) * 0.05;
    score += ((double)element_id + 1.0) * 0.0001;
    return score;
}

int sg_request_time_use_for_vehicle(const SGContext *ctx, uint32_t vehicle_id,
                                    uint32_t request_id, double *time_use_seconds) {
    const SGRequestRecord *request = sg_get_request_record(ctx, request_id);
    const SGVehicleRecord *vehicle;
    uint32_t v_start_loc, v_end_loc;
    int has_locations;
    double shift_early;
    double shift_late;
    double consumed;

    if (!ctx || !time_use_seconds || vehicle_id >= ctx->num_vehicles) {
        return 0;
    }
    vehicle = &ctx->vehicles[vehicle_id];

    shift_early = vehicle->has_shift_time_window ? (double)vehicle->shift_early : 0.0;
    shift_late = vehicle->has_shift_time_window ? (double)vehicle->shift_late : INFINITY;
    v_start_loc = vehicle->start_location_id;
    v_end_loc = vehicle->end_location_id;
    has_locations = (v_start_loc != UINT32_MAX && v_end_loc != UINT32_MAX);

    if (!request || request->kind == SG_REQUEST_KIND_UNBOUND) {
        *time_use_seconds = SG_CONSTRUCT_FALLBACK_SECONDS;
        return 1;
    }

    if (request->kind == SG_REQUEST_KIND_DELIVERY_ONLY && request->has_delivery_task) {
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        double dur1;
        double dur2;
        double arrival;
        double service_start;
        double finish;
        double wait = 0.0;

        if (!delivery || !delivery->has_location || delivery->location_id == UINT32_MAX) {
            *time_use_seconds = SG_CONSTRUCT_FALLBACK_SECONDS;
            return 1;
        }

        dur1 = (has_locations && !vehicle->open_start) ? sg_travel_dur(ctx, v_start_loc, delivery->location_id, vehicle_id, 0.0) : 0.0;
        if (!has_locations && !vehicle->open_start) dur1 = 1.0;
        dur2 = (has_locations && !vehicle->open_end) ? sg_travel_dur(ctx, delivery->location_id, v_end_loc, vehicle_id, 0.0) : 0.0;
        if (!has_locations && !vehicle->open_end) dur2 = 1.0;
        arrival = shift_early + dur1;
        service_start = arrival;

        if (delivery->has_time_window) {
            double snapped = sg_task_snap_forward(delivery, service_start);
            wait = snapped - service_start;
            service_start = snapped;
            if (service_start > (double)delivery->tw_late) {
                return 0;
            }
        }

        finish = service_start + (double)delivery->service_seconds + dur2;
        if (finish > shift_late + 1e-9) {
            return 0;
        }

        consumed = dur1 + wait + (double)delivery->service_seconds + dur2;
        if (vehicle->max_duration_seconds > 0 && consumed > (double)vehicle->max_duration_seconds + 1e-9) {
            return 0;
        }
        if (!isfinite(consumed) || consumed < 0.0) {
            return 0;
        }
        *time_use_seconds = consumed;
        return 1;
    }

    if (request->kind == SG_REQUEST_KIND_PICKUP_DELIVERY &&
        request->has_pickup_task && request->has_delivery_task) {
        const SGTaskRecord *pickup = sg_get_task_record(ctx, request->pickup_task_id);
        const SGTaskRecord *delivery = sg_get_task_record(ctx, request->delivery_task_id);
        double dur_start_pick;
        double dur_pick_drop;
        double dur_drop_end;
        double arrive_pick;
        double start_pick;
        double leave_pick;
        double arrive_drop;
        double start_drop;
        double finish;
        double wait_pick = 0.0;
        double wait_drop = 0.0;

        if (!pickup || !delivery || !pickup->has_location || !delivery->has_location ||
            pickup->location_id == UINT32_MAX || delivery->location_id == UINT32_MAX) {
            *time_use_seconds = SG_CONSTRUCT_FALLBACK_SECONDS * 2.0;
            return 1;
        }

        dur_start_pick = (has_locations && !vehicle->open_start) ? sg_travel_dur(ctx, v_start_loc, pickup->location_id, vehicle_id, 0.0) : 0.0;
        if (!has_locations && !vehicle->open_start) dur_start_pick = 1.0;
        dur_pick_drop = sg_travel_dur(ctx, pickup->location_id, delivery->location_id, vehicle_id, 0.0);
        dur_drop_end = (has_locations && !vehicle->open_end) ? sg_travel_dur(ctx, delivery->location_id, v_end_loc, vehicle_id, 0.0) : 0.0;
        if (!has_locations && !vehicle->open_end) dur_drop_end = 1.0;

        arrive_pick = shift_early + dur_start_pick;
        start_pick = arrive_pick;
        if (pickup->has_time_window) {
            double snapped = sg_task_snap_forward(pickup, start_pick);
            wait_pick = snapped - start_pick;
            start_pick = snapped;
            if (start_pick > (double)pickup->tw_late) {
                return 0;
            }
        }
        leave_pick = start_pick + (double)pickup->service_seconds;
        arrive_drop = leave_pick + dur_pick_drop;
        start_drop = arrive_drop;
        if (delivery->has_time_window) {
            double snapped = sg_task_snap_forward(delivery, start_drop);
            wait_drop = snapped - start_drop;
            start_drop = snapped;
            if (start_drop > (double)delivery->tw_late) {
                return 0;
            }
        }

        finish = start_drop + (double)delivery->service_seconds + dur_drop_end;
        if (finish > shift_late + 1e-9) {
            return 0;
        }

        consumed = dur_start_pick + wait_pick + (double)pickup->service_seconds +
                   dur_pick_drop + wait_drop +
                   (double)delivery->service_seconds + dur_drop_end;
        if (vehicle->max_duration_seconds > 0 && consumed > (double)vehicle->max_duration_seconds + 1e-9) {
            return 0;
        }
        if (!isfinite(consumed) || consumed < 0.0) {
            return 0;
        }
        *time_use_seconds = consumed;
        return 1;
    }

    *time_use_seconds = SG_CONSTRUCT_FALLBACK_SECONDS;
    return 1;
}

static int sg_compute_delivery_only_route_metrics(const SGContext *ctx,
                                                  const SGBootstrapSolution *sol,
                                                  uint32_t *vehicles_used_out,
                                                  double *total_distance_out) {
    uint32_t *remaining = NULL;
    uint32_t *routes = NULL;
    uint32_t *route_lens = NULL;
    uint32_t *candidate = NULL;
    double *route_distances = NULL;
    double *capacity_scratch = NULL;
    uint32_t remaining_count;
    size_t route_stride;
    uint32_t v;
    uint32_t used_vehicles = 0;
    double total_distance = 0.0;
    int ok = 0;

    if (!ctx || !sol || !vehicles_used_out || !total_distance_out ||
        ctx->num_vehicles == 0 || ctx->num_requests == 0) {
        return 0;
    }
    if (sol->num_assigned == 0) {
        *vehicles_used_out = 0;
        *total_distance_out = 0.0;
        return 1;
    }

    route_stride = (size_t)sol->num_assigned;
    if (route_stride == 0 || (size_t)ctx->num_vehicles > SIZE_MAX / route_stride) {
        return 0;
    }

    remaining = (uint32_t *)malloc((size_t)sol->num_assigned * sizeof(uint32_t));
    routes = (uint32_t *)malloc((size_t)ctx->num_vehicles * route_stride * sizeof(uint32_t));
    route_lens = (uint32_t *)calloc((size_t)ctx->num_vehicles, sizeof(uint32_t));
    route_distances = (double *)calloc((size_t)ctx->num_vehicles, sizeof(double));
    candidate = (uint32_t *)malloc((route_stride + 1U) * sizeof(uint32_t));
    capacity_scratch = (double *)malloc((size_t)ctx->dimension_count * sizeof(double));

    if (!remaining || !routes || !route_lens || !route_distances || !candidate ||
        !capacity_scratch) {
        goto done;
    }

    memcpy(remaining, sol->assigned_ids, (size_t)sol->num_assigned * sizeof(uint32_t));
    remaining_count = sol->num_assigned;

    while (remaining_count > 0) {
        int selected_idx = -1;
        uint32_t selected_vehicle = UINT32_MAX;
        uint32_t selected_pos = 0;
        uint32_t selected_request = UINT32_MAX;
        uint32_t selected_feasible_count = UINT32_MAX;
        double selected_route_distance = 0.0;
        double selected_delta = INFINITY;
        uint32_t ri;

        for (ri = 0; ri < remaining_count; ri++) {
            uint32_t request_id = remaining[ri];
            uint32_t req_feasible_count = 0;
            uint32_t req_best_vehicle = UINT32_MAX;
            uint32_t req_best_pos = 0;
            double req_best_route_distance = 0.0;
            double req_best_delta = INFINITY;

            for (v = 0; v < ctx->num_vehicles; v++) {
                uint32_t *route = &routes[(size_t)v * route_stride];
                uint32_t len = route_lens[v];
                uint32_t pos;

                for (pos = 0; pos <= len; pos++) {
                    double candidate_distance = 0.0;
                    double delta;

                    if (pos > 0) {
                        memcpy(candidate, route, (size_t)pos * sizeof(uint32_t));
                    }
                    candidate[pos] = request_id;
                    if (pos < len) {
                        memcpy(&candidate[pos + 1], &route[pos],
                               (size_t)(len - pos) * sizeof(uint32_t));
                    }

                    if (!sg_route_sequence_feasible_distance(ctx, v, candidate, len + 1,
                                                             &candidate_distance,
                                                             capacity_scratch)) {
                        continue;
                    }

                    req_feasible_count++;
                    delta = candidate_distance - route_distances[v];
                    if (delta < req_best_delta - 1e-9 ||
                        (fabs(delta - req_best_delta) <= 1e-9 &&
                         (v < req_best_vehicle ||
                          (v == req_best_vehicle && pos < req_best_pos)))) {
                        req_best_delta = delta;
                        req_best_vehicle = v;
                        req_best_pos = pos;
                        req_best_route_distance = candidate_distance;
                    }
                }
            }

            if (req_feasible_count == 0) {
                continue;
            }

            if (req_feasible_count < selected_feasible_count ||
                (req_feasible_count == selected_feasible_count &&
                 req_best_delta < selected_delta - 1e-9) ||
                (req_feasible_count == selected_feasible_count &&
                 fabs(req_best_delta - selected_delta) <= 1e-9 &&
                 request_id < selected_request)) {
                selected_idx = (int)ri;
                selected_request = request_id;
                selected_vehicle = req_best_vehicle;
                selected_pos = req_best_pos;
                selected_route_distance = req_best_route_distance;
                selected_feasible_count = req_feasible_count;
                selected_delta = req_best_delta;
            }
        }

        if (selected_idx < 0 || selected_vehicle == UINT32_MAX || selected_request == UINT32_MAX) {
            goto done;
        }

        {
            uint32_t *route = &routes[(size_t)selected_vehicle * route_stride];
            uint32_t len = route_lens[selected_vehicle];
            sg_route_insert_request(route, &len, selected_pos, selected_request);
            route_lens[selected_vehicle] = len;
            route_distances[selected_vehicle] = selected_route_distance;
        }

        remaining_count--;
        remaining[(uint32_t)selected_idx] = remaining[remaining_count];
    }

    for (v = 0; v < ctx->num_vehicles; v++) {
        if (route_lens[v] > 0) {
            used_vehicles++;
            total_distance += route_distances[v];
        }
    }

    *vehicles_used_out = used_vehicles;
    *total_distance_out = total_distance;
    ok = 1;

done:
    free(remaining);
    free(routes);
    free(route_lens);
    free(route_distances);
    free(candidate);
    free(capacity_scratch);
    return ok;
}

void sg_compute_solution_route_metrics(const SGContext *ctx, const SGBootstrapSolution *sol,
                                       uint32_t *vehicles_used_out,
                                       double *total_distance_out) {
    uint32_t vehicles_used = 0;
    double total_distance = 0.0;
    uint32_t i;

    if (!vehicles_used_out || !total_distance_out) {
        return;
    }

    if (!ctx || !sol) {
        *vehicles_used_out = 0;
        *total_distance_out = 0.0;
        return;
    }

    if (sg_compute_delivery_only_route_metrics(ctx, sol, &vehicles_used, &total_distance)) {
        *vehicles_used_out = vehicles_used;
        *total_distance_out = total_distance;
        return;
    }

    if (ctx->num_vehicles > 0) {
        vehicles_used = sol->num_assigned < ctx->num_vehicles ? sol->num_assigned : ctx->num_vehicles;
    }
    for (i = 0; i < sol->num_assigned; i++) {
        uint32_t request_id = sol->assigned_ids[i];
        uint32_t v;
        uint32_t rep_loc;
        double best_leg = INFINITY;

        if (!sg_request_representative_location(ctx, request_id, &rep_loc)) {
            continue;
        }

        for (v = 0; v < ctx->num_vehicles; v++) {
            const SGVehicleRecord *vehicle = &ctx->vehicles[v];
            double leg;

            if (vehicle->start_location_id == UINT32_MAX ||
                vehicle->end_location_id == UINT32_MAX) {
                continue;
            }
            leg = sg_travel_dist(ctx, vehicle->start_location_id, rep_loc, v) +
                  sg_travel_dist(ctx, rep_loc, vehicle->end_location_id, v);
            if (leg < best_leg) {
                best_leg = leg;
            }
        }

        if (isfinite(best_leg)) {
            total_distance += best_leg;
        }
    }

    *vehicles_used_out = vehicles_used;
    *total_distance_out = total_distance;
}
