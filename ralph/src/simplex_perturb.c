/*
 * simplex_perturb.c - Primal perturbation (anti-cycling) for the simplex method.
 *
 * Extracted from simplex.c - contains bound perturbation to prevent cycling
 * and artificial variable helpers.
 */

#include <stdlib.h>
#include <math.h>
#include "lp.h"
#include "simplex_perturb.h"

int simplex_smcp_shift_allows_perturb_for_test(int smcp_shift) {
    return smcp_shift != 0;
}

int simplex_smcp_shift_allows_perturb(const SimplexTableau *tab) {
    if (!tab || !tab->owner) return 1;
    return simplex_smcp_shift_allows_perturb_for_test(tab->owner->smcp_shift);
}

/* Linear scan is fine here: perturbation is infrequent and num_artificial << n. */
int is_artificial_var(const SimplexTableau *tab, int var_idx) {
    if (!tab || !tab->artificial_vars || tab->num_artificial <= 0) {
        return 0;
    }
    for (int k = 0; k < tab->num_artificial; k++) {
        if (tab->artificial_vars[k] == var_idx) {
            return 1;
        }
    }
    return 0;
}

/* Mark basic rows backed by artificial variables as redundant hints for LU.
 * If only_infeasible is non-zero, only rows with bound-infeasible basic
 * artificials are marked. */
int mark_basic_artificial_rows_redundant(SimplexTableau *tab, int only_infeasible) {
    if (!tab || !tab->redundant_rows) {
        return 0;
    }

    int marked = 0;
    for (int k = 0; k < tab->m; k++) {
        if (tab->redundant_rows[k]) continue;

        int bj = tab->basis[k];
        if (!is_artificial_var(tab, bj)) continue;

        if (only_infeasible) {
            if (tab->x[bj] >= tab->lb_ext[bj] - RALPH_FEAS_TOL &&
                tab->x[bj] <= tab->ub_ext[bj] + RALPH_FEAS_TOL) {
                continue;
            }
        }

        tab->redundant_rows[k] = 1;
        tab->num_redundant++;
        marked++;
    }

    return marked;
}

void primal_apply_perturbation(SimplexTableau *tab) {
    if (!simplex_smcp_shift_allows_perturb(tab)) return;
    int n = tab->n;

    /* Free any existing perturbation state */
    free(tab->primal_saved_lb);
    free(tab->primal_saved_ub);

    /* Save original bounds */
    tab->primal_saved_lb = (double*)calloc(n, sizeof(double));
    tab->primal_saved_ub = (double*)calloc(n, sizeof(double));
    if (!tab->primal_saved_lb || !tab->primal_saved_ub) {
        free(tab->primal_saved_lb);
        free(tab->primal_saved_ub);
        tab->primal_saved_lb = tab->primal_saved_ub = NULL;
        tab->primal_perturb_active = 0;
        return;
    }

    for (int j = 0; j < n; j++) {
        tab->primal_saved_lb[j] = tab->lb_ext[j];
        tab->primal_saved_ub[j] = tab->ub_ext[j];
    }

    /* Apply perturbations to bounds only.
     * Non-basic variable x values stay at their current (original) bound values.
     * This widens the feasible region so basic variables have positive slack.
     * Note: Do NOT update x values here - that would change the RHS and
     * potentially worsen numerical stability. The key insight is that
     * for the ratio test, only basic variable slacks matter, and those
     * are computed from (x_j - lb_j) where x_j is unchanged and lb_j is now lower. */
    for (int j = 0; j < n; j++) {
        /* In Phase 1, keep artificial bounds exact.
         * Perturbing artificials changes the feasibility objective geometry and
         * can produce false "optimal" Phase 1 terminations on hard instances. */
        if (tab->phase == 1 && is_artificial_var(tab, j)) {
            continue;
        }

        unsigned int offset = lp_determinism_seed_offset(tab->owner, j, 13U);
        unsigned int pattern = (unsigned int)((j * PRIMAL_PERTURB_MULT) % 13);
        double factor = 1.0 + (double)((pattern + offset) % 13U);

        /* Perturb finite lower bounds down */
        if (tab->lb_ext[j] > -RALPH_INFINITY / 2) {
            double eps = PRIMAL_PERTURB_BASE * factor * (1.0 + fabs(tab->lb_ext[j]));
            tab->lb_ext[j] -= eps;
        }

        /* Perturb finite upper bounds up */
        if (tab->ub_ext[j] < RALPH_INFINITY / 2) {
            double eps = PRIMAL_PERTURB_BASE * factor * (1.0 + fabs(tab->ub_ext[j]));
            tab->ub_ext[j] += eps;
        }
    }

    tab->primal_perturb_active = 1;
}

