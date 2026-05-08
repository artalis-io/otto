/*
 * Ralph - LP Model and Algorithm Internals
 */

#ifndef RALPH_LP_H
#define RALPH_LP_H

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "sparse.h"
#include "ralph_core.h"
#include "shared.h"
#include "lp_basis_governor.h"
#include "lp_reinvert_controller.h"

/* Software prefetch (no-op on non-GCC/Clang compilers) */
#if defined(__GNUC__) || defined(__clang__)
#define RALPH_PREFETCH(addr, rw, locality) __builtin_prefetch(addr, rw, locality)
#else
#define RALPH_PREFETCH(addr, rw, locality) ((void)0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Tolerances */
#define RALPH_FEAS_TOL 1e-6
#define RALPH_OPT_TOL 1e-6
#define RALPH_PIVOT_TOL 1e-6  /* Increased for numerical stability */
#define RALPH_ZERO_TOL 1e-12
#define RALPH_INT_TOL 1e-5

/* Numerical stabilization policy defaults */
#define RALPH_FORCE_REFACTOR_PIVOT_TOL 1e-4
#define RALPH_LU_UPDATE_PIVOT_THRESHOLD 1e-5
#define RALPH_LU_GROWTH_REFACTOR_THRESHOLD 1e8
#define RALPH_PHASE1_REPEAT_REFACTOR_TRIGGER 3
#define RALPH_PHASE1_FAIL_REPEAT_LIMIT 20
#define RALPH_PHASE1_RATIO_BREAKDOWN_LIMIT 60
#define RALPH_PHASE1_MAX_REGULARIZATIONS 32
#define RALPH_PHASE1_DUAL_RESCUE_MULT 20
#define RALPH_PHASE1_RESCUE_MAX_REFACTOR_FAILURES 64
#define RALPH_PHASE1_DIR_INF_REFACTOR_TRIGGER 1e4
#define RALPH_PHASE1_ENTERING_EXCLUDE_ITERS 8

/* Runtime policy selectors (mapped from GLPK-compatible control plane). */
#define LP_RATIO_TEST_STANDARD 0
#define LP_RATIO_TEST_HARRIS 1
/* Dual ratio-test selectors (used by dual simplex). */
#define LP_DUAL_RATIO_TEST_STANDARD 0
#define LP_DUAL_RATIO_TEST_HARRIS 1
#define LP_DUAL_RATIO_TEST_FLIP 2

#define LP_LU_BACKEND_POLICY_AUTO   -1
#define LP_LU_BACKEND_POLICY_LUF_FT 0
#define LP_LU_BACKEND_POLICY_CBG    1
#define LP_LU_BACKEND_POLICY_CGR    2

/* Reinvert-controller runtime modes (Phase 4 rollout). */
#define LP_REINVERT_CONTROLLER_MODE_OFF 0
#define LP_REINVERT_CONTROLLER_MODE_SHADOW 1
#define LP_REINVERT_CONTROLLER_MODE_CONTROL_PHASE1 2
#define LP_REINVERT_CONTROLLER_MODE_CONTROL_ALL 3

/* Refactor trigger reason telemetry codes */
typedef enum {
    RALPH_REFACTOR_REASON_OTHER = 0,
    RALPH_REFACTOR_REASON_SETUP = 1,
    RALPH_REFACTOR_REASON_PHASE_TRANSITION = 2,
    RALPH_REFACTOR_REASON_PERIODIC = 3,
    RALPH_REFACTOR_REASON_RATIO_RECOVERY = 4,
    RALPH_REFACTOR_REASON_PIVOT_RECOVERY = 5,
    RALPH_REFACTOR_REASON_FORCED_SMALL_PIVOT = 6,
    RALPH_REFACTOR_REASON_UPDATE_RECOVERY = 7,
    RALPH_REFACTOR_REASON_DIRECTION_STABILIZE = 8,
    RALPH_REFACTOR_REASON_INFEASIBILITY_CLEANUP = 9
} RalphRefactorReason;

/* NOTE: SAFE_FREE macro is provided by shared.h */

/* ============================================================================
 * Arena Allocator (Provided by shared library)
 *
 * Simple bump allocator for allocating many arrays with the same lifetime.
 * Benefits:
 * - Single malloc/free instead of 20+ individual allocations
 * - No fragmentation from repeated alloc/free cycles
 * - Simpler error handling (one check, not 20)
 * - Cache-friendly contiguous memory layout
 *
 * See shared/include/sh_arena.h for API: sh_arena_create/alloc/calloc/reset/free
 * ============================================================================ */

/* Default parameter values */
#define RALPH_DEFAULT_MAX_ITER 1000000
#define RALPH_DEFAULT_TIME_LIMIT 3600.0
#define RALPH_DEFAULT_PRESOLVE 1
#define RALPH_DEFAULT_SCALING 1

/* Variable status in basis */
typedef enum {
    RALPH_BASIC = 0,
    RALPH_NONBASIC_LOWER = 1,
    RALPH_NONBASIC_UPPER = 2,
    RALPH_NONBASIC_FREE = 3,
    RALPH_FIXED = 4
} VarStatus;

/* Forward declaration for build state (opaque) */
typedef struct LPModelBuildState LPModelBuildState;

/* LU failure reason codes (stored in LUFactorization::last_failure_reason). */
typedef enum {
    LU_FAIL_NONE = 0,
    LU_FAIL_BAD_INPUT,
    LU_FAIL_MAX_UPDATES,
    LU_FAIL_SINGULAR_UPDATE,
    LU_FAIL_UPDATE_PIVOT_TOO_SMALL,
    LU_FAIL_SPIKE_POOL_FULL,
    LU_FAIL_DENSE_SPIKE_REJECT,
    LU_FAIL_ETA_ALLOC,
    LU_FAIL_FACTOR_SINGULAR,
    LU_FAIL_FACTOR_ALLOC
} LUFailureReason;

/* LU update backend (internal execution lane for post-factorization updates). */
typedef enum {
    LU_UPDATE_BACKEND_FT = 0,
    LU_UPDATE_BACKEND_BG_COMPAT = 1,
    LU_UPDATE_BACKEND_GR_COMPAT = 2
} LUUpdateBackend;

/* Markowitz sparse numeric failure codes (internal telemetry + fallback routing). */
typedef enum {
    MKZ_FAIL_NONE = 0,
    MKZ_FAIL_WORKSPACE = -1,
    MKZ_FAIL_POOL = -2,
    MKZ_FAIL_SINGULAR = -3,
    MKZ_FAIL_CAPACITY = -4
} MKZFailureReason;

/* Sparse symbolic-stage failure codes (internal retry + telemetry routing). */
typedef enum {
    LU_SYMBOLIC_FAIL_NONE = 0,
    LU_SYMBOLIC_FAIL_WORKSPACE = -1,
    LU_SYMBOLIC_FAIL_UNMATCHED_NO_RESERVED = -2,
    LU_SYMBOLIC_FAIL_INCONSISTENT_IDENTITY = -3
} LUSymbolicFailureReason;

/* Sparse numeric-stage terminal failure codes (internal retry + telemetry routing). */
typedef enum {
    LU_SPARSE_NUMERIC_FAIL_NONE = 0,
    LU_SPARSE_NUMERIC_FAIL_IDENTITY_SEPARATION = 1,
    LU_SPARSE_NUMERIC_FAIL_BACKEND_EXHAUSTED = 2,
    LU_SPARSE_NUMERIC_FAIL_PATHOLOGICAL = 3
} LUSparseNumericFailureReason;

/* Sparse-efficient -> dense fallback classification (per lu_factorize call). */
typedef enum {
    LU_SPARSE_FALLBACK_NONE = 0,
    LU_SPARSE_FALLBACK_SMALL_MATRIX,
    LU_SPARSE_FALLBACK_SYMBOLIC,
    LU_SPARSE_FALLBACK_NUMERIC
} LUSparseFallbackReason;

/* Identity-separation retry-lane selection inside sparse numeric factorization. */
typedef enum {
    LU_IDSEP_RETRY_LANE_NONE = 0,
    LU_IDSEP_RETRY_LANE_DENSE = 1,
    LU_IDSEP_RETRY_LANE_SUPERNODE = 2
} LUIdentitySepRetryLane;

/* LP model internal representation */
struct SimplexSolver;

typedef struct {
    /* Problem dimensions */
    int num_vars;           /* Number of variables (structural) */
    int num_cons;           /* Number of constraints */
    int num_elements;       /* Number of non-zeros in A */

    /* Constraint matrix A (in CSC format) */
    SparseMatrix *A;

    /* Objective: min c'x */
    double *c;              /* Objective coefficients */
    int obj_sense;          /* 1=minimize, -1=maximize */
    double obj_offset;      /* Constant term in objective */

    /* Constraints: Ax (sense) b */
    double *b;              /* RHS values */
    char *sense;            /* 'L', 'E', 'G' */
    int *con_origin;        /* >=0 original structural row id, -1 generated cut row */
    int con_origin_capacity;/* Allocated size of con_origin array */

    /* Variable bounds: lb <= x <= ub */
    double *lb;             /* Lower bounds */
    double *ub;             /* Upper bounds */
    unsigned char *var_shifted; /* 1 if presolve shifted x := x - lb on this variable */
    int var_shifted_capacity; /* Allocated size of var_shifted array */
    double *var_shift;      /* Original lower-bound shift applied by presolve */
    int var_shift_capacity; /* Allocated size of var_shift array */

    /* Variable types */
    char *var_type;         /* 'C', 'I', 'B' */
    int num_integers;       /* Number of integer variables */
    int num_binary;         /* Number of binary variables */

    /* Variable/constraint names (optional) */
    char **var_names;
    int var_names_capacity; /* Allocated size of var_names array */
    char **con_names;
    int con_names_capacity; /* Allocated size of con_names array */
    char *name;             /* Problem name */

    /* Build state for incremental constraint building (thread-safe) */
    LPModelBuildState *build_state;

    /* W2: Runtime-configurable tolerances (defaults from RALPH_*_TOL) */
    double feas_tol;            /* Primal/dual feasibility tolerance */
    double opt_tol;             /* Optimality (reduced cost) tolerance */
    double pivot_tol;           /* Minimum pivot element threshold */

} LPModel;

/* LU telemetry state (counters + timings, no behavior/policy knobs). */
typedef struct {
    /* Sparse Markowitz LU telemetry */
    int mkz_calls;           /* Markowitz factorization attempts */
    int mkz_successes;       /* Markowitz factorization successes */
    int mkz_failures;        /* Markowitz factorization failures */
    int mkz_last_failure;    /* Internal Markowitz failure code (0 on success) */
    int mkz_dense_fallbacks; /* Markowitz failed and dense GE path was used */
    int mkz_fail_workspace;  /* Markowitz attempt failed with MKZ_FAIL_WORKSPACE */
    int mkz_fail_pool;       /* Markowitz attempt failed with MKZ_FAIL_POOL */
    int mkz_fail_singular;   /* Markowitz attempt failed with MKZ_FAIL_SINGULAR */
    int mkz_fail_capacity;   /* Markowitz attempt failed with MKZ_FAIL_CAPACITY */
    int mkz_singular_retry_attempts;  /* Singular micro-retry attempts */
    int mkz_singular_retry_successes; /* Retry recovered a pivot >= pivot_tol */
    int mkz_singular_retry_failures;  /* Retry still ended singular path */
    int mkz_reserved_fallback_attempts; /* Reserved-row fallback scans in singular handling */
    int mkz_reserved_fallback_accepts;  /* Reserved-row fallback selected as pivot */
    int mkz_reserved_fallback_rejects;  /* Reserved-row fallback not selected */
    int mkz_circuit_trips;    /* Markowitz circuit breaker trip events */
    int mkz_circuit_skips;    /* Markowitz attempts skipped by circuit breaker */
    int mkz_circuit_resets;   /* Circuit streak reset after successful Markowitz path */
    int mkz_global_skip_trips; /* Global Markowitz skip-budget trip events */
    int mkz_global_skip_skips; /* Markowitz attempts skipped by global skip-budget */
    int mkz_global_skip_resets; /* Global skip-budget reset after successful Markowitz path */
    int mkz_profile_retry_attempts; /* Numeric retry with relaxed Markowitz profile */
    int mkz_profile_retry_successes; /* Relaxed Markowitz profile recovered factorization */
    int mkz_profile_retry_failures;  /* Relaxed Markowitz profile still failed */
    int mkz_profile_retry_fail_identity_sep; /* Retry profile ended with identity-separation failure */
    int mkz_profile_retry_fail_backend_exhausted; /* Retry profile ended with backend exhaustion */
    int mkz_profile_retry_fail_pathological; /* Retry profile ended with singular/pathological failure */
    uint64_t mkz_primary_scan_entries; /* Entries visited in primary pivot search */
    uint64_t mkz_rescue_scan_entries;  /* Entries visited in rescue pivot scan */
    uint64_t mkz_reserved_scan_entries; /* Entries visited in reserved-row fallback scan */
    uint64_t mkz_update_existing_entries; /* Pass-1 row entries visited during elimination */
    uint64_t mkz_update_fill_candidates;  /* Pass-2 pivot-row candidates visited */
    uint64_t mkz_hint_fallback_scans;     /* Times rv_hint fell back to a full scan */
    uint64_t mkz_hint_fallback_scan_entries; /* Entries scanned during rv_hint fallback */
    uint64_t mkz_affected_columns_total;  /* Live pivot-row columns touched across steps */
    uint64_t mkz_affected_columns_max;    /* Max live pivot-row columns touched in one step */
    uint64_t mkz_col_max_scan_entries;    /* Entries scanned while recomputing col_max */

    /* N1-A: Markowitz quality telemetry */
    int mkz_high_cond_count;  /* Factorizations where mkz cond_estimate > 1e8 */
    double mkz_worst_cond;    /* Worst mkz cond_estimate seen across all factorizations */

    /* Sparse-efficient fallback telemetry */
    int sparse_dense_fallbacks;  /* lu_factorize_sparse_efficient -> lu_factorize_dense */
    int used_dense_fallback_last;/* 1 if last lu_factorize call used dense fallback */
    int sparse_fallback_last_reason;      /* LUSparseFallbackReason */
    int sparse_fallback_reason_small_matrix;
    int sparse_fallback_reason_symbolic;
    int sparse_fallback_reason_numeric;
    int sparse_numeric_last_failure_reason; /* LUSparseNumericFailureReason */
    int sparse_numeric_fail_identity_sep;
    int sparse_numeric_fail_backend_exhausted;
    int sparse_numeric_fail_pathological;
    int numeric_full_retry_attempts; /* Full-structural sparse retry after numeric identity-separation failure */
    int numeric_full_retry_successes;
    int numeric_full_retry_failures;
    int identity_sep_failures;   /* Identity-placement failures in sparse-efficient path */
    int symbolic_failures;       /* Symbolic-stage failures before retry */
    int symbolic_fail_workspace;
    int symbolic_fail_unmatched_no_reserved;
    int symbolic_fail_inconsistent_identity;
    int symbolic_full_retry_attempts;
    int symbolic_full_retry_successes;
    int symbolic_full_retry_numeric_failures;
    int symbolic_full_retry_mkz_attempts;
    int symbolic_full_retry_mkz_successes;
    int symbolic_full_retry_mkz_failures;
    int numeric_backend_markowitz;
    int numeric_backend_supernode;
    int numeric_backend_dense_ge;
    int backend_policy_luf_ft;
    int backend_policy_cbg;
    int backend_policy_cgr;
    int backend_policy_last;
    int update_path_ft;
    int update_path_eta;
    int update_path_bg_compat;
    int update_path_gr_compat;
    int identity_sep_retry_lane_dense_chosen;
    int identity_sep_retry_lane_supernode_chosen;
    int identity_sep_retry_lane_dense_successes;
    int identity_sep_retry_lane_supernode_successes;
    int sn_cost_gate_trips;
    int sn_cost_gate_skips;
    int sn_cost_gate_resets;
    int refactor_need_checks;      /* lu_needs_refactorization checks */
    int refactor_need_triggers;    /* checks that requested reinvert */
    int refactor_need_last_reason; /* LP_BFCP_REFACTOR_REASON_* */
    int refactor_need_reason_max_updates;
    int refactor_need_reason_growth_guard;
    int refactor_need_reason_avg_spike_density;
    int refactor_need_reason_cond_severe;
    int refactor_need_reason_cond_adaptive_limit;
    int refactor_need_reason_spike_pool_warn;
    int refactor_need_reason_spike_work;
    int refactor_need_reason_spike_diag_quality; /* N2: spike diag ratio > 1e8 */
    int update_fail_bad_input;
    int update_fail_max_updates;
    int update_fail_singular_update;
    int update_fail_update_pivot_too_small;
    int update_fail_spike_pool_full;
    int update_fail_dense_spike_reject;
    int update_fail_eta_alloc;

    /* Sparse factorization stage timing telemetry (aggregate + last call) */
    int perf_factorize_calls;      /* Number of lu_factorize() calls */
    int perf_last_basis_nnz;       /* Input basis nnz for last factorization */
    int perf_last_m;               /* Basis dimension (m) for last factorization */
    int perf_last_k;               /* Structural block width (k) for last factorization */
    int perf_symbolic_calls;
    int perf_symbolic_cache_hits;
    int perf_symbolic_cache_misses;
    double perf_last_symbolic_ms;
    double perf_last_sparse_numeric_ms;
    double perf_last_dense_ge_numeric_ms;
    double perf_last_supernode_numeric_ms;
    double perf_last_dense_factorize_ms;
    double perf_last_a_struct_build_ms;
    double perf_last_markowitz_numeric_ms;
    double perf_last_identity_placement_ms;
    double perf_last_coo_to_csc_ms;
    double perf_total_symbolic_ms;
    double perf_total_sparse_numeric_ms;
    double perf_total_dense_ge_numeric_ms;
    double perf_total_supernode_numeric_ms;
    double perf_total_dense_factorize_ms;
    double perf_total_a_struct_build_ms;
    double perf_total_markowitz_numeric_ms;
    double perf_total_identity_placement_ms;
    double perf_total_coo_to_csc_ms;
    int perf_update_apply_forward_calls;
    int perf_update_apply_backward_calls;
    int perf_compact_factor_calls;
    int perf_compact_solve_calls;
    double perf_total_update_apply_forward_ms;
    double perf_total_update_apply_backward_ms;
    double perf_total_compact_factor_ms;
    double perf_total_compact_solve_ms;
    uint64_t perf_sn_phase_samples;
    double perf_sn_panel_factor_ms;
    double perf_sn_panel_pivot_search_ms;
    double perf_sn_panel_swap_scatter_ms;
    double perf_sn_panel_eliminate_ms;
    uint64_t perf_sn_panel_pivot_search_calls;
    uint64_t perf_sn_panel_pivot_search_entries_total;
    uint64_t perf_sn_panel_pivot_search_size1_calls;
    double perf_sn_panel_pivot_search_size1_ms;
    uint64_t perf_sn_panel_pivot_search_size2_calls;
    double perf_sn_panel_pivot_search_size2_ms;
    uint64_t perf_sn_panel_pivot_search_size3_4_calls;
    double perf_sn_panel_pivot_search_size3_4_ms;
    uint64_t perf_sn_panel_pivot_search_size5_8_calls;
    double perf_sn_panel_pivot_search_size5_8_ms;
    uint64_t perf_sn_panel_pivot_search_size9p_calls;
    double perf_sn_panel_pivot_search_size9p_ms;
    uint64_t perf_sn_panel_pivot_search_reserved_present_calls;
    uint64_t perf_sn_panel_pivot_search_reserved_present_entries;
    double perf_sn_panel_pivot_search_reserved_present_ms;
    uint64_t perf_sn_panel_pivot_search_reserved_alt_chosen_calls;
    double perf_sn_panel_pivot_search_reserved_alt_chosen_ms;
    uint64_t perf_sn_size1_u_emit_calls;
    double perf_sn_size1_u_emit_ms;
    uint64_t perf_sn_size1_update_scan_calls;
    double perf_sn_size1_update_scan_ms;
    uint64_t perf_sn_size1_update_apply_calls;
    double perf_sn_size1_update_apply_ms;
    double perf_sn_size1_update_row_gather_ms;
    double perf_sn_size1_update_col_indirection_ms;
    double perf_sn_size1_update_outer_product_ms;
    uint64_t perf_sn_size1_update_full_calls;
    double perf_sn_size1_update_full_ms;
    uint64_t perf_sn_size1_update_cols1_calls;
    double perf_sn_size1_update_cols1_ms;
    uint64_t perf_sn_size1_update_cols2_calls;
    double perf_sn_size1_update_cols2_ms;
    uint64_t perf_sn_size1_update_cols3_calls;
    double perf_sn_size1_update_cols3_ms;
    uint64_t perf_sn_size1_update_cols4_calls;
    double perf_sn_size1_update_cols4_ms;
    uint64_t perf_sn_size1_update_cols5p_calls;
    double perf_sn_size1_update_cols5p_ms;
    uint64_t perf_sn_size1_update_cols5p_rows1_8_calls;
    double perf_sn_size1_update_cols5p_rows1_8_ms;
    uint64_t perf_sn_size1_update_cols5p_rows9_32_calls;
    double perf_sn_size1_update_cols5p_rows9_32_ms;
    uint64_t perf_sn_size1_update_cols5p_rows33_128_calls;
    double perf_sn_size1_update_cols5p_rows33_128_ms;
    uint64_t perf_sn_size1_update_cols5p_rows129p_calls;
    double perf_sn_size1_update_cols5p_rows129p_ms;
    double perf_sn_u_emit_ms;
    double perf_sn_active_set_ms;
    double perf_sn_pack_blocks_ms;
    double perf_sn_full_update_ms;
    double perf_sn_compact_update_ms;
    uint64_t perf_sn_active_row_scan_entries;
    uint64_t perf_sn_active_col_scan_entries;
    uint64_t perf_sn_trailing_rows_total;
    uint64_t perf_sn_trailing_cols_total;
    uint64_t perf_sn_active_rows_total;
    uint64_t perf_sn_active_cols_total;
    uint64_t perf_sn_pack_l_entries_total;
    uint64_t perf_sn_pack_u_entries_total;
    uint64_t perf_sn_dense_triplets_total;
    uint64_t perf_sn_compact_triplets_total;
    uint64_t perf_sn_full_update_calls;
    uint64_t perf_sn_compact_update_calls;
    uint64_t perf_sn_skipped_update_calls;
    uint64_t perf_sn_compact_cols1_calls;
    uint64_t perf_sn_compact_cols1_rows_total;
    double perf_sn_compact_cols1_ms;
    uint64_t perf_sn_compact_cols2_calls;
    uint64_t perf_sn_compact_cols2_rows_total;
    double perf_sn_compact_cols2_ms;
    uint64_t perf_sn_compact_cols3_calls;
    uint64_t perf_sn_compact_cols3_rows_total;
    double perf_sn_compact_cols3_ms;
    uint64_t perf_sn_compact_cols4_calls;
    uint64_t perf_sn_compact_cols4_rows_total;
    double perf_sn_compact_cols4_ms;
    uint64_t perf_sn_compact_cols5p_calls;
    uint64_t perf_sn_compact_cols5p_rows_total;
    double perf_sn_compact_cols5p_ms;
} LUTelemetryState;

