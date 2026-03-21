#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "spp.h"

#define SPP_HEURISTIC_DEFAULT_NODE_LIMIT 20000

typedef struct {
    const SPPContext *ctx;
    const double *lp_x;
    double best_obj;
    double *best_sol;
    int node_limit;
    int stop;
    SPPHeuristicStats *stats;
} SPPHeuristicSearch;

static int spp_row_selected_count(const SPPContext *ctx, const signed char *state, int row) {
    int selected = 0;
    for (int p = ctx->row_ptr[row]; p < ctx->row_ptr[row + 1]; p++) {
        if (state[ctx->row_sets[p]] > 0) selected++;
    }
    return selected;
}

static int spp_all_rows_satisfied(const SPPContext *ctx, const signed char *state) {
    for (int i = 0; i < ctx->num_rows; i++) {
        if (spp_row_selected_count(ctx, state, i) != 1) return 0;
    }
    return 1;
}

static int spp_exclude_set(const SPPContext *ctx, signed char *state, int set,
                           SPPHeuristicStats *stats) {
    (void)ctx;
    if (set < 0) return -1;
    if (state[set] < 0) return 0;
    if (state[set] > 0) return -1;
    state[set] = -1;
    if (stats) stats->forced_exclusions++;
    return 0;
}

static int spp_select_set(const SPPContext *ctx, signed char *state, int set,
                          double *obj, SPPHeuristicStats *stats) {
    if (!ctx || !state || set < 0 || set >= ctx->num_sets || !obj) return -1;
    if (state[set] > 0) return 0;
    if (state[set] < 0) return -1;
    state[set] = 1;
    *obj += ctx->costs[set];
    if (stats) stats->forced_selections++;
    return 0;
}

static int spp_propagate(const SPPContext *ctx, signed char *state, double *obj,
                         SPPHeuristicStats *stats) {
    int changed = 1;

    while (changed) {
        changed = 0;
        for (int ord = 0; ord < ctx->num_rows; ord++) {
            int row = ctx->row_order[ord];
            int selected = 0;
            int available = 0;
            int only = -1;

            for (int p = ctx->row_ptr[row]; p < ctx->row_ptr[row + 1]; p++) {
                int set = ctx->row_sets[p];
                if (state[set] > 0) {
                    selected++;
                }
                if (state[set] >= 0) {
                    available++;
                    only = set;
                }
            }

            if (selected > 1) {
                if (stats) stats->branch_failures++;
                return -1;
            }
            if (selected == 0 && available == 0) {
                if (stats) stats->branch_failures++;
                return -1;
            }

            if (selected == 1) {
                for (int p = ctx->row_ptr[row]; p < ctx->row_ptr[row + 1]; p++) {
                    int set = ctx->row_sets[p];
                    if (state[set] == 0) {
                        if (spp_exclude_set(ctx, state, set, stats) != 0) {
                            if (stats) stats->branch_failures++;
                            return -1;
                        }
                        changed = 1;
                    }
                }
            } else if (available == 1) {
                if (spp_select_set(ctx, state, only, obj, stats) != 0) {
                    if (stats) stats->branch_failures++;
                    return -1;
                }
                changed = 1;
            }
        }
    }

    return 0;
}

static int spp_choose_branch_row(const SPPContext *ctx, const signed char *state) {
    int best_row = -1;
    int best_choices = 0;

    for (int ord = 0; ord < ctx->num_rows; ord++) {
        int row = ctx->row_order[ord];
        int selected = spp_row_selected_count(ctx, state, row);
        int choices = 0;

        if (selected == 1) continue;
        for (int p = ctx->row_ptr[row]; p < ctx->row_ptr[row + 1]; p++) {
            if (state[ctx->row_sets[p]] == 0) choices++;
        }
        if (choices == 0) return -1;
        if (best_row < 0 || choices < best_choices) {
            best_row = row;
            best_choices = choices;
        }
    }

    return best_row;
}

static double spp_candidate_score(const SPPContext *ctx, const signed char *state,
                                  const double *lp_x, int set) {
    int uncovered = 0;
    double lp = (lp_x && lp_x[set] > 0.0) ? lp_x[set] : 0.0;

    for (int p = ctx->set_ptr[set]; p < ctx->set_ptr[set + 1]; p++) {
        int row = ctx->set_rows[p];
        if (spp_row_selected_count(ctx, state, row) == 0) uncovered++;
    }
    if (uncovered <= 0) uncovered = 1;

    return (ctx->costs[set] / (double)uncovered) / (0.25 + lp);
}

static void spp_sort_candidates(const SPPContext *ctx, const signed char *state,
                                const double *lp_x, int *cand, int count) {
    for (int i = 1; i < count; i++) {
        int key = cand[i];
        double key_score = spp_candidate_score(ctx, state, lp_x, key);
        int j = i - 1;
        while (j >= 0) {
            int cur = cand[j];
            double cur_score = spp_candidate_score(ctx, state, lp_x, cur);
            int move = 0;
            if (cur_score > key_score + 1e-12) {
                move = 1;
            } else if (fabs(cur_score - key_score) <= 1e-12 &&
                       ctx->costs[cur] > ctx->costs[key] + RALPH_ZERO_TOL) {
                move = 1;
            } else if (fabs(cur_score - key_score) <= 1e-12 &&
                       fabs(ctx->costs[cur] - ctx->costs[key]) <= RALPH_ZERO_TOL &&
                       cur > key) {
                move = 1;
            }
            if (!move) break;
            cand[j + 1] = cur;
            j--;
        }
        cand[j + 1] = key;
    }
}

