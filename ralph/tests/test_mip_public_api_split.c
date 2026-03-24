/*
 * Modular MIP public API split test.
 *
 * Verifies that MIP users can consume ralph_mip.h (which depends on LP
 * modeling APIs) without including the monolithic ralph.h surface.
 */

#include <math.h>
#include <stdio.h>

#include "ralph_mip.h"

static int tests_run = 0;
static int tests_passed = 0;
static int public_cut_cb_calls = 0;

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

static int test_cut_cb_generate(
    void *user_data,
    const double *x_relaxation,
    int num_vars,
    RalphMIPCut *cuts,
    int max_cuts) {
    int *threshold_signaled = (int *)user_data;
    (void)x_relaxation;
    (void)num_vars;
    (void)cuts;
    (void)max_cuts;
    public_cut_cb_calls++;
    if (threshold_signaled) *threshold_signaled = 1;
    return 0;
}

static int test_branch_cb_select(
    void *user_data,
    const double *x_relaxation,
    int num_vars,
    const int *is_integer,
    const double *lb,
    const double *ub) {
    (void)user_data;
    (void)x_relaxation;
    (void)num_vars;
    (void)is_integer;
    (void)lb;
    (void)ub;
    return -1;
}

static void test_mip_modular_header_and_wrappers(void) {
    RalphMIPModel *model = ralph_mip_create();
    RalphAPIError api_err;
    RalphMIPBranchDirection branch_dir[2] = {
        RALPH_MIP_BRANCH_DOWN,
        RALPH_MIP_BRANCH_UP
    };
    int priorities[2] = {10, 5};
    double warm_start[2] = {0.0, 1.0};
    double x[2] = {0.0, 0.0};
    RalphMIPCutCallback cut_cb;
    RalphMIPBranchCallback branch_cb;

    ASSERT_TRUE(model != NULL, "mip modular: create");
    if (!model) return;

    ASSERT_TRUE(ralph_mip_set_int_param(model, "detect_special", 0) == 0,
                "mip modular: set detect_special");
    ASSERT_TRUE(ralph_mip_set_int_param(model, "presolve", 0) == 0,
                "mip modular: set presolve");
    ASSERT_TRUE(ralph_lp_set_obj_sense(model, RALPH_LP_OBJ_MAXIMIZE) == 0,
                "mip modular: set objective sense");
    ASSERT_TRUE(ralph_mip_get_last_error(model, NULL) == -1,
                "mip modular: get_last_error rejects NULL output");
    ASSERT_TRUE(ralph_mip_get_last_error(model, &api_err) == -1,
                "mip modular: no model error initially");
    ASSERT_TRUE(ralph_mip_clear_error(model) == 0,
                "mip modular: clear model error");

    ASSERT_TRUE(ralph_lp_add_var(model, 0.0, 1.0, 3.0, RALPH_LP_VAR_BINARY) == 0,
                "mip modular: add x");
    ASSERT_TRUE(ralph_lp_add_var(model, 0.0, 1.0, 2.0, RALPH_LP_VAR_BINARY) == 1,
                "mip modular: add y");
    {
        int idx[] = {0, 1};
        double val[] = {1.0, 1.0};
        ASSERT_TRUE(ralph_lp_add_constraint(model, 2, idx, val, RALPH_LP_SENSE_LESS_EQUAL, 1.0) == 0,
                    "mip modular: add knapsack row");
    }

    ASSERT_TRUE(ralph_mip_set_branch_priorities(model, priorities) == 0,
                "mip modular: set branch priorities");
    ASSERT_TRUE(ralph_mip_set_branch_directions(model, branch_dir) == 0,
                "mip modular: set branch directions");
    ASSERT_TRUE(ralph_mip_set_start(model, warm_start) == 0,
                "mip modular: set warm start");
    ASSERT_TRUE(ralph_mip_get_start_status(model) == RALPH_MIP_START_STATUS_PENDING,
                "mip modular: warm start pending");
    ASSERT_TRUE(ralph_mip_set_start_repair_mode(model, RALPH_MIP_START_REPAIR_MODE_PROJECT_AND_ROUND) == 0,
                "mip modular: set warm start repair mode");
    ASSERT_TRUE(ralph_mip_get_start_repair_mode(model) == RALPH_MIP_START_REPAIR_MODE_PROJECT_AND_ROUND,
                "mip modular: get warm start repair mode");

    cut_cb.generate_cuts = test_cut_cb_generate;
    cut_cb.user_data = NULL;
    branch_cb.select_branch_var = test_branch_cb_select;
    branch_cb.user_data = NULL;
    ralph_mip_set_cut_callback(model, &cut_cb);
    ralph_mip_set_branch_callback(model, &branch_cb);
    ralph_mip_set_cut_callback(model, NULL);
    ralph_mip_set_branch_callback(model, NULL);

    ASSERT_TRUE(ralph_mip_solve_benders(NULL, NULL, NULL, NULL) == -1,
                "mip modular: benders rejects null model/config");

    ASSERT_TRUE(ralph_mip_optimize(model) == 0, "mip modular: optimize");
    ASSERT_TRUE(ralph_mip_get_status(model) == RALPH_LP_STATUS_OPTIMAL,
                "mip modular: optimal status");
    ASSERT_NEAR(ralph_mip_get_objval(model), 3.0, 1e-9, "mip modular: objective");

    ASSERT_TRUE(ralph_mip_get_solution(model, x) == 0, "mip modular: solution");
    ASSERT_NEAR(x[0], 1.0, 1e-9, "mip modular: x");
    ASSERT_NEAR(x[1], 0.0, 1e-9, "mip modular: y");
    ASSERT_TRUE(ralph_mip_get_node_count(model) >= 0, "mip modular: node count");
    ASSERT_TRUE(ralph_mip_get_gap(model) >= 0.0, "mip modular: mip gap");
    ASSERT_TRUE(isfinite(ralph_mip_get_best_bound(model)),
                "mip modular: best bound finite");
    ASSERT_TRUE(ralph_mip_get_start_status(model) != RALPH_MIP_START_STATUS_PENDING,
                "mip modular: warm start status updated");

    ralph_mip_clear_start(model);
    ASSERT_TRUE(ralph_mip_get_start_status(model) == RALPH_MIP_START_STATUS_NONE,
                "mip modular: clear warm start");

    {
        RalphMIPBendersConfig cfg = RALPH_MIP_BENDERS_CONFIG_DEFAULT;
        ASSERT_TRUE(ralph_mip_solve_benders(model, &cfg, NULL, NULL) == -1,
                    "mip modular: benders wrapper validation");
    }

    ralph_mip_free(model);
}

