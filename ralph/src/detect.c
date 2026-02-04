/*
 * detect.c - Problem Structure Detection
 *
 * Detects special structure in LP/MIP problems to enable
 * delegation to specialized solvers.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#include "detect.h"
#include "lap.h"

#define TOLERANCE 1e-9

/* Integer overflow check for n*n*sizeof(type) allocations */
static int detect_check_size_overflow(size_t n, size_t elem_size) {
    if (n > 0 && n > SIZE_MAX / n) return -1;
    size_t n_squared = n * n;
    if (elem_size > 0 && n_squared > SIZE_MAX / elem_size) return -1;
    return 0;
}

/* Global setting for LAP detection (thread-safe via atomic) */
static atomic_int lap_detection_enabled = 1;

void ralph_set_detect_lap(int enabled) {
    atomic_store(&lap_detection_enabled, enabled ? 1 : 0);
}

int ralph_get_detect_lap(void) {
    return atomic_load(&lap_detection_enabled);
}

/* ============================================================================
 * LAP Detection
 * ============================================================================ */

/*
 * Detect LAP structure in an LP model.
 *
 * LAP signature:
 * - 2n constraints (n row + n column)
 * - n² variables
 * - Each variable appears in exactly 2 constraints with coef +1
 * - All constraints are equality with RHS = 1
 * - Variables are non-negative
 *
 * The constraints can be partitioned into:
 * - Row constraints: sum_j x[i,j] = 1 for each row i
 * - Col constraints: sum_i x[i,j] = 1 for each col j
 */
