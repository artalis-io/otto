/*
 * simplex_ratio.c - Ratio test (leaving variable selection) for the simplex method.
 *
 * Extracted from simplex.c - contains Bland, standard, and Harris ratio tests.
 */

#include <math.h>
#include "lp.h"
#include "simplex_ratio.h"

static double entering_bound_flip_distance(const SimplexTableau *tab, int entering) {
    double dist;
    if (tab->var_status[entering] == RALPH_NONBASIC_UPPER) {
        if (tab->lb_ext[entering] <= -RALPH_INFINITY / 2.0) {
            return RALPH_INFINITY;
        }
        dist = tab->x[entering] - tab->lb_ext[entering];
    } else {
        if (tab->ub_ext[entering] >= RALPH_INFINITY / 2.0) {
            return RALPH_INFINITY;
        }
        dist = tab->ub_ext[entering] - tab->x[entering];
    }
    return dist > 0.0 ? dist : 0.0;
}

/* Bland's ratio test: among ties, choose smallest index leaving variable */
int ratio_test_bland(SimplexTableau *tab, int entering, int *leaving, double *theta) {
    /* Compute entering column in basis representation */
    int col_nnz;
    const int *col_idx;
    const double *col_val;
    sparse_get_column_sparse(tab->A_ext, entering, &col_nnz, &col_idx, &col_val);

    /* Use hyper-sparse FTRAN for better performance on sparse columns */
    {
        double t_ftran_ms = lp_telemetry_timer_start();
        lu_ftran_hyper_sparse(tab->lu, col_nnz, col_idx, col_val, tab->work2, NULL, NULL);
        if (tab->owner) {
            lp_telemetry_add_ftran_timed(tab->owner, t_ftran_ms);
        }
    }

    /* Zero FTRAN values for stuck artificial positions.
     * LU regularization (diagonal=1.0) produces meaningless values for
     * redundant rows. Zeroing prevents them from affecting the ratio test
     * and the solution update in simplex_pivot. */
    if (tab->num_redundant > 0) {
        for (int a = 0; a < tab->num_artificial; a++) {
            int art_j = tab->artificial_vars[a];
            if (tab->var_status[art_j] == RALPH_BASIC) {
                int pos = tab->basis_pos[art_j];
                if (pos >= 0 && pos < tab->m) {
                    tab->work2[pos] = 0.0;
                }
            }
        }
    }

    double dir = 1.0;
    if (tab->var_status[entering] == RALPH_NONBASIC_UPPER) {
        dir = -1.0;
    }

    /* Phase 1 is the bound-feasibility gate.  On ill-scaled Phase-1 bases, a
     * component that is small relative to the largest FTRAN entry can still be
     * the active row protecting a basic variable from crossing its bound.  In
     * Phase 2, keep the relative filter to avoid numerically fragile pivots. */
    int ftran_nnz = 0;
    double pivot_tol = RALPH_PIVOT_TOL;
    if (tab->phase != 1) {
        double max_abs_dk = 0.0;
        for (int k = 0; k < tab->m; k++) {
            double abs_dk = fabs(tab->work2[k] * dir);
            if (abs_dk > RALPH_ZERO_TOL) ftran_nnz++;
            if (abs_dk > max_abs_dk) {
                max_abs_dk = abs_dk;
            }
        }
        pivot_tol = fmax(RALPH_PIVOT_TOL, 1e-7 * max_abs_dk);
        if (tab->owner) {
            lp_telemetry_record_ftran_nnz(tab->owner, col_nnz, ftran_nnz);
        }
    }

    *leaving = -1;
    *theta = RALPH_INFINITY;
    int leaving_var = tab->n;  /* Track actual variable index for Bland's tie-breaking */

    for (int k = 0; k < tab->m; k++) {
        double dk = tab->work2[k] * dir;
        double abs_dk = fabs(dk);
        int j = tab->basis[k];
        double xj = tab->x[j];

        if (tab->phase == 1 && abs_dk > RALPH_ZERO_TOL) ftran_nnz++;
        double ratio = RALPH_INFINITY;
        if (dk > pivot_tol) {
            ratio = (xj - tab->lb_ext[j]) / dk;
        } else if (dk < -pivot_tol) {
            ratio = (tab->ub_ext[j] - xj) / (-dk);
        }

        if (ratio < *theta - RALPH_FEAS_TOL) {
            *theta = ratio;
            *leaving = k;
            leaving_var = j;
        } else if (fabs(ratio - *theta) <= RALPH_FEAS_TOL && j < leaving_var) {
            /* Bland's rule: among ties, choose smallest variable index */
            *leaving = k;
            leaving_var = j;
        }
    }
    if (tab->phase == 1 && tab->owner) {
        lp_telemetry_record_ftran_nnz(tab->owner, col_nnz, ftran_nnz);
    }

    /* Check bound flip */
    double enter_range = entering_bound_flip_distance(tab, entering);
    if (enter_range < *theta && enter_range < RALPH_INFINITY/2) {
        *theta = enter_range;
        *leaving = -2;
    }

    if (*theta >= RALPH_INFINITY/2) {
        return -1;  /* Unbounded */
    }

    return (*leaving >= 0 || *leaving == -2) ? 0 : -1;
}

