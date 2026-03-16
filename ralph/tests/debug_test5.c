#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "ralph_test_mod_api.h"
#include "lp.h"
#include "sparse.h"

int lp_model_finalize(LPModel *model);

/* Simplified version of tableau_create for debugging */
int main(void) {
    printf("=== Debug: tableau_create ===\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;
    
    lp_model_add_var(model, 0.0, 1e30, -1.0, 'C');
    lp_model_add_var(model, 0.0, 1e30, -1.0, 'C');

    int idx1[] = {0, 1};
    double val1[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx1, val1, 'L', 4.0);

    int idx2[] = {0, 1};
    double val2[] = {2.0, 1.0};
    lp_model_add_constraint(model, 2, idx2, val2, 'L', 6.0);

    lp_model_finalize(model);
    
    printf("Model: %d vars, %d cons\n", model->num_vars, model->num_cons);
    printf("A matrix: %d rows, %d cols, %d nnz\n", 
           model->A->nrows, model->A->ncols, model->A->nnz);

    /* Now simulate tableau creation */
    int m = model->num_cons;  /* 2 */
    int num_slacks = m;       /* One per constraint */
    int n = model->num_vars + num_slacks;  /* 4 */
    
    printf("Creating extended matrix with m=%d, n=%d\n", m, n);
    
    SparseTriplets *trips = triplets_create(m, n, model->A->nnz + num_slacks);
    printf("Triplets created: nrows=%d, ncols=%d\n", trips->nrows, trips->ncols);
    
    /* Copy original matrix */
    printf("Copying original matrix columns...\n");
    for (int j = 0; j < model->num_vars; j++) {
        printf("  Column %d: ", j);
        for (int p = model->A->colptr[j]; p < model->A->colptr[j + 1]; p++) {
            int row = model->A->rowidx[p];
            double val = model->A->values[p];
            printf("(%d,%.1f) ", row, val);
            triplets_add(trips, row, j, val);
        }
        printf("\n");
    }
    
    /* Add slack variables */
    printf("Adding slack variables...\n");
    int slack_idx = model->num_vars;
    for (int i = 0; i < model->num_cons; i++) {
        printf("  Constraint %d (sense=%c): slack at col %d\n", 
               i, model->sense[i], slack_idx);
        triplets_add(trips, i, slack_idx, 1.0);
        slack_idx++;
    }
    
    printf("Triplets: nnz=%d\n", trips->nnz);
    
    /* Convert to CSC */
    SparseMatrix *A_ext = triplets_to_csc(trips);
    if (!A_ext) {
        printf("triplets_to_csc returned NULL!\n");
        triplets_free(trips);
        lp_model_free(model);
        return 1;
    }
    
    printf("A_ext: %d rows, %d cols, %d nnz\n", 
           A_ext->nrows, A_ext->ncols, A_ext->nnz);
    sparse_print_dense(A_ext, "A_ext");
    
    /* Test basis extraction */
    int basis[] = {2, 3};
    printf("\nExtracting basis columns [%d, %d]...\n", basis[0], basis[1]);
    
    SparseMatrix *B = sparse_get_columns(A_ext, m, basis);
    if (B) {
        printf("Basis matrix B:\n");
        sparse_print_dense(B, "B");
        sparse_free(B);
    } else {
        printf("sparse_get_columns failed!\n");
    }
    
    sparse_free(A_ext);
    triplets_free(trips);
    lp_model_free(model);
    
    return 0;
}