int detect_lap(const LPModel *model, LAPSignature *sig) {
    if (!model || !sig || !lap_detection_enabled) {
        return 0;
    }

    /* Initialize signature */
    memset(sig, 0, sizeof(LAPSignature));

    int m = model->num_cons;  /* Number of constraints */
    int num_vars = model->num_vars;

    /* Check: num_cons must be even */
    if (m % 2 != 0 || m < 2) {
        return 0;
    }

    int n = m / 2;  /* LAP size */

    /* Check: num_vars = n² */
    if (num_vars != n * n) {
        return 0;
    }

    /* Check: all constraints are equality with RHS = 1 */
    for (int i = 0; i < m; i++) {
        if (model->sense[i] != 'E') {
            return 0;
        }
        if (fabs(model->b[i] - 1.0) > TOLERANCE) {
            return 0;
        }
    }

    /* Check: all variables are non-negative with upper bound >= 1 (or infinity) */
    for (int j = 0; j < num_vars; j++) {
        if (model->lb[j] < -TOLERANCE) {
            return 0;  /* Negative lower bound */
        }
        if (model->ub[j] < 1.0 - TOLERANCE && model->ub[j] < RALPH_INFINITY * 0.5) {
            return 0;  /* Upper bound too restrictive */
        }
    }

    /* Count appearances of each variable in constraints */
    /* Use calloc for zero-initialization; var_con1/var_con2 initialized to -1 below */
    int *var_count = (int *)calloc(num_vars, sizeof(int));
    int *var_con1 = (int *)calloc(num_vars, sizeof(int));  /* First constraint */
    int *var_con2 = (int *)calloc(num_vars, sizeof(int));  /* Second constraint */

    if (!var_count || !var_con1 || !var_con2) {
        free(var_count); free(var_con1); free(var_con2);
        return 0;
    }

    /* Initialize to -1 (unassigned) */
    for (int j = 0; j < num_vars; j++) {
        var_con1[j] = -1;
        var_con2[j] = -1;
    }

    /* Scan constraint matrix (CSC format) */
    const SparseMatrix *A = model->A;
    if (!A) {
        free(var_count); free(var_con1); free(var_con2);
        return 0;
    }

    for (int col = 0; col < A->ncols; col++) {
        for (int p = A->colptr[col]; p < A->colptr[col + 1]; p++) {
            int row = A->rowidx[p];
            double val = A->values[p];

            /* Check coefficient is +1 */
            if (fabs(val - 1.0) > TOLERANCE) {
                free(var_count); free(var_con1); free(var_con2);
                return 0;
            }

            if (var_count[col] == 0) {
                var_con1[col] = row;
            } else if (var_count[col] == 1) {
                var_con2[col] = row;
            }
            var_count[col]++;
        }
    }

    /* Check: each variable appears in exactly 2 constraints */
    for (int j = 0; j < num_vars; j++) {
        if (var_count[j] != 2) {
            free(var_count); free(var_con1); free(var_con2);
            return 0;
        }
    }

    free(var_count);

    /*
     * Now we need to identify the row and column constraints.
     * The bipartite structure means we can partition constraints into two groups:
     * - Each variable connects one "row" constraint to one "col" constraint
     * - Row constraints: first n constraints (or need to detect)
     * - Col constraints: last n constraints
     *
     * Strategy: Build a bipartite graph of constraints and check it's bipartite.
     * Use BFS/DFS to 2-color the constraint graph.
     */
    int *con_color = (int *)calloc(m, sizeof(int));  /* -1=unvisited, 0=row, 1=col */
    if (!con_color) {
        free(var_con1); free(var_con2);
        return 0;
    }

    for (int i = 0; i < m; i++) {
        con_color[i] = -1;
    }

    /* BFS to 2-color */
    int *queue = (int *)calloc(m, sizeof(int));
    if (!queue) {
        free(var_con1); free(var_con2); free(con_color);
        return 0;
    }

    int head = 0, tail = 0;
    int num_row_cons = 0, num_col_cons = 0;

    /* Process all components */
    for (int start = 0; start < m; start++) {
        if (con_color[start] != -1) continue;

        /* BFS from this constraint */
        con_color[start] = 0;  /* Assign as "row" constraint */
        num_row_cons++;
        queue[tail++] = start;

        while (head < tail) {
            int con = queue[head++];
            int cur_color = con_color[con];

            /* Find all constraints connected to this one through variables */
            for (int v = 0; v < num_vars; v++) {
                if (var_con1[v] == con || var_con2[v] == con) {
                    int other = (var_con1[v] == con) ? var_con2[v] : var_con1[v];

                    if (con_color[other] == -1) {
                        con_color[other] = 1 - cur_color;
                        if (con_color[other] == 0) num_row_cons++;
                        else num_col_cons++;
                        queue[tail++] = other;
                    } else if (con_color[other] == cur_color) {
                        /* Not bipartite! */
                        free(var_con1); free(var_con2); free(con_color); free(queue);
                        return 0;
                    }
                }
            }
        }
    }

    free(queue);

    /* Check we have exactly n row constraints and n col constraints */
    if (num_row_cons != n || num_col_cons != n) {
        free(var_con1); free(var_con2); free(con_color);
        return 0;
    }

    /*
     * Build mapping from constraints to LAP indices.
     * row_map[con] = LAP row index (0..n-1) if con_color[con]==0
     * col_map[con] = LAP col index (0..n-1) if con_color[con]==1
     */
    int *row_map = (int *)calloc(m, sizeof(int));
    int *col_map = (int *)calloc(m, sizeof(int));
    if (!row_map || !col_map) {
        free(var_con1); free(var_con2); free(con_color);
        free(row_map); free(col_map);
        return 0;
    }

    int row_idx = 0, col_idx = 0;
    for (int i = 0; i < m; i++) {
        if (con_color[i] == 0) {
            row_map[i] = row_idx++;
        } else {
            col_map[i] = col_idx++;
        }
    }

    /*
     * Extract cost matrix from objective.
     * For variable v connecting row constraint r and col constraint c:
     * costs[row_map[r]][col_map[c]] = objective coefficient of v
     */

    /* Check for integer overflow in n*n allocation */
    if (detect_check_size_overflow((size_t)n, sizeof(double)) != 0) {
        free(var_con1); free(var_con2); free(con_color);
        free(row_map); free(col_map);
        return 0;
    }

    double *costs = (double *)calloc((size_t)n * n, sizeof(double));
    int *var_to_row = (int *)calloc(num_vars, sizeof(int));
    int *var_to_col = (int *)calloc(num_vars, sizeof(int));

    if (!costs || !var_to_row || !var_to_col) {
        free(var_con1); free(var_con2); free(con_color);
        free(row_map); free(col_map);
        free(costs); free(var_to_row); free(var_to_col);
        return 0;
    }

    for (int v = 0; v < num_vars; v++) {
        int con1 = var_con1[v];
        int con2 = var_con2[v];

        int lap_row, lap_col;
        if (con_color[con1] == 0) {
            lap_row = row_map[con1];
            lap_col = col_map[con2];
        } else {
            lap_row = row_map[con2];
            lap_col = col_map[con1];
        }

        var_to_row[v] = lap_row;
        var_to_col[v] = lap_col;
        costs[lap_row * n + lap_col] = model->c[v];
    }

    /* Clean up intermediate arrays */
    free(var_con1);
    free(var_con2);
    free(con_color);
    free(row_map);
    free(col_map);

    /* Fill signature */
    sig->is_lap = 1;
    sig->n = n;
    sig->costs = costs;
    sig->var_to_row = var_to_row;
    sig->var_to_col = var_to_col;
    sig->obj_sense = model->obj_sense;

    return 1;
}

