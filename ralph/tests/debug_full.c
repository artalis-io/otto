#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lp.h"
#include "sparse.h"

#define TOL 1e-6

int main(void) {
    printf("=== Full Manual Simplex ===\n\n");

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
    int basis_pos[] = {-1, -1, 0, 1};
    VarStatus var_status[] = {RALPH_NONBASIC_LOWER, RALPH_NONBASIC_LOWER, 
                              RALPH_BASIC, RALPH_BASIC};
    double x[] = {0.0, 0.0, 4.0, 6.0};
    
    for (int iter = 0; iter < 10; iter++) {
        printf("=== Iteration %d ===\n", iter);
        printf("Basis: [%d, %d]\n", basis[0], basis[1]);
        printf("x = [%.4f, %.4f, %.4f, %.4f]\n", x[0], x[1], x[2], x[3]);
        
        double obj = 0.0;
        for (int j = 0; j < n; j++) obj += c_ext[j] * x[j];
        printf("Objective = %.4f\n", obj);
        
        /* Build and factorize basis */
        SparseMatrix *B = sparse_get_columns(A_ext, m, basis);
        LUFactorization *lu = lu_create(m);
        lu_factorize(lu, B);
        
        /* Compute dual values */
        double c_B[] = {c_ext[basis[0]], c_ext[basis[1]]};
        double y[2];
        lu_solve_transpose(lu, c_B, y);
        
        /* Compute reduced costs and find entering variable */
        int entering = -1;
        double best_rc = -TOL;
        double work[2], rc[4];
        
        for (int j = 0; j < n; j++) {
            if (basis_pos[j] >= 0) {
                rc[j] = 0.0;
            } else {
                sparse_get_column(A_ext, j, work);
                rc[j] = c_ext[j] - (y[0]*work[0] + y[1]*work[1]);
                if (rc[j] < best_rc) {
                    best_rc = rc[j];
                    entering = j;
                }
            }
        }
        
        printf("Reduced costs: [%.4f, %.4f, %.4f, %.4f]\n", rc[0], rc[1], rc[2], rc[3]);
        
        if (entering < 0) {
            printf("\n*** OPTIMAL ***\n");
            printf("Final solution: x0=%.4f, x1=%.4f\n", x[0], x[1]);
            printf("Final objective: %.4f\n", obj);
            break;
        }
        
        printf("Entering: x%d (rc=%.4f)\n", entering, best_rc);
        
        /* Compute direction */
        sparse_get_column(A_ext, entering, work);
        double d[2];
        lu_solve(lu, work, d);
        printf("Direction d = [%.4f, %.4f]\n", d[0], d[1]);
        
        /* Ratio test */
        int leaving = -1;
        double theta = 1e30;
        
        for (int k = 0; k < m; k++) {
            if (d[k] > TOL) {
                double ratio = (x[basis[k]] - lb_ext[basis[k]]) / d[k];
                if (ratio < theta) {
                    theta = ratio;
                    leaving = k;
                }
            }
        }
        
        if (leaving < 0) {
            printf("UNBOUNDED!\n");
            break;
        }
        
        printf("Leaving: position %d (var %d), theta=%.4f\n", leaving, basis[leaving], theta);
        
        /* Pivot */
        x[entering] += theta;
        for (int k = 0; k < m; k++) {
            x[basis[k]] -= theta * d[k];
        }
        
        /* Update basis */
        int leaving_var = basis[leaving];
        basis_pos[leaving_var] = -1;
        var_status[leaving_var] = RALPH_NONBASIC_LOWER;
        x[leaving_var] = lb_ext[leaving_var];
        
        basis[leaving] = entering;
        basis_pos[entering] = leaving;
        var_status[entering] = RALPH_BASIC;
        
        printf("\n");
        
        lu_free(lu);
        sparse_free(B);
    }
    
    sparse_free(A_ext);
    return 0;
}
