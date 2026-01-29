/*
 * lap.h - Linear Assignment Problem Solver
 *
 * Implements the Jonker-Volgenant-Castanon (JVC) algorithm for solving
 * the Linear Assignment Problem (LAP) in O(n^3) time.
 *
 * The LAP finds an optimal assignment of n workers to n jobs, minimizing
 * or maximizing total cost. Each worker is assigned to exactly one job,
 * and each job is assigned to exactly one worker.
 *
 * Reference:
 * R. Jonker and A. Volgenant, "A Shortest Augmenting Path Algorithm for
 * Dense and Sparse Linear Assignment Problems," Computing 38, 325-340, 1987
 */

#ifndef LAP_H
#define LAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/* Infinity constant for forbidden assignments */
#define RALPH_LAP_INFINITY 1e30

/* Numerical tolerance for comparisons */
#define RALPH_LAP_TOLERANCE 1e-10

/* Unassigned marker */
#define RALPH_LAP_UNASSIGNED (-1)

/* ============================================================================
 * Types
 * ============================================================================ */

/* Result status codes */
typedef enum {
    RALPH_LAP_SUCCESS = 0,
    RALPH_LAP_INFEASIBLE = 1,      /* No valid assignment exists */
    RALPH_LAP_INVALID_INPUT = 2,   /* Invalid input parameters */
    RALPH_LAP_MEMORY_ERROR = 3     /* Memory allocation failed */
} RalphLapStatus;

/* Forward declaration for workspace (opaque type) */
typedef struct RalphLapWorkspace RalphLapWorkspace;

/* Optimization direction */
typedef enum {
    RALPH_LAP_MINIMIZE = 0,
    RALPH_LAP_MAXIMIZE = 1
} RalphLapObjective;

/* ============================================================================
 * Dense LAP Solver (JVC Algorithm)
 * ============================================================================ */

/*
 * Solve the dense Linear Assignment Problem using JVC algorithm.
 *
 * Parameters:
 *   n          - Problem dimension (n x n cost matrix)
 *   cost       - Cost matrix in row-major order (cost[i*n + j] = cost of i->j)
 *   objective  - RALPH_LAP_MINIMIZE or RALPH_LAP_MAXIMIZE
 *   row_sol    - Output: row_sol[i] = column assigned to row i (size n)
 *   col_sol    - Output: col_sol[j] = row assigned to column j (size n, can be NULL)
 *   u          - Output: row dual variables (size n, can be NULL)
 *   v          - Output: column dual variables (size n, can be NULL)
 *   total_cost - Output: total assignment cost (can be NULL)
 *
 * Returns:
 *   RALPH_LAP_SUCCESS on success, error code otherwise.
 *
 * The optimal assignment satisfies the complementary slackness conditions:
 *   cost[i][j] - u[i] - v[j] = 0 for all assigned pairs (i, j)
 *   cost[i][j] - u[i] - v[j] >= 0 for minimization
 *
 * Time complexity: O(n^3)
 * Space complexity: O(n^2) for cost matrix, O(n) for working arrays
 */
RalphLapStatus ralph_lap_solve(
    int n,
    const double *cost,
    RalphLapObjective objective,
    int *row_sol,
    int *col_sol,
    double *u,
    double *v,
    double *total_cost
);

/* ============================================================================
 * Workspace API (for repeated solves)
 * ============================================================================ */

/*
 * Create a reusable workspace for LAP solving.
 *
 * The workspace pre-allocates all working arrays for problems up to max_n.
 * This amortizes allocation overhead when solving multiple LAP instances.
 *
 * Parameters:
 *   max_n - Maximum problem dimension this workspace can handle
 *
 * Returns:
 *   Pointer to workspace, or NULL on allocation failure.
 */
RalphLapWorkspace* ralph_lap_workspace_create(int max_n);

/*
 * Free a workspace and all its memory.
 */
void ralph_lap_workspace_free(RalphLapWorkspace *ws);

/*
 * Get the maximum dimension this workspace supports.
 */
int ralph_lap_workspace_max_n(const RalphLapWorkspace *ws);

/*
 * Solve LAP using a pre-allocated workspace.
 *
 * Identical to ralph_lap_solve() but uses provided workspace instead of
 * allocating new arrays. This is faster for repeated solves.
 *
 * Parameters:
 *   n, cost, objective, row_sol, col_sol, u, v, total_cost - See ralph_lap_solve()
 *   ws - Pre-allocated workspace (must have max_n >= n)
 *
 * Returns:
 *   RALPH_LAP_SUCCESS on success, RALPH_LAP_INVALID_INPUT if n > ws->max_n.
 */
RalphLapStatus ralph_lap_solve_with_workspace(
    int n,
    const double *cost,
    RalphLapObjective objective,
    int *row_sol,
    int *col_sol,
    double *u,
    double *v,
    double *total_cost,
    RalphLapWorkspace *ws
);

/* ============================================================================
 * Rectangular LAP Solver
 * ============================================================================ */

