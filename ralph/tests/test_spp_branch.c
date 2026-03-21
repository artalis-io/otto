/*
 * Focused tests for standalone SPP propagation and row-based branching.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static LPModel *build_chain_fixture(void) {
    LPModel *model = lp_model_create();
    if (!model) return NULL;

    model->obj_sense = 1;

    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S0: row 0 */
    lp_model_add_var(model, 0.0, 1.0, 2.0, 'B');  /* S1: rows 0,1 */
    lp_model_add_var(model, 0.0, 1.0, 3.0, 'B');  /* S2: rows 1,2 */
    lp_model_add_var(model, 0.0, 1.0, 4.0, 'B');  /* S3: row 2 */

    {
        int idx[] = {0, 1};
        double val[] = {1.0, 1.0};
        lp_model_add_constraint(model, 2, idx, val, 'E', 1.0);
    }
    {
        int idx[] = {1, 2};
        double val[] = {1.0, 1.0};
        lp_model_add_constraint(model, 2, idx, val, 'E', 1.0);
    }
    {
        int idx[] = {2, 3};
        double val[] = {1.0, 1.0};
        lp_model_add_constraint(model, 2, idx, val, 'E', 1.0);
    }

    lp_model_finalize(model);
    return model;
}

static LPModel *build_branch_fixture(void) {
    LPModel *model = lp_model_create();
    if (!model) return NULL;

    model->obj_sense = 1;

    for (int j = 0; j < 6; j++) {
        lp_model_add_var(model, 0.0, 1.0, (double)(j + 1), 'B');
    }

    {
        int idx[] = {0, 1, 2};
        double val[] = {1.0, 1.0, 1.0};
        lp_model_add_constraint(model, 3, idx, val, 'E', 1.0);
    }
    {
        int idx[] = {2, 3};
        double val[] = {1.0, 1.0};
        lp_model_add_constraint(model, 2, idx, val, 'E', 1.0);
    }
    {
        int idx[] = {1, 4, 5};
        double val[] = {1.0, 1.0, 1.0};
        lp_model_add_constraint(model, 3, idx, val, 'E', 1.0);
    }

    lp_model_finalize(model);
    return model;
}

static void test_spp_propagation_cascades_fixings(void) {
    LPModel *model = build_chain_fixture();
    SPPContext ctx;
    double lb[] = {1.0, 0.0, 0.0, 0.0};
    double ub[] = {1.0, 1.0, 1.0, 1.0};
    int fixings = 0;

    printf("\n=== Test: SPP Propagation Cascades Fixings ===\n");

    spp_context_init(&ctx);
    ASSERT(model != NULL, "Chain fixture model created");
    ASSERT(spp_context_build(model, &ctx) == 0, "SPP context built");
    ASSERT(spp_propagate_bounds(&ctx, lb, ub, &fixings) == 0,
           "Propagation reached a feasible fixpoint");
    ASSERT(fixings >= 3, "Propagation applied cascading fixings");
    ASSERT(lb[0] > 0.5 && ub[1] < 0.5 && lb[2] > 0.5 && ub[3] < 0.5,
           "Propagation forced the expected chain decisions");

    spp_context_free(&ctx);
    lp_model_free(model);
}

static void test_spp_propagation_detects_conflict(void) {
    LPModel *model = build_chain_fixture();
    SPPContext ctx;
    double lb[] = {1.0, 1.0, 0.0, 0.0};
    double ub[] = {1.0, 1.0, 1.0, 1.0};
    int fixings = 0;

    printf("\n=== Test: SPP Propagation Detects Conflict ===\n");

    spp_context_init(&ctx);
    ASSERT(model != NULL, "Chain fixture model created");
    ASSERT(spp_context_build(model, &ctx) == 0, "SPP context built");
    ASSERT(spp_propagate_bounds(&ctx, lb, ub, &fixings) == -1,
           "Propagation rejects conflicting fixed selections");

    spp_context_free(&ctx);
    lp_model_free(model);
}

static void test_spp_branch_selection_prefers_tight_row(void) {
    LPModel *model = build_branch_fixture();
    SPPContext ctx;
    double lb[6];
    double ub[6];
    double lp_x[] = {0.10, 0.20, 0.70, 0.30, 0.50, 0.30};
    int row = -1;
    int set = -1;

    printf("\n=== Test: SPP Branch Selection Prefers Tight Row ===\n");

    memset(lb, 0, sizeof(lb));
    for (int j = 0; j < 6; j++) ub[j] = 1.0;

    spp_context_init(&ctx);
    ASSERT(model != NULL, "Branch fixture model created");
    ASSERT(spp_context_build(model, &ctx) == 0, "SPP context built");
    ASSERT(spp_select_branch_set(&ctx, lb, ub, lp_x, &row, &set) == 0,
           "Branch selector returned a valid row/set");
    ASSERT(row == 1, "Branch selector chose the row with two remaining candidates");
    ASSERT(set == 2, "Branch selector chose the highest-LP set in that row");

    spp_context_free(&ctx);
    lp_model_free(model);
}

int main(void) {
    printf("Ralph SPP Branch Tests\n");

    test_spp_propagation_cascades_fixings();
    test_spp_propagation_detects_conflict();
    test_spp_branch_selection_prefers_tight_row();

    printf("\n=== Summary ===\n");
    printf("Passed %d/%d tests\n", tests_passed, tests_run);

    return (tests_run == tests_passed) ? 0 : 1;
}