/* Standard (non-Harris) ratio test:
 * - strict minimum ratio selection
 * - no Harris tolerance expansion
 */
int ratio_test_standard(SimplexTableau *tab, int entering, int *leaving, double *theta) {
    int col_nnz;
    const int *col_idx;
    const double *col_val;
    sparse_get_column_sparse(tab->A_ext, entering, &col_nnz, &col_idx, &col_val);

    {
        double t_ftran_ms = lp_telemetry_timer_start();
        lu_ftran_hyper_sparse(tab->lu, col_nnz, col_idx, col_val, tab->work2, NULL, NULL);
        if (tab->owner) {
            lp_telemetry_add_ftran_timed(tab->owner, t_ftran_ms);
        }
    }

    if (tab->num_redundant > 0) {
        for (int a = 0; a < tab->num_artificial; a++) {
            int art_j = tab->artificial_vars[a];
            if (tab->var_status[art_j] == RALPH_BASIC) {
                int pos = tab->basis_pos[art_j];
                if (pos >= 0 && pos < tab->m) {
                    tab->work2[pos] = 0.0;
                }
            }
        }
    }

    double dir = 1.0;
    if (tab->var_status[entering] == RALPH_NONBASIC_UPPER) {
        dir = -1.0;
    }

    /* See ratio_test_bland: Phase 1 uses the absolute floor for bound
     * protection; Phase 2 keeps the relative numerical filter. */
    int ftran_nnz = 0;
    double pivot_tol = RALPH_PIVOT_TOL;
    if (tab->phase != 1) {
        double max_abs_dk = 0.0;
        for (int k = 0; k < tab->m; k++) {
            double abs_dk = fabs(tab->work2[k] * dir);
            if (abs_dk > RALPH_ZERO_TOL) ftran_nnz++;
            if (abs_dk > max_abs_dk) {
                max_abs_dk = abs_dk;
            }
        }
        pivot_tol = fmax(RALPH_PIVOT_TOL, 1e-7 * max_abs_dk);
        if (tab->owner) {
            lp_telemetry_record_ftran_nnz(tab->owner, col_nnz, ftran_nnz);
        }
    }

    *leaving = -1;
    *theta = RALPH_INFINITY;
    for (int k = 0; k < tab->m; k++) {
        double dk = tab->work2[k] * dir;
        double abs_dk = fabs(dk);
        int j = tab->basis[k];
        double xj = tab->x[j];

        if (tab->phase == 1 && abs_dk > RALPH_ZERO_TOL) ftran_nnz++;
        double ratio = RALPH_INFINITY;
        if (dk > pivot_tol) {
            ratio = (xj - tab->lb_ext[j]) / dk;
        } else if (dk < -pivot_tol) {
            ratio = (tab->ub_ext[j] - xj) / (-dk);
        }

        if (ratio < *theta - RALPH_FEAS_TOL) {
            *theta = ratio;
            *leaving = k;
        }
    }
    if (tab->phase == 1 && tab->owner) {
        lp_telemetry_record_ftran_nnz(tab->owner, col_nnz, ftran_nnz);
    }

    {
        double enter_range = entering_bound_flip_distance(tab, entering);
        if (enter_range <= *theta && enter_range < RALPH_INFINITY / 2) {
            *theta = enter_range;
            *leaving = -2;
        }
    }

    if (*theta >= RALPH_INFINITY / 2) {
        return -1;
    }
    return (*leaving >= 0 || *leaving == -2) ? 0 : -1;
}

