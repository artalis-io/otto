#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "surge.h"
#include "sg_internal.h"
#include "../src/sg_profile_matrix.h"

static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(fn) do { \
    tests_run++; \
    printf("  %-55s", #fn); \
    fn(); \
    tests_passed++; \
    printf("OK\n"); \
} while (0)

/* ---- Helpers ---- */

static void add_depot_with_location(SGContext *ctx, uint32_t *id_out,
                                    double x, double y) {
    uint32_t id = sg_add_depot(ctx);
    assert(id != UINT32_MAX);
    assert(sg_depot_set_location(ctx, id, x, y) == SG_STATUS_OK);
    *id_out = id;
}

static void add_vehicle_with_depot(SGContext *ctx, uint32_t depot_id,
                                   int32_t shift_early, int32_t shift_late,
                                   double capacity) {
    uint32_t vid = sg_add_vehicle(ctx);
    assert(vid != UINT32_MAX);
    assert(sg_vehicle_set_depots(ctx, vid, depot_id, depot_id) == SG_STATUS_OK);
    assert(sg_vehicle_set_shift_time_window(ctx, vid, shift_early, shift_late) == SG_STATUS_OK);
    double cap = capacity;
    assert(sg_vehicle_set_capacity(ctx, vid, &cap, 1) == SG_STATUS_OK);
}

static void add_delivery_request(SGContext *ctx, double x, double y,
                                 int32_t tw_early, int32_t tw_late,
                                 int32_t svc, double demand) {
    uint32_t rid = sg_add_delivery_request(ctx, x, y, tw_early, tw_late, svc, demand);
    assert(rid != UINT32_MAX);
}

/* ---- Tests ---- */

static void test_scale_from_count_boundaries(void) {
    /* Lower boundaries */
    assert(sg_scale_from_count(1)   == SG_SCALE_SMALL);
    assert(sg_scale_from_count(50)  == SG_SCALE_SMALL);
    assert(sg_scale_from_count(100) == SG_SCALE_SMALL);

    /* MEDIUM: 101–200 */
    assert(sg_scale_from_count(101) == SG_SCALE_MEDIUM);
    assert(sg_scale_from_count(150) == SG_SCALE_MEDIUM);
    assert(sg_scale_from_count(200) == SG_SCALE_MEDIUM);

    /* LARGE: 201–400 */
    assert(sg_scale_from_count(201) == SG_SCALE_LARGE);
    assert(sg_scale_from_count(300) == SG_SCALE_LARGE);
    assert(sg_scale_from_count(400) == SG_SCALE_LARGE);

    /* XLARGE: 401–800 */
    assert(sg_scale_from_count(401) == SG_SCALE_XLARGE);
    assert(sg_scale_from_count(600) == SG_SCALE_XLARGE);
    assert(sg_scale_from_count(800) == SG_SCALE_XLARGE);

    /* MASSIVE: 801+ */
    assert(sg_scale_from_count(801)  == SG_SCALE_MASSIVE);
    assert(sg_scale_from_count(5000) == SG_SCALE_MASSIVE);
}

static void test_scale_from_count_zero(void) {
    /* Edge case: 0 requests → SMALL */
    assert(sg_scale_from_count(0) == SG_SCALE_SMALL);
}

static void test_matrix_cell_validity(void) {
    int p, s;
    for (p = 0; p < SG_PROFILE_COUNT; p++) {
        for (s = 0; s < SG_SCALE_COUNT; s++) {
            const SGProfileCell *cell = &k_profile_matrix[p][s];
            assert(cell->max_iterations > 0);
            assert(cell->max_time_seconds > 0);
            /* Tune params should have explicit values for key fields */
            assert(cell->tune.sa_accept_pct != SG_TUNE_SENTINEL_D);
            assert(cell->tune.p1_final_temp_ratio != SG_TUNE_SENTINEL_D);
            assert(cell->tune.p2_final_temp_ratio != SG_TUNE_SENTINEL_D);
            assert(cell->tune.phase1_fraction != SG_TUNE_SENTINEL_D);
            assert(cell->tune.neighbor_k != SG_TUNE_SENTINEL_I);
        }
    }
}

static void test_matrix_small_matches_old_profiles(void) {
    /* SMALL column should match the old sg_config_set_profile values */
    const SGProfileCell *rt = &k_profile_matrix[SG_PROFILE_REALTIME][SG_SCALE_SMALL];
    assert(rt->max_iterations == 500);
    assert(rt->max_time_seconds == 1);

    const SGProfileCell *fast = &k_profile_matrix[SG_PROFILE_FAST][SG_SCALE_SMALL];
    assert(fast->max_iterations == 2500);
    assert(fast->max_time_seconds == 5);

    const SGProfileCell *near = &k_profile_matrix[SG_PROFILE_NEAR_OPTIMAL][SG_SCALE_SMALL];
    assert(near->max_iterations == 10000);
    assert(near->max_time_seconds == 15);

    const SGProfileCell *best = &k_profile_matrix[SG_PROFILE_BEST][SG_SCALE_SMALL];
    assert(best->max_iterations == 50000);
    assert(best->max_time_seconds == 60);
}

static void test_profile_matrix_apply(void) {
    SGContext *ctx = sg_create();
    assert(ctx != NULL);

    assert(sg_profile_matrix_apply(ctx, SG_PROFILE_FAST, SG_SCALE_LARGE) == SG_STATUS_OK);
    assert(ctx->config.max_iterations == 2500);
    assert(ctx->config.max_time_seconds == 30);
    assert(ctx->tune_params != NULL);
    assert(ctx->tune_params->sa_accept_pct == 0.074);

    sg_free(ctx);
}

static void test_profile_matrix_apply_invalid(void) {
    SGContext *ctx = sg_create();
    assert(ctx != NULL);

    assert(sg_profile_matrix_apply(ctx, SG_PROFILE_COUNT, SG_SCALE_SMALL) == SG_STATUS_INVALID_ARG);
    assert(sg_profile_matrix_apply(ctx, SG_PROFILE_FAST, SG_SCALE_COUNT) == SG_STATUS_INVALID_ARG);
    assert(sg_profile_matrix_apply(NULL, SG_PROFILE_FAST, SG_SCALE_SMALL) == SG_STATUS_INVALID_ARG);

    sg_free(ctx);
}

static void test_set_profile_defers(void) {
    /* sg_config_set_profile should not immediately apply config */
    SGContext *ctx = sg_create();
    SGConfig cfg;
    assert(ctx != NULL);

    sg_config_default(&cfg);
    cfg.max_iterations = 999;
    cfg.max_time_seconds = 77;
    sg_set_config(ctx, &cfg);

    assert(sg_config_set_profile(ctx, SG_PROFILE_FAST) == SG_STATUS_OK);
    assert(ctx->active_profile == SG_PROFILE_FAST);
    assert(ctx->active_scale == SG_SCALE_COUNT);  /* auto-detect */
    assert(ctx->profile_applied == 0);

    /* Config should still have old values (deferred) */
    assert(ctx->config.max_iterations == 999);
    assert(ctx->config.max_time_seconds == 77);

    sg_free(ctx);
}

static void test_set_profile_scale(void) {
    SGContext *ctx = sg_create();
    assert(ctx != NULL);

    assert(sg_config_set_profile_scale(ctx, SG_PROFILE_BEST, SG_SCALE_XLARGE) == SG_STATUS_OK);
    assert(ctx->active_profile == SG_PROFILE_BEST);
    assert(ctx->active_scale == SG_SCALE_XLARGE);
    assert(ctx->profile_applied == 0);

    sg_free(ctx);
}

static void test_set_profile_scale_invalid(void) {
    SGContext *ctx = sg_create();
    assert(ctx != NULL);

    assert(sg_config_set_profile_scale(ctx, SG_PROFILE_COUNT, SG_SCALE_SMALL) == SG_STATUS_INVALID_ARG);
    assert(sg_config_set_profile_scale(ctx, SG_PROFILE_FAST, SG_SCALE_COUNT) == SG_STATUS_INVALID_ARG);

    sg_free(ctx);
}

static void test_profile_auto_scale_solve(void) {
    /* Set FAST profile, add ~8 requests (SMALL), solve → should get FAST×SMALL */
    SGContext *ctx = sg_create();
    SGConfig cfg;
    uint32_t depot;
    int i;

    assert(ctx != NULL);
    sg_config_default(&cfg);
    cfg.seed = 42;
    cfg.deterministic = true;
    cfg.require_bound_requests_at_solve = true;
    sg_set_config(ctx, &cfg);

    sg_config_set_profile(ctx, SG_PROFILE_FAST);

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 500.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 500.0);

    for (i = 0; i < 8; i++) {
        add_delivery_request(ctx, (double)(i * 3 + 1), (double)(i * 2),
                             0, 86400, 120, -10.0);
    }

    assert(sg_solve(ctx) == SG_STATUS_OK);

    /* After solve, config should match FAST×SMALL */
    assert(ctx->config.max_iterations == 2500);
    assert(ctx->config.max_time_seconds == 5);
    assert(ctx->profile_applied == 1);

    sg_free(ctx);
}

