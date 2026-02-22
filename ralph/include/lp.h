/*
 * Ralph - LP Model and Algorithm Internals
 */

#ifndef RALPH_LP_H
#define RALPH_LP_H

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "sparse.h"
#include "ralph.h"
#include "shared.h"

/* Software prefetch (no-op on non-GCC/Clang compilers) */
#if defined(__GNUC__) || defined(__clang__)
#define RALPH_PREFETCH(addr, rw, locality) __builtin_prefetch(addr, rw, locality)
#else
#define RALPH_PREFETCH(addr, rw, locality) ((void)0)
#endif

/* Tolerances */
#define RALPH_FEAS_TOL 1e-6
#define RALPH_OPT_TOL 1e-6
#define RALPH_PIVOT_TOL 1e-6  /* Increased for numerical stability */
#define RALPH_ZERO_TOL 1e-12
#define RALPH_INT_TOL 1e-5

/* Numerical stabilization policy defaults */
#define RALPH_FORCE_REFACTOR_PIVOT_TOL 1e-4
#define RALPH_LU_UPDATE_PIVOT_THRESHOLD 1e-4
#define RALPH_LU_GROWTH_REFACTOR_THRESHOLD 1e8
#define RALPH_PHASE1_REPEAT_REFACTOR_TRIGGER 3
#define RALPH_PHASE1_FAIL_REPEAT_LIMIT 20
#define RALPH_PHASE1_RATIO_BREAKDOWN_LIMIT 60
#define RALPH_PHASE1_MAX_REGULARIZATIONS 32
#define RALPH_PHASE1_DUAL_RESCUE_MULT 20
#define RALPH_PHASE1_RESCUE_MAX_REFACTOR_FAILURES 64
#define RALPH_PHASE1_DIR_INF_REFACTOR_TRIGGER 1e4
#define RALPH_PHASE1_ENTERING_EXCLUDE_ITERS 8

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
    LU_FAIL_ETA_ALLOC,
    LU_FAIL_FACTOR_SINGULAR,
    LU_FAIL_FACTOR_ALLOC
} LUFailureReason;

/* Markowitz sparse numeric failure codes (internal telemetry + fallback routing). */
typedef enum {
    MKZ_FAIL_NONE = 0,
    MKZ_FAIL_WORKSPACE = -1,
    MKZ_FAIL_POOL = -2,
    MKZ_FAIL_SINGULAR = -3,
    MKZ_FAIL_CAPACITY = -4
} MKZFailureReason;

/* Sparse-efficient -> dense fallback classification (per lu_factorize call). */
typedef enum {
    LU_SPARSE_FALLBACK_NONE = 0,
    LU_SPARSE_FALLBACK_SMALL_MATRIX,
    LU_SPARSE_FALLBACK_SYMBOLIC,
    LU_SPARSE_FALLBACK_NUMERIC
} LUSparseFallbackReason;

/* LP model internal representation */
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

    /* Variable bounds: lb <= x <= ub */
    double *lb;             /* Lower bounds */
    double *ub;             /* Upper bounds */

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

    /* Sparse-efficient fallback telemetry */
    int sparse_dense_fallbacks;  /* lu_factorize_sparse_efficient -> lu_factorize_dense */
    int used_dense_fallback_last;/* 1 if last lu_factorize call used dense fallback */
    int sparse_fallback_last_reason;      /* LUSparseFallbackReason */
    int sparse_fallback_reason_small_matrix;
    int sparse_fallback_reason_symbolic;
    int sparse_fallback_reason_numeric;
    int identity_sep_failures;   /* Identity-placement failures in sparse-efficient path */

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

    /* Forrest-Tomlin update data */
    int use_ft_updates;     /* 1 to use FT updates, 0 for eta-file */
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

    /* (B4: spike compaction removed — was O(m^2*N), worse than sparse application) */

    /* Condition number monitoring */
    double min_diag_U;      /* Minimum |U[i,i]| at factorization */
    double max_diag_U;      /* Maximum |U[i,i]| at factorization */
    double cond_estimate;   /* Estimated condition number */
    double growth_factor;   /* Growth in U during updates */

    /* Redundant row hints (for two-phase simplex with stuck artificials)
     * These point to external data from the tableau, not owned by LU */
    const int *redundant_rows;  /* Bitmap: row[i]=1 if redundant (NULL if none) */
    int num_redundant;          /* Count of redundant rows */
    int allow_regularization;   /* 1 to allow regularizing zero pivots (for rank-deficient problems) */
    int max_regularizations;    /* Limit on number of rows to regularize */
    int num_regularized;        /* Count of rows regularized in current factorization */
    double pivot_tol;           /* Dynamic pivot tolerance (default RALPH_PIVOT_TOL) */
    int last_failure_reason;    /* LUFailureReason (last failed lu_factorize/lu_update reason) */
    int telemetry_enabled;      /* 1 = collect LU telemetry counters/timers */

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
    /* sym uses ws_is_identity, ws_identity_row, ws_identity_val, ws_col_order, ws_col_order_inv */

    /* Sparse Markowitz LU */
    int mkz_enabled;         /* 1 = use Markowitz path when k >= MARKOWITZ_MIN_K */
    int mkz_pool_mult_hint;  /* Adaptive starting pool multiplier (reduces retry churn) */

    /* Supernodal LU (T2.1) */
    int sn_enabled;          /* 1 = use supernodal path when k >= SN_MIN_K */
    struct SNSymbolic_tag *sn_symbolic;  /* Cached symbolic analysis (forward decl) */
    double *sn_work;         /* Pre-allocated workspace for GEMM blocks */
    size_t sn_work_capacity; /* Size in doubles */
    int sn_calls;            /* Number of times supernodal path was attempted */
    int sn_successes;        /* Number of times supernodal path succeeded */

    /* Telemetry state */
    LUTelemetryState telemetry;

    /* W1: Sparse BTRAN readiness flag (set after each refactorization) */
    int csr_valid;           /* 1 if L/U CSC data is valid for sparse BTRAN reach */

    /* Arena allocator for fixed-size arrays (reduces ~20 mallocs to 1) */
    SHArena *arena;
} LUFactorization;

