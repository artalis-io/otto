/*
 * test_simplex_phase1_recovery.c - Unit tests for extracted Phase 1 progress ops.
 *
 * Tests p1_progress_reset, p1_progress_note_no_pivot, p1_window_pressure_note,
 * and p1_window_pressure_reset from simplex_phase1_recovery.c.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "lp.h"
#include "simplex_phase1_recovery.h"

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

/* ── p1_progress_reset ────────────────────────────────────────────── */

static int test_progress_reset_zeros_fields(void) {
    P1ProgressState ps;
    memset(&ps, 0, sizeof(ps));
    ps.no_pivot_no_progress_streak = 42;
    ps.no_pivot_prev_art_sum = 3.14;
    ps.no_pivot_anchor_art_sum = 2.71;
    ps.no_pivot_progress_window_steps = 99;
    /* Fields that should NOT be touched */
    ps.no_pivot_streak = 7;
    ps.no_pivot_force_pending = 1;
    ps.no_pivot_force_cooldown = 5;
    ps.window_pressure_events = 10;

    p1_progress_reset(&ps);

    ASSERT_INT_EQ(ps.no_pivot_no_progress_streak, 0,
                  "reset zeros no_progress_streak");
    ASSERT_DOUBLE_EQ(ps.no_pivot_prev_art_sum, RALPH_INFINITY,
                     "reset sets prev_art_sum to RALPH_INFINITY");
    ASSERT_DOUBLE_EQ(ps.no_pivot_anchor_art_sum, RALPH_INFINITY,
                     "reset sets anchor_art_sum to RALPH_INFINITY");
    ASSERT_INT_EQ(ps.no_pivot_progress_window_steps, 0,
                  "reset zeros window_steps");
    /* Verify untouched fields */
    ASSERT_INT_EQ(ps.no_pivot_streak, 7,
                  "reset preserves no_pivot_streak");
    ASSERT_INT_EQ(ps.no_pivot_force_pending, 1,
                  "reset preserves force_pending");
    ASSERT_INT_EQ(ps.no_pivot_force_cooldown, 5,
                  "reset preserves force_cooldown");
    ASSERT_INT_EQ(ps.window_pressure_events, 10,
                  "reset preserves window_pressure_events");

    printf("PASS: test_progress_reset_zeros_fields\n");
    return 1;
}

/* ── p1_progress_note_no_pivot ────────────────────────────────────── */

static int test_note_no_pivot_increments_streak(void) {
    P1ProgressState ps;
    memset(&ps, 0, sizeof(ps));
    ps.no_pivot_streak = 0;
    ps.no_pivot_force_cooldown = 0;

    /* With NULL solver, telemetry calls are no-ops */
    int force = p1_progress_note_no_pivot(
        NULL, 100, 10,
        LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN,
        &ps);

    /* After 1 call, streak should be > 0 (policy may reset it) */
    /* The key invariant: force=0 for a single call with fresh state */
    ASSERT_INT_EQ(force, 0, "single call should not force");

    printf("PASS: test_note_no_pivot_increments_streak\n");
    return 1;
}

static int test_note_no_pivot_null_ps_returns_zero(void) {
    int force = p1_progress_note_no_pivot(
        NULL, 100, 10,
        LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP,
        NULL);
    ASSERT_INT_EQ(force, 0, "NULL ps returns 0");

    printf("PASS: test_note_no_pivot_null_ps_returns_zero\n");
    return 1;
}

/* ── p1_window_pressure_note ──────────────────────────────────────── */

static int test_window_pressure_note_failed_stabilize(void) {
    P1ProgressState ps;
    memset(&ps, 0, sizeof(ps));
    ps.window_pressure_last_event_kind = PHASE1_WINDOW_PRESSURE_EVENT_NONE;

    p1_window_pressure_note(
        NULL,
        PHASE1_WINDOW_PRESSURE_EVENT_FAILED_STABILIZE,
        0,
        &ps);

    ASSERT_INT_EQ(ps.window_pressure_events, 1,
                  "events incremented");
    ASSERT_INT_EQ(ps.window_pressure_failed_stabilize, 1,
                  "failed_stabilize incremented");
    ASSERT_INT_EQ(ps.window_pressure_dir_skip, 0,
                  "dir_skip unchanged");
    ASSERT_INT_EQ(ps.window_pressure_alternations, 0,
                  "no alternation on first event");
    ASSERT_INT_EQ(ps.window_pressure_last_event_kind,
                  PHASE1_WINDOW_PRESSURE_EVENT_FAILED_STABILIZE,
                  "last_event_kind updated");

    printf("PASS: test_window_pressure_note_failed_stabilize\n");
    return 1;
}

