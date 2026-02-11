/*
 * Unit tests for LP presolve improvements:
 * - Multi-round fixed-point loop
 * - Singleton row bound tightening
 * - Doubleton equality elimination + postsolve
 * - Implied free variable detection
 * - Postsolve stack infrastructure
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ralph.h"
#include "presolve.h"
#include "lp.h"

#define TOLERANCE 1e-4

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
        printf("  PASS: %s\n", msg); \
    } else { \
        printf("  FAIL: %s\n", msg); \
    } \
} while(0)

#define ASSERT_NEAR(a, b, tol, msg) do { \
    tests_run++; \
    if (fabs((a) - (b)) < (tol)) { \
        tests_passed++; \
        printf("  PASS: %s (%.6f ≈ %.6f)\n", msg, (double)(a), (double)(b)); \
    } else { \
        printf("  FAIL: %s (%.6f != %.6f, diff=%.2e)\n", msg, (double)(a), (double)(b), fabs((a)-(b))); \
    } \
} while(0)

/* ============================================================================
 * Helper: Build an LPModel from arrays
 * ============================================================================ */

static LPModel* build_model(int nvars, int ncons,
                             const double *obj, int obj_sense,
                             const double *lb, const double *ub,
                             const char *var_types,
                             /* constraint data: flattened CSR */
                             const int *row_starts, const int *col_idx,
                             const double *values,
                             const char *sense, const double *rhs) {
    LPModel *m = lp_model_create();
    if (!m) return NULL;

    m->obj_sense = obj_sense;

    for (int j = 0; j < nvars; j++) {
        lp_model_add_var(m, lb[j], ub[j], obj[j],
                         var_types ? var_types[j] : 'C');
    }

    for (int i = 0; i < ncons; i++) {
        int nnz = row_starts[i + 1] - row_starts[i];
        lp_model_add_constraint(m, nnz,
                                &col_idx[row_starts[i]],
                                &values[row_starts[i]],
                                sense[i], rhs[i]);
    }

    lp_model_finalize(m);
    return m;
}

/* ============================================================================
 * Test 1: Multi-round presolve convergence
 *
 * Create a chain of singleton rows that require multiple rounds:
 *   x0 <= 10         (singleton row, tightens x0)
 *   x0 + x1 <= 15    (after round 1 tightens x0, x1 becomes singleton-like)
 *   x0 + x1 + x2 = 20
 *
 * With multi-round, bound tightening cascades through the chain.
 * ============================================================================ */
static void test_multi_round(void) {
    printf("\n=== Test: Multi-round presolve convergence ===\n");

    /* min x0 + x1 + x2
     * s.t. x0 <= 5          (singleton)
     *      x1 <= 8          (singleton)
     *      x0 + x1 + x2 <= 20
     *      x0, x1, x2 >= 0 */
    double obj[] = {1.0, 1.0, 1.0};
    double lb[] = {0.0, 0.0, 0.0};
    double ub[] = {1e30, 1e30, 1e30};
    int row_starts[] = {0, 1, 2, 5};
    int col_idx[] = {0, 1, 0, 1, 2};
    double vals[] = {1.0, 1.0, 1.0, 1.0, 1.0};
    char sense[] = {'L', 'L', 'L'};
    double rhs[] = {5.0, 8.0, 20.0};

    LPModel *m = build_model(3, 3, obj, 1, lb, ub, NULL,
                             row_starts, col_idx, vals, sense, rhs);
    ASSERT(m != NULL, "Model created");

    PresolveResult *res = presolve(m);
    ASSERT(res != NULL, "Presolve succeeded");

    /* Singleton rows should be deleted (at least 2 rows removed) */
    ASSERT(res->cons_removed >= 2, "At least 2 singleton rows removed");

    /* After tightening, ub[0] = 5, ub[1] = 8 */
    /* The third constraint should be redundant since 5+8+inf > 20 is still active,
     * but bounds were tightened */
    printf("  INFO: vars_removed=%d, cons_removed=%d, bounds_tightened=%d\n",
           res->vars_removed, res->cons_removed, res->bounds_tightened);

    presolve_free(res);
    lp_model_free(m);
}

/* ============================================================================
 * Test 2: Singleton row bound tightening (inequality)
 *
 *   3*x <= 12  =>  x <= 4  (tighten upper bound)
 *   -2*y <= -6  =>  y >= 3  (tighten lower bound)
 *   x, y >= 0, x <= 100, y <= 100
 *
 * Both rows should be deleted and bounds tightened.
 * ============================================================================ */
static void test_singleton_row_inequality(void) {
    printf("\n=== Test: Singleton row bound tightening (inequality) ===\n");

    /* min x + y
     * s.t. 3*x <= 12
     *      -2*y <= -6
     *      x + y <= 10 (keep this so model isn't trivial)
     *      0 <= x <= 100, 0 <= y <= 100 */
    double obj[] = {1.0, 1.0};
    double lb[] = {0.0, 0.0};
    double ub[] = {100.0, 100.0};
    int row_starts[] = {0, 1, 2, 4};
    int col_idx[] = {0, 1, 0, 1};
    double vals[] = {3.0, -2.0, 1.0, 1.0};
    char sense[] = {'L', 'L', 'L'};
    double rhs[] = {12.0, -6.0, 10.0};

    LPModel *m = build_model(2, 3, obj, 1, lb, ub, NULL,
                             row_starts, col_idx, vals, sense, rhs);
    ASSERT(m != NULL, "Model created");

    PresolveResult *res = presolve(m);
    ASSERT(res != NULL, "Presolve succeeded");

    /* Should remove at least the 2 singleton rows */
    ASSERT(res->cons_removed >= 2, "Singleton rows removed");

    /* Solve the presolved model to verify correctness */
    presolve_free(res);
    lp_model_free(m);

    /* Solve via ralph API to verify end-to-end */
    RalphModel *rm = ralph_create();
    ralph_set_obj_sense(rm, RALPH_MINIMIZE);
    ralph_add_var(rm, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);
    ralph_add_var(rm, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);

    int idx0[] = {0}; double v0[] = {3.0};
    ralph_add_constraint(rm, 1, idx0, v0, RALPH_LESS_EQUAL, 12.0);
    int idx1[] = {1}; double v1[] = {-2.0};
    ralph_add_constraint(rm, 1, idx1, v1, RALPH_LESS_EQUAL, -6.0);
    int idx2[] = {0, 1}; double v2[] = {1.0, 1.0};
    ralph_add_constraint(rm, 2, idx2, v2, RALPH_LESS_EQUAL, 10.0);

    ralph_set_int_param(rm, "presolve", 1);
    ralph_optimize(rm);

    RalphStatus status = ralph_get_status(rm);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Optimal with presolve");

    /* min x+y: x>=0, y>=3, x<=4, x+y<=10 => x=0, y=3, obj=3 */
    double obj_val = ralph_get_objval(rm);
    ASSERT_NEAR(obj_val, 3.0, TOLERANCE, "Objective = 3.0 (x=0, y=3)");

    ralph_free(rm);
}

/* ============================================================================
 * Test 3: Doubleton equality elimination
 *
 *   x + y = 10     (doubleton equality: eliminate one var)
 *   x + 2z <= 15
 *   y + z <= 12
 *   min x + y + z, all >= 0
 *
 * After eliminating y = 10 - x from the equality:
 *   x + 2z <= 15 (unchanged)
 *   (10 - x) + z <= 12  =>  -x + z <= 2
 *   obj = x + (10 - x) + z = 10 + z
 *   min z, z >= 0 => z=0, then x + y = 10
 * ============================================================================ */
