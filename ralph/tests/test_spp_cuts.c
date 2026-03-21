/*
 * Focused tests for the standalone SPP cut separator.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "lp.h"
#include "spp.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
        printf("  PASS: %s\n", msg); \
    } else { \
        printf("  FAIL: %s\n", msg); \
    } \
} while (0)

#define ASSERT_NEAR(a, b, tol, msg) do { \
    tests_run++; \
    if (fabs((a) - (b)) <= (tol)) { \
        tests_passed++; \
        printf("  PASS: %s (%.6f ~= %.6f)\n", msg, (double)(a), (double)(b)); \
    } else { \
        printf("  FAIL: %s (%.6f != %.6f)\n", msg, (double)(a), (double)(b)); \
    } \
} while (0)

typedef struct {
    int nnz;
    int *indices;
    double *values;
    char sense;
    double rhs;
    double violation;
    SPPCutKind kind;
} CollectedCut;

typedef struct {
    int count;
    int capacity;
    CollectedCut *cuts;
} CutCollector;

static void collector_free(CutCollector *collector) {
    if (!collector) return;
    for (int i = 0; i < collector->count; i++) {
        free(collector->cuts[i].indices);
        free(collector->cuts[i].values);
    }
    free(collector->cuts);
    memset(collector, 0, sizeof(*collector));
}

static int collector_emit(void *user, const SPPCut *cut) {
    CutCollector *collector = (CutCollector *)user;
    CollectedCut *slot;

    if (!collector || !cut || cut->nnz <= 0) return -1;
    if (collector->count >= collector->capacity) {
        int new_cap = collector->capacity > 0 ? collector->capacity * 2 : 8;
        CollectedCut *new_cuts =
            (CollectedCut *)realloc(collector->cuts, (size_t)new_cap * sizeof(CollectedCut));
        if (!new_cuts) return -1;
        collector->cuts = new_cuts;
        collector->capacity = new_cap;
    }

    slot = &collector->cuts[collector->count];
    memset(slot, 0, sizeof(*slot));
    slot->indices = (int *)malloc((size_t)cut->nnz * sizeof(int));
    slot->values = (double *)malloc((size_t)cut->nnz * sizeof(double));
    if (!slot->indices || !slot->values) {
        free(slot->indices);
        free(slot->values);
        memset(slot, 0, sizeof(*slot));
        return -1;
    }

    memcpy(slot->indices, cut->indices, (size_t)cut->nnz * sizeof(int));
    memcpy(slot->values, cut->values, (size_t)cut->nnz * sizeof(double));
    slot->nnz = cut->nnz;
    slot->sense = cut->sense;
    slot->rhs = cut->rhs;
    slot->violation = cut->violation;
    slot->kind = cut->kind;
    collector->count++;
    return 1;
}

static LPModel *build_triangle_clique_fixture(void) {
    LPModel *model = lp_model_create();
    if (!model) return NULL;

    model->obj_sense = 1;

    lp_model_add_var(model, 0.0, 1.0, 4.0, 'B');  /* A covers rows 0,1 */
    lp_model_add_var(model, 0.0, 1.0, 4.0, 'B');  /* B covers rows 1,2 */
    lp_model_add_var(model, 0.0, 1.0, 4.0, 'B');  /* C covers rows 0,2 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S0 covers row 0 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S1 covers row 1 */
    lp_model_add_var(model, 0.0, 1.0, 1.0, 'B');  /* S2 covers row 2 */

    {
        int idx[] = {0, 2, 3};
        double val[] = {1.0, 1.0, 1.0};
        lp_model_add_constraint(model, 3, idx, val, 'E', 1.0);
    }
    {
        int idx[] = {0, 1, 4};
        double val[] = {1.0, 1.0, 1.0};
        lp_model_add_constraint(model, 3, idx, val, 'E', 1.0);
    }
    {
        int idx[] = {1, 2, 5};
        double val[] = {1.0, 1.0, 1.0};
        lp_model_add_constraint(model, 3, idx, val, 'E', 1.0);
    }

    lp_model_finalize(model);
    return model;
}

