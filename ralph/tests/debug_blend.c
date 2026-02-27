#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "ralph_test_mod_api.h"
#include "lp.h"

extern LPModel* ralph_get_lp_model(const RalphModel *model);
extern int lp_model_finalize(LPModel *model);

/* GLPK optimal solution for blend.mps (from glpsol --output) */
static double glpk_solution[83] = {
    20.9448, 10.1709, 11.2474, 2.9811, 0.659704, 0.475926, 0, 10.1012, 0, 1.67918,
    0, 10.1012, 0, 11.7804, 0.406743, 0, 0, 2.17326, 1.61182, 5.25,
    3.07922, 0.0589657, 1.14991, 1.39629, 0, 0, 0, 0, 0.3835, 0,
    4.42443, 0, 1.14991, 1.39629, 0.74857, 21.6384, 8.1027, 0.710776, 0.481789, 0,
    4.89294, 0, 0.441675, 14.2857, 6.52718, 2.00582, 0, 0, 0.771329, 4.87626,
    0.224225, 1.81116, 7.87763, 0.320155, 0.989247, 0.443908, 1.43315, 0, 0, 0,
    0, 3.8748, 3.8748, 0.774958, 1.83077, 0, 0, 0.0658734, 0.788912, 3.46051,
    2.75089, 0, 0, 0, 0, 0, 0.169396, 0, 1.1548, 0,
    0.803301, 26.0304, 87.095
};

int main(void) {
    printf("=== Verify GLPK solution against Ralph model ===\n\n");

    RalphModel *model = ralph_test_create();
    if (ralph_test_read_mps(model, "benchmarks/netlib/blend.mps") != 0) {
        printf("Failed to read blend.mps\n");
        ralph_test_free(model);
        return 1;
    }

    LPModel *lp = ralph_get_lp_model(model);
    if (!lp) {
        printf("No model\n");
        ralph_test_free(model);
        return 1;
    }
    /* Force finalization to build A matrix */
    if (!lp->A) {
        lp_model_finalize(lp);
    }
    if (!lp->A) {
        printf("No A matrix after finalize\n");
        ralph_test_free(model);
        return 1;
    }
    int m = lp->num_cons;
    int n = lp->num_vars;
    printf("Model: %d vars, %d cons, A dims: %dx%d, nnz=%d\n",
           n, m, lp->A->nrows, lp->A->ncols, lp->A->colptr[n]);

    /* Compute Ax via CSC: for each column j, add A[:,j] * x[j] */
    double *ax = (double*)calloc(m, sizeof(double));
    for (int j = 0; j < n; j++) {
        double xj = glpk_solution[j];
        if (fabs(xj) < 1e-15) continue;
        for (int p = lp->A->colptr[j]; p < lp->A->colptr[j + 1]; p++) {
            int row = lp->A->rowidx[p];
            if (row >= 0 && row < m) {
                ax[row] += lp->A->values[p] * xj;
            }
        }
    }

    /* Check constraints */
    printf("\nConstraint violations (GLPK solution in Ralph model):\n");
    double max_viol = 0;
    int con_viol = 0;
    for (int i = 0; i < m; i++) {
        double viol = 0;
        if (lp->sense[i] == 'E') {
            viol = fabs(ax[i] - lp->b[i]);
        } else if (lp->sense[i] == 'L') {
            viol = (ax[i] > lp->b[i] + 1e-10) ? ax[i] - lp->b[i] : 0;
        } else if (lp->sense[i] == 'G') {
            viol = (ax[i] < lp->b[i] - 1e-10) ? lp->b[i] - ax[i] : 0;
        }

        if (viol > 1e-6) {
            printf("  Row %d: Ax=%.8f, b=%.8f, sense=%c, viol=%.2e\n",
                   i, ax[i], lp->b[i], lp->sense[i], viol);
            con_viol++;
        }
        if (viol > max_viol) max_viol = viol;
    }
    printf("Violations: %d, max = %.2e\n", con_viol, max_viol);

    /* Compute objective */
    double obj = 0;
    for (int j = 0; j < n; j++) {
        obj += lp->c[j] * glpk_solution[j];
    }
    printf("\nObjective: c'x = %.10f (GLPK says -30.81214985)\n", obj);
    printf("Obj sense: %d\n", lp->obj_sense);

    /* Dump column 0 entries */
    printf("\nColumn 0 entries (should match MPS col '1'):\n");
    for (int p = lp->A->colptr[0]; p < lp->A->colptr[0 + 1]; p++) {
        printf("  row %d: %.6f\n", lp->A->rowidx[p], lp->A->values[p]);
    }

    free(ax);
    ralph_test_free(model);
    return 0;
}
