#include <math.h>
#include <stdio.h>
#include "lp_refactor_policy.h"

#define TEST(cond, msg) \
    do { \
        total++; \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s\n", (msg)); \
        } else { \
            pass++; \
        } \
    } while (0)

int main(void) {
    int pass = 0;
    int total = 0;
    LPPeriodicRefactorPolicy policy;
    LPLUHealthRefactorDecision decision;
    LPPeriodicCostDampenReason periodic_reason;
    int should_run;

    printf("=== LP Refactor Policy Module Tests ===\n");

    policy = lp_refactor_policy_build_from_metrics(2, 650, 100, 10,
                                                   0, 100, 1e3, 1.0,
                                                   0, 0, 0.0);
    TEST(policy.interval == 10, "phase2 large basis clamps to min interval");
    TEST(policy.run_pressure >= 0.99 && policy.run_pressure <= 1.0,
         "phase2 large basis pressure is conservative");
    should_run = lp_refactor_policy_should_run_metrics(10, 10, &policy, 0, 0);
    TEST(should_run == 1, "phase2 large basis should run periodic cadence");

    policy = lp_refactor_policy_build_from_metrics(2, 250, 120, 45,
                                                   0, 100, 1e3, 1.0,
                                                   0, 0, 0.0);
    TEST(policy.interval == 45, "healthy medium phase2 interval relaxed");
    TEST(policy.run_pressure >= 0.37 && policy.run_pressure <= 0.38,
         "healthy medium phase2 run pressure matches baseline");
    should_run = lp_refactor_policy_should_run_metrics(45, 45, &policy, 0, 0);
    TEST(should_run == 0, "healthy medium phase2 skips periodic cadence");

    policy = lp_refactor_policy_build_from_metrics(2, 250, 120, 90,
                                                   0, 100, 1e3, 1.0,
                                                   0, 0, 0.0);
    TEST(policy.interval == 45, "update-pressure case keeps interval");
    TEST(fabs(policy.run_pressure - 0.75) < 1e-9,
         "update-pressure case sets expected run pressure");
    should_run = lp_refactor_policy_should_run_metrics(90, 90, &policy, 0, 0);
    TEST(should_run == 1, "update pressure triggers periodic cadence");

    policy = lp_refactor_policy_build_from_metrics(2, 250, 120, 38,
                                                   0, 100, 1e3, 1.0,
                                                   0, 10, 0.0);
    TEST(policy.interval == 38, "degeneracy pressure tightens interval");
    TEST(fabs(policy.run_pressure - 0.5) < 1e-9,
         "degeneracy pressure case sets expected run pressure");
    should_run = lp_refactor_policy_should_run_metrics(38, 38, &policy, 0, 10);
    TEST(should_run == 1, "degeneracy pressure triggers periodic cadence");

    policy = lp_refactor_policy_build_from_metrics(2, 1503, 120, 24,
                                                   10, 100, 1e4, 10.0,
                                                   0, 40, 0.0);
    TEST(policy.interval == 24, "xlarge stable degeneracy relaxes min interval");

    policy = lp_refactor_policy_build_from_metrics(2, 1503, 120, 10,
                                                   10, 100, 1e8, 10.0,
                                                   0, 40, 0.0);
    TEST(policy.interval == 10, "xlarge poor-health degeneracy keeps tight interval");

    TEST(lp_refactor_policy_phase2_cooldown_eligible(1503, 40, 10, 100, 1e4, 10.0) == 1,
         "phase2 cooldown eligible under long stable degeneracy");
    TEST(lp_refactor_policy_phase2_cooldown_eligible(1503, 40, 10, 100, 1e8, 10.0) == 0,
         "phase2 cooldown disabled when LU condition is poor");
    TEST(lp_refactor_policy_phase2_cooldown_eligible(300, 80, 0, 100, 1e3, 1.0) == 0,
         "phase2 cooldown disabled for small basis");

    TEST(lp_refactor_policy_phase2_cooldown_window_updates(24) == 36,
         "cooldown window scales with interval");
    TEST(lp_refactor_policy_phase2_cooldown_window_updates(2) == 16,
         "cooldown window clamps to minimum");
    TEST(lp_refactor_policy_phase2_cooldown_window_updates(200) == 96,
         "cooldown window clamps to maximum");

    TEST(lp_refactor_policy_phase1_cooldown_eligible(1503, 120, 0, 10, 100, 1e4, 10.0) == 1,
         "phase1 cooldown eligible under large degenerate stable LU");
    TEST(lp_refactor_policy_phase1_cooldown_eligible(1100, 120, 0, 10, 100, 1e4, 10.0) == 0,
         "phase1 cooldown disabled below size threshold");
    TEST(lp_refactor_policy_phase1_cooldown_eligible(1503, 30, 0, 10, 100, 1e4, 10.0) == 0,
         "phase1 cooldown disabled below degeneracy threshold");
    TEST(lp_refactor_policy_phase1_cooldown_eligible(1503, 120, 0, 10, 100, 1e8, 10.0) == 1,
         "phase1 cooldown ignores LU condition (safety triggers handle health)");
    TEST(lp_refactor_policy_phase1_cooldown_eligible(1503, 10, 80, 10, 100, 1e8, 10.0) == 1,
         "phase1 cooldown eligible under sustained policy-refactor pressure");
    TEST(lp_refactor_policy_phase1_cooldown_window_updates(24) == 48,
         "phase1 cooldown window uses wider budget");
    TEST(lp_refactor_policy_phase1_cooldown_window_updates(6) == 24,
         "phase1 cooldown window clamps to minimum");
    TEST(lp_refactor_policy_phase1_cooldown_window_updates(200) == 192,
         "phase1 cooldown window clamps to maximum");

    decision = lp_refactor_policy_lu_health_refactor_decision(1500, 1, 120, 120,
                                                              0, 100, 1e3, 1.0, 0);
    TEST(decision.hard_trigger == 1, "lu health: hard trigger when max updates reached");
    TEST(decision.refactor_now == 1, "lu health: max updates refactors immediately");

    decision = lp_refactor_policy_lu_health_refactor_decision(1500, 1, 10, 120,
                                                              0, 100, 2e8, 100.0, 0);
    TEST(decision.hard_trigger == 1, "lu health: hard trigger on severe cond ratio");
    TEST(decision.refactor_now == 1, "lu health: severe cond ratio refactors immediately");

    decision = lp_refactor_policy_lu_health_refactor_decision(1500, 1, 70, 120,
                                                              0, 100, 1e7, 1.0, 0);
    TEST(decision.hard_trigger == 0, "lu health: adaptive limit is soft trigger");
    TEST(decision.soft_trigger == 1, "lu health: adaptive limit raises soft trigger");
    TEST(decision.refactor_now == 0, "lu health: first soft breach does not refactor");
    TEST(decision.soft_breach_streak_next == 1, "lu health: streak increments on soft breach");

    decision = lp_refactor_policy_lu_health_refactor_decision(1500, 1, 70, 120,
                                                              0, 100, 1e7, 1.0, 2);
    TEST(decision.refactor_now == 1, "lu health: sustained soft breaches refactor");
    TEST(decision.soft_breach_streak_next == 3, "lu health: streak carries into trigger step");

    decision = lp_refactor_policy_lu_health_refactor_decision(1500, 1, 12, 120,
                                                              90, 100, 1e3, 1.0, 1);
    TEST(decision.soft_trigger == 1, "lu health: spike-pool warning is soft trigger");
    TEST(decision.refactor_now == 0, "lu health: soft trigger obeys min update age");
    TEST(decision.soft_breach_streak_next == 2, "lu health: spike soft trigger increments streak");

    should_run = lp_refactor_policy_soft_lu_cost_gate_should_defer(2,
                                                                    1503,
                                                                    0,
                                                                    80,
                                                                    60,
                                                                    120,
                                                                    10,
                                                                    100,
                                                                    1e5,
                                                                    100.0,
                                                                    12.0,
                                                                    1.0);
    TEST(should_run == 1, "soft lu cost gate defers under expensive refactors");

    should_run = lp_refactor_policy_soft_lu_cost_gate_should_defer(2,
                                                                    1503,
                                                                    0,
                                                                    80,
                                                                    118,
                                                                    120,
                                                                    10,
                                                                    100,
                                                                    1e5,
                                                                    100.0,
                                                                    12.0,
                                                                    1.0);
    TEST(should_run == 0, "soft lu cost gate does not defer near max updates");

    should_run = lp_refactor_policy_soft_lu_cost_gate_should_defer(2,
                                                                    1503,
                                                                    0,
                                                                    80,
                                                                    60,
                                                                    120,
                                                                    10,
                                                                    100,
                                                                    1e8,
                                                                    100.0,
                                                                    12.0,
                                                                    1.0);
    TEST(should_run == 0, "soft lu cost gate does not defer poor LU condition");

    should_run = lp_refactor_policy_soft_lu_cost_gate_should_defer(2,
                                                                    400,
                                                                    0,
                                                                    80,
                                                                    60,
                                                                    120,
                                                                    10,
                                                                    100,
                                                                    1e5,
                                                                    100.0,
                                                                    12.0,
                                                                    1.0);
    TEST(should_run == 0, "soft lu cost gate requires large basis");

    should_run = lp_refactor_policy_periodic_cost_dampen_should_defer(2,
                                                                       1503,
                                                                       0,
                                                                       80,
                                                                       60,
                                                                       120,
                                                                       10,
                                                                       100,
                                                                       1e5,
                                                                       100.0,
                                                                       12.0,
                                                                       1.0);
    TEST(should_run == 1, "periodic cost dampen defers expensive periodic refactors");

    should_run = lp_refactor_policy_periodic_cost_dampen_should_defer(2,
                                                                       1503,
                                                                       0,
                                                                       80,
                                                                       116,
                                                                       120,
                                                                       10,
                                                                       100,
                                                                       1e5,
                                                                       100.0,
                                                                       12.0,
                                                                       1.0);
    TEST(should_run == 0, "periodic cost dampen keeps reserve near max updates");

    should_run = lp_refactor_policy_periodic_cost_dampen_should_defer(2,
                                                                       1503,
                                                                       0,
                                                                       80,
                                                                       60,
                                                                       120,
                                                                       10,
                                                                       100,
                                                                       1e5,
                                                                       100.0,
                                                                       5.0,
                                                                       1.0);
    TEST(should_run == 0, "periodic cost dampen requires high cost ratio");

    should_run = lp_refactor_policy_periodic_cost_dampen_should_defer(2,
                                                                       500,
                                                                       0,
                                                                       80,
                                                                       60,
                                                                       120,
                                                                       10,
                                                                       100,
                                                                       1e5,
                                                                       100.0,
                                                                       12.0,
                                                                       1.0);
    TEST(should_run == 0, "periodic cost dampen requires large basis");

    periodic_reason = lp_refactor_policy_periodic_cost_dampen_decision(2,
                                                                        1503,
                                                                        0,
                                                                        80,
                                                                        60,
                                                                        120,
                                                                        10,
                                                                        100,
                                                                        1e5,
                                                                        100.0,
                                                                        12.0,
                                                                        1.0);
    TEST(periodic_reason == LP_PERIODIC_COST_DAMPEN_DEFER,
         "periodic cost dampen decision returns defer");
    periodic_reason = lp_refactor_policy_periodic_cost_dampen_decision(2,
                                                                        500,
                                                                        0,
                                                                        80,
                                                                        60,
                                                                        120,
                                                                        10,
                                                                        100,
                                                                        1e5,
                                                                        100.0,
                                                                        12.0,
                                                                        1.0);
    TEST(periodic_reason == LP_PERIODIC_COST_DAMPEN_BLOCK_SMALL_M,
         "periodic cost dampen decision reports small-m block");
    periodic_reason = lp_refactor_policy_periodic_cost_dampen_decision(2,
                                                                        1503,
                                                                        0,
                                                                        80,
                                                                        116,
                                                                        120,
                                                                        10,
                                                                        100,
                                                                        1e5,
                                                                        100.0,
                                                                        12.0,
                                                                        1.0);
    TEST(periodic_reason == LP_PERIODIC_COST_DAMPEN_BLOCK_UPDATE_RESERVE,
         "periodic cost dampen decision reports update-reserve block");
    TEST(lp_refactor_policy_periodic_cost_dampen_reason_string(
             LP_PERIODIC_COST_DAMPEN_BLOCK_RATIO) != NULL,
         "periodic cost dampen reason string is available");

    policy.interval = 24;
    policy.min_update_age = 12;
    policy.interval_pressure = 0.30;
    policy.run_pressure = 0.39;
    should_run = lp_refactor_policy_should_run_metrics(24, 24, &policy, 0, 0);
    TEST(should_run == 0, "run pressure below trigger does not run");

    policy.run_pressure = 0.40;
    should_run = lp_refactor_policy_should_run_metrics(24, 24, &policy, 0, 0);
    TEST(should_run == 1, "run pressure at trigger runs");

    policy.run_pressure = 0.01;
    should_run = lp_refactor_policy_should_run_metrics(24, 24, &policy, 1, 0);
    TEST(should_run == 1, "bland mode forces periodic cadence");

    should_run = lp_refactor_policy_should_run_metrics(24, 8, &policy, 0, 0);
    TEST(should_run == 0, "min update age gate blocks early cadence");

    printf("Passed %d/%d tests\n", pass, total);
    return (pass == total) ? 0 : 1;
}
