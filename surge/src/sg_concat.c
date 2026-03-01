#include "sg_internal.h"

/*
 * Concatenation-based O(1) feasibility checks (capacity + timing).
 *
 * CAPACITY (Phase 1):
 * For each vehicle route, we maintain prefix and suffix capacity segment
 * summaries per dimension.  Each segment stores (delta, min_prefix,
 * max_prefix) so that two adjacent segments can be concatenated in O(1):
 *
 *   out_delta = left_delta + right_delta
 *   out_min   = min(left_min, left_delta + right_min)
 *   out_max   = max(left_max, left_delta + right_max)
 *
 * Feasibility check: (max - min) <= capacity, which is O(1).
 *
 * TIMING (Phase 2):
 * Prefix/suffix SGSegSummary arrays enable O(1) timing/distance evaluation
 * via Vidal 2012 segment concatenation.  Each segment stores earliest_start,
 * latest_start, duration, time_warp, wait_time, distance, and boundary
 * location/request IDs.
 *
 * The prefix/suffix arrays are rebuilt O(L) in sg_route_build_segments()
 * at the end of sg_route_update_timing() — the same asymptotic cost as the
 * existing timing forward/backward passes.  But each subsequent insertion
 * evaluation drops from O(L) to O(1).
 */

/* O(1) concatenation of two capacity segments for a single dimension. */
void sg_seg_concat_capacity(
    double left_delta, double left_min, double left_max,
    double right_delta, double right_min, double right_max,
    double *out_delta, double *out_min, double *out_max)
{
    double shifted_right_min = left_delta + right_min;
    double shifted_right_max = left_delta + right_max;

    *out_delta = left_delta + right_delta;
    *out_min = left_min < shifted_right_min ? left_min : shifted_right_min;
    *out_max = left_max > shifted_right_max ? left_max : shifted_right_max;
}

/*
 * Build prefix and suffix capacity segment arrays for one vehicle.
 * Called after sg_route_update_timing() + sg_route_update_load().
 *
 * Layout: [vehicle * (stop_stride+1) * dim_count + stop * dim_count + d]
 *
 * prefix[i] = capacity summary for stops[trip_start .. i-1] within the
 *             trip containing position i.  prefix[0] = empty = {0,0,0}.
 *             Resets to {0,0,0} at each trip boundary.
 *
 * suffix[i] = capacity summary for stops[i .. trip_end-1] within the
 *             trip containing position i.  suffix[stop_len] = {0,0,0}.
 *             Resets to {0,0,0} at trip boundaries.
 */
static void sg_route_build_cap_segments(const SGContext *ctx, SGRouteSolution *sol,
                                         uint32_t vehicle_id)
{
    const SGRouteStop *stops;
    uint32_t stop_len, dim_count;
    size_t seg_stride;
    size_t base;
    double *pd, *pm, *px;   /* prefix delta/min/max */
    double *sd, *smn, *sx;  /* suffix delta/min/max */
    uint32_t i, d;

    if (!ctx || !sol || vehicle_id >= sol->num_vehicles) return;

    dim_count = ctx->dimension_count;
    if (dim_count == 0) return;
    if (!sol->route_seg_cap_prefix_delta) return;

    stops = sg_route_vehicle_stop_ptr_const(sol, vehicle_id);
    stop_len = sol->route_stop_lengths[vehicle_id];
    seg_stride = (size_t)(sol->stop_stride + 1U) * (size_t)dim_count;
    base = (size_t)vehicle_id * seg_stride;

    pd = sol->route_seg_cap_prefix_delta + base;
    pm = sol->route_seg_cap_prefix_min   + base;
    px = sol->route_seg_cap_prefix_max   + base;
    sd = sol->route_seg_cap_suffix_delta + base;
    smn = sol->route_seg_cap_suffix_min  + base;
    sx = sol->route_seg_cap_suffix_max   + base;

    /* Initialize prefix[0] = empty segment */
    for (d = 0; d < dim_count; d++) {
        pd[d] = 0.0;
        pm[d] = 0.0;
        px[d] = 0.0;
    }

    /* Forward pass: build prefix arrays */
    for (i = 0; i < stop_len; i++) {
        const SGTaskRecord *task = &ctx->tasks[stops[i].task_id];
        size_t cur = (size_t)i * dim_count;
        size_t nxt = (size_t)(i + 1) * dim_count;

        /* Trip boundary reset: prefix restarts for new trip */
        if (i > 0 && stops[i].trip_start) {
            for (d = 0; d < dim_count; d++) {
                pd[cur + d] = 0.0;
                pm[cur + d] = 0.0;
                px[cur + d] = 0.0;
            }
        }

        for (d = 0; d < dim_count; d++) {
            double demand = (task->has_demand && task->demand)
                ? task->demand[d] : 0.0;
            double stop_min = demand < 0.0 ? demand : 0.0;
            double stop_max = demand > 0.0 ? demand : 0.0;

            sg_seg_concat_capacity(
                pd[cur + d], pm[cur + d], px[cur + d],
                demand, stop_min, stop_max,
                &pd[nxt + d], &pm[nxt + d], &px[nxt + d]);
        }
    }

    /* Initialize suffix[stop_len] = empty segment */
    {
        size_t end = (size_t)stop_len * dim_count;
        for (d = 0; d < dim_count; d++) {
            sd[end + d] = 0.0;
            smn[end + d] = 0.0;
            sx[end + d] = 0.0;
        }
    }

    /* Backward pass: build suffix arrays.
     * When stops[i].trip_start, stop idx (= i-1) is the last in its trip.
     * Use {0,0,0} as the right operand instead of overwriting suffix[i],
     * which already holds the correct summary for the NEXT trip. */
    for (i = stop_len; i > 0; i--) {
        uint32_t idx = i - 1;
        const SGTaskRecord *task = &ctx->tasks[stops[idx].task_id];
        size_t cur = (size_t)idx * dim_count;
        size_t right = (size_t)i * dim_count;
        uint8_t at_trip_end = (i < stop_len && stops[i].trip_start);

        for (d = 0; d < dim_count; d++) {
            double demand = (task->has_demand && task->demand)
                ? task->demand[d] : 0.0;
            double stop_min = demand < 0.0 ? demand : 0.0;
            double stop_max = demand > 0.0 ? demand : 0.0;

            double r_d   = at_trip_end ? 0.0 : sd[right + d];
            double r_min = at_trip_end ? 0.0 : smn[right + d];
            double r_max = at_trip_end ? 0.0 : sx[right + d];

            sg_seg_concat_capacity(
                demand, stop_min, stop_max,
                r_d, r_min, r_max,
                &sd[cur + d], &smn[cur + d], &sx[cur + d]);
        }
    }
}

