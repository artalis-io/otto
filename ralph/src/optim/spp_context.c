#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "spp.h"

static void spp_sort_row_order(SPPContext *ctx) {
    if (!ctx || !ctx->row_order || !ctx->row_degree) return;

    for (int i = 1; i < ctx->num_rows; i++) {
        int key = ctx->row_order[i];
        int j = i - 1;
        while (j >= 0) {
            int cur = ctx->row_order[j];
            int move = 0;
            if (ctx->row_degree[cur] > ctx->row_degree[key]) {
                move = 1;
            } else if (ctx->row_degree[cur] == ctx->row_degree[key] && cur > key) {
                move = 1;
            }
            if (!move) break;
            ctx->row_order[j + 1] = cur;
            j--;
        }
        ctx->row_order[j + 1] = key;
    }
}

static void spp_sort_set_order(SPPContext *ctx) {
    if (!ctx || !ctx->set_order || !ctx->costs || !ctx->set_size) return;

    for (int i = 1; i < ctx->num_sets; i++) {
        int key = ctx->set_order[i];
        int j = i - 1;
        while (j >= 0) {
            int cur = ctx->set_order[j];
            int move = 0;
            if (ctx->costs[cur] > ctx->costs[key] + RALPH_ZERO_TOL) {
                move = 1;
            } else if (fabs(ctx->costs[cur] - ctx->costs[key]) <= RALPH_ZERO_TOL &&
                       ctx->set_size[cur] < ctx->set_size[key]) {
                move = 1;
            } else if (fabs(ctx->costs[cur] - ctx->costs[key]) <= RALPH_ZERO_TOL &&
                       ctx->set_size[cur] == ctx->set_size[key] &&
                       cur > key) {
                move = 1;
            }
            if (!move) break;
            ctx->set_order[j + 1] = cur;
            j--;
        }
        ctx->set_order[j + 1] = key;
    }
}

static int spp_build_conflict_graph(SPPContext *ctx) {
    int *counts = NULL;
    int *marks = NULL;
    int *next = NULL;
    int total_adj = 0;

    if (!ctx || ctx->num_sets < 0) return -1;

    ctx->conflict_ptr = (int *)calloc((size_t)ctx->num_sets + 1, sizeof(int));
    if (!ctx->conflict_ptr) return -1;

    if (ctx->num_sets == 0) return 0;

    counts = (int *)calloc((size_t)ctx->num_sets, sizeof(int));
    marks = (int *)malloc((size_t)ctx->num_sets * sizeof(int));
    if (!counts || !marks) goto fail;

    for (int i = 0; i < ctx->num_sets; i++) marks[i] = -1;

    for (int set = 0; set < ctx->num_sets; set++) {
        int stamp = set + 1;
        for (int p = ctx->set_ptr[set]; p < ctx->set_ptr[set + 1]; p++) {
            int row = ctx->set_rows[p];
            for (int q = ctx->row_ptr[row]; q < ctx->row_ptr[row + 1]; q++) {
                int other = ctx->row_sets[q];
                if (other == set) continue;
                if (marks[other] == stamp) continue;
                marks[other] = stamp;
                counts[set]++;
            }
        }
    }

    for (int set = 0; set < ctx->num_sets; set++) {
        ctx->conflict_ptr[set + 1] = ctx->conflict_ptr[set] + counts[set];
    }
    total_adj = ctx->conflict_ptr[ctx->num_sets];
    ctx->num_conflict_edges = total_adj / 2;

    if (total_adj > 0) {
        ctx->conflict_adj = (int *)calloc((size_t)total_adj, sizeof(int));
        next = (int *)malloc((size_t)ctx->num_sets * sizeof(int));
        if (!ctx->conflict_adj || !next) goto fail;
        memcpy(next, ctx->conflict_ptr, (size_t)ctx->num_sets * sizeof(int));

        for (int i = 0; i < ctx->num_sets; i++) marks[i] = -1;

        for (int set = 0; set < ctx->num_sets; set++) {
            int stamp = set + 1;
            for (int p = ctx->set_ptr[set]; p < ctx->set_ptr[set + 1]; p++) {
                int row = ctx->set_rows[p];
                for (int q = ctx->row_ptr[row]; q < ctx->row_ptr[row + 1]; q++) {
                    int other = ctx->row_sets[q];
                    if (other == set) continue;
                    if (marks[other] == stamp) continue;
                    marks[other] = stamp;
                    ctx->conflict_adj[next[set]++] = other;
                }
            }
        }
    }

    free(counts);
    free(marks);
    free(next);
    return 0;

fail:
    free(counts);
    free(marks);
    free(next);
    free(ctx->conflict_ptr);
    ctx->conflict_ptr = NULL;
    free(ctx->conflict_adj);
    ctx->conflict_adj = NULL;
    ctx->num_conflict_edges = 0;
    return -1;
}

void spp_context_init(SPPContext *ctx) {
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
}

void spp_context_free(SPPContext *ctx) {
    if (!ctx) return;

    free(ctx->row_ptr);
    free(ctx->row_sets);
    free(ctx->set_ptr);
    free(ctx->set_rows);
    free(ctx->row_degree);
    free(ctx->set_size);
    free(ctx->row_order);
    free(ctx->set_order);
    free(ctx->rhs);
    free(ctx->costs);
    free(ctx->conflict_ptr);
    free(ctx->conflict_adj);

    spp_context_init(ctx);
}

