#include "sg_loading_json.h"
#include "sg_loading_v2_internal.h"
#include <inttypes.h>
#include <stdio.h>

/* Strict, version-scoped decoding: V1 intentionally keeps its existing parser.
 */
static int keys(const ShJsonValue *o, const char *allowed) {
    if (!o || o->type != SH_JSON_OBJECT)
        return 0;
    for (size_t i = 0; i < o->u.object_val.count; i++) {
        const ShJsonMember *m = &o->u.object_val.members[i];
        if (strlen(m->key) != m->key_len)
            return 0;
        for (size_t j = 0; j < i; j++)
            if (!strcmp(m->key, o->u.object_val.members[j].key))
                return 0;
        const char *p = allowed;
        int found = 0;
        while (*p) {
            const char *end = strchr(p, '|');
            size_t n = end ? (size_t)(end - p) : strlen(p);
            if (n == m->key_len && !memcmp(p, m->key, n))
                found = 1;
            if (!end)
                break;
            p = end + 1;
        }
        if (!found)
            return 0;
    }
    return 1;
}
static int integer(const ShJsonValue *v, int64_t min, int64_t max, int64_t *out) {
    if (!v || v->type != SH_JSON_NUMBER || !isfinite(v->u.num_val) || v->u.num_val < min ||
        v->u.num_val > max || floor(v->u.num_val) != v->u.num_val)
        return 0;
    *out = (int64_t)v->u.num_val;
    return 1;
}
static const char *string(const ShJsonValue *v) {
    return v && v->type == SH_JSON_STRING && strlen(v->u.string_val.str) == v->u.string_val.len
               ? v->u.string_val.str
               : NULL;
}
static int array(const ShJsonValue *v, size_t max) {
    return v && v->type == SH_JSON_ARRAY && sh_json_array_len(v) <= max;
}
#define GET(o, k, t, lo, hi)                                                                       \
    do {                                                                                           \
        int64_t n_;                                                                                \
        if (!integer(sh_json_get(o, k), lo, hi, &n_))                                              \
            return 0;                                                                              \
        t = n_;                                                                                    \
    } while (0)