static void test_profile_explicit_scale_solve(void) {
    /* Set FAST + LARGE explicitly, add only 8 requests → still FAST×LARGE */
    SGContext *ctx = sg_create();
    SGConfig cfg;
    uint32_t depot;
    int i;

    assert(ctx != NULL);
    sg_config_default(&cfg);
    cfg.seed = 42;
    cfg.deterministic = true;
    cfg.require_bound_requests_at_solve = true;
    sg_set_config(ctx, &cfg);

    sg_config_set_profile_scale(ctx, SG_PROFILE_FAST, SG_SCALE_LARGE);

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 500.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 500.0);

    for (i = 0; i < 8; i++) {
        add_delivery_request(ctx, (double)(i * 3 + 1), (double)(i * 2),
                             0, 86400, 120, -10.0);
    }

    assert(sg_solve(ctx) == SG_STATUS_OK);

    /* Explicit scale overrides auto-detect */
    assert(ctx->config.max_iterations == 2500);
    assert(ctx->config.max_time_seconds == 30);
    assert(ctx->profile_applied == 1);

    sg_free(ctx);
}

static void test_no_profile_manual_config(void) {
    /* Without setting profile, manual config/tune should be unchanged after solve */
    SGContext *ctx = sg_create();
    SGConfig cfg;
    uint32_t depot;
    int i;

    assert(ctx != NULL);
    sg_config_default(&cfg);
    cfg.max_iterations = 77;
    cfg.max_time_seconds = 3;
    cfg.seed = 42;
    cfg.deterministic = true;
    cfg.require_bound_requests_at_solve = true;
    sg_set_config(ctx, &cfg);

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 500.0);

    for (i = 0; i < 4; i++) {
        add_delivery_request(ctx, (double)(i * 3 + 1), (double)(i * 2),
                             0, 86400, 120, -10.0);
    }

    assert(sg_solve(ctx) == SG_STATUS_OK);

    /* Config unchanged — matrix not consulted */
    assert(ctx->config.max_iterations == 77);
    assert(ctx->config.max_time_seconds == 3);
    assert(ctx->profile_applied == 0);

    sg_free(ctx);
}

