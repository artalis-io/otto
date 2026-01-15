#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "ralph.h"
#include "lp.h"
#include "sparse.h"

int lp_model_finalize(LPModel *model);

int main(void) {
    printf("=== Debug: Tableau creation ===\n");

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
    
    printf("Model created: %d vars, %d cons\n", model->num_vars, model->num_cons);
    printf("Original matrix A:\n");
    sparse_print(model->A, "A");
    printf("\n");

    lp_model_free(model);

    return 0;
}
