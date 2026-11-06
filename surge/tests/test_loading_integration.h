/* Included by test_surge.c after its shared helpers. No private fixtures. */
#include "sg_parallel.h"

static SGLoadingVehicle loading_vehicle(unsigned slots, SGLoadingAccess access) {
    SGLoadingVehicle v = {0};
    v.slot_count = slots;
    v.max_layers = 2;
    v.clearance_mm = 2400;
    v.payload_g = 100000;
    v.max_stack_weight_g = 10000;
    v.class_count = 2;
    v.allowed_top[0] = 3;
    v.access = access;
    for (unsigned i = 0; i < slots; i++)
        v.depth[i] = i;
    return v;
}
static SGContext *loading_context(unsigned vehicles, unsigned requests, int stackable) {
    SGContext *ctx = make_config(80, 42);
    uint32_t depot;
    add_depot_with_location(ctx, &depot, 0, 0);
    for (unsigned v = 0; v < vehicles; v++) {
        uint32_t id = sg_add_vehicle(ctx);
        double cap = 10;
        assert(sg_vehicle_set_depots(ctx, id, depot, depot) == SG_STATUS_OK);
        assert(sg_vehicle_set_capacity(ctx, id, &cap, 1) == SG_STATUS_OK);
        SGLoadingVehicle geom = loading_vehicle(1, SG_LOADING_SIDE_FREE);
        assert(sg_vehicle_set_loading(ctx, id, &geom) == SG_STATUS_OK);
    }
    for (unsigned r = 0; r < requests; r++) {
        add_delivery_request(ctx, 1 + r, 1, 0, 100000, 0, -1);
        SGLoadingUnit unit = {0, 1000, 500, 1000, 0, stackable ? 1 : 0};
        assert(sg_request_set_loading(ctx, r, &unit, 1) == SG_STATUS_OK);
    }
    assert(sg_set_hard_loading(ctx, 1, 1000) == SG_STATUS_OK);
    return ctx;
}
static void loading_assert_solution(SGContext *ctx) {
    assert(ctx->final_solution && sg_route_solution_validate(ctx->final_solution, ctx));
    for (uint32_t route = 0; route < sg_solution_get_route_count(ctx); ++route) {
        uint32_t trips = sg_solution_get_route_trip_count(ctx, route);
        for (uint32_t trip = 0; trip < trips; ++trip) {
            SGLoadingResult r;
            uint32_t requests[128], indices[128];
            assert(sg_solution_get_loading(ctx, route, trip, &r, requests, indices) ==
                   SG_STATUS_OK);
            assert(r.outcome == SG_LOADING_FEASIBLE);
            for (uint32_t i = 0; i < r.count; i++)
                assert(requests[i] < ctx->num_requests &&
                       indices[i] < ctx->requests[requests[i]].loading_count);
        }
    }
}
static void test_loading_hgs_and_legacy(void) {
    SGContext *ctx = loading_context(2, 4, 1);
#ifdef SG_HAS_THREADS
    SGPopulationConfig pop = {0};
    pop.num_threads = 2;
    pop.num_generations = 3;
    pop.population_size = 4;
    SGStatus loading_status = sg_solve_population(ctx, &pop);
    if (loading_status != SG_STATUS_OK)
        fprintf(stderr, "loading population status=%d error=%s\n", loading_status,
                sg_get_last_error(ctx));
    assert(loading_status == SG_STATUS_OK);
#else
    assert(sg_solve(ctx) == SG_STATUS_OK);
#endif
    loading_assert_solution(ctx);
    assert(ctx->stats.unassigned == 0 && ctx->stats.vehicles_used == 2);
#ifdef SG_HAS_THREADS
    for (unsigned id = 0; id < 4; ++id) {
        SGLoadingUnit u = {0, 1000, 500, 1000, 0, 0};
        assert(sg_request_set_loading(ctx, id, &u, 1) == SG_STATUS_OK);
    }
    assert(sg_solve_population(ctx, &pop) == SG_STATUS_OK);
    loading_assert_solution(ctx);
    assert(ctx->stats.unassigned == 2); /* fresh workers must not reuse old stacking evidence */
#endif
    sg_free(ctx);
    ctx = loading_context(2, 4, 0);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    loading_assert_solution(ctx);
    assert(ctx->stats.unassigned == 2); /* scalar capacity alone would permit every order */
    assert(sg_set_hard_loading(ctx, 0, 0) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(ctx->stats.unassigned == 0);
    sg_free(ctx);
    /* Directed support is non-monotone: two class-0 units cannot stack alone,
     * but a class-1 unit from another request can support the middle layer. */
    ctx = loading_context(1, 2, 1);
    SGLoadingVehicle bridge = loading_vehicle(1, SG_LOADING_SIDE_FREE);
    bridge.max_layers = 3;
    bridge.clearance_mm = 4000;
    bridge.allowed_top[0] = 2;
    bridge.allowed_top[1] = 1;
    assert(sg_vehicle_set_loading(ctx, 0, &bridge) == SG_STATUS_OK);
    SGLoadingUnit pair[2] = {{0, 1000, 500, 1000, 0, 2}, {0, 1000, 500, 1000, 0, 2}};
    SGLoadingUnit support = {0, 1000, 500, 1000, 1, 2};
    assert(sg_request_set_loading(ctx, 0, pair, 2) == SG_STATUS_OK);
    assert(sg_request_set_loading(ctx, 1, &support, 1) == SG_STATUS_OK);
    uint32_t task = ctx->requests[0].delivery_task_id;
    SGPlanRoute alone = {0, &task, 1};
    assert(sg_validate_plan(ctx, 1, &alone) == SG_STATUS_OK);
    SGLoadingResult result;
    assert(sg_solution_get_loading(ctx, 0, 0, &result, NULL, NULL) == SG_STATUS_OK);
    assert(result.outcome == SG_LOADING_INFEASIBLE);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    loading_assert_solution(ctx);
    assert(ctx->stats.unassigned == 0);
    assert(sg_solution_get_loading(ctx, 0, 0, &result, NULL, NULL) == SG_STATUS_OK);
    assert(result.count == 3);
    sg_free(ctx);
}
static void test_loading_unknown_never_publishes(void) {
    SGContext *ctx = loading_context(1, 1, 1);
    assert(sg_set_hard_loading(ctx, 1, 1) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    loading_assert_solution(ctx);
    assert(ctx->stats.unassigned == 1);
    uint32_t task = ctx->requests[0].delivery_task_id;
    SGPlanRoute route = {0, &task, 1};
    assert(sg_validate_plan(ctx, 1, &route) == SG_STATUS_OK);
    int found = 0;
    for (uint32_t i = 0; i < ctx->num_violations; ++i)
        if (ctx->violations[i].type == SG_VIOLATION_LOADING_UNKNOWN)
            found = 1;
    assert(found);
    SGLoadingResult r;
    assert(sg_solution_get_loading(ctx, 0, 0, &r, NULL, NULL) == SG_STATUS_OK);
    assert(r.outcome == SG_LOADING_UNKNOWN && r.count == 0);
    sg_free(ctx);
}
static void test_loading_manual_order_and_cache(void) {
    SGContext *ctx = loading_context(1, 2, 1);
    SGLoadingVehicle v = loading_vehicle(1, SG_LOADING_REAR_LIFO);
    assert(sg_vehicle_set_loading(ctx, 0, &v) == SG_STATUS_OK);
    SGLoadingUnit top = {0, 1000, 500, 0, 1, 0};
    assert(sg_request_set_loading(ctx, 1, &top, 1) == SG_STATUS_OK);
    uint32_t tasks[2] = {ctx->requests[0].delivery_task_id, ctx->requests[1].delivery_task_id};
    SGPlanRoute route = {0, tasks, 2};
    assert(sg_validate_plan(ctx, 1, &route) == SG_STATUS_OK);
    assert(ctx->num_violations > 0); /* bottom delivered before inaccessible top */
    SGLoadingResult r;
    assert(sg_solution_get_loading(ctx, 0, 0, &r, NULL, NULL) == SG_STATUS_OK);
    assert(r.outcome == SG_LOADING_INFEASIBLE);
    uint32_t swap = tasks[0];
    tasks[0] = tasks[1];
    tasks[1] = swap;
    assert(sg_validate_plan(ctx, 1, &route) == SG_STATUS_OK);
    assert(ctx->num_violations == 0);
    loading_assert_solution(ctx);
    assert(ctx->loading_cache);
    SGLoadingResult again;
    assert(sg_solution_get_loading(ctx, 0, 0, &r, NULL, NULL) == SG_STATUS_OK);
    assert(sg_solution_get_loading(ctx, 0, 0, &again, NULL, NULL) == SG_STATUS_OK);
    assert(memcmp(&r, &again, sizeof(r)) == 0);
    v.clearance_mm = 1900;
    assert(sg_vehicle_set_loading(ctx, 0, &v) == SG_STATUS_OK);
    assert(!ctx->final_solution && !ctx->loading_cache);
    assert(sg_solution_get_loading(ctx, 0, 0, &r, NULL, NULL) == SG_STATUS_INVALID_ARG);
    assert(r.outcome == SG_LOADING_UNKNOWN && r.count == 0);
    assert(sg_validate_plan(ctx, 1, &route) == SG_STATUS_OK);
    assert(ctx->num_violations > 0);
    sg_free(ctx);
}
static void test_loading_multitrip_and_apply_boundary(void) {
    SGContext *ctx = loading_context(1, 2, 0);
    assert(sg_vehicle_set_max_trips(ctx, 0, 2) == SG_STATUS_OK);
    assert(sg_vehicle_set_trip_reload_seconds(ctx, 0, 1) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    loading_assert_solution(ctx);
    assert(ctx->stats.unassigned == 0 && sg_solution_get_route_trip_count(ctx, 0) == 2);
    uint32_t tasks[2] = {ctx->requests[0].delivery_task_id, ctx->requests[1].delivery_task_id};
    SGPlanRoute route = {0, tasks, 2};
    uint8_t flags[2] = {0, 1};
    const uint8_t *starts[1] = {flags};
    assert(sg_validate_plan_with_trips(ctx, 1, &route, starts) == SG_STATUS_OK);
    assert(ctx->num_violations == 0);
    loading_assert_solution(ctx);
    assert(sg_validate_plan(ctx, 1, &route) == SG_STATUS_OK);
    assert(ctx->num_violations > 0);
    SGRouteSolution sol;
    assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);
    assert(sg_route_apply_insertion(ctx, &sol, 0, 0, 0, 0) == AR_STATUS_OK);
    assert(sg_route_apply_insertion(ctx, &sol, 1, 0, 1, 0) == AR_STATUS_INVALID_ARG);
    assert(sol.route_lengths[0] == 1 && !sol.base.assigned_flags[1]);
    assert(sg_route_apply_insertion_mode(ctx, &sol, 1, 0, 1, 0, 1) == AR_STATUS_OK);
    assert(sg_route_solution_validate(&sol, ctx));
    assert(sg_vehicle_set_max_trips(ctx, 0, 1) == SG_STATUS_OK);
    assert(!sg_route_solution_validate(&sol, ctx));
    assert(sg_vehicle_set_max_trips(ctx, 0, 2) == SG_STATUS_OK);
    assert(sg_route_solution_validate(&sol, ctx));
    sg_route_solution_reset(&sol);
    sg_free(ctx);
}
static void test_loading_incomplete_and_unsupported_model(void) {
    SGContext *ctx = loading_context(1, 1, 1);
    ctx->requests[0].has_loading_units = 0;
    assert(sg_solve(ctx) != SG_STATUS_OK);
    assert(!ctx->final_solution);
    ctx->requests[0].has_loading_units = 1;
    ctx->vehicles[0].open_start = 1;
    assert(sg_validate_model(ctx) == SG_STATUS_INVALID_ARG);
    ctx->vehicles[0].open_start = 0;
    ctx->requests[0].kind = SG_REQUEST_KIND_PICKUP_DELIVERY;
    assert(sg_validate_model(ctx) == SG_STATUS_INVALID_ARG);
    ctx->requests[0].kind = SG_REQUEST_KIND_DELIVERY_ONLY;
    assert(sg_set_hard_loading(ctx, 1, 100001) == SG_STATUS_INVALID_ARG);
    sg_free(ctx);
}

static char *loading_api_reply(const char *plan, int *status, size_t *length) {
    FILE *file = fopen("tests/fixtures/loading.json", "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size > 0);
    rewind(file);
    char *text = malloc((size_t)size + 1);
    assert(text);
    assert(fread(text, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    text[size] = 0;
    char *end = strrchr(text, '}');
    assert(end);
    *end = 0;
    size_t capacity = strlen(text) + strlen(plan) + 4;
    char *body = malloc(capacity);
    assert(body);
    snprintf(body, capacity, "%s,%s}", text, plan);
    free(text);
    char *reply = sg_api_solve(body, strlen(body), status, length);
    free(body);
    assert(reply);
    return reply;
}
static void test_loading_json_manual_rounds_and_errors(void) {
    int status;
    size_t length;
    char *reply = loading_api_reply(
        "\"plan\":[{\"vehicle_id\":0,\"task_ids\":[0,1],\"trip_starts\":[0,1]}]", &status, &length);
    assert(status == 200);
    SHArena *arena = sh_arena_create(length * 8 + 4096);
    ShJsonValue *root = NULL;
    assert(sh_json_parse(reply, length, arena, &root) == SH_JSON_OK);
    assert(sh_json_array_len(sh_json_get(root, "violations")) == 0);
    ShJsonValue *rounds = sh_json_get(sh_json_get(root, "loading"), "rounds");
    assert(sh_json_array_len(rounds) == 2);
    for (unsigned i = 0; i < 2; i++)
        assert(!strcmp(sh_json_as_string(sh_json_get(sh_json_array_get(rounds, i), "outcome"), ""),
                       "feasible"));
    sh_arena_free(arena);
    free(reply);
    reply = loading_api_reply("\"plan\":[{\"vehicle_id\":0,\"task_ids\":[0,1]}]", &status, &length);
    assert(status == 200);
    assert(strstr(reply, "\"type\":\"loading\""));
    assert(strstr(reply, "\"outcome\":\"infeasible\""));
    free(reply);
    reply = loading_api_reply(
        "\"plan\":[{\"vehicle_id\":0,\"task_ids\":[0,1],\"trip_starts\":[0,\"1\"]}]", &status,
        &length);
    assert(status == 400);
    free(reply);
}
static void test_loading_interrupted_checks_are_not_cached(void) {
    SGContext *ctx = loading_context(1, 1, 1);
    assert(sg_prepare_travel(ctx) == SG_STATUS_OK);
    SGRouteSolution sol;
    assert(sg_route_solution_init(ctx, &sol) == AR_STATUS_OK);
    atomic_store(&ctx->cancel_requested, 1);
    assert(sg_route_apply_insertion(ctx, &sol, 0, 0, 0, 0) == AR_STATUS_INVALID_ARG);
    assert(!ctx->loading_cache && sol.route_lengths[0] == 0);
    atomic_store(&ctx->cancel_requested, 0);
    assert(sg_route_apply_insertion(ctx, &sol, 0, 0, 0, 0) == AR_STATUS_OK);
    assert(ctx->loading_cache && sg_route_solution_validate(&sol, ctx));
    sg_route_solution_reset(&sol);
    sg_free(ctx);
}

static void test_loading_preserves_other_hard_constraints(void) {
    SGContext *ctx = loading_context(2, 2, 1);
    double capacity = 1;
    assert(sg_set_hard_capacity(ctx, true) == SG_STATUS_OK);
    assert(sg_set_hard_time_windows(ctx, true) == SG_STATUS_OK);
    for (unsigned v = 0; v < 2; v++)
        assert(sg_vehicle_set_capacity(ctx, v, &capacity, 1) == SG_STATUS_OK);
    assert(sg_solve(ctx) == SG_STATUS_OK);
    loading_assert_solution(ctx);
    assert(ctx->stats.unassigned == 0 && ctx->stats.vehicles_used == 2);
    sg_free(ctx);
    ctx = loading_context(1, 1, 1);
    SGLoadingUnit too_tall = {0, 9999, 500, 0, 0, 0};
    assert(sg_request_set_loading(ctx, 0, &too_tall, 1) == SG_STATUS_OK);
    assert(sg_request_set_lock(ctx, 0, SG_LOCK_COMMITTED) == SG_STATUS_OK);
    assert(sg_solve(ctx) != SG_STATUS_OK);
    assert(!ctx->final_solution);
    sg_free(ctx);
}
