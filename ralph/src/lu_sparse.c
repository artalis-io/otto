/*
 * Ralph - Sparse LU Factorization Implementation
 *
 * Implements sparse LU factorization with:
 * - AMD (Approximate Minimum Degree) column ordering
 * - Markowitz pivot selection for fill-in reduction
 * - Threshold pivoting for numerical stability
 * - Dynamic sparse storage
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <limits.h>
#include "lp.h"
#include "lp_log.h"
#include "lp_policy_glpk_compat.h"
#include "lp_glpk_strict.h"
#include "lu_update_backend.h"
#include "lu_supernode.h"

/* ============================================================================
 * AMD (Approximate Minimum Degree) Ordering
 * ============================================================================
 *
 * Computes a fill-reducing column permutation for the matrix.
 * Uses a simplified AMD that works on column structure directly.
 *
 * For a matrix A, we want to minimize fill-in in the LU factors.
 * AMD orders columns by approximate "degree" in the elimination graph,
 * where degree approximates the fill-in caused by eliminating that column.
 */

typedef struct {
    int *head;      /* head[d] = first column with degree d, or -1 */
    int *next;      /* next[j] = next column with same degree as j */
    int *prev;      /* prev[j] = prev column with same degree as j */
    int *degree;    /* degree[j] = current degree of column j */
    int min_degree; /* minimum degree in the structure */
    int n;          /* dimension */
} AMDWorkspace;

/* Forward declaration for cleanup helper */
static void amd_workspace_free(AMDWorkspace *amd);

static AMDWorkspace* amd_workspace_create(int n) {
    AMDWorkspace *amd = (AMDWorkspace*)calloc(1, sizeof(AMDWorkspace));
    if (!amd) return NULL;

    amd->n = n;
    amd->head = (int*)calloc((n + 1), sizeof(int));
    amd->next = (int*)calloc(n, sizeof(int));
    amd->prev = (int*)calloc(n, sizeof(int));
    amd->degree = (int*)calloc(n, sizeof(int));

    if (!amd->head || !amd->next || !amd->prev || !amd->degree) {
        amd_workspace_free(amd);
        return NULL;
    }

    /* Initialize degree lists */
    for (int d = 0; d <= n; d++) {
        amd->head[d] = -1;
    }
    amd->min_degree = n;

    return amd;
}

static void amd_workspace_free(AMDWorkspace *amd) {
    if (!amd) return;
    free(amd->head);
    free(amd->next);
    free(amd->prev);
    free(amd->degree);
    free(amd);
}

/* Add column j with given degree to the degree list */
static void amd_add_to_degree_list(AMDWorkspace *amd, int j, int d) {
    if (d > amd->n) d = amd->n;
    amd->degree[j] = d;
    amd->next[j] = amd->head[d];
    amd->prev[j] = -1;
    if (amd->head[d] >= 0) {
        amd->prev[amd->head[d]] = j;
    }
    amd->head[d] = j;
    if (d < amd->min_degree) {
        amd->min_degree = d;
    }
}

/* Remove column j from its degree list */
static void amd_remove_from_degree_list(AMDWorkspace *amd, int j) {
    int d = amd->degree[j];
    if (amd->prev[j] >= 0) {
        amd->next[amd->prev[j]] = amd->next[j];
    } else {
        amd->head[d] = amd->next[j];
    }
    if (amd->next[j] >= 0) {
        amd->prev[amd->next[j]] = amd->prev[j];
    }
}

/* Get and remove the column with minimum degree */
static int amd_get_min_degree_col(AMDWorkspace *amd) {
    while (amd->min_degree <= amd->n && amd->head[amd->min_degree] < 0) {
        amd->min_degree++;
    }
    if (amd->min_degree > amd->n) return -1;

    int j = amd->head[amd->min_degree];
    amd_remove_from_degree_list(amd, j);
    return j;
}

/*
 * Compute AMD ordering for matrix B with element absorption.
 *
 * This implements a proper AMD algorithm that tracks the elimination graph:
 * - When column j is eliminated, it creates an "element" (clique)
 * - The degree of remaining columns is computed as their external degree
 *   in the elimination graph (original rows + element connections)
 * - Element absorption: when all columns in a row are in the current element,
 *   that row can be pruned from further degree computations
 *
 * Returns a column permutation array where perm[k] = j means
 * column j should be the k-th column in the reordered matrix.
 */
static int* compute_col_ordering(const SparseMatrix *B) {
    int n = B->ncols;
    int m = B->nrows;

    int *perm = (int*)calloc(n, sizeof(int));
    int *eliminated = (int*)calloc(n, sizeof(int));
    int *marker = (int*)calloc(n, sizeof(int));  /* For counting unique neighbors */

    if (!perm || !eliminated || !marker) {
        free(perm);
        free(eliminated);
        free(marker);
        return NULL;
    }

    for (int j = 0; j < n; j++) marker[j] = -1;

    /* Build row-to-column adjacency */
    int *row_ptr = (int*)calloc(m + 1, sizeof(int));
    int *row_cols = (int*)calloc(B->nnz, sizeof(int));
    int *row_len = (int*)calloc(m, sizeof(int));  /* Current active length of each row */

    if (!row_ptr || !row_cols || !row_len) {
        free(perm);
        free(eliminated);
        free(marker);
        free(row_ptr);
        free(row_cols);
        free(row_len);
        return NULL;
    }

    /* Count columns per row */
    for (int j = 0; j < n; j++) {
        for (int p = B->colptr[j]; p < B->colptr[j + 1]; p++) {
            row_ptr[B->rowidx[p] + 1]++;
        }
    }

    /* Cumulative sum */
    for (int i = 0; i < m; i++) {
        row_ptr[i + 1] += row_ptr[i];
    }

    /* Fill row_cols and row_len */
    int *row_count = (int*)calloc(m, sizeof(int));
    if (!row_count) {
        free(perm);
        free(eliminated);
        free(marker);
        free(row_ptr);
        free(row_cols);
        free(row_len);
        return NULL;
    }

    for (int j = 0; j < n; j++) {
        for (int p = B->colptr[j]; p < B->colptr[j + 1]; p++) {
            int i = B->rowidx[p];
            row_cols[row_ptr[i] + row_count[i]++] = j;
        }
    }
    for (int i = 0; i < m; i++) {
        row_len[i] = row_count[i];
    }
    free(row_count);

    /* Element tracking with proper linked list storage.
     * We use a pool-based approach for element adjacency lists.
     */

    /* For each column, track the latest element it belongs to.
     * col_element[j] = most recent element containing column j, or -1
     * This is used for element absorption detection.
     */
    int *col_element = (int*)calloc(n, sizeof(int));

    /* For each element, track its member columns as a simple list */
    int *element_head = (int*)calloc(n, sizeof(int));
    int *element_next = (int*)calloc(n, sizeof(int));

    if (!col_element || !element_head || !element_next) {
        free(perm);
        free(eliminated);
        free(marker);
        free(row_ptr);
        free(row_cols);
        free(row_len);
        free(col_element);
        free(element_head);
        free(element_next);
        return NULL;
    }

    for (int j = 0; j < n; j++) {
        col_element[j] = -1;
        element_head[j] = -1;
    }

    /* Initialize AMD workspace */
    AMDWorkspace *amd = amd_workspace_create(n);
    if (!amd) {
        free(perm);
        free(eliminated);
        free(marker);
        free(row_ptr);
        free(row_cols);
        free(row_len);
        free(col_element);
        free(element_head);
        free(element_next);
        return NULL;
    }

    /* Compute initial degrees: count unique columns reachable through rows */
    for (int j = 0; j < n; j++) {
        int degree = 0;
        for (int p = B->colptr[j]; p < B->colptr[j + 1]; p++) {
            int row = B->rowidx[p];
            for (int q = row_ptr[row]; q < row_ptr[row] + row_len[row]; q++) {
                int neighbor = row_cols[q];
                if (neighbor != j && marker[neighbor] != j) {
                    marker[neighbor] = j;
                    degree++;
                }
            }
        }
        if (degree < 1) degree = 1;
        if (degree > n - 1) degree = n - 1;
        amd_add_to_degree_list(amd, j, degree);
    }

    /* Workspace for tracking adjacent columns */
    int *adj_cols = (int*)calloc(n, sizeof(int));
    if (!adj_cols) {
        amd_workspace_free(amd);
        free(perm);
        free(eliminated);
        free(marker);
        free(row_ptr);
        free(row_cols);
        free(row_len);
        free(col_element);
        free(element_head);
        free(element_next);
        return NULL;
    }

    /* Main AMD loop */
    for (int k = 0; k < n; k++) {
        /* Select minimum degree column */
        int pivot = amd_get_min_degree_col(amd);
        if (pivot < 0) {
            /* Find any remaining column */
            for (int jj = 0; jj < n; jj++) {
                if (!eliminated[jj]) {
                    pivot = jj;
                    break;
                }
            }
        }
        if (pivot < 0) break;  /* All done */

        perm[k] = pivot;
        eliminated[pivot] = 1;

        /* Gather all non-eliminated columns adjacent to pivot through rows */
        int num_adj = 0;
        int mark_id = n + k;  /* Unique marker for this step */

        for (int p = B->colptr[pivot]; p < B->colptr[pivot + 1]; p++) {
            int row = B->rowidx[p];
            if (row_len[row] == 0) continue;  /* Absorbed row */

            for (int q = row_ptr[row]; q < row_ptr[row] + row_len[row]; q++) {
                int neighbor = row_cols[q];
                if (neighbor != pivot && !eliminated[neighbor] && marker[neighbor] != mark_id) {
                    marker[neighbor] = mark_id;
                    adj_cols[num_adj++] = neighbor;
                }
            }
        }

        /* Also include columns from the previous element if pivot was in one */
        int prev_elem = col_element[pivot];
        if (prev_elem >= 0) {
            int col = element_head[prev_elem];
            while (col >= 0) {
                if (!eliminated[col] && marker[col] != mark_id) {
                    marker[col] = mark_id;
                    adj_cols[num_adj++] = col;
                }
                col = element_next[col];
            }
        }

        /* Create new element k containing all adjacent columns */
        element_head[k] = -1;
        for (int i = 0; i < num_adj; i++) {
            int col = adj_cols[i];
            element_next[col] = element_head[k];
            element_head[k] = col;
            col_element[col] = k;  /* Update column's element membership */
        }

        /* Update degrees of adjacent columns */
        for (int i = 0; i < num_adj; i++) {
            int col = adj_cols[i];

            /* Compute new degree: count unique non-eliminated neighbors through rows */
            int new_degree = 0;
            int degree_mark = n * 2 + k * n + col;

            /* From original rows */
            for (int p = B->colptr[col]; p < B->colptr[col + 1]; p++) {
                int row = B->rowidx[p];
                if (row_len[row] == 0) continue;  /* Absorbed row */

                for (int q = row_ptr[row]; q < row_ptr[row] + row_len[row]; q++) {
                    int neighbor = row_cols[q];
                    if (neighbor != col && !eliminated[neighbor] && marker[neighbor] != degree_mark) {
                        marker[neighbor] = degree_mark;
                        new_degree++;
                    }
                }
            }

            /* From current element (all adjacent columns are connected) */
            int c = element_head[k];
            while (c >= 0) {
                if (c != col && !eliminated[c] && marker[c] != degree_mark) {
                    marker[c] = degree_mark;
                    new_degree++;
                }
                c = element_next[c];
            }

            if (new_degree < 1) new_degree = 1;
            if (new_degree > n - k - 1) new_degree = n - k - 1;

            /* Update degree if changed */
            if (new_degree != amd->degree[col]) {
                amd_remove_from_degree_list(amd, col);
                amd_add_to_degree_list(amd, col, new_degree);
            }
        }

        /* Row absorption: prune rows whose non-eliminated columns are all in element k */
        for (int p = B->colptr[pivot]; p < B->colptr[pivot + 1]; p++) {
            int row = B->rowidx[p];
            if (row_len[row] == 0) continue;  /* Already absorbed */

            int all_in_element = 1;
            int active_count = 0;
            for (int q = row_ptr[row]; q < row_ptr[row] + row_len[row]; q++) {
                int col = row_cols[q];
                if (!eliminated[col]) {
                    active_count++;
                    if (marker[col] != mark_id) {
                        all_in_element = 0;
                        break;
                    }
                }
            }
            if (all_in_element && active_count > 0) {
                /* This row is absorbed - all its active columns are in element k */
                row_len[row] = 0;
            }
        }
    }

    /* Cleanup */
    amd_workspace_free(amd);
    free(eliminated);
    free(marker);
    free(row_ptr);
    free(row_cols);
    free(row_len);
    free(col_element);
    free(element_head);
    free(element_next);
    free(adj_cols);

    return perm;
}

/* ============================================================================
 * LP-Specific Column Ordering
 * ============================================================================
 *
 * For LP bases, the matrix B typically contains:
 * - Structural columns from the constraint matrix A
 * - Identity columns from slack variables
 *
 * An efficient ordering for LP bases:
 * 1. Identify identity columns (singletons with |value| = 1)
 * 2. Order structural columns by increasing column count (sparser first)
 * 3. Put identity columns at the end (they become diagonal of U without fill)
 *
 * This exploits the LP structure for faster factorization with less fill-in.
 */
static int* compute_lp_column_ordering(const SparseMatrix *B) {
    int n = B->ncols;
    int *perm = (int*)calloc(n, sizeof(int));
    if (!perm) return NULL;

    /* Classify columns: identity (singleton ±1) vs structural */
    int *is_identity = (int*)calloc(n, sizeof(int));
    int *col_counts = (int*)calloc(n, sizeof(int));

    if (!is_identity || !col_counts) {
        free(perm);
        free(is_identity);
        free(col_counts);
        return NULL;
    }

    for (int j = 0; j < n; j++) {
        int nnz = B->colptr[j + 1] - B->colptr[j];
        col_counts[j] = nnz;

        /* Check if column is identity (exactly 1 nonzero with value ±1) */
        if (nnz == 1) {
            int p = B->colptr[j];
            double val = B->values[p];
            if (fabs(fabs(val) - 1.0) < 1e-10) {
                is_identity[j] = 1;
            }
        }
    }

    /* Sort structural columns by column count (insertion sort for simplicity) */
    int *structural = (int*)calloc(n, sizeof(int));
    int num_structural = 0;

    if (!structural) {
        free(perm);
        free(is_identity);
        free(col_counts);
        return NULL;
    }

    for (int j = 0; j < n; j++) {
        if (!is_identity[j]) {
            structural[num_structural++] = j;
        }
    }

    /* Sort structural columns by increasing column count */
    for (int i = 1; i < num_structural; i++) {
        int key = structural[i];
        int key_count = col_counts[key];
        int j = i - 1;
        while (j >= 0 && col_counts[structural[j]] > key_count) {
            structural[j + 1] = structural[j];
            j--;
        }
        structural[j + 1] = key;
    }

    /* Build permutation: structural columns first (sorted), then identity columns */
    int idx = 0;

    /* Add structural columns */
    for (int i = 0; i < num_structural; i++) {
        perm[idx++] = structural[i];
    }

    /* Add identity columns */
    for (int j = 0; j < n; j++) {
        if (is_identity[j]) {
            perm[idx++] = j;
        }
    }

    free(is_identity);
    free(col_counts);
    free(structural);

    return perm;
}

/* ============================================================================
 * Sparse Working Storage for LU Factorization
 * ============================================================================ */

/* Linked list node for sparse column/row entries */
typedef struct SparseEntry {
    int idx;                    /* Row (for column lists) or column (for row lists) */
    double val;
    struct SparseEntry *next;
} SparseEntry;

/* Working matrix for sparse LU */
typedef struct {
    int m;
    SparseEntry **cols;         /* Column linked lists */
    SparseEntry **rows;         /* Row linked lists (for fast row access) */
    int *col_nnz;               /* Non-zeros per column */
    int *row_nnz;               /* Non-zeros per row */
    int *col_perm;              /* Column permutation (pivot order) */
    int *row_perm;              /* Row permutation */
    int *col_perm_inv;
    int *row_perm_inv;
    int *col_done;              /* 1 if column has been pivoted */
    int *row_done;              /* 1 if row has been pivoted */
    /* Dense workspace for scatter-gather elimination */
    double *work_dense;         /* Dense array of size m for fast row updates */
    int *work_marker;           /* Marker array for tracking non-zeros */
    /* Chunk-based memory pool for entries (avoids realloc invalidating pointers) */
    SparseEntry **chunks;       /* Array of chunk pointers */
    int num_chunks;             /* Number of allocated chunks */
    int max_chunks;             /* Capacity of chunks array */
    int chunk_size;             /* Entries per chunk */
    int cur_chunk;              /* Current chunk index */
    int cur_pos;                /* Position in current chunk */
} SparseLUWork;

static SparseLUWork* sparse_work_create(int m, int nnz_estimate) {
    SparseLUWork *work = (SparseLUWork*)calloc(1, sizeof(SparseLUWork));
    if (!work) return NULL;

    work->m = m;

    /* Initialize chunk-based memory pool */
    work->chunk_size = nnz_estimate * 4 + m;  /* Entries per chunk */
    work->max_chunks = 16;                     /* Start with room for 16 chunks */
    work->chunks = (SparseEntry**)calloc(work->max_chunks, sizeof(SparseEntry*));
    if (!work->chunks) {
        free(work);
        return NULL;
    }

    /* Allocate first chunk */
    work->chunks[0] = (SparseEntry*)calloc(work->chunk_size, sizeof(SparseEntry));
    if (!work->chunks[0]) {
        free(work->chunks);
        free(work);
        return NULL;
    }
    work->num_chunks = 1;
    work->cur_chunk = 0;
    work->cur_pos = 0;

    work->cols = (SparseEntry**)calloc(m, sizeof(SparseEntry*));
    work->rows = (SparseEntry**)calloc(m, sizeof(SparseEntry*));
    work->col_nnz = (int*)calloc(m, sizeof(int));
    work->row_nnz = (int*)calloc(m, sizeof(int));
    work->col_perm = (int*)calloc(m, sizeof(int));
    work->row_perm = (int*)calloc(m, sizeof(int));
    work->col_perm_inv = (int*)calloc(m, sizeof(int));
    work->row_perm_inv = (int*)calloc(m, sizeof(int));
    work->col_done = (int*)calloc(m, sizeof(int));
    work->row_done = (int*)calloc(m, sizeof(int));
    work->work_dense = (double*)calloc(m, sizeof(double));
    work->work_marker = (int*)calloc(m, sizeof(int));

    if (!work->cols || !work->rows || !work->col_nnz ||
        !work->row_nnz || !work->col_perm || !work->row_perm ||
        !work->col_perm_inv || !work->row_perm_inv ||
        !work->col_done || !work->row_done ||
        !work->work_dense || !work->work_marker) {
        for (int i = 0; i < work->num_chunks; i++) {
            free(work->chunks[i]);
        }
        free(work->chunks);
        free(work->cols);
        free(work->rows);
        free(work->col_nnz);
        free(work->row_nnz);
        free(work->col_perm);
        free(work->row_perm);
        free(work->col_perm_inv);
        free(work->row_perm_inv);
        free(work->col_done);
        free(work->row_done);
        free(work->work_dense);
        free(work->work_marker);
        free(work);
        return NULL;
    }

    /* Initialize permutations to identity */
    for (int i = 0; i < m; i++) {
        work->col_perm[i] = i;
        work->row_perm[i] = i;
        work->col_perm_inv[i] = i;
        work->row_perm_inv[i] = i;
    }

    return work;
}

static void sparse_work_free(SparseLUWork *work) {
    if (!work) return;
    /* Free all chunks */
    for (int i = 0; i < work->num_chunks; i++) {
        free(work->chunks[i]);
    }
    free(work->chunks);
    free(work->cols);
    free(work->rows);
    free(work->col_nnz);
    free(work->row_nnz);
    free(work->col_perm);
    free(work->row_perm);
    free(work->col_perm_inv);
    free(work->row_perm_inv);
    free(work->col_done);
    free(work->row_done);
    free(work->work_dense);
    free(work->work_marker);
    free(work);
}

static SparseEntry* alloc_entry(SparseLUWork *work) {
    /* Check if current chunk is full */
    if (work->cur_pos >= work->chunk_size) {
        /* Need a new chunk */
        if (work->cur_chunk + 1 >= work->num_chunks) {
            /* Need to allocate a new chunk */
            if (work->num_chunks >= work->max_chunks) {
                /* Grow chunks array */
                int new_max = work->max_chunks * 2;
                SparseEntry **new_chunks = (SparseEntry**)realloc(work->chunks,
                                                    new_max * sizeof(SparseEntry*));
                if (!new_chunks) return NULL;
                work->chunks = new_chunks;
                work->max_chunks = new_max;
            }

            /* Allocate new chunk */
            work->chunks[work->num_chunks] = (SparseEntry*)calloc(
                                                work->chunk_size, sizeof(SparseEntry));
            if (!work->chunks[work->num_chunks]) return NULL;
            work->num_chunks++;
        }
        work->cur_chunk++;
        work->cur_pos = 0;
    }

    return &work->chunks[work->cur_chunk][work->cur_pos++];
}

/* Add entry to column list (maintains row order) */
static int add_to_col(SparseLUWork *work, int col, int row, double val) {
    if (fabs(val) < RALPH_ZERO_TOL) return 0;  /* Skip zeros */

    SparseEntry *entry = alloc_entry(work);
    if (!entry) return -1;

    entry->idx = row;
    entry->val = val;

    /* Insert in order */
    SparseEntry **pp = &work->cols[col];
    while (*pp && (*pp)->idx < row) {
        pp = &(*pp)->next;
    }
    entry->next = *pp;
    *pp = entry;

    work->col_nnz[col]++;
    return 0;
}

/* Add entry to row list (maintains column order) */
static int add_to_row(SparseLUWork *work, int row, int col, double val) {
    if (fabs(val) < RALPH_ZERO_TOL) return 0;

    SparseEntry *entry = alloc_entry(work);
    if (!entry) return -1;

    entry->idx = col;
    entry->val = val;

    /* Insert in order */
    SparseEntry **pp = &work->rows[row];
    while (*pp && (*pp)->idx < col) {
        pp = &(*pp)->next;
    }
    entry->next = *pp;
    *pp = entry;

    work->row_nnz[row]++;
    return 0;
}

/* Get value from column list */
static double get_col_val(SparseLUWork *work, int col, int row) {
    for (SparseEntry *e = work->cols[col]; e; e = e->next) {
        if (e->idx == row) return e->val;
        if (e->idx > row) break;
    }
    return 0.0;
}

/* Set value in column list only (internal use) */
static int set_col_val_only(SparseLUWork *work, int col, int row, double val) {
    SparseEntry **pp = &work->cols[col];

    while (*pp && (*pp)->idx < row) {
        pp = &(*pp)->next;
    }

    if (*pp && (*pp)->idx == row) {
        /* Update existing */
        if (fabs(val) < RALPH_ZERO_TOL) {
            /* Remove entry */
            *pp = (*pp)->next;
            work->col_nnz[col]--;
            return 1;  /* Indicate removal */
        } else {
            (*pp)->val = val;
            return 0;
        }
    } else if (fabs(val) >= RALPH_ZERO_TOL) {
        /* Add new entry */
        SparseEntry *entry = alloc_entry(work);
        if (!entry) return -1;
        entry->idx = row;
        entry->val = val;
        entry->next = *pp;
        *pp = entry;
        work->col_nnz[col]++;
        return 2;  /* Indicate addition */
    }

    return 0;
}

/* Set value in row list only (internal use) */
static int set_row_val_only(SparseLUWork *work, int row, int col, double val) {
    SparseEntry **pp = &work->rows[row];

    while (*pp && (*pp)->idx < col) {
        pp = &(*pp)->next;
    }

    if (*pp && (*pp)->idx == col) {
        /* Update existing */
        if (fabs(val) < RALPH_ZERO_TOL) {
            /* Remove entry */
            *pp = (*pp)->next;
            work->row_nnz[row]--;
        } else {
            (*pp)->val = val;
        }
    } else if (fabs(val) >= RALPH_ZERO_TOL) {
        /* Add new entry */
        SparseEntry *entry = alloc_entry(work);
        if (!entry) return -1;
        entry->idx = col;
        entry->val = val;
        entry->next = *pp;
        *pp = entry;
        work->row_nnz[row]++;
    }

    return 0;
}

/* Set value in both column and row lists */
static int set_val(SparseLUWork *work, int col, int row, double val) {
    int col_result = set_col_val_only(work, col, row, val);
    if (col_result < 0) return -1;

    int row_result = set_row_val_only(work, row, col, val);
    if (row_result < 0) return -1;

    return 0;
}

/* ============================================================================
 * Markowitz Pivot Selection
 * ============================================================================ */

/* Threshold for numerical stability (Markowitz threshold) */
#define MARKOWITZ_THRESHOLD 0.1

/* Try to find a singleton pivot (row or column with exactly one active entry)
 * Singletons can be eliminated without fill-in, so we process them first.
 * Returns 1 if singleton found, 0 otherwise.
 */
