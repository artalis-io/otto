#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "ralph.h"
#include "lp.h"
#include "sparse.h"

int lp_model_finalize(LPModel *model);

int main(void) {
    printf("=== Debug: Simple 2-variable LP ===\n");

    /* Build model directly using internal API */
    LPModel *model = lp_model_create();
    model->obj_sense = 1;  /* Minimize */
    
    /* Add variables: x, y with objective -1, -1 */
    lp_model_add_var(model, 0.0, 1e30, -1.0, 'C');
    lp_model_add_var(model, 0.0, 1e30, -1.0, 'C');

    /* Constraint 1: x + y <= 4 */
    int idx1[] = {0, 1};
    double val1[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx1, val1, 'L', 4.0);

    /* Constraint 2: 2x + y <= 6 */
    int idx2[] = {0, 1};
    double val2[] = {2.0, 1.0};
    lp_model_add_constraint(model, 2, idx2, val2, 'L', 6.0);

    /* Finalize */
    lp_model_finalize(model);

    printf("Model: %d vars, %d cons\n", model->num_vars, model->num_cons);
    printf("Objective coefficients: ");
    for (int j = 0; j < model->num_vars; j++) {
        printf("%.2f ", model->c[j]);
    }
    printf("\n");

    printf("Constraint matrix:\n");
    sparse_print_dense(model->A, "A");

    printf("RHS: ");
    for (int i = 0; i < model->num_cons; i++) {
        printf("%.2f ", model->b[i]);
    }
    printf("\n");
    printf("Sense: ");
    for (int i = 0; i < model->num_cons; i++) {
        printf("%c ", model->sense[i]);
    }
    printf("\n");

    /* Solve */
    SimplexSolver *solver = simplex_create(model);
    solver->verbose = 1;
    simplex_solve(solver);

    printf("\nStatus: %d\n", solver->status);
    printf("Objective: %.6f\n", solver->obj_value);
    
    if (solver->solution) {
        printf("Solution: ");
        for (int j = 0; j < model->num_vars; j++) {
            printf("x%d=%.4f ", j, solver->solution[j]);
        }
        printf("\n");
    }

    simplex_free(solver);
    lp_model_free(model);

    return 0;
}
