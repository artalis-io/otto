#include "sg_loading_search.h"
#include "sg_loading_v2_internal.h"
#include <limits.h>
#include <stdio.h>

#define V2_CAPS (SG_LOADING_V2_STACKING | SG_LOADING_V2_AXLES)
#define V2_ALL_CAPS (V2_CAPS | SG_LOADING_V2_RESHUFFLING)
#define V2_CACHE_SIZE 32u

typedef struct {
    const SGLoadingVehicle *physical;
    const SGAxleProfile *axles;
    uint32_t count, budget;
    SGLoadingUnit units[128];
    SGAxleSignedInterval cg[128];
    SGAxleLoadingResult result;
    uint8_t occupied;
} V2CacheEntry;
struct SGLoadingV2Cache {
    uint32_t last;
    V2CacheEntry entries[V2_CACHE_SIZE];
};
struct SGLoadingV2Report {
    _Atomic uint32_t references;
    SGLoadingV2ReportInfo info;
    SGStats stats;
    uint8_t has_stats;
    uint32_t round_count, diagnostic_count;
    SGLoadingV2RoundEvidence *rounds;
    SGLoadingV2Diagnostic *diagnostics;
};

static void clear_error(SGContext *ctx) {
    memset(&ctx->loading_v2_error, 0, sizeof(ctx->loading_v2_error));
    ctx->loading_v2_error.vehicle_id = UINT32_MAX;
    ctx->loading_v2_error.trip_index = UINT32_MAX;
    ctx->loading_v2_error.operation_index = UINT32_MAX;
    ctx->last_error[0] = 0;
}
void sg_loading_v2_clear_error(SGContext *ctx) {
    if (ctx)
        clear_error(ctx);
}
static SGStatus failure(SGContext *ctx, SGStatus status, SGLoadingV2ErrorCode code,
                        const char *path, const char *message) {
    if (ctx) {
        clear_error(ctx);
        ctx->loading_v2_error.code = code;
        snprintf(ctx->loading_v2_error.path, sizeof(ctx->loading_v2_error.path), "%s", path);
        sg_set_error(ctx, "%s", message);
    }
    return status;
}
static int enabled(const SGContext *ctx) { return ctx && ctx->loading_v2.schema_version == 2; }
static int sha256(const char *value) {
    if (!value)
        return 0;
    for (unsigned i = 0; i < 64; i++)
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f')))
            return 0;
    return value[64] == '\0';
}
static int declarations(const SGContext *ctx) {
    for (uint32_t i = 0; i < ctx->num_vehicles; i++)
        if (ctx->vehicles[i].loading)
            return 1;
    for (uint32_t i = 0; i < ctx->num_requests; i++)
        if (ctx->requests[i].has_loading_units)
            return 1;
    return 0;
}
SGStatus sg_loading_v2_capabilities(SGLoadingV2Capabilities *out) {
    if (!out || out->struct_size != sizeof(*out))
        return SG_STATUS_INVALID_ARG;
    if (out->schema_version != 2)
        return SG_STATUS_NOT_IMPLEMENTED;
    SGLoadingV2Capabilities v = {sizeof(v), 2,      V2_CAPS, 1, 0, 128, 128, 64, 8,
                                 100000,    100000, 4096,    4, 8, 64,  320, 321};
    *out = v;
    return SG_STATUS_OK;
}
SGStatus sg_loading_v2_get_error(const SGContext *ctx, SGLoadingV2Error *out) {
    if (!ctx || !out)
        return SG_STATUS_INVALID_ARG;
    *out = ctx->loading_v2_error;
    if (out->code == SG_LOADING_V2_ERROR_NONE)
        out->vehicle_id = out->trip_index = out->operation_index = UINT32_MAX;
    return SG_STATUS_OK;
}
SGStatus sg_loading_v2_set_config(SGContext *ctx, const SGLoadingV2Config *c) {
    if (!ctx || !c)
        return SG_STATUS_INVALID_ARG;
    if (c->struct_size != sizeof(*c))
        return failure(ctx, SG_STATUS_INVALID_ARG, SG_LOADING_V2_INVALID_INPUT, "/loading",
                       "Invalid loading configuration size.");
    if (c->schema_version != 2)
        return failure(ctx, SG_STATUS_NOT_IMPLEMENTED, SG_LOADING_V2_UNSUPPORTED_VERSION,
                       "/loading/version", "Unsupported loading schema version.");
    if (!(c->required_capabilities & SG_LOADING_V2_STACKING) ||
        (c->required_capabilities & ~V2_ALL_CAPS))
        return failure(ctx, SG_STATUS_INVALID_ARG, SG_LOADING_V2_INVALID_INPUT,
                       "/loading/required_capabilities",
                       "Unknown or incomplete loading capabilities.");
    if (c->required_capabilities & ~V2_CAPS)
        return failure(ctx, SG_STATUS_NOT_IMPLEMENTED, SG_LOADING_V2_UNSUPPORTED_CAPABILITY,
                       "/loading/required_capabilities", "Bounded reshuffling is not implemented.");
    if (c->node_budget > 100000 || c->frontier_budget)
        return failure(ctx, SG_STATUS_INVALID_ARG, SG_LOADING_V2_INVALID_INPUT, "/loading",
                       "Invalid loading work budget.");
    if ((!enabled(ctx) && (ctx->hard_loading || declarations(ctx))) ||
        (enabled(ctx) && ctx->loading_v2.required_capabilities != c->required_capabilities &&
         declarations(ctx)))
        return failure(ctx, SG_STATUS_INVALID_ARG, SG_LOADING_V2_INVALID_INPUT, "/loading",
                       "Reset loading declarations before changing version/capabilities.");
    sg_loading_invalidate(ctx);
    ctx->loading_v2 = *c;
    ctx->hard_loading = 1;
    ctx->loading_node_budget = c->node_budget;
    clear_error(ctx);
    return SG_STATUS_OK;
}
SGStatus sg_loading_v2_set_vehicle(SGContext *ctx, uint32_t id, const SGLoadingV2Vehicle *value) {
    if (!enabled(ctx) || !value || id >= ctx->num_vehicles)
        return failure(ctx, SG_STATUS_INVALID_ARG, SG_LOADING_V2_INVALID_INPUT, "/loading/vehicles",
                       "Invalid loading vehicle.");
    uint32_t caps = ctx->loading_v2.required_capabilities;
    if (value->handling || (!!value->axles != !!(caps & SG_LOADING_V2_AXLES)))
        return failure(ctx, SG_STATUS_INVALID_ARG, SG_LOADING_V2_INVALID_INPUT, "/loading/vehicles",
                       "Vehicle descriptors do not match required capabilities.");
    SGLoadingResult checked;
    if (sg_loading_check_joint(&value->physical, NULL, 0, 0, NULL, NULL, NULL, NULL, &checked) !=
            SG_STATUS_OK ||
        value->physical.payload_g > SG_AXLE_MAX_MASS_G ||
        value->physical.max_stack_weight_g > SG_AXLE_MAX_MASS_G)
        return failure(ctx, SG_STATUS_INVALID_ARG, SG_LOADING_V2_INVALID_INPUT, "/loading/vehicles",
                       "Invalid physical vehicle descriptor.");
    SGLoadingVehicle normalized = {0};
    const SGLoadingVehicle *v = &value->physical;
    normalized.slot_count = v->slot_count;
    normalized.max_layers = v->max_layers;
    normalized.clearance_mm = v->clearance_mm;
    normalized.class_count = v->class_count;
    normalized.max_stack_weight_g = v->max_stack_weight_g;
    normalized.payload_g = v->payload_g;
    normalized.access = v->access;
    for (uint32_t i = 0; i < v->slot_count; i++) {
        normalized.lane[i] = v->lane[i];
        normalized.depth[i] = v->depth[i];
    }
    for (uint32_t i = 0; i < v->class_count; i++)
        normalized.allowed_top[i] = v->allowed_top[i];
    SGAxleProfile profile = {0};
    if (value->axles) {
        const SGLoadingV2Axles *a = value->axles;
        if (a->model != SG_LOADING_V2_RIGID_TWO_AXLE || !sha256(a->profile_sha256) ||
            a->slot_count != v->slot_count || !a->slot_centres_mm)
            return failure(ctx, SG_STATUS_INVALID_ARG, SG_LOADING_V2_INVALID_INPUT,
                           "/loading/vehicles",
                           "Invalid axle model, profile identity or slot inventory.");
        profile.wheelbase_mm = a->wheelbase_mm;
        profile.footprint_length_mm = a->footprint_length_mm;
        profile.slot_count = a->slot_count;
        profile.deck_min_mm = a->deck_min_mm;
        profile.deck_max_mm = a->deck_max_mm;
        for (uint32_t i = 0; i < profile.slot_count; i++)
            profile.slot_centres_mm[i] = a->slot_centres_mm[i];
        profile.front_tare_g = (SGAxleMassInterval){a->front_tare_g.min, a->front_tare_g.max};
        profile.rear_tare_g = (SGAxleMassInterval){a->rear_tare_g.min, a->rear_tare_g.max};
        profile.front_limits_g = (SGAxleMassInterval){a->front_limits_g.min, a->front_limits_g.max};
        profile.rear_limits_g = (SGAxleMassInterval){a->rear_limits_g.min, a->rear_limits_g.max};
        profile.max_gross_g = a->max_gross_g;
        if (a->min_front_fraction) {
            profile.has_min_front_fraction = 1;
            profile.min_front_fraction = (SGAxleFraction){a->min_front_fraction->numerator,
                                                          a->min_front_fraction->denominator};
        }
        if (sg_axle_profile_validate(v, &profile) != SG_STATUS_OK)
            return failure(ctx, SG_STATUS_INVALID_ARG, SG_LOADING_V2_INVALID_INPUT,
                           "/loading/vehicles", "Invalid axle geometry, interval or numeric rule.");
    }
    SGLoadingVehicle *copy = malloc(sizeof(*copy));
    SGAxleProfile *axles = value->axles ? malloc(sizeof(*axles)) : NULL;
    if (!copy || (value->axles && !axles)) {
        free(copy);
        free(axles);
        return failure(ctx, SG_STATUS_OUT_OF_MEMORY, SG_LOADING_V2_ALLOCATION_FAILED,
                       "/loading/vehicles", "Cannot copy loading vehicle.");
    }
    *copy = normalized;
    if (axles)
        *axles = profile;
    sg_loading_invalidate(ctx);
    free(ctx->vehicles[id].loading);
    free(ctx->vehicles[id].axles);
    ctx->vehicles[id].loading = copy;
    ctx->vehicles[id].axles = axles;
    memset(ctx->vehicles[id].loading_profile_sha256, 0, 65);
    if (value->axles)
        memcpy(ctx->vehicles[id].loading_profile_sha256, value->axles->profile_sha256, 65);
    clear_error(ctx);
    return SG_STATUS_OK;
}
SGStatus sg_loading_v2_set_request(SGContext *ctx, uint32_t id, const SGLoadingV2Unit *units,
                                   uint32_t count) {
    if (!enabled(ctx) || id >= ctx->num_requests || count > 128 || (count && !units))
        return failure(ctx, SG_STATUS_INVALID_ARG, SG_LOADING_V2_INVALID_INPUT, "/loading/requests",
                       "Invalid loading request units.");
    int axles = !!(ctx->loading_v2.required_capabilities & SG_LOADING_V2_AXLES);
    for (uint32_t i = 0; i < count; i++) {
        const SGLoadingUnit *u = &units[i].physical;
        const SGLoadingV2SignedInterval *cg = units[i].cg_offset_mm;
        if (!u->height_mm || u->weight_g > SG_AXLE_MAX_MASS_G ||
            u->max_weight_above_g > SG_AXLE_MAX_MASS_G || u->stack_class >= 64 ||
            u->max_units_above > 127 || (!!cg != axles) ||
            units[i].temporary_offload_allowed != SG_LOADING_V2_UNSPECIFIED ||
            units[i].required_equipment_mask ||
            (cg && (cg->min > cg->max || cg->min < -1000000 || cg->max > 1000000)))
            return failure(ctx, SG_STATUS_INVALID_ARG, SG_LOADING_V2_INVALID_INPUT,
                           "/loading/requests",
                           "Invalid unit facts or capability-dependent fields.");
    }
    SGLoadingUnit *physical = count ? calloc(count, sizeof(*physical)) : NULL;
    SGAxleSignedInterval *cg = axles && count ? calloc(count, sizeof(*cg)) : NULL;
    if (count && (!physical || (axles && !cg))) {
        free(physical);
        free(cg);
        return failure(ctx, SG_STATUS_OUT_OF_MEMORY, SG_LOADING_V2_ALLOCATION_FAILED,
                       "/loading/requests", "Cannot copy loading units.");
    }
    for (uint32_t i = 0; i < count; i++) {
        physical[i].height_mm = units[i].physical.height_mm;
        physical[i].weight_g = units[i].physical.weight_g;
        physical[i].stack_class = units[i].physical.stack_class;
        physical[i].max_units_above = units[i].physical.max_units_above;
        physical[i].max_weight_above_g = units[i].physical.max_weight_above_g;
        if (axles)
            cg[i] = (SGAxleSignedInterval){units[i].cg_offset_mm->min, units[i].cg_offset_mm->max};
    }
    sg_loading_invalidate(ctx);
    SGRequestRecord *r = &ctx->requests[id];
    free(r->loading_units);
    free(r->loading_cg);
    r->loading_units = physical;
    r->loading_cg = cg;
    r->loading_count = count;
    r->has_loading_units = 1;
    clear_error(ctx);
    return SG_STATUS_OK;
}
SGStatus sg_loading_v2_set_stop_policy(SGContext *ctx, uint32_t task,
                                       const SGLoadingV2StopPolicy *policy) {
    (void)task;
    (void)policy;
    return failure(ctx, SG_STATUS_INVALID_ARG, SG_LOADING_V2_INVALID_INPUT, "/loading/stops",
                   "Stop handling policies require the unavailable reshuffling capability.");
}
SGStatus sg_loading_v2_reset(SGContext *ctx) {
    if (!ctx)
        return SG_STATUS_INVALID_ARG;
    if (!enabled(ctx))
        return failure(ctx, SG_STATUS_INVALID_ARG, SG_LOADING_V2_INVALID_INPUT, "/loading",
                       "No v2 declarations to reset.");
    sg_loading_invalidate(ctx);
    for (uint32_t i = 0; i < ctx->num_vehicles; i++) {
        free(ctx->vehicles[i].loading);
        ctx->vehicles[i].loading = NULL;
        free(ctx->vehicles[i].axles);
        ctx->vehicles[i].axles = NULL;
        memset(ctx->vehicles[i].loading_profile_sha256, 0, 65);
    }
    for (uint32_t i = 0; i < ctx->num_requests; i++) {
        free(ctx->requests[i].loading_units);
        ctx->requests[i].loading_units = NULL;
        free(ctx->requests[i].loading_cg);
        ctx->requests[i].loading_cg = NULL;
        ctx->requests[i].loading_count = 0;
        ctx->requests[i].has_loading_units = 0;
    }
    memset(&ctx->loading_v2, 0, sizeof(ctx->loading_v2));
    ctx->hard_loading = 0;
    ctx->loading_node_budget = 0;
    clear_error(ctx);
    return SG_STATUS_OK;
}
SGStatus sg_loading_v2_model_validate(SGContext *ctx) {
    if (!enabled(ctx))
        return SG_STATUS_OK;
    if (ctx->num_vehicles > 100000 || ctx->num_requests > 100000 || ctx->num_tasks > 100000)
        return failure(ctx, SG_STATUS_NOT_IMPLEMENTED, SG_LOADING_V2_UNSUPPORTED_MODEL, "/loading",
                       "V2 model size exceeds supported limits.");
    int axles = !!(ctx->loading_v2.required_capabilities & SG_LOADING_V2_AXLES);
    for (uint32_t v = 0; v < ctx->num_vehicles; v++) {
        const SGVehicleRecord *vehicle = &ctx->vehicles[v];
        SGLoadingResult result;
        if (!vehicle->loading || (!!vehicle->axles != axles) || vehicle->has_initial_load ||
            vehicle->open_start ||
            sg_loading_check_joint(vehicle->loading, NULL, 0, 0, NULL, NULL, NULL, NULL, &result) !=
                SG_STATUS_OK ||
            (axles && sg_axle_profile_validate(vehicle->loading, vehicle->axles) != SG_STATUS_OK))
            return failure(ctx, SG_STATUS_INVALID_ARG, SG_LOADING_V2_INVALID_INPUT,
                           "/loading/vehicles",
                           "Complete supported vehicle loading declarations are required.");
    }
    for (uint32_t r = 0; r < ctx->num_requests; r++) {
        const SGRequestRecord *request = &ctx->requests[r];
        if (!request->has_loading_units || request->kind != SG_REQUEST_KIND_DELIVERY_ONLY ||
            (axles && request->loading_count && !request->loading_cg))
            return failure(ctx, SG_STATUS_INVALID_ARG, SG_LOADING_V2_INVALID_INPUT,
                           "/loading/requests",
                           "Complete delivery-only loading units are required.");
        for (uint32_t v = 0; v < ctx->num_vehicles; v++) {
            for (uint32_t i = 0; i < request->loading_count; i++)
                if (request->loading_units[i].stack_class >= ctx->vehicles[v].loading->class_count)
                    return failure(ctx, SG_STATUS_INVALID_ARG, SG_LOADING_V2_INVALID_INPUT,
                                   "/loading/requests", "Undeclared stack class.");
            if (axles) {
                uint32_t slots[128] = {0};
                uint8_t active[128] = {0};
                SGAxleState state;
                if (sg_axle_state(ctx->vehicles[v].loading, ctx->vehicles[v].axles,
                                  request->loading_units, request->loading_cg, slots, active,
                                  request->loading_count, &state) == SG_STATUS_INVALID_ARG)
                    return failure(ctx, SG_STATUS_INVALID_ARG, SG_LOADING_V2_INVALID_INPUT,
                                   "/loading/requests",
                                   "Mass centres do not match vehicle footprints.");
            }
        }
    }
    return SG_STATUS_OK;
}

