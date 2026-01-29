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
