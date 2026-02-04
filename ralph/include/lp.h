/*
 * Ralph - LP Model and Algorithm Internals
 */

#ifndef RALPH_LP_H
#define RALPH_LP_H

#include <stdlib.h>
#include <string.h>
#include "sparse.h"
#include "ralph.h"

/* Tolerances */
#define RALPH_FEAS_TOL 1e-6
#define RALPH_OPT_TOL 1e-6
#define RALPH_PIVOT_TOL 1e-6  /* Increased for numerical stability */
#define RALPH_ZERO_TOL 1e-12
#define RALPH_INT_TOL 1e-5
#define RALPH_BIG_M 1e8       /* Artificial variable cost for Big-M method */

/* Safe free macro - NULLs pointer after freeing to prevent double-free */
#define SAFE_FREE(p) do { free(p); (p) = NULL; } while(0)

/* ============================================================================
 * Arena Allocator
 *
 * Simple bump allocator for allocating many arrays with the same lifetime.
 * Benefits:
 * - Single malloc/free instead of 20+ individual allocations
 * - No fragmentation from repeated alloc/free cycles
 * - Simpler error handling (one check, not 20)
 * - Cache-friendly contiguous memory layout
 * ============================================================================ */

typedef struct {
    char *buffer;       /* Pre-allocated memory block */
    size_t capacity;    /* Total size of buffer */
    size_t used;        /* Currently used bytes */
} RalphArena;

/* Create arena with given capacity. Returns NULL on failure. */
static inline RalphArena* ralph_arena_create(size_t capacity) {
    RalphArena *arena = (RalphArena*)malloc(sizeof(RalphArena));
    if (!arena) return NULL;

    arena->buffer = (char*)malloc(capacity);
    if (!arena->buffer) {
        free(arena);
        return NULL;
    }
    arena->capacity = capacity;
    arena->used = 0;
    return arena;
}

/* Allocate from arena with 8-byte alignment. Returns NULL if out of space. */
static inline void* ralph_arena_alloc(RalphArena *arena, size_t size) {
    if (!arena || !arena->buffer) return NULL;

    /* Align to 8 bytes for double/pointer alignment */
    size = (size + 7) & ~(size_t)7;

    if (arena->used + size > arena->capacity) {
        return NULL;  /* Out of space */
    }

    void *ptr = arena->buffer + arena->used;
    arena->used += size;
    return ptr;
}

/* Allocate and zero-initialize from arena. */
static inline void* ralph_arena_calloc(RalphArena *arena, size_t count, size_t size) {
    size_t total = count * size;
    void *ptr = ralph_arena_alloc(arena, total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

/* Reset arena for reuse (doesn't free memory, just resets position). */
static inline void ralph_arena_reset(RalphArena *arena) {
    if (arena) {
        arena->used = 0;
    }
}

/* Free arena and all memory. */
static inline void ralph_arena_free(RalphArena *arena) {
    if (arena) {
        free(arena->buffer);
        free(arena);
    }
}

/* Get remaining capacity in arena. */
static inline size_t ralph_arena_remaining(const RalphArena *arena) {
    if (!arena) return 0;
    return arena->capacity - arena->used;
}

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
    char **con_names;
    char *name;             /* Problem name */

    /* Build state for incremental constraint building (thread-safe) */
    LPModelBuildState *build_state;

} LPModel;

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

    /* Compacted spike blocks - periodically merge spikes for faster application */
    int ft_compact_interval;    /* Compact every N spikes (0 = disabled) */
    int ft_num_compacted;       /* Number of spikes already compacted */
    double *ft_compact_matrix;  /* Dense m×m matrix for compacted spikes (when used) */
    int ft_compact_valid;       /* 1 if compact_matrix is valid */

    /* Condition number monitoring */
    double min_diag_U;      /* Minimum |U[i,i]| at factorization */
    double max_diag_U;      /* Maximum |U[i,i]| at factorization */
    double cond_estimate;   /* Estimated condition number */
    double growth_factor;   /* Growth in U during updates */

    /* Pre-allocated workspace for hyper-sparse operations */
    double *hs_work1;       /* Dense workspace 1 */
    double *hs_work2;       /* Dense workspace 2 */
    int *hs_marked;         /* Marked array for reach computation */
    int *hs_idx;            /* Sparse index array */
    double *hs_val;         /* Sparse value array */
    int *hs_stack;          /* Stack for DFS in reach computation */
    double *perm_work;      /* Workspace for permutation operations */

    /* Pre-allocated workspace for dense LU fallback (m×m matrix, column-major) */
    double *dense_work;     /* Reused across factorizations to avoid O(m²) alloc */
} LUFactorization;

