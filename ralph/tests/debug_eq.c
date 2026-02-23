#include <stdio.h>
#include <stdlib.h>
#include "ralph_test_mod_api.h"
#include "lp.h"

extern SimplexTableau* tableau_create(LPModel *model);
extern void tableau_free(SimplexTableau *tab);
extern int tableau_refactorize(SimplexTableau *tab);
extern int tableau_compute_solution(SimplexTableau *tab);
extern int tableau_compute_reduced_costs(SimplexTableau *tab);
int lp_model_finalize(LPModel *model);

int main(void) {
    printf("Debug: Equality constraint\n");
    printf("min x + 2y s.t. x + y = 3, x >= 1, y >= 0\n");
    printf("Expected: x=1, y=2, obj=5\n\n");

    LPModel *model = lp_model_create();
    model->obj_sense = 1;  /* minimize */

    lp_model_add_var(model, 1.0, 1e30, 1.0, 'C');  /* x >= 1 */
    lp_model_add_var(model, 0.0, 1e30, 2.0, 'C');  /* y >= 0 */

    int idx[] = {0, 1};
    double val[] = {1.0, 1.0};
    lp_model_add_constraint(model, 2, idx, val, 'E', 3.0);  /* x + y = 3 */

    lp_model_finalize(model);

    SimplexTableau *tab = tableau_create(model);
    printf("Tableau created: m=%d, n=%d\n", tab->m, tab->n);

    printf("\nInitial state:\n");
    printf("c_ext: ");
    for (int j = 0; j < tab->n; j++) printf("%.1f ", tab->c_ext[j]);
    printf("\n");
    printf("lb_ext: ");
    for (int j = 0; j < tab->n; j++) printf("%.1f ", tab->lb_ext[j]);
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