static void test_doubleton_equality(void) {
    printf("\n=== Test: Doubleton equality elimination ===\n");

    RalphModel *rm = ralph_create();
    ralph_set_obj_sense(rm, RALPH_MINIMIZE);
    ralph_add_var(rm, 0.0, 1e30, 1.0, RALPH_CONTINUOUS);  /* x */
    ralph_add_var(rm, 0.0, 1e30, 1.0, RALPH_CONTINUOUS);  /* y */
    ralph_add_var(rm, 0.0, 1e30, 1.0, RALPH_CONTINUOUS);  /* z */

    /* x + y = 10 */
    int idx0[] = {0, 1}; double v0[] = {1.0, 1.0};
    ralph_add_constraint(rm, 2, idx0, v0, RALPH_EQUAL, 10.0);

    /* x + 2z <= 15 */
    int idx1[] = {0, 2}; double v1[] = {1.0, 2.0};
    ralph_add_constraint(rm, 2, idx1, v1, RALPH_LESS_EQUAL, 15.0);

    /* y + z <= 12 */
    int idx2[] = {1, 2}; double v2[] = {1.0, 1.0};
    ralph_add_constraint(rm, 2, idx2, v2, RALPH_LESS_EQUAL, 12.0);

    ralph_set_int_param(rm, "presolve", 1);
    ralph_optimize(rm);

    RalphStatus status = ralph_get_status(rm);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Optimal with doubleton presolve");

    double obj_val = ralph_get_objval(rm);
    /* obj = x + y + z = 10 + z, min z=0 => obj=10 */
    ASSERT_NEAR(obj_val, 10.0, TOLERANCE, "Objective = 10.0");

    /* Verify solution: x + y = 10, z = 0 */
    double sol[3];
    ralph_get_solution(rm, sol);
    ASSERT_NEAR(sol[0] + sol[1], 10.0, TOLERANCE, "x + y = 10 (equality satisfied)");
    ASSERT_NEAR(sol[2], 0.0, TOLERANCE, "z = 0");

    ralph_free(rm);
}

/* ============================================================================
 * Test 4: Doubleton equality postsolve correctness
 *
 * Verify that postsolve correctly recovers the eliminated variable.
 *   2*x + 3*y = 12, x >= 0, y >= 0
 *   x + z <= 10
 *   min -x - 2*y - z, z >= 0
 *
 * Eliminate y = (12 - 2x)/3 = 4 - 2x/3
 * obj = -x - 2*(4 - 2x/3) - z = -x - 8 + 4x/3 - z = x/3 - z - 8
 * min x/3 - z s.t. x + z <= 10, x >= 0, z >= 0
 * => x=0, z=10, y=4, obj = 0 - 10 - 8 = -18
 * ============================================================================ */
static void test_doubleton_postsolve(void) {
    printf("\n=== Test: Doubleton equality postsolve ===\n");

    RalphModel *rm = ralph_create();
    ralph_set_obj_sense(rm, RALPH_MINIMIZE);
    ralph_add_var(rm, 0.0, 1e30, -1.0, RALPH_CONTINUOUS);  /* x */
    ralph_add_var(rm, 0.0, 1e30, -2.0, RALPH_CONTINUOUS);  /* y */
    ralph_add_var(rm, 0.0, 1e30, -1.0, RALPH_CONTINUOUS);  /* z */

    /* 2x + 3y = 12 */
    int idx0[] = {0, 1}; double v0[] = {2.0, 3.0};
    ralph_add_constraint(rm, 2, idx0, v0, RALPH_EQUAL, 12.0);

    /* x + z <= 10 */
    int idx1[] = {0, 2}; double v1[] = {1.0, 1.0};
    ralph_add_constraint(rm, 2, idx1, v1, RALPH_LESS_EQUAL, 10.0);

    ralph_set_int_param(rm, "presolve", 1);
    ralph_optimize(rm);

    RalphStatus status = ralph_get_status(rm);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Optimal");

    double obj_val = ralph_get_objval(rm);
    ASSERT_NEAR(obj_val, -18.0, TOLERANCE, "Objective = -18.0");

    double sol[3];
    ralph_get_solution(rm, sol);
    printf("  INFO: x=%.4f, y=%.4f, z=%.4f\n", sol[0], sol[1], sol[2]);

    /* Verify equality constraint holds in recovered solution */
    double lhs = 2.0 * sol[0] + 3.0 * sol[1];
    ASSERT_NEAR(lhs, 12.0, TOLERANCE, "2x + 3y = 12 (postsolve correct)");
    ASSERT_NEAR(sol[2], 10.0, 0.1, "z ≈ 10 (at bound)");

    ralph_free(rm);
}

/* ============================================================================
 * Test 5: Implied free variable detection
 *
 *   x + y >= 5     (forces x >= 5 - y)
 *   x + y <= 15    (forces x <= 15 - y)
 *   x >= 0, x <= 20, y >= 0, y <= 10
 *
 * For x: from constraint 1 (y at max=10): x >= 5-10 = -5
 *         from constraint 2 (y at min=0):  x <= 15-0 = 15
 * So x is implied within [-5, 15], which contains [0, 20]?
 * Actually: lb=0 is tighter than -5, and ub=20 is wider than 15.
 * So x's ub=20 is NOT implied (15 < 20). x is NOT implied free.
 *
 * Better test: make bounds clearly implied.
 *   x + y = 10, 0 <= y <= 10 => x in [0, 10]
 *   If x has bounds [0, 100], the ub=100 is implied redundant? No, implied ub=10 < 100.
 *   Actually implied bounds from the equality: x = 10 - y, y in [0,10] => x in [0, 10].
 *   So if x has bounds [-5, 100], the lb=-5 is implied by 0 and ub=100 by 10.
 *   Both are wider than the implied range, so x IS implied free? No:
 *   Implied free means the constraints imply bounds WITHIN [lb, ub], making the
 *   explicit bounds redundant. lb=-5 is weaker than implied lb=0, and ub=100 is
 *   weaker than implied ub=10. So yes, bounds are redundant.
 *
 * Use end-to-end test: solve with and without presolve, verify same answer.
 * ============================================================================ */