static int find_singleton_pivot(SparseLUWork *work, int *pivot_row, int *pivot_col) {
    int m = work->m;

    /* First check for column singletons (column with exactly one active entry) */
    for (int j = 0; j < m; j++) {
        if (work->col_done[j]) continue;
        if (work->col_nnz[j] != 1) continue;

        /* Find the single entry in this column */
        for (SparseEntry *e = work->cols[j]; e; e = e->next) {
            if (work->row_done[e->idx]) continue;
            if (fabs(e->val) >= RALPH_PIVOT_TOL) {
                *pivot_row = e->idx;
                *pivot_col = j;
                return 1;
            }
        }
    }

    /* Then check for row singletons (row with exactly one active entry) */
    for (int i = 0; i < m; i++) {
        if (work->row_done[i]) continue;
        if (work->row_nnz[i] != 1) continue;

        /* Find the single entry in this row */
        for (SparseEntry *e = work->rows[i]; e; e = e->next) {
            if (work->col_done[e->idx]) continue;
            if (fabs(e->val) >= RALPH_PIVOT_TOL) {
                *pivot_row = i;
                *pivot_col = e->idx;
                return 1;
            }
        }
    }

    return 0;
}

/* Select pivot using Markowitz criterion with threshold pivoting */
static int select_pivot(SparseLUWork *work, int step, int *pivot_row, int *pivot_col) {
    (void)step;  /* Reserved for future use (e.g., step-dependent threshold) */
    int m = work->m;

    /* First try to find a singleton - these cause no fill-in */
    if (find_singleton_pivot(work, pivot_row, pivot_col)) {
        return 0;
    }

    int best_row = -1, best_col = -1;
    long long best_cost = (long long)m * m + 1;  /* Markowitz cost */
    double best_val = 0.0;

    /* Find maximum absolute value in each active column for threshold */
    double *col_max = (double*)calloc(m, sizeof(double));
    if (!col_max) return -1;

    for (int j = 0; j < m; j++) {
        if (work->col_done[j]) continue;
        for (SparseEntry *e = work->cols[j]; e; e = e->next) {
            if (work->row_done[e->idx]) continue;
            if (fabs(e->val) > col_max[j]) {
                col_max[j] = fabs(e->val);
            }
        }
    }

    /* Search for best pivot satisfying threshold
     * Optimization: columns with fewer nonzeros are more likely to produce
     * low Markowitz costs, so we process them first and early-exit when
     * we find a cost of 0.
     */
    for (int j = 0; j < m; j++) {
        if (work->col_done[j]) continue;
        if (col_max[j] < RALPH_PIVOT_TOL) continue;  /* Singular column */

        /* Skip columns that can't improve the best cost */
        if ((long long)(work->col_nnz[j] - 1) * (work->col_nnz[j] - 1) >= best_cost) continue;

        double threshold = MARKOWITZ_THRESHOLD * col_max[j];

        for (SparseEntry *e = work->cols[j]; e; e = e->next) {
            int i = e->idx;
            if (work->row_done[i]) continue;
            if (fabs(e->val) < threshold) continue;  /* Doesn't meet threshold */

            /* Compute Markowitz cost */
            long long cost = (long long)(work->row_nnz[i] - 1) * (work->col_nnz[j] - 1);

            if (cost < best_cost || (cost == best_cost && fabs(e->val) > best_val)) {
                best_cost = cost;
                best_row = i;
                best_col = j;
                best_val = fabs(e->val);

                /* Early exit if we found a cost-0 pivot */
                if (cost == 0) {
                    free(col_max);
                    *pivot_row = best_row;
                    *pivot_col = best_col;
                    return 0;
                }
            }
        }
    }

    free(col_max);

    if (best_row < 0 || best_col < 0) {
        return -1;  /* No valid pivot found - singular matrix */
    }

    *pivot_row = best_row;
    *pivot_col = best_col;
    return 0;
}

/* ============================================================================
 * Sparse LU Factorization
 * ============================================================================ */

/* Select row pivot within a fixed column (threshold pivoting) */
static int select_row_pivot(SparseLUWork *work, int col, int *pivot_row) {
    /* Find maximum absolute value in active part of column */
    double col_max = 0.0;
    for (SparseEntry *e = work->cols[col]; e; e = e->next) {
        if (work->row_done[e->idx]) continue;
        if (fabs(e->val) > col_max) {
            col_max = fabs(e->val);
        }
    }

    if (col_max < RALPH_PIVOT_TOL) {
        return -1;  /* Singular column */
    }

    double threshold = MARKOWITZ_THRESHOLD * col_max;

    /* Among entries meeting threshold, prefer smaller row count (less fill-in) */
    int best_row = -1;
    int best_nnz = work->m + 1;
    double best_val = 0.0;

    for (SparseEntry *e = work->cols[col]; e; e = e->next) {
        int i = e->idx;
        if (work->row_done[i]) continue;
        if (fabs(e->val) < threshold) continue;

        int row_nnz = work->row_nnz[i];
        if (row_nnz < best_nnz || (row_nnz == best_nnz && fabs(e->val) > best_val)) {
            best_row = i;
            best_nnz = row_nnz;
            best_val = fabs(e->val);
        }
    }

    if (best_row < 0) return -1;

    *pivot_row = best_row;
    return 0;
}

LUFailureReason lu_factorize_sparse(LUFactorization *lu, const SparseMatrix *B) {
    if (!lu || !B) return LU_FAIL_BAD_INPUT;
    if (B->nrows != B->ncols || B->nrows != lu->m) return LU_FAIL_BAD_INPUT;

    int m = lu->m;

    /* Compute column ordering for fill reduction
     * Options: 0 = natural order, 1 = AMD, 2 = LP-specific (default)
     */
    int ordering_method = 2;  /* LP-specific ordering for LP bases */
    int *col_order = NULL;

    if (ordering_method == 1) {
        col_order = compute_col_ordering(B);
    } else if (ordering_method == 2) {
        col_order = compute_lp_column_ordering(B);
    }

    if (!col_order) {
        /* Fallback to natural order */
        col_order = (int*)calloc(m, sizeof(int));
        if (!col_order) return LU_FAIL_FACTOR_ALLOC;
        for (int j = 0; j < m; j++) col_order[j] = j;
    }

    /* Create working storage */
    SparseLUWork *work = sparse_work_create(m, B->nnz);
    if (!work) {
        free(col_order);
        return LU_FAIL_FACTOR_ALLOC;
    }

    /* Copy matrix into working storage (both column and row lists) */
    for (int j = 0; j < m; j++) {
        for (int p = B->colptr[j]; p < B->colptr[j + 1]; p++) {
            int i = B->rowidx[p];
            double val = B->values[p];
            if (add_to_col(work, j, i, val) < 0 ||
                add_to_row(work, i, j, val) < 0) {
                sparse_work_free(work);
                free(col_order);
                return LU_FAIL_FACTOR_ALLOC;
            }
        }
    }

    /* Arrays to store L and U entries during factorization */
    int L_cap = B->nnz + m;
    int U_cap = B->nnz + m;
    int *L_i = (int*)calloc(L_cap, sizeof(int));
    int *L_j = (int*)calloc(L_cap, sizeof(int));
    double *L_v = (double*)calloc(L_cap, sizeof(double));
    int *U_i = (int*)calloc(U_cap, sizeof(int));
    int *U_j = (int*)calloc(U_cap, sizeof(int));
    double *U_v = (double*)calloc(U_cap, sizeof(double));
    int L_nnz = 0, U_nnz = 0;

    if (!L_i || !L_j || !L_v || !U_i || !U_j || !U_v) {
        free(L_i); free(L_j); free(L_v);
        free(U_i); free(U_j); free(U_v);
        sparse_work_free(work);
        free(col_order);
        return LU_FAIL_FACTOR_ALLOC;
    }

    /* Main elimination loop */
    int use_precomputed_order = (ordering_method > 0);

    for (int step = 0; step < m; step++) {
        int pivot_row, pivot_col;

        if (use_precomputed_order) {
            /* Use precomputed column order (AMD or LP-specific) with row selection */
            pivot_col = col_order[step];
            if (work->col_done[pivot_col]) {
                for (int j = 0; j < m; j++) {
                    if (!work->col_done[j]) {
                        pivot_col = j;
                        break;
                    }
                }
            }

            /* FAST PATH: Singleton column (identity column in LP) */
            /* These columns have exactly 1 nonzero - no elimination needed */
            if (work->col_nnz[pivot_col] == 1) {
                /* Find the single nonzero entry - that's our pivot */
                SparseEntry *e = work->cols[pivot_col];
                while (e && work->row_done[e->idx]) e = e->next;
                if (e) {
                    pivot_row = e->idx;
                    (void)e->val;  /* pivot_val = e->val, stored via loop below */

                    /* Record permutations */
                    work->row_perm[step] = pivot_row;
                    work->col_perm[step] = pivot_col;
                    work->row_done[pivot_row] = 1;
                    work->col_done[pivot_col] = 1;

                    /* Store U entries from pivot row (all active columns) */
                    for (SparseEntry *re = work->rows[pivot_row]; re; re = re->next) {
                        int j = re->idx;
                        if (work->col_done[j] && j != pivot_col) continue;
                        double val = re->val;
                        if (fabs(val) < RALPH_ZERO_TOL) continue;

                        if (U_nnz >= U_cap) {
                            int new_cap = U_cap * 2;
                            int *tmp_i = (int*)realloc(U_i, new_cap * sizeof(int));
                            int *tmp_j = (int*)realloc(U_j, new_cap * sizeof(int));
                            double *tmp_v = (double*)realloc(U_v, new_cap * sizeof(double));
                            if (!tmp_i || !tmp_j || !tmp_v) {
                                free(tmp_i ? tmp_i : U_i);
                                free(tmp_j ? tmp_j : U_j);
                                free(tmp_v ? tmp_v : U_v);
                                free(L_i); free(L_j); free(L_v);
                                sparse_work_free(work);
                                free(col_order);
                                return LU_FAIL_FACTOR_ALLOC;
                            }
                            U_i = tmp_i; U_j = tmp_j; U_v = tmp_v;
                            U_cap = new_cap;
                        }
                        U_i[U_nnz] = step;
                        U_j[U_nnz] = j;
                        U_v[U_nnz] = val;
                        U_nnz++;
                    }

                    /* Store L diagonal (always 1) */
                    if (L_nnz >= L_cap) {
                        int new_cap = L_cap * 2;
                        int *tmp_i = (int*)realloc(L_i, new_cap * sizeof(int));
                        int *tmp_j = (int*)realloc(L_j, new_cap * sizeof(int));
                        double *tmp_v = (double*)realloc(L_v, new_cap * sizeof(double));
                        if (!tmp_i || !tmp_j || !tmp_v) {
                            free(tmp_i ? tmp_i : L_i);
                            free(tmp_j ? tmp_j : L_j);
                            free(tmp_v ? tmp_v : L_v);
                            free(U_i); free(U_j); free(U_v);
                            sparse_work_free(work);
                            free(col_order);
                            return LU_FAIL_FACTOR_ALLOC;
                        }
                        L_i = tmp_i; L_j = tmp_j; L_v = tmp_v;
                        L_cap = new_cap;
                    }
                    L_i[L_nnz] = pivot_row;
                    L_j[L_nnz] = step;
                    L_v[L_nnz] = 1.0;
                    L_nnz++;

                    work->col_nnz[pivot_col] = 0;
                    continue;  /* Skip to next step - no elimination needed */
                }
            }

            if (select_row_pivot(work, pivot_col, &pivot_row) < 0) {
                int found = 0;
                for (int jj = 0; jj < m; jj++) {
                    if (!work->col_done[jj] && jj != pivot_col) {
                        if (select_row_pivot(work, jj, &pivot_row) == 0) {
                            pivot_col = jj;
                            found = 1;
                            break;
                        }
                    }
                }
                if (!found) {
                    free(L_i); free(L_j); free(L_v);
                    free(U_i); free(U_j); free(U_v);
                    sparse_work_free(work);
                    free(col_order);
                    return LU_FAIL_FACTOR_SINGULAR;  /* Singular */
                }
            }
        } else {
            /* Full Markowitz pivot selection (both row and column) */
            if (select_pivot(work, step, &pivot_row, &pivot_col) < 0) {
                free(L_i); free(L_j); free(L_v);
                free(U_i); free(U_j); free(U_v);
                sparse_work_free(work);
                free(col_order);
                return LU_FAIL_FACTOR_SINGULAR;  /* Singular */
            }
        }

        double pivot_val = get_col_val(work, pivot_col, pivot_row);

        /* Record permutations */
        work->row_perm[step] = pivot_row;
        work->col_perm[step] = pivot_col;
        work->row_done[pivot_row] = 1;
        work->col_done[pivot_col] = 1;

        /* Store U entries from pivot row using row list (O(nnz) instead of O(m)) */
        for (SparseEntry *re = work->rows[pivot_row]; re; re = re->next) {
            int j = re->idx;
            if (work->col_done[j] && j != pivot_col) continue;  /* Already eliminated */

            double val = re->val;
            if (fabs(val) < RALPH_ZERO_TOL) continue;  /* Skip zeros */

            /* Ensure capacity */
            if (U_nnz >= U_cap) {
                int new_cap = U_cap * 2;
                int *tmp_i = (int*)realloc(U_i, new_cap * sizeof(int));
                int *tmp_j = (int*)realloc(U_j, new_cap * sizeof(int));
                double *tmp_v = (double*)realloc(U_v, new_cap * sizeof(double));
                if (!tmp_i || !tmp_j || !tmp_v) {
                    free(tmp_i ? tmp_i : U_i);
                    free(tmp_j ? tmp_j : U_j);
                    free(tmp_v ? tmp_v : U_v);
                    free(L_i); free(L_j); free(L_v);
                    sparse_work_free(work);
                    free(col_order);
                    return LU_FAIL_FACTOR_ALLOC;
                }
                U_i = tmp_i; U_j = tmp_j; U_v = tmp_v;
                U_cap = new_cap;
            }

            U_i[U_nnz] = step;  /* Row in factored matrix */
            U_j[U_nnz] = j;     /* Original column */
            U_v[U_nnz] = val;
            U_nnz++;
        }

        /* Store L entries (multipliers) and eliminate */
        /* L diagonal is 1 (implicit) */
        if (L_nnz >= L_cap) {
            int new_cap = L_cap * 2;
            int *tmp_i = (int*)realloc(L_i, new_cap * sizeof(int));
            int *tmp_j = (int*)realloc(L_j, new_cap * sizeof(int));
            double *tmp_v = (double*)realloc(L_v, new_cap * sizeof(double));
            if (!tmp_i || !tmp_j || !tmp_v) {
                free(tmp_i ? tmp_i : L_i);
                free(tmp_j ? tmp_j : L_j);
                free(tmp_v ? tmp_v : L_v);
                free(U_i); free(U_j); free(U_v);
                sparse_work_free(work);
                free(col_order);
                return LU_FAIL_FACTOR_ALLOC;
            }
            L_i = tmp_i; L_j = tmp_j; L_v = tmp_v;
            L_cap = new_cap;
        }
        L_i[L_nnz] = pivot_row;  /* Original row (must match off-diagonal entries) */
        L_j[L_nnz] = step;
        L_v[L_nnz] = 1.0;
        L_nnz++;

        /* For each row with non-zero in pivot column (except pivot row) */
        for (SparseEntry *e = work->cols[pivot_col]; e; e = e->next) {
            int i = e->idx;
            if (i == pivot_row || work->row_done[i]) continue;

            double mult = e->val / pivot_val;

            /* Store L entry */
            if (L_nnz >= L_cap) {
                int new_cap = L_cap * 2;
                int *tmp_i = (int*)realloc(L_i, new_cap * sizeof(int));
                int *tmp_j = (int*)realloc(L_j, new_cap * sizeof(int));
                double *tmp_v = (double*)realloc(L_v, new_cap * sizeof(double));
                if (!tmp_i || !tmp_j || !tmp_v) {
                    free(tmp_i ? tmp_i : L_i);
                    free(tmp_j ? tmp_j : L_j);
                    free(tmp_v ? tmp_v : L_v);
                    free(U_i); free(U_j); free(U_v);
                    sparse_work_free(work);
                    free(col_order);
                    return LU_FAIL_FACTOR_ALLOC;
                }
                L_i = tmp_i; L_j = tmp_j; L_v = tmp_v;
                L_cap = new_cap;
            }
            L_i[L_nnz] = i;  /* Original row */
            L_j[L_nnz] = step;  /* Elimination step (column in L) */
            L_v[L_nnz] = mult;
            L_nnz++;

            /* Update row i: row[i] -= mult * row[pivot_row] */
            for (SparseEntry *pe = work->rows[pivot_row]; pe; pe = pe->next) {
                int j = pe->idx;
                if (work->col_done[j]) continue;

                double old_val = get_col_val(work, j, i);
                double new_val = old_val - mult * pe->val;
                set_val(work, j, i, new_val);
            }
        }

        /* Clear pivot column counts for remaining rows */
        work->col_nnz[pivot_col] = 0;
    }

    /* Build inverse permutations */
    for (int i = 0; i < m; i++) {
        work->row_perm_inv[work->row_perm[i]] = i;
        work->col_perm_inv[work->col_perm[i]] = i;
    }

    /* Convert L and U to CSC format with proper permutation */
    /* Free old storage */
    free(lu->L_colptr);
    free(lu->L_rowidx);
    free(lu->L_values);
    free(lu->U_colptr);
    free(lu->U_rowidx);
    free(lu->U_values);

    /* Allocate CSC arrays */
    lu->L_colptr = (int*)calloc((m + 1), sizeof(int));
    lu->L_rowidx = (int*)calloc(L_nnz, sizeof(int));
    lu->L_values = (double*)calloc(L_nnz, sizeof(double));
    lu->U_colptr = (int*)calloc((m + 1), sizeof(int));
    lu->U_rowidx = (int*)calloc(U_nnz, sizeof(int));
    lu->U_values = (double*)calloc(U_nnz, sizeof(double));

    if (!lu->L_colptr || !lu->L_rowidx || !lu->L_values ||
        !lu->U_colptr || !lu->U_rowidx || !lu->U_values) {
        free(L_i); free(L_j); free(L_v);
        free(U_i); free(U_j); free(U_v);
        sparse_work_free(work);
        return LU_FAIL_FACTOR_ALLOC;
    }

    /* Convert L to CSC (L is stored with step as column index) */
    /* Count entries per column */
    memset(lu->L_colptr, 0, (m + 1) * sizeof(int));
    for (int k = 0; k < L_nnz; k++) {
        lu->L_colptr[L_j[k] + 1]++;
    }
    for (int j = 0; j < m; j++) {
        lu->L_colptr[j + 1] += lu->L_colptr[j];
    }

    /* Fill entries */
    int *L_pos = (int*)calloc(m, sizeof(int));
    for (int k = 0; k < L_nnz; k++) {
        int j = L_j[k];
        int pos = lu->L_colptr[j] + L_pos[j]++;
        lu->L_rowidx[pos] = work->row_perm_inv[L_i[k]];  /* Convert to permuted row */
        lu->L_values[pos] = L_v[k];
    }
    free(L_pos);
    lu->nnz_L = L_nnz;

    /* Convert U to CSC (need to map original columns to step order) */
    memset(lu->U_colptr, 0, (m + 1) * sizeof(int));
    for (int k = 0; k < U_nnz; k++) {
        int orig_col = U_j[k];
        int step_col = work->col_perm_inv[orig_col];
        lu->U_colptr[step_col + 1]++;
    }
    for (int j = 0; j < m; j++) {
        lu->U_colptr[j + 1] += lu->U_colptr[j];
    }

    int *U_pos = (int*)calloc(m, sizeof(int));
    for (int k = 0; k < U_nnz; k++) {
        int orig_col = U_j[k];
        int step_col = work->col_perm_inv[orig_col];
        int pos = lu->U_colptr[step_col] + U_pos[step_col]++;
        /* Row is the step at which this entry's row was the pivot row */
        /* Since U entries come from pivot rows, row index = step */
        /* But we need to figure out which step this row was pivoted */
        /* Actually, U_i already has the step index */
        lu->U_rowidx[pos] = U_i[k];
        lu->U_values[pos] = U_v[k];
    }
    free(U_pos);
    lu->nnz_U = U_nnz;

    /* Copy permutations */
    memcpy(lu->perm, work->row_perm, m * sizeof(int));
    memcpy(lu->perm_inv, work->row_perm_inv, m * sizeof(int));
    memcpy(lu->col_perm, work->col_perm, m * sizeof(int));
    memcpy(lu->col_perm_inv, work->col_perm_inv, m * sizeof(int));

    lu_update_backend_reset(lu);

    lu->num_updates = 0;

    /* Extract U diagonals and compute condition number estimate */
    lu->min_diag_U = RALPH_INFINITY;
    lu->max_diag_U = 0.0;
    for (int j = 0; j < m; j++) {
        lu->U_diag[j] = 0.0;
        for (int p = lu->U_colptr[j]; p < lu->U_colptr[j + 1]; p++) {
            if (lu->U_rowidx[p] == j) {
                double val = lu->U_values[p];
                lu->U_diag[j] = val;
                double absval = fabs(val);
                if (absval < lu->min_diag_U) lu->min_diag_U = absval;
                if (absval > lu->max_diag_U) lu->max_diag_U = absval;
                break;
            }
        }
    }
    if (lu->min_diag_U > RALPH_ZERO_TOL) {
        lu->cond_estimate = lu->max_diag_U / lu->min_diag_U;
    } else {
        lu->cond_estimate = RALPH_INFINITY;
    }
    lu->growth_factor = 1.0;

    /* Cleanup */
    free(L_i); free(L_j); free(L_v);
    free(U_i); free(U_j); free(U_v);
    sparse_work_free(work);
    free(col_order);

    return 0;
}

/* ============================================================================
 * LP-Aware Sparse LU Factorization
 * ============================================================================
 *
 * Exploits LP basis structure for dramatically faster factorization:
 *
 * LP bases typically contain:
 * - Identity columns from slack variables (singleton with value ±1)
 * - Structural columns from the constraint matrix
 *
 * Key insight: If the basis has k structural columns and (m-k) identity columns,
 * we only need to factorize a k×k submatrix instead of the full m×m matrix.
 * This gives O(k³) complexity instead of O(m³).
 *
 * For typical LP problems with 20-50% structural columns:
 * - 20% structural → ~100x speedup (k=0.2m gives 0.008m³ vs m³)
 * - 50% structural → ~8x speedup (k=0.5m gives 0.125m³ vs m³)
 */

/* Forward declaration for cleanup function */
typedef struct LPBasisStructure LPBasisStructure;
static void free_lp_basis_structure(LPBasisStructure *lp);

/* Analysis result for LP basis structure */
struct LPBasisStructure {
    int *identity_cols;     /* Indices of identity columns in B */
    int *identity_rows;     /* Row where each identity col has its 1 */
    double *identity_vals;  /* Value (±1) at each identity position */
    int num_identity;

    int *structural_cols;   /* Indices of structural columns */
    int num_structural;

    int *row_is_identity;   /* For each row: 1 if covered by identity col, 0 otherwise */
    int *row_to_sub;        /* Map: original row -> submatrix row (-1 if identity) */
    int *sub_to_row;        /* Map: submatrix row -> original row */
    int *id_to_step;        /* Map: identity row -> its step in elimination (k+i) */

    /* Cross-terms B21: structural col entries in identity rows */
    int *B21_col;           /* Column index (structural column 0..k-1) */
    int *B21_row;           /* Row index (identity row as step k..m-1) */
    double *B21_val;        /* Value */
    int B21_nnz;            /* Number of cross-term entries */
    int B21_cap;            /* Capacity */
};