static int cut_is_valid_for_all_exact_covers(const SPPContext *ctx, const CollectedCut *cut) {
    double *x;
    int feasible_count = 0;

    if (!ctx || !cut || cut->sense != 'L') return 0;
    x = (double *)calloc((size_t)ctx->num_sets, sizeof(double));
    if (!x) return 0;

    for (unsigned int mask = 0; mask < (1u << ctx->num_sets); mask++) {
        double lhs = 0.0;
        for (int j = 0; j < ctx->num_sets; j++) {
            x[j] = (mask & (1u << j)) ? 1.0 : 0.0;
        }
        if (!spp_context_check_solution(ctx, x, 1e-9)) continue;
        feasible_count++;

        for (int k = 0; k < cut->nnz; k++) {
            lhs += cut->values[k] * x[cut->indices[k]];
        }
        if (lhs > cut->rhs + 1e-9) {
            free(x);
            return 0;
        }
    }

    free(x);
    return feasible_count > 0;
}

static void test_spp_clique_separator_emits_valid_cut(void) {
    LPModel *model = build_triangle_clique_fixture();
    SPPContext ctx;
    SPPCutSink sink;
    SPPCutStats stats;
    CutCollector collector;
    double lp_x[] = {0.5, 0.5, 0.5, 0.0, 0.0, 0.0};
    int found_triangle = 0;

    printf("\n=== Test: SPP Separator Emits Valid Clique Cut ===\n");

    memset(&collector, 0, sizeof(collector));
    spp_context_init(&ctx);
    sink.emit = collector_emit;
    sink.user = &collector;

    ASSERT(model != NULL, "Triangle fixture model created");
    ASSERT(spp_context_build(model, &ctx) == 0, "SPP context built");
    ASSERT(spp_separate(&ctx, lp_x, &sink, &stats) > 0,
           "Separator emitted at least one violated clique cut");
    ASSERT(collector.count > 0, "Collector stored emitted cuts");
    ASSERT(stats.clique_emitted > 0, "Separator stats recorded emitted cliques");

    for (int i = 0; i < collector.count; i++) {
        CollectedCut *cut = &collector.cuts[i];
        ASSERT(cut_is_valid_for_all_exact_covers(&ctx, cut),
               "Emitted cut is valid for every exact-cover solution");

        if (cut->kind == SPP_CUT_CLIQUE &&
            cut->nnz == 3 &&
            cut->indices[0] == 0 &&
            cut->indices[1] == 1 &&
            cut->indices[2] == 2 &&
            cut->sense == 'L' &&
            fabs(cut->rhs - 1.0) <= 1e-9) {
            found_triangle = 1;
            ASSERT_NEAR(cut->violation, 0.5, 1e-9,
                        "Triangle clique cut has expected LP violation");
        }
    }

    ASSERT(found_triangle, "Triangle clique cut was found");

    collector_free(&collector);
    spp_context_free(&ctx);
    lp_model_free(model);
}

static void test_spp_separator_skips_integral_solution(void) {
    LPModel *model = build_triangle_clique_fixture();
    SPPContext ctx;
    SPPCutSink sink;
    SPPCutStats stats;
    CutCollector collector;
    double exact_cover_x[] = {1.0, 0.0, 0.0, 0.0, 0.0, 1.0};

    printf("\n=== Test: SPP Separator Skips Integral Exact Cover ===\n");

    memset(&collector, 0, sizeof(collector));
    spp_context_init(&ctx);
    sink.emit = collector_emit;
    sink.user = &collector;

    ASSERT(model != NULL, "Triangle fixture model created");
    ASSERT(spp_context_build(model, &ctx) == 0, "SPP context built");
    ASSERT(spp_context_check_solution(&ctx, exact_cover_x, 1e-9) == 1,
           "Integral fixture vector is a valid exact cover");
    ASSERT(spp_separate(&ctx, exact_cover_x, &sink, &stats) == 0,
           "Separator emits no cuts at an integral exact cover");
    ASSERT(collector.count == 0, "Collector remains empty");

    collector_free(&collector);
    spp_context_free(&ctx);
    lp_model_free(model);
}

int main(void) {
    printf("Ralph SPP Cut Tests\n");

    test_spp_clique_separator_emits_valid_cut();
    test_spp_separator_skips_integral_solution();

    printf("\n=== Summary ===\n");
    printf("Passed %d/%d tests\n", tests_passed, tests_run);

    return (tests_run == tests_passed) ? 0 : 1;
}
