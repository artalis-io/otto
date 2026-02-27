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

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_run == tests_passed ? 0 : 1;
}
