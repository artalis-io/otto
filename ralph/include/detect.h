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
 * Network Flow Detection
 * ============================================================================ */

/*
 * Network flow signature - extracted from LP model.
 *
 * A problem has network flow structure if:
 * - Each variable appears in exactly 2 constraints
 * - Coefficients are +1 in one constraint (tail/outflow) and -1 in other (head/inflow)
 * - This forms a node-arc incidence matrix
 *
 * The network is:
 *   min  sum_a cost[a] * flow[a]
 *   s.t. sum_{a: tail[a]=i} flow[a] - sum_{a: head[a]=i} flow[a] = supply[i]  for all i
 *        lower[a] <= flow[a] <= upper[a]                                       for all a
 */
typedef struct {
    int is_network;         /* 1 if network structure detected */
    int num_nodes;          /* Number of nodes */
    int num_arcs;           /* Number of arcs (= num_vars in LP) */

    /* Arc data extracted from LP */
    int *tail;              /* tail[a] = source node of arc a */
    int *head;              /* head[a] = destination node of arc a */
    double *cost;           /* cost[a] = objective coefficient */
    double *capacity;       /* capacity[a] = upper bound on flow */
    double *lower;          /* lower[a] = lower bound on flow */
    double *supply;         /* supply[i] = RHS of node i's constraint (demand if negative) */

    /* Mapping from LP to network */
    int *var_to_arc;        /* var_to_arc[v] = arc index for variable v */
    int *con_to_node;       /* con_to_node[c] = node index for constraint c */

    int obj_sense;          /* 1=minimize, -1=maximize */
} NetworkSignature;

/*
 * Network flow type - detected special cases.
 */
typedef enum {
    RALPH_NETWORK_GENERAL = 0,      /* General minimum cost flow */
    RALPH_NETWORK_TRANSPORTATION,   /* Bipartite: sources -> sinks only */
    RALPH_NETWORK_ASSIGNMENT,       /* Transportation with unit supply/demand */
    RALPH_NETWORK_SHORTEST_PATH,    /* Single source/sink, unit flow */
    RALPH_NETWORK_MAX_FLOW          /* Max flow structure */
} RalphNetworkType;

/*
 * Detect network flow structure in an LP model.
 *
 * Parameters:
 *   model - LP model to analyze
 *   sig   - Output: network signature (caller allocates struct)
 *
 * Returns:
 *   1 if network structure detected, 0 otherwise.
 *   If 1, sig arrays are allocated and must be freed with detect_network_free().
 */
int detect_network(const LPModel *model, NetworkSignature *sig);

/*
 * Free memory allocated by detect_network().
 */
void detect_network_free(NetworkSignature *sig);

/*
 * Detect the specific type of network flow problem.
 *
 * Parameters:
 *   sig - Network signature from detect_network()
 *
 * Returns:
 *   Network type (GENERAL, TRANSPORTATION, ASSIGNMENT, etc.)
 */
RalphNetworkType detect_network_type(const NetworkSignature *sig);

/*
 * Solve network flow using detected structure.
 *
 * Parameters:
 *   sig      - Network signature from detect_network()
 *   solution - Output: variable values (size = num_arcs)
 *   obj_val  - Output: objective value
 *
 * Returns:
 *   0 on success, -1 on infeasible, -2 on error.
 */
int solve_as_network(const NetworkSignature *sig, double *solution, double *obj_val);

/* ============================================================================
 * MIP Network Detection
 * ============================================================================ */

/*
 * MIP Network signature - extended for use during branch-and-bound.
 */
typedef struct {
    NetworkSignature base;    /* Base network signature */
    int num_vars;             /* Total number of variables in MIP */
    double *base_cost;        /* Original costs (before modifications) */
    double *base_capacity;    /* Original capacities */
    double *base_lower;       /* Original lower bounds */
    double *base_supply;      /* Original supplies */
    void *netflow_workspace;  /* Reusable network simplex workspace */
} MIPNetworkSignature;

/*
 * Detect network structure in a MIP model.
 */
int detect_network_mip(const LPModel *model, MIPNetworkSignature *sig);

/*
 * Free memory allocated by detect_network_mip().
 */
