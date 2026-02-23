/*
 * Ralph - Benders Decomposition Internal Header
 *
 * Generic Benders decomposition for MIP problems with complicating variables.
 * This is an internal header; use ralph_mip.h for the public API.
 */

#ifndef RALPH_BENDERS_H
#define RALPH_BENDERS_H

#include "lp.h"
#include "mip.h"
#include "ralph_core.h"

/* ============================================================================
 * Data Structures
 * ============================================================================ */

/* Linking constraint information */
typedef struct {
    int constraint_idx;     /* Index in original model */
    int sub_row_idx;        /* Row index in subproblem (set during build) */
    double original_rhs;    /* Original RHS value */

    /* Master variable contributions: T*x part */
    int num_master_terms;
    int *master_var_indices;    /* Which master vars appear */
    double *master_coeffs;      /* Coefficients for master vars */

    /* Subproblem variable contributions: W*y part */
    int num_sub_terms;
    int *sub_var_indices;       /* Which sub vars appear (local indices) */
    double *sub_coeffs;         /* Coefficients for sub vars */

    char sense;                 /* Constraint sense */
} LinkingConstraint;

/* Benders cut (optimality or feasibility) */
typedef struct {
    int is_feasibility;         /* 1 = feasibility cut, 0 = optimality cut */
    int scenario;               /* Scenario index (for stochastic) */

    /* Cut: sum(coeff[i] * x[master_idx[i]]) + theta >= rhs */
    int num_terms;
    int *master_var_indices;    /* Master variable indices */
    double *coeffs;             /* Cut coefficients */
    double rhs;                 /* Cut RHS (for theta coefficient) */

    /* For optimality cuts: theta coefficient is implicitly 1 */
    /* Cut becomes: theta >= rhs - sum(coeff[i] * x[i]) */
} BendersCut;

/* Benders solver context */
typedef struct {
    /* Original model (not modified) */
    LPModel *original_model;

    /* Configuration */
    RalphBendersConfig config;

    /* Variable classification */
    int *is_master_var;         /* Boolean: is variable in master? */
    int *master_to_orig;        /* Master var index -> original index */
    int *sub_to_orig;           /* Subproblem var index -> original index */
    int *orig_to_master;        /* Original index -> master var index (-1 if not) */
    int *orig_to_sub;           /* Original index -> sub var index (-1 if not) */
    int num_master_vars;        /* Including theta if auto-created */
    int num_sub_vars;

    /* Theta variable handling */
    int theta_in_master;        /* Index of theta in master problem */
    int theta_auto_created;     /* 1 if we created theta */

    /* Constraint classification */
    int *constraint_class;      /* 0=master, 1=sub, 2=linking */
    int num_master_cons;
    int num_sub_cons;
    int num_linking_cons;

    /* Linking constraints */
    LinkingConstraint *linking;
    int num_linking;

    /* Master problem (MIP) */
    LPModel *master_model;
    MIPSolver *master_solver;

    /* Subproblem (LP) - one per scenario */
    LPModel **sub_models;
    SimplexSolver **sub_solvers;
    RalphBasis **sub_bases;     /* For warm start */

    /* Cut pool */
    BendersCut *cuts;
    int num_cuts;
    int cuts_capacity;

    /* Solution */
    double *master_solution;    /* Master variable values */
    double *master_start;       /* Staged master incumbent warm start */
    double *full_solution;      /* Full solution in original space */
    double best_ub;             /* Best upper bound (primal) */
    double best_lb;             /* Best lower bound (dual) */

    /* Statistics */
    int iterations;
    int optimality_cuts_added;
    int feasibility_cuts_added;
    int subproblems_solved;
    int master_warm_starts_attempted;
    int master_warm_starts_accepted;
    int subproblem_warm_starts;
    int subproblem_cold_starts;
    double total_time;

} BendersContext;

/* ============================================================================
 * Internal Functions
 * ============================================================================ */

/* Create Benders context from model and config */
BendersContext* benders_create(LPModel *model, const RalphBendersConfig *config);

/* Free Benders context */
void benders_free(BendersContext *ctx);

/* Partition model into master and subproblem */
int benders_partition(BendersContext *ctx);

/* Build master problem (MIP with theta) */
int benders_build_master(BendersContext *ctx);

/* Build subproblem (LP parameterized by master solution) */
int benders_build_subproblem(BendersContext *ctx, int scenario);

/* Update subproblem RHS given master solution */
int benders_update_subproblem_rhs(BendersContext *ctx, int scenario,
                                   const double *x_master);

/* Solve subproblem and extract duals/Farkas ray */
int benders_solve_subproblem(BendersContext *ctx, int scenario,
                              double *obj, double *duals, double *farkas,
                              int *is_feasible);

/* Generate optimality cut from subproblem duals */
int benders_add_optimality_cut(BendersContext *ctx, int scenario,
                                const double *duals, double sub_obj);

/* Generate feasibility cut from Farkas ray */
int benders_add_feasibility_cut(BendersContext *ctx, int scenario,
                                 const double *farkas);

/* Add all accumulated cuts to master problem */
int benders_apply_cuts_to_master(BendersContext *ctx);

/* Main Benders loop (classic algorithm) */
int benders_solve_classic(BendersContext *ctx);

/* Branch-and-Benders-cut (modern algorithm with cuts at LP nodes) */
int benders_solve_modern(BendersContext *ctx);

/* Check convergence */
int benders_check_convergence(BendersContext *ctx, double master_obj,
                               double sub_obj, double *gap);

/* Extract full solution from master + subproblem */
int benders_extract_solution(BendersContext *ctx, double *x);

/* ============================================================================
 * Internal Entry Point (called from ralph.c wrapper)
 * ============================================================================ */

/*
 * Solve an LP model using Benders decomposition.
 * This is the internal entry point; use ralph_mip_solve_benders() from ralph_mip.h
 * for the public API.
 *
 * @param lp_model The LP model (will not be modified)
 * @param config   Benders configuration
 * @param x        Output solution (may be NULL)
 * @param result   Output result info (may be NULL)
 * @return 0 on success, -1 on error
 */
int benders_solve(
    LPModel *lp_model,
    const RalphBendersConfig *config,
    double *x,
    RalphBendersResult *result
);

#endif /* RALPH_BENDERS_H */
