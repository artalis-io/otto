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
 * Model class: multidimensional knapsack, and the choice is load-bearing twice
 * over.
 *
 * A single-row knapsack is useless here -- Ralph closes those at the root, so
 * the branch-and-bound loop is never entered and both sides are identical runs.
 * Multi-row knapsacks branch on every instance below.
 *
 * Capacitated facility location is equally useless, for a less obvious reason.
 * It branches heavily, so it looks like the right choice, but its capacity rows
 * are `sum_j d_j x_ij - cap_i y_i <= 0` -- a negative coefficient and a
 * zero right-hand side, which is not a knapsack. generate_cover_cuts() finds
 * nothing in it, at the root or at a node: measured, 0 cover cuts against 2063
 * Gomory cuts over 40 instances. An earlier version of this test used CFL and
 * passed while exercising nothing at all.
 *
 * Hence the two guards at the bottom. Comparing nothing is a failure, and
 * comparing runs in which no node cut was ever generated is also a failure.
 *
 * Part of `make -C ralph test`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "ralph_mip.h"
#include "lp.h"
#include "mip.h"
#include "sh_pal.h"

MIPSolver* ralph_get_mip_solver(const RalphModel *model);

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
#define NVARS  48
#define NROWS   6
#define TRIALS 40

/*
 * max  sum_j p_j x_j        (expressed as a minimisation with negative costs)
 * s.t. sum_j a_ij x_j <= 0.4 * sum_j a_ij     for each row i
 *      x binary
 *
 * Every row is a genuine knapsack: positive coefficients, positive right-hand
 * side, binary variables. That is what generate_cover_cuts() needs.
 */
static void build_knapmulti(RalphMIPModel *m, long trial)
{
    int i, j, k;
    int idx[NVARS];
    double val[NVARS];

    g_rng = SEED_BASE + (uint64_t)trial;

    for (j = 0; j < NVARS; j++)
        ralph_lp_add_var(m, 0.0, 1.0, -(double)ri(1, 30), RALPH_LP_VAR_BINARY);

    for (i = 0; i < NROWS; i++) {
        double total = 0.0;
        k = 0;
        for (j = 0; j < NVARS; j++) {
            idx[k] = j;
            val[k] = (double)ri(1, 20);
            total += val[k];
            k++;
        }
        ralph_lp_add_constraint(m, k, idx, val, RALPH_LP_SENSE_LESS_EQUAL, total * 0.4);
    }
}

static int solve_trial(long trial, int node_cuts_on,
                       int *status, double *obj, long *cuts_generated)
{
    RalphMIPModel *m = ralph_mip_create();
    MIPSolver *s;

    if (!m) return 0;

    sh_pal_setenv("RALPH_ENABLE_NODE_CUTS", node_cuts_on ? "1" : "");
    build_knapmulti(m, trial);
    ralph_mip_optimize(m);

    s = ralph_get_mip_solver((const RalphModel *)m);
    *cuts_generated = s ? (long)s->node_cut_cuts_generated : 0;
    *status = (int)ralph_mip_get_status(m);
    *obj = ralph_mip_get_objval(m);

    ralph_mip_free(m);
    return 1;
}

int main(void)
{
    long compared = 0, mismatches = 0, total_node_cuts = 0;
    long t;

    printf("Ralph Node-Cut Differential (node cuts must not change the answer)\n");
    printf("=================================================================\n");

    for (t = 0; t < TRIALS; t++) {
        int st_off = 0, st_on = 0;
        double obj_off = 0.0, obj_on = 0.0;
        long gen_off = 0, gen_on = 0;

        if (!solve_trial(t, 0, &st_off, &obj_off, &gen_off)) continue;
        if (!solve_trial(t, 1, &st_on, &obj_on, &gen_on)) continue;

        total_node_cuts += gen_on;

        if (gen_off != 0) {
            printf("\n  BUG at trial %ld: %ld node cuts generated with the "
                   "feature off\n", t, gen_off);
            mismatches++;
        }

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
                printf("    on : obj=%.10g  (%ld node cuts generated)\n", obj_on, gen_on);
                if (mismatches >= 3) { printf("\n(stopping after 3)\n"); break; }
            }
        }
    }

    sh_pal_setenv("RALPH_ENABLE_NODE_CUTS", "");

    printf("\n-----------------------------------------------------------------\n");
    printf("Compared:            %ld\n", compared);
    printf("Node cuts generated: %ld\n", total_node_cuts);
    printf("Mismatches:          %ld\n", mismatches);

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
    /*
     * And a run in which no node cut was ever generated proves nothing either.
     * The second version of this file passed that way: it used capacitated
     * facility location, whose rows are not knapsacks, so the cover generator
     * returned nothing at every node and both sides were again identical.
     */
    if (total_node_cuts == 0) {
        printf("RESULT: node cuts never fired, so the two sides ran identical\n");
        printf("        code. Nothing was actually exercised. FAIL\n");
        return 1;
    }
    if (mismatches == 0) {
        printf("RESULT: node cuts preserve the optimum. PASS\n");
        return 0;
    }
    printf("RESULT: node cuts changed the answer -- a cut is not globally valid. FAIL\n");
    return 1;
}