/* Forward declaration (tableau back-pointer for timing instrumentation) */
struct SimplexSolver;

/* Simplex tableau representation */
typedef struct {
    /* Problem data */
    LPModel *model;
    struct SimplexSolver *owner;   /* Owning solver (NULL when detached) */

    /* Extended problem (with slacks) */
    int n;                  /* Total variables (structural + slack) */
    int m;                  /* Number of constraints */
    SparseMatrix *A_ext;    /* Extended constraint matrix */
    SparseMatrix *basis_work; /* Reusable CSC workspace for basis extraction */
    int *basis_col_cache;   /* Basis-position -> A_ext column id from last basis build */
    int *basis_col_nnz_cache; /* Basis-position -> cached column nnz from last basis build */
    int basis_cache_valid;  /* 1 if basis_work + cache arrays match current basis/A_ext values */
    int basis_cache_total_nnz; /* Cached total nnz for basis_work */
    double *c_ext;          /* Extended objective */
    double *lb_ext;         /* Extended lower bounds */
    double *ub_ext;         /* Extended upper bounds */

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
    int *aux_row;           /* For each aux var j >= num_vars: which constraint row */
    double *aux_coef;       /* For each aux var: coefficient in that row (+1 or -1) */
    int num_aux;            /* Number of auxiliary variables */

    /* Two-phase simplex support */
    int use_two_phase;          /* 1 if using two-phase method (not Big-M) */
    double *c_original;         /* Original objective coefficients (for Phase 2) */
    int *artificial_vars;       /* Indices of artificial variables */
    int num_artificial;         /* Count of artificial variables */
    int num_equalities;         /* Count of equality constraints */
    int *redundant_rows;        /* Bitmap: row[i]=1 if redundant (stuck artificial) */
    int num_redundant;          /* Count of redundant rows */
    int redundant_rows_zeroed;  /* 1 if redundant rows have been zeroed in A_ext */

    /* Statistics */
    int iterations;
    int phase;              /* 1 or 2 */

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
    int perf_phase1_refactor_periodic_policy;
    int perf_phase1_refactor_periodic_lu_health;
    int perf_phase1_refactor_safety_forced;

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
} LPSolverTelemetryState;

