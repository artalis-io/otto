/*
 * Ralph - LU Factorization Implementation
 *
 * Implements LU factorization with partial pivoting for the basis matrix
 * in the revised simplex method. Supports both initial factorization
 * and efficient updates via eta-file method.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <limits.h>
#include "lp.h"
#include "lp_bfcp_policy.h"
#include "lp_glpk_strict.h"
#include "lu_update_backend.h"
#include "lu_supernode.h"

#ifdef _OPENMP
#include <omp.h>
#endif

/* Refactorize when spike pool exceeds this percentage of capacity */
#define RALPH_SPIKE_POOL_WARN_PCT 85
/* Reject extremely dense FT updates on large bases; they poison sparse solve cost. */
#define RALPH_SPIKE_DENSE_REJECT_M_MIN 300
#define RALPH_SPIKE_DENSE_REJECT_MIN_UPDATES 8
#define RALPH_SPIKE_DENSE_BASE_RATIO 0.70
#define RALPH_SPIKE_DENSE_AGED_RATIO 0.55
/* Early reinversion when average stored spike density drifts too high. */
#define RALPH_SPIKE_AVG_REFACTOR_RATIO 0.45
#define RALPH_SPIKE_AVG_REFACTOR_AGED_RATIO 0.35

/* Forward declarations for reach computation (used by sparse solves) */
static void compute_reach_L(const LUFactorization *lu,
                            int nnz_rhs, const int *rhs_idx,
                            int *reach_out, int *reach_nnz,
                            int *marked);
static void compute_reach_U(const LUFactorization *lu,
                            int nnz_rhs, const int *rhs_idx,
                            int *reach_out, int *reach_nnz,
                            int *marked);
/* W1: Forward declaration for sparse BTRAN support */
static void build_csr_transpose(LUFactorization *lu);

static int lu_default_max_updates_for_m(int m) {
    return (m < 100) ? 50 : (m < 500) ? m / 2 : (m < 1000) ? 100 : 120;
}

static int lu_cgr_max_updates_for_base(int base_updates) {
    int cgr_updates;

    if (base_updates <= 0) return 0;
    cgr_updates = (base_updates * 5) / 4;
    if (cgr_updates > 256) cgr_updates = 256;
    if (cgr_updates < 32) cgr_updates = 32;
    return cgr_updates;
}

static int lu_glpk_strict_mode(const LUFactorization *lu) {
    if (!lu || !lu->owner) return 0;
    return lp_glpk_strict_mode_enabled(lu->owner->glpk_strict_mode);
}

static int lu_strict_lane_active(const LUFactorization *lu) {
    if (!lu || !lu->owner) return 0;
    return lu->owner->lu_strict_lane_active ? 1 : 0;
}

static int lu_strict_allow_top_level_dense_fallback(const LUFactorization *lu) {
    if (!lu || !lu->owner) return 1;
    return lu->owner->lu_strict_allow_top_level_dense_fallback ? 1 : 0;
}

static void lu_clamp_max_updates_to_storage(LUFactorization *lu) {
    int cap;

    if (!lu) return;
    cap = lu_update_backend_storage_capacity(lu);
    if (cap > 0 && lu->max_updates > cap) {
        lu->max_updates = cap;
    }
}

static int lu_normalize_backend_policy(int backend_policy) {
    if (backend_policy == LP_LU_BACKEND_POLICY_AUTO) {
        return LP_LU_BACKEND_POLICY_LUF_FT;
    }
    if (backend_policy < LP_LU_BACKEND_POLICY_LUF_FT ||
        backend_policy > LP_LU_BACKEND_POLICY_CGR) {
        return LP_LU_BACKEND_POLICY_LUF_FT;
    }
    return backend_policy;
}

void lu_apply_backend_policy(LUFactorization *lu, int backend_policy) {
    int effective;
    int base_updates;
    int strict_mode;

    if (!lu) return;

    effective = lu_normalize_backend_policy(backend_policy);
    base_updates = lu_default_max_updates_for_m(lu->m);
    strict_mode = lu_glpk_strict_mode(lu);

    lu->backend_policy = effective;
    lu->update_backend = LU_UPDATE_BACKEND_FT;
    lu->pivot_tol = RALPH_PIVOT_TOL;
    lu->growth_refactor_threshold = RALPH_LU_GROWTH_REFACTOR_THRESHOLD;
    lu->max_updates = base_updates;
    lu->use_ft_updates = 1;

    switch (effective) {
        case LP_LU_BACKEND_POLICY_CBG:
            /* CBG: conservative stability posture. */
            lu->update_backend = LU_UPDATE_BACKEND_BG_COMPAT;
            lu->use_ft_updates = 0;
            lu->max_updates = (base_updates * 3) / 4;
            if (lu->max_updates < 24) lu->max_updates = 24;
            lu->pivot_tol = RALPH_PIVOT_TOL * 1.5;
            lu->growth_refactor_threshold = RALPH_LU_GROWTH_REFACTOR_THRESHOLD * 0.5;
            break;
        case LP_LU_BACKEND_POLICY_CGR:
            /* CGR: aggressive posture. Strict GLPK-like mode keeps this off
             * the FT lane until a real GR Schur-complement update exists. */
            lu->update_backend = strict_mode
                ? LU_UPDATE_BACKEND_GR_COMPAT
                : LU_UPDATE_BACKEND_FT;
            lu->use_ft_updates = strict_mode ? 0 : 1;
            lu->max_updates = lu_cgr_max_updates_for_base(base_updates);
            lu->pivot_tol = RALPH_PIVOT_TOL * 0.75;
            lu->growth_refactor_threshold = RALPH_LU_GROWTH_REFACTOR_THRESHOLD * 1.5;
            break;
        case LP_LU_BACKEND_POLICY_LUF_FT:
        default:
            break;
    }

    if (lu->telemetry_enabled) {
        lu->telemetry.backend_policy_last = effective;
        if (effective == LP_LU_BACKEND_POLICY_LUF_FT) {
            lu->telemetry.backend_policy_luf_ft++;
        } else if (effective == LP_LU_BACKEND_POLICY_CBG) {
            lu->telemetry.backend_policy_cbg++;
        } else if (effective == LP_LU_BACKEND_POLICY_CGR) {
            lu->telemetry.backend_policy_cgr++;
        }
    }
}

static void lu_set_failure(LUFactorization *lu, int reason) {
    if (lu) {
        lu->last_failure_reason = reason;
    }
}

static void lu_mark_update_failure(LUFactorization *lu, int reason) {
    if (!lu || !lu->telemetry_enabled) return;
    switch ((LUFailureReason)reason) {
        case LU_FAIL_BAD_INPUT:
            lu->telemetry.update_fail_bad_input++;
            break;
        case LU_FAIL_MAX_UPDATES:
            lu->telemetry.update_fail_max_updates++;
            break;
        case LU_FAIL_SINGULAR_UPDATE:
            lu->telemetry.update_fail_singular_update++;
            break;
        case LU_FAIL_UPDATE_PIVOT_TOO_SMALL:
            lu->telemetry.update_fail_update_pivot_too_small++;
            break;
        case LU_FAIL_SPIKE_POOL_FULL:
            lu->telemetry.update_fail_spike_pool_full++;
            break;
        case LU_FAIL_ETA_ALLOC:
            lu->telemetry.update_fail_eta_alloc++;
            break;
        default:
            break;
    }
}

static void lu_mark_refactor_need(LUFactorization *lu, int reason) {
    if (!lu || !lu->telemetry_enabled) return;
    lu->telemetry.refactor_need_checks++;
    lu->telemetry.refactor_need_last_reason = reason;
    if (reason == LP_BFCP_REFACTOR_REASON_NONE) return;
    lu->telemetry.refactor_need_triggers++;
    switch ((LPBFCPRefactorReason)reason) {
        case LP_BFCP_REFACTOR_REASON_MAX_UPDATES:
            lu->telemetry.refactor_need_reason_max_updates++;
            break;
        case LP_BFCP_REFACTOR_REASON_GROWTH_GUARD:
            lu->telemetry.refactor_need_reason_growth_guard++;
            break;
        case LP_BFCP_REFACTOR_REASON_AVG_SPIKE_DENSITY:
            lu->telemetry.refactor_need_reason_avg_spike_density++;
            break;
        case LP_BFCP_REFACTOR_REASON_COND_SEVERE:
            lu->telemetry.refactor_need_reason_cond_severe++;
            break;
        case LP_BFCP_REFACTOR_REASON_COND_ADAPTIVE_LIMIT:
            lu->telemetry.refactor_need_reason_cond_adaptive_limit++;
            break;
        case LP_BFCP_REFACTOR_REASON_SPIKE_POOL_WARN:
            lu->telemetry.refactor_need_reason_spike_pool_warn++;
            break;
        case LP_BFCP_REFACTOR_REASON_SPIKE_WORK:
            lu->telemetry.refactor_need_reason_spike_work++;
            break;
        default:
            break;
    }
}

static int lu_update_is_aged(const LUFactorization *lu) {
    if (!lu) return 0;
    if (lu->max_updates > 0) {
        return (2 * lu->num_updates >= lu->max_updates);
    }
    return lu->num_updates >= 40;
}

static void lu_fill_bfcp_signals(const LUFactorization *lu,
                                 LPBFCPRefactorSignals *sig) {
    if (!lu || !sig) return;

    lp_bfcp_policy_refactor_signals_init(sig);
    sig->strict_mode = lu_glpk_strict_mode(lu);
    sig->num_updates = lu->num_updates;
    sig->max_updates = lu->max_updates;
    sig->growth_factor = lu->growth_factor;
    sig->growth_guard_threshold = (lu->growth_refactor_threshold > 0.0)
        ? lu->growth_refactor_threshold
        : RALPH_LU_GROWTH_REFACTOR_THRESHOLD;
    sig->use_ft_updates = lu_update_backend_is_ft(lu);
    sig->m = lu->m;
    sig->ft_num_updates = lu->ft_num_updates;
    sig->spike_pool_used = lu->spike_pool_used;
    sig->spike_pool_capacity = lu->spike_pool_capacity;
    sig->update_aged = lu_update_is_aged(lu);
    sig->min_ft_updates_for_avg_density = RALPH_SPIKE_DENSE_REJECT_MIN_UPDATES;
    sig->spike_dense_reject_m_min = RALPH_SPIKE_DENSE_REJECT_M_MIN;
    sig->spike_avg_refactor_ratio = RALPH_SPIKE_AVG_REFACTOR_RATIO;
    sig->spike_avg_refactor_aged_ratio = RALPH_SPIKE_AVG_REFACTOR_AGED_RATIO;
    sig->spike_pool_warn_pct = RALPH_SPIKE_POOL_WARN_PCT;
    sig->spike_work_multiplier = 8;
    sig->cond_min_updates = 10;
    sig->cond_estimate = lu->cond_estimate;
    sig->cond_severe_ratio = 1e10;
    sig->cond_adaptive_hi = 1e8;
    sig->cond_adaptive_mid = 1e6;
}

static double lu_dense_spike_reject_ratio(const LUFactorization *lu) {
    double ratio = RALPH_SPIKE_DENSE_BASE_RATIO;
    int backend_policy = LP_LU_BACKEND_POLICY_LUF_FT;
    if (!lp_glpk_strict_allow_lu_update_adaptive_thresholds(
            lu ? lu_glpk_strict_mode(lu) : 0)) {
        return 1.0;
    }
    if (!lu) return ratio;
    backend_policy = lu_normalize_backend_policy(lu->backend_policy);

    if (lu_update_is_aged(lu) ||
        lu->cond_estimate > 1e7 ||
        lu->growth_factor > 1e3) {
        ratio = RALPH_SPIKE_DENSE_AGED_RATIO;
    }

    if (backend_policy == LP_LU_BACKEND_POLICY_CBG) {
        ratio *= 0.9;
    } else if (backend_policy == LP_LU_BACKEND_POLICY_CGR) {
        ratio *= 1.1;
    }
    if (ratio < 0.25) ratio = 0.25;
    if (ratio > 0.90) ratio = 0.90;
    return ratio;
}

static double lu_update_pivot_ratio_threshold(const LUFactorization *lu) {
    double threshold = RALPH_LU_UPDATE_PIVOT_THRESHOLD;
    double update_ratio = 0.0;
    int backend_policy = LP_LU_BACKEND_POLICY_LUF_FT;
    if (!lp_glpk_strict_allow_lu_update_adaptive_thresholds(
            lu ? lu_glpk_strict_mode(lu) : 0)) {
        return threshold;
    }
    if (!lu) return threshold;
    backend_policy = lu_normalize_backend_policy(lu->backend_policy);

    if (lu->max_updates > 0) {
        update_ratio = (double)lu->num_updates / (double)lu->max_updates;
        if (update_ratio < 0.25) {
            threshold *= 0.5;
        } else if (update_ratio < 0.50) {
            threshold *= 0.75;
        }
    }

    if (lu->cond_estimate <= 1e4 && lu->growth_factor <= 10.0) {
        threshold *= 0.25;
    } else if (lu->cond_estimate <= 1e6 && lu->growth_factor <= 100.0) {
        threshold *= 0.5;
    } else if (lu->cond_estimate >= 1e8 || lu->growth_factor >= 1e4) {
        threshold *= 2.0;
    }

    if (backend_policy == LP_LU_BACKEND_POLICY_CBG) {
        threshold *= 1.25;
    } else if (backend_policy == LP_LU_BACKEND_POLICY_CGR) {
        threshold *= 0.85;
    }

    if (threshold < 1e-5) threshold = 1e-5;
    if (threshold > 5e-4) threshold = 5e-4;
    return threshold;
}

