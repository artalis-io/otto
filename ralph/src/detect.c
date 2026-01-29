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
