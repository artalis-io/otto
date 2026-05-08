/*
 * simplex_crash.c - Triangular crash basis (Maros LTSF).
 *
 * Zero behavior change — pure code motion from simplex.c (R3.9).
 */

#include "simplex_crash.h"
#include "lp.h"
#include "lp_log.h"

#include <stdlib.h>
#include <math.h>

/* ============================================================================
 * Triangular crash basis (Maros LTSF)
 *
 * Replace slack variables (from <= rows) in the initial basis with structural
 * columns that have good pivot elements. This reduces Phase 1 iterations
 * by starting closer to a feasible basis.
 *
 * IMPORTANT: Only displace slacks (from <= rows). Never displace artificials
 * or surplus variables — these are needed for Phase 1 feasibility tracking.
 *
 * Algorithm:
 *   Pass 1: Scan structural columns for singletons — if the singleton element
 *           is in an eligible row with |a_ij| > PIVOT_TOL, swap into basis.
 *   Pass 2: Scan remaining columns for the best pivot in eligible unclaimed rows.
 *
 * Returns: number of structural columns placed in basis.
 * ============================================================================ */
int crash_triangular(SimplexTableau *tab, int verbose) {
    if (!tab || !tab->A_ext) return 0;

    int m = tab->m;
    int n_structural = tab->model->num_vars;
    SparseMatrix *A = tab->A_ext;
    int placed = 0;

    /* Identify which rows are eligible for crash (only slack-basic rows).
     * A row is eligible if its basic variable is a slack (not artificial/surplus).
     * Slacks are aux variables with +1 coefficient in their row. Artificials
     * and surplus variables must not be displaced. */
    int *row_eligible = (int *)calloc(m, sizeof(int));
    if (!row_eligible) return 0;

    for (int i = 0; i < m; i++) {
        int bv = tab->basis[i];
        /* Only eligible if basic var is an auxiliary (slack/surplus/artificial) */
        if (bv >= n_structural) {
            /* Check if this is a simple slack or surplus: |coeff| == 1, zero cost.
             * Slacks have +1 coeff, surplus have -1 coeff — both displaceable. */
            int is_slack = 0;
            for (int p = A->colptr[bv]; p < A->colptr[bv + 1]; p++) {
                if (A->rowidx[p] == i) {
                    if ((fabs(A->values[p] - 1.0) < RALPH_ZERO_TOL ||
                         fabs(A->values[p] + 1.0) < RALPH_ZERO_TOL) &&
                        fabs(tab->c_ext[bv]) < RALPH_ZERO_TOL) {
                        is_slack = 1;
                    }
                    break;
                }
            }
            row_eligible[i] = is_slack;
        }
    }

    /* Track which rows have been claimed by a structural variable */
    int *row_claimed = (int *)calloc(m, sizeof(int));
    int *col_used = (int *)calloc(n_structural, sizeof(int));
    if (!row_claimed || !col_used) {
        free(row_eligible); free(row_claimed); free(col_used);
        return 0;
    }

    /* Pass 1: Singletons in eligible rows.
     * For singletons, we can exactly compute x_j = rhs[i] / a[i,j].
     * Only accept if x_j is within bounds [lb, ub]. */
    for (int j = 0; j < n_structural; j++) {
        if (fabs(tab->ub_ext[j] - tab->lb_ext[j]) < RALPH_ZERO_TOL) continue;

        int nnz = 0;
        int singleton_row = -1;
        double singleton_val = 0.0;

        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            int row = A->rowidx[p];
            if (row < m) {
                nnz++;
                singleton_row = row;
                singleton_val = A->values[p];
            }
        }

        if (nnz == 1 && singleton_row >= 0 &&
            row_eligible[singleton_row] && !row_claimed[singleton_row] &&
            fabs(singleton_val) > RALPH_PIVOT_TOL) {
            /* Check feasibility: x_j = rhs / a_ij */
            double xval = tab->rhs[singleton_row] / singleton_val;
            if (xval < tab->lb_ext[j] - RALPH_FEAS_TOL ||
                xval > tab->ub_ext[j] + RALPH_FEAS_TOL) continue;

            int old_basic = tab->basis[singleton_row];
            tab->var_status[old_basic] = RALPH_NONBASIC_LOWER;
            tab->x[old_basic] = tab->lb_ext[old_basic];
            tab->basis_pos[old_basic] = -1;

            tab->basis[singleton_row] = j;
            tab->basis_pos[j] = singleton_row;
            tab->var_status[j] = RALPH_BASIC;

            row_claimed[singleton_row] = 1;
            col_used[j] = 1;
            placed++;
        }
    }

    /* Pass 2: Multi-element columns in eligible unclaimed rows */
    for (int j = 0; j < n_structural; j++) {
        if (col_used[j]) continue;
        if (fabs(tab->ub_ext[j] - tab->lb_ext[j]) < RALPH_ZERO_TOL) continue;

        int unclaimed_nnz = 0;
        int best_row = -1;
        double best_val = 0.0;
        double best_actual_val = 0.0;

        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            int row = A->rowidx[p];
            if (row < m && row_eligible[row] && !row_claimed[row]) {
                unclaimed_nnz++;
                double absval = fabs(A->values[p]);
                if (absval > best_val) {
                    best_val = absval;
                    best_actual_val = A->values[p];
                    best_row = row;
                }
            }
        }

        if (best_row >= 0 && best_val > RALPH_PIVOT_TOL && unclaimed_nnz <= m / 2 + 1) {
            /* Approximate feasibility: x_j ~ rhs[best_row] / a[best_row,j].
             * For multi-element columns this is approximate (ignores other basics),
             * but filters out clearly infeasible placements. */
            double approx_xval = tab->rhs[best_row] / best_actual_val;
            if (approx_xval < tab->lb_ext[j] - RALPH_FEAS_TOL ||
                approx_xval > tab->ub_ext[j] + RALPH_FEAS_TOL) continue;

            int old_basic = tab->basis[best_row];
            tab->var_status[old_basic] = RALPH_NONBASIC_LOWER;
            tab->x[old_basic] = tab->lb_ext[old_basic];
            tab->basis_pos[old_basic] = -1;

            tab->basis[best_row] = j;
            tab->basis_pos[j] = best_row;
            tab->var_status[j] = RALPH_BASIC;

            row_claimed[best_row] = 1;
            col_used[j] = 1;
            placed++;
        }
    }

    free(row_eligible);
    free(row_claimed);
    free(col_used);

    if (verbose) {
        LP_LOG_STDOUT("[crash] Placed %d structural columns in basis (of %d rows)\n",
               placed, m);
    }

    return placed;
}
