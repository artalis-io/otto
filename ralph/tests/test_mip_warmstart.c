/*
 * Ralph - MIP Warm-Start API Tests
 *
 * Focused tests for public MIP-start support:
 * - staged start status transitions (pending/accepted/rejected/none)
 * - accepted start preserves correctness
 * - rejected start falls back cleanly and preserves correctness
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "ralph_test_mod_api.h"
#include "mip.h"

/* Internal test-only hook from ralph.c */
MIPSolver* ralph_get_mip_solver(const RalphModel *model);

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

static RalphModel* build_fractional_binary_mip(void) {
    RalphModel *model = ralph_test_create();
    if (!model) return NULL;

    ralph_test_set_obj_sense(model, RALPH_MAXIMIZE);

    /* max x + y
     * s.t. 2x + 2y <= 3
     *      x, y binary
     * LP root is fractional (x=y=0.75), integer optimum is 1.0. */
    int x = ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);
    int y = ralph_test_add_var(model, 0.0, 1.0, 1.0, RALPH_BINARY);
    if (x != 0 || y != 1) {
        ralph_test_free(model);
        return NULL;
    }

    {
        int idx[] = {x, y};
        double val[] = {2.0, 2.0};
        if (ralph_test_add_constraint(model, 2, idx, val, RALPH_LESS_EQUAL, 3.0) < 0) {
            ralph_test_free(model);
            return NULL;
        }
    }

    return model;
}

int main(void) {
    /* Feasible MIP start should be accepted and preserve optimal solution. */
    RalphModel *warm = build_fractional_binary_mip();
    ASSERT(warm != NULL, "Warm model created");
    if (!warm) return 1;

    double good_start[] = {1.0, 0.0};  /* Feasible and optimal */
    ASSERT(ralph_test_set_mip_start(warm, good_start) == 0, "Set feasible MIP start succeeds");
    ASSERT(ralph_test_get_mip_start_status(warm) == RALPH_MIP_START_PENDING,
           "MIP start status is pending before solve");

    ASSERT(ralph_test_optimize(warm) == 0, "Warm optimize succeeds");
    ASSERT(ralph_test_get_status(warm) == RALPH_STATUS_OPTIMAL, "Warm status OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(warm), 1.0, 1e-9, "Warm objective is 1.0");
    ASSERT(ralph_test_get_mip_start_status(warm) == RALPH_MIP_START_ACCEPTED,
           "Feasible MIP start is accepted");

    /* Tiny-case sanity guard: warm-start solve should remain bounded. */
    ASSERT(ralph_test_get_node_count(warm) <= 64,
           "Accepted MIP start keeps node count bounded on tiny instance");

    ralph_test_clear_mip_start(warm);
    ASSERT(ralph_test_get_mip_start_status(warm) == RALPH_MIP_START_NONE,
           "Clearing MIP start resets status");
    ralph_test_free(warm);

    /* Infeasible MIP start should be rejected but solve remains correct. */
    RalphModel *bad = build_fractional_binary_mip();
    ASSERT(bad != NULL, "Bad-start model created");
    if (!bad) return 1;

    double bad_start[] = {1.0, 1.0};  /* Violates 2x + 2y <= 3 */
    ASSERT(ralph_test_set_mip_start(bad, bad_start) == 0, "Set infeasible MIP start succeeds");
    ASSERT(ralph_test_optimize(bad) == 0, "Bad-start optimize succeeds");
    ASSERT(ralph_test_get_status(bad) == RALPH_STATUS_OPTIMAL, "Bad-start status OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(bad), 1.0, 1e-9, "Bad-start objective remains correct");
    ASSERT(ralph_test_get_mip_start_status(bad) == RALPH_MIP_START_REJECTED,
           "Infeasible MIP start is rejected");
    ralph_test_free(bad);

    /* Node LP re-solves should use LP warm-basis integration (live or staged). */
    RalphModel *basis_ws = build_fractional_binary_mip();
    ASSERT(basis_ws != NULL, "Basis warm-start model created");
    if (!basis_ws) return 1;

    ASSERT(ralph_test_optimize(basis_ws) == 0, "Basis warm-start optimize succeeds");
    ASSERT(ralph_test_get_status(basis_ws) == RALPH_STATUS_OPTIMAL, "Basis warm-start status OPTIMAL");
    ASSERT_NEAR(ralph_test_get_objval(basis_ws), 1.0, 1e-9, "Basis warm-start objective is 1.0");

    MIPSolver *mip = ralph_get_mip_solver(basis_ws);
    ASSERT(mip != NULL, "MIP solver available for warm-basis telemetry");
    if (mip) {
        ASSERT(mip->simplex_nodes_solved > 0, "Branch-and-bound solved simplex node LPs");
        ASSERT((mip->node_basis_warm_applied > 0) || (mip->node_basis_staged > 0),
               "Node LP solves used LP warm-basis API");
    }

    ralph_test_free(basis_ws);

    printf("MIP warm-start tests: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