/*
 * O(1) capacity feasibility check for insertion at stop-level position
 * insert_stop_pos.  new_stops[] contains the 1 or 2 stops being inserted.
 *
 * Returns 1 if capacity is feasible, 0 if violated.
 * When pen_enabled, accumulates violation magnitude in *violation_out.
 */
int sg_route_check_capacity_concat(const SGContext *ctx, const SGRouteSolution *sol,
                                    uint32_t vehicle_id, uint32_t insert_stop_pos,
                                    const SGRouteStop *new_stops, uint32_t new_stop_count,
                                    uint8_t pen_enabled, double *violation_out)
{
    const SGVehicleRecord *vehicle;
    uint32_t dim_count;
    size_t seg_stride, base;
    const double *pd, *pm, *px;
    const double *sd, *smn, *sx;
    uint32_t d, ns;
    int feasible = 1;

    if (!ctx || !sol || !violation_out) return 0;

    vehicle = &ctx->vehicles[vehicle_id];
    dim_count = ctx->dimension_count;
    if (dim_count == 0) return 1;
    if (!sol->route_seg_cap_prefix_delta) return 0;

    seg_stride = (size_t)(sol->stop_stride + 1U) * (size_t)dim_count;
    base = (size_t)vehicle_id * seg_stride;

    pd = sol->route_seg_cap_prefix_delta + base;
    pm = sol->route_seg_cap_prefix_min   + base;
    px = sol->route_seg_cap_prefix_max   + base;
    sd = sol->route_seg_cap_suffix_delta + base;
    smn = sol->route_seg_cap_suffix_min  + base;
    sx = sol->route_seg_cap_suffix_max   + base;

    *violation_out = 0.0;

    /* Determine trip boundaries for this insertion.
       For multi-trip vehicles, the prefix/suffix arrays reset at trip
       boundaries, so the values at insert_stop_pos already reflect only
       the current trip.  We just need to detect which trip we're in
       for the initial_load check (first trip only). */
    {
        const SGRouteStop *stops = sg_route_vehicle_stop_ptr_const(sol, vehicle_id);
        uint32_t stop_len = sol->route_stop_lengths[vehicle_id];
        uint8_t is_first_trip = 1;

        if (vehicle->has_multi_trip && stop_len > 0) {
            uint32_t s;
            for (s = 1; s <= insert_stop_pos && s < stop_len; s++) {
                if (stops[s].trip_start) {
                    is_first_trip = 0;
                    break;
                }
            }
        }

        for (d = 0; d < dim_count; d++) {
            double cap = (vehicle->has_capacity && vehicle->capacity)
                         ? vehicle->capacity[d] : INFINITY;
            size_t pos_off = (size_t)insert_stop_pos * dim_count + d;
            double comb_delta, comb_min, comb_max;
            double excess = 0.0;

            if (!isfinite(cap)) continue;

            /* Build new-stop segment by concatenating individual stops */
            {
                double new_delta = 0.0, new_min = 0.0, new_max = 0.0;
                for (ns = 0; ns < new_stop_count; ns++) {
                    const SGTaskRecord *task = &ctx->tasks[new_stops[ns].task_id];
                    double demand = (task->has_demand && task->demand)
                        ? task->demand[d] : 0.0;
                    double s_min = demand < 0.0 ? demand : 0.0;
                    double s_max = demand > 0.0 ? demand : 0.0;
                    double tmp_d, tmp_min, tmp_max;

                    sg_seg_concat_capacity(
                        new_delta, new_min, new_max,
                        demand, s_min, s_max,
                        &tmp_d, &tmp_min, &tmp_max);
                    new_delta = tmp_d;
                    new_min = tmp_min;
                    new_max = tmp_max;
                }

                /* Combine: prefix[insert_pos] + new_stops + suffix[insert_pos] */
                {
                    double left_plus_new_d, left_plus_new_min, left_plus_new_max;

                    sg_seg_concat_capacity(
                        pd[pos_off], pm[pos_off], px[pos_off],
                        new_delta, new_min, new_max,
                        &left_plus_new_d, &left_plus_new_min, &left_plus_new_max);

                    sg_seg_concat_capacity(
                        left_plus_new_d, left_plus_new_min, left_plus_new_max,
                        sd[pos_off], smn[pos_off], sx[pos_off],
                        &comb_delta, &comb_min, &comb_max);
                }
            }

            /* Check feasibility */
            if (is_first_trip &&
                vehicle->has_initial_load && vehicle->initial_load) {
                double il = vehicle->initial_load[d];
                if (il + comb_min < -SG_DEMAND_TOLERANCE) {
                    excess += -(il + comb_min);
                }
                if (il + comb_max > cap + SG_DEMAND_TOLERANCE) {
                    excess += (il + comb_max) - cap;
                }
            } else if ((comb_max - comb_min) > cap + SG_DEMAND_TOLERANCE) {
                excess += (comb_max - comb_min) - cap;
            }

            if (excess > 0.0) {
                *violation_out += excess;
                feasible = 0;
                if (!pen_enabled) return 0;
            }
        }
    }

    return feasible;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Phase 2: Timing segment concatenation (Vidal 2012)
 * ────────────────────────────────────────────────────────────────────────── */

/* Initialize a single-stop timing segment from task/stop data. */
void sg_seg_init_single(const SGContext *ctx, const SGRouteStop *stop,
                        SGSegSummary *seg)
{
    const SGTaskRecord *task;
    if (!ctx || !stop || !seg) return;

    task = &ctx->tasks[stop->task_id];
    memset(seg, 0, sizeof(*seg));

    seg->earliest_start = task->has_time_window ? (double)task->tw_early : 0.0;
    seg->latest_start   = task->has_time_window ? (double)task->tw_late  : INFINITY;
    seg->duration       = (double)task->service_seconds;
    seg->time_warp      = 0.0;
    seg->wait_time      = 0.0;
    seg->distance       = 0.0;

    seg->first_location_id = task->location_id;
    seg->last_location_id  = task->location_id;
    seg->first_request_id  = stop->request_id;
    seg->last_request_id   = stop->request_id;
    seg->stop_count        = 1;
}

/*
 * O(1) timing concatenation of two segments with known link costs.
 *
 * Concat(σ₁, σ₂) with link travel/dist/setup:
 *   arr  = σ₁.E + σ₁.D + link_travel + link_setup
 *   Δwt  = max(0, σ₂.E - arr)
 *   Δtw  = max(0, arr - σ₂.L)
 *
 *   out.E    = σ₁.E
 *   out.L    = min(σ₁.L, σ₂.L - σ₁.D - link_travel - link_setup)
 *   out.D    = σ₁.D + link_travel + link_setup + Δwt + σ₂.D
 *   out.TW   = σ₁.TW + Δtw + σ₂.TW
 *   out.WT   = σ₁.WT + Δwt + σ₂.WT
 *   out.Dist = σ₁.Dist + link_dist + σ₂.Dist
 *
 * L is conservative (ignores wait absorption) so time warp may be
 * slightly over-estimated.  Acceptable for Phase 3 pre-filtering.
 */
void sg_seg_concat_timing(const SGSegSummary *left, const SGSegSummary *right,
                          double link_travel_time, double link_travel_dist,
                          double link_setup_time,
                          SGSegSummary *out)
{
    double link = link_travel_time + link_setup_time;
    double arr  = left->earliest_start + left->duration + link;
    double dwt  = (right->earliest_start > arr) ? (right->earliest_start - arr) : 0.0;
    double dtw  = (arr > right->latest_start) ? (arr - right->latest_start) : 0.0;
    double right_L_shifted = right->latest_start - left->duration - link;

    out->earliest_start = left->earliest_start;
    out->latest_start   = (left->latest_start < right_L_shifted)
                          ? left->latest_start : right_L_shifted;
    out->duration       = left->duration + link + dwt + right->duration;
    out->time_warp      = left->time_warp + dtw + right->time_warp;
    out->wait_time      = left->wait_time + dwt + right->wait_time;
    out->distance       = left->distance + link_travel_dist + right->distance;

    out->first_location_id = left->first_location_id;
    out->last_location_id  = right->last_location_id;
    out->first_request_id  = left->first_request_id;
    out->last_request_id   = right->last_request_id;
    out->stop_count        = left->stop_count + right->stop_count;
}

/*
 * Build timing prefix/suffix SGSegSummary arrays for one vehicle.
 * Skipped for vehicles with break policies or multi-trip (identity values).
 *
 * Layout: [vehicle * (stop_stride+1) + stop]
 *
 * prefix[0] = start depot segment.
 * prefix[i+1] = concat(prefix[i], single_stop[i])  for i in [0..stop_len-1]
 * suffix[stop_len] = end depot segment.
 * suffix[i] = concat(single_stop[i], suffix[i+1])   for i in [stop_len-1..0]
 */
static void sg_route_build_timing_segments(const SGContext *ctx,
                                            SGRouteSolution *sol,
                                            uint32_t vehicle_id)
{
    const SGVehicleRecord *vehicle;
    const SGDepotRecord *start_depot;
    const SGDepotRecord *end_depot;
    const SGRouteStop *stops;
    uint32_t stop_len;
    size_t seg_stride, base;
    SGSegSummary *prefix, *suffix;
    uint32_t i;

    if (!ctx || !sol || vehicle_id >= sol->num_vehicles) return;
    if (!sol->route_seg_prefix || !sol->route_seg_suffix) return;

    vehicle = &ctx->vehicles[vehicle_id];
    stop_len = sol->route_stop_lengths[vehicle_id];
    seg_stride = (size_t)sol->stop_stride + 1U;
    base = (size_t)vehicle_id * seg_stride;

    prefix = sol->route_seg_prefix + base;
    suffix = sol->route_seg_suffix + base;

    /* Skip vehicles with break policies or multi-trip:
       leave arrays as zero/identity for Phase 3 fallback detection. */
    if (vehicle->has_break_policy || vehicle->has_multi_trip) {
        for (i = 0; i <= stop_len && i < (uint32_t)seg_stride; i++) {
            memset(&prefix[i], 0, sizeof(SGSegSummary));
            prefix[i].latest_start = INFINITY;
            memset(&suffix[i], 0, sizeof(SGSegSummary));
            suffix[i].latest_start = INFINITY;
        }
        return;
    }

    if (!vehicle->has_depots || vehicle->start_depot_id >= ctx->num_depots ||
        vehicle->end_depot_id >= ctx->num_depots) {
        return;
    }

    start_depot = &ctx->depots[vehicle->start_depot_id];
    end_depot   = &ctx->depots[vehicle->end_depot_id];
    stops = sg_route_vehicle_stop_ptr_const(sol, vehicle_id);

    /* prefix[0] = start depot segment */
    {
        SGSegSummary *p0 = &prefix[0];
        memset(p0, 0, sizeof(*p0));
        p0->earliest_start = vehicle->has_shift_time_window
                             ? (double)vehicle->shift_early : 0.0;
        if (!vehicle->open_start && start_depot->has_time_window &&
            p0->earliest_start < (double)start_depot->tw_early) {
            p0->earliest_start = (double)start_depot->tw_early;
        }
        p0->latest_start = vehicle->has_shift_time_window
                           ? (double)vehicle->shift_late : INFINITY;
        p0->duration       = 0.0;
        p0->time_warp      = 0.0;
        p0->wait_time      = 0.0;
        p0->distance       = 0.0;
        p0->first_location_id = vehicle->start_location_id;
        p0->last_location_id  = vehicle->start_location_id;
        p0->first_request_id  = UINT32_MAX;
        p0->last_request_id   = UINT32_MAX;
        p0->stop_count        = 0;
    }

    /* Forward pass: prefix[i+1] = concat(prefix[i], single_stop[i]) */
    for (i = 0; i < stop_len; i++) {
        SGSegSummary single;
        double link_dur, link_dist, link_setup;

        sg_seg_init_single(ctx, &stops[i], &single);

        /* Travel from previous segment's last location to this stop */
        link_dist = sg_travel_dist(ctx, prefix[i].last_location_id,
                                   single.first_location_id, vehicle_id);
        link_dur  = sg_travel_dur(ctx, prefix[i].last_location_id,
                                  single.first_location_id, vehicle_id,
                                  prefix[i].earliest_start + prefix[i].duration);

        /* Setup time between previous and current request */
        link_setup = sg_setup_time_between(ctx,
                         prefix[i].last_request_id, stops[i].request_id);

        /* Open-start: skip first leg from depot */
        if (vehicle->open_start && i == 0) {
            link_dur  = 0.0;
            link_dist = 0.0;
        }

        sg_seg_concat_timing(&prefix[i], &single,
                             link_dur, link_dist, link_setup,
                             &prefix[i + 1]);
    }

    /* suffix[stop_len] = end depot segment */
    {
        SGSegSummary *sn = &suffix[stop_len];
        memset(sn, 0, sizeof(*sn));
        if (vehicle->open_end) {
            sn->earliest_start = 0.0;
            sn->latest_start   = vehicle->has_shift_time_window
                                 ? (double)vehicle->shift_late : INFINITY;
        } else {
            sn->earliest_start = end_depot->has_time_window
                                 ? (double)end_depot->tw_early : 0.0;
            sn->latest_start   = end_depot->has_time_window
                                 ? (double)end_depot->tw_late : INFINITY;
            /* Also constrain by shift end and max duration */
            if (vehicle->has_shift_time_window &&
                (double)vehicle->shift_late < sn->latest_start) {
                sn->latest_start = (double)vehicle->shift_late;
            }
        }
        if (vehicle->max_duration_seconds > 0) {
            double depot_depart = prefix[0].earliest_start;
            double max_end = depot_depart + (double)vehicle->max_duration_seconds;
            if (max_end < sn->latest_start)
                sn->latest_start = max_end;
        }
        sn->duration       = 0.0;
        sn->time_warp      = 0.0;
        sn->wait_time      = 0.0;
        sn->distance       = 0.0;
        sn->first_location_id = vehicle->end_location_id;
        sn->last_location_id  = vehicle->end_location_id;
        sn->first_request_id  = UINT32_MAX;
        sn->last_request_id   = UINT32_MAX;
        sn->stop_count        = 0;
    }

    /* Backward pass: suffix[i] = concat(single_stop[i], suffix[i+1]) */
    for (i = stop_len; i > 0; i--) {
        uint32_t idx = i - 1;
        SGSegSummary single;
        double link_dur, link_dist, link_setup;

        sg_seg_init_single(ctx, &stops[idx], &single);

        /* Travel from this stop to next segment's first location */
        link_dist = sg_travel_dist(ctx, single.last_location_id,
                                   suffix[i].first_location_id, vehicle_id);
        link_dur  = sg_travel_dur(ctx, single.last_location_id,
                                  suffix[i].first_location_id, vehicle_id,
                                  single.earliest_start + single.duration);

        /* Setup time between this stop and the next */
        link_setup = sg_setup_time_between(ctx,
                         stops[idx].request_id, suffix[i].first_request_id);

        /* Open-end: skip last leg to depot */
        if (vehicle->open_end && i == stop_len) {
            link_dur  = 0.0;
            link_dist = 0.0;
        }

        sg_seg_concat_timing(&single, &suffix[i],
                             link_dur, link_dist, link_setup,
                             &suffix[idx]);
    }
}

/*
 * Build all segment arrays (capacity + timing) for one vehicle.
 * Called at the end of sg_route_update_timing().
 */
void sg_route_build_segments(const SGContext *ctx, SGRouteSolution *sol,
                              uint32_t vehicle_id)
{
    sg_route_build_cap_segments(ctx, sol, vehicle_id);
    sg_route_build_timing_segments(ctx, sol, vehicle_id);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Phase 3: O(1) pre-filtering for local search operators
 * ────────────────────────────────────────────────────────────────────────── */

/*
 * Returns 1 if the concat pre-filter is applicable for the given vehicle.
 * Requirements: delivery-only, valid timing segments, no travel callback,
 * no global time brackets (so sg_travel_dist == sg_travel for distances).
 */
static int sg_concat_filter_applicable(const SGContext *ctx, uint32_t v) {
    if (ctx->has_pd_requests) return 0;
    if (!sg_vehicle_has_timing_segments(ctx, v)) return 0;
    if (ctx->travel_callback) return 0;
    if (ctx->has_travel_time_brackets) return 0;
    return 1;
}

/*
 * Build SGSegSummary for a short stop sequence using a specific vehicle's
 * travel profile.  Used to rebuild a moved segment for the target vehicle
 * in OR-opt and cross-exchange.  O(stop_count), but stop_count <= 6.
 */
void sg_build_segment_for_vehicle(const SGContext *ctx, const SGRouteStop *stops,
                                   uint32_t stop_count, uint32_t vehicle_id,
                                   SGSegSummary *out)
{
    uint32_t i;

    if (!ctx || !stops || stop_count == 0 || !out) {
        if (out) memset(out, 0, sizeof(*out));
        return;
    }

    sg_seg_init_single(ctx, &stops[0], out);

    for (i = 1; i < stop_count; i++) {
        SGSegSummary single, combined;
        double link_dist, link_dur, link_setup;

        sg_seg_init_single(ctx, &stops[i], &single);

        link_dist = sg_travel_dist(ctx, out->last_location_id,
                                   single.first_location_id, vehicle_id);
        link_dur  = sg_travel_dur(ctx, out->last_location_id,
                                  single.first_location_id, vehicle_id,
                                  out->earliest_start + out->duration);
        link_setup = sg_setup_time_between(ctx,
                         stops[i - 1].request_id, stops[i].request_id);

        sg_seg_concat_timing(out, &single, link_dur, link_dist, link_setup,
                             &combined);
        *out = combined;
    }
}

/*
 * O(1) capacity check by concatenating prefix/suffix capacity segments.
 * Returns 1 if capacity is feasible for the concatenated route, 0 otherwise.
 * source_stop_start..source_stop_end-1 are removed from the route.
 * new_seg_delta/min/max describe the replacement segment (empty = {0,0,0}).
 */
static int sg_concat_capacity_ok(const SGContext *ctx, const SGRouteSolution *sol,
                                  uint32_t vehicle_id,
                                  uint32_t prefix_stop, uint32_t suffix_stop,
                                  const double *seg_delta, const double *seg_min,
                                  const double *seg_max)
{
    const SGVehicleRecord *vehicle;
    uint32_t dim_count, d;
    size_t seg_stride, base;
    const double *pd, *pm, *px;
    const double *sd, *smn, *sx;

    dim_count = ctx->dimension_count;
    if (dim_count == 0) return 1;
    if (!sol->route_seg_cap_prefix_delta) return 1;

    vehicle = &ctx->vehicles[vehicle_id];
    seg_stride = (size_t)(sol->stop_stride + 1U) * (size_t)dim_count;
    base = (size_t)vehicle_id * seg_stride;

    pd = sol->route_seg_cap_prefix_delta + base;
    pm = sol->route_seg_cap_prefix_min   + base;
    px = sol->route_seg_cap_prefix_max   + base;
    sd = sol->route_seg_cap_suffix_delta + base;
    smn = sol->route_seg_cap_suffix_min  + base;
    sx = sol->route_seg_cap_suffix_max   + base;

    for (d = 0; d < dim_count; d++) {
        double cap = (vehicle->has_capacity && vehicle->capacity)
                     ? vehicle->capacity[d] : INFINITY;
        size_t pre_off = (size_t)prefix_stop * dim_count + d;
        size_t suf_off = (size_t)suffix_stop * dim_count + d;
        double sd_v = seg_delta ? seg_delta[d] : 0.0;
        double sm_v = seg_min   ? seg_min[d]   : 0.0;
        double sx_v = seg_max   ? seg_max[d]   : 0.0;
        double left_d, left_min, left_max;
        double comb_d, comb_min, comb_max;

        if (!isfinite(cap)) continue;

        /* prefix + new_seg */
        sg_seg_concat_capacity(pd[pre_off], pm[pre_off], px[pre_off],
                               sd_v, sm_v, sx_v,
                               &left_d, &left_min, &left_max);
        /* (prefix + new_seg) + suffix */
        sg_seg_concat_capacity(left_d, left_min, left_max,
                               sd[suf_off], smn[suf_off], sx[suf_off],
                               &comb_d, &comb_min, &comb_max);

        if ((comb_max - comb_min) > cap + SG_DEMAND_TOLERANCE) {
            return 0;
        }
    }
    return 1;
}

/*
 * Build capacity segment (delta, min, max) for a short stop sequence.
 * Arrays must have dim_count elements each.
 */
static void sg_build_cap_segment(const SGContext *ctx, const SGRouteStop *stops,
                                  uint32_t stop_count,
                                  double *out_delta, double *out_min, double *out_max)
{
    uint32_t dim_count = ctx->dimension_count;
    uint32_t i, d;

    for (d = 0; d < dim_count; d++) {
        out_delta[d] = 0.0;
        out_min[d] = 0.0;
        out_max[d] = 0.0;
    }

    for (i = 0; i < stop_count; i++) {
        const SGTaskRecord *task = &ctx->tasks[stops[i].task_id];
        for (d = 0; d < dim_count; d++) {
            double demand = (task->has_demand && task->demand) ? task->demand[d] : 0.0;
            double s_min = demand < 0.0 ? demand : 0.0;
            double s_max = demand > 0.0 ? demand : 0.0;
            double tmp_d, tmp_min, tmp_max;

            sg_seg_concat_capacity(out_delta[d], out_min[d], out_max[d],
                                   demand, s_min, s_max,
                                   &tmp_d, &tmp_min, &tmp_max);
            out_delta[d] = tmp_d;
            out_min[d] = tmp_min;
            out_max[d] = tmp_max;
        }
    }
}

/*
 * OR-opt pre-filter.
 * Move: remove k requests [start..start+k-1] from va, insert at position ins in vb.
 * For delivery-only: request pos == stop pos.
 *
 * Returns 1 if evaluation was performed (check *new_total_out vs threshold).
 * Returns 0 if not applicable (caller should fall through to O(L) path).
 */
int sg_concat_eval_or_opt(const SGContext *ctx, const SGRouteSolution *sol,
                           uint32_t va, uint32_t start, uint32_t k,
                           uint32_t vb, uint32_t ins,
                           double *new_total_out)
{
    size_t seg_stride, base_a, base_b;
    const SGSegSummary *pf_a, *sf_a, *pf_b, *sf_b;
    SGSegSummary moved_seg, temp, new_dest;
    double link_dist, link_dur, link_setup;
    double source_dist, dest_dist;
    uint32_t stop_len_a;
    const SGRouteStop *stops_a, *stops_b;

    if (!new_total_out) return 0;
    if (!sg_concat_filter_applicable(ctx, va)) return 0;
    if (!sg_concat_filter_applicable(ctx, vb)) return 0;
    if (!sol->route_seg_prefix || !sol->route_seg_suffix) return 0;

    /* For delivery-only: request pos == stop pos, route_lengths == route_stop_lengths */
    stop_len_a = sol->route_stop_lengths[va];
    stops_a = sg_route_vehicle_stop_ptr_const(sol, va);

    seg_stride = (size_t)sol->stop_stride + 1U;
    base_a = (size_t)va * seg_stride;
    base_b = (size_t)vb * seg_stride;

    pf_a = sol->route_seg_prefix + base_a;
    sf_a = sol->route_seg_suffix + base_a;
    pf_b = sol->route_seg_prefix + base_b;
    sf_b = sol->route_seg_suffix + base_b;

    /* ── Source route: va without the segment ── */
    if (stop_len_a - k == 0) {
        source_dist = 0.0;
    } else {
        SGSegSummary new_source;
        /* Link from prefix's last location to suffix's first location */
        link_dist = sg_travel_dist(ctx, pf_a[start].last_location_id,
                                   sf_a[start + k].first_location_id, va);
        link_dur  = sg_travel_dur(ctx, pf_a[start].last_location_id,
                                  sf_a[start + k].first_location_id, va,
                                  pf_a[start].earliest_start + pf_a[start].duration);
        link_setup = sg_setup_time_between(ctx,
                         pf_a[start].last_request_id,
                         sf_a[start + k].first_request_id);

        sg_seg_concat_timing(&pf_a[start], &sf_a[start + k],
                             link_dur, link_dist, link_setup,
                             &new_source);
        source_dist = new_source.distance;
    }

    /* ── Build moved segment for vb's profile ── */
    sg_build_segment_for_vehicle(ctx, &stops_a[start], k, vb, &moved_seg);

    /* ── Destination route: vb with segment inserted at ins ── */
    if (va == vb) {
        /* Intra-route: use source route's prefix/suffix.
           After removing the segment, positions shift:
           prefix_src[ins'] and suffix_src[ins'] need to be computed from
           the source route.  This is complex for intra-route, so fall back. */
        return 0;
    }

    stops_b = sg_route_vehicle_stop_ptr_const(sol, vb);
    (void)stops_b;

    /* dest = prefix_B[ins] + moved_seg + suffix_B[ins] */
    link_dist = sg_travel_dist(ctx, pf_b[ins].last_location_id,
                               moved_seg.first_location_id, vb);
    link_dur  = sg_travel_dur(ctx, pf_b[ins].last_location_id,
                              moved_seg.first_location_id, vb,
                              pf_b[ins].earliest_start + pf_b[ins].duration);
    link_setup = sg_setup_time_between(ctx,
                     pf_b[ins].last_request_id,
                     moved_seg.first_request_id);
    sg_seg_concat_timing(&pf_b[ins], &moved_seg,
                         link_dur, link_dist, link_setup, &temp);

    link_dist = sg_travel_dist(ctx, temp.last_location_id,
                               sf_b[ins].first_location_id, vb);
    link_dur  = sg_travel_dur(ctx, temp.last_location_id,
                              sf_b[ins].first_location_id, vb,
                              temp.earliest_start + temp.duration);
    link_setup = sg_setup_time_between(ctx,
                     temp.last_request_id,
                     sf_b[ins].first_request_id);
    sg_seg_concat_timing(&temp, &sf_b[ins],
                         link_dur, link_dist, link_setup, &new_dest);
    dest_dist = new_dest.distance;

    /* ── Capacity check ── */
    if (ctx->dimension_count > 0) {
        /* Source: prefix_A[start] + suffix_A[start+k] */
        if (!sg_concat_capacity_ok(ctx, sol, va, start, start + k,
                                    NULL, NULL, NULL)) {
            *new_total_out = INFINITY;
            return 1;
        }
        /* Dest: prefix_B[ins] + moved_seg + suffix_B[ins] */
        {
            double seg_d[8], seg_mn[8], seg_mx[8];  /* max 8 dims */
            double *sd = seg_d, *sm = seg_mn, *sx = seg_mx;
            int heap = 0;

            if (ctx->dimension_count > 8) {
                sd = (double *)malloc((size_t)ctx->dimension_count * 3 * sizeof(double));
                if (!sd) return 0;
                sm = sd + ctx->dimension_count;
                sx = sm + ctx->dimension_count;
                heap = 1;
            }

            sg_build_cap_segment(ctx, &stops_a[start], k, sd, sm, sx);
            if (!sg_concat_capacity_ok(ctx, sol, vb, ins, ins, sd, sm, sx)) {
                if (heap) free(sd);
                *new_total_out = INFINITY;
                return 1;
            }
            if (heap) free(sd);
        }
    }

    *new_total_out = sol->total_distance - sol->route_distance[va]
                     - sol->route_distance[vb] + source_dist + dest_dist;
    return 1;
}

/*
 * 2-opt* pre-filter.
 * Move: New A = A[0..cut_a-1] + B[cut_b..end], New B = B[0..cut_b-1] + A[cut_a..end].
 *
 * Additional requirement: same travel profile (all vehicles use same distance matrix).
 * Also skip for open-start/open-end vehicles.
 *
 * Returns 1 if evaluation was performed (check *new_total_out vs threshold).
 * Returns 0 if not applicable (caller should fall through to O(L) path).
 */
int sg_concat_eval_2opt_star(const SGContext *ctx, const SGRouteSolution *sol,
                              uint32_t va, uint32_t cut_a,
                              uint32_t vb, uint32_t cut_b,
                              double *new_total_out)
{
    size_t seg_stride, base_a, base_b;
    const SGSegSummary *pf_a, *sf_a, *pf_b, *sf_b;
    const SGVehicleRecord *veh_a, *veh_b;
    double link_dist_ab, link_dur_ab, link_setup_ab;
    double link_dist_ba, link_dur_ba, link_setup_ba;
    double new_dist_a, new_dist_b;
    SGSegSummary new_route_a, new_route_b;
    uint32_t stop_len_a, stop_len_b;
    const SGRouteStop *stops_a, *stops_b;
    double depot_adj_a = 0.0, depot_adj_b = 0.0;

    if (!new_total_out) return 0;
    if (!sg_concat_filter_applicable(ctx, va)) return 0;
    if (!sg_concat_filter_applicable(ctx, vb)) return 0;
    if (!sol->route_seg_prefix || !sol->route_seg_suffix) return 0;

    /* 2-opt* requires same travel profile for cross-vehicle suffix usage */
    if (ctx->has_travel_profiles || ctx->travel_callback) return 0;

    veh_a = &ctx->vehicles[va];
    veh_b = &ctx->vehicles[vb];

    /* Skip open-start/open-end vehicles */
    if (veh_a->open_start || veh_a->open_end) return 0;
    if (veh_b->open_start || veh_b->open_end) return 0;

    stop_len_a = sol->route_stop_lengths[va];
    stop_len_b = sol->route_stop_lengths[vb];
    stops_a = sg_route_vehicle_stop_ptr_const(sol, va);
    stops_b = sg_route_vehicle_stop_ptr_const(sol, vb);

    seg_stride = (size_t)sol->stop_stride + 1U;
    base_a = (size_t)va * seg_stride;
    base_b = (size_t)vb * seg_stride;

    pf_a = sol->route_seg_prefix + base_a;
    sf_a = sol->route_seg_suffix + base_a;
    pf_b = sol->route_seg_prefix + base_b;
    sf_b = sol->route_seg_suffix + base_b;

    /* Depot return adjustment: suffix_B includes return to B's end depot.
       New route A ends at A's end depot, so adjust for the difference. */
    if (veh_a->end_location_id != veh_b->end_location_id) {
        /* Last customer stop of B suffix (last stop on route B) */
        uint32_t last_b_loc = ctx->tasks[stops_b[stop_len_b - 1].task_id].location_id;
        /* Last customer stop of A suffix (last stop on route A) */
        uint32_t last_a_loc = ctx->tasks[stops_a[stop_len_a - 1].task_id].location_id;

        depot_adj_a = sg_travel_dist(ctx, last_b_loc, veh_a->end_location_id, va)
                    - sg_travel_dist(ctx, last_b_loc, veh_b->end_location_id, vb);
        depot_adj_b = sg_travel_dist(ctx, last_a_loc, veh_b->end_location_id, vb)
                    - sg_travel_dist(ctx, last_a_loc, veh_a->end_location_id, va);
    }

    /* ── New route A: prefix_A[cut_a] + link + suffix_B[cut_b] + depot_adj ── */
    link_dist_ab = sg_travel_dist(ctx, pf_a[cut_a].last_location_id,
                                  sf_b[cut_b].first_location_id, va);
    link_dur_ab  = sg_travel_dur(ctx, pf_a[cut_a].last_location_id,
                                 sf_b[cut_b].first_location_id, va,
                                 pf_a[cut_a].earliest_start + pf_a[cut_a].duration);
    link_setup_ab = sg_setup_time_between(ctx,
                        pf_a[cut_a].last_request_id,
                        sf_b[cut_b].first_request_id);
    sg_seg_concat_timing(&pf_a[cut_a], &sf_b[cut_b],
                         link_dur_ab, link_dist_ab, link_setup_ab,
                         &new_route_a);
    new_dist_a = new_route_a.distance + depot_adj_a;

    /* ── New route B: prefix_B[cut_b] + link + suffix_A[cut_a] + depot_adj ── */
    link_dist_ba = sg_travel_dist(ctx, pf_b[cut_b].last_location_id,
                                  sf_a[cut_a].first_location_id, vb);
    link_dur_ba  = sg_travel_dur(ctx, pf_b[cut_b].last_location_id,
                                 sf_a[cut_a].first_location_id, vb,
                                 pf_b[cut_b].earliest_start + pf_b[cut_b].duration);
    link_setup_ba = sg_setup_time_between(ctx,
                        pf_b[cut_b].last_request_id,
                        sf_a[cut_a].first_request_id);
    sg_seg_concat_timing(&pf_b[cut_b], &sf_a[cut_a],
                         link_dur_ba, link_dist_ba, link_setup_ba,
                         &new_route_b);
    new_dist_b = new_route_b.distance + depot_adj_b;

    /* ── Capacity check ── */
    if (ctx->dimension_count > 0) {
        /* New A: prefix_A[cut_a] + suffix_B[cut_b] */
        {
            size_t dim = ctx->dimension_count;
            size_t cap_stride = (size_t)(sol->stop_stride + 1U) * dim;
            const double *sd_b = sol->route_seg_cap_suffix_delta + (size_t)vb * cap_stride;
            const double *sm_b = sol->route_seg_cap_suffix_min   + (size_t)vb * cap_stride;
            const double *sx_b = sol->route_seg_cap_suffix_max   + (size_t)vb * cap_stride;

            if (!sg_concat_capacity_ok(ctx, sol, va, cut_a, cut_a,
                    &sd_b[(size_t)cut_b * dim],
                    &sm_b[(size_t)cut_b * dim],
                    &sx_b[(size_t)cut_b * dim])) {
                *new_total_out = INFINITY;
                return 1;
            }
        }
        /* New B: prefix_B[cut_b] + suffix_A[cut_a] */
        {
            size_t dim = ctx->dimension_count;
            size_t cap_stride = (size_t)(sol->stop_stride + 1U) * dim;
            const double *sd_a = sol->route_seg_cap_suffix_delta + (size_t)va * cap_stride;
            const double *sm_a = sol->route_seg_cap_suffix_min   + (size_t)va * cap_stride;
            const double *sx_a = sol->route_seg_cap_suffix_max   + (size_t)va * cap_stride;

            if (!sg_concat_capacity_ok(ctx, sol, vb, cut_b, cut_b,
                    &sd_a[(size_t)cut_a * dim],
                    &sm_a[(size_t)cut_a * dim],
                    &sx_a[(size_t)cut_a * dim])) {
                *new_total_out = INFINITY;
                return 1;
            }
        }
    }

    *new_total_out = sol->total_distance - sol->route_distance[va]
                     - sol->route_distance[vb] + new_dist_a + new_dist_b;
    return 1;
}

/*
 * Cross-exchange pre-filter.
 * Move: swap segment [ia..ia+sa-1] from A with [ib..ib+sb-1] from B.
 * sa, sb in {1,2,3}.
 *
 * Returns 1 if evaluation was performed (check *new_total_out vs threshold).
 * Returns 0 if not applicable (caller should fall through to O(L) path).
 */
int sg_concat_eval_cross_exchange(const SGContext *ctx, const SGRouteSolution *sol,
                                   uint32_t va, uint32_t ia, uint32_t sa,
                                   uint32_t vb, uint32_t ib, uint32_t sb,
                                   double *new_total_out)
{
    size_t seg_stride, base_a, base_b;
    const SGSegSummary *pf_a, *sf_a, *pf_b, *sf_b;
    SGSegSummary seg_b_for_va, seg_a_for_vb;
    SGSegSummary temp_a, new_a, temp_b, new_b;
    double link_dist, link_dur, link_setup;
    const SGRouteStop *stops_a, *stops_b;

    if (!new_total_out) return 0;
    if (!sg_concat_filter_applicable(ctx, va)) return 0;
    if (!sg_concat_filter_applicable(ctx, vb)) return 0;
    if (!sol->route_seg_prefix || !sol->route_seg_suffix) return 0;

    stops_a = sg_route_vehicle_stop_ptr_const(sol, va);
    stops_b = sg_route_vehicle_stop_ptr_const(sol, vb);

    seg_stride = (size_t)sol->stop_stride + 1U;
    base_a = (size_t)va * seg_stride;
    base_b = (size_t)vb * seg_stride;

    pf_a = sol->route_seg_prefix + base_a;
    sf_a = sol->route_seg_suffix + base_a;
    pf_b = sol->route_seg_prefix + base_b;
    sf_b = sol->route_seg_suffix + base_b;

    /* Rebuild moved segments for target vehicles */
    sg_build_segment_for_vehicle(ctx, &stops_b[ib], sb, va, &seg_b_for_va);
    sg_build_segment_for_vehicle(ctx, &stops_a[ia], sa, vb, &seg_a_for_vb);

    /* ── New route A: prefix_A[ia] + seg_b_for_va + suffix_A[ia+sa] ── */
    link_dist = sg_travel_dist(ctx, pf_a[ia].last_location_id,
                               seg_b_for_va.first_location_id, va);
    link_dur  = sg_travel_dur(ctx, pf_a[ia].last_location_id,
                              seg_b_for_va.first_location_id, va,
                              pf_a[ia].earliest_start + pf_a[ia].duration);
    link_setup = sg_setup_time_between(ctx,
                     pf_a[ia].last_request_id,
                     seg_b_for_va.first_request_id);
    sg_seg_concat_timing(&pf_a[ia], &seg_b_for_va,
                         link_dur, link_dist, link_setup, &temp_a);

    link_dist = sg_travel_dist(ctx, temp_a.last_location_id,
                               sf_a[ia + sa].first_location_id, va);
    link_dur  = sg_travel_dur(ctx, temp_a.last_location_id,
                              sf_a[ia + sa].first_location_id, va,
                              temp_a.earliest_start + temp_a.duration);
    link_setup = sg_setup_time_between(ctx,
                     temp_a.last_request_id,
                     sf_a[ia + sa].first_request_id);
    sg_seg_concat_timing(&temp_a, &sf_a[ia + sa],
                         link_dur, link_dist, link_setup, &new_a);

    /* ── New route B: prefix_B[ib] + seg_a_for_vb + suffix_B[ib+sb] ── */
    link_dist = sg_travel_dist(ctx, pf_b[ib].last_location_id,
                               seg_a_for_vb.first_location_id, vb);
    link_dur  = sg_travel_dur(ctx, pf_b[ib].last_location_id,
                              seg_a_for_vb.first_location_id, vb,
                              pf_b[ib].earliest_start + pf_b[ib].duration);
    link_setup = sg_setup_time_between(ctx,
                     pf_b[ib].last_request_id,
                     seg_a_for_vb.first_request_id);
    sg_seg_concat_timing(&pf_b[ib], &seg_a_for_vb,
                         link_dur, link_dist, link_setup, &temp_b);

    link_dist = sg_travel_dist(ctx, temp_b.last_location_id,
                               sf_b[ib + sb].first_location_id, vb);
    link_dur  = sg_travel_dur(ctx, temp_b.last_location_id,
                              sf_b[ib + sb].first_location_id, vb,
                              temp_b.earliest_start + temp_b.duration);
    link_setup = sg_setup_time_between(ctx,
                     temp_b.last_request_id,
                     sf_b[ib + sb].first_request_id);
    sg_seg_concat_timing(&temp_b, &sf_b[ib + sb],
                         link_dur, link_dist, link_setup, &new_b);

    /* ── Capacity check ── */
    if (ctx->dimension_count > 0) {
        double seg_d[8], seg_mn[8], seg_mx[8];
        double *sd_buf = seg_d, *sm_buf = seg_mn, *sx_buf = seg_mx;
        int heap = 0;

        if (ctx->dimension_count > 8) {
            sd_buf = (double *)malloc((size_t)ctx->dimension_count * 3 * sizeof(double));
            if (!sd_buf) return 0;
            sm_buf = sd_buf + ctx->dimension_count;
            sx_buf = sm_buf + ctx->dimension_count;
            heap = 1;
        }

        /* New A: prefix_A[ia] + seg_b + suffix_A[ia+sa] */
        sg_build_cap_segment(ctx, &stops_b[ib], sb, sd_buf, sm_buf, sx_buf);
        if (!sg_concat_capacity_ok(ctx, sol, va, ia, ia + sa, sd_buf, sm_buf, sx_buf)) {
            if (heap) free(sd_buf);
            *new_total_out = INFINITY;
            return 1;
        }

        /* New B: prefix_B[ib] + seg_a + suffix_B[ib+sb] */
        sg_build_cap_segment(ctx, &stops_a[ia], sa, sd_buf, sm_buf, sx_buf);
        if (!sg_concat_capacity_ok(ctx, sol, vb, ib, ib + sb, sd_buf, sm_buf, sx_buf)) {
            if (heap) free(sd_buf);
            *new_total_out = INFINITY;
            return 1;
        }

        if (heap) free(sd_buf);
    }

    *new_total_out = sol->total_distance - sol->route_distance[va]
                     - sol->route_distance[vb] + new_a.distance + new_b.distance;
    return 1;
}