/* Cache proofs use unit ordinals, never application identities. Identical
 * physical loads can reuse a proof across equivalent vehicles/orders; the
 * collector always supplies the CURRENT IDs when constructing a report.
 * Geometry, CG, stop order, capabilities (profile presence) and budget must
 * still compare exactly. No hash alone authorizes reuse. */
static int cache_equal(const V2CacheEntry *e, const SGContext *ctx, uint32_t vehicle,
                       const SGLoadingUnit *units, const SGAxleSignedInterval *cg, uint32_t n) {
    if (!e->occupied || e->count != n || e->budget != ctx->loading_v2.node_budget)
        return 0;
    const SGLoadingVehicle *v = ctx->vehicles[vehicle].loading;
    const SGAxleProfile *a = ctx->vehicles[vehicle].axles;
    if (e->physical != v && memcmp(e->physical, v, sizeof(*v)))
        return 0;
    if (e->axles != a && (!e->axles || !a || memcmp(e->axles, a, sizeof(*a))))
        return 0;
    return !n || (!memcmp(e->units, units, n * sizeof(*units)) &&
                  (!a || !memcmp(e->cg, cg, n * sizeof(*cg))));
}
static int v2_cancel(void *data) {
    const SGContext *ctx = data;
    return sg_cancelled(ctx) ||
           (ctx->loading_v2_stop && ctx->loading_v2_stop(ctx->loading_v2_stop_data));
}
SGStatus sg_loading_v2_check_units(const SGContext *ctx, uint32_t vehicle,
                                   const SGLoadingUnit *units, const SGAxleSignedInterval *cg,
                                   uint32_t n, int certificate, SGLoadingResult *result,
                                   SGAxleLoadingResult *full) {
    if (v2_cancel((void *)ctx)) {
        memset(result, 0, sizeof(*result));
        result->outcome = SG_LOADING_UNKNOWN;
        result->reason = SG_LOADING_INTERRUPTED;
        if (full) {
            memset(full, 0, sizeof(*full));
            full->loading = *result;
        }
        return SG_STATUS_OK;
    }
    SGLoadingV2Cache *cache = ctx->loading_v2_cache;
    uint32_t bucket = cache ? cache->last : 0;
    V2CacheEntry *entry = cache ? &cache->entries[bucket] : NULL;
    if (!entry || !cache_equal(entry, ctx, vehicle, units, cg, n)) {
        uint64_t hash = ctx->loading_v2.node_budget ^ n;
        for (uint32_t i = 0; i < n; i++) {
            hash = (hash ^ units[i].stop ^ units[i].height_mm ^ units[i].weight_g ^
                    units[i].max_weight_above_g ^ units[i].stack_class ^ units[i].max_units_above) *
                   UINT64_C(1099511628211);
            if (ctx->vehicles[vehicle].axles)
                hash = (hash ^ (uint32_t)cg[i].min ^ (uint32_t)cg[i].max) * UINT64_C(1099511628211);
        }
        bucket = (uint32_t)(hash % V2_CACHE_SIZE);
        entry = cache ? &cache->entries[bucket] : NULL;
    }
    if (entry && cache_equal(entry, ctx, vehicle, units, cg, n)) {
        cache->last = bucket;
        if (certificate) {
            *result = entry->result.loading;
            if (full)
                *full = entry->result;
        } else
            result->outcome = entry->result.loading.outcome;
        return SG_STATUS_OK;
    }
    SGAxleLoadingResult evaluated;
    SGStatus status =
        sg_axle_loading_check(ctx->vehicles[vehicle].loading, ctx->vehicles[vehicle].axles, units,
                              ctx->vehicles[vehicle].axles ? cg : NULL, n,
                              ctx->loading_v2.node_budget, v2_cancel, (void *)ctx, &evaluated);
    if (certificate) {
        *result = evaluated.loading;
        if (full)
            *full = evaluated;
    } else
        result->outcome = evaluated.loading.outcome;
    if (status != SG_STATUS_OK || evaluated.loading.reason == SG_LOADING_INTERRUPTED ||
        v2_cancel((void *)ctx))
        return status;
    if (!cache) {
        cache = calloc(1, sizeof(*cache));
        if (!cache)
            return SG_STATUS_OK;
        ((SGContext *)ctx)->loading_v2_cache = cache;
    }
    entry = &cache->entries[bucket];
    entry->occupied = 1;
    entry->count = n;
    entry->budget = ctx->loading_v2.node_budget;
    entry->physical = ctx->vehicles[vehicle].loading;
    entry->axles = ctx->vehicles[vehicle].axles;
    if (n) {
        memcpy(entry->units, units, n * sizeof(*units));
        if (entry->axles)
            memcpy(entry->cg, cg, n * sizeof(*cg));
    }
    entry->result = evaluated;
    cache->last = bucket;
    return SG_STATUS_OK;
}