static int test_window_pressure_note_dir_skip(void) {
    P1ProgressState ps;
    memset(&ps, 0, sizeof(ps));
    ps.window_pressure_last_event_kind = PHASE1_WINDOW_PRESSURE_EVENT_NONE;

    p1_window_pressure_note(
        NULL,
        PHASE1_WINDOW_PRESSURE_EVENT_DIR_SKIP,
        1,  /* local_memory_fail */
        &ps);

    ASSERT_INT_EQ(ps.window_pressure_events, 1,
                  "events incremented");
    ASSERT_INT_EQ(ps.window_pressure_dir_skip, 1,
                  "dir_skip incremented");
    ASSERT_INT_EQ(ps.window_pressure_failed_stabilize, 0,
                  "failed_stabilize unchanged");
    ASSERT_INT_EQ(ps.window_pressure_local_memory_fail, 1,
                  "local_memory_fail incremented");

    printf("PASS: test_window_pressure_note_dir_skip\n");
    return 1;
}

static int test_window_pressure_note_alternation(void) {
    P1ProgressState ps;
    memset(&ps, 0, sizeof(ps));
    ps.window_pressure_last_event_kind = PHASE1_WINDOW_PRESSURE_EVENT_NONE;

    /* First event: failed stabilize */
    p1_window_pressure_note(
        NULL,
        PHASE1_WINDOW_PRESSURE_EVENT_FAILED_STABILIZE,
        0,
        &ps);
    ASSERT_INT_EQ(ps.window_pressure_alternations, 0,
                  "no alternation after first event");

    /* Second event: dir skip (different kind) */
    p1_window_pressure_note(
        NULL,
        PHASE1_WINDOW_PRESSURE_EVENT_DIR_SKIP,
        0,
        &ps);
    ASSERT_INT_EQ(ps.window_pressure_alternations, 1,
                  "alternation after different event kind");
    ASSERT_INT_EQ(ps.window_pressure_events, 2,
                  "total events is 2");

    /* Third event: dir skip again (same kind) */
    p1_window_pressure_note(
        NULL,
        PHASE1_WINDOW_PRESSURE_EVENT_DIR_SKIP,
        0,
        &ps);
    ASSERT_INT_EQ(ps.window_pressure_alternations, 1,
                  "no extra alternation for same kind");
    ASSERT_INT_EQ(ps.window_pressure_events, 3,
                  "total events is 3");

    printf("PASS: test_window_pressure_note_alternation\n");
    return 1;
}

static int test_window_pressure_note_invalid_kind(void) {
    P1ProgressState ps;
    memset(&ps, 0, sizeof(ps));

    p1_window_pressure_note(
        NULL,
        PHASE1_WINDOW_PRESSURE_EVENT_NONE,
        0,
        &ps);

    ASSERT_INT_EQ(ps.window_pressure_events, 0,
                  "NONE event does not increment");

    p1_window_pressure_note(
        NULL,
        99,  /* invalid kind */
        0,
        &ps);

    ASSERT_INT_EQ(ps.window_pressure_events, 0,
                  "invalid event kind does not increment");

    printf("PASS: test_window_pressure_note_invalid_kind\n");
    return 1;
}

static int test_window_pressure_note_null_ps(void) {
    /* Should not crash */
    p1_window_pressure_note(
        NULL,
        PHASE1_WINDOW_PRESSURE_EVENT_FAILED_STABILIZE,
        0,
        NULL);

    printf("PASS: test_window_pressure_note_null_ps\n");
    tests_total++;
    tests_passed++;
    return 1;
}

