#include <stdio.h>
#include "lp_basis_governor.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT_INT_EQ(actual, expected, msg) do { \
    tests_run++; \
    if ((actual) == (expected)) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%d != %d)\n", msg, (int)(actual), (int)(expected)); \
    } \
} while (0)

static void test_reset_and_shadow_decide(void) {
    printf("  basis_governor: reset + shadow decide...\n");
    LPBasisGovernorState state;
    state.shadow_refactor_yes_phase1 = 7;
    state.shadow_backend_pick_markowitz = 9;
    state.shadow_disagree_lu_backend = 5;

    lp_basis_governor_begin_solve(&state);

    ASSERT_INT_EQ(state.shadow_refactor_yes_phase1, 0, "reset yes_phase1");
    ASSERT_INT_EQ(state.shadow_backend_pick_markowitz, 0, "reset backend_markowitz");
    ASSERT_INT_EQ(state.shadow_disagree_lu_backend, 0, "reset disagree_lu");

    ASSERT_INT_EQ(lp_basis_governor_shadow_decide(LP_BASIS_GOV_PHASE1, 0, 0), 0,
                  "shadow decide none");
    ASSERT_INT_EQ(lp_basis_governor_shadow_decide(LP_BASIS_GOV_PHASE1, 1, 0), 1,
                  "shadow decide lu health");
    ASSERT_INT_EQ(lp_basis_governor_shadow_decide(LP_BASIS_GOV_PHASE2, 0, 1), 1,
                  "shadow decide periodic");
    ASSERT_INT_EQ(lp_basis_governor_shadow_decide(LP_BASIS_GOV_PHASE_DUAL, 1, 1), 1,
                  "shadow decide both");
}

static void test_refactor_observation(void) {
    printf("  basis_governor: refactor observation...\n");
    LPBasisGovernorState state;
    lp_basis_governor_begin_solve(&state);

    lp_basis_governor_observe_refactor(&state, LP_BASIS_GOV_PHASE1, 1, 1);
    lp_basis_governor_observe_refactor(&state, LP_BASIS_GOV_PHASE1, 0, 1);
    lp_basis_governor_observe_refactor(&state, LP_BASIS_GOV_PHASE2, 1, 0);
    lp_basis_governor_observe_iter(&state, LP_BASIS_GOV_PHASE_DUAL, 0, 0);
    lp_basis_governor_observe_refactor(&state, LP_BASIS_GOV_PHASE_DUAL, 1, 0);

    ASSERT_INT_EQ(state.shadow_refactor_yes_phase1, 1, "phase1 yes count");
    ASSERT_INT_EQ(state.shadow_refactor_no_phase1, 1, "phase1 no count");
    ASSERT_INT_EQ(state.shadow_refactor_yes_phase2, 1, "phase2 yes count");
    ASSERT_INT_EQ(state.shadow_refactor_no_dual, 1, "dual no count");
    ASSERT_INT_EQ(state.shadow_refactor_yes_dual, 1, "dual yes count");

    ASSERT_INT_EQ(state.shadow_disagree_primal_refactor, 2, "primal disagreement count");
    ASSERT_INT_EQ(state.shadow_disagree_dual_refactor, 1, "dual disagreement count");
}

static void test_backend_observation(void) {
    printf("  basis_governor: backend observation...\n");
    LPBasisGovernorState state;
    lp_basis_governor_begin_solve(&state);

    ASSERT_INT_EQ(lp_basis_governor_shadow_decide_lu_backend(1, 1),
                  LP_BASIS_GOV_BACKEND_MARKOWITZ,
                  "backend pick prefers markowitz");
    ASSERT_INT_EQ(lp_basis_governor_shadow_decide_lu_backend(0, 1),
                  LP_BASIS_GOV_BACKEND_SUPERNODE,
                  "backend pick supernode");
    ASSERT_INT_EQ(lp_basis_governor_shadow_decide_lu_backend(0, 0),
                  LP_BASIS_GOV_BACKEND_DENSE,
                  "backend pick dense");

    lp_basis_governor_observe_lu_backend(&state,
                                         LP_BASIS_GOV_BACKEND_MARKOWITZ,
                                         LP_BASIS_GOV_BACKEND_MARKOWITZ);
    lp_basis_governor_observe_lu_backend(&state,
                                         LP_BASIS_GOV_BACKEND_SUPERNODE,
                                         LP_BASIS_GOV_BACKEND_DENSE);
    lp_basis_governor_observe_lu_backend(&state,
                                         LP_BASIS_GOV_BACKEND_DENSE,
                                         LP_BASIS_GOV_BACKEND_DENSE);

    ASSERT_INT_EQ(state.shadow_backend_pick_markowitz, 1, "backend markowitz count");
    ASSERT_INT_EQ(state.shadow_backend_pick_supernode, 1, "backend supernode count");
    ASSERT_INT_EQ(state.shadow_backend_pick_dense, 1, "backend dense count");
    ASSERT_INT_EQ(state.shadow_disagree_lu_backend, 1, "backend disagreement count");
}

int main(void) {
    printf("=== LP Basis Governor Tests ===\n");

    test_reset_and_shadow_decide();
    test_refactor_observation();
    test_backend_observation();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
