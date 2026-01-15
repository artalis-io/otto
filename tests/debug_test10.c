#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lp.h"
#include "sparse.h"

int main(void) {
    printf("=== Debug: Pricing and Pivot ===\n");

    int m = 2, n = 4;
    double c_ext[] = {-1.0, -1.0, 0.0, 0.0};
    double lb_ext[] = {0.0, 0.0, 0.0, 0.0};
    double ub_ext[] = {1e30, 1e30, 1e30, 1e30};
    double rhs[] = {4.0, 6.0};
    
    SparseTriplets *trips = triplets_create(2, 4, 10);
    triplets_add(trips, 0, 0, 1.0);
    triplets_add(trips, 1, 0, 2.0);
    triplets_add(trips, 0, 1, 1.0);
    triplets_add(trips, 1, 1, 1.0);
    triplets_add(trips, 0, 2, 1.0);
    triplets_add(trips, 1, 3, 1.0);
    SparseMatrix *A_ext = triplets_to_csc(trips);
    triplets_free(trips);
    
    int basis[] = {2, 3};
    VarStatus var_status[] = {RALPH_NONBASIC_LOWER, RALPH_NONBASIC_LOWER, 
                              RALPH_BASIC, RALPH_BASIC};
    double x[] = {0.0, 0.0, 4.0, 6.0};
    
    /* Get basis matrix and factorize */
    SparseMatrix *B = sparse_get_columns(A_ext, m, basis);
    LUFactorization *lu = lu_create(m);
    lu_factorize(lu, B);
    
    /* Compute dual values: y = B^{-T} * c_B */
    double c_B[2];
    c_B[0] = c_ext[basis[0]];  /* c[2] = 0 */
    c_B[1] = c_ext[basis[1]];  /* c[3] = 0 */
    printf("c_B = [%.2f, %.2f]\n", c_B[0], c_B[1]);
    
    double y[2];
    lu_solve_transpose(lu, c_B, y);
    printf("y = B^{-T} * c_B = [%.4f, %.4f]\n", y[0], y[1]);
    
    /* Compute reduced costs: rc_j = c_j - y' * A_j */
    printf("\nReduced costs:\n");
    double work[2];
    for (int j = 0; j < n; j++) {
        if (var_status[j] == RALPH_BASIC) {
            printf("  rc[%d] = 0 (basic)\n", j);
        } else {
            sparse_get_column(A_ext, j, work);
            double rc = c_ext[j] - (y[0]*work[0] + y[1]*work[1]);
            printf("  rc[%d] = %.4f - (%.4f*%.4f + %.4f*%.4f) = %.4f\n",
                   j, c_ext[j], y[0], work[0], y[1], work[1], rc);
        }
    }
    
    /* x0 and x1 both have negative reduced costs (-1 each), so either can enter */
    /* Let's try entering x0 */
    int entering = 0;
    printf("\nEntering variable: x%d\n", entering);
    
    /* Compute entering column in basis representation: d = B^{-1} * a_entering */
    sparse_get_column(A_ext, entering, work);
    printf("Column a_%d = [%.2f, %.2f]\n", entering, work[0], work[1]);
    
    double d[2];
    lu_solve(lu, work, d);
    printf("d = B^{-1} * a_%d = [%.4f, %.4f]\n", entering, d[0], d[1]);
    
    /* Ratio test */
    printf("\nRatio test:\n");
    int leaving = -1;
    double theta = 1e30;
    
    for (int k = 0; k < m; k++) {
        double dk = d[k];
        int basic_var = basis[k];
        double x_basic = x[basic_var];
        
        printf("  k=%d: basic var=%d, x=%.4f, d=%.4f", k, basic_var, x_basic, dk);
        
        if (dk > 1e-10) {
            /* Variable will decrease */
            double ratio = (x_basic - lb_ext[basic_var]) / dk;
            printf(" -> ratio = %.4f", ratio);
            if (ratio < theta) {
                theta = ratio;
                leaving = k;
            }
        }
        printf("\n");
    }
    
    printf("\nLeaving: position %d (var %d), theta = %.4f\n", leaving, basis[leaving], theta);
    
    /* Perform pivot */
    printf("\nPivot:\n");
    printf("  x[%d] (entering) goes from %.4f to %.4f\n", entering, x[entering], x[entering] + theta);
    for (int k = 0; k < m; k++) {
        int bv = basis[k];
        printf("  x[%d] (basic) goes from %.4f to %.4f\n", bv, x[bv], x[bv] - theta * d[k]);
    }
    
    /* Update x */
    x[entering] += theta;
    for (int k = 0; k < m; k++) {
        x[basis[k]] -= theta * d[k];
    }
    
    printf("\nAfter pivot: x = [%.4f, %.4f, %.4f, %.4f]\n", x[0], x[1], x[2], x[3]);
    
    /* Verify Ax = b */
    double Ax[2] = {0, 0};
    sparse_matvec(A_ext, x, Ax);
    printf("A*x = [%.4f, %.4f], b = [%.4f, %.4f]\n", Ax[0], Ax[1], rhs[0], rhs[1]);
    
    /* New objective */
    double obj = 0.0;
    for (int j = 0; j < n; j++) {
        obj += c_ext[j] * x[j];
    }
    printf("New objective = %.4f\n", obj);
    
    lu_free(lu);
    sparse_free(B);
    sparse_free(A_ext);
    
    return 0;
}
