/*
 * Focused tests for the standalone exact-cover set partitioning context.
 */

#include <stdio.h>
#include <stdlib.h>
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

static LPModel *build_exact_cover_fixture(void) {
    LPModel *model = lp_model_create();
    if (!model) return NULL;

    model->obj_sense = 1;

    /* S0 covers rows 0,1; S1 covers rows 1,2; S2 covers row 2; S3 covers row 2 */
    lp_model_add_var(model, 0.0, 1.0, 8.0, 'B');
    lp_model_add_var(model, 0.0, 1.0, 6.0, 'B');
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');
    lp_model_add_var(model, 0.0, 1.0, 3.0, 'B');

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

static void test_build_exact_cover_context(void) {
    printf("\n=== Test: Build Exact-Cover Context ===\n");

    LPModel *model = build_exact_cover_fixture();
    SPPContext ctx;
    double feasible_x[] = {1.0, 0.0, 1.0, 0.0};
    double infeasible_x[] = {1.0, 1.0, 0.0, 0.0};

    spp_context_init(&ctx);
    ASSERT(model != NULL, "Fixture model created");
    ASSERT(spp_context_build(model, &ctx) == 0, "SPP context built");
    ASSERT(ctx.type == RALPH_SETCOVER_PARTITIONING, "Context type is partitioning");
    ASSERT(ctx.num_rows == 3, "3 exact-cover rows");
    ASSERT(ctx.num_sets == 4, "4 sets");
    ASSERT(ctx.nnz == 6, "6 incidences");
    ASSERT(ctx.num_conflict_edges == 4, "4 undirected conflict edges");

    ASSERT(ctx.row_ptr[0] == 0 && ctx.row_ptr[1] == 1 &&
           ctx.row_ptr[2] == 3 && ctx.row_ptr[3] == 6,
           "Row pointers are consistent");
    ASSERT(ctx.row_order[0] == 0 && ctx.row_order[1] == 1 && ctx.row_order[2] == 2,
           "Rows ordered by ascending degree");
    ASSERT(ctx.set_order[0] == 2 && ctx.set_order[1] == 3 &&
           ctx.set_order[2] == 1 && ctx.set_order[3] == 0,
           "Sets ordered by ascending cost");

    ASSERT(spp_context_sets_conflict(&ctx, 0, 1) == 1, "S0 conflicts with S1");
    ASSERT(spp_context_sets_conflict(&ctx, 1, 2) == 1, "S1 conflicts with S2");
    ASSERT(spp_context_sets_conflict(&ctx, 1, 3) == 1, "S1 conflicts with S3");
    ASSERT(spp_context_sets_conflict(&ctx, 2, 3) == 1, "S2 conflicts with S3");
    ASSERT(spp_context_sets_conflict(&ctx, 0, 2) == 0, "S0 does not conflict with S2");

    ASSERT(spp_context_check_solution(&ctx, feasible_x, 1e-9) == 1,
           "Feasible exact-cover solution accepted");
    ASSERT(spp_context_check_solution(&ctx, infeasible_x, 1e-9) == 0,
           "Overcovering solution rejected");
    ASSERT_NEAR(spp_context_compute_objective(&ctx, feasible_x), 10.0, 1e-9,
                "Objective computed from context");

    spp_context_free(&ctx);
    lp_model_free(model);
}

static void test_reject_covering_model(void) {
    printf("\n=== Test: Reject Covering Model ===\n");

    LPModel *model = lp_model_create();
    SPPContext ctx;

    spp_context_init(&ctx);
    ASSERT(model != NULL, "Model created");
    model->obj_sense = 1;
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');
    {
        int idx[] = {0, 1};
        double val[] = {1.0, 1.0};
        lp_model_add_constraint(model, 2, idx, val, 'G', 1.0);
    }
    lp_model_finalize(model);

    ASSERT(spp_context_build(model, &ctx) == -1, "Covering model rejected by SPP context");

    spp_context_free(&ctx);
    lp_model_free(model);
}

static void test_reject_non_unit_rhs_partitioning(void) {
    printf("\n=== Test: Reject Non-Unit RHS Partitioning ===\n");

    LPModel *model = lp_model_create();
    SPPContext ctx;

    spp_context_init(&ctx);
    ASSERT(model != NULL, "Model created");
    model->obj_sense = 1;
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');
    {
        int idx[] = {0, 1};
        double val[] = {1.0, 1.0};
        lp_model_add_constraint(model, 2, idx, val, 'E', 2.0);
    }
    lp_model_finalize(model);

    ASSERT(spp_context_build(model, &ctx) == -1,
           "Partitioning rows with RHS != 1 are rejected");

    spp_context_free(&ctx);
    lp_model_free(model);
}

int main(void) {
    printf("Ralph SPP Context Tests\n");

    test_build_exact_cover_context();
    test_reject_covering_model();
    test_reject_non_unit_rhs_partitioning();

    printf("\n=== Summary ===\n");
    printf("Passed %d/%d tests\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
