/*
 * netflow.h - Network Simplex Solver for Minimum Cost Network Flow
 *
 * Implements the network simplex algorithm for solving Minimum Cost
 * Network Flow (MCNF) problems. Network simplex exploits the special
 * structure of network constraints for O(nm) per-iteration complexity
 * vs O(n^2 m) for general simplex.
 *
 * The MCNF problem:
 *   min  sum_{(i,j) in A} c_{ij} * x_{ij}
 *   s.t. sum_{j} x_{ij} - sum_{j} x_{ji} = b_i  for all nodes i
 *        l_{ij} <= x_{ij} <= u_{ij}            for all arcs (i,j)
 *
 * Where:
 *   - A is the set of arcs
 *   - c_{ij} is the cost per unit flow on arc (i,j)
 *   - x_{ij} is the flow on arc (i,j)
 *   - b_i is the supply at node i (negative = demand)
 *   - l_{ij}, u_{ij} are lower and upper bounds on arc flow
 *
 * Reference:
 * R.K. Ahuja, T.L. Magnanti, J.B. Orlin, "Network Flows: Theory,
 * Algorithms, and Applications," Prentice Hall, 1993.
 */

#ifndef NETFLOW_H
#define NETFLOW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

/* Infinity constant for unbounded capacity */
#define RALPH_NETFLOW_INFINITY 1e30

/* Numerical tolerance for comparisons */
#define RALPH_NETFLOW_TOLERANCE 1e-9

/* Big-M cost for artificial arcs */
#define RALPH_NETFLOW_BIG_M 1e12

/* Default maximum iterations (0 = unlimited, use heuristic) */
#define RALPH_NETFLOW_DEFAULT_MAX_ITER 0

/* Candidate list parameters */
#define RALPH_NETFLOW_MIN_LIST_SIZE 50
#define RALPH_NETFLOW_MAX_LIST_SIZE 500
#define RALPH_NETFLOW_LIST_FACTOR 10      /* list_size = num_arcs / factor */
#define RALPH_NETFLOW_REBUILD_FACTOR 3    /* rebuild every list_size * factor pivots */

/* Maximum problem size (for DoS protection) */
#define RALPH_NETFLOW_MAX_NODES 10000000
#define RALPH_NETFLOW_MAX_ARCS  100000000

/* Iteration limit multiplier when using heuristic */
#define RALPH_NETFLOW_ITER_MULTIPLIER 10
#define RALPH_NETFLOW_MIN_ITERATIONS 100000

/* Infinity threshold factor (value >= INFINITY * factor is considered infinite) */
#define RALPH_NETFLOW_INF_THRESHOLD 0.5

/* Optimality check tolerance multiplier */
#define RALPH_NETFLOW_OPT_TOL_MULTIPLIER 10

/* ============================================================================
 * Types
 * ============================================================================ */

/* Result status codes */
typedef enum {
    RALPH_NETFLOW_OPTIMAL = 0,          /* Optimal solution found */
    RALPH_NETFLOW_INFEASIBLE = 1,       /* No feasible flow exists */
    RALPH_NETFLOW_UNBOUNDED = 2,        /* Unbounded (negative cost cycle with infinite capacity) */
    RALPH_NETFLOW_MAX_ITERATIONS = 3,   /* Iteration limit reached */
    RALPH_NETFLOW_INVALID_INPUT = 4,    /* Invalid input parameters */
    RALPH_NETFLOW_OUT_OF_MEMORY = 5     /* Memory allocation failed */
} RalphNetflowStatus;

/* Optimization direction */
typedef enum {
    RALPH_NETFLOW_MINIMIZE = 0,
    RALPH_NETFLOW_MAXIMIZE = 1
} RalphNetflowObjective;

/* Arc state in basis */
typedef enum {
    RALPH_NETFLOW_AT_LOWER = 0,         /* Non-basic at lower bound */
    RALPH_NETFLOW_BASIC = 1,            /* Basic (in spanning tree) */
    RALPH_NETFLOW_AT_UPPER = 2          /* Non-basic at upper bound */
} RalphNetflowArcState;

/* Algorithm selection */
typedef enum {
    RALPH_NETFLOW_ALG_STANDARD = 0,     /* Single optimal flow */
    RALPH_NETFLOW_ALG_K_BEST = 1,       /* k-best via partitioning (future) */
    RALPH_NETFLOW_ALG_BOTTLENECK = 2    /* Minimax objective (future) */
} RalphNetflowAlgorithm;

