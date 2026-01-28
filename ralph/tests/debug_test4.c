#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "ralph.h"
#include "lp.h"
#include "sparse.h"

int lp_model_finalize(LPModel *model);

int main(void) {
    printf("=== Debug: triplets_to_csc ===\n");

    /* Create triplets manually */
    SparseTriplets *trips = triplets_create(2, 4, 10);
    printf("Created triplets: nrows=%d, ncols=%d\n", trips->nrows, trips->ncols);
    
    /* Add entries for original matrix (2x2) */
    triplets_add(trips, 0, 0, 1.0);  /* A[0,0] = 1 */
    triplets_add(trips, 1, 0, 2.0);  /* A[1,0] = 2 */
    triplets_add(trips, 0, 1, 1.0);  /* A[0,1] = 1 */
    triplets_add(trips, 1, 1, 1.0);  /* A[1,1] = 1 */
    
    /* Add slack variables */
    triplets_add(trips, 0, 2, 1.0);  /* s1 in row 0 */
    triplets_add(trips, 1, 3, 1.0);  /* s2 in row 1 */
    
    printf("Triplets added: nnz=%d\n", trips->nnz);
    
    /* Convert to CSC */
    SparseMatrix *mat = triplets_to_csc(trips);
    if (!mat) {
        printf("triplets_to_csc returned NULL\n");
        triplets_free(trips);
        return 1;
    }
    
    printf("CSC matrix: nrows=%d, ncols=%d, nnz=%d\n", 
           mat->nrows, mat->ncols, mat->nnz);
    
    printf("colptr: ");
    for (int j = 0; j <= mat->ncols; j++) {
        printf("%d ", mat->colptr[j]);
    }
    printf("\n");
    
    sparse_print_dense(mat, "A_ext");
    
    /* Now test sparse_get_columns with basis indices */
    int basis[] = {2, 3};
    printf("\nTrying to get columns 2 and 3...\n");
    
    SparseMatrix *B = sparse_get_columns(mat, 2, basis);
    if (!B) {
        printf("sparse_get_columns returned NULL\n");
    } else {
        printf("Basis matrix:\n");
        sparse_print_dense(B, "B");
        sparse_free(B);
    }
    
    sparse_free(mat);
    triplets_free(trips);
    
    return 0;
}
