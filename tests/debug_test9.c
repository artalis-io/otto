#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lp.h"
#include "sparse.h"

int lp_model_finalize(LPModel *model);

/* Copy the key functions for step-by-step debugging */

int main(void) {
    printf("=== Debug: Step-by-step simplex ===\n");

    /* Create model */
    LPModel *model = lp_model_create();
    model->obj_sense = 1;
    
    lp_model_add_var(model, 0.0, 1e30, -1.0, 'C');
    lp_model_add_var(model, 0.0, 1e30, -1.0, 'C');

    int idx[] = {0, 1};
    double val1[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx, val1, 'L', 4.0);
    double val2[] = {2.0, 1.0};
    lp_model_add_constraint(model, 2, idx, val2, 'L', 6.0);

    lp_model_finalize(model);
    
    /* Build extended system manually */
    int m = 2;
    int n = 4;  /* 2 structural + 2 slacks */
    
    /* Extended cost: [-1, -1, 0, 0] */
    double c_ext[] = {-1.0, -1.0, 0.0, 0.0};
    
    /* Extended bounds: all >= 0 */
    double lb_ext[] = {0.0, 0.0, 0.0, 0.0};
    double ub_ext[] = {1e30, 1e30, 1e30, 1e30};
    
    /* Extended matrix A_ext */
    SparseTriplets *trips = triplets_create(2, 4, 10);
    triplets_add(trips, 0, 0, 1.0);
    triplets_add(trips, 1, 0, 2.0);
    triplets_add(trips, 0, 1, 1.0);
    triplets_add(trips, 1, 1, 1.0);
    triplets_add(trips, 0, 2, 1.0);
    triplets_add(trips, 1, 3, 1.0);
    SparseMatrix *A_ext = triplets_to_csc(trips);
    triplets_free(trips);
    
    printf("A_ext:\n");
    sparse_print_dense(A_ext, "A_ext");
    
    /* RHS */
    double rhs[] = {4.0, 6.0};
    
    /* Initial basis: columns 2, 3 (slacks) */
    int basis[] = {2, 3};
    VarStatus var_status[] = {RALPH_NONBASIC_LOWER, RALPH_NONBASIC_LOWER, 
                              RALPH_BASIC, RALPH_BASIC};
    int basis_pos[] = {-1, -1, 0, 1};
    
    /* Initial x: structural at 0, slacks at rhs */
    double x[] = {0.0, 0.0, 0.0, 0.0};  /* Will be computed */
    
    printf("\nInitial basis: [%d, %d]\n", basis[0], basis[1]);
    
    /* Extract basis matrix */
    SparseMatrix *B = sparse_get_columns(A_ext, m, basis);
    printf("Basis matrix B:\n");
    sparse_print_dense(B, "B");
    
    /* Factorize B */
    LUFactorization *lu = lu_create(m);
    int rc = lu_factorize(lu, B);
    printf("lu_factorize: %d\n", rc);
    
    /* Compute solution: x_B = B^{-1} * (b - N*x_N) */
    /* Since x_N = 0, this is just x_B = B^{-1} * b */
    double work[2];
    memcpy(work, rhs, m * sizeof(double));
    
    /* Subtract N*x_N (which is 0 since x_N = 0) */
    /* work is already rhs */
    
    double x_B[2];
    lu_solve(lu, work, x_B);
    printf("x_B = B^{-1} * b = [%.4f, %.4f]\n", x_B[0], x_B[1]);
    
    /* Update full x vector */
    x[basis[0]] = x_B[0];
    x[basis[1]] = x_B[1];
    printf("Full x = [%.4f, %.4f, %.4f, %.4f]\n", x[0], x[1], x[2], x[3]);
    
    /* Compute objective */
    double obj = 0.0;
    for (int j = 0; j < n; j++) {
        obj += c_ext[j] * x[j];
    }
    printf("Objective = %.4f\n", obj);
    
    /* Verify: Ax should equal b */
    double Ax[2] = {0, 0};
    sparse_matvec(A_ext, x, Ax);
    printf("A*x = [%.4f, %.4f], b = [%.4f, %.4f]\n", Ax[0], Ax[1], rhs[0], rhs[1]);
    
    lu_free(lu);
    sparse_free(B);
    sparse_free(A_ext);
    lp_model_free(model);
    
    return 0;
}