/* LU factorization of basis matrix */
typedef struct {
    int m;                  /* Dimension */
    int nnz_L;              /* Non-zeros in L */
    int nnz_U;              /* Non-zeros in U */
    int *L_colptr;
    int *L_rowidx;
    double *L_values;
    int *U_colptr;
    int *U_rowidx;
    double *U_values;
    double *U_diag;         /* Diagonal values of U for fast access */
    int *perm;              /* Row permutation */
    int *perm_inv;          /* Inverse permutation */
    int *col_perm;          /* Column permutation */
    int *col_perm_inv;      /* Inverse column permutation */
    int num_updates;        /* Number of updates since refactorization */
    int max_updates;        /* Max updates before refactorization */

    /* Eta file for updates (sparse storage) */
    int eta_capacity;
    int num_eta;
    int *eta_col;           /* Column index for each eta */
    int **eta_indices;      /* Row indices of non-zeros for each eta */
    double **eta_values;    /* Values of non-zeros for each eta */
    int *eta_nnz;           /* Number of non-zeros in each eta */

    /* BG/GR sparse-update lane. BG uses these columns as the low-rank U part
     * of M = I + U V^T; GR continues to use them as a compatibility chain
     * until a real GR kernel exists. */
    int schur_capacity;
    int schur_num_updates;
    int *schur_col;
    int **schur_indices;
    double **schur_values;
    int *schur_nnz;
    double *schur_k;        /* Dense K = I + V^T U for BG backend */
    double *schur_k_work;   /* Cached forward compact factorization */
    double *schur_k_t_work; /* Cached transpose compact factorization */
    double *schur_rhs;      /* RHS/solution workspace for Schur solves */
    int *schur_piv;         /* Forward pivot workspace for BG factor cache */
    int *schur_piv_t;       /* Backward pivot workspace for BG factor cache */
    double *schur_rot_fwd_c;/* Forward Givens c coefficients for GR cache */
    double *schur_rot_fwd_s;/* Forward Givens s coefficients for GR cache */
    double *schur_rot_bwd_c;/* Backward Givens c coefficients for GR cache */
    double *schur_rot_bwd_s;/* Backward Givens s coefficients for GR cache */
    int schur_rot_capacity; /* Max stored Givens rotations */
    int schur_factor_fwd_valid;
    int schur_factor_bwd_valid;

    /* Forrest-Tomlin update data */
    int use_ft_updates;     /* 1 to use FT updates, 0 for non-FT update lanes */
    int backend_policy;     /* LP_LU_BACKEND_POLICY_* effective backend policy */
    int update_backend;     /* LUUpdateBackend */
    int *ft_col_order;      /* Permutation of columns due to FT updates */
    int *ft_col_order_inv;  /* Inverse of ft_col_order */
    int ft_num_updates;     /* Number of FT updates applied */

    /* Spike storage for FT - CONTIGUOUS layout for cache efficiency
     *
     * All spike data stored in contiguous arrays with offset tracking.
     * Access pattern: spike k's data is at pool[spike_start[k]..spike_start[k]+spike_nnz[k])
     * This eliminates double indirection (pointer-of-pointers) for 15-20% speedup.
     */
    int ft_spike_capacity;      /* Max number of spikes */
    int *ft_spike_col;          /* Which column this spike replaces */
    double *ft_spike_diag;      /* Diagonal value (1/pivot) for branchless apply */
    int *ft_spike_nnz;          /* Number of OFF-DIAGONAL non-zeros in each spike */
    int *ft_spike_start;        /* Start offset in pool for each spike */

    /* Contiguous spike pool - single allocation for all spike data */
    int *spike_pool_idx;        /* All spike row indices (contiguous) */
    double *spike_pool_val;     /* All spike values (contiguous) */
    int spike_pool_capacity;    /* Total allocated size of pool */
    int spike_pool_used;        /* Currently used entries in pool */
    int dense_spike_min_updates_override; /* <=0 uses default dense-spike warmup */

    /* (B4: spike compaction removed — was O(m^2*N), worse than sparse application) */

    /* Condition number monitoring */
    double min_diag_U;      /* Minimum |U[i,i]| at factorization */
    double max_diag_U;      /* Maximum |U[i,i]| at factorization */
    double cond_estimate;   /* Estimated condition number */
    double growth_factor;   /* Growth in U during updates */
    double mkz_last_cond;   /* N1-A: cond_estimate from last Markowitz factorization (0 if non-Markowitz) */

    /* N2: FT spike diagonal quality tracking */
    double ft_spike_diag_min;  /* Min |spike_diag| across FT updates (reset on refactor) */
    double ft_spike_diag_max;  /* Max |spike_diag| across FT updates (reset on refactor) */
    double growth_refactor_threshold; /* Growth threshold override (<=0 uses default) */

    /* Redundant row hints (for two-phase simplex with stuck artificials)
     * These point to external data from the tableau, not owned by LU */
    const int *redundant_rows;  /* Bitmap: row[i]=1 if redundant (NULL if none) */
    int num_redundant;          /* Count of redundant rows */
    int allow_regularization;   /* 1 to allow regularizing zero pivots (for rank-deficient problems) */
    int max_regularizations;    /* Limit on number of rows to regularize */
    int num_regularized;        /* Count of rows regularized in current factorization */
    double pivot_tol;           /* Dynamic pivot tolerance (default RALPH_PIVOT_TOL) */
    int last_failure_reason;    /* LUFailureReason (last failed lu_factorize/lu_update reason) */
    int last_refactor_trigger_reason; /* LP_BFCP_REFACTOR_REASON_* from lu_needs_refactorization */
    int telemetry_enabled;      /* 1 = collect LU telemetry counters/timers */
    struct SimplexSolver *owner; /* Owning solver when attached to a tableau */
    LPBasisGovernorState *basis_governor; /* Non-owning pointer to solver governor state */

    /* Pre-allocated workspace for hyper-sparse operations */
    double *hs_work1;       /* Dense workspace 1 */
    double *hs_work2;       /* Dense workspace 2 */
    int *hs_marked;         /* Marked array for reach computation */
    int *hs_idx;            /* Sparse index array */
    double *hs_val;         /* Sparse value array */
    int *hs_stack;          /* Stack for DFS in reach computation */
    double *perm_work;      /* Workspace for permutation operations */

    /* Pre-allocated workspace for dense LU fallback (m×m matrix) */
    double *dense_work;     /* Reused across factorizations to avoid O(m²) alloc */

    /* Dedicated Markowitz workspace (growable, avoids dense_work tail carving) */
    double *mkz_work;       /* Scratch buffer for sparse Markowitz internals */
    size_t mkz_work_capacity;   /* Capacity in doubles */

    /* Pre-allocated workspace for sparse-efficient factorization (T1.4) */
    int *ws_is_identity;     /* [m] identity column classification */
    int *ws_identity_row;    /* [m] row of identity entry */
    double *ws_identity_val; /* [m] value (+-1) of identity entry */
    int *ws_row_used;        /* [m] rows claimed by identity cols */
    int *ws_col_order;       /* [m] column ordering */
    int *ws_col_order_inv;   /* [m] inverse column ordering */
    int *ws_row_perm;        /* [m] row permutation */
    int *ws_L_pos;           /* [m] L CSC column position counters */
    int *ws_U_pos;           /* [m] U CSC column position counters */
    int *ws_row_pos;         /* [m] inverse of row_perm (for identity O(1) lookup) */
    int *ws_struct_nnz;      /* [m] column nnz counts for sorting + fingerprint */
    int *ws_row_identity_col;/* [m] symbolic: row -> identity column mapping */
    int *ws_row_match_col;   /* [m] symbolic: row -> matched structural column */
    int *ws_row_seen;        /* [m] symbolic DFS seen-token workspace */

    /* Capacity tracking for L/U output arrays (T1.4) */
    int LU_out_capacity;     /* Allocated nnz capacity for L/U rowidx/values arrays */

    /* Pre-allocated COO arrays for sparse-efficient factorization (T1.4 full) */
    int *coo_L_row;          /* [coo_capacity] L entries: row indices */
    int *coo_L_col;          /* [coo_capacity] L entries: column indices */
    double *coo_L_val;       /* [coo_capacity] L entries: values */
    int *coo_U_row;          /* [coo_capacity] U entries: row indices */
    int *coo_U_col;          /* [coo_capacity] U entries: column indices */
    double *coo_U_val;       /* [coo_capacity] U entries: values */
    int coo_capacity;        /* Allocated capacity for COO arrays */

    /* Cached symbolic analysis for sparse-efficient path (T1.4 full) */
    int sym_valid;           /* 1 if cached symbolic analysis is valid */
    int sym_num_identity;    /* Cached identity column count */
    int sym_k;               /* Cached structural column count (m - num_identity) */
    uint64_t sym_fingerprint;    /* FNV-1a hash of basis sparsity pattern */
    int sym_factorization_type;  /* LP_GLPK_BFCP_FACTORIZATION_* */
    int sym_btf_blocks;          /* Structural SCC block count for BTF mode */
    /* sym uses ws_is_identity, ws_identity_row, ws_identity_val, ws_col_order, ws_col_order_inv */

    /* Sparse Markowitz LU */
    int mkz_enabled;         /* 1 = use Markowitz path when k >= MARKOWITZ_MIN_K */
    int mkz_pool_mult_hint;  /* Adaptive starting pool multiplier (reduces retry churn) */
    uint64_t mkz_circuit_fingerprint; /* Fingerprint keyed for circuit-breaker state */
    int mkz_circuit_bad_streak;       /* Consecutive bad Markowitz outcomes for fingerprint */
    int mkz_circuit_skip_budget;      /* Remaining calls to skip Markowitz for fingerprint */
    int mkz_global_singular_streak;   /* Consecutive singular Markowitz outcomes (cross-fingerprint) */
    int mkz_global_skip_budget;       /* Remaining calls to skip Markowitz globally */
    uint64_t idsep_retry_fingerprint; /* Fingerprint keyed for identity-separation retry lane */
    int idsep_retry_streak;           /* Consecutive identity-separation events on fingerprint */

    /* Supernodal LU (T2.1) */
    int sn_enabled;          /* 1 = use supernodal path when k >= SN_MIN_K */
    struct SNSymbolic_tag *sn_symbolic;  /* Cached symbolic analysis (forward decl) */
    double *sn_work;         /* Pre-allocated workspace for GEMM blocks */
    size_t sn_work_capacity; /* Size in doubles */
    double sn_cost_gate_supernode_ewma_ms;  /* Behavioral EWMA of supernode factorize cost */
    double sn_cost_gate_markowitz_ewma_ms;  /* Behavioral EWMA of Markowitz factorize cost */
    int sn_cost_gate_skip_budget;           /* Remaining supernode skips after cost-gate trip */
    int sn_calls;            /* Number of times supernodal path was attempted */
    int sn_successes;        /* Number of times supernodal path succeeded */

    /* Telemetry state */
    LUTelemetryState telemetry;

    /* W1: Sparse BTRAN readiness flag (set after each refactorization) */
    int csr_valid;           /* 1 if L/U CSC data is valid for sparse BTRAN reach */

    /* Arena allocator for fixed-size arrays (reduces ~20 mallocs to 1) */
    SHArena *arena;
} LUFactorization;

/* Simplex tableau representation */
typedef struct {
    /* Problem data */
    LPModel *model;
    struct SimplexSolver *owner;   /* Owning solver (NULL when detached) */

    /* Extended problem (with slacks) */
    int n;                  /* Total variables (structural + split + slack) */
    int m;                  /* Number of constraints */
    int num_structural_ext; /* Original structural columns plus generated split columns */
    SparseMatrix *A_ext;    /* Extended constraint matrix */
    SparseMatrix *basis_work; /* Reusable CSC workspace for basis extraction */
    int *basis_col_cache;   /* Basis-position -> A_ext column id from last basis build */
    int *basis_col_nnz_cache; /* Basis-position -> cached column nnz from last basis build */
    int basis_cache_valid;  /* 1 if basis_work + cache arrays match current basis/A_ext values */
    int basis_cache_total_nnz; /* Cached total nnz for basis_work */
    double *c_ext;          /* Extended objective */
    double *lb_ext;         /* Extended lower bounds */
    double *ub_ext;         /* Extended upper bounds */
    int *free_split_col;    /* Original var -> generated negative-part column, or -1 */
    int *free_split_orig;   /* Generated split column -> original var, or -1 */

    /* Basis information */
    int *basis;             /* Indices of basic variables (size m) */
    int *nonbasis;          /* Indices of non-basic variables (size n-m) */
    VarStatus *var_status;  /* Status of each variable */
    int *basis_pos;         /* Position in basis (-1 if non-basic) */

    /* LU factorization of basis */
    LUFactorization *lu;

    /* Current solution */
    double *x;              /* Variable values */
    double *y;              /* Dual values (row prices) */
    double *rc;             /* Reduced costs */
    double obj_value;       /* Current objective value */

    /* Working vectors */
    double *work1;
    double *work2;
    double *work3;
    double *work4;
    double *rhs;            /* Normalized RHS (always >= 0) */
    double *row_sign;       /* Row transformation signs (+1 or -1) for Farkas mapping */
    double *pivot_row;      /* Pre-allocated for simplex_pivot */
    double *tau_work;       /* Pre-allocated for steepest edge */

    /* Steepest edge / Devex weights */
    double *se_weights;     /* Steepest edge weights */
    int use_steepest_edge;
    int pricing_strategy;   /* 0=Dantzig, 1=SE, 2=Devex, 3=Partial, 4=Heap - for pivot fn */
    int devex_refcount;     /* Reference count for Devex weight resets */

    /* Dual steepest edge weights (P6) */
    double *dse_weights;     /* Size m: ||row_i(B^{-1})||^2 per basis position */
    int dse_initialized;     /* 1 = weights valid, 0 = need init */

    /* Bound flipping scratch (P5) */
    int *flip_list;          /* Size n: indices of flipped vars this iteration */
    int flip_count;          /* Number of flips this iteration */

    /* Bound perturbation backup (for dual simplex anti-cycling) */
    double *perturb_backup;    /* Original upper bounds before perturbation */
    double *perturb_backup_lb; /* Original lower bounds before perturbation (W5) */

    /* Primal bound perturbation state (for primal simplex anti-cycling) */
    double *primal_saved_lb;    /* Saved lower bounds before perturbation */
    double *primal_saved_ub;    /* Saved upper bounds before perturbation */
    int primal_perturb_active;  /* 1 if perturbation is currently active */

    /* Progressive perturbation scaling (for stall recovery re-perturbation) */
    double perturb_scale;       /* Multiplier for PERTURB_BASE, default 1.0 */

    /* Positive edge mode: skip weak entering candidates during degeneracy */
    int positive_edge_mode;     /* 1 = active, skip candidates with |rc| < 10% of max */

    /* Partial pricing state */
    int partial_price_pos;  /* Starting position for next partial price scan */

    /* Candidate list for improved partial pricing */
    int *partial_candidates;    /* Hot set of promising variable indices */
    int partial_cand_count;     /* Current number of candidates in hot set */
    int partial_cand_capacity;  /* Allocated capacity for candidates */

    /* Dual candidate list for ratio test (T2.2) */
    int *dual_candidates;       /* Hot set of variables with attractive |rc| */
    int dual_cand_count;        /* Current number of candidates */
    int dual_cand_capacity;     /* Allocated capacity */
    int dual_cand_valid;        /* 1 if list was populated from last RC update */

    /* Heap pricing (T2.2) */
    int *heap;                  /* [n] max-heap of non-basic var indices by |rc| */
    int *heap_pos;              /* [n] position in heap, -1 if not in heap */
    int heap_size;              /* entries in heap */

    /* Lazy reduced cost computation */
    int duals_valid;            /* 1 if y[] contains valid dual values */
    int rc_all_valid;           /* 1 if rc[] contains all valid reduced costs */

    /* CSR (row-form) of A_ext for row-scatter RC update.
     * Only used when matrix density < 2% (row-scatter beats vectorized column-scan). */
    int *csr_rowptr;        /* [m+1] row pointers */
    int *csr_colidx;        /* [nnz] column indices */
    double *csr_values;     /* [nnz] values */
    double *csr_alpha;      /* [n] scratch for accumulating alpha_j in row-scatter */
    int csr_use_scatter;    /* 1 if row-scatter is enabled (sparse enough to benefit) */

    /* Auxiliary variable mapping (for cut generation) */
    int *aux_row;           /* For each aux var j >= num_structural_ext: which constraint row */
    double *aux_coef;       /* For each aux var: coefficient in that row (+1 or -1) */
    int num_aux;            /* Number of auxiliary variables */

    /* Two-phase simplex support */
    int use_two_phase;          /* 1 if using two-phase method (not Big-M) */
    double *c_original;         /* Original objective coefficients (for Phase 2) */
    int *artificial_vars;       /* Indices of artificial variables */
    int num_artificial;         /* Count of artificial variables */
    unsigned char *is_artificial_var; /* O(1) artificial variable membership */
    int artificial_basic_count;  /* Number of artificial variables currently basic */
    int num_equalities;         /* Count of equality constraints */
    int *redundant_rows;        /* Bitmap: row[i]=1 if redundant (stuck artificial) */
    int num_redundant;          /* Count of redundant rows */
    int redundant_rows_zeroed;  /* 1 if redundant rows have been zeroed in A_ext */

    /* Statistics */
    int iterations;
    int phase;              /* 1 or 2 */
    int phase1_compute_solution_context;
    int phase1_compute_rc_context;
    int solution_last_residual_iter;            /* Last iteration with full residual/refinement check */
    int solution_last_residual_factorize_calls; /* LU factorize_calls at last residual/refinement check */
    int solution_last_residual_num_updates;     /* LU num_updates at last residual/refinement check */

    /* Phase-1 failure tracing (deterministic diagnostics for numerical stalls) */
    int trace_phase1_enabled;      /* 1 to emit trace lines */
    int trace_phase1_iter;         /* Iteration index at current pivot attempt */
    int trace_last_entering;       /* Entering var for last pivot attempt */
    int trace_last_leaving_pos;    /* Leaving row position for last pivot attempt */
    double trace_last_theta;       /* Ratio-test step for last pivot attempt */
    double trace_last_pivot;       /* Pivot element d[leaving_pos] */
    double trace_last_dir_inf;     /* ||direction||_inf for last pivot attempt */
    int trace_last_fail_reason;    /* Failure reason code from simplex_pivot */

    /* Pre-allocated sparse workspace for reduced cost computation */
    int *cb_sparse_idx;     /* Sparse indices for c_B (size m) */
    double *cb_sparse_val;  /* Sparse values for c_B (size m) */

    /* Pre-allocated backup for primal simplex pivot rollback */
    double *primal_basic_x_backup; /* size m */

    /* Pre-allocated backup arrays for dual_simplex_pivot rollback (B2 fix) */
    double *dual_x_backup;          /* size n */
    double *dual_rc_backup;         /* size n */
    int *dual_basis_backup;         /* size m */
    int *dual_basis_pos_backup;     /* size n */
    VarStatus *dual_status_backup;  /* size n */

    /* Arena allocator for workspace arrays (reduces 20+ mallocs to 1) */
    SHArena *arena;

} SimplexTableau;

