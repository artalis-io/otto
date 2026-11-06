#include "sg_internal.h"
#include "sg_loading_json.h"

/* JSON numbers must be exact nonnegative integers. Never narrow defaults,
 * fractions, strings, NaN or an out-of-range double to an integer. */
static int number(const ShJsonValue *value, uint64_t max, uint64_t *out) {
    if (!value || sh_json_type(value) != SH_JSON_NUMBER)
        return 0;
    double d = sh_json_as_double(value, -1);
    if (!isfinite(d) || d < 0 || d > (double)max || floor(d) != d)
        return 0;
    *out = (uint64_t)d;
    return 1;
}
static int field(const ShJsonValue *object, const char *key, uint64_t max, uint64_t *out) {
    return object && sh_json_type(object) == SH_JSON_OBJECT &&
           number(sh_json_get(object, key), max, out);
}
#define SAFE_JSON_INTEGER UINT64_C(9007199254740991)
#define READ32(obj, key, target)                                                                   \
    do {                                                                                           \
        uint64_t val_;                                                                             \
        if (!field(obj, key, UINT32_MAX, &val_))                                                   \
            return 0;                                                                              \
        target = (uint32_t)val_;                                                                   \
    } while (0)
#define READ64(obj, key, target)                                                                   \
    do {                                                                                           \
        if (!field(obj, key, SAFE_JSON_INTEGER, &target))                                          \
            return 0;                                                                              \
    } while (0)

static int geometry(const ShJsonValue *object, SGLoadingVehicle *v) {
    memset(v, 0, sizeof(*v));
    READ32(object, "max_layers", v->max_layers);
    READ32(object, "clearance_mm", v->clearance_mm);
    READ64(object, "max_stack_weight_g", v->max_stack_weight_g);
    READ64(object, "payload_g", v->payload_g);
    const char *access = sh_json_as_string(sh_json_get(object, "access_model"), NULL);
    if (!access)
        return 0;
    if (!strcmp(access, "side_free"))
        v->access = SG_LOADING_SIDE_FREE;
    else if (!strcmp(access, "rear_lifo"))
        v->access = SG_LOADING_REAR_LIFO;
    else
        return 0;
    const ShJsonValue *slots = sh_json_get(object, "slots"),
                      *rules = sh_json_get(object, "allowed_top");
    if (!slots || sh_json_type(slots) != SH_JSON_ARRAY || !rules ||
        sh_json_type(rules) != SH_JSON_ARRAY)
        return 0;
    size_t n = sh_json_array_len(slots), classes = sh_json_array_len(rules);
    if (!n || n > SG_LOADING_MAX_SLOTS || !classes || classes > SG_LOADING_MAX_CLASSES)
        return 0;
    v->slot_count = (uint32_t)n;
    v->class_count = (uint32_t)classes;
    for (uint32_t i = 0; i < v->slot_count; ++i) {
        const ShJsonValue *slot = sh_json_array_get(slots, i);
        READ32(slot, "lane", v->lane[i]);
        READ32(slot, "depth", v->depth[i]);
    }
    for (uint32_t i = 0; i < v->class_count; ++i) {
        const ShJsonValue *tops = sh_json_array_get(rules, i);
        if (!tops || sh_json_type(tops) != SH_JSON_ARRAY || sh_json_array_len(tops) > classes)
            return 0;
        for (size_t j = 0; j < sh_json_array_len(tops); ++j) {
            uint64_t cls;
            if (!number(sh_json_array_get(tops, j), classes - 1, &cls) ||
                (v->allowed_top[i] & (UINT64_C(1) << cls)))
                return 0;
            v->allowed_top[i] |= UINT64_C(1) << cls;
        }
    }
    return 1;
}

static int unit(const ShJsonValue *object, SGLoadingUnit *u) {
    memset(u, 0, sizeof(*u));
    READ32(object, "height_mm", u->height_mm);
    READ32(object, "stack_class", u->stack_class);
    READ32(object, "max_units_above", u->max_units_above);
    READ64(object, "weight_g", u->weight_g);
    READ64(object, "max_weight_above_g", u->max_weight_above_g);
    return 1;
}

