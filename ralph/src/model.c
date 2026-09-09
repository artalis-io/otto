/*
 * Ralph - LP Model Management Implementation
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "lp.h"

/* Initial capacity for dynamic arrays */
#define INITIAL_VAR_CAPACITY 128
#define INITIAL_CON_CAPACITY 128
#define INITIAL_NNZ_CAPACITY 512

/* ============================================================================
 * Constraint Entry (for incremental building)
 * ============================================================================ */

typedef struct {
    int nnz;
    int capacity;
    int *indices;
    double *values;
    char sense;
    double rhs;
} ConstraintEntry;

/* ============================================================================
 * Build State Definition (per-model, thread-safe)
 * ============================================================================ */

struct LPModelBuildState {
    ConstraintEntry **constraints;
    int con_count;
    int con_capacity;
};

/* Free build state and all its contents */
static void build_state_free(LPModelBuildState *bs) {
    if (!bs) return;

    if (bs->constraints) {
        for (int i = 0; i < bs->con_count; i++) {
            if (bs->constraints[i]) {
                free(bs->constraints[i]->indices);
                free(bs->constraints[i]->values);
                free(bs->constraints[i]);
            }
        }
        free(bs->constraints);
    }
    free(bs);
}

/* Create a new build state */
static LPModelBuildState* build_state_create(void) {
    LPModelBuildState *bs = (LPModelBuildState*)calloc(1, sizeof(LPModelBuildState));
    return bs;
}

/* ============================================================================
 * LP Model Creation/Destruction
 * ============================================================================ */

LPModel* lp_model_create(void) {
    LPModel *model = (LPModel*)calloc(1, sizeof(LPModel));
    if (!model) return NULL;

    model->num_vars = 0;
    model->num_cons = 0;
    model->num_elements = 0;
    model->obj_sense = 1;  /* Minimize by default */
    model->obj_offset = 0.0;
    model->num_integers = 0;
    model->num_binary = 0;
    model->build_state = NULL;

    /* W2: Initialize runtime tolerances to compile-time defaults */
    model->feas_tol = RALPH_FEAS_TOL;
    model->opt_tol = RALPH_OPT_TOL;
    model->pivot_tol = RALPH_PIVOT_TOL;

    return model;
}

void lp_model_free(LPModel *model) {
    if (!model) return;

    sparse_free(model->A);
    model->A = NULL;
    SAFE_FREE(model->c);
    SAFE_FREE(model->b);
    SAFE_FREE(model->sense);
    SAFE_FREE(model->con_origin);
    SAFE_FREE(model->lb);
    SAFE_FREE(model->ub);
    SAFE_FREE(model->var_shifted);
    SAFE_FREE(model->var_shift);
    SAFE_FREE(model->var_type);

    if (model->var_names) {
        for (int i = 0; i < model->num_vars; i++) {
            SAFE_FREE(model->var_names[i]);
        }
        SAFE_FREE(model->var_names);
    }

    if (model->con_names) {
        for (int i = 0; i < model->num_cons; i++) {
            SAFE_FREE(model->con_names[i]);
        }
        SAFE_FREE(model->con_names);
    }

    SAFE_FREE(model->name);
    build_state_free(model->build_state);
    model->build_state = NULL;
    free(model);
}

/* ============================================================================
 * Model Building via Public API
 * ============================================================================ */