double lu_update_pivot_ratio_threshold_for_test(int num_updates,
                                                int max_updates,
                                                double cond_estimate,
                                                double growth_factor) {
    LUFactorization probe = {0};
    probe.num_updates = num_updates;
    probe.max_updates = max_updates;
    probe.cond_estimate = cond_estimate;
    probe.growth_factor = growth_factor;
    return lu_update_pivot_ratio_threshold(&probe);
}

/* ============================================================================
 * LU Factorization Creation/Destruction
 * ============================================================================ */

LUFactorization* lu_create(int m) {
    LUFactorization *lu = (LUFactorization*)calloc(1, sizeof(LUFactorization));
    int max_upd;
    if (!lu) return NULL;

    lu->m = m;
    lu->telemetry_enabled = 1;
    lu_apply_backend_policy(lu, LP_LU_BACKEND_POLICY_LUF_FT);
    max_upd = lu->max_updates;

    /* Calculate arena size for fixed-size arrays (with 8-byte alignment padding).
     * Arena contains: permutation arrays, FT column order, spike metadata,
     * eta metadata, BG/GR compatibility metadata, and hyper-sparse workspace
     * arrays. */
    size_t arena_size =
        /* int arrays of size m: perm, perm_inv, col_perm, col_perm_inv,
         * ft_col_order, ft_col_order_inv, hs_marked, hs_idx, hs_stack (9 arrays) */
        9 * (size_t)m * sizeof(int) +
        /* double arrays of size m: U_diag, hs_work1, hs_work2, hs_val, perm_work (5 arrays) */
        5 * (size_t)m * sizeof(double) +
        /* int arrays of size max_updates: eta_col, eta_nnz, schur_col,
         * schur_nnz, ft_spike_col, ft_spike_nnz, ft_spike_start (7 arrays) */
        7 * (size_t)max_upd * sizeof(int) +
        /* double array of size max_updates: ft_spike_diag (1 array) */
        (size_t)max_upd * sizeof(double) +
        /* T1.4: workspace for sparse-efficient factorization
         * int arrays of size m: ws_is_identity, ws_identity_row, ws_row_used,
         * ws_col_order, ws_col_order_inv, ws_row_perm, ws_L_pos, ws_U_pos,
         * ws_row_pos, ws_struct_nnz, ws_row_identity_col, ws_row_match_col,
         * ws_row_seen (13 arrays) */
        13 * (size_t)m * sizeof(int) +
        /* double array of size m: ws_identity_val (1 array) */
        1 * (size_t)m * sizeof(double) +
        /* Alignment padding */
        320;

    lu->arena = sh_arena_create(arena_size);
    if (!lu->arena) {
        lu_free(lu);
        return NULL;
    }

    /* Allocate permutation arrays from arena */
    lu->perm = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->perm_inv = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->col_perm = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->col_perm_inv = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));

    /* U diagonal cache from arena */
    lu->U_diag = (double*)sh_arena_alloc(lu->arena, m * sizeof(double));

    /* Initialize to identity permutation */
    for (int i = 0; i < m; i++) {
        lu->perm[i] = i;
        lu->perm_inv[i] = i;
        lu->col_perm[i] = i;
        lu->col_perm_inv[i] = i;
    }

    /* Eta file metadata from arena (but eta_indices/values arrays allocated separately) */
    lu->eta_capacity = max_upd;
    lu->num_eta = 0;
    lu->eta_col = (int*)sh_arena_alloc(lu->arena, max_upd * sizeof(int));
    lu->eta_nnz = (int*)sh_arena_alloc(lu->arena, max_upd * sizeof(int));
    lu->schur_capacity = max_upd;
    lu->schur_num_updates = 0;
    lu->schur_col = (int*)sh_arena_alloc(lu->arena, max_upd * sizeof(int));
    lu->schur_nnz = (int*)sh_arena_alloc(lu->arena, max_upd * sizeof(int));

    /* eta_indices and eta_values are arrays of pointers - allocated separately
     * because their contents are dynamically allocated during updates */
    lu->eta_indices = (int**)calloc(max_upd, sizeof(int*));
    lu->eta_values = (double**)calloc(max_upd, sizeof(double*));
    lu->schur_indices = (int**)calloc(max_upd, sizeof(int*));
    lu->schur_values = (double**)calloc(max_upd, sizeof(double*));

    if (!lu->eta_indices || !lu->eta_values ||
        !lu->schur_indices || !lu->schur_values) {
        lu_free(lu);
        return NULL;
    }

    for (int i = 0; i < max_upd; i++) {
        lu->eta_indices[i] = NULL;
        lu->eta_values[i] = NULL;
        lu->eta_nnz[i] = 0;
        lu->schur_indices[i] = NULL;
        lu->schur_values[i] = NULL;
        lu->schur_nnz[i] = 0;
    }

    /* Update structures from arena. Runtime update kernel mode (FT vs ETA)
     * is selected by backend policy and may change per solve. */
    lu->ft_num_updates = 0;
    lu->ft_col_order = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->ft_col_order_inv = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));

    for (int i = 0; i < m; i++) {
        lu->ft_col_order[i] = i;
        lu->ft_col_order_inv[i] = i;
    }

    /* Spike metadata from arena */
    lu->ft_spike_capacity = max_upd;
    lu->ft_spike_col = (int*)sh_arena_alloc(lu->arena, max_upd * sizeof(int));
    lu->ft_spike_diag = (double*)sh_arena_alloc(lu->arena, max_upd * sizeof(double));
    lu->ft_spike_nnz = (int*)sh_arena_alloc(lu->arena, max_upd * sizeof(int));
    lu->ft_spike_start = (int*)sh_arena_alloc(lu->arena, max_upd * sizeof(int));

    for (int i = 0; i < max_upd; i++) {
        lu->ft_spike_nnz[i] = 0;
        lu->ft_spike_diag[i] = 0.0;
        lu->ft_spike_start[i] = 0;
    }

    /* Hyper-sparse workspace from arena */
    lu->hs_work1 = (double*)sh_arena_calloc(lu->arena, m, sizeof(double));
    lu->hs_work2 = (double*)sh_arena_calloc(lu->arena, m, sizeof(double));
    lu->hs_marked = (int*)sh_arena_calloc(lu->arena, m, sizeof(int));
    lu->hs_idx = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->hs_val = (double*)sh_arena_alloc(lu->arena, m * sizeof(double));
    lu->hs_stack = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->perm_work = (double*)sh_arena_alloc(lu->arena, m * sizeof(double));

    /* T1.4: Sparse-efficient factorization workspace */
    lu->ws_is_identity = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->ws_identity_row = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->ws_identity_val = (double*)sh_arena_alloc(lu->arena, m * sizeof(double));
    lu->ws_row_used = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->ws_col_order = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->ws_col_order_inv = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->ws_row_perm = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->ws_L_pos = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->ws_U_pos = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->ws_row_pos = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->ws_struct_nnz = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->ws_row_identity_col = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->ws_row_match_col = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));
    lu->ws_row_seen = (int*)sh_arena_alloc(lu->arena, m * sizeof(int));

    /* Single check for all arena allocations */
    if (!lu->perm || !lu->perm_inv || !lu->col_perm || !lu->col_perm_inv ||
        !lu->U_diag || !lu->eta_col || !lu->eta_nnz ||
        !lu->schur_col || !lu->schur_nnz ||
        !lu->ft_col_order || !lu->ft_col_order_inv ||
        !lu->ft_spike_col || !lu->ft_spike_diag || !lu->ft_spike_nnz || !lu->ft_spike_start ||
        !lu->hs_work1 || !lu->hs_work2 || !lu->hs_marked ||
        !lu->hs_idx || !lu->hs_val || !lu->hs_stack || !lu->perm_work ||
        !lu->ws_is_identity || !lu->ws_identity_row || !lu->ws_identity_val ||
        !lu->ws_row_used || !lu->ws_col_order || !lu->ws_col_order_inv ||
        !lu->ws_row_perm || !lu->ws_L_pos || !lu->ws_U_pos || !lu->ws_row_pos ||
        !lu->ws_struct_nnz || !lu->ws_row_identity_col || !lu->ws_row_match_col ||
        !lu->ws_row_seen) {
        lu_free(lu);
        return NULL;
    }

    /* Contiguous spike pool - allocated separately (large, variable size)
     * Estimate: each spike has ~m/2 non-zeros on average.
     * Use size_t to prevent integer overflow on large problems. */
    {
        size_t pool_size = (size_t)max_upd * ((size_t)m / 2 + 20);
        if (pool_size > (size_t)INT_MAX) {
            pool_size = (size_t)INT_MAX;
        }
        lu->spike_pool_capacity = (int)pool_size;
    }
    lu->spike_pool_idx = (int*)calloc(lu->spike_pool_capacity, sizeof(int));
    lu->spike_pool_val = (double*)calloc(lu->spike_pool_capacity, sizeof(double));
    lu->spike_pool_used = 0;

    if (!lu->spike_pool_idx || !lu->spike_pool_val) {
        lu_free(lu);
        return NULL;
    }

    /* (B4: spike compaction removed) */

    /* Initialize condition number tracking */
    lu->min_diag_U = RALPH_INFINITY;
    lu->max_diag_U = 0.0;
    lu->cond_estimate = 1.0;
    lu->growth_factor = 1.0;

    /* Initialize redundant row hints (set by caller before factorization) */
    lu->redundant_rows = NULL;
    lu->num_redundant = 0;
    lu->allow_regularization = 0;
    lu->max_regularizations = 0;
    lu->num_regularized = 0;
    lu->last_failure_reason = LU_FAIL_NONE;
    lu->last_refactor_trigger_reason = LP_BFCP_REFACTOR_REASON_NONE;

    /* Pre-allocate dense workspace for fallback factorization (m×m matrix)
     * Allocated separately due to large size O(m²) */
    lu->dense_work = (double*)calloc((size_t)m * (size_t)m, sizeof(double));
    lu->mkz_work = NULL;
    lu->mkz_work_capacity = 0;

    if (!lu->dense_work) {
        lu_free(lu);
        return NULL;
    }

    /* T1.4: Pre-allocate L/U output arrays with initial capacity.
     * Capacity grows as needed; after 1-2 factorizations it stabilizes. */
    lu->LU_out_capacity = m * 4;
    lu->L_colptr = (int*)calloc(m + 1, sizeof(int));
    lu->L_rowidx = (int*)calloc(lu->LU_out_capacity, sizeof(int));
    lu->L_values = (double*)calloc(lu->LU_out_capacity, sizeof(double));
    lu->U_colptr = (int*)calloc(m + 1, sizeof(int));
    lu->U_rowidx = (int*)calloc(lu->LU_out_capacity, sizeof(int));
    lu->U_values = (double*)calloc(lu->LU_out_capacity, sizeof(double));

    if (!lu->L_colptr || !lu->L_rowidx || !lu->L_values ||
        !lu->U_colptr || !lu->U_rowidx || !lu->U_values) {
        lu_free(lu);
        return NULL;
    }

    /* T1.4 full: Pre-allocate COO arrays for sparse-efficient factorization.
     * These replace per-call malloc/free of 6 arrays (L_row/col/val, U_row/col/val).
     * Initial capacity m*4; grows as needed, stabilizes after 1-2 factorizations. */
    lu->coo_capacity = m * 4;
    lu->coo_L_row = (int*)calloc(lu->coo_capacity, sizeof(int));
    lu->coo_L_col = (int*)calloc(lu->coo_capacity, sizeof(int));
    lu->coo_L_val = (double*)calloc(lu->coo_capacity, sizeof(double));
    lu->coo_U_row = (int*)calloc(lu->coo_capacity, sizeof(int));
    lu->coo_U_col = (int*)calloc(lu->coo_capacity, sizeof(int));
    lu->coo_U_val = (double*)calloc(lu->coo_capacity, sizeof(double));

    if (!lu->coo_L_row || !lu->coo_L_col || !lu->coo_L_val ||
        !lu->coo_U_row || !lu->coo_U_col || !lu->coo_U_val) {
        lu_free(lu);
        return NULL;
    }

    /* T1.4 full: Symbolic cache starts invalid */
    lu->sym_valid = 0;
    lu->sym_fingerprint = 0;

    /* W1: CSR transpose arrays start invalid (built after first factorization) */
    lu->csr_valid = 0;

    /* Sparse Markowitz LU (default on for k >= MARKOWITZ_MIN_K) */
    lu->mkz_enabled = 1;
    lu->mkz_pool_mult_hint = 4;
    lu->mkz_circuit_fingerprint = 0;
    lu->mkz_circuit_bad_streak = 0;
    lu->mkz_circuit_skip_budget = 0;
    lu->mkz_global_singular_streak = 0;
    lu->mkz_global_skip_budget = 0;
    lu->idsep_retry_fingerprint = 0;
    lu->idsep_retry_streak = 0;
    lp_telemetry_reset_lu(lu);

    /* T2.1: Supernodal LU (default off, opt-in via lu_supernode param) */
    lu->sn_enabled = 0;
    lu->sn_symbolic = NULL;

    return lu;
}

