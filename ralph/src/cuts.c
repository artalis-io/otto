/*
 * Ralph - Cutting Plane Generation
 *
 * Implements:
 * - Gomory Mixed Integer (GMI) cuts
 * - Mixed Integer Rounding (MIR) cuts
 * - Cut pool management
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "mip.h"

/* ============================================================================
 * Cut Pool Management
 * ============================================================================ */

CutPool* cut_pool_create(int capacity) {
    CutPool *pool = (CutPool*)calloc(1, sizeof(CutPool));
    if (!pool) return NULL;

    pool->capacity = capacity > 0 ? capacity : 256;
    pool->count = 0;

    pool->cuts = (Cut**)malloc(pool->capacity * sizeof(Cut*));
    if (!pool->cuts) {
        free(pool);
        return NULL;
    }

    return pool;
}

void cut_pool_free(CutPool *pool) {
    if (!pool) return;

    for (int i = 0; i < pool->count; i++) {
        cut_free(pool->cuts[i]);
    }
    free(pool->cuts);
    free(pool);
}

Cut* cut_create(int max_nnz) {
    Cut *cut = (Cut*)calloc(1, sizeof(Cut));
    if (!cut) return NULL;

    cut->capacity = max_nnz > 0 ? max_nnz : 64;
    cut->nnz = 0;

    cut->indices = (int*)malloc(cut->capacity * sizeof(int));
    cut->values = (double*)malloc(cut->capacity * sizeof(double));

    if (!cut->indices || !cut->values) {
        cut_free(cut);
        return NULL;
    }

    cut->sense = 'L';  /* Default to <= */
    cut->rhs = 0.0;
    cut->violation = 0.0;
    cut->age = 0;

    return cut;
}

void cut_free(Cut *cut) {
    if (!cut) return;
    free(cut->indices);
    free(cut->values);
    free(cut);
}

int cut_pool_add(CutPool *pool, Cut *cut) {
    if (!pool || !cut) return -1;

    /* Expand if needed */
    if (pool->count >= pool->capacity) {
        int new_cap = pool->capacity * 2;
        Cut **new_cuts = (Cut**)realloc(pool->cuts, new_cap * sizeof(Cut*));
        if (!new_cuts) return -1;
        pool->cuts = new_cuts;
        pool->capacity = new_cap;
    }

    pool->cuts[pool->count++] = cut;
    return 0;
}

/* ============================================================================
 * Gomory Mixed Integer Cuts
 * ============================================================================ */

/*
 * Generate GMI cut from a row of the optimal simplex tableau.
 *
 * For a basic integer variable x_i with fractional value f_0,
 * the GMI cut is derived from the simplex tableau row:
 *
 *   x_i + sum_j (a_ij * x_j) = b_i
 *
 * where j ranges over non-basic variables.
 *
 * The cut has the form:
 *   sum_j (alpha_j * x_j) >= f_0
 *
 * where for each non-basic variable x_j:
 *   - If x_j is continuous at lower bound:
 *       alpha_j = a_ij if a_ij >= 0, else a_ij * f_0 / (1 - f_0)
 *   - If x_j is integer:
 *       f_j = fractional part of a_ij
 *       alpha_j = f_j if f_j <= f_0, else (1 - f_j) * f_0 / (1 - f_0)
 */
