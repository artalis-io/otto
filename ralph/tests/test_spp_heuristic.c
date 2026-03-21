/*
 * Focused tests for the standalone exact-cover SPP heuristic.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "lp.h"
#include "spp.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
        printf("  PASS: %s\n", msg); \
    } else { \
        printf("  FAIL: %s\n", msg); \
    } \
} while (0)

#define ASSERT_NEAR(a, b, tol, msg) do { \
    tests_run++; \
    if (fabs((a) - (b)) <= (tol)) { \
        tests_passed++; \
        printf("  PASS: %s (%.6f ~= %.6f)\n", msg, (double)(a), (double)(b)); \
    } else { \
        printf("  FAIL: %s (%.6f != %.6f)\n", msg, (double)(a), (double)(b)); \
    } \
} while (0)

static LPModel *build_optimal_fixture(void) {
    LPModel *model = lp_model_create();
    if (!model) return NULL;

    model->obj_sense = 1;

    lp_model_add_var(model, 0.0, 1.0, 8.0, 'B');  /* S0 covers rows 0,1 */
    lp_model_add_var(model, 0.0, 1.0, 6.0, 'B');  /* S1 covers rows 1,2 */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');  /* S2 covers row 2 */
    lp_model_add_var(model, 0.0, 1.0, 3.0, 'B');  /* S3 covers row 2 */

    {
        int idx[] = {0};
        double val[] = {1.0};
        lp_model_add_constraint(model, 1, idx, val, 'E', 1.0);
    }
    {
        int idx[] = {0, 1};
        double val[] = {1.0, 1.0};
        lp_model_add_constraint(model, 2, idx, val, 'E', 1.0);
    }
    {
        int idx[] = {1, 2, 3};
        double val[] = {1.0, 1.0, 1.0};
        lp_model_add_constraint(model, 3, idx, val, 'E', 1.0);
    }

    lp_model_finalize(model);
    return model;
}

static LPModel *build_infeasible_fixture(void) {
    LPModel *model = lp_model_create();
    if (!model) return NULL;

    model->obj_sense = 1;

    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S0 covers rows 0,1 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S1 covers rows 0,1 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S2 covers rows 1,2 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S3 covers rows 1,2 */

    {
        int idx[] = {0, 1};
        double val[] = {1.0, 1.0};
        lp_model_add_constraint(model, 2, idx, val, 'E', 1.0);
    }
    {
        int idx[] = {0, 1, 2, 3};
        double val[] = {1.0, 1.0, 1.0, 1.0};
        lp_model_add_constraint(model, 4, idx, val, 'E', 1.0);
    }
    {
        int idx[] = {2, 3};
        double val[] = {1.0, 1.0};
        lp_model_add_constraint(model, 2, idx, val, 'E', 1.0);
    }

    lp_model_finalize(model);
    return model;
}

static void test_spp_heuristic_finds_optimal_fixture(void) {
    LPModel *model = build_optimal_fixture();
    SPPContext ctx;
    SPPHeuristicStats stats;
    double lp_x[] = {0.95, 0.20, 0.75, 0.10};
    double sol[4] = {0.0, 0.0, 0.0, 0.0};
    double obj = 0.0;

    printf("\n=== Test: SPP Heuristic Finds Fixture Optimum ===\n");

    spp_context_init(&ctx);
    ASSERT(model != NULL, "Fixture model created");
    ASSERT(spp_context_build(model, &ctx) == 0, "SPP context built");

    memset(&stats, 0, sizeof(stats));
    stats.node_limit = 256;
    ASSERT(spp_heuristic_run(&ctx, lp_x, sol, &obj, &stats) == 0,
           "Heuristic found feasible exact-cover solution");
    ASSERT(spp_context_check_solution(&ctx, sol, 1e-9) == 1,
           "Heuristic solution satisfies exact cover");
    ASSERT_NEAR(obj, 10.0, 1e-9, "Heuristic objective matches optimum");
    ASSERT(sol[0] > 0.5 && sol[2] > 0.5 && sol[1] < 0.5 && sol[3] < 0.5,
           "Heuristic selected the expected optimal sets");
    ASSERT(stats.nodes_visited > 0, "Heuristic visited search nodes");
    ASSERT(stats.incumbent_updates > 0, "Heuristic recorded an incumbent");

    spp_context_free(&ctx);
    lp_model_free(model);
}

static void test_spp_heuristic_reports_no_solution(void) {
    LPModel *model = build_infeasible_fixture();
    SPPContext ctx;
    SPPHeuristicStats stats;
    double sol[4] = {0.0, 0.0, 0.0, 0.0};
    double obj = 0.0;

    printf("\n=== Test: SPP Heuristic Reports No Solution ===\n");

    spp_context_init(&ctx);
    ASSERT(model != NULL, "Infeasible fixture model created");
    ASSERT(spp_context_build(model, &ctx) == 0, "SPP context built");

    memset(&stats, 0, sizeof(stats));
    stats.node_limit = 256;
    ASSERT(spp_heuristic_run(&ctx, NULL, sol, &obj, &stats) == -1,
           "Heuristic reports no feasible exact cover");
    ASSERT(stats.branch_failures > 0 || stats.nodes_visited > 0,
           "Heuristic explored and rejected branches");

    spp_context_free(&ctx);
    lp_model_free(model);
}

int main(void) {
    printf("Ralph SPP Heuristic Tests\n");

    test_spp_heuristic_finds_optimal_fixture();
    test_spp_heuristic_reports_no_solution();

    printf("\n=== Summary ===\n");
    printf("Passed %d/%d tests\n", tests_passed, tests_run);

    return (tests_run == tests_passed) ? 0 : 1;
}
