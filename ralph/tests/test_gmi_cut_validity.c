/*
 * Regression test: root Gomory cuts must not remove integer optima.
 *
 * Covers the defect recorded in docs/KNOWN_ISSUES.md. GMI cut generation
 * substituted every auxiliary variable out of the cut using
 *
 *     s = aux_coef * (b - A x)
 *
 * which holds only when the auxiliary is its row's only one. In non-dual mode
 * a '>=' row carries a surplus (coef -1) *and* an artificial (coef +1):
 *
 *     A x - surplus + artificial = b
 *
 * so the artificial is b - A x + surplus, not b - A x. Substituting it dropped
 * the surplus and injected a spurious linear term. The defect only surfaced
 * from the second cut round onward, because round 1 has no '>=' cut rows in
 * the tableau to supply such an artificial.
 *
 * The bad term is invisible at the LP point the cut is generated from -- every
 * nonbasic deviation is zero there, so the cut still reports
 * violation == f_0 -- which is why nothing downstream flagged it. It removed
 * integer optima elsewhere, and Ralph returned a suboptimal solution with
 * status OPTIMAL.
 *
 * The two instances below are capacitated facility location models whose
 * optima were verified independently with GLPK 5.0 (--mipgap 0). Before the
 * fix Ralph reported 154 and 174 for them.
 *
 * Build: make test_gmi_cut_validity
 * Run:   ./test_gmi_cut_validity
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ralph_mip.h"
#include "ralph_lp.h"

static int failures = 0;

static uint64_t rng = 1;
static int ri(int lo, int hi) {
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return lo + (int)((unsigned)(rng >> 33) % (unsigned)(hi - lo + 1));
}

/* Capacitated facility location: nf facilities, nc customers. */
static void build_cfl(RalphMIPModel *m, int nf, int nc) {
    int i, j, k;
    int *idx = (int *)malloc(sizeof(int) * (size_t)(nf * nc + nf));
    double *val = (double *)malloc(sizeof(double) * (size_t)(nf * nc + nf));
    int *d = (int *)malloc(sizeof(int) * (size_t)nc);

    if (!idx || !val || !d) { free(idx); free(val); free(d); return; }

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
}

/* seed selects the instance; expected_obj was proved optimal by GLPK. */
static void check_instance(int seed, double expected_obj) {
    RalphMIPModel *m = ralph_mip_create();
    int status;
    double obj;

    if (!m) { printf("FAIL: model creation\n"); failures++; return; }

    rng = (uint64_t)(1234 + seed);
    build_cfl(m, 6, 14);

    ralph_mip_optimize(m);
    status = (int)ralph_mip_get_status(m);
    obj = ralph_mip_get_objval(m);

    if (status != RALPH_LP_STATUS_OPTIMAL) {
        printf("FAIL: seed %d reported status %d, expected OPTIMAL\n", seed, status);
        failures++;
    } else if (obj > expected_obj + 1e-6) {
        printf("FAIL: seed %d returned %.4f but the optimum is %.4f -- a cut removed it\n",
               seed, obj, expected_obj);
        failures++;
    } else if (obj < expected_obj - 1e-6) {
        printf("FAIL: seed %d returned %.4f, below the true optimum %.4f -- infeasible solution\n",
               seed, obj, expected_obj);
        failures++;
    } else {
        printf("  seed %2d: %.4f (optimal)\n", seed, obj);
    }

    ralph_mip_free(m);
}

int main(void) {
    printf("=== GMI cut validity regression ===\n\n");
    printf("Capacitated facility location, optima verified with GLPK 5.0:\n");

    check_instance(1, 152.0);
    check_instance(26, 173.0);

    printf("\n=== Summary ===\n");
    if (failures == 0) {
        printf("All tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
