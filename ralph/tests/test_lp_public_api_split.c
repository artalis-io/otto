/*
 * Modular LP public API split test.
 *
 * Verifies that LP users can model/solve/query/basis-warmstart using
 * ralph_lp.h without depending on MIP headers.
 */

#include <math.h>
#include <stdio.h>

#include "ralph_lp.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT_TRUE(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s\n", msg); \
    } \
} while (0)

#define ASSERT_NEAR(a, b, tol, msg) do { \
    tests_run++; \
    if (fabs((a) - (b)) <= (tol)) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%.12g vs %.12g)\n", msg, (double)(a), (double)(b)); \
    } \
} while (0)

static void test_lp_modular_header_and_wrappers(void) {
    RalphLPModel *model = ralph_lp_create();
    RalphAPIError api_err;
    double x[2] = {0.0, 0.0};
    double y[2] = {0.0, 0.0};
    double rc[2] = {0.0, 0.0};
    RalphLPBasis *basis = NULL;
    RalphLPBasisStatus col_status[2];
    RalphLPBasisStatus row_status[2];
    char sol_buf[1024];
    int bytes = 0;

    ASSERT_TRUE(model != NULL, "lp modular: create");
    if (!model) return;

    ASSERT_TRUE(ralph_lp_set_int_param(model, "detect_special", 0) == 0,
                "lp modular: set detect_special");
    ASSERT_TRUE(ralph_lp_set_int_param(model, "presolve", 0) == 0,
                "lp modular: set presolve");
    ASSERT_TRUE(ralph_lp_set_obj_sense(model, RALPH_LP_OBJ_MINIMIZE) == 0,
                "lp modular: set objective sense");
    ASSERT_TRUE(ralph_lp_get_last_error(model, NULL) == -1,
                "lp modular: get_last_error rejects NULL output");
    ASSERT_TRUE(ralph_lp_get_last_error(model, &api_err) == -1,
                "lp modular: no model error initially");
    ASSERT_TRUE(ralph_lp_get_last_error(NULL, &api_err) == -1,
                "lp modular: no TLS error initially");
    ASSERT_TRUE(ralph_lp_clear_error(model) == 0,
                "lp modular: clear model error");
    ASSERT_TRUE(ralph_lp_clear_error(NULL) == 0,
                "lp modular: clear TLS error");
    ASSERT_TRUE(ralph_lp_error_domain_string(RALPH_ERROR_DOMAIN_ARGUMENT) != NULL,
                "lp modular: domain string accessor");
    ASSERT_TRUE(ralph_lp_error_code_string(RALPH_ERROR_CODE_INVALID_ARGUMENT) != NULL,
                "lp modular: code string accessor");
    ASSERT_TRUE(ralph_lp_error_api_string(RALPH_ERROR_API_MODEL_BUILD) != NULL,
                "lp modular: api string accessor");
    ASSERT_TRUE(ralph_lp_error_message(NULL)[0] == '\0',
                "lp modular: message accessor on NULL");

    ASSERT_TRUE(ralph_lp_add_var(model, 0.0, RALPH_LP_INFINITY, 1.0, RALPH_LP_VAR_CONTINUOUS) == 0,
                "lp modular: add x");
    ASSERT_TRUE(ralph_lp_add_var(model, 0.0, RALPH_LP_INFINITY, 1.0, RALPH_LP_VAR_CONTINUOUS) == 1,
                "lp modular: add y");

    {
        int idx[] = {0};
        double val[] = {1.0};
        ASSERT_TRUE(ralph_lp_add_constraint(model, 1, idx, val, RALPH_LP_SENSE_GREATER_EQUAL, 1.0) == 0,
                    "lp modular: add c0");
    }
    {
        int idx[] = {1};
        double val[] = {1.0};
        ASSERT_TRUE(ralph_lp_add_constraint(model, 1, idx, val, RALPH_LP_SENSE_GREATER_EQUAL, 2.0) == 1,
                    "lp modular: add c1");
    }

    ASSERT_TRUE(ralph_lp_optimize(model) == 0, "lp modular: optimize");
    ASSERT_TRUE(ralph_lp_get_status(model) == RALPH_LP_STATUS_OPTIMAL,
                "lp modular: optimal status");
    ASSERT_NEAR(ralph_lp_get_objval(model), 3.0, 1e-9, "lp modular: objective");

    ASSERT_TRUE(ralph_lp_get_solution(model, x) == 0, "lp modular: primal solution");
    ASSERT_NEAR(x[0], 1.0, 1e-9, "lp modular: x");
    ASSERT_NEAR(x[1], 2.0, 1e-9, "lp modular: y");

    ASSERT_TRUE(ralph_lp_get_dual_solution(model, y) == 0, "lp modular: dual solution");
    ASSERT_TRUE(ralph_lp_get_reduced_costs(model, rc) == 0, "lp modular: reduced costs");
    ASSERT_TRUE(ralph_lp_get_iterations(model) >= 0, "lp modular: iteration count");

    basis = ralph_lp_save_basis(model);
    ASSERT_TRUE(basis != NULL, "lp modular: save basis");
    if (basis) {
        ASSERT_TRUE(ralph_lp_get_basis_status(model, col_status, row_status) == 0,
                    "lp modular: get basis status");
        ASSERT_TRUE(ralph_lp_set_basis_status(model, col_status, row_status) == 0,
                    "lp modular: set basis status");
        ASSERT_TRUE(ralph_lp_load_basis(model, basis) == 0,
                    "lp modular: load basis");
    }

    bytes = ralph_lp_write_solution_buf(model, sol_buf, sizeof(sol_buf));
    ASSERT_TRUE(bytes > 0, "lp modular: solution buffer write");

    ralph_lp_free_basis(basis);
    ralph_lp_free(model);
}

int main(void) {
    printf("Running modular LP API split tests...\n");
    test_lp_modular_header_and_wrappers();
    printf("LP modular tests: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