static int capabilities(const ShJsonValue *v, uint32_t *caps) {
    if (!array(v, 3))
        return 0;
    *caps = 0;
    for (size_t i = 0; i < sh_json_array_len(v); i++) {
        const char *name = string(sh_json_array_get(v, i));
        uint32_t bit = 0;
        if (!name)
            return 0;
        if (!strcmp(name, "stacking"))
            bit = 1;
        else if (!strcmp(name, "axle_loads"))
            bit = 2;
        else if (!strcmp(name, "bounded_reshuffling"))
            bit = 4;
        if (!bit || (*caps & bit))
            return 0;
        *caps |= bit;
    }
    return (*caps & 1) != 0;
}
static int mass(const ShJsonValue *o, SGLoadingV2MassInterval *v) {
    if (!keys(o, "min|max"))
        return 0;
    GET(o, "min", v->min, 0, 1000000000);
    GET(o, "max", v->max, 0, 1000000000);
    return v->min <= v->max;
}
static int signed_interval(const ShJsonValue *o, SGLoadingV2SignedInterval *v) {
    if (!keys(o, "min|max"))
        return 0;
    GET(o, "min", v->min, -1000000, 1000000);
    GET(o, "max", v->max, -1000000, 1000000);
    return v->min <= v->max;
}
static int geometry(const ShJsonValue *o, SGLoadingVehicle *v) {
    memset(v, 0, sizeof(*v));
    GET(o, "max_layers", v->max_layers, 1, 8);
    GET(o, "clearance_mm", v->clearance_mm, 0, UINT32_MAX);
    GET(o, "max_stack_weight_g", v->max_stack_weight_g, 0, 1000000000);
    GET(o, "payload_g", v->payload_g, 0, 1000000000);
    const char *access = string(sh_json_get(o, "access_model"));
    if (!access)
        return 0;
    if (!strcmp(access, "side_free"))
        v->access = SG_LOADING_SIDE_FREE;
    else if (!strcmp(access, "rear_lifo"))
        v->access = SG_LOADING_REAR_LIFO;
    else
        return 0;
    const ShJsonValue *slots = sh_json_get(o, "slots"), *rules = sh_json_get(o, "allowed_top");
    if (!array(slots, 128) || !array(rules, 64) || !sh_json_array_len(slots) ||
        !sh_json_array_len(rules))
        return 0;
    v->slot_count = (uint32_t)sh_json_array_len(slots);
    v->class_count = (uint32_t)sh_json_array_len(rules);
    for (uint32_t i = 0; i < v->slot_count; i++) {
        const ShJsonValue *slot = sh_json_array_get(slots, i);
        if (!keys(slot, "lane|depth"))
            return 0;
        GET(slot, "lane", v->lane[i], 0, UINT32_MAX);
        GET(slot, "depth", v->depth[i], 0, UINT32_MAX);
    }
    for (uint32_t i = 0; i < v->class_count; i++) {
        const ShJsonValue *list = sh_json_array_get(rules, i);
        if (!array(list, v->class_count))
            return 0;
        for (size_t j = 0; j < sh_json_array_len(list); j++) {
            int64_t cls;
            if (!integer(sh_json_array_get(list, j), 0, v->class_count - 1, &cls) ||
                (v->allowed_top[i] & (UINT64_C(1) << cls)))
                return 0;
            v->allowed_top[i] |= UINT64_C(1) << cls;
        }
    }
    return 1;
}
static int axle(const ShJsonValue *o, SGLoadingV2Axles *a, int32_t *centres,
                SGLoadingV2Fraction *fraction) {
    if (!keys(o, "model|profile_sha256|wheelbase_mm|footprint_length_mm|deck_min_"
                 "mm|deck_max_mm|slot_centres_mm|front_tare_g|rear_tare_g|front_"
                 "limits_g|rear_limits_g|max_gross_g|min_front_fraction"))
        return 0;
    const char *model = string(sh_json_get(o, "model"));
    if (!model || strcmp(model, "rigid_two_axle_v1"))
        return 0;
    memset(a, 0, sizeof(*a));
    a->model = SG_LOADING_V2_RIGID_TWO_AXLE;
    a->profile_sha256 = string(sh_json_get(o, "profile_sha256"));
    GET(o, "wheelbase_mm", a->wheelbase_mm, 1, 1000000);
    GET(o, "footprint_length_mm", a->footprint_length_mm, 1, 1000000);
    GET(o, "deck_min_mm", a->deck_min_mm, -1000000, 1000000);
    GET(o, "deck_max_mm", a->deck_max_mm, -1000000, 1000000);
    GET(o, "max_gross_g", a->max_gross_g, 0, 1000000000);
    const ShJsonValue *slots = sh_json_get(o, "slot_centres_mm");
    if (!array(slots, 128) || !sh_json_array_len(slots))
        return 0;
    a->slot_count = (uint32_t)sh_json_array_len(slots);
    a->slot_centres_mm = centres;
    for (uint32_t i = 0; i < a->slot_count; i++) {
        int64_t n;
        if (!integer(sh_json_array_get(slots, i), -1000000, 1000000, &n))
            return 0;
        centres[i] = (int32_t)n;
    }
    if (!mass(sh_json_get(o, "front_tare_g"), &a->front_tare_g) ||
        !mass(sh_json_get(o, "rear_tare_g"), &a->rear_tare_g) ||
        !mass(sh_json_get(o, "front_limits_g"), &a->front_limits_g) ||
        !mass(sh_json_get(o, "rear_limits_g"), &a->rear_limits_g))
        return 0;
    const ShJsonValue *f = sh_json_get(o, "min_front_fraction");
    if (!f)
        return 0;
    if (f->type != SH_JSON_NULL) {
        if (!keys(f, "numerator|denominator"))
            return 0;
        GET(f, "numerator", fraction->numerator, 0, 1000000);
        GET(f, "denominator", fraction->denominator, 1, 1000000);
        a->min_front_fraction = fraction;
    }
    return 1;
}
static int unit(const ShJsonValue *o, SGLoadingV2Unit *u, SGLoadingV2SignedInterval *cg,
                int axles) {
    if (!keys(o, axles ? "height_mm|weight_g|stack_class|max_weight_above_g|max_"
                         "units_above|cg_offset_mm"
                       : "height_mm|weight_g|stack_class|max_weight_above_g|max_"
                         "units_above"))
        return 0;
    memset(u, 0, sizeof(*u));
    GET(o, "height_mm", u->physical.height_mm, 1, UINT32_MAX);
    GET(o, "weight_g", u->physical.weight_g, 0, 1000000000);
    GET(o, "stack_class", u->physical.stack_class, 0, 63);
    GET(o, "max_weight_above_g", u->physical.max_weight_above_g, 0, 1000000000);
    GET(o, "max_units_above", u->physical.max_units_above, 0, 127);
    if (axles) {
        if (!signed_interval(sh_json_get(o, "cg_offset_mm"), cg))
            return 0;
        u->cg_offset_mm = cg;
    }
    return 1;
}
static SGStatus invalid(SGContext *ctx, const char *path) {
    sg_loading_v2_clear_error(ctx);
    ctx->loading_v2_error.code = SG_LOADING_V2_INVALID_INPUT;
    snprintf(ctx->loading_v2_error.path, sizeof(ctx->loading_v2_error.path), "%s", path);
    sg_set_error(ctx, "loading v2: malformed, duplicate, unknown or incomplete fields at %s", path);
    return SG_STATUS_INVALID_ARG;
}
SGStatus sg_loading_v2_build_json(SGContext *ctx, const ShJsonValue *o) {
    if (!keys(o, "version|mode|required_capabilities|node_budget|frontier_budget|"
                 "vehicles|requests"))
        return invalid(ctx, "/loading");
    SGLoadingV2Config config = {sizeof(config), 2, 0, 0, 0};
    int64_t n;
    const char *mode = string(sh_json_get(o, "mode"));
    if (!integer(sh_json_get(o, "version"), 2, 2, &n) || !mode || strcmp(mode, "hard") ||
        !capabilities(sh_json_get(o, "required_capabilities"), &config.required_capabilities) ||
        !integer(sh_json_get(o, "node_budget"), 0, 100000, &n))
        return invalid(ctx, "/loading");
    config.node_budget = (uint32_t)n;
    const ShJsonValue *frontier = sh_json_get(o, "frontier_budget");
    if (frontier) {
        if (!integer(frontier, 1, 4096, &n))
            return invalid(ctx, "/loading/frontier_budget");
        config.frontier_budget = (uint32_t)n;
    }
    SGStatus status = sg_loading_v2_set_config(ctx, &config);
    if (status != SG_STATUS_OK)
        return status;
    const ShJsonValue *vehicles = sh_json_get(o, "vehicles"),
                      *requests = sh_json_get(o, "requests");
    if (!array(vehicles, 100000) || !array(requests, 100000) ||
        sh_json_array_len(vehicles) != ctx->num_vehicles ||
        sh_json_array_len(requests) != ctx->num_requests)
        return invalid(ctx, "/loading");
    for (uint32_t i = 0; i < ctx->num_vehicles; i++) {
        const ShJsonValue *v = sh_json_array_get(vehicles, i);
        int64_t id;
        if (!keys(v, (config.required_capabilities & 2)
                         ? "vehicle_id|access_model|slots|max_layers|clearance_mm|"
                           "max_stack_weight_g|payload_g|allowed_top|axles"
                         : "vehicle_id|access_model|slots|max_layers|clearance_mm|"
                           "max_stack_weight_g|payload_g|allowed_top") ||
            !integer(sh_json_get(v, "vehicle_id"), 0, ctx->num_vehicles - 1, &id) ||
            ctx->vehicles[id].loading)
            return invalid(ctx, "/loading/vehicles");
        SGLoadingV2Vehicle value = {0};
        SGLoadingV2Axles a;
        int32_t centres[128];
        SGLoadingV2Fraction fraction;
        if (!geometry(v, &value.physical))
            return invalid(ctx, "/loading/vehicles");
        if (config.required_capabilities & 2) {
            if (!axle(sh_json_get(v, "axles"), &a, centres, &fraction))
                return invalid(ctx, "/loading/vehicles/axles");
            value.axles = &a;
        }
        status = sg_loading_v2_set_vehicle(ctx, (uint32_t)id, &value);
        if (status != SG_STATUS_OK)
            return status;
    }
    for (uint32_t i = 0; i < ctx->num_requests; i++) {
        const ShJsonValue *r = sh_json_array_get(requests, i), *list = sh_json_get(r, "units");
        int64_t id;
        if (!keys(r, "request_id|units") ||
            !integer(sh_json_get(r, "request_id"), 0, ctx->num_requests - 1, &id) ||
            ctx->requests[id].has_loading_units || !array(list, 128))
            return invalid(ctx, "/loading/requests");
        SGLoadingV2Unit units[128];
        SGLoadingV2SignedInterval cg[128];
        uint32_t count = (uint32_t)sh_json_array_len(list);
        for (uint32_t j = 0; j < count; j++)
            if (!unit(sh_json_array_get(list, j), &units[j], &cg[j],
                      !!(config.required_capabilities & 2)))
                return invalid(ctx, "/loading/requests/units");
        status = sg_loading_v2_set_request(ctx, (uint32_t)id, units, count);
        if (status != SG_STATUS_OK)
            return status;
    }
    return sg_loading_v2_model_validate(ctx);
}
static void write_caps(ShJsonWriter *w, const char *key, uint32_t caps) {
    sh_json_write_key(w, key);
    sh_json_write_array_start(w);
    if (caps & 1)
        sh_json_write_string(w, "stacking");
    if (caps & 2)
        sh_json_write_string(w, "axle_loads");
    if (caps & 4)
        sh_json_write_string(w, "bounded_reshuffling");
    sh_json_write_array_end(w);
}
static const char *reasons[] = {
    "invalid",         "witness",     "capacity_bound",   "exhaustive_no_witness", "node_budget",
    "frontier_budget", "interrupted", "unsupported_size", "schedule_rejected"};
