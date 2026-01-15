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
extern int lu_update(LUFactorization *lu, int leaving_pos, const double *entering_col);
extern int lu_needs_refactorization(const LUFactorization *lu);

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

static int my_simplex_pivot(SimplexTableau *tab, int entering, int leaving_pos, double theta) {
    printf("  Pivot: entering=%d, leaving_pos=%d, theta=%.4f\n", entering, leaving_pos, theta);
    
    double dir = (tab->var_status[entering] == RALPH_NONBASIC_UPPER) ? -1.0 : 1.0;
    printf("  dir=%.1f\n", dir);
    
    /* Update entering variable */
    double old_entering = tab->x[entering];
    if (tab->var_status[entering] == RALPH_NONBASIC_LOWER) {
        tab->x[entering] += theta;
    } else {
        tab->x[entering] -= theta;
    }
    printf("  x[%d]: %.4f -> %.4f\n", entering, old_entering, tab->x[entering]);

    /* Update basic variables */
    printf("  Updating basic vars using work2:\n");
    for (int k = 0; k < tab->m; k++) {
        int bv = tab->basis[k];
        double old_val = tab->x[bv];
        tab->x[bv] -= theta * dir * tab->work2[k];
        printf("    x[%d]: %.4f - %.4f*%.1f*%.4f = %.4f\n", 
               bv, old_val, theta, dir, tab->work2[k], tab->x[bv]);
    }

    if (leaving_pos == -2) {
        /* Bound flip */
        if (tab->var_status[entering] == RALPH_NONBASIC_LOWER) {
            tab->var_status[entering] = RALPH_NONBASIC_UPPER;
            tab->x[entering] = tab->ub_ext[entering];
        } else {
            tab->var_status[entering] = RALPH_NONBASIC_LOWER;
            tab->x[entering] = tab->lb_ext[entering];
        }
        return 0;
    }

    /* Normal pivot: swap entering and leaving */
    int leaving = tab->basis[leaving_pos];
    printf("  Swapping: leaving var %d, entering var %d\n", leaving, entering);

    /* Update basis */
    tab->basis[leaving_pos] = entering;
    tab->basis_pos[entering] = leaving_pos;
    tab->basis_pos[leaving] = -1;

    tab->var_status[entering] = RALPH_BASIC;

    /* Leaving goes to appropriate bound */
    if (tab->work2[leaving_pos] * dir > 0) {
        tab->var_status[leaving] = RALPH_NONBASIC_LOWER;
        tab->x[leaving] = tab->lb_ext[leaving];
    } else {
        tab->var_status[leaving] = RALPH_NONBASIC_UPPER;
        tab->x[leaving] = tab->ub_ext[leaving];
    }
    printf("  leaving var %d set to x=%.4f (status=%d)\n", 
           leaving, tab->x[leaving], tab->var_status[leaving]);

    /* Update LU factorization */
    sparse_get_column(tab->A_ext, entering, tab->work1);
    if (lu_update(tab->lu, leaving_pos, tab->work1) != 0) {
        printf("  LU update failed, refactorizing\n");
        if (tableau_refactorize(tab) != 0) {
            return -1;
        }
    }

    return 0;
}

int main(void) {
    printf("=== Debug: Step-by-step solve ===\n\n");

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
    my_initialize_slack_basis(tab);
    tableau_refactorize(tab);

    for (int iter = 0; iter < 10; iter++) {
        printf("=== Iteration %d ===\n", iter);
        
        tableau_compute_solution(tab);
        printf("x: ");
        for (int j = 0; j < tab->n; j++) printf("%.4f ", tab->x[j]);
        printf("\n");
        printf("obj: %.4f\n", tab->obj_value);
        printf("basis: ");
        for (int k = 0; k < tab->m; k++) printf("%d ", tab->basis[k]);
        printf("\n");

        tableau_compute_reduced_costs(tab);
        printf("rc: ");
        for (int j = 0; j < tab->n; j++) printf("%.4f ", tab->rc[j]);
        printf("\n");

        int entering;
        int price_status = pricing_steepest_edge(tab, &entering);
        printf("pricing: status=%d, entering=%d\n", price_status, entering);

        if (price_status != 0) {
            printf("\n*** OPTIMAL ***\n");
            printf("Final x: [%.4f, %.4f]\n", tab->x[0], tab->x[1]);
            printf("Final obj: %.4f\n", tab->obj_value);
            break;
        }

        int leaving;
        double theta;
        int ratio_status = ratio_test_harris(tab, entering, &leaving, &theta);
        printf("ratio_test: status=%d, leaving=%d, theta=%.4f\n", 
               ratio_status, leaving, theta);
        printf("work2 (direction): ");
        for (int k = 0; k < tab->m; k++) printf("%.4f ", tab->work2[k]);
        printf("\n");

        if (ratio_status != 0) {
            printf("UNBOUNDED!\n");
            break;
        }

        my_simplex_pivot(tab, entering, leaving, theta);
        printf("\n");

        if (lu_needs_refactorization(tab->lu)) {
            printf("Refactorizing...\n");
            tableau_refactorize(tab);
        }
    }

    tableau_free(tab);
    lp_model_free(model);

    return 0;
}
