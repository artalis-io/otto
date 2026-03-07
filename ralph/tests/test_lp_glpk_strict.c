#include <stdio.h>
#include "lp_glpk_strict.h"
#include "lp_bfcp_policy.h"

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
    LPBFCPRefactorSignals sig;

    printf("=== LP GLPK Strict Tests ===\n");

    TEST(lp_glpk_strict_mode_enabled(0) == 0,
         "strict: disabled in default mode");
    TEST(lp_glpk_strict_mode_enabled(1) == 1,
         "strict: enabled in strict mode");
    TEST(lp_glpk_strict_allow_phase1_stagnation_escape(1) == 0,
         "strict: phase1 stagnation escape disabled");
    TEST(lp_glpk_strict_allow_phase1_no_pivot_force(1) == 0,
         "strict: phase1 no-pivot force disabled");
    TEST(lp_glpk_strict_allow_phase1_no_pivot_ladder(1) == 0,
         "strict: phase1 no-pivot ladder disabled");
    TEST(lp_glpk_strict_allow_phase1_dual_rescue(1) == 0,
         "strict: phase1 dual rescue disabled");
    TEST(lp_glpk_strict_allow_phase1_dir_stabilize_force(1) == 0,
         "strict: phase1 dir-stabilize forcing disabled");
    TEST(lp_glpk_strict_allow_phase1_force_pivot_mode(1) == 0,
         "strict: phase1 force-pivot mode disabled");
    TEST(lp_glpk_strict_allow_dual_startup_bound_flip(1) == 0,
         "strict: dual startup bound-flip disabled");
    TEST(lp_glpk_strict_allow_dual_one_shot_recovery(1) == 0,
         "strict: dual one-shot recovery disabled");
    TEST(lp_glpk_strict_use_dual_adaptive_ratio_thresholds(1) == 0,
         "strict: dual adaptive ratio thresholds disabled");
    TEST(lp_glpk_strict_allow_bfcp_adaptive_reasons(1) == 0,
         "strict: bfcp adaptive reasons disabled");
    TEST(lp_glpk_strict_allow_lu_update_adaptive_thresholds(1) == 0,
         "strict: lu adaptive update thresholds disabled");
    TEST(lp_glpk_strict_allow_lu_sparse_skip_heuristics(1) == 0,
         "strict: sparse lu skip heuristics disabled");

    lp_bfcp_policy_refactor_signals_init(&sig);
    sig.strict_mode = 1;
    sig.max_updates = 100;
    sig.num_updates = 50;
    sig.cond_estimate = 2e7;
    TEST(lp_bfcp_policy_effective_update_limit(&sig) == 100,
         "strict: effective update limit stays at max_updates");

    lp_bfcp_policy_refactor_signals_init(&sig);
    sig.strict_mode = 1;
    sig.max_updates = 100;
    sig.num_updates = 12;
    sig.use_ft_updates = 1;
    sig.m = 400;
    sig.ft_num_updates = 12;
    sig.spike_pool_used = 2200;
    TEST(lp_bfcp_policy_refactor_reason(&sig) == LP_BFCP_REFACTOR_REASON_NONE,
         "strict: avg spike density reason suppressed");

    lp_bfcp_policy_refactor_signals_init(&sig);
    sig.strict_mode = 1;
    sig.max_updates = 100;
    sig.num_updates = 50;
    sig.cond_estimate = 2e7;
    TEST(lp_bfcp_policy_refactor_reason(&sig) == LP_BFCP_REFACTOR_REASON_NONE,
         "strict: cond adaptive reason suppressed");

    lp_bfcp_policy_refactor_signals_init(&sig);
    sig.strict_mode = 1;
    sig.growth_factor = 4e8;
    sig.growth_guard_threshold = 1e8;
    TEST(lp_bfcp_policy_refactor_hard_trigger(&sig) == 1,
         "strict: growth guard remains hard trigger");

    lp_bfcp_policy_refactor_signals_init(&sig);
    sig.strict_mode = 1;
    sig.cond_estimate = 2e10;
    TEST(lp_bfcp_policy_refactor_hard_trigger(&sig) == 0,
         "strict: cond severe hard trigger suppressed");

    printf("Passed %d/%d glpk-strict tests\n", pass, total);
    return (pass == total) ? 0 : 1;
}
