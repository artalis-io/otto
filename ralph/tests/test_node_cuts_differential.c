/*
 * test_node_cuts_differential.c
 *
 * Node-level cuts must not change the answer.
 *
 * M2 generates cuts at branch-and-bound nodes rather than only at the root.
 * The hazard is specific and silent: apply_cuts() appends rows to the working
 * model permanently, so a cut applied at a node is in force for every node
 * solved afterwards, including nodes in unrelated subtrees. A cut whose
 * validity depended on that node's local branching bounds would cut the true
 * optimum out of a sibling, and the solver would return a confidently wrong
 * number with nothing in the output to suggest it.
 *
 * generate_node_cuts() therefore runs only cover cuts, which are built from the
 * original rows and original right-hand sides and are valid everywhere. This
 * test is what keeps that true: if anyone adds a node-tableau family (Gomory,
 * MIR) without building cut scoping first, the objectives below start
 * disagreeing.
 *
 * Why capacitated facility location rather than knapsacks. The obvious
 * generator here is a random knapsack, and it is useless: Ralph closes those at
 * the root, so the branch-and-bound loop is never entered and the test compares
 * two identical runs. Measured while writing this -- binary knapsacks from 8 to
 * 60 variables, including strongly correlated ones, produced zero calls to
 * process_node(). CFL branches (hundreds of nodes on a 6x14 instance) and its
 * capacity rows are knapsack-shaped, so cover cuts have something to find. That
 * combination is what makes this test exercise anything at all.
 *
 * Part of `make -C ralph test`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "ralph_mip.h"
#include "sh_pal.h"

/*
 * uint64_t, not unsigned long: on Windows unsigned long is 32 bits, so the
 * `>> 33` this generator depends on is undefined there and the sequence
 * collapses to a constant.
 */
static uint64_t g_rng = 0;
static unsigned int lcg(void) {
    g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (unsigned int)(g_rng >> 33);
}
static int ri(int lo, int hi) { return lo + (int)(lcg() % (unsigned)(hi - lo + 1)); }

#define SEED_BASE 0xC0FFEE11ULL

#define MAX_FAC 7
#define MAX_CUST 14
#define MAX_VARS (MAX_FAC + MAX_FAC * MAX_CUST)
#define TRIALS 40

typedef struct {
    int nf, nc;
    double open_cost[MAX_FAC];
    double assign_cost[MAX_FAC][MAX_CUST];
    int demand[MAX_CUST];
    int cap[MAX_FAC];
} Spec;

static void gen_spec(Spec *s, long trial)
{
    int i, j;

    g_rng = SEED_BASE + (uint64_t)trial;
    s->nf = ri(4, MAX_FAC);
    s->nc = ri(9, MAX_CUST);

    for (i = 0; i < s->nf; i++) {
        s->open_cost[i] = (double)ri(20, 60);
        s->cap[i] = ri(40, 80);
        for (j = 0; j < MAX_CUST; j++) {
            s->assign_cost[i][j] = (double)ri(1, 20);
        }
    }
    for (j = 0; j < s->nc; j++) s->demand[j] = ri(5, 20);
}

/*
 * min  sum_i f_i y_i + sum_ij c_ij x_ij
 * s.t. sum_i x_ij = 1                    for each customer j
 *      sum_j d_j x_ij - cap_i y_i <= 0   for each facility i  (knapsack-shaped)
 *      x, y binary
 *
 * Variable layout: y_i at i, x_ij at nf + i*nc + j.
 */
