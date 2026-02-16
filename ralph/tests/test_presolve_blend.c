/*
 * Regression test for blend - a NETLIB blending problem (Bruce Murtagh).
 *
 * blend is a small problem: 83 variables, 74 constraints.
 * Known optimal objective: -30.812149846 (NETLIB reference).
 *
 * Previously failed due to duplicate entries in MPS not being merged
 * by triplets_to_csc(). Now fixed: duplicates are summed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ralph.h"

#define BLEND_OPT -30.812149846

static int test_count = 0;
static int pass_count = 0;

#define TEST(cond, msg) do { \
    test_count++; \
    if (cond) { pass_count++; printf("  PASS: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); } \
} while(0)

int main(void) {
    printf("\n=== Test: NETLIB blend ===\n\n");

    const char *mps_path = "benchmarks/netlib/blend.mps";

    /* Test WITHOUT presolve */
    printf("--- Test 1: Solve WITHOUT presolve ---\n");
    {
        RalphModel *model = ralph_create();
        if (!model) { printf("Failed to create model\n"); return 1; }

        int load_ret = ralph_read_mps(model, mps_path);
        if (load_ret != 0) {
            printf("Failed to load %s (error %d)\n", mps_path, load_ret);
            ralph_free(model);
            return 1;
        }
        TEST(load_ret == 0, "Loaded blend.mps successfully");

        ralph_set_int_param(model, "presolve", 0);
        ralph_set_int_param(model, "verbose", 0);

        ralph_optimize(model);
        RalphStatus status = ralph_get_status(model);
        int iters = ralph_get_iterations(model);
        printf("  Status: %s, Iterations: %d\n", ralph_status_string(status), iters);

        TEST(status == RALPH_STATUS_OPTIMAL, "blend returns OPTIMAL without presolve");

        if (status == RALPH_STATUS_OPTIMAL) {
            double obj = ralph_get_objval(model);
            double rel_err = fabs(obj - BLEND_OPT) / (fabs(BLEND_OPT) + 1e-10);
            printf("  Objective: %.10f (expected: %.10f, rel_err: %.2e)\n",
                   obj, BLEND_OPT, rel_err);
            TEST(rel_err < 1e-4, "Objective within 0.01% of NETLIB reference");
        }

        ralph_free(model);
    }

    /* Test WITH presolve */
    printf("\n--- Test 2: Solve WITH presolve ---\n");
    {
        RalphModel *model = ralph_create();
        if (!model) { printf("Failed to create model\n"); return 1; }

        int load_ret = ralph_read_mps(model, mps_path);
        if (load_ret != 0) {
            printf("Failed to reload %s\n", mps_path);
            ralph_free(model);
            return 1;
        }

        ralph_set_int_param(model, "presolve", 1);
        ralph_set_int_param(model, "verbose", 0);

        ralph_optimize(model);
        RalphStatus status = ralph_get_status(model);
        int iters = ralph_get_iterations(model);
        printf("  Status: %s, Iterations: %d\n", ralph_status_string(status), iters);

        TEST(status == RALPH_STATUS_OPTIMAL, "blend returns OPTIMAL with presolve");

        if (status == RALPH_STATUS_OPTIMAL) {
            double obj = ralph_get_objval(model);
            double rel_err = fabs(obj - BLEND_OPT) / (fabs(BLEND_OPT) + 1e-10);
            printf("  Objective: %.10f (expected: %.10f, rel_err: %.2e)\n",
                   obj, BLEND_OPT, rel_err);
            TEST(rel_err < 1e-4, "Objective within 0.01% of NETLIB reference (presolve)");
        }

        ralph_free(model);
    }

    printf("\n══════════════════════════════════════════════════════════\n");
    printf("Test Summary: %d/%d passed (%.1f%%)\n", pass_count, test_count,
           100.0 * pass_count / test_count);

    if (pass_count == test_count) {
        printf("\n✓ All tests passed!\n");
        return 0;
    } else {
        printf("\n✗ Some tests failed\n");
        return 1;
    }
}