int primal_ratio_test_with_policy(const SimplexSolver *solver,
                                  SimplexTableau *tab,
                                  int use_bland,
                                  int entering,
                                  int *leaving,
                                  double *theta) {
    if (use_bland) {
        return ratio_test_bland(tab, entering, leaving, theta);
    }
    if (solver && solver->ratio_test_mode == LP_RATIO_TEST_STANDARD) {
        return ratio_test_standard(tab, entering, leaving, theta);
    }
    return ratio_test_harris(tab, entering, leaving, theta);
}

int ratio_test_harris(SimplexTableau *tab, int entering, int *leaving, double *theta) {
    /* Compute entering column in basis representation */
    int col_nnz;
    const int *col_idx;
    const double *col_val;
    sparse_get_column_sparse(tab->A_ext, entering, &col_nnz, &col_idx, &col_val);

    /* Use hyper-sparse FTRAN for better performance on sparse columns */
    {
        double t_ftran_ms = lp_telemetry_timer_start();
        lu_ftran_hyper_sparse(tab->lu, col_nnz, col_idx, col_val, tab->work2, NULL, NULL);
        if (tab->owner) {
            lp_telemetry_add_ftran_timed(tab->owner, t_ftran_ms);
        }
    }

    /* Zero FTRAN values for stuck artificial positions (see ratio_test_bland). */
    if (tab->num_redundant > 0) {
        for (int a = 0; a < tab->num_artificial; a++) {
            int art_j = tab->artificial_vars[a];
            if (tab->var_status[art_j] == RALPH_BASIC) {
                int pos = tab->basis_pos[art_j];
                if (pos >= 0 && pos < tab->m) {
                    tab->work2[pos] = 0.0;
                }
            }
        }
    }

    double dir = 1.0;  /* Direction of movement */
    if (tab->var_status[entering] == RALPH_NONBASIC_UPPER) {
        dir = -1.0;
    }

    const int *basis = tab->basis;
    const double *x = tab->x;
    const double *lb = tab->lb_ext;
    const double *ub = tab->ub_ext;
    const double *work2 = tab->work2;

    /* See ratio_test_bland: Phase 1 uses the absolute floor for bound
     * protection; Phase 2 keeps the relative numerical filter. */
    int ftran_nnz = 0;
    double pivot_tol = RALPH_PIVOT_TOL;
    if (tab->phase != 1) {
        double max_abs_dk = 0.0;
        for (int k = 0; k < tab->m; k++) {
            double dk = (dir > 0.0) ? work2[k] : -work2[k];
            double abs_dk = fabs(dk);
            if (abs_dk > RALPH_ZERO_TOL) ftran_nnz++;
            if (abs_dk > max_abs_dk) {
                max_abs_dk = abs_dk;
            }
        }
        pivot_tol = fmax(RALPH_PIVOT_TOL, 1e-7 * max_abs_dk);
        if (tab->owner) {
            lp_telemetry_record_ftran_nnz(tab->owner, col_nnz, ftran_nnz);
        }
    }

    /* Single-pass Harris ratio test (merged from two passes)
     *
     * Harris ratio test allows small infeasibility (FEAS_TOL) when computing
     * theta_max, then selects among candidates within that tolerance.
     *
     * Tie-breaking strategy:
     * 1. Prefer non-degenerate pivots (ratio > tolerance)
     * 2. Among degenerate ties, prefer larger pivot for numerical stability
     */
    double theta_max = RALPH_INFINITY;
    double best_pivot = 0.0;
    int best_is_degen = 1;
    *leaving = -1;
    *theta = RALPH_INFINITY;

    /* Check bound on entering variable first (contributes to theta_max) */
    double enter_range = entering_bound_flip_distance(tab, entering);
    if (enter_range < RALPH_INFINITY/2) {
        theta_max = enter_range;
    }

    /* Single pass: compute theta_max and select best leaving simultaneously */
    for (int k = 0; k < tab->m; k++) {
        double dk = (dir > 0.0) ? work2[k] : -work2[k];
        double abs_dk = fabs(dk);
        if (tab->phase == 1 && abs_dk > RALPH_ZERO_TOL) ftran_nnz++;
        if (abs_dk < pivot_tol) continue;  /* Skip tiny pivots */

        int j = basis[k];
        double xj = x[j];

        double ratio_harris;  /* Ratio with Harris tolerance */
        double ratio_exact;   /* Exact ratio for selection */

        if (dk > 0) {
            /* Variable will decrease toward lower bound */
            double slack = xj - lb[j];
            ratio_harris = (slack + RALPH_FEAS_TOL) / dk;
            ratio_exact = slack / dk;
        } else {
            /* Variable will increase toward upper bound (dk < 0) */
            double slack = ub[j] - xj;
            ratio_harris = (slack + RALPH_FEAS_TOL) / (-dk);
            ratio_exact = slack / (-dk);
        }

        /* Update theta_max */
        if (ratio_harris < theta_max) {
            theta_max = ratio_harris;
        }

        /* Check if this is a valid candidate (within current theta_max + tolerance) */
        if (ratio_exact <= theta_max + RALPH_FEAS_TOL) {
            int is_degen = (ratio_exact < 1e-8);
            double pivot_size = fabs(dk);

            /* Selection criteria */
            int select = 0;
            if (*leaving < 0) {
                select = 1;  /* First candidate */
            } else if (!is_degen && best_is_degen) {
                select = 1;  /* Prefer non-degenerate */
            } else if (is_degen == best_is_degen && pivot_size > best_pivot * 1.1) {
                select = 1;  /* Significantly larger pivot */
            }

            if (select) {
                best_pivot = pivot_size;
                best_is_degen = is_degen;
                *leaving = k;
                *theta = ratio_exact > 0 ? ratio_exact : 0;
            }
        }
    }
    if (tab->phase == 1 && tab->owner) {
        lp_telemetry_record_ftran_nnz(tab->owner, col_nnz, ftran_nnz);
    }

    /* After the pass, invalidate selection if it's no longer within theta_max
     * (theta_max may have decreased after we selected the candidate) */
    if (*leaving >= 0 && *theta > theta_max + RALPH_FEAS_TOL) {
        /* Re-scan for valid candidates - this is rare */
        best_pivot = 0.0;
        best_is_degen = 1;
        *leaving = -1;
        *theta = RALPH_INFINITY;

        for (int k = 0; k < tab->m; k++) {
            double dk = (dir > 0.0) ? work2[k] : -work2[k];
            if (fabs(dk) < pivot_tol) continue;

            int j = basis[k];
            double xj = x[j];
            double ratio_exact;

            if (dk > 0) {
                ratio_exact = (xj - lb[j]) / dk;
            } else {
                ratio_exact = (ub[j] - xj) / (-dk);
            }

            if (ratio_exact <= theta_max + RALPH_FEAS_TOL) {
                int is_degen = (ratio_exact < 1e-8);
                double pivot_size = fabs(dk);

                int select = 0;
                if (*leaving < 0) {
                    select = 1;
                } else if (!is_degen && best_is_degen) {
                    select = 1;
                } else if (is_degen == best_is_degen && pivot_size > best_pivot * 1.1) {
                    select = 1;
                }

                if (select) {
                    best_pivot = pivot_size;
                    best_is_degen = is_degen;
                    *leaving = k;
                    *theta = ratio_exact > 0 ? ratio_exact : 0;
                }
            }
        }
    }

    /* Check for unbounded */
    if (theta_max >= RALPH_INFINITY/2) {
        *theta = RALPH_INFINITY;
        return -1;  /* Unbounded */
    }

    /* Check if entering variable hits its bound (bound flip) */
    if (enter_range <= theta_max && enter_range < RALPH_INFINITY/2) {
        if (*leaving < 0 || enter_range < *theta) {
            *theta = enter_range;
            *leaving = -2;  /* Special flag for bound flip */
        }
    }

    return (*leaving >= 0 || *leaving == -2) ? 0 : -1;
}

