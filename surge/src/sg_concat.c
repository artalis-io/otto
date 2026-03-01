#include "sg_internal.h"

/*
 * Concatenation-based O(1) capacity feasibility checks.
 *
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
 * The prefix/suffix arrays are rebuilt O(L) in sg_route_build_cap_segments()
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
void sg_route_build_cap_segments(const SGContext *ctx, SGRouteSolution *sol,
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