static const SGRouteStop *view_stop(const SGLoadingV2View *view, uint32_t index) {
    if (index == view->insert_pos)
        return &view->inserted;
    return &view->stops[index - (index > view->insert_pos)];
}
SGStatus sg_loading_v2_round_view(const SGContext *ctx, uint32_t vehicle,
                                  const SGLoadingV2View *view, uint32_t trip,
                                  SGLoadingResult *result, uint32_t *requests, uint32_t *indices,
                                  int certificate, SGAxleLoadingResult *full) {
    if (!result)
        return SG_STATUS_INVALID_ARG;
    memset(result, 0, certificate ? sizeof(*result) : offsetof(SGLoadingResult, placements));
    result->outcome = SG_LOADING_UNKNOWN;
    result->reason = SG_LOADING_UNSUPPORTED;
    if (full) {
        memset(full, 0, sizeof(*full));
        full->loading.outcome = SG_LOADING_UNKNOWN;
        full->loading.reason = SG_LOADING_UNSUPPORTED;
    }
    if (!enabled(ctx) || !view || vehicle >= ctx->num_vehicles || !ctx->vehicles[vehicle].loading ||
        (view->count && !view->stops && !(view->count == 1 && view->insert_pos == 0)))
        return SG_STATUS_INVALID_ARG;
    SGLoadingUnit units[128];
    SGAxleSignedInterval cg[128];
    uint32_t n = 0, ordinal = 0, current = 0;
    int found = !view->count && !trip;
    int axles = !!ctx->vehicles[vehicle].axles;
    for (uint32_t i = 0; i < view->count; i++) {
        const SGRouteStop *stop = view_stop(view, i);
        if (i && stop->trip_start) {
            current++;
            ordinal = 0;
        }
        if (current != trip)
            continue;
        found = 1;
        if (stop->request_id >= ctx->num_requests || stop->is_pickup)
            return SG_STATUS_INVALID_ARG;
        const SGRequestRecord *request = &ctx->requests[stop->request_id];
        if (!request->has_loading_units || request->kind != SG_REQUEST_KIND_DELIVERY_ONLY ||
            request->delivery_task_id != stop->task_id ||
            (axles && request->loading_count && !request->loading_cg))
            return SG_STATUS_INVALID_ARG;
        if (request->loading_count > 128 - n)
            return SG_STATUS_OK;
        for (uint32_t j = 0; j < request->loading_count; j++) {
            units[n] = request->loading_units[j];
            units[n].stop = ordinal;
            if (axles)
                cg[n] = request->loading_cg[j];
            if (requests)
                requests[n] = stop->request_id;
            if (indices)
                indices[n] = j;
            n++;
        }
        ordinal++;
    }
    if (!found)
        return SG_STATUS_INVALID_ARG;
    return sg_loading_v2_check_units(ctx, vehicle, units, axles ? cg : NULL, n, certificate, result,
                                     full);
}
SGStatus sg_loading_round_v2(const SGContext *ctx, uint32_t vehicle, const SGRouteStop *stops,
                             uint32_t count, uint32_t trip, SGAxleLoadingResult *result,
                             uint32_t *requests, uint32_t *indices) {
    if (!result)
        return SG_STATUS_INVALID_ARG;
    SGLoadingV2View view = {0};
    view.stops = stops;
    view.count = count;
    view.insert_pos = UINT32_MAX;
    SGLoadingResult summary;
    return sg_loading_v2_round_view(ctx, vehicle, &view, trip, &summary, requests, indices, 1,
                                    result);
}

