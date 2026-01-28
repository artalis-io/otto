#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "ralph.h"
#include "lp.h"
#include "sparse.h"

int lp_model_finalize(LPModel *model);

/* Extern from simplex.c */
extern SimplexTableau* tableau_create(LPModel *model);
extern void tableau_free(SimplexTableau *tab);

int main(void) {
    printf("=== Debug: Full tableau_create ===\n");

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

    /* Call tableau_create */
    SimplexTableau *tab = tableau_create(model);
    
    if (!tab) {
        printf("tableau_create returned NULL\n");
        lp_model_free(model);
        return 1;
    }
    
    printf("Tableau created: m=%d, n=%d\n", tab->m, tab->n);
    
    if (tab->A_ext) {
        printf("A_ext: %d rows, %d cols, %d nnz\n", 
               tab->A_ext->nrows, tab->A_ext->ncols, tab->A_ext->nnz);
        sparse_print_dense(tab->A_ext, "A_ext");
        
        printf("\nBasis array: ");
        for (int k = 0; k < tab->m; k++) {
            printf("%d ", tab->basis[k]);
        }
        printf("\n");
    } else {
        printf("A_ext is NULL!\n");
    }
    
    tableau_free(tab);
    lp_model_free(model);
    
    return 0;
}
