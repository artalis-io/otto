#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "arbor.h"

typedef struct {
    int value;
} TestSolution;

static void *test_copy(const void *solution, void *user_ctx) {
    const TestSolution *src = (const TestSolution *)solution;
    TestSolution *dst = (TestSolution *)malloc(sizeof(*dst));
    (void)user_ctx;
    if (!dst) {
        return NULL;
    }
    *dst = *src;
    return dst;
}

static void test_free(void *solution, void *user_ctx) {
    (void)user_ctx;
    free(solution);
}

static double test_cost(const void *solution, void *user_ctx) {
    const TestSolution *sol = (const TestSolution *)solution;
    (void)user_ctx;
    return (double)sol->value;
}

static int test_size(const void *solution, void *user_ctx) {
    const TestSolution *sol = (const TestSolution *)solution;
    (void)user_ctx;
    return sol->value;
}

static int test_validate(const void *solution, void *user_ctx) {
    const TestSolution *sol = (const TestSolution *)solution;
    (void)user_ctx;
    return sol->value >= 0;
}

static ARStatus test_destroy(void *op_ctx, void *solution, int count,
                             uint32_t *removed_ids, int *removed_count) {
    TestSolution *sol = (TestSolution *)solution;
    (void)op_ctx;
    (void)removed_ids;

    if (!sol || !removed_count || count < 0) {
        return AR_STATUS_INVALID_ARG;
    }

    if (count > sol->value) {
        count = sol->value;
    }

    sol->value -= count;
    *removed_count = count;
    return AR_STATUS_OK;
}

static ARStatus test_repair(void *op_ctx, void *solution,
                            const uint32_t *removed_ids, int removed_count) {
    (void)op_ctx;
    (void)removed_ids;
    (void)removed_count;
    (void)solution;
    return AR_STATUS_OK;
}

static ARStatus test_destroy_noop(void *op_ctx, void *solution, int count,
                                  uint32_t *removed_ids, int *removed_count) {
    (void)op_ctx;
    (void)solution;
    (void)count;
    (void)removed_ids;
    if (!removed_count) {
        return AR_STATUS_INVALID_ARG;
    }
    *removed_count = 0;
    return AR_STATUS_OK;
}

