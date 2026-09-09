/*
 * test_presolve_differential.c
 *
 * Verify-first differential fuzzer for the presolve numerical-correctness
 * findings (dominated-column fixing, forcing-constraint pinning, fixed-var
 * tolerance, probing integer snap, ...). Presolve must never change the answer:
 * the optimal objective and the feasible/infeasible/unbounded verdict of a
 * model must be identical whether presolve is on or off.
 *
 * Method: generate many small random LPs and MIPs (with structure biased toward
 * what presolve reduces -- proportional columns, fixed variables, singleton
 * rows, tight bounds), solve each twice (presolve=0 and presolve=1), and flag
 * any mismatch in status or objective. A mismatch is a concrete counterexample.
 *
 * This guards the fix for artalis-io/otto#86: the base simplex (default path,
 * presolve off) used to report INFEASIBLE for feasible LPs with a nonzero lower
 * bound -- a <= row violated at the all-vars-at-lower-bounds starting point had
 * no artificial for phase 1 to drive out. With that fixed, presolve on and off
 * must agree. Uses FINITE bounds so the comparison is unambiguous (the
 * unbounded / 1e30-infinity-sentinel edge cases are a separate concern and are
 * excluded here). Part of `make -C ralph test`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "ralph_lp.h"

static unsigned long g_rng = 0xD1CE5EEDUL;
static unsigned int lcg(void) {
    g_rng = g_rng * 6364136223846793005UL + 1442695040888963407UL;
    return (unsigned int)(g_rng >> 33);
}
static int ri(int lo, int hi) { return lo + (int)(lcg() % (unsigned)(hi - lo + 1)); }
static double rd(int lo, int hi) { return (double)ri(lo, hi); }

#define MAXN 6
#define MAXM 5

typedef struct {
    int n, m;
    double lb[MAXN], ub[MAXN], obj[MAXN];
    char type[MAXN];               /* 'C' / 'I' / 'B' */
    double A[MAXM][MAXN];
    char sense[MAXM];              /* 'L' / 'E' / 'G' */
    double rhs[MAXM];
} Spec;

static void gen_spec(Spec *s) {
    s->n = ri(2, MAXN);
    s->m = ri(1, MAXM);
    for (int j = 0; j < s->n; j++) {
        int t = ri(0, 9);
        s->type[j] = (t < 7) ? 'C' : (t < 9 ? 'I' : 'B');
        /* Nonzero lower bounds (positive and negative) exercise the #86 path;
           finite upper bounds keep the on/off comparison unambiguous. */
        s->lb[j] = (ri(0, 2) == 0) ? rd(-2, 2) : 0.0;
        if (s->type[j] == 'B') { s->lb[j] = 0.0; s->ub[j] = 1.0; }
        else s->ub[j] = s->lb[j] + rd(1, 6);
        /* Occasionally fix a variable (lb == ub) to exercise fixed-var removal. */
        if (ri(0, 8) == 0) { s->ub[j] = s->lb[j]; s->type[j] = 'C'; }
        s->obj[j] = rd(-5, 5);
    }
    for (int i = 0; i < s->m; i++) {
        int nnz = ri(1, s->n);
        for (int j = 0; j < s->n; j++) s->A[i][j] = 0.0;
        /* place nnz random nonzeros */
        for (int c = 0; c < nnz; c++) {
            int j = ri(0, s->n - 1);
            double v = rd(-5, 5);
            if (fabs(v) < 0.5) v = 1.0;
            s->A[i][j] = v;
        }
        int se = ri(0, 2);
        s->sense[i] = se == 0 ? 'L' : (se == 1 ? 'E' : 'G');
        s->rhs[i] = rd(-8, 8);
    }
    /* With some probability, make two columns proportional (triggers the
       dominated/proportional-column reduction). */
    if (s->n >= 2 && ri(0, 2) == 0) {
        int k = ri(0, s->n - 1), j = ri(0, s->n - 1);
        if (j != k) {
            double ratio = rd(1, 3);
            for (int i = 0; i < s->m; i++) s->A[i][j] = ratio * s->A[i][k];
        }
    }
}

/* Build the model from the spec, solve with the given presolve flag, and report
   status + objective. Returns 1 on a clean solve, 0 if the build failed. */