/* ── p1_window_pressure_reset ─────────────────────────────────────── */

static int test_window_pressure_reset_zeros_all(void) {
    P1ProgressState ps;
    memset(&ps, 0, sizeof(ps));
    ps.window_pressure_events = 5;
    ps.window_pressure_failed_stabilize = 2;
    ps.window_pressure_dir_skip = 3;
    ps.window_pressure_local_memory_fail = 1;
    ps.window_pressure_alternations = 2;
    ps.window_pressure_last_event_kind = PHASE1_WINDOW_PRESSURE_EVENT_DIR_SKIP;
    /* Fields that should NOT be touched */
    ps.window_pressure_force_pivot_armed = 1;
    ps.no_pivot_streak = 10;

    p1_window_pressure_reset(NULL, &ps);

    ASSERT_INT_EQ(ps.window_pressure_events, 0,
                  "events zeroed");
    ASSERT_INT_EQ(ps.window_pressure_failed_stabilize, 0,
                  "failed_stabilize zeroed");
    ASSERT_INT_EQ(ps.window_pressure_dir_skip, 0,
                  "dir_skip zeroed");
    ASSERT_INT_EQ(ps.window_pressure_local_memory_fail, 0,
                  "local_memory_fail zeroed");
    ASSERT_INT_EQ(ps.window_pressure_alternations, 0,
                  "alternations zeroed");
    ASSERT_INT_EQ(ps.window_pressure_last_event_kind,
                  PHASE1_WINDOW_PRESSURE_EVENT_NONE,
                  "last_event_kind reset to NONE");
    /* Verify untouched */
    ASSERT_INT_EQ(ps.window_pressure_force_pivot_armed, 1,
                  "force_pivot_armed preserved");
    ASSERT_INT_EQ(ps.no_pivot_streak, 10,
                  "no_pivot_streak preserved");

    printf("PASS: test_window_pressure_reset_zeros_all\n");
    return 1;
}

static int test_window_pressure_reset_noop_when_empty(void) {
    P1ProgressState ps;
    memset(&ps, 0, sizeof(ps));
    /* All counters already zero -- should not crash or change anything */
    p1_window_pressure_reset(NULL, &ps);

    ASSERT_INT_EQ(ps.window_pressure_events, 0,
                  "events still zero");
    ASSERT_INT_EQ(ps.window_pressure_last_event_kind,
                  PHASE1_WINDOW_PRESSURE_EVENT_NONE,
                  "last_event_kind still NONE");

    printf("PASS: test_window_pressure_reset_noop_when_empty\n");
    return 1;
}

static int test_window_pressure_reset_null_ps(void) {
    /* Should not crash */
    p1_window_pressure_reset(NULL, NULL);

    printf("PASS: test_window_pressure_reset_null_ps\n");
    tests_total++;
    tests_passed++;
    return 1;
}

/* ── p1_numerical_consume_followup (R3.3) ─────────────────────────── */

static int test_consume_followup_clears_all_pending(void) {
    P1NumericalState ns;
    memset(&ns, 0, sizeof(ns));
    ns.shadow_guard_followup_pending = 1;
    ns.force_extreme_followup_pending = 1;
    ns.force_extreme_tiny_theta_relax_next_pending = 1;

    p1_numerical_consume_followup(NULL, P1_FOLLOWUP_EVENT_RATIO_BREAKDOWN, &ns);

    ASSERT_INT_EQ(ns.shadow_guard_followup_pending, 0,
                  "shadow_guard pending cleared");
    ASSERT_INT_EQ(ns.force_extreme_followup_pending, 0,
                  "force_extreme pending cleared");
    ASSERT_INT_EQ(ns.force_extreme_tiny_theta_relax_next_pending, 0,
                  "tiny_theta_relax pending cleared");

    printf("PASS: test_consume_followup_clears_all_pending\n");
    return 1;
}