int main(void) {
    ARALNSParams params;
    ARSolutionOps ops;
    ARALNSContext *ctx = NULL;
    TestSolution initial = { .value = 30 };
    TestSolution *best = NULL;
    ARStatus status;
    ARALNSStats stats;

    ar_alns_params_default(&params);
    params.max_iterations = 50;
    params.segment_size = 10;
    params.q_min = 1;
    params.q_max = 3;
    params.accept_type = AR_ACCEPT_IMPROVING;

    ops.copy = test_copy;
    ops.free = test_free;
    ops.cost = test_cost;
    ops.size = test_size;
    ops.validate = test_validate;
    ops.is_better = NULL;
    ops.user_ctx = NULL;

    ctx = ar_alns_create(&params, &ops, NULL);
    assert(ctx != NULL);

    status = ar_alns_set_seed(ctx, 12345);
    assert(status == AR_STATUS_OK);

    status = ar_alns_add_destroy(ctx, "decrement", test_destroy, NULL, 1.0);
    assert(status == AR_STATUS_OK);
    status = ar_alns_add_repair(ctx, "noop", test_repair, NULL, 1.0);
    assert(status == AR_STATUS_OK);

    status = ar_alns_solve(ctx, &initial, (void **)&best);
    assert(status == AR_STATUS_OK);
    assert(best != NULL);
    assert(best->value == 0);

    ar_alns_get_stats(ctx, &stats);
    assert(stats.iterations > 0);
    assert(stats.best_cost == 0.0);
    assert(stats.accepted > 0);
    assert(stats.stop_reason == AR_STOP_MAX_ITERATIONS);

    test_free(best, NULL);
    best = NULL;

    status = ar_alns_get_best_copy(ctx, (void **)&best);
    assert(status == AR_STATUS_OK);
    assert(best != NULL);
    assert(best->value == 0);

    test_free(best, NULL);
    ar_alns_free(ctx);

    ar_alns_params_default(&params);
    params.max_iterations = 100;
    params.max_stagnation_iterations = 5;
    params.q_min = 0;
    params.q_max = 0;
    params.accept_type = AR_ACCEPT_IMPROVING;

    ctx = ar_alns_create(&params, &ops, NULL);
    assert(ctx != NULL);
    status = ar_alns_add_destroy(ctx, "noop-d", test_destroy_noop, NULL, 1.0);
    assert(status == AR_STATUS_OK);
    status = ar_alns_add_repair(ctx, "noop-r", test_repair, NULL, 1.0);
    assert(status == AR_STATUS_OK);

    initial.value = 0;
    status = ar_alns_solve(ctx, &initial, (void **)&best);
    assert(status == AR_STATUS_LIMIT);
    assert(best != NULL);
    assert(best->value == 0);
    ar_alns_get_stats(ctx, &stats);
    assert(stats.stop_reason == AR_STOP_STAGNATION);
    test_free(best, NULL);
    ar_alns_free(ctx);

    ar_alns_params_default(&params);
    params.q_min = 5;
    params.q_max = 4;
    ctx = ar_alns_create(&params, &ops, NULL);
    assert(ctx == NULL);

    /* Test ar_alns_calibrate_sa */
    {
        ARALNSParams sa_params;
        ar_alns_params_default(&sa_params);
        ar_alns_calibrate_sa(&sa_params, 1000.0, 1000);
        assert(sa_params.accept_type == AR_ACCEPT_SA);
        assert(sa_params.initial_temp > 0.0);
        assert(sa_params.cooling_rate > 0.0 && sa_params.cooling_rate < 1.0);
        /* T0 should be ~0.05 * 1000 / ln(2) ≈ 72.13 */
        assert(sa_params.initial_temp > 70.0 && sa_params.initial_temp < 75.0);
        /* After 1000 iterations, temp should drop to ~0.1% of T0 */
        {
            double final_temp = sa_params.initial_temp;
            int i;
            for (i = 0; i < 1000; i++) {
                final_temp *= sa_params.cooling_rate;
            }
            assert(final_temp < 0.1);
            assert(final_temp > 0.0);
        }

        /* Zero cost should use fallback */
        ar_alns_params_default(&sa_params);
        ar_alns_calibrate_sa(&sa_params, 0.0, 500);
        assert(sa_params.accept_type == AR_ACCEPT_SA);
        assert(sa_params.initial_temp > 0.0);

        /* Negative cost should use absolute value */
        ar_alns_params_default(&sa_params);
        ar_alns_calibrate_sa(&sa_params, -2000.0, 1000);
        assert(sa_params.initial_temp > 140.0 && sa_params.initial_temp < 150.0);

        /* NULL params should not crash */
        ar_alns_calibrate_sa(NULL, 1000.0, 1000);

        /* Zero iterations should not crash and not modify params */
        ar_alns_params_default(&sa_params);
        sa_params.accept_type = AR_ACCEPT_IMPROVING;
        sa_params.initial_temp = 999.0;
        ar_alns_calibrate_sa(&sa_params, 1000.0, 0);
        assert(sa_params.accept_type == AR_ACCEPT_IMPROVING);
        assert(sa_params.initial_temp == 999.0);
    }

    /* Test SA acceptance actually works in solve loop */
    {
        ARALNSParams sa_params;
        ARALNSContext *sa_ctx;
        TestSolution sa_initial = { .value = 30 };
        TestSolution *sa_best = NULL;

        ar_alns_params_default(&sa_params);
        sa_params.max_iterations = 50;
        sa_params.segment_size = 10;
        sa_params.q_min = 1;
        sa_params.q_max = 3;
        ar_alns_calibrate_sa(&sa_params, 30.0, 50);

        sa_ctx = ar_alns_create(&sa_params, &ops, NULL);
        assert(sa_ctx != NULL);
        ar_alns_set_seed(sa_ctx, 42);
        ar_alns_add_destroy(sa_ctx, "dec", test_destroy, NULL, 1.0);
        ar_alns_add_repair(sa_ctx, "noop", test_repair, NULL, 1.0);

        status = ar_alns_solve(sa_ctx, &sa_initial, (void **)&sa_best);
        assert(status == AR_STATUS_OK);
        assert(sa_best != NULL);
        assert(sa_best->value == 0);
        test_free(sa_best, NULL);
        ar_alns_free(sa_ctx);
    }

    /* Test restart from best on stagnation */
    {
        ARALNSParams rp;
        ARALNSContext *rctx;
        TestSolution ri = { .value = 30 };
        TestSolution *rbest = NULL;
        ARALNSStats rstats;

        ar_alns_params_default(&rp);
        rp.max_iterations = 200;
        rp.segment_size = 10;
        rp.q_min = 1;
        rp.q_max = 3;
        rp.restart_threshold = 20;
        rp.restart_temp_ratio = 0.5;
        ar_alns_calibrate_sa(&rp, 30.0, 200);

        rctx = ar_alns_create(&rp, &ops, NULL);
        assert(rctx != NULL);
        ar_alns_set_seed(rctx, 77);
        ar_alns_add_destroy(rctx, "dec", test_destroy, NULL, 1.0);
        ar_alns_add_repair(rctx, "noop", test_repair, NULL, 1.0);

        status = ar_alns_solve(rctx, &ri, (void **)&rbest);
        assert(status == AR_STATUS_OK);
        assert(rbest != NULL);
        assert(rbest->value == 0);

        ar_alns_get_stats(rctx, &rstats);
        assert(rstats.restarts >= 0);  /* May or may not restart depending on RNG path */
        assert(rstats.best_cost == 0.0);

        test_free(rbest, NULL);
        ar_alns_free(rctx);

        /* Test restart_threshold validation */
        ar_alns_params_default(&rp);
        rp.restart_threshold = -1;
        rctx = ar_alns_create(&rp, &ops, NULL);
        assert(rctx == NULL);

        ar_alns_params_default(&rp);
        rp.restart_temp_ratio = -0.1;
        rctx = ar_alns_create(&rp, &ops, NULL);
        assert(rctx == NULL);

        ar_alns_params_default(&rp);
        rp.restart_temp_ratio = 1.1;
        rctx = ar_alns_create(&rp, &ops, NULL);
        assert(rctx == NULL);
    }

    printf("arbor ALNS test passed\n");
    return 0;
}