int lp_model_add_var(LPModel *model, double lb, double ub, double obj, char type) {
    if (!model) return -1;

    int idx = model->num_vars;
    int new_capacity = model->num_vars + 1;

    /*
     * Reallocate arrays one at a time. This is safe because:
     * 1. num_vars is only incremented after ALL allocations succeed
     * 2. All array accesses are bounded by num_vars, not capacity
     * 3. On partial failure, some arrays may have extra unused capacity,
     *    but all data up to num_vars remains valid and accessible
     *
     * Alternative (malloc new + memcpy + free old) would double memory usage.
     */
    double *new_c = (double*)realloc(model->c, new_capacity * sizeof(double));
    if (!new_c) return -1;
    model->c = new_c;

    double *new_lb = (double*)realloc(model->lb, new_capacity * sizeof(double));
    if (!new_lb) return -1;
    model->lb = new_lb;

    double *new_ub = (double*)realloc(model->ub, new_capacity * sizeof(double));
    if (!new_ub) return -1;
    model->ub = new_ub;

    unsigned char *new_var_shifted =
        (unsigned char*)realloc(model->var_shifted, new_capacity * sizeof(unsigned char));
    if (!new_var_shifted) return -1;
    model->var_shifted = new_var_shifted;
    model->var_shifted_capacity = new_capacity;

    double *new_var_shift =
        (double*)realloc(model->var_shift, new_capacity * sizeof(double));
    if (!new_var_shift) return -1;
    model->var_shift = new_var_shift;
    model->var_shift_capacity = new_capacity;

    char *new_type = (char*)realloc(model->var_type, new_capacity * sizeof(char));
    if (!new_type) return -1;
    model->var_type = new_type;

    model->c[idx] = obj;
    model->lb[idx] = lb;
    model->ub[idx] = ub;
    model->var_shifted[idx] = 0;
    model->var_shift[idx] = 0.0;
    model->var_type[idx] = type;

    if (type == 'I' || type == 'B') {
        model->num_integers++;
        if (type == 'B') model->num_binary++;
    }

    model->num_vars++;
    return idx;
}

/* Rebuild build state from a finalized model's sparse matrix.
 * This is called when adding constraints after the model has been solved. */
static int rebuild_build_state(LPModel *model) {
    if (!model || !model->A) return -1;

    int m = model->num_cons;
    int n = model->num_vars;
    SparseMatrix *A = model->A;

    /* Free old build state */
    build_state_free(model->build_state);
    model->build_state = NULL;

    /* Create new build state */
    LPModelBuildState *bs = build_state_create();
    if (!bs) return -1;

    /* Allocate constraint array */
    bs->constraints = (ConstraintEntry**)calloc(m + 64, sizeof(ConstraintEntry*));
    if (!bs->constraints) {
        build_state_free(bs);
        return -1;
    }

    bs->con_count = m;
    bs->con_capacity = m + 64;

    /* Count non-zeros per row */
    int *row_nnz = NULL;
    if (m > 0) {
        row_nnz = (int*)calloc((size_t)m, sizeof(int));
    }
    if (m > 0 && !row_nnz) {
        build_state_free(bs);
        return -1;
    }

    for (int j = 0; j < n; j++) {
        for (int p = A->colptr[j]; p < A->colptr[j+1]; p++) {
            row_nnz[A->rowidx[p]]++;
        }
    }

    /* Create constraint entries */
    for (int i = 0; i < m; i++) {
        ConstraintEntry *entry = (ConstraintEntry*)calloc(1, sizeof(ConstraintEntry));
        if (!entry) {
            free(row_nnz);
            build_state_free(bs);
            return -1;
        }

        entry->nnz = 0;
        entry->capacity = row_nnz[i];
        entry->indices = (int*)calloc(row_nnz[i], sizeof(int));
        entry->values = (double*)calloc(row_nnz[i], sizeof(double));
        entry->sense = model->sense[i];
        entry->rhs = model->b[i];

        if ((row_nnz[i] > 0) && (!entry->indices || !entry->values)) {
            free(entry->indices);
            free(entry->values);
            free(entry);
            free(row_nnz);
            build_state_free(bs);
            return -1;
        }

        bs->constraints[i] = entry;
    }

    /* Fill in constraint entries from sparse matrix (CSC -> row format) */
    for (int j = 0; j < n; j++) {
        for (int p = A->colptr[j]; p < A->colptr[j+1]; p++) {
            int i = A->rowidx[p];
            double v = A->values[p];
            ConstraintEntry *entry = bs->constraints[i];
            entry->indices[entry->nnz] = j;
            entry->values[entry->nnz] = v;
            entry->nnz++;
        }
    }

    free(row_nnz);

    /* Free the old sparse matrix */
    sparse_free(model->A);
    model->A = NULL;

    model->build_state = bs;
    return 0;
}

/* Ensure model is in editable row-oriented form (build_state, no CSC matrix). */
static int ensure_build_state(LPModel *model) {
    if (!model) return -1;
    if (model->A != NULL) {
        if (rebuild_build_state(model) != 0) {
            return -1;
        }
    }
    if (!model->build_state) return -1;
    return 0;
}

