#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "ralph.h"
#include "lp.h"
#include "sparse.h"

int lp_model_finalize(LPModel *model);

/* Extern from simplex.c - note: these are static, so we need to copy the code */

int main(void) {
    printf("=== Debug: Full simplex_create + solve ===\n");

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

    /* Use high-level API */
    SimplexSolver *solver = simplex_create(model);
    if (!solver) {
        printf("simplex_create failed\n");
        lp_model_free(model);
        return 1;
    }
    
    solver->verbose = 1;
    
    printf("\nCalling simplex_solve...\n");
    int rc = simplex_solve(solver);
    printf("simplex_solve returned: %d\n", rc);
    
    printf("Status: %d\n", solver->status);
    printf("Objective: %.6f\n", solver->obj_value);
    printf("Iterations: %d\n", solver->iterations);
    
    if (solver->tableau) {
        printf("\nTableau state:\n");
        printf("  m=%d, n=%d\n", solver->tableau->m, solver->tableau->n);
        printf("  Basis: ");
        for (int k = 0; k < solver->tableau->m; k++) {
            printf("%d ", solver->tableau->basis[k]);
        }
        printf("\n");
        printf("  x = ");
        for (int j = 0; j < solver->tableau->n; j++) {
            printf("%.4f ", solver->tableau->x[j]);
        }
        printf("\n");
    }
    
    if (solver->solution) {
        printf("\nSolution: ");
        for (int j = 0; j < model->num_vars; j++) {
            printf("x%d=%.4f ", j, solver->solution[j]);
        }
        printf("\n");
    }
    
    simplex_free(solver);
    lp_model_free(model);
    
    return 0;
}