/* Solver telemetry state (timings/counters only). */
typedef struct {
    double perf_primal_setup_ms;   /* Tableau create + initial factorization */
    double perf_dual_ms;           /* Dual path runtime (method 1/2 attempt) */
    double perf_phase1_ms;         /* Primal Phase 1 runtime */
    double perf_transition_ms;     /* Phase 1 -> 2 transition runtime */
    double perf_phase2_ms;         /* Primal Phase 2 runtime */
    double perf_pricing_ms;        /* Entering variable pricing time */
    double perf_ratio_ms;          /* Ratio test time */
    double perf_pivot_ms;          /* Pivot application time */
    double perf_refactor_ms;       /* Refactorization time */
    double perf_ftran_ms;          /* FTRAN solve time (B^{-1} * a) */
    double perf_btran_ms;          /* BTRAN solve time (B^{-T} * e) */
    double perf_ftran_base_ms;     /* FTRAN solve time excluding update-backend apply */
    double perf_ftran_update_apply_ms; /* FTRAN time spent in update-backend apply */
    int perf_ftran_update_apply_calls; /* FTRAN solves that applied backend updates */
    double perf_btran_base_ms;     /* BTRAN solve time excluding update-backend apply */
    double perf_btran_update_apply_ms; /* BTRAN time spent in update-backend apply */
    int perf_btran_update_apply_calls; /* BTRAN solves that applied backend updates */
    int perf_ftran_calls;          /* Number of FTRAN solve calls */
    int perf_btran_calls;          /* Number of BTRAN solve calls */
    int perf_ftran_nnz_samples;    /* Number of FTRAN solves with nnz telemetry */
    int perf_btran_nnz_samples;    /* Number of BTRAN solves with nnz telemetry */
    long long perf_ftran_rhs_nnz_total; /* Sum of FTRAN RHS nnz across sampled calls */
    long long perf_ftran_sol_nnz_total; /* Sum of FTRAN solution nnz across sampled calls */
    long long perf_btran_rhs_nnz_total; /* Sum of BTRAN RHS nnz across sampled calls */
    long long perf_btran_sol_nnz_total; /* Sum of BTRAN solution nnz across sampled calls */
    double perf_lu_update_ms;      /* LU update time */
    double perf_compute_solution_ms; /* tableau_compute_solution time */
    double perf_compute_rc_ms;     /* tableau_compute_reduced_costs time */
    double perf_refactor_all_ms;   /* All tableau_refactorize time (fully covered) */
    int perf_refactor_count;       /* Number of tableau_refactorize calls */
    double perf_refactor_last_ms;  /* Last refactor duration */
    double perf_refactor_max_ms;   /* Max single refactor duration */
    int perf_refactor_last_reason; /* RalphRefactorReason */
    int perf_refactor_reason_setup;
    int perf_refactor_reason_transition;
    int perf_refactor_reason_periodic;
    int perf_refactor_reason_ratio_recovery;
    int perf_refactor_reason_pivot_recovery;
    int perf_refactor_reason_forced_small_pivot;
    int perf_refactor_reason_update_recovery;
    int perf_refactor_reason_direction_stabilize;
    int perf_refactor_reason_infeas_cleanup;
    int perf_refactor_reason_other;
    int perf_refactor_periodic_policy;    /* periodic refactors triggered by scheduler policy */
    int perf_refactor_periodic_lu_health; /* periodic refactors triggered by LU health guard */
    int perf_refactor_safety_forced;      /* non-periodic safety-driven refactors */
    int perf_basis_fastpath_hits;         /* basis extraction fast-path hits */
    int perf_basis_cols_rewritten;        /* changed basis columns rewritten from A_ext */
    unsigned long long perf_basis_tail_shift_bytes; /* bytes moved by basis tail shifts */
    int perf_refactor_last_m;
    int perf_refactor_last_k;
    int perf_refactor_last_nnz_B;

    /* Per-phase hot-path breakdown (Phase 1 vs Phase 2) */
    double perf_phase1_pricing_ms;
    double perf_phase1_ratio_ms;
    double perf_phase1_pivot_ms;
    double perf_phase1_refactor_ms;
    double perf_phase1_compute_solution_ms;
    double perf_phase1_compute_rc_ms;
    int perf_phase1_pricing_calls;
    int perf_phase1_ratio_calls;
    int perf_phase1_pivot_calls;
    int perf_phase1_refactor_calls;
    int perf_phase1_compute_solution_calls;
    int perf_phase1_compute_rc_calls;
    int perf_phase1_compute_solution_ctx_other;
    int perf_phase1_compute_solution_ctx_recompute_full;
    int perf_phase1_compute_solution_ctx_recompute_guard_forced_full;
    int perf_phase1_compute_solution_ctx_init;
    int perf_phase1_compute_solution_ctx_no_entering_cleanup;
    int perf_phase1_compute_solution_ctx_infeas_cleanup;
    int perf_phase1_compute_solution_ctx_refactor_fail_continue;
    int perf_phase1_compute_solution_ctx_refactor_failure_recovery;
    int perf_phase1_compute_solution_ctx_refactor_success;
    int perf_phase1_compute_solution_ctx_drift_refresh;
    int perf_phase1_compute_solution_ctx_dual_rescue;
    int perf_phase1_compute_rc_ctx_other;
    int perf_phase1_compute_rc_ctx_recompute_full;
    int perf_phase1_compute_rc_ctx_recompute_rc_only;
    int perf_phase1_compute_rc_ctx_recompute_guard_forced_full;
    int perf_phase1_compute_rc_ctx_init;
    int perf_phase1_compute_rc_ctx_infeas_cleanup;
    int perf_phase1_compute_rc_ctx_refactor_fail_continue;
    int perf_phase1_compute_rc_ctx_refactor_failure_recovery;
    int perf_phase1_compute_rc_ctx_refactor_success;
    int perf_phase1_compute_rc_ctx_drift_refresh;
    int perf_phase1_compute_rc_ctx_dual_rescue;
    int perf_phase1_entering_exclusions;
    int perf_phase1_entering_exclusion_repeats;
    int perf_phase1_entering_exclusion_hits;
    int perf_phase1_entering_exclusion_reroutes;
    int perf_phase1_entering_exclusion_no_alt;
    int perf_phase1_refactor_periodic_policy;
    int perf_phase1_refactor_periodic_lu_health;
    int perf_phase1_refactor_safety_forced;
    int perf_phase1_dir_stabilize_force_extreme_dir;
    int perf_phase1_dir_stabilize_force_lu_health;
    int perf_phase1_dir_stabilize_cooldown_candidates;
    int perf_phase1_dir_stabilize_ratio_le_3;
    int perf_phase1_dir_stabilize_ratio_le_10;
    int perf_phase1_dir_stabilize_ratio_le_30;
    int perf_phase1_dir_stabilize_ratio_le_100;
    int perf_phase1_dir_stabilize_ratio_gt_100;
    int perf_phase1_dir_stabilize_ratio_gt_300;
    int perf_phase1_dir_stabilize_ratio_gt_1000;
    int perf_phase1_dir_stabilize_skip_rc_only;
    int perf_phase1_dir_stabilize_skip_full;
    int perf_phase1_dir_stabilize_skip_no_recompute;
    int perf_phase1_dir_stabilize_skip_guard_refresh;
    int perf_phase1_dir_stabilize_escape_gate_triggers;
    int perf_phase1_dir_stabilize_escape_gate_suppressed_lu_health;
    int perf_phase1_dir_stabilize_escape_gate_suppressed_force_pivot_mode;
    int perf_phase1_dir_stabilize_escape_gate_hard_bypass;
    int perf_phase1_dir_stabilize_refactor_from_no_pivot_force;
    int perf_phase1_dir_stabilize_refactor_from_force_extreme_dir;
    int perf_phase1_dir_stabilize_refactor_from_force_lu_health;
    int perf_phase1_dir_stabilize_refactor_from_force_pivot_mode;
    int perf_phase1_dir_stabilize_refactor_from_ladder_force;
    int perf_phase1_force_pivot_budget_dir_event_seen;
    int perf_phase1_force_pivot_budget_pivot_spend;
    int perf_phase1_force_pivot_relax_applied;
    int perf_phase1_force_extreme_relax_applied;
    int perf_phase1_force_extreme_bound_flip_relax_applied;
    int perf_phase1_force_extreme_catastrophic_tiny_theta_relax_applied;
    int perf_phase1_force_extreme_tiny_theta_relax_applied;
    int perf_phase1_force_extreme_tiny_theta_relax_post_dir_skip_retry;
    int perf_phase1_force_extreme_tiny_theta_relax_post_dir_skip_dual_rescue;
    int perf_phase1_force_extreme_tiny_theta_relax_post_dir_skip_forced_refactor;
    int perf_phase1_force_extreme_tiny_theta_relax_refactor_force_lu_health;
    int perf_phase1_force_extreme_tiny_theta_relax_refactor_force_pivot_mode;
    int perf_phase1_force_extreme_tiny_theta_relax_refactor_ladder_force;
    int perf_phase1_force_extreme_tiny_theta_relax_next_failed_stabilize;
    int perf_phase1_force_extreme_tiny_theta_relax_next_ratio_breakdown;
    int perf_phase1_force_extreme_tiny_theta_relax_next_pivot_fail;
    int perf_phase1_force_extreme_tiny_theta_relax_next_pivot_success;
    int perf_phase1_recompute_after_ratio_breakdown;
    int perf_phase1_recompute_after_dir_skip;
    int perf_phase1_recompute_after_dir_refactor;
    int perf_phase1_recompute_after_pivot_fail_recovery;
    int perf_phase1_recompute_after_perturb;
    int perf_phase1_recompute_rc_only_calls;
    int perf_phase1_recompute_rc_guard_forced_full;
    int perf_phase1_ratio_breakdown_retries;
    int perf_phase1_ratio_breakdown_escalations;
    int perf_phase1_pivot_fail_recovery_exclusions;
    int perf_phase1_no_pivot_events;
    int perf_phase1_no_pivot_forced_refactor;
    int perf_phase1_no_pivot_forced_ratio_breakdown;
    int perf_phase1_no_pivot_forced_dir_skip;
    int perf_phase1_no_pivot_forced_pivot_fail;
    int perf_phase1_no_pivot_events_ratio_breakdown;
    int perf_phase1_no_pivot_events_dir_skip;
    int perf_phase1_no_pivot_events_pivot_fail;
    int perf_phase1_no_pivot_no_progress_events;
    int perf_phase1_no_pivot_ladder_retry_defers;
    int perf_phase1_no_pivot_ladder_retry_ratio_breakdown;
    int perf_phase1_no_pivot_ladder_retry_dir_skip;
    int perf_phase1_no_pivot_ladder_retry_pivot_fail;
    int perf_phase1_no_pivot_ladder_dual_rescue_attempts;
    int perf_phase1_no_pivot_ladder_dual_rescue_successes;
    int perf_phase1_no_pivot_ladder_dual_rescue_failures;
    int perf_phase1_no_pivot_ladder_dual_rescue_attempts_ratio_breakdown;
    int perf_phase1_no_pivot_ladder_dual_rescue_attempts_dir_skip;
    int perf_phase1_no_pivot_ladder_dual_rescue_attempts_pivot_fail;
    int perf_phase1_no_pivot_ladder_forced_refactors;
    int perf_phase1_no_pivot_ladder_forced_refactors_ratio_breakdown;
    int perf_phase1_no_pivot_ladder_forced_refactors_dir_skip;
    int perf_phase1_no_pivot_ladder_forced_refactors_pivot_fail;
    int perf_phase1_no_pivot_ladder_rescue_guard_cooldown_blocks;
    int perf_phase1_no_pivot_ladder_rescue_guard_fail_cap_forces;
    int perf_phase1_direct_dual_rescue_attempts;
    int perf_phase1_direct_dual_rescue_successes;
    int perf_phase1_direct_dual_rescue_failures;
    int perf_phase1_direct_dual_rescue_guard_cooldown_blocks;
    int perf_phase1_direct_dual_rescue_guard_fail_cap_blocks;
    int perf_phase1_soft_lu_policy_cooldown_defers;
    int perf_phase1_dir_skip_same_entering_repeats;
    int perf_phase1_dir_skip_same_entering_max_streak;
    int perf_phase1_failed_stabilize_events;
    int perf_phase1_failed_stabilize_primary_failures;
    int perf_phase1_failed_stabilize_alternate_failures;
    int perf_phase1_failed_stabilize_same_entering_repeats;
    int perf_phase1_failed_stabilize_same_entering_max_streak;
    int perf_phase1_failed_stabilize_retry_penalty_arms;
    int perf_phase1_failed_stabilize_retry_penalty_alt_found;
    int perf_phase1_failed_stabilize_retry_penalty_no_alt;
    int perf_phase1_failed_stabilize_retry_penalty_alt_stabilized;
    int perf_phase1_failed_stabilize_retry_penalty_alt_failed;
    int perf_phase1_failed_stabilize_retry_penalty_same_alt_repeats;
    int perf_phase1_failed_stabilize_retry_penalty_same_alt_max_streak;
    int perf_phase1_failed_stabilize_retry_local_memory_arms;
    int perf_phase1_failed_stabilize_retry_local_memory_alt_found;
    int perf_phase1_failed_stabilize_retry_local_memory_no_alt;
    int perf_phase1_failed_stabilize_retry_local_memory_fallback_same_alt;
    int perf_phase1_failed_stabilize_retry_local_memory_alt_stabilized;
    int perf_phase1_failed_stabilize_retry_local_memory_alt_failed;
    int perf_phase1_failed_stabilize_retry_pool_samples;
    int perf_phase1_failed_stabilize_retry_pool_eligible_total;
    int perf_phase1_failed_stabilize_retry_pool_eligible_max;
    int perf_phase1_failed_stabilize_retry_pool_singleton_samples;
    int perf_phase1_failed_stabilize_retry_pool_best_differs_samples;
    int perf_phase1_failed_stabilize_retry_selector_eval_samples;
    int perf_phase1_failed_stabilize_retry_selector_eval_best_differs_samples;
    double perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_total;
    double perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_max;
    int perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_2;
    int perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_4;
    int perf_phase1_failed_stabilize_retry_shadow_samples;
    int perf_phase1_failed_stabilize_retry_shadow_ratio_failed;
    int perf_phase1_failed_stabilize_retry_shadow_dir_stable;
    int perf_phase1_failed_stabilize_retry_shadow_dir_failed;
    int perf_phase1_failed_stabilize_retry_shadow_dir_nnz_total;
    int perf_phase1_failed_stabilize_retry_shadow_dir_nnz_max;
    double perf_phase1_failed_stabilize_retry_shadow_dir_inf_total;
    double perf_phase1_failed_stabilize_retry_shadow_dir_inf_max;
    double perf_phase1_failed_stabilize_retry_shadow_pivot_abs_total;
    double perf_phase1_failed_stabilize_retry_shadow_pivot_abs_max;
    int perf_phase1_failed_stabilize_retry_shadow_guard_arms;
    int perf_phase1_failed_stabilize_retry_shadow_guard_original_exclusions;
    int perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_retry;
    int perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_dual_rescue;
    int perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_forced_refactor;
    int perf_phase1_failed_stabilize_retry_shadow_next_failed_stabilize;
    int perf_phase1_failed_stabilize_retry_shadow_next_ratio_breakdown;
    int perf_phase1_failed_stabilize_retry_shadow_next_pivot_fail;
    int perf_phase1_failed_stabilize_retry_shadow_next_pivot_success;
    int perf_phase1_force_extreme_followup_stabilized;
    int perf_phase1_force_extreme_followup_ratio_breakdown;
    int perf_phase1_force_extreme_followup_failed_stabilize;
    int perf_phase1_force_extreme_followup_post_dir_skip_retry;
    int perf_phase1_force_extreme_followup_post_dir_skip_dual_rescue;
    int perf_phase1_force_extreme_followup_post_dir_skip_forced_refactor;
    int perf_phase1_force_extreme_followup_next_failed_stabilize;
    int perf_phase1_force_extreme_followup_next_ratio_breakdown;
    int perf_phase1_force_extreme_followup_next_pivot_fail;
    int perf_phase1_force_extreme_followup_next_pivot_success;
    int perf_phase1_force_extreme_followup_dir_samples;
    int perf_phase1_force_extreme_followup_dir_bound_geometry;
    int perf_phase1_force_extreme_followup_dir_bound_flip;
    int perf_phase1_force_extreme_followup_dir_tiny_theta;
    int perf_phase1_force_extreme_followup_dir_weak_leaving;
    int perf_phase1_force_extreme_followup_dir_ftran_shape;
    int perf_phase1_force_extreme_followup_dir_nnz_total;
    int perf_phase1_force_extreme_followup_dir_nnz_max;
    double perf_phase1_force_extreme_followup_dir_inf_total;
    double perf_phase1_force_extreme_followup_dir_inf_max;
    double perf_phase1_force_extreme_followup_pivot_abs_total;
    double perf_phase1_force_extreme_followup_pivot_abs_max;
    double perf_phase1_force_extreme_followup_theta_total;
    double perf_phase1_force_extreme_followup_theta_max;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_samples;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_bound_geometry;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_bound_flip;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_tiny_theta;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_weak_leaving;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_ftran_shape;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_nnz_total;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_nnz_max;
    double perf_phase1_failed_stabilize_retry_shadow_followup_dir_inf_total;
    double perf_phase1_failed_stabilize_retry_shadow_followup_dir_inf_max;
    double perf_phase1_failed_stabilize_retry_shadow_followup_pivot_abs_total;
    double perf_phase1_failed_stabilize_retry_shadow_followup_pivot_abs_max;
    double perf_phase1_failed_stabilize_retry_shadow_followup_theta_total;
    double perf_phase1_failed_stabilize_retry_shadow_followup_theta_max;
    int perf_phase1_failed_stabilize_retry_selector_bland_arms;
    int perf_phase1_failed_stabilize_retry_selector_guarded_arms;
    int perf_phase1_failed_stabilize_retry_selector_guarded_eligible_total;
    int perf_phase1_failed_stabilize_retry_selector_guarded_eligible_max;
    int perf_phase1_failed_stabilize_retry_selector_bland_alt_stabilized;
    int perf_phase1_failed_stabilize_retry_selector_bland_alt_failed;
    int perf_phase1_failed_stabilize_retry_selector_bland_ratio_failed;
    int perf_phase1_failed_stabilize_retry_selector_bland_dir_failed;
    int perf_phase1_failed_stabilize_retry_selector_guarded_alt_stabilized;
    int perf_phase1_failed_stabilize_retry_selector_guarded_alt_failed;
    int perf_phase1_failed_stabilize_retry_selector_guarded_ratio_failed;
    int perf_phase1_failed_stabilize_retry_selector_guarded_dir_failed;
    int perf_phase1_failed_stabilize_retry_selector_guarded_fallback_to_bland;
    int perf_phase1_failed_stabilize_retry_dir_fail_shape_samples;
    int perf_phase1_failed_stabilize_retry_dir_fail_nnz_total;
    int perf_phase1_failed_stabilize_retry_dir_fail_nnz_max;
    double perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_total;
    double perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_max;
    double perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_total;
    double perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_max;
    int perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_30;
    int perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_100;
    int perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_1000;
    int perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_gt_1000;
    int perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_8;
    int perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_6;
    int perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_4;
    int perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_gt_1e_4;
    int perf_phase1_failed_stabilize_retry_dir_second_chance_arms;
    int perf_phase1_failed_stabilize_retry_dir_second_chance_no_alt;
    int perf_phase1_failed_stabilize_retry_dir_second_chance_stabilized;
    int perf_phase1_failed_stabilize_retry_dir_second_chance_failed;
    int perf_phase1_failed_stabilize_retry_dir_guard_arms;
    int perf_phase1_failed_stabilize_retry_dir_guard_original_exclusions;
    int perf_phase1_window_pressure_windows_started;
    int perf_phase1_window_pressure_progress_resets;
    int perf_phase1_window_pressure_force_pivot_arms;
    int perf_phase1_window_pressure_force_pivot_blocked_pending;
    int perf_phase1_window_pressure_force_pivot_blocked_budget;
    int perf_phase1_window_pressure_force_pivot_reject_under_trigger;
    int perf_phase1_window_pressure_force_pivot_reject_failed_share;
    int perf_phase1_window_pressure_force_pivot_reject_dir_skip_share;
    int perf_phase1_window_pressure_force_pivot_reject_local_fail;
    int perf_phase1_window_pressure_force_pivot_reject_alternation;
    int perf_phase1_window_pressure_event_total;
    int perf_phase1_window_pressure_failed_stabilize_total;
    int perf_phase1_window_pressure_dir_skip_total;
    int perf_phase1_window_pressure_local_memory_fail_total;
    int perf_phase1_window_pressure_alternation_total;
    int perf_phase1_window_pressure_event_max;
    int perf_phase1_window_pressure_failed_stabilize_max;
    int perf_phase1_window_pressure_dir_skip_max;
    int perf_phase1_window_pressure_local_memory_fail_max;
    int perf_phase1_window_pressure_alternation_max;

    double perf_phase2_pricing_ms;
    double perf_phase2_ratio_ms;
    double perf_phase2_pivot_ms;
    double perf_phase2_refactor_ms;
    double perf_phase2_compute_solution_ms;
    double perf_phase2_compute_rc_ms;
    int perf_phase2_pricing_calls;
    int perf_phase2_ratio_calls;
    int perf_phase2_pivot_calls;
    int perf_phase2_refactor_calls;
    int perf_phase2_compute_solution_calls;
    int perf_phase2_compute_rc_calls;
    int perf_phase2_refactor_periodic_policy;
    int perf_phase2_refactor_periodic_lu_health;
    int perf_phase2_refactor_safety_forced;
    int perf_phase2_degenerate_episodes;
    int perf_phase2_degenerate_streak_max;
    int perf_phase2_theta_le_1e_9;
    int perf_phase2_theta_le_1e_6;
    int perf_phase2_theta_le_1e_3;
    int perf_phase2_theta_gt_1e_3;
    int perf_phase2_weak_pivot_samples;
    double perf_phase2_weak_pivot_ratio_total;
    double perf_phase2_weak_pivot_ratio_min;
    int perf_phase2_weak_pivot_ratio_le_1e_8;
    int perf_phase2_weak_pivot_ratio_le_1e_6;
    int perf_phase2_weak_pivot_ratio_le_1e_4;
    int perf_phase2_weak_pivot_ratio_gt_1e_4;
    int perf_phase2_repeat_entering_events;
    int perf_phase2_repeat_entering_max_streak;
    int perf_phase2_repeat_leaving_events;
    int perf_phase2_repeat_leaving_max_streak;
    int perf_phase2_bland_pricing_iters;
    int perf_phase2_adaptive_devex_partial_iters;
    int perf_phase2_bland_enter_episodes;
    int perf_phase2_bland_exit_episodes;
    int perf_phase2_perturb_applied;
    int perf_phase2_devex_reset_count;
    int perf_phase2_devex_age_max;
    int perf_phase2_degen_refactor_calls;
    int perf_phase2_degen_refactor_ratio_recovery;
    int perf_phase2_degen_refactor_pivot_recovery;
    int perf_phase2_degen_refactor_periodic_policy;
    int perf_phase2_degen_refactor_periodic_lu_health;
    int perf_phase2_degen_refactor_safety_forced;
    int perf_phase2_degen_escape_triggers;
    int perf_dual_ratio_no_entering;   /* dual ratio test failed to find entering variable */
    int perf_dual_theta_nonpositive;   /* dual ratio test returned theta <= 0 */
    int perf_dual_pivot_reject_small;  /* dual pivot rejected on small/non-finite pivot */
    int perf_dual_bound_flip_applied;  /* dual bound flips applied (startup + iterative mode) */
    int perf_dual_bound_flip_startup;  /* dual bound flips applied in startup dual-feasibility pass */
    int perf_dual_bound_flip_iterative;/* dual bound flips applied in iterative flip-ratio path */
    int perf_dual_lu_hard_trigger;     /* dual pivot path triggered hard LU recovery/refactor */

    /* Reinvert-controller shadow telemetry (Phase 2 scaffolding; no behavior change). */
    int perf_reinvert_shadow_checks_phase1;
    int perf_reinvert_shadow_checks_phase2;
    int perf_reinvert_shadow_checks_dual;
    int perf_reinvert_shadow_suggest_allow_phase1;
    int perf_reinvert_shadow_suggest_allow_phase2;
    int perf_reinvert_shadow_suggest_allow_dual;
    int perf_reinvert_shadow_suggest_defer_phase1;
    int perf_reinvert_shadow_suggest_defer_phase2;
    int perf_reinvert_shadow_suggest_defer_dual;
    int perf_reinvert_shadow_suggest_force_phase1;
    int perf_reinvert_shadow_suggest_force_phase2;
    int perf_reinvert_shadow_suggest_force_dual;
    int perf_reinvert_shadow_actual_refactor_yes_phase1;
    int perf_reinvert_shadow_actual_refactor_yes_phase2;
    int perf_reinvert_shadow_actual_refactor_yes_dual;
    int perf_reinvert_shadow_actual_refactor_no_phase1;
    int perf_reinvert_shadow_actual_refactor_no_phase2;
    int perf_reinvert_shadow_actual_refactor_no_dual;
    int perf_reinvert_shadow_disagree_phase1;
    int perf_reinvert_shadow_disagree_phase2;
    int perf_reinvert_shadow_disagree_dual;
    int perf_reinvert_shadow_last_reason_phase1;
    int perf_reinvert_shadow_last_reason_phase2;
    int perf_reinvert_shadow_last_reason_dual;
} LPSolverTelemetryState;

