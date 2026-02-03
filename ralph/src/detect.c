/*
 * detect.c - Problem Structure Detection
 *
 * Detects special structure in LP/MIP problems to enable
 * delegation to specialized solvers.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
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

/* Global setting for LAP detection */
static int lap_detection_enabled = 1;

void ralph_set_detect_lap(int enabled) {
    lap_detection_enabled = enabled ? 1 : 0;
}

int ralph_get_detect_lap(void) {
    return lap_detection_enabled;
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
    int *var_count = (int *)calloc(num_vars, sizeof(int));
    int *var_con1 = (int *)malloc(num_vars * sizeof(int));  /* First constraint */
    int *var_con2 = (int *)malloc(num_vars * sizeof(int));  /* Second constraint */

    if (!var_count || !var_con1 || !var_con2) {
        free(var_count); free(var_con1); free(var_con2);
        return 0;
    }

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
    int *con_color = (int *)malloc(m * sizeof(int));  /* -1=unvisited, 0=row, 1=col */
    if (!con_color) {
        free(var_con1); free(var_con2);
        return 0;
    }

    for (int i = 0; i < m; i++) {
        con_color[i] = -1;
    }

    /* BFS to 2-color */
    int *queue = (int *)malloc(m * sizeof(int));
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
    int *row_map = (int *)malloc(m * sizeof(int));
    int *col_map = (int *)malloc(m * sizeof(int));
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
    double *costs = (double *)calloc(n * n, sizeof(double));
    int *var_to_row = (int *)malloc(num_vars * sizeof(int));
    int *var_to_col = (int *)malloc(num_vars * sizeof(int));

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
    int *row_sol = (int *)malloc(n * sizeof(int));
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
    sig->base_costs = (double *)malloc(n * n * sizeof(double));
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
    int *row_sol = (int *)malloc(n * sizeof(int));
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