static LPBasisStructure* analyze_lp_basis(const SparseMatrix *B) {
    int m = B->nrows;

    LPBasisStructure *lp = (LPBasisStructure*)calloc(1, sizeof(LPBasisStructure));
    if (!lp) return NULL;

    lp->identity_cols = (int*)calloc(m, sizeof(int));
    lp->identity_rows = (int*)calloc(m, sizeof(int));
    lp->identity_vals = (double*)calloc(m, sizeof(double));
    lp->structural_cols = (int*)calloc(m, sizeof(int));
    lp->row_is_identity = (int*)calloc(m, sizeof(int));
    lp->row_to_sub = (int*)calloc(m, sizeof(int));
    lp->sub_to_row = (int*)calloc(m, sizeof(int));
    lp->id_to_step = (int*)calloc(m, sizeof(int));

    /* Initial capacity for cross-terms */
    lp->B21_cap = B->nnz / 4 + 16;
    lp->B21_col = (int*)calloc(lp->B21_cap, sizeof(int));
    lp->B21_row = (int*)calloc(lp->B21_cap, sizeof(int));
    lp->B21_val = (double*)calloc(lp->B21_cap, sizeof(double));
    lp->B21_nnz = 0;

    if (!lp->identity_cols || !lp->identity_rows || !lp->identity_vals ||
        !lp->structural_cols || !lp->row_is_identity ||
        !lp->row_to_sub || !lp->sub_to_row || !lp->id_to_step ||
        !lp->B21_col || !lp->B21_row || !lp->B21_val) {
        free(lp->identity_cols); free(lp->identity_rows); free(lp->identity_vals);
        free(lp->structural_cols); free(lp->row_is_identity);
        free(lp->row_to_sub); free(lp->sub_to_row); free(lp->id_to_step);
        free(lp->B21_col); free(lp->B21_row); free(lp->B21_val);
        free(lp);
        return NULL;
    }

    for (int i = 0; i < m; i++) {
        lp->row_to_sub[i] = -1;
        lp->id_to_step[i] = -1;
    }

    /* Classify columns: identity (singleton ±1) vs structural */
    for (int j = 0; j < m; j++) {
        int nnz = B->colptr[j + 1] - B->colptr[j];

        if (nnz == 1) {
            int p = B->colptr[j];
            int row = B->rowidx[p];
            double val = B->values[p];

            /* Identity column: exactly one entry with |value| = 1, row not yet claimed */
            if (fabs(fabs(val) - 1.0) < RALPH_ZERO_TOL && !lp->row_is_identity[row]) {
                lp->identity_cols[lp->num_identity] = j;
                lp->identity_rows[lp->num_identity] = row;
                lp->identity_vals[lp->num_identity] = val;
                lp->row_is_identity[row] = 1;
                lp->num_identity++;
                continue;
            }
        }

        /* Not an identity column - it's structural */
        lp->structural_cols[lp->num_structural++] = j;
    }

    /* Build row mapping: non-identity rows -> submatrix indices */
    int sub_idx = 0;
    for (int i = 0; i < m; i++) {
        if (!lp->row_is_identity[i]) {
            lp->row_to_sub[i] = sub_idx;
            lp->sub_to_row[sub_idx] = i;
            sub_idx++;
        }
    }

    /* Build id_to_step mapping for identity rows */
    int k = lp->num_structural;
    for (int i = 0; i < lp->num_identity; i++) {
        int row = lp->identity_rows[i];
        lp->id_to_step[row] = k + i;  /* Identity rows come after structural */
    }

    /* Capture cross-terms: structural columns with entries in identity rows */
    /* These go into B21 for Schur complement computation */
    /* Also check that each structural column has at least one entry in B11 */
    int *structural_has_b11_entry = (int*)calloc(lp->num_structural, sizeof(int));
    if (!structural_has_b11_entry) {
        free_lp_basis_structure(lp);
        return NULL;
    }

    for (int jj = 0; jj < lp->num_structural; jj++) {
        int j = lp->structural_cols[jj];
        for (int p = B->colptr[j]; p < B->colptr[j + 1]; p++) {
            int row = B->rowidx[p];
            double val = B->values[p];
            if (fabs(val) < RALPH_ZERO_TOL) continue;

            if (lp->row_is_identity[row]) {
                /* This is a cross-term: structural col jj has entry in identity row */
                if (lp->B21_nnz >= lp->B21_cap) {
                    int new_cap = lp->B21_cap * 2;
                    int *tmp_col = (int*)realloc(lp->B21_col, new_cap * sizeof(int));
                    int *tmp_row = (int*)realloc(lp->B21_row, new_cap * sizeof(int));
                    double *tmp_val = (double*)realloc(lp->B21_val, new_cap * sizeof(double));
                    if (!tmp_col || !tmp_row || !tmp_val) {
                        /* On failure, keep valid pointers where possible */
                        if (tmp_col) lp->B21_col = tmp_col;
                        if (tmp_row) lp->B21_row = tmp_row;
                        if (tmp_val) lp->B21_val = tmp_val;
                        free(structural_has_b11_entry);
                        free_lp_basis_structure(lp);
                        return NULL;
                    }
                    lp->B21_col = tmp_col;
                    lp->B21_row = tmp_row;
                    lp->B21_val = tmp_val;
                    lp->B21_cap = new_cap;
                }
                lp->B21_col[lp->B21_nnz] = jj;  /* Structural column index (0..k-1) */
                lp->B21_row[lp->B21_nnz] = lp->id_to_step[row];  /* Step index (k..m-1) */
                lp->B21_val[lp->B21_nnz] = val;
                lp->B21_nnz++;
            } else {
                /* Entry in non-identity row - goes to B11 */
                structural_has_b11_entry[jj] = 1;
            }
        }
    }

    /* Check that all structural columns have at least one B11 entry */
    /* If any structural column has all entries in identity rows, B11 is singular */
    for (int jj = 0; jj < lp->num_structural; jj++) {
        if (!structural_has_b11_entry[jj]) {
            /* Degenerate case: structural column has no B11 entries */
            free(structural_has_b11_entry);
            free_lp_basis_structure(lp);
            return NULL;
        }
    }
    free(structural_has_b11_entry);

    return lp;
}

static void free_lp_basis_structure(LPBasisStructure *lp) {
    if (!lp) return;
    free(lp->identity_cols);
    free(lp->identity_rows);
    free(lp->identity_vals);
    free(lp->structural_cols);
    free(lp->row_is_identity);
    free(lp->row_to_sub);
    free(lp->sub_to_row);
    free(lp->id_to_step);
    free(lp->B21_col);
    free(lp->B21_row);
    free(lp->B21_val);
    free(lp);
}

/*
 * LP-Aware LU Factorization (T1.4 Full: Symbolic/Numeric Separation)
 *
 * Split into three functions:
 * 1. lu_symbolic_analyze()  — identity detection, fill-reducing column ordering,
 *                             FNV-1a fingerprint caching (pattern-only, cacheable)
 * 2. lu_numeric_factorize() — dense GE with partial pivoting, COO→CSC conversion,
 *                             condition estimation (value-dependent, always runs)
 * 3. lu_factorize_sparse_efficient() — thin wrapper calling symbolic then numeric
 *
 * Fill-reducing sort: structural columns sorted by nnz (sparsest first) reduces
 * fill-in during dense GE, producing sparser L/U factors. Sparser L/U directly
 * speeds FTRAN/BTRAN (called 3+ times per simplex iteration).
 *
 * Symbolic caching: the symbolic analysis depends only on the sparsity pattern.
 * When the basis changes by a single pivot (typical), the pattern often stays
 * the same. FNV-1a fingerprint detects pattern changes cheaply.
 */

/* FNV-1a hash constants for 64-bit */
#define FNV_OFFSET_BASIS 0xcbf29ce484222325ULL
#define FNV_PRIME         0x100000001b3ULL

static int lu_requested_factorization_type(const LUFactorization *lu) {
    if (!lu || !lu->owner) return LP_GLPK_BFCP_FACTORIZATION_LUF;
    return (lu->owner->lu_factorization_type == LP_GLPK_BFCP_FACTORIZATION_BTF)
        ? LP_GLPK_BFCP_FACTORIZATION_BTF
        : LP_GLPK_BFCP_FACTORIZATION_LUF;
}

/* Augmenting-path matcher used by symbolic identity/structural split.
 * row_used[row] == 1 means identity row (forbidden for structural matching). */
static int symbolic_match_col(const SparseMatrix *B,
                              int col,
                              const int *row_used,
                              int *row_match_col,
                              int *row_seen,
                              int seen_token) {
    for (int p = B->colptr[col]; p < B->colptr[col + 1]; p++) {
        int row = B->rowidx[p];
        if (row_used[row]) continue;
        if (row_seen[row] == seen_token) continue;
        row_seen[row] = seen_token;

        int prev_col = row_match_col[row];
        if (prev_col < 0 ||
            symbolic_match_col(B, prev_col, row_used, row_match_col, row_seen, seen_token)) {
            row_match_col[row] = col;
            return 1;
        }
    }
    return 0;
}

typedef struct {
    int k;
    const int *adj_start;
    const int *adj_list;
    int *disc;
    int *low;
    int *stack;
    unsigned char *on_stack;
    int *node_scc;
    int next_disc;
    int stack_top;
    int scc_count;
} BTFSCCContext;

static int btf_scc_visit(BTFSCCContext *ctx, int v) {
    ctx->disc[v] = ctx->next_disc;
    ctx->low[v] = ctx->next_disc;
    ctx->next_disc++;
    ctx->stack[ctx->stack_top++] = v;
    ctx->on_stack[v] = 1;

    for (int p = ctx->adj_start[v]; p < ctx->adj_start[v + 1]; p++) {
        int w = ctx->adj_list[p];
        if (ctx->disc[w] < 0) {
            if (btf_scc_visit(ctx, w) != 0) return -1;
            if (ctx->low[w] < ctx->low[v]) ctx->low[v] = ctx->low[w];
        } else if (ctx->on_stack[w] && ctx->disc[w] < ctx->low[v]) {
            ctx->low[v] = ctx->disc[w];
        }
    }

    if (ctx->low[v] == ctx->disc[v]) {
        for (;;) {
            int w;
            if (ctx->stack_top <= 0) return -1;
            w = ctx->stack[--ctx->stack_top];
            ctx->on_stack[w] = 0;
            ctx->node_scc[w] = ctx->scc_count;
            if (w == v) break;
        }
        ctx->scc_count++;
    }

    return 0;
}

static int lu_symbolic_apply_btf_ordering(const SparseMatrix *B,
                                          int m,
                                          int k,
                                          const int *is_identity_col,
                                          const int *row_used,
                                          const int *row_match_col,
                                          int *col_order,
                                          int *col_order_inv,
                                          int *btf_blocks_out) {
    int *struct_cols = NULL;
    int *col_to_struct = NULL;
    int *matched_row = NULL;
    int *row_ptr = NULL;
    int *row_cols = NULL;
    int *row_fill = NULL;
    int *edge_count = NULL;
    int *adj_start = NULL;
    int *adj_list = NULL;
    int *seen_struct = NULL;
    int *disc = NULL;
    int *low = NULL;
    int *stack = NULL;
    unsigned char *on_stack = NULL;
    int *node_scc = NULL;
    int *scc_edge_count = NULL;
    int *scc_adj_start = NULL;
    int *scc_adj_list = NULL;
    int *scc_indegree = NULL;
    int *queue = NULL;
    int *topo = NULL;
    int *temp_order = NULL;
    int total_edges = 0;
    int total_scc_edges = 0;
    int btf_blocks = 0;
    int rc = LU_SYMBOLIC_FAIL_WORKSPACE;

    if (btf_blocks_out) *btf_blocks_out = 0;
    if (k <= 0) {
        if (btf_blocks_out) *btf_blocks_out = 0;
        return 0;
    }

    struct_cols = (int *)calloc((size_t)k, sizeof(int));
    col_to_struct = (int *)calloc((size_t)m, sizeof(int));
    matched_row = (int *)calloc((size_t)k, sizeof(int));
    row_ptr = (int *)calloc((size_t)m + 1u, sizeof(int));
    row_cols = (int *)calloc((size_t)B->nnz, sizeof(int));
    row_fill = (int *)calloc((size_t)m, sizeof(int));
    edge_count = (int *)calloc((size_t)k, sizeof(int));
    seen_struct = (int *)calloc((size_t)k, sizeof(int));
    disc = (int *)calloc((size_t)k, sizeof(int));
    low = (int *)calloc((size_t)k, sizeof(int));
    stack = (int *)calloc((size_t)k, sizeof(int));
    on_stack = (unsigned char *)calloc((size_t)k, sizeof(unsigned char));
    node_scc = (int *)calloc((size_t)k, sizeof(int));
    temp_order = (int *)calloc((size_t)k, sizeof(int));
    if (!struct_cols || !col_to_struct || !matched_row || !row_ptr || !row_cols ||
        !row_fill || !edge_count || !seen_struct || !disc || !low || !stack ||
        !on_stack || !node_scc || !temp_order) {
        goto cleanup;
    }

    for (int i = 0; i < m; i++) col_to_struct[i] = -1;
    for (int i = 0; i < k; i++) {
        int orig_col = col_order[i];
        struct_cols[i] = orig_col;
        if (orig_col < 0 || orig_col >= m) {
            rc = LU_SYMBOLIC_FAIL_INCONSISTENT_IDENTITY;
            goto cleanup;
        }
        col_to_struct[orig_col] = i;
        matched_row[i] = -1;
        disc[i] = -1;
        node_scc[i] = -1;
        seen_struct[i] = -1;
    }

    for (int j = 0; j < B->ncols; j++) {
        for (int p = B->colptr[j]; p < B->colptr[j + 1]; p++) {
            row_ptr[B->rowidx[p] + 1]++;
        }
    }
    for (int i = 0; i < m; i++) {
        row_ptr[i + 1] += row_ptr[i];
    }
    for (int j = 0; j < B->ncols; j++) {
        for (int p = B->colptr[j]; p < B->colptr[j + 1]; p++) {
            int row = B->rowidx[p];
            row_cols[row_ptr[row] + row_fill[row]++] = j;
        }
    }

    for (int row = 0; row < m; row++) {
        int matched_col;
        int struct_pos;
        if (row_used[row]) continue;
        matched_col = row_match_col[row];
        if (matched_col < 0 || matched_col >= m || is_identity_col[matched_col]) {
            rc = LU_SYMBOLIC_FAIL_UNMATCHED_NO_RESERVED;
            goto cleanup;
        }
        struct_pos = col_to_struct[matched_col];
        if (struct_pos < 0 || struct_pos >= k) {
            rc = LU_SYMBOLIC_FAIL_INCONSISTENT_IDENTITY;
            goto cleanup;
        }
        matched_row[struct_pos] = row;
    }
    for (int s = 0; s < k; s++) {
        if (matched_row[s] < 0) {
            rc = LU_SYMBOLIC_FAIL_UNMATCHED_NO_RESERVED;
            goto cleanup;
        }
    }

    for (int s = 0; s < k; s++) {
        int row = matched_row[s];
        int token = s;
        for (int p = row_ptr[row]; p < row_ptr[row + 1]; p++) {
            int orig_col = row_cols[p];
            int to;
            if (orig_col < 0 || orig_col >= m || is_identity_col[orig_col]) continue;
            to = col_to_struct[orig_col];
            if (to < 0 || to == s) continue;
            if (seen_struct[to] == token) continue;
            seen_struct[to] = token;
            edge_count[s]++;
            total_edges++;
        }
    }

    adj_start = (int *)calloc((size_t)k + 1u, sizeof(int));
    if (!adj_start) goto cleanup;
    for (int s = 0; s < k; s++) {
        adj_start[s + 1] = adj_start[s] + edge_count[s];
    }
    adj_list = (int *)calloc((size_t)(total_edges > 0 ? total_edges : 1), sizeof(int));
    if (!adj_list) goto cleanup;

    memset(edge_count, 0, (size_t)k * sizeof(int));
    for (int s = 0; s < k; s++) {
        int row = matched_row[s];
        int token = s + k;
        for (int p = row_ptr[row]; p < row_ptr[row + 1]; p++) {
            int orig_col = row_cols[p];
            int to;
            if (orig_col < 0 || orig_col >= m || is_identity_col[orig_col]) continue;
            to = col_to_struct[orig_col];
            if (to < 0 || to == s) continue;
            if (seen_struct[to] == token) continue;
            seen_struct[to] = token;
            adj_list[adj_start[s] + edge_count[s]++] = to;
        }
    }

    {
        BTFSCCContext ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.k = k;
        ctx.adj_start = adj_start;
        ctx.adj_list = adj_list;
        ctx.disc = disc;
        ctx.low = low;
        ctx.stack = stack;
        ctx.on_stack = on_stack;
        ctx.node_scc = node_scc;

        for (int v = 0; v < k; v++) {
            if (ctx.disc[v] < 0 && btf_scc_visit(&ctx, v) != 0) {
                goto cleanup;
            }
        }
        btf_blocks = ctx.scc_count;
    }

    if (btf_blocks <= 0) {
        rc = LU_SYMBOLIC_FAIL_WORKSPACE;
        goto cleanup;
    }

    scc_edge_count = (int *)calloc((size_t)btf_blocks, sizeof(int));
    scc_adj_start = (int *)calloc((size_t)btf_blocks + 1u, sizeof(int));
    scc_indegree = (int *)calloc((size_t)btf_blocks, sizeof(int));
    queue = (int *)calloc((size_t)btf_blocks, sizeof(int));
    topo = (int *)calloc((size_t)btf_blocks, sizeof(int));
    if (!scc_edge_count || !scc_adj_start || !scc_indegree || !queue || !topo) {
        goto cleanup;
    }

    for (int s = 0; s < k; s++) {
        int src_scc = node_scc[s];
        for (int p = adj_start[s]; p < adj_start[s + 1]; p++) {
            int dst_scc = node_scc[adj_list[p]];
            if (dst_scc == src_scc) continue;
            scc_edge_count[src_scc]++;
            total_scc_edges++;
            scc_indegree[dst_scc]++;
        }
    }
    for (int c = 0; c < btf_blocks; c++) {
        scc_adj_start[c + 1] = scc_adj_start[c] + scc_edge_count[c];
    }
    scc_adj_list = (int *)calloc((size_t)(total_scc_edges > 0 ? total_scc_edges : 1), sizeof(int));
    if (!scc_adj_list) goto cleanup;

    memset(scc_edge_count, 0, (size_t)btf_blocks * sizeof(int));
    for (int s = 0; s < k; s++) {
        int src_scc = node_scc[s];
        for (int p = adj_start[s]; p < adj_start[s + 1]; p++) {
            int dst_scc = node_scc[adj_list[p]];
            if (dst_scc == src_scc) continue;
            scc_adj_list[scc_adj_start[src_scc] + scc_edge_count[src_scc]++] = dst_scc;
        }
    }

    {
        int q_head = 0;
        int q_tail = 0;
        int topo_n = 0;
        for (int c = 0; c < btf_blocks; c++) {
            if (scc_indegree[c] == 0) queue[q_tail++] = c;
        }
        while (q_head < q_tail) {
            int src = queue[q_head++];
            topo[topo_n++] = src;
            for (int p = scc_adj_start[src]; p < scc_adj_start[src + 1]; p++) {
                int dst = scc_adj_list[p];
                scc_indegree[dst]--;
                if (scc_indegree[dst] == 0) {
                    queue[q_tail++] = dst;
                }
            }
        }
        if (topo_n != btf_blocks) {
            rc = LU_SYMBOLIC_FAIL_WORKSPACE;
            goto cleanup;
        }
    }

    {
        int out = 0;
        for (int t = 0; t < btf_blocks; t++) {
            int scc = topo[t];
            for (int s = 0; s < k; s++) {
                if (node_scc[s] == scc) {
                    temp_order[out++] = struct_cols[s];
                }
            }
        }
        if (out != k) {
            rc = LU_SYMBOLIC_FAIL_WORKSPACE;
            goto cleanup;
        }
        for (int s = 0; s < k; s++) {
            col_order[s] = temp_order[s];
        }
        for (int j = 0; j < m; j++) {
            col_order_inv[col_order[j]] = j;
        }
    }

    if (btf_blocks_out) *btf_blocks_out = btf_blocks;
    rc = 0;

cleanup:
    free(struct_cols);
    free(col_to_struct);
    free(matched_row);
    free(row_ptr);
    free(row_cols);
    free(row_fill);
    free(edge_count);
    free(adj_start);
    free(adj_list);
    free(seen_struct);
    free(disc);
    free(low);
    free(stack);
    free(on_stack);
    free(node_scc);
    free(scc_edge_count);
    free(scc_adj_start);
    free(scc_adj_list);
    free(scc_indegree);
    free(queue);
    free(topo);
    free(temp_order);
    return rc;
}

/* Finalize symbolic plan as full-structural (k=m, no identity placement).
 * Used for both normal k=m path and symbolic-failure retry path. */
static int lu_symbolic_finalize_full_structural(LUFactorization *lu,
                                                const SparseMatrix *B) {
    int m = lu->m;
    int *struct_nnz = lu->ws_struct_nnz;
    int *is_identity_col = lu->ws_is_identity;
    int *row_used = lu->ws_row_used;
    int *row_identity_col = lu->ws_row_identity_col;
    int *row_match_col = lu->ws_row_match_col;
    int *row_seen = lu->ws_row_seen;
    int *col_order = lu->ws_col_order;
    int *col_order_inv = lu->ws_col_order_inv;
    int factorization_type = lu_requested_factorization_type(lu);
    int btf_blocks = 0;
    uint64_t fingerprint = FNV_OFFSET_BASIS;

    if (!struct_nnz || !is_identity_col || !row_used || !row_identity_col ||
        !row_match_col || !row_seen || !col_order || !col_order_inv) {
        return -1;
    }

    memset(is_identity_col, 0, m * sizeof(int));
    memset(row_used, 0, m * sizeof(int));
    for (int i = 0; i < m; i++) {
        row_identity_col[i] = -1;
    }

    for (int j = 0; j < m; j++) {
        int nnz = B->colptr[j + 1] - B->colptr[j];
        struct_nnz[j] = nnz;
        fingerprint ^= (uint64_t)nnz;
        fingerprint *= FNV_PRIME;
    }

    if (lu->sym_valid &&
        lu->sym_fingerprint == fingerprint &&
        lu->sym_num_identity == 0 &&
        lu->sym_k == m &&
        lu->sym_factorization_type == factorization_type) {
        lp_telemetry_lu_record_symbolic_cache_hit(lu);
        return 0;
    }
    lp_telemetry_lu_record_symbolic_cache_miss(lu);

    for (int j = 0; j < m; j++) {
        col_order[j] = j;
        col_order_inv[j] = j;
    }
    if (factorization_type == LP_GLPK_BFCP_FACTORIZATION_BTF) {
        for (int i = 0; i < m; i++) {
            row_match_col[i] = -1;
            row_seen[i] = 0;
        }
        for (int j = 0; j < m; j++) {
            if (!symbolic_match_col(B, j, row_used, row_match_col, row_seen, j + 1)) {
                return -1;
            }
        }
        if (lu_symbolic_apply_btf_ordering(B, m, m, is_identity_col, row_used,
                                           row_match_col, col_order, col_order_inv,
                                           &btf_blocks) != 0) {
            return -1;
        }
    }
    lu->sym_valid = 1;
    lu->sym_num_identity = 0;
    lu->sym_k = m;
    lu->sym_fingerprint = fingerprint;
    lu->sym_factorization_type = factorization_type;
    lu->sym_btf_blocks = btf_blocks;
    return 0;
}

/*
 * Symbolic analysis: identity detection + fill-reducing column ordering.
 * Populates ws_is_identity, ws_identity_row, ws_identity_val, ws_row_used,
 * ws_col_order, ws_col_order_inv, ws_struct_nnz, sym_num_identity, sym_k.
 *
 * Returns 0 on success, negative LUSymbolicFailureReason on failure.
 */
static int lu_symbolic_analyze(LUFactorization *lu, const SparseMatrix *B) {
    int m = lu->m;
    int factorization_type = lu_requested_factorization_type(lu);

    /* Compute column nnz counts + identity/structural split */
    int *struct_nnz = lu->ws_struct_nnz;

    int *is_identity_col = lu->ws_is_identity;
    int *identity_row = lu->ws_identity_row;
    double *identity_val = lu->ws_identity_val;
    int *row_used = lu->ws_row_used;
    int *row_identity_col = lu->ws_row_identity_col;
    int *row_match_col = lu->ws_row_match_col;
    int *row_seen = lu->ws_row_seen;
    int *col_order = lu->ws_col_order;
    int *col_order_inv = lu->ws_col_order_inv;
    if (!row_identity_col || !row_match_col || !row_seen) {
        return LU_SYMBOLIC_FAIL_WORKSPACE;
    }
    memset(is_identity_col, 0, m * sizeof(int));
    memset(row_used, 0, m * sizeof(int));
    for (int i = 0; i < m; i++) {
        row_identity_col[i] = -1;
    }

    int num_identity = 0;
    uint64_t initial_fingerprint = FNV_OFFSET_BASIS;
    for (int j = 0; j < m; j++) {
        int nnz = B->colptr[j + 1] - B->colptr[j];
        struct_nnz[j] = nnz;
        initial_fingerprint ^= (uint64_t)nnz;
        initial_fingerprint *= FNV_PRIME;

        if (nnz == 1) {
            int p = B->colptr[j];
            int row = B->rowidx[p];
            double val = B->values[p];
            if (fabs(fabs(val) - 1.0) < RALPH_ZERO_TOL && !row_used[row]) {
                is_identity_col[j] = 1;
                identity_row[j] = row;
                identity_val[j] = val;
                row_used[row] = 1;
                row_identity_col[row] = j;
                num_identity++;
                initial_fingerprint ^= (uint64_t)row;
                initial_fingerprint *= FNV_PRIME;
            }
        }
    }

    /* Full-structural basis (k=m): no identity placement needed.
     * Keep sparse path eligible and avoid unnecessary symbolic fallback. */
    if (num_identity == 0) {
        return lu_symbolic_finalize_full_structural(lu, B) == 0
            ? 0
            : LU_SYMBOLIC_FAIL_WORKSPACE;
    }

    /* If the cheap identity scan exactly reproduces the cached split, the
     * matching/BTF symbolic plan is still valid and can be reused directly. */
    if (lu->sym_valid &&
        lu->sym_fingerprint == initial_fingerprint &&
        lu->sym_num_identity == num_identity &&
        lu->sym_factorization_type == factorization_type) {
        lp_telemetry_lu_record_symbolic_cache_hit(lu);
        return 0;
    }

    /* Ensure structural columns can be matched to non-identity rows.
     * If matching fails, demote one conflicting identity row and retry. */

    for (;;) {
        for (int i = 0; i < m; i++) {
            row_match_col[i] = -1;
            row_seen[i] = 0;
        }
        for (int j = 0; j < m; j++) {
            col_order_inv[j] = is_identity_col[j] ? 1 : 0;
        }
        for (int j = 0; j < m; j++) {
            if (is_identity_col[j]) continue;
            for (int p = B->colptr[j]; p < B->colptr[j + 1]; p++) {
                int row = B->rowidx[p];
                if (row_used[row] || row_match_col[row] >= 0) continue;
                row_match_col[row] = j;
                col_order_inv[j] = 1;
                break;
            }
        }

        int seen_token = 1;
        int unmatched_col = -1;
        for (int j = 0; j < m; j++) {
            if (is_identity_col[j]) continue;
            if (col_order_inv[j]) continue;
            if (!symbolic_match_col(B, j, row_used, row_match_col, row_seen, seen_token++)) {
                unmatched_col = j;
                break;
            }
            col_order_inv[j] = 1;
        }
        if (unmatched_col < 0) {
            break;
        }

        int candidate_identity_row = -1;
        for (int p = B->colptr[unmatched_col]; p < B->colptr[unmatched_col + 1]; p++) {
            int row = B->rowidx[p];
            if (row_used[row]) {
                candidate_identity_row = row;
                break;
            }
        }
        if (candidate_identity_row < 0) {
            return LU_SYMBOLIC_FAIL_UNMATCHED_NO_RESERVED;
        }

        int id_col = row_identity_col[candidate_identity_row];
        if (id_col < 0 || !is_identity_col[id_col]) {
            return LU_SYMBOLIC_FAIL_INCONSISTENT_IDENTITY;
        }

        is_identity_col[id_col] = 0;
        row_used[candidate_identity_row] = 0;
        row_identity_col[candidate_identity_row] = -1;
        num_identity--;
    }

    uint64_t fingerprint = FNV_OFFSET_BASIS;
    for (int j = 0; j < m; j++) {
        int nnz = struct_nnz[j];
        fingerprint ^= (uint64_t)nnz;
        fingerprint *= FNV_PRIME;
        if (is_identity_col[j]) {
            fingerprint ^= (uint64_t)identity_row[j];
            fingerprint *= FNV_PRIME;
        }
    }

    /* Check symbolic cache: if fingerprint matches, reuse previous analysis */
    if (lu->sym_valid &&
        lu->sym_fingerprint == fingerprint &&
        lu->sym_factorization_type == factorization_type) {
        lp_telemetry_lu_record_symbolic_cache_hit(lu);
        return 0;  /* Cache hit — ws arrays still valid from last call */
    }
    lp_telemetry_lu_record_symbolic_cache_miss(lu);

    int k = m - num_identity;  /* Number of structural columns */

    /* Build column ordering: structural first (sorted by nnz), then identity */
    /* Collect structural columns into col_order[0..k-1] */
    int struct_idx = 0, ident_idx = k;
    for (int j = 0; j < m; j++) {
        if (is_identity_col[j]) {
            col_order[ident_idx] = j;
            col_order_inv[j] = ident_idx;
            ident_idx++;
        } else {
            col_order[struct_idx] = j;
            col_order_inv[j] = struct_idx;
            struct_idx++;
        }
    }

    if (factorization_type == LP_GLPK_BFCP_FACTORIZATION_BTF) {
        int btf_blocks = 0;
        int btf_rc = lu_symbolic_apply_btf_ordering(
            B, m, k, is_identity_col, row_used, row_match_col,
            col_order, col_order_inv, &btf_blocks);
        if (btf_rc != 0) {
            return btf_rc;
        }
        lu->sym_btf_blocks = btf_blocks;
    } else {
        lu->sym_btf_blocks = 0;
    }

    /* NOTE: Fill-reducing sort (sorting structural columns by nnz ascending) is
     * disabled for now. While it reduces fill-in during dense GE, it changes the
     * partial pivoting row selection, which causes numerical regressions on
     * sensitive problems (grow7, beaconfd). The sort can be re-enabled once
     * a stability-aware ordering (e.g., threshold-based or AMD) is implemented.
     * The infrastructure (ws_struct_nnz, fingerprint) is in place for that. */

    /* Save symbolic results */
    lu->sym_valid = 1;
    lu->sym_num_identity = num_identity;
    lu->sym_k = k;
    lu->sym_fingerprint = fingerprint;
    lu->sym_factorization_type = factorization_type;

    return 0;
}

