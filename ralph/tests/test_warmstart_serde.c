/*
 * Ralph - P4 Warm-Start Serialization Tests
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

static RalphModel* build_lp_model(void) {
    RalphModel *model = ralph_test_create();
    if (!model) return NULL;
    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    int x = ralph_test_add_var(model, 0.0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
    int y = ralph_test_add_var(model, 0.0, RALPH_INFINITY, -1.0, RALPH_CONTINUOUS);
    if (x != 0 || y != 1) {
        ralph_test_free(model);
        return NULL;
    }
    {
        int idx[] = {x, y};
        double val[] = {1.0, 1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 4.0);
    }
    {
        int idx[] = {x};
        double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_LESS_EQUAL, 2.0);
    }
    {
        int idx[] = {y};
        double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_LESS_EQUAL, 3.0);
    }
    return model;
}

static RalphModel* build_mip_model(void) {
    RalphModel *model = ralph_test_create();
    if (!model) return NULL;
    ralph_test_set_obj_sense(model, RALPH_MAXIMIZE);

    int x = ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);
    int y = ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);
    if (x != 0 || y != 1) {
        ralph_test_free(model);
        return NULL;
    }
    {
        int idx[] = {x, y};
        double val[] = {1.0, 1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 1.0);
    }
    return model;
}

int main(void) {
    const char *basis_file = "/tmp/ralph_basis_v1.chk";
    const char *mip_file = "/tmp/ralph_mipstart_v1.chk";

    /* Basis serialization round-trip */
    {
        RalphModel *src = build_lp_model();
        ASSERT(src != NULL, "LP source model created");
        if (!src) return 1;
        ASSERT(ralph_test_optimize(src) == 0, "LP source optimize succeeds");
        ASSERT(ralph_test_get_status(src) == RALPH_STATUS_OPTIMAL, "LP source OPTIMAL");

        RalphBasis *basis = ralph_test_save_basis(src);
        ASSERT(basis != NULL, "LP basis saved");
        ASSERT(ralph_test_write_basis_file(basis, basis_file) == 0, "Basis written to file");
        ralph_test_free_basis(basis);
        ralph_test_free(src);

        RalphBasis *loaded = ralph_test_read_basis_file(basis_file);
        ASSERT(loaded != NULL, "Basis loaded from file");

        RalphModel *dst = build_lp_model();
        ASSERT(dst != NULL, "LP destination model created");
        ASSERT(ralph_test_load_basis(dst, loaded) == 0, "Loaded basis staged/applied");
        ASSERT(ralph_test_optimize(dst) == 0, "LP destination optimize succeeds");
        ASSERT(ralph_test_get_status(dst) == RALPH_STATUS_OPTIMAL, "LP destination OPTIMAL");
        ASSERT_NEAR(ralph_test_get_objval(dst), -4.0, 1e-9, "Basis round-trip objective");

        ralph_test_free(dst);
        ralph_test_free_basis(loaded);
    }

    /* MIP-start serialization round-trip */
    {
        RalphModel *src = build_mip_model();
        ASSERT(src != NULL, "MIP source model created");
        if (!src) return 1;

        {
            int idx[] = {0};
            double val[] = {1.0};
            ASSERT(ralph_test_set_mip_start_sparse(src, 1, idx, val) == 0,
                   "MIP sparse start set");
        }
        ASSERT(ralph_test_set_mip_start_repair_mode(src, RALPH_MIP_START_REPAIR_PROJECT_AND_ROUND) == 0,
               "MIP repair mode set");
        ASSERT(ralph_test_write_mip_start_file(src, mip_file) == 0, "MIP start written to file");
        ralph_test_free(src);

        RalphModel *dst = build_mip_model();
        ASSERT(dst != NULL, "MIP destination model created");
        ASSERT(ralph_test_read_mip_start_file(dst, mip_file) == 0, "MIP start loaded from file");
        ASSERT(ralph_test_get_mip_start_status(dst) == RALPH_MIP_START_PENDING,
               "Loaded MIP start status pending");
        ASSERT(ralph_test_get_mip_start_repair_mode(dst) == RALPH_MIP_START_REPAIR_PROJECT_AND_ROUND,
               "Loaded MIP repair mode restored");
        ASSERT(ralph_test_optimize(dst) == 0, "MIP destination optimize succeeds");
        ASSERT(ralph_test_get_status(dst) == RALPH_STATUS_OPTIMAL, "MIP destination OPTIMAL");
        ASSERT(ralph_test_get_mip_start_status(dst) == RALPH_MIP_START_ACCEPTED,
               "Loaded MIP start accepted");
        ASSERT_NEAR(ralph_test_get_objval(dst), 1.0, 1e-9, "MIP round-trip objective");
        ralph_test_free(dst);
    }

    (void)remove(basis_file);
    (void)remove(mip_file);

    printf("Warm-start serialization tests: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}

