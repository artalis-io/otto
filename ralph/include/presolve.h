/*
 * Ralph - Presolve Routines
 */

#ifndef RALPH_PRESOLVE_H
#define RALPH_PRESOLVE_H

#include "lp.h"

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

    /* Statistics */
    int vars_removed;
    int cons_removed;
    int bounds_tightened;
    int coefficients_reduced;

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
    int coefficient_reduction;
    int probing;                /* For MIP only */
    int detect_redundant_rows;  /* Detect linearly dependent rows via rank */

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
int presolve_coefficient_reduction(PresolveContext *ctx);

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

/* MIP-specific presolve */
int presolve_probing(PresolveContext *ctx);
int presolve_clique_detection(PresolveContext *ctx);

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

/* Utility */
void presolve_compute_implied_bounds(PresolveContext *ctx);

#endif /* RALPH_PRESOLVE_H */