/* Fallback ratio test for already-computed direction (tab->work2) while
 * excluding a specific leaving position. Used to avoid repeated failing pivots.
 */
int ratio_test_harris_excluding_current(SimplexTableau *tab, int entering,
                                        int exclude_pos, int *leaving, double *theta) {
    if (!tab || !leaving || !theta) return -1;

    double dir = 1.0;
    if (tab->var_status[entering] == RALPH_NONBASIC_UPPER) {
        dir = -1.0;
    }

    double max_abs_dk = 0.0;
    for (int k = 0; k < tab->m; k++) {
        if (k == exclude_pos) continue;
        double abs_dk = fabs(tab->work2[k] * dir);
        if (abs_dk > max_abs_dk) {
            max_abs_dk = abs_dk;
        }
    }
    double pivot_tol = (tab->phase == 1)
        ? RALPH_PIVOT_TOL
        : fmax(RALPH_PIVOT_TOL, 1e-7 * max_abs_dk);

    double theta_max = RALPH_INFINITY;
    double best_pivot = 0.0;
    int best_is_degen = 1;
    *leaving = -1;
    *theta = RALPH_INFINITY;

    double enter_range = entering_bound_flip_distance(tab, entering);
    if (enter_range < RALPH_INFINITY / 2) {
        theta_max = enter_range;
    }

    for (int k = 0; k < tab->m; k++) {
        if (k == exclude_pos) continue;

        double dk = tab->work2[k] * dir;
        if (fabs(dk) < pivot_tol) continue;

        int j = tab->basis[k];
        double xj = tab->x[j];

        double ratio_harris;
        double ratio_exact;
        if (dk > 0) {
            double slack = xj - tab->lb_ext[j];
            ratio_harris = (slack + RALPH_FEAS_TOL) / dk;
            ratio_exact = slack / dk;
        } else {
            double slack = tab->ub_ext[j] - xj;
            ratio_harris = (slack + RALPH_FEAS_TOL) / (-dk);
            ratio_exact = slack / (-dk);
        }

        if (ratio_harris < theta_max) {
            theta_max = ratio_harris;
        }

        if (ratio_exact <= theta_max + RALPH_FEAS_TOL) {
            int is_degen = (ratio_exact < 1e-8);
            double pivot_size = fabs(dk);

            int select = 0;
            if (*leaving < 0) {
                select = 1;
            } else if (!is_degen && best_is_degen) {
                select = 1;
            } else if (is_degen == best_is_degen && pivot_size > best_pivot * 1.1) {
                select = 1;
            }

            if (select) {
                best_pivot = pivot_size;
                best_is_degen = is_degen;
                *leaving = k;
                *theta = ratio_exact > 0 ? ratio_exact : 0;
            }
        }
    }

    if (theta_max >= RALPH_INFINITY / 2) {
        return -1;
    }

    if (enter_range <= theta_max && enter_range < RALPH_INFINITY / 2) {
        if (*leaving < 0 || enter_range < *theta) {
            *theta = enter_range;
            *leaving = -2;
        }
    }

    return (*leaving >= 0 || *leaving == -2) ? 0 : -1;
}
