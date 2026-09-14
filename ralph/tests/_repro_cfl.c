/*
 * Does the corruption need capacitated facility location, or does any model
 * that branches enough do it? Same loop, three generators.
 *
 *   probe_multi <kind> <size> <trials>
 *     kind: cfl | setcover | knapmulti
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ralph_mip.h"

static uint64_t g = 1;
static int ri(int lo, int hi) {
    g = g * 6364136223846793005ULL + 1442695040888963407ULL;
    return lo + (int)((unsigned)(g >> 33) % (unsigned)(hi - lo + 1));
}

/* Capacitated facility location: nf facilities, nc customers. */
static void build_cfl(RalphMIPModel *m, int nf, int nc)
{
    int i, j, k;
    int *idx = (int *)malloc(sizeof(int) * (size_t)(nf * nc + nf));
    double *val = (double *)malloc(sizeof(double) * (size_t)(nf * nc + nf));
    int *d = (int *)malloc(sizeof(int) * (size_t)nc);

    for (i = 0; i < nf; i++) ralph_lp_add_var(m, 0.0, 1.0, (double)ri(20, 60), RALPH_LP_VAR_BINARY);
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

/* Set covering: n sets, m elements, each element covered by a random subset. */
static void build_setcover(RalphMIPModel *m, int nsets, int nelem)
{
    int i, j, k;
    int *idx = (int *)malloc(sizeof(int) * (size_t)nsets);
    double *val = (double *)malloc(sizeof(double) * (size_t)nsets);

    for (j = 0; j < nsets; j++) ralph_lp_add_var(m, 0.0, 1.0, (double)ri(1, 20), RALPH_LP_VAR_BINARY);
    for (i = 0; i < nelem; i++) {
        k = 0;
        for (j = 0; j < nsets; j++) {
            if (ri(0, 3) == 0) { idx[k] = j; val[k] = 1.0; k++; }
        }
        if (k == 0) { idx[0] = ri(0, nsets - 1); val[0] = 1.0; k = 1; }
        ralph_lp_add_constraint(m, k, idx, val, RALPH_LP_SENSE_GREATER_EQUAL, 1.0);
    }
    free(idx); free(val);
}

/* Multidimensional knapsack: n binaries, m capacity rows. */
static void build_knapmulti(RalphMIPModel *m, int n, int nrows)
{
    int i, j, k;
    int *idx = (int *)malloc(sizeof(int) * (size_t)n);
    double *val = (double *)malloc(sizeof(double) * (size_t)n);

    for (j = 0; j < n; j++) ralph_lp_add_var(m, 0.0, 1.0, -(double)ri(1, 30), RALPH_LP_VAR_BINARY);
    for (i = 0; i < nrows; i++) {
        double total = 0.0;
        k = 0;
        for (j = 0; j < n; j++) { idx[k] = j; val[k] = (double)ri(1, 20); total += val[k]; k++; }
        ralph_lp_add_constraint(m, k, idx, val, RALPH_LP_SENSE_LESS_EQUAL, total * 0.4);
    }
    free(idx); free(val);
}

int main(int argc, char **argv)
{
    const char *kind = (argc > 1) ? argv[1] : "cfl";
    int size = (argc > 2) ? atoi(argv[2]) : 6;
    int trials = (argc > 3) ? atoi(argv[3]) : 40;
    int t;
    long total_nodes = 0;

    setvbuf(stdout, NULL, _IONBF, 0);

    {
        const char *only = getenv("PROBE_ONLY");
        if (only) { t = atoi(only); trials = t + 1; } else { t = 0; }
    }
    for (; t < trials; t++) {
        RalphMIPModel *m = ralph_mip_create();
        if (!m) return 1;
        g = (uint64_t)(1234 + t);

        if (strcmp(kind, "cfl") == 0)            build_cfl(m, size, size * 2 + 2);
        else if (strcmp(kind, "setcover") == 0)  build_setcover(m, size * 8, size * 4);
        else                                     build_knapmulti(m, size * 8, size);

        if (getenv("PROBE_NO_DETECT")) {
            ralph_mip_set_int_param(m, "detect_special", 0);
        }
        ralph_mip_optimize(m);
        total_nodes += ralph_mip_get_node_count(m);
        printf("%s trial %d: status=%d obj=%.4f nodes=%d\n", kind, t,
               (int)ralph_mip_get_status(m), ralph_mip_get_objval(m),
               ralph_mip_get_node_count(m));
        ralph_mip_free(m);
    }
    printf("%s: %d trials, %ld total nodes, completed\n", kind, trials, total_nodes);
    return 0;
}