SGStatus sg_loading_build_json(SGContext *ctx, const ShJsonValue *value) {
    if (!value)
        return SG_STATUS_OK;
    uint64_t version, budget;
    const char *mode = sh_json_as_string(sh_json_get(value, "mode"), NULL);
    if (!field(value, "version", 1, &version) || version != 1 || !mode || strcmp(mode, "hard") ||
        !field(value, "node_budget", 100000, &budget))
        goto invalid;
    const ShJsonValue *vehicles = sh_json_get(value, "vehicles"),
                      *requests = sh_json_get(value, "requests");
    if (!vehicles || sh_json_type(vehicles) != SH_JSON_ARRAY || !requests ||
        sh_json_type(requests) != SH_JSON_ARRAY ||
        sh_json_array_len(vehicles) != ctx->num_vehicles ||
        sh_json_array_len(requests) != ctx->num_requests)
        goto invalid;
    uint8_t *seen_v = calloc(ctx->num_vehicles ? ctx->num_vehicles : 1, 1);
    uint8_t *seen_r = calloc(ctx->num_requests ? ctx->num_requests : 1, 1);
    if (!seen_v || !seen_r) {
        free(seen_v);
        free(seen_r);
        return SG_STATUS_OUT_OF_MEMORY;
    }
    int ok = 1;
    for (uint32_t i = 0; i < ctx->num_vehicles && ok; ++i) {
        const ShJsonValue *obj = sh_json_array_get(vehicles, i);
        uint64_t id;
        SGLoadingVehicle v;
        if (!field(obj, "vehicle_id", UINT32_MAX, &id) || id >= ctx->num_vehicles || seen_v[id] ||
            !geometry(obj, &v) || sg_vehicle_set_loading(ctx, (uint32_t)id, &v) != SG_STATUS_OK) {
            ok = 0;
            break;
        }
        seen_v[id] = 1;
    }
    for (uint32_t i = 0; i < ctx->num_requests && ok; ++i) {
        const ShJsonValue *obj = sh_json_array_get(requests, i);
        uint64_t id;
        const ShJsonValue *list = sh_json_get(obj, "units");
        if (!field(obj, "request_id", UINT32_MAX, &id) || id >= ctx->num_requests || seen_r[id] ||
            !list || sh_json_type(list) != SH_JSON_ARRAY ||
            sh_json_array_len(list) > SG_LOADING_MAX_UNITS) {
            ok = 0;
            break;
        }
        SGLoadingUnit units[SG_LOADING_MAX_UNITS];
        uint32_t n = (uint32_t)sh_json_array_len(list);
        for (uint32_t j = 0; j < n; ++j)
            if (!unit(sh_json_array_get(list, j), &units[j])) {
                ok = 0;
                break;
            }
        if (ok && sg_request_set_loading(ctx, (uint32_t)id, units, n) != SG_STATUS_OK)
            ok = 0;
        seen_r[id] = 1;
    }
    free(seen_v);
    free(seen_r);
    if (!ok)
        goto invalid;
    if (sg_set_hard_loading(ctx, 1, budget) != SG_STATUS_OK)
        goto invalid;
    return sg_loading_model_validate(ctx);
invalid:
    sg_set_error(ctx, "loading v1: invalid, incomplete or unsupported hard-loading input");
    return SG_STATUS_INVALID_ARG;
}

static const char *outcome(SGLoadingOutcome value) {
    return value == SG_LOADING_FEASIBLE     ? "feasible"
           : value == SG_LOADING_INFEASIBLE ? "infeasible"
                                            : "unknown";
}
static const char *reason(SGLoadingReason value) {
    switch (value) {
    case SG_LOADING_WITNESS:
        return "witness";
    case SG_LOADING_CAPACITY:
        return "capacity_bound";
    case SG_LOADING_NO_ARRANGEMENT:
        return "exhaustive_no_arrangement";
    case SG_LOADING_BUDGET:
        return "node_budget";
    case SG_LOADING_INTERRUPTED:
        return "interrupted";
    default:
        return "unsupported_or_incomplete";
    }
}

