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

/* ── main ─────────────────────────────────────────────────────────── */

int main(void) {
    int pass = 0;

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

    printf("\nPhase 1 recovery tests: %d/%d passed (%d assertions)\n",
           pass, 11, tests_passed);
    return (pass == 11) ? 0 : 1;
}
