/*
 * Which model shape actually makes Ralph branch? Node cuts cannot be tested
 * until something reaches process_node().
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "ralph_mip.h"
#include "sh_pal.h"

static uint64_t g = 1;
static int ri(int lo, int hi) {
    g = g * 6364136223846793005ULL + 1442695040888963407ULL;
    return lo + (int)((unsigned)(g >> 33) % (unsigned)(hi - lo + 1));
}

/* Capacitated facility location:
 *   min  sum_i f_i y_i + sum_ij c_ij x_ij
 *   s.t. sum_i x_ij = 1              for each customer j
 *        sum_j d_j x_ij <= cap_i y_i for each facility i   (knapsack-shaped)
 *        x, y binary
 */
static void build_cfl(RalphMIPModel *m, int nf, int nc, unsigned seed)
{
    int i, j, k;
    int *idx = malloc(sizeof(int) * (nf * nc + nf));
    double *val = malloc(sizeof(double) * (nf * nc + nf));
    int *d = malloc(sizeof(int) * nc);

    g = seed;
    /* y_i : 0 .. nf-1 */
    for (i = 0; i < nf; i++) ralph_lp_add_var(m, 0.0, 1.0, (double)ri(20, 60), RALPH_LP_VAR_BINARY);
    /* x_ij : nf + i*nc + j */
    for (i = 0; i < nf; i++)
        for (j = 0; j < nc; j++)
            ralph_lp_add_var(m, 0.0, 1.0, (double)ri(1, 20), RALPH_LP_VAR_BINARY);

    for (j = 0; j < nc; j++) d[j] = ri(5, 20);

    /* each customer served exactly once */
    for (j = 0; j < nc; j++) {
        k = 0;
        for (i = 0; i < nf; i++) { idx[k] = nf + i * nc + j; val[k] = 1.0; k++; }
        ralph_lp_add_constraint(m, k, idx, val, RALPH_LP_SENSE_EQUAL, 1.0);
    }
    /* capacity: sum_j d_j x_ij - cap_i y_i <= 0 */
    for (i = 0; i < nf; i++) {
        int cap = ri(40, 80);
        k = 0;
        for (j = 0; j < nc; j++) { idx[k] = nf + i * nc + j; val[k] = (double)d[j]; k++; }
        idx[k] = i; val[k] = -(double)cap; k++;
        ralph_lp_add_constraint(m, k, idx, val, RALPH_LP_SENSE_LESS_EQUAL, 0.0);
    }
    free(idx); free(val); free(d);
}

int main(int argc, char **argv)
{
    int nf = (argc > 1) ? atoi(argv[1]) : 5;
    int nc = (argc > 2) ? atoi(argv[2]) : 12;
    int trials = (argc > 3) ? atoi(argv[3]) : 5;
    int t;

    setvbuf(stdout, NULL, _IONBF, 0);
    sh_pal_setenv("RALPH_ENABLE_NODE_CUTS", "1");

    for (t = 0; t < trials; t++) {
        RalphMIPModel *m = ralph_mip_create();
        if (!m) return 1;
        build_cfl(m, nf, nc, (unsigned)(1234 + t));
        ralph_mip_optimize(m);
        printf("trial %d: status=%d obj=%.4f nodes=%d\n",
               t, (int)ralph_mip_get_status(m), ralph_mip_get_objval(m),
               ralph_mip_get_node_count(m));
        ralph_mip_free(m);
    }
    return 0;
}