int lp_model_add_constraint(LPModel *model, int nnz, const int *indices,
                            const double *values, char sense, double rhs) {
    if (!model) return -1;
    if (nnz < 0) return -1;
    if (nnz > 0 && (!indices || !values)) return -1;

    /* Validate column indices up front. An out-of-range index must be a clean
     * error, not a coefficient silently dropped later by triplets_to_csc at
     * finalize (which would build the model wrong with no signal). Mirrors the
     * var-bounds check in lp_model_set_coefficient. */
    for (int k = 0; k < nnz; k++) {
        if (indices[k] < 0 || indices[k] >= model->num_vars) return -1;
    }

    /* If model was already finalized, rebuild build state from A
     * so we can add the new constraint. */
    if (model->A != NULL) {
        if (rebuild_build_state(model) != 0) {
            return -1;
        }
    }

    /* Initialize build state if needed */
    if (!model->build_state) {
        model->build_state = build_state_create();
        if (!model->build_state) return -1;
    }

    LPModelBuildState *bs = model->build_state;

    /* Expand if needed */
    if (bs->con_count >= bs->con_capacity) {
        int new_cap = bs->con_capacity == 0 ? 64 : bs->con_capacity * 2;
        ConstraintEntry **new_cons = (ConstraintEntry**)realloc(bs->constraints,
                                                                 new_cap * sizeof(ConstraintEntry*));
        if (!new_cons) return -1;
        bs->constraints = new_cons;
        bs->con_capacity = new_cap;
    }

    /* Create new constraint entry */
    ConstraintEntry *entry = (ConstraintEntry*)calloc(1, sizeof(ConstraintEntry));
    if (!entry) return -1;

    entry->nnz = nnz;
    entry->capacity = nnz;
    entry->indices = (int*)calloc(nnz, sizeof(int));
    entry->values = (double*)calloc(nnz, sizeof(double));
    entry->sense = sense;
    entry->rhs = rhs;

    if ((nnz > 0) && (!entry->indices || !entry->values)) {
        free(entry->indices);
        free(entry->values);
        free(entry);
        return -1;
    }

    if (nnz > 0) {
        memcpy(entry->indices, indices, nnz * sizeof(int));
        memcpy(entry->values, values, nnz * sizeof(double));
    }

    bs->constraints[bs->con_count] = entry;

    /* Update model's RHS and sense arrays */
    int idx = model->num_cons;
    int new_capacity = model->num_cons + 1;

    double *new_b = (double*)realloc(model->b, new_capacity * sizeof(double));
    if (!new_b) return -1;
    model->b = new_b;

    char *new_sense = (char*)realloc(model->sense, new_capacity * sizeof(char));
    if (!new_sense) return -1;
    model->sense = new_sense;

    int *new_con_origin = (int*)realloc(model->con_origin, new_capacity * sizeof(int));
    if (!new_con_origin) return -1;
    model->con_origin = new_con_origin;
    model->con_origin_capacity = new_capacity;

    model->b[idx] = rhs;
    model->sense[idx] = sense;
    model->con_origin[idx] = idx;

    model->num_cons++;
    bs->con_count++;

    return idx;
}

/* Set/update/remove one coefficient within a row-oriented constraint entry.
 * Returns delta nnz via delta_nnz: +1 insert, -1 remove, 0 replace/no-op. */
static int set_constraint_entry_coef(ConstraintEntry *entry, int var, double value,
                                     int *delta_nnz) {
    if (!entry) return -1;
    if (delta_nnz) *delta_nnz = 0;

    int pos = -1;
    for (int k = 0; k < entry->nnz; k++) {
        if (entry->indices[k] == var) {
            pos = k;
            break;
        }
    }

    /* Near-zero means remove from sparsity pattern. */
    if (fabs(value) <= 1e-15) {
        if (pos >= 0) {
            int last = entry->nnz - 1;
            if (pos != last) {
                entry->indices[pos] = entry->indices[last];
                entry->values[pos] = entry->values[last];
            }
            entry->nnz--;
            if (delta_nnz) *delta_nnz = -1;
        }
        return 0;
    }

    if (pos >= 0) {
        entry->values[pos] = value;
        return 0;
    }

    /* Insert new non-zero. */
    if (entry->nnz >= entry->capacity) {
        int new_cap = entry->capacity > 0 ? entry->capacity * 2 : 4;
        int *new_indices = (int*)realloc(entry->indices, new_cap * sizeof(int));
        double *new_values = (double*)realloc(entry->values, new_cap * sizeof(double));

        if (!new_indices || !new_values) {
            /* Preserve any successful realloc to avoid leaks. */
            if (new_indices) entry->indices = new_indices;
            if (new_values) entry->values = new_values;
            return -1;
        }

        entry->indices = new_indices;
        entry->values = new_values;
        entry->capacity = new_cap;
    }

    entry->indices[entry->nnz] = var;
    entry->values[entry->nnz] = value;
    entry->nnz++;
    if (delta_nnz) *delta_nnz = 1;
    return 0;
}