static void test_implied_free(void) {
    printf("\n=== Test: Implied free variable detection ===\n");

    /* min -x - y
     * s.t. x + y = 10
     *      x + z <= 15
     *      -5 <= x <= 100, 0 <= y <= 10, 0 <= z <= 100
     *
     * x is implied free: equality + y bounds => x in [0, 10], wider than [-5, 100]
     * After implied free: x has no bounds, enabling more doubleton elimination.
     */
    RalphModel *rm_pre = ralph_create();
    ralph_set_obj_sense(rm_pre, RALPH_MINIMIZE);
    ralph_add_var(rm_pre, -5.0, 100.0, -1.0, RALPH_CONTINUOUS);  /* x */
    ralph_add_var(rm_pre,  0.0,  10.0, -1.0, RALPH_CONTINUOUS);  /* y */
    ralph_add_var(rm_pre,  0.0, 100.0, -1.0, RALPH_CONTINUOUS);  /* z */

    int idx0[] = {0, 1}; double v0[] = {1.0, 1.0};
    ralph_add_constraint(rm_pre, 2, idx0, v0, RALPH_EQUAL, 10.0);
    int idx1[] = {0, 2}; double v1[] = {1.0, 1.0};
    ralph_add_constraint(rm_pre, 2, idx1, v1, RALPH_LESS_EQUAL, 15.0);

    ralph_set_int_param(rm_pre, "presolve", 1);
    ralph_optimize(rm_pre);
    RalphStatus status_pre = ralph_get_status(rm_pre);
    double obj_pre = ralph_get_objval(rm_pre);

    /* Solve without presolve for reference */
    RalphModel *rm_nopre = ralph_create();
    ralph_set_obj_sense(rm_nopre, RALPH_MINIMIZE);
    ralph_add_var(rm_nopre, -5.0, 100.0, -1.0, RALPH_CONTINUOUS);
    ralph_add_var(rm_nopre,  0.0,  10.0, -1.0, RALPH_CONTINUOUS);
    ralph_add_var(rm_nopre,  0.0, 100.0, -1.0, RALPH_CONTINUOUS);

    ralph_add_constraint(rm_nopre, 2, idx0, v0, RALPH_EQUAL, 10.0);
    ralph_add_constraint(rm_nopre, 2, idx1, v1, RALPH_LESS_EQUAL, 15.0);

    ralph_set_int_param(rm_nopre, "presolve", 0);
    ralph_optimize(rm_nopre);
    RalphStatus status_nopre = ralph_get_status(rm_nopre);
    double obj_nopre = ralph_get_objval(rm_nopre);

    ASSERT(status_pre == RALPH_STATUS_OPTIMAL, "Optimal with presolve");
    ASSERT(status_nopre == RALPH_STATUS_OPTIMAL, "Optimal without presolve");
    ASSERT_NEAR(obj_pre, obj_nopre, TOLERANCE,
                "Same objective with and without presolve");

    /* Optimal: min -x-y-z, x+y=10, x+z<=15
     * x=10, y=0 maximizes x contribution
     * Wait: min -x-y-z means we want to maximize x+y+z
     * x+y=10 (fixed sum), so maximize z. z <= 15-x.
     * To maximize z: minimize x. x >= 0 (from y<=10: x=10-y>=0).
     * x=0, y=10, z=15. obj = -0-10-15 = -25 */
    ASSERT_NEAR(obj_pre, -25.0, TOLERANCE, "Objective = -25.0");

    ralph_free(rm_pre);
    ralph_free(rm_nopre);
}

/* ============================================================================
 * Test 6: Doubleton equality with integer variable (should not eliminate)
 *
 * Verify that doubleton equality doesn't eliminate integer/binary variables.
 *   x + y = 5, x integer, y continuous
 *   Should eliminate y (continuous), NOT x (integer).
 * ============================================================================ */
static void test_doubleton_integer_guard(void) {
    printf("\n=== Test: Doubleton equality respects integer variables ===\n");

    RalphModel *rm = ralph_create();
    ralph_set_obj_sense(rm, RALPH_MINIMIZE);
    ralph_add_var(rm, 0.0, 10.0, 1.0, RALPH_INTEGER);     /* x (integer) */
    ralph_add_var(rm, 0.0, 10.0, 2.0, RALPH_CONTINUOUS);   /* y */
    ralph_add_var(rm, 0.0, 10.0, 1.0, RALPH_CONTINUOUS);   /* z */

    /* x + y = 5 */
    int idx0[] = {0, 1}; double v0[] = {1.0, 1.0};
    ralph_add_constraint(rm, 2, idx0, v0, RALPH_EQUAL, 5.0);

    /* y + z <= 8 */
    int idx1[] = {1, 2}; double v1[] = {1.0, 1.0};
    ralph_add_constraint(rm, 2, idx1, v1, RALPH_LESS_EQUAL, 8.0);

    ralph_set_int_param(rm, "presolve", 1);
    ralph_optimize(rm);

    RalphStatus status = ralph_get_status(rm);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Optimal (integer var preserved)");

    double sol[3];
    ralph_get_solution(rm, sol);

    /* Verify x is integer-valued */
    double x_frac = sol[0] - floor(sol[0] + 0.5);
    ASSERT(fabs(x_frac) < TOLERANCE, "x is integer-valued");

    /* Verify equality holds */
    ASSERT_NEAR(sol[0] + sol[1], 5.0, TOLERANCE, "x + y = 5");

    ralph_free(rm);
}

/* ============================================================================
 * Test 7: Presolve on problem where all variables get fixed
 *
 *   x = 3   (singleton equality)
 *   y = 7   (singleton equality)
 *   x + y <= 15 (should be redundant after fixing)
 *   min 2x + 3y => obj = 6 + 21 = 27
 * ============================================================================ */
static void test_all_fixed(void) {
    printf("\n=== Test: Presolve fixes all variables ===\n");

    RalphModel *rm = ralph_create();
    ralph_set_obj_sense(rm, RALPH_MINIMIZE);
    ralph_add_var(rm, 0.0, 100.0, 2.0, RALPH_CONTINUOUS);  /* x */
    ralph_add_var(rm, 0.0, 100.0, 3.0, RALPH_CONTINUOUS);  /* y */

    /* x = 3 */
    int idx0[] = {0}; double v0[] = {1.0};
    ralph_add_constraint(rm, 1, idx0, v0, RALPH_EQUAL, 3.0);

    /* y = 7 */
    int idx1[] = {1}; double v1[] = {1.0};
    ralph_add_constraint(rm, 1, idx1, v1, RALPH_EQUAL, 7.0);

    /* x + y <= 15 */
    int idx2[] = {0, 1}; double v2[] = {1.0, 1.0};
    ralph_add_constraint(rm, 2, idx2, v2, RALPH_LESS_EQUAL, 15.0);

    ralph_set_int_param(rm, "presolve", 1);
    ralph_optimize(rm);

    RalphStatus status = ralph_get_status(rm);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Optimal (all fixed)");

    double obj_val = ralph_get_objval(rm);
    ASSERT_NEAR(obj_val, 27.0, TOLERANCE, "Objective = 27.0 (2*3 + 3*7)");

    double sol[2];
    ralph_get_solution(rm, sol);
    ASSERT_NEAR(sol[0], 3.0, TOLERANCE, "x = 3");
    ASSERT_NEAR(sol[1], 7.0, TOLERANCE, "y = 7");

    ralph_free(rm);
}

/* ============================================================================
 * Test 8: Doubleton equality chain (multiple eliminations)
 *
 *   x + y = 10
 *   y + z = 15
 *   z + w <= 20
 *   min x + w
 *
 * After eliminating y = 10 - x: second eq becomes (10 - x) + z = 15 => z = 5 + x
 * After eliminating z (round 2): z + w <= 20 => (5 + x) + w <= 20 => x + w <= 15
 * min x + w s.t. x + w <= 15, x >= 0, w >= 0 => x=0, w=0, obj=0
 * Then y = 10, z = 5.
 * ============================================================================ */
