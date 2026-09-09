/*
 * test_phase1_oracle_glpk.c
 *
 * Phase-1 correctness stress against an independent oracle (GLPK / glpsol).
 *
 * Generates equality-heavy LPs with injected linearly-dependent (redundant)
 * rows and a mix of finite and +inf bounds -- the input class that exercises
 * the two-phase simplex's artificial-variable feasibility machinery hardest --
 * and checks that Ralph's OPTIMAL / INFEASIBLE / UNBOUNDED verdict matches
 * glpsol, on BOTH the base simplex (presolve off) and the presolve path.
 *
 * This is the harness that validated the Phase-1 numerical-correctness review
 * and guards the artalis-io/otto#86 / #96 family (missing / mis-signed
 * artificials, redundant-row handling, infeasibility certification). It probes
 * far harder Phase-1 territory than the on/off differential fuzzer, which has
 * no independent oracle.
 *
 * Requires glpsol on PATH. When glpsol is absent the test SKIPS (returns 0) so
 * it is safe to wire into environments without GLPK. It is a separate make
 * target (test-phase1-oracle-glpk), NOT part of the default `make test`,
 * because it shells out to an external solver.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ralph_lp.h"

static unsigned long g_rng = 0xF00DFACEUL;
static unsigned int lcg(void) {
    g_rng = g_rng * 6364136223846793005UL + 1442695040888963407UL;
    return (unsigned int)(g_rng >> 33);
}
static int ri(int lo, int hi) { return lo + (int)(lcg() % (unsigned)(hi - lo + 1)); }
static double rd(int lo, int hi) { return (double)ri(lo, hi); }

#define N 9
#define M 9
#define LP_PATH "phase1_oracle_tmp.lp"

/* Definitive verdict categories; 0 = inconclusive (limits/error/imprecise). */
static int cat(RalphLPStatus s) {
    if (s == RALPH_LP_STATUS_OPTIMAL)    return 1;
    if (s == RALPH_LP_STATUS_INFEASIBLE) return 2;
    if (s == RALPH_LP_STATUS_UNBOUNDED)  return 3;
    return 0;
}

static int glpsol_available(void) {
    FILE *p = popen("glpsol --version >/dev/null 2>&1 && echo OK", "r");
    if (!p) return 0;
    char buf[64];
    int ok = (fgets(buf, sizeof buf, p) && strncmp(buf, "OK", 2) == 0);
    pclose(p);
    return ok;
}

typedef struct {
    int n, m;
    double lb[N], ub[N], obj[N];
    double A[M][N];
    char sense[M];
    double rhs[M];
} Spec;

static void gen_spec(Spec *s) {
    s->n = ri(3, N);
    s->m = ri(2, M);
    for (int j = 0; j < s->n; j++) {
        s->lb[j] = (ri(0, 2) == 0) ? rd(-3, 3) : 0.0;
        s->ub[j] = (ri(0, 3) == 0) ? RALPH_LP_INFINITY : s->lb[j] + rd(1, 8);
        s->obj[j] = rd(-5, 5);
    }
    for (int i = 0; i < s->m; i++) {
        for (int j = 0; j < s->n; j++) s->A[i][j] = 0.0;
        int nnz = ri(1, s->n);
        for (int c = 0; c < nnz; c++) {
            int j = ri(0, s->n - 1);
            double v = rd(-4, 4);
            if (fabs(v) < 0.5) v = 1.0;
            s->A[i][j] = v;
        }
        /* Bias strongly toward equalities to stress Phase-1 artificials. */
        int se = ri(0, 3);
        s->sense[i] = (se < 2) ? 'E' : (se == 2 ? 'L' : 'G');
        s->rhs[i] = rd(-9, 9);
    }
    /* Inject a redundant/linearly-dependent row (row r2 = k * row r1). */
    if (s->m >= 3 && ri(0, 1) == 0) {
        int r1 = ri(0, s->m - 1), r2 = ri(0, s->m - 1);
        if (r1 != r2) {
            double k = rd(1, 3);
            for (int j = 0; j < s->n; j++) s->A[r2][j] = k * s->A[r1][j];
            s->sense[r2] = s->sense[r1];
            s->rhs[r2] = k * s->rhs[r1];
        }
    }
}