int lp_model_set_coefficient(LPModel *model, int constraint, int var, double value) {
    if (!model) return -1;
    if (constraint < 0 || constraint >= model->num_cons) return -1;
    if (var < 0 || var >= model->num_vars) return -1;

    if (ensure_build_state(model) != 0) return -1;
    if (constraint >= model->build_state->con_count) return -1;

    ConstraintEntry *entry = model->build_state->constraints[constraint];
    int delta_nnz = 0;
    if (set_constraint_entry_coef(entry, var, value, &delta_nnz) != 0) {
        return -1;
    }

    model->num_elements += delta_nnz;
    if (model->num_elements < 0) model->num_elements = 0;
    return 0;
}

int lp_model_set_coefficients(LPModel *model, int count, const int *constraints,
                              const int *vars, const double *values) {
    if (!model || count < 0) return -1;
    if (count == 0) return 0;
    if (!constraints || !vars || !values) return -1;

    /* Validate indices first to avoid partial edits on bad input. */
    for (int i = 0; i < count; i++) {
        if (constraints[i] < 0 || constraints[i] >= model->num_cons) return -1;
        if (vars[i] < 0 || vars[i] >= model->num_vars) return -1;
    }

    if (ensure_build_state(model) != 0) return -1;

    int delta_total = 0;
    for (int i = 0; i < count; i++) {
        int row = constraints[i];
        if (row < 0 || row >= model->build_state->con_count) return -1;

        int delta_nnz = 0;
        if (set_constraint_entry_coef(model->build_state->constraints[row],
                                      vars[i], values[i], &delta_nnz) != 0) {
            return -1;
        }
        delta_total += delta_nnz;
    }

    model->num_elements += delta_total;
    if (model->num_elements < 0) model->num_elements = 0;
    return 0;
}

int lp_model_get_coefficient(const LPModel *model, int constraint, int var, double *value) {
    if (!model || !value) return -1;
    if (constraint < 0 || constraint >= model->num_cons) return -1;
    if (var < 0 || var >= model->num_vars) return -1;

    *value = 0.0;

    if (model->A) {
        const SparseMatrix *A = model->A;
        for (int p = A->colptr[var]; p < A->colptr[var + 1]; p++) {
            if (A->rowidx[p] == constraint) {
                *value = A->values[p];
                return 0;
            }
        }
        return 0;
    }

    if (!model->build_state || constraint >= model->build_state->con_count) {
        return -1;
    }

    ConstraintEntry *entry = model->build_state->constraints[constraint];
    if (!entry) return -1;

    for (int k = 0; k < entry->nnz; k++) {
        if (entry->indices[k] == var) {
            *value = entry->values[k];
            break;
        }
    }

    return 0;
}

