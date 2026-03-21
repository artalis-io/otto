#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "spp.h"

#define SPP_CUT_ACTIVITY_TOL 1e-9
#define SPP_CUT_VIOLATION_TOL 1e-6

typedef struct {
    int count;
    int capacity;
    int *sizes;
    int **members;
} SPPCliqueSet;

static void spp_sort_vertex_list(const SPPContext *ctx, const double *lp_x,
                                 int *verts, int count) {
    if (!ctx || !verts || count <= 1) return;

    for (int i = 1; i < count; i++) {
        int key = verts[i];
        double key_x = lp_x ? lp_x[key] : 0.0;
        int j = i - 1;
        while (j >= 0) {
            int cur = verts[j];
            double cur_x = lp_x ? lp_x[cur] : 0.0;
            int move = 0;

            if (cur_x + RALPH_ZERO_TOL < key_x) {
                move = 1;
            } else if (fabs(cur_x - key_x) <= RALPH_ZERO_TOL &&
                       ctx->costs[cur] > ctx->costs[key] + RALPH_ZERO_TOL) {
                move = 1;
            } else if (fabs(cur_x - key_x) <= RALPH_ZERO_TOL &&
                       fabs(ctx->costs[cur] - ctx->costs[key]) <= RALPH_ZERO_TOL &&
                       cur > key) {
                move = 1;
            }

            if (!move) break;
            verts[j + 1] = cur;
            j--;
        }
        verts[j + 1] = key;
    }
}

static void spp_sort_indices_ascending(int *values, int count) {
    if (!values || count <= 1) return;

    for (int i = 1; i < count; i++) {
        int key = values[i];
        int j = i - 1;
        while (j >= 0 && values[j] > key) {
            values[j + 1] = values[j];
            j--;
        }
        values[j + 1] = key;
    }
}

static int spp_conflicts_with_clique(const SPPContext *ctx, int set,
                                     const int *clique, int clique_size) {
    for (int i = 0; i < clique_size; i++) {
        if (!spp_context_sets_conflict(ctx, set, clique[i])) return 0;
    }
    return 1;
}

static int spp_build_greedy_clique(const SPPContext *ctx, const double *lp_x,
                                   int seed, int *clique, int max_size) {
    int *candidates = NULL;
    int cand_count = 0;
    int clique_size = 0;

    if (!ctx || !lp_x || !clique || max_size <= 0) return 0;
    if (seed < 0 || seed >= ctx->num_sets) return 0;
    if (lp_x[seed] <= SPP_CUT_ACTIVITY_TOL) return 0;

    candidates = (int *)malloc((size_t)ctx->num_sets * sizeof(int));
    if (!candidates) return 0;

    clique[clique_size++] = seed;
    for (int set = 0; set < ctx->num_sets; set++) {
        if (set == seed) continue;
        if (lp_x[set] <= SPP_CUT_ACTIVITY_TOL) continue;
        if (!spp_context_sets_conflict(ctx, seed, set)) continue;
        candidates[cand_count++] = set;
    }

    spp_sort_vertex_list(ctx, lp_x, candidates, cand_count);

    for (int i = 0; i < cand_count && clique_size < max_size; i++) {
        int set = candidates[i];
        if (spp_conflicts_with_clique(ctx, set, clique, clique_size)) {
            clique[clique_size++] = set;
        }
    }

    spp_sort_indices_ascending(clique, clique_size);
    free(candidates);
    return clique_size;
}

static int spp_clique_lhs(const double *lp_x, const int *clique, int clique_size,
                          double *lhs_out) {
    double lhs = 0.0;

    if (!lp_x || !clique || clique_size <= 0 || !lhs_out) return -1;
    for (int i = 0; i < clique_size; i++) {
        lhs += lp_x[clique[i]];
    }
    *lhs_out = lhs;
    return 0;
}

static void spp_clique_set_free(SPPCliqueSet *set) {
    if (!set) return;
    for (int i = 0; i < set->count; i++) {
        free(set->members[i]);
    }
    free(set->members);
    free(set->sizes);
    memset(set, 0, sizeof(*set));
}

static int spp_clique_set_contains(const SPPCliqueSet *set,
                                   const int *members, int size) {
    if (!set || !members || size <= 0) return 0;

    for (int i = 0; i < set->count; i++) {
        if (set->sizes[i] != size) continue;
        if (memcmp(set->members[i], members, (size_t)size * sizeof(int)) == 0) {
            return 1;
        }
    }
    return 0;
}

