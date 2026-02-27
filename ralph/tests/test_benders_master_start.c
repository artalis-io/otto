/*
 * Ralph - P3 Benders Initial Master-Start Tests
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
    int x = ralph_test_add_var(model, 0.0, 1e9, 1.0, RALPH_INTEGER);    /* master */
    int y = ralph_test_add_var(model, 0.0, 1e9, 1.0, RALPH_CONTINUOUS); /* sub */
    if (x != 0 || y != 1) {
        ralph_test_free(model);
        return NULL;
    }

    /* linking: y <= x */
    {
        int idx[] = {y, x};
        double val[] = {1.0, -1.0};
        ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 0.0);
    }
    /* sub demand: y >= 5 */
    {
        int idx[] = {y};
        double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 5.0);
    }
    /* master-only row */
    {
        int idx[] = {x};
        double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 0.0);
    }

    return model;
}

int main(void) {
    int master_vars[] = {0}; /* x */

    RalphBendersConfig base_cfg = RALPH_BENDERS_CONFIG_DEFAULT;
    base_cfg.master_var_indices = master_vars;
    base_cfg.num_master_vars = 1;
    base_cfg.theta_var = -1;
    base_cfg.max_iterations = 50;
    base_cfg.verbose = 0;
    base_cfg.warm_start_master = 1;
    base_cfg.warm_start_subproblems = 1;

    RalphBendersResult base_res = {0};
    RalphBendersResult warm_res = {0};
    RalphBendersResult cold_res = {0};

    RalphModel *base = build_policy_model();
    ASSERT(base != NULL, "Baseline model created");
    if (!base) return 1;
    ASSERT(ralph_test_solve_benders(base, &base_cfg, NULL, &base_res) == 0,
           "Baseline Benders solve succeeds");
    ASSERT(base_res.status == RALPH_STATUS_OPTIMAL, "Baseline Benders OPTIMAL");
    ralph_test_free(base);

    /* Use external initial master solution in original variable space. */
    double initial_master[] = {7.0, 0.0};
    RalphBendersConfig warm_cfg = base_cfg;
    warm_cfg.initial_master_solution = initial_master;

    RalphModel *warm = build_policy_model();
    ASSERT(warm != NULL, "Warm-started model created");
    if (!warm) return 1;
    ASSERT(ralph_test_solve_benders(warm, &warm_cfg, NULL, &warm_res) == 0,
           "Warm-started Benders solve succeeds");
    ASSERT(warm_res.status == RALPH_STATUS_OPTIMAL, "Warm-started Benders OPTIMAL");
    ASSERT(warm_res.master_warm_starts_attempted > 0,
           "Initial master solution triggers master warm-start attempts");
    ASSERT(warm_res.master_warm_starts_accepted <= warm_res.master_warm_starts_attempted,
           "Master warm-start counters are consistent");
    ASSERT_NEAR(warm_res.objective, base_res.objective, 1e-6,
                "Initial master solution preserves final objective");
    ralph_test_free(warm);

    /* warm_start_master=0 should disable master start attempts, even if provided. */
    RalphBendersConfig cold_cfg = warm_cfg;
    cold_cfg.warm_start_master = 0;
    RalphModel *cold = build_policy_model();
    ASSERT(cold != NULL, "Cold-master model created");
    if (!cold) return 1;
    ASSERT(ralph_test_solve_benders(cold, &cold_cfg, NULL, &cold_res) == 0,
           "Cold-master Benders solve succeeds");
    ASSERT(cold_res.status == RALPH_STATUS_OPTIMAL, "Cold-master Benders OPTIMAL");
    ASSERT(cold_res.master_warm_starts_attempted == 0,
           "warm_start_master=0 disables master warm starts");
    ralph_test_free(cold);

    printf("Benders master-start tests: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}