int lp_model_delete_constraint(LPModel *model, int constraint) {
    if (!model) return -1;
    if (constraint < 0 || constraint >= model->num_cons) return -1;
    if (ensure_build_state(model) != 0) return -1;
    if (constraint >= model->build_state->con_count) return -1;

    LPModelBuildState *bs = model->build_state;
    ConstraintEntry *entry = bs->constraints[constraint];
    int removed_nnz = entry ? entry->nnz : 0;

    if (entry) {
        free(entry->indices);
        free(entry->values);
        free(entry);
    }

    for (int i = constraint + 1; i < bs->con_count; i++) {
        bs->constraints[i - 1] = bs->constraints[i];
    }
    bs->con_count--;
    bs->constraints[bs->con_count] = NULL;

    int tail = model->num_cons - constraint - 1;
    if (tail > 0) {
        memmove(&model->b[constraint], &model->b[constraint + 1], (size_t)tail * sizeof(double));
        memmove(&model->sense[constraint], &model->sense[constraint + 1], (size_t)tail * sizeof(char));
        if (model->con_origin) {
            memmove(&model->con_origin[constraint], &model->con_origin[constraint + 1],
                    (size_t)tail * sizeof(int));
        }
    }

    if (model->con_names && constraint < model->con_names_capacity) {
        SAFE_FREE(model->con_names[constraint]);
        if (tail > 0 && constraint + 1 < model->con_names_capacity) {
            memmove(&model->con_names[constraint], &model->con_names[constraint + 1],
                    (size_t)tail * sizeof(char*));
        }
        if (model->num_cons - 1 >= 0 && model->num_cons - 1 < model->con_names_capacity) {
            model->con_names[model->num_cons - 1] = NULL;
        }
    }

    model->num_cons--;
    model->num_elements -= removed_nnz;
    if (model->num_elements < 0) model->num_elements = 0;
    return 0;
}

int lp_model_delete_var(LPModel *model, int var) {
    if (!model) return -1;
    if (var < 0 || var >= model->num_vars) return -1;

    char removed_type = model->var_type[var];

    if (model->num_cons > 0) {
        if (ensure_build_state(model) != 0) return -1;
    } else if (model->A != NULL) {
        sparse_free(model->A);
        model->A = NULL;
    }

    int removed_nnz = 0;
    if (model->build_state) {
        LPModelBuildState *bs = model->build_state;
        for (int i = 0; i < bs->con_count; i++) {
            ConstraintEntry *entry = bs->constraints[i];
            if (!entry) continue;

            int write = 0;
            for (int k = 0; k < entry->nnz; k++) {
                int idx = entry->indices[k];
                double val = entry->values[k];
                if (idx == var) {
                    removed_nnz++;
                    continue;
                }
                if (idx > var) idx--;
                entry->indices[write] = idx;
                entry->values[write] = val;
                write++;
            }
            entry->nnz = write;
        }
    }

    int tail = model->num_vars - var - 1;
    if (tail > 0) {
        memmove(&model->c[var], &model->c[var + 1], (size_t)tail * sizeof(double));
        memmove(&model->lb[var], &model->lb[var + 1], (size_t)tail * sizeof(double));
        memmove(&model->ub[var], &model->ub[var + 1], (size_t)tail * sizeof(double));
        if (model->var_shifted) {
            memmove(&model->var_shifted[var], &model->var_shifted[var + 1],
                    (size_t)tail * sizeof(unsigned char));
        }
        if (model->var_shift) {
            memmove(&model->var_shift[var], &model->var_shift[var + 1],
                    (size_t)tail * sizeof(double));
        }
        memmove(&model->var_type[var], &model->var_type[var + 1], (size_t)tail * sizeof(char));
    }

    if (model->var_names && var < model->var_names_capacity) {
        SAFE_FREE(model->var_names[var]);
        if (tail > 0 && var + 1 < model->var_names_capacity) {
            memmove(&model->var_names[var], &model->var_names[var + 1],
                    (size_t)tail * sizeof(char*));
        }
        if (model->num_vars - 1 >= 0 && model->num_vars - 1 < model->var_names_capacity) {
            model->var_names[model->num_vars - 1] = NULL;
        }
    }

    model->num_vars--;
    model->num_elements -= removed_nnz;
    if (model->num_elements < 0) model->num_elements = 0;

    if (removed_type == 'I' || removed_type == 'B') model->num_integers--;
    if (removed_type == 'B') model->num_binary--;
    if (model->num_integers < 0) model->num_integers = 0;
    if (model->num_binary < 0) model->num_binary = 0;

    return 0;
}

