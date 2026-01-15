#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lp.h"
#include "sparse.h"

int main(void) {
    printf("=== Debug: LU Update ===\n\n");

    /* Initial basis matrix: columns 2, 3 of A_ext = identity */
    /* After pivot: basis = [2, 0], so B = columns [2, 0] */
    
    /* A_ext = [[1, 1, 1, 0], [2, 1, 0, 1]] */
    /* Column 0: [1, 2], Column 2: [1, 0] */
    /* New B = [[1, 1], [0, 2]] */
    
    printf("Testing LU solve with new basis B = [[1, 1], [0, 2]]\n");
    
    /* Create B directly */
    double B_dense[] = {1.0, 0.0, 1.0, 2.0};  /* Column-major */
    SparseMatrix *B = sparse_create_from_dense(2, 2, B_dense);
    printf("B:\n");
    sparse_print_dense(B, "B");
    
    LUFactorization *lu = lu_create(2);
    int rc = lu_factorize(lu, B);
    printf("lu_factorize: %d\n", rc);
    
    /* Solve B * x = b where b = [4, 6] */
    double b[] = {4.0, 6.0};
    double x[2];
    lu_solve(lu, b, x);
    printf("Solving B*x = [4, 6]\n");
    printf("x = [%.4f, %.4f]\n", x[0], x[1]);
    printf("Expected: x[0]=1 (for col 2), x[1]=3 (for col 0)\n");
    
    /* So in terms of original variables: x[2]=1, x[0]=3 */
    
    /* Verify */
    double Bx[2] = {B_dense[0]*x[0] + B_dense[2]*x[1], 
                    B_dense[1]*x[0] + B_dense[3]*x[1]};
    printf("B*x = [%.4f, %.4f]\n", Bx[0], Bx[1]);
    
    lu_free(lu);
    sparse_free(B);
    
    printf("\n--- Now test LU update ---\n");
    
    /* Start with initial basis B0 = identity (columns 2, 3) */
    double B0_dense[] = {1.0, 0.0, 0.0, 1.0};
    SparseMatrix *B0 = sparse_create_from_dense(2, 2, B0_dense);
    printf("Initial B0 = I:\n");
    sparse_print_dense(B0, "B0");
    
    LUFactorization *lu0 = lu_create(2);
    lu_factorize(lu0, B0);
    
    /* Column 0 of A_ext that we're adding to basis position 1 */
    double entering_col[] = {1.0, 2.0};
    printf("Entering column (col 0 of A_ext): [%.1f, %.1f]\n", 
           entering_col[0], entering_col[1]);
    printf("Leaving position: 1 (column 3 of A_ext)\n");
    
    /* Update LU */
    rc = lu_update(lu0, 1, entering_col);
    printf("lu_update returned: %d\n", rc);
    
    /* Now solve with updated LU */
    printf("\nSolving with updated LU:\n");
    lu_solve(lu0, b, x);
    printf("x = [%.4f, %.4f]\n", x[0], x[1]);
    printf("Expected: same as above [1, 3]\n");
    
    lu_free(lu0);
    sparse_free(B0);
    
    return 0;
}