static void test_mip_public_cut_callback_invocation(void) {
    RalphMIPModel *model = ralph_mip_create();
    RalphMIPCutCallback cut_cb;
    double x[2] = {0.0, 0.0};
    int callback_seen = 0;

    ASSERT_TRUE(model != NULL, "mip modular: callback model created");
    if (!model) return;

    ASSERT_TRUE(ralph_mip_set_int_param(model, "presolve", 0) == 0,
                "mip modular: callback presolve off");
    ASSERT_TRUE(ralph_mip_set_int_param(model, "max_cut_rounds", 1) == 0,
                "mip modular: callback cut rounds set");
    ASSERT_TRUE(ralph_lp_set_obj_sense(model, RALPH_LP_OBJ_MAXIMIZE) == 0,
                "mip modular: callback objective sense");
    ASSERT_TRUE(ralph_lp_add_var(model, 0.0, 1.0, 1.0, RALPH_LP_VAR_BINARY) == 0,
                "mip modular: callback add x");
    ASSERT_TRUE(ralph_lp_add_var(model, 0.0, 1.0, 1.0, RALPH_LP_VAR_BINARY) == 1,
                "mip modular: callback add y");
    {
        int idx[] = {0, 1};
        double val[] = {2.0, 2.0};
        ASSERT_TRUE(ralph_lp_add_constraint(model, 2, idx, val, RALPH_LP_SENSE_LESS_EQUAL, 3.0) == 0,
                    "mip modular: callback add fractional row");
    }

    public_cut_cb_calls = 0;
    cut_cb.generate_cuts = test_cut_cb_generate;
    cut_cb.user_data = &callback_seen;
    ralph_mip_set_cut_callback(model, &cut_cb);

    ASSERT_TRUE(ralph_mip_optimize(model) == 0, "mip modular: callback optimize");
    ASSERT_TRUE(ralph_mip_get_status(model) == RALPH_LP_STATUS_OPTIMAL,
                "mip modular: callback status optimal");
    ASSERT_TRUE(ralph_mip_get_solution(model, x) == 0, "mip modular: callback solution");
    ASSERT_NEAR(ralph_mip_get_objval(model), 1.0, 1e-9, "mip modular: callback objective");
    ASSERT_TRUE(callback_seen == 1, "mip modular: callback user data observed");
    ASSERT_TRUE(public_cut_cb_calls > 0, "mip modular: callback invoked");

    ralph_mip_free(model);
}

int main(void) {
    printf("Running modular MIP API split tests...\n");
    test_mip_modular_header_and_wrappers();
    test_mip_public_cut_callback_invocation();
    printf("MIP modular tests: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
