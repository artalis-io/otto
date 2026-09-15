/*
 * Regression tests for the B&B node pool.
 *
 * These cover the capacitated-facility-location crash recorded in
 * docs/KNOWN_ISSUES.md: once a tree outgrows the pool, bb_node_pool_get()
 * hands back standalone bb_node_create() nodes, and those come back through
 * bb_node_pool_return(). The old membership test formed `node - pool->nodes`,
 * a pointer difference between unrelated objects, so at -O3 the compiler was
 * free to assume the node really was in the pool and drop the `offset < 0`
 * half of the range check. A standalone node then pushed a garbage index onto
 * free_list, and a later get() returned &pool->nodes[garbage].
 *
 * Build: make test_bb_node_pool
 * Run:   ./test_bb_node_pool
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lp.h"
#include <stdint.h>
#include "mip.h"
#include "ralph_mip.h"
#include "ralph_lp.h"

#define POOL_CAP  8
#define NUM_VARS  5
#define EXTRA     6   /* standalone nodes requested past the pool's capacity */

static int failures = 0;

static void check(int cond, const char *what) {
    if (!cond) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

/*
 * Exhaust the pool, then return every node -- pooled and standalone alike --
 * and assert the pool's accounting survives it.
 */
static void test_standalone_return_keeps_free_list_sane(void) {
    BBNodePool *pool = bb_node_pool_create(POOL_CAP, NUM_VARS);
    BBNode *nodes[POOL_CAP + EXTRA];
    int i;

    printf("Test: returning standalone nodes keeps free_list sane\n");
    check(pool != NULL, "pool created");
    if (!pool) return;

    for (i = 0; i < POOL_CAP + EXTRA; i++) {
        nodes[i] = bb_node_pool_get(pool);
        check(nodes[i] != NULL, "node allocated");
    }
    check(pool->free_count == 0, "pool is exhausted after capacity gets");

    /* The last EXTRA nodes came from bb_node_create(), not the pool. */
    for (i = POOL_CAP; i < POOL_CAP + EXTRA; i++) {
        uintptr_t base = (uintptr_t)pool->nodes;
        uintptr_t addr = (uintptr_t)nodes[i];
        check(addr < base || addr >= base + (uintptr_t)POOL_CAP * sizeof(BBNode),
              "overflow node is outside the pool block");
    }

    for (i = 0; i < POOL_CAP + EXTRA; i++) {
        bb_node_pool_return(pool, nodes[i]);
    }

    /* Only the POOL_CAP pooled nodes may come back to the free list. */
    check(pool->free_count == POOL_CAP,
          "free_count returns to capacity, not beyond it");
    for (i = 0; i < pool->free_count; i++) {
        check(pool->free_list[i] >= 0 && pool->free_list[i] < pool->capacity,
              "every free_list entry is a valid node index");
    }

    bb_node_pool_free(pool);
}

/*
 * After the round trip above, the pool must still hand out distinct, in-range
 * nodes. Before the fix a garbage index produced a wild pointer here.
 */
static void test_reuse_after_standalone_return(void) {
    BBNodePool *pool = bb_node_pool_create(POOL_CAP, NUM_VARS);
    BBNode *nodes[POOL_CAP + EXTRA];
    BBNode *again[POOL_CAP];
    int i, j;

    printf("Test: pool still hands out valid nodes afterwards\n");
    if (!pool) { check(0, "pool created"); return; }

    for (i = 0; i < POOL_CAP + EXTRA; i++) nodes[i] = bb_node_pool_get(pool);
    for (i = 0; i < POOL_CAP + EXTRA; i++) bb_node_pool_return(pool, nodes[i]);

    for (i = 0; i < POOL_CAP; i++) {
        uintptr_t base = (uintptr_t)pool->nodes;
        uintptr_t addr;

        again[i] = bb_node_pool_get(pool);
        check(again[i] != NULL, "node re-allocated from pool");
        if (!again[i]) continue;

        addr = (uintptr_t)again[i];
        check(addr >= base && addr < base + (uintptr_t)POOL_CAP * sizeof(BBNode),
              "re-allocated node lies inside the pool block");
        check(((addr - base) % sizeof(BBNode)) == 0,
              "re-allocated node is correctly aligned in the pool block");

        /* Writing through lb/ub must stay inside the pool's arrays. */
        for (j = 0; j < NUM_VARS; j++) {
            again[i]->lb[j] = (double)j;
            again[i]->ub[j] = (double)j + 1.0;
        }
    }

    for (i = 0; i < POOL_CAP; i++)
        for (j = i + 1; j < POOL_CAP; j++)
            check(again[i] != again[j], "pool never hands out the same node twice");

    for (i = 0; i < POOL_CAP; i++) bb_node_pool_return(pool, again[i]);
    bb_node_pool_free(pool);
}

/*
 * bb_node_pool_copy() is where the wild pointer used to be dereferenced.
 * Copy through an exhausted pool and make sure the result is usable.
 */
static void test_copy_through_exhausted_pool(void) {
    BBNodePool *pool = bb_node_pool_create(POOL_CAP, NUM_VARS);
    BBNode *held[POOL_CAP + EXTRA];
    BBNode *src, *dst;
    int i;

    printf("Test: bb_node_pool_copy through an exhausted pool\n");
    if (!pool) { check(0, "pool created"); return; }

    src = bb_node_create(NUM_VARS);
    check(src != NULL, "source node created");
    if (!src) { bb_node_pool_free(pool); return; }
    for (i = 0; i < NUM_VARS; i++) { src->lb[i] = (double)i; src->ub[i] = 10.0; }

    for (i = 0; i < POOL_CAP + EXTRA; i++) held[i] = bb_node_pool_get(pool);
    for (i = 0; i < POOL_CAP + EXTRA; i++) bb_node_pool_return(pool, held[i]);

    dst = bb_node_pool_copy(pool, src, NUM_VARS);
    check(dst != NULL, "copy succeeded");
    if (dst) {
        for (i = 0; i < NUM_VARS; i++) {
            check(dst->lb[i] == src->lb[i], "lb copied intact");
            check(dst->ub[i] == src->ub[i], "ub copied intact");
        }
        bb_node_pool_return(pool, dst);
    }

    bb_node_free(src);
    bb_node_pool_free(pool);
}


/*
 * End-to-end reproduction: solving a run of capacitated facility location
 * instances in one process exhausts the node pool, so the standalone-node
 * return path above is exercised for real. Before the fix this segfaulted in
 * bb_node_pool_copy() around the 22nd instance on Windows, having already
 * returned at least one silently wrong objective.
 *
 * CFL is the shape that matters here only because it branches enough to
 * outgrow the 1024-node pool; set covering at the same size never does.
 */
static uint64_t cfl_rng = 1;

static int cfl_ri(int lo, int hi) {
    cfl_rng = cfl_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return lo + (int)((unsigned)(cfl_rng >> 33) % (unsigned)(hi - lo + 1));
}

static void build_cfl(RalphMIPModel *m, int nf, int nc) {
    int i, j, k;
    int *idx = (int *)malloc(sizeof(int) * (size_t)(nf * nc + nf));
    double *val = (double *)malloc(sizeof(double) * (size_t)(nf * nc + nf));
    int *d = (int *)malloc(sizeof(int) * (size_t)nc);

    if (!idx || !val || !d) { free(idx); free(val); free(d); return; }

    for (i = 0; i < nf; i++)
        ralph_lp_add_var(m, 0.0, 1.0, (double)cfl_ri(20, 60), RALPH_LP_VAR_BINARY);
    for (i = 0; i < nf; i++)
        for (j = 0; j < nc; j++)
            ralph_lp_add_var(m, 0.0, 1.0, (double)cfl_ri(1, 20), RALPH_LP_VAR_BINARY);
    for (j = 0; j < nc; j++) d[j] = cfl_ri(5, 20);

    for (j = 0; j < nc; j++) {
        k = 0;
        for (i = 0; i < nf; i++) { idx[k] = nf + i * nc + j; val[k] = 1.0; k++; }
        ralph_lp_add_constraint(m, k, idx, val, RALPH_LP_SENSE_EQUAL, 1.0);
    }
    for (i = 0; i < nf; i++) {
        int cap = cfl_ri(40, 80);
        k = 0;
        for (j = 0; j < nc; j++) { idx[k] = nf + i * nc + j; val[k] = (double)d[j]; k++; }
        idx[k] = i; val[k] = -(double)cap; k++;
        ralph_lp_add_constraint(m, k, idx, val, RALPH_LP_SENSE_LESS_EQUAL, 0.0);
    }
    free(idx); free(val); free(d);
}

#define CFL_SIZE   6
#define CFL_TRIALS 23

static void test_repeated_cfl_solves(void) {
    int t;

    printf("Test: %d capacitated facility location solves in one process\n",
           CFL_TRIALS);

    for (t = 0; t < CFL_TRIALS; t++) {
        RalphMIPModel *m = ralph_mip_create();
        int status;

        check(m != NULL, "model created");
        if (!m) return;

        cfl_rng = (uint64_t)(1234 + t);
        build_cfl(m, CFL_SIZE, CFL_SIZE * 2 + 2);

        ralph_mip_optimize(m);
        status = (int)ralph_mip_get_status(m);
        check(status == RALPH_STATUS_OPTIMAL, "instance solved to optimality");
        ralph_mip_free(m);
    }

    check(1, "sequence completed without corrupting the node pool");
}

int main(void) {
    printf("=== B&B node pool regression tests ===\n\n");

    test_standalone_return_keeps_free_list_sane();
    test_reuse_after_standalone_return();
    test_copy_through_exhausted_pool();
    test_repeated_cfl_solves();

    printf("\n=== Summary ===\n");
    if (failures == 0) {
        printf("All tests passed\n");
        return 0;
    }
    printf("%d check(s) failed\n", failures);
    return 1;
}