void lu_free(LUFactorization *lu) {
    if (!lu) return;

    /* Free L/U storage (allocated during factorization, not in arena) */
    SAFE_FREE(lu->L_colptr);
    SAFE_FREE(lu->L_rowidx);
    SAFE_FREE(lu->L_values);
    SAFE_FREE(lu->U_colptr);
    SAFE_FREE(lu->U_rowidx);
    SAFE_FREE(lu->U_values);

    /* Free COO arrays (T1.4 full: pre-allocated, not in arena) */
    SAFE_FREE(lu->coo_L_row);
    SAFE_FREE(lu->coo_L_col);
    SAFE_FREE(lu->coo_L_val);
    SAFE_FREE(lu->coo_U_row);
    SAFE_FREE(lu->coo_U_col);
    SAFE_FREE(lu->coo_U_val);

    /* Free eta file contents (dynamically allocated during updates) */
    if (lu->eta_indices) {
        for (int i = 0; i < lu->eta_capacity; i++) {
            SAFE_FREE(lu->eta_indices[i]);
        }
        SAFE_FREE(lu->eta_indices);
    }
    if (lu->eta_values) {
        for (int i = 0; i < lu->eta_capacity; i++) {
            SAFE_FREE(lu->eta_values[i]);
        }
        SAFE_FREE(lu->eta_values);
    }
    if (lu->schur_indices) {
        for (int i = 0; i < lu->schur_capacity; i++) {
            SAFE_FREE(lu->schur_indices[i]);
        }
        SAFE_FREE(lu->schur_indices);
    }
    if (lu->schur_values) {
        for (int i = 0; i < lu->schur_capacity; i++) {
            SAFE_FREE(lu->schur_values[i]);
        }
        SAFE_FREE(lu->schur_values);
    }

    /* (B4: spike compaction removed) */

    /* Free spike pool (large variable-size arrays, not in arena) */
    SAFE_FREE(lu->spike_pool_idx);
    SAFE_FREE(lu->spike_pool_val);

    /* Free dense workspace (O(m²), not in arena) */
    SAFE_FREE(lu->dense_work);
    SAFE_FREE(lu->mkz_work);
    lu->mkz_work_capacity = 0;

    /* T2.1: Free cached supernodal symbolic analysis */
    if (lu->sn_symbolic) {
        sn_symbolic_free((SNSymbolic *)lu->sn_symbolic);
        lu->sn_symbolic = NULL;
    }
    SAFE_FREE(lu->sn_work);
    lu->sn_work_capacity = 0;

    /* Free arena (frees all fixed-size arrays in one call:
     * perm, perm_inv, col_perm, col_perm_inv, U_diag,
     * eta_col, eta_nnz, ft_col_order, ft_col_order_inv,
     * ft_spike_col, ft_spike_diag, ft_spike_nnz, ft_spike_start,
     * hs_work1, hs_work2, hs_marked, hs_idx, hs_val, hs_stack, perm_work) */
    sh_arena_free(lu->arena);
    lu->arena = NULL;

    /* NULL out arena-allocated pointers for safety */
    lu->perm = NULL;
    lu->perm_inv = NULL;
    lu->col_perm = NULL;
    lu->col_perm_inv = NULL;
    lu->U_diag = NULL;
    lu->eta_col = NULL;
    lu->eta_nnz = NULL;
    lu->schur_indices = NULL;
    lu->schur_values = NULL;
    lu->schur_col = NULL;
    lu->schur_nnz = NULL;
    lu->ft_col_order = NULL;
    lu->ft_col_order_inv = NULL;
    lu->ft_spike_col = NULL;
    lu->ft_spike_diag = NULL;
    lu->ft_spike_nnz = NULL;
    lu->ft_spike_start = NULL;
    lu->hs_work1 = NULL;
    lu->hs_work2 = NULL;
    lu->hs_marked = NULL;
    lu->hs_idx = NULL;
    lu->hs_val = NULL;
    lu->hs_stack = NULL;
    lu->perm_work = NULL;
    lu->ws_is_identity = NULL;
    lu->ws_identity_row = NULL;
    lu->ws_identity_val = NULL;
    lu->ws_row_used = NULL;
    lu->ws_col_order = NULL;
    lu->ws_col_order_inv = NULL;
    lu->ws_row_perm = NULL;
    lu->ws_L_pos = NULL;
    lu->ws_U_pos = NULL;
    lu->ws_row_pos = NULL;
    lu->ws_struct_nnz = NULL;
    lu->ws_row_identity_col = NULL;
    lu->ws_row_match_col = NULL;
    lu->ws_row_seen = NULL;

    free(lu);
}

/* External sparse factorization (from lu_sparse.c) */
int lu_factorize_sparse(LUFactorization *lu, const SparseMatrix *B);
int lu_factorize_sparse_efficient(LUFactorization *lu, const SparseMatrix *B);
int lu_factorize_sparse_strict_dispatch(LUFactorization *lu, const SparseMatrix *B);

/* ============================================================================
 * Main LU Factorization Entry Point
 * ============================================================================ */

/* Main factorization entry point.
 *
 * Uses the efficient sparse implementation with AMD ordering for fill-in
 * reduction. Falls back to dense if sparse fails.
 */
int lu_factorize(LUFactorization *lu, const SparseMatrix *B) {
    if (!lu || !B) {
        lu_set_failure(lu, LU_FAIL_BAD_INPUT);
        return -1;
    }
    lu_set_failure(lu, LU_FAIL_NONE);
    lu->last_refactor_trigger_reason = LP_BFCP_REFACTOR_REASON_NONE;
    lp_telemetry_prepare_lu_factorize(lu, B);

    /* Try sparse factorization first. Strict GLPK-like lane uses a dedicated
     * one-shot dispatch path rather than the adaptive default sparse retries. */
    int result = lu_strict_lane_active(lu)
        ? lu_factorize_sparse_strict_dispatch(lu, B)
        : lu_factorize_sparse_efficient(lu, B);
    if (result == 0) {
        build_csr_transpose(lu);  /* W1: CSR transposes for sparse BTRAN */
        lu_set_failure(lu, LU_FAIL_NONE);
        return 0;
    }

    if (!lu_strict_allow_top_level_dense_fallback(lu)) {
        return result;
    }

    /* Fall back to dense */
    result = lu_factorize_dense(lu, B);
    if (result == 0) {
        build_csr_transpose(lu);  /* W1: CSR transposes for sparse BTRAN */
        lu_set_failure(lu, LU_FAIL_NONE);
        lp_telemetry_lu_mark_dense_fallback(lu);
    }
    return result;
}

/* ============================================================================
 * Dense LU Factorization (fallback for numerical robustness)
 * ============================================================================ */

/* Perform LU factorization: PA = LU using partial pivoting */
int lu_factorize_dense(LUFactorization *lu, const SparseMatrix *B) {
    if (!lu || !B) {
        lu_set_failure(lu, LU_FAIL_BAD_INPUT);
        return -1;
    }
    double t_dense_start_ms = lp_telemetry_timer_start();
#define DENSE_RETURN(code) do { \
    lp_telemetry_lu_record_dense_factorize_timed(lu, t_dense_start_ms); \
    return (code); \
} while (0)
    lu_set_failure(lu, LU_FAIL_NONE);
    if (B->nrows != B->ncols || B->nrows != lu->m) {
        lu_set_failure(lu, LU_FAIL_BAD_INPUT);
        DENSE_RETURN(-1);
    }

    int m = lu->m;

    /* Use pre-allocated dense workspace (m×m matrix) */
    double *A = lu->dense_work;
    if (!A) {
        lu_set_failure(lu, LU_FAIL_FACTOR_ALLOC);
        DENSE_RETURN(-1);
    }

    /* Zero the workspace */
    memset(A, 0, (size_t)m * (size_t)m * sizeof(double));

    /* Fill dense matrix from sparse (column-major order) */
    for (int j = 0; j < m; j++) {
        for (int p = B->colptr[j]; p < B->colptr[j + 1]; p++) {
            A[B->rowidx[p] + j * m] = B->values[p];
        }
    }

    /* Initialize permutations to identity.
     * col_perm MUST be reset here: when lu_factorize_sparse_efficient fails
     * and falls back to dense, col_perm retains the non-trivial column ordering
     * from a previous sparse factorization. Dense LU uses no column pivoting,
     * so col_perm must be identity for lu_solve to produce correct results. */
    for (int i = 0; i < m; i++) {
        lu->perm[i] = i;
        lu->col_perm[i] = i;
        lu->col_perm_inv[i] = i;
    }

    /* Gaussian elimination with partial pivoting */
    for (int k = 0; k < m; k++) {
        /* Find pivot */
        int pivot_row = k;
        double max_val = fabs(A[k + k * m]);

        for (int i = k + 1; i < m; i++) {
            double val = fabs(A[i + k * m]);
            if (val > max_val) {
                max_val = val;
                pivot_row = i;
            }
        }

        /* Check for singular matrix */
        if (max_val < lu->pivot_tol) {
            /* Check if this row corresponds to a redundant constraint.
             * For two-phase simplex, redundant rows (with stuck artificials)
             * cause singularity but can be safely regularized.
             * The original row number at position k is lu->perm[k]. */
            int can_regularize = 0;

            /* First check: pre-marked redundant rows (most reliable) */
            if (lu->redundant_rows && lu->num_redundant > 0) {
                /* Check all unfactored rows (k to m-1) that could be pivot row */
                for (int check = k; check < m; check++) {
                    int orig_row = lu->perm[check];
                    if (lu->redundant_rows[orig_row]) {
                        can_regularize = 1;
                        pivot_row = check;  /* Use this redundant row */
                        break;
                    }
                }
            }

            /* Second check: allow_regularization flag for rank-deficient problems.
             * If no pre-marked rows but regularization is allowed, regularize this row
             * up to the limit. This handles problems with implicit redundancy. */
            if (!can_regularize && lu->allow_regularization &&
                lu->num_regularized < lu->max_regularizations) {
                can_regularize = 1;
                /* Use current pivot_row (k) - no need to search */
            }

            if (can_regularize) {
                /* Regularize: set diagonal to 1.0 to make row independent.
                 * NOTE: This is currently disabled in simplex.c because:
                 * - Small values (1e-6) cause NaN via large multipliers (1/1e-6 = 1e6)
                 * - Large values (1.0) destroy constraint structure -> UNBOUNDED
                 * The proper fix is threshold pivoting to avoid near-singular bases. */
                lu->num_regularized++;
#ifdef RALPH_DEBUG_LU
                fprintf(stderr, "[lu_factorize_dense] Regularizing row %d (orig %d) at step %d (total: %d)\n",
                        pivot_row, lu->perm[pivot_row], k, lu->num_regularized);
#endif
                if (pivot_row != k) {
                    /* Swap the redundant row into position k */
                    for (int j = 0; j < m; j++) {
                        double tmp = A[k + j * m];
                        A[k + j * m] = A[pivot_row + j * m];
                        A[pivot_row + j * m] = tmp;
                    }
                    int tmp = lu->perm[k];
                    lu->perm[k] = lu->perm[pivot_row];
                    lu->perm[pivot_row] = tmp;
                }
                /* Set diagonal to 1.0 (regularization) */
                A[k + k * m] = 1.0;
                max_val = 1.0;
            } else {
#ifdef RALPH_DEBUG_LU
                fprintf(stderr, "[lu_factorize_dense] Singular at step %d, max_val=%.2e, redundant_rows=%p, num_redundant=%d, allow_reg=%d, max_reg=%d\n",
                        k, max_val, (void*)lu->redundant_rows, lu->num_redundant,
                        lu->allow_regularization, lu->max_regularizations);
#endif
                lu_set_failure(lu, LU_FAIL_FACTOR_SINGULAR);
                DENSE_RETURN(-1);  /* Truly singular, no redundant row to help */
            }
        }

        /* Swap rows if necessary */
        if (pivot_row != k) {
            for (int j = 0; j < m; j++) {
                double tmp = A[k + j * m];
                A[k + j * m] = A[pivot_row + j * m];
                A[pivot_row + j * m] = tmp;
            }
            int tmp = lu->perm[k];
            lu->perm[k] = lu->perm[pivot_row];
            lu->perm[pivot_row] = tmp;
        }

        /* Eliminate below diagonal */
        double pivot = A[k + k * m];
        for (int i = k + 1; i < m; i++) {
            double mult = A[i + k * m] / pivot;
            A[i + k * m] = mult;  /* Store L entry */

            for (int j = k + 1; j < m; j++) {
                A[i + j * m] -= mult * A[k + j * m];
            }
        }
    }

    /* Compute inverse permutation */
    for (int i = 0; i < m; i++) {
        lu->perm_inv[lu->perm[i]] = i;
    }

    /* Extract L and U in sparse format */
    /* Count non-zeros */
    int nnz_L = 0, nnz_U = 0;
    for (int j = 0; j < m; j++) {
        for (int i = j + 1; i < m; i++) {
            if (fabs(A[i + j * m]) > RALPH_ZERO_TOL) nnz_L++;
        }
        for (int i = 0; i <= j; i++) {
            if (fabs(A[i + j * m]) > RALPH_ZERO_TOL) nnz_U++;
        }
    }

    /* Add diagonal of L (implicit ones) */
    nnz_L += m;

    /* T1.4: Reuse pre-allocated L/U arrays, grow only if needed */
    int needed = nnz_L > nnz_U ? nnz_L : nnz_U;
    if (needed > lu->LU_out_capacity) {
        int new_cap = needed * 2;
        SAFE_FREE(lu->L_rowidx); SAFE_FREE(lu->L_values);
        SAFE_FREE(lu->U_rowidx); SAFE_FREE(lu->U_values);
        lu->L_rowidx = (int*)calloc(new_cap, sizeof(int));
        lu->L_values = (double*)calloc(new_cap, sizeof(double));
        lu->U_rowidx = (int*)calloc(new_cap, sizeof(int));
        lu->U_values = (double*)calloc(new_cap, sizeof(double));
        lu->LU_out_capacity = new_cap;
        if (!lu->L_rowidx || !lu->L_values || !lu->U_rowidx || !lu->U_values) {
            lu_set_failure(lu, LU_FAIL_FACTOR_ALLOC);
            DENSE_RETURN(-1);
        }
    }
    memset(lu->L_colptr, 0, (m + 1) * sizeof(int));
    memset(lu->U_colptr, 0, (m + 1) * sizeof(int));

    /* Fill L (unit lower triangular stored with explicit diagonal) */
    int idx = 0;
    for (int j = 0; j < m; j++) {
        lu->L_colptr[j] = idx;
        /* Diagonal (1.0) */
        lu->L_rowidx[idx] = j;
        lu->L_values[idx] = 1.0;
        idx++;
        /* Below diagonal */
        for (int i = j + 1; i < m; i++) {
            double val = A[i + j * m];
            if (fabs(val) > RALPH_ZERO_TOL) {
                lu->L_rowidx[idx] = i;
                lu->L_values[idx] = val;
                idx++;
            }
        }
    }
    lu->L_colptr[m] = idx;
    lu->nnz_L = idx;

    /* Fill U (upper triangular) */
    idx = 0;
    for (int j = 0; j < m; j++) {
        lu->U_colptr[j] = idx;
        for (int i = 0; i <= j; i++) {
            double val = A[i + j * m];
            if (fabs(val) > RALPH_ZERO_TOL) {
                lu->U_rowidx[idx] = i;
                lu->U_values[idx] = val;
                idx++;
            }
        }
    }
    lu->U_colptr[m] = idx;
    lu->nnz_U = idx;

    lu_update_backend_reset(lu);

    lu->num_updates = 0;

    /* Extract U diagonals and compute condition number estimate */
    lu->min_diag_U = RALPH_INFINITY;
    lu->max_diag_U = 0.0;
    for (int j = 0; j < m; j++) {
        lu->U_diag[j] = 0.0;  /* Default in case not found */
        for (int p = lu->U_colptr[j]; p < lu->U_colptr[j + 1]; p++) {
            if (lu->U_rowidx[p] == j) {
                double val = lu->U_values[p];
                lu->U_diag[j] = val;
                double absval = fabs(val);
                if (absval < lu->min_diag_U) lu->min_diag_U = absval;
                if (absval > lu->max_diag_U) lu->max_diag_U = absval;
                break;
            }
        }
    }
    if (lu->min_diag_U > RALPH_ZERO_TOL) {
        lu->cond_estimate = lu->max_diag_U / lu->min_diag_U;
    } else {
        lu->cond_estimate = RALPH_INFINITY;
    }
    lu->growth_factor = 1.0;

    /* Note: A is pre-allocated lu->dense_work, no free needed */
    lu_set_failure(lu, LU_FAIL_NONE);
    DENSE_RETURN(0);