static void test_doubleton_chain(void) {
    printf("\n=== Test: Doubleton equality chain ===\n");

    RalphModel *rm = ralph_create();
    ralph_set_obj_sense(rm, RALPH_MINIMIZE);
    ralph_add_var(rm, 0.0, 1e30, 1.0, RALPH_CONTINUOUS);  /* x */
    ralph_add_var(rm, 0.0, 1e30, 0.0, RALPH_CONTINUOUS);  /* y */
    ralph_add_var(rm, 0.0, 1e30, 0.0, RALPH_CONTINUOUS);  /* z */
    ralph_add_var(rm, 0.0, 1e30, 1.0, RALPH_CONTINUOUS);  /* w */

    /* x + y = 10 */
    int idx0[] = {0, 1}; double v0[] = {1.0, 1.0};
    ralph_add_constraint(rm, 2, idx0, v0, RALPH_EQUAL, 10.0);

    /* y + z = 15 */
    int idx1[] = {1, 2}; double v1[] = {1.0, 1.0};
    ralph_add_constraint(rm, 2, idx1, v1, RALPH_EQUAL, 15.0);

    /* z + w <= 20 */
    int idx2[] = {2, 3}; double v2[] = {1.0, 1.0};
    ralph_add_constraint(rm, 2, idx2, v2, RALPH_LESS_EQUAL, 20.0);

    ralph_set_int_param(rm, "presolve", 1);
    ralph_optimize(rm);

    RalphStatus status = ralph_get_status(rm);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Optimal");

    double obj_val = ralph_get_objval(rm);
    ASSERT_NEAR(obj_val, 0.0, TOLERANCE, "Objective = 0.0 (x=0, w=0)");

    double sol[4];
    ralph_get_solution(rm, sol);
    printf("  INFO: x=%.4f, y=%.4f, z=%.4f, w=%.4f\n",
           sol[0], sol[1], sol[2], sol[3]);

    /* Verify constraints hold in recovered solution */
    ASSERT_NEAR(sol[0] + sol[1], 10.0, TOLERANCE, "x + y = 10");
    ASSERT_NEAR(sol[1] + sol[2], 15.0, TOLERANCE, "y + z = 15");
    ASSERT(sol[2] + sol[3] <= 20.0 + TOLERANCE, "z + w <= 20");

    ralph_free(rm);
}

/* ============================================================================
 * Test 9: Singleton row with GE sense
 *
 *   -4*x >= -20  =>  x <= 5  (negative coefficient flips direction)
 *   2*y >= 6     =>  y >= 3
 *   min x + y, x >= 0, y >= 0
 * ============================================================================ */
static void test_singleton_row_ge(void) {
    printf("\n=== Test: Singleton row with >= sense ===\n");

    RalphModel *rm = ralph_create();
    ralph_set_obj_sense(rm, RALPH_MINIMIZE);
    ralph_add_var(rm, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);  /* x */
    ralph_add_var(rm, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);  /* y */

    /* -4*x >= -20 => x <= 5 */
    int idx0[] = {0}; double v0[] = {-4.0};
    ralph_add_constraint(rm, 1, idx0, v0, RALPH_GREATER_EQUAL, -20.0);

    /* 2*y >= 6 => y >= 3 */
    int idx1[] = {1}; double v1[] = {2.0};
    ralph_add_constraint(rm, 1, idx1, v1, RALPH_GREATER_EQUAL, 6.0);

    /* x + y <= 20 (extra constraint to keep model non-trivial) */
    int idx2[] = {0, 1}; double v2[] = {1.0, 1.0};
    ralph_add_constraint(rm, 2, idx2, v2, RALPH_LESS_EQUAL, 20.0);

    ralph_set_int_param(rm, "presolve", 1);
    ralph_optimize(rm);

    RalphStatus status = ralph_get_status(rm);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Optimal");

    double obj_val = ralph_get_objval(rm);
    /* min x + y: x=0, y=3 => obj=3 */
    ASSERT_NEAR(obj_val, 3.0, TOLERANCE, "Objective = 3.0 (x=0, y=3)");

    ralph_free(rm);
}

/* ============================================================================
 * Test 10: Presolve detects infeasibility from singleton row
 *
 *   5*x <= -1, x >= 0  =>  x <= -0.2, but x >= 0 → infeasible
 * ============================================================================ */
static void test_singleton_infeasible(void) {
    printf("\n=== Test: Presolve detects infeasibility ===\n");

    RalphModel *rm = ralph_create();
    ralph_set_obj_sense(rm, RALPH_MINIMIZE);
    ralph_add_var(rm, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);  /* x */
    ralph_add_var(rm, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);  /* y */

    /* 5*x <= -1 (infeasible with x >= 0) */
    int idx0[] = {0}; double v0[] = {5.0};
    ralph_add_constraint(rm, 1, idx0, v0, RALPH_LESS_EQUAL, -1.0);

    /* x + y <= 10 */
    int idx1[] = {0, 1}; double v1[] = {1.0, 1.0};
    ralph_add_constraint(rm, 2, idx1, v1, RALPH_LESS_EQUAL, 10.0);

    ralph_set_int_param(rm, "presolve", 1);
    ralph_optimize(rm);

    RalphStatus status = ralph_get_status(rm);
    ASSERT(status == RALPH_STATUS_INFEASIBLE || status == RALPH_STATUS_ERROR,
           "Infeasibility detected");

    ralph_free(rm);
}

/* ============================================================================
 * Test 11: Doubleton equality with large coefficient ratio
 *
 * Test numerical stability: 1000*x + 0.001*y = 5
 * Should eliminate x (larger coefficient, better pivot).
 * ============================================================================ */
static void test_doubleton_large_ratio(void) {
    printf("\n=== Test: Doubleton equality with large coefficient ratio ===\n");

    RalphModel *rm = ralph_create();
    ralph_set_obj_sense(rm, RALPH_MINIMIZE);
    ralph_add_var(rm, 0.0, 1e30, 1.0, RALPH_CONTINUOUS);   /* x */
    ralph_add_var(rm, 0.0, 1e30, 1.0, RALPH_CONTINUOUS);   /* y */
    ralph_add_var(rm, 0.0, 1e30, 1.0, RALPH_CONTINUOUS);   /* z */

    /* 1000*x + 0.001*y = 5 */
    int idx0[] = {0, 1}; double v0[] = {1000.0, 0.001};
    ralph_add_constraint(rm, 2, idx0, v0, RALPH_EQUAL, 5.0);

    /* y + z <= 100 */
    int idx1[] = {1, 2}; double v1[] = {1.0, 1.0};
    ralph_add_constraint(rm, 2, idx1, v1, RALPH_LESS_EQUAL, 100.0);

    ralph_set_int_param(rm, "presolve", 1);
    ralph_optimize(rm);

    RalphStatus status = ralph_get_status(rm);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Optimal (large ratio)");

    double sol[3];
    ralph_get_solution(rm, sol);

    /* Verify equality constraint */
    double lhs = 1000.0 * sol[0] + 0.001 * sol[1];
    ASSERT_NEAR(lhs, 5.0, 0.01, "1000*x + 0.001*y = 5 satisfied");

    ralph_free(rm);
}

/* ============================================================================
 * Test 12: Presolve preserves optimality on diet problem
 *
 * Classic diet problem — verify presolve doesn't change the optimal.
 * ============================================================================ */