static Cut* generate_gmi_cut_from_row(SimplexTableau *tab, int basic_pos,
                                      const int *is_integer) {
    int m = tab->m;
    int n = tab->n;
    int num_orig = tab->model->num_vars;
    int basic_var = tab->basis[basic_pos];
    int verbose = 0;  /* Set to 1 to enable debug output */

    /* Get fractional part of basic variable */
    double b_i = tab->x[basic_var];
    double f_0 = b_i - floor(b_i);

    if (verbose) {
        printf("[GMI-row] basic_var=%d, b_i=%.6f, f_0=%.6f\n", basic_var, b_i, f_0);
    }

    /* Check if fractional enough to generate cut */
    if (f_0 < RALPH_INT_TOL || f_0 > 1.0 - RALPH_INT_TOL) {
        return NULL;
    }

    /* Compute tableau row: e_i' * B^{-1} */
    double *row = (double*)calloc(m, sizeof(double));
    if (!row) return NULL;

    row[basic_pos] = 1.0;
    lu_solve_transpose(tab->lu, row, row);

    /* Create cut - allocate space for all original variables */
    Cut *cut = cut_create(num_orig);
    if (!cut) {
        free(row);
        return NULL;
    }

    /* Use dense array for accumulating coefficients (enables slack substitution) */
    double *cut_coefs = (double*)calloc(num_orig, sizeof(double));
    if (!cut_coefs) {
        free(row);
        cut_free(cut);
        return NULL;
    }

    cut->type = CUT_GOMORY;
    cut->sense = 'G';  /* >= cut */
    cut->rhs = f_0;

    /* Compute cut coefficients for each non-basic variable */
    for (int j = 0; j < n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) continue;

        /* Get tableau coefficient a_ij = row' * A_j */
        double a_ij = 0.0;
        for (int p = tab->A_ext->colptr[j]; p < tab->A_ext->colptr[j + 1]; p++) {
            a_ij += row[tab->A_ext->rowidx[p]] * tab->A_ext->values[p];
        }

        if (verbose) {
            const char *status_str = (tab->var_status[j] == RALPH_NONBASIC_LOWER) ? "NB_LO" :
                                     (tab->var_status[j] == RALPH_NONBASIC_UPPER) ? "NB_UP" : "OTHER";
            printf("[GMI-row] var %d: a_ij=%.6f, status=%s, x=%.4f, lb=%.4f, ub=%.4f\n",
                   j, a_ij, status_str, tab->x[j], tab->lb_ext[j], tab->ub_ext[j]);
        }

        if (fabs(a_ij) < RALPH_ZERO_TOL) continue;

        double alpha_j = 0.0;
        double coef_for_formula = a_ij;

        /* For variables at upper bound, use complemented coefficient (-a_ij)
         * because we substitute x_j = u_j - s_j where s_j >= 0 */
        if (tab->var_status[j] == RALPH_NONBASIC_UPPER) {
            coef_for_formula = -a_ij;
        }

        if (j < num_orig && is_integer && is_integer[j]) {
            /* Integer variable */
            double f_j = coef_for_formula - floor(coef_for_formula);

            if (f_j <= f_0 + RALPH_ZERO_TOL) {
                alpha_j = f_j;
            } else {
                alpha_j = (1.0 - f_j) * f_0 / (1.0 - f_0);
            }
            if (verbose) {
                printf("[GMI-row]   INTEGER: coef_for_formula=%.6f, f_j=%.6f, alpha_j=%.6f\n",
                       coef_for_formula, f_j, alpha_j);
            }
        } else {
            /* Continuous variable (or slack) */
            if (coef_for_formula >= 0) {
                alpha_j = coef_for_formula;
            } else {
                alpha_j = -coef_for_formula * f_0 / (1.0 - f_0);
            }
            if (verbose) {
                printf("[GMI-row]   CONTINUOUS: coef_for_formula=%.6f, alpha_j=%.6f\n",
                       coef_for_formula, alpha_j);
            }
        }

        if (fabs(alpha_j) > RALPH_ZERO_TOL) {
            double final_coef = alpha_j;

            /* Convert back to original variable for upper-bounded variables */
            if (tab->var_status[j] == RALPH_NONBASIC_UPPER) {
                /* s_j = u_j - x_j, so alpha_j * s_j = alpha_j * u_j - alpha_j * x_j */
                final_coef = -alpha_j;
                cut->rhs -= alpha_j * tab->ub_ext[j];
                if (verbose) {
                    printf("[GMI-row]   NB_UPPER adjustment: final_coef=%.6f, rhs adjusted by -%.6f*%.6f\n",
                           final_coef, alpha_j, tab->ub_ext[j]);
                }
            } else {
                /* For lower bound: x_j - l_j */
                cut->rhs -= alpha_j * tab->lb_ext[j];
                if (verbose && fabs(tab->lb_ext[j]) > RALPH_ZERO_TOL) {
                    printf("[GMI-row]   NB_LOWER adjustment: rhs adjusted by -%.6f*%.6f\n",
                           alpha_j, tab->lb_ext[j]);
                }
            }

            if (j < num_orig) {
                /* Original variable: accumulate coefficient directly */
                if (verbose) {
                    printf("[GMI-row]   Adding to cut: x%d with coef %.6f\n", j, final_coef);
                }
                cut_coefs[j] += final_coef;
            } else {
                /* Auxiliary variable (slack/surplus): substitute using constraint row
                 *
                 * For slack with aux_coef = +1: s = b - Ax  (from Ax + s = b)
                 * For surplus with aux_coef = -1: s = Ax - b  (from Ax - s = b)
                 * General: s = aux_coef * (b - Ax)
                 *
                 * Substituting alpha * s in cut LHS:
                 *   alpha * s = alpha * aux_coef * (b - sum_k a_k * x_k)
                 *             = alpha * aux_coef * b - alpha * aux_coef * sum_k a_k * x_k
                 *
                 * So: add -alpha * aux_coef * a_k to x_k coefficient
                 *     subtract alpha * aux_coef * b from RHS
                 */
                int aux_idx = j - num_orig;
                if (aux_idx >= 0 && aux_idx < tab->num_aux && tab->aux_row && tab->aux_coef) {
                    int con_row = tab->aux_row[aux_idx];
                    double aux_c = tab->aux_coef[aux_idx];

                    if (verbose) {
                        printf("[GMI-row]   SLACK var %d -> row %d, aux_coef=%.1f, substituting...\n",
                               j, con_row, aux_c);
                    }

                    /* Get original constraint row coefficients and RHS */
                    LPModel *model = tab->model;
                    double con_rhs = model->b[con_row];

                    /* Adjust RHS: subtract alpha * aux_coef * b */
                    cut->rhs -= final_coef * aux_c * con_rhs;

                    /* Add coefficient contributions from original variables in this row */
                    for (int k = 0; k < num_orig; k++) {
                        /* Get coefficient A[con_row, k] by scanning column k */
                        double a_rk = 0.0;
                        for (int p = model->A->colptr[k]; p < model->A->colptr[k + 1]; p++) {
                            if (model->A->rowidx[p] == con_row) {
                                a_rk = model->A->values[p];
                                break;
                            }
                        }
                        if (fabs(a_rk) > RALPH_ZERO_TOL) {
                            /* Add -alpha * aux_coef * a_rk to coefficient of x_k */
                            double contrib = -final_coef * aux_c * a_rk;
                            cut_coefs[k] += contrib;
                            if (verbose) {
                                printf("[GMI-row]     x%d += %.6f (from slack sub)\n", k, contrib);
                            }
                        }
                    }
                } else {
                    /* No mapping available (artificial variable) - skip */
                    if (verbose) {
                        printf("[GMI-row]   Skipping auxiliary var %d (no mapping or artificial)\n", j);
                    }
                }
            }
        }
    }

    free(row);

    /* Convert dense coefficient array to sparse cut */
    for (int k = 0; k < num_orig; k++) {
        if (fabs(cut_coefs[k]) > RALPH_ZERO_TOL) {
            cut->indices[cut->nnz] = k;
            cut->values[cut->nnz] = cut_coefs[k];
            cut->nnz++;
        }
    }
    free(cut_coefs);

    /* Calculate violation */
    double lhs = 0.0;
    for (int k = 0; k < cut->nnz; k++) {
        lhs += cut->values[k] * tab->x[cut->indices[k]];
    }
    cut->violation = cut->rhs - lhs;

    if (verbose) {
        printf("[GMI-row] Final cut: nnz=%d, rhs=%.6f, lhs=%.6f, violation=%.6f\n",
               cut->nnz, cut->rhs, lhs, cut->violation);
    }

    /* Skip cuts with no variable coefficients */
    if (cut->nnz == 0) {
        if (verbose) printf("[GMI-row] Rejected: no variable coefficients\n");
        cut_free(cut);
        return NULL;
    }

    /* Skip cuts where all coefficients are negative and RHS > 0.
     * Such cuts have form: -a*x - b*y >= c (with a,b,c > 0)
     * which means a*x + b*y <= -c, impossible for non-negative vars. */
    int has_positive_coef = 0;
    for (int k = 0; k < cut->nnz; k++) {
        if (cut->values[k] > RALPH_ZERO_TOL) {
            has_positive_coef = 1;
            break;
        }
    }
    if (!has_positive_coef && cut->rhs > RALPH_ZERO_TOL) {
        if (verbose) printf("[GMI-row] Rejected: all negative coefs with positive RHS\n");
        cut_free(cut);
        return NULL;
    }

    /* Only return if cut is violated */
    if (cut->violation < RALPH_FEAS_TOL) {
        if (verbose) printf("[GMI-row] Rejected: not violated (violation < %.9f)\n", RALPH_FEAS_TOL);
        cut_free(cut);
        return NULL;
    }

    if (verbose) printf("[GMI-row] Cut accepted!\n");
    return cut;
}

