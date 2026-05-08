/*
 * test_simplex_phase1_zones.c - Unit tests for Phase 1 zone handlers.
 *
 * Tests the pure-decision helpers and state-transition functions extracted
 * into simplex_phase1_zones.c.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "lp.h"
#include "simplex_phase1_recovery.h"
#include "simplex_phase1_zones.h"

static int tests_passed = 0;
static int tests_total = 0;

#define ASSERT_INT_EQ(a, b, msg) do { \
    tests_total++; \
    if ((a) != (b)) { \
        printf("FAIL: %s (expected %d, got %d)\n", msg, (b), (a)); \
        return 0; \
    } \
    tests_passed++; \
} while(0)

#define ASSERT_DOUBLE_EQ(a, b, msg) do { \
    tests_total++; \
    if (fabs((a) - (b)) > 1e-15) { \
        printf("FAIL: %s (expected %.15g, got %.15g)\n", msg, (double)(b), (double)(a)); \
        return 0; \
    } \
    tests_passed++; \
} while(0)

#define ASSERT_TRUE(cond, msg) do { \
    tests_total++; \
    if (!(cond)) { \
        printf("FAIL: %s\n", msg); \
        return 0; \
    } \
    tests_passed++; \
} while(0)

/* ── P1ZoneResult enum values ────────────────────────────────────── */

static int test_zone_result_enum_values(void) {
    ASSERT_INT_EQ(P1_ZONE_CONTINUE, 0, "P1_ZONE_CONTINUE == 0");
    ASSERT_INT_EQ(P1_ZONE_RETURN_OK, 1, "P1_ZONE_RETURN_OK == 1");
    ASSERT_INT_EQ(P1_ZONE_RETURN_FAIL, 2, "P1_ZONE_RETURN_FAIL == 2");
    ASSERT_INT_EQ(P1_ZONE_PROCEED, 3, "P1_ZONE_PROCEED == 3");
    return 1;
}

/* ── P1IterContext zero-init ─────────────────────────────────────── */

static int test_iter_context_zero_init(void) {
    P1IterContext ctx = {0};
    ASSERT_INT_EQ(ctx.entering, 0, "ctx.entering zero");
    ASSERT_INT_EQ(ctx.leaving, 0, "ctx.leaving zero");
    ASSERT_DOUBLE_EQ(ctx.theta, 0.0, "ctx.theta zero");
    ASSERT_INT_EQ(ctx.ratio_status, 0, "ctx.ratio_status zero");
    ASSERT_INT_EQ(ctx.pivot_status, 0, "ctx.pivot_status zero");
    ASSERT_INT_EQ(ctx.stabilized, 0, "ctx.stabilized zero");
    ASSERT_INT_EQ(ctx.force_dir_refactor_extreme, 0, "ctx.force_dir_refactor_extreme zero");
    ASSERT_INT_EQ(ctx.force_pivot_mode_active, 0, "ctx.force_pivot_mode_active zero");
    ASSERT_DOUBLE_EQ(ctx.phase1_hot_ms_prev, 0.0, "ctx.phase1_hot_ms_prev zero");
    return 1;
}

/* ── p1_zone_tick_cooldowns ──────────────────────────────────────── */

static void seed_exclusion(P1BasisRepairState *bs, int slot, int var, int ttl) {
    bs->excluded_entering_pool[slot] = var;
    bs->excluded_entering_pool_ttl[slot] = ttl;
}

