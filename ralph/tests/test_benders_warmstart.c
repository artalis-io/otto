/*
 * Ralph - Benders Warm-Start Policy Tests
 *
 * Focused tests for warm_start_subproblems behavior:
 * - warm_start_master controls master MIP incumbent seeding between iterations
 * - warm_start_subproblems=1 reuses subproblem tableau/basis state
 * - warm_start_subproblems=0 forces cold starts
 * - both modes preserve objective/status correctness
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

static RalphModel* build_policy_model(void) {
    RalphModel *model = ralph_test_create();
    if (!model) return NULL;

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);

    int x = ralph_test_add_var(model, 0.0, 1e9, 1.0, RALPH_INTEGER);      /* master */
    int y = ralph_test_add_var(model, 0.0, 1e9, 1.0, RALPH_CONTINUOUS);   /* sub */
    if (x != 0 || y != 1) {
        ralph_test_free(model);
        return NULL;
    }

    /* Linking: y <= x */
    {
        int idx[] = {y, x};
        double val[] = {1.0, -1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 0.0);
    }

    /* Subproblem demand: y >= 5 */
    {
        int idx[] = {y};
        double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 5.0);
    }

    /* Pure-master row (required by current Benders partitioning path) */
    {
        int idx[] = {x};
        double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 0.0);
    }

    return model;
}

int main(void) {
    int master_vars[] = {0};  /* x */

    RalphBendersConfig warm_cfg = RALPH_BENDERS_CONFIG_DEFAULT;
    warm_cfg.master_var_indices = master_vars;
    warm_cfg.num_master_vars = 1;
    warm_cfg.theta_var = -1;
    warm_cfg.max_iterations = 50;
    warm_cfg.verbose = 0;
    warm_cfg.warm_start_master = 1;
    warm_cfg.warm_start_subproblems = 1;

    RalphBendersConfig cold_cfg = warm_cfg;
    cold_cfg.warm_start_master = 0;
    cold_cfg.warm_start_subproblems = 0;

    RalphBendersResult warm_res = {0};
    RalphBendersResult cold_res = {0};

    RalphModel *warm_model = build_policy_model();
    ASSERT(warm_model != NULL, "Warm policy model created");
    if (!warm_model) return 1;

    int warm_ret = ralph_test_solve_benders(warm_model, &warm_cfg, NULL, &warm_res);
    ASSERT(warm_ret == 0, "Warm policy solve succeeds");
    ASSERT(warm_res.status == RALPH_STATUS_OPTIMAL, "Warm policy status OPTIMAL");
    ASSERT(warm_res.iterations >= 2, "Warm policy requires multiple Benders iterations");
    ASSERT(warm_res.subproblems_solved > 0, "Warm policy reports subproblem solves");
    ASSERT(warm_res.master_warm_starts_attempted > 0, "Warm policy attempts master MIP warm starts");
    ASSERT(warm_res.master_warm_starts_accepted <= warm_res.master_warm_starts_attempted,
           "Warm policy master start counters are consistent");
    ASSERT(warm_res.subproblem_warm_starts > 0, "Warm policy reuses subproblem warm starts");
    ASSERT(warm_res.subproblem_warm_starts + warm_res.subproblem_cold_starts ==
           warm_res.subproblems_solved,
           "Warm policy start counters sum to total subproblem solves");
    ralph_test_free(warm_model);

    RalphModel *cold_model = build_policy_model();
    ASSERT(cold_model != NULL, "Cold policy model created");
    if (!cold_model) return 1;

    int cold_ret = ralph_test_solve_benders(cold_model, &cold_cfg, NULL, &cold_res);
    ASSERT(cold_ret == 0, "Cold policy solve succeeds");
    ASSERT(cold_res.status == RALPH_STATUS_OPTIMAL, "Cold policy status OPTIMAL");
    ASSERT(cold_res.master_warm_starts_attempted == 0, "Cold policy disables master MIP warm starts");
    ASSERT(cold_res.master_warm_starts_accepted == 0, "Cold policy has zero accepted master warm starts");
    ASSERT(cold_res.subproblem_warm_starts == 0, "Cold policy has zero warm starts");
    ASSERT(cold_res.subproblem_cold_starts == cold_res.subproblems_solved,
           "Cold policy subproblem solves are all cold starts");
    ASSERT_NEAR(warm_res.objective, cold_res.objective, 1e-6,
                "Warm/cold policies preserve objective correctness");
    ralph_test_free(cold_model);

    printf("Benders warm-start tests: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
