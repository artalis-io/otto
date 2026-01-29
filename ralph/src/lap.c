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

/* Heap threshold: use heap-based Dijkstra for n >= this value
 * The linear scan version is faster for smaller n due to:
 * - Lower constant factors
 * - Better cache utilization
 * - Simultaneous processing of columns at same distance
 * Heap is only beneficial for very large problems (n > 3000)
 */
#define LAP_HEAP_THRESHOLD 3000

/* SIMD threshold: use SIMD optimization for n >= this value */
#define LAP_SIMD_THRESHOLD 100

/* Parallel threshold: use OpenMP threading for n >= this value
 * Set very high to avoid parallel overhead - SIMD provides most benefit.
 * OpenMP thread parallelism only helps for very large problems where
 * thread creation/synchronization costs are amortized.
 */
#define LAP_PARALLEL_THRESHOLD 5000

/* Block size for cache-friendly column processing */
#define LAP_BLOCK_SIZE 64

/* Global setting for parallelization (1 = enabled, 0 = disabled) */
static int lap_parallel_enabled = 1;

/* Global settings for ε-scaling auction */
static int lap_epsilon_scaling_enabled = 0;
static double lap_epsilon_factor = 4.0;

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
 * Workspace Structure (consolidated allocation)
 * ============================================================================ */

struct RalphLapWorkspace {
    int max_n;              /* Maximum problem size */

    /* Memory block (single allocation) */
    void *memory_block;
    size_t block_size;

    /* Working arrays (pointers into memory_block) */
    double *work_cost;      /* n×n working cost matrix */
    double *col_price;      /* Column dual variables (v) */
    double *row_price;      /* Row dual variables (u) */
    double *dist;           /* Shortest path distances */
    int *row_assign;        /* row_assign[i] = column assigned to row i */
    int *col_assign;        /* col_assign[j] = row assigned to column j */
    int *matches;           /* Number of times each row matches minimum */
    int *free_rows;         /* List of unassigned rows (2n capacity) */
    int *pred;              /* Predecessor row in augmenting path */
    int *col_list;          /* Columns partitioned by state */
    int *in_free_list;      /* Track which rows are in free list */

    /* Heap for Dijkstra (Phase 4) */
    int *heap;              /* Binary min-heap of column indices */
    int *heap_pos;          /* heap_pos[j] = position of column j in heap (-1 if not in heap) */

    /* Warm start state */
    int warm_start_valid;   /* 1 if warm start data is valid, 0 otherwise */
    int warm_start_n;       /* Problem size for warm start */
    double *warm_u;         /* Saved row duals for warm start */
    double *warm_v;         /* Saved column duals for warm start */
    int *warm_row_sol;      /* Saved row solution for warm start */
    int *warm_col_sol;      /* Saved column solution for warm start */
};

/* ============================================================================
 * Binary Heap for Dijkstra
 * ============================================================================ */

/* Swap two elements in heap and update positions */
static inline void heap_swap(int *heap, int *heap_pos, int i, int j) {
    int ci = heap[i];
    int cj = heap[j];
    heap[i] = cj;
    heap[j] = ci;
    heap_pos[ci] = j;
    heap_pos[cj] = i;
}

/* Sift up element at index i */
static inline void heap_sift_up(int *heap, int *heap_pos, const double *dist, int i) {
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (dist[heap[i]] < dist[heap[parent]]) {
            heap_swap(heap, heap_pos, i, parent);
            i = parent;
        } else {
            break;
        }
    }
}

/* Sift down element at index i with heap size n */
static inline void heap_sift_down(int *heap, int *heap_pos, const double *dist, int i, int n) {
    while (1) {
        int smallest = i;
        int left = 2 * i + 1;
        int right = 2 * i + 2;

        if (left < n && dist[heap[left]] < dist[heap[smallest]]) {
            smallest = left;
        }
        if (right < n && dist[heap[right]] < dist[heap[smallest]]) {
            smallest = right;
        }

        if (smallest != i) {
            heap_swap(heap, heap_pos, i, smallest);
            i = smallest;
        } else {
            break;
        }
    }
}

/* Extract minimum from heap */
static inline int heap_pop(int *heap, int *heap_pos, const double *dist, int *heap_size) {
    int min_col = heap[0];
    heap_pos[min_col] = -1;
    (*heap_size)--;

    if (*heap_size > 0) {
        heap[0] = heap[*heap_size];
        heap_pos[heap[0]] = 0;
        heap_sift_down(heap, heap_pos, dist, 0, *heap_size);
    }

    return min_col;
}

/* Decrease key (update distance) for column in heap */
static inline void heap_decrease_key(int *heap, int *heap_pos, const double *dist, int col) {
    int pos = heap_pos[col];
    if (pos >= 0) {
        heap_sift_up(heap, heap_pos, dist, pos);
    }
}

/* ============================================================================
 * Workspace Management
 * ============================================================================ */

RalphLapWorkspace* ralph_lap_workspace_create(int max_n) {
    if (max_n <= 0) {
        return NULL;
    }

    RalphLapWorkspace *ws = (RalphLapWorkspace *)malloc(sizeof(RalphLapWorkspace));
    if (!ws) {
        return NULL;
    }

    ws->max_n = max_n;

    /* Calculate total memory needed with alignment padding */
    size_t n = (size_t)max_n;
    size_t n2 = n * n;

    /* Calculate sizes with alignment for each array */
    size_t work_cost_size = ((n2 * sizeof(double) + LAP_ALIGNMENT - 1) / LAP_ALIGNMENT) * LAP_ALIGNMENT;
    size_t col_price_size = ((n * sizeof(double) + LAP_ALIGNMENT - 1) / LAP_ALIGNMENT) * LAP_ALIGNMENT;
    size_t row_price_size = ((n * sizeof(double) + LAP_ALIGNMENT - 1) / LAP_ALIGNMENT) * LAP_ALIGNMENT;
    size_t dist_size = ((n * sizeof(double) + LAP_ALIGNMENT - 1) / LAP_ALIGNMENT) * LAP_ALIGNMENT;
    size_t row_assign_size = ((n * sizeof(int) + LAP_ALIGNMENT - 1) / LAP_ALIGNMENT) * LAP_ALIGNMENT;
    size_t col_assign_size = ((n * sizeof(int) + LAP_ALIGNMENT - 1) / LAP_ALIGNMENT) * LAP_ALIGNMENT;
    size_t matches_size = ((n * sizeof(int) + LAP_ALIGNMENT - 1) / LAP_ALIGNMENT) * LAP_ALIGNMENT;
    size_t free_rows_size = ((2 * n * sizeof(int) + LAP_ALIGNMENT - 1) / LAP_ALIGNMENT) * LAP_ALIGNMENT;
    size_t pred_size = ((n * sizeof(int) + LAP_ALIGNMENT - 1) / LAP_ALIGNMENT) * LAP_ALIGNMENT;
    size_t col_list_size = ((n * sizeof(int) + LAP_ALIGNMENT - 1) / LAP_ALIGNMENT) * LAP_ALIGNMENT;
    size_t in_free_list_size = ((n * sizeof(int) + LAP_ALIGNMENT - 1) / LAP_ALIGNMENT) * LAP_ALIGNMENT;
    size_t heap_size = ((n * sizeof(int) + LAP_ALIGNMENT - 1) / LAP_ALIGNMENT) * LAP_ALIGNMENT;
    size_t heap_pos_size = ((n * sizeof(int) + LAP_ALIGNMENT - 1) / LAP_ALIGNMENT) * LAP_ALIGNMENT;
    /* Warm start arrays */
    size_t warm_u_size = ((n * sizeof(double) + LAP_ALIGNMENT - 1) / LAP_ALIGNMENT) * LAP_ALIGNMENT;
    size_t warm_v_size = ((n * sizeof(double) + LAP_ALIGNMENT - 1) / LAP_ALIGNMENT) * LAP_ALIGNMENT;
    size_t warm_row_sol_size = ((n * sizeof(int) + LAP_ALIGNMENT - 1) / LAP_ALIGNMENT) * LAP_ALIGNMENT;
    size_t warm_col_sol_size = ((n * sizeof(int) + LAP_ALIGNMENT - 1) / LAP_ALIGNMENT) * LAP_ALIGNMENT;

    ws->block_size = work_cost_size + col_price_size + row_price_size + dist_size +
                     row_assign_size + col_assign_size + matches_size + free_rows_size +
                     pred_size + col_list_size + in_free_list_size + heap_size + heap_pos_size +
                     warm_u_size + warm_v_size + warm_row_sol_size + warm_col_sol_size;

    /* Allocate single aligned block */
    ws->memory_block = lap_aligned_alloc(ws->block_size);
    if (!ws->memory_block) {
        free(ws);
        return NULL;
    }

    /* Set up pointers into the block */
    char *ptr = (char *)ws->memory_block;

    ws->work_cost = (double *)ptr; ptr += work_cost_size;
    ws->col_price = (double *)ptr; ptr += col_price_size;
    ws->row_price = (double *)ptr; ptr += row_price_size;
    ws->dist = (double *)ptr; ptr += dist_size;
    ws->row_assign = (int *)ptr; ptr += row_assign_size;
    ws->col_assign = (int *)ptr; ptr += col_assign_size;
    ws->matches = (int *)ptr; ptr += matches_size;
    ws->free_rows = (int *)ptr; ptr += free_rows_size;
    ws->pred = (int *)ptr; ptr += pred_size;
    ws->col_list = (int *)ptr; ptr += col_list_size;
    ws->in_free_list = (int *)ptr; ptr += in_free_list_size;
    ws->heap = (int *)ptr; ptr += heap_size;
    ws->heap_pos = (int *)ptr; ptr += heap_pos_size;
    /* Warm start arrays */
    ws->warm_u = (double *)ptr; ptr += warm_u_size;
    ws->warm_v = (double *)ptr; ptr += warm_v_size;
    ws->warm_row_sol = (int *)ptr; ptr += warm_row_sol_size;
    ws->warm_col_sol = (int *)ptr;

    /* Initialize warm start as invalid */
    ws->warm_start_valid = 0;
    ws->warm_start_n = 0;

    /* Initialize warm start arrays to safe values */
    memset(ws->warm_u, 0, n * sizeof(double));
    memset(ws->warm_v, 0, n * sizeof(double));
    for (size_t i = 0; i < n; i++) {
        ws->warm_row_sol[i] = RALPH_LAP_UNASSIGNED;
        ws->warm_col_sol[i] = RALPH_LAP_UNASSIGNED;
    }

    return ws;
}

void ralph_lap_workspace_free(RalphLapWorkspace *ws) {
    if (ws) {
        lap_aligned_free(ws->memory_block);
        free(ws);
    }
}

int ralph_lap_workspace_max_n(const RalphLapWorkspace *ws) {
    return ws ? ws->max_n : 0;
}

RalphLapStatus ralph_lap_warm_start(
    RalphLapWorkspace *ws,
    int n,
    const double *u,
    const double *v,
    const int *row_sol,
    const int *col_sol
) {
    if (!ws || n <= 0 || n > ws->max_n || !v) {
        return RALPH_LAP_INVALID_INPUT;
    }

    /* Store dimension */
    ws->warm_start_n = n;

    /* Copy column duals (required) */
    memcpy(ws->warm_v, v, n * sizeof(double));

    /* Copy row duals if provided */
    if (u) {
        memcpy(ws->warm_u, u, n * sizeof(double));
    } else {
        memset(ws->warm_u, 0, n * sizeof(double));
    }

    /* Copy solution if provided */
    if (row_sol && col_sol) {
        memcpy(ws->warm_row_sol, row_sol, n * sizeof(int));
        memcpy(ws->warm_col_sol, col_sol, n * sizeof(int));
    } else {
        /* Mark solution as invalid by setting to -1 */
        for (int i = 0; i < n; i++) {
            ws->warm_row_sol[i] = RALPH_LAP_UNASSIGNED;
            ws->warm_col_sol[i] = RALPH_LAP_UNASSIGNED;
        }
    }

    ws->warm_start_valid = 1;
    return RALPH_LAP_SUCCESS;
}

void ralph_lap_warm_start_clear(RalphLapWorkspace *ws) {
    if (ws) {
        ws->warm_start_valid = 0;
        ws->warm_start_n = 0;
    }
}

