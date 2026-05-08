/*
 * Ralph - Presolve Routines
 */

#ifndef RALPH_PRESOLVE_H
#define RALPH_PRESOLVE_H

#include "lp.h"

/* Presolve technique bitmask (for selective enable/disable) */
#define PRESOLVE_FIXED_VARS        (1u << 0)
#define PRESOLVE_EMPTY_ROWS        (1u << 1)
#define PRESOLVE_EMPTY_COLS        (1u << 2)
#define PRESOLVE_SINGLETON_ROWS    (1u << 3)
#define PRESOLVE_SINGLETON_COLS    (1u << 4)
#define PRESOLVE_IMPLIED_FREE      (1u << 5)
#define PRESOLVE_DOUBLETON_EQ      (1u << 6)
#define PRESOLVE_FORCING           (1u << 7)
#define PRESOLVE_BOUND_TIGHTENING  (1u << 8)
#define PRESOLVE_PROPORTIONAL_ROWS (1u << 9)
#define PRESOLVE_PROPORTIONAL_COLS (1u << 10)
#define PRESOLVE_PROBING           (1u << 11)
#define PRESOLVE_SHIFT_BOUNDS      (1u << 12)
#define PRESOLVE_REDUNDANT_ROWS    (1u << 13)
/* Safe presolve mask: lightweight techniques that are numerically reliable.
 * FIXED_VARS + EMPTY_ROWS + EMPTY_COLS + SINGLETON_ROWS + BOUND_TIGHTENING
 * + SHIFT_BOUNDS. Redundant-row detection is intentionally excluded from the
 * default safe mask because a false redundant-row classification changes the
 * feasible region and postsolve cannot reconstruct the removed row. */
#define PRESOLVE_SAFE              0x110Fu

/* PRESOLVE_ALL includes all techniques. Use with caution — some combinations
 * (IMPLIED_FREE, PROPORTIONAL_COLS) have known correctness issues on certain
 * problem classes. */
#define PRESOLVE_ALL               0xFFFFu

/* Postsolve operation types for LIFO replay */
typedef enum {
    POSTSOLVE_FIXED_VAR,       /* Variable fixed to a value */
    POSTSOLVE_SUBSTITUTION,    /* x_elim = offset + factor * x_remain */
    POSTSOLVE_SHIFT,           /* x_orig = x_shifted + value (bound shift) */
} PostsolveOpType;

typedef struct {
    PostsolveOpType type;
    int var;                   /* Primary variable (original index) */
    int var2;                  /* Secondary variable (for substitution, original index) */
    double value;              /* Fixed value or offset */
    double factor;             /* Multiplication factor (for substitution) */
} PostsolveOp;

