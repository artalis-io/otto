#include "sg_internal.h"
#include "sg_loading_v2_internal.h"
#ifdef SG_LOADING_PROFILE
#include "sh_pal.h"
#define PROFILE(ctx, field) (++((SGContext *)(ctx))->loading_profile.field)
#else
#define PROFILE(ctx, field) ((void)0)
#endif

/* Bounded worker-local cache. Hashes only choose an entry; exact geometry,
 * ordered units and budget comparisons alone authorize reuse. Keep the last
 * hit as L0 so uniform loads do not pay for hashing on every insertion. */
#define SG_LOADING_CACHE_ENTRIES 32
typedef struct {
    const SGLoadingVehicle *source_geometry; /* owned, immutable until setter invalidation */
    SGLoadingVehicle vehicle;
    uint64_t budget;
    uint32_t count;
    SGLoadingUnit units[SG_LOADING_MAX_UNITS];
    SGLoadingResult result;
} SGLoadingCacheEntry;
struct SGLoadingCache {
    uint32_t last;
    SGLoadingCacheEntry entries[SG_LOADING_CACHE_ENTRIES];
};

/* A candidate insertion is a view, not an allocated/copied route. Preserve the
 * exact trip_start behavior of materializing the inserted stop, including pos 0. */
typedef SGLoadingV2View SGLoadingView;

static const SGRouteStop *loading_stop(const SGLoadingView *view, uint32_t i) {
    if (i == view->insert_pos) return &view->inserted;
    return &view->stops[i - (i > view->insert_pos)];
}

static int loading_key_equal(const SGLoadingCacheEntry *entry,
    const SGLoadingVehicle *v, const SGLoadingUnit *units, uint32_t count, uint64_t budget) {
    return entry->count == count && entry->budget == budget &&
        (entry->source_geometry == v || memcmp(&entry->vehicle, v, sizeof(*v)) == 0) &&
        memcmp(entry->units, units, count * sizeof(*units)) == 0;
}

static uint32_t loading_bucket(const SGLoadingUnit *units, uint32_t count, uint64_t budget) {
    uint64_t hash = budget ^ count;
    for (uint32_t i = 0; i < count; ++i) {
        /* Hash named fields, never padding or pointers. A collision only evicts. */
        const SGLoadingUnit *u = &units[i];
        hash = (hash ^ u->stop) * UINT64_C(1099511628211);
        hash = (hash ^ u->height_mm) * UINT64_C(1099511628211);
        hash = (hash ^ u->weight_g) * UINT64_C(1099511628211);
        hash = (hash ^ u->max_weight_above_g) * UINT64_C(1099511628211);
        hash = (hash ^ u->stack_class) * UINT64_C(1099511628211);
        hash = (hash ^ u->max_units_above) * UINT64_C(1099511628211);
    }
    return (uint32_t)(hash ^ (hash >> 32)) % SG_LOADING_CACHE_ENTRIES;
}

void sg_loading_invalidate(SGContext *ctx) {
    free(ctx->loading_cache);
    free(ctx->loading_v2_cache);
    ctx->loading_v2_cache = NULL;
    sg_loading_v2_report_invalidate(ctx);
    ctx->loading_cache = NULL;
    ctx->num_violations = 0;
    if (ctx->final_solution) {
        sg_route_solution_free(ctx->final_solution, NULL);
        ctx->final_solution = NULL;
        memset(&ctx->stats, 0, sizeof(ctx->stats));
    }
}

SGStatus sg_set_hard_loading(SGContext *ctx, int enabled, uint64_t budget) {
    if (!ctx || ctx->loading_v2.schema_version || (enabled != 0 && enabled != 1) || budget > 100000)
        return SG_STATUS_INVALID_ARG;
    sg_loading_invalidate(ctx);
    ctx->hard_loading = (uint8_t)enabled;
    ctx->loading_node_budget = budget;
    return SG_STATUS_OK;
}

