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
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include "mip.h"
#include "detect.h"

static double cut_cpu_time_now(void) {
    return (double)clock() / CLOCKS_PER_SEC;
}

static int cut_env_int_or_default(const char *name, int default_value)
{
    const char *value = getenv(name);
    if (!value || !*value) return default_value;
    char *endptr = NULL;
    long parsed = strtol(value, &endptr, 10);
    if (!endptr || *endptr != '\0') return default_value;
    return (int)parsed;
}

static int cut_env_flag_enabled(const char *name)
{
    const char *value = getenv(name);
    return (value && *value && strcmp(value, "0") != 0) ? 1 : 0;
}

static void cut_trace_log(const char *fmt, ...)
{
    va_list ap;
    if (!cut_env_flag_enabled("RALPH_CMIR_TRACE")) return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static double cut_compute_lp_violation(const Cut *cut, const SimplexTableau *tab)
{
    double lhs = 0.0;
    if (!cut || !tab || !tab->x) return 0.0;
    for (int i = 0; i < cut->nnz; i++) {
        lhs += cut->values[i] * tab->x[cut->indices[i]];
    }
    if (cut->sense == 'L') return lhs - cut->rhs;
    if (cut->sense == 'G') return cut->rhs - lhs;
    return fabs(lhs - cut->rhs);
}

static int cut_uses_shifted_continuous(const LPModel *model,
                                       const int *is_integer,
                                       const double *coefs,
                                       int num_orig)
{
    if (!model || !model->var_shifted) return 0;

    for (int j = 0; j < num_orig; j++) {
        if (fabs(coefs[j]) < RALPH_ZERO_TOL) continue;
        if (j >= model->var_shifted_capacity || !model->var_shifted[j]) continue;
        if (is_integer && is_integer[j]) continue;
        return 1;
    }

    return 0;
}

static int model_has_shifted_continuous(const LPModel *model,
                                        const int *is_integer,
                                        int num_orig)
{
    if (!model || !model->var_shifted) return 0;

    int limit = num_orig;
    if (limit > model->var_shifted_capacity) {
        limit = model->var_shifted_capacity;
    }
    for (int j = 0; j < limit; j++) {
        if (!model->var_shifted[j]) continue;
        if (is_integer && is_integer[j]) continue;
        return 1;
    }

    return 0;
}

static double model_var_shift_amount(const LPModel *model, int j)
{
    if (!model || !model->var_shift || j < 0 || j >= model->var_shift_capacity) {
        return 0.0;
    }
    return model->var_shift[j];
}

static double cmir_row_rhs_in_original_space(const LPModel *model,
                                             const double *row_coefs,
                                             double shifted_rhs,
                                             int num_orig,
                                             int basic_var)
{
    double rhs = shifted_rhs;

    if (!model || !row_coefs || num_orig <= 0) return rhs;

    if (basic_var >= 0 && basic_var < num_orig) {
        rhs += model_var_shift_amount(model, basic_var);
    }

    for (int j = 0; j < num_orig; j++) {
        if (fabs(row_coefs[j]) < RALPH_ZERO_TOL) continue;
        rhs += row_coefs[j] * model_var_shift_amount(model, j);
    }

    return rhs;
}

typedef enum {
    GMI_REJECT_NONE = 0,
    GMI_REJECT_EMPTY = 1,
    GMI_REJECT_SIGN = 2,
    GMI_REJECT_VIOLATION = 3,
    GMI_REJECT_SHIFTED = 4
} GMICutRejectReason;

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
static Cut* generate_gmi_cut_from_row(MIPSolver *solver,
                                      SimplexTableau *tab,
                                      int basic_pos,
                                      const int *is_integer,
                                      GMICutRejectReason *reject_reason_out) {
    int m = tab->m;
    int n = tab->n;
    int num_orig = tab->model->num_vars;
    int basic_var = tab->basis[basic_pos];
    int verbose = 0;  /* Set to 1 to enable debug output */
    if (reject_reason_out) *reject_reason_out = GMI_REJECT_NONE;

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
    {
        double t_row_solve = cut_cpu_time_now();
        lu_solve_transpose(tab->lu, row, row);
        if (solver) {
            solver->time_root_gomory_row_solve += cut_cpu_time_now() - t_row_solve;
        }
    }

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
    int uses_shifted_support = 0;

    /* Compute cut coefficients for each non-basic variable */
    {
        double t_substitute = cut_cpu_time_now();
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
            }

            if (j < num_orig) {
                /* Original variable: accumulate coefficient directly */
                if (tab->model->var_shifted &&
                    j < tab->model->var_shifted_capacity &&
                    tab->model->var_shifted[j] &&
                    (!is_integer || !is_integer[j])) {
                    uses_shifted_support = 1;
                }
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

                    /* Get constraint row coefficients and RHS, applying
                     * row normalization sign. The tableau normalizes rows so
                     * RHS >= 0 (multiplying by -1 if needed), and aux_coef
                     * reflects the normalized form. We must use the same
                     * normalization when substituting back. */
                    LPModel *model = tab->model;
                    double rsign = tab->row_sign[con_row];
                    double con_rhs = model->b[con_row] * rsign;

                    /* Adjust RHS: subtract alpha * aux_coef * b */
                    cut->rhs -= final_coef * aux_c * con_rhs;

                    /* Add coefficient contributions from original variables in this row */
                    for (int k = 0; k < num_orig; k++) {
                        /* Get coefficient A[con_row, k] by scanning column k */
                        double a_rk = 0.0;
                        for (int p = model->A->colptr[k]; p < model->A->colptr[k + 1]; p++) {
                            if (model->A->rowidx[p] == con_row) {
                                a_rk = model->A->values[p] * rsign;
                                break;
                            }
                        }
                        if (fabs(a_rk) > RALPH_ZERO_TOL) {
                            if (model->var_shifted &&
                                k < model->var_shifted_capacity &&
                                model->var_shifted[k] &&
                                (!is_integer || !is_integer[k])) {
                                uses_shifted_support = 1;
                            }
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
        if (solver) {
            solver->time_root_gomory_substitute += cut_cpu_time_now() - t_substitute;
        }
    }

    free(row);

    if (uses_shifted_support) {
        if (verbose) {
            printf("[GMI-row] Rejected: shifted continuous support in source row\n");
        }
        if (reject_reason_out) *reject_reason_out = GMI_REJECT_SHIFTED;
        free(cut_coefs);
        cut_free(cut);
        return NULL;
    }

    if (cut_uses_shifted_continuous(tab->model, is_integer, cut_coefs, num_orig)) {
        if (verbose) {
            printf("[GMI-row] Rejected: shifted continuous variable in cut support\n");
        }
        if (reject_reason_out) *reject_reason_out = GMI_REJECT_SHIFTED;
        free(cut_coefs);
        cut_free(cut);
        return NULL;
    }

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
        if (reject_reason_out) *reject_reason_out = GMI_REJECT_EMPTY;
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
        if (reject_reason_out) *reject_reason_out = GMI_REJECT_SIGN;
        cut_free(cut);
        return NULL;
    }

    /* Only return if cut is violated */
    if (cut->violation < RALPH_FEAS_TOL) {
        if (verbose) printf("[GMI-row] Rejected: not violated (violation < %.9f)\n", RALPH_FEAS_TOL);
        if (reject_reason_out) *reject_reason_out = GMI_REJECT_VIOLATION;
        cut_free(cut);
        return NULL;
    }

    if (verbose) printf("[GMI-row] Cut accepted!\n");
    return cut;
}

int generate_gomory_cuts(MIPSolver *solver, CutPool *pool) {
    SimplexTableau *tab = solver->lp_solver->tableau;
    int candidate_cap = solver->max_cuts_per_round * MIP_GOMORY_PREFILTER_MULT;
    int candidate_count = 0;
    int *candidate_rows = NULL;
    double *candidate_scores = NULL;
    int cuts_added = 0;

    if (model_has_shifted_continuous(tab->model, solver->is_integer, tab->model->num_vars)) {
        return 0;
    }

    if (candidate_cap < MIP_GOMORY_PREFILTER_MIN_ROWS) {
        candidate_cap = MIP_GOMORY_PREFILTER_MIN_ROWS;
    }
    if (candidate_cap > MIP_GOMORY_PREFILTER_MAX_ROWS) {
        candidate_cap = MIP_GOMORY_PREFILTER_MAX_ROWS;
    }
    if (candidate_cap > tab->m) {
        candidate_cap = tab->m;
    }
    if (candidate_cap > 0) {
        candidate_rows = (int *)calloc((size_t)candidate_cap, sizeof(int));
        candidate_scores = (double *)calloc((size_t)candidate_cap, sizeof(double));
    }
    if (candidate_cap > 0 && (!candidate_rows || !candidate_scores)) {
        free(candidate_rows);
        free(candidate_scores);
        candidate_rows = NULL;
        candidate_scores = NULL;
        candidate_cap = 0;
    }

    if (solver->verbose >= 2) {
        printf("[GMI] Scanning %d basic positions for cuts\n", tab->m);
        printf("[GMI] Original vars: %d, is_integer array: %p\n",
               solver->original_model->num_vars, (void*)solver->is_integer);
    }

    {
        double t_rank = cut_cpu_time_now();
    for (int k = 0; k < tab->m; k++) {
        int j = tab->basis[k];
        solver->root_gomory_rows_scanned++;

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
        solver->root_gomory_rows_fractional++;

        double frac_score = frac;
        if (frac_score > 0.5) frac_score = 1.0 - frac_score;

        if (solver->verbose >= 2) {
            printf(" -> fractional! score=%.6f\n", frac_score);
        }

        if (candidate_cap <= 0) {
            continue;
        }

        int insert_pos = candidate_count;
        while (insert_pos > 0 && candidate_scores[insert_pos - 1] < frac_score) {
            if (insert_pos < candidate_cap) {
                candidate_scores[insert_pos] = candidate_scores[insert_pos - 1];
                candidate_rows[insert_pos] = candidate_rows[insert_pos - 1];
            }
            insert_pos--;
        }

        if (insert_pos >= candidate_cap) {
            continue;
        }

        candidate_scores[insert_pos] = frac_score;
        candidate_rows[insert_pos] = k;
        if (candidate_count < candidate_cap) {
            candidate_count++;
        }
    }
        solver->time_root_gomory_rank += cut_cpu_time_now() - t_rank;
    }
    solver->root_gomory_rows_ranked += candidate_count;

    if (solver->verbose >= 2) {
        printf("[GMI] Ranked %d candidate rows (cap=%d)\n", candidate_count, candidate_cap);
    }

    for (int idx = 0; idx < candidate_count; idx++) {
        int k = candidate_rows[idx];
        int j = tab->basis[k];
        GMICutRejectReason reject_reason = GMI_REJECT_NONE;
        if (solver->verbose >= 2) {
            printf("[GMI] Candidate %d/%d: basic pos %d var %d score %.6f -> generating cut...\n",
                   idx + 1, candidate_count, k, j, candidate_scores[idx]);
        }

        solver->root_gomory_rows_built++;
        double t_build = cut_cpu_time_now();
        Cut *cut = generate_gmi_cut_from_row(solver, tab, k, solver->is_integer, &reject_reason);
        solver->time_root_gomory_build += cut_cpu_time_now() - t_build;
        if (cut) {
            if (solver->verbose >= 2) {
                printf("[GMI] Cut generated: ");
                for (int i = 0; i < cut->nnz; i++) {
                    printf("%.4f*x%d ", cut->values[i], cut->indices[i]);
                }
                printf(">= %.4f (violation=%.4f)\n", cut->rhs, cut->violation);
            }
            {
                int pool_count_before = pool->count;
                double t_pool = cut_cpu_time_now();
                cut_pool_add(pool, cut);
                solver->time_root_gomory_pool += cut_cpu_time_now() - t_pool;
                if (pool->count == pool_count_before) {
                    solver->root_gomory_pool_duplicates++;
                }
            }
            cuts_added++;

            if (cuts_added >= solver->max_cuts_per_round) break;
        } else {
            if (reject_reason == GMI_REJECT_EMPTY) {
                solver->root_gomory_reject_empty++;
            } else if (reject_reason == GMI_REJECT_SIGN) {
                solver->root_gomory_reject_sign++;
            } else if (reject_reason == GMI_REJECT_VIOLATION) {
                solver->root_gomory_reject_violation++;
            }
            if (solver->verbose >= 2) printf("[GMI] Cut was NULL (filtered out)\n");
        }
    }

    free(candidate_rows);
    free(candidate_scores);
    return cuts_added;
}

/* ============================================================================
 * Complemented Mixed Integer Rounding (c-MIR) Cuts
 *
 * Implements the c-MIR separation procedure with:
 * 1. Bound substitution: transform variables to non-negative
 * 2. Complement set search: find best subset C of integer variables to complement
 * 3. Delta search: find best divisor for MIR rounding
 * 4. Row aggregation: combine multiple constraint rows for stronger cuts
 *
 * References:
 * - Marchand & Wolsey, "Aggregation and MIR closures" (2001)
 * - GLPK mirgen.c implementation
 * ============================================================================ */

/* c-MIR local constants */
#define CMIR_MAX_AGGR     3      /* Hard cap; runtime default matches this unless gated */
#define CMIR_FRAC_TOL     0.01   /* Min fractionality for MIR RHS */
#define CMIR_COEF_MAX     1e6    /* Reject cuts with coefficients beyond this */
#define CMIR_PIVOT_MIN    0.001  /* Min coefficient for aggregation pivot */
#define CMIR_DELTA_MIN    1e-6   /* Min divisor to avoid numerical instability */

/* Working data for c-MIR separation pipeline */
typedef struct {
    double *a;          /* Dense source row coefficients [num_orig] */
    double b;           /* Source row RHS */
    double *x_val;      /* LP solution snapshot [num_orig] */
    double *lb;         /* Variable lower bounds [num_orig] */
    double *ub;         /* Variable upper bounds [num_orig] */
    const int *is_int;  /* Integer flags [num_orig] (borrowed, not owned) */
    int num_orig;       /* Number of original variables */
    double *a_sub;      /* After bound substitution [num_orig] */
    double b_sub;       /* Substituted RHS */
    int *sub_type;      /* 0=lower-bound sub, 1=upper-bound sub [num_orig] */
    int *in_C;          /* Complement set: 1 if j complemented [num_orig] */
} CMIRWork;

/*
 * Extract a (possibly aggregated) source row from the simplex tableau.
 *
 * Base case (aggr_depth==0): extracts the tableau row for basic_pos and
 * substitutes slacks back to original variables.
 *
 * Aggregated case (aggr_depth>0): finds a continuous pivot variable in the
 * current row, locates an original constraint containing it, and performs
 * Gaussian elimination to combine the rows.
 */
static int cmir_extract_source_row(
    SimplexTableau *tab, int basic_pos, const int *is_integer,
    double *row_coefs, double *row_rhs,
    int aggr_depth, int *used_rows, const double *x_val,
    const double *lb_val, const double *ub_val,
    int *used_shifted_pivot_out)
{
    int m = tab->m;
    int n = tab->n;
    int num_orig = tab->model->num_vars;
    LPModel *model = tab->model;
    int basic_var = tab->basis[basic_pos];

    if (used_shifted_pivot_out) {
        *used_shifted_pivot_out = 0;
    }

    if (basic_pos < 0 || basic_pos >= model->num_cons) return 0;
    if (model->con_origin &&
        (basic_pos >= model->con_origin_capacity || model->con_origin[basic_pos] < 0)) {
        return 0;
    }

    if (aggr_depth == 0) {
        /* Base case: compute tableau row e_i' * B^{-1} * A */
        double *pi = (double*)calloc(m, sizeof(double));
        if (!pi) return 0;

        pi[basic_pos] = 1.0;
        lu_solve_transpose(tab->lu, pi, pi);

        /* Zero out the result */
        memset(row_coefs, 0, num_orig * sizeof(double));

        /* For each non-basic variable, compute tableau coefficient */
        for (int j = 0; j < n; j++) {
            if (tab->var_status[j] == RALPH_BASIC) continue;

            double a_ij = 0.0;
            for (int p = tab->A_ext->colptr[j]; p < tab->A_ext->colptr[j + 1]; p++) {
                a_ij += pi[tab->A_ext->rowidx[p]] * tab->A_ext->values[p];
            }

            if (fabs(a_ij) < RALPH_ZERO_TOL) continue;

            if (j < num_orig) {
                /* Original variable: accumulate directly
                 * Tableau row: x_B = beta - sum_NB a_ij x_j
                 * Rearrange: sum_NB a_ij x_j <= beta
                 */
                row_coefs[j] += a_ij;
            } else {
                /* Slack variable: substitute using constraint mapping */
                int aux_idx = j - num_orig;
                if (aux_idx >= 0 && aux_idx < tab->num_aux &&
                    tab->aux_row && tab->aux_coef) {
                    int con_row = tab->aux_row[aux_idx];
                    double aux_c = tab->aux_coef[aux_idx];
                    double rsign = tab->row_sign[con_row];
                    double con_rhs = model->b[con_row] * rsign;

                    /* s = aux_c * (norm_b - norm_A*x), so a_ij * s contributes:
                     * -a_ij * aux_c * norm_a_rk to each x_k
                     * +a_ij * aux_c * norm_b to RHS
                     * Row normalization sign (rsign) must be applied since
                     * aux_coef reflects the normalized constraint form. */
                    for (int k = 0; k < num_orig; k++) {
                        double a_rk = 0.0;
                        for (int p = model->A->colptr[k]; p < model->A->colptr[k + 1]; p++) {
                            if (model->A->rowidx[p] == con_row) {
                                a_rk = model->A->values[p] * rsign;
                                break;
                            }
                        }
                        if (fabs(a_rk) > RALPH_ZERO_TOL) {
                            row_coefs[k] -= a_ij * aux_c * a_rk;
                        }
                    }
                    *row_rhs += a_ij * aux_c * con_rhs;
                }
            }
        }

        /* RHS = basic variable value + slack adjustments.
         * The basic variable is NOT added to the LHS — its value is fully
         * captured in the RHS via tab->x[basic_var], same as GMI treatment.
         * Adding it to LHS causes degenerate complement decisions after
         * presolve tightens its bounds. */
        *row_rhs = tab->x[basic_var] + (*row_rhs);
        *row_rhs = cmir_row_rhs_in_original_space(model, row_coefs, *row_rhs, num_orig, basic_var);

        free(pi);

        /* Reject if row is trivially empty or has exploded coefficients */
        double max_coef = 0.0;
        int nnz = 0;
        for (int j = 0; j < num_orig; j++) {
            if (fabs(row_coefs[j]) > RALPH_ZERO_TOL) {
                nnz++;
                if (fabs(row_coefs[j]) > max_coef)
                    max_coef = fabs(row_coefs[j]);
            }
        }
        if (nnz == 0 || max_coef > 1e8) return 0;

        return 1;
    }

    /* Aggregation case: find a continuous pivot variable in the current row */
    int kappa = -1;
    double best_slack = -1.0;

    for (int j = 0; j < num_orig; j++) {
        if (fabs(row_coefs[j]) < CMIR_PIVOT_MIN) continue;
        if (is_integer && is_integer[j]) continue;
        double dist_lb = (lb_val && lb_val[j] > -RALPH_INFINITY + 1.0) ?
                          x_val[j] - lb_val[j] : RALPH_INFINITY;
        double dist_ub = (ub_val && ub_val[j] < RALPH_INFINITY - 1.0) ?
                          ub_val[j] - x_val[j] : RALPH_INFINITY;

        if (dist_lb < CMIR_PIVOT_MIN && dist_ub < CMIR_PIVOT_MIN) continue;

        double min_dist = (dist_lb < dist_ub) ? dist_lb : dist_ub;
        if (min_dist > best_slack) {
            best_slack = min_dist;
            kappa = j;
        }
    }

    if (kappa < 0) return 0;
    if (used_shifted_pivot_out &&
        model->var_shifted &&
        kappa < model->var_shifted_capacity &&
        model->var_shifted[kappa]) {
        *used_shifted_pivot_out = 1;
    }
    /* Find an original constraint row containing kappa that isn't used yet */
    int pivot_row = -1;
    double pivot_val = 0.0;

    for (int p = model->A->colptr[kappa]; p < model->A->colptr[kappa + 1]; p++) {
        int row = model->A->rowidx[p];
        if (model->con_origin &&
            (row >= model->con_origin_capacity || model->con_origin[row] < 0)) {
            continue;
        }
        if (used_rows[row]) continue;
        double val = model->A->values[p];
        if (fabs(val) < CMIR_PIVOT_MIN) continue;

        /* Prefer the constraint with the largest pivot element */
        if (fabs(val) > fabs(pivot_val)) {
            pivot_val = val;
            pivot_row = row;
        }
    }

    if (pivot_row < 0) return 0;

    cut_trace_log("[CMIR] basic_pos=%d basic_var=%d kappa=%d shifted=%d pivot_row=%d origin=%d sense=%c pivot_val=%.6f scale=%.6f rhs=%.6f\n",
                  basic_pos,
                  basic_var,
                  kappa,
                  (model->var_shifted && kappa < model->var_shifted_capacity) ? model->var_shifted[kappa] : 0,
                  pivot_row,
                  (model->con_origin && pivot_row < model->con_origin_capacity) ? model->con_origin[pivot_row] : pivot_row,
                  (pivot_row >= 0 && pivot_row < model->num_cons) ? model->sense[pivot_row] : '?',
                  pivot_val,
                  -row_coefs[kappa] / pivot_val,
                  *row_rhs);

    /* Extract the constraint row into a temp array */
    double *con_row = (double*)calloc(num_orig, sizeof(double));
    if (!con_row) return 0;
    sparse_get_row(model->A, pivot_row, con_row);

    /* Gaussian eliminate: scale and add to cancel kappa */
    double scale = -row_coefs[kappa] / pivot_val;

    /* Check for coefficient explosion before committing */
    for (int j = 0; j < num_orig; j++) {
        if (fabs(con_row[j]) > RALPH_ZERO_TOL) {
            double new_val = row_coefs[j] + scale * con_row[j];
            if (fabs(new_val) > 1e8) {
                free(con_row);
                return 0;
            }
        }
    }

    for (int j = 0; j < num_orig; j++) {
        row_coefs[j] += scale * con_row[j];
        if (fabs(row_coefs[j]) < RALPH_ZERO_TOL)
            row_coefs[j] = 0.0;
    }
    {
        double pivot_rhs = model->b[pivot_row];
        pivot_rhs = cmir_row_rhs_in_original_space(model, con_row, pivot_rhs, num_orig, -1);
        *row_rhs += scale * pivot_rhs;
    }

    used_rows[pivot_row] = 1;
    free(con_row);

    return 1;
}

/*
 * Bound substitution: transform all variables to non-negative.
 *
 * For each variable x_j with coefficient a_j:
 *   Lower sub: x_j' = x_j - lb_j >= 0, a_j unchanged, b -= a_j * lb_j
 *   Upper sub: x_j' = ub_j - x_j >= 0, a_j = -a_j, b += a_j * ub_j
 *
 * For integer vars: choose bound with smaller |coef * bound| (less growth).
 * For continuous vars: choose closer bound (less slack).
 */
static void cmir_bound_substitute(CMIRWork *work)
{
    int num_orig = work->num_orig;

    memcpy(work->a_sub, work->a, num_orig * sizeof(double));
    work->b_sub = work->b;

    for (int j = 0; j < num_orig; j++) {
        if (fabs(work->a_sub[j]) < RALPH_ZERO_TOL) {
            work->sub_type[j] = 0;
            continue;
        }

        int has_lb = (work->lb[j] > -RALPH_INFINITY + 1.0);
        int has_ub = (work->ub[j] < RALPH_INFINITY - 1.0);

        if (!has_lb && !has_ub) {
            /* Free variable: leave as-is */
            work->sub_type[j] = 0;
            continue;
        }

        if (!has_ub) {
            /* Only lower bound available */
            work->b_sub -= work->a_sub[j] * work->lb[j];
            work->sub_type[j] = 0;
            continue;
        }

        if (!has_lb) {
            /* Only upper bound available */
            work->b_sub -= work->a_sub[j] * work->ub[j];
            work->a_sub[j] = -work->a_sub[j];
            work->sub_type[j] = 1;
            continue;
        }

        /* Both bounds available: choose based on variable type */
        int use_upper;
        if (work->is_int && work->is_int[j]) {
            /* Integer: minimize coefficient growth */
            double lb_cost = fabs(work->a_sub[j] * work->lb[j]);
            double ub_cost = fabs(work->a_sub[j] * work->ub[j]);
            use_upper = (ub_cost < lb_cost);
        } else {
            /* Continuous: choose closer bound */
            use_upper = (work->ub[j] - work->x_val[j]) <
                        (work->x_val[j] - work->lb[j]);
        }

        if (use_upper) {
            work->b_sub -= work->a_sub[j] * work->ub[j];
            work->a_sub[j] = -work->a_sub[j];
            work->sub_type[j] = 1;
        } else {
            work->b_sub -= work->a_sub[j] * work->lb[j];
            work->sub_type[j] = 0;
        }
    }
}

/*
 * Evaluate the c-MIR cut violation for a given complement set and divisor delta.
 *
 * Applies complementation (for j in C), divides by delta, drops positive
 * continuous terms, then applies the MIR formula. Returns the violation
 * (LHS - floor(b_d)) where positive means the cut is violated.
 *
 * Does NOT modify work->a_sub or work->in_C; uses local temporaries.
 */
static double cmir_eval(const CMIRWork *work, double delta)
{
    int num_orig = work->num_orig;
    double b_d = work->b_sub;

    /* Apply complementation to RHS */
    for (int j = 0; j < num_orig; j++) {
        if (work->in_C[j] && work->is_int[j] && fabs(work->a_sub[j]) > RALPH_ZERO_TOL) {
            /* Complementing j: x_j' -> ub_j' - x_j', coef -> -coef */
            /* ub_j' = ub[j] - lb[j] if lower-sub, or ub[j] - lb[j] if upper-sub
             * But after bound sub, the effective upper bound is ub - lb (for lower)
             * or ub - lb (for upper, but sign flipped) */
            double eff_ub;
            if (work->sub_type[j] == 0) {
                /* Lower-bound sub: x' = x - lb, so x' in [0, ub - lb] */
                eff_ub = work->ub[j] - work->lb[j];
            } else {
                /* Upper-bound sub: x' = ub - x, so x' in [0, ub - lb] */
                eff_ub = work->ub[j] - work->lb[j];
            }
            if (eff_ub < 0.5 || eff_ub > 1e8) continue;  /* Skip if no meaningful ub */
            b_d -= work->a_sub[j] * eff_ub;
        }
    }

    /* Divide by delta */
    if (fabs(delta) < CMIR_DELTA_MIN) return -1.0;
    b_d /= delta;

    double f_0 = b_d - floor(b_d);
    if (f_0 < CMIR_FRAC_TOL || f_0 > 1.0 - CMIR_FRAC_TOL) return -1.0;

    /* Compute MIR LHS and violation */
    double lhs = 0.0;
    double rhs_floor = floor(b_d);

    for (int j = 0; j < num_orig; j++) {
        double a_d = work->a_sub[j];
        if (fabs(a_d) < RALPH_ZERO_TOL) continue;

        /* Apply complementation */
        double eff_ub = 0.0;
        int complemented = 0;
        if (work->in_C[j] && work->is_int[j]) {
            if (work->sub_type[j] == 0) {
                eff_ub = work->ub[j] - work->lb[j];
            } else {
                eff_ub = work->ub[j] - work->lb[j];
            }
            if (eff_ub >= 0.5 && eff_ub <= 1e8) {
                a_d = -a_d;
                complemented = 1;
            }
        }

        a_d /= delta;

        /* Get substituted LP value for this variable */
        double x_sub;
        if (work->sub_type[j] == 0) {
            x_sub = work->x_val[j] - work->lb[j];
        } else {
            x_sub = work->ub[j] - work->x_val[j];
        }
        if (complemented) {
            x_sub = eff_ub - x_sub;
        }

        double mir_coef;
        if (work->is_int && work->is_int[j]) {
            /* Integer: MIR formula */
            double f_j = a_d - floor(a_d);
            mir_coef = floor(a_d) + fmax(f_j - f_0, 0.0) / (1.0 - f_0);
        } else {
            /* Continuous: drop positive, scale negative */
            if (a_d > RALPH_ZERO_TOL) {
                continue;  /* Drop positive continuous terms */
            }
            mir_coef = a_d / (1.0 - f_0);
        }

        lhs += mir_coef * x_sub;
    }

    return lhs - rhs_floor;
}

/*
 * Search for the best complement set C and divisor delta.
 *
 * Initial C: for each integer j with finite ub, set in_C[j] if the
 * substituted value is in the upper half of its range.
 *
 * Delta candidates: |a_sub[j]| for each fractional integer j, plus
 * halved/quartered variants. Complement flipping: try flipping the
 * most ambiguous variables and keep improvements.
 *
 * Returns best violation; stores winning delta in *best_delta.
 */
static double cmir_separate(CMIRWork *work, double *best_delta)
{
    int num_orig = work->num_orig;
    int disable_flip = cut_env_flag_enabled("RALPH_CMIR_DISABLE_COMPLEMENT_FLIPS");

    /* Initialize complement set C */
    for (int j = 0; j < num_orig; j++) {
        work->in_C[j] = 0;
        if (!work->is_int || !work->is_int[j]) continue;
        if (fabs(work->a_sub[j]) < RALPH_ZERO_TOL) continue;

        double eff_ub;
        int has_lb = (work->lb[j] > -RALPH_INFINITY + 1.0);
        int has_ub = (work->ub[j] < RALPH_INFINITY - 1.0);
        if (!has_lb || !has_ub) continue;
        eff_ub = work->ub[j] - work->lb[j];
        if (eff_ub < 0.5) continue;

        /* Substituted LP value */
        double x_sub;
        if (work->sub_type[j] == 0) {
            x_sub = work->x_val[j] - work->lb[j];
        } else {
            x_sub = work->ub[j] - work->x_val[j];
        }

        work->in_C[j] = (x_sub >= 0.5 * eff_ub) ? 1 : 0;
    }

    /* Collect delta candidates from integer variable coefficients */
    double best_viol = -1.0;
    *best_delta = 1.0;

    /* Try delta = |a_sub[j]| for each fractional integer variable, and scaled versions */
    for (int j = 0; j < num_orig; j++) {
        if (!work->is_int || !work->is_int[j]) continue;
        if (fabs(work->a_sub[j]) < CMIR_DELTA_MIN) continue;

        /* Check if variable is fractional */
        double x_sub;
        if (work->sub_type[j] == 0) {
            x_sub = work->x_val[j] - work->lb[j];
        } else {
            x_sub = work->ub[j] - work->x_val[j];
        }
        if (work->in_C[j]) {
            double eff_ub = work->ub[j] - work->lb[j];
            x_sub = eff_ub - x_sub;
        }
        double frac = x_sub - floor(x_sub);
        if (frac < RALPH_INT_TOL || frac > 1.0 - RALPH_INT_TOL) continue;

        double base_d = fabs(work->a_sub[j]);
        if (work->in_C[j]) base_d = fabs(-work->a_sub[j]);  /* After complementation */

        /* Try d, d/2, d/4, d/8 */
        double d = base_d;
        for (int s = 0; s < 4; s++) {
            if (d < CMIR_DELTA_MIN) break;
            double viol = cmir_eval(work, d);
            if (viol > best_viol) {
                best_viol = viol;
                *best_delta = d;
            }
            d *= 0.5;
        }
    }

    /* Also try delta = 1.0 as a baseline */
    {
        double viol = cmir_eval(work, 1.0);
        if (viol > best_viol) {
            best_viol = viol;
            *best_delta = 1.0;
        }
    }

    /* Complement flipping: sort integer variables by ambiguity and try flips */
    /* Build list of flippable integers */
    int *flip_order = (int*)calloc(num_orig, sizeof(int));
    double *flip_score = (double*)calloc(num_orig, sizeof(double));
    int nflip = 0;

    if (!disable_flip && flip_order && flip_score) {
        for (int j = 0; j < num_orig; j++) {
            if (!work->is_int || !work->is_int[j]) continue;
            if (fabs(work->a_sub[j]) < RALPH_ZERO_TOL) continue;
            int has_lb = (work->lb[j] > -RALPH_INFINITY + 1.0);
            int has_ub = (work->ub[j] < RALPH_INFINITY - 1.0);
            if (!has_lb || !has_ub) continue;

            double eff_ub = work->ub[j] - work->lb[j];
            if (eff_ub < 0.5) continue;

            double x_sub;
            if (work->sub_type[j] == 0)
                x_sub = work->x_val[j] - work->lb[j];
            else
                x_sub = work->ub[j] - work->x_val[j];

            /* Score: how ambiguous is the complement decision (closer to 0.5 = more ambiguous) */
            flip_order[nflip] = j;
            flip_score[nflip] = fabs(x_sub - 0.5 * eff_ub);
            nflip++;
        }

        /* Sort by score ascending (most ambiguous first) - simple insertion sort */
        for (int i = 1; i < nflip; i++) {
            int idx = flip_order[i];
            double sc = flip_score[i];
            int k = i - 1;
            while (k >= 0 && flip_score[k] > sc) {
                flip_order[k + 1] = flip_order[k];
                flip_score[k + 1] = flip_score[k];
                k--;
            }
            flip_order[k + 1] = idx;
            flip_score[k + 1] = sc;
        }

        /* Try flipping up to 10 most ambiguous variables.
         * Only evaluate the incumbent delta + the flipped variable's own
         * |a_sub[j]| and halved variants (up to 5 candidates per flip)
         * instead of re-searching all integer variables. Reduces flip phase
         * from O(10 × K × 4 × N) to O(10 × 5 × N). */
        int max_flips = (nflip < 10) ? nflip : 10;
        for (int i = 0; i < max_flips; i++) {
            int j = flip_order[i];
            work->in_C[j] = 1 - work->in_C[j];  /* Flip */

            /* Evaluate incumbent delta with new C */
            double flip_best_delta = *best_delta;
            double flip_best_viol = cmir_eval(work, *best_delta);

            /* Evaluate flipped variable's own |a_sub[j]| and halved variants */
            if (fabs(work->a_sub[j]) >= CMIR_DELTA_MIN) {
                double d = fabs(work->a_sub[j]);
                for (int s = 0; s < 4; s++) {
                    if (d < CMIR_DELTA_MIN) break;
                    double viol = cmir_eval(work, d);
                    if (viol > flip_best_viol) {
                        flip_best_viol = viol;
                        flip_best_delta = d;
                    }
                    d *= 0.5;
                }
            }

            if (flip_best_viol > best_viol) {
                best_viol = flip_best_viol;
                *best_delta = flip_best_delta;
                /* Keep the flip */
            } else {
                work->in_C[j] = 1 - work->in_C[j];  /* Revert */
            }
        }
    }

    free(flip_order);
    free(flip_score);

    return best_viol;
}

/*
 * Build the final cut from the c-MIR formula with winning (C, delta).
 *
 * Applies the c-MIR transformation, computes MIR coefficients, then
 * back-substitutes through complementation and bound substitution
 * to get coefficients in terms of the original variables.
 */
static Cut* cmir_build_cut(const CMIRWork *work, double delta, SimplexTableau *tab)
{
    int num_orig = work->num_orig;

    /* Compute floor(b_d) for the RHS */
    double b_d = work->b_sub;
    for (int j = 0; j < num_orig; j++) {
        if (work->in_C[j] && work->is_int[j] && fabs(work->a_sub[j]) > RALPH_ZERO_TOL) {
            double eff_ub;
            if (work->sub_type[j] == 0)
                eff_ub = work->ub[j] - work->lb[j];
            else
                eff_ub = work->ub[j] - work->lb[j];
            if (eff_ub >= 0.5 && eff_ub <= 1e8)
                b_d -= work->a_sub[j] * eff_ub;
        }
    }
    b_d /= delta;

    double f_0 = b_d - floor(b_d);
    if (f_0 < CMIR_FRAC_TOL || f_0 > 1.0 - CMIR_FRAC_TOL) return NULL;

    /* Compute MIR coefficients in substituted space, then back-substitute */
    double *orig_coefs = (double*)calloc(num_orig, sizeof(double));
    if (!orig_coefs) return NULL;

    double cut_rhs = floor(b_d);
    double max_coef = 0.0;
    int nnz = 0;

    for (int j = 0; j < num_orig; j++) {
        double a_d = work->a_sub[j];
        if (fabs(a_d) < RALPH_ZERO_TOL) continue;

        /* Apply complementation */
        double eff_ub = 0.0;
        int complemented = 0;
        if (work->in_C[j] && work->is_int[j]) {
            if (work->sub_type[j] == 0)
                eff_ub = work->ub[j] - work->lb[j];
            else
                eff_ub = work->ub[j] - work->lb[j];
            if (eff_ub >= 0.5 && eff_ub <= 1e8) {
                a_d = -a_d;
                complemented = 1;
            }
        }

        a_d /= delta;

        double mir_coef;
        if (work->is_int && work->is_int[j]) {
            double f_j = a_d - floor(a_d);
            mir_coef = floor(a_d) + fmax(f_j - f_0, 0.0) / (1.0 - f_0);
        } else {
            if (a_d > RALPH_ZERO_TOL) {
                continue;  /* Drop positive continuous */
            }
            mir_coef = a_d / (1.0 - f_0);
        }

        if (fabs(mir_coef) < RALPH_ZERO_TOL) continue;

        /* Back-substitute: undo delta scaling (already done above),
         * undo complementation, undo bound substitution */

        /* mir_coef is in terms of the substituted+complemented variable x'.
         * We need to express in terms of original x_j.
         *
         * If complemented (j in C):
         *   x' = eff_ub - x_sub, so mir_coef * x' = mir_coef * eff_ub - mir_coef * x_sub
         *   The constant mir_coef * eff_ub moves from LHS to RHS with sign flip:
         *   cut_rhs -= mir_coef * eff_ub, and we use -mir_coef for x_sub
         *
         * If sub_type[j] == 0 (lower-bound sub):
         *   x_sub = x_j - lb_j, so coef * x_sub = coef * x_j - coef * lb_j
         *   orig_coefs[j] = coef, cut_rhs += coef * lb_j
         *
         * If sub_type[j] == 1 (upper-bound sub):
         *   x_sub = ub_j - x_j, so coef * x_sub = coef * ub_j - coef * x_j
         *   The constant coef * ub_j moves from LHS to RHS with sign flip:
         *   orig_coefs[j] = -coef, cut_rhs -= coef * ub_j
         */

        double coef_for_sub = mir_coef;

        if (complemented) {
            cut_rhs -= mir_coef * eff_ub;
            coef_for_sub = -mir_coef;
        }

        if (work->sub_type[j] == 0) {
            /* Lower bound sub: x_sub = x_j - lb */
            orig_coefs[j] = coef_for_sub;
            cut_rhs += coef_for_sub * work->lb[j];
        } else {
            /* Upper bound sub: x_sub = ub - x_j */
            orig_coefs[j] = -coef_for_sub;
            cut_rhs -= coef_for_sub * work->ub[j];
        }

        if (fabs(orig_coefs[j]) > RALPH_ZERO_TOL) {
            nnz++;
            if (fabs(orig_coefs[j]) > max_coef)
                max_coef = fabs(orig_coefs[j]);
        }
    }

    /* Reject if empty or coefficients too large */
    if (nnz == 0 || max_coef > CMIR_COEF_MAX) {
        free(orig_coefs);
        return NULL;
    }

    /* Build the Cut structure */
    Cut *cut = cut_create(nnz);
    if (!cut) {
        free(orig_coefs);
        return NULL;
    }

    cut->type = CUT_MIR;
    cut->sense = 'L';
    cut->rhs = cut_rhs;

    for (int j = 0; j < num_orig; j++) {
        if (fabs(orig_coefs[j]) > RALPH_ZERO_TOL) {
            cut->indices[cut->nnz] = j;
            cut->values[cut->nnz] = orig_coefs[j];
            cut->nnz++;
        }
    }

    free(orig_coefs);

    /* Compute violation against current LP solution */
    double lhs = 0.0;
    for (int k = 0; k < cut->nnz; k++) {
        lhs += cut->values[k] * tab->x[cut->indices[k]];
    }
    cut->violation = lhs - cut->rhs;

    if (cut->violation < RALPH_FEAS_TOL || cut->nnz == 0) {
        cut_free(cut);
        return NULL;
    }

    /* Reject trivially infeasible cuts: compute minimum possible LHS
     * given variable bounds. If min_lhs > rhs, no feasible point can
     * satisfy the cut, indicating a back-substitution error. */
    {
        double min_lhs = 0.0;
        for (int k = 0; k < cut->nnz; k++) {
            int j = cut->indices[k];
            double v = cut->values[k];
            if (v > 0)
                min_lhs += v * work->lb[j];
            else
                min_lhs += v * work->ub[j];
        }
        if (min_lhs > cut->rhs + RALPH_FEAS_TOL) {
            cut_free(cut);
            return NULL;
        }
    }

    return cut;
}

/*
 * Generate c-MIR cuts from the LP relaxation.
 *
 * For each tableau row with fractional RHS (where GMI doesn't apply),
 * tries increasing aggregation depths (0, 1, ..., CMIR_MAX_AGGR).
 * At each depth:
 *   1. Extract (possibly aggregated) source row
 *   2. Bound-substitute to make variables non-negative
 *   3. Search for best complement set C and divisor delta
 *   4. If violation > tolerance, build cut and add to pool
 */
int generate_mir_cuts(MIPSolver *solver, CutPool *pool)
{
    SimplexTableau *tab = solver->lp_solver->tableau;
    int num_orig = tab->model->num_vars;
    int m = tab->m;
    LPModel *row_model = tab->model;

    /* c-MIR is kept opt-in for now. The shifted-presolve correctness path is
     * repaired below, but MIR is still expensive on the current benchmark
     * families, so targeted runs should enable it explicitly. */
    if (!cut_env_flag_enabled("RALPH_ENABLE_MIR_ROOT_CUTS")) {
        return 0;
    }

    int cuts_added = 0;
    int candidate_cap = solver->max_cuts_per_round * MIP_MIR_PREFILTER_MULT;
    int candidate_count = 0;
    int candidate_rows[MIP_MIR_PREFILTER_MAX_ROWS];
    double candidate_scores[MIP_MIR_PREFILTER_MAX_ROWS];
    int max_aggr = cut_env_int_or_default("RALPH_CMIR_MAX_AGGR", CMIR_MAX_AGGR);
    if (max_aggr < 0) max_aggr = 0;
    if (max_aggr > CMIR_MAX_AGGR) max_aggr = CMIR_MAX_AGGR;
    /* Allocate CMIRWork arrays once */
    CMIRWork work;
    memset(&work, 0, sizeof(work));
    work.num_orig = num_orig;
    work.is_int = solver->is_integer;

    work.a = (double*)calloc(num_orig, sizeof(double));
    work.x_val = (double*)calloc(num_orig, sizeof(double));
    work.lb = (double*)calloc(num_orig, sizeof(double));
    work.ub = (double*)calloc(num_orig, sizeof(double));
    work.a_sub = (double*)calloc(num_orig, sizeof(double));
    work.sub_type = (int*)calloc(num_orig, sizeof(int));
    work.in_C = (int*)calloc(num_orig, sizeof(int));

    int *used_rows = (int*)calloc(m, sizeof(int));

    if (!work.a || !work.x_val || !work.lb || !work.ub ||
        !work.a_sub || !work.sub_type || !work.in_C || !used_rows) {
        goto cleanup;
    }

    /* Snapshot LP solution and bounds for original variables */
    for (int j = 0; j < num_orig; j++) {
        double shift = model_var_shift_amount(row_model, j);
        work.x_val[j] = tab->x[j] + shift;
        work.lb[j] = tab->lb_ext[j] + shift;
        work.ub[j] = tab->ub_ext[j] + shift;
    }

    if (candidate_cap < MIP_MIR_PREFILTER_MIN_ROWS) {
        candidate_cap = MIP_MIR_PREFILTER_MIN_ROWS;
    }
    if (candidate_cap > MIP_MIR_PREFILTER_MAX_ROWS) {
        candidate_cap = MIP_MIR_PREFILTER_MAX_ROWS;
    }
    if (candidate_cap > m) {
        candidate_cap = m;
    }

    /* Rank MIR candidate rows by fractionality */
    for (int k = 0; k < m; k++) {
        if (row_model->con_origin &&
            (k >= row_model->con_origin_capacity || row_model->con_origin[k] < 0)) {
            continue;
        }
        int basic_var = tab->basis[k];
        double val = tab->x[basic_var];
        double frac = val - floor(val);

        solver->root_mir_rows_scanned++;

        /* Skip rows with nearly-integer RHS */
        if (frac < 0.05 || frac > 0.95) continue;

        /* Skip rows where GMI already applies (integer basic variable) */
        if (basic_var < num_orig && solver->is_integer[basic_var]) {
            continue;
        }
        solver->root_mir_rows_candidate++;

        if (candidate_cap <= 0) {
            continue;
        }

        double frac_score = frac;
        if (frac_score > 0.5) frac_score = 1.0 - frac_score;

        int insert_pos = candidate_count;
        while (insert_pos > 0 && candidate_scores[insert_pos - 1] < frac_score) {
            if (insert_pos < candidate_cap) {
                candidate_scores[insert_pos] = candidate_scores[insert_pos - 1];
                candidate_rows[insert_pos] = candidate_rows[insert_pos - 1];
            }
            insert_pos--;
        }
        if (insert_pos >= candidate_cap) {
            continue;
        }

        candidate_scores[insert_pos] = frac_score;
        candidate_rows[insert_pos] = k;
        if (candidate_count < candidate_cap) {
            candidate_count++;
        }
    }
    solver->root_mir_rows_ranked += candidate_count;

    for (int idx = 0; idx < candidate_count; idx++) {
        int k = candidate_rows[idx];

        /* Try increasing aggregation depth */
        int found_cut = 0;
        for (int depth = 0; depth <= max_aggr && !found_cut; depth++) {

            /* Reset source row */
            memset(work.a, 0, num_orig * sizeof(double));
            work.b = 0.0;
            memset(used_rows, 0, m * sizeof(int));

            /* Extract base row */
            if (!cmir_extract_source_row(tab, k, solver->is_integer,
                                         work.a, &work.b, 0, used_rows,
                                         work.x_val, work.lb, work.ub, NULL)) {
                break;  /* Base extraction failed, skip this row */
            }

            /* Apply aggregation steps */
            int aggr_ok = 1;
            int used_shifted_pivot = 0;
            for (int d = 0; d < depth; d++) {
                int step_used_shifted_pivot = 0;
                if (!cmir_extract_source_row(tab, k, solver->is_integer,
                                             work.a, &work.b, 1, used_rows,
                                             work.x_val, work.lb, work.ub,
                                             &step_used_shifted_pivot)) {
                    aggr_ok = 0;
                    break;
                }
                if (step_used_shifted_pivot) {
                    used_shifted_pivot = 1;
                }
            }
            if (!aggr_ok) continue;

            /* Bound substitution */
            cmir_bound_substitute(&work);

            /* Search for best (C, delta) */
            double best_delta = 1.0;
            double best_viol = cmir_separate(&work, &best_delta);

            if (best_viol > RALPH_FEAS_TOL) {
                Cut *cut = cmir_build_cut(&work, best_delta, tab);
                if (cut) {
                    if (used_shifted_pivot) {
                        int integer_terms = 0;
                        int continuous_terms = 0;
                        int all_positive = 1;
                        for (int c = 0; c < cut->nnz; c++) {
                            int var = cut->indices[c];
                            double coef = cut->values[c];
                            if (coef <= RALPH_ZERO_TOL) {
                                all_positive = 0;
                            }
                            if (var >= 0 && var < num_orig &&
                                solver->is_integer && solver->is_integer[var]) {
                                integer_terms++;
                            } else {
                                continuous_terms++;
                            }
                        }
                        if (all_positive && integer_terms == 1 && continuous_terms > 0) {
                            cut_free(cut);
                            cut = NULL;
                        }
                    }
                }
                if (cut) {
                    double built_viol = cut_compute_lp_violation(cut, tab);
                    cut_trace_log("[CMIR-CUT] basic_pos=%d depth=%d viol=%.6f delta=%.6f nnz=%d rhs=%.6f\n",
                                  k, depth, best_viol, best_delta, cut->nnz, cut->rhs);
                    cut_trace_log("[CMIR-CUT-CHECK] basic_pos=%d depth=%d built_viol=%.6f gap=%.6f\n",
                                  k, depth, built_viol, built_viol - best_viol);
                    for (int c = 0; c < cut->nnz; c++) {
                        cut_trace_log("[CMIR-CUT-TERM] basic_pos=%d depth=%d idx=%d coef=%.6f\n",
                                      k, depth, cut->indices[c], cut->values[c]);
                    }
                    cut_pool_add(pool, cut);
                    cuts_added++;
                    found_cut = 1;
                }
            }
        }

        if (cuts_added >= solver->max_cuts_per_round) break;
    }

cleanup:
    free(work.a);
    free(work.x_val);
    free(work.lb);
    free(work.ub);
    free(work.a_sub);
    free(work.sub_type);
    free(work.in_C);
    free(used_rows);

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

        solver->root_cover_rows_scanned++;

        if (!is_knapsack_constraint(model, i, solver->is_integer,
                                    coefs, vars, &num_vars_in_row)) {
            continue;
        }
        solver->root_cover_knapsack_rows++;

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
 * Cut Quality Filters
 * ============================================================================ */

/*
 * Check individual cut quality.
 * Rejects cuts with:
 *   - Coefficient dynamism > MIP_CUT_MAX_DYNAMISM
 *   - Fewer than 2 nonzeros (trivial/degenerate)
 */
static int cut_quality_ok(const Cut *cut) {
    if (!cut || cut->nnz < 2) return 0;

    double max_abs = 0.0;
    double min_abs = RALPH_INFINITY;

    for (int k = 0; k < cut->nnz; k++) {
        double a = fabs(cut->values[k]);
        if (a > RALPH_ZERO_TOL) {
            if (a > max_abs) max_abs = a;
            if (a < min_abs) min_abs = a;
        }
    }

    if (min_abs < RALPH_ZERO_TOL) return 0;
    if (max_abs / min_abs > MIP_CUT_MAX_DYNAMISM) return 0;

    return 1;
}

/*
 * Check if two cuts are nearly parallel using cosine similarity.
 * Two cuts with cosine similarity > MIP_CUT_PARALLEL_TOL are considered parallel.
 */
static int cuts_are_parallel(const Cut *a, const Cut *b) {
    if (!a || !b) return 0;
    if (a->nnz == 0 || b->nnz == 0) return 0;

    /* Compute dot product and norms using merge of sorted index arrays */
    double dot = 0.0, norm_a = 0.0, norm_b = 0.0;
    int ia = 0, ib = 0;

    while (ia < a->nnz && ib < b->nnz) {
        if (a->indices[ia] == b->indices[ib]) {
            dot += a->values[ia] * b->values[ib];
            norm_a += a->values[ia] * a->values[ia];
            norm_b += b->values[ib] * b->values[ib];
            ia++; ib++;
        } else if (a->indices[ia] < b->indices[ib]) {
            norm_a += a->values[ia] * a->values[ia];
            ia++;
        } else {
            norm_b += b->values[ib] * b->values[ib];
            ib++;
        }
    }
    while (ia < a->nnz) {
        norm_a += a->values[ia] * a->values[ia];
        ia++;
    }
    while (ib < b->nnz) {
        norm_b += b->values[ib] * b->values[ib];
        ib++;
    }

    if (norm_a < RALPH_ZERO_TOL || norm_b < RALPH_ZERO_TOL) return 0;

    double cosine = dot / sqrt(norm_a * norm_b);
    return fabs(cosine) > MIP_CUT_PARALLEL_TOL;
}

/* ============================================================================
 * Cut Application
 * ============================================================================ */

/* Add cuts to the LP relaxation */
int apply_cuts(MIPSolver *solver, CutPool *pool, int max_cuts, Cut ***applied_out) {
    if (applied_out) *applied_out = NULL;
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

    /* Track applied cuts for parallel filtering */
    int applied_cap = max_cuts < pool->count ? max_cuts : pool->count;
    Cut **applied = (Cut **)calloc(applied_cap, sizeof(Cut *));
    int cuts_applied = 0;

    for (int i = 0; i < pool->count && cuts_applied < max_cuts; i++) {
        Cut *cut = pool->cuts[i];

        /* Skip if not violated enough */
        if (cut->violation < MIP_CUT_MIN_VIOLATION) continue;

        /* Skip if poor coefficient quality */
        if (!cut_quality_ok(cut)) continue;

        /* Skip if parallel to an already-applied cut */
        if (applied) {
            int is_parallel = 0;
            for (int a = 0; a < cuts_applied; a++) {
                if (cuts_are_parallel(cut, applied[a])) {
                    is_parallel = 1;
                    break;
                }
            }
            if (is_parallel) continue;
        }

        /* Add cut as new constraint and mark it as generated, not structural. */
        int row = lp_model_add_constraint(model, cut->nnz, cut->indices, cut->values,
                                          cut->sense, cut->rhs);
        if (row < 0) {
            continue;
        }
        if (model->con_origin && row < model->con_origin_capacity) {
            model->con_origin[row] = -1;
        }
        if (applied) {
            applied[cuts_applied] = cut;
        }
        cuts_applied++;
    }

    if (applied_out && cuts_applied > 0) {
        Cut **exact = (Cut **)calloc((size_t)cuts_applied, sizeof(Cut *));
        if (exact) {
            memcpy(exact, applied, (size_t)cuts_applied * sizeof(Cut *));
            *applied_out = exact;
        }
    }

    free(applied);

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