#undef DENSE_RETURN
}

/* ============================================================================
 * Solve Systems Using LU Factorization
 * ============================================================================ */

/* Triangular-solve micro-kernels shared by dense/sparse variants.
 * Keep these simple and branch-light so the hot inner loops stay predictable. */
static inline void tri_scatter_sub(const int *idx, const double *val,
                                   int nnz, double alpha, double *x) {
    int p = 0;
    int nnz4 = nnz & ~3;
    for (; p < nnz4; p += 4) {
        x[idx[p]]     -= val[p] * alpha;
        x[idx[p + 1]] -= val[p + 1] * alpha;
        x[idx[p + 2]] -= val[p + 2] * alpha;
        x[idx[p + 3]] -= val[p + 3] * alpha;
    }
    for (; p < nnz; p++) {
        x[idx[p]] -= val[p] * alpha;
    }
}

static inline void tri_scatter_sub_lt(const int *idx, const double *val,
                                      int nnz, int limit, double alpha, double *x) {
    int p = 0;
    int nnz4 = nnz & ~3;
    for (; p < nnz4; p += 4) {
        int i0 = idx[p];
        int i1 = idx[p + 1];
        int i2 = idx[p + 2];
        int i3 = idx[p + 3];
        if (i0 < limit) x[i0] -= val[p] * alpha;
        if (i1 < limit) x[i1] -= val[p + 1] * alpha;
        if (i2 < limit) x[i2] -= val[p + 2] * alpha;
        if (i3 < limit) x[i3] -= val[p + 3] * alpha;
    }
    for (; p < nnz; p++) {
        int i = idx[p];
        if (i < limit) x[i] -= val[p] * alpha;
    }
}

static inline double tri_dot(const int *idx, const double *val,
                             int nnz, const double *x) {
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    int p = 0;
    int nnz4 = nnz & ~3;
    for (; p < nnz4; p += 4) {
        s0 += val[p] * x[idx[p]];
        s1 += val[p + 1] * x[idx[p + 1]];
        s2 += val[p + 2] * x[idx[p + 2]];
        s3 += val[p + 3] * x[idx[p + 3]];
    }
    double sum = s0 + s1 + s2 + s3;
    for (; p < nnz; p++) {
        sum += val[p] * x[idx[p]];
    }
    return sum;
}

static inline double tri_dot_lt(const int *idx, const double *val,
                                int nnz, int limit, const double *x) {
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    int p = 0;
    int nnz4 = nnz & ~3;
    for (; p < nnz4; p += 4) {
        int i0 = idx[p];
        int i1 = idx[p + 1];
        int i2 = idx[p + 2];
        int i3 = idx[p + 3];
        if (i0 < limit) s0 += val[p] * x[i0];
        if (i1 < limit) s1 += val[p + 1] * x[i1];
        if (i2 < limit) s2 += val[p + 2] * x[i2];
        if (i3 < limit) s3 += val[p + 3] * x[i3];
    }
    double sum = s0 + s1 + s2 + s3;
    for (; p < nnz; p++) {
        int i = idx[p];
        if (i < limit) sum += val[p] * x[i];
    }
    return sum;
}

static inline double tri_dot_masked(const int *idx, const double *val,
                                    int nnz, const double *x, const int *mask) {
    double sum = 0.0;
    for (int p = 0; p < nnz; p++) {
        int i = idx[p];
        if (mask[i]) sum += val[p] * x[i];
    }
    return sum;
}

static inline double tri_dot_lt_masked(const int *idx, const double *val,
                                       int nnz, int limit,
                                       const double *x, const int *mask) {
    double sum = 0.0;
    for (int p = 0; p < nnz; p++) {
        int i = idx[p];
        if (i < limit && mask[i]) sum += val[p] * x[i];
    }
    return sum;
}

/* Solve Lx = b (forward substitution) */
static void solve_L(const LUFactorization *lu, const double *b, double *x) {
    int m = lu->m;
    const int *L_colptr = lu->L_colptr;
    const int *L_rowidx = lu->L_rowidx;
    const double *L_values = lu->L_values;

    /* Apply row permutation */
    for (int i = 0; i < m; i++) {
        x[i] = b[lu->perm[i]];
    }

    /* Forward substitution */
    for (int j = 0; j < m; j++) {
        /* x[j] already has the right value (L[j,j] = 1) */
        double xj = x[j];
        if (fabs(xj) <= RALPH_ZERO_TOL) continue;

        /* Update remaining elements */
        int p0 = L_colptr[j] + 1;
        int p1 = L_colptr[j + 1];
        tri_scatter_sub(L_rowidx + p0, L_values + p0, p1 - p0, xj, x);
    }
}

/* Solve Ux = b (backward substitution) */
static void solve_U(const LUFactorization *lu, const double *b, double *x) {
    int m = lu->m;
    const int *U_colptr = lu->U_colptr;
    const int *U_rowidx = lu->U_rowidx;
    const double *U_values = lu->U_values;
    const double *U_diag = lu->U_diag;

    vec_copy_data(x, b, m);

    /* Backward substitution - use cached diagonals for speed */
    for (int j = m - 1; j >= 0; j--) {
        double diag = U_diag[j];

        if (fabs(diag) < RALPH_PIVOT_TOL) {
            x[j] = 0.0;  /* Effectively zero row */
            continue;
        }

        x[j] /= diag;
        double xj = x[j];
        if (fabs(xj) <= RALPH_ZERO_TOL) continue;

        /* Update remaining elements (off-diagonal) */
        int p0 = U_colptr[j];
        int p1 = U_colptr[j + 1];
        tri_scatter_sub_lt(U_rowidx + p0, U_values + p0, p1 - p0, j, xj, x);
    }
}

/* Solve L'x = b (backward substitution with L transpose) */
static void solve_Lt(const LUFactorization *lu, const double *b, double *x) {
    int m = lu->m;
    const int *L_colptr = lu->L_colptr;
    const int *L_rowidx = lu->L_rowidx;
    const double *L_values = lu->L_values;

    vec_copy_data(x, b, m);

    /* Backward substitution with L transpose */
    for (int j = m - 1; j >= 0; j--) {
        int p0 = L_colptr[j] + 1;
        int p1 = L_colptr[j + 1];
        double sum = tri_dot(L_rowidx + p0, L_values + p0, p1 - p0, x);
        x[j] -= sum;
        /* L[j,j] = 1, so no division needed */
    }

    /* Apply inverse row permutation using pre-allocated workspace */
    double *temp = lu->perm_work;
    for (int i = 0; i < m; i++) {
        temp[lu->perm[i]] = x[i];
    }
    vec_copy_data(x, temp, m);
}

/* Solve U'x = b (forward substitution with U transpose) */
static void solve_Ut(const LUFactorization *lu, const double *b, double *x) {
    int m = lu->m;
    const int *U_colptr = lu->U_colptr;
    const int *U_rowidx = lu->U_rowidx;
    const double *U_values = lu->U_values;
    const double *U_diag = lu->U_diag;

    vec_copy_data(x, b, m);

    /* Forward substitution with U transpose (U' is lower triangular)
     * For i = 0, 1, ..., m-1:
     *   x[i] = (b[i] - sum_{j<i} U'[i,j] * x[j]) / U'[i,i]
     *        = (b[i] - sum_{j<i} U[j,i] * x[j]) / U[i,i]
     * Note: U[j,i] is in column i, row j (for j < i)
     */
    for (int i = 0; i < m; i++) {
        /* Subtract contributions from earlier solved variables:
         * sum of U'[i,j] * x[j] = U[j,i] * x[j] for j < i
         * U[j,i] is in COLUMN i (not column j!) at ROW j
         */
        int p0 = U_colptr[i];
        int p1 = U_colptr[i + 1];
        double sum = tri_dot_lt(U_rowidx + p0, U_values + p0, p1 - p0, i, x);

        double diag = U_diag[i];

        if (fabs(diag) < RALPH_PIVOT_TOL) {
            x[i] = 0.0;
            continue;
        }

        x[i] = (x[i] - sum) / diag;
    }
}

/* Solve Bx = b where B = basis matrix */
void lu_solve(const LUFactorization *lu, double *rhs, double *solution) {
    int m = lu->m;
    /* Use pre-allocated workspace (avoids malloc in hot path) */
    double *work = lu->hs_work1;
    double *work2 = lu->hs_work2;

    /* For sparse LU with column pivoting: PAQ = LU
     * Solve Bx = b  =>  PAQx = Pb  =>  LUQ'x = Pb
     * Let z = Q'x, then LUz = Pb
     * 1. Solve Ly = Pb (forward subst with row perm)
     * 2. Solve Uz = y (backward subst)
     * 3. x = Qz (apply column permutation)
     */

    /* First: solve Ly = Pb */
    solve_L(lu, rhs, work);

    /* Then: solve Uz = y */
    solve_U(lu, work, work2);

    /* Apply updates (in step coordinates, before column permutation) */
    (void)lu_update_backend_apply_forward(lu, work2);

    /* Apply column permutation: x[col_perm[i]] = z[i] */
    for (int i = 0; i < m; i++) {
        solution[lu->col_perm[i]] = work2[i];
    }
}