int spp_context_build_from_signature(const LPModel *model,
                                     const SetCoverSignature *sig,
                                     SPPContext *ctx) {
    SPPContext temp;
    const SparseMatrix *A;
    int *row_next = NULL;

    if (!model || !sig || !ctx || !model->A) return -1;
    if (sig->type != RALPH_SETCOVER_PARTITIONING) return -1;

    spp_context_init(&temp);
    A = model->A;

    temp.type = sig->type;
    temp.num_rows = sig->num_elements;
    temp.num_sets = sig->num_sets;
    temp.nnz = A->colptr[temp.num_sets];
    temp.obj_sense = model->obj_sense;
    temp.obj_offset = model->obj_offset;

    for (int row = 0; row < temp.num_rows; row++) {
        if (fabs(sig->rhs[row] - 1.0) > RALPH_ZERO_TOL) {
            return -1;
        }
    }

    temp.row_ptr = (int *)calloc((size_t)temp.num_rows + 1, sizeof(int));
    temp.row_sets = (int *)calloc((size_t)(temp.nnz > 0 ? temp.nnz : 1), sizeof(int));
    temp.set_ptr = (int *)calloc((size_t)temp.num_sets + 1, sizeof(int));
    temp.set_rows = (int *)calloc((size_t)(temp.nnz > 0 ? temp.nnz : 1), sizeof(int));
    temp.row_degree = (int *)calloc((size_t)temp.num_rows, sizeof(int));
    temp.set_size = (int *)calloc((size_t)temp.num_sets, sizeof(int));
    temp.row_order = (int *)calloc((size_t)temp.num_rows, sizeof(int));
    temp.set_order = (int *)calloc((size_t)temp.num_sets, sizeof(int));
    temp.rhs = (double *)calloc((size_t)temp.num_rows, sizeof(double));
    temp.costs = (double *)calloc((size_t)temp.num_sets, sizeof(double));
    row_next = (int *)calloc((size_t)temp.num_rows, sizeof(int));

    if (!temp.row_ptr || !temp.row_sets || !temp.set_ptr || !temp.set_rows ||
        !temp.row_degree || !temp.set_size || !temp.row_order || !temp.set_order ||
        !temp.rhs || !temp.costs || !row_next) {
        goto fail;
    }

    memcpy(temp.set_ptr, A->colptr, ((size_t)temp.num_sets + 1) * sizeof(int));
    memcpy(temp.set_rows, A->rowidx, (size_t)temp.nnz * sizeof(int));
    memcpy(temp.row_degree, sig->element_coverage, (size_t)temp.num_rows * sizeof(int));
    memcpy(temp.set_size, sig->set_size, (size_t)temp.num_sets * sizeof(int));
    memcpy(temp.rhs, sig->rhs, (size_t)temp.num_rows * sizeof(double));
    memcpy(temp.costs, model->c, (size_t)temp.num_sets * sizeof(double));

    for (int row = 0; row < temp.num_rows; row++) {
        temp.row_ptr[row + 1] = temp.row_ptr[row] + temp.row_degree[row];
        row_next[row] = temp.row_ptr[row];
        temp.row_order[row] = row;
    }
    for (int set = 0; set < temp.num_sets; set++) {
        temp.set_order[set] = set;
    }

    for (int set = 0; set < temp.num_sets; set++) {
        for (int p = temp.set_ptr[set]; p < temp.set_ptr[set + 1]; p++) {
            int row = temp.set_rows[p];
            if (row < 0 || row >= temp.num_rows) goto fail;
            temp.row_sets[row_next[row]++] = set;
        }
    }

    spp_sort_row_order(&temp);
    spp_sort_set_order(&temp);

    if (spp_build_conflict_graph(&temp) != 0) goto fail;

    free(row_next);
    *ctx = temp;
    return 0;

fail:
    free(row_next);
    spp_context_free(&temp);
    return -1;
}

int spp_context_build(const LPModel *model, SPPContext *ctx) {
    SetCoverSignature sig;
    int rc;

    if (!model || !ctx) return -1;
    memset(&sig, 0, sizeof(sig));

    if (!detect_set_cover(model, &sig)) {
        return -1;
    }

    rc = spp_context_build_from_signature(model, &sig, ctx);
    detect_set_cover_free(&sig);
    return rc;
}

int spp_context_sets_conflict(const SPPContext *ctx, int set_a, int set_b) {
    int start;
    int end;

    if (!ctx || !ctx->conflict_ptr || !ctx->conflict_adj) return 0;
    if (set_a < 0 || set_b < 0 || set_a >= ctx->num_sets || set_b >= ctx->num_sets) {
        return 0;
    }
    if (set_a == set_b) return 0;

    start = ctx->conflict_ptr[set_a];
    end = ctx->conflict_ptr[set_a + 1];
    for (int p = start; p < end; p++) {
        if (ctx->conflict_adj[p] == set_b) return 1;
    }
    return 0;
}

int spp_context_check_solution(const SPPContext *ctx, const double *x, double tol) {
    if (!ctx || !x || tol < 0.0) return 0;

    for (int set = 0; set < ctx->num_sets; set++) {
        double value = x[set];
        if (value < -tol || value > 1.0 + tol) return 0;
        if (fabs(value) > tol && fabs(value - 1.0) > tol) return 0;
    }

    for (int row = 0; row < ctx->num_rows; row++) {
        double sum = 0.0;
        for (int p = ctx->row_ptr[row]; p < ctx->row_ptr[row + 1]; p++) {
            sum += x[ctx->row_sets[p]];
        }
        if (fabs(sum - ctx->rhs[row]) > tol) return 0;
    }

    return 1;
}

double spp_context_compute_objective(const SPPContext *ctx, const double *x) {
    double obj = 0.0;

    if (!ctx || !x) return RALPH_INFINITY;

    obj = ctx->obj_offset;
    for (int set = 0; set < ctx->num_sets; set++) {
        obj += ctx->costs[set] * x[set];
    }
    return obj;
}