/* Pricing rule selection */
typedef enum {
    RALPH_NETFLOW_PRICING_CANDIDATE = 0, /* Candidate list (default, fast) */
    RALPH_NETFLOW_PRICING_FIRST = 1,     /* First eligible (simple) */
    RALPH_NETFLOW_PRICING_BEST = 2       /* Most violated (more pivots but better) */
} RalphNetflowPricing;

/* Forward declaration for workspace (opaque type) */
typedef struct RalphNetflowWorkspace RalphNetflowWorkspace;

/* ============================================================================
 * Problem Definition
 * ============================================================================ */

/*
 * Network flow problem specification (forward star representation).
 *
 * The network is defined by:
 *   - num_nodes nodes numbered 0 to num_nodes-1
 *   - num_arcs arcs numbered 0 to num_arcs-1
 *   - Each arc a has tail[a] -> head[a] with cost[a], capacity[a], lower[a]
 *   - Each node i has supply[i] (negative = demand)
 *
 * Feasibility requires: sum(supply) = 0 (total supply equals total demand)
 */
typedef struct {
    int num_nodes;                  /* Number of nodes */
    int num_arcs;                   /* Number of arcs */

    const int *tail;                /* Source node of each arc (size num_arcs) */
    const int *head;                /* Destination node of each arc (size num_arcs) */
    const double *cost;             /* Cost per unit flow (size num_arcs) */
    const double *capacity;         /* Upper bound (size num_arcs, NULL = infinite) */
    const double *lower;            /* Lower bound (size num_arcs, NULL = zero) */
    const double *supply;           /* Node supply/demand (size num_nodes) */

    RalphNetflowObjective objective; /* MINIMIZE or MAXIMIZE */
} RalphNetflowProblem;

/* ============================================================================
 * Solver Options
 * ============================================================================ */

/*
 * Solver options for controlling algorithm behavior.
 *
 * Orthogonal concepts are combined through flags rather than separate functions:
 * - Algorithm: standard / k-best / bottleneck
 * - Execution: cold start / warm start
 * - Pricing: candidate list / first eligible / best
 * - Scaling: none / cost scaling
 */
typedef struct {
    /* Algorithm selection */
    RalphNetflowAlgorithm algorithm;  /* STANDARD, K_BEST, BOTTLENECK */
    int k;                            /* For k-best: number of solutions (default: 1) */

    /* Warm start control
     * When warm_start=1 and workspace has valid warm start data:
     * - Skips Phase 1 (artificial arc initialization)
     * - Reuses basis (tree structure) from previous solve
     * - Recomputes potentials for new costs
     * - Significantly faster when only costs change (same supply/demand)
     */
    int warm_start;                   /* 1 = use workspace warm start data if valid */
    int save_warm_start;              /* 1 = save solution for next warm start (default: 1) */

    /* Pricing strategy */
    RalphNetflowPricing pricing;      /* Pricing rule selection */

    /* Cost scaling (for degeneracy handling) */
    int cost_scaling;                 /* 1 = enable epsilon cost scaling */
    double epsilon_factor;            /* Cost scaling reduction factor (default: 4.0) */

    /* Limits and output */
    int64_t max_iterations;           /* Maximum pivots (0 = auto based on problem size) */
    int verbosity;                    /* 0 = silent, 1 = summary, 2 = iteration log */
} RalphNetflowOptions;

/* Default options initializer */
#define RALPH_NETFLOW_OPTIONS_DEFAULT { \
    .algorithm = RALPH_NETFLOW_ALG_STANDARD, \
    .k = 1, \
    .warm_start = 0, \
    .save_warm_start = 1, \
    .pricing = RALPH_NETFLOW_PRICING_CANDIDATE, \
    .cost_scaling = 0, \
    .epsilon_factor = 4.0, \
    .max_iterations = RALPH_NETFLOW_DEFAULT_MAX_ITER, \
    .verbosity = 0 \
}

/* ============================================================================
 * Path Structure (for flow decomposition)
 * ============================================================================ */

/*
 * A path in a flow decomposition.
 *
 * Any feasible network flow can be decomposed into at most m (num_arcs) paths
 * from sources to sinks, plus cycles. For acyclic networks, the decomposition
 * is unique up to path ordering.
 *
 * Use case: Given an optimal shipment plan (flow), decompose it into individual
 * routes that trucks/shipments would follow.
 */
typedef struct {
    int *arcs;              /* Arc indices in path order (source to sink) */
    int num_arcs;           /* Number of arcs in this path */
    int source;             /* Source node (supply > 0) */
    int sink;               /* Sink node (supply < 0) */
    double flow;            /* Flow amount on this path */
    double cost;            /* Total cost = sum(cost[a]) * flow */
    double unit_cost;       /* Cost per unit flow = sum(cost[a]) */
} RalphNetflowPath;