/* Solve B'x = b (for computing row prices) */
void lu_solve_transpose(const LUFactorization *lu, double *rhs, double *solution) {
    int m = lu->m;
    /* Use pre-allocated workspace (avoids malloc in hot path) */
    double *work = lu->hs_work1;

    /* For sparse LU with column pivoting: PAQ = LU
     * So B = P'LUQ', and B' = QU'L'P
     * With eta updates: B_new' = E_n' * ... * E_1' * B'
     * Solve B_new'x = b:
     * 1. Apply inverse col perm: y[i] = b[col_perm[i]] (converts to step coords)
     * 2. Apply eta updates in reverse (in step coordinates)
     * 3. Solve U'z = y
     * 4. Solve L'w = z, then apply P': x[perm[i]] = w[i]
     */

    /* Apply inverse column permutation: y[i] = b[col_perm[i]] */
    for (int i = 0; i < m; i++) {
        work[i] = rhs[lu->col_perm[i]];
    }

    /* Apply updates in reverse (in step coordinates) */
    lu_update_backend_apply_backward(lu, work);

    /* Solve U'z = y */
    solve_Ut(lu, work, solution);

    /* Solve L'x = z and apply P' (solve_Lt handles the row permutation) */
    solve_Lt(lu, solution, work);

    vec_copy_data(solution, work, m);
}

/* ============================================================================
 * Sparse LU Solves - Exploit RHS Sparsity
 * ============================================================================
 *
 * These routines exploit sparsity in both L/U factors AND the RHS vector.
 * Key optimization: when solving Lx = b with sparse b, we only need to
 * compute x[j] for j in the "reach" of the nonzero pattern of b.
 */

/*
 * Sparse FTRAN: Solve Bx = b where b is sparse
 *
 * Simplified version that:
 * 1. Falls back to dense for non-sparse RHS
 * 2. Uses selective forward/backward substitution for sparse RHS
 */
void lu_solve_sparse(const LUFactorization *lu,
                     int nnz_rhs, const int *rhs_idx, const double *rhs_val,
                     double *solution) {
    if (!lu || !solution) return;

    int m = lu->m;

    /* Use pre-allocated workspace (avoids malloc in hot path) */
    double *work = lu->hs_work1;
    double *work2 = lu->hs_work2;

    /* Clear workspace */
    memset(work, 0, m * sizeof(double));

    /* If RHS is too dense, use direct dense solve path
     * (must inline to avoid aliasing issues with workspace)
     * Threshold: use sparse path for RHS with <= 25% density (m/4)
     */
    if (nnz_rhs > m / 4) {
        /* Build dense RHS vector in work */
        for (int k = 0; k < nnz_rhs; k++) {
            if (rhs_idx[k] >= 0 && rhs_idx[k] < m)
                work[rhs_idx[k]] = rhs_val[k];
        }

        /* Inline the dense solve: Ly=Pb, Uz=y, apply updates, apply col perm */
        solve_L(lu, work, work2);        /* work2 = L^{-1} * P * work */
        solve_U(lu, work2, work);        /* work = U^{-1} * work2 */

        (void)lu_update_backend_apply_forward(lu, work);

        for (int i = 0; i < m; i++) {
            solution[lu->col_perm[i]] = work[i];
        }
        return;
    }

    /* Sparse path: Clear work2 for sparse solve (work already cleared) */
    memset(work2, 0, m * sizeof(double));

    /* Build permuted RHS and track nonzero indices */
    int *perm_rhs_idx = (int*)lu->perm_work;  /* Reuse perm_work as int array */
    int perm_rhs_nnz = 0;
    for (int k = 0; k < nnz_rhs; k++) {
        int orig_row = rhs_idx[k];
        if (orig_row >= 0 && orig_row < m) {
            int perm_row = lu->perm_inv[orig_row];
            work[perm_row] = rhs_val[k];
            perm_rhs_idx[perm_rhs_nnz++] = perm_row;
        }
    }

    /* Compute reach through L - only these indices need processing */
    int *reach = lu->hs_idx;
    int reach_nnz;
    compute_reach_L(lu, perm_rhs_nnz, perm_rhs_idx, reach, &reach_nnz, lu->hs_marked);

    /* Forward solve L: only process indices in reach (sorted ascending) */
    for (int i = 0; i < reach_nnz; i++) {
        int j = reach[i];
        double xj = work[j];
        if (fabs(xj) < RALPH_ZERO_TOL) continue;

        /* Update rows below j where L[i,j] != 0 */
        for (int p = lu->L_colptr[j] + 1; p < lu->L_colptr[j + 1]; p++) {
            work[lu->L_rowidx[p]] -= lu->L_values[p] * xj;
        }
    }

    /* Backward solve U - use reach-based solve for U as well */
    /* After L solve, nonzeros are at reach indices; use these for U reach */
    int *u_reach = (int*)lu->perm_work;  /* Reuse for U reach */
    int u_reach_nnz;
    compute_reach_U(lu, reach_nnz, reach, u_reach, &u_reach_nnz, lu->hs_marked);

    /* Backward solve U: only process indices in u_reach (sorted descending) */
    for (int i = 0; i < u_reach_nnz; i++) {
        int j = u_reach[i];

        /* Use cached diagonal for speed (avoids O(nnz_col) search) */
        double diag = lu->U_diag[j];

        if (fabs(diag) < RALPH_PIVOT_TOL) {
            work2[j] = 0.0;
            continue;
        }

        work2[j] = work[j] / diag;
        double xj = work2[j];

        if (fabs(xj) > RALPH_ZERO_TOL) {
            /* Update predecessors */
            for (int p = lu->U_colptr[j]; p < lu->U_colptr[j + 1]; p++) {
                int r = lu->U_rowidx[p];
                if (r < j) {
                    work[r] -= lu->U_values[p] * xj;
                }
            }
        }
    }

    /* Clear work for non-reached indices to avoid stale values */
    for (int i = 0; i < reach_nnz; i++) {
        work[reach[i]] = 0.0;
    }

    /* Apply updates */
    (void)lu_update_backend_apply_forward(lu, work2);

    /* Apply column permutation */
    for (int i = 0; i < m; i++) {
        solution[lu->col_perm[i]] = work2[i];
    }
}

/*
 * Sparse BTRAN: Solve B'x = b where b is sparse
 *
 * Used for computing dual prices (pi = c_B * B^{-1}).
 * Exploits sparsity of the cost vector.
 *
 * For PAQ = LU, we have B' = QU'L'P
 * So B'^{-1}b = P'L'^{-1}U'^{-1}Q'b
 */
void lu_solve_transpose_sparse(const LUFactorization *lu,
                               int nnz_rhs, const int *rhs_idx, const double *rhs_val,
                               double *solution) {
    if (!lu || !solution) return;

    int m = lu->m;

    /* Use pre-allocated workspace (avoids malloc in hot path) */
    double *work = lu->hs_work1;
    double *work2 = lu->hs_work2;

    /* Clear workspace */
    memset(work, 0, m * sizeof(double));

    /* If RHS is too dense, use direct dense solve path
     * (must inline to avoid aliasing issues with workspace)
     * Threshold: use sparse path for RHS with <= 25% density (m/4)
     */
    if (nnz_rhs > m / 4) {
        /* Build dense RHS vector in work */
        for (int k = 0; k < nnz_rhs; k++) {
            if (rhs_idx[k] >= 0 && rhs_idx[k] < m)
                work[rhs_idx[k]] = rhs_val[k];
        }

        /* Inline the dense transpose solve */
        /* Apply inverse column permutation: work2[i] = work[col_perm[i]] */
        for (int i = 0; i < m; i++) {
            work2[i] = work[lu->col_perm[i]];
        }

        /* Apply updates in reverse (in step coordinates) */
        lu_update_backend_apply_backward(lu, work2);

        /* Solve U'z = work2 */
        solve_Ut(lu, work2, work);

        /* Solve L'x = z and apply P' */
        solve_Lt(lu, work, work2);

        vec_copy_data(solution, work2, m);
        return;
    }

    /* Sparse path: Apply inverse column permutation */
    for (int k = 0; k < nnz_rhs; k++) {
        int orig_idx = rhs_idx[k];
        if (orig_idx >= 0 && orig_idx < m) {
            /* Find which step position this original index maps to */
            int step_pos = lu->col_perm_inv[orig_idx];
            work[step_pos] = rhs_val[k];
        }
    }

    /* Apply updates in reverse */
    lu_update_backend_apply_backward(lu, work);

    /* Solve U'z = work (U' is lower triangular) */
    solve_Ut(lu, work, work2);

    /* Solve L'x = z and apply P' */
    solve_Lt(lu, work2, solution);
}

/* ============================================================================
 * Hyper-Sparse Triangular Solves with Reach Computation
 * ============================================================================
 *
 * For very sparse RHS vectors (e.g., single column of A), we can dramatically
 * reduce work by only computing entries in the "reach" of the nonzero pattern.
 *
 * Reach of b in L: all rows i such that x[i] may be nonzero after solving Lx=b
 * This is computed via DFS on the graph where j -> i if L[i,j] != 0.
 *
 * Key insight: For a column a_j with k nonzeros, the reach typically has
 * O(k * avg_L_col_nnz) entries, much smaller than m.
 */

/*
 * Compute reach of sparse RHS through lower triangular L using DFS.
 * Returns indices in topological order (increasing for lower triangular).
 *
 * reach_out: output array of size m (will contain reached indices)
 * reach_nnz: output count of reached indices
 * marked: workspace array of size m (will be modified)
 *
 * OPTIMIZED: Collects visited indices during DFS, then sorts only those
 * instead of scanning all m indices. This makes the function O(reach) instead of O(m).
 */
static void compute_reach_L(const LUFactorization *lu,
                            int nnz_rhs, const int *rhs_idx,
                            int *reach_out, int *reach_nnz,
                            int *marked) {
    int m = lu->m;
    *reach_nnz = 0;

    /* For lower triangular L, topological order is ascending indices.
     * Strategy: mark reachable indices via DFS, then scan 0..m-1 to collect.
     * This avoids sorting since we collect in ascending order naturally.
     */

    /* Track min/max reached for efficient scan bounds */
    int min_reached = m, max_reached = -1;

    /* Stack-based DFS to mark all reachable indices */
    int *stack = lu->hs_stack;  /* Dedicated stack workspace */
    int stack_top;

    for (int k = 0; k < nnz_rhs; k++) {
        int start = rhs_idx[k];
        if (start < 0 || start >= m || marked[start]) continue;

        stack_top = 0;
        stack[stack_top++] = start;
        marked[start] = 1;
        if (start < min_reached) min_reached = start;
        if (start > max_reached) max_reached = start;

        while (stack_top > 0) {
            int j = stack[--stack_top];

            /* Visit all unvisited children (L[i,j] != 0 means i > j) */
            for (int p = lu->L_colptr[j] + 1; p < lu->L_colptr[j + 1]; p++) {
                int i = lu->L_rowidx[p];
                if (marked[i] == 0) {
                    marked[i] = 1;
                    stack[stack_top++] = i;
                    if (i > max_reached) max_reached = i;
                }
            }
        }
    }

    /* Collect marked indices in ascending order (no sorting needed) */
    for (int j = min_reached; j <= max_reached; j++) {
        if (marked[j]) {
            reach_out[(*reach_nnz)++] = j;
            marked[j] = 0;  /* Clear as we go */
        }
    }
}

/*
 * Compute reach of sparse RHS through upper triangular U using DFS.
 * Returns indices in reverse topological order (decreasing for upper triangular).
 *
 * OPTIMIZED: Uses mark + descending scan instead of collect + sort.
 * For upper triangular U, topological order is descending indices.
 * Strategy: mark reachable via DFS, then scan max..min to collect in order.
 */
static void compute_reach_U(const LUFactorization *lu,
                            int nnz_rhs, const int *rhs_idx,
                            int *reach_out, int *reach_nnz,
                            int *marked) {
    int m = lu->m;
    *reach_nnz = 0;

    /* For U, we need to find rows affected by nonzeros.
     * U is upper triangular: U[i,j] != 0 for i <= j.
     * If x[j] is nonzero and U[i,j] != 0, then x[i] is affected.
     * So we go from j to i where i < j.
     */

    /* Track min/max reached for efficient scan bounds */
    int min_reached = m, max_reached = -1;

    int *stack = lu->hs_stack;  /* Dedicated stack workspace */
    int stack_top;

    for (int k = 0; k < nnz_rhs; k++) {
        int start = rhs_idx[k];
        if (start < 0 || start >= m || marked[start]) continue;

        stack_top = 0;
        stack[stack_top++] = start;
        marked[start] = 1;
        if (start < min_reached) min_reached = start;
        if (start > max_reached) max_reached = start;

        while (stack_top > 0) {
            int j = stack[--stack_top];

            /* Visit all unvisited predecessors (row i < j where U[i,j] != 0) */
            for (int p = lu->U_colptr[j]; p < lu->U_colptr[j + 1]; p++) {
                int i = lu->U_rowidx[p];
                if (i < j && marked[i] == 0) {
                    marked[i] = 1;
                    stack[stack_top++] = i;
                    if (i < min_reached) min_reached = i;
                }
            }
        }
    }

    /* Collect marked indices in DESCENDING order (no sorting needed) */
    for (int j = max_reached; j >= min_reached; j--) {
        if (marked[j]) {
            reach_out[(*reach_nnz)++] = j;
            marked[j] = 0;  /* Clear as we go */
        }
    }
}

