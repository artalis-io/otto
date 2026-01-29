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

    ws->block_size = work_cost_size + col_price_size + row_price_size + dist_size +
                     row_assign_size + col_assign_size + matches_size + free_rows_size +
                     pred_size + col_list_size + in_free_list_size + heap_size + heap_pos_size;

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
    ws->heap_pos = (int *)ptr;

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