void detect_lap_free(LAPSignature *sig) {
    if (!sig) return;
    free(sig->costs);
    free(sig->var_to_row);
    free(sig->var_to_col);
    sig->costs = NULL;
    sig->var_to_row = NULL;
    sig->var_to_col = NULL;
    sig->is_lap = 0;
}

/*
 * Solve LAP using detected structure.
 */
int solve_as_lap(const LAPSignature *sig, double *solution, double *obj_val) {
    if (!sig || !sig->is_lap || !solution) {
        return -1;
    }

    int n = sig->n;
    int num_vars = n * n;

    /* Solve using JVC */
    int *row_sol = (int *)calloc(n, sizeof(int));
    if (!row_sol) {
        return -1;
    }

    double total_cost;
    RalphLapObjective objective = (sig->obj_sense == 1) ?
        RALPH_LAP_MINIMIZE : RALPH_LAP_MAXIMIZE;

    RalphLapStatus status = ralph_lap_solve(
        n, sig->costs, objective, row_sol, NULL, NULL, NULL, &total_cost);

    if (status != RALPH_LAP_SUCCESS) {
        free(row_sol);
        return -1;
    }

    /* Convert LAP solution to LP solution */
    memset(solution, 0, num_vars * sizeof(double));

    for (int v = 0; v < num_vars; v++) {
        int lap_row = sig->var_to_row[v];
        int lap_col = sig->var_to_col[v];

        if (row_sol[lap_row] == lap_col) {
            solution[v] = 1.0;
        }
    }

    if (obj_val) {
        *obj_val = total_cost;
    }

    free(row_sol);
    return 0;
}

/* ============================================================================
 * MIP LAP Detection and Solving
 * ============================================================================ */

/*
 * Detect LAP structure in a MIP model.
 *
 * This reuses the LP detection but also:
 * - Allocates a reusable LAP workspace
 * - Stores a copy of the base costs for modification during B&B
 */
int detect_lap_mip(const LPModel *model, MIPLAPSignature *sig) {
    if (!model || !sig) {
        return 0;
    }

    /* Initialize */
    memset(sig, 0, sizeof(MIPLAPSignature));

    /* Use standard LAP detection */
    if (!detect_lap(model, &sig->base)) {
        return 0;
    }

    int n = sig->base.n;
    sig->num_vars = model->num_vars;

    /* Check for overflow in n*n allocation */
    if (detect_check_size_overflow(n, sizeof(double)) != 0) {
        detect_lap_free(&sig->base);
        return 0;
    }

    /* Allocate base costs copy */
    sig->base_costs = (double *)calloc((size_t)n * n, sizeof(double));
    if (!sig->base_costs) {
        detect_lap_free(&sig->base);
        return 0;
    }
    memcpy(sig->base_costs, sig->base.costs, n * n * sizeof(double));

    /* Create reusable LAP workspace */
    sig->lap_workspace = ralph_lap_workspace_create(n);
    if (!sig->lap_workspace) {
        free(sig->base_costs);
        detect_lap_free(&sig->base);
        return 0;
    }

    return 1;
}

void detect_lap_mip_free(MIPLAPSignature *sig) {
    if (!sig) return;

    detect_lap_free(&sig->base);
    free(sig->base_costs);
    sig->base_costs = NULL;

    if (sig->lap_workspace) {
        ralph_lap_workspace_free((RalphLapWorkspace *)sig->lap_workspace);
        sig->lap_workspace = NULL;
    }

    sig->num_vars = 0;
}

/*
 * Solve LAP relaxation at a B&B node.
 *
 * Handles variable fixings from branching:
 * - Fixed to 0 (lb=ub=0): set cost to infinity (forbidden)
 * - Fixed to 1 (lb=ub=1): set all other costs in row/col to infinity
 */
