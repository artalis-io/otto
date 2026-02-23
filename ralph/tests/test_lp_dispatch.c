/*
 * Tests for internal LP backend dispatch planner.
 *
 * This module is intentionally separate from public API tests.
 */

#include <stdio.h>
#include <string.h>

#include "../src/lp_dispatch.h"

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

#define ASSERT_INT_EQ(a, b, msg) do { \
    tests_run++; \
    if ((a) == (b)) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%d != %d)\n", msg, (int)(a), (int)(b)); \
    } \
} while (0)

static void test_dispatch_validation(void) {
    ASSERT_INT_EQ(lp_dispatch_algorithm_value_valid(-1), 0,
                  "validation: algorithm -1 invalid");
    ASSERT_INT_EQ(lp_dispatch_algorithm_value_valid((int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX), 1,
                  "validation: primal valid");
    ASSERT_INT_EQ(lp_dispatch_algorithm_value_valid((int)RALPH_LP_ALGORITHM_BARRIER), 1,
                  "validation: barrier valid");
    ASSERT_INT_EQ(lp_dispatch_algorithm_value_valid(4), 0,
                  "validation: algorithm 4 invalid");

    ASSERT_INT_EQ(lp_dispatch_crossover_value_valid(-1), 0,
                  "validation: crossover -1 invalid");
    ASSERT_INT_EQ(lp_dispatch_crossover_value_valid((int)RALPH_LP_CROSSOVER_AUTO), 1,
                  "validation: crossover auto valid");
    ASSERT_INT_EQ(lp_dispatch_crossover_value_valid((int)RALPH_LP_CROSSOVER_ON), 1,
                  "validation: crossover on valid");
    ASSERT_INT_EQ(lp_dispatch_crossover_value_valid(3), 0,
                  "validation: crossover 3 invalid");
}

static void test_dispatch_setters(void) {
    int alg = -1;
    int method = -1;
    int crossover = -1;

    ASSERT_INT_EQ(lp_dispatch_set_requested_algorithm((int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX,
                                                      &alg,
                                                      &method),
                  0,
                  "setters: dual algorithm accepted");
    ASSERT_INT_EQ(alg, (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX,
                  "setters: normalized dual algorithm");
    ASSERT_INT_EQ(method, 1,
                  "setters: dual legacy method");

    ASSERT_INT_EQ(lp_dispatch_set_requested_algorithm((int)RALPH_LP_ALGORITHM_BARRIER,
                                                      &alg,
                                                      &method),
                  0,
                  "setters: barrier algorithm accepted");
    ASSERT_INT_EQ(alg, (int)RALPH_LP_ALGORITHM_BARRIER,
                  "setters: normalized barrier algorithm");
    ASSERT_INT_EQ(method, 2,
                  "setters: barrier maps legacy method to auto");

    ASSERT_INT_EQ(lp_dispatch_set_requested_algorithm(4, &alg, &method), -1,
                  "setters: reject invalid algorithm");
    ASSERT_INT_EQ(lp_dispatch_set_requested_algorithm((int)RALPH_LP_ALGORITHM_AUTO,
                                                      NULL,
                                                      &method),
                  -1,
                  "setters: reject NULL normalized algorithm output");

    ASSERT_INT_EQ(lp_dispatch_set_requested_crossover((int)RALPH_LP_CROSSOVER_ON,
                                                      &crossover),
                  0,
                  "setters: crossover on accepted");
    ASSERT_INT_EQ(crossover, (int)RALPH_LP_CROSSOVER_ON,
                  "setters: normalized crossover on");
    ASSERT_INT_EQ(lp_dispatch_set_requested_crossover(3, &crossover), -1,
                  "setters: reject invalid crossover");
    ASSERT_INT_EQ(lp_dispatch_set_requested_crossover((int)RALPH_LP_CROSSOVER_AUTO,
                                                      NULL),
                  -1,
                  "setters: reject NULL normalized crossover output");
}

static void test_dispatch_capabilities(void) {
    RalphLPCapabilities caps;

    memset(&caps, 0, sizeof(caps));
    lp_dispatch_get_capabilities(&caps);

    ASSERT_INT_EQ(caps.supports_primal_simplex, 1,
                  "capabilities: primal supported");
    ASSERT_INT_EQ(caps.supports_dual_simplex, 1,
                  "capabilities: dual supported");
    ASSERT_INT_EQ(caps.supports_barrier, 0,
                  "capabilities: barrier unsupported");
    ASSERT_INT_EQ(caps.supports_crossover, 0,
                  "capabilities: crossover unsupported");
}

static void test_dispatch_plan_simplex_path(void) {
    LPDispatchPlan plan;
    RalphLPSolveAlgorithmReport report;

    ASSERT_INT_EQ(lp_dispatch_build_plan((int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX,
                                         (int)RALPH_LP_CROSSOVER_AUTO,
                                         &plan),
                  0,
                  "plan/simplex: build succeeds");
    ASSERT_INT_EQ((int)plan.requested_backend, (int)LP_DISPATCH_BACKEND_SIMPLEX,
                  "plan/simplex: requested backend simplex");
    ASSERT_INT_EQ((int)plan.effective_backend, (int)LP_DISPATCH_BACKEND_SIMPLEX,
                  "plan/simplex: effective backend simplex");
    ASSERT_INT_EQ((int)plan.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX,
                  "plan/simplex: effective algorithm dual");
    ASSERT_INT_EQ(plan.simplex_method, 1,
                  "plan/simplex: simplex method dual");
    ASSERT_INT_EQ(plan.fallback_applied, 0,
                  "plan/simplex: no fallback applied");

    memset(&report, 0, sizeof(report));
    lp_dispatch_plan_to_report(&plan, &report);
    ASSERT_INT_EQ((int)report.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX,
                  "plan/simplex: report effective algorithm dual");
    ASSERT_INT_EQ(report.fallback_applied, 0,
                  "plan/simplex: report no fallback");
}

static void test_dispatch_plan_barrier_stub_fallback(void) {
    LPDispatchPlan plan;

    ASSERT_INT_EQ(lp_dispatch_build_plan((int)RALPH_LP_ALGORITHM_BARRIER,
                                         (int)RALPH_LP_CROSSOVER_ON,
                                         &plan),
                  0,
                  "plan/barrier: build succeeds");
    ASSERT_INT_EQ((int)plan.requested_backend, (int)LP_DISPATCH_BACKEND_BARRIER,
                  "plan/barrier: requested backend barrier");
    ASSERT_INT_EQ((int)plan.effective_backend, (int)LP_DISPATCH_BACKEND_SIMPLEX,
                  "plan/barrier: effective backend simplex fallback");
    ASSERT_INT_EQ((int)plan.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_AUTO,
                  "plan/barrier: effective algorithm auto fallback");
    ASSERT_INT_EQ((int)plan.requested_crossover,
                  (int)RALPH_LP_CROSSOVER_ON,
                  "plan/barrier: requested crossover captured");
    ASSERT_INT_EQ((int)plan.effective_crossover,
                  (int)RALPH_LP_CROSSOVER_AUTO,
                  "plan/barrier: effective crossover fallback to auto");
    ASSERT_INT_EQ(plan.fallback_applied, 1,
                  "plan/barrier: fallback applied");
    ASSERT_INT_EQ((int)plan.fallback_reason,
                  (int)RALPH_LP_FALLBACK_BARRIER_UNAVAILABLE,
                  "plan/barrier: fallback reason barrier unavailable");
    ASSERT_INT_EQ(plan.simplex_method, 2,
                  "plan/barrier: simplex method auto after fallback");
}

static void test_dispatch_plan_crossover_only_fallback(void) {
    LPDispatchPlan plan;

    ASSERT_INT_EQ(lp_dispatch_build_plan((int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                                         (int)RALPH_LP_CROSSOVER_ON,
                                         &plan),
                  0,
                  "plan/crossover: build succeeds");
    ASSERT_INT_EQ((int)plan.requested_backend, (int)LP_DISPATCH_BACKEND_SIMPLEX,
                  "plan/crossover: requested backend simplex");
    ASSERT_INT_EQ((int)plan.effective_backend, (int)LP_DISPATCH_BACKEND_SIMPLEX,
                  "plan/crossover: effective backend simplex");
    ASSERT_INT_EQ((int)plan.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                  "plan/crossover: effective algorithm primal");
    ASSERT_INT_EQ((int)plan.effective_crossover,
                  (int)RALPH_LP_CROSSOVER_AUTO,
                  "plan/crossover: effective crossover auto");
    ASSERT_INT_EQ(plan.fallback_applied, 1,
                  "plan/crossover: fallback applied");
    ASSERT_INT_EQ((int)plan.fallback_reason,
                  (int)RALPH_LP_FALLBACK_CROSSOVER_UNAVAILABLE,
                  "plan/crossover: fallback reason crossover unavailable");
}

int main(void) {
    printf("=== LP Dispatch Module Tests ===\n");

    test_dispatch_validation();
    test_dispatch_setters();
    test_dispatch_capabilities();
    test_dispatch_plan_simplex_path();
    test_dispatch_plan_barrier_stub_fallback();
    test_dispatch_plan_crossover_only_fallback();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
