/*
 * lap.c - Linear Assignment Problem Solver
 *
 * Implementation of the Jonker-Volgenant-Castanon (JVC) algorithm.
 *
 * The algorithm has 4 phases:
 * 1. Column Reduction: Initialize column prices from minimum costs
 * 2. Reduction Transfer: Improve prices for singly-assigned rows
 * 3. Augmenting Row Reduction: Auction-style bidding (run twice)
 * 4. Augmentation: Dijkstra-based shortest path for remaining rows
 *
 * Performance optimizations:
 * - OpenMP SIMD for vectorized min-finding
 * - Aligned memory allocations for SIMD
 * - Parallel matrix operations
 */

#include "lap.h"
#include "ralph.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#ifdef _OPENMP
#include <omp.h>
#endif

/* Memory alignment for SIMD (64 bytes = AVX-512 cache line) */
#define LAP_ALIGNMENT 64

/* Aligned allocation helpers */
static void* lap_aligned_alloc(size_t size) {
#ifdef _WIN32
    return _aligned_malloc(size, LAP_ALIGNMENT);
#else
    void *ptr = NULL;
    if (posix_memalign(&ptr, LAP_ALIGNMENT, size) != 0) {
        return NULL;
    }
    return ptr;
#endif
}

static void lap_aligned_free(void *ptr) {
#ifdef _WIN32
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

/* Access cost matrix element */
#define COST(i, j) cost[(i) * n + (j)]

/* Check if value represents infinity/forbidden */
static inline int is_infinite(double val) {
    return val >= RALPH_LAP_INFINITY * 0.5;
}

/* Compare with tolerance */
static inline int approx_equal(double a, double b) {
    return fabs(a - b) < RALPH_LAP_TOLERANCE;
}

static inline int approx_less_or_equal(double a, double b) {
    return a < b || approx_equal(a, b);
}

/* ============================================================================
 * JVC Algorithm Implementation
 * ============================================================================ */

RalphLapStatus ralph_lap_solve(
    int n,
    const double *cost,
    RalphLapObjective objective,
    int *row_sol,
    int *col_sol,
    double *u,
    double *v,
    double *total_cost
) {
    if (n <= 0 || cost == NULL || row_sol == NULL) {
        return RALPH_LAP_INVALID_INPUT;
    }

    /* Handle trivial case */
    if (n == 1) {
        row_sol[0] = 0;
        if (col_sol) col_sol[0] = 0;
        if (u) u[0] = COST(0, 0);
        if (v) v[0] = 0.0;
        if (total_cost) *total_cost = COST(0, 0);
        return RALPH_LAP_SUCCESS;
    }

    /* Allocate working arrays */
    double *work_cost = NULL;   /* Working cost matrix (possibly negated) */
    double *col_price = NULL;   /* Column dual variables (v) */
    double *row_price = NULL;   /* Row dual variables (u) */
    int *row_assign = NULL;     /* row_assign[i] = column assigned to row i */
    int *col_assign = NULL;     /* col_assign[j] = row assigned to column j */
    int *matches = NULL;        /* Number of times each row matches minimum */
    int *free_rows = NULL;      /* List of unassigned rows */
    double *dist = NULL;        /* Shortest path distances */
    int *pred = NULL;           /* Predecessor row in augmenting path */
    int *col_list = NULL;       /* Columns partitioned by state */
    int *in_free_list = NULL;   /* Track which rows are in free list */

    RalphLapStatus status = RALPH_LAP_SUCCESS;
    int i, j, k;

    /* Allocate memory with alignment for SIMD */
    work_cost = (double *)lap_aligned_alloc(n * n * sizeof(double));
    col_price = (double *)lap_aligned_alloc(n * sizeof(double));
    row_price = (double *)lap_aligned_alloc(n * sizeof(double));
    dist = (double *)lap_aligned_alloc(n * sizeof(double));
    row_assign = (int *)malloc(n * sizeof(int));
    col_assign = (int *)malloc(n * sizeof(int));
    matches = (int *)calloc(n, sizeof(int));
    free_rows = (int *)malloc(2 * n * sizeof(int));  /* Extra space for auction phase */
    pred = (int *)malloc(n * sizeof(int));
    col_list = (int *)malloc(n * sizeof(int));
    in_free_list = (int *)calloc(n, sizeof(int));

    if (!work_cost || !col_price || !row_price || !row_assign || !col_assign ||
        !matches || !free_rows || !dist || !pred || !col_list || !in_free_list) {
        status = RALPH_LAP_MEMORY_ERROR;
        goto cleanup;
    }

    /* Zero-initialize aligned arrays */
    memset(col_price, 0, n * sizeof(double));
    memset(row_price, 0, n * sizeof(double));

    /* Initialize assignments to unassigned */
    for (i = 0; i < n; i++) {
        row_assign[i] = RALPH_LAP_UNASSIGNED;
        col_assign[i] = RALPH_LAP_UNASSIGNED;
    }

    /* Copy cost matrix, negate if maximizing (parallelized) */
    if (objective == RALPH_LAP_MAXIMIZE) {
        #pragma omp parallel for simd if(n > 100)
        for (i = 0; i < n * n; i++) {
            work_cost[i] = -cost[i];
        }
    } else {
        memcpy(work_cost, cost, n * n * sizeof(double));
    }

    /* ========================================================================
     * PHASE 1: Column Reduction
     * For each column, find minimum cost and use as column price.
     * Assign column to row with minimum cost if not already assigned.
     *
     * Cache-optimized: Process rows sequentially (row-major access),
     * track minimum and min_row for each column.
     * ======================================================================== */

    /* Temporary arrays to track min value and row per column */
    int *col_min_row = pred;  /* Reuse pred array temporarily */

    /* Initialize with first row */
    #pragma omp simd
    for (j = 0; j < n; j++) {
        col_price[j] = work_cost[j];
        col_min_row[j] = 0;
    }

    /* Process remaining rows with cache-friendly row-major access
     * Note: Can't easily parallelize across rows due to col_price/col_min_row updates.
     * Instead, for large n, we can parallelize the inner loop. */
    for (i = 1; i < n; i++) {
        const double *row_costs = &work_cost[i * n];
        /* SIMD vectorization of the comparison/update */
        for (j = 0; j < n; j++) {
            if (row_costs[j] < col_price[j]) {
                col_price[j] = row_costs[j];
                col_min_row[j] = i;
            }
        }
    }

    /* Now process columns in reverse order to assign (as before) */
    for (j = n - 1; j >= 0; j--) {
        int min_row = col_min_row[j];
        matches[min_row]++;

        if (matches[min_row] == 1) {
            /* First time this row is matched - assign it */
            row_assign[min_row] = j;
            col_assign[j] = min_row;
        } else {
            /* Row already matched to another column */
            /* Check if this column is better than current */
            int cur_col = row_assign[min_row];
            if (cur_col >= 0 && col_price[j] < col_price[cur_col]) {
                /* Reassign to this column */
                col_assign[cur_col] = RALPH_LAP_UNASSIGNED;
                row_assign[min_row] = j;
                col_assign[j] = min_row;
            } else {
                col_assign[j] = RALPH_LAP_UNASSIGNED;
            }
        }
    }

    /* ========================================================================
     * PHASE 2: Reduction Transfer
     * For rows matched exactly once, compute the gap to the second-best
     * column and use it to improve column prices.
     * ======================================================================== */
    int num_free = 0;

    for (i = 0; i < n; i++) {
        if (matches[i] == 0) {
            /* Row not matched - add to free list */
            free_rows[num_free++] = i;
            in_free_list[i] = 1;
        } else if (matches[i] == 1) {
            /* Row matched exactly once - transfer reduction */
            int j1 = row_assign[i];
            double min_reduced = DBL_MAX;
            const double *row_costs = &work_cost[i * n];

            /* Find minimum reduced cost for columns other than j1 */
            for (j = 0; j < n; j++) {
                if (j != j1) {
                    double reduced = row_costs[j] - col_price[j];
                    if (reduced < min_reduced) {
                        min_reduced = reduced;
                    }
                }
            }

            /* Reduce column price by the gap */
            col_price[j1] -= min_reduced;
        }
    }

    /* ========================================================================
     * PHASE 3: Augmenting Row Reduction (Auction Phase)
     * Run twice for better convergence.
     * ======================================================================== */
    for (int loop = 0; loop < 2 && num_free > 0; loop++) {
        int k_free = 0;
        int max_iter = n * n;  /* Limit iterations to prevent infinite loops */
        int iter = 0;

        while (k_free < num_free && iter < max_iter) {
            iter++;
            i = free_rows[k_free++];
            /* Note: Keep in_free_list[i] = 1 to prevent re-adding if displaced */

            /* Find minimum and second-minimum reduced costs */
            double u1 = DBL_MAX;  /* Minimum reduced cost */
            double u2 = DBL_MAX;  /* Second minimum */
            int j1 = -1, j2 = -1;
            const double *row_costs = &work_cost[i * n];

            /* Single pass to find both min and second-min */
            for (j = 0; j < n; j++) {
                double reduced = row_costs[j] - col_price[j];
                if (reduced < u1) {
                    u2 = u1;
                    j2 = j1;
                    u1 = reduced;
                    j1 = j;
                } else if (reduced < u2) {
                    u2 = reduced;
                    j2 = j;
                }
            }

            if (j1 < 0) {
                /* All costs are infinite - problem is infeasible */
                status = RALPH_LAP_INFEASIBLE;
                goto cleanup;
            }

            /* Record row price (for potential dual output) */
            row_price[i] = u1;

            /* If gap exists, reduce column price */
            if (u1 < u2 - RALPH_LAP_TOLERANCE) {
                col_price[j1] = col_price[j1] - u2 + u1;
            } else if (col_assign[j1] >= 0 && j2 >= 0 && col_assign[j2] < 0) {
                /* No gap, j1 is assigned but j2 is free - use j2 */
                j1 = j2;
            }
            /* Otherwise keep j1 - will displace current assignee */

            /* If j1 is still invalid (shouldn't happen with proper input) */
            if (j1 < 0) {
                status = RALPH_LAP_INFEASIBLE;
                goto cleanup;
            }

            /* Assign row i to column j1 */
            int prev_row = col_assign[j1];
            if (prev_row >= 0) {
                row_assign[prev_row] = RALPH_LAP_UNASSIGNED;  /* Unassign displaced row */
            }
            row_assign[i] = j1;
            col_assign[j1] = i;

            /* If j1 was previously assigned, that row becomes free */
            if (prev_row >= 0 && !in_free_list[prev_row]) {
                in_free_list[prev_row] = 1;
                if (loop == 1) {
                    /* In second pass, don't add back - will be handled in Phase 4 */
                    free_rows[--k_free] = prev_row;
                } else {
                    /* In first pass, add to end of free list */
                    free_rows[num_free++] = prev_row;
                }
            }
        }

        /* Rebuild free list for next pass */
        if (loop == 0) {
            num_free = 0;
            memset(in_free_list, 0, n * sizeof(int));
            for (i = 0; i < n; i++) {
                if (row_assign[i] == RALPH_LAP_UNASSIGNED) {
                    free_rows[num_free++] = i;
                    in_free_list[i] = 1;
                }
            }
        }
    }

    /* Count remaining free rows */
    num_free = 0;
    for (i = 0; i < n; i++) {
        if (row_assign[i] == RALPH_LAP_UNASSIGNED) {
            free_rows[num_free++] = i;
        }
    }

    /* ========================================================================
     * PHASE 4: Augmentation (Dijkstra-based shortest path)
     * For each remaining unassigned row, find shortest augmenting path.
     * ======================================================================== */
    for (int f = 0; f < num_free; f++) {
        int free_row = free_rows[f];

        /* Initialize column list and distances */
        const double *row_costs = &work_cost[free_row * n];

        #pragma omp simd
        for (j = 0; j < n; j++) {
            col_list[j] = j;
            dist[j] = row_costs[j] - col_price[j];
            pred[j] = free_row;
        }

        int low = 0;    /* Start of scanned columns with min distance */
        int up = 0;     /* Start of columns not yet scanned */
        int last = -1;  /* Last scanned column */
        int end_col = -1;
        double min_dist = 0.0;

        /* Dijkstra's algorithm to find shortest path to unassigned column */
        while (end_col < 0) {
            /* Find minimum distance among unscanned columns */
            if (up == low) {
                last = low - 1;
                min_dist = DBL_MAX;

                for (k = up; k < n; k++) {
                    j = col_list[k];
                    double d = dist[j];
                    if (d <= min_dist) {
                        if (d < min_dist) {
                            up = low;
                            min_dist = d;
                        }
                        /* Add to current minimum set */
                        col_list[k] = col_list[up];
                        col_list[up] = j;
                        up++;
                    }
                }

                /* Check if any minimum column is unassigned */
                for (k = low; k < up; k++) {
                    if (col_assign[col_list[k]] == RALPH_LAP_UNASSIGNED) {
                        end_col = col_list[k];
                        break;
                    }
                }
            }

            if (end_col < 0) {
                /* Relax edges from next minimum column */
                j = col_list[low++];
                last++;
                int assigned_row = col_assign[j];
                double h = work_cost[assigned_row * n + j] - col_price[j] - min_dist;

                for (k = up; k < n; k++) {
                    int jj = col_list[k];
                    double new_dist = work_cost[assigned_row * n + jj] - col_price[jj] - h;

                    if (new_dist < dist[jj]) {
                        pred[jj] = assigned_row;
                        dist[jj] = new_dist;

                        if (approx_less_or_equal(new_dist, min_dist)) {
                            /* Found at current minimum distance */
                            if (col_assign[jj] == RALPH_LAP_UNASSIGNED) {
                                end_col = jj;
                                break;
                            } else {
                                /* Add to scan list */
                                col_list[k] = col_list[up];
                                col_list[up] = jj;
                                up++;
                            }
                        }
                    }
                }
            }
        }

        /* Update column prices for all scanned columns */
        for (k = 0; k <= last; k++) {
            j = col_list[k];
            col_price[j] += dist[j] - min_dist;
        }

        /* Trace back and flip assignments along augmenting path */
        int cur_row;
        do {
            cur_row = pred[end_col];
            col_assign[end_col] = cur_row;
            int prev_col = row_assign[cur_row];
            row_assign[cur_row] = end_col;
            end_col = prev_col;
        } while (cur_row != free_row);
    }

    /* ========================================================================
     * Compute outputs
     * ======================================================================== */

    /* Copy row solution */
    memcpy(row_sol, row_assign, n * sizeof(int));

    /* Copy column solution if requested */
    if (col_sol) {
        memcpy(col_sol, col_assign, n * sizeof(int));
    }

    /* Compute row prices (u) from complementary slackness */
    if (u) {
        for (i = 0; i < n; i++) {
            j = row_assign[i];
            u[i] = work_cost[i * n + j] - col_price[j];
        }
        /* Negate if we were maximizing */
        if (objective == RALPH_LAP_MAXIMIZE) {
            #pragma omp simd
            for (i = 0; i < n; i++) {
                u[i] = -u[i];
            }
        }
    }

    /* Copy column prices (v) if requested */
    if (v) {
        memcpy(v, col_price, n * sizeof(double));
        /* Negate if we were maximizing */
        if (objective == RALPH_LAP_MAXIMIZE) {
            #pragma omp simd
            for (j = 0; j < n; j++) {
                v[j] = -v[j];
            }
        }
    }

    /* Compute total cost */
    if (total_cost) {
        double sum = 0.0;
        #pragma omp simd reduction(+:sum)
        for (i = 0; i < n; i++) {
            int jj = row_assign[i];
            double c = cost[i * n + jj];  /* Use original cost matrix */
            if (c < RALPH_LAP_INFINITY * 0.5) {
                sum += c;
            }
        }
        *total_cost = sum;
    }

cleanup:
    lap_aligned_free(work_cost);
    lap_aligned_free(col_price);
    lap_aligned_free(row_price);
    lap_aligned_free(dist);
    free(row_assign);
    free(col_assign);
    free(matches);
    free(free_rows);
    free(pred);
    free(col_list);
    free(in_free_list);

    return status;
}

/* ============================================================================
 * Sparse LAP Solver
 * ============================================================================ */

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
) {
    if (n <= 0 || row_ptr == NULL || col_idx == NULL || values == NULL || row_sol == NULL) {
        return RALPH_LAP_INVALID_INPUT;
    }

    /* Convert sparse to dense and use dense solver */
    /* For truly sparse problems, a dedicated sparse JVC would be better */
    double *cost = (double *)malloc(n * n * sizeof(double));
    if (!cost) {
        return RALPH_LAP_MEMORY_ERROR;
    }

    /* Initialize with infinity */
    for (int i = 0; i < n * n; i++) {
        cost[i] = RALPH_LAP_INFINITY;
    }

    /* Fill in finite costs from CSR */
    for (int i = 0; i < n; i++) {
        for (int k = row_ptr[i]; k < row_ptr[i + 1]; k++) {
            int j = col_idx[k];
            if (j >= 0 && j < n) {
                cost[i * n + j] = values[k];
            }
        }
    }

    RalphLapStatus status = ralph_lap_solve(n, cost, objective, row_sol, col_sol,
                                             NULL, NULL, total_cost);

    free(cost);
    return status;
}