/* ============================================================================
 * Result Structure
 * ============================================================================ */

/*
 * Result structure filled by the solver.
 *
 * Caller provides storage for flow array (size num_arcs for standard,
 * k * num_arcs for k-best).
 *
 * For warm start support, provide potential and arc_state arrays to receive
 * the dual solution that can be used to warm start subsequent solves.
 */
typedef struct {
    RalphNetflowStatus status;      /* Solver status */

    /* Primary solution */
    double objective;               /* Optimal objective value (first solution) */
    double *flow;                   /* Arc flows: [num_arcs] or [num_found × num_arcs] for k-best */

    /* For k-best (flow decomposition) */
    int num_found;                  /* Number of paths found (1 for standard, up to k for k-best) */
    double *objectives;             /* Path costs: [num_found] (NULL to skip) */
    RalphNetflowPath *paths;        /* Path details: [num_found] (NULL to skip, caller allocates) */

    /* Dual variables for warm start (optional) */
    double *potential;              /* Node potentials: [num_nodes] */
    int *arc_state;                 /* Arc states: [num_arcs] (LOWER/BASIC/UPPER) */

    /* Statistics */
    int64_t iterations;             /* Number of pivots performed */
    int64_t degenerate_pivots;      /* Number of degenerate pivots (zero flow change) */
} RalphNetflowResult;

/* ============================================================================
 * Workspace Management
 * ============================================================================ */

/*
 * Create a reusable workspace for network simplex solving.
 *
 * The workspace pre-allocates all working arrays for networks up to
 * max_nodes nodes and max_arcs arcs. This amortizes allocation overhead
 * when solving multiple network flow instances.
 *
 * Parameters:
 *   max_nodes - Maximum number of nodes this workspace can handle
 *   max_arcs  - Maximum number of arcs this workspace can handle
 *
 * Returns:
 *   Pointer to workspace, or NULL on allocation failure.
 */
RalphNetflowWorkspace* ralph_netflow_workspace_create(int max_nodes, int max_arcs);

/*
 * Free a workspace and all its memory.
 */
void ralph_netflow_workspace_free(RalphNetflowWorkspace *ws);

/*
 * Get workspace memory requirements in bytes.
 *
 * Parameters:
 *   max_nodes - Maximum number of nodes
 *   max_arcs  - Maximum number of arcs
 *
 * Returns:
 *   Total bytes required for workspace, or 0 if invalid.
 */
size_t ralph_netflow_workspace_size(int max_nodes, int max_arcs);

/*
 * Get the maximum number of nodes this workspace supports.
 */
int ralph_netflow_workspace_max_nodes(const RalphNetflowWorkspace *ws);

/*
 * Get the maximum number of arcs this workspace supports.
 */
int ralph_netflow_workspace_max_arcs(const RalphNetflowWorkspace *ws);

/* ============================================================================
 * Warm Start API
 * ============================================================================ */

/*
 * Initialize warm start data in workspace from a previous solution.
 *
 * Stores the basis (tree structure) and dual variables so the next solve
 * can skip Phase 1 and continue optimization from this point.
 *
 * Use cases:
 * - Re-optimization after small cost changes
 * - Sensitivity analysis (varying parameters)
 * - Branch-and-bound (modifying bounds between nodes)
 *
 * Parameters:
 *   ws         - Workspace to store warm start data
 *   num_nodes  - Number of nodes in the solution
 *   num_arcs   - Number of arcs in the solution
 *   potential  - Node potentials (size num_nodes, required)
 *   flow       - Arc flows (size num_arcs, optional but recommended)
 *   arc_state  - Arc states (size num_arcs, optional but recommended)
 *
 * Returns:
 *   RALPH_NETFLOW_OPTIMAL on success, error code otherwise.
 *
 * Note: Warm start is invalidated if problem structure changes (different
 * nodes/arcs). Cost and bound changes are handled automatically.
 */
RalphNetflowStatus ralph_netflow_warm_start(
    RalphNetflowWorkspace *ws,
    int num_nodes,
    int num_arcs,
    const double *potential,
    const double *flow,
    const int *arc_state
);

/*
 * Clear warm start data from workspace.
 *
 * After calling this, the next solve will be a cold start (full solve
 * with artificial arcs in Phase 1).
 */
void ralph_netflow_warm_start_clear(RalphNetflowWorkspace *ws);

/*
 * Check if workspace has valid warm start data.
 *
 * Returns:
 *   1 if workspace has valid warm start for the given dimensions, 0 otherwise.
 */