int generate_gomory_cuts(MIPSolver *solver, CutPool *pool) {
    SimplexTableau *tab = solver->lp_solver->tableau;

    int cuts_added = 0;

    if (solver->verbose >= 2) {
        printf("[GMI] Scanning %d basic positions for cuts\n", tab->m);
        printf("[GMI] Original vars: %d, is_integer array: %p\n",
               solver->original_model->num_vars, (void*)solver->is_integer);
    }

    for (int k = 0; k < tab->m; k++) {
        int j = tab->basis[k];

        if (solver->verbose >= 2) {
            printf("[GMI] Basic pos %d: var %d, val %.4f", k, j, tab->x[j]);
        }

        /* Only generate cuts from integer variables */
        if (j >= solver->original_model->num_vars) {
            if (solver->verbose >= 2) printf(" -> skip (slack/aux)\n");
            continue;
        }
        if (!solver->is_integer[j]) {
            if (solver->verbose >= 2) printf(" -> skip (continuous)\n");
            continue;
        }

        /* Check if fractional */
        double val = tab->x[j];
        double frac = val - floor(val);
        if (frac < RALPH_INT_TOL || frac > 1.0 - RALPH_INT_TOL) {
            if (solver->verbose >= 2) printf(" -> skip (integer: frac=%.6f)\n", frac);
            continue;
        }

        if (solver->verbose >= 2) printf(" -> fractional! Generating cut...\n");

        Cut *cut = generate_gmi_cut_from_row(tab, k, solver->is_integer);
        if (cut) {
            if (solver->verbose >= 2) {
                printf("[GMI] Cut generated: ");
                for (int i = 0; i < cut->nnz; i++) {
                    printf("%.4f*x%d ", cut->values[i], cut->indices[i]);
                }
                printf(">= %.4f (violation=%.4f)\n", cut->rhs, cut->violation);
            }
            cut_pool_add(pool, cut);
            cuts_added++;

            if (cuts_added >= solver->max_cuts_per_round) break;
        } else {
            if (solver->verbose >= 2) printf("[GMI] Cut was NULL (filtered out)\n");
        }
    }

    return cuts_added;
}