/* ============================================================================
 * Verification
 * ============================================================================ */

int ralph_lap_verify(
    int n,
    const double *cost,
    const int *row_sol,
    double *cost_out
) {
    if (n <= 0 || cost == NULL || row_sol == NULL) {
        return 0;
    }

    /* Check that row_sol is a valid permutation */
    int *seen = (int *)calloc(n, sizeof(int));
    if (!seen) return 0;

    for (int i = 0; i < n; i++) {
        int j = row_sol[i];
        if (j < 0 || j >= n || seen[j]) {
            free(seen);
            return 0;
        }
        seen[j] = 1;
    }
    free(seen);

    /* Compute cost if requested */
    if (cost_out) {
        double sum = 0.0;
        for (int i = 0; i < n; i++) {
            int j = row_sol[i];
            double c = COST(i, j);
            if (!is_infinite(c)) {
                sum += c;
            }
        }
        *cost_out = sum;
    }

    return 1;
}

/* ============================================================================
 * LP-based solver (for verification)
 * ============================================================================ */

RalphLapStatus ralph_lap_solve_lp(
    int n,
    const double *cost,
    RalphLapObjective objective,
    int *row_sol,
    double *total_cost
) {
    if (n <= 0 || cost == NULL || row_sol == NULL) {
        return RALPH_LAP_INVALID_INPUT;
    }

    /* Create LP model:
     * Variables: x[i][j] for i,j in 0..n-1 (n^2 variables)
     * Objective: minimize/maximize sum c[i][j] * x[i][j]
     * Constraints:
     *   sum_j x[i][j] = 1 for all i (row assignment)
     *   sum_i x[i][j] = 1 for all j (column assignment)
     *   x[i][j] >= 0
     */

    RalphModel *model = ralph_create();
    if (!model) {
        return RALPH_LAP_MEMORY_ERROR;
    }

    RalphLapStatus status = RALPH_LAP_SUCCESS;

    /* Set objective sense */
    ralph_set_obj_sense(model, objective == RALPH_LAP_MAXIMIZE ?
                        RALPH_MAXIMIZE : RALPH_MINIMIZE);

    /* Add n^2 variables with objective coefficients from cost matrix */
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            double c = COST(i, j);
            /* Replace infinite costs with large finite value */
            if (is_infinite(c)) {
                c = RALPH_LAP_INFINITY;
            }
            ralph_add_var(model, 0.0, 1.0, c, RALPH_CONTINUOUS);
        }
    }

    /* Constraint arrays */
    int *indices = (int *)malloc(n * sizeof(int));
    double *values = (double *)malloc(n * sizeof(double));
    if (!indices || !values) {
        free(indices);
        free(values);
        ralph_free(model);
        return RALPH_LAP_MEMORY_ERROR;
    }

    for (int k = 0; k < n; k++) {
        values[k] = 1.0;
    }

    /* Row assignment constraints: sum_j x[i][j] = 1 */
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            indices[j] = i * n + j;
        }
        ralph_add_constraint(model, n, indices, values, RALPH_EQUAL, 1.0);
    }

    /* Column assignment constraints: sum_i x[i][j] = 1 */
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) {
            indices[i] = i * n + j;
        }
        ralph_add_constraint(model, n, indices, values, RALPH_EQUAL, 1.0);
    }

    free(indices);
    free(values);

    /* Solve */
    ralph_optimize(model);

    if (ralph_get_status(model) != RALPH_STATUS_OPTIMAL) {
        ralph_free(model);
        return RALPH_LAP_INFEASIBLE;
    }

    /* Extract solution */
    double *x = (double *)malloc(n * n * sizeof(double));
    if (!x) {
        ralph_free(model);
        return RALPH_LAP_MEMORY_ERROR;
    }

    ralph_get_solution(model, x);

    /* Convert continuous solution to assignment (pick highest value in each row) */
    for (int i = 0; i < n; i++) {
        int best_j = 0;
        double best_val = x[i * n + 0];
        for (int j = 1; j < n; j++) {
            if (x[i * n + j] > best_val) {
                best_val = x[i * n + j];
                best_j = j;
            }
        }
        row_sol[i] = best_j;
    }

    /* Get total cost */
    if (total_cost) {
        *total_cost = ralph_get_objval(model);
    }

    free(x);
    ralph_free(model);
    return status;
}

/* ============================================================================
 * Status string
 * ============================================================================ */

const char* ralph_lap_status_string(RalphLapStatus status) {
    switch (status) {
        case RALPH_LAP_SUCCESS:       return "Success";
        case RALPH_LAP_INFEASIBLE:    return "Infeasible";
        case RALPH_LAP_INVALID_INPUT: return "Invalid input";
        case RALPH_LAP_MEMORY_ERROR:  return "Memory error";
        default:                       return "Unknown status";
    }
}