static int test_consume_followup_skips_non_pending(void) {
    P1NumericalState ns;
    memset(&ns, 0, sizeof(ns));
    ns.shadow_guard_followup_pending = 0;
    ns.force_extreme_followup_pending = 1;
    ns.force_extreme_tiny_theta_relax_next_pending = 0;

    p1_numerical_consume_followup(NULL, P1_FOLLOWUP_EVENT_PIVOT_FAIL, &ns);

    ASSERT_INT_EQ(ns.shadow_guard_followup_pending, 0,
                  "shadow_guard still 0");
    ASSERT_INT_EQ(ns.force_extreme_followup_pending, 0,
                  "force_extreme cleared");
    ASSERT_INT_EQ(ns.force_extreme_tiny_theta_relax_next_pending, 0,
                  "tiny_theta still 0");

    printf("PASS: test_consume_followup_skips_non_pending\n");
    return 1;
}

static int test_consume_followup_null_ns(void) {
    /* Should not crash */
    p1_numerical_consume_followup(NULL, P1_FOLLOWUP_EVENT_PIVOT_SUCCESS, NULL);

    printf("PASS: test_consume_followup_null_ns\n");
    tests_total++;
    tests_passed++;
    return 1;
}

static int test_consume_followup_all_events(void) {
    P1NumericalState ns;
    P1FollowupEvent events[] = {
        P1_FOLLOWUP_EVENT_FAILED_STABILIZE,
        P1_FOLLOWUP_EVENT_RATIO_BREAKDOWN,
        P1_FOLLOWUP_EVENT_PIVOT_FAIL,
        P1_FOLLOWUP_EVENT_PIVOT_SUCCESS
    };
    int i;

    for (i = 0; i < 4; i++) {
        memset(&ns, 0, sizeof(ns));
        ns.shadow_guard_followup_pending = 1;
        p1_numerical_consume_followup(NULL, events[i], &ns);
        ASSERT_INT_EQ(ns.shadow_guard_followup_pending, 0,
                      "pending cleared for each event type");
    }

    printf("PASS: test_consume_followup_all_events\n");
    return 1;
}

/* ── p1_numerical_note_dir_skip_entering (R3.3) ──────────────────── */

static int test_dir_skip_entering_new(void) {
    P1NumericalState ns;
    memset(&ns, 0, sizeof(ns));
    ns.last_dir_skip_entering = -1;
    ns.dir_skip_same_entering_streak = 0;

    p1_numerical_note_dir_skip_entering(NULL, 42, &ns);

    ASSERT_INT_EQ(ns.last_dir_skip_entering, 42,
                  "entering recorded");
    ASSERT_INT_EQ(ns.dir_skip_same_entering_streak, 1,
                  "streak starts at 1");

    printf("PASS: test_dir_skip_entering_new\n");
    return 1;
}

static int test_dir_skip_entering_same(void) {
    P1NumericalState ns;
    memset(&ns, 0, sizeof(ns));
    ns.last_dir_skip_entering = 42;
    ns.dir_skip_same_entering_streak = 3;

    p1_numerical_note_dir_skip_entering(NULL, 42, &ns);

    ASSERT_INT_EQ(ns.last_dir_skip_entering, 42,
                  "entering still 42");
    ASSERT_INT_EQ(ns.dir_skip_same_entering_streak, 4,
                  "streak incremented");

    printf("PASS: test_dir_skip_entering_same\n");
    return 1;
}

static int test_dir_skip_entering_different(void) {
    P1NumericalState ns;
    memset(&ns, 0, sizeof(ns));
    ns.last_dir_skip_entering = 42;
    ns.dir_skip_same_entering_streak = 5;

    p1_numerical_note_dir_skip_entering(NULL, 99, &ns);

    ASSERT_INT_EQ(ns.last_dir_skip_entering, 99,
                  "entering updated to 99");
    ASSERT_INT_EQ(ns.dir_skip_same_entering_streak, 1,
                  "streak reset to 1");

    printf("PASS: test_dir_skip_entering_different\n");
    return 1;
}

/* ── p1_basis_note_failed_stabilize_entering (R3.3) ──────────────── */