int sg_loading_v2_apply_departure(const SGContext *ctx, uint32_t vehicle, double *time) {
    if (!ctx->loading_v2_has_departure || vehicle != ctx->loading_v2_departure_vehicle)
        return 1;
    double departure = ctx->loading_v2_departure;
    const SGVehicleRecord *v = &ctx->vehicles[vehicle];
    if (!isfinite(departure) || departure < *time || departure > 1e12)
        return 0;
    const SGDepotRecord *depot = &ctx->depots[v->start_depot_id];
    if (depot->has_time_window && departure > depot->tw_late)
        return 0;
    *time = departure;
    return 1;
}
static SGLoadingV2Report *new_report(const SGContext *ctx, uint32_t rounds) {
    SGLoadingV2Report *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    atomic_init(&r->references, 1);
    r->info = (SGLoadingV2ReportInfo){2, ctx->loading_v2.required_capabilities,
                                      ctx->loading_v2.node_budget, ctx->loading_v2.frontier_budget};
    r->rounds = rounds ? calloc(rounds, sizeof(*r->rounds)) : NULL;
    if (rounds && !r->rounds) {
        free(r);
        return NULL;
    }
    r->round_count = rounds;
    return r;
}
static int retain_report(SGLoadingV2Report *r) {
    uint32_t n = atomic_load(&r->references);
    while (n < UINT32_MAX) {
        if (atomic_compare_exchange_weak(&r->references, &n, n + 1))
            return 1;
    }
    return 0;
}
void sg_loading_v2_report_free(SGLoadingV2Report *r) {
    if (!r || atomic_fetch_sub(&r->references, 1) != 1)
        return;
    for (uint32_t i = 0; i < r->round_count; i++) {
        free((void *)r->rounds[i].placements);
        free((void *)r->rounds[i].operations);
        free((void *)r->rounds[i].axle_states);
        free((void *)r->rounds[i].stops);
    }
    free(r->rounds);
    free(r->diagnostics);
    free(r);
}
void sg_loading_v2_report_invalidate(SGContext *ctx) {
    if (!ctx)
        return;
    sg_loading_v2_report_free(ctx->loading_v2_committed_report);
    ctx->loading_v2_committed_report = NULL;
}
SGStatus sg_loading_v2_report_info(const SGLoadingV2Report *r, SGLoadingV2ReportInfo *out) {
    if (!r || !out)
        return SG_STATUS_INVALID_ARG;
    *out = r->info;
    return SG_STATUS_OK;
}
SGStatus sg_loading_v2_report_stats(const SGLoadingV2Report *r, SGStats *out) {
    if (!r || !out)
        return SG_STATUS_INVALID_ARG;
    if (!r->has_stats)
        return SG_STATUS_ERROR;
    *out = r->stats;
    return SG_STATUS_OK;
}
SGStatus sg_loading_v2_report_view(const SGLoadingV2Report *r, uint32_t *count,
                                   const SGLoadingV2RoundEvidence **rounds,
                                   uint32_t *diagnostic_count,
                                   const SGLoadingV2Diagnostic **diagnostics) {
    if (!r || !count || !rounds || !diagnostic_count || !diagnostics)
        return SG_STATUS_INVALID_ARG;
    *count = r->round_count;
    *rounds = r->rounds;
    *diagnostic_count = r->diagnostic_count;
    *diagnostics = r->diagnostics;
    return SG_STATUS_OK;
}
static SGLoadingV2Reason v2_reason(SGLoadingReason reason) {
    switch (reason) {
    case SG_LOADING_WITNESS:
        return SG_LOADING_V2_WITNESS;
    case SG_LOADING_CAPACITY:
        return SG_LOADING_V2_CAPACITY_BOUND;
    case SG_LOADING_NO_ARRANGEMENT:
        return SG_LOADING_V2_EXHAUSTIVE_NO_WITNESS;
    case SG_LOADING_BUDGET:
        return SG_LOADING_V2_NODE_BUDGET;
    case SG_LOADING_INTERRUPTED:
        return SG_LOADING_V2_INTERRUPTED;
    default:
        return SG_LOADING_V2_UNSUPPORTED_SIZE;
    }
}
static SGLoadingV2AxleState exported_state(const SGAxleState *s, uint32_t step) {
    SGLoadingV2AxleState out = {0};
    out.step = step;
    out.front_min_g = (SGLoadingV2Rational){s->front_min_num, s->denominator};
    out.front_max_g = (SGLoadingV2Rational){s->front_max_num, s->denominator};
    out.rear_min_g = (SGLoadingV2Rational){s->rear_min_num, s->denominator};
    out.rear_max_g = (SGLoadingV2Rational){s->rear_max_num, s->denominator};
    out.gross_min_g = s->gross_min_g;
    out.gross_max_g = s->gross_max_g;
    return out;
}
static SGStatus report_round(const SGContext *ctx, uint32_t vehicle, const SGRouteStop *stops,
                             uint32_t count, uint32_t trip, SGLoadingV2RoundEvidence *out) {
    SGAxleLoadingResult result;
    uint32_t ids[128], indices[128];
    SGStatus status = sg_loading_round_v2(ctx, vehicle, stops, count, trip, &result, ids, indices);
    if (status != SG_STATUS_OK)
        return status;
    memset(out, 0, sizeof(*out));
    out->vehicle_id = vehicle;
    out->trip_index = trip;
    out->outcome = result.loading.outcome;
    out->reason = v2_reason(result.loading.reason);
    out->nodes = (uint32_t)result.loading.nodes;
    if (out->outcome != SG_LOADING_FEASIBLE)
        return SG_STATUS_OK;
    uint32_t n = result.loading.count, first = 0, end = count, current = 0;
    for (uint32_t i = 1; i < count; i++)
        if (stops[i].trip_start) {
            if (current == trip) {
                end = i;
                break;
            }
            current++;
            first = i;
        }
    if (!count) {
        first = 0;
        end = 0;
    }
    uint32_t stop_count = end - first;
    SGLoadingV2Placement *placements = n ? calloc(n, sizeof(*placements)) : NULL;
    SGLoadingV2Operation *operations = n ? calloc(2 * n, sizeof(*operations)) : NULL;
    uint32_t state_count = ctx->vehicles[vehicle].axles ? 2 * n + 1 : 0;
    SGLoadingV2AxleState *states = state_count ? calloc(state_count, sizeof(*states)) : NULL;
    SGLoadingV2StopEvidence *timing = stop_count ? calloc(stop_count, sizeof(*timing)) : NULL;
    if ((n && (!placements || !operations)) || (state_count && !states) ||
        (stop_count && !timing)) {
        free(placements);
        free(operations);
        free(states);
        free(timing);
        return SG_STATUS_OUT_OF_MEMORY;
    }
    for (uint32_t i = 0; i < n; i++) {
        placements[i] =
            (SGLoadingV2Placement){ids[i], indices[i], result.loading.placements[i].slot,
                                   result.loading.placements[i].layer};
        uint32_t load = result.unload_order[n - i - 1], deliver = result.unload_order[i];
        const SGLoadingPlacement *lp = &result.loading.placements[load],
                                 *dp = &result.loading.placements[deliver];
        operations[i] =
            (SGLoadingV2Operation){SG_LOADING_V2_LOAD, UINT32_MAX, ids[load], indices[load],
                                   lp->slot,           lp->layer,  UINT32_MAX};
        uint32_t ordinal = 0, found = 0;
        for (uint32_t j = first; j < end; j++, ordinal++)
            if (stops[j].request_id == ids[deliver]) {
                found = ordinal;
                break;
            }
        operations[n + i] = (SGLoadingV2Operation){SG_LOADING_V2_DELIVER,
                                                   found,
                                                   ids[deliver],
                                                   indices[deliver],
                                                   dp->slot,
                                                   dp->layer,
                                                   UINT32_MAX};
    }
    if (state_count) {
        for (uint32_t i = 0; i <= n; i++)
            states[i] = exported_state(&result.states[n - i], i);
        for (uint32_t i = 1; i <= n; i++)
            states[n + i] = exported_state(&result.states[i], n + i);
    }
    for (uint32_t i = 0; i < stop_count; i++) {
        const SGRouteStop *s = &stops[first + i];
        timing[i] = (SGLoadingV2StopEvidence){i,          s->task_id,
                                              s->arrival, s->service_start,
                                              s->depart,  ctx->tasks[s->task_id].service_seconds,
                                              0,          0,
                                              0};
    }
    out->placement_count = n;
    out->operation_count = 2 * n;
    out->axle_state_count = state_count;
    out->stop_count = stop_count;
    out->placements = placements;
    out->operations = operations;
    out->axle_states = states;
    out->stops = timing;
    return SG_STATUS_OK;
}
static SGStatus report_diagnostics(const SGContext *ctx, SGLoadingV2Report *r) {
    uint32_t unassigned = ctx->final_solution->base.num_unassigned;
    r->diagnostics = unassigned ? calloc(unassigned, sizeof(*r->diagnostics)) : NULL;
    if (unassigned && !r->diagnostics)
        return SG_STATUS_OUT_OF_MEMORY;
    r->diagnostic_count = unassigned;
    uint64_t budget = ctx->loading_v2.node_budget < 128 ? ctx->loading_v2.node_budget : 128;
    for (uint32_t i = 0; i < unassigned; i++) {
        uint32_t id = ctx->final_solution->base.unassigned_ids[i];
        const SGRequestRecord *request = &ctx->requests[id];
        SGLoadingUnit units[128];
        for (uint32_t j = 0; j < request->loading_count; j++) {
            units[j] = request->loading_units[j];
            units[j].stop = 0;
        }
        SGLoadingV2Diagnostic *d = &r->diagnostics[i];
        d->request_id = id;
        d->diagnostic_node_budget = (uint32_t)budget;
        for (uint32_t v = 0; v < ctx->num_vehicles; v++) {
            SGAxleLoadingResult result;
            SGStatus status = sg_axle_loading_check(
                ctx->vehicles[v].loading, ctx->vehicles[v].axles, units,
                ctx->vehicles[v].axles ? request->loading_cg : NULL, request->loading_count, budget,
                v2_cancel, (void *)ctx, &result);
            if (status != SG_STATUS_OK || result.loading.outcome == SG_LOADING_UNKNOWN)
                d->unknown_vehicles++;
            else if (result.loading.outcome == SG_LOADING_FEASIBLE)
                d->feasible_vehicles++;
            else
                d->infeasible_vehicles++;
        }
    }
    return SG_STATUS_OK;
}
static SGStatus solution_report_build(const SGContext *ctx, SGLoadingV2Report **out) {
    uint32_t rounds = 0;
    for (uint32_t v = 0; v < ctx->num_vehicles; v++) {
        uint32_t count = ctx->final_solution->route_stop_lengths[v];
        if (!count)
            continue;
        rounds++;
        const SGRouteStop *stops = sg_route_vehicle_stop_ptr_const(ctx->final_solution, v);
        for (uint32_t i = 1; i < count; i++)
            if (stops[i].trip_start)
                rounds++;
    }
    SGLoadingV2Report *r = new_report(ctx, rounds);
    if (!r)
        return SG_STATUS_OUT_OF_MEMORY;
    r->has_stats = 1;
    r->stats = ctx->stats;
    uint32_t index = 0;
    for (uint32_t v = 0; v < ctx->num_vehicles; v++) {
        uint32_t count = ctx->final_solution->route_stop_lengths[v];
        if (!count)
            continue;
        const SGRouteStop *stops = sg_route_vehicle_stop_ptr_const(ctx->final_solution, v);
        uint32_t trips = 1;
        for (uint32_t i = 1; i < count; i++)
            if (stops[i].trip_start)
                trips++;
        for (uint32_t trip = 0; trip < trips; trip++) {
            SGStatus status = report_round(ctx, v, stops, count, trip, &r->rounds[index++]);
            if (status != SG_STATUS_OK) {
                sg_loading_v2_report_free(r);
                return status;
            }
        }
    }
    SGStatus diagnostic_status = report_diagnostics(ctx, r);
    if (diagnostic_status != SG_STATUS_OK) {
        sg_loading_v2_report_free(r);
        return diagnostic_status;
    }
    *out = r;
    return SG_STATUS_OK;
}
SGStatus sg_loading_v2_solution_report(const SGContext *ctx, SGLoadingV2Report **out) {
    if (!out)
        return SG_STATUS_INVALID_ARG;
    *out = NULL;
    if (!enabled(ctx) || !ctx->final_solution)
        return failure((SGContext *)ctx, SG_STATUS_ERROR, SG_LOADING_V2_NO_RESULT, "",
                       "No committed loading result.");
    if (sg_cancelled(ctx))
        return failure((SGContext *)ctx, SG_STATUS_LIMIT, SG_LOADING_V2_VALIDATION_UNKNOWN, "",
                       "Loading result interrupted.");
    SGLoadingV2Report *r = ctx->loading_v2_committed_report;
    if (!r) {
        SGStatus status = solution_report_build(ctx, &r);
        if (status != SG_STATUS_OK)
            return failure((SGContext *)ctx, status,
                           status == SG_STATUS_OUT_OF_MEMORY ? SG_LOADING_V2_ALLOCATION_FAILED
                                                             : SG_LOADING_V2_INTERNAL_ERROR,
                           "", "Cannot snapshot loading evidence.");
        if (v2_cancel((void *)ctx)) {
            sg_loading_v2_report_free(r);
            return failure((SGContext *)ctx, SG_STATUS_LIMIT, SG_LOADING_V2_VALIDATION_UNKNOWN, "",
                           "Loading snapshot interrupted.");
        }
        ((SGContext *)ctx)->loading_v2_committed_report = r;
    }
    if (!retain_report(r))
        return failure((SGContext *)ctx, SG_STATUS_ERROR, SG_LOADING_V2_INTERNAL_ERROR, "",
                       "Loading snapshot reference limit.");
    *out = r;
    return SG_STATUS_OK;
}

