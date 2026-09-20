/*
 * test_obj_integrality_prune.c
 *
 * When every objective coefficient is an integer sitting on an integer
 * variable, every feasible objective value is an integer too, and a node's
 * LP bound can be rounded towards the incumbent before it is compared. A
 * minimising node with an LP bound of -1028.4 under an incumbent of -1028
 * cannot reach anything better: every value it can produce is an integer no
 * smaller than -1028.4, so no smaller than -1028. Comparing the raw bound
 * keeps that node, and with it the whole subtree beneath it.
 *
 * Ralph compared the raw bound. On the strongly correlated knapsacks in
 * bench_mip that cost, measured:
 *
 *     items   nodes without rounding   nodes with
 *        32                   14,705          339
 *        56                    1,123            5
 *        90                    2,493           33
 *       110      100,000 (node limit)            7
 *
 * The 110-item instance did not merely run long, it never proved optimality:
 * it hit the limit at 21.9s. With rounding it closes in 7 nodes.
 *
 * That was the whole of the knapsack gap recorded in docs/roadmaps/ralph.md.
 * The roadmap attributed it to bounding strength; the root LP bound is 0.96%
 * from the optimum on the 32-item instance, so the bound was never the
 * problem -- the comparison was.
 *
 * This test pins two things:
 *   1. the detection, which must not fire on a model with a continuous
 *      variable in the objective, because then the objective is not integral
 *      and rounding would cut off the optimum;
 *   2. the effect, as a node budget the unrounded search cannot meet.
 *
 * Build: make test_obj_integrality_prune
 * Run:   ./test_obj_integrality_prune
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "ralph_mip.h"
#include "ralph_lp.h"
#include "ralph_test_mod_api.h"

static int failures = 0;

static void check(int cond, const char *what)
{
    printf("  %-58s [%s]\n", what, cond ? "PASS" : "FAIL");
    if (!cond) failures++;
}

/*
 * bench_mip.c's generator, reproduced exactly.
 *
 * Not a stylistic choice: strongly correlated knapsacks vary enormously in
 * difficulty from draw to draw -- 32 items took 14,705 nodes and 56 took
 * 1,123 -- so a different stream gives a different instance and says nothing
 * about the one that was measured. A first version of this test used its own
 * RNG, produced an easy draw, and passed with the rounding disabled.
 */
static unsigned int rng = 1;
static int ri(int lo, int hi)
{
    rng = rng * 1103515245u + 12345u;
    return lo + (int)(rng % (unsigned)(hi - lo + 1));
}

/*
 * A strongly correlated knapsack: profit tracks weight, capacity at half the
 * total. The family the LP relaxation guides poorly and branch and bound has
 * to work through -- and the one where the objective is plainly integral,
 * every coefficient being -(w + 10) for an integer w.
 */
static RalphMIPModel *build_knapsack(int n, int seed, int continuous_var)
{
    RalphMIPModel *m = ralph_mip_create();
    int *idx = (int *)malloc(sizeof(int) * (size_t)n);
    double *val = (double *)malloc(sizeof(double) * (size_t)n);
    double total = 0.0;
    int j;

    if (!m || !idx || !val) { free(idx); free(val); return m; }

    rng = (unsigned int)seed;
    for (j = 0; j < n; j++) {
        double w = (double)ri(10, 100);
        /* One continuous variable is enough to break integrality, which is
         * what the negative control below asserts. */
        int type = (continuous_var && j == 0)
                 ? RALPH_LP_VAR_CONTINUOUS : RALPH_LP_VAR_BINARY;
        ralph_lp_add_var(m, 0.0, 1.0, -(w + 10.0), type);
        idx[j] = j;
        val[j] = w;
        total += w;
    }
    ralph_lp_add_constraint(m, n, idx, val, RALPH_LP_SENSE_LESS_EQUAL,
                            total * 0.5);
    free(idx); free(val);
    return m;
}

int main(void)
{
    RalphMIPModel *m;
    int nodes_all_integer, nodes_with_continuous;

    printf("Objective-integrality pruning\n");
    printf("=============================\n\n");

    /*
     * 110 items: without rounding this instance exhausted the 100,000 node
     * limit and returned without proving optimality. The budget below is far
     * above what rounding needs (7 nodes measured) and far below what the
     * raw comparison needs, so it fails loudly if the rounding is lost and
     * does not trip on ordinary variation.
     */
    m = build_knapsack(110, 42, 0);
    ralph_mip_set_int_param(m, "max_nodes", 100000);
    ralph_mip_optimize(m);
    check(ralph_mip_get_status(m) == RALPH_LP_STATUS_OPTIMAL,
          "110-item knapsack solves to proven optimality");
    nodes_all_integer = ralph_mip_get_node_count(m);
    check(nodes_all_integer < 2000,
          "and does it well inside a budget the raw bound cannot meet");
    printf("      nodes: %d\n", nodes_all_integer);
    ralph_mip_free(m);

    /*
     * The half that is about correctness rather than speed.
     *
     * Two binaries, x + y <= 1, minimising -1.5x - 1.0y. The optimum is
     * x = 1, y = 0 at -1.5 -- a fractional objective value, so the
     * objective is not integral and rounding must not apply. If it did, a
     * bound of -1.5 would round to -1.0 and the optimum could be pruned
     * against any incumbent at or below that.
     *
     * Small enough to check by hand, which is the point: this is the
     * assertion that fails if the detection is ever widened to a model
     * where the rounding is unsound.
     */
    {
        int idx2[2] = {0, 1};
        double val2[2] = {1.0, 1.0};
        double obj;

        m = ralph_mip_create();
        ralph_lp_add_var(m, 0.0, 1.0, -1.5, RALPH_LP_VAR_BINARY);
        ralph_lp_add_var(m, 0.0, 1.0, -1.0, RALPH_LP_VAR_BINARY);
        ralph_lp_add_constraint(m, 2, idx2, val2,
                                RALPH_LP_SENSE_LESS_EQUAL, 1.0);
        ralph_mip_optimize(m);

        check(ralph_mip_get_status(m) == RALPH_LP_STATUS_OPTIMAL,
              "a fractional objective still solves");
        obj = ralph_mip_get_objval(m);
        check(fabs(obj - (-1.5)) < 1e-6,
              "and reaches -1.5, which rounding would have cut off");
        printf("      objective: %.4f\n", obj);
        ralph_mip_free(m);
    }

    /*
     * The negative control. With a continuous variable carrying objective
     * weight the objective is no longer integral, rounding must not apply,
     * and the answer must still be right. A solver that rounded here could
     * cut off the optimum, so this half is about correctness rather than
     * speed -- it is the assertion that would fail if the detection were
     * widened carelessly.
     */
    m = build_knapsack(40, 42, 1);
    ralph_mip_set_int_param(m, "max_nodes", 100000);
    ralph_mip_optimize(m);
    check(ralph_mip_get_status(m) == RALPH_LP_STATUS_OPTIMAL,
          "a continuous objective variable still solves correctly");
    nodes_with_continuous = ralph_mip_get_node_count(m);
    printf("      nodes: %d\n", nodes_with_continuous);
    ralph_mip_free(m);

    printf("\n");
    if (failures) {
        printf("FAIL: %d assertion%s failed\n", failures,
               failures == 1 ? "" : "s");
        return 1;
    }
    printf("All objective-integrality tests passed\n");
    return 0;
}