int ralph_lap_warm_start_valid(const RalphLapWorkspace *ws) {
    return ws ? ws->warm_start_valid : 0;
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
 * JVC Algorithm Implementation (Internal)
 * ============================================================================ */

/* Internal solver that works with a workspace */
static RalphLapStatus lap_solve_internal(
    int n,
    const double *cost,
    RalphLapObjective objective,
    int *row_sol,
    int *col_sol,
    double *u,
    double *v,
    double *total_cost,
    RalphLapWorkspace *ws
) {
    int i, j, k;
    RalphLapStatus status = RALPH_LAP_SUCCESS;

    /* Get working arrays from workspace */
    double *work_cost = ws->work_cost;
    double *col_price = ws->col_price;
    double *row_price = ws->row_price;
    double *dist = ws->dist;
    int *row_assign = ws->row_assign;
    int *col_assign = ws->col_assign;
    int *matches = ws->matches;
    int *free_rows = ws->free_rows;
    int *pred = ws->pred;
    int *col_list = ws->col_list;
    int *in_free_list = ws->in_free_list;
    int *heap = ws->heap;
    int *heap_pos = ws->heap_pos;

    /* Use heap for large problems */
    int use_heap = (n >= LAP_HEAP_THRESHOLD);

    /* Zero-initialize arrays */
    memset(col_price, 0, n * sizeof(double));
    memset(row_price, 0, n * sizeof(double));
    memset(matches, 0, n * sizeof(int));
    memset(in_free_list, 0, n * sizeof(int));

    /* Initialize assignments to unassigned */
    for (i = 0; i < n; i++) {
        row_assign[i] = RALPH_LAP_UNASSIGNED;
        col_assign[i] = RALPH_LAP_UNASSIGNED;
    }

    /* Copy cost matrix, negate if maximizing
     * IMPORTANT: Preserve INFINITY values so forbidden edges stay forbidden */
    if (objective == RALPH_LAP_MAXIMIZE) {
        for (i = 0; i < n * n; i++) {
            if (is_infinite(cost[i])) {
                work_cost[i] = RALPH_LAP_INFINITY;
            } else {
                work_cost[i] = -cost[i];
            }
        }
    } else {
        memcpy(work_cost, cost, n * n * sizeof(double));
    }

    /* ========================================================================
     * PHASE 1: Column Reduction
     * For each column, find minimum cost and use as column price.
     * Assign column to row with minimum cost if not already assigned.
     *
     * Optimization strategies:
     * - Small n: Row-major sequential scan (cache-friendly)
     * - Large n: Parallel column blocks with OpenMP
     * ======================================================================== */

    /* Temporary arrays to track min value and row per column */
    int *col_min_row = pred;  /* Reuse pred array temporarily */

    int use_parallel = lap_parallel_enabled && (n >= LAP_PARALLEL_THRESHOLD);
    int use_simd = (n >= LAP_SIMD_THRESHOLD);

    if (use_parallel) {
        /* Parallel version: process column blocks independently */
        #pragma omp parallel
        {
            #pragma omp for schedule(static)
            for (int jb = 0; jb < n; jb += LAP_BLOCK_SIZE) {
                int j_end = (jb + LAP_BLOCK_SIZE < n) ? jb + LAP_BLOCK_SIZE : n;

                /* Initialize block with first row */
                for (int jj = jb; jj < j_end; jj++) {
                    col_price[jj] = work_cost[jj];
                    col_min_row[jj] = 0;
                }

                /* Process all rows for this column block */
                for (int ii = 1; ii < n; ii++) {
                    const double *row_costs = &work_cost[ii * n];
                    for (int jj = jb; jj < j_end; jj++) {
                        if (row_costs[jj] < col_price[jj]) {
                            col_price[jj] = row_costs[jj];
                            col_min_row[jj] = ii;
                        }
                    }
                }
            }
        }
    } else {
        /* Sequential version: row-major access (cache-friendly for small n) */
        /* Initialize with first row */
        #pragma omp simd
        for (j = 0; j < n; j++) {
            col_price[j] = work_cost[j];
            col_min_row[j] = 0;
        }

        /* Process remaining rows */
        for (i = 1; i < n; i++) {
            const double *row_costs = &work_cost[i * n];
            for (j = 0; j < n; j++) {
                if (row_costs[j] < col_price[j]) {
                    col_price[j] = row_costs[j];
                    col_min_row[j] = i;
                }
            }
        }
    }

    /* Process columns in reverse order to assign (sequential - has dependencies) */
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
     *
     * Parallelization: Each row's reduction computation is independent.
     * Column price updates don't conflict (each singly-matched row has unique j1).
     * Free list building done separately to avoid synchronization.
     * ======================================================================== */
    int num_free = 0;

    if (use_parallel && n >= LAP_PARALLEL_THRESHOLD) {
        /* Parallel version: compute reductions in parallel */
        /* First, compute all reductions (no dependencies between rows) */
        #pragma omp parallel for schedule(static)
        for (int ii = 0; ii < n; ii++) {
            if (matches[ii] == 1) {
                int j1 = row_assign[ii];
                double min_reduced = DBL_MAX;
                const double *row_costs = &work_cost[ii * n];

                /* Find minimum reduced cost for columns other than j1 */
                for (int jj = 0; jj < n; jj++) {
                    if (jj != j1) {
                        double reduced = row_costs[jj] - col_price[jj];
                        if (reduced < min_reduced) {
                            min_reduced = reduced;
                        }
                    }
                }

                /* Each singly-matched row has unique j1, so no race */
                col_price[j1] -= min_reduced;
            }
        }

        /* Build free list sequentially (fast, just counting) */
        for (i = 0; i < n; i++) {
            if (matches[i] == 0) {
                free_rows[num_free++] = i;
                in_free_list[i] = 1;
            }
        }
    } else {
        /* Sequential version */
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
    }

    /* ========================================================================
     * PHASE 3: Augmenting Row Reduction (Auction Phase)
     *
     * Two modes:
     * - Standard: Run twice for better convergence (original JVC)
     * - ε-scaling: Add epsilon to price adjustments for guaranteed progress
     *
     * For large n, we pre-compute reduced costs into dist[] array, then
     * use SIMD to find minimum. This trades memory bandwidth for SIMD gains.
     * ======================================================================== */
    int use_simd_auction = use_simd;

    /* Compute epsilon for ε-scaling mode */
    double epsilon = 0.0;
    if (lap_epsilon_scaling_enabled) {
        /* Find cost range to set epsilon */
        double max_cost = 0.0;
        for (i = 0; i < n; i++) {
            for (j = 0; j < n; j++) {
                double c = work_cost[i * n + j];
                if (c < RALPH_LAP_INFINITY * 0.5 && fabs(c) > max_cost) {
                    max_cost = fabs(c);
                }
            }
        }
        /* Small epsilon: just enough to break ties without affecting optimality
         * Use n² in denominator to make epsilon very small */
        epsilon = (max_cost > 0) ? max_cost / ((double)n * n * lap_epsilon_factor) : 1.0 / ((double)n * n * lap_epsilon_factor);
    }

    for (int loop = 0; loop < 2 && num_free > 0; loop++) {
        int k_free = 0;
        int max_iter = n * n;
        int iter = 0;

        while (k_free < num_free && iter < max_iter) {
            iter++;
            i = free_rows[k_free++];

            double u1 = DBL_MAX;
            double u2 = DBL_MAX;
            int j1 = -1, j2 = -1;
            const double *row_costs = &work_cost[i * n];

            if (use_simd_auction) {
                #pragma omp simd
                for (j = 0; j < n; j++) {
                    dist[j] = row_costs[j] - col_price[j];
                }

                double vmin = dist[0];
                #pragma omp simd reduction(min:vmin)
                for (j = 1; j < n; j++) {
                    if (dist[j] < vmin) vmin = dist[j];
                }

                for (j = 0; j < n; j++) {
                    double v = dist[j];
                    if (v <= vmin + RALPH_LAP_TOLERANCE && j1 < 0) {
                        u1 = v;
                        j1 = j;
                    } else if (v < u2) {
                        u2 = v;
                        j2 = j;
                    }
                }
            } else {
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
            }

            if (j1 < 0) {
                status = RALPH_LAP_INFEASIBLE;
                goto cleanup;
            }

            row_price[i] = u1;

            /* Price adjustment with optional epsilon for tie-breaking */
            if (u1 < u2 - RALPH_LAP_TOLERANCE - epsilon) {
                col_price[j1] = col_price[j1] - u2 + u1;
            } else if (col_assign[j1] >= 0 && j2 >= 0 && col_assign[j2] < 0) {
                /* j1 is assigned, j2 is free and within tolerance - use j2 */
                j1 = j2;
            } else if (lap_epsilon_scaling_enabled && col_assign[j1] >= 0) {
                /* ε-scaling: adjust price even in near-tie case */
                col_price[j1] = col_price[j1] - epsilon;
            }

            if (j1 < 0) {
                status = RALPH_LAP_INFEASIBLE;
                goto cleanup;
            }

            int prev_row = col_assign[j1];
            if (prev_row >= 0) {
                row_assign[prev_row] = RALPH_LAP_UNASSIGNED;
            }
            row_assign[i] = j1;
            col_assign[j1] = i;

            if (prev_row >= 0 && !in_free_list[prev_row]) {
                in_free_list[prev_row] = 1;
                if (loop == 1) {
                    free_rows[--k_free] = prev_row;
                } else {
                    free_rows[num_free++] = prev_row;
                }
            }
        }

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
     * Uses binary heap for O(n log n) per augmentation when n is large.
     * ======================================================================== */
    for (int f = 0; f < num_free; f++) {
        int free_row = free_rows[f];
        const double *row_costs = &work_cost[free_row * n];
        int end_col = -1;
        double min_dist = 0.0;

        /* Initialize distances and predecessors */
        #pragma omp simd
        for (j = 0; j < n; j++) {
            dist[j] = row_costs[j] - col_price[j];
            pred[j] = free_row;
        }

        if (use_heap) {
            /* ============================================================
             * HEAP-BASED DIJKSTRA (for large n)
             * O(n log n) per augmentation
             * ============================================================ */
            int heap_size = n;
            int num_scanned = 0;

            /* Build initial heap */
            for (j = 0; j < n; j++) {
                heap[j] = j;
                heap_pos[j] = j;
            }

            /* Heapify (build min-heap) */
            for (j = n / 2 - 1; j >= 0; j--) {
                heap_sift_down(heap, heap_pos, dist, j, n);
            }

            /* Process columns in order of increasing distance */
            while (heap_size > 0 && end_col < 0) {
                /* Extract minimum */
                j = heap_pop(heap, heap_pos, dist, &heap_size);
                min_dist = dist[j];
                col_list[num_scanned++] = j;

                /* Check if this is an unassigned column */
                if (col_assign[j] == RALPH_LAP_UNASSIGNED) {
                    end_col = j;
                    break;
                }

                /* Relax edges from assigned row */
                int assigned_row = col_assign[j];
                double h = work_cost[assigned_row * n + j] - col_price[j] - min_dist;
                const double *assigned_row_costs = &work_cost[assigned_row * n];

                for (k = 0; k < heap_size; k++) {
                    int jj = heap[k];
                    double new_dist = assigned_row_costs[jj] - col_price[jj] - h;

                    if (new_dist < dist[jj]) {
                        pred[jj] = assigned_row;
                        dist[jj] = new_dist;
                        heap_decrease_key(heap, heap_pos, dist, jj);
                    }
                }
            }

            /* Update column prices for all scanned columns */
            for (k = 0; k < num_scanned; k++) {
                j = col_list[k];
                col_price[j] += dist[j] - min_dist;
            }
        } else {
            /* ============================================================
             * LINEAR SCAN DIJKSTRA (for small n)
             * Lower constant factor for small problems
             * ============================================================ */
            int low = 0;    /* Start of scanned columns with min distance */
            int up = 0;     /* Start of columns not yet scanned */
            int last = -1;  /* Last scanned column */

            /* Initialize column list */
            for (j = 0; j < n; j++) {
                col_list[j] = j;
            }

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
    /* No memory to free - workspace manages all allocations */
    (void)heap;      /* Suppress unused warning when not using heap */
    (void)heap_pos;

    return status;
}

/* ============================================================================
 * Public API Functions
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
        if (u) u[0] = cost[0];
        if (v) v[0] = 0.0;
        if (total_cost) *total_cost = cost[0];
        return RALPH_LAP_SUCCESS;
    }

    /* Create temporary workspace */
    RalphLapWorkspace *ws = ralph_lap_workspace_create(n);
    if (!ws) {
        return RALPH_LAP_MEMORY_ERROR;
    }

    /* Solve using workspace */
    RalphLapStatus status = lap_solve_internal(n, cost, objective, row_sol,
                                                col_sol, u, v, total_cost, ws);

    /* Free temporary workspace */
    ralph_lap_workspace_free(ws);

    return status;
}

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
) {
    if (n <= 0 || cost == NULL || row_sol == NULL || ws == NULL) {
        return RALPH_LAP_INVALID_INPUT;
    }

    /* Check workspace capacity */
    if (n > ws->max_n) {
        return RALPH_LAP_INVALID_INPUT;
    }

    /* Handle trivial case */
    if (n == 1) {
        row_sol[0] = 0;
        if (col_sol) col_sol[0] = 0;
        if (u) u[0] = cost[0];
        if (v) v[0] = 0.0;
        if (total_cost) *total_cost = cost[0];
        return RALPH_LAP_SUCCESS;
    }

    return lap_solve_internal(n, cost, objective, row_sol, col_sol, u, v, total_cost, ws);
}

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
) {
    if (n <= 0 || cost == NULL || row_sol == NULL || ws == NULL) {
        return RALPH_LAP_INVALID_INPUT;
    }

    if (n > ws->max_n) {
        return RALPH_LAP_INVALID_INPUT;
    }

    /* Handle trivial case */
    if (n == 1) {
        row_sol[0] = 0;
        if (col_sol) col_sol[0] = 0;
        if (u) u[0] = cost[0];
        if (v) v[0] = 0.0;
        if (total_cost) *total_cost = cost[0];
        ws->warm_start_valid = 0;  /* No benefit to warm start for n=1 */
        return RALPH_LAP_SUCCESS;
    }

    RalphLapStatus status;

    /* Check if warm start is applicable */
    int use_warm_start = ws->warm_start_valid && ws->warm_start_n == n;

    if (use_warm_start) {
        /* ================================================================
         * WARM START SOLVE
         *
         * Use saved dual variables to initialize the solver.
         * This skips most of Phase 1 work.
         * ================================================================ */
        int i, j, k;
        int num_free = 0;  /* Declared here to be visible across goto labels */

        double *work_cost = ws->work_cost;
        double *col_price = ws->col_price;
        double *row_price = ws->row_price;
        double *dist = ws->dist;
        int *row_assign = ws->row_assign;
        int *col_assign = ws->col_assign;
        int *matches = ws->matches;
        int *free_rows = ws->free_rows;
        int *pred = ws->pred;
        int *col_list = ws->col_list;
        int *in_free_list = ws->in_free_list;
        int *heap = ws->heap;
        int *heap_pos = ws->heap_pos;

        int use_heap = (n >= LAP_HEAP_THRESHOLD);

        /* Copy cost matrix, negate if maximizing */
        if (objective == RALPH_LAP_MAXIMIZE) {
            for (i = 0; i < n * n; i++) {
                if (is_infinite(cost[i])) {
                    work_cost[i] = RALPH_LAP_INFINITY;
                } else {
                    work_cost[i] = -cost[i];
                }
            }
        } else {
            memcpy(work_cost, cost, n * n * sizeof(double));
        }

        /* Initialize from warm start data */
        memcpy(col_price, ws->warm_v, n * sizeof(double));
        memset(row_price, 0, n * sizeof(double));
        memset(matches, 0, n * sizeof(int));
        memset(in_free_list, 0, n * sizeof(int));

        /* Check if we can use the saved assignment */
        int valid_assignment = 1;
        for (i = 0; i < n && valid_assignment; i++) {
            int j_saved = ws->warm_row_sol[i];
            if (j_saved < 0 || j_saved >= n) {
                valid_assignment = 0;
            } else {
                /* Check if the edge is still valid (not infinite cost) */
                if (is_infinite(work_cost[i * n + j_saved])) {
                    valid_assignment = 0;
                }
            }
        }

        if (valid_assignment) {
            /* Use saved assignment, find rows that might need repair */
            memcpy(row_assign, ws->warm_row_sol, n * sizeof(int));
            memcpy(col_assign, ws->warm_col_sol, n * sizeof(int));

            /* Check optimality for each row by comparing reduced costs */
            num_free = 0;
            for (i = 0; i < n; i++) {
                int j_cur = row_assign[i];
                double cur_reduced = work_cost[i * n + j_cur] - col_price[j_cur];

                /* Find the minimum reduced cost for this row (SIMD) */
                double min_reduced = DBL_MAX;
                const double *row_cost = &work_cost[i * n];
                #pragma omp simd reduction(min:min_reduced)
                for (j = 0; j < n; j++) {
                    double reduced = row_cost[j] - col_price[j];
                    if (reduced < min_reduced) {
                        min_reduced = reduced;
                    }
                }

                /* If current assignment is not optimal, mark row as free */
                if (cur_reduced > min_reduced + RALPH_LAP_TOLERANCE) {
                    /* Unassign this row */
                    col_assign[j_cur] = RALPH_LAP_UNASSIGNED;
                    row_assign[i] = RALPH_LAP_UNASSIGNED;
                    free_rows[num_free++] = i;
                    in_free_list[i] = 1;
                }
            }

            /* If few rows need repair, go directly to Phase 4 */
            if (num_free > 0 && num_free <= n / 4) {
                /* Skip to Phase 4 (Dijkstra augmentation) */
                goto warm_phase4;
            } else if (num_free > 0) {
                /* Too many free rows, run Phase 3 first */
                goto warm_phase3;
            }
            /* else: all rows optimal, skip to output */
            goto warm_output;
        }

        /* No valid saved assignment - initialize from scratch but with warm prices */
        for (i = 0; i < n; i++) {
            row_assign[i] = RALPH_LAP_UNASSIGNED;
            col_assign[i] = RALPH_LAP_UNASSIGNED;
        }

        /* Run a modified Phase 1 that uses warm prices as starting point */
        /* Find minimum reduced cost for each column */
        int *col_min_row = pred;
        for (j = 0; j < n; j++) col_min_row[j] = -1;

        #pragma omp simd
        for (j = 0; j < n; j++) {
            col_price[j] = ws->warm_v[j];  /* Use warm prices as base */
        }

        /* Find column minimums with warm prices */
        for (i = 0; i < n; i++) {
            const double *row_costs = &work_cost[i * n];
            for (j = 0; j < n; j++) {
                double reduced = row_costs[j] - col_price[j];
                if (col_min_row[j] < 0 || reduced < -RALPH_LAP_TOLERANCE) {
                    /* Update if this row has better reduced cost */
                    if (col_min_row[j] < 0) {
                        col_min_row[j] = i;
                    } else {
                        double prev_reduced = row_costs[col_min_row[j]] - col_price[j];
                        if (reduced < prev_reduced - RALPH_LAP_TOLERANCE) {
                            col_min_row[j] = i;
                        }
                    }
                }
            }
        }

        /* Re-run column reduction to get proper initial prices */
        for (j = 0; j < n; j++) col_min_row[j] = -1;
        memset(col_price, 0, n * sizeof(double));

        /* Initialize with first row */
        for (j = 0; j < n; j++) {
            col_price[j] = work_cost[j];
            col_min_row[j] = 0;
        }

        /* Find true column minimums */
        for (i = 1; i < n; i++) {
            const double *row_costs = &work_cost[i * n];
            for (j = 0; j < n; j++) {
                if (row_costs[j] < col_price[j]) {
                    col_price[j] = row_costs[j];
                    col_min_row[j] = i;
                }
            }
        }

        /* Assign based on column minimums */
        for (j = n - 1; j >= 0; j--) {
            int min_row = col_min_row[j];
            if (min_row < 0) continue;
            matches[min_row]++;
            if (matches[min_row] == 1) {
                row_assign[min_row] = j;
                col_assign[j] = min_row;
            } else {
                int cur_col = row_assign[min_row];
                if (cur_col >= 0 && col_price[j] < col_price[cur_col]) {
                    col_assign[cur_col] = RALPH_LAP_UNASSIGNED;
                    row_assign[min_row] = j;
                    col_assign[j] = min_row;
                } else {
                    col_assign[j] = RALPH_LAP_UNASSIGNED;
                }
            }
        }

        /* Phase 2: Reduction transfer */
        num_free = 0;
        for (i = 0; i < n; i++) {
            if (row_assign[i] == RALPH_LAP_UNASSIGNED) {
                free_rows[num_free++] = i;
                in_free_list[i] = 1;
            } else if (matches[i] == 1) {
                int j1 = row_assign[i];
                double min_reduced = DBL_MAX;
                const double *row_costs = &work_cost[i * n];
                for (j = 0; j < n; j++) {
                    if (j != j1) {
                        double reduced = row_costs[j] - col_price[j];
                        if (reduced < min_reduced) min_reduced = reduced;
                    }
                }
                col_price[j1] -= min_reduced;
            }
        }

    warm_phase3:
        /* Phase 3: Auction */
        for (int loop = 0; loop < 2 && num_free > 0; loop++) {
            int k_free = 0;
            int max_iter = n * n;
            int iter = 0;

            while (k_free < num_free && iter < max_iter) {
                iter++;
                i = free_rows[k_free++];

                double u1 = DBL_MAX, u2 = DBL_MAX;
                int j1 = -1, j2 = -1;
                const double *row_costs = &work_cost[i * n];

                for (j = 0; j < n; j++) {
                    double reduced = row_costs[j] - col_price[j];
                    if (reduced < u1) {
                        u2 = u1; j2 = j1;
                        u1 = reduced; j1 = j;
                    } else if (reduced < u2) {
                        u2 = reduced; j2 = j;
                    }
                }

                if (j1 < 0) {
                    status = RALPH_LAP_INFEASIBLE;
                    goto warm_cleanup;
                }

                row_price[i] = u1;

                if (u1 < u2 - RALPH_LAP_TOLERANCE) {
                    col_price[j1] = col_price[j1] - u2 + u1;
                } else if (col_assign[j1] >= 0 && j2 >= 0 && col_assign[j2] < 0) {
                    j1 = j2;
                }

                int prev_row = col_assign[j1];
                if (prev_row >= 0) {
                    row_assign[prev_row] = RALPH_LAP_UNASSIGNED;
                }
                row_assign[i] = j1;
                col_assign[j1] = i;

                if (prev_row >= 0 && !in_free_list[prev_row]) {
                    in_free_list[prev_row] = 1;
                    if (loop == 1) {
                        free_rows[--k_free] = prev_row;
                    } else {
                        free_rows[num_free++] = prev_row;
                    }
                }
            }

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

    warm_phase4:
        /* Phase 4: Dijkstra augmentation */
        for (int f = 0; f < num_free; f++) {
            int free_row = free_rows[f];
            const double *row_costs = &work_cost[free_row * n];
            int end_col = -1;
            double min_dist = 0.0;

            #pragma omp simd
            for (j = 0; j < n; j++) {
                dist[j] = row_costs[j] - col_price[j];
                pred[j] = free_row;
            }

            if (use_heap) {
                int heap_size = n;
                for (j = 0; j < n; j++) {
                    heap[j] = j;
                    heap_pos[j] = j;
                }
                for (j = n / 2 - 1; j >= 0; j--) {
                    heap_sift_down(heap, heap_pos, dist, j, n);
                }

                int num_scanned = 0;
                while (heap_size > 0 && end_col < 0) {
                    j = heap_pop(heap, heap_pos, dist, &heap_size);
                    min_dist = dist[j];
                    col_list[num_scanned++] = j;

                    if (col_assign[j] == RALPH_LAP_UNASSIGNED) {
                        end_col = j;
                        break;
                    }

                    int assigned_row = col_assign[j];
                    double h = work_cost[assigned_row * n + j] - col_price[j] - min_dist;
                    const double *assigned_row_costs = &work_cost[assigned_row * n];

                    for (k = 0; k < heap_size; k++) {
                        int jj = heap[k];
                        double new_dist = assigned_row_costs[jj] - col_price[jj] - h;
                        if (new_dist < dist[jj]) {
                            pred[jj] = assigned_row;
                            dist[jj] = new_dist;
                            heap_decrease_key(heap, heap_pos, dist, jj);
                        }
                    }
                }

                for (k = 0; k < num_scanned; k++) {
                    j = col_list[k];
                    col_price[j] += dist[j] - min_dist;
                }
            } else {
                int low = 0, up = 0, last = -1;
                for (j = 0; j < n; j++) col_list[j] = j;

                while (end_col < 0) {
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
                                col_list[k] = col_list[up];
                                col_list[up] = j;
                                up++;
                            }
                        }
                        for (k = low; k < up; k++) {
                            if (col_assign[col_list[k]] == RALPH_LAP_UNASSIGNED) {
                                end_col = col_list[k];
                                break;
                            }
                        }
                    }

                    if (end_col < 0) {
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
                                    if (col_assign[jj] == RALPH_LAP_UNASSIGNED) {
                                        end_col = jj;
                                        break;
                                    } else {
                                        col_list[k] = col_list[up];
                                        col_list[up] = jj;
                                        up++;
                                    }
                                }
                            }
                        }
                    }
                }

                for (k = 0; k <= last; k++) {
                    j = col_list[k];
                    col_price[j] += dist[j] - min_dist;
                }
            }

            /* Trace back and flip assignments */
            int cur_row;
            do {
                cur_row = pred[end_col];
                col_assign[end_col] = cur_row;
                int prev_col = row_assign[cur_row];
                row_assign[cur_row] = end_col;
                end_col = prev_col;
            } while (cur_row != free_row);
        }

    warm_output:
        /* Output results */
        memcpy(row_sol, row_assign, n * sizeof(int));

        if (col_sol) {
            memcpy(col_sol, col_assign, n * sizeof(int));
        }

        if (u) {
            for (i = 0; i < n; i++) {
                j = row_assign[i];
                u[i] = work_cost[i * n + j] - col_price[j];
            }
            if (objective == RALPH_LAP_MAXIMIZE) {
                for (i = 0; i < n; i++) u[i] = -u[i];
            }
        }

        if (v) {
            memcpy(v, col_price, n * sizeof(double));
            if (objective == RALPH_LAP_MAXIMIZE) {
                for (j = 0; j < n; j++) v[j] = -v[j];
            }
        }

        if (total_cost) {
            double sum = 0.0;
            for (i = 0; i < n; i++) {
                int jj = row_assign[i];
                double c = cost[i * n + jj];
                if (c < RALPH_LAP_INFINITY * 0.5) sum += c;
            }
            *total_cost = sum;
        }

        status = RALPH_LAP_SUCCESS;

    warm_cleanup:
        (void)heap;
        (void)heap_pos;

    } else {
        /* No valid warm start - do full solve */
        status = lap_solve_internal(n, cost, objective, row_sol, col_sol, u, v, total_cost, ws);
    }

    /* Save for next warm start if requested */
    if (save_for_warm_start && status == RALPH_LAP_SUCCESS) {
        /* Save INTERNAL dual variables (for the possibly-negated cost matrix).
         * These can be directly used as starting values in the next warm start.
         * Note: ws->work_cost contains the (possibly negated) costs after solve,
         * and ws->col_price contains the internal column prices.
         */
        for (int i = 0; i < n; i++) {
            int j = row_sol[i];
            /* Compute internal u from complementary slackness: u[i] = c[i,j] - v[j] */
            ws->warm_u[i] = ws->work_cost[i * n + j] - ws->col_price[j];
        }
        memcpy(ws->warm_v, ws->col_price, n * sizeof(double));

        memcpy(ws->warm_row_sol, row_sol, n * sizeof(int));
        if (col_sol) {
            memcpy(ws->warm_col_sol, col_sol, n * sizeof(int));
        } else {
            /* Reconstruct col_sol */
            for (int j = 0; j < n; j++) ws->warm_col_sol[j] = RALPH_LAP_UNASSIGNED;
            for (int i = 0; i < n; i++) {
                int j = row_sol[i];
                if (j >= 0 && j < n) ws->warm_col_sol[j] = i;
            }
        }

        ws->warm_start_n = n;
        ws->warm_start_valid = 1;
    }

    return status;
}

