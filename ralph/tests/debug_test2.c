#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "ralph.h"
#include "lp.h"
#include "sparse.h"

int lp_model_finalize(LPModel *model);

/* External declarations for debugging */
extern SimplexTableau* tableau_create(LPModel *model);
extern int tableau_refactorize(SimplexTableau *tab);
extern int tableau_compute_solution(SimplexTableau *tab);
extern int tableau_compute_reduced_costs(SimplexTableau *tab);
extern void tableau_free(SimplexTableau *tab);

int main(void) {
    printf("=== Debug: Tableau internals ===\n");

    /* Build model directly using internal API */
    LPModel *model = lp_model_create();
    model->obj_sense = 1;  /* Minimize */
    
    lp_model_add_var(model, 0.0, 1e30, -1.0, 'C');
    lp_model_add_var(model, 0.0, 1e30, -1.0, 'C');

    int idx1[] = {0, 1};
    double val1[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx1, val1, 'L', 4.0);

    int idx2[] = {0, 1};
    double val2[] = {2.0, 1.0};
    lp_model_add_constraint(model, 2, idx2, val2, 'L', 6.0);

    lp_model_finalize(model);

    /* Create tableau */
    SimplexTableau *tab = tableau_create(model);
    if (!tab) {
        printf("Failed to create tableau\n");
        return 1;
    }

    printf("Tableau: n=%d, m=%d\n", tab->n, tab->m);
    printf("Extended objective c_ext: ");
    for (int j = 0; j < tab->n; j++) {
        printf("%.2f ", tab->c_ext[j]);
    }
    printf("\n");

    printf("Extended matrix A_ext:\n");
    sparse_print_dense(tab->A_ext, "A_ext");

    printf("RHS: ");
    for (int i = 0; i < tab->m; i++) {
        printf("%.2f ", tab->rhs[i]);
    }
    printf("\n");

    printf("Initial basis: ");
    for (int k = 0; k < tab->m; k++) {
        printf("%d ", tab->basis[k]);
    }
    printf("\n");

    printf("Initial x values: ");
    for (int j = 0; j < tab->n; j++) {
        printf("%.2f ", tab->x[j]);
    }
    printf("\n");

    printf("Variable status: ");
    for (int j = 0; j < tab->n; j++) {
        printf("%d ", tab->var_status[j]);
    }
    printf("\n");

    /* Refactorize basis */
    int rc = tableau_refactorize(tab);
    printf("Refactorize returned: %d\n", rc);

    /* Compute solution */
    tableau_compute_solution(tab);
    printf("After compute_solution, x = ");
    for (int j = 0; j < tab->n; j++) {
        printf("%.4f ", tab->x[j]);
    }
    printf("\n");
    printf("Objective = %.4f\n", tab->obj_value);

    /* Compute reduced costs */
    tableau_compute_reduced_costs(tab);
    printf("Reduced costs: ");
    for (int j = 0; j < tab->n; j++) {
        printf("%.4f ", tab->rc[j]);
    }
    printf("\n");

    tableau_free(tab);
    lp_model_free(model);

    return 0;
}