typedef struct {
    int nnz;
    const int *indices;
    const double *values;
    char sense;
    double rhs;
} LPAugmentRow;

typedef struct {
    double bias;
    int last_reason;
    int last_interval;
    int hint_interval;
    double hint_pressure;
} LPPeriodicFeedbackPhaseState;

typedef struct {
    int defers;
    int consecutive_defers;
    int defer_cap_forced;
    double refactor_cost_ewma;
    double iter_cost_ewma;
} LPSoftLUCostGatePhaseState;

typedef struct {
    int defers;
    int consecutive_defers;
    int defer_cap_forced;
    int checks;
    int block_small_m;
    int block_invalid_inputs;
    int block_warmup;
    int block_invalid_cost;
    int block_ratio;
    int block_update_reserve;
    int last_reason;
    int iter_samples;
    int refactor_samples;
} LPPeriodicCostGatePhaseState;

typedef struct {
    int refactors;
} LPPeriodicPolicyPhaseState;

typedef struct {
    double last_hot_ms;
    int control_demoted;
    int control_demotions;
    int hard_trigger_last_total;
    int hard_trigger_last_iter;
    int hard_trigger_burst;
} LPDualReinvertPolicyState;

typedef struct {
    int control_demoted;
    int control_demotions;
    int pressure_last_iter;
    int pressure_burst;
} LPPhase1ReinvertPolicyState;

typedef struct {
    int window_start_iter;
    double window_start_obj;
    int window_retry_base;
    int window_no_pivot_base;
    int window_refactor_base;
    int window_update_recovery_base;
    int window_recompute_ratio_base;
    int window_recompute_dir_skip_base;
    int window_recompute_dir_refactor_base;
    int window_recompute_pivot_fail_base;
    int window_recompute_perturb_base;
    int escape_cooldown;
    int escape_triggers;
    int escape_successes;
    int escape_failures;
    int escape_cooldown_blocks;
    int last_window_iters;
    double last_obj_delta;
    double last_retry_defer_ratio;
    double last_update_recovery_ratio;
    int last_retry_defers;
    int last_no_pivot_events;
    int last_update_recovery_refactors;
    int last_refactors;
    int last_recompute_ratio;
    int last_recompute_dir_skip;
    int last_recompute_dir_refactor;
    int last_recompute_pivot_fail;
    int last_recompute_perturb;
} LPPhase1StagnationPolicyState;

/* Runtime-configurable refactoring policy parameters (R2).
 * Replaces compile-time #define constants in lp_refactor_policy.c.
 * Defaults match the existing #define values for behavioral equivalence. */
typedef struct {
    int phase1_refactor_min_interval;
    int phase1_refactor_max_interval;
    int phase2_refactor_min_interval;
    int phase2_refactor_max_interval;
    double refactor_pressure_trigger;
    int periodic_min_update_age;
    int degen_escape_min_m;
    int degen_escape_trigger;
    int phase1_auto_dantzig_min_m;
    double lu_cost_ewma_alpha;
    int lu_max_consec_defer_phase1;
    int lu_max_consec_defer_phase2;
    double lu_cost_gate_ratio;
    int lu_spike_warn_pct;
    int no_pivot_progress_window;
    int phase1_stall_threshold_default;
    int phase2_degen_escape_policy_trigger;
    double feedback_decay;
    double feedback_relax_step;
    double feedback_tighten_step;
} LPRefactorPolicyConfig;

void lp_refactor_policy_config_defaults(LPRefactorPolicyConfig *cfg);

/* Solver policy state (behavioral scheduling/control, not telemetry). */
typedef struct {
    int refactor_next_reason;  /* RalphRefactorReason hint consumed by tableau_refactorize */
    int basis_governor_mode;   /* LPBasisGovernorMode runtime mode */
    int reinvert_controller_mode; /* LPReinvertControllerMode runtime mode */
    LPBasisGovernorState basis_governor; /* Shadow governor state (G0) */

    /* Runtime scheduling counters. */
    LPPeriodicPolicyPhaseState periodic_policy_phase1;
    LPPeriodicPolicyPhaseState periodic_policy_phase2;

    /* Dual scheduling cadence (runtime-tunable; replaces hardcoded literals). */
    int dual_refactor_base_interval;
    int dual_rc_recompute_interval;

    /* Adaptive periodic scheduler feedback, composed by simplex phase. */
    LPPeriodicFeedbackPhaseState periodic_feedback_phase1;
    LPPeriodicFeedbackPhaseState periodic_feedback_phase2;

    /* Phase E: soft LU-health refactor cost gating (behavioral, not telemetry). */
    int soft_lu_cost_gate_enabled;        /* 1=enabled (default), 0=disabled */
    LPSoftLUCostGatePhaseState soft_lu_cost_gate_phase1;
    LPSoftLUCostGatePhaseState soft_lu_cost_gate_phase2;
    int periodic_cost_gate_enabled;       /* 1=enabled (default), 0=disabled */
    LPPeriodicCostGatePhaseState periodic_cost_gate_phase1;
    LPPeriodicCostGatePhaseState periodic_cost_gate_phase2;

    /* Unified reinversion controller state (shadow-only in Phase 2). */
    LPReinvertControllerState reinvert_state_phase1;
    LPReinvertControllerState reinvert_state_phase2;
    LPReinvertControllerState reinvert_state_dual;
    LPDualReinvertPolicyState reinvert_dual;
    LPPhase1ReinvertPolicyState reinvert_phase1;
    LPPhase1StagnationPolicyState phase1_stagnation;
} LPSolverPolicyState;

/* Explicit solver phase tracking (R0.1) */
typedef enum {
    SIMPLEX_PHASE_INIT = 0,
    SIMPLEX_PHASE_1,
    SIMPLEX_PHASE_TRANSITION,
    SIMPLEX_PHASE_2,
    SIMPLEX_PHASE_OPTIMAL,
    SIMPLEX_PHASE_INFEASIBLE,
    SIMPLEX_PHASE_UNBOUNDED,
    SIMPLEX_PHASE_ERROR
} SimplexPhase;

/* Simplex solver */
typedef struct SimplexSolver {
    LPModel *model;
    SimplexTableau *tableau;

    /* Parameters */
    int max_iterations;
    double time_limit;
    int presolve;
    int scaling;
    int pricing_strategy;   /* 0=Dantzig, 1=Steepest edge, 2=Devex, 3=Partial, 4=Heap */
    int verbose;
    int telemetry_enabled;  /* 1 = collect solver/LU telemetry counters/timers */
    int force_two_phase;    /* 1 = force two-phase simplex (for Benders duals) */
    int crash;              /* 0=off, 1=triangular crash basis */
    int verify;             /* 0=off, 1=post-solve verification (T2.3) */
    int method;             /* 0=primal, 1=dual-only, 2=dual-first fallback lane */
    int ratio_test_mode;    /* 0=standard ratio, 1=Harris ratio (default) */
    int dual_ratio_test_mode; /* 0=standard, 1=Harris, 2=iterative bound-flip mode */
    double objective_limit; /* Early-exit when obj >= limit (internal min space), default RALPH_INFINITY */
    int phase1_pricing;     /* Override pricing for Phase 1: 0=Dantzig, -1=disabled (use solver pricing) */
    int trace_phase1;       /* 1 = emit deterministic Phase-1 pivot-failure trace */
    int deterministic;      /* 1 = enforce deterministic LP runtime policy */
    unsigned int random_seed; /* Seed used by deterministic anti-cycling perturbation offsets */
    int lp_threads;         /* LP thread policy (0 = auto; deterministic mode defaults to 1) */
    int determinism_effective_threads; /* Runtime-applied LP thread count (0 = runtime default) */
    RalphLPProgressCallback lp_progress_callback; /* LP-only progress callback */
    int has_lp_progress_callback; /* 1 if lp_progress_callback is active */
    RalphLPCancelCallback lp_cancel_callback; /* LP-only cancellation poll callback */
    int has_lp_cancel_callback; /* 1 if lp_cancel_callback is active */
    double progress_start_ms; /* Solve start wall-clock in ms for callback elapsed time */
    int warm_basis_m;       /* Staged warm-start basis rows (constraints) */
    int warm_basis_n;       /* Staged warm-start basis cols (extended vars) */
    int *warm_basis;        /* Staged basic variable indices (size warm_basis_m) */
    VarStatus *warm_var_status; /* Staged variable statuses (size warm_basis_n) */
    int warm_basis_last_attempted; /* 1 if last simplex_solve attempted warm basis apply */
    int warm_basis_last_applied;   /* 1 if last simplex_solve accepted staged warm basis */
    int warm_basis_last_rejected;  /* 1 if last simplex_solve rejected staged warm basis */

    /* Scaling factors (used if scaling enabled) */
    double *row_scale;      /* Row scaling factors */
    double *col_scale;      /* Column scaling factors */
    int is_scaled;          /* Flag indicating if problem was scaled */

    /* Solution */
    RalphStatus status;
    double obj_value;
    double *solution;
    double *dual_solution;
    double *reduced_costs;

    /* Statistics */
    int iterations;
    double solve_time;
    int degenerate_pivots;
    LPSolverTelemetryState telemetry;
    LPSolverPolicyState policy;
    LPRefactorPolicyConfig refactor_config;

    /* Post-solve verification metrics (T2.3 + T3.6) */
    double verify_primal_infeas;    /* ||Ax - b||_inf for satisfied constraints */
    double verify_bound_infeas;     /* max bound violation */
    double verify_dual_infeas;      /* max dual feasibility violation */
    double verify_comp_slack;       /* max complementary slackness violation */
    double verify_obj_error;        /* |recomputed_obj - reported_obj| / max(1, |obj|) */
    double verify_cond_estimate;    /* Basis condition number from LU */

    /* Phase-1 trace summary */
    int trace_phase1_pivot_failures;
    int trace_phase1_fail_small_pivot;
    int trace_phase1_fail_invalid_column;
    int trace_phase1_fail_lu_max_updates;
    int trace_phase1_fail_lu_spike_pool_full;
    int trace_phase1_fail_lu_update_pivot_small;
    int trace_phase1_fail_lu_singular_update;
    int trace_phase1_fail_factor_singular;
    int trace_phase1_fail_refactor_forced_other;
    int trace_phase1_fail_refactor_after_update_other;
    int trace_phase1_no_entering_events;
    int trace_phase1_first_fail_iter;
    int trace_phase1_last_fail_iter;
    unsigned long long trace_phase1_signature;

    /* Farkas ray (certificate of infeasibility) */
    double *farkas_ray;     /* Size num_cons, valid when status == INFEASIBLE */
    int farkas_ray_capacity;
    int farkas_valid;       /* 1 if farkas_ray contains valid certificate */
    double *unbounded_ray;  /* Size num_vars, valid when status == UNBOUNDED */
    int unbounded_ray_capacity;
    int unbounded_valid;    /* 1 if unbounded_ray contains a valid direction */

    /* Dual simplex enhancements */
    int use_dual_bound_flip;    /* 0=off, 1=on (default 1) */
    int use_dual_steepest_edge; /* 0=off, 1=on (default 1) */
    int glpk_strict_mode;       /* 0=off (default), 1=strict GLPK control semantics */
    double smcp_tol_bnd;        /* GLPK-like primal feasibility tolerance */
    double smcp_tol_dj;         /* GLPK-like dual feasibility tolerance */
    double smcp_tol_piv;        /* GLPK-like pivot tolerance */
    int smcp_excl;              /* GLPK-like fixed non-basic exclusion mode */
    int smcp_shift;             /* GLPK-like bound-shift mode */
    int smcp_aorn;              /* GLPK-like row-wise matrix option (A^T / N^T) */

    /* Supernodal LU (T2.1) — propagated to LU after tableau creation */
    int lu_supernode;           /* 0=off, 1=enable */
    int lu_factorization_type;  /* 0=luf, 1=btf */
    int lu_backend_policy;      /* -1=auto/default, 0=luf_ft, 1=cbg, 2=cgr */
    int lu_update_limit_override; /* <=0 => use LU default */
    double lu_pivot_tol_override; /* <=0 => use LU default */
    double lu_growth_guard_override; /* <=0 => use default growth threshold */
    int lu_strict_lane_active;  /* 1 = strict LU/BFCP dispatch lane active */
    int lu_strict_prefer_dense_ge_numeric; /* 1 = strict lane skips Markowitz */
    int lu_strict_allow_supernode_lane;      /* 0 disables Ralph supernode lane */
    int lu_strict_allow_symbolic_full_retry; /* 0 disables full-structural sparse retry */
    int lu_strict_allow_top_level_dense_fallback; /* 0 disables lu_factorize dense fallback */

    /* D4: Flag set when primal runs after dual fallback (known degenerate) */
    int from_dual_fallback;     /* 1 = arrived from failed dual simplex */

    /* R0.1: Explicit solver phase tracking */
    SimplexPhase current_phase; /* Current solver phase (updated at each transition) */

} SimplexSolver;