static void test_presolve_diet(void) {
    printf("\n=== Test: Presolve preserves diet problem optimality ===\n");

    /* min 2*bread + 3.5*milk + 8*cheese + 1.5*potato + 4.2*fish + 2.3*maize
     * s.t. protein >= 55, fat >= 33, carbs >= 70, calories <= 2400
     * All vars >= 0 */

    double cost[] = {2.0, 3.5, 8.0, 1.5, 4.2, 2.3};
    double lb6[] = {0,0,0,0,0,0};
    double ub6[] = {1e30,1e30,1e30,1e30,1e30,1e30};
    int nvars = 6;

    /* Solve with presolve */
    RalphModel *rm_pre = ralph_create();
    ralph_set_obj_sense(rm_pre, RALPH_MINIMIZE);
    for (int j = 0; j < nvars; j++)
        ralph_add_var(rm_pre, lb6[j], ub6[j], cost[j], RALPH_CONTINUOUS);

    /* Protein >= 55 */
    int ip[] = {0,1,2,3,4,5}; double vp[] = {3.7,8.0,34.0,0.5,15.0,4.0};
    ralph_add_constraint(rm_pre, 6, ip, vp, RALPH_GREATER_EQUAL, 55.0);
    /* Fat >= 33 */
    double vf[] = {2.2,6.0,15.0,0.3,11.0,2.5};
    ralph_add_constraint(rm_pre, 6, ip, vf, RALPH_GREATER_EQUAL, 33.0);
    /* Carbs >= 70 */
    double vc[] = {15.0,12.0,1.0,22.0,0.0,18.0};
    ralph_add_constraint(rm_pre, 6, ip, vc, RALPH_GREATER_EQUAL, 70.0);
    /* Calories <= 2400 */
    double vk[] = {90.0,120.0,106.0,97.0,130.0,100.0};
    ralph_add_constraint(rm_pre, 6, ip, vk, RALPH_LESS_EQUAL, 2400.0);

    ralph_set_int_param(rm_pre, "presolve", 1);
    ralph_optimize(rm_pre);

    /* Solve without presolve */
    RalphModel *rm_nopre = ralph_create();
    ralph_set_obj_sense(rm_nopre, RALPH_MINIMIZE);
    for (int j = 0; j < nvars; j++)
        ralph_add_var(rm_nopre, lb6[j], ub6[j], cost[j], RALPH_CONTINUOUS);

    ralph_add_constraint(rm_nopre, 6, ip, vp, RALPH_GREATER_EQUAL, 55.0);
    ralph_add_constraint(rm_nopre, 6, ip, vf, RALPH_GREATER_EQUAL, 33.0);
    ralph_add_constraint(rm_nopre, 6, ip, vc, RALPH_GREATER_EQUAL, 70.0);
    ralph_add_constraint(rm_nopre, 6, ip, vk, RALPH_LESS_EQUAL, 2400.0);

    ralph_set_int_param(rm_nopre, "presolve", 0);
    ralph_optimize(rm_nopre);

    RalphStatus s1 = ralph_get_status(rm_pre);
    RalphStatus s2 = ralph_get_status(rm_nopre);
    ASSERT(s1 == RALPH_STATUS_OPTIMAL, "Diet with presolve: OPTIMAL");
    ASSERT(s2 == RALPH_STATUS_OPTIMAL, "Diet without presolve: OPTIMAL");

    double obj1 = ralph_get_objval(rm_pre);
    double obj2 = ralph_get_objval(rm_nopre);
    ASSERT_NEAR(obj1, obj2, 0.01, "Same objective with and without presolve");

    ralph_free(rm_pre);
    ralph_free(rm_nopre);
}

/* ============================================================================
 * Test 13: Proportional rows (duplicate <=)
 *
 * Two proportional <= constraints: 2x + 4y <= 20 and x + 2y <= 8
 * Row 1 = 2 * Row 2, so they're proportional with ratio=2.
 * After scaling: Row 1 says x + 2y <= 10, Row 2 says x + 2y <= 8.
 * Row 2 is tighter (8 < 10), so Row 1 should be removed.
 * ============================================================================ */
static void test_proportional_rows(void) {
    printf("\n=== Test: Proportional rows (keep tighter) ===\n");

    RalphModel *rm = ralph_create();
    ralph_set_obj_sense(rm, RALPH_MINIMIZE);
    ralph_add_var(rm, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);  /* x */
    ralph_add_var(rm, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);  /* y */

    /* 2x + 4y <= 20  (equivalent to x + 2y <= 10) */
    int idx0[] = {0, 1}; double v0[] = {2.0, 4.0};
    ralph_add_constraint(rm, 2, idx0, v0, RALPH_LESS_EQUAL, 20.0);

    /* x + 2y <= 8  (tighter) */
    int idx1[] = {0, 1}; double v1[] = {1.0, 2.0};
    ralph_add_constraint(rm, 2, idx1, v1, RALPH_LESS_EQUAL, 8.0);

    /* x + y >= 1  (extra constraint) */
    int idx2[] = {0, 1}; double v2[] = {1.0, 1.0};
    ralph_add_constraint(rm, 2, idx2, v2, RALPH_GREATER_EQUAL, 1.0);

    ralph_set_int_param(rm, "presolve", 1);
    ralph_optimize(rm);

    RalphStatus status = ralph_get_status(rm);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Optimal with proportional rows");

    /* min x + y, x + 2y <= 8, x + y >= 1, x,y >= 0
     * Optimal: x=1, y=0 => obj=1 */
    double obj_val = ralph_get_objval(rm);
    ASSERT_NEAR(obj_val, 1.0, TOLERANCE, "Objective = 1.0");

    ralph_free(rm);
}

/* ============================================================================
 * Test 14: Proportional rows detect infeasibility
 *
 * Two proportional equality rows with inconsistent RHS:
 *   x + y = 5
 *   2x + 2y = 12   (=> x + y = 6, contradicts x + y = 5)
 * ============================================================================ */
static void test_proportional_rows_infeasible(void) {
    printf("\n=== Test: Proportional rows detect infeasibility ===\n");

    RalphModel *rm = ralph_create();
    ralph_set_obj_sense(rm, RALPH_MINIMIZE);
    ralph_add_var(rm, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);  /* x */
    ralph_add_var(rm, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);  /* y */

    /* x + y = 5 */
    int idx0[] = {0, 1}; double v0[] = {1.0, 1.0};
    ralph_add_constraint(rm, 2, idx0, v0, RALPH_EQUAL, 5.0);

    /* 2x + 2y = 12 (inconsistent: implies x + y = 6) */
    int idx1[] = {0, 1}; double v1[] = {2.0, 2.0};
    ralph_add_constraint(rm, 2, idx1, v1, RALPH_EQUAL, 12.0);

    ralph_set_int_param(rm, "presolve", 1);
    ralph_optimize(rm);

    RalphStatus status = ralph_get_status(rm);
    ASSERT(status == RALPH_STATUS_INFEASIBLE || status == RALPH_STATUS_ERROR,
           "Infeasibility detected from inconsistent proportional equalities");

    ralph_free(rm);
}

/* ============================================================================
 * Test 15: Proportional columns (dominated variable elimination)
 *
 * min x + y + z
 * s.t. x + y + 2z <= 10   (columns x and y have identical coefficients)
 *      x + y + z <= 8
 *      x, y, z >= 0
 *
 * Columns 0 (x) and 1 (y) are proportional (ratio=1).
 * Same cost c[x]=c[y]=1. One should be fixed at lb=0.
 * After fixing one, the problem reduces. Verify optimal is correct.
 * ============================================================================ */
