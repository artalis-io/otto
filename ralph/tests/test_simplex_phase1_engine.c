/*
 * test_simplex_phase1_engine.c - Unit tests for Phase 1 feasibility primitives.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lp.h"
#include "simplex_phase1_engine.h"

static int tests_passed = 0;
static int tests_total = 0;

#define ASSERT_TRUE(cond, msg) do { \
    tests_total++; \
    if (!(cond)) { \
        printf("FAIL: %s\n", msg); \
        return 0; \
    } \
    tests_passed++; \
} while (0)

#define ASSERT_INT_EQ(a, b, msg) do { \
    tests_total++; \
    if ((a) != (b)) { \
        printf("FAIL: %s (expected %d, got %d)\n", msg, (b), (a)); \
        return 0; \
    } \
    tests_passed++; \
} while (0)

#define ASSERT_DBL_NEAR(a, b, tol, msg) do { \
    tests_total++; \
    if (fabs((a) - (b)) > (tol)) { \
        printf("FAIL: %s (expected %.17g, got %.17g)\n", msg, (double)(b), (double)(a)); \
        return 0; \
    } \
    tests_passed++; \
} while (0)

static void seed_tableau(SimplexTableau *tab,
                         int *basis,
                         int *basis_pos,
                         VarStatus *status,
                         int *artificials,
                         double *x,
                         double *lb,
                         double *ub,
                         double *work2) {
    memset(tab, 0, sizeof(*tab));
    tab->m = 2;
    tab->n = 4;
    tab->basis = basis;
    tab->basis_pos = basis_pos;
    tab->var_status = status;
    tab->artificial_vars = artificials;
    tab->num_artificial = 2;
    tab->x = x;
    tab->lb_ext = lb;
    tab->ub_ext = ub;
    tab->work2 = work2;

    for (int j = 0; j < 4; j++) {
        basis_pos[j] = -1;
        status[j] = RALPH_NONBASIC_LOWER;
        x[j] = 0.0;
        lb[j] = 0.0;
        ub[j] = RALPH_INFINITY;
    }
    work2[0] = 0.0;
    work2[1] = 0.0;
    artificials[0] = 2;
    artificials[1] = 3;
}

static int test_artificial_sum(void) {
    SimplexTableau tab;
    int basis[2];
    int basis_pos[4];
    VarStatus status[4];
    int artificials[2];
    double x[4], lb[4], ub[4], work2[2];

    seed_tableau(&tab, basis, basis_pos, status, artificials, x, lb, ub, work2);
    x[2] = 5.0;
    x[3] = -2.0;

    ASSERT_DBL_NEAR(p1_engine_artificial_sum(&tab), 7.0, 1e-15,
                    "artificial sum uses absolute artificial values");
    return 1;
}

static int test_predict_artificial_decrease_when_positive_artificial_leaves(void) {
    SimplexTableau tab;
    int basis[2] = {2, 1};
    int basis_pos[4];
    VarStatus status[4];
    int artificials[2];
    double x[4], lb[4], ub[4], work2[2];
    double current = 0.0;
    double predicted = 0.0;

    seed_tableau(&tab, basis, basis_pos, status, artificials, x, lb, ub, work2);
    status[2] = RALPH_BASIC;
    status[1] = RALPH_BASIC;
    basis_pos[2] = 0;
    basis_pos[1] = 1;
    x[2] = 5.0;
    x[3] = 2.0;
    work2[0] = 1.0;
    work2[1] = 0.0;

    ASSERT_TRUE(p1_engine_predict_artificial_sum(&tab, 0, 0, 5.0,
                                                 &current, &predicted),
                "prediction succeeds");
    ASSERT_DBL_NEAR(current, 7.0, 1e-15, "current artificial sum");
    ASSERT_DBL_NEAR(predicted, 2.0, 1e-15,
                    "leaving artificial is predicted at lower bound");
    ASSERT_TRUE(p1_engine_direction_preserves_artificial_progress(&tab, 0, 0, 5.0),
                "decreasing artificial direction preserves progress");
    return 1;
}

static int test_predict_rejects_artificial_increase(void) {
    SimplexTableau tab;
    int basis[2] = {2, 1};
    int basis_pos[4];
    VarStatus status[4];
    int artificials[2];
    double x[4], lb[4], ub[4], work2[2];
    double current = 0.0;
    double predicted = 0.0;

    seed_tableau(&tab, basis, basis_pos, status, artificials, x, lb, ub, work2);
    status[2] = RALPH_BASIC;
    status[1] = RALPH_BASIC;
    basis_pos[2] = 0;
    basis_pos[1] = 1;
    x[2] = 5.0;
    x[3] = 2.0;
    work2[0] = -1.0;
    work2[1] = 0.0;

    ASSERT_TRUE(p1_engine_predict_artificial_sum(&tab, 0, 1, 1.0,
                                                 &current, &predicted),
                "prediction succeeds for increasing artificial");
    ASSERT_DBL_NEAR(current, 7.0, 1e-15, "current artificial sum");
    ASSERT_DBL_NEAR(predicted, 8.0, 1e-15, "predicted artificial increase");
    ASSERT_TRUE(!p1_engine_direction_preserves_artificial_progress(&tab, 0, 1, 1.0),
                "increasing artificial direction is rejected");
    return 1;
}

static int test_score_prefers_larger_decrease(void) {
    P1FeasCandidate a = {
        .current_art_sum = 10.0,
        .predicted_art_sum = 4.0,
        .pivot_abs = 1.0,
        .theta = 2.0,
        .artificial_basic_after = 5
    };
    P1FeasCandidate b = a;
    b.predicted_art_sum = 6.0;
    b.pivot_abs = 10.0;

    P1FeasScore sa = p1_engine_score_candidate(a);
    P1FeasScore sb = p1_engine_score_candidate(b);

    ASSERT_INT_EQ(sa.valid, 1, "candidate a valid");
    ASSERT_INT_EQ(sb.valid, 1, "candidate b valid");
    ASSERT_TRUE(p1_engine_score_better(sa, sb),
                "larger artificial decrease dominates pivot size");
    return 1;
}

static int test_score_prefers_removing_positive_artificial_on_tie(void) {
    P1FeasCandidate a = {
        .current_art_sum = 10.0,
        .predicted_art_sum = 5.0,
        .pivot_abs = 1.0,
        .theta = 1.0,
        .leaving_is_artificial = 1,
        .leaving_positive_artificial = 1,
        .artificial_basic_after = 4
    };
    P1FeasCandidate b = a;
    b.leaving_is_artificial = 0;
    b.pivot_abs = 10.0;

    ASSERT_TRUE(p1_engine_score_better(p1_engine_score_candidate(a),
                                       p1_engine_score_candidate(b)),
                "removing positive artificial wins tied decrease");
    return 1;
}

static int test_score_rejects_artificial_increase(void) {
    P1FeasCandidate candidate = {
        .current_art_sum = 5.0,
        .predicted_art_sum = 6.0,
        .pivot_abs = 1.0,
        .theta = 1.0
    };

    ASSERT_INT_EQ(p1_engine_score_candidate(candidate).valid, 0,
                  "candidate increasing artificial sum invalid");
    return 1;
}

static int test_progress_window_initializes_on_first_update(void) {
    P1ProgressWindow window;

    p1_progress_window_init(&window);

    ASSERT_INT_EQ(p1_progress_window_update(&window, 10.0, 3, 0.10, 0.1, 1, 2, 5),
                  0, "first update is not stale");
    ASSERT_DBL_NEAR(window.anchor_art_sum, 10.0, 1e-15, "anchor initialized");
    ASSERT_DBL_NEAR(window.previous_art_sum, 10.0, 1e-15, "previous initialized");
    ASSERT_INT_EQ(window.window_iters, 0, "window iters reset on init");
    ASSERT_INT_EQ(window.cleanup_due, 0, "cleanup not due on init");
    ASSERT_INT_EQ(window.perturb_due, 0, "perturb not due on init");
    return 1;
}

static int test_progress_window_resets_on_sufficient_drop(void) {
    P1ProgressWindow window;

    p1_progress_window_init(&window);
    p1_progress_window_update(&window, 10.0, 3, 0.10, 0.1, 1, 2, 5);
    p1_progress_window_update(&window, 9.7, 3, 0.10, 0.1, 1, 2, 5);

    ASSERT_INT_EQ(p1_progress_window_update(&window, 8.8, 3, 0.10, 0.1, 1, 2, 5),
                  0, "sufficient artificial drop is progress");
    ASSERT_DBL_NEAR(window.anchor_art_sum, 8.8, 1e-15, "anchor moves to progress point");
    ASSERT_INT_EQ(window.window_iters, 0, "window resets after progress");
    ASSERT_INT_EQ(window.stale_windows, 0, "stale count resets after progress");
    return 1;
}

static int test_progress_window_triggers_cleanup_before_perturb(void) {
    P1ProgressWindow window;

    p1_progress_window_init(&window);
    p1_progress_window_update(&window, 10.0, 2, 0.10, 0.1, 1, 2, 5);
    ASSERT_INT_EQ(p1_progress_window_update(&window, 9.99, 2, 0.10, 0.1, 1, 2, 5),
                  0, "first stale iteration not a complete window");
    ASSERT_INT_EQ(p1_progress_window_update(&window, 9.98, 2, 0.10, 0.1, 1, 2, 5),
                  1, "stale window detected");
    ASSERT_INT_EQ(window.refactor_due, 1, "refactor due on stale window");
    ASSERT_INT_EQ(window.cleanup_due, 1, "cleanup due on stale window");
    ASSERT_INT_EQ(window.perturb_due, 0, "perturb is not first response");
    return 1;
}

static int test_progress_window_perturb_uses_cooldown(void) {
    P1ProgressWindow window;

    p1_progress_window_init(&window);
    p1_progress_window_update(&window, 10.0, 1, 0.10, 0.1, 1, 2, 3);
    p1_progress_window_update(&window, 9.99, 1, 0.10, 0.1, 1, 2, 3);
    ASSERT_INT_EQ(p1_progress_window_update(&window, 9.98, 1, 0.10, 0.1, 1, 2, 3),
                  1, "second stale window detected");
    ASSERT_INT_EQ(window.perturb_due, 1, "perturb due after configured stale windows");
    ASSERT_INT_EQ(window.perturb_cooldown, 3, "perturb cooldown armed");

    ASSERT_INT_EQ(p1_progress_window_update(&window, 9.97, 1, 0.10, 0.1, 1, 2, 3),
                  1, "next stale window detected");
    ASSERT_INT_EQ(window.cleanup_due, 1, "cleanup remains available during cooldown");
    ASSERT_INT_EQ(window.perturb_due, 0, "cooldown suppresses repeated perturb");
    return 1;
}

static int test_candidate_basis_rejects_missing_lu(void) {
    SimplexTableau tab;
    int basis[2] = {2, 1};
    int basis_pos[4];
    VarStatus status[4];
    int artificials[2];
    double x[4], lb[4], ub[4], work2[2];

    seed_tableau(&tab, basis, basis_pos, status, artificials, x, lb, ub, work2);
    status[2] = RALPH_BASIC;
    status[1] = RALPH_BASIC;
    basis_pos[2] = 0;
    basis_pos[1] = 1;
    x[2] = 5.0;
    work2[0] = 1.0;

    ASSERT_INT_EQ(p1_candidate_basis_refactorable(&tab, 0, 0, 1.0, 1e-8),
                  0, "basis candidate without LU is rejected");
    return 1;
}

static int test_candidate_basis_bound_flip_uses_artificial_prediction(void) {
    SimplexTableau tab;
    int basis[2] = {2, 1};
    int basis_pos[4];
    VarStatus status[4];
    int artificials[2];
    double x[4], lb[4], ub[4], work2[2];

    seed_tableau(&tab, basis, basis_pos, status, artificials, x, lb, ub, work2);
    status[2] = RALPH_BASIC;
    status[1] = RALPH_BASIC;
    basis_pos[2] = 0;
    basis_pos[1] = 1;
    x[2] = 5.0;
    x[3] = 2.0;
    work2[0] = -1.0;
    work2[1] = 0.0;

    ASSERT_INT_EQ(p1_candidate_basis_refactorable(&tab, 0, -2, 1.0, 1e-8),
                  0, "artificial-increasing bound flip is rejected");
    work2[0] = 0.0;
    ASSERT_INT_EQ(p1_candidate_basis_refactorable(&tab, 0, -2, 1.0, 1e-8),
                  1, "artificial-preserving bound flip is accepted");
    return 1;
}

typedef int (*TestFunc)(void);

static struct { const char *name; TestFunc func; } all_tests[] = {
    {"artificial_sum", test_artificial_sum},
    {"predict_artificial_decrease_when_positive_artificial_leaves",
     test_predict_artificial_decrease_when_positive_artificial_leaves},
    {"predict_rejects_artificial_increase", test_predict_rejects_artificial_increase},
    {"score_prefers_larger_decrease", test_score_prefers_larger_decrease},
    {"score_prefers_removing_positive_artificial_on_tie",
     test_score_prefers_removing_positive_artificial_on_tie},
    {"score_rejects_artificial_increase", test_score_rejects_artificial_increase},
    {"progress_window_initializes_on_first_update",
     test_progress_window_initializes_on_first_update},
    {"progress_window_resets_on_sufficient_drop",
     test_progress_window_resets_on_sufficient_drop},
    {"progress_window_triggers_cleanup_before_perturb",
     test_progress_window_triggers_cleanup_before_perturb},
    {"progress_window_perturb_uses_cooldown",
     test_progress_window_perturb_uses_cooldown},
    {"candidate_basis_rejects_missing_lu",
     test_candidate_basis_rejects_missing_lu},
    {"candidate_basis_bound_flip_uses_artificial_prediction",
     test_candidate_basis_bound_flip_uses_artificial_prediction},
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

    printf("\nPhase 1 engine tests: %d/%d passed (%d assertions)\n",
           passed, num_tests, tests_total);
    return failed == 0 ? 0 : 1;
}
