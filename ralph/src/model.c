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

    return model;
}

void lp_model_free(LPModel *model) {
    if (!model) return;

    sparse_free(model->A);
    free(model->c);
    free(model->b);
    free(model->sense);
    free(model->lb);
    free(model->ub);
    free(model->var_type);

    if (model->var_names) {
        for (int i = 0; i < model->num_vars; i++) {
            free(model->var_names[i]);
        }
        free(model->var_names);
    }

    if (model->con_names) {
        for (int i = 0; i < model->num_cons; i++) {
            free(model->con_names[i]);
        }
        free(model->con_names);
    }

    free(model->name);
    free(model);
}

/* ============================================================================
 * Model Building (using triplet format for incremental building)
 * ============================================================================ */

/* Internal structure for building model incrementally */
typedef struct {
    int var_capacity;
    int con_capacity;
    SparseTriplets *triplets;
} ModelBuilder;

static ModelBuilder* builder_create(void) {
    ModelBuilder *builder = (ModelBuilder*)calloc(1, sizeof(ModelBuilder));
    if (!builder) return NULL;

    builder->var_capacity = INITIAL_VAR_CAPACITY;
    builder->con_capacity = INITIAL_CON_CAPACITY;
    builder->triplets = triplets_create(INITIAL_CON_CAPACITY, INITIAL_VAR_CAPACITY,
                                        INITIAL_NNZ_CAPACITY);

    if (!builder->triplets) {
        free(builder);
        return NULL;
    }

    return builder;
}

/* ============================================================================
 * Model Building via Public API
 * ============================================================================ */

/* We use a simpler approach: store constraints in a list first, then build matrix */

typedef struct {
    int nnz;
    int capacity;
    int *indices;
    double *values;
    char sense;
    double rhs;
} ConstraintEntry;

typedef struct {
    int num_vars;
    int var_capacity;
    double *c;
    double *lb;
    double *ub;
    char *var_type;

    int num_cons;
    int con_capacity;
    ConstraintEntry **constraints;

    int obj_sense;
    double obj_offset;
} ModelBuildState;

/* Thread-local or per-model build state - we'll attach to model via A pointer being NULL */

int lp_model_add_var(LPModel *model, double lb, double ub, double obj, char type) {
    if (!model) return -1;

    int idx = model->num_vars;
    int new_capacity = model->num_vars + 1;

    /* Reallocate arrays */
    double *new_c = (double*)realloc(model->c, new_capacity * sizeof(double));
    double *new_lb = (double*)realloc(model->lb, new_capacity * sizeof(double));
    double *new_ub = (double*)realloc(model->ub, new_capacity * sizeof(double));
    char *new_type = (char*)realloc(model->var_type, new_capacity * sizeof(char));

    if (!new_c || !new_lb || !new_ub || !new_type) {
        return -1;
    }

    model->c = new_c;
    model->lb = new_lb;
    model->ub = new_ub;
    model->var_type = new_type;

    model->c[idx] = obj;
    model->lb[idx] = lb;
    model->ub[idx] = ub;
    model->var_type[idx] = type;

    if (type == 'I' || type == 'B') {
        model->num_integers++;
        if (type == 'B') model->num_binary++;
    }

    model->num_vars++;
    return idx;
}

/* Temporary storage for constraints during model building */
static ConstraintEntry** temp_constraints = NULL;
static int temp_con_count = 0;
static int temp_con_capacity = 0;
static LPModel* temp_model = NULL;