/* Solver-level telemetry snapshot used by benchmarks and diagnostics. */
typedef struct {
    double perf_primal_setup_ms;
    double perf_dual_ms;
    double perf_phase1_ms;
    double perf_transition_ms;
    double perf_phase2_ms;
    double perf_pricing_ms;
    double perf_ratio_ms;
    double perf_pivot_ms;
    double perf_refactor_ms;
    double perf_ftran_ms;
    double perf_btran_ms;
    double perf_ftran_base_ms;
    double perf_ftran_update_apply_ms;
    int perf_ftran_update_apply_calls;
    double perf_btran_base_ms;
    double perf_btran_update_apply_ms;
    int perf_btran_update_apply_calls;
    int perf_ftran_calls;
    int perf_btran_calls;
    int perf_ftran_nnz_samples;
    int perf_btran_nnz_samples;
    long long perf_ftran_rhs_nnz_total;
    long long perf_ftran_sol_nnz_total;
    long long perf_btran_rhs_nnz_total;
    long long perf_btran_sol_nnz_total;
    double perf_lu_update_ms;
    double perf_compute_solution_ms;
    double perf_compute_rc_ms;
    double perf_refactor_all_ms;
    int perf_refactor_count;
    double perf_refactor_last_ms;
    double perf_refactor_max_ms;
    int perf_refactor_last_reason;
    int perf_refactor_next_reason;
    int perf_refactor_reason_setup;
    int perf_refactor_reason_transition;
    int perf_refactor_reason_periodic;
    int perf_refactor_reason_ratio_recovery;
    int perf_refactor_reason_pivot_recovery;
    int perf_refactor_reason_forced_small_pivot;
    int perf_refactor_reason_update_recovery;
    int perf_refactor_reason_direction_stabilize;
    int perf_refactor_reason_infeas_cleanup;
    int perf_refactor_reason_other;
    int perf_refactor_periodic_policy;
    int perf_refactor_periodic_lu_health;
    int perf_refactor_safety_forced;
    int perf_basis_fastpath_hits;
    int perf_basis_cols_rewritten;
    unsigned long long perf_basis_tail_shift_bytes;
    int perf_refactor_last_m;
    int perf_refactor_last_k;
    int perf_refactor_last_nnz_B;

    double perf_phase1_pricing_ms;
    double perf_phase1_ratio_ms;
    double perf_phase1_pivot_ms;
    double perf_phase1_refactor_ms;
    double perf_phase1_compute_solution_ms;
    double perf_phase1_compute_rc_ms;
    int perf_phase1_pricing_calls;
    int perf_phase1_ratio_calls;
    int perf_phase1_pivot_calls;
    int perf_phase1_refactor_calls;
    int perf_phase1_compute_solution_calls;
    int perf_phase1_compute_rc_calls;
    int perf_phase1_compute_solution_ctx_other;
    int perf_phase1_compute_solution_ctx_recompute_full;
    int perf_phase1_compute_solution_ctx_recompute_guard_forced_full;
    int perf_phase1_compute_solution_ctx_init;
    int perf_phase1_compute_solution_ctx_no_entering_cleanup;
    int perf_phase1_compute_solution_ctx_infeas_cleanup;
    int perf_phase1_compute_solution_ctx_refactor_fail_continue;
    int perf_phase1_compute_solution_ctx_refactor_failure_recovery;
    int perf_phase1_compute_solution_ctx_refactor_success;
    int perf_phase1_compute_solution_ctx_drift_refresh;
    int perf_phase1_compute_solution_ctx_dual_rescue;
    int perf_phase1_compute_rc_ctx_other;
    int perf_phase1_compute_rc_ctx_recompute_full;
    int perf_phase1_compute_rc_ctx_recompute_rc_only;
    int perf_phase1_compute_rc_ctx_recompute_guard_forced_full;
    int perf_phase1_compute_rc_ctx_init;
    int perf_phase1_compute_rc_ctx_infeas_cleanup;
    int perf_phase1_compute_rc_ctx_refactor_fail_continue;
    int perf_phase1_compute_rc_ctx_refactor_failure_recovery;
    int perf_phase1_compute_rc_ctx_refactor_success;
    int perf_phase1_compute_rc_ctx_drift_refresh;
    int perf_phase1_compute_rc_ctx_dual_rescue;
    int perf_phase1_entering_exclusions;
    int perf_phase1_entering_exclusion_repeats;
    int perf_phase1_entering_exclusion_hits;
    int perf_phase1_entering_exclusion_reroutes;
    int perf_phase1_entering_exclusion_no_alt;
    int perf_phase1_refactor_periodic_policy;
    int perf_phase1_refactor_periodic_lu_health;
    int perf_phase1_refactor_safety_forced;
    int perf_phase1_dir_stabilize_force_extreme_dir;
    int perf_phase1_dir_stabilize_force_lu_health;
    int perf_phase1_dir_stabilize_cooldown_candidates;
    int perf_phase1_dir_stabilize_ratio_le_3;
    int perf_phase1_dir_stabilize_ratio_le_10;
    int perf_phase1_dir_stabilize_ratio_le_30;
    int perf_phase1_dir_stabilize_ratio_le_100;
    int perf_phase1_dir_stabilize_ratio_gt_100;
    int perf_phase1_dir_stabilize_ratio_gt_300;
    int perf_phase1_dir_stabilize_ratio_gt_1000;
    int perf_phase1_dir_stabilize_skip_rc_only;
    int perf_phase1_dir_stabilize_skip_full;
    int perf_phase1_dir_stabilize_skip_no_recompute;
    int perf_phase1_dir_stabilize_skip_guard_refresh;
    int perf_phase1_dir_stabilize_escape_gate_triggers;
    int perf_phase1_dir_stabilize_escape_gate_suppressed_lu_health;
    int perf_phase1_dir_stabilize_escape_gate_suppressed_force_pivot_mode;
    int perf_phase1_dir_stabilize_escape_gate_hard_bypass;
    int perf_phase1_dir_stabilize_refactor_from_no_pivot_force;
    int perf_phase1_dir_stabilize_refactor_from_force_extreme_dir;
    int perf_phase1_dir_stabilize_refactor_from_force_lu_health;
    int perf_phase1_dir_stabilize_refactor_from_force_pivot_mode;
    int perf_phase1_dir_stabilize_refactor_from_ladder_force;
    int perf_phase1_force_pivot_budget_dir_event_seen;
    int perf_phase1_force_pivot_budget_pivot_spend;
    int perf_phase1_force_pivot_relax_applied;
    int perf_phase1_force_extreme_relax_applied;
    int perf_phase1_force_extreme_bound_flip_relax_applied;
    int perf_phase1_force_extreme_catastrophic_tiny_theta_relax_applied;
    int perf_phase1_force_extreme_tiny_theta_relax_applied;
    int perf_phase1_force_extreme_tiny_theta_relax_post_dir_skip_retry;
    int perf_phase1_force_extreme_tiny_theta_relax_post_dir_skip_dual_rescue;
    int perf_phase1_force_extreme_tiny_theta_relax_post_dir_skip_forced_refactor;
    int perf_phase1_force_extreme_tiny_theta_relax_refactor_force_lu_health;
    int perf_phase1_force_extreme_tiny_theta_relax_refactor_force_pivot_mode;
    int perf_phase1_force_extreme_tiny_theta_relax_refactor_ladder_force;
    int perf_phase1_force_extreme_tiny_theta_relax_next_failed_stabilize;
    int perf_phase1_force_extreme_tiny_theta_relax_next_ratio_breakdown;
    int perf_phase1_force_extreme_tiny_theta_relax_next_pivot_fail;
    int perf_phase1_force_extreme_tiny_theta_relax_next_pivot_success;
    int perf_phase1_recompute_after_ratio_breakdown;
    int perf_phase1_recompute_after_dir_skip;
    int perf_phase1_recompute_after_dir_refactor;
    int perf_phase1_recompute_after_pivot_fail_recovery;
    int perf_phase1_recompute_after_perturb;
    int perf_phase1_recompute_rc_only_calls;
    int perf_phase1_recompute_rc_guard_forced_full;
    int perf_phase1_ratio_breakdown_retries;
    int perf_phase1_ratio_breakdown_escalations;
    int perf_phase1_pivot_fail_recovery_exclusions;
    int perf_phase1_no_pivot_events;
    int perf_phase1_no_pivot_forced_refactor;
    int perf_phase1_no_pivot_forced_ratio_breakdown;
    int perf_phase1_no_pivot_forced_dir_skip;
    int perf_phase1_no_pivot_forced_pivot_fail;
    int perf_phase1_no_pivot_events_ratio_breakdown;
    int perf_phase1_no_pivot_events_dir_skip;
    int perf_phase1_no_pivot_events_pivot_fail;
    int perf_phase1_no_pivot_no_progress_events;
    int perf_phase1_no_pivot_ladder_retry_defers;
    int perf_phase1_no_pivot_ladder_retry_ratio_breakdown;
    int perf_phase1_no_pivot_ladder_retry_dir_skip;
    int perf_phase1_no_pivot_ladder_retry_pivot_fail;
    int perf_phase1_no_pivot_ladder_dual_rescue_attempts;
    int perf_phase1_no_pivot_ladder_dual_rescue_successes;
    int perf_phase1_no_pivot_ladder_dual_rescue_failures;
    int perf_phase1_no_pivot_ladder_dual_rescue_attempts_ratio_breakdown;
    int perf_phase1_no_pivot_ladder_dual_rescue_attempts_dir_skip;
    int perf_phase1_no_pivot_ladder_dual_rescue_attempts_pivot_fail;
    int perf_phase1_no_pivot_ladder_forced_refactors;
    int perf_phase1_no_pivot_ladder_forced_refactors_ratio_breakdown;
    int perf_phase1_no_pivot_ladder_forced_refactors_dir_skip;
    int perf_phase1_no_pivot_ladder_forced_refactors_pivot_fail;
    int perf_phase1_no_pivot_ladder_rescue_guard_cooldown_blocks;
    int perf_phase1_no_pivot_ladder_rescue_guard_fail_cap_forces;
    int perf_phase1_direct_dual_rescue_attempts;
    int perf_phase1_direct_dual_rescue_successes;
    int perf_phase1_direct_dual_rescue_failures;
    int perf_phase1_direct_dual_rescue_guard_cooldown_blocks;
    int perf_phase1_direct_dual_rescue_guard_fail_cap_blocks;
    int perf_phase1_soft_lu_policy_cooldown_defers;
    int perf_phase1_dir_skip_same_entering_repeats;
    int perf_phase1_dir_skip_same_entering_max_streak;
    int perf_phase1_failed_stabilize_events;
    int perf_phase1_failed_stabilize_primary_failures;
    int perf_phase1_failed_stabilize_alternate_failures;
    int perf_phase1_failed_stabilize_same_entering_repeats;
    int perf_phase1_failed_stabilize_same_entering_max_streak;
    int perf_phase1_failed_stabilize_retry_penalty_arms;
    int perf_phase1_failed_stabilize_retry_penalty_alt_found;
    int perf_phase1_failed_stabilize_retry_penalty_no_alt;
    int perf_phase1_failed_stabilize_retry_penalty_alt_stabilized;
    int perf_phase1_failed_stabilize_retry_penalty_alt_failed;
    int perf_phase1_failed_stabilize_retry_penalty_same_alt_repeats;
    int perf_phase1_failed_stabilize_retry_penalty_same_alt_max_streak;
    int perf_phase1_failed_stabilize_retry_local_memory_arms;
    int perf_phase1_failed_stabilize_retry_local_memory_alt_found;
    int perf_phase1_failed_stabilize_retry_local_memory_no_alt;
    int perf_phase1_failed_stabilize_retry_local_memory_fallback_same_alt;
    int perf_phase1_failed_stabilize_retry_local_memory_alt_stabilized;
    int perf_phase1_failed_stabilize_retry_local_memory_alt_failed;
    int perf_phase1_failed_stabilize_retry_pool_samples;
    int perf_phase1_failed_stabilize_retry_pool_eligible_total;
    int perf_phase1_failed_stabilize_retry_pool_eligible_max;
    int perf_phase1_failed_stabilize_retry_pool_singleton_samples;
    int perf_phase1_failed_stabilize_retry_pool_best_differs_samples;
    int perf_phase1_failed_stabilize_retry_selector_eval_samples;
    int perf_phase1_failed_stabilize_retry_selector_eval_best_differs_samples;
    double perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_total;
    double perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_max;
    int perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_2;
    int perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_4;
    int perf_phase1_failed_stabilize_retry_shadow_samples;
    int perf_phase1_failed_stabilize_retry_shadow_ratio_failed;
    int perf_phase1_failed_stabilize_retry_shadow_dir_stable;
    int perf_phase1_failed_stabilize_retry_shadow_dir_failed;
    int perf_phase1_failed_stabilize_retry_shadow_dir_nnz_total;
    int perf_phase1_failed_stabilize_retry_shadow_dir_nnz_max;
    double perf_phase1_failed_stabilize_retry_shadow_dir_inf_total;
    double perf_phase1_failed_stabilize_retry_shadow_dir_inf_max;
    double perf_phase1_failed_stabilize_retry_shadow_pivot_abs_total;
    double perf_phase1_failed_stabilize_retry_shadow_pivot_abs_max;
    int perf_phase1_failed_stabilize_retry_shadow_guard_arms;
    int perf_phase1_failed_stabilize_retry_shadow_guard_original_exclusions;
    int perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_retry;
    int perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_dual_rescue;
    int perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_forced_refactor;
    int perf_phase1_failed_stabilize_retry_shadow_next_failed_stabilize;
    int perf_phase1_failed_stabilize_retry_shadow_next_ratio_breakdown;
    int perf_phase1_failed_stabilize_retry_shadow_next_pivot_fail;
    int perf_phase1_failed_stabilize_retry_shadow_next_pivot_success;
    int perf_phase1_force_extreme_followup_stabilized;
    int perf_phase1_force_extreme_followup_ratio_breakdown;
    int perf_phase1_force_extreme_followup_failed_stabilize;
    int perf_phase1_force_extreme_followup_post_dir_skip_retry;
    int perf_phase1_force_extreme_followup_post_dir_skip_dual_rescue;
    int perf_phase1_force_extreme_followup_post_dir_skip_forced_refactor;
    int perf_phase1_force_extreme_followup_next_failed_stabilize;
    int perf_phase1_force_extreme_followup_next_ratio_breakdown;
    int perf_phase1_force_extreme_followup_next_pivot_fail;
    int perf_phase1_force_extreme_followup_next_pivot_success;
    int perf_phase1_force_extreme_followup_dir_samples;
    int perf_phase1_force_extreme_followup_dir_bound_geometry;
    int perf_phase1_force_extreme_followup_dir_bound_flip;
    int perf_phase1_force_extreme_followup_dir_tiny_theta;
    int perf_phase1_force_extreme_followup_dir_weak_leaving;
    int perf_phase1_force_extreme_followup_dir_ftran_shape;
    int perf_phase1_force_extreme_followup_dir_nnz_total;
    int perf_phase1_force_extreme_followup_dir_nnz_max;
    double perf_phase1_force_extreme_followup_dir_inf_total;
    double perf_phase1_force_extreme_followup_dir_inf_max;
    double perf_phase1_force_extreme_followup_pivot_abs_total;
    double perf_phase1_force_extreme_followup_pivot_abs_max;
    double perf_phase1_force_extreme_followup_theta_total;
    double perf_phase1_force_extreme_followup_theta_max;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_samples;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_bound_geometry;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_bound_flip;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_tiny_theta;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_weak_leaving;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_ftran_shape;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_nnz_total;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_nnz_max;
    double perf_phase1_failed_stabilize_retry_shadow_followup_dir_inf_total;
    double perf_phase1_failed_stabilize_retry_shadow_followup_dir_inf_max;
    double perf_phase1_failed_stabilize_retry_shadow_followup_pivot_abs_total;
    double perf_phase1_failed_stabilize_retry_shadow_followup_pivot_abs_max;
    double perf_phase1_failed_stabilize_retry_shadow_followup_theta_total;
    double perf_phase1_failed_stabilize_retry_shadow_followup_theta_max;
    int perf_phase1_failed_stabilize_retry_selector_bland_arms;
    int perf_phase1_failed_stabilize_retry_selector_guarded_arms;
    int perf_phase1_failed_stabilize_retry_selector_guarded_eligible_total;
    int perf_phase1_failed_stabilize_retry_selector_guarded_eligible_max;
    int perf_phase1_failed_stabilize_retry_selector_bland_alt_stabilized;
    int perf_phase1_failed_stabilize_retry_selector_bland_alt_failed;
    int perf_phase1_failed_stabilize_retry_selector_bland_ratio_failed;
    int perf_phase1_failed_stabilize_retry_selector_bland_dir_failed;
    int perf_phase1_failed_stabilize_retry_selector_guarded_alt_stabilized;
    int perf_phase1_failed_stabilize_retry_selector_guarded_alt_failed;
    int perf_phase1_failed_stabilize_retry_selector_guarded_ratio_failed;
    int perf_phase1_failed_stabilize_retry_selector_guarded_dir_failed;
    int perf_phase1_failed_stabilize_retry_selector_guarded_fallback_to_bland;
    int perf_phase1_failed_stabilize_retry_dir_fail_shape_samples;
    int perf_phase1_failed_stabilize_retry_dir_fail_nnz_total;
    int perf_phase1_failed_stabilize_retry_dir_fail_nnz_max;
    double perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_total;
    double perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_max;
    double perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_total;
    double perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_max;
    int perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_30;
    int perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_100;
    int perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_1000;
    int perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_gt_1000;
    int perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_8;
    int perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_6;
    int perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_4;
    int perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_gt_1e_4;
    int perf_phase1_failed_stabilize_retry_dir_second_chance_arms;
    int perf_phase1_failed_stabilize_retry_dir_second_chance_no_alt;
    int perf_phase1_failed_stabilize_retry_dir_second_chance_stabilized;
    int perf_phase1_failed_stabilize_retry_dir_second_chance_failed;
    int perf_phase1_failed_stabilize_retry_dir_guard_arms;
    int perf_phase1_failed_stabilize_retry_dir_guard_original_exclusions;
    int perf_phase1_window_pressure_windows_started;
    int perf_phase1_window_pressure_progress_resets;
    int perf_phase1_window_pressure_force_pivot_arms;
    int perf_phase1_window_pressure_force_pivot_blocked_pending;
    int perf_phase1_window_pressure_force_pivot_blocked_budget;
    int perf_phase1_window_pressure_force_pivot_reject_under_trigger;
    int perf_phase1_window_pressure_force_pivot_reject_failed_share;
    int perf_phase1_window_pressure_force_pivot_reject_dir_skip_share;
    int perf_phase1_window_pressure_force_pivot_reject_local_fail;
    int perf_phase1_window_pressure_force_pivot_reject_alternation;
    int perf_phase1_window_pressure_event_total;
    int perf_phase1_window_pressure_failed_stabilize_total;
    int perf_phase1_window_pressure_dir_skip_total;
    int perf_phase1_window_pressure_local_memory_fail_total;
    int perf_phase1_window_pressure_alternation_total;
    int perf_phase1_window_pressure_event_max;
    int perf_phase1_window_pressure_failed_stabilize_max;
    int perf_phase1_window_pressure_dir_skip_max;
    int perf_phase1_window_pressure_local_memory_fail_max;
    int perf_phase1_window_pressure_alternation_max;

    double perf_phase2_pricing_ms;
    double perf_phase2_ratio_ms;
    double perf_phase2_pivot_ms;
    double perf_phase2_refactor_ms;
    double perf_phase2_compute_solution_ms;
    double perf_phase2_compute_rc_ms;
    int perf_phase2_pricing_calls;
    int perf_phase2_ratio_calls;
    int perf_phase2_pivot_calls;
    int perf_phase2_refactor_calls;
    int perf_phase2_compute_solution_calls;
    int perf_phase2_compute_rc_calls;
    int perf_phase2_refactor_periodic_policy;
    int perf_phase2_refactor_periodic_lu_health;
    int perf_phase2_refactor_safety_forced;
    int perf_phase2_degenerate_episodes;
    int perf_phase2_degenerate_streak_max;
    int perf_phase2_theta_le_1e_9;
    int perf_phase2_theta_le_1e_6;
    int perf_phase2_theta_le_1e_3;
    int perf_phase2_theta_gt_1e_3;
    int perf_phase2_weak_pivot_samples;
    double perf_phase2_weak_pivot_ratio_total;
    double perf_phase2_weak_pivot_ratio_min;
    int perf_phase2_weak_pivot_ratio_le_1e_8;
    int perf_phase2_weak_pivot_ratio_le_1e_6;
    int perf_phase2_weak_pivot_ratio_le_1e_4;
    int perf_phase2_weak_pivot_ratio_gt_1e_4;
    int perf_phase2_repeat_entering_events;
    int perf_phase2_repeat_entering_max_streak;
    int perf_phase2_repeat_leaving_events;
    int perf_phase2_repeat_leaving_max_streak;
    int perf_phase2_bland_pricing_iters;
    int perf_phase2_adaptive_devex_partial_iters;
    int perf_phase2_bland_enter_episodes;
    int perf_phase2_bland_exit_episodes;
    int perf_phase2_perturb_applied;
    int perf_phase2_devex_reset_count;
    int perf_phase2_devex_age_max;
    int perf_phase2_degen_refactor_calls;
    int perf_phase2_degen_refactor_ratio_recovery;
    int perf_phase2_degen_refactor_pivot_recovery;
    int perf_phase2_degen_refactor_periodic_policy;
    int perf_phase2_degen_refactor_periodic_lu_health;
    int perf_phase2_degen_refactor_safety_forced;
    int perf_phase2_degen_escape_triggers;
    int perf_dual_ratio_no_entering;
    int perf_dual_theta_nonpositive;
    int perf_dual_pivot_reject_small;
    int perf_dual_bound_flip_applied;
    int perf_dual_bound_flip_startup;
    int perf_dual_bound_flip_iterative;
    int perf_dual_lu_hard_trigger;

    double periodic_feedback_bias_phase1;
    double periodic_feedback_bias_phase2;
    int periodic_feedback_last_reason_phase1;
    int periodic_feedback_last_reason_phase2;
    int periodic_feedback_last_interval_phase1;
    int periodic_feedback_last_interval_phase2;
    int periodic_feedback_hint_interval_phase1;
    int periodic_feedback_hint_interval_phase2;
    double periodic_feedback_hint_pressure_phase1;
    double periodic_feedback_hint_pressure_phase2;
    int soft_lu_cost_gate_enabled;
    int soft_lu_cost_gate_defers_phase1;
    int soft_lu_cost_gate_defers_phase2;
    int soft_lu_consecutive_defers_phase1;
    int soft_lu_consecutive_defers_phase2;
    int soft_lu_defer_cap_forced_phase1;
    int soft_lu_defer_cap_forced_phase2;
    int periodic_cost_gate_enabled;
    int periodic_cost_gate_defers_phase1;
    int periodic_cost_gate_defers_phase2;
    int periodic_cost_consecutive_defers_phase1;
    int periodic_cost_consecutive_defers_phase2;
    int periodic_cost_defer_cap_forced_phase1;
    int periodic_cost_defer_cap_forced_phase2;
    int periodic_cost_gate_checks_phase1;
    int periodic_cost_gate_checks_phase2;
    int periodic_cost_gate_block_small_m_phase1;
    int periodic_cost_gate_block_small_m_phase2;
    int periodic_cost_gate_block_invalid_inputs_phase1;
    int periodic_cost_gate_block_invalid_inputs_phase2;
    int periodic_cost_gate_block_warmup_phase1;
    int periodic_cost_gate_block_warmup_phase2;
    int periodic_cost_gate_block_invalid_cost_phase1;
    int periodic_cost_gate_block_invalid_cost_phase2;
    int periodic_cost_gate_block_ratio_phase1;
    int periodic_cost_gate_block_ratio_phase2;
    int periodic_cost_gate_block_update_reserve_phase1;
    int periodic_cost_gate_block_update_reserve_phase2;
    int periodic_cost_gate_last_reason_phase1;
    int periodic_cost_gate_last_reason_phase2;
    int periodic_cost_iter_samples_phase1;
    int periodic_cost_iter_samples_phase2;
    int periodic_cost_refactor_samples_phase1;
    int periodic_cost_refactor_samples_phase2;
    double soft_lu_refactor_cost_ewma_phase1;
    double soft_lu_refactor_cost_ewma_phase2;
    double soft_lu_iter_cost_ewma_phase1;
    double soft_lu_iter_cost_ewma_phase2;
    int basis_governor_mode;
    int reinvert_controller_mode;
    int reinvert_dual_control_demoted;
    int reinvert_dual_control_demotions;
    int reinvert_dual_hard_trigger_last_total;
    int reinvert_dual_hard_trigger_last_iter;
    int reinvert_dual_hard_trigger_burst;
    int reinvert_phase1_control_demoted;
    int reinvert_phase1_control_demotions;
    int reinvert_phase1_pressure_last_iter;
    int reinvert_phase1_pressure_burst;
    int phase1_stagnation_escape_cooldown;
    int phase1_stagnation_escape_triggers;
    int phase1_stagnation_escape_successes;
    int phase1_stagnation_escape_failures;
    int phase1_stagnation_escape_cooldown_blocks;
    int phase1_stagnation_last_window_iters;
    double phase1_stagnation_last_obj_delta;
    double phase1_stagnation_last_retry_defer_ratio;
    double phase1_stagnation_last_update_recovery_ratio;
    int phase1_stagnation_last_retry_defers;
    int phase1_stagnation_last_no_pivot_events;
    int phase1_stagnation_last_update_recovery_refactors;
    int phase1_stagnation_last_refactors;
    int phase1_stagnation_last_recompute_ratio;
    int phase1_stagnation_last_recompute_dir_skip;
    int phase1_stagnation_last_recompute_dir_refactor;
    int phase1_stagnation_last_recompute_pivot_fail;
    int phase1_stagnation_last_recompute_perturb;
    int shadow_refactor_yes_phase1;
    int shadow_refactor_yes_phase2;
    int shadow_refactor_yes_dual;
    int shadow_refactor_no_phase1;
    int shadow_refactor_no_phase2;
    int shadow_refactor_no_dual;
    int shadow_backend_pick_markowitz;
    int shadow_backend_pick_supernode;
    int shadow_backend_pick_dense;
    int shadow_disagree_primal_refactor;
    int shadow_disagree_dual_refactor;
    int shadow_disagree_lu_backend;
    int reinvert_shadow_checks_phase1;
    int reinvert_shadow_checks_phase2;
    int reinvert_shadow_checks_dual;
    int reinvert_shadow_suggest_allow_phase1;
    int reinvert_shadow_suggest_allow_phase2;
    int reinvert_shadow_suggest_allow_dual;
    int reinvert_shadow_suggest_defer_phase1;
    int reinvert_shadow_suggest_defer_phase2;
    int reinvert_shadow_suggest_defer_dual;
    int reinvert_shadow_suggest_force_phase1;
    int reinvert_shadow_suggest_force_phase2;
    int reinvert_shadow_suggest_force_dual;
    int reinvert_shadow_actual_refactor_yes_phase1;
    int reinvert_shadow_actual_refactor_yes_phase2;
    int reinvert_shadow_actual_refactor_yes_dual;
    int reinvert_shadow_actual_refactor_no_phase1;
    int reinvert_shadow_actual_refactor_no_phase2;
    int reinvert_shadow_actual_refactor_no_dual;
    int reinvert_shadow_disagree_phase1;
    int reinvert_shadow_disagree_phase2;
    int reinvert_shadow_disagree_dual;
    int reinvert_shadow_last_reason_phase1;
    int reinvert_shadow_last_reason_phase2;
    int reinvert_shadow_last_reason_dual;
} LPSolverTelemetrySnapshot;