/* ============================================================================
 * Rectangular LAP Solver
 * ============================================================================ */

RalphLapStatus ralph_lap_solve_rect(
    int m,
    int n,
    const double *cost,
    RalphLapObjective objective,
    int *row_sol,
    int *col_sol,
    double *total_cost
) {
    if (m <= 0 || n <= 0 || cost == NULL || row_sol == NULL) {
        return RALPH_LAP_INVALID_INPUT;
    }

    /* Square case - delegate to standard solver */
    if (m == n) {
        return ralph_lap_solve(n, cost, objective, row_sol, col_sol,
                               NULL, NULL, total_cost);
    }

    /* Determine padded dimension */
    int k = (m > n) ? m : n;

    /* Allocate padded cost matrix */
    double *padded_cost = (double *)malloc(k * k * sizeof(double));
    int *padded_row_sol = (int *)malloc(k * sizeof(int));
    int *padded_col_sol = col_sol ? (int *)malloc(k * sizeof(int)) : NULL;

    if (!padded_cost || !padded_row_sol || (col_sol && !padded_col_sol)) {
        free(padded_cost);
        free(padded_row_sol);
        free(padded_col_sol);
        return RALPH_LAP_MEMORY_ERROR;
    }

    /* Fill padded cost matrix
     * - Original costs in top-left m x n block
     * - Dummy costs (0) for padding to allow "unassigned" matches
     */
    for (int i = 0; i < k; i++) {
        for (int j = 0; j < k; j++) {
            if (i < m && j < n) {
                /* Original cost */
                padded_cost[i * k + j] = cost[i * n + j];
            } else {
                /* Dummy assignment - zero cost so it doesn't affect objective */
                padded_cost[i * k + j] = 0.0;
            }
        }
    }

    /* Solve padded square problem */
    RalphLapStatus status = ralph_lap_solve(k, padded_cost, objective,
                                             padded_row_sol, padded_col_sol,
                                             NULL, NULL, NULL);

    if (status != RALPH_LAP_SUCCESS) {
        free(padded_cost);
        free(padded_row_sol);
        free(padded_col_sol);
        return status;
    }

    /* Extract solution for original dimensions */
    /* row_sol[i] = assigned column, or -1 if assigned to dummy column */
    for (int i = 0; i < m; i++) {
        int j = padded_row_sol[i];
        row_sol[i] = (j < n) ? j : RALPH_LAP_UNASSIGNED;
    }

    /* col_sol[j] = assigned row, or -1 if assigned to dummy row */
    if (col_sol) {
        for (int j = 0; j < n; j++) {
            int i = padded_col_sol[j];
            col_sol[j] = (i < m) ? i : RALPH_LAP_UNASSIGNED;
        }
    }

    /* Compute total cost from actual assignments only */
    if (total_cost) {
        double sum = 0.0;
        for (int i = 0; i < m; i++) {
            int j = row_sol[i];
            if (j >= 0 && j < n) {
                double c = cost[i * n + j];
                if (c < RALPH_LAP_INFINITY * 0.5) {
                    sum += c;
                }
            }
        }
        *total_cost = sum;
    }

    free(padded_cost);
    free(padded_row_sol);
    free(padded_col_sol);

    return RALPH_LAP_SUCCESS;
}