int solve_lap_at_node(
    MIPLAPSignature *sig,
    const double *lb,
    const double *ub,
    double *solution,
    double *obj_val
) {
    if (!sig || !sig->base.is_lap || !lb || !ub || !solution) {
        return -1;
    }

    int n = sig->base.n;
    int num_vars = sig->num_vars;
    double *work_costs = sig->base.costs;  /* Reuse the costs array */
    RalphLapWorkspace *ws = (RalphLapWorkspace *)sig->lap_workspace;

    /* Reset costs to base values */
    memcpy(work_costs, sig->base_costs, n * n * sizeof(double));

    /* Track which rows and columns have forced assignments */
    int *row_forced = (int *)calloc(n, sizeof(int));  /* row_forced[i] = col if forced, -1 otherwise */
    int *col_forced = (int *)calloc(n, sizeof(int));  /* col_forced[j] = row if forced, -1 otherwise */

    if (!row_forced || !col_forced) {
        free(row_forced);
        free(col_forced);
        return -1;
    }

    for (int i = 0; i < n; i++) {
        row_forced[i] = -1;
        col_forced[i] = -1;
    }

    /* Process variable fixings */
    for (int v = 0; v < num_vars; v++) {
        int lap_row = sig->base.var_to_row[v];
        int lap_col = sig->base.var_to_col[v];

        /* Check if variable is fixed */
        if (ub[v] < 0.5) {
            /* Fixed to 0: forbid this assignment */
            work_costs[lap_row * n + lap_col] = RALPH_LAP_INFINITY;
        } else if (lb[v] > 0.5) {
            /* Fixed to 1: this assignment is forced */
            row_forced[lap_row] = lap_col;
            col_forced[lap_col] = lap_row;
        }
    }

    /* For forced assignments, forbid all other options in that row/column */
    for (int i = 0; i < n; i++) {
        if (row_forced[i] >= 0) {
            int forced_col = row_forced[i];
            /* Forbid all other columns in this row */
            for (int j = 0; j < n; j++) {
                if (j != forced_col) {
                    work_costs[i * n + j] = RALPH_LAP_INFINITY;
                }
            }
        }
    }
    for (int j = 0; j < n; j++) {
        if (col_forced[j] >= 0) {
            int forced_row = col_forced[j];
            /* Forbid all other rows in this column */
            for (int i = 0; i < n; i++) {
                if (i != forced_row) {
                    work_costs[i * n + j] = RALPH_LAP_INFINITY;
                }
            }
        }
    }

    free(row_forced);
    free(col_forced);

    /* Solve LAP with modified costs */
    int *row_sol = (int *)calloc(n, sizeof(int));
    if (!row_sol) {
        return -1;
    }

    double total_cost;
    RalphLapObjective objective = (sig->base.obj_sense == 1) ?
        RALPH_LAP_MINIMIZE : RALPH_LAP_MAXIMIZE;

    RalphLapStatus status = ralph_lap_solve_with_workspace(
        n, work_costs, objective, row_sol, NULL, NULL, NULL, &total_cost, ws);

    if (status != RALPH_LAP_SUCCESS) {
        free(row_sol);
        return -1;  /* Infeasible - probably due to conflicting fixings */
    }

    /* Convert LAP solution to LP solution */
    memset(solution, 0, num_vars * sizeof(double));

    for (int v = 0; v < num_vars; v++) {
        int lap_row = sig->base.var_to_row[v];
        int lap_col = sig->base.var_to_col[v];

        if (row_sol[lap_row] == lap_col) {
            solution[v] = 1.0;
        }
    }

    if (obj_val) {
        *obj_val = total_cost;
    }

    free(row_sol);
    return 0;
}

/* ============================================================================
 * Network Flow Detection
 * ============================================================================ */

#include "netflow.h"

/* Global setting for network detection (thread-safe via atomic) */
static atomic_int network_detection_enabled = 1;

void ralph_set_detect_network(int enabled) {
    atomic_store(&network_detection_enabled, enabled ? 1 : 0);
}

int ralph_get_detect_network(void) {
    return atomic_load(&network_detection_enabled);
}

/* Forward declarations for static helper functions */
static int solve_assignment_as_lap(const NetworkSignature *sig, double *solution, double *obj_val);
static int solve_network_simplex(const NetworkSignature *sig, double *solution, double *obj_val);

/*
 * Detect network flow structure in an LP model.
 *
 * Network signature:
 * - Each variable appears in exactly 2 constraints
 * - Coefficients are +1 (outflow) and -1 (inflow)
 * - Forms a node-arc incidence matrix
 */