/* Finalize model: build sparse matrix from constraints */
int lp_model_finalize(LPModel *model) {
    if (!model || model->A) return 0;  /* Already finalized or error */

    LPModelBuildState *bs = model->build_state;

    if (!bs || bs->con_count == 0) {
        /* No constraints added, create empty matrix */
        model->A = sparse_create(model->num_cons, model->num_vars, 0);
        build_state_free(model->build_state);
        model->build_state = NULL;
        return model->A ? 0 : -1;
    }

    /* Count total non-zeros */
    int total_nnz = 0;
    for (int i = 0; i < bs->con_count; i++) {
        total_nnz += bs->constraints[i]->nnz;
    }

    /* Build using triplet format */
    SparseTriplets *trips = triplets_create(model->num_cons, model->num_vars, total_nnz);
    if (!trips) return -1;

    for (int i = 0; i < bs->con_count; i++) {
        ConstraintEntry *entry = bs->constraints[i];
        for (int k = 0; k < entry->nnz; k++) {
            triplets_add(trips, i, entry->indices[k], entry->values[k]);
        }
    }

    model->A = triplets_to_csc(trips);
    triplets_free(trips);

    if (!model->A) return -1;

    model->num_elements = model->A->nnz;

    /* Free build state */
    build_state_free(model->build_state);
    model->build_state = NULL;

    return 0;
}

/* ============================================================================
 * Model Copy
 * ============================================================================ */

LPModel* lp_model_copy(const LPModel *src) {
    if (!src) return NULL;

    LPModel *dst = lp_model_create();
    if (!dst) return NULL;

    dst->num_vars = src->num_vars;
    dst->num_cons = src->num_cons;
    dst->num_elements = src->num_elements;
    dst->obj_sense = src->obj_sense;
    dst->obj_offset = src->obj_offset;
    dst->num_integers = src->num_integers;
    dst->num_binary = src->num_binary;

    /* Copy matrix */
    if (src->A) {
        dst->A = sparse_copy(src->A);
        if (!dst->A) goto error;
    }

    /* Copy vectors */
    if (src->num_vars > 0) {
        dst->c = (double*)calloc(src->num_vars, sizeof(double));
        dst->lb = (double*)calloc(src->num_vars, sizeof(double));
        dst->ub = (double*)calloc(src->num_vars, sizeof(double));
        dst->var_shifted = (unsigned char*)calloc(src->num_vars, sizeof(unsigned char));
        dst->var_shift = (double*)calloc(src->num_vars, sizeof(double));
        dst->var_type = (char*)calloc(src->num_vars, sizeof(char));

        if (!dst->c || !dst->lb || !dst->ub || !dst->var_shifted || !dst->var_shift ||
            !dst->var_type) goto error;

        memcpy(dst->c, src->c, src->num_vars * sizeof(double));
        memcpy(dst->lb, src->lb, src->num_vars * sizeof(double));
        memcpy(dst->ub, src->ub, src->num_vars * sizeof(double));
        if (src->var_shifted) {
            memcpy(dst->var_shifted, src->var_shifted, src->num_vars * sizeof(unsigned char));
        }
        dst->var_shifted_capacity = src->num_vars;
        if (src->var_shift) {
            memcpy(dst->var_shift, src->var_shift, src->num_vars * sizeof(double));
        }
        dst->var_shift_capacity = src->num_vars;
        memcpy(dst->var_type, src->var_type, src->num_vars * sizeof(char));
    }

    if (src->num_cons > 0) {
        dst->b = (double*)calloc(src->num_cons, sizeof(double));
        dst->sense = (char*)calloc(src->num_cons, sizeof(char));
        dst->con_origin = (int*)calloc(src->num_cons, sizeof(int));

        if (!dst->b || !dst->sense || !dst->con_origin) goto error;

        memcpy(dst->b, src->b, src->num_cons * sizeof(double));
        memcpy(dst->sense, src->sense, src->num_cons * sizeof(char));
        if (src->con_origin) {
            memcpy(dst->con_origin, src->con_origin, src->num_cons * sizeof(int));
        } else {
            for (int i = 0; i < src->num_cons; i++) dst->con_origin[i] = i;
        }
        dst->con_origin_capacity = src->num_cons;
    }

    /* Copy names if present */
    if (src->name) {
        dst->name = strdup(src->name);
        if (!dst->name) goto error;
    }

    /* Note: build_state is not copied - copy results in finalized model */

    return dst;

error:
    lp_model_free(dst);
    return NULL;
}

/* ============================================================================
 * Model Queries
 * ============================================================================ */

int lp_model_is_mip(const LPModel *model) {
    return model && model->num_integers > 0;
}

/* ============================================================================
 * Name Management
 * ============================================================================ */

