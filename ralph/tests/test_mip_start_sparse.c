/*
 * Ralph - P1 Sparse/Partial MIP Start Tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "ralph_test_mod_api.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
    } else { \
        printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    } \
} while (0)

#define ASSERT_NEAR(a, b, tol, msg) \
    ASSERT(fabs((a) - (b)) <= (tol), msg)

static RalphModel* build_binary_pair_model(void) {
    RalphModel *model = ralph_test_create();
    if (!model) return NULL;

    ralph_test_set_obj_sense(model, RALPH_MAXIMIZE);
    int x = ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);
    int y = ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);
    if (x != 0 || y != 1) {
        ralph_test_free(model);
        return NULL;
    }

    /* x + y <= 1 */
    {
        int idx[] = {x, y};
        double val[] = {1.0, 1.0};
        if (ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 1.0) < 0) {
            ralph_test_free(model);
            return NULL;
        }
    }

    return model;
}

int main(void) {
    RalphModel *model = build_binary_pair_model();
    ASSERT(model != NULL, "Model created");
    if (!model) return 1;

    /* Partial start: specify only x=1. y defaults to lb=0. */
    {
        int idx[] = {0};
        double val[] = {1.0};
        ASSERT(ralph_test_set_mip_start_sparse(model, 1, idx, val) == 0,
               "Sparse MIP start set succeeds");
        ASSERT(ralph_test_get_mip_start_status(model) == RALPH_MIP_START_PENDING,
               "Sparse start status pending");
    }

    ASSERT(ralph_test_optimize(model) == 0, "Optimize succeeds with sparse start");
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Status OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(model), 1.0, 1e-9, "Objective = 1");
    ASSERT(ralph_test_get_mip_start_status(model) == RALPH_MIP_START_ACCEPTED,
           "Sparse start accepted");

    /* Update sparse start to infeasible x=1,y=1 -> should reject cleanly. */
    {
        int idx[] = {1};
        double val[] = {1.0};
        ASSERT(ralph_test_set_mip_start_sparse(model, 1, idx, val) == 0,
               "Sparse incremental update succeeds");
    }
    ASSERT(ralph_test_optimize(model) == 0, "Optimize succeeds after infeasible sparse start");
    ASSERT(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "Status still OPTIMAL");
    ASSERT(ralph_test_get_mip_start_status(model) == RALPH_MIP_START_REJECTED,
           "Infeasible sparse start rejected");

    /* Input validation */
    {
        int idx[] = {99};
        double val[] = {0.0};
        ASSERT(ralph_test_set_mip_start_sparse(model, 1, idx, val) == -1,
               "Sparse start rejects invalid index");
    }

    ralph_test_free(model);

    printf("MIP sparse-start tests: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}