int ralph_netflow_warm_start_valid(const RalphNetflowWorkspace *ws, int num_nodes, int num_arcs);

/* ============================================================================
 * Main Solver API
 * ============================================================================ */

/*
 * Solve a minimum cost network flow problem using network simplex.
 *
 * Parameters:
 *   problem   - Problem definition (nodes, arcs, costs, supplies)
 *   options   - Solver options (NULL for defaults)
 *   result    - Output structure (caller allocates flow array)
 *   workspace - Reusable workspace (NULL to auto-allocate internally)
 *
 * Returns:
 *   RALPH_NETFLOW_OPTIMAL on success, error code otherwise.
 *
 * The algorithm:
 * 1. Phase 1: Find initial basic feasible solution using artificial arcs
 * 2. Phase 2: Network simplex iterations until optimal or limit reached
 *
 * Optimality conditions (for minimization):
 *   - Flow conservation: inflow - outflow = supply at each node
 *   - Capacity: lower <= flow <= upper for each arc
 *   - Reduced cost: rc[a] = cost[a] - potential[tail[a]] + potential[head[a]]
 *     - Basic arcs: can have any reduced cost
 *     - At lower bound: rc >= 0
 *     - At upper bound: rc <= 0
 *
 * Example:
 *   // Simple 3-node network: source -> intermediate -> sink
 *   int tail[] = {0, 1};
 *   int head[] = {1, 2};
 *   double cost[] = {1.0, 2.0};
 *   double cap[] = {10.0, 10.0};
 *   double supply[] = {5.0, 0.0, -5.0};  // Source supplies 5, sink demands 5
 *
 *   RalphNetflowProblem prob = {
 *       .num_nodes = 3, .num_arcs = 2,
 *       .tail = tail, .head = head,
 *       .cost = cost, .capacity = cap, .supply = supply,
 *       .objective = RALPH_NETFLOW_MINIMIZE
 *   };
 *
 *   double flow[2];
 *   RalphNetflowResult res = {.flow = flow};
 *   ralph_netflow_solve(&prob, NULL, &res, NULL);
 *   // flow[0] = 5.0, flow[1] = 5.0, objective = 15.0
 */
RalphNetflowStatus ralph_netflow_solve(
    const RalphNetflowProblem *problem,
    const RalphNetflowOptions *options,
    RalphNetflowResult *result,
    RalphNetflowWorkspace *workspace
);

/*
 * Unified network flow solver - extended API.
 *
 * This is the main entry point supporting all algorithm variants:
 * - RALPH_NETFLOW_ALG_STANDARD: Single optimal flow (default)
 * - RALPH_NETFLOW_ALG_K_BEST: Find k best distinct flows
 * - RALPH_NETFLOW_ALG_BOTTLENECK: Minimize maximum arc cost used
 *
 * Features controlled via options:
 * - Algorithm selection via options->algorithm
 * - Warm start via options->warm_start
 * - Pricing strategy via options->pricing
 *
 * The basic ralph_netflow_solve() is equivalent to:
 *   ralph_netflow_solve_ex(problem, options, result, workspace)
 * with algorithm = STANDARD.
 *
 * Parameters:
 *   problem   - Problem definition
 *   options   - Solver options (NULL for defaults)
 *   result    - Output structure
 *   workspace - Reusable workspace (NULL to auto-allocate)
 *
 * Returns:
 *   RALPH_NETFLOW_OPTIMAL on success, error code otherwise.
 */
RalphNetflowStatus ralph_netflow_solve_ex(
    const RalphNetflowProblem *problem,
    const RalphNetflowOptions *options,
    RalphNetflowResult *result,
    RalphNetflowWorkspace *workspace
);

/* ============================================================================
 * Convenience Wrapper
 * ============================================================================ */

/*
 * Simplified interface for common MCNF problems.
 *
 * This is a convenience wrapper around ralph_netflow_solve() for users who
 * don't need advanced options or workspace management.
 *
 * Parameters:
 *   num_nodes - Number of nodes in the network
 *   num_arcs  - Number of arcs in the network
 *   tail      - Source node of each arc (size num_arcs)
 *   head      - Destination node of each arc (size num_arcs)
 *   cost      - Cost per unit flow (size num_arcs)
 *   capacity  - Upper bound on flow (size num_arcs, NULL = infinite)
 *   supply    - Node supply/demand (size num_nodes, negative = demand)
 *   flow      - Output: optimal flow on each arc (size num_arcs)
 *   objective - Output: optimal objective value (can be NULL)
 *
 * Returns:
 *   RALPH_NETFLOW_OPTIMAL on success, error code otherwise.
 *
 * Example - Transportation problem (2 sources, 3 sinks):
 *   // Sources: 0 (supply 10), 1 (supply 15)
 *   // Sinks: 2 (demand 8), 3 (demand 7), 4 (demand 10)
 *   int tail[] = {0, 0, 0, 1, 1, 1};
 *   int head[] = {2, 3, 4, 2, 3, 4};
 *   double cost[] = {2, 4, 5, 3, 1, 8};
 *   double supply[] = {10, 15, -8, -7, -10};
 *
 *   double flow[6];
 *   double obj;
 *   ralph_mcnf_solve(5, 6, tail, head, cost, NULL, supply, flow, &obj);
 */