static void spp_store_solution(SPPHeuristicSearch *search, const signed char *state,
                               double obj) {
    if (!search || !state || !search->best_sol) return;
    if (obj >= search->best_obj - RALPH_OPT_TOL) return;

    search->best_obj = obj;
    for (int j = 0; j < search->ctx->num_sets; j++) {
        search->best_sol[j] = (state[j] > 0) ? 1.0 : 0.0;
    }
    if (search->stats) search->stats->incumbent_updates++;
}

static void spp_search_dfs(SPPHeuristicSearch *search, const signed char *state_in,
                           double obj_in) {
    const SPPContext *ctx;
    signed char *state = NULL;
    double obj;
    int row;
    int cand_count = 0;
    int *cand = NULL;

    if (!search || search->stop) return;
    if (search->stats) search->stats->nodes_visited++;
    if (search->node_limit > 0 && search->stats &&
        search->stats->nodes_visited > search->node_limit) {
        search->stop = 1;
        return;
    }
    if (obj_in >= search->best_obj - RALPH_OPT_TOL) return;

    ctx = search->ctx;
    state = (signed char *)malloc((size_t)ctx->num_sets * sizeof(signed char));
    if (!state) {
        search->stop = 1;
        return;
    }
    memcpy(state, state_in, (size_t)ctx->num_sets * sizeof(signed char));
    obj = obj_in;

    if (spp_propagate(ctx, state, &obj, search->stats) != 0) {
        free(state);
        return;
    }
    if (obj >= search->best_obj - RALPH_OPT_TOL) {
        free(state);
        return;
    }
    if (spp_all_rows_satisfied(ctx, state)) {
        spp_store_solution(search, state, obj);
        free(state);
        return;
    }

    row = spp_choose_branch_row(ctx, state);
    if (row < 0) {
        free(state);
        return;
    }

    for (int p = ctx->row_ptr[row]; p < ctx->row_ptr[row + 1]; p++) {
        if (state[ctx->row_sets[p]] == 0) cand_count++;
    }
    if (cand_count <= 0) {
        free(state);
        return;
    }

    cand = (int *)malloc((size_t)cand_count * sizeof(int));
    if (!cand) {
        search->stop = 1;
        free(state);
        return;
    }
    cand_count = 0;
    for (int p = ctx->row_ptr[row]; p < ctx->row_ptr[row + 1]; p++) {
        int set = ctx->row_sets[p];
        if (state[set] == 0) cand[cand_count++] = set;
    }
    spp_sort_candidates(ctx, state, search->lp_x, cand, cand_count);

    for (int i = 0; i < cand_count && !search->stop; i++) {
        int set = cand[i];
        double child_obj = obj;
        signed char *child = (signed char *)malloc((size_t)ctx->num_sets * sizeof(signed char));
        if (!child) {
            search->stop = 1;
            break;
        }
        memcpy(child, state, (size_t)ctx->num_sets * sizeof(signed char));
        if (spp_select_set(ctx, child, set, &child_obj, NULL) == 0) {
            spp_search_dfs(search, child, child_obj);
        }
        free(child);
    }

    free(cand);
    free(state);
}

int spp_heuristic_run(const SPPContext *ctx,
                      const double *lp_x,
                      double *sol_out,
                      double *obj_out,
                      SPPHeuristicStats *stats) {
    SPPHeuristicSearch search;
    signed char *state = NULL;

    if (!ctx || !sol_out || !obj_out) return -1;
    if (ctx->type != RALPH_SETCOVER_PARTITIONING || ctx->num_sets <= 0 || ctx->num_rows <= 0) {
        return -1;
    }

    memset(&search, 0, sizeof(search));
    if (stats) memset(stats, 0, sizeof(*stats));

    state = (signed char *)calloc((size_t)ctx->num_sets, sizeof(signed char));
    search.best_sol = (double *)calloc((size_t)ctx->num_sets, sizeof(double));
    if (!state || !search.best_sol) {
        free(state);
        free(search.best_sol);
        return -1;
    }

    search.ctx = ctx;
    search.lp_x = lp_x;
    search.best_obj = RALPH_INFINITY;
    search.node_limit = stats && stats->node_limit > 0
                            ? stats->node_limit
                            : SPP_HEURISTIC_DEFAULT_NODE_LIMIT;
    search.stats = stats;
    if (stats) stats->node_limit = search.node_limit;

    spp_search_dfs(&search, state, ctx->obj_offset);

    free(state);

    if (!isfinite(search.best_obj) || search.best_obj >= RALPH_INFINITY * 0.5) {
        free(search.best_sol);
        return -1;
    }

    memcpy(sol_out, search.best_sol, (size_t)ctx->num_sets * sizeof(double));
    *obj_out = search.best_obj;
    free(search.best_sol);
    return 0;
}
