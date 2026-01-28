#include <stdio.h>
#include <stdlib.h>
#include "ralph.h"
#include "lp.h"

extern SimplexTableau* tableau_create(LPModel *model);
extern void tableau_free(SimplexTableau *tab);
extern int tableau_refactorize(SimplexTableau *tab);
extern int tableau_compute_solution(SimplexTableau *tab);
extern int tableau_compute_reduced_costs(SimplexTableau *tab);
int lp_model_finalize(LPModel *model);

int main(void) {
    printf("Debug: Infeasible LP\n");
    printf("min x s.t. x <= 1, x >= 2\n");
    printf("Expected: INFEASIBLE\n\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;  /* minimize */

    lp_model_add_var(model, 0, 1e30, 1.0, 'C');  /* x >= 0 */

    int idx[] = {0};
    double val[] = {1.0};
    lp_model_add_constraint(model, 1, idx, val, 'L', 1.0);  /* x <= 1 */
    lp_model_add_constraint(model, 1, idx, val, 'G', 2.0);  /* x >= 2 */

    lp_model_finalize(model);

    SimplexTableau *tab = tableau_create(model);
    printf("Tableau created: m=%d, n=%d\n", tab->m, tab->n);

    printf("\nInitial state:\n");
    printf("c_ext: ");
    for (int j = 0; j < tab->n; j++) printf("%.0f ", tab->c_ext[j]);
    printf("\n");
    printf("lb_ext: ");
    for (int j = 0; j < tab->n; j++) printf("%.0f ", tab->lb_ext[j]);
    printf("\n");
    printf("x (initial): ");
    for (int j = 0; j < tab->n; j++) printf("%.1f ", tab->x[j]);
    printf("\n");
    printf("basis: ");
    for (int k = 0; k < tab->m; k++) printf("%d ", tab->basis[k]);
    printf("\n");
    printf("var_status: ");
    for (int j = 0; j < tab->n; j++) printf("%d ", tab->var_status[j]);
    printf("\n");
    printf("rhs: ");
    for (int i = 0; i < tab->m; i++) printf("%.1f ", tab->rhs[i]);
    printf("\n");

    /* Print A_ext */
    printf("A_ext:\n");
    for (int j = 0; j < tab->n; j++) {
        printf("  col %d: ", j);
        for (int p = tab->A_ext->colptr[j]; p < tab->A_ext->colptr[j + 1]; p++) {
            printf("(%d, %.1f) ", tab->A_ext->rowidx[p], tab->A_ext->values[p]);
        }
        printf("\n");
    }

    tableau_refactorize(tab);
    tableau_compute_solution(tab);

    printf("\nAfter compute_solution:\n");
    printf("x: ");
    for (int j = 0; j < tab->n; j++) printf("%.4f ", tab->x[j]);
    printf("\n");
    printf("obj_value: %.6f\n", tab->obj_value);

    tableau_compute_reduced_costs(tab);
    printf("rc: ");
    for (int j = 0; j < tab->n; j++) printf("%.4f ", tab->rc[j]);
    printf("\n");

    tableau_free(tab);
    lp_model_free(model);
    return 0;
}
