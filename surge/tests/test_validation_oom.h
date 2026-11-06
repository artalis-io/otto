/* Compile the production validator with test-only allocator substitutions.
 * No allocation hooks/state are added to production contexts, and no validation
 * logic is duplicated. The real public entry points are tested separately. */
static int validation_fail_realloc, validation_realloc_allow;
static unsigned validation_malloc_calls, validation_calloc_calls;
static unsigned validation_fail_malloc_at, validation_fail_calloc_at;
static void *validation_test_malloc(size_t size) {
    if (++validation_malloc_calls == validation_fail_malloc_at) return NULL;
    return malloc(size);
}
static void *validation_test_calloc(size_t count, size_t size) {
    if (++validation_calloc_calls == validation_fail_calloc_at) return NULL;
    return calloc(count, size);
}
static void *validation_test_realloc(void *ptr, size_t size) {
    if (validation_fail_realloc && !validation_realloc_allow) return NULL;
    if (validation_fail_realloc) --validation_realloc_allow;
    return realloc(ptr, size);
}
SGStatus validation_test_impl_with_trips(SGContext *, uint32_t,
    const SGPlanRoute *, const uint8_t *const *);
#define malloc validation_test_malloc
#define calloc validation_test_calloc
#define realloc validation_test_realloc
#define sg_validate_plan_impl validation_test_impl
#define sg_validate_plan_impl_with_trips validation_test_impl_with_trips
#include "../src/sg_validate.c"
#undef sg_validate_plan_impl_with_trips
#undef sg_validate_plan_impl
#undef realloc
#undef calloc
#undef malloc