void detect_network_mip_free(MIPNetworkSignature *sig);

/*
 * Solve network relaxation at a B&B node.
 *
 * Handles variable fixings:
 *   - x[a] fixed to 0: capacity[a] = 0
 *   - x[a] fixed to value: lower[a] = upper[a] = value
 */
int solve_network_at_node(
    MIPNetworkSignature *sig,
    const double *lb,
    const double *ub,
    double *solution,
    double *obj_val
);

/* ============================================================================
 * Set Covering/Partitioning Detection
 * ============================================================================ */

/*
 * Set cover problem type.
 *
 * A problem has set cover structure if:
 * - All variables are binary (0 <= x <= 1, integer)
 * - All coefficients are 0 or 1
 * - RHS values are positive
 *
 * Types based on constraint senses:
 * - COVERING: Ax >= b (each element covered by at least one set)
 * - PARTITIONING: Ax = b (each element covered by exactly one set)
 * - PACKING: Ax <= b (each element covered by at most one set)
 * - MIXED: combination of above
 */
typedef enum {
    RALPH_SETCOVER_NONE = 0,        /* Not a set cover problem */
    RALPH_SETCOVER_COVERING,        /* All constraints are >= (set covering) */
    RALPH_SETCOVER_PARTITIONING,    /* All constraints are = (set partitioning) */
    RALPH_SETCOVER_PACKING,         /* All constraints are <= (set packing) */
    RALPH_SETCOVER_MIXED            /* Mix of constraint types */
} RalphSetCoverType;

/*
 * Set cover signature - structural information extracted from LP/MIP model.
 */
typedef struct {
    RalphSetCoverType type;         /* Problem classification */
    int num_elements;               /* Number of constraints (elements to cover) */
    int num_sets;                   /* Number of variables (sets) */

    /* Constraint type counts */
    int num_covering;               /* Count of >= constraints */
    int num_partitioning;           /* Count of = constraints */
    int num_packing;                /* Count of <= constraints */

    /* Structural statistics */
    int *set_size;                  /* Number of elements in each set (size num_sets) */
    int *element_coverage;          /* Number of sets covering each element (size num_elements) */
    double *rhs;                    /* RHS values (size num_elements) */
    double density;                 /* nnz / (num_elements * num_sets) */

    /* Cost statistics */
    double min_cost;                /* Minimum set cost */
    double max_cost;                /* Maximum set cost */
    double avg_cost;                /* Average set cost */

    /* For internal use */
    int obj_sense;                  /* 1=minimize, -1=maximize */
} SetCoverSignature;

/*
 * Detect set covering/partitioning structure in an LP/MIP model.
 *
 * Parameters:
 *   model - LP/MIP model to analyze
 *   sig   - Output: set cover signature (caller allocates struct)
 *
 * Returns:
 *   1 if set cover structure detected, 0 otherwise.
 *   If 1, sig arrays are allocated and must be freed with detect_set_cover_free().
 */
int detect_set_cover(const LPModel *model, SetCoverSignature *sig);

/*
 * Free memory allocated by detect_set_cover().
 */
void detect_set_cover_free(SetCoverSignature *sig);

/*
 * Get human-readable name for set cover type.
 */
const char *ralph_set_cover_type_name(RalphSetCoverType type);

/* ============================================================================
 * Runtime Configuration
 * ============================================================================ */

/*
 * Enable or disable automatic network detection.
 *
 * When enabled, LP/MIP optimize entry points will check if the problem has network
 * structure and use the specialized network simplex solver if so.
 */
void ralph_set_detect_network(int enabled);
int ralph_get_detect_network(void);

/*
 * Enable or disable automatic LAP detection.
 *
 * When enabled, LP/MIP optimize entry points will check if the problem has LAP
 * structure and use the specialized JVC solver if so.
 *
 * Default: disabled (0) - for fair benchmarking against LP baseline
 * Enable with: ralph_set_detect_lap(1) or ralph_lp_set_int_param(model, "detect_special", 1)
 */
void ralph_set_detect_lap(int enabled);
int ralph_get_detect_lap(void);

#ifdef __cplusplus
}
#endif

#endif /* RALPH_DETECT_H */
