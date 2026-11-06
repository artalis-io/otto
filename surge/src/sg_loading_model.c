#include "sg_internal.h"

/* Exact-key one-entry cache per worker. No hashes can certify a different load.
 * A small cache deliberately bounds memory independently of solve duration. */
struct SGLoadingCache {
    SGLoadingVehicle vehicle;
    uint64_t budget;
    uint32_t count;
    SGLoadingUnit units[SG_LOADING_MAX_UNITS];
    SGLoadingResult result;
};

void sg_loading_invalidate(SGContext *ctx) {
    free(ctx->loading_cache);
    ctx->loading_cache = NULL;
    ctx->num_violations = 0;
    if (ctx->final_solution) {
        sg_route_solution_free(ctx->final_solution, NULL);
        ctx->final_solution = NULL;
        memset(&ctx->stats, 0, sizeof(ctx->stats));
    }
}

SGStatus sg_set_hard_loading(SGContext *ctx, int enabled, uint64_t budget) {
    if (!ctx || (enabled != 0 && enabled != 1) || budget > 100000)
        return SG_STATUS_INVALID_ARG;
    sg_loading_invalidate(ctx);
    ctx->hard_loading = (uint8_t)enabled;
    ctx->loading_node_budget = budget;
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_loading(SGContext *ctx, uint32_t id, const SGLoadingVehicle *v) {
    SGLoadingResult r;
    if (!ctx || id >= ctx->num_vehicles || !v ||
        sg_loading_check(v, NULL, 0, 1, &r) != SG_STATUS_OK)
        return SG_STATUS_INVALID_ARG;
    SGLoadingVehicle *copy = malloc(sizeof(*copy));
    if (!copy)
        return SG_STATUS_OUT_OF_MEMORY;
    *copy = *v;
    sg_loading_invalidate(ctx);
    free(ctx->vehicles[id].loading);
    ctx->vehicles[id].loading = copy;
    return SG_STATUS_OK;
}

SGStatus sg_request_set_loading(SGContext *ctx, uint32_t id, const SGLoadingUnit *u, uint32_t n) {
    if (!ctx || id >= ctx->num_requests || n > SG_LOADING_MAX_UNITS || (n && !u))
        return SG_STATUS_INVALID_ARG;
    for (uint32_t i = 0; i < n; ++i)
        if (!u[i].height_mm || u[i].stack_class >= SG_LOADING_MAX_CLASSES)
            return SG_STATUS_INVALID_ARG;
    SGLoadingUnit *copy = n ? calloc(n, sizeof(*copy)) : NULL;
    if (n && !copy)
        return SG_STATUS_OUT_OF_MEMORY;
    for (uint32_t i = 0; i < n; ++i) {
        copy[i].height_mm = u[i].height_mm;
        copy[i].weight_g = u[i].weight_g;
        copy[i].max_weight_above_g = u[i].max_weight_above_g;
        copy[i].stack_class = u[i].stack_class;
        copy[i].max_units_above = u[i].max_units_above;
        /* Stop sequence is derived from the actual round, never caller-supplied. */
    }
    sg_loading_invalidate(ctx);
    free(ctx->requests[id].loading_units);
    ctx->requests[id].loading_units = copy;
    ctx->requests[id].loading_count = n;
    ctx->requests[id].has_loading_units = 1;
    return SG_STATUS_OK;
}

SGStatus sg_loading_model_validate(SGContext *ctx) {
    if (!ctx->hard_loading)
        return SG_STATUS_OK;
    for (uint32_t v = 0; v < ctx->num_vehicles; ++v) {
        if (!ctx->vehicles[v].loading || ctx->vehicles[v].has_initial_load ||
            ctx->vehicles[v].open_start) {
            sg_set_error(
                ctx, "hard loading: vehicle %u needs geometry and a closed, empty depot start", v);
            return SG_STATUS_INVALID_ARG;
        }
    }
    for (uint32_t r = 0; r < ctx->num_requests; ++r) {
        if (!ctx->requests[r].has_loading_units ||
            ctx->requests[r].kind != SG_REQUEST_KIND_DELIVERY_ONLY) {
            sg_set_error(ctx, "hard loading: request %u needs explicit delivery-only loading units",
                         r);
            return SG_STATUS_INVALID_ARG;
        }
        for (uint32_t i = 0; i < ctx->requests[r].loading_count; ++i)
            for (uint32_t v = 0; v < ctx->num_vehicles; ++v)
                if (ctx->requests[r].loading_units[i].stack_class >=
                    ctx->vehicles[v].loading->class_count) {
                    sg_set_error(ctx, "hard loading: request %u has an undeclared stack class", r);
                    return SG_STATUS_INVALID_ARG;
                }
    }
    return SG_STATUS_OK;
}

static int loading_should_stop(void *data) {
    const SGContext *ctx = data;
    return atomic_load(&ctx->cancel_requested) ||
           (ctx->loading_parent_cancel && atomic_load(ctx->loading_parent_cancel));
}

SGStatus sg_loading_round(const SGContext *ctx, uint32_t vehicle, const SGRouteStop *stops,
                          uint32_t count, uint32_t trip, SGLoadingResult *result,
                          uint32_t *requests, uint32_t *indices) {
    if (!result)
        return SG_STATUS_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    result->outcome = SG_LOADING_UNKNOWN;
    result->reason = SG_LOADING_UNSUPPORTED;
    if (!ctx || !ctx->hard_loading || vehicle >= ctx->num_vehicles ||
        !ctx->vehicles[vehicle].loading || (count && !stops))
        return SG_STATUS_INVALID_ARG;
    SGLoadingUnit units[SG_LOADING_MAX_UNITS];
    memset(units, 0, sizeof(units));
    uint32_t current = 0, n = 0, ordinal = 0;
    int found = count == 0 && trip == 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (i && stops[i].trip_start) {
            ++current;
            ordinal = 0;
        }
        if (current != trip)
            continue;
        found = 1;
        uint32_t r = stops[i].request_id;
        if (r >= ctx->num_requests || !ctx->requests[r].has_loading_units ||
            ctx->requests[r].kind != SG_REQUEST_KIND_DELIVERY_ONLY || stops[i].is_pickup)
            return SG_STATUS_INVALID_ARG;
        const SGRequestRecord *req = &ctx->requests[r];
        if (req->loading_count > SG_LOADING_MAX_UNITS - n)
            return SG_STATUS_OK; /* UNKNOWN */
        for (uint32_t j = 0; j < req->loading_count; ++j) {
            units[n] = req->loading_units[j];
            units[n].stop = ordinal;
            if (requests)
                requests[n] = r;
            if (indices)
                indices[n] = j;
            ++n;
        }
        ++ordinal;
    }
    if (!found)
        return SG_STATUS_INVALID_ARG;
    const SGLoadingVehicle *v = ctx->vehicles[vehicle].loading;
    if (n == 0) {
        result->outcome = SG_LOADING_FEASIBLE;
        result->reason = SG_LOADING_WITNESS;
        return SG_STATUS_OK;
    }
    SGLoadingCache *cache = ctx->loading_cache;
    if (cache && cache->count == n && cache->budget == ctx->loading_node_budget &&
        memcmp(&cache->vehicle, v, sizeof(*v)) == 0 &&
        memcmp(cache->units, units, n * sizeof(*units)) == 0) {
        *result = cache->result;
        return SG_STATUS_OK;
    }
    SGStatus status = sg_loading_check_controlled(v, units, n, ctx->loading_node_budget,
                                                  loading_should_stop, (void *)ctx, result);
    if (status != SG_STATUS_OK || result->reason == SG_LOADING_INTERRUPTED)
        return status;
    if (!cache) {
        cache = calloc(1, sizeof(*cache));
        if (!cache)
            return SG_STATUS_OK; /* result verified; no cache required */
        ((SGContext *)ctx)->loading_cache = cache;
    }
    cache->vehicle = *v;
    cache->budget = ctx->loading_node_budget;
    cache->count = n;
    memcpy(cache->units, units, n * sizeof(*units));
    cache->result = *result;
    return SG_STATUS_OK;
}