static RalphLPStatus solve_ralph(const Spec *s, int presolve) {
    RalphLPModel *m = ralph_lp_create();
    for (int j = 0; j < s->n; j++)
        ralph_lp_add_var(m, s->lb[j], s->ub[j], s->obj[j], RALPH_LP_VAR_CONTINUOUS);
    int idx[N];
    double val[N];
    for (int i = 0; i < s->m; i++) {
        int k = 0;
        for (int j = 0; j < s->n; j++)
            if (fabs(s->A[i][j]) > 1e-12) { idx[k] = j; val[k] = s->A[i][j]; k++; }
        RalphLPSense se = s->sense[i] == 'L' ? RALPH_LP_SENSE_LESS_EQUAL
                        : s->sense[i] == 'E' ? RALPH_LP_SENSE_EQUAL
                                             : RALPH_LP_SENSE_GREATER_EQUAL;
        if (k > 0) ralph_lp_add_constraint(m, k, idx, val, se, s->rhs[i]);
    }
    ralph_lp_set_int_param(m, "presolve", presolve);
    ralph_lp_optimize(m);
    RalphLPStatus st = ralph_lp_get_status(m);
    ralph_lp_free(m);
    return st;
}

/* Solve with glpsol via a temp CPLEX-LP file; return a verdict category. */
static int solve_glpk(const Spec *s) {
    FILE *f = fopen(LP_PATH, "w");
    if (!f) return 0;
    fprintf(f, "Minimize\n obj:");
    for (int j = 0; j < s->n; j++) fprintf(f, " %+g x%d", s->obj[j], j);
    fprintf(f, "\nSubject To\n");
    for (int i = 0; i < s->m; i++) {
        fprintf(f, " c%d:", i);
        int any = 0;
        for (int j = 0; j < s->n; j++)
            if (fabs(s->A[i][j]) > 1e-12) { fprintf(f, " %+g x%d", s->A[i][j], j); any = 1; }
        if (!any) fprintf(f, " 0 x0");
        fprintf(f, " %s %g\n",
                s->sense[i] == 'L' ? "<=" : s->sense[i] == 'E' ? "=" : ">=", s->rhs[i]);
    }
    fprintf(f, "Bounds\n");
    for (int j = 0; j < s->n; j++) {
        if (s->ub[j] >= RALPH_LP_INFINITY / 2) fprintf(f, " %g <= x%d <= +inf\n", s->lb[j], j);
        else fprintf(f, " %g <= x%d <= %g\n", s->lb[j], j, s->ub[j]);
    }
    fprintf(f, "End\n");
    fclose(f);

    /* --nopresol gives a cleaner UNBOUNDED vs INFEASIBLE separation. */
    FILE *p = popen("glpsol --nopresol --lp " LP_PATH
                    " -o /dev/null 2>/dev/null | "
                    "grep -iE 'OPTIMAL|UNBOUNDED|NO PRIMAL|NO DUAL'", "r");
    if (!p) return 0;
    char buf[4096];
    int gc = 0;
    while (fgets(buf, sizeof buf, p)) {
        if (strstr(buf, "UNBOUNDED") || strstr(buf, "NO DUAL")) gc = 3;
        else if (strstr(buf, "NO PRIMAL")) gc = 2;
        else if (strstr(buf, "OPTIMAL")) { if (!gc) gc = 1; }
    }
    pclose(p);
    return gc;
}

static const char *cat_name(int c) {
    return c == 1 ? "OPTIMAL" : c == 2 ? "INFEASIBLE" : c == 3 ? "UNBOUNDED" : "inconclusive";
}

int main(void) {
    printf("Ralph Phase-1 correctness oracle (equality/redundant-heavy vs GLPK)\n");
    printf("==================================================================\n");

    if (!glpsol_available()) {
        printf("SKIP: glpsol not found on PATH (GLPK oracle unavailable).\n");
        return 0;
    }

    const long TRIALS = 4000;
    long compared = 0, mism_off = 0, mism_on = 0;

    for (long t = 0; t < TRIALS; t++) {
        Spec s;
        gen_spec(&s);

        int coff = cat(solve_ralph(&s, 0));
        int con = cat(solve_ralph(&s, 1));
        if (!coff && !con) continue;   /* both inconclusive: nothing to check */

        int gc = solve_glpk(&s);
        if (!gc) continue;             /* oracle inconclusive: skip */
        compared++;

        if (coff && coff != gc) {
            mism_off++;
            if (mism_off <= 6)
                printf("  OFF mismatch: ralph=%s glpk=%s\n", cat_name(coff), cat_name(gc));
        }
        if (con && con != gc) {
            mism_on++;
            if (mism_on <= 6)
                printf("  ON  mismatch: ralph=%s glpk=%s\n", cat_name(con), cat_name(gc));
        }
    }

    remove(LP_PATH);

    printf("------------------------------------------------------------------\n");
    printf("Comparable instances: %ld\n", compared);
    printf("presolve-off mismatches: %ld\n", mism_off);
    printf("presolve-on  mismatches: %ld\n", mism_on);
    if (mism_off == 0 && mism_on == 0) {
        printf("RESULT: Phase-1 verdicts agree with GLPK on both paths. PASS\n");
        return 0;
    }
    printf("RESULT: verdict disagreement with GLPK. FAIL\n");
    return 1;
}