int detect_network(const LPModel *model, NetworkSignature *sig) {
    if (!model || !sig) {
        return 0;
    }

    /* Initialize signature */
    memset(sig, 0, sizeof(NetworkSignature));

    /* Allow detection even if global flag is off (for explicit calls) */
    int num_vars = model->num_vars;
    int num_cons = model->num_cons;

    if (num_vars == 0 || num_cons == 0) {
        return 0;
    }

    const SparseMatrix *A = model->A;
    if (!A) {
        return 0;
    }

    /*
     * First pass: Check each variable appears in exactly 2 constraints
     * and has coefficients +1 and -1.
     */
    int *var_count = (int *)calloc(num_vars, sizeof(int));
    int *var_con1 = (int *)calloc(num_vars, sizeof(int));
    int *var_con2 = (int *)calloc(num_vars, sizeof(int));
    double *var_coef1 = (double *)calloc(num_vars, sizeof(double));
    double *var_coef2 = (double *)calloc(num_vars, sizeof(double));

    if (!var_count || !var_con1 || !var_con2 || !var_coef1 || !var_coef2) {
        free(var_count); free(var_con1); free(var_con2);
        free(var_coef1); free(var_coef2);
        return 0;
    }

    for (int v = 0; v < num_vars; v++) {
        var_con1[v] = -1;
        var_con2[v] = -1;
        var_coef1[v] = 0.0;
        var_coef2[v] = 0.0;
    }

    /* Scan constraint matrix (CSC format) */
    for (int col = 0; col < A->ncols && col < num_vars; col++) {
        for (int p = A->colptr[col]; p < A->colptr[col + 1]; p++) {
            int row = A->rowidx[p];
            double val = A->values[p];

            /* Check coefficient is +1 or -1 */
            if (fabs(fabs(val) - 1.0) > TOLERANCE) {
                free(var_count); free(var_con1); free(var_con2);
                free(var_coef1); free(var_coef2);
                return 0;
            }

            if (var_count[col] == 0) {
                var_con1[col] = row;
                var_coef1[col] = val;
            } else if (var_count[col] == 1) {
                var_con2[col] = row;
                var_coef2[col] = val;
            }
            var_count[col]++;
        }
    }

    /* Check: each variable appears in exactly 2 constraints */
    for (int v = 0; v < num_vars; v++) {
        if (var_count[v] != 2) {
            free(var_count); free(var_con1); free(var_con2);
            free(var_coef1); free(var_coef2);
            return 0;
        }
    }

    /* Check: coefficients are +1 and -1 (not both +1 or both -1) */
    for (int v = 0; v < num_vars; v++) {
        double sum = var_coef1[v] + var_coef2[v];
        if (fabs(sum) > TOLERANCE) {
            /* Coefficients must be +1 and -1, so sum should be 0 */
            free(var_count); free(var_con1); free(var_con2);
            free(var_coef1); free(var_coef2);
            return 0;
        }
    }

    free(var_count);

    /*
     * Network structure detected!
     * Now extract the network: constraints are nodes, variables are arcs.
     *
     * For arc v:
     *   - Node with +1 coefficient is the tail (outflow)
     *   - Node with -1 coefficient is the head (inflow)
     */
    int num_nodes = num_cons;
    int num_arcs = num_vars;

    /* Allocate network data */
    sig->tail = (int *)calloc(num_arcs, sizeof(int));
    sig->head = (int *)calloc(num_arcs, sizeof(int));
    sig->cost = (double *)calloc(num_arcs, sizeof(double));
    sig->capacity = (double *)calloc(num_arcs, sizeof(double));
    sig->lower = (double *)calloc(num_arcs, sizeof(double));
    sig->supply = (double *)calloc(num_nodes, sizeof(double));
    sig->var_to_arc = (int *)calloc(num_vars, sizeof(int));
    sig->con_to_node = (int *)calloc(num_cons, sizeof(int));

    if (!sig->tail || !sig->head || !sig->cost || !sig->capacity ||
        !sig->lower || !sig->supply || !sig->var_to_arc || !sig->con_to_node) {
        detect_network_free(sig);
        free(var_con1); free(var_con2); free(var_coef1); free(var_coef2);
        return 0;
    }

    /* Build arc data */
    for (int v = 0; v < num_arcs; v++) {
        int con1 = var_con1[v];
        int con2 = var_con2[v];
        double coef1 = var_coef1[v];

        /* +1 coefficient is tail (outflow), -1 is head (inflow) */
        if (coef1 > 0) {
            sig->tail[v] = con1;
            sig->head[v] = con2;
        } else {
            sig->tail[v] = con2;
            sig->head[v] = con1;
        }

        sig->cost[v] = model->c ? model->c[v] : 0.0;
        sig->capacity[v] = model->ub ? model->ub[v] : RALPH_INFINITY;
        sig->lower[v] = model->lb ? (model->lb[v] > 0 ? model->lb[v] : 0.0) : 0.0;
        sig->var_to_arc[v] = v;
    }

    free(var_con1);
    free(var_con2);
    free(var_coef1);
    free(var_coef2);

    /* Build node data - constraint to node mapping is identity */
    for (int c = 0; c < num_cons; c++) {
        sig->con_to_node[c] = c;
    }

    /* Extract supply from RHS
     *
     * Flow conservation: outflow - inflow = supply
     * In LP form: Ax = b, where A is node-arc incidence
     * So supply[i] = b[i] for equality constraints
     *
     * For inequality constraints, we need to handle them:
     * - '<=' means supply <= b[i], which we interpret as demand
     * - '>=' means supply >= b[i], which we interpret as supply
     *
     * For now, require equality constraints for pure network structure.
     */
    for (int i = 0; i < num_nodes; i++) {
        if (model->sense && model->sense[i] != 'E') {
            /* Non-equality constraint - not pure network flow */
            /* We could handle this with slack arcs, but for now reject */
            detect_network_free(sig);
            return 0;
        }
        sig->supply[i] = model->b ? model->b[i] : 0.0;
    }

    /* Fill signature */
    sig->is_network = 1;
    sig->num_nodes = num_nodes;
    sig->num_arcs = num_arcs;
    sig->obj_sense = model->obj_sense;

    return 1;
}