/*
 * Sparse forward solve: Lx = b where b and x are sparse.
 *
 * NOTE: b_idx and x_idx MUST NOT alias. The caller must ensure this.
 *
 * Input:
 *   nnz_b, b_idx, b_val: sparse RHS (in permuted coordinates)
 *   x: dense output vector (will be cleared for reach indices)
 *   marked: workspace of size m
 *
 * Output:
 *   x: solution values at reach indices
 *   x_idx, x_nnz: sparse representation of result
 *   reach_nnz_out: total reach size (for cleanup)
 */
static void solve_L_sparse(const LUFactorization *lu,
                           int nnz_b, const int *b_idx, const double *b_val,
                           double *x,
                           int *x_idx, int *x_nnz,
                           int *marked,
                           int *reach_nnz_out) {
    int m = lu->m;
    const int *L_colptr = lu->L_colptr;
    const int *L_rowidx = lu->L_rowidx;
    const double *L_values = lu->L_values;

    /* Compute reach - stored in x_idx */
    int *reach = x_idx;
    int reach_nnz;
    compute_reach_L(lu, nnz_b, b_idx, reach, &reach_nnz, marked);

    /* Clear x for all reach indices (critical for correctness!) */
    for (int k = 0; k < reach_nnz; k++) {
        x[reach[k]] = 0.0;
    }

    /* Initialize x with RHS values (b_idx must not alias x_idx!) */
    for (int k = 0; k < nnz_b; k++) {
        int j = b_idx[k];
        if (j >= 0 && j < m) {
            x[j] = b_val[k];
        }
    }

    /* Forward substitution only on reach (already in topological order) */
    for (int k = 0; k < reach_nnz; k++) {
        int j = reach[k];
        double xj = x[j];  /* L[j,j] = 1, so no division needed */

        if (fabs(xj) <= RALPH_ZERO_TOL) continue;

        /* Update successors */
        int p0 = L_colptr[j] + 1;
        int p1 = L_colptr[j + 1];
        tri_scatter_sub(L_rowidx + p0, L_values + p0, p1 - p0, xj, x);
    }

    /* Return reach size for caller to use for cleanup */
    if (reach_nnz_out) *reach_nnz_out = reach_nnz;

    /* Build sparse output (indices already in reach) */
    *x_nnz = 0;
    for (int k = 0; k < reach_nnz; k++) {
        int j = reach[k];
        if (fabs(x[j]) > RALPH_ZERO_TOL) {
            x_idx[*x_nnz] = j;
            (*x_nnz)++;
        }
    }
}

/*
 * Sparse backward solve: Ux = b where b and x are sparse.
 *
 * NOTE: b_idx and x_idx MUST NOT alias. The caller must ensure this.
 */
static void solve_U_sparse(const LUFactorization *lu,
                           int nnz_b, const int *b_idx, const double *b_val,
                           double *x,
                           int *x_idx, int *x_nnz,
                           int *marked,
                           int *reach_nnz_out) {
    int m = lu->m;
    const int *U_colptr = lu->U_colptr;
    const int *U_rowidx = lu->U_rowidx;
    const double *U_values = lu->U_values;

    /* Compute reach */
    int *reach = x_idx;
    int reach_nnz;
    compute_reach_U(lu, nnz_b, b_idx, reach, &reach_nnz, marked);

    /* Clear x for all reach indices (critical for correctness!) */
    for (int k = 0; k < reach_nnz; k++) {
        x[reach[k]] = 0.0;
    }

    /* Initialize x with RHS values */
    for (int k = 0; k < nnz_b; k++) {
        int j = b_idx[k];
        if (j >= 0 && j < m) {
            x[j] = b_val[k];
        }
    }

    /* Backward substitution on reach (in decreasing order) */
    for (int k = 0; k < reach_nnz; k++) {
        int j = reach[k];

        /* Use cached diagonal for speed (avoids O(nnz_col) search) */
        double diag = lu->U_diag[j];

        if (fabs(diag) < RALPH_PIVOT_TOL) {
            x[j] = 0.0;
            continue;
        }

        x[j] /= diag;
        double xj = x[j];

        if (fabs(xj) <= RALPH_ZERO_TOL) continue;

        /* Update predecessors */
        int p0 = U_colptr[j];
        int p1 = U_colptr[j + 1];
        tri_scatter_sub_lt(U_rowidx + p0, U_values + p0, p1 - p0, j, xj, x);
    }

    /* Return reach size for caller to use for cleanup */
    if (reach_nnz_out) *reach_nnz_out = reach_nnz;

    /* Build sparse output */
    *x_nnz = 0;
    for (int k = 0; k < reach_nnz; k++) {
        int j = reach[k];
        if (fabs(x[j]) > RALPH_ZERO_TOL) {
            x_idx[*x_nnz] = j;
            (*x_nnz)++;
        }
    }
}

/*
 * Hyper-sparse FTRAN: Solve Bx = b where b is very sparse.
 *
 * Returns result in both dense (solution) and sparse (sol_idx, sol_nnz) form.
 * Uses reach computation to minimize work.
 *
 * For a typical LP pivot column with 5-10 nonzeros, this can be 10-100x
 * faster than dense solve.
 */
void lu_ftran_hyper_sparse(const LUFactorization *lu,
                           int nnz_rhs, const int *rhs_idx, const double *rhs_val,
                           double *solution,
                           int *sol_idx, int *sol_nnz) {
    if (!lu || !solution) return;

    int m = lu->m;

    /* Threshold: if RHS too dense, fall back to regular sparse solve */
    if (nnz_rhs > m / 8) {
        /* Use existing sparse solve */
        lu_solve_sparse(lu, nnz_rhs, rhs_idx, rhs_val, solution);
        /* Build sparse output by scanning */
        if (sol_idx && sol_nnz) {
            *sol_nnz = 0;
            for (int j = 0; j < m; j++) {
                if (fabs(solution[j]) > RALPH_ZERO_TOL) {
                    sol_idx[(*sol_nnz)++] = j;
                }
            }
        }
        return;
    }

    /* Use pre-allocated workspace (avoid malloc in hot path) */
    /* Cast away const for workspace access - workspace is logically mutable */
    LUFactorization *lu_mut = (LUFactorization*)lu;
    double *work = lu_mut->hs_work1;
    double *work2 = lu_mut->hs_work2;
    int *marked = lu_mut->hs_marked;
    int *perm_rhs_idx = lu_mut->hs_idx;   /* Permuted RHS indices (input to L solve) */
    int *L_out_idx = (int*)lu_mut->perm_work;  /* L solve output indices (separate!) */
    double *temp_val = lu_mut->hs_val;

    /* Clear work2 - may have stale values from lu_update or previous calls */
    memset(work2, 0, m * sizeof(double));

    /* Step 1: Apply row permutation to RHS */
    int perm_nnz = 0;
    for (int k = 0; k < nnz_rhs; k++) {
        int orig_row = rhs_idx[k];
        if (orig_row >= 0 && orig_row < m) {
            perm_rhs_idx[perm_nnz] = lu->perm_inv[orig_row];
            temp_val[perm_nnz] = rhs_val[k];
            perm_nnz++;
        }
    }

    /* Step 2: Sparse L solve
     * Input: perm_rhs_idx, temp_val (no aliasing with L_out_idx) */
    int L_nnz, L_reach_nnz;
    solve_L_sparse(lu, perm_nnz, perm_rhs_idx, temp_val, work, L_out_idx, &L_nnz, marked, &L_reach_nnz);

    /* Step 3: Sparse U solve */
    /* Gather values for U solve - L_out_idx has L solve nonzero indices */
    for (int k = 0; k < L_nnz; k++) {
        temp_val[k] = work[L_out_idx[k]];
    }

    /* U solve: L_out_idx is INPUT, reuse perm_rhs_idx as OUTPUT (safe now) */
    int U_nnz, U_reach_nnz;
    solve_U_sparse(lu, L_nnz, L_out_idx, temp_val, work2, perm_rhs_idx, &U_nnz, marked, &U_reach_nnz);

    /* Step 4: Apply FT/eta updates */
    int has_updates = 0;
    if ((lu_update_backend_is_ft(lu) && lu->ft_num_updates > 0) ||
        (lu->update_backend == LU_UPDATE_BACKEND_BG_COMPAT && lu->schur_num_updates > 0) ||
        (lu->update_backend == LU_UPDATE_BACKEND_GR_COMPAT && lu->schur_num_updates > 0) ||
        (!lu_update_backend_is_ft(lu) &&
         lu->update_backend != LU_UPDATE_BACKEND_BG_COMPAT &&
         lu->update_backend != LU_UPDATE_BACKEND_GR_COMPAT &&
         lu->num_eta > 0)) {
        has_updates = 1;
        (void)lu_update_backend_apply_forward(lu, work2);
    }

    /* Step 5: Apply column permutation and build output.
     * No-update fast path avoids an O(m) scan of work2. */
    memset(solution, 0, m * sizeof(double));
    if (sol_nnz) *sol_nnz = 0;

    if (!has_updates) {
        for (int k = 0; k < U_nnz; k++) {
            int i = perm_rhs_idx[k];
            double xi = work2[i];
            if (fabs(xi) <= RALPH_ZERO_TOL) continue;
            int out_idx = lu->col_perm[i];
            solution[out_idx] = xi;
            if (sol_idx && sol_nnz) {
                sol_idx[(*sol_nnz)++] = out_idx;
            }
        }
        return;
    }

    for (int i = 0; i < m; i++) {
        double xi = work2[i];
        if (fabs(xi) <= RALPH_ZERO_TOL) continue;
        int out_idx = lu->col_perm[i];
        solution[out_idx] = xi;
        if (sol_idx && sol_nnz) {
            sol_idx[(*sol_nnz)++] = out_idx;
        }
    }
}

/* ============================================================================
 * Sparse BTRAN Support (W1: Sparse BTRAN for LP speedup)
 *
 * Build CSR transposes of L and U for efficient DFS reach computation
 * on L^T and U^T. This enables sparse forward/backward substitution
 * on the transpose system, reducing BTRAN from O(m²) to O(reach).
 * ============================================================================ */

/*
 * Mark L/U CSC data as valid for sparse BTRAN reach computation.
 * The reach functions (compute_reach_Ut_forward, compute_reach_Lt_backward)
 * use the CSC of U and L directly — no separate CSR transpose is needed.
 * Called after every refactorization.
 */
static void build_csr_transpose(LUFactorization *lu) {
    lu->csr_valid = 1;
}