/*
 * Solve rectangular Linear Assignment Problem (m workers, n jobs).
 *
 * Handles non-square assignment problems:
 * - m < n: All workers assigned, some jobs unassigned
 * - m > n: All jobs assigned, some workers unassigned
 * - m == n: Equivalent to ralph_lap_solve()
 *
 * Parameters:
 *   m          - Number of rows (workers)
 *   n          - Number of columns (jobs)
 *   cost       - Cost matrix in row-major order, size m x n
 *   objective  - RALPH_LAP_MINIMIZE or RALPH_LAP_MAXIMIZE
 *   row_sol    - Output: row_sol[i] = column assigned to row i, or -1 if unassigned
 *   col_sol    - Output: col_sol[j] = row assigned to column j, or -1 if unassigned (can be NULL)
 *   total_cost - Output: total assignment cost (can be NULL)
 *
 * Returns:
 *   RALPH_LAP_SUCCESS on success, error code otherwise.
 *
 * Time complexity: O(max(m,n)^3)
 */
RalphLapStatus ralph_lap_solve_rect(
    int m,
    int n,
    const double *cost,
    RalphLapObjective objective,
    int *row_sol,
    int *col_sol,
    double *total_cost
);

/* ============================================================================
 * Sparse LAP Solver
 * ============================================================================ */

/*
 * Solve the sparse Linear Assignment Problem.
 *
 * For problems where many assignments are forbidden (infinite cost),
 * the sparse version can be more efficient.
 *
 * Parameters:
 *   n           - Number of rows (= number of columns for square LAP)
 *   nnz         - Number of finite-cost entries
 *   row_ptr     - CSR row pointers (size n+1)
 *   col_idx     - CSR column indices (size nnz)
 *   values      - CSR cost values (size nnz)
 *   objective   - RALPH_LAP_MINIMIZE or RALPH_LAP_MAXIMIZE
 *   row_sol     - Output: row_sol[i] = column assigned to row i
 *   col_sol     - Output: col_sol[j] = row assigned to column j (can be NULL)
 *   total_cost  - Output: total assignment cost (can be NULL)
 *
 * Returns:
 *   RALPH_LAP_SUCCESS on success, RALPH_LAP_INFEASIBLE if no valid assignment.
 */
RalphLapStatus ralph_lap_solve_sparse(
    int n,
    int nnz,
    const int *row_ptr,
    const int *col_idx,
    const double *values,
    RalphLapObjective objective,
    int *row_sol,
    int *col_sol,
    double *total_cost
);

/* ============================================================================
 * Verification and LP Comparison
 * ============================================================================ */

/*
 * Verify that an assignment is valid (permutation) and compute its cost.
 *
 * Parameters:
 *   n        - Problem dimension
 *   cost     - Cost matrix (row-major)
 *   row_sol  - Assignment to verify
 *   cost_out - Output: computed cost (can be NULL)
 *
 * Returns:
 *   1 if valid permutation, 0 otherwise
 */
int ralph_lap_verify(
    int n,
    const double *cost,
    const int *row_sol,
    double *cost_out
);

/*
 * Solve LAP using general LP formulation (for verification/comparison).
 *
 * This uses the Ralph LP solver with the standard assignment formulation:
 *   min sum_{i,j} c_{ij} x_{ij}
 *   s.t. sum_j x_{ij} = 1  for all i  (each row assigned once)
 *        sum_i x_{ij} = 1  for all j  (each column assigned once)
 *        x_{ij} >= 0
 *
 * Note: This is much slower than ralph_lap_solve() but useful for verification.
 *
 * Parameters:
 *   n          - Problem dimension
 *   cost       - Cost matrix (row-major)
 *   objective  - RALPH_LAP_MINIMIZE or RALPH_LAP_MAXIMIZE
 *   row_sol    - Output: row_sol[i] = column assigned to row i
 *   total_cost - Output: total assignment cost (can be NULL)
 *
 * Returns:
 *   RALPH_LAP_SUCCESS on success, error code otherwise.
 */
RalphLapStatus ralph_lap_solve_lp(
    int n,
    const double *cost,
    RalphLapObjective objective,
    int *row_sol,
    double *total_cost
);

/*
 * Get status string for error reporting.
 */
const char* ralph_lap_status_string(RalphLapStatus status);

/* ============================================================================
 * Runtime Configuration
 * ============================================================================ */

/*
 * Enable or disable OpenMP parallelization.
 *
 * When disabled, the solver uses only sequential code (no threading).
 * SIMD vectorization hints are still used but don't require OpenMP runtime.
 *
 * Parameters:
 *   enabled - 1 to enable parallelization (default), 0 to disable
 */
void ralph_lap_set_parallel(int enabled);

/*
 * Check if parallelization is enabled.
 *
 * Returns:
 *   1 if enabled, 0 if disabled
 */
int ralph_lap_get_parallel(void);

#ifdef __cplusplus
}
#endif

#endif /* LAP_H */