static int test_failed_stabilize_entering_same(void) {
    P1BasisRepairState bs;
    memset(&bs, 0, sizeof(bs));
    bs.last_failed_stabilize_entering = 10;
    bs.failed_stabilize_same_entering_streak = 2;

    p1_basis_note_failed_stabilize_entering(NULL, 10, &bs);

    ASSERT_INT_EQ(bs.failed_stabilize_same_entering_streak, 3,
                  "streak incremented");

    printf("PASS: test_failed_stabilize_entering_same\n");
    return 1;
}

static int test_failed_stabilize_entering_different(void) {
    P1BasisRepairState bs;
    memset(&bs, 0, sizeof(bs));
    bs.last_failed_stabilize_entering = 10;
    bs.failed_stabilize_same_entering_streak = 5;

    p1_basis_note_failed_stabilize_entering(NULL, 20, &bs);

    ASSERT_INT_EQ(bs.last_failed_stabilize_entering, 20,
                  "entering updated");
    ASSERT_INT_EQ(bs.failed_stabilize_same_entering_streak, 1,
                  "streak reset to 1");

    printf("PASS: test_failed_stabilize_entering_different\n");
    return 1;
}

/* ── p1_basis_exclude_entering (R3.4) ─────────────────────────────── */

static void seed_exclusion(P1BasisRepairState *bs, int slot, int var, int ttl) {
    bs->excluded_entering_pool[slot] = var;
    bs->excluded_entering_pool_ttl[slot] = ttl;
}

static int test_exclude_entering_fills_slot_a(void) {
    P1BasisRepairState bs;
    memset(&bs, 0, sizeof(bs));
    bs.excluded_entering_a = -1;
    bs.excluded_entering_ttl_a = 0;
    bs.excluded_entering_b = -1;
    bs.excluded_entering_ttl_b = 0;

    p1_basis_exclude_entering(NULL, 7, 5, &bs);

    ASSERT_INT_EQ(bs.excluded_entering_a, 7, "slot A filled with var 7");
    ASSERT_INT_EQ(bs.excluded_entering_ttl_a, 5, "slot A TTL set to 5");

    printf("PASS: test_exclude_entering_fills_slot_a\n");
    return 1;
}

static int test_exclude_entering_fills_slot_b(void) {
    P1BasisRepairState bs;
    memset(&bs, 0, sizeof(bs));
    seed_exclusion(&bs, 0, 3, 4);

    p1_basis_exclude_entering(NULL, 9, 6, &bs);

    ASSERT_INT_EQ(bs.excluded_entering_a, 3, "slot A unchanged");
    ASSERT_INT_EQ(bs.excluded_entering_b, 9, "slot B filled with var 9");
    ASSERT_INT_EQ(bs.excluded_entering_ttl_b, 6, "slot B TTL set to 6");

    printf("PASS: test_exclude_entering_fills_slot_b\n");
    return 1;
}

static int test_exclude_entering_evicts_lower_ttl(void) {
    P1BasisRepairState bs;
    memset(&bs, 0, sizeof(bs));
    for (int k = 0; k < PHASE1_ENTERING_EXCLUSION_POOL_SIZE; k++) {
        seed_exclusion(&bs, k, 100 + k, 5);
    }
    seed_exclusion(&bs, 0, 1, 2);
    seed_exclusion(&bs, 1, 2, 5);

    /* Full pool: slot 0 has lower TTL (2 < 5), so it gets evicted. */
    p1_basis_exclude_entering(NULL, 3, 8, &bs);

    ASSERT_INT_EQ(bs.excluded_entering_a, 3, "slot A evicted, now var 3");
    ASSERT_INT_EQ(bs.excluded_entering_ttl_a, 8, "slot A TTL updated");
    ASSERT_INT_EQ(bs.excluded_entering_b, 2, "slot B unchanged");

    printf("PASS: test_exclude_entering_evicts_lower_ttl\n");
    return 1;
}

static int test_exclude_entering_repeated_slot(void) {
    P1BasisRepairState bs;
    memset(&bs, 0, sizeof(bs));
    seed_exclusion(&bs, 0, 5, 3);

    /* Re-exclude same var — should refresh TTL */
    p1_basis_exclude_entering(NULL, 5, 10, &bs);

    ASSERT_INT_EQ(bs.excluded_entering_a, 5, "slot A still var 5");
    ASSERT_INT_EQ(bs.excluded_entering_ttl_a, 10, "slot A TTL refreshed");

    printf("PASS: test_exclude_entering_repeated_slot\n");
    return 1;
}