void sg_loading_write_json(const SGContext *ctx, ShJsonWriter *w) {
    if (!ctx->hard_loading)
        return;
    sh_json_write_key(w, "loading");
    sh_json_write_object_start(w);
    sh_json_write_kv_int(w, "version", 1);
    sh_json_write_kv_string(w, "mode", "hard");
    sh_json_write_kv_int(w, "node_budget", (int64_t)ctx->loading_node_budget);
    sh_json_write_key(w, "rounds");
    sh_json_write_array_start(w);
    for (uint32_t route = 0; route < sg_solution_get_route_count(ctx); ++route) {
        uint32_t v = sg_solution_get_route_vehicle_id(ctx, route);
        const SGRouteSolution *sol = ctx->final_solution;
        const SGRouteStop *stops = sg_route_vehicle_stop_ptr_const(sol, v);
        uint32_t trips = 1;
        for (uint32_t i = 1; i < sol->route_stop_lengths[v]; ++i)
            if (stops[i].trip_start)
                ++trips;
        for (uint32_t trip = 0; trip < trips; ++trip) {
            SGLoadingResult r;
            uint32_t requests[SG_LOADING_MAX_UNITS], indices[SG_LOADING_MAX_UNITS];
            SGStatus status = sg_solution_get_loading(ctx, route, trip, &r, requests, indices);
            if (status != SG_STATUS_OK) {
                r.outcome = SG_LOADING_UNKNOWN;
                r.reason = SG_LOADING_UNSUPPORTED;
                r.count = 0;
                r.nodes = 0;
            }
            sh_json_write_object_start(w);
            sh_json_write_kv_int(w, "vehicle_id", v);
            sh_json_write_kv_int(w, "trip_index", trip);
            sh_json_write_kv_string(w, "outcome", outcome(r.outcome));
            sh_json_write_kv_string(w, "reason", reason(r.reason));
            sh_json_write_kv_int(w, "nodes", (int64_t)r.nodes);
            sh_json_write_key(w, "placements");
            sh_json_write_array_start(w);
            if (r.outcome == SG_LOADING_FEASIBLE)
                for (uint32_t i = 0; i < r.count; ++i) {
                    sh_json_write_object_start(w);
                    sh_json_write_kv_int(w, "request_id", requests[i]);
                    sh_json_write_kv_int(w, "unit_index", indices[i]);
                    sh_json_write_kv_int(w, "slot", r.placements[i].slot);
                    sh_json_write_kv_int(w, "layer", r.placements[i].layer);
                    sh_json_write_object_end(w);
                }
            sh_json_write_array_end(w);
            sh_json_write_object_end(w);
        }
    }
    sh_json_write_array_end(w);
    /* Singleton diagnostics are evidence about loading alone, NOT attribution
     * of unassignment to loading or proof of insertability into current routes. */
    sh_json_write_key(w, "unassigned_checks");
    sh_json_write_array_start(w);
    uint64_t budget = ctx->loading_node_budget < 128 ? ctx->loading_node_budget : 128;
    for (uint32_t id = 0; id < ctx->num_requests; ++id) {
        if (ctx->final_solution && ctx->final_solution->base.assigned_flags[id])
            continue;
        SGLoadingUnit units[SG_LOADING_MAX_UNITS];
        uint32_t n = ctx->requests[id].loading_count;
        if (n)
            memcpy(units, ctx->requests[id].loading_units, n * sizeof(*units));
        for (uint32_t i = 0; i < n; ++i)
            units[i].stop = 0;
        uint32_t feasible = 0, infeasible = 0, unknown = 0;
        for (uint32_t v = 0; v < ctx->num_vehicles; ++v) {
            if (!n) {
                ++feasible;
                continue;
            }
            SGLoadingResult r;
            if (sg_loading_check(ctx->vehicles[v].loading, units, n, budget, &r) != SG_STATUS_OK ||
                r.outcome == SG_LOADING_UNKNOWN)
                ++unknown;
            else if (r.outcome == SG_LOADING_FEASIBLE)
                ++feasible;
            else
                ++infeasible;
        }
        sh_json_write_object_start(w);
        sh_json_write_kv_int(w, "request_id", id);
        sh_json_write_kv_string(w, "scope", "loading_alone_not_route_insertability");
        sh_json_write_kv_int(w, "diagnostic_node_budget", (int64_t)budget);
        sh_json_write_kv_int(w, "feasible_vehicles", feasible);
        sh_json_write_kv_int(w, "infeasible_vehicles", infeasible);
        sh_json_write_kv_int(w, "unknown_vehicles", unknown);
        sh_json_write_object_end(w);
    }
    sh_json_write_array_end(w);
    sh_json_write_object_end(w);
}