static void test_proportional_cols(void) {
    printf("\n=== Test: Proportional columns (dominated variable) ===\n");

    /* Solve with presolve */
    RalphModel *rm_pre = ralph_create();
    ralph_set_obj_sense(rm_pre, RALPH_MINIMIZE);
    ralph_add_var(rm_pre, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);  /* x */
    ralph_add_var(rm_pre, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);  /* y */
    ralph_add_var(rm_pre, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);  /* z */

    int idx0[] = {0, 1, 2}; double v0[] = {1.0, 1.0, 2.0};
    ralph_add_constraint(rm_pre, 3, idx0, v0, RALPH_LESS_EQUAL, 10.0);
    int idx1[] = {0, 1, 2}; double v1[] = {1.0, 1.0, 1.0};
    ralph_add_constraint(rm_pre, 3, idx1, v1, RALPH_LESS_EQUAL, 8.0);

    ralph_set_int_param(rm_pre, "presolve", 1);
    ralph_optimize(rm_pre);

    /* Solve without presolve for reference */
    RalphModel *rm_nopre = ralph_create();
    ralph_set_obj_sense(rm_nopre, RALPH_MINIMIZE);
    ralph_add_var(rm_nopre, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);
    ralph_add_var(rm_nopre, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);
    ralph_add_var(rm_nopre, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);

    ralph_add_constraint(rm_nopre, 3, idx0, v0, RALPH_LESS_EQUAL, 10.0);
    ralph_add_constraint(rm_nopre, 3, idx1, v1, RALPH_LESS_EQUAL, 8.0);

    ralph_set_int_param(rm_nopre, "presolve", 0);
    ralph_optimize(rm_nopre);

    RalphStatus s1 = ralph_get_status(rm_pre);
    RalphStatus s2 = ralph_get_status(rm_nopre);
    ASSERT(s1 == RALPH_STATUS_OPTIMAL, "Optimal with presolve");
    ASSERT(s2 == RALPH_STATUS_OPTIMAL, "Optimal without presolve");

    double obj1 = ralph_get_objval(rm_pre);
    double obj2 = ralph_get_objval(rm_nopre);
    ASSERT_NEAR(obj1, obj2, TOLERANCE,
                "Same objective with and without presolve");

    /* min x+y+z, all >= 0 and constraints are satisfied by x=y=z=0 => obj=0 */
    ASSERT_NEAR(obj1, 0.0, TOLERANCE, "Objective = 0.0");

    ralph_free(rm_pre);
    ralph_free(rm_nopre);
}

/* ============================================================================
 * Test 16: Proportional columns with cost dominance
 *
 * min 2x + y + z
 * s.t. x + y + z <= 10      (columns x and y are proportional, ratio=1)
 *      x + y + 2z <= 15
 *      x, y, z >= 0
 *
 * Columns 0 (x) and 1 (y) are proportional with ratio=1.
 * c[x]=2 > c[y]=1, so x is dominated. Fix x at lb=0.
 * Verify objective matches solving without presolve.
 * ============================================================================ */
static void test_proportional_cols_cost(void) {
    printf("\n=== Test: Proportional columns with cost dominance ===\n");

    /* Solve with presolve */
    RalphModel *rm_pre = ralph_create();
    ralph_set_obj_sense(rm_pre, RALPH_MINIMIZE);
    ralph_add_var(rm_pre, 0.0, 100.0, 2.0, RALPH_CONTINUOUS);  /* x (more expensive) */
    ralph_add_var(rm_pre, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);  /* y (cheaper) */
    ralph_add_var(rm_pre, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);  /* z */

    int idx0[] = {0, 1, 2}; double v0[] = {1.0, 1.0, 1.0};
    ralph_add_constraint(rm_pre, 3, idx0, v0, RALPH_LESS_EQUAL, 10.0);
    int idx1[] = {0, 1, 2}; double v1[] = {1.0, 1.0, 2.0};
    ralph_add_constraint(rm_pre, 3, idx1, v1, RALPH_LESS_EQUAL, 15.0);

    ralph_set_int_param(rm_pre, "presolve", 1);
    ralph_optimize(rm_pre);

    /* Solve without presolve */
    RalphModel *rm_nopre = ralph_create();
    ralph_set_obj_sense(rm_nopre, RALPH_MINIMIZE);
    ralph_add_var(rm_nopre, 0.0, 100.0, 2.0, RALPH_CONTINUOUS);
    ralph_add_var(rm_nopre, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);
    ralph_add_var(rm_nopre, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);

    ralph_add_constraint(rm_nopre, 3, idx0, v0, RALPH_LESS_EQUAL, 10.0);
    ralph_add_constraint(rm_nopre, 3, idx1, v1, RALPH_LESS_EQUAL, 15.0);

    ralph_set_int_param(rm_nopre, "presolve", 0);
    ralph_optimize(rm_nopre);

    RalphStatus s1 = ralph_get_status(rm_pre);
    RalphStatus s2 = ralph_get_status(rm_nopre);
    ASSERT(s1 == RALPH_STATUS_OPTIMAL, "Optimal with presolve");
    ASSERT(s2 == RALPH_STATUS_OPTIMAL, "Optimal without presolve");

    double obj1 = ralph_get_objval(rm_pre);
    double obj2 = ralph_get_objval(rm_nopre);
    ASSERT_NEAR(obj1, obj2, TOLERANCE,
                "Same objective with and without presolve");

    /* x should be 0 (dominated), so solution uses y and z instead */
    double sol[3];
    ralph_get_solution(rm_pre, sol);
    ASSERT_NEAR(sol[0], 0.0, TOLERANCE, "x = 0 (dominated, fixed at lb)");

    ralph_free(rm_pre);
    ralph_free(rm_nopre);
}

/* ============================================================================
 * Test 16b: Proportional columns with maximization (regression)
 *
 * Regression test for bug where proportional_cols fixed dominated variable
 * at upper bound for maximization problems with negative effective cost,
 * producing suboptimal solutions.
 *
 * max 2x + 3y
 * s.t. x + y <= 10
 *      x, y in [0, 7]
 *
 * Columns are proportional (ratio=1). For max, y is more valuable (c_y=3 > c_x=2).
 * Optimal: y=7, x=3, obj=6+21=27.
 * Bug was: x fixed at ub=7, y=3, obj=14+9=23 (suboptimal).
 * Fix: skip proportional col elimination when dominated var has negative
 * effective cost (would need bound expansion on non-dominated variable).
 * ============================================================================ */