#ifdef __cplusplus
extern "C" {
#endif

/* Presolve result */
typedef struct {
    LPModel *reduced_model;     /* Presolved model */

    /* Mapping from reduced to original */
    int num_fixed_vars;
    int *fixed_vars;            /* Original indices of fixed variables */
    double *fixed_values;       /* Values they were fixed to */

    int num_removed_cons;
    int *removed_cons;          /* Original indices of removed constraints */

    /* Mapping arrays */
    int *var_map;               /* reduced var -> original var (-1 if removed) */
    int *con_map;               /* reduced con -> original con (-1 if removed) */
    int *var_map_inv;           /* original var -> reduced var (-1 if removed) */
    int *con_map_inv;           /* original con -> reduced con (-1 if removed) */

    /* Variable type preservation for MIP */
    char *orig_var_types;       /* Original variable types before presolve */
    int num_orig_vars;          /* Original number of variables */

    /* Bound changes for postsolve */
    int num_bound_changes;
    int *bound_change_vars;
    double *old_lb;
    double *old_ub;

    /* Postsolve stack (LIFO — replay in reverse order) */
    int num_postsolve_ops;
    int postsolve_capacity;
    PostsolveOp *postsolve_stack;

    /* Statistics */
    int rounds;                 /* Number of presolve rounds executed */
    int vars_removed;
    int cons_removed;
    int bounds_tightened;
    int matrix_rank;            /* Computed rank of constraint matrix (0 if not computed) */
    int redundant_rows_found;   /* Count of linearly dependent rows removed */

} PresolveResult;

/* Presolve context */
typedef struct {
    LPModel *original;
    LPModel *working;

    /* Flags for which reductions to apply */
    int remove_fixed_vars;
    int remove_empty_rows;
    int remove_empty_cols;
    int remove_singleton_rows;
    int remove_singleton_cols;
    int remove_forcing_cons;
    int bound_tightening;
    int probing;                /* For MIP only */
    int detect_redundant_rows;  /* Detect linearly dependent rows via rank */
    unsigned int technique_mask; /* Bitmask controlling individual techniques (0xFFFF=all) */

    /* Iteration control */
    int max_rounds;
    int current_round;

    /* Working arrays */
    int *row_deleted;
    int *col_deleted;
    double *row_lb;             /* Implied row lower bounds */
    double *row_ub;             /* Implied row upper bounds */

    /* Redundant row detection statistics */
    int redundant_rows_found;   /* Count of linearly dependent rows */
    int matrix_rank;            /* Computed rank of constraint matrix */

} PresolveContext;

/* Main presolve interface */
PresolveResult* presolve(LPModel *model);
PresolveResult* presolve_with_mask(LPModel *model, unsigned int technique_mask);
void presolve_free(PresolveResult *result);

/* Postsolve: recover original solution from presolved solution */
int postsolve(const PresolveResult *result, const double *reduced_solution,
              double *original_solution);

/* Individual presolve operations */
int presolve_remove_fixed_vars(PresolveContext *ctx);
int presolve_remove_empty_rows(PresolveContext *ctx);
int presolve_remove_empty_cols(PresolveContext *ctx);
int presolve_singleton_rows(PresolveContext *ctx);
int presolve_singleton_cols(PresolveContext *ctx);
int presolve_forcing_constraints(PresolveContext *ctx);
int presolve_bound_tightening(PresolveContext *ctx);
/* Proportional row detection: remove duplicate/dominated parallel rows */
int presolve_proportional_rows(PresolveContext *ctx);

/* Proportional column detection: fix dominated parallel columns */
int presolve_proportional_cols(PresolveContext *ctx);

/* Shift variable bounds: x' = x - lb so all lower bounds are zero */
int presolve_shift_bounds(PresolveContext *ctx, PresolveResult *result);

/*
 * Detect and remove linearly dependent (redundant) rows.
 *
 * Uses Gaussian elimination with partial pivoting to compute the rank
 * of the constraint matrix. Rows that reduce to all-zeros are redundant
 * and can be safely removed (unless the RHS is non-zero, which indicates
 * infeasibility).
 *
 * This is essential for problems like beaconfd which have many equality
 * constraints where some are linear combinations of others.
 *
 * Parameters:
 *   ctx - Presolve context with working model
 *
 * Returns:
 *   Number of redundant rows removed, or -1 if infeasible
 *   (inconsistent system where 0 = non-zero).
 *
 * Complexity: O(m * n * min(m,n)) for dense elimination.
 *
 * Note: This should be called AFTER other presolve operations that might
 * make rows empty or reveal hidden redundancy.
 */
int presolve_detect_redundant_rows(PresolveContext *ctx);

/* Doubleton equality elimination: a*x + b*y = c → substitute one variable */
int presolve_doubleton_equality(PresolveContext *ctx, PresolveResult *result);

/* Implied free variable detection: remove redundant variable bounds */
int presolve_implied_free(PresolveContext *ctx);

/* MIP-specific presolve */
int presolve_probing(PresolveContext *ctx);

/* Set covering/partitioning specific presolve */

/*
 * Essential set detection for SCP.
 *
 * If an element (constraint) is covered by only one set (variable),
 * that set must be selected. Fixes the variable to 1 and propagates.
 *
 * For set covering (>=): reduces RHS and may remove constraint
 * For set partitioning (=): reduces RHS to 0 and removes constraint
 *
 * Parameters:
 *   ctx   - Presolve context with working model
 *   model must have SCP structure (binary vars, 0-1 coefficients)
 *
 * Returns:
 *   Number of variables fixed, or -1 if infeasible.
 */
int presolve_scp_essential_sets(PresolveContext *ctx);

/*
 * Row dominance reduction for SCP.
 *
 * For set covering (>=): row i dominates row j if every set covering i
 * also covers j (and RHS[i] >= RHS[j]). The dominated row i can be removed.
 *
 * For set partitioning (=): dominance doesn't apply (exact coverage required).
 *
 * Parameters:
 *   ctx - Presolve context
 *
 * Returns:
 *   Number of rows removed, or -1 on error.
 */
int presolve_scp_row_dominance(PresolveContext *ctx);

/*
 * Column dominance reduction for SCP.
 *
 * Column j dominates column k if:
 *   - Set j covers everything set k covers (column j >= column k elementwise)
 *   - Cost c[j] <= c[k]
 *
 * The dominated column k can be fixed to 0.
 *
 * Parameters:
 *   ctx - Presolve context
 *
 * Returns:
 *   Number of variables fixed to 0, or -1 on error.
 */
int presolve_scp_column_dominance(PresolveContext *ctx);

/*
 * Combined SCP presolve pass.
 *
 * Runs essential sets, row dominance, and column dominance
 * iteratively until no more reductions are found.
 *
 * Parameters:
 *   ctx - Presolve context
 *
 * Returns:
 *   Total reductions made, or -1 if infeasible.
 */
int presolve_scp(PresolveContext *ctx);

/* Row activity bounds result */
typedef struct {
    double lb;           /* Lower bound on a'x */
    double ub;           /* Upper bound on a'x */
    double abs_sum;      /* Sum of |contributions| for cancellation detection */
    int lb_finite;       /* All lower bound contributions were finite */
    int ub_finite;       /* All upper bound contributions were finite */
} RowBounds;

/*
 * Compute row activity bounds: lb <= a'x <= ub given variable bounds.
 *
 * For each variable j with coefficient a_ij:
 *   - If a_ij > 0: contributes a_ij*lb_j to row lb, a_ij*ub_j to row ub
 *   - If a_ij < 0: contributes a_ij*ub_j to row lb, a_ij*lb_j to row ub
 *
 * Tracks finiteness flags and absolute contribution sum for cancellation
 * detection. This is the shared primitive used by forcing constraints,
 * bound tightening, and implied free detection.
 */
void compute_row_bounds(const double *row, int n,
                        const double *var_lb, const double *var_ub,
                        const int *col_deleted, RowBounds *out);

/* Utility */
void presolve_compute_implied_bounds(PresolveContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* RALPH_PRESOLVE_H */