/* ============================================================================
 * Sparse Markowitz LU Factorization
 *
 * Replaces the dense O(k³) GE inner loop with sparse Markowitz factorization.
 * Uses the GLPK/GLOP scatter/gather pattern:
 *   - Dual storage: both row-indexed and column-indexed SVA pools
 *   - Dense flag[] + work[] arrays for O(1) entry lookup in elimination
 *   - Degree-bucketed doubly-linked lists for pivot selection
 *   - Dense phase switch when active submatrix density > 70%
 *
 * Total work: O(nnz × fill) instead of O(k³).
 * ============================================================================ */

#define MARKOWITZ_MIN_K       40    /* Below this, dense GE is faster */
#define MARKOWITZ_THRESHOLD   0.1   /* Threshold pivoting ratio */
#define MARKOWITZ_MAX_SEARCH  2     /* Candidates per degree bucket */
#define MARKOWITZ_SINGULAR_RETRY_THRESHOLD 0.02 /* Relaxed threshold for one singular micro-retry */
#define MARKOWITZ_RESERVED_RELAX_RATIO 0.1 /* Keep non-reserved if within 10x of reserved best */
#define MARKOWITZ_RETRY_THRESHOLD 0.02 /* Secondary retry profile threshold ratio */
#define MARKOWITZ_RETRY_MAX_SEARCH 8   /* Secondary retry profile candidate budget */

/* N1-B: Shadow stability retry — only fires when primary Markowitz succeeds
 * but produces a poorly-conditioned U factor. Overwrites the primary result. */
#define MKZ_SHADOW_COND_TRIGGER  1e8   /* Trigger shadow when cond > this */
#define MKZ_SHADOW_THRESHOLD     0.3   /* Tighter pivot threshold for shadow */
#define MKZ_SHADOW_MAX_SEARCH    6     /* Wider candidate search for shadow */
#define MARKOWITZ_RETRY_SINGULAR_THRESHOLD 0.005 /* Retry profile singular scan threshold */
#define MARKOWITZ_RETRY_RESERVED_RELAX_RATIO 0.05 /* Retry profile reserved-row relax */
#define MARKOWITZ_CIRCUIT_BAD_STREAK 3 /* Trip breaker after this many bad outcomes */
#define MARKOWITZ_CIRCUIT_SKIP_BUDGET 128 /* Skip this many same-structure Markowitz attempts */
#define MARKOWITZ_GLOBAL_SINGULAR_BAD_STREAK 6 /* Trip global skip after this many singular outcomes */
#define MARKOWITZ_GLOBAL_SKIP_BUDGET 192 /* Skip this many Markowitz attempts globally after chronic singulars */
#define MARKOWITZ_FILL_GAP    8     /* Extra slots per column/row for fill-in */
#define MARKOWITZ_POOL_MULT   4     /* Pool = MULT × initial nnz */
#define MARKOWITZ_POOL_RETRY_MULT 8 /* Legacy retry multiplier (first growth target) */
#define MARKOWITZ_POOL_MAX_MULT 64  /* Upper bound for progressive pool growth */
#define MARKOWITZ_DENSE_SWITCH 0.7  /* Switch to dense when density exceeds this */

static int mkz_count_init_nnz(const SparseMatrix *B, const int *col_order, int k) {
    int init_nnz = 0;
    for (int jj = 0; jj < k; jj++) {
        int j = col_order[jj];
        for (int p = B->colptr[j]; p < B->colptr[j + 1]; p++) {
            if (fabs(B->values[p]) > RALPH_ZERO_TOL) {
                init_nnz++;
            }
        }
    }
    return init_nnz;
}

static int mkz_compute_workspace_requirements(int init_nnz, int m, int k, int pool_mult,
                                              int *pool_cap_out, size_t *need_doubles_out) {
    if (!pool_cap_out || !need_doubles_out || m <= 0 || k < 0) {
        return -1;
    }

    if (pool_mult < 1) pool_mult = MARKOWITZ_POOL_MULT;

    size_t pool_cap = (size_t)init_nnz * (size_t)pool_mult;
    size_t min_pool = (size_t)k * (size_t)MARKOWITZ_FILL_GAP * 2u;
    if (pool_cap < min_pool) pool_cap = min_pool;
    if (pool_cap > (size_t)INT_MAX) return -1;

    size_t dbl_need = 2u * pool_cap + (size_t)k + (size_t)k + (size_t)k;
    size_t int_count = 4u * pool_cap + 7u * (size_t)k + 3u * (size_t)m
                     + (size_t)k + (size_t)k + (size_t)m + (size_t)k + (size_t)m
                     + (size_t)k + 1u + 2u * (size_t)k
                     + 2u * ((size_t)k + 1u)
                     + ((size_t)k + 1u) + 2u * (size_t)m;

    if (int_count > (((size_t)-1) - (sizeof(double) - 1u)) / sizeof(int)) {
        return -1;
    }
    size_t int_doubles = (int_count * sizeof(int) + sizeof(double) - 1u) / sizeof(double);
    if (dbl_need > ((size_t)-1) - int_doubles) {
        return -1;
    }

    *pool_cap_out = (int)pool_cap;
    *need_doubles_out = dbl_need + int_doubles;
    return 0;
}

static int mkz_workspace_reserve(LUFactorization *lu, size_t need_doubles) {
    if (!lu) return -1;
    if (need_doubles <= lu->mkz_work_capacity) return 0;

    size_t new_cap = lu->mkz_work_capacity ? lu->mkz_work_capacity : 1024u;
    while (new_cap < need_doubles) {
        if (new_cap > ((size_t)-1) / 2u) {
            new_cap = need_doubles;
            break;
        }
        new_cap *= 2u;
    }

    double *new_work = (double *)realloc(lu->mkz_work, new_cap * sizeof(double));
    if (!new_work && new_cap != need_doubles) {
        new_cap = need_doubles;
        new_work = (double *)realloc(lu->mkz_work, new_cap * sizeof(double));
    }
    if (!new_work) return -1;

    lu->mkz_work = new_work;
    lu->mkz_work_capacity = new_cap;
    return 0;
}

static int sn_workspace_reserve(LUFactorization *lu, size_t need_doubles) {
    if (!lu) return -1;
    if (need_doubles <= lu->sn_work_capacity) return 0;

    size_t new_cap = lu->sn_work_capacity ? lu->sn_work_capacity : 1024u;
    while (new_cap < need_doubles) {
        if (new_cap > ((size_t)-1) / 2u) {
            new_cap = need_doubles;
            break;
        }
        new_cap *= 2u;
    }

    double *new_work = (double *)realloc(lu->sn_work, new_cap * sizeof(double));
    if (!new_work && new_cap != need_doubles) {
        new_cap = need_doubles;
        new_work = (double *)realloc(lu->sn_work, new_cap * sizeof(double));
    }
    if (!new_work) return -1;

    lu->sn_work = new_work;
    lu->sn_work_capacity = new_cap;
    return 0;
}

static void mkz_record_failure_reason(LUFactorization *lu, int rc) {
    lp_telemetry_lu_mark_mkz_failure_reason(lu, rc);
}

/* Structure-local Markowitz circuit breaker.
 * If Markowitz repeatedly gives bad outcomes for the same symbolic fingerprint,
 * skip Markowitz for a bounded number of future refactorizations so sparse
 * numeric can go directly to the next path (supernode or dense-GE). */
static int mkz_circuit_should_skip(LUFactorization *lu, uint64_t fingerprint) {
    if (!lu || lu->mkz_circuit_skip_budget <= 0) return 0;
    if (lu->mkz_circuit_fingerprint != fingerprint) return 0;
    lu->mkz_circuit_skip_budget--;
    lp_telemetry_lu_mark_mkz_circuit_skip(lu);
    return 1;
}

static void mkz_circuit_note_bad_outcome(LUFactorization *lu, uint64_t fingerprint) {
    if (!lu) return;
    if (lu->mkz_circuit_fingerprint != fingerprint) {
        lu->mkz_circuit_fingerprint = fingerprint;
        lu->mkz_circuit_bad_streak = 0;
        lu->mkz_circuit_skip_budget = 0;
    }
    if (lu->mkz_circuit_bad_streak < INT_MAX) {
        lu->mkz_circuit_bad_streak++;
    }
    if (lu->mkz_circuit_bad_streak >= MARKOWITZ_CIRCUIT_BAD_STREAK) {
        lu->mkz_circuit_skip_budget = MARKOWITZ_CIRCUIT_SKIP_BUDGET;
        lu->mkz_circuit_bad_streak = 0;
        lp_telemetry_lu_mark_mkz_circuit_trip(lu);
    }
}

static void mkz_circuit_note_good_outcome(LUFactorization *lu, uint64_t fingerprint) {
    if (!lu) return;
    if (lu->mkz_circuit_fingerprint == fingerprint && lu->mkz_circuit_bad_streak > 0) {
        lu->mkz_circuit_bad_streak = 0;
        lp_telemetry_lu_mark_mkz_circuit_reset(lu);
    } else if (lu->mkz_circuit_fingerprint != fingerprint) {
        lu->mkz_circuit_fingerprint = fingerprint;
        lu->mkz_circuit_bad_streak = 0;
        lu->mkz_circuit_skip_budget = 0;
    }
}

/* Global Markowitz skip budget.
 * This complements the fingerprint-local circuit: if singular outcomes are
 * chronic across changing fingerprints, skip Markowitz for a bounded window. */
static int mkz_global_skip_should_skip(LUFactorization *lu) {
    if (!lu || lu->mkz_global_skip_budget <= 0) return 0;
    lu->mkz_global_skip_budget--;
    lp_telemetry_lu_mark_mkz_global_skip_skip(lu);
    return 1;
}

static void mkz_global_skip_note_singular_bad_outcome(LUFactorization *lu) {
    if (!lu) return;
    if (lu->mkz_global_singular_streak < INT_MAX) {
        lu->mkz_global_singular_streak++;
    }
    if (lu->mkz_global_singular_streak >= MARKOWITZ_GLOBAL_SINGULAR_BAD_STREAK) {
        lu->mkz_global_skip_budget = MARKOWITZ_GLOBAL_SKIP_BUDGET;
        lu->mkz_global_singular_streak = 0;
        lp_telemetry_lu_mark_mkz_global_skip_trip(lu);
    }
}

static void mkz_global_skip_note_reset(LUFactorization *lu) {
    if (!lu) return;
    if (lu->mkz_global_singular_streak > 0 || lu->mkz_global_skip_budget > 0) {
        lu->mkz_global_singular_streak = 0;
        lu->mkz_global_skip_budget = 0;
        lp_telemetry_lu_mark_mkz_global_skip_reset(lu);
    }
}

/*
 * Sparse Markowitz factorization of the m×k structural submatrix.
 *
 * Uses scatter/gather pattern: pivot row scattered into dense work[]/flag[]
 * arrays, enabling O(1) entry lookup during elimination (vs O(col_len) search).
 * Dual SVA storage (row-indexed + column-indexed) enables walking a row's
 * entries in O(row_len) instead of scanning all k columns.
 *
 * Returns 0 on success, negative MKZ_FAIL_* on failure.
 * Caller falls back to dense GE on failure.
 */