static int solve_spec(const Spec *s, int node_cuts, int *status, double *obj)
{
    RalphMIPModel *m;
    int idx[MAX_VARS];
    double val[MAX_VARS];
    int i, j, k;

    /* Read when the MIP solver is created, so it must be set before
     * ralph_mip_optimize(), not merely before the process starts. */
    sh_pal_setenv("RALPH_ENABLE_NODE_CUTS", node_cuts ? "1" : "");

    m = ralph_mip_create();
    if (!m) return 0;

    for (i = 0; i < s->nf; i++) {
        ralph_lp_add_var(m, 0.0, 1.0, s->open_cost[i], RALPH_LP_VAR_BINARY);
    }
    for (i = 0; i < s->nf; i++) {
        for (j = 0; j < s->nc; j++) {
            ralph_lp_add_var(m, 0.0, 1.0, s->assign_cost[i][j], RALPH_LP_VAR_BINARY);
        }
    }

    for (j = 0; j < s->nc; j++) {
        k = 0;
        for (i = 0; i < s->nf; i++) {
            idx[k] = s->nf + i * s->nc + j;
            val[k] = 1.0;
            k++;
        }
        ralph_lp_add_constraint(m, k, idx, val, RALPH_LP_SENSE_EQUAL, 1.0);
    }
    for (i = 0; i < s->nf; i++) {
        k = 0;
        for (j = 0; j < s->nc; j++) {
            idx[k] = s->nf + i * s->nc + j;
            val[k] = (double)s->demand[j];
            k++;
        }
        idx[k] = i;
        val[k] = -(double)s->cap[i];
        k++;
        ralph_lp_add_constraint(m, k, idx, val, RALPH_LP_SENSE_LESS_EQUAL, 0.0);
    }

    ralph_mip_optimize(m);
    *status = (int)ralph_mip_get_status(m);
    *obj = ralph_mip_get_objval(m);
    ralph_mip_free(m);
    return 1;
}

static void print_spec(const Spec *s)
{
    int i, j;
    printf("    facilities=%d customers=%d\n", s->nf, s->nc);
    for (i = 0; i < s->nf; i++) {
        printf("    f%-2d open=%g cap=%d assign:", i, s->open_cost[i], s->cap[i]);
        for (j = 0; j < s->nc; j++) printf(" %g", s->assign_cost[i][j]);
        printf("\n");
    }
    printf("    demand:");
    for (j = 0; j < s->nc; j++) printf(" %d", s->demand[j]);
    printf("\n");
}

int main(void)
{
    long compared = 0, mismatches = 0;
    long t;

    printf("Ralph Node-Cut Differential (node cuts must not change the answer)\n");
    printf("=================================================================\n");

    for (t = 0; t < TRIALS; t++) {
        Spec s;
        int st_off = 0, st_on = 0;
        double obj_off = 0.0, obj_on = 0.0;

        gen_spec(&s, t);
        if (!solve_spec(&s, 0, &st_off, &obj_off)) continue;
        if (!solve_spec(&s, 1, &st_on, &obj_on)) continue;

        /* Only compare definitive verdicts; a limit or an error on either side
         * says nothing about cut validity. */
        if (st_off != (int)RALPH_LP_STATUS_OPTIMAL ||
            st_on != (int)RALPH_LP_STATUS_OPTIMAL) {
            continue;
        }
        compared++;

        {
            double tol = 1e-6 * (1.0 + fabs(obj_off) + fabs(obj_on));
            if (fabs(obj_off - obj_on) > tol) {
                mismatches++;
                printf("\n  MISMATCH at trial %ld: node cuts changed the optimum\n", t);
                printf("    off: obj=%.10g\n", obj_off);
                printf("    on : obj=%.10g\n", obj_on);
                print_spec(&s);
                if (mismatches >= 3) { printf("\n(stopping after 3)\n"); break; }
            }
        }
    }

    sh_pal_setenv("RALPH_ENABLE_NODE_CUTS", "");

    printf("\n-----------------------------------------------------------------\n");
    printf("Compared:   %ld\n", compared);
    printf("Mismatches: %ld\n", mismatches);

    /*
     * A run that compared nothing proves nothing. The first version of this
     * file passed exactly that way: every model was solved at the root, so both
     * sides were identical runs of code that never diverged.
     */
    if (compared == 0) {
        printf("RESULT: no model produced a definitive optimum on both sides.\n");
        printf("        Nothing was actually compared. FAIL\n");
        return 1;
    }
    if (mismatches == 0) {
        printf("RESULT: node cuts preserve the optimum. PASS\n");
        return 0;
    }
    printf("RESULT: node cuts changed the answer -- a cut is not globally valid. FAIL\n");
    return 1;
}