/*
 * Compute reach of sparse RHS through U^T (lower triangular) using DFS.
 * U^T is lower triangular: U^T[i,j] = U[j,i] exists for j <= i.
 * From index j, successors are all i > j where U^T[i,j] != 0.
 * In CSR for U^T: Ut_rowptr[j] gives entries where U^T[j,col] != 0,
 * i.e. the columns col that have U[col,j] != 0.
 *
 * But we want: given RHS nonzero at j, who gets affected?
 * U^T x = b: forward sub, ascending j. x[j] depends on x[i] for i < j.
 * Reach: all j reachable from RHS by following U^T edges downward.
 * U^T[i,j] != 0 for j >= i (U^T is lower triangular).
 * Edge from source i: go to rows j > i where U^T[j,i] != 0, i.e. U[i,j] != 0.
 * In CSR of U^T: row i has columns col where U^T[i,col] != 0.
 * But we need rows j where U^T[j,i] != 0 — that's column i of U^T = row i of U.
 * In CSC of U: column i has entries at rows j where U[j,i] != 0.
 * For upper triangular U: j <= i. So U^T[i,j] != 0 means j <= i (i >= j).
 *
 * Actually for forward sub on U^T (lower tri):
 * x[i] = (b[i] - Σ_{j<i} U^T[i,j] * x[j]) / U^T[i,i]
 * So x[i] depends on x[j] for j < i where U^T[i,j] != 0.
 * If x[j] is nonzero, it can make x[i] nonzero for all i > j with U^T[i,j] != 0.
 * U^T[i,j] = U[j,i]. For j < i: U[j,i] is in column i of U, row j.
 *
 * Reach from source j: find all i where U^T[i,j] != 0 and i > j.
 * This is: find all i where U[j,i] != 0 and i > j.
 * In CSR of U^T: row j has columns that equal row indices of col j in U.
 * We need the CSR row j of U: entries at columns i where U[j,i] != 0.
 * Ut_rowptr[j]..Ut_rowptr[j+1] gives us columns col where U^T[j,col] != 0,
 * i.e. U[col,j] != 0 (col <= j since U is upper tri).
 * That's the WRONG direction — those are predecessors, not successors!
 *
 * For successors of j in U^T forward sub:
 * We need rows i > j where U^T[i,j] != 0 = U[j,i] != 0.
 * U[j,i] != 0 means entry in row j, column i of U.
 * In CSC of U: column i has rowidx entries, look for row j.
 * That's O(nnz) per query — too expensive.
 *
 * Alternative: use CSR of U directly (= CSC of U^T).
 * U CSR: row j has columns i where U[j,i] != 0, with i >= j (upper tri).
 * Successors of j in U^T: all i > j where U[j,i] != 0.
 * Perfect! Ut_rowptr/Ut_colidx give us row j of U^T, but we need
 * "who does j feed into" = columns i > j in row j of U.
 *
 * Wait — Ut_rowptr[j] gives ROW j of U^T. Entries are columns col
 * where U^T[j,col] != 0 = U[col,j] != 0. For upper tri U: col <= j.
 * These are predecessors of j, not successors.
 *
 * We actually want the CSR of U (NOT U^T) for forward traversal on U^T.
 * CSR of U = CSC of U^T. Row j of U has columns i >= j.
 * Successors of j in U^T forward sub: columns i > j in row j of U.
 *
 * So build_csr_transpose should build CSR of U for this purpose.
 * But we already built CSR of U^T. Let me reconsider...
 *
 * Actually, let's just use the CSC of U directly:
 * For U^T forward sub, reach from j: find all columns i > j where U[j,i] != 0.
 * CSC of U: scan columns i > j, check if row j appears.
 * This is O(m * avg_col_nnz) in worst case — not better than dense.
 *
 * The correct approach is to build CSR of U (row-oriented U, not U^T).
 * Row j of U: columns i >= j. Successors: all i > j in that row.
 * This IS what we need. Let me rename: Ut_rowptr is actually U_rowptr (CSR of U).
 *
 * OK, I realize the naming was confusing. Let me redefine:
 * - For sparse BTRAN on U^T (forward sub, ascending):
 *   Need CSR of U (= row-oriented U) to find successors
 * - For sparse BTRAN on L^T (backward sub, descending):
 *   Need CSR of L (= row-oriented L) to find predecessors
 *
 * But we already built CSR of U^T and L^T above. Let me fix this.
 * Actually, the CSR of U^T = transposed CSC of U, gives us:
 * Row i of U^T: columns j where U^T[i,j] != 0, i.e. U[j,i] != 0.
 * For upper tri U: j <= i. So these are entries above diagonal in U.
 * These tell us: x[i] depends on x[j] for j < i — PREDECESSORS.
 *
 * For DFS reach, we need both directions. Actually for topological ordering,
 * we can DFS from sources following predecessors then reverse.
 * But the simpler approach: mark + ascending scan (like compute_reach_L).
 *
 * Simplest correct approach: DFS following successors.
 * Successors of j: all i > j affected by x[j] being nonzero.
 * U^T[i,j] != 0 ⟺ U[j,i] != 0.
 * Need: for each j, find all i > j with U[j,i] != 0.
 * This requires CSR of U (row j → columns i ≥ j).
 *
 * So let's rebuild: Ut_rowptr/Ut_colidx = CSR of U (not U^T).
 * Similarly, Lt_rowptr/Lt_colidx = CSR of L (not L^T).
 *
 * Actually, what we built IS CSR of U^T and L^T. Let's just use a different
 * approach for the reach: mark-and-sweep with the CSC of U directly.
 */

/*
 * Compute reach for sparse forward sub on U^T (lower triangular).
 * Returns indices in ascending topological order.
 *
 * U^T forward sub: x[j] = (b[j] - Σ_{r<j} U^T[j,r]*x[r]) / U^T[j,j]
 * If x[r] is nonzero, it feeds into x[j] for all j > r where U^T[j,r] != 0.
 * U^T[j,r] = U[r,j] — entry in column j of U at row r (upper tri: r <= j).
 *
 * Ascending mark propagation: for each column j, if any marked r < j has
 * U[r,j] != 0, then j gets marked.
 */
static void compute_reach_Ut_forward(const LUFactorization *lu,
                                      int nnz_rhs, const int *rhs_idx,
                                      int *reach_out, int *reach_nnz,
                                      int *marked) {
    int m = lu->m;
    *reach_nnz = 0;
    if (nnz_rhs <= 0) return;

    /* Mark all RHS indices */
    int min_rhs = m;
    for (int k = 0; k < nnz_rhs; k++) {
        int j = rhs_idx[k];
        if (j >= 0 && j < m) {
            marked[j] = 1;
            if (j < min_rhs) min_rhs = j;
        }
    }
    if (min_rhs >= m) return;

    /* Ascending propagation using CSC of U:
     * Column j of U has entries U[r,j] for r <= j.
     * If any r < j is marked, mark j (nonzero propagates forward). */
    for (int j = min_rhs; j < m; j++) {
        if (!marked[j]) {
            for (int p = lu->U_colptr[j]; p < lu->U_colptr[j + 1]; p++) {
                int r = lu->U_rowidx[p];
                if (r < j && marked[r]) {
                    marked[j] = 1;
                    break;
                }
            }
        }
        if (marked[j]) {
            reach_out[(*reach_nnz)++] = j;
        }
    }

    /* Clear marks */
    for (int k = 0; k < *reach_nnz; k++) {
        marked[reach_out[k]] = 0;
    }
}

/*
 * Compute reach for sparse backward sub on L^T (upper triangular).
 * Returns indices in descending topological order.
 *
 * L^T backward sub: x[j] = b[j] - Σ_{r>j} L^T[j,r]*x[r], descending j.
 * If x[r] is nonzero, it feeds into x[j] for all j < r where L^T[j,r] != 0.
 * L^T[j,r] = L[r,j] — entry in column j of L at row r (lower tri: r >= j).
 *
 * Descending mark propagation: for each column j, if any marked r > j has
 * L[r,j] != 0 (below diagonal of L), then j gets marked.
 */
static void compute_reach_Lt_backward(const LUFactorization *lu,
                                       int nnz_rhs, const int *rhs_idx,
                                       int *reach_out, int *reach_nnz,
                                       int *marked) {
    int m = lu->m;
    *reach_nnz = 0;
    if (nnz_rhs <= 0) return;

    /* Mark all RHS indices */
    int max_rhs = -1;
    for (int k = 0; k < nnz_rhs; k++) {
        int j = rhs_idx[k];
        if (j >= 0 && j < m) {
            marked[j] = 1;
            if (j > max_rhs) max_rhs = j;
        }
    }
    if (max_rhs < 0) return;

    /* Descending propagation using CSC of L:
     * Column j of L has entries L[r,j] for r > j (below diagonal).
     * If any r > j is marked, mark j (nonzero propagates backward). */
    for (int j = max_rhs; j >= 0; j--) {
        if (!marked[j]) {
            for (int p = lu->L_colptr[j] + 1; p < lu->L_colptr[j + 1]; p++) {
                int r = lu->L_rowidx[p];
                if (marked[r]) {
                    marked[j] = 1;
                    break;
                }
            }
        }
        if (marked[j]) {
            reach_out[(*reach_nnz)++] = j;  /* Collected in descending order */
        }
    }

    /* Clear marks */
    for (int k = 0; k < *reach_nnz; k++) {
        marked[reach_out[k]] = 0;
    }
}

/*
 * Sparse forward sub on U^T: solve U^T x = b, restricted to reach indices.
 * x is a dense workspace (input b, output x in-place).
 * Reach must be in ascending order.
 *
 * U^T[i,j] = U[j,i]. For forward sub (ascending i):
 * x[i] = (b[i] - Σ_{j<i} U^T[i,j] * x[j]) / U^T[i,i]
 *       = (b[i] - Σ_{j<i} U[j,i] * x[j]) / U[i,i]
 *
 * In CSC of U: column i has entries U[r,i] for r <= i.
 * U^T[i,r] = U[r,i] for r < i — these are the contributions.
 */
static void solve_Ut_sparse_reach(const LUFactorization *lu,
                                   int reach_nnz, const int *reach,
                                   double *x, const int *reach_mask,
                                   int use_reach_mask) {
    const int *U_colptr = lu->U_colptr;
    const int *U_rowidx = lu->U_rowidx;
    const double *U_values = lu->U_values;
    const double *U_diag = lu->U_diag;

    if (use_reach_mask) {
        for (int k = 0; k < reach_nnz; k++) {
            int i = reach[k];
            int p0 = U_colptr[i];
            int p1 = U_colptr[i + 1];
            double sum = tri_dot_lt_masked(U_rowidx + p0, U_values + p0, p1 - p0,
                                           i, x, reach_mask);

            double diag = U_diag[i];
            if (fabs(diag) < RALPH_PIVOT_TOL) {
                x[i] = 0.0;
            } else {
                x[i] = (x[i] - sum) / diag;
            }
        }
        return;
    }

    for (int k = 0; k < reach_nnz; k++) {
        int i = reach[k];
        int p0 = U_colptr[i];
        int p1 = U_colptr[i + 1];
        double sum = tri_dot_lt(U_rowidx + p0, U_values + p0, p1 - p0, i, x);

        double diag = U_diag[i];
        if (fabs(diag) < RALPH_PIVOT_TOL) {
            x[i] = 0.0;
        } else {
            x[i] = (x[i] - sum) / diag;
        }
    }
}

/*
 * Sparse backward sub on L^T: solve L^T x = b, restricted to reach indices.
 * Reach must be in descending order. Includes inverse row permutation.
 *
 * L^T[j,i] = L[i,j]. For backward sub (descending j):
 * x[j] = b[j] - Σ_{i>j} L^T[j,i] * x[i]
 *       = b[j] - Σ_{i>j} L[i,j] * x[i]
 * L[j,j] = 1, so no division needed.
 *
 * In CSC of L: column j has entries L[i,j] for i > j (below diagonal).
 * These are exactly L^T[j,i] = L[i,j] for the backward sub sum.
 */
static void solve_Lt_sparse_reach(const LUFactorization *lu,
                                   int reach_nnz, const int *reach,
                                   double *x, double *solution,
                                   int *sol_idx, int *sol_nnz,
                                   const int *reach_mask,
                                   int use_reach_mask) {
    const int *L_colptr = lu->L_colptr;
    const int *L_rowidx = lu->L_rowidx;
    const double *L_values = lu->L_values;

    if (use_reach_mask) {
        for (int k = 0; k < reach_nnz; k++) {
            int j = reach[k];  /* Descending order */
            int p0 = L_colptr[j] + 1;
            int p1 = L_colptr[j + 1];
            double sum = tri_dot_masked(L_rowidx + p0, L_values + p0, p1 - p0, x, reach_mask);
            x[j] -= sum;
        }
    } else {
        for (int k = 0; k < reach_nnz; k++) {
            int j = reach[k];  /* Descending order */
            int p0 = L_colptr[j] + 1;
            int p1 = L_colptr[j + 1];
            double sum = tri_dot(L_rowidx + p0, L_values + p0, p1 - p0, x);
            x[j] -= sum;
            /* L[j,j] = 1, so no division needed */
        }
    }

    /* Apply inverse row permutation on the reached subset only. */
    int out_nnz = 0;
    for (int k = 0; k < reach_nnz; k++) {
        int i = reach[k];
        double xi = x[i];
        if (fabs(xi) <= RALPH_ZERO_TOL) continue;
        int out = lu->perm[i];
        solution[out] = xi;
        if (sol_idx) sol_idx[out_nnz] = out;
        out_nnz++;
    }
    if (sol_nnz) *sol_nnz = out_nnz;
}

/*
 * Hyper-sparse BTRAN: Solve B'x = b where b is very sparse.
 *
 * Used for computing dual prices when cost vector is sparse.
 */