static int lu_factorize_markowitz(
    LUFactorization *lu,
    const SparseMatrix *B, const int *col_order, int m, int k, int init_nnz,
    int *row_perm, int *row_pos, double pivot_tol,
    const int *row_reserved,
    const int *redundant_rows, int num_redundant,
    int allow_regularization, int max_regularizations, int *num_regularized,
    int *L_row, int *L_col, double *L_val, int *L_nnz_out, int L_capacity,
    int *U_row, int *U_col, double *U_val, int *U_nnz_out, int U_capacity,
    int *mkz_col_perm, int pool_mult,
    double threshold_ratio_base, int max_search_base,
    double singular_retry_threshold, double reserved_relax_ratio,
    double *workspace, size_t workspace_doubles)
{
    /* SVA pool sizing: MULT × initial nnz for both row and column pools */
    int pool_cap = 0;
    size_t total_need = 0;
    if (mkz_compute_workspace_requirements(init_nnz, m, k, pool_mult, &pool_cap, &total_need) != 0) {
        return MKZ_FAIL_WORKSPACE;
    }

    /* Workspace layout (all carved from workspace buffer):
     *
     * DOUBLES:
     *   cv_val[pool_cap]     — column SVA values
     *   rv_val[pool_cap]     — row SVA values
     *   work[k]              — dense scatter buffer for pivot row values
     *   col_max[k]           — column maximum absolute value
     *   col_max_prev[k]      — previous column max bound when a column is dirty
     *
     * INTS (packed after doubles):
     *   cv_idx[pool_cap]     — column SVA row indices
     *   rv_idx[pool_cap]     — row SVA column indices
     *   rv_hint[pool_cap]    — hint: local row position within column SVA
     *   cv_hint[pool_cap]    — hint: local column position within row SVA
     *   cv_ptr[k], cv_len[k], cv_cap[k]  — column SVA metadata
     *   rv_ptr[m], rv_len[m], rv_cap[m]  — row SVA metadata
     *   flag[k]              — dense flag for scatter/gather
     *   pivot_live_rp[k]     — row-SVA positions of live pivot-row entries
     *   pivot_live_col[k]    — structural column ids of live pivot-row entries
     *   col_deg[k]           — active column degree (for degree buckets)
     *   col_max_pos[k]       — local position of current column max, -1 if unknown
     *   col_max_dirty[k]     — 1 when col_max needs exact rescan
     *   row_deg[m]           — active row degree
     *   col_alive[k]         — 1 if column not yet eliminated
     *   row_alive[m]         — 1 if row not yet eliminated
     *   dg_head[k+1]         — degree bucket heads
     *   dg_next[k], dg_prev[k] — degree bucket DLL
     *   row_deg_hist_all[k+1] — histogram of active row degrees
     *   row_deg_hist_nonres[k+1] — histogram of active non-reserved row degrees
     *   rd_head[k+1]         — row degree bucket heads
     *   rd_next[m], rd_prev[m] — row degree bucket DLL
     */
    size_t dbl_need = 2u * (size_t)pool_cap + (size_t)k + (size_t)k + (size_t)k;

    if (workspace_doubles < total_need)
        return MKZ_FAIL_WORKSPACE;

    /* Carve double arrays */
    double *cv_val  = workspace;
    double *rv_val  = cv_val + pool_cap;
    double *work    = rv_val + pool_cap;
    double *col_max = work + k;
    double *col_max_prev = col_max + k;

    /* Carve int arrays */
    int *ib = (int *)(workspace + dbl_need);
    int *cv_idx   = ib;         ib += pool_cap;
    int *rv_idx   = ib;         ib += pool_cap;
    int *rv_hint  = ib;         ib += pool_cap;
    int *cv_hint  = ib;         ib += pool_cap;
    int *cv_ptr   = ib;         ib += k;
    int *cv_len   = ib;         ib += k;
    int *cv_cap_a = ib;         ib += k;
    int *rv_ptr   = ib;         ib += m;
    int *rv_len   = ib;         ib += m;
    int *rv_cap_a = ib;         ib += m;
    int *flag     = ib;         ib += k;
    int *pivot_live_rp = ib;    ib += k;
    int *pivot_live_col = ib;   ib += k;
    int *col_deg  = ib;         ib += k;
    int *row_deg  = ib;         ib += m;
    int *col_alive = ib;        ib += k;
    int *row_alive = ib;        ib += m;
    int *col_max_pos = ib;      ib += k;
    int *col_max_dirty = ib;    ib += k;
    int *dg_head  = ib;         ib += k + 1;
    int *dg_next  = ib;         ib += k;
    int *dg_prev  = ib;         ib += k;
    int *row_deg_hist_all = ib; ib += k + 1;
    int *row_deg_hist_nonres = ib; ib += k + 1;
    int *rd_head = ib;          ib += k + 1;
    int *rd_next = ib;          ib += m;
    int *rd_prev = ib;          /* ib += m; */

    /* Initialize */
    memset(flag, 0, k * sizeof(int));
    memset(work, 0, k * sizeof(double));
    memset(col_max_prev, 0, k * sizeof(double));
    memset(row_deg, 0, m * sizeof(int));
    for (int jj = 0; jj < k; jj++) col_alive[jj] = 1;
    memset(row_alive, 0, m * sizeof(int));
    memset(col_max_pos, 0xff, k * sizeof(int));
    memset(col_max_dirty, 0, k * sizeof(int));
    memset(row_deg_hist_all, 0, (size_t)(k + 1) * sizeof(int));
    memset(row_deg_hist_nonres, 0, (size_t)(k + 1) * sizeof(int));
    for (int d = 0; d <= k; d++) rd_head[d] = -1;
    for (int i = 0; i < m; i++) { rd_next[i] = -1; rd_prev[i] = -1; }

    int L_nnz = 0, U_nnz = 0;
    uint64_t mkz_primary_scan_entries = 0;
    uint64_t mkz_rescue_scan_entries = 0;
    uint64_t mkz_reserved_scan_entries = 0;
    uint64_t mkz_update_existing_entries = 0;
    uint64_t mkz_update_fill_candidates = 0;
    uint64_t mkz_hint_fallback_scans = 0;
    uint64_t mkz_hint_fallback_scan_entries = 0;
    uint64_t mkz_affected_columns_total = 0;
    uint64_t mkz_affected_columns_max = 0;
    uint64_t mkz_col_max_scan_entries = 0;

    #define MKZ_FLUSH_SCAN_WORK() \
        lp_telemetry_lu_add_mkz_scan_work(lu, \
                                          mkz_primary_scan_entries, \
                                          mkz_rescue_scan_entries, \
                                          mkz_reserved_scan_entries, \
                                          mkz_update_existing_entries, \
                                          mkz_update_fill_candidates, \
                                          mkz_hint_fallback_scans, \
                                          mkz_hint_fallback_scan_entries)
    #define MKZ_FLUSH_COLMAX_WORK() \
        lp_telemetry_lu_add_mkz_colmax_work(lu, \
                                            mkz_affected_columns_total, \
                                            mkz_affected_columns_max, \
                                            mkz_col_max_scan_entries)
    #define MKZ_FLUSH_TELEMETRY() do { \
        MKZ_FLUSH_SCAN_WORK(); \
        MKZ_FLUSH_COLMAX_WORK(); \
    } while (0)

    /* Build column SVA from B */
    int cv_used = 0;
    for (int jj = 0; jj < k; jj++) {
        int j = col_order[jj];
        cv_ptr[jj] = cv_used;
        int cnt = 0;
        for (int p = B->colptr[j]; p < B->colptr[j + 1]; p++) {
            if (fabs(B->values[p]) > RALPH_ZERO_TOL) {
                if (cv_used >= pool_cap) {
                    MKZ_FLUSH_TELEMETRY();
                    return MKZ_FAIL_POOL;
                }
                cv_idx[cv_used] = B->rowidx[p];
                cv_val[cv_used] = B->values[p];
                cv_used++;
                cnt++;
                row_deg[B->rowidx[p]]++;
            }
        }
        cv_len[jj] = cnt;
        cv_cap_a[jj] = cnt + MARKOWITZ_FILL_GAP;
        cv_used += MARKOWITZ_FILL_GAP;
        if (cv_used > pool_cap) cv_used = pool_cap;
        col_deg[jj] = cnt;
    }

    /* Mark active rows (those that appear in at least one structural column) */
    for (int jj = 0; jj < k; jj++) {
        int s = cv_ptr[jj], n2 = cv_len[jj];
        for (int e = 0; e < n2; e++)
            row_alive[cv_idx[s + e]] = 1;
    }

    int min_row_deg_all = k + 1;
    int min_row_deg_nonres = k + 1;
    for (int i = 0; i < m; i++) {
        if (!row_alive[i]) continue;
        int deg = row_deg[i];
        if (deg <= 0 || deg > k) continue;
        row_deg_hist_all[deg]++;
        rd_next[i] = rd_head[deg];
        rd_prev[i] = -1;
        if (rd_head[deg] >= 0) rd_prev[rd_head[deg]] = i;
        rd_head[deg] = i;
        if (deg < min_row_deg_all) min_row_deg_all = deg;
        if (!row_reserved || !row_reserved[i]) {
            row_deg_hist_nonres[deg]++;
            if (deg < min_row_deg_nonres) min_row_deg_nonres = deg;
        }
    }

    /* Build row SVA from column SVA (transpose) */
    /* First pass: count entries per row (already in row_deg) */
    int rv_used = 0;
    for (int i = 0; i < m; i++) {
        if (!row_alive[i]) { rv_ptr[i] = 0; rv_len[i] = 0; rv_cap_a[i] = 0; continue; }
        rv_ptr[i] = rv_used;
        rv_len[i] = 0;
        rv_cap_a[i] = row_deg[i] + MARKOWITZ_FILL_GAP;
        rv_used += rv_cap_a[i];
        if (rv_used > pool_cap) rv_used = pool_cap;
    }
    /* Second pass: fill row entries */
    for (int jj = 0; jj < k; jj++) {
        int s = cv_ptr[jj], n2 = cv_len[jj];
        for (int e = 0; e < n2; e++) {
            int row = cv_idx[s + e];
            int re = rv_len[row];
            int rp = rv_ptr[row] + re;
            if (rp >= pool_cap) {
                MKZ_FLUSH_TELEMETRY();
                return MKZ_FAIL_POOL;
            }
            rv_idx[rp] = jj;
            rv_val[rp] = cv_val[s + e];
            rv_hint[rp] = e;
            cv_hint[s + e] = re;
            rv_len[row]++;
        }
    }

    /* Compute column maximums */
    for (int jj = 0; jj < k; jj++) {
        double mx = 0.0;
        int mx_pos = -1;
        int s = cv_ptr[jj], n2 = cv_len[jj];
        for (int e = 0; e < n2; e++) {
            double av = fabs(cv_val[s + e]);
            if (av > mx) {
                mx = av;
                mx_pos = e;
            }
        }
        col_max[jj] = mx;
        col_max_pos[jj] = mx_pos;
    }

    /* Initialize degree buckets (by column degree) */
    for (int d = 0; d <= k; d++) dg_head[d] = -1;
    for (int jj = 0; jj < k; jj++) {
        int d = col_deg[jj]; if (d > k) d = k;
        dg_next[jj] = dg_head[d];
        dg_prev[jj] = -1;
        if (dg_head[d] >= 0) dg_prev[dg_head[d]] = jj;
        dg_head[d] = jj;
    }

    #define DG_REMOVE(jj) do { \
        int _d = col_deg[jj]; if (_d > k) _d = k; \
        if (dg_prev[jj] >= 0) dg_next[dg_prev[jj]] = dg_next[jj]; \
        else dg_head[_d] = dg_next[jj]; \
        if (dg_next[jj] >= 0) dg_prev[dg_next[jj]] = dg_prev[jj]; \
    } while(0)

    #define DG_INSERT(jj) do { \
        int _d = col_deg[jj]; if (_d > k) _d = k; \
        dg_next[jj] = dg_head[_d]; dg_prev[jj] = -1; \
        if (dg_head[_d] >= 0) dg_prev[dg_head[_d]] = jj; \
        dg_head[_d] = jj; \
    } while(0)

    #define ADVANCE_MIN_ROW_DEG(_min_deg, _hist) do { \
        while ((_min_deg) <= k && (_hist)[(_min_deg)] == 0) (_min_deg)++; \
    } while (0)

    #define ROW_DG_REMOVE(_row) do { \
        int _d = row_deg[_row]; \
        if ((_d) > 0 && (_d) <= k) { \
            if (rd_prev[_row] >= 0) rd_next[rd_prev[_row]] = rd_next[_row]; \
            else rd_head[_d] = rd_next[_row]; \
            if (rd_next[_row] >= 0) rd_prev[rd_next[_row]] = rd_prev[_row]; \
        } \
        rd_next[_row] = -1; \
        rd_prev[_row] = -1; \
    } while (0)

    #define ROW_DG_INSERT(_row) do { \
        int _d = row_deg[_row]; \
        if (row_alive[_row] && (_d) > 0 && (_d) <= k) { \
            rd_next[_row] = rd_head[_d]; \
            rd_prev[_row] = -1; \
            if (rd_head[_d] >= 0) rd_prev[rd_head[_d]] = (_row); \
            rd_head[_d] = (_row); \
        } \
    } while (0)

    #define ROW_DEG_HIST_REMOVE(_row, _old_deg) do { \
        if ((_old_deg) > 0 && (_old_deg) <= k) { \
            row_deg_hist_all[_old_deg]--; \
            if ((_old_deg) == min_row_deg_all && row_deg_hist_all[_old_deg] == 0) \
                ADVANCE_MIN_ROW_DEG(min_row_deg_all, row_deg_hist_all); \
            if (!row_reserved || !row_reserved[_row]) { \
                row_deg_hist_nonres[_old_deg]--; \
                if ((_old_deg) == min_row_deg_nonres && row_deg_hist_nonres[_old_deg] == 0) \
                    ADVANCE_MIN_ROW_DEG(min_row_deg_nonres, row_deg_hist_nonres); \
            } \
        } \
    } while (0)

    #define ROW_DEG_HIST_ADD(_row, _new_deg) do { \
        if ((_new_deg) > 0 && (_new_deg) <= k) { \
            row_deg_hist_all[_new_deg]++; \
            if ((_new_deg) < min_row_deg_all) min_row_deg_all = (_new_deg); \
            if (!row_reserved || !row_reserved[_row]) { \
                row_deg_hist_nonres[_new_deg]++; \
                if ((_new_deg) < min_row_deg_nonres) min_row_deg_nonres = (_new_deg); \
            } \
        } \
    } while (0)

    #define ROW_DEG_CHANGE(_row, _delta) do { \
        int _old_deg = row_deg[_row]; \
        ROW_DG_REMOVE(_row); \
        ROW_DEG_HIST_REMOVE(_row, _old_deg); \
        row_deg[_row] = _old_deg + (_delta); \
        ROW_DEG_HIST_ADD(_row, row_deg[_row]); \
        ROW_DG_INSERT(_row); \
    } while (0)

    /* Helper: remove entry at position e from column jj's SVA segment */
    #define CV_REMOVE(jj, e) do { \
        int _s = cv_ptr[jj]; \
        cv_len[jj]--; \
        if ((e) < cv_len[jj]) { \
            int _moved_row = cv_idx[_s + cv_len[jj]]; \
            int _moved_re = cv_hint[_s + cv_len[jj]]; \
            cv_idx[_s + (e)] = cv_idx[_s + cv_len[jj]]; \
            cv_val[_s + (e)] = cv_val[_s + cv_len[jj]]; \
            cv_hint[_s + (e)] = _moved_re; \
            rv_hint[rv_ptr[_moved_row] + _moved_re] = (e); \
        } \
    } while(0)

    /* Helper: remove entry at position e from row i's SVA segment */
    #define RV_REMOVE(i, e) do { \
        int _s = rv_ptr[i]; \
        rv_len[i]--; \
        if ((e) < rv_len[i]) { \
            int _moved_jj = rv_idx[_s + rv_len[i]]; \
            int _moved_ce = rv_hint[_s + rv_len[i]]; \
            rv_idx[_s + (e)] = rv_idx[_s + rv_len[i]]; \
            rv_val[_s + (e)] = rv_val[_s + rv_len[i]]; \
            rv_hint[_s + (e)] = rv_hint[_s + rv_len[i]]; \
            cv_hint[cv_ptr[_moved_jj] + _moved_ce] = (e); \
        } \
    } while(0)

    *num_regularized = 0;
    for (int jj = 0; jj < k; jj++) mkz_col_perm[jj] = -1;

    if (!(threshold_ratio_base > 0.0)) {
        threshold_ratio_base = MARKOWITZ_THRESHOLD;
    }
    if (max_search_base < 1) {
        max_search_base = MARKOWITZ_MAX_SEARCH;
    }
    if (!(singular_retry_threshold > 0.0)) {
        singular_retry_threshold = MARKOWITZ_SINGULAR_RETRY_THRESHOLD;
    }
    if (!(reserved_relax_ratio > 0.0)) {
        reserved_relax_ratio = MARKOWITZ_RESERVED_RELAX_RATIO;
    }

    (void)0;  /* active rows/cols tracked implicitly by degree lists */
    /* For large structural fronts, test the cached column maximum first.
     * It is threshold-eligible by construction and can avoid full column scans
     * when it also reaches the current bucket's Markowitz lower bound. */
    int use_colmax_probe = (k >= 500);

    #define MKZ_TRY_COLMAX_PROBE(row_ok_expr) do { \
        int max_pos = use_colmax_probe ? col_max_pos[jj] : -1; \
        if (max_pos >= 0 && max_pos < n2) { \
            int row = cv_idx[s + max_pos]; \
            if ((row_ok_expr)) { \
                double av = fabs(cv_val[s + max_pos]); \
                if (av >= thr) { \
                    long long cost = (long long)(row_deg[row] - 1) * (col_deg[jj] - 1); \
                    if (cost < best_cost || (cost == best_cost && av > best_piv_val)) { \
                        best_cost = cost; \
                        piv_col = jj; piv_row = row; best_piv_val = av; \
                        if (cost == bucket_lower_bound) { \
                            scanned_entries = 1; \
                            mkz_primary_scan_entries += (uint64_t)scanned_entries; \
                            goto pivot_found; \
                        } \
                    } \
                } \
            } \
        } \
    } while (0)

    #define MKZ_TRY_ROW_DEG1_PIVOT(row_ok_expr) do { \
        if (min_row_deg_bound == 1) { \
            for (int row = rd_head[1]; row >= 0; row = rd_next[row]) { \
                if (!(row_ok_expr)) continue; \
                int rs = rv_ptr[row], rn = rv_len[row]; \
                for (int re = 0; re < rn; re++) { \
                    int jj = rv_idx[rs + re]; \
                    if (!col_alive[jj]) continue; \
                    double max_col = col_max[jj]; \
                    double av = fabs(rv_val[rs + re]); \
                    if (av >= threshold_ratio * max_col) { \
                        best_cost = 0; \
                        piv_col = jj; \
                        piv_row = row; \
                        best_piv_val = av; \
                        goto pivot_found; \
                    } \
                } \
            } \
        } \
    } while (0)

    for (int step = 0; step < k; step++) {
        int singular_retry_used = 0;
        int reserve_non_reserved = (row_reserved != NULL);
        int piv_col = -1, piv_row = -1;
        long long best_cost = (long long)m * m + 1;
        double best_piv_val = 0.0;

        while (1) {
            /* === 1. Pivot selection: Markowitz with threshold === */
            piv_col = -1;
            piv_row = -1;
            best_cost = (long long)m * m + 1;
            best_piv_val = 0.0;
            int max_search = singular_retry_used ? k : max_search_base;
            double threshold_ratio = singular_retry_used
                ? singular_retry_threshold
                : threshold_ratio_base;
            int min_row_deg_bound = reserve_non_reserved
                ? min_row_deg_nonres
                : min_row_deg_all;

            if (reserve_non_reserved && row_reserved) {
                MKZ_TRY_ROW_DEG1_PIVOT(row_alive[row] && !row_reserved[row]);
            } else {
                MKZ_TRY_ROW_DEG1_PIVOT(row_alive[row]);
            }

            for (int d = 1; d <= k && min_row_deg_bound <= k; d++) {
                long long bucket_lower_bound = (long long)(min_row_deg_bound - 1) * (long long)(d - 1);
                if (best_cost < (long long)m * m + 1 && bucket_lower_bound > best_cost)
                    break;
                int cand = 0;
                if (reserve_non_reserved && row_reserved) {
                    for (int jj = dg_head[d]; jj >= 0 && cand < max_search; jj = dg_next[jj]) {
                        if (best_cost < (long long)m * m + 1 && bucket_lower_bound > best_cost) {
                            cand++;
                            continue;
                        }
                        if (best_cost < (long long)m * m + 1 &&
                            bucket_lower_bound == best_cost &&
                            col_max[jj] <= best_piv_val) {
                            cand++;
                            continue;
                        }
                        double max_col = col_max[jj];
                        double thr = threshold_ratio * max_col;
                        int s = cv_ptr[jj], n2 = cv_len[jj];
                        int scanned_entries = n2;
                        MKZ_TRY_COLMAX_PROBE(row_alive[row] && !row_reserved[row]);
                        for (int e = 0; e < n2; e++) {
                            if (e + 8 < n2) {
                                int next_row = cv_idx[s + e + 8];
                                RALPH_PREFETCH(&cv_idx[s + e + 8], 0, 1);
                                RALPH_PREFETCH(&cv_val[s + e + 8], 0, 1);
                                RALPH_PREFETCH(&row_deg[next_row], 0, 1);
                            }
                            int row = cv_idx[s + e];
                            if (!row_alive[row] || row_reserved[row]) continue;
                            double av = fabs(cv_val[s + e]);
                            if (av < thr) continue;
                            long long cost = (long long)(row_deg[row] - 1) * (col_deg[jj] - 1);
                            if (cost < best_cost || (cost == best_cost && av > best_piv_val)) {
                                best_cost = cost;
                                piv_col = jj; piv_row = row; best_piv_val = av;
                                if (cost == 0) {
                                    scanned_entries = e + 1;
                                    mkz_primary_scan_entries += (uint64_t)scanned_entries;
                                    goto pivot_found;
                                }
                            }
                        }
                        mkz_primary_scan_entries += (uint64_t)scanned_entries;
                        cand++;
                    }
                } else {
                    for (int jj = dg_head[d]; jj >= 0 && cand < max_search; jj = dg_next[jj]) {
                        if (best_cost < (long long)m * m + 1) {
                            if (bucket_lower_bound > best_cost) {
                                cand++;
                                continue;
                            }
                            if (bucket_lower_bound == best_cost && col_max[jj] <= best_piv_val) {
                                cand++;
                                continue;
                            }
                        }
                        double max_col = col_max[jj];
                        double thr = threshold_ratio * max_col;
                        int s = cv_ptr[jj], n2 = cv_len[jj];
                        int scanned_entries = n2;
                        MKZ_TRY_COLMAX_PROBE(row_alive[row]);
                        for (int e = 0; e < n2; e++) {
                            if (e + 8 < n2) {
                                int next_row = cv_idx[s + e + 8];
                                RALPH_PREFETCH(&cv_idx[s + e + 8], 0, 1);
                                RALPH_PREFETCH(&cv_val[s + e + 8], 0, 1);
                                RALPH_PREFETCH(&row_deg[next_row], 0, 1);
                            }
                            int row = cv_idx[s + e];
                            if (!row_alive[row]) continue;
                            double av = fabs(cv_val[s + e]);
                            if (av < thr) continue;
                            long long cost = (long long)(row_deg[row] - 1) * (col_deg[jj] - 1);
                            if (cost < best_cost || (cost == best_cost && av > best_piv_val)) {
                                best_cost = cost;
                                piv_col = jj; piv_row = row; best_piv_val = av;
                                if (cost == 0) {
                                    scanned_entries = e + 1;
                                    mkz_primary_scan_entries += (uint64_t)scanned_entries;
                                    goto pivot_found;
                                }
                            }
                        }
                        mkz_primary_scan_entries += (uint64_t)scanned_entries;
                        cand++;
                    }
                }
            }

            if (piv_col < 0 || best_piv_val < pivot_tol) {
                int rescue_col = -1;
                int rescue_row = -1;
                long long rescue_cost = (long long)m * m + 1;
                double rescue_piv = 0.0;
                for (int jj = 0; jj < k; jj++) {
                    if (!col_alive[jj]) continue;
                    int s = cv_ptr[jj], n2 = cv_len[jj];
                    mkz_rescue_scan_entries += (uint64_t)n2;
                    for (int e = 0; e < n2; e++) {
                        int row = cv_idx[s + e];
                        if (!row_alive[row]) continue;
                        if (reserve_non_reserved && row_reserved && row_reserved[row]) continue;
                        double av = fabs(cv_val[s + e]);
                        if (av <= rescue_piv && rescue_col >= 0) continue;
                        long long cost = (long long)(row_deg[row] - 1) * (col_deg[jj] - 1);
                        rescue_col = jj;
                        rescue_row = row;
                        rescue_cost = cost;
                        rescue_piv = av;
                    }
                }
                if (rescue_col >= 0 && rescue_piv >= pivot_tol) {
                    piv_col = rescue_col;
                    piv_row = rescue_row;
                    best_cost = rescue_cost;
                    best_piv_val = rescue_piv;
                }
            }

            if (piv_col < 0 || best_piv_val < pivot_tol) {
                if (!singular_retry_used) {
                    singular_retry_used = 1;
                    lp_telemetry_lu_mark_mkz_singular_retry_attempt(lu);
                    continue;
                }

                lp_telemetry_lu_mark_mkz_singular_retry_failure(lu);

                /* Reserved-row fallback: allow identity rows only when they are
                 * materially stronger than the best non-reserved option. */
                if (reserve_non_reserved && row_reserved) {
                    int nonres_col = -1;
                    int reserved_col = -1, reserved_row = -1;
                    long long nonres_cost = (long long)m * m + 1;
                    long long reserved_cost = (long long)m * m + 1;
                    double nonres_val = 0.0;
                    double reserved_val = 0.0;

                    lp_telemetry_lu_mark_mkz_reserved_fallback_attempt(lu);
                    for (int jj = 0; jj < k; jj++) {
                        if (!col_alive[jj]) continue;
                        int s = cv_ptr[jj], n2 = cv_len[jj];
                        mkz_reserved_scan_entries += (uint64_t)n2;
                        for (int e = 0; e < n2; e++) {
                            int row = cv_idx[s + e];
                            if (!row_alive[row]) continue;
                            double av = fabs(cv_val[s + e]);
                            long long cost = (long long)(row_deg[row] - 1) * (col_deg[jj] - 1);

                            if (row_reserved[row]) {
                                if (av > reserved_val ||
                                    (av == reserved_val && cost < reserved_cost)) {
                                    reserved_col = jj;
                                    reserved_row = row;
                                    reserved_cost = cost;
                                    reserved_val = av;
                                }
                            } else {
                                if (av > nonres_val ||
                                    (av == nonres_val && cost < nonres_cost)) {
                                    nonres_col = jj;
                                    nonres_cost = cost;
                                    nonres_val = av;
                                }
                            }
                        }
                    }

                    if (reserved_col >= 0 && reserved_val >= pivot_tol) {
                        int use_reserved = 0;
                        if (nonres_col < 0 || nonres_val < pivot_tol) {
                            use_reserved = 1;
                        } else if (nonres_val <
                                   reserved_relax_ratio * reserved_val) {
                            use_reserved = 1;
                        }

                        if (use_reserved) {
                            piv_col = reserved_col;
                            piv_row = reserved_row;
                            best_cost = reserved_cost;
                            best_piv_val = reserved_val;
                            lp_telemetry_lu_mark_mkz_reserved_fallback_accept(lu);
                        } else {
                            lp_telemetry_lu_mark_mkz_reserved_fallback_reject(lu);
                        }
                    } else {
                        lp_telemetry_lu_mark_mkz_reserved_fallback_reject(lu);
                    }
                }

                if (piv_col < 0 || best_piv_val < pivot_tol) {
                    /* Singular pivot handling */
                    int can_reg = 0;
                    if (redundant_rows && num_redundant > 0) {
                        for (int jj = 0; jj < k && !can_reg; jj++) {
                            if (!col_alive[jj]) continue;
                            int s = cv_ptr[jj], n2 = cv_len[jj];
                            for (int e = 0; e < n2; e++) {
                                int row = cv_idx[s + e];
                                if (!row_alive[row]) continue;
                                if (reserve_non_reserved && row_reserved && row_reserved[row]) continue;
                                if (redundant_rows[row]) {
                                    piv_col = jj; piv_row = row; can_reg = 1; break;
                                }
                            }
                        }
                    }
                    if (!can_reg && allow_regularization && *num_regularized < max_regularizations) {
                        for (int jj = 0; jj < k && !can_reg; jj++) {
                            if (!col_alive[jj]) continue;
                            int s = cv_ptr[jj], n2 = cv_len[jj];
                            for (int e = 0; e < n2; e++) {
                                int row = cv_idx[s + e];
                                if (!row_alive[row]) continue;
                                if (reserve_non_reserved && row_reserved && row_reserved[row]) continue;
                                piv_col = jj; piv_row = row; can_reg = 1; break;
                            }
                        }
                    }
                    if (!can_reg) {
                        /* No viable non-reserved pivot remains; caller will
                         * fall back to the next sparse numeric path. */
                        MKZ_FLUSH_TELEMETRY();
                        return MKZ_FAIL_SINGULAR;
                    }
                    (*num_regularized)++;
                    best_piv_val = 1.0;
                }
            } else if (singular_retry_used) {
                lp_telemetry_lu_mark_mkz_singular_retry_success(lu);
            }

        pivot_found:;
            break;
        }

        /* === 2. Record permutations === */
        /* piv_col is in structural-column space (0..k-1), so record elimination
         * order directly instead of swapping positions in a proxy permutation. */
        mkz_col_perm[step] = piv_col;
        { int pp = row_pos[piv_row];
          if (pp != step) { int a = row_perm[step], b = row_perm[pp];
            row_perm[step] = b; row_perm[pp] = a; row_pos[b] = step; row_pos[a] = pp; } }

        /* Find pivot value from column SVA */
        double pivot_val = 0.0;
        { int s = cv_ptr[piv_col], n2 = cv_len[piv_col];
          for (int e = 0; e < n2; e++)
              if (cv_idx[s + e] == piv_row) { pivot_val = cv_val[s + e]; break; } }
        if (fabs(pivot_val) < pivot_tol) pivot_val = 1.0;  /* regularized */

        /* Mark eliminated */
        DG_REMOVE(piv_col);
        col_alive[piv_col] = 0;
        ROW_DG_REMOVE(piv_row);
        ROW_DEG_HIST_REMOVE(piv_row, row_deg[piv_row]);
        row_alive[piv_row] = 0;

        /* === 3. Phase A: Scatter pivot row into work[]/flag[] using row SVA === */
        int pivot_live_n = 0;
        { int s = rv_ptr[piv_row], n2 = rv_len[piv_row];
          for (int e = 0; e < n2; e++) {
              int rp = s + e;
              int jj = rv_idx[rp];
              if (!col_alive[jj]) continue;
              work[jj] = rv_val[rp];
              flag[jj] = 1;
              pivot_live_rp[pivot_live_n++] = rp;
              pivot_live_col[pivot_live_n - 1] = jj;
          } }

        /* === 4. Emit L diagonal + U pivot row === */
        if (L_nnz >= L_capacity || U_nnz >= U_capacity) {
            MKZ_FLUSH_TELEMETRY();
            return MKZ_FAIL_CAPACITY;
        }
        L_row[L_nnz] = piv_row; L_col[L_nnz] = piv_col; L_val[L_nnz] = 1.0; L_nnz++;
        U_row[U_nnz] = step; U_col[U_nnz] = piv_col; U_val[U_nnz] = pivot_val; U_nnz++;

        for (int pe = 0; pe < pivot_live_n; pe++) {
              int rp = pivot_live_rp[pe];
              int jj = pivot_live_col[pe];
              if (fabs(rv_val[rp]) > RALPH_ZERO_TOL) {
                  if (U_nnz >= U_capacity) {
                      MKZ_FLUSH_TELEMETRY();
                      return MKZ_FAIL_CAPACITY;
                  }
                  U_row[U_nnz] = step; U_col[U_nnz] = jj; U_val[U_nnz] = rv_val[rp]; U_nnz++;
              }
        }

        /* === 5. Phase B: Eliminate — for each row with entry in pivot column === */
        { int s = cv_ptr[piv_col], n2 = cv_len[piv_col];
          for (int e = 0; e < n2; e++) {
              int row = cv_idx[s + e];
              if (!row_alive[row]) continue;
              double a_ik = cv_val[s + e];
              if (fabs(a_ik) < RALPH_ZERO_TOL) continue;
              double mult = a_ik / pivot_val;

              /* Emit L entry */
              if (L_nnz >= L_capacity) {
                  MKZ_FLUSH_TELEMETRY();
                  return MKZ_FAIL_CAPACITY;
              }
              L_row[L_nnz] = row; L_col[L_nnz] = piv_col; L_val[L_nnz] = mult; L_nnz++;

              /* Pass 1: Walk row's entries, update existing entries using flag[] O(1) */
              int rs = rv_ptr[row], rn = rv_len[row];
              mkz_update_existing_entries += (uint64_t)rn;
              for (int re = 0; re < rn; re++) {
                  int jj = rv_idx[rs + re];
                  if (!col_alive[jj]) continue;
                  if (flag[jj]) {
                      /* Existing entry — update in place */
                      flag[jj] = 0;  /* mark handled */
                      double new_val = rv_val[rs + re] - mult * work[jj];
                      rv_val[rs + re] = new_val;
                      int cs = cv_ptr[jj], cn = cv_len[jj];
                      int rp = rs + re;
                      int ce = rv_hint[rp];
                      if (ce < 0 || ce >= cn || cv_idx[cs + ce] != row) {
                          ce = -1;
                          mkz_hint_fallback_scans++;
                          for (int scan = 0; scan < cn; scan++) {
                              mkz_hint_fallback_scan_entries++;
                              if (cv_idx[cs + scan] == row) {
                                  ce = scan;
                                  break;
                              }
                          }
                          if (ce < 0) continue;
                          rv_hint[rp] = ce;
                      }

                      if (fabs(new_val) < RALPH_ZERO_TOL) {
                          /* Cancellation: remove from both SVAs */
                          int old_cn = cn;
                          double old_abs = fabs(cv_val[cs + ce]);
                          if (!col_max_dirty[jj]) {
                              if (ce == col_max_pos[jj]) {
                                  col_max_dirty[jj] = 1;
                                  col_max_prev[jj] = old_abs;
                                  col_max_pos[jj] = -1;
                              } else if (col_max_pos[jj] == old_cn - 1) {
                                  col_max_pos[jj] = ce;
                              }
                          }
                          CV_REMOVE(jj, ce);
                          RV_REMOVE(row, re); re--;
                          DG_REMOVE(jj); col_deg[jj]--; DG_INSERT(jj);
                          ROW_DEG_CHANGE(row, -1);
                      } else {
                          cv_val[cs + ce] = new_val;
                          {
                              double new_abs = fabs(new_val);
                              if (col_max_dirty[jj]) {
                                  if (new_abs >= col_max_prev[jj]) {
                                      col_max_dirty[jj] = 0;
                                      col_max[jj] = new_abs;
                                      col_max_pos[jj] = ce;
                                  }
                              } else if (ce == col_max_pos[jj]) {
                                  if (new_abs >= col_max[jj]) {
                                      col_max[jj] = new_abs;
                                  } else {
                                      col_max_dirty[jj] = 1;
                                      col_max_prev[jj] = col_max[jj];
                                      col_max_pos[jj] = -1;
                                  }
                              } else if (new_abs > col_max[jj]) {
                                  col_max[jj] = new_abs;
                                  col_max_pos[jj] = ce;
                              }
                          }
                      }
                  }
              }

              /* Pass 2: Fill-in — flag[jj] still set means no existing entry */
              {
                mkz_update_fill_candidates += (uint64_t)pivot_live_n;
                for (int pe = 0; pe < pivot_live_n; pe++) {
                    int jj = pivot_live_col[pe];
                    if (!col_alive[jj] || !flag[jj]) continue;
                    double fill = -mult * work[jj];
                    if (fabs(fill) < RALPH_ZERO_TOL) continue;
                    int rn2 = rv_len[row];

                    /* Insert into column SVA */
                    int cn = cv_len[jj];
                    if (cn >= cv_cap_a[jj]) {
                        /* Relocate column */
                        int new_cap = cn + MARKOWITZ_FILL_GAP + 4;
                        if (cv_used + new_cap > pool_cap) {
                            MKZ_FLUSH_TELEMETRY();
                            return MKZ_FAIL_POOL;
                        }
                        int ns = cv_used;
                        for (int f = 0; f < cn; f++) {
                            cv_idx[ns + f] = cv_idx[cv_ptr[jj] + f];
                            cv_val[ns + f] = cv_val[cv_ptr[jj] + f];
                            cv_hint[ns + f] = cv_hint[cv_ptr[jj] + f];
                        }
                        cv_ptr[jj] = ns; cv_cap_a[jj] = new_cap; cv_used += new_cap;
                    }
                    cv_idx[cv_ptr[jj] + cn] = row;
                    cv_val[cv_ptr[jj] + cn] = fill;
                    cv_hint[cv_ptr[jj] + cn] = rn2;
                    cv_len[jj]++;
                    {
                        double fill_abs = fabs(fill);
                        if (col_max_dirty[jj]) {
                            if (fill_abs >= col_max_prev[jj]) {
                                col_max_dirty[jj] = 0;
                                col_max[jj] = fill_abs;
                                col_max_pos[jj] = cn;
                            }
                        } else if (fill_abs > col_max[jj]) {
                            col_max[jj] = fill_abs;
                            col_max_pos[jj] = cn;
                        }
                    }

                    /* Insert into row SVA */
                    if (rn2 >= rv_cap_a[row]) {
                        int new_cap = rn2 + MARKOWITZ_FILL_GAP + 4;
                        if (rv_used + new_cap > pool_cap) {
                            MKZ_FLUSH_TELEMETRY();
                            return MKZ_FAIL_POOL;
                        }
                        int ns = rv_used;
                        for (int f = 0; f < rn2; f++) {
                            rv_idx[ns + f] = rv_idx[rv_ptr[row] + f];
                            rv_val[ns + f] = rv_val[rv_ptr[row] + f];
                            rv_hint[ns + f] = rv_hint[rv_ptr[row] + f];
                        }
                        rv_ptr[row] = ns; rv_cap_a[row] = new_cap; rv_used += new_cap;
                    }
                    rv_idx[rv_ptr[row] + rn2] = jj;
                    rv_val[rv_ptr[row] + rn2] = fill;
                    rv_hint[rv_ptr[row] + rn2] = cn;
                    rv_len[row]++;

                    DG_REMOVE(jj); col_deg[jj]++; DG_INSERT(jj);
                    ROW_DEG_CHANGE(row, +1);
                } }

              /* Restore flags for next row */
              for (int pe = 0; pe < pivot_live_n; pe++) {
                  int jj = pivot_live_col[pe];
                  flag[jj] = 1;
              }

              ROW_DEG_CHANGE(row, -1);
          } }

        /* Remove pivot-row entries from column SVAs (cleanup).
         * Use row SVA to visit only affected columns instead of scanning all k columns. */
        for (int pe = 0; pe < pivot_live_n; pe++) {
              int rp = pivot_live_rp[pe];
              int jj = pivot_live_col[pe];
              int s = cv_ptr[jj], n2 = cv_len[jj];
              int ce = rv_hint[rp];
              if (ce < 0 || ce >= n2 || cv_idx[s + ce] != piv_row) {
                  ce = -1;
                  mkz_hint_fallback_scans++;
                  for (int scan = 0; scan < n2; scan++) {
                      mkz_hint_fallback_scan_entries++;
                      if (cv_idx[s + scan] == piv_row) {
                          ce = scan;
                          break;
                      }
                  }
                  if (ce < 0) continue;
                  rv_hint[rp] = ce;
              }
              if (!col_max_dirty[jj]) {
                  double removed_abs = fabs(cv_val[s + ce]);
                  int old_cn = n2;
                  if (ce == col_max_pos[jj]) {
                      col_max_dirty[jj] = 1;
                      col_max_prev[jj] = removed_abs;
                      col_max_pos[jj] = -1;
                  } else if (col_max_pos[jj] == old_cn - 1) {
                      col_max_pos[jj] = ce;
                  }
              }
              CV_REMOVE(jj, ce);
              DG_REMOVE(jj); col_deg[jj]--; DG_INSERT(jj);
        }

        /* Phase C: Clean up scatter arrays */
        for (int pe = 0; pe < pivot_live_n; pe++) {
              int jj = pivot_live_col[pe];
              work[jj] = 0.0; flag[jj] = 0;
        }

        /* Update col_max for affected columns */
        {
          uint64_t step_affected_columns = (uint64_t)pivot_live_n;
          for (int pe = 0; pe < pivot_live_n; pe++) {
              int jj = pivot_live_col[pe];
              if (!col_max_dirty[jj]) continue;
              double mx = 0.0;
              int mx_pos = -1;
              int s = cv_ptr[jj], n2 = cv_len[jj];
              mkz_col_max_scan_entries += (uint64_t)n2;
              for (int e = 0; e < n2; e++) {
                  if (!row_alive[cv_idx[s + e]]) continue;
                  double av = fabs(cv_val[s + e]);
                  if (av > mx) {
                      mx = av;
                      mx_pos = e;
                  }
              }
              col_max[jj] = mx;
              col_max_pos[jj] = mx_pos;
              col_max_dirty[jj] = 0;
          }
          mkz_affected_columns_total += step_affected_columns;
          if (step_affected_columns > mkz_affected_columns_max) {
              mkz_affected_columns_max = step_affected_columns;
          }
        }
    }

    #undef DG_REMOVE
    #undef DG_INSERT
    #undef ADVANCE_MIN_ROW_DEG
    #undef ROW_DEG_HIST_REMOVE
    #undef ROW_DEG_HIST_ADD
    #undef ROW_DEG_CHANGE
    #undef ROW_DG_REMOVE
    #undef ROW_DG_INSERT
    #undef CV_REMOVE
    #undef RV_REMOVE
    #undef MKZ_TRY_COLMAX_PROBE
    #undef MKZ_TRY_ROW_DEG1_PIVOT
    #undef MKZ_FLUSH_TELEMETRY
    #undef MKZ_FLUSH_COLMAX_WORK
    #undef MKZ_FLUSH_SCAN_WORK

    *L_nnz_out = L_nnz;
    *U_nnz_out = U_nnz;
    lp_telemetry_lu_add_mkz_scan_work(lu,
                                      mkz_primary_scan_entries,
                                      mkz_rescue_scan_entries,
                                      mkz_reserved_scan_entries,
                                      mkz_update_existing_entries,
                                      mkz_update_fill_candidates,
                                      mkz_hint_fallback_scans,
                                      mkz_hint_fallback_scan_entries);
    lp_telemetry_lu_add_mkz_colmax_work(lu,
                                        mkz_affected_columns_total,
                                        mkz_affected_columns_max,
                                        mkz_col_max_scan_entries);
    return 0;
}