static int solve_spec(const Spec *s, int presolve, RalphLPStatus *st, double *obj) {
    RalphLPModel *m = ralph_lp_create();
    if (!m) return 0;
    for (int j = 0; j < s->n; j++) {
        RalphLPVarType t = s->type[j] == 'I' ? RALPH_LP_VAR_INTEGER
                         : s->type[j] == 'B' ? RALPH_LP_VAR_BINARY
                                             : RALPH_LP_VAR_CONTINUOUS;
        ralph_lp_add_var(m, s->lb[j], s->ub[j], s->obj[j], t);
    }
    int idx[MAXN];
    double val[MAXN];
    for (int i = 0; i < s->m; i++) {
        int nnz = 0;
        for (int j = 0; j < s->n; j++)
            if (fabs(s->A[i][j]) > 1e-12) { idx[nnz] = j; val[nnz] = s->A[i][j]; nnz++; }
        RalphLPSense se = s->sense[i] == 'L' ? RALPH_LP_SENSE_LESS_EQUAL
                        : s->sense[i] == 'E' ? RALPH_LP_SENSE_EQUAL
                                             : RALPH_LP_SENSE_GREATER_EQUAL;
        if (nnz > 0) ralph_lp_add_constraint(m, nnz, idx, val, se, s->rhs[i]);
    }
    ralph_lp_set_int_param(m, "presolve", presolve);
    ralph_lp_optimize(m);
    *st = ralph_lp_get_status(m);
    *obj = ralph_lp_get_objval(m);
    ralph_lp_free(m);
    return 1;
}

/* Definitive verdicts we can compare. Everything else (limits, imprecise,
   error, inf-or-unbd) is inconclusive -> skip. */
static int category(RalphLPStatus s) {
    if (s == RALPH_LP_STATUS_OPTIMAL)    return 1;
    if (s == RALPH_LP_STATUS_INFEASIBLE) return 2;
    if (s == RALPH_LP_STATUS_UNBOUNDED)  return 3;
    return 0;  /* inconclusive */
}

static void print_spec(const Spec *s) {
    printf("    n=%d m=%d\n", s->n, s->m);
    for (int j = 0; j < s->n; j++)
        printf("    var x%d: type=%c lb=%g ub=%g obj=%g\n",
               j, s->type[j], s->lb[j], s->ub[j], s->obj[j]);
    for (int i = 0; i < s->m; i++) {
        printf("    row%d: ", i);
        for (int j = 0; j < s->n; j++)
            if (fabs(s->A[i][j]) > 1e-12) printf("%g*x%d ", s->A[i][j], j);
        printf("%c %g\n", s->sense[i], s->rhs[i]);
    }
}

int main(void) {
    printf("Ralph Presolve Differential Fuzzer (presolve on vs off)\n");
    printf("=======================================================\n");

    const long TRIALS = 100000;
    long compared = 0, mism = 0;

    for (long t = 0; t < TRIALS; t++) {
        Spec s;
        gen_spec(&s);

        RalphLPStatus st0, st1;
        double o0, o1;
        if (!solve_spec(&s, 0, &st0, &o0)) continue;
        if (!solve_spec(&s, 1, &st1, &o1)) continue;

        int c0 = category(st0), c1 = category(st1);
        if (c0 == 0 || c1 == 0) continue;  /* inconclusive on either side */
        compared++;

        int bad = 0;
        if (c0 != c1) {
            bad = 1;
        } else if (c0 == 1) {  /* both optimal: objectives must match */
            double tol = 1e-4 * (1.0 + fabs(o0) + fabs(o1));
            if (fabs(o0 - o1) > tol) bad = 1;
        }

        if (bad) {
            mism++;
            printf("\n  MISMATCH (presolve changes the answer):\n");
            printf("    presolve=off: status=%s obj=%g\n", ralph_lp_status_string(st0), o0);
            printf("    presolve=on : status=%s obj=%g\n", ralph_lp_status_string(st1), o1);
            print_spec(&s);
            if (mism >= 5) { printf("\n(stopping after 5 mismatches)\n"); break; }
        }
    }

    printf("\n-------------------------------------------------------\n");
    printf("Comparable instances: %ld\n", compared);
    printf("Mismatches:           %ld\n", mism);
    if (mism == 0) {
        printf("RESULT: presolve on/off agree. Findings NOT reproduced.\n");
        return 0;
    }
    printf("RESULT: BUG CONFIRMED -- presolve changes the optimum/verdict.\n");
    return 1;
}