static int test_tick_cooldowns_decrements_all(void) {
    SimplexSolver solver;
    memset(&solver, 0, sizeof(solver));
    SimplexTableau tab;
    memset(&tab, 0, sizeof(tab));
    tab.m = 10;

    P1RecoveryState rs;
    memset(&rs, 0, sizeof(rs));
    seed_exclusion(&rs.basis, 0, 5, 3);
    seed_exclusion(&rs.basis, 1, 7, 1);
    rs.numerical.dir_stabilize_cooldown = 2;
    rs.progress.no_pivot_force_cooldown = 4;
    rs.progress.no_pivot_ladder_rescue_cooldown = 1;
    rs.progress.dir_escape_cooldown = 1;

    p1_zone_tick_cooldowns(&solver, &tab, &rs);

    ASSERT_INT_EQ(rs.basis.excluded_entering_ttl_a, 2, "ttl_a decremented to 2");
    ASSERT_INT_EQ(rs.basis.excluded_entering_a, 5, "entering_a unchanged at non-zero TTL");
    ASSERT_INT_EQ(rs.basis.excluded_entering_ttl_b, 0, "ttl_b decremented to 0");
    ASSERT_INT_EQ(rs.basis.excluded_entering_b, -1, "entering_b cleared at zero TTL");
    ASSERT_INT_EQ(rs.numerical.dir_stabilize_cooldown, 1, "dir_stabilize_cooldown decremented");
    ASSERT_INT_EQ(rs.progress.no_pivot_force_cooldown, 3, "no_pivot_force_cooldown decremented");
    ASSERT_INT_EQ(rs.progress.no_pivot_ladder_rescue_cooldown, 0, "ladder rescue cooldown decremented to 0");
    ASSERT_INT_EQ(rs.progress.dir_escape_cooldown, 0, "dir_escape_cooldown decremented to 0");
    return 1;
}

static int test_tick_cooldowns_no_underflow(void) {
    SimplexSolver solver;
    memset(&solver, 0, sizeof(solver));
    SimplexTableau tab;
    memset(&tab, 0, sizeof(tab));

    P1RecoveryState rs;
    memset(&rs, 0, sizeof(rs));
    /* All cooldowns already at 0 */

    p1_zone_tick_cooldowns(&solver, &tab, &rs);

    ASSERT_INT_EQ(rs.basis.excluded_entering_ttl_a, 0, "ttl_a stays 0");
    ASSERT_INT_EQ(rs.basis.excluded_entering_ttl_b, 0, "ttl_b stays 0");
    ASSERT_INT_EQ(rs.numerical.dir_stabilize_cooldown, 0, "dir_stabilize stays 0");
    ASSERT_INT_EQ(rs.progress.no_pivot_force_cooldown, 0, "no_pivot_force stays 0");
    return 1;
}

static int test_tick_cooldowns_ttl_a_clears_entering(void) {
    SimplexSolver solver;
    memset(&solver, 0, sizeof(solver));
    SimplexTableau tab;
    memset(&tab, 0, sizeof(tab));

    P1RecoveryState rs;
    memset(&rs, 0, sizeof(rs));
    seed_exclusion(&rs.basis, 0, 42, 1);

    p1_zone_tick_cooldowns(&solver, &tab, &rs);

    ASSERT_INT_EQ(rs.basis.excluded_entering_ttl_a, 0, "ttl_a reaches 0");
    ASSERT_INT_EQ(rs.basis.excluded_entering_a, -1, "entering_a cleared to -1");
    return 1;
}

/* ── p1_zone_post_pivot_reset ────────────────────────────────────── */