static int spp_clique_set_add(SPPCliqueSet *set, const int *members, int size) {
    int **new_members;
    int *new_sizes;
    int *copy;

    if (!set || !members || size <= 0) return -1;
    if (set->count >= set->capacity) {
        int new_cap = set->capacity > 0 ? set->capacity * 2 : 16;
        new_members = (int **)realloc(set->members, (size_t)new_cap * sizeof(int *));
        if (!new_members) {
            return -1;
        }
        new_sizes = (int *)realloc(set->sizes, (size_t)new_cap * sizeof(int));
        if (!new_sizes) {
            set->members = new_members;
            return -1;
        }
        set->members = new_members;
        set->sizes = new_sizes;
        set->capacity = new_cap;
    }

    copy = (int *)malloc((size_t)size * sizeof(int));
    if (!copy) return -1;
    memcpy(copy, members, (size_t)size * sizeof(int));

    set->members[set->count] = copy;
    set->sizes[set->count] = size;
    set->count++;
    return 0;
}

static int spp_emit_clique_cut(const double *lp_x,
                               const int *clique,
                               int clique_size,
                               const SPPCutSink *sink,
                               SPPCutStats *stats) {
    double lhs = 0.0;
    double violation;
    double *values = NULL;
    SPPCut cut;
    int rc;

    if (!lp_x || !clique || clique_size < 2 || !sink || !sink->emit) return 0;
    if (spp_clique_lhs(lp_x, clique, clique_size, &lhs) != 0) return 0;

    violation = lhs - 1.0;
    if (violation <= SPP_CUT_VIOLATION_TOL) {
        if (stats) stats->unviolated_cliques++;
        return 0;
    }

    values = (double *)malloc((size_t)clique_size * sizeof(double));
    if (!values) return 0;
    for (int i = 0; i < clique_size; i++) values[i] = 1.0;

    memset(&cut, 0, sizeof(cut));
    cut.nnz = clique_size;
    cut.indices = clique;
    cut.values = values;
    cut.sense = 'L';
    cut.rhs = 1.0;
    cut.violation = violation;
    cut.kind = SPP_CUT_CLIQUE;

    rc = sink->emit(sink->user, &cut);
    free(values);

    if (rc > 0 && stats) stats->clique_emitted++;
    return rc > 0 ? 1 : rc;
}

int spp_separate(const SPPContext *ctx,
                 const double *lp_x,
                 const SPPCutSink *sink,
                 SPPCutStats *stats) {
    SPPCliqueSet seen;
    int *seed_order = NULL;
    int *clique = NULL;
    int seed_count = 0;
    int cuts_added = 0;

    if (!ctx || !lp_x || !sink || !sink->emit) return 0;
    if (ctx->type != RALPH_SETCOVER_PARTITIONING || ctx->num_sets <= 0) return 0;

    memset(&seen, 0, sizeof(seen));
    if (stats) memset(stats, 0, sizeof(*stats));

    seed_order = (int *)malloc((size_t)ctx->num_sets * sizeof(int));
    clique = (int *)malloc((size_t)ctx->num_sets * sizeof(int));
    if (!seed_order || !clique) {
        free(seed_order);
        free(clique);
        return 0;
    }

    for (int set = 0; set < ctx->num_sets; set++) {
        if (lp_x[set] > SPP_CUT_ACTIVITY_TOL) {
            seed_order[seed_count++] = set;
        }
    }
    spp_sort_vertex_list(ctx, lp_x, seed_order, seed_count);

    for (int i = 0; i < seed_count; i++) {
        int seed = seed_order[i];
        int clique_size;
        int rc;

        clique_size = spp_build_greedy_clique(ctx, lp_x, seed, clique, ctx->num_sets);
        if (clique_size < 2) continue;
        if (stats) stats->clique_candidates++;

        if (spp_clique_set_contains(&seen, clique, clique_size)) {
            if (stats) stats->duplicate_cliques++;
            continue;
        }

        rc = spp_emit_clique_cut(lp_x, clique, clique_size, sink, stats);
        if (rc < 0) break;
        if (rc > 0) {
            if (spp_clique_set_add(&seen, clique, clique_size) != 0) break;
            cuts_added += rc;
        }
    }

    spp_clique_set_free(&seen);
    free(seed_order);
    free(clique);
    return cuts_added;
}