typedef enum {
    LU_NUMERIC_MODE_STANDARD = 0,
    LU_NUMERIC_MODE_SYMBOLIC_FULL_RETRY = 1,
    LU_NUMERIC_MODE_STRICT_DISPATCH = 2
} LUNumericMode;

typedef enum {
    LU_NUMERIC_BACKEND_NONE = 0,
    LU_NUMERIC_BACKEND_MARKOWITZ = 1,
    LU_NUMERIC_BACKEND_SUPERNODE = 2,
    LU_NUMERIC_BACKEND_DENSE_GE = 3
} LUNumericBackend;

#define IDSEP_RETRY_SUPERNODE_STREAK_TRIGGER 2

static int lu_identity_sep_retry_lane_plan(int idsep_retry_streak,
                                           int sn_enabled,
                                           int k) {
    if (sn_enabled &&
        k >= SN_MIN_K &&
        idsep_retry_streak >= IDSEP_RETRY_SUPERNODE_STREAK_TRIGGER) {
        return LU_IDSEP_RETRY_LANE_SUPERNODE;
    }
    return LU_IDSEP_RETRY_LANE_DENSE;
}

int lu_identity_sep_retry_lane_plan_for_test(int idsep_retry_streak,
                                             int sn_enabled,
                                             int k) {
    return lu_identity_sep_retry_lane_plan(idsep_retry_streak, sn_enabled, k);
}

enum {
    LU_MKZ_GLOBAL_EVENT_NONE = 0,
    LU_MKZ_GLOBAL_EVENT_SINGULAR_FAILURE = 1,
    LU_MKZ_GLOBAL_EVENT_SUCCESS = 2
};

int lu_markowitz_global_skip_plan_for_test(int bad_streak,
                                           int skip_budget,
                                           int event,
                                           int *next_bad_streak_out,
                                           int *next_skip_budget_out) {
    int should_skip = 0;
    if (bad_streak < 0) bad_streak = 0;
    if (skip_budget < 0) skip_budget = 0;

    if (event == LU_MKZ_GLOBAL_EVENT_SINGULAR_FAILURE) {
        if (bad_streak < INT_MAX) bad_streak++;
        if (bad_streak >= MARKOWITZ_GLOBAL_SINGULAR_BAD_STREAK) {
            skip_budget = MARKOWITZ_GLOBAL_SKIP_BUDGET;
            bad_streak = 0;
        }
    } else if (event == LU_MKZ_GLOBAL_EVENT_SUCCESS) {
        bad_streak = 0;
        skip_budget = 0;
    }

    if (skip_budget > 0) {
        should_skip = 1;
        skip_budget--;
    }

    if (next_bad_streak_out) *next_bad_streak_out = bad_streak;
    if (next_skip_budget_out) *next_skip_budget_out = skip_budget;
    return should_skip;
}

static int lu_numeric_backend_to_basis_governor_backend(int backend) {
    if (backend == LU_NUMERIC_BACKEND_MARKOWITZ) {
        return LP_BASIS_GOV_BACKEND_MARKOWITZ;
    }
    if (backend == LU_NUMERIC_BACKEND_SUPERNODE) {
        return LP_BASIS_GOV_BACKEND_SUPERNODE;
    }
    if (backend == LU_NUMERIC_BACKEND_DENSE_GE) {
        return LP_BASIS_GOV_BACKEND_DENSE;
    }
    return LP_BASIS_GOV_BACKEND_NONE;
}

static int lu_sparse_glpk_strict_mode(const LUFactorization *lu) {
    if (!lu || !lu->owner) return 0;
    return lp_glpk_strict_mode_enabled(lu->owner->glpk_strict_mode);
}

static int lu_sparse_strict_prefer_dense_ge_numeric(const LUFactorization *lu) {
    if (!lu || !lu->owner) return 0;
    return lu->owner->lu_strict_prefer_dense_ge_numeric ? 1 : 0;
}

static int lu_sparse_medium_dispatch_prefers_dense_ge(const LUFactorization *lu,
                                                      const SparseMatrix *B) {
    if (!lu || !B || lu_sparse_glpk_strict_mode(lu)) return 0;
    return lu->m >= 340 && lu->m < 500 && B->nnz >= 8 * lu->m;
}

static int lu_sparse_strict_allow_supernode_lane(const LUFactorization *lu) {
    if (!lu || !lu->owner) return 1;
    return lu->owner->lu_strict_allow_supernode_lane ? 1 : 0;
}

static int lu_sparse_strict_allow_symbolic_full_retry(const LUFactorization *lu) {
    if (!lu || !lu->owner) return 1;
    return lu->owner->lu_strict_allow_symbolic_full_retry ? 1 : 0;
}

static int lu_numeric_terminal_failure_reason(int reason_hint,
                                              int saw_identity_sep_failure,
                                              int saw_mkz_singular_failure,
                                              int saw_dense_ge_singular_failure) {
    if (reason_hint != LU_SPARSE_NUMERIC_FAIL_NONE) {
        return reason_hint;
    }
    /* If numeric path hit identity-separation at any point, preserve that
     * terminal reason even if a retry lane later fails singularly. This keeps
     * top-level full-structural sparse retry enabled before dense fallback. */
    if (saw_identity_sep_failure) {
        return LU_SPARSE_NUMERIC_FAIL_IDENTITY_SEPARATION;
    }
    if (saw_mkz_singular_failure || saw_dense_ge_singular_failure) {
        return LU_SPARSE_NUMERIC_FAIL_PATHOLOGICAL;
    }
    return LU_SPARSE_NUMERIC_FAIL_BACKEND_EXHAUSTED;
}

#define SN_COST_GATE_MIN_K 128
#define SN_COST_GATE_TRIP_MIN_CALLS 6
#define SN_COST_GATE_TRIP_MS 120.0
#define SN_COST_GATE_TRIP_RATIO 6.0
#define SN_COST_GATE_SKIP_BUDGET 2
#define SN_COST_GATE_EWMA_ALPHA 0.25
#define SN_COST_GATE_DENSE_RESET_RATIO 0.75

static double lu_cost_gate_update_ewma(double prev, double sample) {
    if (sample <= 0.0) return prev;
    if (prev <= 0.0) return sample;
    return (1.0 - SN_COST_GATE_EWMA_ALPHA) * prev + SN_COST_GATE_EWMA_ALPHA * sample;
}

static void lu_supernode_cost_gate_note_markowitz(LUFactorization *lu,
                                                   int k,
                                                   double markowitz_ms) {
    if (!lu || k < SN_COST_GATE_MIN_K || markowitz_ms <= 0.0) return;
    lu->sn_cost_gate_markowitz_ewma_ms =
        lu_cost_gate_update_ewma(lu->sn_cost_gate_markowitz_ewma_ms, markowitz_ms);
}

static void lu_supernode_cost_gate_note_supernode(LUFactorization *lu,
                                                   int k,
                                                   double supernode_ms) {
    if (!lu || k < SN_COST_GATE_MIN_K || supernode_ms <= 0.0) return;
    lu->sn_cost_gate_supernode_ewma_ms =
        lu_cost_gate_update_ewma(lu->sn_cost_gate_supernode_ewma_ms, supernode_ms);

    if (lu->sn_calls < SN_COST_GATE_TRIP_MIN_CALLS) return;
    if (lu->sn_cost_gate_markowitz_ewma_ms <= 0.0) return;
    if (lu->sn_cost_gate_supernode_ewma_ms < SN_COST_GATE_TRIP_MS) return;
    if (lu->sn_cost_gate_supernode_ewma_ms <
        lu->sn_cost_gate_markowitz_ewma_ms * SN_COST_GATE_TRIP_RATIO) {
        return;
    }

    if (lu->sn_cost_gate_skip_budget < SN_COST_GATE_SKIP_BUDGET) {
        lu->sn_cost_gate_skip_budget = SN_COST_GATE_SKIP_BUDGET;
        lp_telemetry_lu_mark_sn_cost_gate_trip(lu);
    }
}

static int lu_supernode_cost_gate_should_skip(LUFactorization *lu,
                                              int k,
                                              int full_retry_mode) {
    if (!lu || full_retry_mode || k < SN_COST_GATE_MIN_K) return 0;
    if (lu->sn_cost_gate_skip_budget <= 0) return 0;
    lu->sn_cost_gate_skip_budget--;
    lp_telemetry_lu_mark_sn_cost_gate_skip(lu);
    return 1;
}

static void lu_supernode_cost_gate_note_dense_after_skip(LUFactorization *lu,
                                                         int k,
                                                         double dense_ge_ms) {
    if (!lu || k < SN_COST_GATE_MIN_K || dense_ge_ms <= 0.0) return;
    if (lu->sn_cost_gate_supernode_ewma_ms <= 0.0) return;
    if (dense_ge_ms <
        (lu->sn_cost_gate_supernode_ewma_ms * SN_COST_GATE_DENSE_RESET_RATIO)) {
        return;
    }
    if (lu->sn_cost_gate_skip_budget > 0) {
        lu->sn_cost_gate_skip_budget = 0;
        lp_telemetry_lu_mark_sn_cost_gate_reset(lu);
    }
}

/*
 * Numeric factorization: dense GE with partial pivoting on structural columns,
 * identity column placement, COO→CSC conversion, condition estimation.
 *
 * Uses symbolic results from lu_symbolic_analyze() (ws_is_identity, ws_identity_row,
 * ws_identity_val, ws_col_order, ws_col_order_inv, sym_num_identity, sym_k).
 *
 * Returns 0 on success, -1 on failure (singular pivot or alloc failure).
 */
static int lu_numeric_factorize(LUFactorization *lu, const SparseMatrix *B,
                                int num_identity, int k, LUNumericMode mode,
                                int *terminal_failure_reason_out) {
    int m = lu->m;
    int dense_ge_retry_done = 0;
    int supernode_retry_done = 0;
    int force_supernode_attempt = 0;
    int strict_dispatch_mode = (mode == LU_NUMERIC_MODE_STRICT_DISPATCH);
    int skip_sparse_numeric = strict_dispatch_mode &&
        (lu_sparse_strict_prefer_dense_ge_numeric(lu) ||
         lu_sparse_medium_dispatch_prefers_dense_ge(lu, B));
    int full_retry_mode = (mode == LU_NUMERIC_MODE_SYMBOLIC_FULL_RETRY);
    double t_a_struct_build_ms = 0.0;
    double t_markowitz_numeric_ms = 0.0;
    double t_supernode_numeric_ms = 0.0;
    double t_dense_ge_numeric_ms = 0.0;
    double t_identity_placement_ms = 0.0;
    double t_coo_to_csc_ms = 0.0;
    double t_stage_start_ms = 0.0;
    uint64_t mkz_fingerprint = lu->sym_fingerprint;
    int mkz_used_this_call = 0;
    int mkz_bad_outcome_this_call = 0;
    int mkz_attempted_in_full_retry = 0;
    LUNumericBackend backend_used = LU_NUMERIC_BACKEND_NONE;
    int shadow_backend_pick = LP_BASIS_GOV_BACKEND_NONE;
    int terminal_failure_reason_hint = LU_SPARSE_NUMERIC_FAIL_NONE;
    int saw_mkz_singular_failure = 0;
    int saw_dense_ge_singular_failure = 0;
    int mkz_profile_retry_used_this_call = 0;
    int sn_skip_by_cost_gate = 0;
    int identity_sep_failure_this_call = 0;
    int identity_sep_retry_lane = LU_IDSEP_RETRY_LANE_NONE;
#define NUMERIC_COMMIT() do { \
    lp_telemetry_lu_record_numeric_stages(lu, \
        k, \
        t_a_struct_build_ms, \
        t_markowitz_numeric_ms, \
        t_supernode_numeric_ms, \
        t_dense_ge_numeric_ms, \
        t_identity_placement_ms, \
        t_coo_to_csc_ms); \
} while (0)
#define NUMERIC_RETURN(code) do { \
    int __code = (code); \
    if (__code < 0) { \
        int __reason = lu_numeric_terminal_failure_reason( \
            terminal_failure_reason_hint, \
            identity_sep_failure_this_call, \
            saw_mkz_singular_failure, \
            saw_dense_ge_singular_failure); \
        if (terminal_failure_reason_out) { \
            *terminal_failure_reason_out = __reason; \
        } \
        lp_telemetry_lu_mark_sparse_numeric_failure(lu, __reason); \
        if (mkz_profile_retry_used_this_call) { \
            lp_telemetry_lu_mark_mkz_profile_retry_terminal_failure(lu, __reason); \
        } \
    } else if (terminal_failure_reason_out) { \
        *terminal_failure_reason_out = LU_SPARSE_NUMERIC_FAIL_NONE; \
    } \
    NUMERIC_COMMIT(); \
    return __code; \
} while (0)
    if (terminal_failure_reason_out) {
        *terminal_failure_reason_out = LU_SPARSE_NUMERIC_FAIL_NONE;
    }

    int *identity_row = lu->ws_identity_row;
    double *identity_val = lu->ws_identity_val;
    int *col_order = lu->ws_col_order;
    (void)num_identity;  /* Used implicitly: k = m - num_identity */

    /* Use dense_work for A_struct (m×k fits in m×m, row-major layout).
     * Build it lazily: sparse Markowitz consumes B directly and is the common
     * successful backend on large NETLIB bases, so zeroing/scattering this dense
     * buffer before Markowitz is wasted work on that path. */
    double *A_struct = lu->dense_work;
    int a_struct_valid = 0;