void detect_network_free(NetworkSignature *sig) {
    if (!sig) return;

    free(sig->tail);
    free(sig->head);
    free(sig->cost);
    free(sig->capacity);
    free(sig->lower);
    free(sig->supply);
    free(sig->var_to_arc);
    free(sig->con_to_node);

    memset(sig, 0, sizeof(NetworkSignature));
}

/*
 * Detect the specific type of network flow problem.
 */
RalphNetworkType detect_network_type(const NetworkSignature *sig) {
    if (!sig || !sig->is_network) {
        return RALPH_NETWORK_GENERAL;
    }

    int num_nodes = sig->num_nodes;
    int num_arcs = sig->num_arcs;

    /* Count sources (positive supply) and sinks (negative supply) */
    int num_sources = 0;
    int num_sinks = 0;
    int num_transship = 0;
    double total_supply = 0;
    double total_demand = 0;

    for (int i = 0; i < num_nodes; i++) {
        double s = sig->supply[i];
        if (s > TOLERANCE) {
            num_sources++;
            total_supply += s;
        } else if (s < -TOLERANCE) {
            num_sinks++;
            total_demand -= s;  /* Make positive */
        } else {
            num_transship++;
        }
    }

    /* Check for shortest path: single source, single sink, unit flow */
    if (num_sources == 1 && num_sinks == 1 && num_transship == num_nodes - 2) {
        if (fabs(total_supply - 1.0) < TOLERANCE && fabs(total_demand - 1.0) < TOLERANCE) {
            return RALPH_NETWORK_SHORTEST_PATH;
        }
    }

    /* Check for assignment: all supplies = 1, all demands = -1 */
    int is_assignment = 1;
    for (int i = 0; i < num_nodes; i++) {
        double s = sig->supply[i];
        if (fabs(s) > TOLERANCE) {
            if (fabs(fabs(s) - 1.0) > TOLERANCE) {
                is_assignment = 0;
                break;
            }
        }
    }

    if (is_assignment && num_sources == num_sinks && num_transship == 0) {
        /* Check bipartite structure: no arcs within sources or within sinks */
        int bipartite = 1;
        for (int a = 0; a < num_arcs; a++) {
            int t = sig->tail[a];
            int h = sig->head[a];
            double st = sig->supply[t];
            double sh = sig->supply[h];

            /* Tail should be source (+1), head should be sink (-1) */
            if (!(st > TOLERANCE && sh < -TOLERANCE)) {
                bipartite = 0;
                break;
            }
        }

        if (bipartite) {
            return RALPH_NETWORK_ASSIGNMENT;
        }
    }

    /* Check for transportation: bipartite (sources -> sinks), no transshipment */
    if (num_transship == 0) {
        int is_transport = 1;
        for (int a = 0; a < num_arcs; a++) {
            int t = sig->tail[a];
            int h = sig->head[a];
            double st = sig->supply[t];
            double sh = sig->supply[h];

            /* Tail should be source (positive), head should be sink (negative) */
            if (!(st > TOLERANCE && sh < -TOLERANCE)) {
                is_transport = 0;
                break;
            }
        }

        if (is_transport) {
            return RALPH_NETWORK_TRANSPORTATION;
        }
    }

    return RALPH_NETWORK_GENERAL;
}

/*
 * Solve network flow using detected structure.
 */
int solve_as_network(const NetworkSignature *sig, double *solution, double *obj_val) {
    if (!sig || !sig->is_network || !solution) {
        return -2;
    }

    /* Check for assignment problem - delegate to LAP solver */
    RalphNetworkType type = detect_network_type(sig);

    if (type == RALPH_NETWORK_ASSIGNMENT) {
        /* Use LAP solver for assignment problems */
        return solve_assignment_as_lap(sig, solution, obj_val);
    }

    /* Use network simplex for general network flow */
    return solve_network_simplex(sig, solution, obj_val);
}

/*
 * Solve assignment problem using LAP solver.
 */