static void test_proportional_cols_maximize(void) {
    printf("\n=== Test: Proportional columns with maximization (regression) ===\n");

    /* Solve with presolve */
    RalphModel *rm_pre = ralph_create();
    ralph_set_obj_sense(rm_pre, RALPH_MAXIMIZE);
    ralph_add_var(rm_pre, 0.0, 7.0, 2.0, RALPH_CONTINUOUS);  /* x */
    ralph_add_var(rm_pre, 0.0, 7.0, 3.0, RALPH_CONTINUOUS);  /* y */

    int idx0[] = {0, 1}; double v0[] = {1.0, 1.0};
    ralph_add_constraint(rm_pre, 2, idx0, v0, RALPH_LESS_EQUAL, 10.0);

    ralph_set_int_param(rm_pre, "presolve", 1);
    ralph_optimize(rm_pre);

    /* Solve without presolve for reference */
    RalphModel *rm_nopre = ralph_create();
    ralph_set_obj_sense(rm_nopre, RALPH_MAXIMIZE);
    ralph_add_var(rm_nopre, 0.0, 7.0, 2.0, RALPH_CONTINUOUS);
    ralph_add_var(rm_nopre, 0.0, 7.0, 3.0, RALPH_CONTINUOUS);

    ralph_add_constraint(rm_nopre, 2, idx0, v0, RALPH_LESS_EQUAL, 10.0);

    ralph_set_int_param(rm_nopre, "presolve", 0);
    ralph_optimize(rm_nopre);

    RalphStatus s1 = ralph_get_status(rm_pre);
    RalphStatus s2 = ralph_get_status(rm_nopre);
    ASSERT(s1 == RALPH_STATUS_OPTIMAL, "Optimal with presolve");
    ASSERT(s2 == RALPH_STATUS_OPTIMAL, "Optimal without presolve");

    double obj1 = ralph_get_objval(rm_pre);
    double obj2 = ralph_get_objval(rm_nopre);
    ASSERT_NEAR(obj1, obj2, TOLERANCE,
                "Same objective with and without presolve");

    /* max 2x + 3y, x+y<=10, x,y in [0,7] => y=7, x=3, obj=27 */
    ASSERT_NEAR(obj1, 27.0, TOLERANCE, "Objective = 27.0 (not 23)");

    ralph_free(rm_pre);
    ralph_free(rm_nopre);
}

/* ============================================================================
 * Test 17: Shift-variable-bounds postsolve
 *
 * min x + y
 * s.t. x + y <= 20
 *      x >= 5, y >= 3  (non-zero lower bounds trigger shifting)
 *      x <= 15, y <= 10
 *
 * After shifting: x' = x - 5, y' = y - 3
 *   min (x'+5) + (y'+3) = x' + y' + 8
 *   s.t. (x'+5) + (y'+3) <= 20 => x' + y' <= 12
 *        x' >= 0, y' >= 0, x' <= 10, y' <= 7
 *
 * Optimal: x'=0, y'=0 => x=5, y=3, obj=8
 * ============================================================================ */
static void test_shift_bounds(void) {
    printf("\n=== Test: Shift-variable-bounds postsolve ===\n");

    RalphModel *rm = ralph_create();
    ralph_set_obj_sense(rm, RALPH_MINIMIZE);
    ralph_add_var(rm, 5.0, 15.0, 1.0, RALPH_CONTINUOUS);  /* x: lb=5, ub=15 */
    ralph_add_var(rm, 3.0, 10.0, 1.0, RALPH_CONTINUOUS);  /* y: lb=3, ub=10 */

    /* x + y <= 20 */
    int idx0[] = {0, 1}; double v0[] = {1.0, 1.0};
    ralph_add_constraint(rm, 2, idx0, v0, RALPH_LESS_EQUAL, 20.0);

    ralph_set_int_param(rm, "presolve", 1);
    ralph_optimize(rm);

    RalphStatus status = ralph_get_status(rm);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Optimal");

    double obj_val = ralph_get_objval(rm);
    ASSERT_NEAR(obj_val, 8.0, TOLERANCE, "Objective = 8.0 (x=5, y=3)");

    double sol[2];
    ralph_get_solution(rm, sol);
    ASSERT_NEAR(sol[0], 5.0, TOLERANCE, "x = 5.0 (shifted back from lb)");
    ASSERT_NEAR(sol[1], 3.0, TOLERANCE, "y = 3.0 (shifted back from lb)");

    ralph_free(rm);
}

/* ============================================================================
 * Test 18: Shift-variable-bounds with maximization
 *
 * max 2x + 3y
 * s.t. x + 2y <= 18    (non-proportional columns to avoid prop-cols interaction)
 *      x + y <= 10
 *      x >= 2, y >= 1
 *      x <= 8, y <= 7
 *
 * Columns are NOT proportional (coefficients [1,1] vs [2,1]).
 * Shift-bounds transforms: x'=x-2, y'=y-1
 * Optimal: max 2x + 3y, x+2y<=18, x+y<=10 => y=7, x=3, obj=6+21=27
 * (x+2y = 3+14 = 17 <= 18, x+y = 10 <= 10)
 * ============================================================================ */
static void test_shift_bounds_maximize(void) {
    printf("\n=== Test: Shift-variable-bounds with maximization ===\n");

    /* Solve with presolve */
    RalphModel *rm_pre = ralph_create();
    ralph_set_obj_sense(rm_pre, RALPH_MAXIMIZE);
    ralph_add_var(rm_pre, 2.0, 8.0, 2.0, RALPH_CONTINUOUS);  /* x */
    ralph_add_var(rm_pre, 1.0, 7.0, 3.0, RALPH_CONTINUOUS);  /* y */

    int idx0[] = {0, 1}; double v0[] = {1.0, 2.0};
    ralph_add_constraint(rm_pre, 2, idx0, v0, RALPH_LESS_EQUAL, 18.0);
    int idx1[] = {0, 1}; double v1[] = {1.0, 1.0};
    ralph_add_constraint(rm_pre, 2, idx1, v1, RALPH_LESS_EQUAL, 10.0);

    ralph_set_int_param(rm_pre, "presolve", 1);
    ralph_optimize(rm_pre);

    /* Solve without presolve */
    RalphModel *rm_nopre = ralph_create();
    ralph_set_obj_sense(rm_nopre, RALPH_MAXIMIZE);
    ralph_add_var(rm_nopre, 2.0, 8.0, 2.0, RALPH_CONTINUOUS);
    ralph_add_var(rm_nopre, 1.0, 7.0, 3.0, RALPH_CONTINUOUS);

    ralph_add_constraint(rm_nopre, 2, idx0, v0, RALPH_LESS_EQUAL, 18.0);
    ralph_add_constraint(rm_nopre, 2, idx1, v1, RALPH_LESS_EQUAL, 10.0);

    ralph_set_int_param(rm_nopre, "presolve", 0);
    ralph_optimize(rm_nopre);

    RalphStatus s1 = ralph_get_status(rm_pre);
    RalphStatus s2 = ralph_get_status(rm_nopre);
    ASSERT(s1 == RALPH_STATUS_OPTIMAL, "Optimal with presolve");
    ASSERT(s2 == RALPH_STATUS_OPTIMAL, "Optimal without presolve");

    double obj1 = ralph_get_objval(rm_pre);
    double obj2 = ralph_get_objval(rm_nopre);
    ASSERT_NEAR(obj1, obj2, TOLERANCE,
                "Same objective with and without presolve");

    /* max 2x + 3y => y=7, x=3, obj=27 */
    ASSERT_NEAR(obj1, 27.0, TOLERANCE, "Objective = 27.0");

    double sol[2];
    ralph_get_solution(rm_pre, sol);
    ASSERT_NEAR(sol[0], 3.0, TOLERANCE, "x = 3.0");
    ASSERT_NEAR(sol[1], 7.0, TOLERANCE, "y = 7.0");

    ralph_free(rm_pre);
    ralph_free(rm_nopre);
}

/* ============================================================================
 * Test 19: Forcing constraint
 *
 * min x + y + z
 * s.t. x + y + z <= 0   (forcing: all vars must be at lb=0)
 *      x, y, z >= 0
 *
 * Row lower bound = 0+0+0 = 0 >= rhs=0, so this is forcing.
 * All variables fixed at lower bounds: x=y=z=0, obj=0.
 * ============================================================================ */
