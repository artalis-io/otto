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

    /* Iteration control */
    int max_rounds;
    int current_round;

    /* Working arrays */
    int *row_deleted;
    int *col_deleted;
    double *row_lb;             /* Implied row lower bounds */
    double *row_ub;             /* Implied row upper bounds */

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

/* MIP-specific presolve */
int presolve_probing(PresolveContext *ctx);
int presolve_clique_detection(PresolveContext *ctx);

/* Utility */
void presolve_compute_implied_bounds(PresolveContext *ctx);

#endif /* RALPH_PRESOLVE_H */