/* Scaled variant for stall-recovery re-perturbation.
 * scale > 1.0 widens the perturbation to break a different cycling pattern. */
void primal_apply_perturbation_scaled(SimplexTableau *tab, double scale) {
    if (!simplex_smcp_shift_allows_perturb(tab)) return;
    int n = tab->n;

    /* If perturbation is already active, remove it first to start fresh */
    if (tab->primal_perturb_active) {
        primal_remove_perturbation(tab);
    }

    /* Allocate and save original bounds */
    free(tab->primal_saved_lb);
    free(tab->primal_saved_ub);
    tab->primal_saved_lb = (double*)calloc(n, sizeof(double));
    tab->primal_saved_ub = (double*)calloc(n, sizeof(double));
    if (!tab->primal_saved_lb || !tab->primal_saved_ub) {
        free(tab->primal_saved_lb);
        free(tab->primal_saved_ub);
        tab->primal_saved_lb = tab->primal_saved_ub = NULL;
        tab->primal_perturb_active = 0;
        return;
    }

    for (int j = 0; j < n; j++) {
        tab->primal_saved_lb[j] = tab->lb_ext[j];
        tab->primal_saved_ub[j] = tab->ub_ext[j];
    }

    double base = PRIMAL_PERTURB_BASE * scale;

    for (int j = 0; j < n; j++) {
        if (tab->phase == 1 && is_artificial_var(tab, j)) continue;

        unsigned int offset = lp_determinism_seed_offset(tab->owner, j, 13U);
        unsigned int pattern = (unsigned int)((j * PRIMAL_PERTURB_MULT) % 13);
        double factor = 1.0 + (double)((pattern + offset) % 13U);

        if (tab->lb_ext[j] > -RALPH_INFINITY / 2) {
            double eps = base * factor * (1.0 + fabs(tab->lb_ext[j]));
            tab->lb_ext[j] -= eps;
        }
        if (tab->ub_ext[j] < RALPH_INFINITY / 2) {
            double eps = base * factor * (1.0 + fabs(tab->ub_ext[j]));
            tab->ub_ext[j] += eps;
        }
    }

    tab->primal_perturb_active = 1;
}

void primal_remove_perturbation(SimplexTableau *tab) {
    if (!tab->primal_perturb_active || !tab->primal_saved_lb || !tab->primal_saved_ub) {
        return;
    }

    /* Restore original bounds and reset non-basic variable values */
    for (int j = 0; j < tab->n; j++) {
        tab->lb_ext[j] = tab->primal_saved_lb[j];
        tab->ub_ext[j] = tab->primal_saved_ub[j];

        /* Reset non-basic variables to their proper bounds */
        if (tab->var_status[j] == RALPH_NONBASIC_LOWER) {
            tab->x[j] = tab->lb_ext[j];
        } else if (tab->var_status[j] == RALPH_NONBASIC_UPPER) {
            tab->x[j] = tab->ub_ext[j];
        }
        /* Basic variables will be recomputed by tableau_compute_solution */
    }

    free(tab->primal_saved_lb);
    free(tab->primal_saved_ub);
    tab->primal_saved_lb = tab->primal_saved_ub = NULL;
    tab->primal_perturb_active = 0;
}