static void test_profile_backward_compat(void) {
    /* FAST + 80 requests → FAST×SMALL: same as old hardcoded behavior */
    SGContext *ctx = sg_create();
    assert(ctx != NULL);

    sg_config_set_profile(ctx, SG_PROFILE_FAST);

    /* Simulate: the matrix should give same values after resolution */
    SGScale scale = sg_scale_from_count(80);
    assert(scale == SG_SCALE_SMALL);

    /* Apply manually to check values */
    sg_profile_matrix_apply(ctx, SG_PROFILE_FAST, scale);
    assert(ctx->config.max_iterations == 2500);
    assert(ctx->config.max_time_seconds == 5);
    assert(ctx->tune_params != NULL);
    assert(ctx->tune_params->sa_accept_pct == 0.074);
    assert(ctx->tune_params->p1_final_temp_ratio == 0.08);
    assert(ctx->tune_params->p2_final_temp_ratio == 0.0001);

    sg_free(ctx);
}

static void test_solve_twice_profile_applied_once(void) {
    /* Profile should be applied only on first solve (profile_applied flag) */
    SGContext *ctx = sg_create();
    SGConfig cfg;
    uint32_t depot;
    int i;

    assert(ctx != NULL);
    sg_config_default(&cfg);
    cfg.seed = 42;
    cfg.deterministic = true;
    cfg.require_bound_requests_at_solve = true;
    sg_set_config(ctx, &cfg);

    sg_config_set_profile(ctx, SG_PROFILE_REALTIME);

    add_depot_with_location(ctx, &depot, 0.0, 0.0);
    add_vehicle_with_depot(ctx, depot, 0, 86400, 500.0);

    for (i = 0; i < 4; i++) {
        add_delivery_request(ctx, (double)(i * 3 + 1), (double)(i * 2),
                             0, 86400, 120, -10.0);
    }

    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(ctx->profile_applied == 1);
    assert(ctx->config.max_iterations == 500);
    assert(ctx->config.max_time_seconds == 1);

    /* Manually change iterations — second solve should keep them */
    ctx->config.max_iterations = 100;
    assert(sg_solve(ctx) == SG_STATUS_OK);
    assert(ctx->config.max_iterations == 100);  /* Not overwritten */

    sg_free(ctx);
}