static void test_forcing_constraint(void) {
    printf("\n=== Test: Forcing constraint ===\n");

    RalphModel *rm = ralph_create();
    ralph_set_obj_sense(rm, RALPH_MINIMIZE);
    ralph_add_var(rm, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);  /* x */
    ralph_add_var(rm, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);  /* y */
    ralph_add_var(rm, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);  /* z */

    /* x + y + z <= 0 (all non-negative vars, so forces all to 0) */
    int idx0[] = {0, 1, 2}; double v0[] = {1.0, 1.0, 1.0};
    ralph_add_constraint(rm, 3, idx0, v0, RALPH_LESS_EQUAL, 0.0);

    ralph_set_int_param(rm, "presolve", 1);
    ralph_optimize(rm);

    RalphStatus status = ralph_get_status(rm);
    ASSERT(status == RALPH_STATUS_OPTIMAL, "Optimal (forcing constraint)");

    double obj_val = ralph_get_objval(rm);
    ASSERT_NEAR(obj_val, 0.0, TOLERANCE, "Objective = 0.0 (all at lb)");

    double sol[3];
    ralph_get_solution(rm, sol);
    ASSERT_NEAR(sol[0], 0.0, TOLERANCE, "x = 0");
    ASSERT_NEAR(sol[1], 0.0, TOLERANCE, "y = 0");
    ASSERT_NEAR(sol[2], 0.0, TOLERANCE, "z = 0");

    ralph_free(rm);
}

/* ============================================================================
 * Test 20: Combined presolve stress test
 *
 * A problem that exercises multiple presolve techniques together:
 * - Singleton rows
 * - Proportional columns
 * - Doubleton equality
 * - Shift-variable-bounds
 * - Bound tightening
 *
 * min 3a + 2b + c + d
 * s.t. a <= 5             (singleton row)
 *      b + c = 10         (doubleton equality)
 *      b + d + e <= 20    (b and d proportional in constraints)
 *      a + c + e <= 15
 *      a >= 2, b >= 1, c >= 0, d >= 0, e >= 0
 * ============================================================================ */
static void test_combined_presolve(void) {
    printf("\n=== Test: Combined presolve stress test ===\n");

    /* Solve with presolve */
    RalphModel *rm_pre = ralph_create();
    ralph_set_obj_sense(rm_pre, RALPH_MINIMIZE);
    ralph_add_var(rm_pre, 2.0, 100.0, 3.0, RALPH_CONTINUOUS);  /* a: lb=2 (shift) */
    ralph_add_var(rm_pre, 1.0, 100.0, 2.0, RALPH_CONTINUOUS);  /* b: lb=1 (shift) */
    ralph_add_var(rm_pre, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);  /* c */
    ralph_add_var(rm_pre, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);  /* d */
    ralph_add_var(rm_pre, 0.0, 100.0, 0.0, RALPH_CONTINUOUS);  /* e */

    /* a <= 5 (singleton) */
    int ia[] = {0}; double va[] = {1.0};
    ralph_add_constraint(rm_pre, 1, ia, va, RALPH_LESS_EQUAL, 5.0);

    /* b + c = 10 (doubleton equality) */
    int ibc[] = {1, 2}; double vbc[] = {1.0, 1.0};
    ralph_add_constraint(rm_pre, 2, ibc, vbc, RALPH_EQUAL, 10.0);

    /* b + d + e <= 20 */
    int ibde[] = {1, 3, 4}; double vbde[] = {1.0, 1.0, 1.0};
    ralph_add_constraint(rm_pre, 3, ibde, vbde, RALPH_LESS_EQUAL, 20.0);

    /* a + c + e <= 15 */
    int iace[] = {0, 2, 4}; double vace[] = {1.0, 1.0, 1.0};
    ralph_add_constraint(rm_pre, 3, iace, vace, RALPH_LESS_EQUAL, 15.0);

    ralph_set_int_param(rm_pre, "presolve", 1);
    ralph_optimize(rm_pre);

    /* Solve without presolve */
    RalphModel *rm_nopre = ralph_create();
    ralph_set_obj_sense(rm_nopre, RALPH_MINIMIZE);
    ralph_add_var(rm_nopre, 2.0, 100.0, 3.0, RALPH_CONTINUOUS);
    ralph_add_var(rm_nopre, 1.0, 100.0, 2.0, RALPH_CONTINUOUS);
    ralph_add_var(rm_nopre, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);
    ralph_add_var(rm_nopre, 0.0, 100.0, 1.0, RALPH_CONTINUOUS);
    ralph_add_var(rm_nopre, 0.0, 100.0, 0.0, RALPH_CONTINUOUS);

    ralph_add_constraint(rm_nopre, 1, ia, va, RALPH_LESS_EQUAL, 5.0);
    ralph_add_constraint(rm_nopre, 2, ibc, vbc, RALPH_EQUAL, 10.0);
    ralph_add_constraint(rm_nopre, 3, ibde, vbde, RALPH_LESS_EQUAL, 20.0);
    ralph_add_constraint(rm_nopre, 3, iace, vace, RALPH_LESS_EQUAL, 15.0);

    ralph_set_int_param(rm_nopre, "presolve", 0);
    ralph_optimize(rm_nopre);

    RalphStatus s1 = ralph_get_status(rm_pre);
    RalphStatus s2 = ralph_get_status(rm_nopre);
    ASSERT(s1 == RALPH_STATUS_OPTIMAL, "Optimal with presolve");
    ASSERT(s2 == RALPH_STATUS_OPTIMAL, "Optimal without presolve");

    double obj1 = ralph_get_objval(rm_pre);
    double obj2 = ralph_get_objval(rm_nopre);
    ASSERT_NEAR(obj1, obj2, TOLERANCE,
                "Same objective with and without presolve");

    /* Verify constraint satisfaction */
    double sol[5];
    ralph_get_solution(rm_pre, sol);
    ASSERT(sol[0] <= 5.0 + TOLERANCE, "a <= 5 satisfied");
    ASSERT_NEAR(sol[1] + sol[2], 10.0, TOLERANCE, "b + c = 10 satisfied");

    ralph_free(rm_pre);
    ralph_free(rm_nopre);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════╗\n");
    printf("║     Ralph Presolve Unit Tests            ║\n");
    printf("╚══════════════════════════════════════════╝\n");

    test_multi_round();
    test_singleton_row_inequality();
    test_doubleton_equality();
    test_doubleton_postsolve();
    test_implied_free();
    test_doubleton_integer_guard();
    test_all_fixed();
    test_doubleton_chain();
    test_singleton_row_ge();
    test_singleton_infeasible();
    test_doubleton_large_ratio();
    test_presolve_diet();
    test_proportional_rows();
    test_proportional_rows_infeasible();
    test_proportional_cols();
    test_proportional_cols_cost();
    test_proportional_cols_maximize();
    test_shift_bounds();
    test_shift_bounds_maximize();
    test_forcing_constraint();
    test_combined_presolve();

    printf("\n══════════════════════════════════════════════════════════\n");
    printf("Presolve Tests: %d/%d passed (%.1f%%)\n",
           tests_passed, tests_run, 100.0 * tests_passed / tests_run);

    if (tests_passed == tests_run) {
        printf("\n✓ All presolve tests passed!\n");
        return 0;
    } else {
        printf("\n✗ Some presolve tests failed\n");
        return 1;
    }
}
