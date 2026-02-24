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
