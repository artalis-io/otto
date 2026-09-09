/*
 * test_unbounded_infinite_bound.c
 *
 * Regression test for the artalis-io/otto#86 "1e30-infinity-sentinel" sibling.
 *
 * The primal ratio test computes a leaving-variable ratio as
 * (ub - x) / (-dk) for a basic variable moving toward its upper bound. When
 * that upper bound is the infinity sentinel RALPH_INFINITY (1e30), the ratio
 * is a huge but *finite* number; for pivot magnitudes |dk| > ~2 it falls below
 * the RALPH_INFINITY/2 unbounded threshold, so a genuinely non-blocking
 * sentinel bound was treated as a real blocking constraint. The simplex then
 * capped an unbounded ray at ~1e30 and reported a spurious OPTIMAL (obj ~1e30)
 * instead of UNBOUNDED.
 *
 * The fix makes every ratio test treat a bound of sentinel magnitude
 * (|bound| >= RALPH_INFINITY/2) as non-blocking, mirroring the guard already
 * applied to the entering variable in entering_bound_flip_distance().
 *
 * Each case below is a genuinely unbounded LP whose unbounded ray runs through
 * a variable with an infinite upper bound. Both the base simplex (presolve off)
 * and the presolve path must report UNBOUNDED. Part of `make -C ralph test`.
 */

#include <stdio.h>
#include "ralph_lp.h"

static int check_unbounded(const char *name,
                           void (*build)(RalphLPModel *),
                           int presolve) {
    RalphLPModel *m = ralph_lp_create();
    build(m);
    ralph_lp_set_int_param(m, "presolve", presolve);
    ralph_lp_optimize(m);
    RalphLPStatus st = ralph_lp_get_status(m);
    double obj = ralph_lp_get_objval(m);
    int ok = (st == RALPH_LP_STATUS_UNBOUNDED);
    printf("  [%s] presolve=%d: %s obj=%g  %s\n",
           name, presolve, ralph_lp_status_string(st), obj,
           ok ? "OK" : "FAIL (expected UNBOUNDED)");
    ralph_lp_free(m);
    return ok;
}

/* min -x0, x0 in [0, +inf): the entering variable itself is unbounded. */
static void build_single(RalphLPModel *m) {
    ralph_lp_add_var(m, 0, RALPH_LP_INFINITY, -1.0, RALPH_LP_VAR_CONTINUOUS);
    int idx[] = {0};
    double val[] = {1.0};
    ralph_lp_add_constraint(m, 1, idx, val, RALPH_LP_SENSE_GREATER_EQUAL, 0.0);
}

/*
 * min 4 x0 - 5 x1, x0,x1 in [0, +inf)
 *   5 x1        >= -2
 *  -5 x0 - 2 x1 <=  8
 *  -3 x0        <=  7
 *  -2 x0 +   x1 <=  3
 * Raising x0 lets x1 rise with it (x1 <= 3 + 2 x0); the objective
 * 4 x0 - 5 x1 -> -inf. Unbounded. The ray drives a basic variable through its
 * +inf bound, which is exactly what the sentinel guard must not treat as
 * blocking. (Minimal reproducer from the infinite-bounds differential fuzzer.)
 */
static void build_ray_through_inf(RalphLPModel *m) {
    ralph_lp_add_var(m, 0, RALPH_LP_INFINITY, 4.0, RALPH_LP_VAR_CONTINUOUS);
    ralph_lp_add_var(m, 0, RALPH_LP_INFINITY, -5.0, RALPH_LP_VAR_CONTINUOUS);
    int i0[] = {1};    double v0[] = {5};      ralph_lp_add_constraint(m, 1, i0, v0, RALPH_LP_SENSE_GREATER_EQUAL, -2);
    int i1[] = {0, 1}; double v1[] = {-5, -2}; ralph_lp_add_constraint(m, 2, i1, v1, RALPH_LP_SENSE_LESS_EQUAL, 8);
    int i2[] = {0};    double v2[] = {-3};     ralph_lp_add_constraint(m, 1, i2, v2, RALPH_LP_SENSE_LESS_EQUAL, 7);
    int i3[] = {0, 1}; double v3[] = {-2, 1};  ralph_lp_add_constraint(m, 2, i3, v3, RALPH_LP_SENSE_LESS_EQUAL, 3);
}

int main(void) {
    printf("Ralph unbounded-with-infinite-bound regression (otto#86 sibling)\n");
    printf("================================================================\n");

    int fails = 0;
    for (int p = 0; p <= 1; p++) {
        fails += !check_unbounded("single-var", build_single, p);
        fails += !check_unbounded("ray-through-inf", build_ray_through_inf, p);
    }

    printf("----------------------------------------------------------------\n");
    if (fails == 0) {
        printf("RESULT: all unbounded cases detected on both paths. PASS\n");
        return 0;
    }
    printf("RESULT: %d case(s) misreported. FAIL\n", fails);
    return 1;
}