static void test_all_profiles_resolve(void) {
    /* Loop over all 4 profiles with 100 requests, verify each resolves */
    int p;
    for (p = 0; p < SG_PROFILE_COUNT; p++) {
        SGContext *ctx = sg_create();
        SGConfig cfg;
        uint32_t depot;
        int i;

        assert(ctx != NULL);
        sg_config_default(&cfg);
        cfg.seed = 42;
        cfg.deterministic = true;
        cfg.require_bound_requests_at_solve = true;
        sg_set_config(ctx, &cfg);

        sg_config_set_profile(ctx, (SGProfile)p);

        add_depot_with_location(ctx, &depot, 0.0, 0.0);
        add_vehicle_with_depot(ctx, depot, 0, 86400, 5000.0);
        add_vehicle_with_depot(ctx, depot, 0, 86400, 5000.0);

        for (i = 0; i < 8; i++) {
            add_delivery_request(ctx, (double)(i * 3 + 1), (double)(i * 2),
                                 0, 86400, 120, -10.0);
        }

        assert(sg_solve(ctx) == SG_STATUS_OK);
        assert(ctx->profile_applied == 1);
        assert(ctx->config.max_iterations > 0);
        assert(ctx->config.max_time_seconds > 0);

        sg_free(ctx);
    }
}

static void test_increasing_time_with_scale(void) {
    /* For each profile, time budget should be non-decreasing across scales */
    int p;
    for (p = 0; p < SG_PROFILE_COUNT; p++) {
        int prev_time = 0;
        int s;
        for (s = 0; s < SG_SCALE_COUNT; s++) {
            const SGProfileCell *cell = &k_profile_matrix[p][s];
            assert(cell->max_time_seconds >= prev_time);
            prev_time = cell->max_time_seconds;
        }
    }
}

/* ---- Main ---- */

int main(void) {
    printf("test_profile_matrix\n");

    RUN_TEST(test_scale_from_count_boundaries);
    RUN_TEST(test_scale_from_count_zero);
    RUN_TEST(test_matrix_cell_validity);
    RUN_TEST(test_matrix_small_matches_old_profiles);
    RUN_TEST(test_profile_matrix_apply);
    RUN_TEST(test_profile_matrix_apply_invalid);
    RUN_TEST(test_set_profile_defers);
    RUN_TEST(test_set_profile_scale);
    RUN_TEST(test_set_profile_scale_invalid);
    RUN_TEST(test_profile_auto_scale_solve);
    RUN_TEST(test_profile_explicit_scale_solve);
    RUN_TEST(test_no_profile_manual_config);
    RUN_TEST(test_profile_backward_compat);
    RUN_TEST(test_solve_twice_profile_applied_once);
    RUN_TEST(test_all_profiles_resolve);
    RUN_TEST(test_increasing_time_with_scale);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