/* Staged validation shares immutable prepared model data, but never a saved
 * solution, proof, violation buffer or mutable cache with its caller. */
static SGStatus shadow_init(SGContext *ctx, SGContext *shadow) {
    SGStatus status = sg_validate_model(ctx);
    if (status != SG_STATUS_OK)
        return status;
    status = sg_prepare_travel(ctx);
    if (status != SG_STATUS_OK)
        return status;
    size_t offset = offsetof(SGContext, cancel_requested),
           after = offset + sizeof(ctx->cancel_requested);
    memcpy(shadow, ctx, offset);
    memcpy((char *)shadow + after, (char *)ctx + after, sizeof(*ctx) - after);
    atomic_init(&shadow->cancel_requested, atomic_load(&ctx->cancel_requested));
    shadow->parent_cancel = ctx->parent_cancel ? ctx->parent_cancel : &ctx->cancel_requested;
    shadow->final_solution = NULL;
    shadow->active_solution = NULL;
    shadow->loading_cache = NULL;
    shadow->loading_v2_cache = NULL;
    shadow->loading_v2_committed_report = NULL;
    shadow->violations = NULL;
    shadow->num_violations = 0;
    shadow->violations_capacity = 0;
    memset(&shadow->stats, 0, sizeof(shadow->stats));
    memset(&shadow->scratch, 0, sizeof(shadow->scratch));
    memset(&shadow->penalty, 0, sizeof(shadow->penalty));
    clear_error(shadow);
    /* No physical search is allowed to replace a supplied witness. Ordinary
     * hard validation is staged here; independent joint replay/search follows
     * before any snapshot may be transferred to the authoritative context. */
    shadow->hard_loading = 0;
    return SG_STATUS_OK;
}
static void shadow_free(SGContext *shadow, const SGContext *source) {
    sg_route_solution_free(shadow->final_solution, NULL);
    free(shadow->loading_cache);
    free(shadow->loading_v2_cache);
    sg_loading_v2_report_invalidate(shadow);
    free(shadow->violations);
    sg_scratch_free(shadow);
    if (shadow->request_locks != source->request_locks)
        free(shadow->request_locks);
}
static int valid_info(const SGContext *ctx, const SGLoadingV2ReportInfo *info) {
    return info && info->schema_version == 2 &&
           info->required_capabilities == ctx->loading_v2.required_capabilities &&
           info->node_budget == ctx->loading_v2.node_budget &&
           info->frontier_budget == ctx->loading_v2.frontier_budget;
}
static int equal_reactions(const SGLoadingV2AxleState *reported, const SGAxleState *actual,
                           uint32_t step) {
    return reported->step == step && reported->front_min_g.denominator == actual->denominator &&
           reported->front_max_g.denominator == actual->denominator &&
           reported->rear_min_g.denominator == actual->denominator &&
           reported->rear_max_g.denominator == actual->denominator &&
           reported->front_min_g.numerator == actual->front_min_num &&
           reported->front_max_g.numerator == actual->front_max_num &&
           reported->rear_min_g.numerator == actual->rear_min_num &&
           reported->rear_max_g.numerator == actual->rear_max_num &&
           reported->gross_min_g == actual->gross_min_g &&
           reported->gross_max_g == actual->gross_max_g;
}
static int accessible(const SGLoadingVehicle *v, const SGLoadingPlacement *placements,
                      const uint8_t *active, uint32_t n, uint32_t slot, uint32_t layer) {
    for (uint32_t i = 0; i < n; i++)
        if (active[i]) {
            if (placements[i].slot == slot && placements[i].layer > layer)
                return 0;
            if (v->access == SG_LOADING_REAR_LIFO && v->lane[placements[i].slot] == v->lane[slot] &&
                v->depth[placements[i].slot] < v->depth[slot])
                return 0;
        }
    return 1;
}
static SGStatus replay_physical(const SGContext *ctx, uint32_t vehicle, const SGRouteStop *stops,
                                uint32_t count, const SGLoadingV2RoundEvidence *w) {
    SGLoadingUnit units[128];
    SGAxleSignedInterval cg[128];
    uint32_t ids[128], indices[128], n = 0;
    for (uint32_t stop = 0; stop < count; stop++) {
        uint32_t id = stops[stop].request_id;
        if (id >= ctx->num_requests)
            return SG_STATUS_INVALID_ARG;
        const SGRequestRecord *r = &ctx->requests[id];
        if (r->loading_count > 128 - n)
            return SG_STATUS_LIMIT;
        for (uint32_t j = 0; j < r->loading_count; j++) {
            units[n] = r->loading_units[j];
            units[n].stop = stop;
            ids[n] = id;
            indices[n] = j;
            if (ctx->vehicles[vehicle].axles)
                cg[n] = r->loading_cg[j];
            n++;
        }
    }
    uint32_t state_count = ctx->vehicles[vehicle].axles ? 2 * n + 1 : 0;
    if (!w || w->outcome != SG_LOADING_FEASIBLE || w->reason != SG_LOADING_V2_WITNESS ||
        w->placement_count != n || w->operation_count != 2 * n ||
        w->axle_state_count != state_count || w->stop_count != count ||
        w->nodes > ctx->loading_v2.node_budget || w->extra_moves || w->extra_service_sec ||
        w->extra_fee_units || (n && (!w->placements || !w->operations)) ||
        (state_count && !w->axle_states) || (count && !w->stops))
        return SG_STATUS_INVALID_ARG;
    SGLoadingPlacement placements[128];
    uint32_t slots[128];
    uint8_t active[128] = {0}, loaded[128] = {0};
    for (uint32_t i = 0; i < n; i++) {
        const SGLoadingV2Placement *p = &w->placements[i];
        if (p->request_id != ids[i] || p->unit_index != indices[i] ||
            p->slot >= ctx->vehicles[vehicle].loading->slot_count ||
            p->layer >= ctx->vehicles[vehicle].loading->max_layers)
            return SG_STATUS_INVALID_ARG;
        placements[i] = (SGLoadingPlacement){p->slot, p->layer};
        slots[i] = p->slot;
    }
    const SGLoadingVehicle *v = ctx->vehicles[vehicle].loading;
    SGStatus status = sg_loading_validate_joint(v, units, n, placements);
    if (status != SG_STATUS_OK)
        return status;
    if (state_count) {
        SGAxleState empty;
        status =
            sg_axle_state(v, ctx->vehicles[vehicle].axles, units, cg, slots, active, n, &empty);
        if (status != SG_STATUS_OK)
            return status;
        if (!equal_reactions(&w->axle_states[0], &empty, 0))
            return SG_STATUS_INVALID_ARG;
    }
    uint32_t last_stop = 0;
    for (uint32_t step = 0; step < 2 * n; step++) {
        const SGLoadingV2Operation *op = &w->operations[step];
        uint32_t id = UINT32_MAX;
        for (uint32_t j = 0; j < n; j++)
            if (ids[j] == op->request_id && indices[j] == op->unit_index) {
                id = j;
                break;
            }
        if (id == UINT32_MAX || op->slot != placements[id].slot ||
            op->layer != placements[id].layer || op->staging_position != UINT32_MAX)
            return SG_STATUS_INVALID_ARG;
        if (step < n) {
            if (op->kind != SG_LOADING_V2_LOAD || op->stop_index != UINT32_MAX || loaded[id] ||
                !accessible(v, placements, active, n, op->slot, op->layer))
                return SG_STATUS_INFEASIBLE;
            active[id] = loaded[id] = 1;
        } else {
            if (op->kind != SG_LOADING_V2_DELIVER || op->stop_index != units[id].stop ||
                op->stop_index < last_stop || !active[id] ||
                !accessible(v, placements, active, n, op->slot, op->layer))
                return SG_STATUS_INFEASIBLE;
            for (uint32_t j = 0; j < n; j++)
                if (active[j] && units[j].stop < units[id].stop)
                    return SG_STATUS_INFEASIBLE;
            last_stop = op->stop_index;
            active[id] = 0;
        }
        /* Replay every partial stack, not only the departure arrangement. */
        SGLoadingUnit remaining[128];
        SGLoadingPlacement remaining_p[128];
        uint32_t m = 0;
        for (uint32_t j = 0; j < n; j++)
            if (active[j]) {
                remaining[m] = units[j];
                remaining_p[m++] = placements[j];
            }
        status = sg_loading_validate_joint(v, remaining, m, remaining_p);
        if (status != SG_STATUS_OK)
            return status;
        if (state_count) {
            SGAxleState state;
            status =
                sg_axle_state(v, ctx->vehicles[vehicle].axles, units, cg, slots, active, n, &state);
            if (status != SG_STATUS_OK)
                return status;
            if (!equal_reactions(&w->axle_states[step + 1], &state, step + 1))
                return SG_STATUS_INVALID_ARG;
        }
        if (sg_cancelled(ctx))
            return SG_STATUS_LIMIT;
    }
    for (uint32_t i = 0; i < n; i++)
        if (!loaded[i] || active[i])
            return SG_STATUS_INFEASIBLE;
    for (uint32_t i = 0; i < count; i++) {
        const SGLoadingV2StopEvidence *s = &w->stops[i];
        const SGRouteStop *actual = &stops[i];
        if (s->stop_index != i || s->task_id != actual->task_id || !isfinite(s->arrival_sec) ||
            !isfinite(s->service_start_sec) || !isfinite(s->departure_sec) ||
            s->arrival_sec != actual->arrival || s->service_start_sec != actual->service_start ||
            s->departure_sec != actual->depart ||
            s->base_service_sec != ctx->tasks[actual->task_id].service_seconds || s->extra_moves ||
            s->extra_service_sec || s->extra_fee_units)
            return SG_STATUS_INVALID_ARG;
    }
    return SG_STATUS_OK;
}
static SGStatus copy_evidence(SGLoadingV2RoundEvidence *out,
                              const SGLoadingV2RoundEvidence *source) {
    *out = *source;
    out->placements = NULL;
    out->operations = NULL;
    out->axle_states = NULL;
    out->stops = NULL;
#define COPY_ARRAY(field, count)                                                                   \
    do {                                                                                           \
        if (source->count) {                                                                       \
            void *copy = malloc((size_t)source->count * sizeof(*source->field));                   \
            if (!copy)                                                                             \
                return SG_STATUS_OUT_OF_MEMORY;                                                    \
            memcpy(copy, source->field, (size_t)source->count * sizeof(*source->field));           \
            out->field = copy;                                                                     \
        }                                                                                          \
    } while (0)
    COPY_ARRAY(placements, placement_count);
    COPY_ARRAY(operations, operation_count);
    COPY_ARRAY(axle_states, axle_state_count);
    COPY_ARRAY(stops, stop_count);
#undef COPY_ARRAY
    return SG_STATUS_OK;
}
static SGStatus plan_shape(SGContext *ctx, uint32_t count, const SGPlanRoute *routes,
                           const uint8_t *const *flags) {
    if (count > ctx->num_vehicles || (count && !routes))
        return SG_STATUS_INVALID_ARG;
    uint32_t total_tasks = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (routes[i].vehicle_id >= ctx->num_vehicles || routes[i].task_count > 100000 ||
            (routes[i].task_count && !routes[i].task_ids))
            return SG_STATUS_INVALID_ARG;
        if (routes[i].task_count > ctx->num_tasks - total_tasks)
            return SG_STATUS_INVALID_ARG;
        total_tasks += routes[i].task_count;
        for (uint32_t j = 0; j < i; j++)
            if (routes[j].vehicle_id == routes[i].vehicle_id)
                return SG_STATUS_INVALID_ARG;
        if (flags && flags[i])
            for (uint32_t j = 0; j < routes[i].task_count; j++)
                if (flags[i][j] > 1 || (!j && flags[i][j]) ||
                    (flags[i][j] && !ctx->vehicles[routes[i].vehicle_id].has_multi_trip))
                    return SG_STATUS_INVALID_ARG;
    }
    return SG_STATUS_OK;
}
SGStatus
sg_loading_v2_validate_plan(SGContext *ctx, uint32_t route_count, const SGPlanRoute *routes,
                            const uint8_t *const *trip_starts, SGLoadingV2PlanMode mode,
                            const SGLoadingV2ReportInfo *submitted_info, uint32_t witness_count,
                            const SGLoadingV2RoundEvidence *witnesses, SGLoadingV2Report **out) {
    if (!out)
        return SG_STATUS_INVALID_ARG;
    *out = NULL;
    if (!enabled(ctx) ||
        (mode != SG_LOADING_V2_FIND_WITNESSES && mode != SG_LOADING_V2_REPLAY_WITNESSES) ||
        (mode == SG_LOADING_V2_FIND_WITNESSES && (submitted_info || witness_count || witnesses)) ||
        (mode == SG_LOADING_V2_REPLAY_WITNESSES &&
         (!valid_info(ctx, submitted_info) || (witness_count && !witnesses))) ||
        plan_shape(ctx, route_count, routes, trip_starts) != SG_STATUS_OK)
        return failure(ctx, SG_STATUS_INVALID_ARG, SG_LOADING_V2_INVALID_INPUT, "/plan",
                       "Invalid v2 fixed-plan validation input.");
    SGContext shadow;
    SGStatus status = shadow_init(ctx, &shadow);
    if (status != SG_STATUS_OK)
        return status;
    status = sg_validate_plan_impl_with_trips(&shadow, route_count, routes, trip_starts);
    SGLoadingV2Report *report = NULL;
    if (status == SG_STATUS_OK && shadow.num_violations)
        status = SG_STATUS_INFEASIBLE;
    if (status == SG_STATUS_OK) {
        uint32_t rounds = 0;
        for (uint32_t v = 0; v < shadow.num_vehicles; v++) {
            uint32_t n = shadow.final_solution->route_stop_lengths[v];
            if (!n)
                continue;
            rounds++;
            const SGRouteStop *stops = sg_route_vehicle_stop_ptr_const(shadow.final_solution, v);
            for (uint32_t i = 1; i < n; i++)
                if (stops[i].trip_start)
                    rounds++;
        }
        if (mode == SG_LOADING_V2_REPLAY_WITNESSES) {
            if (witness_count != rounds)
                status = SG_STATUS_INVALID_ARG;
            if (status == SG_STATUS_OK) {
                report = new_report(&shadow, rounds);
                if (!report)
                    status = SG_STATUS_OUT_OF_MEMORY;
            }
            uint8_t *used = witness_count ? calloc(witness_count, 1) : NULL;
            if (witness_count && !used)
                status = SG_STATUS_OUT_OF_MEMORY;
            uint32_t index = 0;
            for (uint32_t v = 0; status == SG_STATUS_OK && v < shadow.num_vehicles; v++) {
                uint32_t n = shadow.final_solution->route_stop_lengths[v];
                if (!n)
                    continue;
                const SGRouteStop *stops =
                    sg_route_vehicle_stop_ptr_const(shadow.final_solution, v);
                uint32_t first = 0, trip = 0;
                while (first < n && status == SG_STATUS_OK) {
                    uint32_t end = first + 1;
                    while (end < n && !stops[end].trip_start)
                        end++;
                    uint32_t found = UINT32_MAX;
                    for (uint32_t k = 0; k < witness_count; k++)
                        if (!used[k] && witnesses[k].vehicle_id == v &&
                            witnesses[k].trip_index == trip) {
                            found = k;
                            break;
                        }
                    if (found == UINT32_MAX) {
                        status = SG_STATUS_INVALID_ARG;
                        break;
                    }
                    used[found] = 1;
                    status =
                        replay_physical(&shadow, v, stops + first, end - first, &witnesses[found]);
                    if (status == SG_STATUS_OK)
                        status = copy_evidence(&report->rounds[index++], &witnesses[found]);
                    first = end;
                    trip++;
                }
            }
            free(used);
            if (status == SG_STATUS_OK) {
                report->has_stats = 1;
                report->stats = shadow.stats;
                status = report_diagnostics(&shadow, report);
            }
        } else {
            status = solution_report_build(&shadow, &report);
            if (status == SG_STATUS_OK)
                for (uint32_t i = 0; i < report->round_count; i++) {
                    if (report->rounds[i].outcome == SG_LOADING_UNKNOWN) {
                        status = SG_STATUS_LIMIT;
                        break;
                    }
                    if (report->rounds[i].outcome != SG_LOADING_FEASIBLE)
                        status = SG_STATUS_INFEASIBLE;
                }
        }
    }
    if (sg_cancelled(ctx))
        status = SG_STATUS_LIMIT;
    if (status == SG_STATUS_OK && !retain_report(report))
        status = SG_STATUS_ERROR;
    if (status == SG_STATUS_OK) {
        sg_loading_v2_report_invalidate(ctx);
        sg_route_solution_free(ctx->final_solution, NULL);
        ctx->final_solution = shadow.final_solution;
        shadow.final_solution = NULL;
        ctx->stats = shadow.stats;
        free(ctx->violations);
        ctx->violations = shadow.violations;
        ctx->num_violations = shadow.num_violations;
        ctx->violations_capacity = shadow.violations_capacity;
        shadow.violations = NULL;
        ctx->loading_v2_committed_report = report;
        *out = report;
        clear_error(ctx);
    } else {
        sg_loading_v2_report_free(report);
        failure(ctx, status,
                status == SG_STATUS_LIMIT           ? SG_LOADING_V2_VALIDATION_UNKNOWN
                : status == SG_STATUS_OUT_OF_MEMORY ? SG_LOADING_V2_ALLOCATION_FAILED
                : status == SG_STATUS_INVALID_ARG   ? SG_LOADING_V2_INVALID_WITNESS
                : status == SG_STATUS_ERROR         ? SG_LOADING_V2_INTERNAL_ERROR
                                                    : SG_LOADING_V2_WITNESS_REJECTED,
                mode == SG_LOADING_V2_REPLAY_WITNESSES ? "/loading_witnesses" : "/plan",
                "Fixed plan or submitted loading evidence was not accepted.");
    }
    shadow_free(&shadow, ctx);
    return status;
}

