#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "lp.h"
#include "sparse.h"

int main(void) {
    printf("=== Debug: LU factorization ===\n");

    /* Create a 2x2 identity matrix (the basis when slacks are basic) */
    double dense[] = {1.0, 0.0, 0.0, 1.0};
    SparseMatrix *B = sparse_create_from_dense(2, 2, dense);
    
    printf("Basis matrix B:\n");
    sparse_print_dense(B, "B");
    
    /* Factorize */
    LUFactorization *lu = lu_create(2);
    if (!lu) {
        printf("lu_create failed\n");
        sparse_free(B);
        return 1;
    }
    
    int rc = lu_factorize(lu, B);
    printf("lu_factorize returned: %d\n", rc);
    
    /* Test solving B*x = b */
    double b[] = {4.0, 6.0};
    double x[2];
    
    lu_solve(lu, b, x);
    printf("Solving B*x = [4, 6]\n");
    printf("Solution x = [%.4f, %.4f]\n", x[0], x[1]);
    printf("Expected: [4, 6]\n");
    
    /* Verify */
    double Bx[2] = {0, 0};
    for (int j = 0; j < 2; j++) {
        for (int p = B->colptr[j]; p < B->colptr[j + 1]; p++) {
            Bx[B->rowidx[p]] += B->values[p] * x[j];
        }
    }
    printf("B*x = [%.4f, %.4f]\n", Bx[0], Bx[1]);
    
    lu_free(lu);
    sparse_free(B);
    
    return 0;
}
