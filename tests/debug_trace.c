#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lp.h"
#include "sparse.h"

int lp_model_finalize(LPModel *model);

extern SimplexTableau* tableau_create(LPModel *model);
extern void tableau_free(SimplexTableau *tab);
extern int tableau_refactorize(SimplexTableau *tab);
extern int tableau_compute_solution(SimplexTableau *tab);
extern int tableau_compute_reduced_costs(SimplexTableau *tab);
extern int pricing_steepest_edge(SimplexTableau *tab, int *entering);
extern int ratio_test_harris(SimplexTableau *tab, int entering, int *leaving, double *theta);

/* Copy initialize_slack_basis since it's static */
static int my_initialize_slack_basis(SimplexTableau *tab) {
    int m = tab->m;
    int n = tab->n;
    int num_struct = tab->model->num_vars;

    for (int j = 0; j < n; j++) {
        tab->basis_pos[j] = -1;
    }

    int basis_idx = 0;
    for (int j = num_struct; j < n && basis_idx < m; j++) {
        tab->basis[basis_idx] = j;
        tab->var_status[j] = RALPH_BASIC;
        tab->basis_pos[j] = basis_idx;
        basis_idx++;
    }

    int nonbasis_idx = 0;
    for (int j = 0; j < num_struct; j++) {
        if (tab->lb_ext[j] > -1e30/2) {
            tab->var_status[j] = RALPH_NONBASIC_LOWER;
            tab->x[j] = tab->lb_ext[j];
        } else if (tab->ub_ext[j] < 1e30/2) {
            tab->var_status[j] = RALPH_NONBASIC_UPPER;
            tab->x[j] = tab->ub_ext[j];
        } else {
            tab->var_status[j] = RALPH_NONBASIC_FREE;
            tab->x[j] = 0.0;
        }
        tab->nonbasis[nonbasis_idx++] = j;
    }

    return 0;
}

int main(void) {
    printf("=== Debug: Full Trace ===\n\n");

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

    SimplexTableau *tab = tableau_create(model);
    printf("Created tableau: m=%d, n=%d\n", tab->m, tab->n);
    printf("A_ext:\n");
    sparse_print_dense(tab->A_ext, "A_ext");
    printf("c_ext: ");
    for (int j = 0; j < tab->n; j++) printf("%.2f ", tab->c_ext[j]);
    printf("\n");
    printf("rhs: ");
    for (int i = 0; i < tab->m; i++) printf("%.2f ", tab->rhs[i]);
    printf("\n");

    my_initialize_slack_basis(tab);
    printf("\nAfter initialize_slack_basis:\n");
    printf("basis: ");
    for (int k = 0; k < tab->m; k++) printf("%d ", tab->basis[k]);
    printf("\n");
    printf("var_status: ");
    for (int j = 0; j < tab->n; j++) printf("%d ", tab->var_status[j]);
    printf("\n");
    printf("x: ");
    for (int j = 0; j < tab->n; j++) printf("%.4f ", tab->x[j]);
    printf("\n");

    int rc = tableau_refactorize(tab);
    printf("\nAfter refactorize (rc=%d):\n", rc);

    tableau_compute_solution(tab);
    printf("After compute_solution:\n");
    printf("x: ");
    for (int j = 0; j < tab->n; j++) printf("%.4f ", tab->x[j]);
    printf("\n");
    printf("obj_value: %.6f\n", tab->obj_value);

    tableau_compute_reduced_costs(tab);
    printf("\nAfter compute_reduced_costs:\n");
    printf("y (dual): ");
    for (int i = 0; i < tab->m; i++) printf("%.4f ", tab->y[i]);
    printf("\n");
    printf("rc: ");
    for (int j = 0; j < tab->n; j++) printf("%.4f ", tab->rc[j]);
    printf("\n");

    int entering;
    int price_status = pricing_steepest_edge(tab, &entering);
    printf("\nPricing: status=%d, entering=%d\n", price_status, entering);

    if (entering >= 0) {
        int leaving;
        double theta;
        int ratio_status = ratio_test_harris(tab, entering, &leaving, &theta);
        printf("Ratio test: status=%d, leaving=%d, theta=%.4f\n", 
               ratio_status, leaving, theta);
        printf("work2 (direction): ");
        for (int k = 0; k < tab->m; k++) printf("%.4f ", tab->work2[k]);
        printf("\n");
    }

    tableau_free(tab);
    lp_model_free(model);

    return 0;
}
