#include <math.h>
#include <stdio.h>
#include <string.h>
#include "lp.h"
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
    LPPeriodicFeedbackState feedback;
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

    feedback = (LPPeriodicFeedbackState){0.0, 0, 0, 0, 0.0};
    lp_refactor_policy_periodic_feedback_set_hint(&feedback, -5, 2.0);
    TEST(feedback.hint_interval == 0,
         "periodic feedback hint: negative interval clamps to zero");
    TEST(fabs(feedback.hint_pressure - 1.0) < 1e-12,
         "periodic feedback hint: run pressure clamps to unit interval");
    lp_refactor_policy_periodic_feedback_set_hint(&feedback, 12, -1.0);
    TEST(feedback.hint_interval == 12,
         "periodic feedback hint: interval stores positive value");
    TEST(fabs(feedback.hint_pressure - 0.0) < 1e-12,
         "periodic feedback hint: negative pressure clamps to zero");

    feedback = (LPPeriodicFeedbackState){0.0, 0, 0, 24, 0.7};
    lp_refactor_policy_periodic_feedback_record_refactor(
        &feedback, RALPH_REFACTOR_REASON_PERIODIC, 24, -1);
    TEST(fabs(feedback.bias - 0.08) < 1e-12,
         "periodic feedback record: failed periodic refactor tightens bias");
    TEST(feedback.last_reason == RALPH_REFACTOR_REASON_PERIODIC,
         "periodic feedback record: tracks last periodic reason");
    TEST(feedback.last_interval == 24,
         "periodic feedback record: keeps periodic hint interval");
    TEST(feedback.hint_interval == 0 && fabs(feedback.hint_pressure) < 1e-12,
         "periodic feedback record: clears hint state");

    feedback = (LPPeriodicFeedbackState){0.0, 0, 0, 40, 0.2};
    lp_refactor_policy_periodic_feedback_record_refactor(
        &feedback, RALPH_REFACTOR_REASON_PERIODIC, 40, 0);
    TEST(fabs(feedback.bias + 0.06) < 1e-12,
         "periodic feedback record: low-pressure periodic success relaxes bias");
    TEST(feedback.last_interval == 40,
         "periodic feedback record: periodic success updates interval history");

    feedback = (LPPeriodicFeedbackState){
        0.0, RALPH_REFACTOR_REASON_PERIODIC, 30, 0, 0.0};
    lp_refactor_policy_periodic_feedback_record_refactor(
        &feedback, RALPH_REFACTOR_REASON_UPDATE_RECOVERY, 15, 0);
    TEST(fabs(feedback.bias - 0.08) < 1e-12,
         "periodic feedback record: early recovery after periodic tightens bias");

    feedback = (LPPeriodicFeedbackState){0.30, 0, 0, 0, 0.0};
    lp_refactor_policy_periodic_feedback_record_refactor(
        &feedback, RALPH_REFACTOR_REASON_PERIODIC, 0, -1);
    TEST(fabs(feedback.bias - 0.25) < 1e-12,
         "periodic feedback record: bias clamps to configured ceiling");

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

    TEST(lp_refactor_policy_dense_spike_min_updates_override(2, 410) == 16,
         "dense spike warmup override applies to medium Phase 2 basis");
    TEST(lp_refactor_policy_dense_spike_min_updates_override(1, 410) == 0,
         "dense spike warmup override does not apply to Phase 1");
    TEST(lp_refactor_policy_dense_spike_min_updates_override(2, 250) == 0,
         "dense spike warmup override does not apply below medium basis");
    TEST(lp_refactor_policy_dense_spike_min_updates_override(2, 700) == 0,
         "dense spike warmup override leaves large basis policy unchanged");

    TEST(fabs(lp_refactor_policy_periodic_pressure_effective(0, 0.9, 0.2) - 0.9) < 1e-12,
         "periodic pressure effective: cooldown-off keeps run pressure");
    TEST(fabs(lp_refactor_policy_periodic_pressure_effective(1, 0.9, 0.2) - 0.7) < 1e-12,
         "periodic pressure effective: cooldown-on subtracts decay");
    TEST(fabs(lp_refactor_policy_periodic_pressure_effective(1, 0.1, 0.4) - 0.0) < 1e-12,
         "periodic pressure effective: clamps at zero");

    TEST(fabs(lp_refactor_policy_periodic_pressure_decay_recover(1, 0.24) - 0.23) < 1e-12,
         "periodic pressure recover: phase1 uses configured recovery step");
    TEST(fabs(lp_refactor_policy_periodic_pressure_decay_recover(2, 0.24) - 0.23) < 1e-12,
         "periodic pressure recover: phase2 uses configured recovery step");
    TEST(fabs(lp_refactor_policy_periodic_pressure_decay_recover(1, 0.005) - 0.0) < 1e-12,
         "periodic pressure recover: floors at zero");

    TEST(fabs(lp_refactor_policy_periodic_pressure_decay_penalty(1, 0.22) - 0.24) < 1e-12,
         "periodic pressure penalty: phase1 clamps to max");
    TEST(fabs(lp_refactor_policy_periodic_pressure_decay_penalty(2, 0.22) - 0.24) < 1e-12,
         "periodic pressure penalty: phase2 clamps to max");
    TEST(fabs(lp_refactor_policy_periodic_pressure_decay_penalty(1, -1.0) - 0.06) < 1e-12,
         "periodic pressure penalty: negative input sanitized");
    TEST(fabs(lp_refactor_policy_periodic_pressure_decay_recover(99, 0.3) - 0.3) < 1e-12,
         "periodic pressure recover: invalid phase is no-op");
    TEST(fabs(lp_refactor_policy_periodic_pressure_decay_penalty(99, 0.3) - 0.3) < 1e-12,
         "periodic pressure penalty: invalid phase is no-op");

    {
        LPPeriodicRefactorPlan plan = lp_refactor_policy_periodic_plan(
            2, 24, 1503, 120, 24, 10, 100, 1e4, 10.0, 0, 40, 0.0, 0, 0, 0.0);
        TEST(plan.policy.interval == 24,
             "periodic plan: phase2 exposes policy interval");
        TEST(plan.cooldown_eligible == 1,
             "periodic plan: phase2 marks cooldown eligibility");
        TEST(fabs(plan.effective_run_pressure - 1.0) < 1e-12,
             "periodic plan: phase2 effective pressure matches policy");
        TEST(plan.should_run == 1,
             "periodic plan: phase2 cadence selected when due");
    }

    {
        LPPeriodicRefactorPlan plan = lp_refactor_policy_periodic_plan(
            2, 24, 1503, 120, 24, 10, 100, 1e4, 10.0, 0, 40, 0.0, 0, 12, 0.24);
        TEST(fabs(plan.effective_run_pressure - 0.76) < 1e-12,
             "periodic plan: pressure decay lowers effective pressure");
        TEST(plan.should_run == 0,
             "periodic plan: cooldown suppresses due periodic run");
    }

    {
        LPPeriodicRefactorPlan plan_low = lp_refactor_policy_periodic_plan(
            1, 24, 1503, 120, 24, 10, 100, 1e4, 10.0, 0, 10, 0.0, 0, 12, 0.0);
        LPPeriodicRefactorPlan plan_hi = lp_refactor_policy_periodic_plan(
            1, 24, 1503, 120, 24, 10, 100, 1e4, 10.0, 0, 10, 0.0, 80, 12, 0.0);
        TEST(plan_low.cooldown_eligible == 0,
             "periodic plan: phase1 low policy-refactor count not cooldown-eligible");
        TEST(plan_hi.cooldown_eligible == 1,
             "periodic plan: phase1 high policy-refactor count enables cooldown eligibility");
    }

    {
        LPPeriodicRefactorPlan plan = lp_refactor_policy_periodic_plan(
            99, 24, 1503, 120, 24, 10, 100, 1e4, 10.0, 0, 40, 0.0, 0, 12, 0.24);
        TEST(plan.policy.interval == 0 &&
                 plan.cooldown_eligible == 0 &&
                 fabs(plan.effective_run_pressure) < 1e-12 &&
                 plan.should_run == 0,
             "periodic plan: invalid phase returns empty plan");
    }

    TEST(lp_refactor_policy_periodic_cooldown_tick(3) == 2,
         "periodic cooldown tick: decrements positive cooldown");
    TEST(lp_refactor_policy_periodic_cooldown_tick(0) == 0,
         "periodic cooldown tick: clamps at zero");
    TEST(lp_refactor_policy_periodic_cooldown_tick(-7) == 0,
         "periodic cooldown tick: sanitizes negative input");

    TEST(lp_refactor_policy_periodic_cooldown_extend(5, 8) == 8,
         "periodic cooldown extend: raises cooldown to candidate");
    TEST(lp_refactor_policy_periodic_cooldown_extend(10, 3) == 10,
         "periodic cooldown extend: keeps larger current cooldown");
    TEST(lp_refactor_policy_periodic_cooldown_extend(-2, -5) == 0,
         "periodic cooldown extend: sanitizes negative inputs");

    {
        int cooldown = 12;
        double decay = 0.20;
        lp_refactor_policy_periodic_post_refactor_update(
            1, 1, 24, 0, &cooldown, &decay);
        TEST(cooldown == 48,
             "periodic post-refactor: phase1 success extends cooldown window");
        TEST(fabs(decay - 0.24) < 1e-12,
             "periodic post-refactor: phase1 success applies pressure penalty");
    }

    {
        int cooldown = 12;
        double decay = 0.20;
        lp_refactor_policy_periodic_post_refactor_update(
            2, 1, 24, 0, &cooldown, &decay);
        TEST(cooldown == 36,
             "periodic post-refactor: phase2 success extends cooldown window");
        TEST(fabs(decay - 0.24) < 1e-12,
             "periodic post-refactor: phase2 success applies pressure penalty");
    }

    {
        int cooldown = 12;
        double decay = 0.20;
        lp_refactor_policy_periodic_post_refactor_update(
            1, 0, 24, 0, &cooldown, &decay);
        TEST(cooldown == 12 && fabs(decay - 0.20) < 1e-12,
             "periodic post-refactor: cooldown-ineligible success keeps state");
    }

    {
        int cooldown = 12;
        double decay = 0.20;
        lp_refactor_policy_periodic_post_refactor_update(
            1, 1, 24, -1, &cooldown, &decay);
        TEST(cooldown == 0 && fabs(decay) < 1e-12,
             "periodic post-refactor: failure clears cooldown and pressure decay");
    }

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

    TEST(lp_refactor_policy_phase1_small_pivot_refactor_allowed(0, 0, 20) == 0,
         "small-pivot policy: disabled when force flag is off");
    TEST(lp_refactor_policy_phase1_small_pivot_refactor_allowed(1, 0, 0) == 0,
         "small-pivot policy: blocked on fresh basis without repeats");
    TEST(lp_refactor_policy_phase1_small_pivot_refactor_allowed(
             1, RALPH_PHASE1_REPEAT_REFACTOR_TRIGGER, 0) == 1,
         "small-pivot policy: repeat streak enables forced refactor");
    TEST(lp_refactor_policy_phase1_small_pivot_refactor_allowed(1, 0, 6) == 1,
         "small-pivot policy: update age enables forced refactor");

    TEST(lp_refactor_policy_choose_basis_action(0.0, 0, 0, 0, 10, 1.0, 1e8) ==
             LP_BASIS_ACTION_ABORT,
         "basis action: tiny pivot aborts");
    TEST(lp_refactor_policy_choose_basis_action(1.0, 0, -1, 0, 10, 1.0, 1e8) ==
             LP_BASIS_ACTION_REFACTOR,
         "basis action: failed update escalates to refactor");
    TEST(lp_refactor_policy_choose_basis_action(1.0, 0, -2, 0, 10, 1.0, 1e8) ==
             LP_BASIS_ACTION_REPAIR,
         "basis action: failed refactor escalates to repair");
    TEST(lp_refactor_policy_choose_basis_action(1.0, 0, -3, 0, 10, 1.0, 1e8) ==
             LP_BASIS_ACTION_ABORT,
         "basis action: failed repair aborts");
    TEST(lp_refactor_policy_choose_basis_action(
             1.0, 1, 0, RALPH_PHASE1_REPEAT_REFACTOR_TRIGGER, 0, 1.0, 1e8) ==
             LP_BASIS_ACTION_REFACTOR,
         "basis action: repeat-triggered force requests refactor");
    TEST(lp_refactor_policy_choose_basis_action(1.0, 0, 0, 0, 10, 2e8, 1e8) ==
             LP_BASIS_ACTION_REFACTOR,
         "basis action: growth-triggered refactor");
    TEST(lp_refactor_policy_choose_basis_action(1.0, 0, 0, 0, 10, 1.0, 1e8) ==
             LP_BASIS_ACTION_UPDATE,
         "basis action: healthy path uses update");

    TEST(strcmp(lp_refactor_policy_phase1_no_pivot_force_reason_string(
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN),
                "ratio_breakdown") == 0,
         "no-pivot reason string: ratio");
    TEST(strcmp(lp_refactor_policy_phase1_no_pivot_force_reason_string(
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP),
                "dir_skip") == 0,
         "no-pivot reason string: dir-skip");
    TEST(strcmp(lp_refactor_policy_phase1_no_pivot_force_reason_string(
                    LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL),
                "pivot_fail") == 0,
         "no-pivot reason string: pivot-fail");
    TEST(strcmp(lp_refactor_policy_phase1_no_pivot_force_reason_string(999),
                "unknown") == 0,
         "no-pivot reason string: unknown fallback");

    TEST(lp_refactor_policy_phase1_no_pivot_force_threshold(300, 0) == 48,
         "no-pivot force threshold: baseline");
    TEST(lp_refactor_policy_phase1_no_pivot_force_threshold(1300, 90) == 32,
         "no-pivot force threshold: large+degenerate tightens");
    TEST(lp_refactor_policy_phase1_no_pivot_force_threshold(1300, 200) == 24,
         "no-pivot force threshold: clamps to minimum");

    {
        int next_streak = -1;
        int next_cooldown = -1;
        int force = lp_refactor_policy_phase1_no_pivot_force_transition(
            1300, 100, 40, 5, &next_streak, &next_cooldown);
        TEST(force == 0, "no-pivot force transition: blocked by cooldown");
        TEST(next_streak == 40 && next_cooldown == 5,
             "no-pivot force transition: cooldown keeps state");
    }

    {
        int next_streak = -1;
        int next_cooldown = -1;
        int force = lp_refactor_policy_phase1_no_pivot_force_transition(
            400, 120, 100, 0, &next_streak, &next_cooldown);
        TEST(force == 0, "no-pivot force transition: blocked on small m");
        TEST(next_streak == 100 && next_cooldown == 0,
             "no-pivot force transition: small m keeps state");
    }

    {
        int next_streak = -1;
        int next_cooldown = -1;
        int force = lp_refactor_policy_phase1_no_pivot_force_transition(
            1300, 90, 32, 0, &next_streak, &next_cooldown);
        TEST(force == 1, "no-pivot force transition: triggers at threshold");
        TEST(next_streak == 0 && next_cooldown == 24,
             "no-pivot force transition: resets streak and arms cooldown");
    }

    TEST(lp_refactor_policy_phase1_no_pivot_ladder_refactor_threshold(
             800,
             0,
             LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN,
             0) == 8,
         "no-pivot ladder threshold: ratio baseline");
    TEST(lp_refactor_policy_phase1_no_pivot_ladder_refactor_threshold(
             1300,
             100,
             LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP,
             1) == 7,
         "no-pivot ladder threshold: dir-skip adjusted");
    TEST(lp_refactor_policy_phase1_no_pivot_ladder_refactor_threshold(
             300,
             0,
             LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL,
             0) == 4,
         "no-pivot ladder threshold: pivot-fail baseline");

    {
        int threshold = -1;
        int step = lp_refactor_policy_phase1_no_pivot_ladder_step(
            800,
            0,
            LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN,
            1,
            2,
            0,
            &threshold);
        TEST(step == 0 && threshold == 8,
             "no-pivot ladder step: retry before rescue threshold");
    }

    {
        int threshold = -1;
        int step = lp_refactor_policy_phase1_no_pivot_ladder_step(
            800,
            0,
            LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN,
            1,
            4,
            0,
            &threshold);
        TEST(step == 1 && threshold == 8,
             "no-pivot ladder step: dual rescue on rescue cadence");
    }

    {
        int threshold = -1;
        int step = lp_refactor_policy_phase1_no_pivot_ladder_step(
            800,
            0,
            LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN,
            1,
            8,
            0,
            &threshold);
        TEST(step == 2 && threshold == 8,
             "no-pivot ladder step: force refactor on sustained no-progress");
    }

    TEST(lp_refactor_policy_phase1_dir_skip_ladder_rescue_due(15) == 0,
         "dir-skip rescue cadence: below start");
    TEST(lp_refactor_policy_phase1_dir_skip_ladder_rescue_due(16) == 1,
         "dir-skip rescue cadence: start threshold");
    TEST(lp_refactor_policy_phase1_dir_skip_ladder_rescue_due(24) == 1,
         "dir-skip rescue cadence: periodic multiple");

    TEST(lp_refactor_policy_phase1_dir_skip_force_pivot_threshold(600, 0) == 64,
         "force-pivot threshold: baseline");
    TEST(lp_refactor_policy_phase1_dir_skip_force_pivot_threshold(1300, 100) == 32,
         "force-pivot threshold: tightened by scale+degeneracy");
    TEST(lp_refactor_policy_phase1_dir_skip_force_pivot_budget(600, 0) == 12,
         "force-pivot budget: baseline");
    TEST(lp_refactor_policy_phase1_dir_skip_force_pivot_budget(1300, 200) == 28,
         "force-pivot budget: increased under heavy degeneracy");

    {
        int next_streak = -1;
        int next_budget = -1;
        int next_pending = 0;
        int next_reason = LP_PHASE1_NO_PIVOT_FORCE_REASON_UNKNOWN;
        int activated = lp_refactor_policy_phase1_activate_force_pivot_mode(
            1300, 120, 40, 0, 1, &next_streak, &next_budget, &next_pending, &next_reason);
        TEST(activated == 1, "force-pivot activation: triggers above threshold");
        TEST(next_streak == 0 && next_budget == 20,
             "force-pivot activation: resets streak and arms budget");
        TEST(next_pending == 1 && next_reason == LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP,
             "force-pivot activation: marks pending DIR_SKIP force");
    }

    {
        int next_streak = -1;
        int next_budget = -1;
        int next_pending = 0;
        int next_reason = LP_PHASE1_NO_PIVOT_FORCE_REASON_UNKNOWN;
        int activated = lp_refactor_policy_phase1_activate_force_pivot_mode(
            1300, 120, 20, 5, 1, &next_streak, &next_budget, &next_pending, &next_reason);
        TEST(activated == 0, "force-pivot activation: blocked by active budget");
        TEST(next_streak == 20 && next_budget == 5,
             "force-pivot activation: preserves state when blocked");
    }

    {
        int next_cooldown = -1;
        int triggered = -1;
        int hard_bypass = -1;
        int suppress = lp_refactor_policy_phase1_dir_stabilize_escape_gate_plan(
            1200, 120, 48, 12, 0, 0, 1, 0,
            &next_cooldown, &triggered, &hard_bypass);
        TEST(suppress == 1 && triggered == 1 && hard_bypass == 0 && next_cooldown > 0,
             "dir-escape gate: triggers suppression on chronic treadmill");
    }

    {
        int next_cooldown = -1;
        int triggered = -1;
        int hard_bypass = -1;
        int suppress = lp_refactor_policy_phase1_dir_stabilize_escape_gate_plan(
            1200, 120, 48, 12, 20, 0, 1, 1,
            &next_cooldown, &triggered, &hard_bypass);
        TEST(suppress == 0 && hard_bypass == 1,
             "dir-escape gate: hard LU bypass avoids suppression");
    }

    TEST(lp_refactor_policy_phase1_force_pivot_refactor_relax_plan(
             1200, 120, 2, 1, 0, 0, 0, 10, 8, 0) == 1,
         "force-pivot relax: enabled under stable successful rescue");
    TEST(lp_refactor_policy_phase1_force_pivot_refactor_relax_plan(
             1200, 120, 2, 1, 0, 1, 0, 10, 8, 0) == 0,
         "force-pivot relax: blocked by LU-health force");

    TEST(lp_refactor_policy_phase1_force_extreme_refactor_relax_plan(
             1200, 120, 2, 120.0, 1, 0, 0, 10, 8, 0) == 1,
         "force-extreme relax: enabled on moderate ratio");
    TEST(lp_refactor_policy_phase1_force_extreme_refactor_relax_plan(
             1200, 120, 2, 400.0, 1, 0, 0, 10, 8, 0) == 0,
         "force-extreme relax: blocked on severe ratio");

    TEST(lp_refactor_policy_phase1_soft_lu_policy_cooldown_updates(600, 50, 80) == 0,
         "phase1 soft-lu cooldown: disabled for small basis");
    TEST(lp_refactor_policy_phase1_soft_lu_policy_cooldown_updates(1200, 50, 80) == 40,
         "phase1 soft-lu cooldown: scales with periodic interval");
    TEST(lp_refactor_policy_phase1_soft_lu_policy_cooldown_updates(1200, 50, 300) == 48,
         "phase1 soft-lu cooldown: clamped to max");

    {
        int last_iter = -1;
        int burst = 0;
        int demoted = 0;
        int demotions = 0;
        for (int k = 0; k < 4; k++) {
            lp_refactor_policy_phase1_reinvert_pressure_safety_step(
                k * 10, 10, 20, 0, 0, 0, &last_iter, &burst, &demoted, &demotions);
        }
        TEST(demoted == 1 && demotions == 1,
             "reinvert pressure: repeated events demote control");
        lp_refactor_policy_phase1_reinvert_pressure_safety_step(
            50, 10, 20, 0, 0, 0, &last_iter, &burst, &demoted, &demotions);
        TEST(demoted == 1, "reinvert pressure: demotion holds during cooldown");
        lp_refactor_policy_phase1_reinvert_pressure_safety_step(
            90, 10, 20, 0, 0, 0, &last_iter, &burst, &demoted, &demotions);
        TEST(demoted == 0, "reinvert pressure: demotion clears after cooldown");
    }

    {
        int last_iter = 0;
        int burst = 3;
        int demoted = 0;
        int demotions = 0;
        lp_refactor_policy_phase1_reinvert_pressure_safety_step(
            10, 10, 20, 5, 30, 1, &last_iter, &burst, &demoted, &demotions);
        TEST(demoted == 0 && burst == 3,
             "reinvert pressure: hard LU trigger blocks burst escalation");
    }

    TEST(lp_refactor_policy_phase1_stagnation_escape_decision(
             96, 0.0, 10.0, 18, 20, 4, 8, 10, 8, 1, 2, 0, 0) == 1,
         "stagnation decision: triggers on flat objective + retry/recompute pressure");
    TEST(lp_refactor_policy_phase1_stagnation_escape_decision(
             96, 1e-2, 10.0, 18, 20, 4, 8, 10, 8, 1, 2, 0, 0) == 0,
         "stagnation decision: blocked when objective is still moving");
    TEST(lp_refactor_policy_phase1_stagnation_escape_decision(
             96, 0.0, 10.0, 18, 20, 4, 8, 10, 8, 1, 2, 0, 5) == 0,
         "stagnation decision: blocked by cooldown");

    TEST(lp_refactor_policy_phase1_degen_threshold(699) == 50,
         "phase1 degen threshold: default below large-m boundary");
    TEST(lp_refactor_policy_phase1_degen_threshold(700) == 20,
         "phase1 degen threshold: tightened at large-m boundary");

    TEST(lp_refactor_policy_phase1_stall_threshold(699) == 50,
         "phase1 stall threshold: default below large-m boundary");
    TEST(lp_refactor_policy_phase1_stall_threshold(700) == 30,
         "phase1 stall threshold: tightened at large-m boundary");

    TEST(lp_refactor_policy_phase1_recompute_interval() == 25,
         "phase1 recompute interval: policy constant");

    TEST(fabs(lp_refactor_policy_phase1_stall_obj_tol(9.0) - 1e-3) < 1e-12,
         "phase1 stall obj tol: finite input uses relative rule");
    TEST(fabs(lp_refactor_policy_phase1_stall_obj_tol(NAN) - 1e-4) < 1e-12,
         "phase1 stall obj tol: non-finite input sanitized");

    TEST(lp_refactor_policy_phase1_ratio_breakdown_limit(699, 100) == 60,
         "phase1 ratio-breakdown limit: no tighten for small basis");
    TEST(lp_refactor_policy_phase1_ratio_breakdown_limit(700, 2) == 60,
         "phase1 ratio-breakdown limit: no tighten below repeat threshold");
    TEST(lp_refactor_policy_phase1_ratio_breakdown_limit(700, 3) == 20,
         "phase1 ratio-breakdown limit: tighten on large basis repeated entering");
    TEST(lp_refactor_policy_phase1_ratio_breakdown_limit(700, -7) == 60,
         "phase1 ratio-breakdown limit: negative streak sanitized");

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
                                                                    799,
                                                                    0,
                                                                    0,
                                                                    60,
                                                                    120,
                                                                    10,
                                                                    100,
                                                                    1e5,
                                                                    100.0,
                                                                    12.0,
                                                                    1.0);
    TEST(should_run == 1,
         "soft lu cost gate defers large phase2 nondegenerate health refactors");

    should_run = lp_refactor_policy_soft_lu_cost_gate_should_defer(1,
                                                                    1503,
                                                                    0,
                                                                    0,
                                                                    60,
                                                                    120,
                                                                    10,
                                                                    100,
                                                                    1e5,
                                                                    100.0,
                                                                    12.0,
                                                                    1.0);
    TEST(should_run == 0,
         "soft lu cost gate keeps phase1 nondegenerate threshold");

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
                                                                       1.0,
                                                                       2,
                                                                       16);
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
                                                                       1.0,
                                                                       2,
                                                                       16);
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
                                                                       1.0,
                                                                       2,
                                                                       16);
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
                                                                       1.0,
                                                                       2,
                                                                       16);
    TEST(should_run == 0, "periodic cost dampen requires large basis");

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
                                                                       1.0,
                                                                       0,
                                                                       16);
    TEST(should_run == 0, "periodic cost dampen waits for warmup");

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
                                                                        1.0,
                                                                        2,
                                                                        16);
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
                                                                        1.0,
                                                                        2,
                                                                        16);
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
                                                                        1.0,
                                                                        2,
                                                                        16);
    TEST(periodic_reason == LP_PERIODIC_COST_DAMPEN_BLOCK_UPDATE_RESERVE,
         "periodic cost dampen decision reports update-reserve block");
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
                                                                        1.0,
                                                                        0,
                                                                        16);
    TEST(periodic_reason == LP_PERIODIC_COST_DAMPEN_BLOCK_WARMUP,
         "periodic cost dampen decision reports warmup block");
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