RalphNetflowStatus ralph_mcnf_solve(
    int num_nodes,
    int num_arcs,
    const int *tail,
    const int *head,
    const double *cost,
    const double *capacity,
    const double *supply,
    double *flow,
    double *objective
);

/* ============================================================================
 * Flow Decomposition
 * ============================================================================ */

/*
 * Decompose a flow into source-to-sink paths.
 *
 * Any feasible network flow can be decomposed into at most m paths from
 * sources (supply > 0) to sinks (supply < 0). This is useful for logistics
 * applications where you want to know the actual routes, not just arc flows.
 *
 * Parameters:
 *   problem    - Original problem definition (for topology and costs)
 *   flow       - Flow values on each arc (from ralph_netflow_solve)
 *   max_paths  - Maximum number of paths to return (0 = all paths)
 *   paths      - Output: array of paths (caller allocates array of max_paths)
 *   num_paths  - Output: actual number of paths found
 *
 * Returns:
 *   RALPH_NETFLOW_OPTIMAL on success, error code otherwise.
 *
 * Memory:
 *   Caller allocates the paths array. This function allocates paths[i].arcs
 *   for each path found. Caller must free these with ralph_netflow_path_free().
 *
 * Sorting:
 *   Paths are returned sorted by unit_cost (ascending for minimize problems).
 *   The first path is the cheapest route per unit of goods shipped.
 *
 * Example:
 *   RalphNetflowPath paths[10];
 *   int num_paths;
 *   ralph_netflow_decompose(&prob, flow, 10, paths, &num_paths);
 *   for (int i = 0; i < num_paths; i++) {
 *       printf("Path %d: %d arcs, flow=%.1f, cost=%.1f\n",
 *              i, paths[i].num_arcs, paths[i].flow, paths[i].cost);
 *       ralph_netflow_path_free(&paths[i]);
 *   }
 */
RalphNetflowStatus ralph_netflow_decompose(
    const RalphNetflowProblem *problem,
    const double *flow,
    int max_paths,
    RalphNetflowPath *paths,
    int *num_paths
);

/*
 * Free memory allocated for a path's arc array.
 */
void ralph_netflow_path_free(RalphNetflowPath *path);

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/*
 * Get human-readable string for status code.
 */
const char* ralph_netflow_status_string(RalphNetflowStatus status);

/*
 * Verify a flow solution for feasibility and compute objective.
 *
 * Checks:
 *   - Flow conservation at each node
 *   - Capacity bounds on each arc
 *
 * Parameters:
 *   problem      - Problem definition
 *   flow         - Flow values to verify (size num_arcs)
 *   objective    - Output: computed objective (can be NULL)
 *   max_violation - Output: maximum constraint violation (can be NULL)
 *
 * Returns:
 *   1 if feasible (violations within tolerance), 0 otherwise.
 */
int ralph_netflow_verify(
    const RalphNetflowProblem *problem,
    const double *flow,
    double *objective,
    double *max_violation
);

/*
 * Check reduced cost optimality conditions.
 *
 * For a solution to be optimal:
 *   - If flow < capacity: reduced cost >= 0 (for minimization)
 *   - If flow > lower: reduced cost <= 0 (for minimization)
 *   - Basic arcs (lower < flow < capacity): reduced cost = 0
 *
 * Parameters:
 *   problem       - Problem definition
 *   flow          - Flow values (size num_arcs)
 *   potential     - Node potentials (size num_nodes)
 *   max_violation - Output: maximum reduced cost violation (can be NULL)
 *
 * Returns:
 *   1 if optimality conditions satisfied, 0 otherwise.
 */
int ralph_netflow_check_optimality(
    const RalphNetflowProblem *problem,
    const double *flow,
    const double *potential,
    double *max_violation
);

#ifdef __cplusplus
}
#endif

#endif /* NETFLOW_H */
