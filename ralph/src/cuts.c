/*
 * Ralph - Cutting Plane Generation
 *
 * Implements:
 * - Gomory Mixed Integer (GMI) cuts
 * - Mixed Integer Rounding (MIR) cuts
 * - Cut pool management
 * - SCP-specific cuts: clique, odd-hole, lifted cover (Phase 3)
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "mip.h"
#include "detect.h"

/* ============================================================================
 * Cut Pool Management
 * ============================================================================ */

CutPool* cut_pool_create(int capacity) {
    CutPool *pool = (CutPool*)calloc(1, sizeof(CutPool));
    if (!pool) return NULL;

    pool->capacity = capacity > 0 ? capacity : 256;
    pool->count = 0;

    pool->cuts = (Cut**)calloc(pool->capacity, sizeof(Cut*));
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

    cut->indices = (int*)calloc(cut->capacity, sizeof(int));
    cut->values = (double*)calloc(cut->capacity, sizeof(double));

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

/*
 * Check if two cuts are duplicates (parallel or identical).
 * Returns 1 if duplicate, 0 otherwise.
 */
static int cuts_are_duplicate(const Cut *a, const Cut *b) {
    /* Must have same sense and similar number of nonzeros */
    if (a->sense != b->sense) return 0;
    if (a->nnz != b->nnz) return 0;
    if (a->nnz == 0) return 1;  /* Both empty */

    /* Check if indices match (cuts are stored sorted by index) */
    for (int i = 0; i < a->nnz; i++) {
        if (a->indices[i] != b->indices[i]) return 0;
    }

    /* Indices match - check if coefficients are proportional */
    /* Find first non-zero coefficient to compute ratio */
    double ratio = 0.0;
    int found_ratio = 0;
    for (int i = 0; i < a->nnz; i++) {
        if (fabs(b->values[i]) > RALPH_ZERO_TOL) {
            ratio = a->values[i] / b->values[i];
            found_ratio = 1;
            break;
        }
    }
    if (!found_ratio) return 1;  /* All zeros in b */

    /* Check all coefficients have same ratio */
    for (int i = 0; i < a->nnz; i++) {
        double expected = b->values[i] * ratio;
        if (fabs(a->values[i] - expected) > RALPH_ZERO_TOL * (1 + fabs(expected))) {
            return 0;  /* Not parallel */
        }
    }

    /* Check RHS ratio */
    if (fabs(b->rhs) > RALPH_ZERO_TOL) {
        double rhs_ratio = a->rhs / b->rhs;
        if (fabs(ratio - rhs_ratio) > RALPH_ZERO_TOL * (1 + fabs(ratio))) {
            return 0;  /* Different RHS scaling */
        }
    } else if (fabs(a->rhs) > RALPH_ZERO_TOL) {
        return 0;  /* a has non-zero RHS, b has zero */
    }

    return 1;  /* Cuts are duplicates */
}

/*
 * Normalize a cut by sorting indices and making lead coefficient positive.
 * This helps with duplicate detection.
 */
static void cut_normalize(Cut *cut) {
    if (!cut || cut->nnz <= 1) return;

    /* Simple insertion sort (cuts are typically small) */
    for (int i = 1; i < cut->nnz; i++) {
        int idx = cut->indices[i];
        double val = cut->values[i];
        int j = i - 1;
        while (j >= 0 && cut->indices[j] > idx) {
            cut->indices[j + 1] = cut->indices[j];
            cut->values[j + 1] = cut->values[j];
            j--;
        }
        cut->indices[j + 1] = idx;
        cut->values[j + 1] = val;
    }

    /* Make lead coefficient positive for consistent comparison */
    if (cut->nnz > 0 && cut->values[0] < -RALPH_ZERO_TOL) {
        for (int i = 0; i < cut->nnz; i++) {
            cut->values[i] = -cut->values[i];
        }
        cut->rhs = -cut->rhs;
        /* Flip sense */
        if (cut->sense == 'L') cut->sense = 'G';
        else if (cut->sense == 'G') cut->sense = 'L';
    }
}

int cut_pool_add(CutPool *pool, Cut *cut) {
    if (!pool || !cut) return -1;

    /* Normalize the cut for consistent comparison */
    cut_normalize(cut);

    /* Check for duplicates */
    for (int i = 0; i < pool->count; i++) {
        if (cuts_are_duplicate(cut, pool->cuts[i])) {
            /* Keep the one with higher violation */
            if (cut->violation > pool->cuts[i]->violation) {
                cut_free(pool->cuts[i]);
                pool->cuts[i] = cut;
            } else {
                cut_free(cut);
            }
            return 0;  /* Duplicate handled */
        }
    }

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
 * Cover Cuts (Knapsack Covers)
 * ============================================================================ */

/*
 * Check if a constraint is a knapsack constraint:
 * - All variables are binary
 * - All coefficients are positive
 * - Constraint sense is <= (or =)
 *
 * Returns 1 if knapsack, 0 otherwise
 */
static int is_knapsack_constraint(LPModel *model, int row, const int *is_integer,
                                  double *coefs, int *vars, int *num_vars) {
    int n = model->num_vars;
    int count = 0;

    /* Get row coefficients */
    double *row_data = (double*)calloc(n, sizeof(double));
    if (!row_data) return 0;

    sparse_get_row(model->A, row, row_data);

    /* Check each variable in the row */
    for (int j = 0; j < n; j++) {
        double aij = row_data[j];
        if (fabs(aij) < RALPH_ZERO_TOL) continue;

        /* Must be positive coefficient */
        if (aij < RALPH_ZERO_TOL) {
            free(row_data);
            return 0;
        }

        /* Must be binary variable */
        if (model->var_type[j] != 'B') {
            free(row_data);
            return 0;
        }

        /* Must be in is_integer array */
        if (!is_integer || !is_integer[j]) {
            free(row_data);
            return 0;
        }

        coefs[count] = aij;
        vars[count] = j;
        count++;
    }

    free(row_data);

    /* Must have at least 2 variables */
    if (count < 2) return 0;

    /* Constraint sense must be <= */
    if (model->sense[row] != 'L' && model->sense[row] != 'E') return 0;

    *num_vars = count;
    return 1;
}

/*
 * Generate a cover cut from a knapsack constraint.
 *
 * A cover C is a set of variables where sum(a_j for j in C) > b.
 * The cover inequality is: sum(x_j for j in C) <= |C| - 1
 *
 * We greedily build a minimal cover by selecting variables with highest
 * LP solution values first (most violated in current LP relaxation).
 */
static Cut* generate_single_cover_cut(LPModel *model, int row, double rhs,
                                      int *vars, double *coefs, int num_vars,
                                      const double *x) {
    (void)model;  /* Available for future use (e.g., constraint tightening) */
    (void)row;    /* Available for future use (e.g., row-specific logic) */
    /* Compute coefficient sum and create sorted list by LP value (descending) */
    double coef_sum = 0.0;
    for (int i = 0; i < num_vars; i++) {
        coef_sum += coefs[i];
    }

    /* If coefficient sum <= rhs, no cover exists */
    if (coef_sum <= rhs + RALPH_ZERO_TOL) return NULL;

    /* Sort variables by LP value (descending) - simple bubble sort for small sets */
    int *order = (int*)calloc(num_vars, sizeof(int));
    for (int i = 0; i < num_vars; i++) order[i] = i;

    for (int i = 0; i < num_vars - 1; i++) {
        for (int j = i + 1; j < num_vars; j++) {
            if (x[vars[order[j]]] > x[vars[order[i]]]) {
                int tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }

    /* Greedily build minimal cover */
    int *in_cover = (int*)calloc(num_vars, sizeof(int));
    double cover_coef_sum = 0.0;
    int cover_size = 0;

    for (int i = 0; i < num_vars && cover_coef_sum <= rhs; i++) {
        int idx = order[i];
        in_cover[idx] = 1;
        cover_coef_sum += coefs[idx];
        cover_size++;
    }

    free(order);

    /* Verify we have a valid cover */
    if (cover_coef_sum <= rhs + RALPH_ZERO_TOL) {
        free(in_cover);
        return NULL;
    }

    /* Check if cover cut is violated by current LP solution */
    double lhs = 0.0;
    for (int i = 0; i < num_vars; i++) {
        if (in_cover[i]) {
            lhs += x[vars[i]];
        }
    }

    double cut_rhs = cover_size - 1.0;
    double violation = lhs - cut_rhs;

    if (violation < RALPH_FEAS_TOL) {
        /* Cut not violated */
        free(in_cover);
        return NULL;
    }

    /* Create the cut */
    Cut *cut = cut_create(cover_size);
    if (!cut) {
        free(in_cover);
        return NULL;
    }

    cut->type = CUT_KNAPSACK;
    cut->sense = 'L';
    cut->rhs = cut_rhs;
    cut->violation = violation;

    for (int i = 0; i < num_vars; i++) {
        if (in_cover[i]) {
            cut->indices[cut->nnz] = vars[i];
            cut->values[cut->nnz] = 1.0;
            cut->nnz++;
        }
    }

    free(in_cover);
    return cut;
}

int generate_cover_cuts(MIPSolver *solver, CutPool *pool) {
    if (!solver || !solver->lp_solver || !solver->lp_solver->solution) return 0;

    LPModel *model = solver->original_model;
    const double *x = solver->lp_solver->solution;
    int n = model->num_vars;
    int m = model->num_cons;
    int cuts_added = 0;

    /* Skip if no binary variables */
    if (model->num_binary == 0) return 0;

    /* Allocate working arrays */
    double *coefs = (double*)calloc(n, sizeof(double));
    int *vars = (int*)calloc(n, sizeof(int));
    if (!coefs || !vars) {
        free(coefs);
        free(vars);
        return 0;
    }

    /* Scan constraints for knapsack structure */
    for (int i = 0; i < m; i++) {
        int num_vars_in_row = 0;

        if (!is_knapsack_constraint(model, i, solver->is_integer,
                                    coefs, vars, &num_vars_in_row)) {
            continue;
        }

        double rhs = model->b[i];

        /* Generate cover cut if possible */
        Cut *cut = generate_single_cover_cut(model, i, rhs, vars, coefs,
                                             num_vars_in_row, x);
        if (cut) {
            cut_pool_add(pool, cut);
            cuts_added++;

            if (cuts_added >= solver->max_cuts_per_round) break;
        }
    }

    free(coefs);
    free(vars);
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
 * Cut Pool Cleanup and Aging
 * ============================================================================ */

/* Remove old/weak cuts from the pool */
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

/* Increment age of all cuts */
void cut_pool_age(CutPool *pool) {
    if (!pool) return;

    for (int i = 0; i < pool->count; i++) {
        pool->cuts[i]->age++;
    }
}

/*
 * Update cut efficacy based on current solution.
 * Cuts that are binding (tight) have their age reset to 0.
 * Returns number of binding cuts.
 */
int cut_pool_update_efficacy(CutPool *pool, const double *x, int n) {
    if (!pool || !x) return 0;

    int binding_count = 0;

    for (int i = 0; i < pool->count; i++) {
        Cut *cut = pool->cuts[i];

        /* Compute LHS */
        double lhs = 0.0;
        for (int k = 0; k < cut->nnz; k++) {
            if (cut->indices[k] < n) {
                lhs += cut->values[k] * x[cut->indices[k]];
            }
        }

        /* Check if binding (slack < tolerance) */
        double slack;
        if (cut->sense == 'L') {
            slack = cut->rhs - lhs;  /* <= : slack = rhs - lhs */
        } else if (cut->sense == 'G') {
            slack = lhs - cut->rhs;  /* >= : slack = lhs - rhs */
        } else {
            slack = fabs(lhs - cut->rhs);  /* = : slack = |lhs - rhs| */
        }

        if (slack < RALPH_FEAS_TOL) {
            /* Cut is binding - reset age */
            cut->age = 0;
            binding_count++;
        }

        /* Update violation for potential future use */
        if (cut->sense == 'L') {
            cut->violation = lhs - cut->rhs;  /* Positive if violated */
        } else if (cut->sense == 'G') {
            cut->violation = cut->rhs - lhs;  /* Positive if violated */
        } else {
            cut->violation = fabs(lhs - cut->rhs);
        }
    }

    return binding_count;
}

/* Clear the cut pool (free all cuts) */
void cut_pool_clear(CutPool *pool) {
    if (!pool) return;

    for (int i = 0; i < pool->count; i++) {
        cut_free(pool->cuts[i]);
    }
    pool->count = 0;
}

/* ============================================================================
 * SCP-Specific Cutting Planes (Phase 3)
 * ============================================================================ */

/*
 * Build conflict graph for set covering/partitioning problems.
 *
 * Two sets (variables) i and j conflict if they both cover the same element.
 * This is detected by finding non-zero entries in the same row of the matrix.
 */
ConflictGraph *conflict_graph_create(const LPModel *model, const SetCoverSignature *sig) {
    if (!model || !sig || sig->type == RALPH_SETCOVER_NONE) return NULL;

    int n = sig->num_sets;
    int m = sig->num_elements;

    ConflictGraph *graph = (ConflictGraph *)calloc(1, sizeof(ConflictGraph));
    if (!graph) return NULL;

    graph->num_vars = n;
    graph->adj_ptr = (int *)calloc(n + 1, sizeof(int));
    if (!graph->adj_ptr) {
        free(graph);
        return NULL;
    }

    /* First pass: count edges per vertex
     * For each element (row), all pairs of covering sets form edges */

    /* Build row representation: for each row, list of columns with non-zero */
    int **row_cols = (int **)calloc(m, sizeof(int *));
    int *row_count = (int *)calloc(m, sizeof(int));
    if (!row_cols || !row_count) {
        free(row_cols);
        free(row_count);
        free(graph->adj_ptr);
        free(graph);
        return NULL;
    }

    /* Count non-zeros per row using column-major sparse matrix */
    SparseMatrix *A = model->A;
    for (int j = 0; j < n; j++) {
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            int row = A->rowidx[p];
            if (row < m && fabs(A->values[p]) > RALPH_ZERO_TOL) {
                row_count[row]++;
            }
        }
    }

    /* Allocate row column lists */
    for (int i = 0; i < m; i++) {
        if (row_count[i] > 0) {
            row_cols[i] = (int *)calloc(row_count[i], sizeof(int));
            if (!row_cols[i]) {
                for (int k = 0; k < i; k++) free(row_cols[k]);
                free(row_cols);
                free(row_count);
                free(graph->adj_ptr);
                free(graph);
                return NULL;
            }
        }
        row_count[i] = 0;  /* Reset for filling */
    }

    /* Fill row column lists */
    for (int j = 0; j < n; j++) {
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            int row = A->rowidx[p];
            if (row < m && fabs(A->values[p]) > RALPH_ZERO_TOL) {
                row_cols[row][row_count[row]++] = j;
            }
        }
    }

    /* Count edges: for each row, each pair of columns is an edge
     * Use a hash set approach with bit array for deduplication */
    int *edge_count = (int *)calloc(n, sizeof(int));
    if (!edge_count) {
        for (int i = 0; i < m; i++) free(row_cols[i]);
        free(row_cols);
        free(row_count);
        free(graph->adj_ptr);
        free(graph);
        return NULL;
    }

    /* Temporary adjacency sets (bit arrays would be more efficient for large n) */
    int **temp_adj = (int **)calloc(n, sizeof(int *));
    int *temp_cap = (int *)calloc(n, sizeof(int));
    if (!temp_adj || !temp_cap) {
        free(temp_adj);
        free(temp_cap);
        free(edge_count);
        for (int i = 0; i < m; i++) free(row_cols[i]);
        free(row_cols);
        free(row_count);
        free(graph->adj_ptr);
        free(graph);
        return NULL;
    }

    /* For each element (row), add edges between all covering sets */
    for (int i = 0; i < m; i++) {
        int cnt = row_count[i];
        for (int a = 0; a < cnt; a++) {
            int col_a = row_cols[i][a];
            for (int b = a + 1; b < cnt; b++) {
                int col_b = row_cols[i][b];

                /* Add edge col_a -- col_b (both directions) */
                /* Check if edge already exists */
                int found_ab = 0, found_ba = 0;
                for (int k = 0; k < edge_count[col_a]; k++) {
                    if (temp_adj[col_a][k] == col_b) { found_ab = 1; break; }
                }
                for (int k = 0; k < edge_count[col_b]; k++) {
                    if (temp_adj[col_b][k] == col_a) { found_ba = 1; break; }
                }

                if (!found_ab) {
                    /* Expand if needed */
                    if (edge_count[col_a] >= temp_cap[col_a]) {
                        int new_cap = temp_cap[col_a] == 0 ? 8 : temp_cap[col_a] * 2;
                        int *new_adj = (int *)realloc(temp_adj[col_a], new_cap * sizeof(int));
                        if (!new_adj) goto cleanup_error;
                        temp_adj[col_a] = new_adj;
                        temp_cap[col_a] = new_cap;
                    }
                    temp_adj[col_a][edge_count[col_a]++] = col_b;
                }
                if (!found_ba) {
                    if (edge_count[col_b] >= temp_cap[col_b]) {
                        int new_cap = temp_cap[col_b] == 0 ? 8 : temp_cap[col_b] * 2;
                        int *new_adj = (int *)realloc(temp_adj[col_b], new_cap * sizeof(int));
                        if (!new_adj) goto cleanup_error;
                        temp_adj[col_b] = new_adj;
                        temp_cap[col_b] = new_cap;
                    }
                    temp_adj[col_b][edge_count[col_b]++] = col_a;
                }
            }
        }
    }

    /* Build CSR structure */
    graph->adj_ptr[0] = 0;
    int total_edges = 0;
    for (int j = 0; j < n; j++) {
        total_edges += edge_count[j];
        graph->adj_ptr[j + 1] = total_edges;
    }
    graph->num_edges = total_edges / 2;  /* Each edge counted twice */

    graph->adj_list = (int *)calloc(total_edges > 0 ? total_edges : 1, sizeof(int));
    if (!graph->adj_list) goto cleanup_error;

    /* Copy adjacency lists */
    for (int j = 0; j < n; j++) {
        for (int k = 0; k < edge_count[j]; k++) {
            graph->adj_list[graph->adj_ptr[j] + k] = temp_adj[j][k];
        }
    }

    /* Cleanup temporary structures */
    for (int j = 0; j < n; j++) free(temp_adj[j]);
    free(temp_adj);
    free(temp_cap);
    free(edge_count);
    for (int i = 0; i < m; i++) free(row_cols[i]);
    free(row_cols);
    free(row_count);

    return graph;

cleanup_error:
    for (int j = 0; j < n; j++) free(temp_adj[j]);
    free(temp_adj);
    free(temp_cap);
    free(edge_count);
    for (int i = 0; i < m; i++) free(row_cols[i]);
    free(row_cols);
    free(row_count);
    free(graph->adj_list);
    free(graph->adj_ptr);
    free(graph);
    return NULL;
}

void conflict_graph_free(ConflictGraph *graph) {
    if (!graph) return;
    free(graph->adj_list);
    free(graph->adj_ptr);
    free(graph);
}

/*
 * Check if vertex v is adjacent to all vertices in the clique.
 */
static int is_clique_neighbor(const ConflictGraph *graph, const int *clique,
                               int clique_size, int v) {
    for (int i = 0; i < clique_size; i++) {
        int u = clique[i];
        /* Check if v is in adjacency list of u */
        int found = 0;
        for (int p = graph->adj_ptr[u]; p < graph->adj_ptr[u + 1]; p++) {
            if (graph->adj_list[p] == v) {
                found = 1;
                break;
            }
        }
        if (!found) return 0;
    }
    return 1;
}

/*
 * Greedily extend a clique starting from edge (u, v).
 * Returns clique size.
 */
static int extend_clique_greedy(const ConflictGraph *graph, int u, int v,
                                 const double *x, int *clique, int max_size) {
    clique[0] = u;
    clique[1] = v;
    int size = 2;

    /* Find common neighbors of all vertices in clique */
    /* Greedily add vertex with highest LP value */
    while (size < max_size) {
        int best_w = -1;
        double best_val = -1.0;

        /* Check all neighbors of first vertex */
        for (int p = graph->adj_ptr[u]; p < graph->adj_ptr[u + 1]; p++) {
            int w = graph->adj_list[p];

            /* Skip if already in clique */
            int in_clique = 0;
            for (int i = 0; i < size; i++) {
                if (clique[i] == w) { in_clique = 1; break; }
            }
            if (in_clique) continue;

            /* Check if w is adjacent to all clique members */
            if (is_clique_neighbor(graph, clique, size, w)) {
                if (x[w] > best_val) {
                    best_val = x[w];
                    best_w = w;
                }
            }
        }

        if (best_w < 0) break;  /* No more vertices can be added */
        clique[size++] = best_w;
    }

    return size;
}

/*
 * Generate clique cuts from conflict graph.
 */
int generate_clique_cuts(MIPSolver *solver, CutPool *pool, const ConflictGraph *graph) {
    if (!solver || !pool || !graph) return 0;
    if (!solver->lp_solver || !solver->lp_solver->solution) return 0;

    const double *x = solver->lp_solver->solution;
    int n = graph->num_vars;
    int cuts_added = 0;
    int max_clique_size = 64;  /* Reasonable limit */

    int *clique = (int *)calloc(max_clique_size, sizeof(int));
    if (!clique) return 0;

    /* For each edge with sufficient fractional LP value, try to extend */
    for (int u = 0; u < n && cuts_added < solver->max_cuts_per_round; u++) {
        if (x[u] < 0.1) continue;  /* Skip vertices with low LP value */

        for (int p = graph->adj_ptr[u]; p < graph->adj_ptr[u + 1]; p++) {
            int v = graph->adj_list[p];
            if (v <= u) continue;  /* Avoid processing same edge twice */
            if (x[u] + x[v] < 0.8) continue;  /* Skip edges unlikely to be violated */

            /* Extend clique greedily */
            int size = extend_clique_greedy(graph, u, v, x, clique, max_clique_size);

            if (size < 2) continue;

            /* Calculate violation: sum(x_j) - 1 for clique cut sum <= 1 */
            double lhs = 0.0;
            for (int i = 0; i < size; i++) {
                lhs += x[clique[i]];
            }
            double violation = lhs - 1.0;

            if (violation < RALPH_FEAS_TOL) continue;

            /* Create clique cut: sum(x_j : j in clique) <= 1 */
            Cut *cut = cut_create(size);
            if (!cut) continue;

            cut->type = CUT_CLIQUE;
            cut->sense = 'L';
            cut->rhs = 1.0;
            cut->violation = violation;

            for (int i = 0; i < size; i++) {
                cut->indices[cut->nnz] = clique[i];
                cut->values[cut->nnz] = 1.0;
                cut->nnz++;
            }

            cut_pool_add(pool, cut);
            cuts_added++;

            if (cuts_added >= solver->max_cuts_per_round) break;
        }
    }

    free(clique);
    return cuts_added;
}

/*
 * BFS to find shortest odd cycle containing start vertex.
 * Returns cycle length, fills cycle array. Returns 0 if no odd cycle found.
 */
static int find_odd_cycle_bfs(const ConflictGraph *graph, int start,
                               int *cycle, int max_len, int *visited, int *parent) {
    int n = graph->num_vars;

    /* Reset visited and parent */
    for (int i = 0; i < n; i++) {
        visited[i] = -1;
        parent[i] = -1;
    }

    /* BFS with distance tracking */
    int *queue = (int *)calloc(n, sizeof(int));
    if (!queue) return 0;

    int head = 0, tail = 0;
    queue[tail++] = start;
    visited[start] = 0;

    int cycle_len = 0;
    int cycle_end = -1;

    while (head < tail && cycle_len == 0) {
        int u = queue[head++];
        int u_dist = visited[u];

        for (int p = graph->adj_ptr[u]; p < graph->adj_ptr[u + 1]; p++) {
            int v = graph->adj_list[p];

            if (visited[v] < 0) {
                /* Not visited */
                visited[v] = u_dist + 1;
                parent[v] = u;
                queue[tail++] = v;
            } else if (v != parent[u]) {
                /* Found a cycle: path from start to u, edge u-v, path from v to start */
                int total_len = u_dist + 1 + visited[v];
                if (total_len % 2 == 1 && total_len >= 3) {
                    /* Odd cycle found */
                    if (total_len <= max_len) {
                        cycle_len = total_len;
                        cycle_end = u;
                        /* Also need to track v for reconstruction */
                        /* Store v in a way we can recover */
                        parent[u] = v;  /* Temporarily overwrite */
                    }
                    break;
                }
            }
        }
    }

    free(queue);

    if (cycle_len == 0) return 0;

    /* Reconstruct cycle */
    /* Path from start to cycle_end, then edge to v, then path from v to start */
    /* This is complex - simplified approach: just return the cycle vertices */

    /* For now, use simplified reconstruction: trace back from both ends */
    int *path1 = (int *)calloc(cycle_len, sizeof(int));
    int *path2 = (int *)calloc(cycle_len, sizeof(int));
    if (!path1 || !path2) {
        free(path1);
        free(path2);
        return 0;
    }

    /* Trace from cycle_end back */
    int len1 = 0;
    int curr = cycle_end;
    int v_end = parent[cycle_end];  /* The vertex we connected to */

    /* Restore parent */
    parent[cycle_end] = -1;
    for (int p = graph->adj_ptr[cycle_end]; p < graph->adj_ptr[cycle_end + 1]; p++) {
        int maybe_parent = graph->adj_list[p];
        if (maybe_parent != v_end && visited[maybe_parent] == visited[cycle_end] - 1) {
            parent[cycle_end] = maybe_parent;
            break;
        }
    }

    /* Trace path from cycle_end to start */
    curr = cycle_end;
    while (curr != start && len1 < cycle_len) {
        path1[len1++] = curr;
        curr = parent[curr];
        if (curr < 0) break;
    }
    if (curr == start) path1[len1++] = start;

    /* Trace path from v_end to start */
    int len2 = 0;
    curr = v_end;
    while (curr != start && len2 < cycle_len) {
        path2[len2++] = curr;
        /* Find parent of curr */
        int found_parent = -1;
        for (int p = graph->adj_ptr[curr]; p < graph->adj_ptr[curr + 1]; p++) {
            int maybe = graph->adj_list[p];
            if (visited[maybe] == visited[curr] - 1) {
                found_parent = maybe;
                break;
            }
        }
        curr = found_parent;
        if (curr < 0) break;
    }

    /* Combine: path1 (reversed) + path2 */
    int idx = 0;
    for (int i = len1 - 1; i >= 0 && idx < cycle_len; i--) {
        cycle[idx++] = path1[i];
    }
    for (int i = 0; i < len2 && idx < cycle_len; i++) {
        cycle[idx++] = path2[i];
    }

    free(path1);
    free(path2);

    /* Verify cycle length is odd */
    if (idx % 2 == 0) return 0;  /* Not odd */

    return idx;
}

/*
 * Generate odd-hole cuts from conflict graph.
 */
int generate_odd_hole_cuts(MIPSolver *solver, CutPool *pool, const ConflictGraph *graph) {
    if (!solver || !pool || !graph) return 0;
    if (!solver->lp_solver || !solver->lp_solver->solution) return 0;

    const double *x = solver->lp_solver->solution;
    int n = graph->num_vars;
    int cuts_added = 0;
    int max_cycle_len = 15;  /* Limit cycle length for efficiency */

    int *cycle = (int *)calloc(max_cycle_len, sizeof(int));
    int *visited = (int *)calloc(n, sizeof(int));
    int *parent = (int *)calloc(n, sizeof(int));
    if (!cycle || !visited || !parent) {
        free(cycle);
        free(visited);
        free(parent);
        return 0;
    }

    /* Try to find odd cycles starting from vertices with high fractional value */
    for (int start = 0; start < n && cuts_added < solver->max_cuts_per_round; start++) {
        if (x[start] < 0.3) continue;  /* Skip low-value vertices */

        int len = find_odd_cycle_bfs(graph, start, cycle, max_cycle_len, visited, parent);
        if (len < 3 || len % 2 == 0) continue;  /* Need odd cycle of length >= 3 */

        /* Calculate violation: sum(x_j) - k for cycle length 2k+1 */
        double lhs = 0.0;
        for (int i = 0; i < len; i++) {
            lhs += x[cycle[i]];
        }
        int k = len / 2;
        double rhs = (double)k;
        double violation = lhs - rhs;

        if (violation < RALPH_FEAS_TOL) continue;

        /* Create odd-hole cut: sum(x_j : j in cycle) <= k */
        Cut *cut = cut_create(len);
        if (!cut) continue;

        cut->type = CUT_ODD_HOLE;
        cut->sense = 'L';
        cut->rhs = rhs;
        cut->violation = violation;

        for (int i = 0; i < len; i++) {
            cut->indices[cut->nnz] = cycle[i];
            cut->values[cut->nnz] = 1.0;
            cut->nnz++;
        }

        cut_pool_add(pool, cut);
        cuts_added++;
    }

    free(cycle);
    free(visited);
    free(parent);

    return cuts_added;
}

/*
 * Sequential lifting for cover inequalities.
 *
 * Given a cover C with sum(x_j : j in C) <= |C| - 1,
 * we can strengthen it by lifting coefficients for variables not in C.
 *
 * For each variable k not in C, compute lifting coefficient a_k:
 *   a_k = |C| - 1 - max{ sum(x_j : j in C) : x feasible, x_k = 1 }
 *
 * This is done via a simple knapsack DP.
 */
static int compute_lifting_coef(const double *coefs, const int *vars, int num_vars,
                                  double rhs, const int *in_cover, int cover_size,
                                  int var_to_lift) {
    /* Knapsack DP: compute max number of cover elements we can select
     * when var_to_lift is fixed to 1 */

    /* Get coefficient of var_to_lift */
    double lift_coef = 0.0;
    for (int i = 0; i < num_vars; i++) {
        if (vars[i] == var_to_lift) {
            lift_coef = coefs[i];
            break;
        }
    }

    if (lift_coef < RALPH_ZERO_TOL) return 0;

    /* Remaining capacity after selecting var_to_lift */
    double remaining = rhs - lift_coef;
    if (remaining < -RALPH_ZERO_TOL) {
        /* var_to_lift alone exceeds capacity - lifting coef is |C| - 1 */
        return cover_size - 1;
    }

    /* DP: dp[w] = max items from cover we can fit with capacity w */
    /* Use integer weights (scale by 1000 for precision) */
    int scale = 1000;
    int capacity = (int)(remaining * scale + 0.5);
    if (capacity < 0) capacity = 0;
    if (capacity > 100000) capacity = 100000;  /* Limit for efficiency */

    int *dp = (int *)calloc(capacity + 1, sizeof(int));
    if (!dp) return 0;

    /* Process each cover item */
    for (int i = 0; i < num_vars; i++) {
        if (!in_cover[i]) continue;
        int w = (int)(coefs[i] * scale + 0.5);
        if (w <= 0) continue;

        for (int c = capacity; c >= w; c--) {
            if (dp[c - w] + 1 > dp[c]) {
                dp[c] = dp[c - w] + 1;
            }
        }
    }

    int max_selected = dp[capacity];
    free(dp);

    /* Lifting coefficient: |C| - 1 - max_selected */
    int lift = cover_size - 1 - max_selected;
    return lift > 0 ? lift : 0;
}

/*
 * Generate lifted cover inequalities.
 */
int generate_lifted_cover_cuts(MIPSolver *solver, CutPool *pool) {
    if (!solver || !solver->lp_solver || !solver->lp_solver->solution) return 0;

    LPModel *model = solver->original_model;
    const double *x = solver->lp_solver->solution;
    int n = model->num_vars;
    int m = model->num_cons;
    int cuts_added = 0;

    if (model->num_binary == 0) return 0;

    double *coefs = (double *)calloc(n, sizeof(double));
    int *vars = (int *)calloc(n, sizeof(int));
    int *in_cover = (int *)calloc(n, sizeof(int));
    double *lift_coefs = (double *)calloc(n, sizeof(double));
    if (!coefs || !vars || !in_cover || !lift_coefs) {
        free(coefs);
        free(vars);
        free(in_cover);
        free(lift_coefs);
        return 0;
    }

    /* Scan constraints for knapsack structure */
    for (int row = 0; row < m && cuts_added < solver->max_cuts_per_round; row++) {
        /* Get row data */
        double *row_data = (double *)calloc(n, sizeof(double));
        if (!row_data) continue;
        sparse_get_row(model->A, row, row_data);

        /* Check if knapsack constraint */
        int num_vars_in_row = 0;
        int is_knapsack = 1;
        for (int j = 0; j < n; j++) {
            double aij = row_data[j];
            if (fabs(aij) < RALPH_ZERO_TOL) continue;

            if (aij < RALPH_ZERO_TOL) { is_knapsack = 0; break; }
            if (model->var_type[j] != 'B') { is_knapsack = 0; break; }

            coefs[num_vars_in_row] = aij;
            vars[num_vars_in_row] = j;
            num_vars_in_row++;
        }
        free(row_data);

        if (!is_knapsack || num_vars_in_row < 3) continue;
        if (model->sense[row] != 'L' && model->sense[row] != 'E') continue;

        double rhs = model->b[row];

        /* Build minimal cover greedily (same as basic cover cuts) */
        double coef_sum = 0.0;
        for (int i = 0; i < num_vars_in_row; i++) {
            coef_sum += coefs[i];
            in_cover[i] = 0;
        }
        if (coef_sum <= rhs + RALPH_ZERO_TOL) continue;

        /* Sort by LP value descending */
        int *order = (int *)calloc(num_vars_in_row, sizeof(int));
        if (!order) continue;
        for (int i = 0; i < num_vars_in_row; i++) order[i] = i;
        for (int i = 0; i < num_vars_in_row - 1; i++) {
            for (int j = i + 1; j < num_vars_in_row; j++) {
                if (x[vars[order[j]]] > x[vars[order[i]]]) {
                    int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
                }
            }
        }

        /* Build cover */
        double cover_coef_sum = 0.0;
        int cover_size = 0;
        for (int i = 0; i < num_vars_in_row && cover_coef_sum <= rhs; i++) {
            int idx = order[i];
            in_cover[idx] = 1;
            cover_coef_sum += coefs[idx];
            cover_size++;
        }
        free(order);

        if (cover_coef_sum <= rhs + RALPH_ZERO_TOL || cover_size < 2) continue;

        /* Compute lifting coefficients for non-cover variables */
        for (int i = 0; i < num_vars_in_row; i++) {
            if (in_cover[i]) {
                lift_coefs[i] = 1.0;  /* Cover variables have coefficient 1 */
            } else {
                int lc = compute_lifting_coef(coefs, vars, num_vars_in_row,
                                              rhs, in_cover, cover_size, vars[i]);
                lift_coefs[i] = (double)lc;
            }
        }

        /* Check if lifting improved the cut (any non-zero lifting coef) */
        int has_lifting = 0;
        for (int i = 0; i < num_vars_in_row; i++) {
            if (!in_cover[i] && lift_coefs[i] > 0.5) {
                has_lifting = 1;
                break;
            }
        }
        if (!has_lifting) continue;  /* No improvement over basic cover */

        /* Calculate violation */
        double lhs = 0.0;
        for (int i = 0; i < num_vars_in_row; i++) {
            if (lift_coefs[i] > 0.5) {
                lhs += lift_coefs[i] * x[vars[i]];
            }
        }
        double cut_rhs = cover_size - 1.0;
        double violation = lhs - cut_rhs;

        if (violation < RALPH_FEAS_TOL) continue;

        /* Create lifted cover cut */
        Cut *cut = cut_create(num_vars_in_row);
        if (!cut) continue;

        cut->type = CUT_LIFTED_COVER;
        cut->sense = 'L';
        cut->rhs = cut_rhs;
        cut->violation = violation;

        for (int i = 0; i < num_vars_in_row; i++) {
            if (lift_coefs[i] > 0.5) {
                cut->indices[cut->nnz] = vars[i];
                cut->values[cut->nnz] = lift_coefs[i];
                cut->nnz++;
            }
        }

        cut_pool_add(pool, cut);
        cuts_added++;
    }

    free(coefs);
    free(vars);
    free(in_cover);
    free(lift_coefs);

    return cuts_added;
}

/*
 * Combined SCP cut generation.
 */
int generate_scp_cuts(MIPSolver *solver, CutPool *pool) {
    if (!solver || !pool) return 0;

    /* Detect SCP structure */
    SetCoverSignature sig;
    memset(&sig, 0, sizeof(sig));

    if (!detect_set_cover(solver->original_model, &sig)) {
        return 0;  /* Not an SCP */
    }

    int total_cuts = 0;

    /* Build conflict graph */
    ConflictGraph *graph = conflict_graph_create(solver->original_model, &sig);
    if (graph) {
        /* Generate clique cuts */
        int clique_cuts = generate_clique_cuts(solver, pool, graph);
        total_cuts += clique_cuts;

        /* Generate odd-hole cuts (only for SPP where they're most useful) */
        if (sig.type == RALPH_SETCOVER_PARTITIONING) {
            int odd_hole_cuts = generate_odd_hole_cuts(solver, pool, graph);
            total_cuts += odd_hole_cuts;
        }

        conflict_graph_free(graph);
    }

    /* Generate lifted cover cuts */
    int lifted_cuts = generate_lifted_cover_cuts(solver, pool);
    total_cuts += lifted_cuts;

    /* Cleanup */
    detect_set_cover_free(&sig);

    return total_cuts;
}