static SGContext *validation_optional_constraints(void) {
    SGContext *ctx = loading_context(1, 2, 1);
    uint32_t type;
    double capacity = 10;
    assert(sg_add_compartment_type(ctx, &type) == SG_STATUS_OK);
    assert(sg_vehicle_add_compartment(ctx, 0, type, &capacity, 1) == SG_STATUS_OK);
    for (uint32_t r = 0; r < 2; ++r)
        assert(sg_request_set_compartment_type(ctx, r, type) == SG_STATUS_OK);
    assert(sg_add_precedence(ctx, 0, 1) == SG_STATUS_OK);
    assert(sg_vehicle_set_pd_policy(ctx, 0, SG_PD_POLICY_LIFO) == SG_STATUS_OK);
    assert(sg_validate_model(ctx) == SG_STATUS_OK);
    return ctx;
}
static void test_validation_allocation_failure_never_publishes(void) {
    for (unsigned kind = 0; kind < 8; ++kind) {
        SGContext *ctx = loading_context(kind == 7 ? 2 : 1, 2, 1);
        uint32_t tasks[] = {ctx->requests[0].delivery_task_id, ctx->requests[1].delivery_task_id};
        SGPlanRoute route = {0, tasks, 2};
        uint32_t routes = 1;
        if (kind == 0) {
            double cap = 0.5;
            assert(sg_vehicle_set_capacity(ctx, 0, &cap, 1) == SG_STATUS_OK);
        } else if (kind == 1) {
            assert(sg_task_set_time_window(ctx, tasks[0], 0, 0) == SG_STATUS_OK);
        } else if (kind == 2) {
            assert(sg_request_set_lock(ctx, 0, SG_LOCK_COMMITTED) == SG_STATUS_OK);
            routes = 0;
        } else if (kind == 3) {
            tasks[0] = UINT32_MAX;
        } else if (kind == 4) {
            SGLoadingUnit tall = {0, 9999, 500, 0, 0, 0};
            assert(sg_request_set_loading(ctx, 0, &tall, 1) == SG_STATUS_OK);
        } else if (kind == 5) {
            assert(sg_request_add_forbidden_vehicle(ctx, 0, 0) == SG_STATUS_OK);
        } else if (kind == 6) {
            tasks[1] = tasks[0];
        } else {
            uint32_t vehicle = 0, length = 2, requests[] = {0, 1};
            assert(sg_set_initial_routes(ctx, 1, &vehicle, &length, requests) == SG_STATUS_OK);
            assert(sg_request_set_lock(ctx, 0, SG_LOCK_FROZEN) == SG_STATUS_OK);
            route.vehicle_id = 1;
        }
        assert(sg_validate_model(ctx) == SG_STATUS_OK);
        validation_realloc_allow = 0; validation_fail_realloc = 1;
        SGStatus status = validation_test_impl(ctx, routes, &route);
        validation_fail_realloc = 0;
        if (status != SG_STATUS_OUT_OF_MEMORY) {
            printf("\nOOM regression kind=%u status=%d violations=%u\n",kind,status,ctx->num_violations);
            fflush(stdout);
        }
        assert(status == SG_STATUS_OUT_OF_MEMORY && ctx->final_solution == NULL);
        assert(validation_test_impl(ctx, routes, &route) == SG_STATUS_OK);
        assert(ctx->num_violations > 0);
        sg_free(ctx);
    }
    /* A failed growth must not publish a partially collected report. */
    {
        SGContext *ctx = loading_context(1, 1, 1);
        uint32_t bad[20]; for (unsigned i = 0; i < 20; ++i) bad[i] = UINT32_MAX;
        SGPlanRoute route = {0, bad, 20};
        validation_realloc_allow = 1; validation_fail_realloc = 1;
        SGStatus status = validation_test_impl(ctx, 1, &route);
        validation_fail_realloc = 0;
        assert(status == SG_STATUS_OUT_OF_MEMORY && !ctx->final_solution);
        assert(ctx->num_violations == 16);
        assert(validation_test_impl(ctx, 1, &route) == SG_STATUS_OK);
        assert(ctx->num_violations == 20);
        sg_free(ctx);
    }
    /* Exercise every direct scratch allocation, including optional checks. */
    unsigned mallocs, callocs;
    {
        SGContext *ctx = validation_optional_constraints();
        uint32_t tasks[] = {0, 1}; SGPlanRoute route = {0, tasks, 2};
        validation_malloc_calls = validation_calloc_calls = 0;
        assert(validation_test_impl(ctx, 1, &route) == SG_STATUS_OK);
        assert(ctx->num_violations == 0);
        mallocs = validation_malloc_calls; callocs = validation_calloc_calls;
        assert(mallocs >= 2 && callocs >= 6);
        sg_free(ctx);
    }
    for (unsigned allocator = 0; allocator < 2; ++allocator) {
        unsigned count = allocator ? callocs : mallocs;
        for (unsigned failure = 1; failure <= count; ++failure) {
            SGContext *ctx = validation_optional_constraints();
            uint32_t tasks[] = {0, 1}; SGPlanRoute route = {0, tasks, 2};
            validation_malloc_calls = validation_calloc_calls = 0;
            validation_fail_malloc_at = allocator ? 0 : failure;
            validation_fail_calloc_at = allocator ? failure : 0;
            SGStatus status = validation_test_impl(ctx, 1, &route);
            validation_fail_malloc_at = validation_fail_calloc_at = 0;
            assert(status == SG_STATUS_OUT_OF_MEMORY && !ctx->final_solution);
            assert(validation_test_impl(ctx, 1, &route) == SG_STATUS_OK);
            assert(ctx->num_violations == 0);
            sg_free(ctx);
        }
    }
}
static void test_manual_validation_capacity_and_authority(void) {
    SGContext *ctx = loading_context(1, 1, 1);
    uint32_t task = 0; SGPlanRoute route = {0, &task, 1};
    const double limits[] = {0, 0.5, 1};
    for (unsigned i = 0; i < 3; ++i) {
        assert(sg_vehicle_set_capacity(ctx, 0, &limits[i], 1) == SG_STATUS_OK);
        assert(sg_validate_plan(ctx, 1, &route) == SG_STATUS_OK);
        assert((ctx->num_violations > 0) == (limits[i] < 1));
        if (limits[i] < 1) assert(ctx->violations[0].type == SG_VIOLATION_CAPACITY);
    }
    sg_free(ctx);
    ctx = loading_context(1, 2, 1);
    assert(sg_vehicle_set_max_trips(ctx, 0, 2) == SG_STATUS_OK);
    assert(sg_vehicle_set_trip_reload_seconds(ctx, 0, 1) == SG_STATUS_OK);
    double cap = 0.5;
    assert(sg_vehicle_set_capacity(ctx, 0, &cap, 1) == SG_STATUS_OK);
    uint32_t tasks[] = {0, 1}; uint8_t starts[] = {0, 1}; const uint8_t *boundaries[] = {starts};
    route.task_ids = tasks; route.task_count = 2;
    assert(sg_validate_plan_with_trips(ctx, 1, &route, boundaries) == SG_STATUS_OK);
    assert(ctx->num_violations == 2);
    for (unsigned i = 0; i < 2; ++i) assert(ctx->violations[i].type == SG_VIOLATION_CAPACITY);
    cap = 1;
    assert(sg_vehicle_set_capacity(ctx, 0, &cap, 1) == SG_STATUS_OK);
    assert(sg_validate_plan_with_trips(ctx, 1, &route, boundaries) == SG_STATUS_OK);
    assert(ctx->num_violations == 0);
    sg_free(ctx);
    /* A missed annotation must still fail the complete hard validator. */
    ctx = loading_context(1, 1, 1);
    uint32_t type; cap = 0.5;
    assert(sg_add_compartment_type(ctx, &type) == SG_STATUS_OK);
    assert(sg_vehicle_add_compartment(ctx, 0, type, &cap, 1) == SG_STATUS_OK);
    assert(sg_request_set_compartment_type(ctx, 0, type) == SG_STATUS_OK);
    route.task_ids = &task; route.task_count = 1;
    assert(sg_validate_plan(ctx, 1, &route) != SG_STATUS_OK);
    assert(!ctx->final_solution);
    sg_free(ctx);
}