#define ENSURE_A_STRUCT_BUILT() do { \
    if (!a_struct_valid) { \
        t_stage_start_ms = lp_telemetry_timer_start(); \
        memset(A_struct, 0, (size_t)m * k * sizeof(double)); \
        for (int astruct_jj = 0; astruct_jj < k; astruct_jj++) { \
            int astruct_j = col_order[astruct_jj]; \
            for (int astruct_p = B->colptr[astruct_j]; \
                 astruct_p < B->colptr[astruct_j + 1]; astruct_p++) { \
                int astruct_row = B->rowidx[astruct_p]; \
                A_struct[(size_t)astruct_row * k + astruct_jj] = B->values[astruct_p]; \
            } \
        } \
        t_a_struct_build_ms += lp_telemetry_timer_elapsed_ms(t_stage_start_ms); \
        a_struct_valid = 1; \
    } \
} while (0)

    /* Dense LU with partial pivoting on the m×k structural part */
    int *row_perm = lu->ws_row_perm;
    int *row_pos = lu->ws_row_pos;
    int *row_is_identity = lu->ws_row_used;
    /* Rebuild reserved-row bitmap directly from identity columns in col_order.
     * This keeps numeric partitioning consistent with the finalized symbolic order. */
    memset(row_is_identity, 0, m * sizeof(int));
    for (int step = k; step < m; step++) {
        int orig_col = col_order[step];
        if (orig_col < 0 || orig_col >= m) {
            terminal_failure_reason_hint = LU_SPARSE_NUMERIC_FAIL_IDENTITY_SEPARATION;
            NUMERIC_RETURN(-1);
        }
        int r = identity_row[orig_col];
        if (r < 0 || r >= m || row_is_identity[r]) {
            terminal_failure_reason_hint = LU_SPARSE_NUMERIC_FAIL_IDENTITY_SEPARATION;
            NUMERIC_RETURN(-1);
        }
        row_is_identity[r] = 1;
    }
    {
        int struct_pos = 0;
        int ident_pos = k;
        if (lu_requested_factorization_type(lu) == LP_GLPK_BFCP_FACTORIZATION_BTF) {
            int *matched_row_for_col = lu->ws_struct_nnz;
            for (int i = 0; i < m; i++) matched_row_for_col[i] = -1;
            for (int row = 0; row < m; row++) {
                int matched_col;
                if (row_is_identity[row]) continue;
                matched_col = lu->ws_row_match_col[row];
                if (matched_col >= 0 && matched_col < m) {
                    matched_row_for_col[matched_col] = row;
                }
            }
            for (int step = 0; step < k; step++) {
                int row = matched_row_for_col[col_order[step]];
                if (row < 0 || row >= m || row_is_identity[row]) {
                    terminal_failure_reason_hint = LU_SPARSE_NUMERIC_FAIL_IDENTITY_SEPARATION;
                    NUMERIC_RETURN(-1);
                }
                row_perm[struct_pos++] = row;
            }
            for (int i = 0; i < m; i++) {
                if (row_is_identity[i]) {
                    row_perm[ident_pos++] = i;
                }
            }
        } else {
            for (int i = 0; i < m; i++) {
                if (row_is_identity[i]) {
                    row_perm[ident_pos++] = i;
                } else {
                    row_perm[struct_pos++] = i;
                }
            }
        }
        if (struct_pos != k || ident_pos != m) {
            NUMERIC_RETURN(-1);
        }
        for (int i = 0; i < m; i++) {
            row_pos[row_perm[i]] = i;
        }
    }

    /* Use pre-allocated COO arrays, grow if needed */
    int coo_needed = m * k + m;  /* Structural entries + identity diagonals */
    if (coo_needed > lu->coo_capacity) {
        int new_cap = coo_needed * 2;
        SAFE_FREE(lu->coo_L_row); SAFE_FREE(lu->coo_L_col); SAFE_FREE(lu->coo_L_val);
        SAFE_FREE(lu->coo_U_row); SAFE_FREE(lu->coo_U_col); SAFE_FREE(lu->coo_U_val);
        lu->coo_L_row = (int*)calloc(new_cap, sizeof(int));
        lu->coo_L_col = (int*)calloc(new_cap, sizeof(int));
        lu->coo_L_val = (double*)calloc(new_cap, sizeof(double));
        lu->coo_U_row = (int*)calloc(new_cap, sizeof(int));
        lu->coo_U_col = (int*)calloc(new_cap, sizeof(int));
        lu->coo_U_val = (double*)calloc(new_cap, sizeof(double));
        lu->coo_capacity = new_cap;
        if (!lu->coo_L_row || !lu->coo_L_col || !lu->coo_L_val ||
            !lu->coo_U_row || !lu->coo_U_col || !lu->coo_U_val) {
            NUMERIC_RETURN(-1);
        }
    }

    int *L_row = lu->coo_L_row;
    int *L_col = lu->coo_L_col;
    double *L_val = lu->coo_L_val;
    int *U_row = lu->coo_U_row;
    int *U_col = lu->coo_U_col;
    double *U_val = lu->coo_U_val;

    int L_nnz = 0, U_nnz = 0;
    lp_telemetry_lu_clear_mkz_last_failure(lu);
    int mkz_skip_by_circuit = 0;
    int mkz_skip_by_global = 0;
    if (!skip_sparse_numeric &&
        lu->mkz_enabled &&
        k >= MARKOWITZ_MIN_K &&
        lp_glpk_strict_allow_lu_sparse_skip_heuristics(
            lu_sparse_glpk_strict_mode(lu))) {
        if (!full_retry_mode) {
            mkz_skip_by_circuit = mkz_circuit_should_skip(lu, mkz_fingerprint);
        }
        if (!mkz_skip_by_circuit) {
            mkz_skip_by_global = mkz_global_skip_should_skip(lu);
        }
    }
    if (lu->basis_governor &&
        lp_basis_governor_get_mode(lu->basis_governor) != LP_BASIS_GOV_MODE_OFF) {
        int mkz_eligible = (!skip_sparse_numeric &&
                            lu->mkz_enabled &&
                            k >= MARKOWITZ_MIN_K &&
                            !mkz_skip_by_circuit &&
                            !mkz_skip_by_global);
        int sn_eligible = (!skip_sparse_numeric &&
                           lu->sn_enabled &&
                           k >= SN_MIN_K &&
                           (!full_retry_mode || mkz_eligible));
        shadow_backend_pick = lp_basis_governor_shadow_decide_lu_backend(
            mkz_eligible,
            sn_eligible);
    }

    /* Try sparse Markowitz factorization if enabled and k is large enough.
     * This exploits sparsity within structural columns, reducing O(k³) to O(nnz×fill).
     * Uses dedicated growable mkz_work to avoid contention with dense_work layout. */
    if (!skip_sparse_numeric && lu->mkz_enabled && k >= MARKOWITZ_MIN_K &&
        !mkz_skip_by_circuit &&
        !mkz_skip_by_global) {
        mkz_used_this_call = 1;
        if (full_retry_mode) {
            mkz_attempted_in_full_retry = 1;
            lp_telemetry_lu_mark_symbolic_full_retry_mkz_attempt(lu);
        }
        int mkz_init_nnz = mkz_count_init_nnz(B, col_order, k);
        size_t mkz_perm_doubles = ((size_t)k * sizeof(int) + sizeof(double) - 1u) / sizeof(double);
        int *mkz_col_perm = NULL;
        int mkz_reg = 0;
        int rc = MKZ_FAIL_NONE;
        int rc_final = MKZ_FAIL_NONE;
        int pool_mult = lu->mkz_pool_mult_hint;
        int profile_count = strict_dispatch_mode ? 1 : 2;
        if (pool_mult < MARKOWITZ_POOL_MULT) pool_mult = MARKOWITZ_POOL_MULT;
        if (pool_mult > MARKOWITZ_POOL_MAX_MULT) pool_mult = MARKOWITZ_POOL_MAX_MULT;

        for (int profile_idx = 0; profile_idx < profile_count; profile_idx++) {
            double profile_threshold_ratio = (profile_idx == 0)
                ? MARKOWITZ_THRESHOLD
                : MARKOWITZ_RETRY_THRESHOLD;
            int profile_max_search = (profile_idx == 0)
                ? MARKOWITZ_MAX_SEARCH
                : MARKOWITZ_RETRY_MAX_SEARCH;
            double profile_singular_retry_threshold = (profile_idx == 0)
                ? MARKOWITZ_SINGULAR_RETRY_THRESHOLD
                : MARKOWITZ_RETRY_SINGULAR_THRESHOLD;
            double profile_reserved_relax_ratio = (profile_idx == 0)
                ? MARKOWITZ_RESERVED_RELAX_RATIO
                : MARKOWITZ_RETRY_RESERVED_RELAX_RATIO;
            int pool_mult_profile = pool_mult;

            if (profile_idx > 0) {
                lp_telemetry_lu_mark_mkz_profile_retry_attempt(lu);
                mkz_profile_retry_used_this_call = 1;
            }

            while (1) {
                int pool_cap_dummy = 0;
                size_t mkz_need = 0;
                if (mkz_compute_workspace_requirements(mkz_init_nnz, m, k, pool_mult_profile,
                                                       &pool_cap_dummy, &mkz_need) != 0) {
                    rc = MKZ_FAIL_WORKSPACE;
                    lp_telemetry_lu_mark_mkz_attempt(lu);
                    mkz_record_failure_reason(lu, rc);
                    break;
                }

                if (mkz_need > ((size_t)-1) - mkz_perm_doubles ||
                    mkz_workspace_reserve(lu, mkz_perm_doubles + mkz_need) != 0) {
                    rc = MKZ_FAIL_WORKSPACE;
                    lp_telemetry_lu_mark_mkz_attempt(lu);
                    mkz_record_failure_reason(lu, rc);
                    break;
                }

                mkz_col_perm = (int *)lu->mkz_work;
                double *mkz_workspace = lu->mkz_work + mkz_perm_doubles;
                size_t mkz_ws_doubles = lu->mkz_work_capacity - mkz_perm_doubles;

                lp_telemetry_lu_mark_mkz_attempt(lu);
                t_stage_start_ms = lp_telemetry_timer_start();
                rc = lu_factorize_markowitz(
                    lu, B, col_order, m, k, mkz_init_nnz, row_perm, row_pos, lu->pivot_tol,
                    row_is_identity,
                    lu->redundant_rows, lu->num_redundant,
                    lu->allow_regularization, lu->max_regularizations, &mkz_reg,
                    L_row, L_col, L_val, &L_nnz, lu->coo_capacity,
                    U_row, U_col, U_val, &U_nnz, lu->coo_capacity,
                    mkz_col_perm, pool_mult_profile,
                    profile_threshold_ratio, profile_max_search,
                    profile_singular_retry_threshold, profile_reserved_relax_ratio,
                    mkz_workspace, mkz_ws_doubles);
                t_markowitz_numeric_ms += lp_telemetry_timer_elapsed_ms(t_stage_start_ms);
                if (rc == 0) break;
                mkz_record_failure_reason(lu, rc);
                if (rc != MKZ_FAIL_POOL || pool_mult_profile >= MARKOWITZ_POOL_MAX_MULT) break;

                {
                    int next_pool_mult = pool_mult_profile * 2;
                    if (next_pool_mult < MARKOWITZ_POOL_RETRY_MULT) {
                        next_pool_mult = MARKOWITZ_POOL_RETRY_MULT;
                    }
                    if (next_pool_mult <= pool_mult_profile) {
                        next_pool_mult = pool_mult_profile + 1;
                    }
                    if (next_pool_mult > MARKOWITZ_POOL_MAX_MULT) {
                        next_pool_mult = MARKOWITZ_POOL_MAX_MULT;
                    }
                    pool_mult_profile = next_pool_mult;
                    lu->mkz_pool_mult_hint = pool_mult_profile;
                }

                L_nnz = 0;
                U_nnz = 0;
                {
                    int struct_pos = 0;
                    int ident_pos = k;
                    for (int i = 0; i < m; i++) {
                        if (row_is_identity[i]) {
                            row_perm[ident_pos++] = i;
                        } else {
                            row_perm[struct_pos++] = i;
                        }
                    }
                    for (int i = 0; i < m; i++) {
                        row_pos[row_perm[i]] = i;
                    }
                }
            }

            if (rc == 0) {
                rc_final = 0;
                pool_mult = pool_mult_profile;
                if (profile_idx > 0) {
                    lp_telemetry_lu_mark_mkz_profile_retry_success(lu);
                }
                break;
            }

            if (profile_idx > 0) {
                lp_telemetry_lu_mark_mkz_profile_retry_failure(lu);
            }
            rc_final = rc;

            if (profile_idx + 1 >= profile_count || rc != MKZ_FAIL_SINGULAR) {
                break;
            }

            /* Retry once with a more permissive candidate profile. */
            L_nnz = 0;
            U_nnz = 0;
            {
                int struct_pos = 0;
                int ident_pos = k;
                for (int i = 0; i < m; i++) {
                    if (row_is_identity[i]) {
                        row_perm[ident_pos++] = i;
                    } else {
                        row_perm[struct_pos++] = i;
                    }
                }
                for (int i = 0; i < m; i++) {
                    row_pos[row_perm[i]] = i;
                }
            }
        }

        rc = rc_final;
        if (rc == 0) {
            /* N1-B: Shadow stability retry. Compute cond from U COO diagonals.
             * If cond exceeds trigger, re-run Markowitz with tighter params.
             * The primary was bad, so overwriting is acceptable. */
            {
                double mkz_min_diag = RALPH_INFINITY;
                double mkz_max_diag = 0.0;
                for (int i = 0; i < U_nnz; i++) {
                    if (U_row[i] == U_col[i]) {
                        double av = fabs(U_val[i]);
                        if (av > RALPH_ZERO_TOL) {
                            if (av < mkz_min_diag) mkz_min_diag = av;
                            if (av > mkz_max_diag) mkz_max_diag = av;
                        }
                    }
                }
                double mkz_cond = (mkz_min_diag > RALPH_ZERO_TOL)
                    ? mkz_max_diag / mkz_min_diag : RALPH_INFINITY;

                if (mkz_cond > MKZ_SHADOW_COND_TRIGGER && !strict_dispatch_mode) {
                    if (lu->telemetry_enabled) {
                        lu->telemetry.mkz_high_cond_count++;
                    }
                    /* Save primary COO state before shadow overwrites it */
                    int saved_L_nnz = L_nnz;
                    int saved_U_nnz = U_nnz;
                    int saved_mkz_reg = mkz_reg;
                    size_t coo_save_size = (size_t)(L_nnz + U_nnz);
                    int *saved_L_row = NULL, *saved_L_col = NULL;
                    double *saved_L_val = NULL;
                    int *saved_U_row = NULL, *saved_U_col = NULL;
                    double *saved_U_val = NULL;
                    int *saved_mkz_col_perm = NULL;
                    int shadow_attempted = 0;

                    if (coo_save_size > 0 && coo_save_size < (size_t)INT_MAX / 4) {
                        saved_L_row = (int *)malloc((size_t)L_nnz * sizeof(int));
                        saved_L_col = (int *)malloc((size_t)L_nnz * sizeof(int));
                        saved_L_val = (double *)malloc((size_t)L_nnz * sizeof(double));
                        saved_U_row = (int *)malloc((size_t)U_nnz * sizeof(int));
                        saved_U_col = (int *)malloc((size_t)U_nnz * sizeof(int));
                        saved_U_val = (double *)malloc((size_t)U_nnz * sizeof(double));
                        saved_mkz_col_perm = (int *)malloc((size_t)k * sizeof(int));
                    }
                    int *saved_row_perm = (int *)malloc((size_t)m * sizeof(int));
                    int *saved_row_pos = (int *)malloc((size_t)m * sizeof(int));
                    if (saved_L_row && saved_L_col && saved_L_val &&
                        saved_U_row && saved_U_col && saved_U_val &&
                        saved_mkz_col_perm && saved_row_perm && saved_row_pos) {
                        memcpy(saved_L_row, L_row, (size_t)L_nnz * sizeof(int));
                        memcpy(saved_L_col, L_col, (size_t)L_nnz * sizeof(int));
                        memcpy(saved_L_val, L_val, (size_t)L_nnz * sizeof(double));
                        memcpy(saved_U_row, U_row, (size_t)U_nnz * sizeof(int));
                        memcpy(saved_U_col, U_col, (size_t)U_nnz * sizeof(int));
                        memcpy(saved_U_val, U_val, (size_t)U_nnz * sizeof(double));
                        memcpy(saved_mkz_col_perm, mkz_col_perm, (size_t)k * sizeof(int));
                        memcpy(saved_row_perm, row_perm, (size_t)m * sizeof(int));
                        memcpy(saved_row_pos, row_pos, (size_t)m * sizeof(int));

                        /* Re-populate A_struct for shadow Markowitz */
                        memset(A_struct, 0, (size_t)k * k * sizeof(double));
                        for (int jj = 0; jj < k; jj++) {
                            int orig_col = col_order[jj];
                            for (int p = B->colptr[orig_col]; p < B->colptr[orig_col + 1]; p++) {
                                int orig_row = B->rowidx[p];
                                int perm_row = row_pos[orig_row];
                                if (perm_row >= 0 && perm_row < k) {
                                    A_struct[(size_t)perm_row * k + jj] = B->values[p];
                                }
                            }
                        }

                        int shadow_pool_mult = pool_mult;
                        if (shadow_pool_mult < MARKOWITZ_POOL_MULT)
                            shadow_pool_mult = MARKOWITZ_POOL_MULT;
                        size_t mkz_need = 0;
                        int pool_cap_dummy = 0;
                        if (mkz_compute_workspace_requirements(mkz_init_nnz, m, k, shadow_pool_mult,
                                                               &pool_cap_dummy, &mkz_need) == 0 &&
                            mkz_workspace_reserve(lu, mkz_perm_doubles + mkz_need) == 0) {
                            int *shadow_mkz_perm = (int *)lu->mkz_work;
                            double *shadow_ws = lu->mkz_work + mkz_perm_doubles;
                            size_t shadow_ws_d = lu->mkz_work_capacity - mkz_perm_doubles;
                            int shadow_L_nnz = 0, shadow_U_nnz = 0, shadow_reg = 0;
                            shadow_attempted = 1;
                            int shadow_rc = lu_factorize_markowitz(
                                lu, B, col_order, m, k, mkz_init_nnz,
                                row_perm, row_pos, lu->pivot_tol,
                                row_is_identity,
                                lu->redundant_rows, lu->num_redundant,
                                lu->allow_regularization, lu->max_regularizations, &shadow_reg,
                                L_row, L_col, L_val, &shadow_L_nnz, lu->coo_capacity,
                                U_row, U_col, U_val, &shadow_U_nnz, lu->coo_capacity,
                                shadow_mkz_perm, shadow_pool_mult,
                                MKZ_SHADOW_THRESHOLD, MKZ_SHADOW_MAX_SEARCH,
                                MARKOWITZ_SINGULAR_RETRY_THRESHOLD,
                                MARKOWITZ_RESERVED_RELAX_RATIO,
                                shadow_ws, shadow_ws_d);
                            int accept_shadow = 0;
                            if (shadow_rc == 0) {
                                double shadow_min = RALPH_INFINITY, shadow_max = 0.0;
                                for (int i = 0; i < shadow_U_nnz; i++) {
                                    if (U_row[i] == U_col[i]) {
                                        double av = fabs(U_val[i]);
                                        if (av > RALPH_ZERO_TOL) {
                                            if (av < shadow_min) shadow_min = av;
                                            if (av > shadow_max) shadow_max = av;
                                        }
                                    }
                                }
                                double shadow_cond = (shadow_min > RALPH_ZERO_TOL)
                                    ? shadow_max / shadow_min : RALPH_INFINITY;
                                if (shadow_cond < mkz_cond) {
                                    accept_shadow = 1;
                                    L_nnz = shadow_L_nnz;
                                    U_nnz = shadow_U_nnz;
                                    mkz_reg = shadow_reg;
                                    mkz_col_perm = shadow_mkz_perm;
                                }
                            }
                            if (!accept_shadow) {
                                /* Restore primary COO + row permutation */
                                L_nnz = saved_L_nnz;
                                U_nnz = saved_U_nnz;
                                mkz_reg = saved_mkz_reg;
                                memcpy(L_row, saved_L_row, (size_t)L_nnz * sizeof(int));
                                memcpy(L_col, saved_L_col, (size_t)L_nnz * sizeof(int));
                                memcpy(L_val, saved_L_val, (size_t)L_nnz * sizeof(double));
                                memcpy(U_row, saved_U_row, (size_t)U_nnz * sizeof(int));
                                memcpy(U_col, saved_U_col, (size_t)U_nnz * sizeof(int));
                                memcpy(U_val, saved_U_val, (size_t)U_nnz * sizeof(double));
                                memcpy(mkz_col_perm, saved_mkz_col_perm, (size_t)k * sizeof(int));
                                memcpy(row_perm, saved_row_perm, (size_t)m * sizeof(int));
                                memcpy(row_pos, saved_row_pos, (size_t)m * sizeof(int));
                            }
                        }
                    }
                    free(saved_L_row); free(saved_L_col); free(saved_L_val);
                    free(saved_U_row); free(saved_U_col); free(saved_U_val);
                    free(saved_mkz_col_perm);
                    free(saved_row_perm); free(saved_row_pos);
                    (void)shadow_attempted;
                }
            }

            lp_telemetry_lu_mark_mkz_success(lu);
            mkz_global_skip_note_reset(lu);
            lu_supernode_cost_gate_note_markowitz(lu, k, t_markowitz_numeric_ms);
            if (full_retry_mode) {
                lp_telemetry_lu_mark_symbolic_full_retry_mkz_success(lu);
            }
            lu->num_regularized = mkz_reg;
            lu->mkz_pool_mult_hint = pool_mult;
            backend_used = LU_NUMERIC_BACKEND_MARKOWITZ;

            /* Markowitz emits L_col/U_col in structural column space (0..k-1).
             * The COO→CSC path and identity_placement expect step-indexed columns.
             * mkz_col_perm[step] = structural index chosen at each step.
             * Build inverse: structural_col → step, then remap all L/U col entries. */

            /* Build mkz_col_perm_inv in A_struct (consumed by Markowitz) */
            int *mkz_inv = (int *)A_struct;
            for (int s = 0; s < k; s++)
                mkz_inv[mkz_col_perm[s]] = s;

            /* Remap L_col and U_col from structural→step space */
            for (int i = 0; i < L_nnz; i++) {
                int sc = L_col[i];
                if (sc >= 0 && sc < k) L_col[i] = mkz_inv[sc];
            }
            for (int i = 0; i < U_nnz; i++) {
                int sc = U_col[i];
                if (sc >= 0 && sc < k) U_col[i] = mkz_inv[sc];
            }

            /* Reorder col_order to match Markowitz pivot ordering:
             * new_col_order[step] = old_col_order[mkz_col_perm[step]] */
            {
                int *temp_order = mkz_inv + k;  /* Reuse space past inverse */
                for (int s = 0; s < k; s++)
                    temp_order[s] = col_order[mkz_col_perm[s]];
                for (int s = 0; s < k; s++)
                    col_order[s] = temp_order[s];
                /* Rebuild col_order_inv for identity columns */
                int *col_order_inv = lu->ws_col_order_inv;
                for (int s = 0; s < m; s++)
                    col_order_inv[col_order[s]] = s;
            }

            goto identity_placement;
        }
        lp_telemetry_lu_mark_mkz_failure(lu, rc);
        lu_supernode_cost_gate_note_markowitz(lu, k, t_markowitz_numeric_ms);
        if (full_retry_mode && mkz_attempted_in_full_retry) {
            lp_telemetry_lu_mark_symbolic_full_retry_mkz_failure(lu);
        }
        if (rc == MKZ_FAIL_SINGULAR) {
            mkz_bad_outcome_this_call = 1;
            saw_mkz_singular_failure = 1;
            mkz_global_skip_note_singular_bad_outcome(lu);
            mkz_circuit_note_bad_outcome(lu, mkz_fingerprint);
        } else {
            mkz_global_skip_note_reset(lu);
        }

        if (strict_dispatch_mode) {
            NUMERIC_RETURN(-1);
        }

        /* Markowitz failed — reset and fall through to supernodal/dense */
        L_nnz = 0;
        U_nnz = 0;
        {
            int struct_pos = 0;
            int ident_pos = k;
            for (int i = 0; i < m; i++) {
                if (row_is_identity[i]) {
                    row_perm[ident_pos++] = i;
                } else {
                    row_perm[struct_pos++] = i;
                }
            }
            for (int i = 0; i < m; i++) {
                row_pos[row_perm[i]] = i;
            }
        }
    }

    /* T2.1: Try supernodal factorization if enabled and k is large enough */
supernode_factorization:
    if (!skip_sparse_numeric && lu_sparse_strict_allow_supernode_lane(lu) &&
        lu->sn_enabled && k >= SN_MIN_K &&
        (!full_retry_mode || mkz_attempted_in_full_retry)) {
        if (!force_supernode_attempt &&
            lp_glpk_strict_allow_lu_sparse_skip_heuristics(
                lu_sparse_glpk_strict_mode(lu)) &&
            lu_supernode_cost_gate_should_skip(lu, k, full_retry_mode)) {
            sn_skip_by_cost_gate = 1;
        } else {
            force_supernode_attempt = 0;
            lu->sn_calls++;
            ENSURE_A_STRUCT_BUILT();
            /* Build or reuse symbolic analysis */
            SNSymbolic *sn_sym = lu->sn_symbolic;
            if (!sn_sym || sn_sym->k != k || sn_sym->m != m) {
                /* Invalidate stale cached analysis */
                if (sn_sym) {
                    sn_symbolic_free(sn_sym);
                    lu->sn_symbolic = NULL;
                }
                sn_sym = sn_analyze(A_struct, m, k, row_perm);
                lu->sn_symbolic = sn_sym;
            }

            if (sn_sym && sn_sym->num_supernodes > 0) {
                /* Pre-allocate workspace: 3 * m * max_sn_size covers L+U+C blocks */
                size_t sn_need = (size_t)3 * m * (sn_sym->max_supernode_size > 0 ?
                                 sn_sym->max_supernode_size : 1);
                (void)sn_workspace_reserve(lu, sn_need);

                int sn_reg = 0;
                SNSupernodeWork sn_work_stats;
                int sn_phase_timing_sampled = 0;
                memset(&sn_work_stats, 0, sizeof(sn_work_stats));
                sn_phase_timing_sampled = ((lu->sn_calls & 63) == 0);
                sn_work_stats.phase_timing_sampled = sn_phase_timing_sampled;
                t_stage_start_ms = lp_telemetry_timer_start();
                int rc = sn_factorize(A_struct, m, k, row_perm, row_pos,
                                      lu->pivot_tol, row_is_identity,
                                      sn_sym->supernodes, sn_sym->num_supernodes,
                                      lu->redundant_rows, lu->num_redundant,
                                      lu->allow_regularization,
                                      lu->max_regularizations, &sn_reg,
                                      L_row, L_col, L_val, &L_nnz,
                                      lu->coo_capacity,
                                      U_row, U_col, U_val, &U_nnz,
                                      lu->coo_capacity,
                                      lu->sn_work, lu->sn_work_capacity,
                                      &sn_work_stats);
                {
                    double sn_attempt_ms = lp_telemetry_timer_elapsed_ms(t_stage_start_ms);
                    t_supernode_numeric_ms += sn_attempt_ms;
                    lu_supernode_cost_gate_note_supernode(lu, k, sn_attempt_ms);
                    lp_telemetry_lu_add_supernode_work(
                        lu,
                        sn_phase_timing_sampled ? 1u : 0u,
                        sn_work_stats.panel_factor_ms,
                        sn_work_stats.panel_pivot_search_ms,
                        sn_work_stats.panel_swap_scatter_ms,
                        sn_work_stats.panel_eliminate_ms,
                        sn_work_stats.panel_pivot_search_calls,
                        sn_work_stats.panel_pivot_search_entries_total,
                        sn_work_stats.panel_pivot_search_size1_calls,
                        sn_work_stats.panel_pivot_search_size1_ms,
                        sn_work_stats.panel_pivot_search_size2_calls,
                        sn_work_stats.panel_pivot_search_size2_ms,
                        sn_work_stats.panel_pivot_search_size3_4_calls,
                        sn_work_stats.panel_pivot_search_size3_4_ms,
                        sn_work_stats.panel_pivot_search_size5_8_calls,
                        sn_work_stats.panel_pivot_search_size5_8_ms,
                        sn_work_stats.panel_pivot_search_size9p_calls,
                        sn_work_stats.panel_pivot_search_size9p_ms,
                        sn_work_stats.panel_pivot_search_reserved_present_calls,
                        sn_work_stats.panel_pivot_search_reserved_present_entries,
                        sn_work_stats.panel_pivot_search_reserved_present_ms,
                        sn_work_stats.panel_pivot_search_reserved_alt_chosen_calls,
                        sn_work_stats.panel_pivot_search_reserved_alt_chosen_ms,
                        sn_work_stats.size1_u_emit_calls,
                        sn_work_stats.size1_u_emit_ms,
                        sn_work_stats.size1_update_scan_calls,
                        sn_work_stats.size1_update_scan_ms,
                        sn_work_stats.size1_update_apply_calls,
                        sn_work_stats.size1_update_apply_ms,
                        sn_work_stats.size1_update_row_gather_ms,
                        sn_work_stats.size1_update_col_indirection_ms,
                        sn_work_stats.size1_update_outer_product_ms,
                        sn_work_stats.size1_update_full_calls,
                        sn_work_stats.size1_update_full_ms,
                        sn_work_stats.size1_update_cols1_calls,
                        sn_work_stats.size1_update_cols1_ms,
                        sn_work_stats.size1_update_cols2_calls,
                        sn_work_stats.size1_update_cols2_ms,
                        sn_work_stats.size1_update_cols3_calls,
                        sn_work_stats.size1_update_cols3_ms,
                        sn_work_stats.size1_update_cols4_calls,
                        sn_work_stats.size1_update_cols4_ms,
                        sn_work_stats.size1_update_cols5p_calls,
                        sn_work_stats.size1_update_cols5p_ms,
                        sn_work_stats.size1_update_cols5p_rows1_8_calls,
                        sn_work_stats.size1_update_cols5p_rows1_8_ms,
                        sn_work_stats.size1_update_cols5p_rows9_32_calls,
                        sn_work_stats.size1_update_cols5p_rows9_32_ms,
                        sn_work_stats.size1_update_cols5p_rows33_128_calls,
                        sn_work_stats.size1_update_cols5p_rows33_128_ms,
                        sn_work_stats.size1_update_cols5p_rows129p_calls,
                        sn_work_stats.size1_update_cols5p_rows129p_ms,
                        sn_work_stats.u_emit_ms,
                        sn_work_stats.active_set_ms,
                        sn_work_stats.pack_blocks_ms,
                        sn_work_stats.full_update_ms,
                        sn_work_stats.compact_update_ms,
                        sn_work_stats.active_row_scan_entries,
                        sn_work_stats.active_col_scan_entries,
                        sn_work_stats.trailing_rows_total,
                        sn_work_stats.trailing_cols_total,
                        sn_work_stats.active_rows_total,
                        sn_work_stats.active_cols_total,
                        sn_work_stats.pack_l_entries_total,
                        sn_work_stats.pack_u_entries_total,
                        sn_work_stats.dense_triplets_total,
                        sn_work_stats.compact_triplets_total,
                        sn_work_stats.full_update_calls,
                        sn_work_stats.compact_update_calls,
                        sn_work_stats.skipped_update_calls,
                        sn_work_stats.compact_cols1_calls,
                        sn_work_stats.compact_cols1_rows_total,
                        sn_work_stats.compact_cols1_ms,
                        sn_work_stats.compact_cols2_calls,
                        sn_work_stats.compact_cols2_rows_total,
                        sn_work_stats.compact_cols2_ms,
                        sn_work_stats.compact_cols3_calls,
                        sn_work_stats.compact_cols3_rows_total,
                        sn_work_stats.compact_cols3_ms,
                        sn_work_stats.compact_cols4_calls,
                        sn_work_stats.compact_cols4_rows_total,
                        sn_work_stats.compact_cols4_ms,
                        sn_work_stats.compact_cols5p_calls,
                        sn_work_stats.compact_cols5p_rows_total,
                        sn_work_stats.compact_cols5p_ms);
                }
                if (rc == 0) {
                    lu->sn_successes++;
                    lu->num_regularized = sn_reg;
                    backend_used = LU_NUMERIC_BACKEND_SUPERNODE;
                    /* Skip column-by-column GE, go straight to identity placement */
                    goto identity_placement;
                }
                /* Supernodal failed — reset and fall through to column-by-column */
                L_nnz = 0;
                U_nnz = 0;
                /* Restore row ordering: structural rows first, identity rows last. */
                {
                    int struct_pos = 0;
                    int ident_pos = k;
                    for (int i = 0; i < m; i++) {
                        if (row_is_identity[i]) {
                            row_perm[ident_pos++] = i;
                        } else {
                            row_perm[struct_pos++] = i;
                        }
                    }
                    for (int i = 0; i < m; i++) {
                        row_pos[row_perm[i]] = i;
                    }
                }
                /* Re-populate A_struct from B (sn_factorize modifies it in-place) */
                t_stage_start_ms = lp_telemetry_timer_start();
                memset(A_struct, 0, (size_t)m * k * sizeof(double));
                for (int jj = 0; jj < k; jj++) {
                    int j = col_order[jj];
                    for (int p = B->colptr[j]; p < B->colptr[j + 1]; p++) {
                        int row = B->rowidx[p];
                        A_struct[row * k + jj] = B->values[p];
                    }
                }
                t_a_struct_build_ms += lp_telemetry_timer_elapsed_ms(t_stage_start_ms);
                a_struct_valid = 1;
            }
        }
    }

