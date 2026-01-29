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
 * Initialize warm start from previous dual variables.
 *
 * This stores the dual variables (u, v) and optionally the previous solution
 * in the workspace. The next solve will use these as starting point, which
 * can significantly speed up solving similar problems.
 *
 * Parameters:
 *   ws      - Workspace to initialize
 *   n       - Problem dimension
 *   u       - Previous row dual variables (size n, can be NULL)
 *   v       - Previous column dual variables (size n, required)
 *   row_sol - Previous row solution (size n, can be NULL)
 *   col_sol - Previous column solution (size n, can be NULL)
 *
 * Returns:
 *   RALPH_LAP_SUCCESS on success, error code otherwise.
 *
 * Note: If row_sol and col_sol are provided, the solver can potentially
 * repair the solution locally for small cost changes. If only v is provided,
 * the solver skips Phase 1 but still runs Phase 2-4.
 */
RalphLapStatus ralph_lap_warm_start(
    RalphLapWorkspace *ws,
    int n,
    const double *u,
    const double *v,
    const int *row_sol,
    const int *col_sol
);

/*
 * Clear warm start state from workspace.
 *
 * After calling this, the next solve will be a cold start (full solve).
 */
void ralph_lap_warm_start_clear(RalphLapWorkspace *ws);

/*
 * Check if workspace has valid warm start data.
 *
 * Returns:
 *   1 if warm start is valid, 0 otherwise
 */
int ralph_lap_warm_start_valid(const RalphLapWorkspace *ws);

/*
 * Solve LAP with warm start using a pre-allocated workspace.
 *
 * If the workspace has valid warm start data (from ralph_lap_warm_start()),
 * this function will use it to speed up the solve. Otherwise, it behaves
 * identically to ralph_lap_solve_with_workspace().
 *
 * After solving, the workspace automatically saves the solution for potential
 * warm start on the next call (unless save_for_warm_start is 0).
 *
 * Parameters:
 *   n, cost, objective, row_sol, col_sol, u, v, total_cost - See ralph_lap_solve()
 *   ws                 - Pre-allocated workspace (must have max_n >= n)
 *   save_for_warm_start - If non-zero, save solution for next warm start
 *
 * Returns:
 *   RALPH_LAP_SUCCESS on success, error code otherwise.
 */
RalphLapStatus ralph_lap_solve_warm(
    int n,
    const double *cost,
    RalphLapObjective objective,
    int *row_sol,
    int *col_sol,
    double *u,
    double *v,
    double *total_cost,
    RalphLapWorkspace *ws,
    int save_for_warm_start
);

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
 * Callback-Based LAP Solver (O(n) memory)
 * ============================================================================ */

/*
 * Cost function callback type.
 *
 * Returns the cost of assigning row i to column j.
 * Return RALPH_LAP_INFINITY for forbidden assignments.
 *
 * Parameters:
 *   i         - Row index (0 to n-1)
 *   j         - Column index (0 to n-1)
 *   user_data - User-provided context pointer
 *
 * Returns:
 *   Cost value for assignment (i, j)
 */
typedef double (*RalphLapCostFn)(int i, int j, void *user_data);

/*
 * Solve LAP using a cost callback instead of storing the full matrix.
 *
 * This is useful for very large problems where storing n² costs is prohibitive.
 * The callback is invoked on-demand during the JVC algorithm phases.
 *
 * Memory usage: O(n) instead of O(n²)
 * Time complexity: O(n³) with higher constant factor due to callback overhead
 *
 * Parameters:
 *   n          - Problem dimension
 *   cost_fn    - Callback function that returns cost(i, j)
 *   user_data  - User context passed to cost_fn
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
 * Example - Euclidean distance assignment:
 *
 *   typedef struct { double *x, *y; } Points;
 *
 *   double euclidean_cost(int i, int j, void *user_data) {
 *       Points *p = (Points *)user_data;
 *       double dx = p->x[i] - p->x[j];
 *       double dy = p->y[i] - p->y[j];
 *       return sqrt(dx*dx + dy*dy);
 *   }
 *
 *   ralph_lap_solve_callback(n, euclidean_cost, &points, RALPH_LAP_MINIMIZE,
 *                            row_sol, NULL, NULL, NULL, &cost);
 */
RalphLapStatus ralph_lap_solve_callback(
    int n,
    RalphLapCostFn cost_fn,
    void *user_data,
    RalphLapObjective objective,
    int *row_sol,
    int *col_sol,
    double *u,
    double *v,
    double *total_cost
);

/*
 * Solve callback-based LAP with pre-allocated workspace.
 *
 * Same as ralph_lap_solve_callback() but uses provided workspace
 * to avoid repeated allocations.
 *
 * Parameters:
 *   n, cost_fn, user_data, objective, row_sol, col_sol, u, v, total_cost
 *       - See ralph_lap_solve_callback()
 *   ws  - Pre-allocated workspace (must have max_n >= n)
 *
 * Returns:
 *   RALPH_LAP_SUCCESS on success, error code otherwise.
 */
RalphLapStatus ralph_lap_solve_callback_with_workspace(
    int n,
    RalphLapCostFn cost_fn,
    void *user_data,
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
 * the sparse version is much more efficient than the dense solver.
 * Uses a native sparse JVC algorithm that only iterates over finite-cost edges.
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
 *
 * Note: The problem is infeasible if any row has zero finite-cost edges,
 * or if the bipartite graph has no perfect matching.
 *
 * Time complexity: O(n * nnz) average case, O(n² * nnz/n) = O(n * nnz) worst case
 * Space complexity: O(n + nnz)
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

/*
 * Enable or disable ε-scaling auction algorithm.
 *
 * The ε-scaling auction provides guaranteed polynomial convergence O(n³ log(nC))
 * by progressively refining the solution through decreasing epsilon values.
 *
 * Benefits:
 * - Guaranteed convergence (no cycling on degenerate problems)
 * - More predictable iteration counts
 * - Better handling of tied/near-tied costs
 *
 * Trade-offs:
 * - Slightly higher overhead on easy problems
 * - Multiple auction passes required
 *
 * Parameters:
 *   enabled - 1 to enable ε-scaling (default: 0), 0 to use standard auction
 */
void ralph_lap_set_epsilon_scaling(int enabled);

/*
 * Check if ε-scaling auction is enabled.
 *
 * Returns:
 *   1 if enabled, 0 if disabled
 */
int ralph_lap_get_epsilon_scaling(void);

/*
 * Set the ε-scaling factor (default: 4.0).
 *
 * Each iteration divides epsilon by this factor. Larger values mean fewer
 * passes but potentially more work per pass. Typical values: 2-10.
 *
 * Parameters:
 *   factor - Scaling factor (must be > 1.0)
 */
void ralph_lap_set_epsilon_factor(double factor);

/*
 * Get the current ε-scaling factor.
 *
 * Returns:
 *   Current scaling factor
 */
double ralph_lap_get_epsilon_factor(void);

#ifdef __cplusplus
}
#endif

#endif /* LAP_H */