int lp_model_add_constraint(LPModel *model, int nnz, const int *indices,
                            const double *values, char sense, double rhs) {
    if (!model) return -1;

    /* Initialize temporary storage if needed */
    if (temp_model != model) {
        /* Free old temp storage */
        if (temp_constraints) {
            for (int i = 0; i < temp_con_count; i++) {
                if (temp_constraints[i]) {
                    free(temp_constraints[i]->indices);
                    free(temp_constraints[i]->values);
                    free(temp_constraints[i]);
                }
            }
            free(temp_constraints);
        }
        temp_constraints = NULL;
        temp_con_count = 0;
        temp_con_capacity = 0;
        temp_model = model;
    }

    /* Expand if needed */
    if (temp_con_count >= temp_con_capacity) {
        int new_cap = temp_con_capacity == 0 ? 64 : temp_con_capacity * 2;
        ConstraintEntry **new_cons = (ConstraintEntry**)realloc(temp_constraints,
                                                                 new_cap * sizeof(ConstraintEntry*));
        if (!new_cons) return -1;
        temp_constraints = new_cons;
        temp_con_capacity = new_cap;
    }

    /* Create new constraint entry */
    ConstraintEntry *entry = (ConstraintEntry*)malloc(sizeof(ConstraintEntry));
    if (!entry) return -1;

    entry->nnz = nnz;
    entry->capacity = nnz;
    entry->indices = (int*)malloc(nnz * sizeof(int));
    entry->values = (double*)malloc(nnz * sizeof(double));
    entry->sense = sense;
    entry->rhs = rhs;

    if (!entry->indices || !entry->values) {
        free(entry->indices);
        free(entry->values);
        free(entry);
        return -1;
    }

    memcpy(entry->indices, indices, nnz * sizeof(int));
    memcpy(entry->values, values, nnz * sizeof(double));

    temp_constraints[temp_con_count] = entry;

    /* Update model's RHS and sense arrays */
    int idx = model->num_cons;
    int new_capacity = model->num_cons + 1;

    double *new_b = (double*)realloc(model->b, new_capacity * sizeof(double));
    char *new_sense = (char*)realloc(model->sense, new_capacity * sizeof(char));

    if (!new_b || !new_sense) return -1;

    model->b = new_b;
    model->sense = new_sense;
    model->b[idx] = rhs;
    model->sense[idx] = sense;

    model->num_cons++;
    temp_con_count++;

    return idx;
}

/* Finalize model: build sparse matrix from constraints */
int lp_model_finalize(LPModel *model) {
    if (!model || model->A) return 0;  /* Already finalized or error */

    if (temp_model != model || temp_con_count == 0) {
        /* No constraints added, create empty matrix */
        model->A = sparse_create(model->num_cons, model->num_vars, 0);
        return model->A ? 0 : -1;
    }

    /* Count total non-zeros */
    int total_nnz = 0;
    for (int i = 0; i < temp_con_count; i++) {
        total_nnz += temp_constraints[i]->nnz;
    }

    /* Build using triplet format */
    SparseTriplets *trips = triplets_create(model->num_cons, model->num_vars, total_nnz);
    if (!trips) return -1;

    for (int i = 0; i < temp_con_count; i++) {
        ConstraintEntry *entry = temp_constraints[i];
        for (int k = 0; k < entry->nnz; k++) {
            triplets_add(trips, i, entry->indices[k], entry->values[k]);
        }
    }

    model->A = triplets_to_csc(trips);
    triplets_free(trips);

    if (!model->A) return -1;

    model->num_elements = model->A->nnz;

    /* Free temporary storage */
    for (int i = 0; i < temp_con_count; i++) {
        free(temp_constraints[i]->indices);
        free(temp_constraints[i]->values);
        free(temp_constraints[i]);
    }
    free(temp_constraints);
    temp_constraints = NULL;
    temp_con_count = 0;
    temp_con_capacity = 0;
    temp_model = NULL;

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
        dst->c = (double*)malloc(src->num_vars * sizeof(double));
        dst->lb = (double*)malloc(src->num_vars * sizeof(double));
        dst->ub = (double*)malloc(src->num_vars * sizeof(double));
        dst->var_type = (char*)malloc(src->num_vars * sizeof(char));

        if (!dst->c || !dst->lb || !dst->ub || !dst->var_type) goto error;

        memcpy(dst->c, src->c, src->num_vars * sizeof(double));
        memcpy(dst->lb, src->lb, src->num_vars * sizeof(double));
        memcpy(dst->ub, src->ub, src->num_vars * sizeof(double));
        memcpy(dst->var_type, src->var_type, src->num_vars * sizeof(char));
    }

    if (src->num_cons > 0) {
        dst->b = (double*)malloc(src->num_cons * sizeof(double));
        dst->sense = (char*)malloc(src->num_cons * sizeof(char));

        if (!dst->b || !dst->sense) goto error;

        memcpy(dst->b, src->b, src->num_cons * sizeof(double));
        memcpy(dst->sense, src->sense, src->num_cons * sizeof(char));
    }

    /* Copy names if present */
    if (src->name) {
        dst->name = strdup(src->name);
    }

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