/* LU telemetry snapshot used by benchmarks and diagnostics. */
typedef struct {
    int mkz_enabled;
    int sn_enabled;
    int mkz_calls;
    int mkz_successes;
    int mkz_failures;
    int mkz_last_failure;
    int mkz_dense_fallbacks;
    int mkz_fail_workspace;
    int mkz_fail_pool;
    int mkz_fail_singular;
    int mkz_fail_capacity;
    int mkz_singular_retry_attempts;
    int mkz_singular_retry_successes;
    int mkz_singular_retry_failures;
    int mkz_reserved_fallback_attempts;
    int mkz_reserved_fallback_accepts;
    int mkz_reserved_fallback_rejects;
    int mkz_circuit_trips;
    int mkz_circuit_skips;
    int mkz_circuit_resets;
    int mkz_global_skip_trips;
    int mkz_global_skip_skips;
    int mkz_global_skip_resets;
    int mkz_profile_retry_attempts;
    int mkz_profile_retry_successes;
    int mkz_profile_retry_failures;
    int mkz_profile_retry_fail_identity_sep;
    int mkz_profile_retry_fail_backend_exhausted;
    int mkz_profile_retry_fail_pathological;
    uint64_t mkz_primary_scan_entries;
    uint64_t mkz_rescue_scan_entries;
    uint64_t mkz_reserved_scan_entries;
    uint64_t mkz_update_existing_entries;
    uint64_t mkz_update_fill_candidates;
    uint64_t mkz_hint_fallback_scans;
    uint64_t mkz_hint_fallback_scan_entries;
    uint64_t mkz_affected_columns_total;
    uint64_t mkz_affected_columns_max;
    uint64_t mkz_col_max_scan_entries;
    int mkz_high_cond_count;
    double mkz_worst_cond;

    int sparse_dense_fallbacks;
    int used_dense_fallback_last;
    int sparse_fallback_last_reason;
    int sparse_fallback_reason_small_matrix;
    int sparse_fallback_reason_symbolic;
    int sparse_fallback_reason_numeric;
    int sparse_numeric_last_failure_reason;
    int sparse_numeric_fail_identity_sep;
    int sparse_numeric_fail_backend_exhausted;
    int sparse_numeric_fail_pathological;
    int numeric_full_retry_attempts;
    int numeric_full_retry_successes;
    int numeric_full_retry_failures;
    int identity_sep_failures;
    int symbolic_failures;
    int symbolic_fail_workspace;
    int symbolic_fail_unmatched_no_reserved;
    int symbolic_fail_inconsistent_identity;
    int symbolic_full_retry_attempts;
    int symbolic_full_retry_successes;
    int symbolic_full_retry_numeric_failures;
    int symbolic_full_retry_mkz_attempts;
    int symbolic_full_retry_mkz_successes;
    int symbolic_full_retry_mkz_failures;
    int numeric_backend_markowitz;
    int numeric_backend_supernode;
    int numeric_backend_dense_ge;
    int backend_policy_luf_ft;
    int backend_policy_cbg;
    int backend_policy_cgr;
    int backend_policy_last;
    int update_path_ft;
    int update_path_eta;
    int update_path_bg_compat;
    int update_path_gr_compat;
    int identity_sep_retry_lane_dense_chosen;
    int identity_sep_retry_lane_supernode_chosen;
    int identity_sep_retry_lane_dense_successes;
    int identity_sep_retry_lane_supernode_successes;
    int sn_cost_gate_trips;
    int sn_cost_gate_skips;
    int sn_cost_gate_resets;
    int refactor_need_checks;
    int refactor_need_triggers;
    int refactor_need_last_reason;
    int refactor_need_reason_max_updates;
    int refactor_need_reason_growth_guard;
    int refactor_need_reason_avg_spike_density;
    int refactor_need_reason_cond_severe;
    int refactor_need_reason_cond_adaptive_limit;
    int refactor_need_reason_spike_pool_warn;
    int refactor_need_reason_spike_work;
    int refactor_need_reason_spike_diag_quality; /* N2: spike diag ratio > 1e8 */
    int update_fail_bad_input;
    int update_fail_max_updates;
    int update_fail_singular_update;
    int update_fail_update_pivot_too_small;
    int update_fail_spike_pool_full;
    int update_fail_dense_spike_reject;
    int update_fail_eta_alloc;

    int sn_calls;
    int sn_successes;
    int num_updates;
    int max_updates;
    int last_failure_reason;
    int last_refactor_trigger_reason;

    int perf_factorize_calls;
    int perf_last_basis_nnz;
    int perf_last_m;
    int perf_last_k;
    int perf_symbolic_calls;
    int perf_symbolic_cache_hits;
    int perf_symbolic_cache_misses;
    double perf_last_symbolic_ms;
    double perf_last_sparse_numeric_ms;
    double perf_last_dense_ge_numeric_ms;
    double perf_last_supernode_numeric_ms;
    double perf_last_dense_factorize_ms;
    double perf_last_a_struct_build_ms;
    double perf_last_markowitz_numeric_ms;
    double perf_last_identity_placement_ms;
    double perf_last_coo_to_csc_ms;
    double perf_total_symbolic_ms;
    double perf_total_sparse_numeric_ms;
    double perf_total_dense_ge_numeric_ms;
    double perf_total_supernode_numeric_ms;
    double perf_total_dense_factorize_ms;
    double perf_total_a_struct_build_ms;
    double perf_total_markowitz_numeric_ms;
    double perf_total_identity_placement_ms;
    double perf_total_coo_to_csc_ms;
    int perf_update_apply_forward_calls;
    int perf_update_apply_backward_calls;
    int perf_compact_factor_calls;
    int perf_compact_solve_calls;
    double perf_total_update_apply_forward_ms;
    double perf_total_update_apply_backward_ms;
    double perf_total_compact_factor_ms;
    double perf_total_compact_solve_ms;
    uint64_t perf_sn_phase_samples;
    double perf_sn_panel_factor_ms;
    double perf_sn_panel_pivot_search_ms;
    double perf_sn_panel_swap_scatter_ms;
    double perf_sn_panel_eliminate_ms;
    uint64_t perf_sn_panel_pivot_search_calls;
    uint64_t perf_sn_panel_pivot_search_entries_total;
    uint64_t perf_sn_panel_pivot_search_size1_calls;
    double perf_sn_panel_pivot_search_size1_ms;
    uint64_t perf_sn_panel_pivot_search_size2_calls;
    double perf_sn_panel_pivot_search_size2_ms;
    uint64_t perf_sn_panel_pivot_search_size3_4_calls;
    double perf_sn_panel_pivot_search_size3_4_ms;
    uint64_t perf_sn_panel_pivot_search_size5_8_calls;
    double perf_sn_panel_pivot_search_size5_8_ms;
    uint64_t perf_sn_panel_pivot_search_size9p_calls;
    double perf_sn_panel_pivot_search_size9p_ms;
    uint64_t perf_sn_panel_pivot_search_reserved_present_calls;
    uint64_t perf_sn_panel_pivot_search_reserved_present_entries;
    double perf_sn_panel_pivot_search_reserved_present_ms;
    uint64_t perf_sn_panel_pivot_search_reserved_alt_chosen_calls;
    double perf_sn_panel_pivot_search_reserved_alt_chosen_ms;
    uint64_t perf_sn_size1_u_emit_calls;
    double perf_sn_size1_u_emit_ms;
    uint64_t perf_sn_size1_update_scan_calls;
    double perf_sn_size1_update_scan_ms;
    uint64_t perf_sn_size1_update_apply_calls;
    double perf_sn_size1_update_apply_ms;
    double perf_sn_size1_update_row_gather_ms;
    double perf_sn_size1_update_col_indirection_ms;
    double perf_sn_size1_update_outer_product_ms;
    uint64_t perf_sn_size1_update_full_calls;
    double perf_sn_size1_update_full_ms;
    uint64_t perf_sn_size1_update_cols1_calls;
    double perf_sn_size1_update_cols1_ms;
    uint64_t perf_sn_size1_update_cols2_calls;
    double perf_sn_size1_update_cols2_ms;
    uint64_t perf_sn_size1_update_cols3_calls;
    double perf_sn_size1_update_cols3_ms;
    uint64_t perf_sn_size1_update_cols4_calls;
    double perf_sn_size1_update_cols4_ms;
    uint64_t perf_sn_size1_update_cols5p_calls;
    double perf_sn_size1_update_cols5p_ms;
    uint64_t perf_sn_size1_update_cols5p_rows1_8_calls;
    double perf_sn_size1_update_cols5p_rows1_8_ms;
    uint64_t perf_sn_size1_update_cols5p_rows9_32_calls;
    double perf_sn_size1_update_cols5p_rows9_32_ms;
    uint64_t perf_sn_size1_update_cols5p_rows33_128_calls;
    double perf_sn_size1_update_cols5p_rows33_128_ms;
    uint64_t perf_sn_size1_update_cols5p_rows129p_calls;
    double perf_sn_size1_update_cols5p_rows129p_ms;
    double perf_sn_u_emit_ms;
    double perf_sn_active_set_ms;
    double perf_sn_pack_blocks_ms;
    double perf_sn_full_update_ms;
    double perf_sn_compact_update_ms;
    uint64_t perf_sn_active_row_scan_entries;
    uint64_t perf_sn_active_col_scan_entries;
    uint64_t perf_sn_trailing_rows_total;
    uint64_t perf_sn_trailing_cols_total;
    uint64_t perf_sn_active_rows_total;
    uint64_t perf_sn_active_cols_total;
    uint64_t perf_sn_pack_l_entries_total;
    uint64_t perf_sn_pack_u_entries_total;
    uint64_t perf_sn_dense_triplets_total;
    uint64_t perf_sn_compact_triplets_total;
    uint64_t perf_sn_full_update_calls;
    uint64_t perf_sn_compact_update_calls;
    uint64_t perf_sn_skipped_update_calls;
    uint64_t perf_sn_compact_cols1_calls;
    uint64_t perf_sn_compact_cols1_rows_total;
    double perf_sn_compact_cols1_ms;
    uint64_t perf_sn_compact_cols2_calls;
    uint64_t perf_sn_compact_cols2_rows_total;
    double perf_sn_compact_cols2_ms;
    uint64_t perf_sn_compact_cols3_calls;
    uint64_t perf_sn_compact_cols3_rows_total;
    double perf_sn_compact_cols3_ms;
    uint64_t perf_sn_compact_cols4_calls;
    uint64_t perf_sn_compact_cols4_rows_total;
    double perf_sn_compact_cols4_ms;
    uint64_t perf_sn_compact_cols5p_calls;
    uint64_t perf_sn_compact_cols5p_rows_total;
    double perf_sn_compact_cols5p_ms;
} LUTelemetrySnapshot;