SGStatus sg_vehicle_set_loading(SGContext *ctx, uint32_t id, const SGLoadingVehicle *v) {
    SGLoadingResult r;
    if (!ctx || ctx->loading_v2.schema_version || id >= ctx->num_vehicles || !v ||
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
    if (!ctx || ctx->loading_v2.schema_version || id >= ctx->num_requests || n > SG_LOADING_MAX_UNITS || (n && !u))
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
    if (ctx->loading_v2.schema_version) return sg_loading_v2_model_validate(ctx);
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
    return sg_cancelled(ctx);
}

static SGStatus loading_round(const SGContext *ctx, uint32_t vehicle,
    const SGLoadingView *view, uint32_t trip, SGLoadingResult *result,
    uint32_t *requests, uint32_t *indices, int certificate) {
    if (ctx) PROFILE(ctx, round_calls);
    if (!result) return SG_STATUS_INVALID_ARG;
    if (ctx && ctx->loading_v2.schema_version)
        return sg_loading_v2_round_view(ctx, vehicle, view, trip, result, requests, indices, certificate, NULL);
    /* Boolean feasibility needs no placement buffer on an exact cache hit. */
    memset(result, 0, certificate ? sizeof(*result) : offsetof(SGLoadingResult, placements));
    result->outcome = SG_LOADING_UNKNOWN;
    result->reason = SG_LOADING_UNSUPPORTED;
    if (!ctx || !ctx->hard_loading || vehicle >= ctx->num_vehicles ||
        !ctx->vehicles[vehicle].loading || (view->count && !view->stops && !(view->count == 1 && view->insert_pos == 0)))
        return SG_STATUS_INVALID_ARG;
    SGLoadingUnit units[SG_LOADING_MAX_UNITS];
    uint32_t current = 0, n = 0, ordinal = 0;
    int found = view->count == 0 && trip == 0;
    for (uint32_t i = 0; i < view->count; ++i) {
        const SGRouteStop *stop = loading_stop(view, i);
        if (i && stop->trip_start) { ++current; ordinal = 0; }
        if (current != trip) continue;
        found = 1;
        uint32_t r = stop->request_id;
        if (r >= ctx->num_requests || !ctx->requests[r].has_loading_units ||
            ctx->requests[r].kind != SG_REQUEST_KIND_DELIVERY_ONLY || stop->is_pickup)
            return SG_STATUS_INVALID_ARG;
        const SGRequestRecord *req = &ctx->requests[r];
        if (req->loading_count > SG_LOADING_MAX_UNITS - n)
            return SG_STATUS_OK; /* UNKNOWN */
        for (uint32_t j = 0; j < req->loading_count; ++j) {
            units[n] = req->loading_units[j];
            units[n].stop = ordinal;
            if (requests) requests[n] = r;
            if (indices) indices[n] = j;
            ++n;
        }
        ++ordinal;
    }
    if (!found) return SG_STATUS_INVALID_ARG;
    if (n == 0) {
        result->outcome = SG_LOADING_FEASIBLE;
        result->reason = SG_LOADING_WITNESS;
        return SG_STATUS_OK;
    }
    if (sg_cancelled(ctx)) {
        result->reason = SG_LOADING_INTERRUPTED;
        return SG_STATUS_OK;
    }
    const SGLoadingVehicle *v = ctx->vehicles[vehicle].loading;
    SGLoadingCache *cache = ctx->loading_cache;
    SGLoadingCacheEntry *entry = cache ? &cache->entries[cache->last] : NULL;
    uint32_t bucket = 0;
    int hit = entry && loading_key_equal(entry, v, units, n, ctx->loading_node_budget);
    if (!hit) {
        bucket = loading_bucket(units, n, ctx->loading_node_budget);
        entry = cache ? &cache->entries[bucket] : NULL;
        hit = entry && loading_key_equal(entry, v, units, n, ctx->loading_node_budget);
    } else {
        bucket = cache->last;
    }
    if (hit) {
        PROFILE(ctx, cache_hits);
        cache->last = bucket;
        entry->source_geometry = v;
        if (certificate) *result = entry->result;
        else result->outcome = entry->result.outcome;
        return SG_STATUS_OK;
    }
    PROFILE(ctx, checker_calls);
#ifdef SG_LOADING_PROFILE
    uint64_t checker_begin = sh_monotonic_ns();
#endif
    SGStatus status = sg_loading_check_controlled(v, units, n, ctx->loading_node_budget,
        loading_should_stop, (void *)ctx, result);
#ifdef SG_LOADING_PROFILE
    ((SGContext *)ctx)->loading_profile.checker_ns += sh_monotonic_ns() - checker_begin;
#endif
    if (status != SG_STATUS_OK || result->reason == SG_LOADING_INTERRUPTED) return status;
    if (!cache) {
        cache = calloc(1, sizeof(*cache));
        if (!cache) return SG_STATUS_OK; /* verified result, no cache required */
        ((SGContext *)ctx)->loading_cache = cache;
    }
    entry = &cache->entries[bucket];
    entry->source_geometry = v;
    entry->vehicle = *v;
    entry->budget = ctx->loading_node_budget;
    entry->count = n;
    memcpy(entry->units, units, n * sizeof(*units));
    entry->result = *result;
    cache->last = bucket;
    return SG_STATUS_OK;
}

SGStatus sg_loading_round(const SGContext *ctx, uint32_t vehicle, const SGRouteStop *stops,
    uint32_t count, uint32_t trip, SGLoadingResult *result, uint32_t *requests, uint32_t *indices) {
    SGLoadingView view = {0};
    view.stops = stops; view.count = count; view.insert_pos = UINT32_MAX;
    return loading_round(ctx, vehicle, &view, trip, result, requests, indices, 1);
}

static int loading_view_ok(const SGContext *ctx, uint32_t vehicle, const SGLoadingView *view) {
    uint32_t trips = 1;
    for (uint32_t i = 1; i < view->count; ++i)
        if (loading_stop(view, i)->trip_start) ++trips;
    if (vehicle >= ctx->num_vehicles) return 0;
    const SGVehicleRecord *v = &ctx->vehicles[vehicle];
    if ((trips > 1 && !v->has_multi_trip) || (v->max_trips && trips > v->max_trips)) return 0;
    for (uint32_t trip = 0; trip < trips; ++trip) {
        SGLoadingResult result;
        if (loading_round(ctx, vehicle, view, trip, &result, NULL, NULL, 0) != SG_STATUS_OK ||
            result.outcome != SG_LOADING_FEASIBLE) return 0;
    }
    return 1;
}

int sg_loading_stops_ok(const SGContext *ctx, uint32_t vehicle, const SGRouteStop *stops,
    uint32_t count) {
    if (!ctx->hard_loading) return 1;
    if (count && !stops) return 0;
    SGLoadingView view = {0};
    view.stops = stops; view.count = count; view.insert_pos = UINT32_MAX;
    return loading_view_ok(ctx, vehicle, &view);
}

int sg_loading_insertion_ok(const SGContext *ctx, const SGRouteSolution *sol, uint32_t request,
    uint32_t vehicle, uint32_t position, int new_trip) {
    if (!ctx->hard_loading) return 1;
    if (request >= ctx->num_requests || vehicle >= sol->num_vehicles ||
        ctx->requests[request].kind != SG_REQUEST_KIND_DELIVERY_ONLY ||
        position > sol->route_lengths[vehicle]) return 0;
    PROFILE(ctx, insertion_calls);
#ifdef SG_LOADING_PROFILE
    uint64_t insertion_begin = sh_monotonic_ns();
#endif
    uint32_t count = sol->route_stop_lengths[vehicle];
    if (count != sol->route_lengths[vehicle] || count == UINT32_MAX) return 0;
    SGLoadingView view = {0};
    view.stops = sg_route_vehicle_stop_ptr_const(sol, vehicle);
    view.count = count + 1; view.insert_pos = position;
    view.inserted.request_id = request;
    view.inserted.task_id = ctx->requests[request].delivery_task_id;
    view.inserted.trip_start = new_trip ? 1 : 0;
    int ok = loading_view_ok(ctx, vehicle, &view);
#ifdef SG_LOADING_PROFILE
    ((SGContext *)ctx)->loading_profile.insertion_ns += sh_monotonic_ns() - insertion_begin;
#endif
    return ok;
}

SGStatus sg_solution_get_loading(const SGContext *ctx, uint32_t route, uint32_t trip,
                                 SGLoadingResult *result, uint32_t *requests, uint32_t *units) {
    if (!result)
        return SG_STATUS_INVALID_ARG;
    memset(result, 0, sizeof(*result));
    result->outcome = SG_LOADING_UNKNOWN;
    result->reason = SG_LOADING_UNSUPPORTED;
    if (!ctx || ctx->loading_v2.schema_version || !ctx->final_solution || route >= sg_solution_get_route_count(ctx))
        return SG_STATUS_INVALID_ARG;
    uint32_t v = sg_solution_get_route_vehicle_id(ctx, route);
    return sg_loading_round(ctx, v, sg_route_vehicle_stop_ptr_const(ctx->final_solution, v),
                            ctx->final_solution->route_stop_lengths[v], trip, result, requests,
                            units);
}