int sg_loading_stops_ok(const SGContext *ctx, uint32_t vehicle, const SGRouteStop *stops,
                        uint32_t count) {
    if (!ctx->hard_loading)
        return 1;
    uint32_t trips = 1;
    for (uint32_t i = 1; i < count; ++i)
        if (stops[i].trip_start)
            ++trips;
    if (vehicle >= ctx->num_vehicles)
        return 0;
    const SGVehicleRecord *v = &ctx->vehicles[vehicle];
    if ((trips > 1 && !v->has_multi_trip) || (v->max_trips && trips > v->max_trips))
        return 0;
    for (uint32_t trip = 0; trip < trips; ++trip) {
        SGLoadingResult result;
        if (sg_loading_round(ctx, vehicle, stops, count, trip, &result, NULL, NULL) !=
                SG_STATUS_OK ||
            result.outcome != SG_LOADING_FEASIBLE)
            return 0;
    }
    return 1;
}

int sg_loading_insertion_ok(const SGContext *ctx, const SGRouteSolution *sol, uint32_t request,
                            uint32_t vehicle, uint32_t position, int new_trip) {
    if (!ctx->hard_loading)
        return 1;
    if (request >= ctx->num_requests || vehicle >= sol->num_vehicles ||
        ctx->requests[request].kind != SG_REQUEST_KIND_DELIVERY_ONLY ||
        position > sol->route_lengths[vehicle])
        return 0;
    uint32_t count = sol->route_stop_lengths[vehicle];
    /* Delivery-only hard mode: one stop per request. */
    if (count != sol->route_lengths[vehicle] || (size_t)count > SIZE_MAX / sizeof(SGRouteStop) - 1)
        return 0;
    SGRouteStop *candidate = malloc(((size_t)count + 1) * sizeof(*candidate));
    if (!candidate)
        return 0;
    const SGRouteStop *stops = sg_route_vehicle_stop_ptr_const(sol, vehicle);
    if (position)
        memcpy(candidate, stops, position * sizeof(*candidate));
    memset(&candidate[position], 0, sizeof(*candidate));
    candidate[position].request_id = request;
    candidate[position].task_id = ctx->requests[request].delivery_task_id;
    candidate[position].trip_start = new_trip ? 1 : 0;
    if (count > position)
        memcpy(candidate + position + 1, stops + position, (count - position) * sizeof(*candidate));
    int ok = sg_loading_stops_ok(ctx, vehicle, candidate, count + 1);
    free(candidate);
    return ok;
}

SGStatus sg_solution_get_loading(const SGContext *ctx, uint32_t route, uint32_t trip,
                                 SGLoadingResult *result, uint32_t *requests, uint32_t *units) {
    if (!result)
        return SG_STATUS_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    result->outcome = SG_LOADING_UNKNOWN;
    result->reason = SG_LOADING_UNSUPPORTED;
    if (!ctx || !ctx->final_solution || route >= sg_solution_get_route_count(ctx))
        return SG_STATUS_INVALID_ARG;
    uint32_t v = sg_solution_get_route_vehicle_id(ctx, route);
    return sg_loading_round(ctx, v, sg_route_vehicle_stop_ptr_const(ctx->final_solution, v),
                            ctx->final_solution->route_stop_lengths[v], trip, result, requests,
                            units);
}
