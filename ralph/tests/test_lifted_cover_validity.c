/*
 * test_lifted_cover_validity.c
 *
 * Verify-first harness for the numerical-correctness audit finding:
 *   "generate_lifted_cover_cuts lifts each non-cover variable INDEPENDENTLY
 *    against the original cover (not sequentially), which can over-lift and
 *    cut off an integer-feasible point -> invalid cut -> wrong MIP optimum."
 *
 * Method: drive the real generate_lifted_cover_cuts() over many random 0/1
 * knapsack rows, injecting a fractional LP point to steer cover selection.
 * For every emitted cut, EXHAUSTIVELY enumerate all 2^n binary points; if any
 * point that is FEASIBLE for the knapsack VIOLATES the cut, the cut is invalid
 * and the finding is confirmed with a concrete instance.
 *
 * A cut of the original knapsack row must not remove any integer-feasible
 * point of that row. (The separator only ever adds these to the original
 * model, so validity in the original 0/1 space is exactly the requirement.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "lp.h"
#include "mip.h"

/* Deterministic LCG so a found counterexample is reproducible. */
static unsigned long g_rng = 0x9e3779b97f4a7c15UL;
static unsigned int lcg(void) {
    g_rng = g_rng * 6364136223846793005UL + 1442695040888963407UL;
    return (unsigned int)(g_rng >> 33);
}
static int rand_range(int lo, int hi) { return lo + (int)(lcg() % (unsigned)(hi - lo + 1)); }

/*
 * Run one knapsack row through the lifted-cover separator and enumerate the
 * emitted cuts for validity. Returns the number of INVALID cuts found (0 = all
 * emitted cuts valid). Prints the first violation of each invalid cut.
 */
static int check_instance(int n, const double *a, double b, const double *lp_x,
                          int verbose) {
    LPModel *model = lp_model_create();
    model->obj_sense = 1;
    for (int j = 0; j < n; j++) {
        lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');
    }
    int *idx = (int *)malloc((size_t)n * sizeof(int));
    double *cf = (double *)malloc((size_t)n * sizeof(double));
    for (int j = 0; j < n; j++) { idx[j] = j; cf[j] = a[j]; }
    if (lp_model_add_constraint(model, n, idx, cf, 'L', b) != 0) {
        free(idx); free(cf); lp_model_free(model); return 0;
    }
    free(idx); free(cf);
    lp_model_finalize(model);

    MIPSolver *solver = mip_create(model, 0, 64);
    if (!solver) { lp_model_free(model); return 0; }
    solver->max_cuts_per_round = 100;

    /* mip_create does not build the LP solver; the separator needs one plus a
       fractional solution to pick the cover. */
    if (!solver->lp_solver) {
        solver->lp_solver = simplex_create(solver->working_model);
    }
    if (!solver->lp_solver) { mip_free(solver); lp_model_free(model); return 0; }

    double *xin = (double *)malloc((size_t)n * sizeof(double));
    for (int j = 0; j < n; j++) xin[j] = lp_x[j];
    double *saved = solver->lp_solver->solution;
    solver->lp_solver->solution = xin;

    CutPool *pool = cut_pool_create(64);
    generate_lifted_cover_cuts(solver, pool);

    solver->lp_solver->solution = saved;  /* restore before free */

    int invalid_cuts = 0;
    for (int c = 0; c < pool->count; c++) {
        Cut *cut = pool->cuts[c];
        if (cut->sense != 'L') continue;  /* cover cuts are <= */
        int violated = 0;
        for (long mask = 0; mask < (1L << n); mask++) {
            double wsum = 0.0;
            for (int j = 0; j < n; j++)
                if (mask & (1L << j)) wsum += a[j];
            if (wsum > b + 1e-9) continue;  /* infeasible for the knapsack */

            double lhs = 0.0;
            for (int t = 0; t < cut->nnz; t++) {
                int v = cut->indices[t];
                if (v >= 0 && v < n && (mask & (1L << v))) lhs += cut->values[t];
            }
            if (lhs > cut->rhs + 1e-6) {
                if (!violated) {
                    invalid_cuts++;
                    printf("  INVALID CUT (feasible point violates it):\n");
                    printf("    knapsack:  ");
                    for (int j = 0; j < n; j++) printf("%g*x%d ", a[j], j);
                    printf("<= %g\n", b);
                    printf("    lp point:  ");
                    for (int j = 0; j < n; j++) printf("x%d=%.2f ", j, lp_x[j]);
                    printf("\n    cut:       ");
                    for (int t = 0; t < cut->nnz; t++)
                        printf("%g*x%d ", cut->values[t], cut->indices[t]);
                    printf("<= %g\n", cut->rhs);
                    printf("    violated by feasible x: ");
                    for (int j = 0; j < n; j++) printf("x%d=%d ", j, (mask >> j) & 1);
                    printf("(cut lhs=%g)\n", lhs);
                }
                violated = 1;
                break;
            }
        }
    }

    cut_pool_free(pool);
    mip_free(solver);
    lp_model_free(model);
    (void)verbose;
    return invalid_cuts;
}

int main(void) {
    printf("Ralph Lifted-Cover Cut Validity Harness\n");
    printf("=======================================\n");

    long instances = 0, with_cuts = 0, invalid = 0;
    /* Pre-fix, the bug reproduced within ~100 random instances, so this is far
       more than enough to catch a regression while staying fast for CI. */
    const long TRIALS = 50000;

    for (long t = 0; t < TRIALS; t++) {
        int n = rand_range(5, 8);
        double a[8];
        double sum = 0.0;
        for (int j = 0; j < n; j++) { a[j] = rand_range(1, 9); sum += a[j]; }
        /* capacity strictly below the total so covers exist, and above the max
           single weight so the row isn't trivially forcing */
        double maxw = 0; for (int j = 0; j < n; j++) if (a[j] > maxw) maxw = a[j];
        if (sum - 1 < maxw) continue;
        double b = rand_range((int)maxw, (int)sum - 1);

        double lp_x[8];
        for (int j = 0; j < n; j++) lp_x[j] = (lcg() % 1000) / 1000.0;

        instances++;
        int inv = check_instance(n, a, b, lp_x, 0);
        if (inv > 0) { invalid += inv; with_cuts++; }
        if (invalid >= 5) { printf("\n(stopping after 5 invalid cuts)\n"); break; }
    }

    printf("\n---------------------------------------\n");
    printf("Random instances tested: %ld\n", instances);
    printf("Invalid cuts found:      %ld\n", invalid);
    if (invalid == 0) {
        printf("RESULT: No invalid lifted-cover cut found. Finding NOT reproduced.\n");
        return 0;
    } else {
        printf("RESULT: BUG CONFIRMED -- lifted-cover cuts can be invalid.\n");
        return 1;
    }
}