typedef enum {
    LP_SOLVER_STAGE_PRIMAL_SETUP = 0,
    LP_SOLVER_STAGE_DUAL = 1,
    LP_SOLVER_STAGE_PHASE1 = 2,
    LP_SOLVER_STAGE_TRANSITION = 3,
    LP_SOLVER_STAGE_PHASE2 = 4
} LPSolverStage;

typedef enum {
    LP_PHASE1_RECOMPUTE_REASON_RATIO_BREAKDOWN = 0,
    LP_PHASE1_RECOMPUTE_REASON_DIR_SKIP = 1,
    LP_PHASE1_RECOMPUTE_REASON_DIR_REFACTOR = 2,
    LP_PHASE1_RECOMPUTE_REASON_PIVOT_FAIL_RECOVERY = 3,
    LP_PHASE1_RECOMPUTE_REASON_PERTURB = 4
} LPPhase1RecomputeReason;

typedef enum {
    LP_PHASE1_COMPUTE_CTX_OTHER = 0,
    LP_PHASE1_COMPUTE_CTX_RECOMPUTE_FULL = 1,
    LP_PHASE1_COMPUTE_CTX_RECOMPUTE_GUARD_FORCED_FULL = 2,
    LP_PHASE1_COMPUTE_CTX_INIT = 3,
    LP_PHASE1_COMPUTE_CTX_NO_ENTERING_CLEANUP = 4,
    LP_PHASE1_COMPUTE_CTX_INFEAS_CLEANUP = 5,
    LP_PHASE1_COMPUTE_CTX_REFACTOR_FAIL_CONTINUE = 6,
    LP_PHASE1_COMPUTE_CTX_REFACTOR_FAILURE_RECOVERY = 7,
    LP_PHASE1_COMPUTE_CTX_REFACTOR_SUCCESS = 8,
    LP_PHASE1_COMPUTE_CTX_DRIFT_REFRESH = 9,
    LP_PHASE1_COMPUTE_CTX_RECOMPUTE_RC_ONLY = 10,
    LP_PHASE1_COMPUTE_CTX_DUAL_RESCUE = 11
} LPPhase1ComputeContext;

typedef enum {
    LP_PHASE1_NO_PIVOT_FORCE_REASON_UNKNOWN = 0,
    LP_PHASE1_NO_PIVOT_FORCE_REASON_RATIO_BREAKDOWN = 1,
    LP_PHASE1_NO_PIVOT_FORCE_REASON_DIR_SKIP = 2,
    LP_PHASE1_NO_PIVOT_FORCE_REASON_PIVOT_FAIL = 3
} LPPhase1NoPivotForceReason;

/* LP model functions */
LPModel* lp_model_create(void);
void lp_model_free(LPModel *model);
int lp_model_add_var(LPModel *model, double lb, double ub, double obj, char type);
int lp_model_add_constraint(LPModel *model, int nnz, const int *indices,
                            const double *values, char sense, double rhs);
int lp_model_get_coefficient(const LPModel *model, int constraint, int var, double *value);
int lp_model_set_coefficient(LPModel *model, int constraint, int var, double value);
int lp_model_set_coefficients(LPModel *model, int count, const int *constraints,
                              const int *vars, const double *values);
int lp_model_delete_constraint(LPModel *model, int constraint);
int lp_model_delete_var(LPModel *model, int var);
int lp_model_finalize(LPModel *model);
LPModel* lp_model_copy(const LPModel *model);

/* Name management */
int lp_model_set_var_name(LPModel *model, int var, const char *name);
int lp_model_set_con_name(LPModel *model, int con, const char *name);
const char* lp_model_get_var_name(const LPModel *model, int var);
const char* lp_model_get_con_name(const LPModel *model, int con);
int lp_model_set_name(LPModel *model, const char *name);
const char* lp_model_get_name(const LPModel *model);

/* LU factorization functions */
LUFactorization* lu_create(int m);
void lu_free(LUFactorization *lu);
LUFailureReason lu_factorize(LUFactorization *lu, const SparseMatrix *B);
LUFailureReason lu_factorize_sparse_no_dense(LUFactorization *lu, const SparseMatrix *B);
void lu_apply_backend_policy(LUFactorization *lu, int backend_policy);
LUFailureReason lu_factorize_sparse(LUFactorization *lu, const SparseMatrix *B);  /* Sparse with Markowitz */
LUFailureReason lu_factorize_dense(LUFactorization *lu, const SparseMatrix *B);   /* Dense fallback */
void lu_solve(const LUFactorization *lu, double *rhs, double *solution);
void lu_solve_transpose(const LUFactorization *lu, double *rhs, double *solution);
LUFailureReason lu_update(LUFactorization *lu, int leaving_pos, const double *entering_col);
int lu_needs_refactorization(LUFactorization *lu);
int lu_refactor_hard_trigger(const LUFactorization *lu);
const char* lu_failure_reason_string(int reason);
const char* lu_refactor_trigger_reason_string(int reason);

/* LU getter/setter API (R0.3) — encapsulated access to LU state */
int lu_get_num_updates(const LUFactorization *lu);
int lu_get_max_updates(const LUFactorization *lu);
double lu_get_growth_factor(const LUFactorization *lu);
double lu_get_cond_estimate(const LUFactorization *lu);
double lu_get_growth_refactor_threshold(const LUFactorization *lu);
int lu_get_spike_pool_used(const LUFactorization *lu);
int lu_get_spike_pool_capacity(const LUFactorization *lu);
int lu_get_use_ft_updates(const LUFactorization *lu);
int lu_get_ft_num_updates(const LUFactorization *lu);
LUFailureReason lu_get_last_failure_reason(const LUFactorization *lu);
int lu_get_last_refactor_trigger_reason(const LUFactorization *lu);
double lu_get_pivot_tol(const LUFactorization *lu);
int lu_get_backend_policy(const LUFactorization *lu);
int lu_get_num_regularized(const LUFactorization *lu);
int lu_get_telemetry_enabled(const LUFactorization *lu);
int lu_get_sym_valid(const LUFactorization *lu);
uint64_t lu_get_factorize_calls(const LUFactorization *lu);
void lu_set_pivot_tol(LUFactorization *lu, double tol);
void lu_set_max_updates(LUFactorization *lu, int max);
void lu_set_growth_refactor_threshold(LUFactorization *lu, double threshold);
void lu_set_backend_policy(LUFactorization *lu, int policy);
void lu_set_dense_spike_min_updates_override(LUFactorization *lu, int min_updates);
void lu_set_telemetry_enabled(LUFactorization *lu, int enabled);
void lu_set_owner(LUFactorization *lu, void *owner);
void lu_set_basis_governor(LUFactorization *lu, void *governor);
void lu_set_mkz_enabled(LUFactorization *lu, int enabled);
void lu_set_sn_enabled(LUFactorization *lu, int enabled);
void lu_invalidate_symbolic_cache(LUFactorization *lu);
void lu_force_refactorization(LUFactorization *lu);
void lu_configure_regularization(LUFactorization *lu, int allow, int max_reg,
                                  const int *redundant_rows, int num_redundant);

/* Sparse LU solves - exploit sparsity in RHS */
void lu_solve_sparse(const LUFactorization *lu,
                     int nnz_rhs, const int *rhs_idx, const double *rhs_val,
                     double *solution);
void lu_solve_transpose_sparse(const LUFactorization *lu,
                               int nnz_rhs, const int *rhs_idx, const double *rhs_val,
                               double *solution);

/* Hyper-sparse LU solves - with reach computation for very sparse RHS */
void lu_ftran_hyper_sparse(const LUFactorization *lu,
                           int nnz_rhs, const int *rhs_idx, const double *rhs_val,
                           double *solution,
                           int *sol_idx, int *sol_nnz);
void lu_btran_hyper_sparse(const LUFactorization *lu,
                           int nnz_rhs, const int *rhs_idx, const double *rhs_val,
                           double *solution,
                           int *sol_idx, int *sol_nnz);

/* Simplex tableau functions */
SimplexTableau* tableau_create(LPModel *model);
void tableau_free(SimplexTableau *tableau);
int tableau_compute_solution(SimplexTableau *tableau);
int tableau_compute_reduced_costs(SimplexTableau *tableau);
int tableau_refactorize(SimplexTableau *tableau);
int tableau_apply_warm_basis(SimplexTableau *tableau, int m, int n,
                             const int *basis, const VarStatus *var_status);
int tableau_apply_structural_bounds(SimplexTableau *tableau, int num_struct_vars,
                                    const double *lb, const double *ub);

/* Simplex solver functions */
SimplexSolver* simplex_create(LPModel *model);
void simplex_free(SimplexSolver *solver);
int simplex_set_warm_basis(SimplexSolver *solver, int m, int n,
                           const int *basis, const VarStatus *var_status);
int simplex_prepare_primal_tableau(SimplexSolver *solver, int allow_crash);
int simplex_prepare_augmented_primal_tableau(SimplexSolver *solver,
                                             const LPAugmentRow *rows,
                                             int num_rows,
                                             int warm_m,
                                             int warm_n,
                                             const int *warm_basis,
                                             const VarStatus *warm_var_status);
int simplex_resolve_prepared_primal_tableau(SimplexSolver *solver);
int simplex_solve(SimplexSolver *solver);

/* Dual simplex */
int dual_simplex_phase1_rescue(SimplexSolver *solver, int max_iters);
void dual_v2_clear_perturbation(SimplexTableau *tab);  /* Clear stale perturbation backup before warm start */
int dual_simplex_solve_v2(SimplexSolver *solver);  /* Clean dual Phase 2 — no primal fallbacks */
int dual_simplex_solve_from_scratch_v2(SimplexSolver *solver);  /* Clean dual from scratch (T1.3) */
int dual_phase1(SimplexSolver *solver);            /* Auxiliary-objective dual Phase 1 */
int make_dual_feasible(SimplexTableau *tab, int obj_sense, int allow_bound_flip); /* Flip bounds for dual feasibility */

/* Pricing strategies */
int pricing_dantzig(SimplexTableau *tableau, int *entering);
int pricing_steepest_edge(SimplexTableau *tableau, int *entering);
int pricing_devex(SimplexTableau *tableau, int *entering);
int pricing_partial(SimplexTableau *tableau, int *entering);
int pricing_heap(SimplexTableau *tableau, int *entering);

/* Ratio test */
int ratio_test_harris(SimplexTableau *tableau, int entering, int *leaving, double *theta);
int dual_ratio_test(SimplexTableau *tableau, int leaving, int *entering, double *theta); /* entering=-2 => flip-only step */

/* Utility */
void lp_print_stats(const SimplexSolver *solver);

/* Fixed-basis LP sensitivity helpers (internal minimization space). */
typedef struct {
    double lower_min;
    double lower_max;
    double upper_min;
    double upper_max;
} LPBoundRangeInternal;

int lp_sensitivity_rhs_range_internal(const SimplexTableau *tab,
                                      int row,
                                      double *rhs_min,
                                      double *rhs_max);
int lp_sensitivity_obj_coef_range_internal(const SimplexTableau *tab,
                                           int var,
                                           double *coef_min,
                                           double *coef_max);
int lp_sensitivity_var_bound_range_internal(const SimplexTableau *tab,
                                            int var,
                                            LPBoundRangeInternal *range);

/* Determinism helpers (behavioral policy; orthogonal to telemetry/logging). */
int lp_determinism_effective_threads(const SimplexSolver *solver);
void lp_determinism_apply_runtime(SimplexSolver *solver);
unsigned int lp_determinism_seed_offset(const SimplexSolver *solver,
                                        int key,
                                        unsigned int modulus);

/* Telemetry helpers */
double lp_telemetry_now_ms(void);
double lp_telemetry_timer_start(void);
double lp_telemetry_timer_elapsed_ms(double start_ms);
int lp_telemetry_refactor_reason_is_safety_forced(int reason);
void lp_telemetry_reset_solver(SimplexSolver *solver);
void lp_telemetry_reset_lu(LUFactorization *lu);
void lp_telemetry_prepare_lu_factorize(LUFactorization *lu, const SparseMatrix *B);
void lp_telemetry_record_basis_build(SimplexSolver *owner,
                                     int fastpath_hit,
                                     int cols_rewritten,
                                     unsigned long long tail_shift_bytes);
void lp_telemetry_begin_refactor(SimplexSolver *owner, int *reason_out);
void lp_telemetry_set_refactor_next_reason(SimplexSolver *owner, int reason);
void lp_telemetry_record_refactor(SimplexSolver *owner,
                                  int phase,
                                  int reason,
                                  double elapsed_ms,
                                  int m,
                                  int lu_last_k,
                                  int lu_last_basis_nnz);
void lp_telemetry_record_refactor_with_lu(SimplexSolver *owner,
                                          int phase,
                                          int reason,
                                          double elapsed_ms,
                                          int m,
                                          const LUFactorization *lu);
void lp_telemetry_record_refactor_with_lu_timed(SimplexSolver *owner,
                                                int phase,
                                                int reason,
                                                double start_ms,
                                                int m,
                                                const LUFactorization *lu);
void lp_telemetry_snapshot_solver(const SimplexSolver *solver,
                                  LPSolverTelemetrySnapshot *out);
void lp_telemetry_snapshot_lu(const LUFactorization *lu,
                              LUTelemetrySnapshot *out);
void lp_telemetry_add_solver_stage_ms(SimplexSolver *solver,
                                      LPSolverStage stage,
                                      double elapsed_ms);
void lp_telemetry_add_solver_stage_timed(SimplexSolver *solver,
                                         LPSolverStage stage,
                                         double start_ms);
void lp_telemetry_add_refactor_runtime_ms(SimplexSolver *solver,
                                          double elapsed_ms);
void lp_telemetry_add_refactor_runtime_timed(SimplexSolver *solver,
                                             double start_ms);
void lp_telemetry_add_ftran_ms(SimplexSolver *solver,
                               double elapsed_ms);
void lp_telemetry_add_ftran_timed(SimplexSolver *solver,
                                  double start_ms);
void lp_telemetry_add_ftran_base_ms(SimplexSolver *solver,
                                    double elapsed_ms);
void lp_telemetry_add_ftran_base_timed(SimplexSolver *solver,
                                       double start_ms);
void lp_telemetry_add_ftran_update_apply_ms(SimplexSolver *solver,
                                            double elapsed_ms);
void lp_telemetry_add_ftran_update_apply_timed(SimplexSolver *solver,
                                               double start_ms);
void lp_telemetry_record_ftran_nnz(SimplexSolver *solver,
                                   int rhs_nnz,
                                   int sol_nnz);
void lp_telemetry_add_btran_ms(SimplexSolver *solver,
                               double elapsed_ms);
void lp_telemetry_add_btran_timed(SimplexSolver *solver,
                                  double start_ms);
void lp_telemetry_add_btran_base_ms(SimplexSolver *solver,
                                    double elapsed_ms);
void lp_telemetry_add_btran_base_timed(SimplexSolver *solver,
                                       double start_ms);
void lp_telemetry_add_btran_update_apply_ms(SimplexSolver *solver,
                                            double elapsed_ms);
void lp_telemetry_add_btran_update_apply_timed(SimplexSolver *solver,
                                               double start_ms);
void lp_telemetry_record_btran_nnz(SimplexSolver *solver,
                                   int rhs_nnz,
                                   int sol_nnz);
void lp_telemetry_add_lu_update_ms(SimplexSolver *solver,
                                   double elapsed_ms);
void lp_telemetry_add_lu_update_timed(SimplexSolver *solver,
                                      double start_ms);
void lp_telemetry_record_compute_solution(SimplexSolver *solver,
                                          int phase,
                                          double elapsed_ms);
void lp_telemetry_record_compute_solution_timed(SimplexSolver *solver,
                                                int phase,
                                                double start_ms);
void lp_telemetry_record_compute_reduced_costs(SimplexSolver *solver,
                                               int phase,
                                               double elapsed_ms);
void lp_telemetry_record_phase1_compute_solution_context(SimplexSolver *solver,
                                                         LPPhase1ComputeContext context);
void lp_telemetry_record_phase1_compute_rc_context(SimplexSolver *solver,
                                                   LPPhase1ComputeContext context);
void lp_telemetry_record_phase1_entering_exclusion(SimplexSolver *solver,
                                                   int repeated_slot);
void lp_telemetry_record_phase1_entering_exclusion_hit(SimplexSolver *solver,
                                                       int rerouted);
