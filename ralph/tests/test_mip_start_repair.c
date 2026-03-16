/*
 * Ralph - P2 MIP Start Repair-Mode Tests
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

static RalphModel* build_model(void) {
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
    /* Strict mode: fractional start should reject. */
    {
        RalphModel *m = build_model();
        ASSERT(m != NULL, "Strict model created");
        if (!m) return 1;

        ASSERT(ralph_test_set_mip_start_repair_mode(m, RALPH_MIP_START_REPAIR_STRICT) == 0,
               "Set strict repair mode");
        ASSERT(ralph_test_get_mip_start_repair_mode(m) == RALPH_MIP_START_REPAIR_STRICT,
               "Get strict repair mode");

        {
            double s[] = {0.6, 0.2};
            ASSERT(ralph_test_set_mip_start(m, s) == 0, "Set fractional start");
        }
        ASSERT(ralph_test_optimize(m) == 0, "Strict optimize succeeds");
        ASSERT(ralph_test_get_status(m) == RALPH_STATUS_OPTIMAL, "Strict OPTIMAL");
        ASSERT(ralph_test_get_mip_start_status(m) == RALPH_MIP_START_REJECTED,
               "Strict mode rejects fractional start");
        ASSERT_NEAR(ralph_test_get_objval(m), 1.0, 1e-9, "Strict objective correct");

        ralph_test_free(m);
    }

    /* Project-bounds mode: out-of-bounds start clamps and may accept. */
    {
        RalphModel *m = build_model();
        ASSERT(m != NULL, "Project model created");
        if (!m) return 1;

        ASSERT(ralph_test_set_mip_start_repair_mode(m, RALPH_MIP_START_REPAIR_PROJECT_BOUNDS) == 0,
               "Set project-bounds mode");
        {
            double s[] = {1.7, -0.4};  /* clamps to (1,0) */
            ASSERT(ralph_test_set_mip_start(m, s) == 0, "Set out-of-bounds start");
        }
        ASSERT(ralph_test_optimize(m) == 0, "Project optimize succeeds");
        ASSERT(ralph_test_get_status(m) == RALPH_STATUS_OPTIMAL, "Project OPTIMAL");
        ASSERT(ralph_test_get_mip_start_status(m) == RALPH_MIP_START_ACCEPTED,
               "Project-bounds mode accepts clamped start");
        ASSERT_NEAR(ralph_test_get_objval(m), 1.0, 1e-9, "Project objective correct");

        ralph_test_free(m);
    }

    /* Project+round mode: in-bounds fractional start rounds and may accept. */
    {
        RalphModel *m = build_model();
        ASSERT(m != NULL, "Round model created");
        if (!m) return 1;

        ASSERT(ralph_test_set_mip_start_repair_mode(m, RALPH_MIP_START_REPAIR_PROJECT_AND_ROUND) == 0,
               "Set project+round mode");
        {
            double s[] = {0.6, 0.2};  /* rounds to (1,0) */
            ASSERT(ralph_test_set_mip_start(m, s) == 0, "Set fractional start for round mode");
        }
        ASSERT(ralph_test_optimize(m) == 0, "Round optimize succeeds");
        ASSERT(ralph_test_get_status(m) == RALPH_STATUS_OPTIMAL, "Round OPTIMAL");
        ASSERT(ralph_test_get_mip_start_status(m) == RALPH_MIP_START_ACCEPTED,
               "Project+round accepts rounded start");
        ASSERT_NEAR(ralph_test_get_objval(m), 1.0, 1e-9, "Round objective correct");

        ralph_test_free(m);
    }

    {
        RalphModel *m = build_model();
        ASSERT(m != NULL, "Validation model created");
        if (!m) return 1;
        ASSERT(ralph_test_set_mip_start_repair_mode(m, (RalphMIPStartRepairMode)99) == -1,
               "Invalid repair mode rejected");
        ralph_test_free(m);
    }

    printf("MIP start-repair tests: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}

