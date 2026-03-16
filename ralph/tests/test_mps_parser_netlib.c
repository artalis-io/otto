/*
 * NETLIB MPS parser regression tests
 *
 * Focused on fixed-format edge cases:
 * - embedded spaces in symbol names (forplan)
 * - omitted BOUNDS set name (sierra)
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ralph_test_mod_api.h"

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        printf("PASS: %s\n", msg); \
    } else { \
        printf("FAIL: %s\n", msg); \
        tests_failed++; \
    } \
} while (0)

static int find_var(const RalphModel *model, const char *name) {
    if (!model || !name) return -1;
    int n = ralph_test_get_num_vars(model);
    for (int i = 0; i < n; i++) {
        const char *v = ralph_test_get_var_name(model, i);
        if (v && strcmp(v, name) == 0) return i;
    }
    return -1;
}

static void test_forplan_embedded_space_names(void) {
    printf("\n=== forplan.mps: embedded-space names ===\n");

    RalphModel *model = ralph_test_create();
    TEST(model != NULL, "model created");
    if (!model) return;

    int rc = ralph_test_read_mps(model, "benchmarks/netlib/forplan.mps");
    TEST(rc == 0, "forplan parsed successfully");
    if (rc == 0) {
        int nvars = ralph_test_get_num_vars(model);
        int ncons = ralph_test_get_num_cons(model);
        TEST(nvars > 0, "forplan has variables");
        TEST(ncons > 0, "forplan has constraints");

        int idx = find_var(model, "DEDO3 11");
        TEST(idx >= 0, "forplan keeps embedded-space column name");
        if (idx >= 0) {
            double lb = 0.0, ub = 0.0;
            int brc = ralph_test_get_var_bounds(model, idx, &lb, &ub);
            TEST(brc == 0, "forplan bounds query succeeds");
            if (brc == 0) {
                TEST(fabs(ub - 200000.0) < 1e-6, "forplan UP bound parsed on spaced name");
            }
        }
    }

    ralph_test_free(model);
}

static void test_sierra_omitted_bound_set_name(void) {
    printf("\n=== sierra.mps: omitted BOUNDS set name ===\n");

    RalphModel *model = ralph_test_create();
    TEST(model != NULL, "model created");
    if (!model) return;

    int rc = ralph_test_read_mps(model, "benchmarks/netlib/sierra.mps");
    TEST(rc == 0, "sierra parsed successfully");
    if (rc == 0) {
        int idx = find_var(model, "BWSI1T");
        TEST(idx >= 0, "sierra variable with omitted BNDNAME line exists");
        if (idx >= 0) {
            double lb = 0.0, ub = 0.0;
            int brc = ralph_test_get_var_bounds(model, idx, &lb, &ub);
            TEST(brc == 0, "sierra bounds query succeeds");
            if (brc == 0) {
                TEST(fabs(ub - 100000.0) < 1e-6, "sierra UP bound parsed without BNDNAME");
            }
        }
    }

    ralph_test_free(model);
}

static void test_e226_objective_constant_from_rhs(void) {
    printf("\n=== e226.mps: objective constant in RHS ===\n");

    RalphModel *model = ralph_test_create();
    TEST(model != NULL, "model created");
    if (!model) return;

    int rc = ralph_test_read_mps(model, "benchmarks/netlib/e226.mps");
    TEST(rc == 0, "e226 parsed successfully");
    if (rc == 0) {
        rc = ralph_test_optimize(model);
        TEST(rc == 0, "e226 optimize succeeds");
        if (rc == 0) {
            TEST(ralph_test_get_status(model) == RALPH_STATUS_OPTIMAL, "e226 status optimal");
            TEST(fabs(ralph_test_get_objval(model) - (-25.86492907)) < 1e-4,
                 "e226 objective includes RHS objective constant");
        }
    }

    ralph_test_free(model);
}

int main(void) {
    printf("=== NETLIB MPS Parser Regression Tests ===\n");

    test_forplan_embedded_space_names();
    test_sierra_omitted_bound_set_name();
    test_e226_objective_constant_from_rhs();

    printf("\nSummary: %d run, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
