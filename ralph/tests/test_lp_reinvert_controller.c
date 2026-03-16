#include <math.h>
#include <stdio.h>
#include "lp_reinvert_controller.h"

#define TEST(cond, msg) \
    do { \
        total++; \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s\n", (msg)); \
        } else { \
            pass++; \
        } \
    } while (0)

static int decision_equal(const LPReinvertControllerDecision *a,
                          const LPReinvertControllerDecision *b) {
    if (!a || !b) return 0;
    if (a->decision != b->decision) return 0;
    if (a->reason != b->reason) return 0;
    if (a->cooldown_updates_next != b->cooldown_updates_next) return 0;
    if (a->soft_lu_breach_streak_next != b->soft_lu_breach_streak_next) return 0;
    if (fabs(a->refactor_to_iter_cost_ratio - b->refactor_to_iter_cost_ratio) > 1e-12) return 0;
    if (fabs(a->solve_density - b->solve_density) > 1e-12) return 0;
    return 1;
}

static void prime_warmup_samples(LPReinvertControllerState *state) {
    int i;
    if (!state) return;
    for (i = 0; i < 10; i++) {
        lp_reinvert_controller_state_record_iter_cost(state, 1.0);
    }
    lp_reinvert_controller_state_record_refactor_cost(state, 10.0);
}

int main(void) {
    int pass = 0;
    int total = 0;
    LPReinvertControllerState state;
    LPReinvertControllerSignals sig;
    LPReinvertControllerDecision d0;
    LPReinvertControllerDecision d1;

    printf("=== LP Reinvert Controller Tests ===\n");

    d0 = lp_reinvert_controller_decide(NULL, NULL);
    TEST(d0.decision == LP_REINVERT_DECISION_DEFER &&
             d0.reason == LP_REINVERT_REASON_INVALID_INPUTS,
         "invalid input: null pointers");

    lp_reinvert_controller_state_reset(&state);
    sig.phase = 1;
    sig.iter = 10;
    sig.m = 100;
    sig.num_updates = 15;
    sig.max_updates = 100;
    sig.periodic_due = 1;
    sig.min_update_age = 8;
    sig.cooldown_updates = 0;
    sig.hard_lu_trigger = 1;
    sig.soft_lu_trigger = 0;
    sig.ftran_density = 0.1;
    sig.btran_density = 0.1;
    d0 = lp_reinvert_controller_decide(&state, &sig);
    TEST(d0.decision == LP_REINVERT_DECISION_FORCE &&
             d0.reason == LP_REINVERT_REASON_HARD_LU_HEALTH,
         "hard LU signal forces reinvert");

    sig.hard_lu_trigger = 0;
    sig.soft_lu_trigger = 1;
    d0 = lp_reinvert_controller_decide(&state, &sig);
    TEST(d0.decision == LP_REINVERT_DECISION_DEFER &&
             d0.reason == LP_REINVERT_REASON_SOFT_LU_HEALTH &&
             d0.soft_lu_breach_streak_next == 1,
         "first soft LU signal defers and starts streak");
    lp_reinvert_controller_state_apply_decision(&state, &d0);
    d1 = lp_reinvert_controller_decide(&state, &sig);
    TEST(d1.decision == LP_REINVERT_DECISION_FORCE &&
             d1.reason == LP_REINVERT_REASON_SOFT_LU_HEALTH &&
             d1.soft_lu_breach_streak_next >= 2,
         "sustained soft LU signal forces reinvert");

    sig.soft_lu_trigger = 0;
    lp_reinvert_controller_state_reset(&state);
    prime_warmup_samples(&state);
    lp_reinvert_controller_state_set_cooldown(&state, 3);
    d0 = lp_reinvert_controller_decide(&state, &sig);
    TEST(d0.decision == LP_REINVERT_DECISION_DEFER &&
             d0.reason == LP_REINVERT_REASON_COOLDOWN &&
             d0.cooldown_updates_next == 2,
         "cooldown defers periodic reinvert");

    lp_reinvert_controller_state_reset(&state);
    d0 = lp_reinvert_controller_decide(&state, &sig);
    TEST(d0.decision == LP_REINVERT_DECISION_DEFER &&
             d0.reason == LP_REINVERT_REASON_WARMUP,
         "warmup defers before cost EWMAs are ready");

    lp_reinvert_controller_state_reset(&state);
    prime_warmup_samples(&state);
    sig.num_updates = 20;
    sig.max_updates = 100;
    sig.ftran_density = 0.05;
    sig.btran_density = 0.05;
    d0 = lp_reinvert_controller_decide(&state, &sig);
    TEST(d0.decision == LP_REINVERT_DECISION_DEFER &&
             d0.reason == LP_REINVERT_REASON_COST_DAMPEN &&
             d0.refactor_to_iter_cost_ratio >= 9.9,
         "high refactor/iter ratio defers periodic reinvert");

    lp_reinvert_controller_state_reset(&state);
    prime_warmup_samples(&state);
    lp_reinvert_controller_state_record_solve_density(&state, 0.9, 0.85);
    sig.ftran_density = 0.9;
    sig.btran_density = 0.85;
    d0 = lp_reinvert_controller_decide(&state, &sig);
    TEST(d0.decision == LP_REINVERT_DECISION_FORCE &&
             d0.reason == LP_REINVERT_REASON_DENSITY_PRESSURE &&
             d0.solve_density >= 0.85,
         "high solve density forces reinvert");

    lp_reinvert_controller_state_reset(&state);
    prime_warmup_samples(&state);
    lp_reinvert_controller_state_record_refactor_cost(&state, 1.5);
    sig.ftran_density = 0.1;
    sig.btran_density = 0.1;
    sig.num_updates = 95;
    sig.max_updates = 100;
    d0 = lp_reinvert_controller_decide(&state, &sig);
    TEST(d0.decision == LP_REINVERT_DECISION_ALLOW &&
             d0.reason == LP_REINVERT_REASON_PERIODIC_CADENCE,
         "near update limit keeps periodic reinvert allowed");

    sig.periodic_due = 0;
    d0 = lp_reinvert_controller_decide(&state, &sig);
    TEST(d0.decision == LP_REINVERT_DECISION_ALLOW &&
             d0.reason == LP_REINVERT_REASON_NONE,
         "not due path allows without forcing reasons");

    sig.periodic_due = 1;
    sig.phase = 3;
    d0 = lp_reinvert_controller_decide(&state, &sig);
    TEST(d0.decision == LP_REINVERT_DECISION_DEFER &&
             d0.reason == LP_REINVERT_REASON_INVALID_INPUTS,
         "invalid phase is rejected");

    sig.phase = 1;
    d0 = lp_reinvert_controller_decide(&state, &sig);
    d1 = lp_reinvert_controller_decide(&state, &sig);
    TEST(decision_equal(&d0, &d1), "deterministic output for identical inputs");

    TEST(lp_reinvert_controller_mode_is_valid(LP_REINVERT_MODE_OFF) == 1,
         "mode validation accepts off");
    TEST(lp_reinvert_controller_mode_is_valid(LP_REINVERT_MODE_SHADOW) == 1,
         "mode validation accepts shadow");
    TEST(lp_reinvert_controller_mode_is_valid(LP_REINVERT_MODE_CONTROL_PHASE1) == 1,
         "mode validation accepts control_phase1");
    TEST(lp_reinvert_controller_mode_is_valid(LP_REINVERT_MODE_CONTROL_ALL) == 1,
         "mode validation accepts control_all");
    TEST(lp_reinvert_controller_mode_is_valid(-1) == 0,
         "mode validation rejects negative values");
    TEST(lp_reinvert_controller_mode_is_valid(4) == 0,
         "mode validation rejects out-of-range values");

    TEST(lp_reinvert_controller_reason_string(LP_REINVERT_REASON_COST_DAMPEN) != NULL,
         "reason strings are available");

    printf("Passed %d/%d reinvert-controller tests\n", pass, total);
    return (pass == total) ? 0 : 1;
}
