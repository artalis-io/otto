#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>

#include "../src/sg_time_budget.h"

static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(fn) do { \
    tests_run++; \
    printf("  %-55s", #fn); \
    fn(); \
    tests_passed++; \
    printf("OK\n"); \
} while (0)

#define ASSERT_NEAR(a, b, eps) \
    assert(fabs((a) - (b)) < (eps))

/* ---- Tests ---- */

static void test_basic_init(void) {
    SGTimeBudget tb;
    sg_time_budget_init(&tb, 0.0, 60.0);
    /* deadline should be 60.0 */
    assert(!sg_time_budget_expired(&tb, 0.0));
    ASSERT_NEAR(sg_time_budget_remaining(&tb, 0.0), 60.0, 1e-9);
}

static void test_not_expired_before_deadline(void) {
    SGTimeBudget tb;
    sg_time_budget_init(&tb, 0.0, 60.0);
    assert(!sg_time_budget_expired(&tb, 30.0));
}

static void test_expired_at_deadline(void) {
    SGTimeBudget tb;
    sg_time_budget_init(&tb, 0.0, 60.0);
    assert(sg_time_budget_expired(&tb, 60.0));
}

static void test_expired_after_deadline(void) {
    SGTimeBudget tb;
    sg_time_budget_init(&tb, 0.0, 60.0);
    assert(sg_time_budget_expired(&tb, 90.0));
}

static void test_remaining_before_deadline(void) {
    SGTimeBudget tb;
    sg_time_budget_init(&tb, 0.0, 60.0);
    ASSERT_NEAR(sg_time_budget_remaining(&tb, 30.0), 30.0, 1e-9);
}

static void test_remaining_after_deadline(void) {
    SGTimeBudget tb;
    sg_time_budget_init(&tb, 0.0, 60.0);
    ASSERT_NEAR(sg_time_budget_remaining(&tb, 90.0), 0.0, 1e-9);
}

static void test_unlimited_zero(void) {
    SGTimeBudget tb;
    sg_time_budget_init(&tb, 0.0, 0.0);
    assert(!sg_time_budget_expired(&tb, 1e9));
    assert(sg_time_budget_remaining(&tb, 1e9) == DBL_MAX);
}

static void test_unlimited_negative(void) {
    SGTimeBudget tb;
    sg_time_budget_init(&tb, 0.0, -1.0);
    assert(!sg_time_budget_expired(&tb, 1e9));
    assert(sg_time_budget_remaining(&tb, 1e9) == DBL_MAX);
}

static void test_phase_allocation(void) {
    SGTimeBudget tb;
    sg_time_budget_init(&tb, 0.0, 60.0);
    /* 55% of 60s = 33.0 */
    ASSERT_NEAR(sg_time_budget_phase(&tb, 0.0, 0.55, 5.0), 33.0, 1e-9);
}

static void test_phase_respects_remaining(void) {
    SGTimeBudget tb;
    sg_time_budget_init(&tb, 0.0, 60.0);
    /* At now=50, remaining=10.  55% of 10 = 5.5, min=5.0 → 5.5 */
    ASSERT_NEAR(sg_time_budget_phase(&tb, 50.0, 0.55, 5.0), 5.5, 1e-9);
}

static void test_phase_min_capped_at_remaining(void) {
    SGTimeBudget tb;
    sg_time_budget_init(&tb, 0.0, 60.0);
    /* At now=58, remaining=2.  55% of 2 = 1.1, min=5.0 → 5.0 but capped at 2.0 */
    ASSERT_NEAR(sg_time_budget_phase(&tb, 58.0, 0.55, 5.0), 2.0, 1e-9);
}

static void test_phase_when_expired(void) {
    SGTimeBudget tb;
    sg_time_budget_init(&tb, 0.0, 60.0);
    ASSERT_NEAR(sg_time_budget_phase(&tb, 70.0, 0.55, 5.0), 0.0, 1e-9);
}

static void test_phase_unlimited(void) {
    SGTimeBudget tb;
    sg_time_budget_init(&tb, 0.0, 0.0);
    assert(sg_time_budget_phase(&tb, 50.0, 0.55, 5.0) == DBL_MAX);
}

static void test_remaining_int(void) {
    SGTimeBudget tb;
    sg_time_budget_init(&tb, 0.0, 60.0);
    /* remaining = 30.0 → ceil(30.0) = 30 */
    assert(sg_time_budget_remaining_int(&tb, 30.0) == 30);
}

static void test_remaining_int_fractional(void) {
    SGTimeBudget tb;
    sg_time_budget_init(&tb, 0.0, 60.0);
    /* remaining = 10.3 → ceil(10.3) = 11 */
    assert(sg_time_budget_remaining_int(&tb, 49.7) == 11);
}

static void test_remaining_int_expired(void) {
    SGTimeBudget tb;
    sg_time_budget_init(&tb, 0.0, 60.0);
    assert(sg_time_budget_remaining_int(&tb, 90.0) == 0);
}

static void test_remaining_int_unlimited(void) {
    SGTimeBudget tb;
    sg_time_budget_init(&tb, 0.0, 0.0);
    /* unlimited: return 0 (callers interpret as "no time limit") */
    assert(sg_time_budget_remaining_int(&tb, 50.0) == 0);
}

static void test_elapsed(void) {
    SGTimeBudget tb;
    sg_time_budget_init(&tb, 10.0, 60.0);
    ASSERT_NEAR(sg_time_budget_elapsed(&tb, 45.0), 35.0, 1e-9);
}

static void test_nonzero_start(void) {
    SGTimeBudget tb;
    sg_time_budget_init(&tb, 100.0, 60.0);
    assert(!sg_time_budget_expired(&tb, 130.0));
    assert(sg_time_budget_expired(&tb, 160.0));
    ASSERT_NEAR(sg_time_budget_remaining(&tb, 130.0), 30.0, 1e-9);
}

/* ---- SGBudgetProbe tests ---- */

static void test_probe_init_defaults(void) {
    SGTimeBudget tb;
    SGBudgetProbe p;
    sg_time_budget_init(&tb, 0.0, 60.0);
    sg_budget_probe_init(&p, &tb, 4);
    assert(p.counter == 0);
    assert(p.expired == 0);
    assert(p.interval == 4);
    assert(p.budget == &tb);
}

static void test_probe_no_check_before_interval(void) {
    SGTimeBudget tb;
    SGBudgetProbe p;
    sg_time_budget_init(&tb, 0.0, 60.0);
    sg_budget_probe_init(&p, &tb, 4);
    /* First 3 calls: counter increments but no real check.
       Even with now=999 (past deadline), probe returns cached 0. */
    assert(!sg_budget_probe_expired_at(&p, 999.0)); /* counter=1 */
    assert(!sg_budget_probe_expired_at(&p, 999.0)); /* counter=2 */
    assert(!sg_budget_probe_expired_at(&p, 999.0)); /* counter=3 */
    assert(p.counter == 3);
    assert(p.expired == 0);
}

static void test_probe_checks_at_interval(void) {
    SGTimeBudget tb;
    SGBudgetProbe p;
    sg_time_budget_init(&tb, 0.0, 60.0);
    sg_budget_probe_init(&p, &tb, 4);
    /* Ticks 1-3: no check */
    sg_budget_probe_expired_at(&p, 999.0);
    sg_budget_probe_expired_at(&p, 999.0);
    sg_budget_probe_expired_at(&p, 999.0);
    /* Tick 4: real check with now=999.0 (past deadline=60.0) → expired */
    assert(sg_budget_probe_expired_at(&p, 999.0));
    assert(p.expired == 1);
    assert(p.counter == 0);
}

static void test_probe_expired_sticky(void) {
    SGTimeBudget tb;
    SGBudgetProbe p;
    uint32_t saved_counter;
    sg_time_budget_init(&tb, 0.0, 60.0);
    sg_budget_probe_init(&p, &tb, 2);
    /* Tick 1: no check */
    sg_budget_probe_expired_at(&p, 999.0);
    /* Tick 2: real check → expired */
    assert(sg_budget_probe_expired_at(&p, 999.0));
    saved_counter = p.counter;
    /* Subsequent calls: return 1 immediately, counter unchanged */
    assert(sg_budget_probe_expired_at(&p, 0.0));
    assert(p.counter == saved_counter);
    assert(sg_budget_probe_expired_at(&p, 0.0));
    assert(p.counter == saved_counter);
}

static void test_probe_resets_counter(void) {
    SGTimeBudget tb;
    SGBudgetProbe p;
    sg_time_budget_init(&tb, 0.0, 60.0);
    sg_budget_probe_init(&p, &tb, 3);
    /* Ticks 1-2 */
    sg_budget_probe_expired_at(&p, 10.0);
    sg_budget_probe_expired_at(&p, 20.0);
    assert(p.counter == 2);
    /* Tick 3: real check, not expired (now=30 < deadline=60), counter resets */
    assert(!sg_budget_probe_expired_at(&p, 30.0));
    assert(p.counter == 0);
    /* Next interval: ticks 1-2 */
    sg_budget_probe_expired_at(&p, 40.0);
    sg_budget_probe_expired_at(&p, 50.0);
    assert(p.counter == 2);
}

static void test_probe_unlimited_never_expires(void) {
    SGTimeBudget tb;
    SGBudgetProbe p;
    int i;
    sg_time_budget_init(&tb, 0.0, 0.0); /* unlimited */
    sg_budget_probe_init(&p, &tb, 1);   /* check every call */
    for (i = 0; i < 100; i++) {
        assert(!sg_budget_probe_expired_at(&p, (double)i * 1e6));
    }
}

static void test_probe_interval_zero_treated_as_one(void) {
    SGTimeBudget tb;
    SGBudgetProbe p;
    sg_time_budget_init(&tb, 0.0, 60.0);
    sg_budget_probe_init(&p, &tb, 0); /* 0 → treated as 1 */
    assert(p.interval == 1);
    /* Every call is a real check: now=30 < deadline=60 → not expired */
    assert(!sg_budget_probe_expired_at(&p, 30.0));
    assert(p.counter == 0);
    /* now=70 > deadline=60 → expired */
    assert(sg_budget_probe_expired_at(&p, 70.0));
}

static void test_probe_not_expired_until_deadline(void) {
    SGTimeBudget tb;
    SGBudgetProbe p;
    sg_time_budget_init(&tb, 0.0, 60.0);
    sg_budget_probe_init(&p, &tb, 2);
    /* Interval 1: ticks 1-2 at now=10 (not expired) */
    assert(!sg_budget_probe_expired_at(&p, 10.0));
    assert(!sg_budget_probe_expired_at(&p, 10.0));
    /* Interval 2: ticks 1-2 at now=50 (not expired) */
    assert(!sg_budget_probe_expired_at(&p, 50.0));
    assert(!sg_budget_probe_expired_at(&p, 50.0));
    /* Interval 3: ticks 1-2 at now=65 (expired at check) */
    assert(!sg_budget_probe_expired_at(&p, 65.0)); /* tick 1: no check */
    assert(sg_budget_probe_expired_at(&p, 65.0));  /* tick 2: real check → expired */
}

int main(void) {
    printf("sg_time_budget tests:\n");

    RUN_TEST(test_basic_init);
    RUN_TEST(test_not_expired_before_deadline);
    RUN_TEST(test_expired_at_deadline);
    RUN_TEST(test_expired_after_deadline);
    RUN_TEST(test_remaining_before_deadline);
    RUN_TEST(test_remaining_after_deadline);
    RUN_TEST(test_unlimited_zero);
    RUN_TEST(test_unlimited_negative);
    RUN_TEST(test_phase_allocation);
    RUN_TEST(test_phase_respects_remaining);
    RUN_TEST(test_phase_min_capped_at_remaining);
    RUN_TEST(test_phase_when_expired);
    RUN_TEST(test_phase_unlimited);
    RUN_TEST(test_remaining_int);
    RUN_TEST(test_remaining_int_fractional);
    RUN_TEST(test_remaining_int_expired);
    RUN_TEST(test_remaining_int_unlimited);
    RUN_TEST(test_elapsed);
    RUN_TEST(test_nonzero_start);

    printf("\nsg_budget_probe tests:\n");
    RUN_TEST(test_probe_init_defaults);
    RUN_TEST(test_probe_no_check_before_interval);
    RUN_TEST(test_probe_checks_at_interval);
    RUN_TEST(test_probe_expired_sticky);
    RUN_TEST(test_probe_resets_counter);
    RUN_TEST(test_probe_unlimited_never_expires);
    RUN_TEST(test_probe_interval_zero_treated_as_one);
    RUN_TEST(test_probe_not_expired_until_deadline);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_run == tests_passed ? 0 : 1;
}