/* Simplex tableau representation */
typedef struct {
    /* Problem data */
    LPModel *model;

    /* Extended problem (with slacks) */
    int n;                  /* Total variables (structural + slack) */
    int m;                  /* Number of constraints */
    SparseMatrix *A_ext;    /* Extended constraint matrix */
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
    double *rhs;
    double *pivot_row;      /* Pre-allocated for simplex_pivot */
    double *tau_work;       /* Pre-allocated for steepest edge */

    /* Steepest edge / Devex weights */
    double *se_weights;     /* Steepest edge weights */
    int use_steepest_edge;
    int pricing_strategy;   /* 0=Dantzig, 1=SE, 2=Devex, 3=Partial - for pivot fn */
    int devex_refcount;     /* Reference count for Devex weight resets */

    /* Bound perturbation backup (for dual simplex anti-cycling) */
    double *perturb_backup; /* Original upper bounds before perturbation */

    /* Primal bound perturbation state (for primal simplex anti-cycling) */
    double *primal_saved_lb;    /* Saved lower bounds before perturbation */
    double *primal_saved_ub;    /* Saved upper bounds before perturbation */
    int primal_perturb_active;  /* 1 if perturbation is currently active */

    /* Partial pricing state */
    int partial_price_pos;  /* Starting position for next partial price scan */

    /* Candidate list for improved partial pricing */
    int *partial_candidates;    /* Hot set of promising variable indices */
    int partial_cand_count;     /* Current number of candidates in hot set */
    int partial_cand_capacity;  /* Allocated capacity for candidates */

    /* Lazy reduced cost computation */
    int duals_valid;            /* 1 if y[] contains valid dual values */
    int rc_all_valid;           /* 1 if rc[] contains all valid reduced costs */

    /* Auxiliary variable mapping (for cut generation) */
    int *aux_row;           /* For each aux var j >= num_vars: which constraint row */
    double *aux_coef;       /* For each aux var: coefficient in that row (+1 or -1) */
    int num_aux;            /* Number of auxiliary variables */

    /* Statistics */
    int iterations;
    int phase;              /* 1 or 2 */

    /* Pre-allocated sparse workspace for reduced cost computation */
    int *cb_sparse_idx;     /* Sparse indices for c_B (size m) */
    double *cb_sparse_val;  /* Sparse values for c_B (size m) */

    /* Arena allocator for workspace arrays (reduces 20+ mallocs to 1) */
    RalphArena *arena;

} SimplexTableau;

/* Simplex solver */
typedef struct {
    LPModel *model;
    SimplexTableau *tableau;

    /* Parameters */
    int max_iterations;
    double time_limit;
    int presolve;
    int scaling;
    int pricing_strategy;   /* 0=Dantzig, 1=Steepest edge, 2=Devex, 3=Partial */
    int verbose;

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

    /* Farkas ray (certificate of infeasibility) */
    double *farkas_ray;     /* Size num_cons, valid when status == INFEASIBLE */
    int farkas_valid;       /* 1 if farkas_ray contains valid certificate */

} SimplexSolver;

/* LP model functions */
LPModel* lp_model_create(void);
void lp_model_free(LPModel *model);
int lp_model_add_var(LPModel *model, double lb, double ub, double obj, char type);
int lp_model_add_constraint(LPModel *model, int nnz, const int *indices,
                            const double *values, char sense, double rhs);
int lp_model_finalize(LPModel *model);
LPModel* lp_model_copy(const LPModel *model);

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

/* Simplex solver functions */
SimplexSolver* simplex_create(LPModel *model);
void simplex_free(SimplexSolver *solver);
int simplex_solve(SimplexSolver *solver);

/* Dual simplex */
int dual_simplex_solve(SimplexSolver *solver);
int dual_simplex_solve_from_scratch(SimplexSolver *solver);

/* Pricing strategies */
int pricing_dantzig(SimplexTableau *tableau, int *entering);
int pricing_steepest_edge(SimplexTableau *tableau, int *entering);
int pricing_devex(SimplexTableau *tableau, int *entering);
int pricing_partial(SimplexTableau *tableau, int *entering);

/* Ratio test */
int ratio_test_harris(SimplexTableau *tableau, int entering, int *leaving, double *theta);
int dual_ratio_test(SimplexTableau *tableau, int leaving, int *entering, double *theta);

/* Utility */
void lp_print_stats(const SimplexSolver *solver);

#endif /* RALPH_LP_H */