static int solve_assignment_as_lap(const NetworkSignature *sig, double *solution, double *obj_val) {
    int num_nodes = sig->num_nodes;
    int num_arcs = sig->num_arcs;

    /* Count sources and sinks */
    int n_sources = 0, n_sinks = 0;
    for (int i = 0; i < num_nodes; i++) {
        if (sig->supply[i] > TOLERANCE) n_sources++;
        else if (sig->supply[i] < -TOLERANCE) n_sinks++;
    }

    if (n_sources != n_sinks) {
        return -1;  /* Infeasible: unequal sources and sinks */
    }

    int n = n_sources;

    /* Map nodes to LAP indices */
    int *source_map = (int *)calloc(num_nodes, sizeof(int));  /* node -> source index */
    int *sink_map = (int *)calloc(num_nodes, sizeof(int));    /* node -> sink index */
    int *source_nodes = (int *)calloc(n, sizeof(int));        /* source index -> node */
    int *sink_nodes = (int *)calloc(n, sizeof(int));          /* sink index -> node */

    if (!source_map || !sink_map || !source_nodes || !sink_nodes) {
        free(source_map); free(sink_map); free(source_nodes); free(sink_nodes);
        return -2;
    }

    int si = 0, ti = 0;
    for (int i = 0; i < num_nodes; i++) {
        source_map[i] = -1;
        sink_map[i] = -1;
        if (sig->supply[i] > TOLERANCE) {
            source_map[i] = si;
            source_nodes[si] = i;
            si++;
        } else if (sig->supply[i] < -TOLERANCE) {
            sink_map[i] = ti;
            sink_nodes[ti] = i;
            ti++;
        }
    }

    /* Build cost matrix for LAP */
    double *lap_cost = (double *)calloc((size_t)n * n, sizeof(double));
    int *arc_matrix = (int *)calloc((size_t)n * n, sizeof(int));  /* arc_matrix[i*n+j] = arc index */

    if (!lap_cost || !arc_matrix) {
        free(source_map); free(sink_map); free(source_nodes); free(sink_nodes);
        free(lap_cost); free(arc_matrix);
        return -2;
    }

    /* Initialize with infinity (forbidden) */
    for (int i = 0; i < n * n; i++) {
        lap_cost[i] = RALPH_LAP_INFINITY;
        arc_matrix[i] = -1;
    }

    /* Fill in costs from arcs */
    for (int a = 0; a < num_arcs; a++) {
        int t = sig->tail[a];
        int h = sig->head[a];
        int lap_row = source_map[t];
        int lap_col = sink_map[h];

        if (lap_row >= 0 && lap_col >= 0) {
            lap_cost[lap_row * n + lap_col] = sig->cost[a];
            arc_matrix[lap_row * n + lap_col] = a;
        }
    }

    /* Solve LAP */
    int *row_sol = (int *)calloc(n, sizeof(int));
    if (!row_sol) {
        free(source_map); free(sink_map); free(source_nodes); free(sink_nodes);
        free(lap_cost); free(arc_matrix);
        return -2;
    }

    double total_cost;
    RalphLapObjective objective = (sig->obj_sense == 1) ?
        RALPH_LAP_MINIMIZE : RALPH_LAP_MAXIMIZE;

    RalphLapStatus status = ralph_lap_solve(n, lap_cost, objective,
                                            row_sol, NULL, NULL, NULL, &total_cost);

    if (status != RALPH_LAP_SUCCESS) {
        free(source_map); free(sink_map); free(source_nodes); free(sink_nodes);
        free(lap_cost); free(arc_matrix); free(row_sol);
        return -1;  /* Infeasible */
    }

    /* Convert LAP solution to flow */
    memset(solution, 0, num_arcs * sizeof(double));

    for (int i = 0; i < n; i++) {
        int j = row_sol[i];
        int a = arc_matrix[i * n + j];
        if (a >= 0) {
            solution[a] = 1.0;
        }
    }

    if (obj_val) {
        *obj_val = total_cost;
    }

    free(source_map); free(sink_map); free(source_nodes); free(sink_nodes);
    free(lap_cost); free(arc_matrix); free(row_sol);

    return 0;
}

/*
 * Solve network flow using network simplex.
 */
static int solve_network_simplex(const NetworkSignature *sig, double *solution, double *obj_val) {
    RalphNetflowProblem prob = {
        .num_nodes = sig->num_nodes,
        .num_arcs = sig->num_arcs,
        .tail = sig->tail,
        .head = sig->head,
        .cost = sig->cost,
        .capacity = sig->capacity,
        .lower = sig->lower,
        .supply = sig->supply,
        .objective = (sig->obj_sense == 1) ? RALPH_NETFLOW_MINIMIZE : RALPH_NETFLOW_MAXIMIZE
    };

    RalphNetflowResult result = {
        .flow = solution,
        .potential = NULL
    };

    RalphNetflowStatus status = ralph_netflow_solve(&prob, NULL, &result, NULL);

    if (status == RALPH_NETFLOW_OPTIMAL) {
        if (obj_val) {
            *obj_val = result.objective;
        }
        return 0;
    } else if (status == RALPH_NETFLOW_INFEASIBLE) {
        return -1;
    } else {
        return -2;
    }
}