/* ============================================================================
 * Sparse LAP Solver (Native Sparse JVC)
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

    /* Handle trivial case */
    if (n == 1) {
        if (row_ptr[1] - row_ptr[0] == 0) {
            return RALPH_LAP_INFEASIBLE;  /* No edges from row 0 */
        }
        row_sol[0] = col_idx[row_ptr[0]];
        if (col_sol) col_sol[row_sol[0]] = 0;
        if (total_cost) *total_cost = values[row_ptr[0]];
        return RALPH_LAP_SUCCESS;
    }

    /* For very sparse problems (density < 30%), use native sparse algorithm.
     * For denser problems, the dense algorithm is faster due to better cache access. */
    double density = (double)nnz / ((double)n * n);
    if (density > 0.3) {
        /* Convert to dense - faster for denser problems */
        double *cost = (double *)malloc(n * n * sizeof(double));
        if (!cost) return RALPH_LAP_MEMORY_ERROR;

        for (int i = 0; i < n * n; i++) cost[i] = RALPH_LAP_INFINITY;
        for (int i = 0; i < n; i++) {
            for (int k = row_ptr[i]; k < row_ptr[i + 1]; k++) {
                int j = col_idx[k];
                if (j >= 0 && j < n) cost[i * n + j] = values[k];
            }
        }

        RalphLapStatus status = ralph_lap_solve(n, cost, objective, row_sol, col_sol,
                                                 NULL, NULL, total_cost);
        free(cost);
        return status;
    }

    /* ========================================================================
     * Native Sparse JVC Algorithm
     * ======================================================================== */

    RalphLapStatus status = RALPH_LAP_SUCCESS;
    int i, j, k;

    /* Allocate working arrays */
    double *work_values = NULL;  /* Possibly negated costs */
    double *col_price = NULL;    /* Column dual variables */
    double *dist = NULL;         /* Shortest path distances */
    int *row_assign = NULL;      /* row_assign[i] = column assigned to row i */
    int *col_assign = NULL;      /* col_assign[j] = row assigned to column j */
    int *matches = NULL;         /* Number of times each row matches minimum */
    int *free_rows = NULL;       /* List of unassigned rows */
    int *pred = NULL;            /* Predecessor row in augmenting path */
    int *in_queue = NULL;        /* Whether column is in priority queue */
    int *in_free_list = NULL;    /* Track which rows are in free list */
    int *scanned = NULL;         /* Scanned columns in Dijkstra */

    work_values = (double *)malloc(nnz * sizeof(double));
    col_price = (double *)calloc(n, sizeof(double));
    dist = (double *)malloc(n * sizeof(double));
    row_assign = (int *)malloc(n * sizeof(int));
    col_assign = (int *)malloc(n * sizeof(int));
    matches = (int *)calloc(n, sizeof(int));
    free_rows = (int *)malloc(2 * n * sizeof(int));
    pred = (int *)malloc(n * sizeof(int));
    in_queue = (int *)calloc(n, sizeof(int));
    in_free_list = (int *)calloc(n, sizeof(int));
    scanned = (int *)malloc(n * sizeof(int));

    if (!work_values || !col_price || !dist || !row_assign || !col_assign ||
        !matches || !free_rows || !pred || !in_queue || !in_free_list || !scanned) {
        status = RALPH_LAP_MEMORY_ERROR;
        goto sparse_cleanup;
    }

    /* Initialize assignments (SIMD-friendly) */
    #pragma omp simd
    for (i = 0; i < n; i++) {
        row_assign[i] = RALPH_LAP_UNASSIGNED;
        col_assign[i] = RALPH_LAP_UNASSIGNED;
        dist[i] = RALPH_LAP_INFINITY;
    }

    /* Copy values, negate if maximizing (SIMD-friendly) */
    if (objective == RALPH_LAP_MAXIMIZE) {
        #pragma omp simd
        for (k = 0; k < nnz; k++) {
            work_values[k] = -values[k];
        }
    } else {
        memcpy(work_values, values, nnz * sizeof(double));
    }

    /* ========================================================================
     * PHASE 1: Column Reduction (Sparse)
     * For each column, find minimum cost among edges to that column.
     * This matches the dense algorithm's Phase 1.
     * ======================================================================== */

    /* First pass: find minimum cost edge to each column and track which row */
    int *col_min_row = pred;  /* Reuse pred array: col_min_row[j] = row with min cost to j */
    #pragma omp simd
    for (j = 0; j < n; j++) {
        col_min_row[j] = -1;
    }

    /* Scan all edges to find column minimums */
    for (i = 0; i < n; i++) {
        if (row_ptr[i + 1] == row_ptr[i]) {
            /* Row has no edges - infeasible */
            status = RALPH_LAP_INFEASIBLE;
            goto sparse_cleanup;
        }

        for (k = row_ptr[i]; k < row_ptr[i + 1]; k++) {
            j = col_idx[k];
            double cost = work_values[k];

            if (col_min_row[j] < 0 || cost < col_price[j]) {
                col_price[j] = cost;
                col_min_row[j] = i;
            }
        }
    }

    /* Check that all columns are reachable */
    for (j = 0; j < n; j++) {
        if (col_min_row[j] < 0) {
            /* Column has no incoming edges - infeasible */
            status = RALPH_LAP_INFEASIBLE;
            goto sparse_cleanup;
        }
    }

    /* Process columns in reverse order (matching dense algorithm) */
    for (j = n - 1; j >= 0; j--) {
        int min_row = col_min_row[j];
        matches[min_row]++;

        if (matches[min_row] == 1) {
            /* First time this row is matched - assign it */
            row_assign[min_row] = j;
            col_assign[j] = min_row;
        } else {
            /* Row already matched to another column - check if this is better */
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
     * PHASE 2: Reduction Transfer (Sparse)
     * ======================================================================== */
    int num_free = 0;

    for (i = 0; i < n; i++) {
        if (row_assign[i] == RALPH_LAP_UNASSIGNED) {
            free_rows[num_free++] = i;
            in_free_list[i] = 1;
        } else if (matches[i] == 1) {
            /* Row matched exactly once - transfer reduction */
            int j1 = row_assign[i];
            double min_reduced = RALPH_LAP_INFINITY;

            for (k = row_ptr[i]; k < row_ptr[i + 1]; k++) {
                j = col_idx[k];
                if (j != j1) {
                    double reduced = work_values[k] - col_price[j];
                    if (reduced < min_reduced) {
                        min_reduced = reduced;
                    }
                }
            }

            if (min_reduced < RALPH_LAP_INFINITY) {
                col_price[j1] -= min_reduced;
            }
        }
    }

    /* ========================================================================
     * PHASE 3: Augmenting Row Reduction (Sparse Auction)
     * ======================================================================== */
    for (int loop = 0; loop < 2 && num_free > 0; loop++) {
        int k_free = 0;
        int max_iter = n * n;
        int iter = 0;

        while (k_free < num_free && iter < max_iter) {
            iter++;
            i = free_rows[k_free++];

            /* Find min and second-min reduced costs among this row's edges */
            double u1 = RALPH_LAP_INFINITY, u2 = RALPH_LAP_INFINITY;
            int j1 = -1, j2 = -1;

            for (k = row_ptr[i]; k < row_ptr[i + 1]; k++) {
                j = col_idx[k];
                double reduced = work_values[k] - col_price[j];
                if (reduced < u1) {
                    u2 = u1; j2 = j1;
                    u1 = reduced; j1 = j;
                } else if (reduced < u2) {
                    u2 = reduced; j2 = j;
                }
            }

            if (j1 < 0) {
                status = RALPH_LAP_INFEASIBLE;
                goto sparse_cleanup;
            }

            /* Adjust column price */
            if (u1 < u2 - RALPH_LAP_TOLERANCE) {
                col_price[j1] = col_price[j1] - u2 + u1;
            } else if (col_assign[j1] >= 0 && j2 >= 0 && col_assign[j2] < 0) {
                j1 = j2;
            }

            /* Assign */
            int prev_row = col_assign[j1];
            if (prev_row >= 0) {
                row_assign[prev_row] = RALPH_LAP_UNASSIGNED;
            }
            row_assign[i] = j1;
            col_assign[j1] = i;

            if (prev_row >= 0 && !in_free_list[prev_row]) {
                in_free_list[prev_row] = 1;
                if (loop == 1) {
                    free_rows[--k_free] = prev_row;
                } else {
                    free_rows[num_free++] = prev_row;
                }
            }
        }

        /* Rebuild free list */
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
     * PHASE 4: Sparse Dijkstra Augmentation
     * ======================================================================== */
    for (int f = 0; f < num_free; f++) {
        int free_row = free_rows[f];

        /* Initialize distances from free_row's edges (SIMD-friendly) */
        #pragma omp simd
        for (j = 0; j < n; j++) {
            dist[j] = RALPH_LAP_INFINITY;
            pred[j] = -1;
            in_queue[j] = 0;
        }

        for (k = row_ptr[free_row]; k < row_ptr[free_row + 1]; k++) {
            j = col_idx[k];
            dist[j] = work_values[k] - col_price[j];
            pred[j] = free_row;
            in_queue[j] = 1;
        }

        int end_col = -1;
        double min_dist = 0.0;
        int num_scanned = 0;

        /* Dijkstra's algorithm
         * in_queue states: 0 = not reached, 1 = in queue, 2 = finalized (scanned)
         */
        while (end_col < 0) {
            /* Find minimum distance column among those in queue
             * Two-pass approach for SIMD: first find min, then find index */
            min_dist = RALPH_LAP_INFINITY;
            int min_col = -1;

            /* Pass 1: Find minimum distance using SIMD reduction */
            #pragma omp simd reduction(min:min_dist)
            for (j = 0; j < n; j++) {
                double d = (in_queue[j] == 1) ? dist[j] : RALPH_LAP_INFINITY;
                if (d < min_dist) min_dist = d;
            }

            /* Pass 2: Find column with that minimum distance */
            for (j = 0; j < n; j++) {
                if (in_queue[j] == 1 && dist[j] <= min_dist + RALPH_LAP_TOLERANCE) {
                    min_col = j;
                    break;
                }
            }

            if (min_col < 0) {
                /* No more reachable columns - infeasible */
                status = RALPH_LAP_INFEASIBLE;
                goto sparse_cleanup;
            }

            /* Mark as finalized (scanned) */
            in_queue[min_col] = 2;
            scanned[num_scanned++] = min_col;

            if (col_assign[min_col] == RALPH_LAP_UNASSIGNED) {
                end_col = min_col;
                break;
            }

            /* Relax edges from assigned row */
            int assigned_row = col_assign[min_col];
            double h = 0.0;

            /* Find the edge cost from assigned_row to min_col */
            for (k = row_ptr[assigned_row]; k < row_ptr[assigned_row + 1]; k++) {
                if (col_idx[k] == min_col) {
                    h = work_values[k] - col_price[min_col] - min_dist;
                    break;
                }
            }

            /* Relax all edges from assigned_row */
            for (k = row_ptr[assigned_row]; k < row_ptr[assigned_row + 1]; k++) {
                int jj = col_idx[k];
                double new_dist = work_values[k] - col_price[jj] - h;

                /* Only update if not yet finalized and distance improves */
                if (new_dist < dist[jj] && in_queue[jj] != 2) {
                    dist[jj] = new_dist;
                    pred[jj] = assigned_row;
                    in_queue[jj] = 1;  /* Add to queue (or keep in queue) */
                }
            }
        }

        /* Update column prices for scanned columns */
        /* Note: Can't easily SIMD this due to indirect indexing via scanned[] */
        for (k = 0; k < num_scanned; k++) {
            j = scanned[k];
            col_price[j] += dist[j] - min_dist;
        }

        /* Trace back and flip assignments */
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
     * Output results
     * ======================================================================== */
    memcpy(row_sol, row_assign, n * sizeof(int));

    if (col_sol) {
        memcpy(col_sol, col_assign, n * sizeof(int));
    }

    if (total_cost) {
        double sum = 0.0;
        for (i = 0; i < n; i++) {
            j = row_assign[i];
            /* Find the cost of edge (i, j) */
            for (k = row_ptr[i]; k < row_ptr[i + 1]; k++) {
                if (col_idx[k] == j) {
                    sum += values[k];  /* Use original values, not negated */
                    break;
                }
            }
        }
        *total_cost = sum;
    }

sparse_cleanup:
    free(work_values);
    free(col_price);
    free(dist);
    free(row_assign);
    free(col_assign);
    free(matches);
    free(free_rows);
    free(pred);
    free(in_queue);
    free(in_free_list);
    free(scanned);

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
 * Callback-Based LAP Solver (O(n) memory)
 * ============================================================================ */

/*
 * Helper macro to get cost with objective handling.
 * For maximize, we negate non-infinite costs.
 */
#define GET_COST_CALLBACK(i, j, cost_fn, user_data, is_maximize) \
    ({ \
        double _c = (cost_fn)((i), (j), (user_data)); \
        (is_infinite(_c)) ? RALPH_LAP_INFINITY : \
        ((is_maximize) ? -_c : _c); \
    })

/*
 * Internal callback-based JVC solver.
 * Uses O(n) memory instead of O(n²) by computing costs on-demand.
 */
static RalphLapStatus lap_solve_callback_internal(
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
) {
    RalphLapStatus status = RALPH_LAP_SUCCESS;
    int i, j, k;
    int is_maximize = (objective == RALPH_LAP_MAXIMIZE);

    /* Use workspace arrays (but NOT work_cost - that's the whole point) */
    double *col_price = ws->col_price;
    double *row_price = ws->row_price;
    double *dist = ws->dist;
    int *row_assign = ws->row_assign;
    int *col_assign = ws->col_assign;
    int *matches = ws->matches;
    int *free_rows = ws->free_rows;
    int *pred = ws->pred;
    int *col_list = ws->col_list;
    int *in_free_list = ws->in_free_list;
    int *heap = ws->heap;
    int *heap_pos = ws->heap_pos;

    int use_heap = (n >= LAP_HEAP_THRESHOLD);

    /* Zero-initialize arrays */
    memset(col_price, 0, n * sizeof(double));
    memset(row_price, 0, n * sizeof(double));
    memset(matches, 0, n * sizeof(int));
    memset(in_free_list, 0, n * sizeof(int));

    /* Initialize assignments to unassigned */
    for (i = 0; i < n; i++) {
        row_assign[i] = RALPH_LAP_UNASSIGNED;
        col_assign[i] = RALPH_LAP_UNASSIGNED;
    }

    /* ========================================================================
     * PHASE 1: Column Reduction
     * For each column, find minimum cost and use as column price.
     * ======================================================================== */

    /* Temporary array to track min row per column */
    int *col_min_row = pred;  /* Reuse pred array temporarily */

    /* Initialize with first row */
    for (j = 0; j < n; j++) {
        col_price[j] = GET_COST_CALLBACK(0, j, cost_fn, user_data, is_maximize);
        col_min_row[j] = 0;
    }

    /* Process remaining rows */
    for (i = 1; i < n; i++) {
        for (j = 0; j < n; j++) {
            double c = GET_COST_CALLBACK(i, j, cost_fn, user_data, is_maximize);
            if (c < col_price[j]) {
                col_price[j] = c;
                col_min_row[j] = i;
            }
        }
    }

    /* Process columns in reverse order to assign */
    for (j = n - 1; j >= 0; j--) {
        int min_row = col_min_row[j];
        matches[min_row]++;

        if (matches[min_row] == 1) {
            /* First time this row is matched - assign it */
            row_assign[min_row] = j;
            col_assign[j] = min_row;
        } else {
            /* Row already matched to another column */
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

            /* Find minimum reduced cost for columns other than j1 */
            for (j = 0; j < n; j++) {
                if (j != j1) {
                    double c = GET_COST_CALLBACK(i, j, cost_fn, user_data, is_maximize);
                    double reduced = c - col_price[j];
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
     * ======================================================================== */
    double epsilon = 0.0;
    if (lap_epsilon_scaling_enabled) {
        /* Find max cost for epsilon initialization */
        double max_cost = 0.0;
        for (i = 0; i < n && i < 100; i++) {  /* Sample first 100 rows */
            for (j = 0; j < n; j++) {
                double c = GET_COST_CALLBACK(i, j, cost_fn, user_data, is_maximize);
                if (c < RALPH_LAP_INFINITY * 0.5 && fabs(c) > max_cost) {
                    max_cost = fabs(c);
                }
            }
        }
        epsilon = max_cost / ((double)n * n * lap_epsilon_factor);
        if (epsilon < RALPH_LAP_TOLERANCE) {
            epsilon = RALPH_LAP_TOLERANCE;
        }
    }

    int num_passes = lap_epsilon_scaling_enabled ? 4 : 2;

    for (int pass = 0; pass < num_passes && num_free > 0; pass++) {
        int next_free = 0;
        k = 0;

        while (k < num_free) {
            int i0 = free_rows[k++];

            /* Find two best columns for this row */
            double u1 = DBL_MAX, u2 = DBL_MAX;
            int j1 = -1, j2 = -1;

            for (j = 0; j < n; j++) {
                double c = GET_COST_CALLBACK(i0, j, cost_fn, user_data, is_maximize);
                double reduced = c - col_price[j];

                if (reduced < u1) {
                    u2 = u1; j2 = j1;
                    u1 = reduced; j1 = j;
                } else if (reduced < u2) {
                    u2 = reduced; j2 = j;
                }
            }

            if (j1 < 0) {
                status = RALPH_LAP_INFEASIBLE;
                goto cleanup;
            }

            int i1 = col_assign[j1];

            if (u1 < u2 - epsilon) {
                /* Unique minimum - assign and update price */
                col_price[j1] -= (u2 - u1) + epsilon;
            } else if (i1 >= 0 && j2 >= 0) {
                /* Tie - try second best */
                j1 = j2;
                i1 = col_assign[j1];
            }

            /* Update assignments */
            row_assign[i0] = j1;
            col_assign[j1] = i0;

            if (i1 >= 0) {
                /* Previous occupant becomes free */
                if (u1 < u2 - epsilon) {
                    free_rows[next_free++] = i1;
                } else {
                    free_rows[k++] = i1;
                }
                row_assign[i1] = RALPH_LAP_UNASSIGNED;
            }
        }

        num_free = next_free;

        /* Reduce epsilon for next pass */
        if (lap_epsilon_scaling_enabled) {
            epsilon /= lap_epsilon_factor;
            if (epsilon < RALPH_LAP_TOLERANCE) {
                epsilon = 0.0;
            }
        }
    }

    /* Rebuild free_rows list after Phase 3 */
    num_free = 0;
    for (i = 0; i < n; i++) {
        if (row_assign[i] == RALPH_LAP_UNASSIGNED) {
            free_rows[num_free++] = i;
        }
    }

    /* ========================================================================
     * PHASE 4: Augmentation via Dijkstra
     * ======================================================================== */
    for (k = 0; k < num_free; k++) {
        int i0 = free_rows[k];

        /* Initialize distances from row i0 */
        for (j = 0; j < n; j++) {
            double c = GET_COST_CALLBACK(i0, j, cost_fn, user_data, is_maximize);
            dist[j] = c - col_price[j];
            pred[j] = i0;
            col_list[j] = j;
        }

        int lo = 0, hi = n;
        int final_j = -1;

        if (use_heap) {
            /* Heap-based Dijkstra */
            int heap_size = n;
            int num_scanned = 0;
            double min_dist = 0.0;

            /* Build initial heap */
            for (j = 0; j < n; j++) {
                heap[j] = j;
                heap_pos[j] = j;
            }

            /* Heapify (build min-heap) */
            for (j = n / 2 - 1; j >= 0; j--) {
                heap_sift_down(heap, heap_pos, dist, j, n);
            }

            /* Process columns in order of increasing distance */
            while (heap_size > 0 && final_j < 0) {
                /* Extract minimum */
                j = heap_pop(heap, heap_pos, dist, &heap_size);
                min_dist = dist[j];
                col_list[num_scanned++] = j;

                /* Check if this is an unassigned column */
                if (col_assign[j] == RALPH_LAP_UNASSIGNED) {
                    final_j = j;
                    break;
                }

                /* Relax edges from assigned row */
                int assigned_row = col_assign[j];
                double c_aj = GET_COST_CALLBACK(assigned_row, j, cost_fn, user_data, is_maximize);
                double h = c_aj - col_price[j] - min_dist;

                for (k = 0; k < heap_size; k++) {
                    int jj = heap[k];
                    double c = GET_COST_CALLBACK(assigned_row, jj, cost_fn, user_data, is_maximize);
                    double new_dist = c - col_price[jj] - h;

                    if (new_dist < dist[jj]) {
                        pred[jj] = assigned_row;
                        dist[jj] = new_dist;
                        heap_decrease_key(heap, heap_pos, dist, jj);
                    }
                }
            }

            /* Update column prices for all scanned columns */
            for (k = 0; k < num_scanned; k++) {
                j = col_list[k];
                col_price[j] += dist[j] - min_dist;
            }

            /* Augment path */
            j = final_j;
            while (j >= 0) {
                int i_pred = pred[j];
                int prev_j = (i_pred == i0) ? -1 : row_assign[i_pred];

                col_assign[j] = i_pred;
                row_assign[i_pred] = j;

                j = prev_j;
            }

            continue;  /* Skip the non-heap code below */
        } else {
            /* Linear scan Dijkstra */
            while (lo < hi) {
                /* Find minimum in [lo, hi) */
                int j_min = col_list[lo];
                double d_min = dist[j_min];
                int min_idx = lo;

                for (int idx = lo + 1; idx < hi; idx++) {
                    int jj = col_list[idx];
                    if (dist[jj] < d_min) {
                        d_min = dist[jj];
                        j_min = jj;
                        min_idx = idx;
                    }
                }

                /* Swap minimum to position lo */
                col_list[min_idx] = col_list[lo];
                col_list[lo] = j_min;
                lo++;

                int assigned_row = col_assign[j_min];
                if (assigned_row < 0) {
                    final_j = j_min;
                    break;
                }

                /* Relax edges from assigned_row */
                double c_min = GET_COST_CALLBACK(assigned_row, j_min, cost_fn, user_data, is_maximize);
                double h = c_min - col_price[j_min] - d_min;

                for (int idx = lo; idx < hi; idx++) {
                    j = col_list[idx];
                    double c = GET_COST_CALLBACK(assigned_row, j, cost_fn, user_data, is_maximize);
                    double new_dist = c - col_price[j] - h;
                    if (new_dist < dist[j]) {
                        dist[j] = new_dist;
                        pred[j] = assigned_row;
                    }
                }
            }
        }

        if (final_j < 0) {
            status = RALPH_LAP_INFEASIBLE;
            goto cleanup;
        }

        /* Update column prices */
        double d_final = dist[final_j];
        for (int idx = 0; idx < lo; idx++) {
            j = col_list[idx];
            col_price[j] += dist[j] - d_final;
        }

        /* Augment path */
        j = final_j;
        while (j >= 0) {
            int i_pred = pred[j];
            int prev_j = (i_pred == i0) ? -1 : row_assign[i_pred];

            col_assign[j] = i_pred;
            row_assign[i_pred] = j;

            j = prev_j;
        }
    }

    /* ========================================================================
     * OUTPUT
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
            double c = GET_COST_CALLBACK(i, j, cost_fn, user_data, is_maximize);
            u[i] = c - col_price[j];
        }
        /* Negate if we were maximizing */
        if (is_maximize) {
            for (i = 0; i < n; i++) {
                u[i] = -u[i];
            }
        }
    }

    /* Copy column prices (v) if requested */
    if (v) {
        memcpy(v, col_price, n * sizeof(double));
        /* Negate if we were maximizing */
        if (is_maximize) {
            for (j = 0; j < n; j++) {
                v[j] = -v[j];
            }
        }
    }

    /* Compute total cost using original callback */
    if (total_cost) {
        double sum = 0.0;
        for (i = 0; i < n; i++) {
            int jj = row_assign[i];
            double c = cost_fn(i, jj, user_data);  /* Original cost, not negated */
            if (c < RALPH_LAP_INFINITY * 0.5) {
                sum += c;
            }
        }
        *total_cost = sum;
    }

cleanup:
    (void)heap;
    (void)heap_pos;

    return status;
}

#undef GET_COST_CALLBACK

/* Public API for callback-based solver */
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
) {
    if (n <= 0 || cost_fn == NULL || row_sol == NULL) {
        return RALPH_LAP_INVALID_INPUT;
    }

    /* Handle trivial case */
    if (n == 1) {
        row_sol[0] = 0;
        if (col_sol) col_sol[0] = 0;
        double c = cost_fn(0, 0, user_data);
        if (u) u[0] = c;
        if (v) v[0] = 0.0;
        if (total_cost) *total_cost = c;
        return RALPH_LAP_SUCCESS;
    }

    /* Create temporary workspace */
    RalphLapWorkspace *ws = ralph_lap_workspace_create(n);
    if (!ws) {
        return RALPH_LAP_MEMORY_ERROR;
    }

    /* Solve using workspace */
    RalphLapStatus status = lap_solve_callback_internal(
        n, cost_fn, user_data, objective, row_sol, col_sol, u, v, total_cost, ws);

    /* Free temporary workspace */
    ralph_lap_workspace_free(ws);

    return status;
}

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
) {
    if (n <= 0 || cost_fn == NULL || row_sol == NULL || ws == NULL) {
        return RALPH_LAP_INVALID_INPUT;
    }

    if (n > ws->max_n) {
        return RALPH_LAP_INVALID_INPUT;
    }

    /* Handle trivial case */
    if (n == 1) {
        row_sol[0] = 0;
        if (col_sol) col_sol[0] = 0;
        double c = cost_fn(0, 0, user_data);
        if (u) u[0] = c;
        if (v) v[0] = 0.0;
        if (total_cost) *total_cost = c;
        return RALPH_LAP_SUCCESS;
    }

    return lap_solve_callback_internal(
        n, cost_fn, user_data, objective, row_sol, col_sol, u, v, total_cost, ws);
}

/* ============================================================================
 * k-Best Assignments (Murty's Algorithm)
 * ============================================================================ */

/*
 * Murty's algorithm finds the k best solutions by systematically partitioning
 * the solution space. Starting from the optimal solution, it creates subproblems
 * by forbidding edges and maintaining a priority queue of candidate solutions.
 */

/* Node in Murty's priority queue */
typedef struct MurtyNode {
    double priority;       /* Heap ordering key (negated for maximize) */
    double cost;           /* Actual solution cost to return */
    int *row_sol;          /* Assignment: row_sol[i] = column for row i */
    int num_excluded;      /* Number of excluded edges */
    int *excluded_rows;    /* Row indices of excluded edges */
    int *excluded_cols;    /* Column indices of excluded edges */
    /* For warm start potential */
    double *u;             /* Row dual variables from this solve */
    double *v;             /* Column dual variables from this solve */
} MurtyNode;

/* Min-heap priority queue for Murty's algorithm */
typedef struct MurtyQueue {
    MurtyNode **nodes;     /* Array of pointers to nodes */
    int size;              /* Current number of nodes */
    int capacity;          /* Maximum capacity */
} MurtyQueue;

/* Create a Murty node */
static MurtyNode* murty_node_create(int n, int max_excluded) {
    MurtyNode *node = (MurtyNode *)malloc(sizeof(MurtyNode));
    if (!node) return NULL;

    node->row_sol = (int *)malloc(n * sizeof(int));
    node->excluded_rows = (int *)malloc(max_excluded * sizeof(int));
    node->excluded_cols = (int *)malloc(max_excluded * sizeof(int));
    node->u = (double *)malloc(n * sizeof(double));
    node->v = (double *)malloc(n * sizeof(double));

    if (!node->row_sol || !node->excluded_rows || !node->excluded_cols ||
        !node->u || !node->v) {
        free(node->row_sol);
        free(node->excluded_rows);
        free(node->excluded_cols);
        free(node->u);
        free(node->v);
        free(node);
        return NULL;
    }

    node->priority = 0.0;
    node->cost = 0.0;
    node->num_excluded = 0;
    return node;
}

/* Free a Murty node */
static void murty_node_free(MurtyNode *node) {
    if (node) {
        free(node->row_sol);
        free(node->excluded_rows);
        free(node->excluded_cols);
        free(node->u);
        free(node->v);
        free(node);
    }
}

/* Create a Murty priority queue */
static MurtyQueue* murty_queue_create(int capacity) {
    MurtyQueue *queue = (MurtyQueue *)malloc(sizeof(MurtyQueue));
    if (!queue) return NULL;

    queue->nodes = (MurtyNode **)malloc(capacity * sizeof(MurtyNode *));
    if (!queue->nodes) {
        free(queue);
        return NULL;
    }

    queue->size = 0;
    queue->capacity = capacity;
    return queue;
}

/* Free a Murty queue and all its nodes */
static void murty_queue_free(MurtyQueue *queue) {
    if (queue) {
        for (int i = 0; i < queue->size; i++) {
            murty_node_free(queue->nodes[i]);
        }
        free(queue->nodes);
        free(queue);
    }
}

/* Swap nodes in heap */
static void murty_heap_swap(MurtyQueue *queue, int i, int j) {
    MurtyNode *tmp = queue->nodes[i];
    queue->nodes[i] = queue->nodes[j];
    queue->nodes[j] = tmp;
}

/* Sift up in min-heap (uses priority, not cost) */
static void murty_heap_sift_up(MurtyQueue *queue, int i) {
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (queue->nodes[i]->priority < queue->nodes[parent]->priority) {
            murty_heap_swap(queue, i, parent);
            i = parent;
        } else {
            break;
        }
    }
}

/* Sift down in min-heap (uses priority, not cost) */
static void murty_heap_sift_down(MurtyQueue *queue, int i) {
    while (1) {
        int smallest = i;
        int left = 2 * i + 1;
        int right = 2 * i + 2;

        if (left < queue->size &&
            queue->nodes[left]->priority < queue->nodes[smallest]->priority) {
            smallest = left;
        }
        if (right < queue->size &&
            queue->nodes[right]->priority < queue->nodes[smallest]->priority) {
            smallest = right;
        }

        if (smallest != i) {
            murty_heap_swap(queue, i, smallest);
            i = smallest;
        } else {
            break;
        }
    }
}

/* Insert node into queue (takes ownership) */
static int murty_queue_insert(MurtyQueue *queue, MurtyNode *node) {
    if (queue->size >= queue->capacity) {
        /* Expand capacity */
        int new_capacity = queue->capacity * 2;
        MurtyNode **new_nodes = (MurtyNode **)realloc(
            queue->nodes, new_capacity * sizeof(MurtyNode *));
        if (!new_nodes) return 0;
        queue->nodes = new_nodes;
        queue->capacity = new_capacity;
    }

    queue->nodes[queue->size] = node;
    murty_heap_sift_up(queue, queue->size);
    queue->size++;
    return 1;
}

/* Extract minimum node from queue (transfers ownership) */
static MurtyNode* murty_queue_extract_min(MurtyQueue *queue) {
    if (queue->size == 0) return NULL;

    MurtyNode *min_node = queue->nodes[0];
    queue->size--;

    if (queue->size > 0) {
        queue->nodes[0] = queue->nodes[queue->size];
        murty_heap_sift_down(queue, 0);
    }

    return min_node;
}

/* Check if queue is empty */
static int murty_queue_is_empty(MurtyQueue *queue) {
    return queue->size == 0;
}

/*
 * Internal k-best solver using Murty's algorithm.
 *
 * Algorithm:
 * 1. Solve base LAP to get optimal solution
 * 2. For each edge (i, row_sol[i]) in solution, create child subproblem
 *    that includes parent's exclusions + one new exclusion
 * 3. Solve each subproblem and add to priority queue
 * 4. Extract minimum from queue, add to results
 * 5. Repeat step 2-4 until k solutions found
 */
static RalphLapStatus lap_solve_k_best_internal(
    int n,
    const double *cost,
    RalphLapObjective objective,
    int k,
    int *solutions,
    double *costs,
    int *num_found,
    RalphLapWorkspace *ws
) {
    if (n <= 0 || cost == NULL || k <= 0 || solutions == NULL ||
        costs == NULL || num_found == NULL) {
        return RALPH_LAP_INVALID_INPUT;
    }

    *num_found = 0;
    RalphLapStatus status;

    /* Working cost matrix for applying exclusions */
    double *work_cost = (double *)malloc(n * n * sizeof(double));
    if (!work_cost) {
        return RALPH_LAP_MEMORY_ERROR;
    }

    /* Maximum possible exclusions per node:
     * When fixing rows 0..i-1, we exclude (n-1) columns per row.
     * Plus 1 for the forbidden edge in row i.
     * Worst case: (n-1)*(n-1) + 1 = n² - 2n + 2 ≈ n² */
    int max_excluded = n * n;

    /* Create priority queue - initial capacity for typical usage */
    MurtyQueue *queue = murty_queue_create(k * n);
    if (!queue) {
        free(work_cost);
        return RALPH_LAP_MEMORY_ERROR;
    }

    /* Step 1: Solve base problem */
    int *row_sol = (int *)malloc(n * sizeof(int));
    int *col_sol = (int *)malloc(n * sizeof(int));
    double *u = (double *)malloc(n * sizeof(double));
    double *v = (double *)malloc(n * sizeof(double));
    double total_cost;

    if (!row_sol || !col_sol || !u || !v) {
        free(row_sol);
        free(col_sol);
        free(u);
        free(v);
        free(work_cost);
        murty_queue_free(queue);
        return RALPH_LAP_MEMORY_ERROR;
    }

    status = ralph_lap_solve_with_workspace(n, cost, objective,
                                            row_sol, col_sol, u, v, &total_cost, ws);
    if (status != RALPH_LAP_SUCCESS) {
        free(row_sol);
        free(col_sol);
        free(u);
        free(v);
        free(work_cost);
        murty_queue_free(queue);
        return status;
    }

    /* Store first solution */
    memcpy(&solutions[0], row_sol, n * sizeof(int));
    costs[0] = total_cost;
    *num_found = 1;

    if (k == 1) {
        free(row_sol);
        free(col_sol);
        free(u);
        free(v);
        free(work_cost);
        murty_queue_free(queue);
        return RALPH_LAP_SUCCESS;
    }

    /* Step 2: Create initial partition from base solution */
    /* For each row i, create subproblem excluding (i, row_sol[i])
     * but requiring all previous assignments (0..i-1) */
    for (int i = 0; i < n; i++) {
        MurtyNode *node = murty_node_create(n, max_excluded);
        if (!node) {
            free(row_sol);
            free(col_sol);
            free(u);
            free(v);
            free(work_cost);
            murty_queue_free(queue);
            return RALPH_LAP_MEMORY_ERROR;
        }

        /* Copy exclusions: require rows 0..i-1 to match base solution,
         * and forbid (i, row_sol[i]) */
        node->num_excluded = 0;

        /* For Murty's algorithm, we need cumulative exclusions:
         * - Rows 0..i-1: fixed to their base solution assignments (exclude all other columns)
         * - Row i: exclude the base solution column
         * Implementing this efficiently: we exclude (i, row_sol[i])
         * and additionally exclude all other options for rows 0..i-1 */

        /* Actually, the standard Murty partition is:
         * Subproblem i: fix rows 0..i-1 to base solution, forbid (i, base[i])
         * This is equivalent to: exclude (j, c) for all j < i where c != base[j],
         * plus exclude (i, base[i])
         *
         * More efficient approach: just store which edges to forbid */

        /* For partition i: forbid (i, row_sol[i]) and require rows < i match */
        /* We implement this by excluding:
         * - (i, row_sol[i])
         * - For each j < i: all columns except row_sol[j] */

        /* Actually, the simplest correct implementation:
         * Node i excludes (0, row_sol[0]), ..., (i-1, row_sol[i-1]) REQUIRE these
         * and excludes (i, row_sol[i]) FORBID this
         *
         * The "require" can be implemented by excluding all OTHER columns for rows 0..i-1
         */

        /* Simple approach: for row j < i, exclude all cols except row_sol[j]
         * For row i, exclude row_sol[i] */
        for (int j = 0; j < i; j++) {
            for (int c = 0; c < n; c++) {
                if (c != row_sol[j]) {
                    node->excluded_rows[node->num_excluded] = j;
                    node->excluded_cols[node->num_excluded] = c;
                    node->num_excluded++;
                }
            }
        }
        /* Forbid (i, row_sol[i]) */
        node->excluded_rows[node->num_excluded] = i;
        node->excluded_cols[node->num_excluded] = row_sol[i];
        node->num_excluded++;

        /* Apply exclusions to cost matrix */
        memcpy(work_cost, cost, n * n * sizeof(double));
        for (int e = 0; e < node->num_excluded; e++) {
            int er = node->excluded_rows[e];
            int ec = node->excluded_cols[e];
            work_cost[er * n + ec] = RALPH_LAP_INFINITY;
        }

        /* Solve subproblem */
        status = ralph_lap_solve_with_workspace(n, work_cost, objective,
                                                node->row_sol, NULL,
                                                node->u, node->v,
                                                &node->cost, ws);

        if (status == RALPH_LAP_SUCCESS) {
            /* Verify solution uses only allowed edges */
            int valid = 1;
            for (int r = 0; r < n && valid; r++) {
                int c = node->row_sol[r];
                if (work_cost[r * n + c] >= RALPH_LAP_INFINITY * 0.5) {
                    valid = 0;
                }
            }

            if (valid) {
                /* Set priority for heap ordering:
                 * minimize: priority = cost (extract minimum)
                 * maximize: priority = -cost (extract maximum) */
                node->priority = (objective == RALPH_LAP_MAXIMIZE) ? -node->cost : node->cost;
                murty_queue_insert(queue, node);
            } else {
                murty_node_free(node);
            }
        } else {
            /* Infeasible subproblem - discard */
            murty_node_free(node);
        }
    }

    /* Step 3: Main loop - extract best and partition */
    while (*num_found < k && !murty_queue_is_empty(queue)) {
        MurtyNode *best = murty_queue_extract_min(queue);

        /* Store this solution */
        memcpy(&solutions[(*num_found) * n], best->row_sol, n * sizeof(int));
        costs[*num_found] = best->cost;
        (*num_found)++;

        if (*num_found >= k) {
            murty_node_free(best);
            break;
        }

        /* Partition: create children from this solution
         * Find the first "free" row (not fixed by parent's exclusions) */
        int first_free_row = 0;

        /* Determine which rows are "fixed" in best's exclusions */
        /* A row is fixed if all but one column is excluded */
        int *row_excluded_count = (int *)calloc(n, sizeof(int));
        if (!row_excluded_count) {
            murty_node_free(best);
            break;
        }

        for (int e = 0; e < best->num_excluded; e++) {
            row_excluded_count[best->excluded_rows[e]]++;
        }

        /* Row is fixed if it has n-1 exclusions (only one column allowed) */
        for (int r = 0; r < n; r++) {
            if (row_excluded_count[r] == n - 1) {
                first_free_row = r + 1;
            } else {
                break;
            }
        }
        free(row_excluded_count);

        /* Create child nodes for rows >= first_free_row */
        for (int i = first_free_row; i < n; i++) {
            MurtyNode *child = murty_node_create(n, max_excluded);
            if (!child) continue;

            /* Copy parent's exclusions */
            child->num_excluded = best->num_excluded;
            memcpy(child->excluded_rows, best->excluded_rows,
                   best->num_excluded * sizeof(int));
            memcpy(child->excluded_cols, best->excluded_cols,
                   best->num_excluded * sizeof(int));

            /* Add exclusions to fix rows first_free_row..i-1 to best's solution */
            for (int j = first_free_row; j < i; j++) {
                for (int c = 0; c < n; c++) {
                    if (c != best->row_sol[j]) {
                        child->excluded_rows[child->num_excluded] = j;
                        child->excluded_cols[child->num_excluded] = c;
                        child->num_excluded++;
                    }
                }
            }

            /* Forbid (i, best->row_sol[i]) */
            child->excluded_rows[child->num_excluded] = i;
            child->excluded_cols[child->num_excluded] = best->row_sol[i];
            child->num_excluded++;

            /* Apply exclusions */
            memcpy(work_cost, cost, n * n * sizeof(double));
            for (int e = 0; e < child->num_excluded; e++) {
                int er = child->excluded_rows[e];
                int ec = child->excluded_cols[e];
                work_cost[er * n + ec] = RALPH_LAP_INFINITY;
            }

            /* Solve */
            status = ralph_lap_solve_with_workspace(n, work_cost, objective,
                                                    child->row_sol, NULL,
                                                    child->u, child->v,
                                                    &child->cost, ws);

            if (status == RALPH_LAP_SUCCESS) {
                /* Verify solution doesn't use forbidden edges */
                int valid = 1;
                for (int r = 0; r < n && valid; r++) {
                    int c = child->row_sol[r];
                    if (work_cost[r * n + c] >= RALPH_LAP_INFINITY * 0.5) {
                        valid = 0;
                    }
                }

                if (valid) {
                    /* Set priority for heap ordering:
                     * minimize: priority = cost (extract minimum)
                     * maximize: priority = -cost (extract maximum) */
                    child->priority = (objective == RALPH_LAP_MAXIMIZE) ? -child->cost : child->cost;
                    murty_queue_insert(queue, child);
                } else {
                    murty_node_free(child);
                }
            } else {
                murty_node_free(child);
            }
        }

        murty_node_free(best);
    }

    /* Cleanup */
    free(row_sol);
    free(col_sol);
    free(u);
    free(v);
    free(work_cost);
    murty_queue_free(queue);

    return (*num_found > 0) ? RALPH_LAP_SUCCESS : RALPH_LAP_INFEASIBLE;
}

/* Public API: k-best with automatic workspace */
RalphLapStatus ralph_lap_solve_k_best(
    int n,
    const double *cost,
    RalphLapObjective objective,
    int k,
    int *solutions,
    double *costs,
    int *num_found
) {
    if (n <= 0 || cost == NULL || k <= 0 || solutions == NULL ||
        costs == NULL || num_found == NULL) {
        return RALPH_LAP_INVALID_INPUT;
    }

    /* Create temporary workspace */
    RalphLapWorkspace *ws = ralph_lap_workspace_create(n);
    if (!ws) {
        return RALPH_LAP_MEMORY_ERROR;
    }

    RalphLapStatus status = lap_solve_k_best_internal(
        n, cost, objective, k, solutions, costs, num_found, ws);

    ralph_lap_workspace_free(ws);
    return status;
}

/* Public API: k-best with provided workspace */
RalphLapStatus ralph_lap_solve_k_best_with_workspace(
    int n,
    const double *cost,
    RalphLapObjective objective,
    int k,
    int *solutions,
    double *costs,
    int *num_found,
    RalphLapWorkspace *ws
) {
    if (n <= 0 || cost == NULL || k <= 0 || solutions == NULL ||
        costs == NULL || num_found == NULL || ws == NULL) {
        return RALPH_LAP_INVALID_INPUT;
    }

    if (n > ws->max_n) {
        return RALPH_LAP_INVALID_INPUT;
    }

    return lap_solve_k_best_internal(n, cost, objective, k, solutions, costs, num_found, ws);
}

/* ============================================================================
 * Runtime Configuration
 * ============================================================================ */

void ralph_lap_set_parallel(int enabled) {
    lap_parallel_enabled = enabled ? 1 : 0;
}

int ralph_lap_get_parallel(void) {
    return lap_parallel_enabled;
}

void ralph_lap_set_epsilon_scaling(int enabled) {
    lap_epsilon_scaling_enabled = enabled ? 1 : 0;
}

int ralph_lap_get_epsilon_scaling(void) {
    return lap_epsilon_scaling_enabled;
}

void ralph_lap_set_epsilon_factor(double factor) {
    if (factor > 1.0) {
        lap_epsilon_factor = factor;
    }
}

double ralph_lap_get_epsilon_factor(void) {
    return lap_epsilon_factor;
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

/* ============================================================================
 * Unified LAP API Implementation
 * ============================================================================ */

/*
 * Apply forbidden assignments by setting costs to infinity.
 * Returns pointer to modified cost array (either original or workspace copy).
 */
static const double* apply_forbidden_dense(
    const RalphLapProblem *prob,
    const RalphLapOptions *opts,
    RalphLapWorkspace *ws
) {
    if (opts->num_forbidden == 0) {
        return prob->dense_cost;
    }

    /* Copy cost matrix to workspace and apply forbidden */
    int nm = prob->n * prob->m;
    memcpy(ws->work_cost, prob->dense_cost, nm * sizeof(double));

    for (int f = 0; f < opts->num_forbidden; f++) {
        int i = opts->forbidden_rows[f];
        int j = opts->forbidden_cols[f];
        if (i >= 0 && i < prob->n && j >= 0 && j < prob->m) {
            ws->work_cost[i * prob->m + j] = RALPH_LAP_INFINITY;
        }
    }

    return ws->work_cost;
}

/*
 * Internal: solve standard (single solution) LAP with unified problem/options.
 */
static RalphLapStatus lap_solve_standard_unified(
    const RalphLapProblem *prob,
    const RalphLapOptions *opts,
    RalphLapResult *result,
    RalphLapWorkspace *ws
) {
    RalphLapStatus status;
    double single_cost = 0;
    double *cost_ptr = result->costs ? result->costs : &single_cost;

    /* Apply algorithm-level settings */
    int old_eps = lap_epsilon_scaling_enabled;
    double old_factor = lap_epsilon_factor;
    int old_parallel = lap_parallel_enabled;

    if (opts) {
        lap_epsilon_scaling_enabled = opts->epsilon_scaling;
        if (opts->epsilon_factor > 1.0) lap_epsilon_factor = opts->epsilon_factor;
        lap_parallel_enabled = opts->parallel;
    }

    /* Dispatch based on cost representation and dimensions */
    switch (prob->cost_type) {
        case RALPH_LAP_COST_DENSE:
            if (prob->n == prob->m) {
                /* Square dense LAP */
                const double *cost = apply_forbidden_dense(prob, opts, ws);

                if (opts && opts->warm_start && ws) {
                    status = ralph_lap_solve_warm(
                        prob->n, cost, prob->objective,
                        result->row_sol, result->col_sol,
                        result->u, result->v, cost_ptr,
                        ws, 1
                    );
                } else if (ws) {
                    status = ralph_lap_solve_with_workspace(
                        prob->n, cost, prob->objective,
                        result->row_sol, result->col_sol,
                        result->u, result->v, cost_ptr,
                        ws
                    );
                } else {
                    status = ralph_lap_solve(
                        prob->n, cost, prob->objective,
                        result->row_sol, result->col_sol,
                        result->u, result->v, cost_ptr
                    );
                }
            } else {
                /* Rectangular dense LAP */
                /* Note: forbidden assignments for rect would need work_cost copy */
                status = ralph_lap_solve_rect(
                    prob->n, prob->m, prob->dense_cost, prob->objective,
                    result->row_sol, result->col_sol, cost_ptr
                );
            }
            break;

        case RALPH_LAP_COST_SPARSE:
            /* Sparse LAP - forbidden assignments already implicit in sparse format */
            /* TODO: Add support for additional forbidden on top of sparse */
            status = ralph_lap_solve_sparse(
                prob->n, prob->sparse.nnz,
                prob->sparse.row_ptr, prob->sparse.col_idx, prob->sparse.values,
                prob->objective, result->row_sol, result->col_sol, cost_ptr
            );
            break;

        case RALPH_LAP_COST_CALLBACK:
            /* Callback-based LAP */
            /* TODO: Add forbidden support via wrapper callback */
            if (ws) {
                status = ralph_lap_solve_callback_with_workspace(
                    prob->n, prob->callback.fn, prob->callback.user_data,
                    prob->objective, result->row_sol, result->col_sol,
                    result->u, result->v, cost_ptr, ws
                );
            } else {
                status = ralph_lap_solve_callback(
                    prob->n, prob->callback.fn, prob->callback.user_data,
                    prob->objective, result->row_sol, result->col_sol,
                    result->u, result->v, cost_ptr
                );
            }
            break;

        default:
            status = RALPH_LAP_INVALID_INPUT;
            break;
    }

    /* Restore settings */
    lap_epsilon_scaling_enabled = old_eps;
    lap_epsilon_factor = old_factor;
    lap_parallel_enabled = old_parallel;

    if (status == RALPH_LAP_SUCCESS) {
        result->num_found = 1;
    }

    return status;
}

/*
 * Internal: solve k-best LAP with unified problem/options.
 */
static RalphLapStatus lap_solve_k_best_unified(
    const RalphLapProblem *prob,
    const RalphLapOptions *opts,
    RalphLapResult *result,
    RalphLapWorkspace *ws
) {
    /* Currently k-best only supports dense square */
    if (prob->cost_type != RALPH_LAP_COST_DENSE) {
        /* For non-dense, we could convert to dense first */
        /* For now, return error - future enhancement */
        return RALPH_LAP_INVALID_INPUT;
    }

    if (prob->n != prob->m) {
        /* Rectangular k-best not yet supported */
        return RALPH_LAP_INVALID_INPUT;
    }

    const double *cost = prob->dense_cost;

    /* Apply forbidden assignments if any */
    if (opts->num_forbidden > 0 && ws) {
        cost = apply_forbidden_dense(prob, opts, ws);
    }

    /* Call existing k-best implementation */
    if (ws) {
        return ralph_lap_solve_k_best_with_workspace(
            prob->n, cost, prob->objective, opts->k,
            result->row_sol, result->costs, &result->num_found, ws
        );
    } else {
        return ralph_lap_solve_k_best(
            prob->n, cost, prob->objective, opts->k,
            result->row_sol, result->costs, &result->num_found
        );
    }
}

/*
 * Unified LAP solve function - main entry point.
 */
RalphLapStatus ralph_lap_solve_ex(
    const RalphLapProblem *problem,
    const RalphLapOptions *options,
    RalphLapResult *result,
    RalphLapWorkspace *workspace
) {
    /* Validate inputs */
    if (!problem || !result) {
        return RALPH_LAP_INVALID_INPUT;
    }

    if (problem->n <= 0 || problem->m <= 0) {
        return RALPH_LAP_INVALID_INPUT;
    }

    if (!result->row_sol) {
        return RALPH_LAP_INVALID_INPUT;
    }

    /* Validate cost representation */
    switch (problem->cost_type) {
        case RALPH_LAP_COST_DENSE:
            if (!problem->dense_cost) return RALPH_LAP_INVALID_INPUT;
            break;
        case RALPH_LAP_COST_SPARSE:
            if (!problem->sparse.row_ptr || !problem->sparse.col_idx ||
                !problem->sparse.values) return RALPH_LAP_INVALID_INPUT;
            break;
        case RALPH_LAP_COST_CALLBACK:
            if (!problem->callback.fn) return RALPH_LAP_INVALID_INPUT;
            break;
        default:
            return RALPH_LAP_INVALID_INPUT;
    }

    /* Use default options if not provided */
    RalphLapOptions default_opts = RALPH_LAP_OPTIONS_DEFAULT;
    const RalphLapOptions *opts = options ? options : &default_opts;

    /* Handle workspace */
    RalphLapWorkspace *ws = workspace;
    int ws_allocated = 0;

    int max_dim = (problem->n > problem->m) ? problem->n : problem->m;

    if (!ws && (opts->warm_start || opts->num_forbidden > 0 ||
                opts->algorithm == RALPH_LAP_ALG_K_BEST)) {
        /* Need workspace for these features */
        ws = ralph_lap_workspace_create(max_dim);
        if (!ws) return RALPH_LAP_MEMORY_ERROR;
        ws_allocated = 1;
    }

    if (ws && max_dim > ws->max_n) {
        if (ws_allocated) ralph_lap_workspace_free(ws);
        return RALPH_LAP_INVALID_INPUT;
    }

    /* Initialize result */
    result->status = RALPH_LAP_SUCCESS;
    result->num_found = 0;

    /* Dispatch based on algorithm */
    RalphLapStatus status;

    switch (opts->algorithm) {
        case RALPH_LAP_ALG_K_BEST:
            if (opts->k <= 0) {
                status = RALPH_LAP_INVALID_INPUT;
            } else if (opts->k == 1) {
                /* k=1 is just standard solve */
                status = lap_solve_standard_unified(problem, opts, result, ws);
            } else {
                status = lap_solve_k_best_unified(problem, opts, result, ws);
            }
            break;

        case RALPH_LAP_ALG_BOTTLENECK:
            /* Not yet implemented */
            status = RALPH_LAP_INVALID_INPUT;
            break;

        case RALPH_LAP_ALG_STANDARD:
        default:
            status = lap_solve_standard_unified(problem, opts, result, ws);
            break;
    }

    result->status = status;

    if (ws_allocated) {
        ralph_lap_workspace_free(ws);
    }

    return status;
}