static int test_post_pivot_reset_clears_state(void) {
    SimplexSolver solver;
    memset(&solver, 0, sizeof(solver));
    SimplexTableau tab;
    memset(&tab, 0, sizeof(tab));

    P1RecoveryState rs;
    p1_recovery_init(&rs, &solver, &tab);

    /* Set some non-zero state that should be reset */
    rs.progress.no_pivot_streak = 10;
    rs.basis.fail_repeat_count = 3;
    rs.basis.fail_reason = 2;
    rs.basis.ratio_breakdown_count = 5;
    rs.basis.ratio_breakdown_last_entering = 42;
    rs.basis.ratio_breakdown_same_entering_streak = 2;
    rs.numerical.dir_skip_event_streak = 7;
    rs.numerical.rc_only_streak = 3;
    rs.numerical.dir_skip_no_recompute_streak = 2;
    rs.numerical.dir_force_refactor_streak = 1;
    rs.numerical.last_dir_skip_entering = 99;
    rs.numerical.dir_skip_same_entering_streak = 4;
    rs.basis.last_failed_stabilize_entering = 50;
    rs.basis.failed_stabilize_same_entering_streak = 3;
    rs.basis.last_failed_stabilize_retry_alt = 60;
    rs.basis.failed_stabilize_retry_alt_streak = 2;
    rs.basis.failed_stabilize_retry_alt_ratio_fail_streak = 1;
    seed_exclusion(&rs.basis, 0, 10, 5);
    seed_exclusion(&rs.basis, 1, 20, 3);
    rs.progress.dir_escape_cooldown = 4;
    rs.numerical.dir_stabilize_moderate_defer_pending = 1;

    p1_zone_post_pivot_reset(&solver, &tab, &rs);

    ASSERT_INT_EQ(rs.progress.no_pivot_streak, 0, "no_pivot_streak reset");
    ASSERT_INT_EQ(rs.basis.fail_repeat_count, 0, "fail_repeat_count reset");
    ASSERT_INT_EQ(rs.basis.ratio_breakdown_count, 0, "ratio_breakdown_count reset");
    ASSERT_INT_EQ(rs.basis.ratio_breakdown_last_entering, -1, "ratio_breakdown_last_entering reset");
    ASSERT_INT_EQ(rs.numerical.dir_skip_event_streak, 0, "dir_skip_event_streak reset");
    ASSERT_INT_EQ(rs.numerical.rc_only_streak, 0, "rc_only_streak reset");
    ASSERT_INT_EQ(rs.numerical.dir_force_refactor_streak, 0, "dir_force_refactor_streak reset");
    ASSERT_INT_EQ(rs.numerical.last_dir_skip_entering, -1, "last_dir_skip_entering reset");
    ASSERT_INT_EQ(rs.basis.last_failed_stabilize_entering, -1, "last_failed_stabilize_entering reset");
    ASSERT_INT_EQ(rs.basis.last_failed_stabilize_retry_alt, -1, "last_failed_stabilize_retry_alt reset");
    ASSERT_INT_EQ(rs.basis.excluded_entering_a, -1, "excluded_entering_a reset");
    ASSERT_INT_EQ(rs.basis.excluded_entering_ttl_a, 0, "excluded_entering_ttl_a reset");
    ASSERT_INT_EQ(rs.basis.excluded_entering_b, -1, "excluded_entering_b reset");
    ASSERT_INT_EQ(rs.basis.excluded_entering_ttl_b, 0, "excluded_entering_ttl_b reset");
    ASSERT_INT_EQ(rs.progress.dir_escape_cooldown, 0, "dir_escape_cooldown reset");
    ASSERT_INT_EQ(rs.numerical.dir_stabilize_moderate_defer_pending, 0, "moderate_defer_pending reset");
    return 1;
}

/* ── Test runner ─────────────────────────────────────────────────── */

typedef int (*TestFunc)(void);

static struct { const char *name; TestFunc func; } all_tests[] = {
    {"zone_result_enum_values", test_zone_result_enum_values},
    {"iter_context_zero_init", test_iter_context_zero_init},
    {"tick_cooldowns_decrements_all", test_tick_cooldowns_decrements_all},
    {"tick_cooldowns_no_underflow", test_tick_cooldowns_no_underflow},
    {"tick_cooldowns_ttl_a_clears_entering", test_tick_cooldowns_ttl_a_clears_entering},
    {"post_pivot_reset_clears_state", test_post_pivot_reset_clears_state},
};

int main(void) {
    int num_tests = (int)(sizeof(all_tests) / sizeof(all_tests[0]));
    int passed = 0;
    int failed = 0;

    for (int i = 0; i < num_tests; i++) {
        printf("=== test_%s ===\n", all_tests[i].name);
        if (all_tests[i].func()) {
            printf("PASS\n");
            passed++;
        } else {
            printf("FAIL\n");
            failed++;
        }
    }

    printf("\nPhase 1 zones tests: %d/%d passed", passed, num_tests);
    if (failed > 0) {
        printf(" (%d FAILED)\n", failed);
        return 1;
    }
    printf("\n");
    return 0;
}