int lp_model_set_var_name(LPModel *model, int var, const char *name) {
    if (!model || var < 0 || var >= model->num_vars) return -1;

    /* Allocate or grow name array if needed */
    if (!model->var_names) {
        int cap = model->num_vars > 16 ? model->num_vars : 16;
        model->var_names = (char**)calloc(cap, sizeof(char*));
        if (!model->var_names) return -1;
        model->var_names_capacity = cap;
    } else if (var >= model->var_names_capacity) {
        int new_cap = model->var_names_capacity * 2;
        if (new_cap <= var) new_cap = var + 16;
        char **new_names = (char**)realloc(model->var_names, new_cap * sizeof(char*));
        if (!new_names) return -1;
        /* Zero the new entries */
        for (int i = model->var_names_capacity; i < new_cap; i++) {
            new_names[i] = NULL;
        }
        model->var_names = new_names;
        model->var_names_capacity = new_cap;
    }

    /* Free old name if present */
    SAFE_FREE(model->var_names[var]);

    /* Set new name */
    if (name) {
        model->var_names[var] = strdup(name);
        if (!model->var_names[var]) return -1;
    }

    return 0;
}

int lp_model_set_con_name(LPModel *model, int con, const char *name) {
    if (!model || con < 0 || con >= model->num_cons) return -1;

    /* Allocate or grow name array if needed */
    if (!model->con_names) {
        int cap = model->num_cons > 16 ? model->num_cons : 16;
        model->con_names = (char**)calloc(cap, sizeof(char*));
        if (!model->con_names) return -1;
        model->con_names_capacity = cap;
    } else if (con >= model->con_names_capacity) {
        int new_cap = model->con_names_capacity * 2;
        if (new_cap <= con) new_cap = con + 16;
        char **new_names = (char**)realloc(model->con_names, new_cap * sizeof(char*));
        if (!new_names) return -1;
        /* Zero the new entries */
        for (int i = model->con_names_capacity; i < new_cap; i++) {
            new_names[i] = NULL;
        }
        model->con_names = new_names;
        model->con_names_capacity = new_cap;
    }

    /* Free old name if present */
    SAFE_FREE(model->con_names[con]);

    /* Set new name */
    if (name) {
        model->con_names[con] = strdup(name);
        if (!model->con_names[con]) return -1;
    }

    return 0;
}

const char* lp_model_get_var_name(const LPModel *model, int var) {
    if (!model || var < 0 || var >= model->num_vars) return NULL;
    if (!model->var_names) return NULL;
    return model->var_names[var];
}

const char* lp_model_get_con_name(const LPModel *model, int con) {
    if (!model || con < 0 || con >= model->num_cons) return NULL;
    if (!model->con_names) return NULL;
    return model->con_names[con];
}

int lp_model_set_name(LPModel *model, const char *name) {
    if (!model) return -1;

    SAFE_FREE(model->name);
    if (name) {
        model->name = strdup(name);
        if (!model->name) return -1;
    }

    return 0;
}

const char* lp_model_get_name(const LPModel *model) {
    return model ? model->name : NULL;
}

void lp_model_print(const LPModel *model) {
    if (!model) {
        printf("NULL model\n");
        return;
    }

    printf("LP Model: %s\n", model->name ? model->name : "(unnamed)");
    printf("  Variables: %d (%d integer, %d binary)\n",
           model->num_vars, model->num_integers, model->num_binary);
    printf("  Constraints: %d\n", model->num_cons);
    printf("  Non-zeros: %d\n", model->num_elements);
    printf("  Objective: %s\n", model->obj_sense == 1 ? "minimize" : "maximize");

    if (model->num_vars <= 10 && model->num_cons <= 10) {
        printf("\n  Objective coefficients:\n    ");
        for (int j = 0; j < model->num_vars; j++) {
            printf("%.2f ", model->c[j]);
        }
        printf("\n");

        printf("\n  Variable bounds:\n");
        for (int j = 0; j < model->num_vars; j++) {
            printf("    x%d: [%.2f, %.2f] %c\n", j, model->lb[j], model->ub[j],
                   model->var_type[j]);
        }

        if (model->A) {
            printf("\n  Constraint matrix:\n");
            sparse_print_dense(model->A, "A");
        }

        printf("\n  RHS:\n    ");
        for (int i = 0; i < model->num_cons; i++) {
            printf("%c %.2f ", model->sense[i], model->b[i]);
        }
        printf("\n");
    }
}