dense_ge_factorization:
    backend_used = LU_NUMERIC_BACKEND_DENSE_GE;
    ENSURE_A_STRUCT_BUILT();
    /* LU factorization of structural columns with partial pivoting */
    t_stage_start_ms = lp_telemetry_timer_start();
    for (int step = 0; step < k; step++) {
        /* Find pivot in column step using only structural rows (step..k-1). */
        int pivot_row = -1;
        double max_val = 0.0;

        for (int i = step; i < k; i++) {
            int orig_row = row_perm[i];
            double val = fabs(A_struct[orig_row * k + step]);
            if (val > max_val) {
                max_val = val;
                pivot_row = i;
            }
        }

        if (max_val < lu->pivot_tol) {
            /* Structural part is singular at this step.
             * Check for redundant rows that can be regularized. */
            int can_regularize = 0;

            /* Check pre-marked redundant rows */
            if (lu->redundant_rows && lu->num_redundant > 0) {
                for (int i = step; i < k; i++) {
                    int orig_row = row_perm[i];
                    if (lu->redundant_rows[orig_row]) {
                        can_regularize = 1;
                        pivot_row = i;
                        break;
                    }
                }
            }

            /* Check allow_regularization flag */
            if (!can_regularize && lu->allow_regularization &&
                lu->num_regularized < lu->max_regularizations) {
                can_regularize = 1;
                pivot_row = step;
            }

            if (can_regularize) {
                /* Regularize: set diagonal to 1.0 */
                lu->num_regularized++;
                if (pivot_row != step) {
                    int a = row_perm[step], b = row_perm[pivot_row];
                    row_perm[step] = b; row_perm[pivot_row] = a;
                    row_pos[b] = step; row_pos[a] = pivot_row;
                    pivot_row = step; /* Prevent double-swap below */
                }
                int piv_orig = row_perm[step];
                A_struct[piv_orig * k + step] = 1.0;
                max_val = 1.0;
            } else {
                saw_dense_ge_singular_failure = 1;
                NUMERIC_RETURN(-1);
            }
        }

        /* Swap rows in permutation + inverse */
        if (pivot_row != step) {
            int a = row_perm[step], b = row_perm[pivot_row];
            row_perm[step] = b; row_perm[pivot_row] = a;
            row_pos[b] = step; row_pos[a] = pivot_row;
        }

        int piv_orig = row_perm[step];
        double pivot_val = A_struct[piv_orig * k + step];

        /* Store L diagonal */
        L_row[L_nnz] = piv_orig;
        L_col[L_nnz] = step;
        L_val[L_nnz] = 1.0;
        L_nnz++;

        /* Store U row: U[step, jj] for jj >= step */
        for (int jj = step; jj < k; jj++) {
            double val = A_struct[piv_orig * k + jj];
            if (fabs(val) > RALPH_ZERO_TOL || jj == step) {
                U_row[U_nnz] = step;
                U_col[U_nnz] = jj;
                U_val[U_nnz] = val;
                U_nnz++;
            }
        }

        /* Eliminate: compute multipliers and update remaining rows */
        for (int i = step + 1; i < m; i++) {
            int row_orig = row_perm[i];
            double a_ik = A_struct[row_orig * k + step];

            if (fabs(a_ik) < RALPH_ZERO_TOL) continue;

            double mult = a_ik / pivot_val;

            /* Store L multiplier */
            L_row[L_nnz] = row_orig;
            L_col[L_nnz] = step;
            L_val[L_nnz] = mult;
            L_nnz++;

            /* Row-major inner loop — stride-1 for cache + auto-vectorization */
            for (int jj = step + 1; jj < k; jj++) {
                A_struct[row_orig * k + jj] -= mult * A_struct[piv_orig * k + jj];
            }
        }
    }
    t_dense_ge_numeric_ms += lp_telemetry_timer_elapsed_ms(t_stage_start_ms);
    if (sn_skip_by_cost_gate) {
        lu_supernode_cost_gate_note_dense_after_skip(lu, k, t_dense_ge_numeric_ms);
    }

identity_placement:
    /* Handle identity columns (steps k..m-1).
     * Use row_pos[] for O(1) lookup instead of O(n) linear scan. */
    t_stage_start_ms = lp_telemetry_timer_start();
    for (int step = k; step < m; step++) {
        int orig_col = col_order[step];  /* Original identity column */
        int orig_row = identity_row[orig_col];
        double val = identity_val[orig_col];

        /* O(1) lookup via inverse permutation array */
        int perm_pos = row_pos[orig_row];

        if (perm_pos < step) {
            /* Row already used at identity placement.
             * Select retry lane adaptively:
             * - dense-GE lane by default
             * - supernode lane after repeated same-signature identity-separation
             * Keep retry in sparse-efficient path (no top-level dense fallback). */
            int retry_lane = LU_IDSEP_RETRY_LANE_DENSE;
            identity_sep_failure_this_call = 1;
            lp_telemetry_lu_mark_identity_sep_failure(lu);
            if (mkz_used_this_call) {
                mkz_bad_outcome_this_call = 1;
                mkz_circuit_note_bad_outcome(lu, mkz_fingerprint);
            }
            if (lu->idsep_retry_fingerprint == mkz_fingerprint) {
                if (lu->idsep_retry_streak < INT_MAX) {
                    lu->idsep_retry_streak++;
                }
            } else {
                lu->idsep_retry_fingerprint = mkz_fingerprint;
                lu->idsep_retry_streak = 1;
            }
            if (strict_dispatch_mode) {
                terminal_failure_reason_hint = LU_SPARSE_NUMERIC_FAIL_IDENTITY_SEPARATION;
                NUMERIC_RETURN(-1);
            }
            retry_lane = lu_identity_sep_retry_lane_plan(
                lu->idsep_retry_streak,
                lu->sn_enabled,
                k);
            if (retry_lane == LU_IDSEP_RETRY_LANE_SUPERNODE && supernode_retry_done) {
                retry_lane = LU_IDSEP_RETRY_LANE_DENSE;
            }

            if (retry_lane == LU_IDSEP_RETRY_LANE_SUPERNODE) {
                lp_telemetry_lu_mark_identity_sep_retry_lane_chosen(
                    lu, LU_IDSEP_RETRY_LANE_SUPERNODE);
                identity_sep_retry_lane = LU_IDSEP_RETRY_LANE_SUPERNODE;
                supernode_retry_done = 1;
                force_supernode_attempt = 1;
                L_nnz = 0;
                U_nnz = 0;

                {
                    int struct_pos = 0;
                    int ident_pos = k;
                    for (int i = 0; i < m; i++) {
                        if (row_is_identity[i]) {
                            row_perm[ident_pos++] = i;
                        } else {
                            row_perm[struct_pos++] = i;
                        }
                    }
                    for (int i = 0; i < m; i++) {
                        row_pos[row_perm[i]] = i;
                    }
                }

                t_stage_start_ms = lp_telemetry_timer_start();
                memset(A_struct, 0, (size_t)m * k * sizeof(double));
                for (int jj = 0; jj < k; jj++) {
                    int j = col_order[jj];
                    for (int p = B->colptr[j]; p < B->colptr[j + 1]; p++) {
                        int row = B->rowidx[p];
                        A_struct[row * k + jj] = B->values[p];
                    }
                }
                t_a_struct_build_ms += lp_telemetry_timer_elapsed_ms(t_stage_start_ms);
                a_struct_valid = 1;
                goto supernode_factorization;
            }

            if (!dense_ge_retry_done) {
                dense_ge_retry_done = 1;
                lp_telemetry_lu_mark_identity_sep_retry_lane_chosen(
                    lu, LU_IDSEP_RETRY_LANE_DENSE);
                identity_sep_retry_lane = LU_IDSEP_RETRY_LANE_DENSE;
                skip_sparse_numeric = 1;
                L_nnz = 0;
                U_nnz = 0;

                {
                    int struct_pos = 0;
                    int ident_pos = k;
                    for (int i = 0; i < m; i++) {
                        if (row_is_identity[i]) {
                            row_perm[ident_pos++] = i;
                        } else {
                            row_perm[struct_pos++] = i;
                        }
                    }
                    for (int i = 0; i < m; i++) {
                        row_pos[row_perm[i]] = i;
                    }
                }

                t_stage_start_ms = lp_telemetry_timer_start();
                memset(A_struct, 0, (size_t)m * k * sizeof(double));
                for (int jj = 0; jj < k; jj++) {
                    int j = col_order[jj];
                    for (int p = B->colptr[j]; p < B->colptr[j + 1]; p++) {
                        int row = B->rowidx[p];
                        A_struct[row * k + jj] = B->values[p];
                    }
                }
                t_a_struct_build_ms += lp_telemetry_timer_elapsed_ms(t_stage_start_ms);
                a_struct_valid = 1;
                goto dense_ge_factorization;
            }
            terminal_failure_reason_hint = LU_SPARSE_NUMERIC_FAIL_IDENTITY_SEPARATION;
            NUMERIC_RETURN(-1);
        }

        /* Swap to bring this row to position step + update inverse */
        if (perm_pos != step) {
            int a = row_perm[step], b = row_perm[perm_pos];
            row_perm[step] = b; row_perm[perm_pos] = a;
            row_pos[b] = step; row_pos[a] = perm_pos;
        }

        /* L diagonal = 1, U diagonal = val (±1) */
        L_row[L_nnz] = orig_row;
        L_col[L_nnz] = step;
        L_val[L_nnz] = 1.0;
        L_nnz++;

        U_row[U_nnz] = step;
        U_col[U_nnz] = step;
        U_val[U_nnz] = val;
        U_nnz++;
    }
    t_identity_placement_ms += lp_telemetry_timer_elapsed_ms(t_stage_start_ms);

    /* L entries were emitted in original-row space. Convert them once after all
     * row swaps (structural pivoting + identity placement) are complete. */
    for (int i = 0; i < L_nnz; i++) {
        L_row[i] = row_pos[L_row[i]];
    }

    /* Build final permutation arrays */
    for (int i = 0; i < m; i++) {
        lu->perm[i] = row_perm[i];
        lu->perm_inv[row_perm[i]] = i;
        lu->col_perm[i] = col_order[i];
        lu->col_perm_inv[col_order[i]] = i;
    }

    /* Convert L and U from COO to CSC — reuse pre-allocated arrays */
    t_stage_start_ms = lp_telemetry_timer_start();
    int needed = L_nnz > U_nnz ? L_nnz : U_nnz;
    if (needed > lu->LU_out_capacity) {
        int new_cap = needed * 2;
        SAFE_FREE(lu->L_rowidx); SAFE_FREE(lu->L_values);
        SAFE_FREE(lu->U_rowidx); SAFE_FREE(lu->U_values);
        lu->L_rowidx = (int*)calloc(new_cap, sizeof(int));
        lu->L_values = (double*)calloc(new_cap, sizeof(double));
        lu->U_rowidx = (int*)calloc(new_cap, sizeof(int));
        lu->U_values = (double*)calloc(new_cap, sizeof(double));
        lu->LU_out_capacity = new_cap;
        if (!lu->L_rowidx || !lu->L_values || !lu->U_rowidx || !lu->U_values) {
            NUMERIC_RETURN(-1);
        }
    }
    memset(lu->L_colptr, 0, (m + 1) * sizeof(int));
    memset(lu->U_colptr, 0, (m + 1) * sizeof(int));

    /* Count entries per column for L */
    for (int i = 0; i < L_nnz; i++) {
        lu->L_colptr[L_col[i] + 1]++;
    }
    for (int j = 0; j < m; j++) {
        lu->L_colptr[j + 1] += lu->L_colptr[j];
    }

    int *L_pos = lu->ws_L_pos;
    memset(L_pos, 0, m * sizeof(int));
    for (int i = 0; i < L_nnz; i++) {
        int col = L_col[i];
        int pos = lu->L_colptr[col] + L_pos[col]++;
        lu->L_rowidx[pos] = L_row[i];
        lu->L_values[pos] = L_val[i];
    }
    lu->nnz_L = L_nnz;

    /* Count entries per column for U */
    for (int i = 0; i < U_nnz; i++) {
        lu->U_colptr[U_col[i] + 1]++;
    }
    for (int j = 0; j < m; j++) {
        lu->U_colptr[j + 1] += lu->U_colptr[j];
    }

    int *U_pos = lu->ws_U_pos;
    memset(U_pos, 0, m * sizeof(int));
    for (int i = 0; i < U_nnz; i++) {
        int col = U_col[i];
        int pos = lu->U_colptr[col] + U_pos[col]++;
        lu->U_rowidx[pos] = U_row[i];
        lu->U_values[pos] = U_val[i];
    }
    lu->nnz_U = U_nnz;
    t_coo_to_csc_ms += lp_telemetry_timer_elapsed_ms(t_stage_start_ms);

    /* Extract U diagonals */
    lu->min_diag_U = RALPH_INFINITY;
    lu->max_diag_U = 0.0;
    for (int j = 0; j < m; j++) {
        lu->U_diag[j] = 0.0;
        for (int p = lu->U_colptr[j]; p < lu->U_colptr[j + 1]; p++) {
            if (lu->U_rowidx[p] == j) {
                double val = lu->U_values[p];
                lu->U_diag[j] = val;
                double absval = fabs(val);
                if (absval > RALPH_ZERO_TOL) {
                    if (absval < lu->min_diag_U) lu->min_diag_U = absval;
                    if (absval > lu->max_diag_U) lu->max_diag_U = absval;
                }
                break;
            }
        }
    }

    if (lu->min_diag_U > RALPH_ZERO_TOL) {
        lu->cond_estimate = lu->max_diag_U / lu->min_diag_U;
    } else {
        lu->cond_estimate = RALPH_INFINITY;
    }
    lu->growth_factor = 1.0;

    /* Reset update structures */
    lu->num_updates = 0;
    lu_update_backend_reset(lu);

    /* N1-A: Record Markowitz quality telemetry */
    if (backend_used == LU_NUMERIC_BACKEND_MARKOWITZ) {
        lu->mkz_last_cond = lu->cond_estimate;
        if (lu->telemetry_enabled) {
            if (lu->cond_estimate > 1e8) {
                lu->telemetry.mkz_high_cond_count++;
            }
            if (lu->cond_estimate > lu->telemetry.mkz_worst_cond) {
                lu->telemetry.mkz_worst_cond = lu->cond_estimate;
            }
        }
    } else {
        lu->mkz_last_cond = 0.0;
    }

    if (mkz_used_this_call && !mkz_bad_outcome_this_call) {
        mkz_circuit_note_good_outcome(lu, mkz_fingerprint);
    }
    if (lu->basis_governor) {
        lp_basis_governor_observe_lu_backend(
            lu->basis_governor,
            shadow_backend_pick,
            lu_numeric_backend_to_basis_governor_backend((int)backend_used));
    }
    if (backend_used == LU_NUMERIC_BACKEND_MARKOWITZ) {
        lp_telemetry_lu_mark_numeric_backend_markowitz(lu);
    } else if (backend_used == LU_NUMERIC_BACKEND_SUPERNODE) {
        lp_telemetry_lu_mark_numeric_backend_supernode(lu);
    } else if (backend_used == LU_NUMERIC_BACKEND_DENSE_GE) {
        lp_telemetry_lu_mark_numeric_backend_dense_ge(lu);
    }
    if (identity_sep_retry_lane == LU_IDSEP_RETRY_LANE_DENSE &&
        backend_used == LU_NUMERIC_BACKEND_DENSE_GE) {
        lp_telemetry_lu_mark_identity_sep_retry_lane_success(
            lu, LU_IDSEP_RETRY_LANE_DENSE);
    } else if (identity_sep_retry_lane == LU_IDSEP_RETRY_LANE_SUPERNODE &&
               backend_used == LU_NUMERIC_BACKEND_SUPERNODE) {
        lp_telemetry_lu_mark_identity_sep_retry_lane_success(
            lu, LU_IDSEP_RETRY_LANE_SUPERNODE);
    }
    if (!identity_sep_failure_this_call &&
        lu->idsep_retry_fingerprint == mkz_fingerprint) {
        lu->idsep_retry_streak = 0;
    }

    NUMERIC_RETURN(0);
#undef ENSURE_A_STRUCT_BUILT
#undef NUMERIC_RETURN
#undef NUMERIC_COMMIT
}

/*
 * LP-Aware LU Factorization — thin wrapper.
 *
 * Calls lu_symbolic_analyze() for identity detection + fill-reducing column ordering,
 * then lu_numeric_factorize() for dense GE + COO→CSC conversion.
 * Returns -1 when sparse-efficient path cannot proceed.
 */
int lu_factorize_sparse_efficient(LUFactorization *lu, const SparseMatrix *B) {
    if (!lu || !B) return -1;
    if (B->nrows != B->ncols || B->nrows != lu->m) return -1;

    int m = lu->m;

    /* For small matrices, dense is faster due to overhead */
    if (m < 20) {
        lp_telemetry_lu_set_sparse_fallback_reason(lu, LU_SPARSE_FALLBACK_SMALL_MATRIX);
        return -1;
    }

    /* Symbolic analysis (identity detection + fill-reducing column ordering) */
    double t_symbolic_ms = lp_telemetry_timer_start();
    int sym_result = lu_symbolic_analyze(lu, B);
    lp_telemetry_lu_record_symbolic_call_timed(lu, t_symbolic_ms);
    if (sym_result < 0) {
        lp_telemetry_lu_mark_symbolic_failure(lu, sym_result);
        if (!lu_sparse_strict_allow_symbolic_full_retry(lu)) {
            lu->sym_valid = 0;
            lp_telemetry_lu_set_sparse_fallback_reason(lu, LU_SPARSE_FALLBACK_SYMBOLIC);
            return -1;
        }
        lp_telemetry_lu_mark_symbolic_full_retry_attempt(lu);

        /* Retry sparse numeric once in full-structural mode (k=m). This avoids
         * dense top-level fallback when only identity/structural partitioning
         * failed but Markowitz numeric can still factorize robustly. */
        if (lu_symbolic_finalize_full_structural(lu, B) == 0) {
            int retry_failure_reason = LU_SPARSE_NUMERIC_FAIL_NONE;
            int retry_num_result = lu_numeric_factorize(
                lu, B, lu->sym_num_identity, lu->sym_k,
                LU_NUMERIC_MODE_SYMBOLIC_FULL_RETRY,
                &retry_failure_reason);
            if (retry_num_result == 0) {
                lp_telemetry_lu_mark_symbolic_full_retry_success(lu);
                lp_telemetry_lu_mark_sparse_success(lu);
                return 0;
            }
            lu->sym_valid = 0;
            lp_telemetry_lu_mark_symbolic_full_retry_numeric_failure(lu);
        } else {
            lu->sym_valid = 0;
        }
        lp_telemetry_lu_set_sparse_fallback_reason(lu, LU_SPARSE_FALLBACK_SYMBOLIC);
        return -1;
    }

    /* Numeric factorization (dense GE + COO→CSC) */
    int num_failure_reason = LU_SPARSE_NUMERIC_FAIL_NONE;
    int num_result = lu_numeric_factorize(
        lu, B, lu->sym_num_identity, lu->sym_k,
        LU_NUMERIC_MODE_STANDARD,
        &num_failure_reason);
    if (num_result < 0) {
        if (num_failure_reason == LU_SPARSE_NUMERIC_FAIL_IDENTITY_SEPARATION) {
            if (!lu_sparse_strict_allow_symbolic_full_retry(lu)) {
                lu->sym_valid = 0;
                lp_telemetry_lu_set_sparse_fallback_reason(lu, LU_SPARSE_FALLBACK_NUMERIC);
                return -1;
            }
            lp_telemetry_lu_mark_numeric_full_retry_attempt(lu);
            if (lu_symbolic_finalize_full_structural(lu, B) == 0) {
                int retry_failure_reason = LU_SPARSE_NUMERIC_FAIL_NONE;
                int retry_num_result = lu_numeric_factorize(
                    lu, B, lu->sym_num_identity, lu->sym_k,
                    LU_NUMERIC_MODE_SYMBOLIC_FULL_RETRY,
                    &retry_failure_reason);
                if (retry_num_result == 0) {
                    lp_telemetry_lu_mark_numeric_full_retry_success(lu);
                    lp_telemetry_lu_mark_sparse_success(lu);
                    return 0;
                }
            }
            lp_telemetry_lu_mark_numeric_full_retry_failure(lu);
        }
        lu->sym_valid = 0;  /* Invalidate on numeric failure */
        lp_telemetry_lu_set_sparse_fallback_reason(lu, LU_SPARSE_FALLBACK_NUMERIC);
        return -1;
    }

    lp_telemetry_lu_mark_sparse_success(lu);
    return 0;
}

int lu_factorize_sparse_strict_dispatch(LUFactorization *lu, const SparseMatrix *B) {
    int num_failure_reason = LU_SPARSE_NUMERIC_FAIL_NONE;
    int num_result;
    double t_symbolic_ms;
    int sym_result;

    if (!lu || !B) return -1;
    if (B->nrows != B->ncols || B->nrows != lu->m) return -1;

    if (lu->m < 20) {
        lp_telemetry_lu_set_sparse_fallback_reason(lu, LU_SPARSE_FALLBACK_SMALL_MATRIX);
        return -1;
    }

    t_symbolic_ms = lp_telemetry_timer_start();
    sym_result = lu_symbolic_analyze(lu, B);
    lp_telemetry_lu_record_symbolic_call_timed(lu, t_symbolic_ms);
    if (sym_result < 0) {
        lu->sym_valid = 0;
        lp_telemetry_lu_mark_symbolic_failure(lu, sym_result);
        lp_telemetry_lu_set_sparse_fallback_reason(lu, LU_SPARSE_FALLBACK_SYMBOLIC);
        return -1;
    }

    num_result = lu_numeric_factorize(
        lu, B, lu->sym_num_identity, lu->sym_k,
        LU_NUMERIC_MODE_STRICT_DISPATCH,
        &num_failure_reason);
    if (num_result < 0) {
        lu->sym_valid = 0;
        lp_telemetry_lu_set_sparse_fallback_reason(lu, LU_SPARSE_FALLBACK_NUMERIC);
        return -1;
    }

    lp_telemetry_lu_mark_sparse_success(lu);
    return 0;
}

/* Suppress unused function warnings for old code (referenced in lu_factorize_sparse_efficient) */
__attribute__((unused))
static void lu_sparse_suppress_warnings_(void) {
    (void)analyze_lp_basis;
    (void)free_lp_basis_structure;
}