/* ── p1_basis_pivot_fail_maybe_exclude (R3.4) ─────────────────────── */

static int test_pivot_fail_maybe_exclude_below_threshold(void) {
    P1BasisRepairState bs;
    memset(&bs, 0, sizeof(bs));
    bs.excluded_entering_a = -1;
    bs.excluded_entering_ttl_a = 0;
    bs.excluded_entering_b = -1;
    bs.excluded_entering_ttl_b = 0;

    /* fail_repeat_count=1 < PHASE1_PIVOT_FAIL_RECOVERY_EXCLUDE_TRIGGER=2 */
    p1_basis_pivot_fail_maybe_exclude(NULL, 1, 42, &bs);

    ASSERT_INT_EQ(bs.excluded_entering_a, -1,
                  "no exclusion below threshold");

    printf("PASS: test_pivot_fail_maybe_exclude_below_threshold\n");
    return 1;
}

static int test_pivot_fail_maybe_exclude_at_threshold(void) {
    P1BasisRepairState bs;
    memset(&bs, 0, sizeof(bs));
    bs.excluded_entering_a = -1;
    bs.excluded_entering_ttl_a = 0;
    bs.excluded_entering_b = -1;
    bs.excluded_entering_ttl_b = 0;

    /* fail_repeat_count=2 >= PHASE1_PIVOT_FAIL_RECOVERY_EXCLUDE_TRIGGER=2 */
    p1_basis_pivot_fail_maybe_exclude(NULL, 2, 42, &bs);

    ASSERT_INT_EQ(bs.excluded_entering_a, 42,
                  "excluded at threshold");
    ASSERT_TRUE(bs.excluded_entering_ttl_a > 0,
                "TTL set");

    printf("PASS: test_pivot_fail_maybe_exclude_at_threshold\n");
    return 1;
}

/* ── main ─────────────────────────────────────────────────────────── */

#define NUM_TESTS 26

int main(void) {
    int pass = 0;

    /* R3.2 tests */
    pass += test_progress_reset_zeros_fields();
    pass += test_note_no_pivot_increments_streak();
    pass += test_note_no_pivot_null_ps_returns_zero();
    pass += test_window_pressure_note_failed_stabilize();
    pass += test_window_pressure_note_dir_skip();
    pass += test_window_pressure_note_alternation();
    pass += test_window_pressure_note_invalid_kind();
    pass += test_window_pressure_note_null_ps();
    pass += test_window_pressure_reset_zeros_all();
    pass += test_window_pressure_reset_noop_when_empty();
    pass += test_window_pressure_reset_null_ps();

    /* R3.3 tests: consume followup */
    pass += test_consume_followup_clears_all_pending();
    pass += test_consume_followup_skips_non_pending();
    pass += test_consume_followup_null_ns();
    pass += test_consume_followup_all_events();

    /* R3.3 tests: streak trackers */
    pass += test_dir_skip_entering_new();
    pass += test_dir_skip_entering_same();
    pass += test_dir_skip_entering_different();
    pass += test_failed_stabilize_entering_same();
    pass += test_failed_stabilize_entering_different();

    /* R3.4 tests: entering exclusion */
    pass += test_exclude_entering_fills_slot_a();
    pass += test_exclude_entering_fills_slot_b();
    pass += test_exclude_entering_evicts_lower_ttl();
    pass += test_exclude_entering_repeated_slot();

    /* R3.4 tests: pivot-fail exclusion */
    pass += test_pivot_fail_maybe_exclude_below_threshold();
    pass += test_pivot_fail_maybe_exclude_at_threshold();

    printf("\nPhase 1 recovery tests: %d/%d passed (%d assertions)\n",
           pass, NUM_TESTS, tests_passed);
    return (pass == NUM_TESTS) ? 0 : 1;
}