/* ============================================================================
 * MIP Network Detection and Solving
 * ============================================================================ */

int detect_network_mip(const LPModel *model, MIPNetworkSignature *sig) {
    if (!model || !sig) {
        return 0;
    }

    /* Initialize */
    memset(sig, 0, sizeof(MIPNetworkSignature));

    /* Use standard network detection */
    if (!detect_network(model, &sig->base)) {
        return 0;
    }

    int num_nodes = sig->base.num_nodes;
    int num_arcs = sig->base.num_arcs;
    sig->num_vars = model->num_vars;

    /* Allocate base copies */
    sig->base_cost = (double *)calloc(num_arcs, sizeof(double));
    sig->base_capacity = (double *)calloc(num_arcs, sizeof(double));
    sig->base_lower = (double *)calloc(num_arcs, sizeof(double));
    sig->base_supply = (double *)calloc(num_nodes, sizeof(double));

    if (!sig->base_cost || !sig->base_capacity || !sig->base_lower || !sig->base_supply) {
        detect_network_mip_free(sig);
        return 0;
    }

    memcpy(sig->base_cost, sig->base.cost, num_arcs * sizeof(double));
    memcpy(sig->base_capacity, sig->base.capacity, num_arcs * sizeof(double));
    memcpy(sig->base_lower, sig->base.lower, num_arcs * sizeof(double));
    memcpy(sig->base_supply, sig->base.supply, num_nodes * sizeof(double));

    /* Create reusable workspace */
    sig->netflow_workspace = ralph_netflow_workspace_create(num_nodes, num_arcs);
    if (!sig->netflow_workspace) {
        detect_network_mip_free(sig);
        return 0;
    }

    return 1;
}

void detect_network_mip_free(MIPNetworkSignature *sig) {
    if (!sig) return;

    detect_network_free(&sig->base);

    free(sig->base_cost);
    free(sig->base_capacity);
    free(sig->base_lower);
    free(sig->base_supply);

    if (sig->netflow_workspace) {
        ralph_netflow_workspace_free((RalphNetflowWorkspace *)sig->netflow_workspace);
    }

    memset(sig, 0, sizeof(MIPNetworkSignature));
}

int solve_network_at_node(
    MIPNetworkSignature *sig,
    const double *lb,
    const double *ub,
    double *solution,
    double *obj_val
) {
    if (!sig || !sig->base.is_network || !lb || !ub || !solution) {
        return -2;
    }

    int num_nodes = sig->base.num_nodes;
    int num_arcs = sig->base.num_arcs;
    RalphNetflowWorkspace *ws = (RalphNetflowWorkspace *)sig->netflow_workspace;

    /* Reset to base values */
    memcpy(sig->base.cost, sig->base_cost, num_arcs * sizeof(double));
    memcpy(sig->base.capacity, sig->base_capacity, num_arcs * sizeof(double));
    memcpy(sig->base.lower, sig->base_lower, num_arcs * sizeof(double));
    memcpy(sig->base.supply, sig->base_supply, num_nodes * sizeof(double));

    /* Apply variable bounds as capacity modifications */
    for (int a = 0; a < num_arcs; a++) {
        int v = sig->base.var_to_arc[a];
        if (v < 0 || v >= sig->num_vars) continue;

        /* Apply lower bound */
        if (lb[v] > sig->base.lower[a] + TOLERANCE) {
            sig->base.lower[a] = lb[v];
        }

        /* Apply upper bound */
        if (ub[v] < sig->base.capacity[a] - TOLERANCE) {
            sig->base.capacity[a] = ub[v];
        }

        /* Check for infeasibility */
        if (sig->base.lower[a] > sig->base.capacity[a] + TOLERANCE) {
            return -1;  /* Infeasible */
        }
    }

    /* Solve with modified bounds */
    RalphNetflowProblem prob = {
        .num_nodes = num_nodes,
        .num_arcs = num_arcs,
        .tail = sig->base.tail,
        .head = sig->base.head,
        .cost = sig->base.cost,
        .capacity = sig->base.capacity,
        .lower = sig->base.lower,
        .supply = sig->base.supply,
        .objective = (sig->base.obj_sense == 1) ? RALPH_NETFLOW_MINIMIZE : RALPH_NETFLOW_MAXIMIZE
    };

    RalphNetflowResult result = {
        .flow = solution,
        .potential = NULL
    };

    RalphNetflowStatus status = ralph_netflow_solve(&prob, NULL, &result, ws);

    if (status == RALPH_NETFLOW_OPTIMAL) {
        if (obj_val) {
            *obj_val = result.objective;
        }
        return 0;
    } else if (status == RALPH_NETFLOW_INFEASIBLE) {
        return -1;
    } else {
        return -2;
    }
}
