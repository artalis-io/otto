/*
 * detect.h - Problem Structure Detection
 *
 * Detects special structure in LP/MIP problems to enable
 * delegation to specialized solvers.
 */

#ifndef RALPH_DETECT_H
#define RALPH_DETECT_H

#include "lp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * LAP Detection
 * ============================================================================ */

/*
 * LAP signature - extracted from LP model.
 *
 * A problem has LAP structure if:
 * - Exactly 2n constraints (n row + n column constraints)
 * - Each variable appears in exactly 2 constraints with coefficient +1
 * - All constraints are equality with RHS = 1
 * - Variables are non-negative (or binary)
 */
typedef struct {
    int is_lap;             /* 1 if LAP structure detected */
    int n;                  /* Problem size (n x n assignment) */
    double *costs;          /* Cost matrix (row-major, n x n) */
    int *var_to_row;        /* var_to_row[v] = which row constraint var v is in */
    int *var_to_col;        /* var_to_col[v] = which col constraint var v is in */
    int obj_sense;          /* 1=minimize, -1=maximize */
} LAPSignature;

/*
 * Detect LAP structure in an LP model.
 *
 * Parameters:
 *   model - LP model to analyze
 *   sig   - Output: LAP signature (caller allocates)
 *
 * Returns:
 *   1 if LAP structure detected, 0 otherwise.
 *   If 1, sig->costs is allocated and must be freed.
 */
int detect_lap(const LPModel *model, LAPSignature *sig);

/*
 * Free memory allocated by detect_lap().
 */
void detect_lap_free(LAPSignature *sig);

/*
 * Solve LAP using detected structure.
 *
 * Parameters:
 *   sig      - LAP signature from detect_lap()
 *   solution - Output: variable values (size = model->num_vars)
 *   obj_val  - Output: objective value
 *
 * Returns:
 *   0 on success, -1 on error.
 */
int solve_as_lap(const LAPSignature *sig, double *solution, double *obj_val);

/* ============================================================================
 * MIP LAP Detection and Solving
 * ============================================================================ */

/*
 * MIP LAP signature - extended for use during branch-and-bound.
 *
 * Used when a MIP has LAP structure in its LP relaxation. The LAP solver
 * can be used at each B&B node by modifying costs based on variable fixings.
 */
typedef struct {
    LAPSignature base;        /* Base LAP signature */
    int num_vars;             /* Total number of variables in MIP */
    double *base_costs;       /* Original cost matrix (before modifications) */
    void *lap_workspace;      /* Reusable LAP workspace */
} MIPLAPSignature;

/*
 * Detect LAP structure in a MIP model.
 *
 * This is similar to detect_lap() but:
 * - Works with models that have integer/binary variables
 * - Allocates workspace for repeated solving during B&B
 * - Stores base costs that can be modified per-node
 *
 * Parameters:
 *   model - LP/MIP model to analyze
 *   sig   - Output: MIP LAP signature (caller allocates struct)
 *
 * Returns:
 *   1 if LAP structure detected, 0 otherwise.
 */
int detect_lap_mip(const LPModel *model, MIPLAPSignature *sig);

/*
 * Free memory allocated by detect_lap_mip().
 */
void detect_lap_mip_free(MIPLAPSignature *sig);

/*
 * Solve LAP relaxation at a B&B node.
 *
 * This solves the LP relaxation of the assignment problem given the current
 * variable bounds (fixings from branching).
 *
 * Variable fixings are handled as:
 * - x[i,j] fixed to 0: cost[i,j] = infinity (forbidden)
 * - x[i,j] fixed to 1: all other costs in row i and col j = infinity
 *
 * Parameters:
 *   sig       - MIP LAP signature from detect_lap_mip()
 *   lb        - Current lower bounds for all variables
 *   ub        - Current upper bounds for all variables
 *   solution  - Output: variable values
 *   obj_val   - Output: objective value
 *
 * Returns:
 *   0 on success, -1 on infeasible/error.
 */
int solve_lap_at_node(
    MIPLAPSignature *sig,
    const double *lb,
    const double *ub,
    double *solution,
    double *obj_val
);

/* ============================================================================
 * Runtime Configuration
 * ============================================================================ */

/*
 * Enable or disable automatic LAP detection.
 *
 * When enabled, ralph_optimize() will check if the problem has LAP
 * structure and use the specialized JVC solver if so.
 *
 * Default: disabled (0) - for fair benchmarking against LP baseline
 * Enable with: ralph_set_detect_lap(1) or ralph_set_int_param(model, "detect_special", 1)
 */
void ralph_set_detect_lap(int enabled);
int ralph_get_detect_lap(void);

#ifdef __cplusplus
}
#endif

#endif /* RALPH_DETECT_H */