void lp_telemetry_record_phase1_dir_skip_entering(SimplexSolver *solver,
                                                  int same_entering,
                                                  int streak);
void lp_telemetry_record_phase1_failed_stabilize_entering(
    SimplexSolver *solver,
    int same_entering,
    int streak);
void lp_telemetry_record_phase1_failed_stabilize_site(
    SimplexSolver *solver,
    int used_alternate);
void lp_telemetry_record_phase1_failed_stabilize_retry_penalty_arm(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_penalty_alternate(
    SimplexSolver *solver,
    int same_alt,
    int streak);
void lp_telemetry_record_phase1_failed_stabilize_retry_penalty_no_alt(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_penalty_outcome(
    SimplexSolver *solver,
    int stabilized);
void lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_arm(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_alternate(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_no_alt(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_fallback_same_alt(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_local_memory_outcome(
    SimplexSolver *solver,
    int stabilized);
void lp_telemetry_record_phase1_failed_stabilize_retry_pool_sample(
    SimplexSolver *solver,
    int eligible_count,
    int best_differs_from_bland);
void lp_telemetry_record_phase1_failed_stabilize_retry_selector_eval(
    SimplexSolver *solver,
    int best_differs_from_bland,
    double bland_score,
    double best_score);
void lp_telemetry_record_phase1_failed_stabilize_retry_shadow(
    SimplexSolver *solver,
    int ratio_success,
    int dir_stable,
    double dir_inf,
    int dir_nnz,
    double pivot_abs);
void lp_telemetry_record_phase1_failed_stabilize_retry_shadow_guard_arm(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_shadow_guard_original_exclusion(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_shadow_post_dir_skip_retry(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_shadow_post_dir_skip_dual_rescue(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_shadow_post_dir_skip_forced_refactor(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_shadow_next_failed_stabilize(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_shadow_next_ratio_breakdown(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_shadow_next_pivot_fail(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_shadow_next_pivot_success(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_followup_stabilized(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_followup_ratio_breakdown(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_followup_failed_stabilize(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_followup_post_dir_skip_retry(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_followup_post_dir_skip_dual_rescue(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_followup_post_dir_skip_forced_refactor(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_followup_next_failed_stabilize(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_followup_next_ratio_breakdown(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_followup_next_pivot_fail(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_followup_next_pivot_success(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_followup_direction(
    SimplexSolver *solver,
    int leaving,
    double theta,
    double dir_inf,
    int dir_nnz,
    double pivot_abs);
void lp_telemetry_record_phase1_failed_stabilize_retry_shadow_followup_direction(
    SimplexSolver *solver,
    int leaving,
    double theta,
    double dir_inf,
    int dir_nnz,
    double pivot_abs);
void lp_telemetry_record_phase1_failed_stabilize_retry_selector_choice(
    SimplexSolver *solver,
    int used_guarded,
    int eligible_count);
void lp_telemetry_record_phase1_failed_stabilize_retry_selector_outcome(
    SimplexSolver *solver,
    int used_guarded,
    int stabilized);
void lp_telemetry_record_phase1_failed_stabilize_retry_selector_ratio_failure(
    SimplexSolver *solver,
    int used_guarded);
void lp_telemetry_record_phase1_failed_stabilize_retry_selector_dir_failure(
    SimplexSolver *solver,
    int used_guarded);
void lp_telemetry_record_phase1_failed_stabilize_retry_selector_guarded_fallback(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_dir_fail_shape(
    SimplexSolver *solver,
    double dir_inf,
    int dir_nnz,
    double pivot_abs);
void lp_telemetry_record_phase1_failed_stabilize_retry_dir_second_chance_arm(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_dir_second_chance_no_alt(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_dir_second_chance_outcome(
    SimplexSolver *solver,
    int stabilized);
void lp_telemetry_record_phase1_failed_stabilize_retry_dir_guard_arm(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_failed_stabilize_retry_dir_guard_original_exclusion(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_window_pressure_event(
    SimplexSolver *solver,
    int failed_stabilize_event,
    int dir_skip_event,
    int local_memory_fail_event,
    int alternated,
    int window_events,
    int window_failed_stabilize,
    int window_dir_skip,
    int window_local_memory_fail,
    int window_alternations);
void lp_telemetry_record_phase1_window_pressure_progress_reset(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_window_pressure_force_pivot_arm(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_window_pressure_force_pivot_reject(
    SimplexSolver *solver,
    int reject_reason);
void lp_telemetry_record_phase1_window_pressure_force_pivot_blocked_pending(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_window_pressure_force_pivot_blocked_budget(
    SimplexSolver *solver);
void lp_telemetry_record_phase2_pivot_geometry(SimplexSolver *solver,
                                               double theta,
                                               double dir_inf,
                                               double pivot_abs);
void lp_telemetry_record_phase2_devex_reset(SimplexSolver *solver,
                                            int devex_age);
void lp_telemetry_record_phase2_degenerate_refactor(SimplexSolver *solver,
                                                    int reason,
                                                    int lu_health_triggered,
                                                    int safety_forced);
void lp_telemetry_record_compute_reduced_costs_timed(SimplexSolver *solver,
                                                     int phase,
                                                     double start_ms);
void lp_telemetry_record_pricing(SimplexSolver *solver,
                                 int phase,
                                 double elapsed_ms);
void lp_telemetry_record_pricing_timed(SimplexSolver *solver,
                                       int phase,
                                       double start_ms);
void lp_telemetry_record_ratio(SimplexSolver *solver,
                               int phase,
                               double elapsed_ms);
void lp_telemetry_record_ratio_timed(SimplexSolver *solver,
                                     int phase,
                                     double start_ms);
void lp_telemetry_record_pivot(SimplexSolver *solver,
                               int phase,
                               double elapsed_ms);
void lp_telemetry_record_pivot_timed(SimplexSolver *solver,
                                     int phase,
                                     double start_ms);
void lp_telemetry_record_periodic_refactor_trigger(SimplexSolver *solver,
                                                   int phase,
                                                   int lu_health_triggered);
void lp_telemetry_record_phase1_dir_stabilize_force(SimplexSolver *solver,
                                                    int force_extreme_dir,
                                                    int force_lu_health);
void lp_telemetry_record_phase1_dir_stabilize_cooldown_candidate(
    SimplexSolver *solver,
    double dir_inf_ratio);
void lp_telemetry_record_phase1_dir_stabilize_skip(SimplexSolver *solver,
                                                   int used_full_recompute);
void lp_telemetry_record_phase1_dir_stabilize_skip_no_recompute(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_dir_stabilize_skip_guard_refresh(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_dir_stabilize_escape_gate(
    SimplexSolver *solver,
    int event);
void lp_telemetry_record_phase1_dir_stabilize_refactor_trigger(
    SimplexSolver *solver,
    int trigger);
void lp_telemetry_record_phase1_force_pivot_budget_dir_event_seen(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_pivot_budget_pivot_spend(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_pivot_relax(SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_relax(SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_bound_flip_relax(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_catastrophic_tiny_theta_relax(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_tiny_theta_relax(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_post_dir_skip_retry(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_post_dir_skip_dual_rescue(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_post_dir_skip_forced_refactor(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_refactor(
    SimplexSolver *solver,
    int reason);
void lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_next_failed_stabilize(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_next_ratio_breakdown(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_next_pivot_fail(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_force_extreme_tiny_theta_relax_next_pivot_success(
    SimplexSolver *solver);
void lp_telemetry_record_phase1_recompute(SimplexSolver *solver,
                                          LPPhase1RecomputeReason reason);
void lp_telemetry_record_phase1_recompute_rc_only(SimplexSolver *solver);
void lp_telemetry_record_phase1_recompute_guard_forced_full(SimplexSolver *solver);
void lp_telemetry_record_phase1_ratio_breakdown_retry(SimplexSolver *solver);
void lp_telemetry_record_phase1_ratio_breakdown_escalation(SimplexSolver *solver);
void lp_telemetry_record_phase1_pivot_fail_recovery_exclusion(SimplexSolver *solver);
void lp_telemetry_record_phase1_no_pivot_event(SimplexSolver *solver,
                                               LPPhase1NoPivotForceReason reason);
void lp_telemetry_record_phase1_no_pivot_force(SimplexSolver *solver,
                                               LPPhase1NoPivotForceReason reason);
void lp_telemetry_record_phase1_no_pivot_no_progress(SimplexSolver *solver);
void lp_telemetry_record_phase1_no_pivot_ladder_retry(SimplexSolver *solver,
                                                      LPPhase1NoPivotForceReason reason);
void lp_telemetry_record_phase1_no_pivot_ladder_dual_rescue(SimplexSolver *solver,
                                                             LPPhase1NoPivotForceReason reason,
                                                             int success);
void lp_telemetry_record_phase1_no_pivot_ladder_forced_refactor(
    SimplexSolver *solver,
    LPPhase1NoPivotForceReason reason);
void lp_telemetry_record_phase1_no_pivot_ladder_rescue_guard(
    SimplexSolver *solver,
    int forced_refactor);
void lp_telemetry_record_phase1_direct_dual_rescue(SimplexSolver *solver,
                                                   int success);
void lp_telemetry_record_phase1_direct_dual_rescue_guard(
    SimplexSolver *solver,
    int fail_cap_block);
void lp_telemetry_record_phase1_soft_lu_policy_cooldown_defer(
    SimplexSolver *solver);
void lp_telemetry_record_dual_ratio_no_entering(SimplexSolver *solver);
void lp_telemetry_record_dual_theta_nonpositive(SimplexSolver *solver);
void lp_telemetry_record_dual_pivot_reject_small(SimplexSolver *solver);
void lp_telemetry_record_dual_bound_flip_applied(SimplexSolver *solver,
                                                 int flips);
void lp_telemetry_record_dual_bound_flip_applied_startup(SimplexSolver *solver,
                                                         int flips);
void lp_telemetry_record_dual_bound_flip_applied_iterative(SimplexSolver *solver,
                                                           int flips);
void lp_telemetry_record_dual_lu_hard_trigger(SimplexSolver *solver);
void lp_telemetry_record_reinvert_shadow(SimplexSolver *solver,
                                         int phase,
                                         LPReinvertDecision suggested_decision,
                                         LPReinvertReason suggested_reason,
                                         int suggested_refactor,
                                         int actual_refactor);
void lp_telemetry_lu_record_dense_factorize_ms(LUFactorization *lu,
                                               double elapsed_ms);
void lp_telemetry_lu_record_dense_factorize_timed(LUFactorization *lu,
                                                  double start_ms);
void lp_telemetry_lu_record_update_apply_forward_ms(LUFactorization *lu,
                                                    double elapsed_ms);
void lp_telemetry_lu_record_update_apply_backward_ms(LUFactorization *lu,
                                                     double elapsed_ms);
void lp_telemetry_lu_record_compact_factor_ms(LUFactorization *lu,
                                              double elapsed_ms);
void lp_telemetry_lu_record_compact_solve_ms(LUFactorization *lu,
                                             double elapsed_ms);
void lp_telemetry_lu_record_symbolic_cache_hit(LUFactorization *lu);
void lp_telemetry_lu_record_symbolic_cache_miss(LUFactorization *lu);
void lp_telemetry_lu_record_symbolic_call(LUFactorization *lu,
                                          double elapsed_ms);
void lp_telemetry_lu_record_symbolic_call_timed(LUFactorization *lu,
                                                double start_ms);
void lp_telemetry_lu_mark_identity_sep_failure(LUFactorization *lu);
void lp_telemetry_lu_record_numeric_stages(LUFactorization *lu,
                                           int last_k,
                                           double a_struct_build_ms,
                                           double markowitz_numeric_ms,
                                           double supernode_numeric_ms,
                                           double dense_ge_numeric_ms,
                                           double identity_placement_ms,
                                           double coo_to_csc_ms);
void lp_telemetry_lu_mark_sparse_numeric_failure(LUFactorization *lu,
                                                 int reason);
void lp_telemetry_lu_set_sparse_fallback_reason(LUFactorization *lu,
                                                int reason);
void lp_telemetry_lu_mark_numeric_full_retry_attempt(LUFactorization *lu);
void lp_telemetry_lu_mark_numeric_full_retry_success(LUFactorization *lu);
void lp_telemetry_lu_mark_numeric_full_retry_failure(LUFactorization *lu);
void lp_telemetry_lu_mark_symbolic_failure(LUFactorization *lu,
                                           int reason);
void lp_telemetry_lu_mark_symbolic_full_retry_attempt(LUFactorization *lu);
void lp_telemetry_lu_mark_symbolic_full_retry_success(LUFactorization *lu);
void lp_telemetry_lu_mark_symbolic_full_retry_numeric_failure(LUFactorization *lu);
void lp_telemetry_lu_mark_symbolic_full_retry_mkz_attempt(LUFactorization *lu);
void lp_telemetry_lu_mark_symbolic_full_retry_mkz_success(LUFactorization *lu);
void lp_telemetry_lu_mark_symbolic_full_retry_mkz_failure(LUFactorization *lu);
void lp_telemetry_lu_mark_numeric_backend_markowitz(LUFactorization *lu);
void lp_telemetry_lu_mark_numeric_backend_supernode(LUFactorization *lu);
void lp_telemetry_lu_mark_numeric_backend_dense_ge(LUFactorization *lu);
void lp_telemetry_lu_mark_identity_sep_retry_lane_chosen(LUFactorization *lu,
                                                         int lane);
void lp_telemetry_lu_mark_identity_sep_retry_lane_success(LUFactorization *lu,
                                                          int lane);
void lp_telemetry_lu_mark_sn_cost_gate_trip(LUFactorization *lu);
void lp_telemetry_lu_mark_sn_cost_gate_skip(LUFactorization *lu);
void lp_telemetry_lu_mark_sn_cost_gate_reset(LUFactorization *lu);
void lp_telemetry_lu_mark_sparse_success(LUFactorization *lu);
void lp_telemetry_lu_mark_dense_fallback(LUFactorization *lu);
void lp_telemetry_lu_clear_mkz_last_failure(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_attempt(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_success(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_failure_reason(LUFactorization *lu,
                                             int rc);
void lp_telemetry_lu_mark_mkz_failure(LUFactorization *lu,
                                      int rc);
void lp_telemetry_lu_mark_mkz_singular_retry_attempt(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_singular_retry_success(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_singular_retry_failure(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_reserved_fallback_attempt(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_reserved_fallback_accept(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_reserved_fallback_reject(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_circuit_trip(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_circuit_skip(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_circuit_reset(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_global_skip_trip(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_global_skip_skip(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_global_skip_reset(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_profile_retry_attempt(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_profile_retry_success(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_profile_retry_failure(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_profile_retry_terminal_failure(LUFactorization *lu,
                                                             int reason);
void lp_telemetry_lu_add_mkz_scan_work(LUFactorization *lu,
                                       uint64_t primary_scan_entries,
                                       uint64_t rescue_scan_entries,
                                       uint64_t reserved_scan_entries,
                                       uint64_t update_existing_entries,
                                       uint64_t update_fill_candidates,
                                       uint64_t hint_fallback_scans,
                                       uint64_t hint_fallback_scan_entries);
void lp_telemetry_lu_add_mkz_colmax_work(LUFactorization *lu,
                                         uint64_t affected_columns,
                                         uint64_t affected_columns_max,
                                         uint64_t col_max_scan_entries);
void lp_telemetry_lu_add_supernode_work(LUFactorization *lu,
                                        uint64_t phase_samples,
                                        double panel_factor_ms,
                                        double panel_pivot_search_ms,
                                        double panel_swap_scatter_ms,
                                        double panel_eliminate_ms,
                                        uint64_t panel_pivot_search_calls,
                                        uint64_t panel_pivot_search_entries_total,
                                        uint64_t panel_pivot_search_size1_calls,
                                        double panel_pivot_search_size1_ms,
                                        uint64_t panel_pivot_search_size2_calls,
                                        double panel_pivot_search_size2_ms,
                                        uint64_t panel_pivot_search_size3_4_calls,
                                        double panel_pivot_search_size3_4_ms,
                                        uint64_t panel_pivot_search_size5_8_calls,
                                        double panel_pivot_search_size5_8_ms,
                                        uint64_t panel_pivot_search_size9p_calls,
                                        double panel_pivot_search_size9p_ms,
                                        uint64_t panel_pivot_search_reserved_present_calls,
                                        uint64_t panel_pivot_search_reserved_present_entries,
                                        double panel_pivot_search_reserved_present_ms,
                                        uint64_t panel_pivot_search_reserved_alt_chosen_calls,
                                        double panel_pivot_search_reserved_alt_chosen_ms,
                                        uint64_t size1_u_emit_calls,
                                        double size1_u_emit_ms,
                                        uint64_t size1_update_scan_calls,
                                        double size1_update_scan_ms,
                                        uint64_t size1_update_apply_calls,
                                        double size1_update_apply_ms,
                                        double size1_update_row_gather_ms,
                                        double size1_update_col_indirection_ms,
                                        double size1_update_outer_product_ms,
                                        uint64_t size1_update_full_calls,
                                        double size1_update_full_ms,
                                        uint64_t size1_update_cols1_calls,
                                        double size1_update_cols1_ms,
                                        uint64_t size1_update_cols2_calls,
                                        double size1_update_cols2_ms,
                                        uint64_t size1_update_cols3_calls,
                                        double size1_update_cols3_ms,
                                        uint64_t size1_update_cols4_calls,
                                        double size1_update_cols4_ms,
                                        uint64_t size1_update_cols5p_calls,
                                        double size1_update_cols5p_ms,
                                        uint64_t size1_update_cols5p_rows1_8_calls,
                                        double size1_update_cols5p_rows1_8_ms,
                                        uint64_t size1_update_cols5p_rows9_32_calls,
                                        double size1_update_cols5p_rows9_32_ms,
                                        uint64_t size1_update_cols5p_rows33_128_calls,
                                        double size1_update_cols5p_rows33_128_ms,
                                        uint64_t size1_update_cols5p_rows129p_calls,
                                        double size1_update_cols5p_rows129p_ms,
                                        double u_emit_ms,
                                        double active_set_ms,
                                        double pack_blocks_ms,
                                        double full_update_ms,
                                        double compact_update_ms,
                                        uint64_t active_row_scan_entries,
                                        uint64_t active_col_scan_entries,
                                        uint64_t trailing_rows_total,
                                        uint64_t trailing_cols_total,
                                        uint64_t active_rows_total,
                                        uint64_t active_cols_total,
                                        uint64_t pack_l_entries_total,
                                        uint64_t pack_u_entries_total,
                                        uint64_t dense_triplets_total,
                                        uint64_t compact_triplets_total,
                                        uint64_t full_update_calls,
                                        uint64_t compact_update_calls,
                                        uint64_t skipped_update_calls,
                                        uint64_t compact_cols1_calls,
                                        uint64_t compact_cols1_rows_total,
                                        double compact_cols1_ms,
                                        uint64_t compact_cols2_calls,
                                        uint64_t compact_cols2_rows_total,
                                        double compact_cols2_ms,
                                        uint64_t compact_cols3_calls,
                                        uint64_t compact_cols3_rows_total,
                                        double compact_cols3_ms,
                                        uint64_t compact_cols4_calls,
                                        uint64_t compact_cols4_rows_total,
                                        double compact_cols4_ms,
                                        uint64_t compact_cols5p_calls,
                                        uint64_t compact_cols5p_rows_total,
                                        double compact_cols5p_ms);

#ifdef __cplusplus
}
#endif

#endif /* RALPH_LP_H */