/* ============================================================================
 * Mixed Integer Rounding Cuts
 * ============================================================================ */

/*
 * MIR cuts from optimal simplex tableau rows.
 *
 * For a tableau row: x_B[i] + sum_j (a_ij * x_j) = beta_i
 * (where j ranges over non-basic variables)
 *
 * If beta_i has fractional part f_0 and there are integer non-basic variables,
 * apply the MIR inequality:
 *
 *   sum_j mir_coef(a_ij) * x_j <= floor(beta_i)
 *
 * where:
 *   - For integer x_j: mir_coef = floor(a_ij) + max(frac(a_ij) - f_0, 0)/(1-f_0)
 *   - For continuous x_j >= 0: mir_coef = a_ij / (1 - f_0) if a_ij > 0, else 0
 *
 * MIR cuts differ from GMI in that they generate <= inequalities and can be
 * applied to any tableau row with fractional RHS, not just rows with integer
 * basic variables.
 */
static Cut* generate_mir_cut_from_tableau(SimplexTableau *tab, int basic_pos,
                                          const int *is_integer) {
    int m = tab->m;
    int n = tab->n;
    int num_orig = tab->model->num_vars;
    int basic_var = tab->basis[basic_pos];

    /* Get the RHS (value of basic variable) */
    double beta = tab->x[basic_var];
    double f_0 = beta - floor(beta);

    /* Need sufficient fractionality in the RHS */
    if (f_0 < 0.05 || f_0 > 0.95) return NULL;

    /* Check if row has integer non-basic variables (otherwise MIR won't help) */
    int has_int_nonbasic = 0;
    for (int j = 0; j < num_orig; j++) {
        if (tab->var_status[j] != RALPH_BASIC && is_integer && is_integer[j]) {
            has_int_nonbasic = 1;
            break;
        }
    }
    if (!has_int_nonbasic) return NULL;

    /* Compute tableau row: e_i' * B^{-1} */
    double *row = (double*)calloc(m, sizeof(double));
    if (!row) return NULL;

    row[basic_pos] = 1.0;
    lu_solve_transpose(tab->lu, row, row);

    /* Allocate dense array for cut coefficients */
    double *cut_coefs = (double*)calloc(num_orig, sizeof(double));
    if (!cut_coefs) {
        free(row);
        return NULL;
    }

    Cut *cut = cut_create(num_orig);
    if (!cut) {
        free(row);
        free(cut_coefs);
        return NULL;
    }

    cut->type = CUT_MIR;
    cut->sense = 'L';  /* MIR generates <= cuts */
    cut->rhs = floor(beta);

    /* Process each non-basic variable */
    for (int j = 0; j < n; j++) {
        if (tab->var_status[j] == RALPH_BASIC) continue;

        /* Get tableau coefficient a_ij = row' * A_j */
        double a_ij = 0.0;
        for (int p = tab->A_ext->colptr[j]; p < tab->A_ext->colptr[j + 1]; p++) {
            a_ij += row[tab->A_ext->rowidx[p]] * tab->A_ext->values[p];
        }

        if (fabs(a_ij) < RALPH_ZERO_TOL) continue;

        /* Adjust for variables at upper bound */
        double coef = a_ij;
        if (tab->var_status[j] == RALPH_NONBASIC_UPPER) {
            coef = -a_ij;  /* Complement: x_j -> u_j - x_j */
        }

        double mir_coef = 0.0;

        if (j < num_orig && is_integer && is_integer[j]) {
            /* Integer variable: use MIR formula */
            double f_j = coef - floor(coef);
            mir_coef = floor(coef) + fmax(f_j - f_0, 0.0) / (1.0 - f_0);
        } else {
            /* Continuous variable (including slacks) */
            if (coef > RALPH_ZERO_TOL) {
                mir_coef = coef / (1.0 - f_0);
            }
            /* Negative coefficients contribute 0 in MIR */
        }

        if (fabs(mir_coef) > RALPH_ZERO_TOL) {
            if (j < num_orig) {
                /* Original variable */
                if (tab->var_status[j] == RALPH_NONBASIC_UPPER) {
                    /* Undo complementation: mir_coef * (u_j - x_j) */
                    cut_coefs[j] -= mir_coef;
                    cut->rhs -= mir_coef * tab->ub_ext[j];
                } else {
                    cut_coefs[j] += mir_coef;
                    cut->rhs += mir_coef * tab->lb_ext[j];
                }
            } else {
                /* Slack variable: substitute back using constraint mapping */
                int aux_idx = j - num_orig;
                if (aux_idx >= 0 && aux_idx < tab->num_aux && tab->aux_row && tab->aux_coef) {
                    int con_row = tab->aux_row[aux_idx];
                    double aux_c = tab->aux_coef[aux_idx];
                    LPModel *model = tab->model;
                    double con_rhs = model->b[con_row];

                    /* s = aux_c * (b - Ax), so mir_coef * s contributes:
                     * -mir_coef * aux_c * a_k to x_k, and mir_coef * aux_c * b to RHS */
                    if (tab->var_status[j] == RALPH_NONBASIC_UPPER) {
                        /* Complemented slack */
                        cut->rhs += mir_coef * aux_c * con_rhs;
                        for (int k = 0; k < num_orig; k++) {
                            double a_rk = 0.0;
                            for (int p = model->A->colptr[k]; p < model->A->colptr[k + 1]; p++) {
                                if (model->A->rowidx[p] == con_row) {
                                    a_rk = model->A->values[p];
                                    break;
                                }
                            }
                            if (fabs(a_rk) > RALPH_ZERO_TOL) {
                                cut_coefs[k] += mir_coef * aux_c * a_rk;
                            }
                        }
                    } else {
                        cut->rhs -= mir_coef * aux_c * con_rhs;
                        for (int k = 0; k < num_orig; k++) {
                            double a_rk = 0.0;
                            for (int p = model->A->colptr[k]; p < model->A->colptr[k + 1]; p++) {
                                if (model->A->rowidx[p] == con_row) {
                                    a_rk = model->A->values[p];
                                    break;
                                }
                            }
                            if (fabs(a_rk) > RALPH_ZERO_TOL) {
                                cut_coefs[k] -= mir_coef * aux_c * a_rk;
                            }
                        }
                    }
                }
            }
        }
    }

    free(row);

    /* Convert dense to sparse */
    for (int k = 0; k < num_orig; k++) {
        if (fabs(cut_coefs[k]) > RALPH_ZERO_TOL) {
            cut->indices[cut->nnz] = k;
            cut->values[cut->nnz] = cut_coefs[k];
            cut->nnz++;
        }
    }
    free(cut_coefs);

    /* Calculate violation: LHS - RHS for <= cut */
    double lhs = 0.0;
    for (int k = 0; k < cut->nnz; k++) {
        lhs += cut->values[k] * tab->x[cut->indices[k]];
    }
    cut->violation = lhs - cut->rhs;

    /* Reject if not violated or empty */
    if (cut->violation < RALPH_FEAS_TOL || cut->nnz == 0) {
        cut_free(cut);
        return NULL;
    }

    return cut;
}