static void rational(ShJsonWriter *w, const char *key, SGLoadingV2Rational r) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRId64, r.numerator);
    sh_json_write_key(w, key);
    sh_json_write_object_start(w);
    sh_json_write_kv_string(w, "numerator", buf);
    sh_json_write_kv_int(w, "denominator", r.denominator);
    sh_json_write_object_end(w);
}
void sg_loading_v2_write_report(ShJsonWriter *w, const SGLoadingV2Report *report) {
    SGLoadingV2ReportInfo info;
    uint32_t count, dc;
    const SGLoadingV2RoundEvidence *rounds;
    const SGLoadingV2Diagnostic *diagnostics;
    if (sg_loading_v2_report_info(report, &info) != SG_STATUS_OK ||
        sg_loading_v2_report_view(report, &count, &rounds, &dc, &diagnostics) != SG_STATUS_OK)
        return;
    sh_json_write_key(w, "loading");
    sh_json_write_object_start(w);
    sh_json_write_kv_int(w, "version", 2);
    sh_json_write_kv_string(w, "mode", "hard");
    write_caps(w, "required_capabilities", info.required_capabilities);
    sh_json_write_kv_int(w, "node_budget", info.node_budget);
    if (info.frontier_budget)
        sh_json_write_kv_int(w, "frontier_budget", info.frontier_budget);
    sh_json_write_key(w, "rounds");
    sh_json_write_array_start(w);
    for (uint32_t i = 0; i < count; i++) {
        const SGLoadingV2RoundEvidence *r = &rounds[i];
        sh_json_write_object_start(w);
        sh_json_write_kv_int(w, "vehicle_id", r->vehicle_id);
        sh_json_write_kv_int(w, "trip_index", r->trip_index);
        sh_json_write_kv_string(w, "outcome",
                                r->outcome == SG_LOADING_FEASIBLE     ? "feasible"
                                : r->outcome == SG_LOADING_INFEASIBLE ? "infeasible"
                                                                      : "unknown");
        sh_json_write_kv_string(w, "reason",
                                r->reason >= 1 && r->reason <= 8 ? reasons[r->reason]
                                                                 : "unsupported_size");
        sh_json_write_kv_int(w, "nodes", r->nodes);
        sh_json_write_key(w, "placements");
        sh_json_write_array_start(w);
        for (uint32_t j = 0; j < r->placement_count; j++) {
            const SGLoadingV2Placement *p = &r->placements[j];
            sh_json_write_object_start(w);
            sh_json_write_kv_int(w, "request_id", p->request_id);
            sh_json_write_kv_int(w, "unit_index", p->unit_index);
            sh_json_write_kv_int(w, "slot", p->slot);
            sh_json_write_kv_int(w, "layer", p->layer);
            sh_json_write_object_end(w);
        }
        sh_json_write_array_end(w);
        sh_json_write_key(w, "operations");
        sh_json_write_array_start(w);
        for (uint32_t j = 0; j < r->operation_count; j++) {
            const SGLoadingV2Operation *p = &r->operations[j];
            sh_json_write_object_start(w);
            sh_json_write_kv_string(w, "kind", p->kind == SG_LOADING_V2_LOAD ? "load" : "deliver");
            if (p->stop_index == UINT32_MAX)
                sh_json_write_kv_null(w, "stop_index");
            else
                sh_json_write_kv_int(w, "stop_index", p->stop_index);
            sh_json_write_kv_int(w, "request_id", p->request_id);
            sh_json_write_kv_int(w, "unit_index", p->unit_index);
            sh_json_write_kv_int(w, "slot", p->slot);
            sh_json_write_kv_int(w, "layer", p->layer);
            sh_json_write_object_end(w);
        }
        sh_json_write_array_end(w);
        sh_json_write_key(w, "axle_states");
        sh_json_write_array_start(w);
        for (uint32_t j = 0; j < r->axle_state_count; j++) {
            const SGLoadingV2AxleState *a = &r->axle_states[j];
            sh_json_write_object_start(w);
            sh_json_write_kv_int(w, "step", a->step);
            rational(w, "front_min_g", a->front_min_g);
            rational(w, "front_max_g", a->front_max_g);
            rational(w, "rear_min_g", a->rear_min_g);
            rational(w, "rear_max_g", a->rear_max_g);
            sh_json_write_kv_int(w, "gross_min_g", a->gross_min_g);
            sh_json_write_kv_int(w, "gross_max_g", a->gross_max_g);
            sh_json_write_object_end(w);
        }
        sh_json_write_array_end(w);
        sh_json_write_key(w, "stops");
        sh_json_write_array_start(w);
        for (uint32_t j = 0; j < r->stop_count; j++) {
            const SGLoadingV2StopEvidence *s = &r->stops[j];
            sh_json_write_object_start(w);
            sh_json_write_kv_int(w, "stop_index", s->stop_index);
            sh_json_write_kv_int(w, "task_id", s->task_id);
            sh_json_write_kv_double_fmt(w, "arrival_sec", s->arrival_sec, 17);
            sh_json_write_kv_double_fmt(w, "service_start_sec", s->service_start_sec, 17);
            sh_json_write_kv_double_fmt(w, "departure_sec", s->departure_sec, 17);
            sh_json_write_kv_double_fmt(w, "base_service_sec", s->base_service_sec, 17);
            sh_json_write_kv_int(w, "extra_service_sec", s->extra_service_sec);
            sh_json_write_kv_int(w, "extra_moves", s->extra_moves);
            sh_json_write_kv_int(w, "extra_fee_units", s->extra_fee_units);
            sh_json_write_object_end(w);
        }
        sh_json_write_array_end(w);
        sh_json_write_kv_int(w, "extra_moves", r->extra_moves);
        sh_json_write_kv_int(w, "extra_service_sec", r->extra_service_sec);
        sh_json_write_kv_int(w, "extra_fee_units", r->extra_fee_units);
        sh_json_write_object_end(w);
    }
    sh_json_write_array_end(w);
    sh_json_write_key(w, "unassigned_checks");
    sh_json_write_array_start(w);
    for (uint32_t i = 0; i < dc; i++) {
        const SGLoadingV2Diagnostic *d = &diagnostics[i];
        sh_json_write_object_start(w);
        sh_json_write_kv_int(w, "request_id", d->request_id);
        sh_json_write_kv_string(w, "scope", "physical_singleton_not_route_insertability");
        sh_json_write_kv_int(w, "diagnostic_node_budget", d->diagnostic_node_budget);
        sh_json_write_kv_int(w, "feasible_vehicles", d->feasible_vehicles);
        sh_json_write_kv_int(w, "infeasible_vehicles", d->infeasible_vehicles);
        sh_json_write_kv_int(w, "unknown_vehicles", d->unknown_vehicles);
        sh_json_write_object_end(w);
    }
    sh_json_write_array_end(w);
    sh_json_write_object_end(w);
}
void sg_loading_v2_write_capabilities(ShJsonWriter *w) {
    SGLoadingV2Capabilities c = {0};
    c.struct_size = sizeof(c);
    c.schema_version = 2;
    sg_loading_v2_capabilities(&c);
    sh_json_write_object_start(w);
    sh_json_write_kv_int(w, "schema_version", 2);
    write_caps(w, "implemented_capabilities", c.implemented_capabilities);
    sh_json_write_key(w, "axle_models");
    sh_json_write_array_start(w);
    sh_json_write_string(w, "rigid_two_axle_v1");
    sh_json_write_array_end(w);
    sh_json_write_key(w, "handling_models");
    sh_json_write_array_start(w);
    sh_json_write_array_end(w);
    sh_json_write_key(w, "limits");
    sh_json_write_object_start(w);
#define LIMIT(name) sh_json_write_kv_int(w, #name, c.name)
    LIMIT(max_units);
    LIMIT(max_slots);
    LIMIT(max_classes);
    LIMIT(max_layers);
    LIMIT(max_stops);
    LIMIT(max_nodes);
    LIMIT(max_frontier);
    LIMIT(max_staged);
    LIMIT(max_extra_moves_stop);
    LIMIT(max_extra_moves_round);
    LIMIT(max_operations);
    LIMIT(max_axle_states);
#undef LIMIT
    sh_json_write_object_end(w);
    sh_json_write_object_end(w);
}
/* Submitted evidence is bounded before allocation; no search repairs it. */
static int read_rational(const ShJsonValue *o, SGLoadingV2Rational *r) {
    if (!keys(o, "numerator|denominator"))
        return 0;
    const char *s = string(sh_json_get(o, "numerator"));
    if (!s || !*s)
        return 0;
    const char *p = s;
    if (*p == '-')
        p++;
    if (!*p || (*p == '0' && (p[1] || p != s)))
        return 0;
    uint64_t n = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9' || n > (UINT64_C(257000000000000000) - (*p - '0')) / 10)
            return 0;
        n = n * 10 + (*p - '0');
    }
    r->numerator = *s == '-' ? -(int64_t)n : (int64_t)n;
    GET(o, "denominator", r->denominator, 1, 1000000);
    return 1;
}
static int evidence(const ShJsonValue *root, SGLoadingV2RoundEvidence *r) {
    if (!keys(root, "vehicle_id|trip_index|outcome|reason|nodes|placements|operations|"
                    "axle_states|stops|extra_moves|extra_service_sec|extra_fee_units"))
        return 0;
    GET(root, "vehicle_id", r->vehicle_id, 0, UINT32_MAX);
    GET(root, "trip_index", r->trip_index, 0, UINT32_MAX);
    GET(root, "nodes", r->nodes, 0, 100000);
    GET(root, "extra_moves", r->extra_moves, 0, 0);
    GET(root, "extra_service_sec", r->extra_service_sec, 0, 0);
    GET(root, "extra_fee_units", r->extra_fee_units, 0, 0);
    const char *outcome = string(sh_json_get(root, "outcome")),
               *reason = string(sh_json_get(root, "reason"));
    if (!outcome || strcmp(outcome, "feasible") || !reason || strcmp(reason, "witness"))
        return 0;
    r->outcome = SG_LOADING_FEASIBLE;
    r->reason = SG_LOADING_V2_WITNESS;
    const ShJsonValue *ps = sh_json_get(root, "placements"), *ops = sh_json_get(root, "operations"),
                      *as = sh_json_get(root, "axle_states"), *ss = sh_json_get(root, "stops");
    if (!array(ps, 128) || !array(ops, 256) || !array(as, 257) || !array(ss, 100000))
        return 0;
    r->placement_count = (uint32_t)sh_json_array_len(ps);
    r->operation_count = (uint32_t)sh_json_array_len(ops);
    r->axle_state_count = (uint32_t)sh_json_array_len(as);
    r->stop_count = (uint32_t)sh_json_array_len(ss);
#define ALLOC(field, count)                                                                        \
    do {                                                                                           \
        if (r->count) {                                                                            \
            r->field = calloc(r->count, sizeof(*r->field));                                        \
            if (!r->field)                                                                         \
                return -1;                                                                         \
        }                                                                                          \
    } while (0)
    ALLOC(placements, placement_count);
    ALLOC(operations, operation_count);
    ALLOC(axle_states, axle_state_count);
    ALLOC(stops, stop_count);
#undef ALLOC
    for (uint32_t i = 0; i < r->placement_count; i++) {
        const ShJsonValue *o = sh_json_array_get(ps, i);
        SGLoadingV2Placement *p = (SGLoadingV2Placement *)&r->placements[i];
        if (!keys(o, "request_id|unit_index|slot|layer"))
            return 0;
        GET(o, "request_id", p->request_id, 0, UINT32_MAX);
        GET(o, "unit_index", p->unit_index, 0, 127);
        GET(o, "slot", p->slot, 0, 127);
        GET(o, "layer", p->layer, 0, 7);
    }
    for (uint32_t i = 0; i < r->operation_count; i++) {
        const ShJsonValue *o = sh_json_array_get(ops, i);
        SGLoadingV2Operation *p = (SGLoadingV2Operation *)&r->operations[i];
        if (!keys(o, "kind|stop_index|request_id|unit_index|slot|layer"))
            return 0;
        const char *kind = string(sh_json_get(o, "kind"));
        if (!kind)
            return 0;
        if (!strcmp(kind, "load"))
            p->kind = SG_LOADING_V2_LOAD;
        else if (!strcmp(kind, "deliver"))
            p->kind = SG_LOADING_V2_DELIVER;
        else
            return 0;
        const ShJsonValue *stop = sh_json_get(o, "stop_index");
        if (!stop)
            return 0;
        p->stop_index = UINT32_MAX;
        p->staging_position = UINT32_MAX;
        if (stop->type != SH_JSON_NULL) {
            GET(o, "stop_index", p->stop_index, 0, 99999);
        }
        GET(o, "request_id", p->request_id, 0, UINT32_MAX);
        GET(o, "unit_index", p->unit_index, 0, 127);
        GET(o, "slot", p->slot, 0, 127);
        GET(o, "layer", p->layer, 0, 7);
    }
    for (uint32_t i = 0; i < r->axle_state_count; i++) {
        const ShJsonValue *o = sh_json_array_get(as, i);
        SGLoadingV2AxleState *a = (SGLoadingV2AxleState *)&r->axle_states[i];
        if (!keys(o, "step|front_min_g|front_max_g|rear_min_g|rear_max_g|gross_min_"
                     "g|gross_max_g"))
            return 0;
        GET(o, "step", a->step, 0, 256);
        GET(o, "gross_min_g", a->gross_min_g, 0, 129000000000);
        GET(o, "gross_max_g", a->gross_max_g, 0, 129000000000);
        if (!read_rational(sh_json_get(o, "front_min_g"), &a->front_min_g) ||
            !read_rational(sh_json_get(o, "front_max_g"), &a->front_max_g) ||
            !read_rational(sh_json_get(o, "rear_min_g"), &a->rear_min_g) ||
            !read_rational(sh_json_get(o, "rear_max_g"), &a->rear_max_g))
            return 0;
    }
    for (uint32_t i = 0; i < r->stop_count; i++) {
        const ShJsonValue *o = sh_json_array_get(ss, i);
        SGLoadingV2StopEvidence *s = (SGLoadingV2StopEvidence *)&r->stops[i];
        if (!keys(o, "stop_index|task_id|arrival_sec|service_start_sec|departure_sec|"
                     "base_service_sec|extra_service_sec|extra_moves|extra_fee_units"))
            return 0;
        GET(o, "stop_index", s->stop_index, 0, 99999);
        GET(o, "task_id", s->task_id, 0, UINT32_MAX);
#define TIME(name)                                                                                 \
    do {                                                                                           \
        const ShJsonValue *v = sh_json_get(o, #name);                                              \
        if (!v || v->type != SH_JSON_NUMBER || !isfinite(v->u.num_val) || v->u.num_val < 0 ||      \
            v->u.num_val > 1e12)                                                                   \
            return 0;                                                                              \
        s->name = v->u.num_val;                                                                    \
    } while (0)
        TIME(arrival_sec);
        TIME(service_start_sec);
        TIME(departure_sec);
        TIME(base_service_sec);
#undef TIME
        GET(o, "extra_service_sec", s->extra_service_sec, 0, 0);
        GET(o, "extra_moves", s->extra_moves, 0, 0);
        GET(o, "extra_fee_units", s->extra_fee_units, 0, 0);
    }
    return 1;
}
SGStatus sg_loading_v2_validate_json(SGContext *ctx, const ShJsonValue *o, uint32_t count,
                                     const SGPlanRoute *routes, const uint8_t *const *flags) {
    SGLoadingV2Report *report = NULL;
    if (!o) {
        SGStatus status = sg_loading_v2_validate_plan(
            ctx, count, routes, flags, SG_LOADING_V2_FIND_WITNESSES, NULL, 0, NULL, &report);
        sg_loading_v2_report_free(report);
        return status;
    }
    if (!keys(o, "version|mode|required_capabilities|node_budget|rounds|"
                 "unassigned_checks"))
        return invalid(ctx, "/loading_witnesses");
    SGLoadingV2ReportInfo info = {2, 0, 0, 0};
    int64_t n;
    const char *mode = string(sh_json_get(o, "mode"));
    const ShJsonValue *rs = sh_json_get(o, "rounds"), *ds = sh_json_get(o, "unassigned_checks");
    if (!integer(sh_json_get(o, "version"), 2, 2, &n) || !mode || strcmp(mode, "hard") ||
        !capabilities(sh_json_get(o, "required_capabilities"), &info.required_capabilities) ||
        !integer(sh_json_get(o, "node_budget"), 0, 100000, &n) || !array(rs, 100000) ||
        !array(ds, 0))
        return invalid(ctx, "/loading_witnesses");
    info.node_budget = (uint32_t)n;
    uint32_t wc = (uint32_t)sh_json_array_len(rs);
    SGLoadingV2RoundEvidence *w = wc ? calloc(wc, sizeof(*w)) : NULL;
    if (wc && !w)
        return SG_STATUS_OUT_OF_MEMORY;
    SGStatus status = SG_STATUS_OK;
    for (uint32_t i = 0; i < wc; i++) {
        int ok = evidence(sh_json_array_get(rs, i), &w[i]);
        if (ok != 1) {
            status = ok < 0 ? SG_STATUS_OUT_OF_MEMORY : invalid(ctx, "/loading_witnesses/rounds");
            break;
        }
    }
    if (status == SG_STATUS_OK)
        status = sg_loading_v2_validate_plan(ctx, count, routes, flags,
                                             SG_LOADING_V2_REPLAY_WITNESSES, &info, wc, w, &report);
    for (uint32_t i = 0; i < wc; i++) {
        free((void *)w[i].placements);
        free((void *)w[i].operations);
        free((void *)w[i].axle_states);
        free((void *)w[i].stops);
    }
    free(w);
    sg_loading_v2_report_free(report);
    return status;
}
void sg_loading_v2_write_error(const SGContext *ctx, ShJsonWriter *w, SGStatus status) {
    static const char *codes[] = {"none",
                                  "invalid_input",
                                  "unsupported_version",
                                  "unsupported_capability",
                                  "unsupported_model",
                                  "invalid_witness",
                                  "witness_rejected",
                                  "no_result",
                                  "allocation_failed",
                                  "internal_error",
                                  "validation_unknown"};
    SGLoadingV2Error e;
    sg_loading_v2_get_error(ctx, &e);
    if (e.code == SG_LOADING_V2_ERROR_NONE)
        e.code = status == SG_STATUS_OUT_OF_MEMORY ? SG_LOADING_V2_ALLOCATION_FAILED
                 : status == SG_STATUS_ERROR       ? SG_LOADING_V2_INTERNAL_ERROR
                 : status == SG_STATUS_LIMIT       ? SG_LOADING_V2_VALIDATION_UNKNOWN
                 : status == SG_STATUS_INFEASIBLE  ? SG_LOADING_V2_WITNESS_REJECTED
                                                   : SG_LOADING_V2_INVALID_INPUT;
    const char *name = status == SG_STATUS_INVALID_ARG       ? "INVALID_ARG"
                       : status == SG_STATUS_NOT_IMPLEMENTED ? "NOT_IMPLEMENTED"
                       : status == SG_STATUS_OUT_OF_MEMORY   ? "OUT_OF_MEMORY"
                       : status == SG_STATUS_INFEASIBLE      ? "INFEASIBLE"
                       : status == SG_STATUS_LIMIT           ? "LIMIT"
                                                             : "ERROR";
    sh_json_write_object_start(w);
    sh_json_write_kv_string(w, "status", name);
    sh_json_write_key(w, "loading_error");
    sh_json_write_object_start(w);
    sh_json_write_kv_string(
        w, "code",
        e.code > 0 && e.code <= SG_LOADING_V2_VALIDATION_UNKNOWN ? codes[e.code] : "invalid_input");
    sh_json_write_kv_string(w, "path", e.path);
    sh_json_write_kv_string(w, "message", sg_get_last_error(ctx));
    if (e.vehicle_id != UINT32_MAX)
        sh_json_write_kv_int(w, "vehicle_id", e.vehicle_id);
    if (e.trip_index != UINT32_MAX)
        sh_json_write_kv_int(w, "trip_index", e.trip_index);
    if (e.operation_index != UINT32_MAX)
        sh_json_write_kv_int(w, "operation_index", e.operation_index);
    sh_json_write_object_end(w);
    sh_json_write_object_end(w);
}
int sg_loading_v2_plan_json_shape(const SGContext *ctx, const ShJsonValue *plan) {
    if (!array(plan, ctx->num_vehicles))
        return 0;
    size_t total = 0;
    for (size_t i = 0; i < sh_json_array_len(plan); i++) {
        const ShJsonValue *r = sh_json_array_get(plan, i);
        int64_t id;
        if (!keys(r, "vehicle_id|task_ids|trip_starts") ||
            !integer(sh_json_get(r, "vehicle_id"), 0, (int64_t)ctx->num_vehicles - 1, &id))
            return 0;
        const ShJsonValue *tasks = sh_json_get(r, "task_ids"),
                          *flags = sh_json_get(r, "trip_starts");
        if (!array(tasks, ctx->num_tasks - total))
            return 0;
        size_t count = sh_json_array_len(tasks);
        total += count;
        if (flags && (!array(flags, count) || sh_json_array_len(flags) != count))
            return 0;
        for (size_t j = 0; j < count; j++) {
            if (!integer(sh_json_array_get(tasks, j), 0, (int64_t)ctx->num_tasks - 1, &id))
                return 0;
            if (flags && !integer(sh_json_array_get(flags, j), 0, 1, &id))
                return 0;
        }
    }
    return 1;
}
