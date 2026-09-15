/*
 * test_cover_cuts_generated.c
 *
 * Cover cuts must actually be generated on a real model.
 *
 * test_lifted_cover_validity.c fuzzes the lifted-cover generator and proves
 * that the cuts it produces are valid. Nothing proved that any are produced.
 * That gap is recorded against M1 in docs/roadmaps/ralph.md, and it mattered:
 * the benchmark suite contained no knapsack-shaped row at all -- set covering,
 * set partitioning, assignment, network flow and facility location are none of
 * them knapsacks -- so cover cuts could have been silently inert on every
 * problem the project measured and no test would have noticed.
 *
 * A row is separable by generate_cover_cuts() when it has positive
 * coefficients, a positive right-hand side and binary variables. The
 * multidimensional knapsack below is built to that shape deliberately.
 *
 * The negative control matters as much as the positive one. Capacitated
 * facility location branches heavily and looks like a reasonable cover cut
 * target, but its capacity rows are `sum_j d_j x_ij - cap_i y_i <= 0`: a
 * negative coefficient against a zero right-hand side, which is not a
 * knapsack. Measured, it yields zero cover cuts at any node. Asserting that
 * keeps the first assertion honest -- if someone changes the generator to fire
 * on anything at all, this half fails.
 *
 * Build: make test_cover_cuts_generated
 * Run:   ./test_cover_cuts_generated
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "ralph_mip.h"
#include "ralph_lp.h"
#include "ralph_test_mod_api.h"

static int failures = 0;

static uint64_t rng = 1;
static int ri(int lo, int hi) {
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return lo + (int)((unsigned)(rng >> 33) % (unsigned)(hi - lo + 1));
}

/* Every row a genuine knapsack: positive coefficients, positive RHS, binaries. */
static RalphMIPModel *build_multiknapsack(int n, int rows, int seed) {
    RalphMIPModel *m = ralph_mip_create();
    int *idx = (int *)malloc(sizeof(int) * (size_t)n);
    double *val = (double *)malloc(sizeof(double) * (size_t)n);
    int i, j, k;

    if (!m || !idx || !val) { free(idx); free(val); return m; }

    rng = (uint64_t)(1234 + seed);
    for (j = 0; j < n; j++)
        ralph_lp_add_var(m, 0.0, 1.0, -(double)ri(1, 30), RALPH_LP_VAR_BINARY);

    for (i = 0; i < rows; i++) {
        double total = 0.0;
        k = 0;
        for (j = 0; j < n; j++) {
            idx[k] = j;
            val[k] = (double)ri(1, 20);
            total += val[k];
            k++;
        }
        ralph_lp_add_constraint(m, k, idx, val, RALPH_LP_SENSE_LESS_EQUAL, total * 0.4);
    }
    free(idx); free(val);
    return m;
}

/* Capacity rows carry a negative coefficient and a zero RHS: not knapsacks. */
static RalphMIPModel *build_cfl(int nf, int nc, int seed) {
    RalphMIPModel *m = ralph_mip_create();
    int *idx = (int *)malloc(sizeof(int) * (size_t)(nf * nc + nf));
    double *val = (double *)malloc(sizeof(double) * (size_t)(nf * nc + nf));
    int *d = (int *)malloc(sizeof(int) * (size_t)nc);
    int i, j, k;

    if (!m || !idx || !val || !d) { free(idx); free(val); free(d); return m; }

    rng = (uint64_t)(1234 + seed);
    for (i = 0; i < nf; i++)
        ralph_lp_add_var(m, 0.0, 1.0, (double)ri(20, 60), RALPH_LP_VAR_BINARY);
    for (i = 0; i < nf; i++)
        for (j = 0; j < nc; j++)
            ralph_lp_add_var(m, 0.0, 1.0, (double)ri(1, 20), RALPH_LP_VAR_BINARY);
    for (j = 0; j < nc; j++) d[j] = ri(5, 20);

    for (j = 0; j < nc; j++) {
        k = 0;
        for (i = 0; i < nf; i++) { idx[k] = nf + i * nc + j; val[k] = 1.0; k++; }
        ralph_lp_add_constraint(m, k, idx, val, RALPH_LP_SENSE_EQUAL, 1.0);
    }
    for (i = 0; i < nf; i++) {
        int cap = ri(40, 80);
        k = 0;
        for (j = 0; j < nc; j++) { idx[k] = nf + i * nc + j; val[k] = (double)d[j]; k++; }
        idx[k] = i; val[k] = -(double)cap; k++;
        ralph_lp_add_constraint(m, k, idx, val, RALPH_LP_SENSE_LESS_EQUAL, 0.0);
    }
    free(idx); free(val); free(d);
    return m;
}

int main(void) {
    long knap_total = 0;
    int seed;

    printf("=== Cover cut generation ===\n\n");

    printf("Multidimensional knapsack (rows ARE knapsacks -- expect cuts):\n");
    for (seed = 0; seed < 5; seed++) {
        RalphMIPModel *m = build_multiknapsack(48, 6, seed);
        int cuts;
        if (!m) { printf("FAIL: model creation\n"); failures++; continue; }
        ralph_mip_optimize(m);
        cuts = ralph_test_get_root_cover_cuts((const RalphModel *)m);
        knap_total += cuts;
        printf("  seed %d: %d root cover cuts\n", seed, cuts);
        ralph_mip_free(m);
    }
    if (knap_total == 0) {
        printf("FAIL: no cover cut was generated on any knapsack instance.\n");
        printf("      The generator is inert on the one shape it is built for.\n");
        failures++;
    }

    printf("\nCapacitated facility location (rows are NOT knapsacks -- expect none):\n");
    for (seed = 0; seed < 3; seed++) {
        RalphMIPModel *m = build_cfl(6, 14, seed);
        int cuts;
        if (!m) { printf("FAIL: model creation\n"); failures++; continue; }
        ralph_mip_optimize(m);
        cuts = ralph_test_get_root_cover_cuts((const RalphModel *)m);
        printf("  seed %d: %d root cover cuts\n", seed, cuts);
        if (cuts != 0) {
            printf("FAIL: cover cuts separated from a row that is not a knapsack\n");
            failures++;
        }
        ralph_mip_free(m);
    }

    printf("\n=== Summary ===\n");
    printf("Knapsack cover cuts generated: %ld\n", knap_total);
    if (failures == 0) {
        printf("All tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