static SGStatus round_stops(const SGContext *ctx, const SGLoadingV2Round *round, SGRouteStop **out,
                            uint8_t **selected) {
    *out = NULL;
    *selected = NULL;
    if (!round || round->vehicle_id >= ctx->num_vehicles || round->task_count > 100000 ||
        (round->task_count && !round->task_ids) || !isfinite(round->departure_sec) ||
        round->departure_sec < 0 || round->departure_sec > 1e12)
        return SG_STATUS_INVALID_ARG;
    uint32_t *map = ctx->num_tasks ? malloc((size_t)ctx->num_tasks * sizeof(*map)) : NULL;
    SGRouteStop *stops = round->task_count ? calloc(round->task_count, sizeof(*stops)) : NULL;
    uint8_t *used = ctx->num_requests ? calloc(ctx->num_requests, 1) : NULL;
    if ((ctx->num_tasks && !map) || (round->task_count && !stops) || (ctx->num_requests && !used)) {
        free(map);
        free(stops);
        free(used);
        return SG_STATUS_OUT_OF_MEMORY;
    }
    for (uint32_t i = 0; i < ctx->num_tasks; i++)
        map[i] = UINT32_MAX;
    for (uint32_t i = 0; i < ctx->num_requests; i++)
        if (ctx->requests[i].delivery_task_id < ctx->num_tasks)
            map[ctx->requests[i].delivery_task_id] = i;
    for (uint32_t i = 0; i < round->task_count; i++) {
        uint32_t task = round->task_ids[i];
        if (task >= ctx->num_tasks || map[task] == UINT32_MAX || used[map[task]]) {
            free(map);
            free(stops);
            free(used);
            return SG_STATUS_INVALID_ARG;
        }
        stops[i].task_id = task;
        stops[i].request_id = map[task];
        used[map[task]] = 1;
    }
    free(map);
    *out = stops;
    *selected = used;
    return SG_STATUS_OK;
}
static SGStatus round_shadow(SGContext *ctx, const SGLoadingV2Round *round, SGContext *shadow) {
    SGStatus status = shadow_init(ctx, shadow);
    if (status != SG_STATUS_OK)
        return status;
    SGRouteStop *unused = NULL;
    uint8_t *selected = NULL;
    status = round_stops(ctx, round, &unused, &selected);
    free(unused);
    if (status != SG_STATUS_OK) {
        shadow_free(shadow, ctx);
        return status;
    }
    if (ctx->request_locks && ctx->num_requests) {
        shadow->request_locks = malloc(ctx->num_requests);
        if (!shadow->request_locks) {
            shadow->request_locks = ctx->request_locks;
            free(selected);
            shadow_free(shadow, ctx);
            return SG_STATUS_OUT_OF_MEMORY;
        }
        for (uint32_t i = 0; i < ctx->num_requests; i++)
            shadow->request_locks[i] = selected[i] ? ctx->request_locks[i] : SG_LOCK_NONE;
    }
    free(selected);
    shadow->loading_v2_has_departure = 1;
    shadow->loading_v2_departure_vehicle = round->vehicle_id;
    shadow->loading_v2_departure = round->departure_sec;
    double earliest = ctx->vehicles[round->vehicle_id].has_shift_time_window
                          ? ctx->vehicles[round->vehicle_id].shift_early
                          : 0;
    const SGDepotRecord *depot = &ctx->depots[ctx->vehicles[round->vehicle_id].start_depot_id];
    if (depot->has_time_window && earliest < depot->tw_early)
        earliest = depot->tw_early;
    if (!sg_loading_v2_apply_departure(shadow, round->vehicle_id, &earliest)) {
        shadow_free(shadow, ctx);
        return SG_STATUS_INFEASIBLE;
    }
    return SG_STATUS_OK;
}
static SGStatus ordinary_round(SGContext *shadow, const SGLoadingV2Round *round) {
    SGPlanRoute route = {round->vehicle_id, round->task_ids, round->task_count};
    SGStatus status = sg_validate_plan_impl_with_trips(shadow, 1, &route, NULL);
    if (status == SG_STATUS_OK && shadow->num_violations)
        return SG_STATUS_INFEASIBLE;
    return status;
}
SGStatus sg_loading_v2_check_round(SGContext *ctx, const SGLoadingV2Round *round,
                                   SGLoadingShouldStop should_stop, void *user_data,
                                   SGLoadingV2Report **out) {
    if (!out)
        return SG_STATUS_INVALID_ARG;
    *out = NULL;
    if (!enabled(ctx) || !round)
        return SG_STATUS_INVALID_ARG;
    SGContext shadow;
    SGStatus status = round_shadow(ctx, round, &shadow);
    if (status != SG_STATUS_OK)
        return failure(ctx, status,
                       status == SG_STATUS_OUT_OF_MEMORY ? SG_LOADING_V2_ALLOCATION_FAILED
                       : status == SG_STATUS_INFEASIBLE  ? SG_LOADING_V2_WITNESS_REJECTED
                                                         : SG_LOADING_V2_INVALID_INPUT,
                       "", "Invalid scoped round or departure.");
    shadow.loading_v2_stop = should_stop;
    shadow.loading_v2_stop_data = user_data;
    SGRouteStop *stops = NULL;
    uint8_t *selected = NULL;
    status = round_stops(ctx, round, &stops, &selected);
    free(selected);
    SGAxleLoadingResult physical = {0};
    physical.loading.outcome = SG_LOADING_UNKNOWN;
    if (status == SG_STATUS_OK)
        status = sg_loading_round_v2(&shadow, round->vehicle_id, stops, round->task_count, 0,
                                     &physical, NULL, NULL);
    SGLoadingV2Report *report = NULL;
    if (status == SG_STATUS_OK) {
        report = new_report(ctx, 1);
        if (!report)
            status = SG_STATUS_OUT_OF_MEMORY;
    }
    if (status == SG_STATUS_OK && physical.loading.outcome == SG_LOADING_FEASIBLE) {
        SGStatus ordinary = ordinary_round(&shadow, round);
        if (ordinary == SG_STATUS_INFEASIBLE) {
            physical.loading.outcome = SG_LOADING_UNKNOWN;
            physical.loading.reason = SG_LOADING_UNSUPPORTED;
            report->rounds[0].reason = SG_LOADING_V2_SCHEDULE_REJECTED;
        } else if (ordinary != SG_STATUS_OK)
            status = ordinary;
        if (status == SG_STATUS_OK && physical.loading.outcome == SG_LOADING_FEASIBLE) {
            const SGRouteStop *actual =
                sg_route_vehicle_stop_ptr_const(shadow.final_solution, round->vehicle_id);
            status = report_round(&shadow, round->vehicle_id, actual, round->task_count, 0,
                                  &report->rounds[0]);
            report->rounds[0].trip_index = round->trip_index;
        }
    }
    if (status == SG_STATUS_OK && physical.loading.outcome != SG_LOADING_FEASIBLE) {
        SGLoadingV2RoundEvidence *r = &report->rounds[0];
        r->vehicle_id = round->vehicle_id;
        r->trip_index = round->trip_index;
        r->outcome = physical.loading.outcome;
        if (r->reason != SG_LOADING_V2_SCHEDULE_REJECTED)
            r->reason = v2_reason(physical.loading.reason);
        r->nodes = (uint32_t)physical.loading.nodes;
    }
    if (status == SG_STATUS_OK && v2_cancel(&shadow)) {
        sg_loading_v2_report_free(report);
        report = new_report(ctx, 1);
        if (!report)
            status = SG_STATUS_OUT_OF_MEMORY;
        else {
            report->rounds[0].vehicle_id = round->vehicle_id;
            report->rounds[0].trip_index = round->trip_index;
            report->rounds[0].outcome = SG_LOADING_UNKNOWN;
            report->rounds[0].reason = SG_LOADING_V2_INTERRUPTED;
        }
    }
    free(stops);
    shadow_free(&shadow, ctx);
    if (status != SG_STATUS_OK) {
        sg_loading_v2_report_free(report);
        return failure(ctx, status,
                       status == SG_STATUS_OUT_OF_MEMORY ? SG_LOADING_V2_ALLOCATION_FAILED
                                                         : SG_LOADING_V2_INTERNAL_ERROR,
                       "", "Scoped loading check failed.");
    }
    *out = report;
    clear_error(ctx);
    return SG_STATUS_OK;
}
SGStatus sg_loading_v2_validate_round_witness(SGContext *ctx, const SGLoadingV2Round *round,
                                              const SGLoadingV2ReportInfo *submitted_info,
                                              const SGLoadingV2RoundEvidence *witness) {
    if (!enabled(ctx) || !round || !witness || !valid_info(ctx, submitted_info) ||
        witness->vehicle_id != round->vehicle_id || witness->trip_index != round->trip_index)
        return failure(ctx, SG_STATUS_INVALID_ARG, SG_LOADING_V2_INVALID_WITNESS, "",
                       "Round/evidence identities do not match.");
    SGContext shadow;
    SGStatus status = round_shadow(ctx, round, &shadow);
    if (status != SG_STATUS_OK)
        return failure(ctx, status,
                       status == SG_STATUS_OUT_OF_MEMORY ? SG_LOADING_V2_ALLOCATION_FAILED
                                                         : SG_LOADING_V2_INVALID_INPUT,
                       "", "Invalid scoped round.");
    status = ordinary_round(&shadow, round);
    if (status == SG_STATUS_OK) {
        const SGRouteStop *actual =
            sg_route_vehicle_stop_ptr_const(shadow.final_solution, round->vehicle_id);
        status = replay_physical(&shadow, round->vehicle_id, actual, round->task_count, witness);
    }
    if (sg_cancelled(ctx))
        status = SG_STATUS_LIMIT;
    shadow_free(&shadow, ctx);
    if (status != SG_STATUS_OK)
        return failure(ctx, status,
                       status == SG_STATUS_LIMIT           ? SG_LOADING_V2_VALIDATION_UNKNOWN
                       : status == SG_STATUS_OUT_OF_MEMORY ? SG_LOADING_V2_ALLOCATION_FAILED
                       : status == SG_STATUS_INVALID_ARG   ? SG_LOADING_V2_INVALID_WITNESS
                       : status == SG_STATUS_ERROR         ? SG_LOADING_V2_INTERNAL_ERROR
                                                           : SG_LOADING_V2_WITNESS_REJECTED,
                       "", "Round witness was not accepted.");
    clear_error(ctx);
    return SG_STATUS_OK;
}
