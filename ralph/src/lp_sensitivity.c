/*
 * Ralph - Fixed-basis LP sensitivity/ranging helpers
 *
 * This module computes local fixed-basis ranges in internal minimization space.
 * Public API wrappers in ralph.c map these ranges to user-space objective/RHS.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lp.h"

#define LP_SENS_INF RALPH_INFINITY
#define LP_SENS_ZERO_TOL RALPH_ZERO_TOL
#define LP_SENS_FEAS_TOL (10.0 * RALPH_FEAS_TOL)
#define LP_SENS_RC_TOL (10.0 * RALPH_OPT_TOL)

static int lp_sens_lb_finite(double lb) {
    return lb > -RALPH_INFINITY * 0.5;
}

static int lp_sens_ub_finite(double ub) {
    return ub < RALPH_INFINITY * 0.5;
}

static double lp_sens_clip_inf(double x) {
    if (!isfinite(x)) return (x < 0.0) ? -LP_SENS_INF : LP_SENS_INF;
    if (x > LP_SENS_INF) return LP_SENS_INF;
    if (x < -LP_SENS_INF) return -LP_SENS_INF;
    return x;
}

static int lp_sens_intersect(double *lo, double *hi, double a, double b) {
    double left = a;
    double right = b;
    if (!lo || !hi) return -1;

    if (left > right) {
        double tmp = left;
        left = right;
        right = tmp;
    }
    if (left > *lo) *lo = left;
    if (right < *hi) *hi = right;
    if (*lo > *hi + LP_SENS_FEAS_TOL) return -1;
    return 0;
}

/* Intersect delta interval with x + delta * coeff in [lb, ub]. */
static int lp_sens_apply_box_constraint(double x,
                                        double coeff,
                                        double lb,
                                        double ub,
                                        double *delta_lo,
                                        double *delta_hi) {
    if (!delta_lo || !delta_hi) return -1;

    if (fabs(coeff) <= LP_SENS_ZERO_TOL) {
        if (lp_sens_lb_finite(lb) && x < lb - LP_SENS_FEAS_TOL) return -1;
        if (lp_sens_ub_finite(ub) && x > ub + LP_SENS_FEAS_TOL) return -1;
        return 0;
    }

    if (lp_sens_lb_finite(lb)) {
        double t = (lb - x) / coeff;
        if (coeff > 0.0) {
            if (t > *delta_lo) *delta_lo = t;
        } else {
            if (t < *delta_hi) *delta_hi = t;
        }
    }
    if (lp_sens_ub_finite(ub)) {
        double t = (ub - x) / coeff;
        if (coeff > 0.0) {
            if (t < *delta_hi) *delta_hi = t;
        } else {
            if (t > *delta_lo) *delta_lo = t;
        }
    }
    if (*delta_lo > *delta_hi + LP_SENS_FEAS_TOL) return -1;
    return 0;
}

/* Apply one reduced-cost sign/equality condition:
 *   LOWER: rc - delta*alpha >= 0
 *   UPPER: rc - delta*alpha <= 0
 *   FREE : rc - delta*alpha == 0
 */
static int lp_sens_apply_rc_constraint(VarStatus st,
                                       double rc,
                                       double alpha,
                                       double *delta_lo,
                                       double *delta_hi) {
    if (!delta_lo || !delta_hi) return -1;
    if (st == RALPH_FIXED || st == RALPH_BASIC) return 0;

    if (st == RALPH_NONBASIC_LOWER) {
        /* delta*alpha <= rc */
        if (fabs(alpha) <= LP_SENS_ZERO_TOL) {
            return (rc >= -LP_SENS_RC_TOL) ? 0 : -1;
        }
        if (alpha > 0.0) {
            double hi = rc / alpha;
            if (hi < *delta_hi) *delta_hi = hi;
        } else {
            double lo = rc / alpha;
            if (lo > *delta_lo) *delta_lo = lo;
        }
    } else if (st == RALPH_NONBASIC_UPPER) {
        /* delta*alpha >= rc */
        if (fabs(alpha) <= LP_SENS_ZERO_TOL) {
            return (rc <= LP_SENS_RC_TOL) ? 0 : -1;
        }
        if (alpha > 0.0) {
            double lo = rc / alpha;
            if (lo > *delta_lo) *delta_lo = lo;
        } else {
            double hi = rc / alpha;
            if (hi < *delta_hi) *delta_hi = hi;
        }
    } else if (st == RALPH_NONBASIC_FREE) {
        /* delta*alpha == rc */
        if (fabs(alpha) <= LP_SENS_ZERO_TOL) {
            return (fabs(rc) <= LP_SENS_RC_TOL) ? 0 : -1;
        }
        {
            double val = rc / alpha;
            if (val > *delta_lo) *delta_lo = val;
            if (val < *delta_hi) *delta_hi = val;
        }
    } else {
        return -1;
    }

    if (*delta_lo > *delta_hi + LP_SENS_FEAS_TOL) return -1;
    return 0;
}