int generate_mir_cuts(MIPSolver *solver, CutPool *pool) {
    SimplexTableau *tab = solver->lp_solver->tableau;
    int cuts_added = 0;

    /* Generate MIR cuts from tableau rows with fractional RHS */
    for (int k = 0; k < tab->m; k++) {
        int basic_var = tab->basis[k];
        double val = tab->x[basic_var];
        double frac = val - floor(val);

        /* Skip rows with nearly-integer RHS */
        if (frac < 0.05 || frac > 0.95) continue;

        /* Skip rows where GMI already applies (integer basic var) */
        if (basic_var < solver->original_model->num_vars &&
            solver->is_integer[basic_var]) {
            continue;  /* GMI handles these */
        }

        Cut *cut = generate_mir_cut_from_tableau(tab, k, solver->is_integer);
        if (cut) {
            cut_pool_add(pool, cut);
            cuts_added++;

            if (cuts_added >= solver->max_cuts_per_round) break;
        }
    }

    return cuts_added;
}

/* ============================================================================
 * Cut Application
 * ============================================================================ */

/* Add cuts to the LP relaxation */
int apply_cuts(MIPSolver *solver, CutPool *pool, int max_cuts) {
    if (!pool || pool->count == 0) return 0;

    LPModel *model = solver->working_model;

    /* Sort cuts by violation (descending) */
    for (int i = 0; i < pool->count - 1; i++) {
        for (int j = i + 1; j < pool->count; j++) {
            if (pool->cuts[j]->violation > pool->cuts[i]->violation) {
                Cut *tmp = pool->cuts[i];
                pool->cuts[i] = pool->cuts[j];
                pool->cuts[j] = tmp;
            }
        }
    }

    int cuts_applied = 0;

    for (int i = 0; i < pool->count && cuts_applied < max_cuts; i++) {
        Cut *cut = pool->cuts[i];

        /* Skip if not violated enough */
        if (cut->violation < RALPH_FEAS_TOL) continue;

        /* Add cut as new constraint */
        lp_model_add_constraint(model, cut->nnz, cut->indices, cut->values,
                               cut->sense, cut->rhs);
        cuts_applied++;
    }

    /* Rebuild the LP if cuts were added */
    if (cuts_applied > 0) {
        /* Need to rebuild simplex tableau with new constraints */
        solver->cuts_applied += cuts_applied;
    }

    return cuts_applied;
}

/* ============================================================================
 * Cut Pool Cleanup
 * ============================================================================ */

/* Remove old/weak cuts */
void cut_pool_cleanup(CutPool *pool, int max_age) {
    if (!pool) return;

    int write_idx = 0;
    for (int i = 0; i < pool->count; i++) {
        if (pool->cuts[i]->age > max_age) {
            cut_free(pool->cuts[i]);
        } else {
            pool->cuts[write_idx++] = pool->cuts[i];
        }
    }
    pool->count = write_idx;
}

/* Increment age of all cuts and mark as active if binding */
void cut_pool_age(CutPool *pool) {
    if (!pool) return;

    for (int i = 0; i < pool->count; i++) {
        pool->cuts[i]->age++;
    }
}