void lu_btran_hyper_sparse(const LUFactorization *lu,
                           int nnz_rhs, const int *rhs_idx, const double *rhs_val,
                           double *solution,
                           int *sol_idx, int *sol_nnz) {
    if (!lu || !solution) return;

    int m = lu->m;

    /* Threshold for falling back to dense */
    if (nnz_rhs > m / 8) {
        lu_solve_transpose_sparse(lu, nnz_rhs, rhs_idx, rhs_val, solution);
        if (sol_idx && sol_nnz) {
            *sol_nnz = 0;
            for (int j = 0; j < m; j++) {
                if (fabs(solution[j]) > RALPH_ZERO_TOL) {
                    sol_idx[(*sol_nnz)++] = j;
                }
            }
        }
        return;
    }

    /* For transpose solve, the operations are reversed:
     * B' = Q U' L' P
     * B'^{-1} = P' L'^{-1} U'^{-1} Q'
     *
     * 1. Apply Q' (inverse column perm)
     * 2. Apply eta/FT updates in reverse
     * 3. Solve U'^{-1} (forward sub on U')
     * 4. Solve L'^{-1} (backward sub on L')
     * 5. Apply P'
     */

    /* Use pre-allocated workspace - need to cast away const for workspace access */
    LUFactorization *lu_mut = (LUFactorization*)lu;
    double *work = lu_mut->hs_work1;
    double *work2 = lu_mut->hs_work2;
    int *bt_idx = lu_mut->hs_idx;

    /* Step 1: Apply inverse column permutation */
    int bt_nnz = 0;
    for (int k = 0; k < nnz_rhs; k++) {
        int orig_idx = rhs_idx[k];
        if (orig_idx >= 0 && orig_idx < m) {
            int step_pos = lu->col_perm_inv[orig_idx];
            double v = rhs_val[k];
            work[step_pos] = v;
            if (fabs(v) > RALPH_ZERO_TOL) {
                bt_idx[bt_nnz++] = step_pos;
            }
        }
    }

    /* Step 2: Apply updates in reverse */
    int has_updates = 0;
    if ((lu_update_backend_is_ft(lu) && lu->ft_num_updates > 0) ||
        (lu->update_backend == LU_UPDATE_BACKEND_BG_COMPAT && lu->schur_num_updates > 0) ||
        (lu->update_backend == LU_UPDATE_BACKEND_GR_COMPAT && lu->schur_num_updates > 0) ||
        (!lu_update_backend_is_ft(lu) &&
         lu->update_backend != LU_UPDATE_BACKEND_BG_COMPAT &&
         lu->update_backend != LU_UPDATE_BACKEND_GR_COMPAT &&
         lu->num_eta > 0)) {
        has_updates = 1;
        lu_update_backend_apply_backward(lu, work);
    }

    /* Step 3 & 4: Solve U'^{-1} and L'^{-1}
     * W1: Use sparse reach-based solve when CSR transposes are available
     * and the post-update RHS is sparse enough. */

    /* If updates were applied, rebuild sparse RHS pattern (updates may densify). */
    if (has_updates) {
        bt_nnz = 0;
        for (int j = 0; j < m; j++) {
            if (fabs(work[j]) > RALPH_ZERO_TOL) {
                bt_idx[bt_nnz++] = j;
            }
        }
    }

    int used_sparse_path = 0;
    if (lu->csr_valid && bt_nnz < m / 4) {
        /* Sparse path: reach-based forward sub on U^T, then backward sub on L^T */
        int *reach = (int*)lu_mut->perm_work;  /* Reuse as int array */
        int *reach_mark = lu_mut->hs_marked;
        int reach_nnz;
        used_sparse_path = 1;

        /* Forward sub on U^T: solve U^T work = work (in-place) */
        compute_reach_Ut_forward(lu, bt_nnz, bt_idx, reach, &reach_nnz, lu_mut->hs_marked);
        int use_reach_mask_ut = (reach_nnz > 0 &&
                                 bt_nnz > 0 &&
                                 reach_nnz <= 2 * bt_nnz &&
                                 reach_nnz < m / 2);
        if (use_reach_mask_ut) {
            for (int k = 0; k < reach_nnz; k++) {
                reach_mark[reach[k]] = 1;
            }
        }
        solve_Ut_sparse_reach(lu, reach_nnz, reach, work, reach_mark, use_reach_mask_ut);
        if (use_reach_mask_ut) {
            for (int k = 0; k < reach_nnz; k++) {
                reach_mark[reach[k]] = 0;
            }
        }

        /* Gather nonzeros after U^T solve for L^T reach */
        int ut_nnz = 0;
        for (int k = 0; k < reach_nnz; k++) {
            if (fabs(work[reach[k]]) > RALPH_ZERO_TOL) {
                bt_idx[ut_nnz++] = reach[k];
            }
        }

        /* Backward sub on L^T in-place on work, then permute to solution */
        compute_reach_Lt_backward(lu, ut_nnz, bt_idx, reach, &reach_nnz, lu_mut->hs_marked);
        memset(solution, 0, m * sizeof(double));
        int use_reach_mask_lt = (reach_nnz > 0 &&
                                 ut_nnz > 0 &&
                                 reach_nnz <= 2 * ut_nnz &&
                                 reach_nnz < m / 2);
        if (use_reach_mask_lt) {
            for (int k = 0; k < reach_nnz; k++) {
                reach_mark[reach[k]] = 1;
            }
        }
        solve_Lt_sparse_reach(lu, reach_nnz, reach, work, solution, sol_idx, sol_nnz,
                              reach_mark, use_reach_mask_lt);
        if (use_reach_mask_lt) {
            for (int k = 0; k < reach_nnz; k++) {
                reach_mark[reach[k]] = 0;
            }
        }
    } else {
        /* Dense fallback */
        solve_Ut(lu, work, work2);
        solve_Lt(lu, work2, solution);
    }

    /* Build sparse output */
    if (!used_sparse_path && sol_idx && sol_nnz) {
        *sol_nnz = 0;
        for (int j = 0; j < m; j++) {
            if (fabs(solution[j]) > RALPH_ZERO_TOL) {
                sol_idx[(*sol_nnz)++] = j;
            }
        }
    }

    /* Clear workspace */
    memset(work, 0, m * sizeof(double));
    memset(work2, 0, m * sizeof(double));
}

/* ============================================================================
 * Basis Updates
 * ============================================================================
 *
 * Update application/storage is backend-owned in `lu_update_backend.c`.
 * This keeps FT, eta, and the emerging BG/GR Schur-compat lane orthogonal.
 */

/* Update factorization when basis column changes */
int lu_update(LUFactorization *lu, int leaving_pos, const double *entering_col) {
    LPBFCPRefactorSignals sig;
    int effective_update_limit;
    int storage_cap;

    if (!lu || !entering_col) {
        lu_set_failure(lu, LU_FAIL_BAD_INPUT);
        lu_mark_update_failure(lu, LU_FAIL_BAD_INPUT);
        return -1;
    }
    lu_clamp_max_updates_to_storage(lu);
    lu_set_failure(lu, LU_FAIL_NONE);
    lu_fill_bfcp_signals(lu, &sig);
    effective_update_limit = lp_bfcp_policy_effective_update_limit(&sig);
    if (effective_update_limit <= 0) {
        effective_update_limit = lu->max_updates;
    }
    if (effective_update_limit > 0 && lu->num_updates >= effective_update_limit) {
        lu->last_refactor_trigger_reason =
            (effective_update_limit < lu->max_updates)
                ? LP_BFCP_REFACTOR_REASON_COND_ADAPTIVE_LIMIT
                : LP_BFCP_REFACTOR_REASON_MAX_UPDATES;
        lu_set_failure(lu, LU_FAIL_MAX_UPDATES);
        lu_mark_update_failure(lu, LU_FAIL_MAX_UPDATES);
        return -1;  /* Need refactorization */
    }

    int m = lu->m;

    /* Convert leaving_pos to step coordinates */
    int step_pos = lu->col_perm_inv[leaving_pos];

    /* Solve for spike: s = U^{-1} * L^{-1} * P * entering_col
     * Use pre-allocated workspaces to avoid malloc in hot path */
    double *work = lu->hs_work1;
    double *spike = lu->hs_work2;

    /* Solve L * y = P * entering_col */
    solve_L(lu, entering_col, work);

    /* Solve U * spike = y */
    solve_U(lu, work, spike);

    /* Apply existing updates (FT spikes or eta matrices) */
    if (lu_update_backend_apply_forward(lu, spike) != 0) {
        return -1;
    }

    /* Check pivot element (in step coordinates) */
    if (fabs(spike[step_pos]) < RALPH_PIVOT_TOL) {
        lu_set_failure(lu, LU_FAIL_SINGULAR_UPDATE);
        lu_mark_update_failure(lu, LU_FAIL_SINGULAR_UPDATE);
        return -1;  /* Singular update */
    }

    /* Threshold pivoting for updates: check if pivot is too small relative to
     * the maximum element in the spike column.
     *
     * The ratio threshold is health-adaptive: early/healthy update runs allow
     * slightly smaller pivots to avoid unnecessary reinversions, while poor
     * cond/growth states tighten the threshold to preserve stability. */
    double max_abs_spike = fabs(spike[step_pos]);
    for (int i = 0; i < m; i++) {
        double absval = fabs(spike[i]);
        if (absval > max_abs_spike) max_abs_spike = absval;
    }
    {
        double pivot_ratio_threshold = lu_update_pivot_ratio_threshold(lu);
        if (fabs(spike[step_pos]) < pivot_ratio_threshold * max_abs_spike) {
            /* Pivot is too small relative to column magnitude.
             * Force refactorization to get a more stable basis representation. */
            lu_set_failure(lu, LU_FAIL_UPDATE_PIVOT_TOO_SMALL);
            lu_mark_update_failure(lu, LU_FAIL_UPDATE_PIVOT_TOO_SMALL);
            return -1;
        }
    }

    /* Normalize spike column and count OFF-DIAGONAL non-zeros */
    double pivot = spike[step_pos];
    double diag_val = 1.0 / pivot;
    int off_diag_nnz = 0;
    double max_spike = fabs(diag_val);

    for (int i = 0; i < m; i++) {
        if (i != step_pos) {
            spike[i] = -spike[i] / pivot;
            double absval = fabs(spike[i]);
            if (absval > max_spike) max_spike = absval;
            if (absval > RALPH_ZERO_TOL) off_diag_nnz++;
        }
    }
    spike[step_pos] = diag_val;  /* For eta-file compatibility */

    /* Dense spike guard: very dense updates make every future FTRAN/BTRAN expensive.
     * Keep an initial warmup window so updates do not immediately collapse into
     * update-fail -> reinvert loops before density policy can react. */
    if (lu_update_backend_is_ft(lu) &&
        lu->ft_num_updates >= RALPH_SPIKE_DENSE_REJECT_MIN_UPDATES &&
        m >= RALPH_SPIKE_DENSE_REJECT_M_MIN &&
        m > 1) {
        double spike_ratio = (double)off_diag_nnz / (double)(m - 1);
        double reject_ratio = lu_dense_spike_reject_ratio(lu);
        if (spike_ratio > reject_ratio) {
            lu_set_failure(lu, LU_FAIL_SPIKE_POOL_FULL);
            lu_mark_update_failure(lu, LU_FAIL_SPIKE_POOL_FULL);
            return -1;
        }
    }

    if (lu_update_backend_store(lu, step_pos, spike, off_diag_nnz) != 0) {
        return -1;
    }
    lu->num_updates++;
    storage_cap = lu_update_backend_storage_capacity(lu);
    if (storage_cap > 0 && lu->num_updates > storage_cap) {
        lu->num_updates = storage_cap;
    }

    /* Track growth factor */
    if (max_spike > lu->growth_factor) {
        lu->growth_factor = max_spike;
    }

    /* T3.2: Update condition estimate from new pivot diagonal.
     * The diagonal of the updated U matrix is approximated by 1/pivot.
     * Track min/max to estimate condition degradation during updates. */
    double abs_diag = fabs(diag_val);
    if (abs_diag > 0 && abs_diag < lu->min_diag_U) lu->min_diag_U = abs_diag;
    if (abs_diag > lu->max_diag_U) lu->max_diag_U = abs_diag;
    if (lu->min_diag_U > RALPH_ZERO_TOL)
        lu->cond_estimate = lu->max_diag_U / lu->min_diag_U;

    /* (B4: spike compaction removed — refactorization handles accumulated fill) */

    lu_set_failure(lu, LU_FAIL_NONE);
    return 0;
}

int lu_needs_refactorization(LUFactorization *lu) {
    LPBFCPRefactorSignals sig;
    int reason;

    if (!lu) return 0;

    lu_fill_bfcp_signals(lu, &sig);

    reason = lp_bfcp_policy_refactor_reason(&sig);
    lu->last_refactor_trigger_reason = reason;
    lu_mark_refactor_need(lu, reason);
    return reason != LP_BFCP_REFACTOR_REASON_NONE;
}

int lu_refactor_hard_trigger(const LUFactorization *lu) {
    LPBFCPRefactorSignals sig;

    if (!lu) return 0;
    lu_fill_bfcp_signals(lu, &sig);
    return lp_bfcp_policy_refactor_hard_trigger(&sig);
}

/* ============================================================================
 * Utility
 * ============================================================================ */

const char* lu_failure_reason_string(int reason) {
    switch ((LUFailureReason)reason) {
        case LU_FAIL_NONE: return "none";
        case LU_FAIL_BAD_INPUT: return "bad_input";
        case LU_FAIL_MAX_UPDATES: return "max_updates";
        case LU_FAIL_SINGULAR_UPDATE: return "singular_update";
        case LU_FAIL_UPDATE_PIVOT_TOO_SMALL: return "update_pivot_too_small";
        case LU_FAIL_SPIKE_POOL_FULL: return "spike_pool_full";
        case LU_FAIL_ETA_ALLOC: return "eta_alloc";
        case LU_FAIL_FACTOR_SINGULAR: return "factor_singular";
        case LU_FAIL_FACTOR_ALLOC: return "factor_alloc";
        default: return "unknown";
    }
}

const char* lu_refactor_trigger_reason_string(int reason) {
    return lp_bfcp_policy_refactor_reason_string(reason);
}

void lu_print(const LUFactorization *lu) {
    if (!lu) {
        printf("NULL LU factorization\n");
        return;
    }

    printf("LU Factorization: %d x %d\n", lu->m, lu->m);
    printf("  L: %d non-zeros\n", lu->nnz_L);
    printf("  U: %d non-zeros\n", lu->nnz_U);
    printf("  Updates: %d / %d\n", lu->num_updates, lu->max_updates);

    printf("  Row permutation: [");
    for (int i = 0; i < lu->m && i < 10; i++) {
        printf("%d ", lu->perm[i]);
    }
    printf("...]\n");
}