/* Solver policy state (behavioral scheduling/control, not telemetry). */
typedef struct {
    int refactor_next_reason;  /* RalphRefactorReason hint consumed by tableau_refactorize */

    /* Runtime scheduling counters. */
    int periodic_policy_refactors_phase1;
    int periodic_policy_refactors_phase2;

    /* Adaptive periodic scheduler feedback (per-phase bias in [-0.25, +0.25]). */
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
} LPSolverPolicyState;

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
    int method;             /* 0=primal, 1=dual, 2=auto (dual first, primal fallback) */
    double objective_limit; /* Early-exit when obj >= limit (internal min space), default RALPH_INFINITY */
    int phase1_pricing;     /* Override pricing for Phase 1: 0=Dantzig, -1=disabled (use solver pricing) */
    int trace_phase1;       /* 1 = emit deterministic Phase-1 pivot-failure trace */
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
    int farkas_valid;       /* 1 if farkas_ray contains valid certificate */
    double *unbounded_ray;  /* Size num_vars, valid when status == UNBOUNDED */
    int unbounded_valid;    /* 1 if unbounded_ray contains a valid direction */

    /* Dual simplex enhancements */
    int use_dual_bound_flip;    /* 0=off, 1=on (default 1) */
    int use_dual_steepest_edge; /* 0=off, 1=on (default 1) */

    /* Supernodal LU (T2.1) — propagated to LU after tableau creation */
    int lu_supernode;           /* 0=off, 1=enable */

    /* D4: Flag set when primal runs after dual fallback (known degenerate) */
    int from_dual_fallback;     /* 1 = arrived from failed dual simplex */

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
    int perf_phase1_refactor_periodic_policy;
    int perf_phase1_refactor_periodic_lu_health;
    int perf_phase1_refactor_safety_forced;

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

    int sparse_dense_fallbacks;
    int used_dense_fallback_last;
    int sparse_fallback_last_reason;
    int sparse_fallback_reason_small_matrix;
    int sparse_fallback_reason_symbolic;
    int sparse_fallback_reason_numeric;
    int identity_sep_failures;

    int sn_calls;
    int sn_successes;
    int num_updates;
    int max_updates;
    int last_failure_reason;

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
} LUTelemetrySnapshot;

typedef enum {
    LP_SOLVER_STAGE_PRIMAL_SETUP = 0,
    LP_SOLVER_STAGE_DUAL = 1,
    LP_SOLVER_STAGE_PHASE1 = 2,
    LP_SOLVER_STAGE_TRANSITION = 3,
    LP_SOLVER_STAGE_PHASE2 = 4
} LPSolverStage;

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
int lu_factorize(LUFactorization *lu, const SparseMatrix *B);
int lu_factorize_sparse(LUFactorization *lu, const SparseMatrix *B);  /* Sparse with Markowitz */
int lu_factorize_dense(LUFactorization *lu, const SparseMatrix *B);   /* Dense fallback */
void lu_solve(const LUFactorization *lu, double *rhs, double *solution);
void lu_solve_transpose(const LUFactorization *lu, double *rhs, double *solution);
int lu_update(LUFactorization *lu, int leaving_pos, const double *entering_col);
int lu_needs_refactorization(const LUFactorization *lu);
const char* lu_failure_reason_string(int reason);

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
int simplex_solve(SimplexSolver *solver);

/* Dual simplex */
int dual_simplex_phase1_rescue(SimplexSolver *solver, int max_iters);
void dual_v2_clear_perturbation(SimplexTableau *tab);  /* Clear stale perturbation backup before warm start */
int dual_simplex_solve_v2(SimplexSolver *solver);  /* Clean dual Phase 2 — no primal fallbacks */
int dual_simplex_solve_from_scratch_v2(SimplexSolver *solver);  /* Clean dual from scratch (T1.3) */
int dual_phase1(SimplexSolver *solver);            /* Auxiliary-objective dual Phase 1 */
int make_dual_feasible(SimplexTableau *tab, int obj_sense); /* Flip bounds for dual feasibility */

/* Pricing strategies */
int pricing_dantzig(SimplexTableau *tableau, int *entering);
int pricing_steepest_edge(SimplexTableau *tableau, int *entering);
int pricing_devex(SimplexTableau *tableau, int *entering);
int pricing_partial(SimplexTableau *tableau, int *entering);
int pricing_heap(SimplexTableau *tableau, int *entering);

/* Ratio test */
int ratio_test_harris(SimplexTableau *tableau, int entering, int *leaving, double *theta);
int dual_ratio_test(SimplexTableau *tableau, int leaving, int *entering, double *theta);

/* Utility */
void lp_print_stats(const SimplexSolver *solver);

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
void lp_telemetry_add_btran_ms(SimplexSolver *solver,
                               double elapsed_ms);
void lp_telemetry_add_btran_timed(SimplexSolver *solver,
                                  double start_ms);
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
void lp_telemetry_lu_record_dense_factorize_ms(LUFactorization *lu,
                                               double elapsed_ms);
void lp_telemetry_lu_record_dense_factorize_timed(LUFactorization *lu,
                                                  double start_ms);
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
void lp_telemetry_lu_set_sparse_fallback_reason(LUFactorization *lu,
                                                int reason);
void lp_telemetry_lu_mark_sparse_success(LUFactorization *lu);
void lp_telemetry_lu_mark_dense_fallback(LUFactorization *lu);
void lp_telemetry_lu_clear_mkz_last_failure(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_attempt(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_success(LUFactorization *lu);
void lp_telemetry_lu_mark_mkz_failure_reason(LUFactorization *lu,
                                             int rc);
void lp_telemetry_lu_mark_mkz_failure(LUFactorization *lu,
                                      int rc);

#endif /* RALPH_LP_H */