static int lp_sens_check_ready(const SimplexTableau *tab) {
    if (!tab || !tab->lu || !tab->A_ext || !tab->basis || !tab->basis_pos ||
        !tab->var_status || !tab->x || !tab->rc || !tab->rhs ||
        !tab->lb_ext || !tab->ub_ext) {
        return -1;
    }
    if (tab->phase != 2) return -1;
    if (tab->m <= 0 || tab->n <= 0) return -1;
    return 0;
}

static int lp_sens_active_bound_delta_interval(const SimplexTableau *tab,
                                               int var,
                                               int active_is_lower,
                                               double *delta_lo,
                                               double *delta_hi) {
    int m;
    double *dir = NULL;
    int nnz = 0;
    const int *col_idx = NULL;
    const double *col_val = NULL;
    double lo = -LP_SENS_INF;
    double hi = LP_SENS_INF;

    if (!tab || !delta_lo || !delta_hi) return -1;
    m = tab->m;
    if (var < 0 || var >= tab->n) return -1;

    dir = (double*)calloc((size_t)m, sizeof(double));
    if (!dir) return -1;

    sparse_get_column_sparse(tab->A_ext, var, &nnz, &col_idx, &col_val);
    lu_solve_sparse(tab->lu, nnz, col_idx, col_val, dir);

    for (int k = 0; k < m; k++) {
        int bvar = tab->basis[k];
        double coeff = -dir[k];
        if (bvar < 0 || bvar >= tab->n) {
            free(dir);
            return -1;
        }
        if (lp_sens_apply_box_constraint(tab->x[bvar], coeff,
                                         tab->lb_ext[bvar], tab->ub_ext[bvar],
                                         &lo, &hi) != 0) {
            free(dir);
            return -1;
        }
    }

    if (active_is_lower) {
        double cur_lb = tab->lb_ext[var];
        double ub = tab->ub_ext[var];
        if (lp_sens_ub_finite(ub) &&
            lp_sens_intersect(&lo, &hi, -LP_SENS_INF, ub - cur_lb) != 0) {
            free(dir);
            return -1;
        }
    } else {
        double cur_ub = tab->ub_ext[var];
        double lb = tab->lb_ext[var];
        if (lp_sens_lb_finite(lb) &&
            lp_sens_intersect(&lo, &hi, lb - cur_ub, LP_SENS_INF) != 0) {
            free(dir);
            return -1;
        }
    }

    free(dir);
    *delta_lo = lo;
    *delta_hi = hi;
    return 0;
}

int lp_sensitivity_rhs_range_internal(const SimplexTableau *tab,
                                      int row,
                                      double *rhs_min,
                                      double *rhs_max) {
    int m;
    double *rhs_vec = NULL;
    double *dir = NULL;
    double lo = -LP_SENS_INF;
    double hi = LP_SENS_INF;

    if (lp_sens_check_ready(tab) != 0) return -1;
    m = tab->m;
    if (row < 0 || row >= m) return -1;
    if (!rhs_min || !rhs_max) return -1;

    rhs_vec = (double*)calloc((size_t)m, sizeof(double));
    dir = (double*)calloc((size_t)m, sizeof(double));
    if (!rhs_vec || !dir) {
        free(rhs_vec);
        free(dir);
        return -1;
    }

    rhs_vec[row] = 1.0;
    lu_solve(tab->lu, rhs_vec, dir);  /* dir = B^{-1} e_row */

    for (int k = 0; k < m; k++) {
        int bvar = tab->basis[k];
        if (bvar < 0 || bvar >= tab->n) {
            free(rhs_vec);
            free(dir);
            return -1;
        }
        if (lp_sens_apply_box_constraint(tab->x[bvar], dir[k],
                                         tab->lb_ext[bvar], tab->ub_ext[bvar],
                                         &lo, &hi) != 0) {
            free(rhs_vec);
            free(dir);
            return -1;
        }
    }

    free(rhs_vec);
    free(dir);

    *rhs_min = lp_sens_clip_inf(tab->rhs[row] + lo);
    *rhs_max = lp_sens_clip_inf(tab->rhs[row] + hi);
    if (*rhs_min > *rhs_max + LP_SENS_FEAS_TOL) return -1;
    return 0;
}

int lp_sensitivity_obj_coef_range_internal(const SimplexTableau *tab,
                                           int var,
                                           double *coef_min,
                                           double *coef_max) {
    double lo = -LP_SENS_INF;
    double hi = LP_SENS_INF;

    if (lp_sens_check_ready(tab) != 0) return -1;
    if (var < 0 || var >= tab->n) return -1;
    if (!coef_min || !coef_max) return -1;

    if (tab->basis_pos[var] < 0) {
        VarStatus st = tab->var_status[var];
        double rc = tab->rc[var];
        if (st == RALPH_NONBASIC_LOWER) {
            if (-rc > lo) lo = -rc;
        } else if (st == RALPH_NONBASIC_UPPER) {
            if (-rc < hi) hi = -rc;
        } else if (st == RALPH_NONBASIC_FREE) {
            if (-rc > lo) lo = -rc;
            if (-rc < hi) hi = -rc;
        } else if (st != RALPH_FIXED) {
            return -1;
        }
    } else {
        int m = tab->m;
        int k = tab->basis_pos[var];
        double *rhs = NULL;
        double *w = NULL;

        if (k < 0 || k >= m) return -1;
        rhs = (double*)calloc((size_t)m, sizeof(double));
        w = (double*)calloc((size_t)m, sizeof(double));
        if (!rhs || !w) {
            free(rhs);
            free(w);
            return -1;
        }

        rhs[k] = 1.0;
        lu_solve_transpose(tab->lu, rhs, w);  /* w = B^{-T} e_k */

        for (int q = 0; q < tab->n; q++) {
            if (tab->basis_pos[q] >= 0) continue;  /* only nonbasic constraints */
            if (q == var) continue;

            if (lp_sens_apply_rc_constraint(tab->var_status[q],
                                            tab->rc[q],
                                            sparse_dot_column(tab->A_ext, q, w),
                                            &lo, &hi) != 0) {
                free(rhs);
                free(w);
                return -1;
            }
        }

        free(rhs);
        free(w);
    }

    if (lo > hi + LP_SENS_FEAS_TOL) return -1;
    *coef_min = lp_sens_clip_inf(tab->c_ext[var] + lo);
    *coef_max = lp_sens_clip_inf(tab->c_ext[var] + hi);
    if (*coef_min > *coef_max + LP_SENS_FEAS_TOL) return -1;
    return 0;
}

int lp_sensitivity_var_bound_range_internal(const SimplexTableau *tab,
                                            int var,
                                            LPBoundRangeInternal *range) {
    VarStatus st;
    double x;
    double lb;
    double ub;

    if (lp_sens_check_ready(tab) != 0) return -1;
    if (!range) return -1;
    if (var < 0 || var >= tab->n) return -1;

    memset(range, 0, sizeof(*range));
    st = tab->var_status[var];
    x = tab->x[var];
    lb = tab->lb_ext[var];
    ub = tab->ub_ext[var];

    if (st == RALPH_BASIC) {
        range->lower_min = -LP_SENS_INF;
        range->lower_max = lp_sens_clip_inf(x);
        range->upper_min = lp_sens_clip_inf(x);
        range->upper_max = LP_SENS_INF;
        return 0;
    }

    if (st == RALPH_NONBASIC_LOWER) {
        double dlo = 0.0, dhi = 0.0;
        if (!lp_sens_lb_finite(lb)) return -1;
        if (lp_sens_active_bound_delta_interval(tab, var, 1, &dlo, &dhi) != 0) return -1;
        range->lower_min = lp_sens_clip_inf(lb + dlo);
        range->lower_max = lp_sens_clip_inf(lb + dhi);
        range->upper_min = lp_sens_clip_inf(lb);
        range->upper_max = LP_SENS_INF;
        return 0;
    }

    if (st == RALPH_NONBASIC_UPPER) {
        double dlo = 0.0, dhi = 0.0;
        if (!lp_sens_ub_finite(ub)) return -1;
        if (lp_sens_active_bound_delta_interval(tab, var, 0, &dlo, &dhi) != 0) return -1;
        range->lower_min = -LP_SENS_INF;
        range->lower_max = lp_sens_clip_inf(ub);
        range->upper_min = lp_sens_clip_inf(ub + dlo);
        range->upper_max = lp_sens_clip_inf(ub + dhi);
        return 0;
    }

    if (st == RALPH_NONBASIC_FREE) {
        range->lower_min = -LP_SENS_INF;
        range->lower_max = lp_sens_clip_inf(x);
        range->upper_min = lp_sens_clip_inf(x);
        range->upper_max = LP_SENS_INF;
        return 0;
    }

    if (st == RALPH_FIXED) {
        range->lower_min = lp_sens_clip_inf(lb);
        range->lower_max = lp_sens_clip_inf(lb);
        range->upper_min = lp_sens_clip_inf(ub);
        range->upper_max = lp_sens_clip_inf(ub);
        return 0;
    }

    return -1;
}
