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

int lu_factorize_sparse(LUFactorization *lu, const SparseMatrix *B) {
    if (!lu || !B) return -1;
    if (B->nrows != B->ncols || B->nrows != lu->m) return -1;

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
        if (!col_order) return -1;
        for (int j = 0; j < m; j++) col_order[j] = j;
    }

    /* Create working storage */
    SparseLUWork *work = sparse_work_create(m, B->nnz);
    if (!work) {
        free(col_order);
        return -1;
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
                return -1;
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
        return -1;
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
                                return -1;
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
                            return -1;
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
                    return -1;  /* Singular */
                }
            }
        } else {
            /* Full Markowitz pivot selection (both row and column) */
            if (select_pivot(work, step, &pivot_row, &pivot_col) < 0) {
                free(L_i); free(L_j); free(L_v);
                free(U_i); free(U_j); free(U_v);
                sparse_work_free(work);
                free(col_order);
                return -1;  /* Singular */
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
                    return -1;
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
                return -1;
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
                    return -1;
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
        return -1;
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

    /* Clear sparse eta file */
    for (int i = 0; i < lu->num_eta; i++) {
        free(lu->eta_indices[i]);
        free(lu->eta_values[i]);
        lu->eta_indices[i] = NULL;
        lu->eta_values[i] = NULL;
        lu->eta_nnz[i] = 0;
    }
    lu->num_eta = 0;

    /* Clear Forrest-Tomlin spikes - contiguous pool storage, just reset counters */
    for (int i = 0; i < lu->ft_num_updates; i++) {
        lu->ft_spike_nnz[i] = 0;
        lu->ft_spike_diag[i] = 0.0;
        lu->ft_spike_start[i] = 0;
    }
    lu->ft_num_updates = 0;
    lu->spike_pool_used = 0;  /* Reset contiguous pool */
    for (int i = 0; i < m; i++) {
        lu->ft_col_order[i] = i;
        lu->ft_col_order_inv[i] = i;
    }

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

/* Finalize symbolic plan as full-structural (k=m, no identity placement).
 * Used for both normal k=m path and symbolic-failure retry path. */
static int lu_symbolic_finalize_full_structural(LUFactorization *lu,
                                                const SparseMatrix *B) {
    int m = lu->m;
    int *struct_nnz = lu->ws_struct_nnz;
    int *is_identity_col = lu->ws_is_identity;
    int *row_used = lu->ws_row_used;
    int *row_identity_col = lu->ws_row_identity_col;
    int *col_order = lu->ws_col_order;
    int *col_order_inv = lu->ws_col_order_inv;
    uint64_t fingerprint = FNV_OFFSET_BASIS;

    if (!struct_nnz || !is_identity_col || !row_used || !row_identity_col ||
        !col_order || !col_order_inv) {
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
        lu->sym_k == m) {
        lp_telemetry_lu_record_symbolic_cache_hit(lu);
        return 0;
    }
    lp_telemetry_lu_record_symbolic_cache_miss(lu);

    for (int j = 0; j < m; j++) {
        col_order[j] = j;
        col_order_inv[j] = j;
    }
    lu->sym_valid = 1;
    lu->sym_num_identity = 0;
    lu->sym_k = m;
    lu->sym_fingerprint = fingerprint;
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
    for (int j = 0; j < m; j++) {
        int nnz = B->colptr[j + 1] - B->colptr[j];
        struct_nnz[j] = nnz;

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

    /* Ensure structural columns can be matched to non-identity rows.
     * If matching fails, demote one conflicting identity row and retry. */

    for (;;) {
        for (int i = 0; i < m; i++) {
            row_match_col[i] = -1;
            row_seen[i] = 0;
        }

        int seen_token = 1;
        int unmatched_col = -1;
        for (int j = 0; j < m; j++) {
            if (is_identity_col[j]) continue;
            if (!symbolic_match_col(B, j, row_used, row_match_col, row_seen, seen_token++)) {
                unmatched_col = j;
                break;
            }
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
    if (lu->sym_valid && lu->sym_fingerprint == fingerprint) {
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
#define MARKOWITZ_MAX_SEARCH  3     /* Candidates per degree bucket */
#define MARKOWITZ_SINGULAR_RETRY_THRESHOLD 0.02 /* Relaxed threshold for one singular micro-retry */
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

    size_t dbl_need = 2u * pool_cap + (size_t)k + (size_t)k;
    size_t int_count = 3u * pool_cap + 3u * (size_t)k + 3u * (size_t)m
                     + (size_t)k + (size_t)k + (size_t)m + (size_t)k + (size_t)m
                     + (size_t)k + 1u + 2u * (size_t)k;

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

static void mkz_record_failure_reason(LUFactorization *lu, int rc) {
    lp_telemetry_lu_mark_mkz_failure_reason(lu, rc);
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
    int *mkz_col_perm, int pool_mult, double *workspace, size_t workspace_doubles)
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
     *
     * INTS (packed after doubles):
     *   cv_idx[pool_cap]     — column SVA row indices
     *   rv_idx[pool_cap]     — row SVA column indices
     *   rv_hint[pool_cap]    — hint: local row position within column SVA
     *   cv_ptr[k], cv_len[k], cv_cap[k]  — column SVA metadata
     *   rv_ptr[m], rv_len[m], rv_cap[m]  — row SVA metadata
     *   flag[k]              — dense flag for scatter/gather
     *   col_deg[k]           — active column degree (for degree buckets)
     *   row_deg[m]           — active row degree
     *   col_alive[k]         — 1 if column not yet eliminated
     *   row_alive[m]         — 1 if row not yet eliminated
     *   dg_head[k+1]         — degree bucket heads
     *   dg_next[k], dg_prev[k] — degree bucket DLL
     */
    size_t dbl_need = 2u * (size_t)pool_cap + (size_t)k + (size_t)k;

    if (workspace_doubles < total_need)
        return MKZ_FAIL_WORKSPACE;

    /* Carve double arrays */
    double *cv_val  = workspace;
    double *rv_val  = cv_val + pool_cap;
    double *work    = rv_val + pool_cap;
    double *col_max = work + k;

    /* Carve int arrays */
    int *ib = (int *)(workspace + dbl_need);
    int *cv_idx   = ib;         ib += pool_cap;
    int *rv_idx   = ib;         ib += pool_cap;
    int *rv_hint  = ib;         ib += pool_cap;
    int *cv_ptr   = ib;         ib += k;
    int *cv_len   = ib;         ib += k;
    int *cv_cap_a = ib;         ib += k;
    int *rv_ptr   = ib;         ib += m;
    int *rv_len   = ib;         ib += m;
    int *rv_cap_a = ib;         ib += m;
    int *flag     = ib;         ib += k;
    int *col_deg  = ib;         ib += k;
    int *row_deg  = ib;         ib += m;
    int *col_alive = ib;        ib += k;
    int *row_alive = ib;        ib += m;
    int *dg_head  = ib;         ib += k + 1;
    int *dg_next  = ib;         ib += k;
    int *dg_prev  = ib;         /* ib += k; */

    /* Initialize */
    memset(flag, 0, k * sizeof(int));
    memset(work, 0, k * sizeof(double));
    memset(row_deg, 0, m * sizeof(int));
    for (int jj = 0; jj < k; jj++) col_alive[jj] = 1;
    memset(row_alive, 0, m * sizeof(int));

    /* Build column SVA from B */
    int cv_used = 0;
    for (int jj = 0; jj < k; jj++) {
        int j = col_order[jj];
        cv_ptr[jj] = cv_used;
        int cnt = 0;
        for (int p = B->colptr[j]; p < B->colptr[j + 1]; p++) {
            if (fabs(B->values[p]) > RALPH_ZERO_TOL) {
                if (cv_used >= pool_cap) return MKZ_FAIL_POOL;
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
            int rp = rv_ptr[row] + rv_len[row];
            if (rp >= pool_cap) return MKZ_FAIL_POOL;
            rv_idx[rp] = jj;
            rv_val[rp] = cv_val[s + e];
            rv_hint[rp] = e;
            rv_len[row]++;
        }
    }

    /* Compute column maximums */
    for (int jj = 0; jj < k; jj++) {
        double mx = 0.0;
        int s = cv_ptr[jj], n2 = cv_len[jj];
        for (int e = 0; e < n2; e++) {
            double av = fabs(cv_val[s + e]);
            if (av > mx) mx = av;
        }
        col_max[jj] = mx;
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

    /* Helper: remove entry at position e from column jj's SVA segment */
    #define CV_REMOVE(jj, e) do { \
        int _s = cv_ptr[jj]; \
        cv_len[jj]--; \
        if ((e) < cv_len[jj]) { \
            cv_idx[_s + (e)] = cv_idx[_s + cv_len[jj]]; \
            cv_val[_s + (e)] = cv_val[_s + cv_len[jj]]; \
        } \
    } while(0)

    /* Helper: remove entry at position e from row i's SVA segment */
    #define RV_REMOVE(i, e) do { \
        int _s = rv_ptr[i]; \
        rv_len[i]--; \
        if ((e) < rv_len[i]) { \
            rv_idx[_s + (e)] = rv_idx[_s + rv_len[i]]; \
            rv_val[_s + (e)] = rv_val[_s + rv_len[i]]; \
            rv_hint[_s + (e)] = rv_hint[_s + rv_len[i]]; \
        } \
    } while(0)

    int L_nnz = 0, U_nnz = 0;
    *num_regularized = 0;
    for (int jj = 0; jj < k; jj++) mkz_col_perm[jj] = -1;

    (void)0;  /* active rows/cols tracked implicitly by degree lists */

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

            for (int d = 1; d <= k; d++) {
                if ((long long)(d - 1) >= best_cost && best_cost < (long long)m * m + 1)
                    break;
                int cand = 0;
                int max_search = singular_retry_used ? k : MARKOWITZ_MAX_SEARCH;
                double threshold_ratio = singular_retry_used
                    ? MARKOWITZ_SINGULAR_RETRY_THRESHOLD
                    : MARKOWITZ_THRESHOLD;
                for (int jj = dg_head[d]; jj >= 0 && cand < max_search; jj = dg_next[jj]) {
                    double thr = threshold_ratio * col_max[jj];
                    int s = cv_ptr[jj], n2 = cv_len[jj];
                    for (int e = 0; e < n2; e++) {
                        int row = cv_idx[s + e];
                        if (!row_alive[row]) continue;
                        if (reserve_non_reserved && row_reserved && row_reserved[row]) continue;
                        double av = fabs(cv_val[s + e]);
                        if (av < thr) continue;
                        long long cost = (long long)(row_deg[row] - 1) * (col_deg[jj] - 1);
                        if (cost < best_cost || (cost == best_cost && av > best_piv_val)) {
                            best_cost = cost;
                            piv_col = jj; piv_row = row; best_piv_val = av;
                            if (cost == 0) goto pivot_found;
                        }
                    }
                    cand++;
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
                    /* Do not consume reserved (identity) rows as a second-pass
                     * fallback. If no viable non-reserved pivot exists, fail the
                     * Markowitz attempt and let caller fall back to GE path. */
                    return MKZ_FAIL_SINGULAR;
                }
                (*num_regularized)++;
                best_piv_val = 1.0;
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
        row_alive[piv_row] = 0;

        /* === 3. Phase A: Scatter pivot row into work[]/flag[] using row SVA === */
        { int s = rv_ptr[piv_row], n2 = rv_len[piv_row];
          for (int e = 0; e < n2; e++) {
              int jj = rv_idx[s + e];
              if (!col_alive[jj]) continue;
              work[jj] = rv_val[s + e];
              flag[jj] = 1;
          } }

        /* === 4. Emit L diagonal + U pivot row === */
        if (L_nnz >= L_capacity || U_nnz >= U_capacity) return MKZ_FAIL_CAPACITY;
        L_row[L_nnz] = piv_row; L_col[L_nnz] = piv_col; L_val[L_nnz] = 1.0; L_nnz++;
        U_row[U_nnz] = step; U_col[U_nnz] = piv_col; U_val[U_nnz] = pivot_val; U_nnz++;

        { int s = rv_ptr[piv_row], n2 = rv_len[piv_row];
          for (int e = 0; e < n2; e++) {
              int jj = rv_idx[s + e];
              if (!col_alive[jj]) continue;
              if (fabs(rv_val[s + e]) > RALPH_ZERO_TOL) {
                  if (U_nnz >= U_capacity) return MKZ_FAIL_CAPACITY;
                  U_row[U_nnz] = step; U_col[U_nnz] = jj; U_val[U_nnz] = rv_val[s + e]; U_nnz++;
              }
          } }

        /* === 5. Phase B: Eliminate — for each row with entry in pivot column === */
        { int s = cv_ptr[piv_col], n2 = cv_len[piv_col];
          for (int e = 0; e < n2; e++) {
              int row = cv_idx[s + e];
              if (!row_alive[row]) continue;
              double a_ik = cv_val[s + e];
              if (fabs(a_ik) < RALPH_ZERO_TOL) continue;
              double mult = a_ik / pivot_val;

              /* Emit L entry */
              if (L_nnz >= L_capacity) return MKZ_FAIL_CAPACITY;
              L_row[L_nnz] = row; L_col[L_nnz] = piv_col; L_val[L_nnz] = mult; L_nnz++;

              /* Pass 1: Walk row's entries, update existing entries using flag[] O(1) */
              int rs = rv_ptr[row], rn = rv_len[row];
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
                          for (int scan = 0; scan < cn; scan++) {
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
                          CV_REMOVE(jj, ce);
                          RV_REMOVE(row, re); re--;
                          DG_REMOVE(jj); col_deg[jj]--; DG_INSERT(jj);
                          row_deg[row]--;
                      } else {
                          cv_val[cs + ce] = new_val;
                      }
                  }
              }

              /* Pass 2: Fill-in — flag[jj] still set means no existing entry */
              { int ps = rv_ptr[piv_row], pn = rv_len[piv_row];
                for (int pe = 0; pe < pn; pe++) {
                    int jj = rv_idx[ps + pe];
                    if (!col_alive[jj] || !flag[jj]) continue;
                    double fill = -mult * work[jj];
                    if (fabs(fill) < RALPH_ZERO_TOL) continue;

                    /* Insert into column SVA */
                    int cn = cv_len[jj];
                    if (cn >= cv_cap_a[jj]) {
                        /* Relocate column */
                        int new_cap = cn + MARKOWITZ_FILL_GAP + 4;
                        if (cv_used + new_cap > pool_cap) return MKZ_FAIL_POOL;
                        int ns = cv_used;
                        for (int f = 0; f < cn; f++) { cv_idx[ns+f] = cv_idx[cv_ptr[jj]+f]; cv_val[ns+f] = cv_val[cv_ptr[jj]+f]; }
                        cv_ptr[jj] = ns; cv_cap_a[jj] = new_cap; cv_used += new_cap;
                    }
                    cv_idx[cv_ptr[jj] + cn] = row;
                    cv_val[cv_ptr[jj] + cn] = fill;
                    cv_len[jj]++;

                    /* Insert into row SVA */
                    int rn2 = rv_len[row];
                    if (rn2 >= rv_cap_a[row]) {
                        int new_cap = rn2 + MARKOWITZ_FILL_GAP + 4;
                        if (rv_used + new_cap > pool_cap) return MKZ_FAIL_POOL;
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
                    row_deg[row]++;
                } }

              /* Restore flags for next row */
              { int ps = rv_ptr[piv_row], pn = rv_len[piv_row];
                for (int pe = 0; pe < pn; pe++) {
                    int jj = rv_idx[ps + pe];
                    if (col_alive[jj]) flag[jj] = 1;
                } }

              /* Decrement row_deg for pivot column entry (removed) */
              row_deg[row]--;
          } }

        /* Remove pivot-row entries from column SVAs (cleanup).
         * Use row SVA to visit only affected columns instead of scanning all k columns. */
        { int ps = rv_ptr[piv_row], pn = rv_len[piv_row];
          for (int pe = 0; pe < pn; pe++) {
              int jj = rv_idx[ps + pe];
              if (!col_alive[jj]) continue;
              int s = cv_ptr[jj], n2 = cv_len[jj];
              for (int e = 0; e < n2; e++) {
                  if (cv_idx[s + e] == piv_row) {
                      CV_REMOVE(jj, e);
                      DG_REMOVE(jj); col_deg[jj]--; DG_INSERT(jj);
                      break;
                  }
              }
          } }

        /* Phase C: Clean up scatter arrays */
        { int s = rv_ptr[piv_row], n2 = rv_len[piv_row];
          for (int pe = 0; pe < n2; pe++) {
              int jj = rv_idx[s + pe];
              work[jj] = 0.0; flag[jj] = 0;
          } }

        /* Update col_max for affected columns */
        { int ps = rv_ptr[piv_row], pn = rv_len[piv_row];
          for (int pe = 0; pe < pn; pe++) {
              int jj = rv_idx[ps + pe];
              if (!col_alive[jj]) continue;
              double mx = 0.0;
              int s = cv_ptr[jj], n2 = cv_len[jj];
              for (int e = 0; e < n2; e++) {
                  if (!row_alive[cv_idx[s + e]]) continue;
                  double av = fabs(cv_val[s + e]);
                  if (av > mx) mx = av;
              }
              col_max[jj] = mx;
          } }
    }

    #undef DG_REMOVE
    #undef DG_INSERT
    #undef CV_REMOVE
    #undef RV_REMOVE

    *L_nnz_out = L_nnz;
    *U_nnz_out = U_nnz;
    return 0;
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
                                 int num_identity, int k) {
    int m = lu->m;
    int dense_ge_retry_done = 0;
    int skip_sparse_numeric = 0;
    double t_a_struct_build_ms = 0.0;
    double t_markowitz_numeric_ms = 0.0;
    double t_supernode_numeric_ms = 0.0;
    double t_dense_ge_numeric_ms = 0.0;
    double t_identity_placement_ms = 0.0;
    double t_coo_to_csc_ms = 0.0;
    double t_stage_start_ms = 0.0;
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
    NUMERIC_COMMIT(); \
    return (code); \
} while (0)

    int *identity_row = lu->ws_identity_row;
    double *identity_val = lu->ws_identity_val;
    int *col_order = lu->ws_col_order;
    (void)num_identity;  /* Used implicitly: k = m - num_identity */

    /* Use dense_work for A_struct (m×k fits in m×m, row-major layout) */
    double *A_struct = lu->dense_work;
    t_stage_start_ms = lp_telemetry_timer_start();
    memset(A_struct, 0, (size_t)m * k * sizeof(double));

    /* Row-major layout A_struct[row * k + col] for cache-friendly GE */
    for (int jj = 0; jj < k; jj++) {
        int j = col_order[jj];  /* Original column index */
        for (int p = B->colptr[j]; p < B->colptr[j + 1]; p++) {
            int row = B->rowidx[p];
            A_struct[row * k + jj] = B->values[p];
        }
    }
    t_a_struct_build_ms += lp_telemetry_timer_elapsed_ms(t_stage_start_ms);

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
            NUMERIC_RETURN(-1);
        }
        int r = identity_row[orig_col];
        if (r < 0 || r >= m || row_is_identity[r]) {
            NUMERIC_RETURN(-1);
        }
        row_is_identity[r] = 1;
    }
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

    /* Try sparse Markowitz factorization if enabled and k is large enough.
     * This exploits sparsity within structural columns, reducing O(k³) to O(nnz×fill).
     * Uses dedicated growable mkz_work to avoid contention with dense_work layout. */
    if (!skip_sparse_numeric && lu->mkz_enabled && k >= MARKOWITZ_MIN_K) {
        int mkz_init_nnz = mkz_count_init_nnz(B, col_order, k);
        size_t mkz_perm_doubles = ((size_t)k * sizeof(int) + sizeof(double) - 1u) / sizeof(double);
        int *mkz_col_perm = NULL;
        int mkz_reg = 0;
        int rc = MKZ_FAIL_NONE;
        int pool_mult = lu->mkz_pool_mult_hint;
        if (pool_mult < MARKOWITZ_POOL_MULT) pool_mult = MARKOWITZ_POOL_MULT;
        if (pool_mult > MARKOWITZ_POOL_MAX_MULT) pool_mult = MARKOWITZ_POOL_MAX_MULT;

        while (1) {
            int pool_cap_dummy = 0;
            size_t mkz_need = 0;
            if (mkz_compute_workspace_requirements(mkz_init_nnz, m, k, pool_mult,
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
                mkz_col_perm, pool_mult, mkz_workspace, mkz_ws_doubles);
            t_markowitz_numeric_ms += lp_telemetry_timer_elapsed_ms(t_stage_start_ms);
            if (rc == 0) break;
            mkz_record_failure_reason(lu, rc);
            if (rc != MKZ_FAIL_POOL || pool_mult >= MARKOWITZ_POOL_MAX_MULT) break;

            int next_pool_mult = pool_mult * 2;
            if (next_pool_mult < MARKOWITZ_POOL_RETRY_MULT) {
                next_pool_mult = MARKOWITZ_POOL_RETRY_MULT;
            }
            if (next_pool_mult <= pool_mult) {
                next_pool_mult = pool_mult + 1;
            }
            if (next_pool_mult > MARKOWITZ_POOL_MAX_MULT) {
                next_pool_mult = MARKOWITZ_POOL_MAX_MULT;
            }
            pool_mult = next_pool_mult;
            lu->mkz_pool_mult_hint = pool_mult;

            L_nnz = 0;
            U_nnz = 0;
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

        if (rc == 0) {
            lp_telemetry_lu_mark_mkz_success(lu);
            lu->num_regularized = mkz_reg;
            lu->mkz_pool_mult_hint = pool_mult;

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
        /* Re-populate A_struct from B */
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
    }

    /* T2.1: Try supernodal factorization if enabled and k is large enough */
    if (!skip_sparse_numeric && lu->sn_enabled && k >= SN_MIN_K) {
        lu->sn_calls++;
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
            if (!lu->sn_work || lu->sn_work_capacity < sn_need) {
                free(lu->sn_work);
                lu->sn_work = (double *)calloc(sn_need, sizeof(double));
                lu->sn_work_capacity = lu->sn_work ? sn_need : 0;
            }

            int sn_reg = 0;
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
                                  lu->sn_work, lu->sn_work_capacity);
            t_supernode_numeric_ms += lp_telemetry_timer_elapsed_ms(t_stage_start_ms);
            if (rc == 0) {
                lu->sn_successes++;
                lu->num_regularized = sn_reg;
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
        }
    }

dense_ge_factorization:
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
            /* Row already used. Retry once with dense GE only (skip sparse numeric)
             * to preserve sparse-efficient path without top-level dense fallback. */
            lp_telemetry_lu_mark_identity_sep_failure(lu);
            if (!dense_ge_retry_done) {
                dense_ge_retry_done = 1;
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
                goto dense_ge_factorization;
            }
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
    lu->num_eta = 0;
    lu->ft_num_updates = 0;
    lu->spike_pool_used = 0;
    for (int i = 0; i < m; i++) {
        lu->ft_col_order[i] = i;
        lu->ft_col_order_inv[i] = i;
    }

    NUMERIC_RETURN(0);
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
        lp_telemetry_lu_mark_symbolic_full_retry_attempt(lu);

        /* Retry sparse numeric once in full-structural mode (k=m). This avoids
         * dense top-level fallback when only identity/structural partitioning
         * failed but Markowitz numeric can still factorize robustly. */
        if (lu_symbolic_finalize_full_structural(lu, B) == 0) {
            int retry_num_result = lu_numeric_factorize(lu, B, lu->sym_num_identity, lu->sym_k);
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
    int num_result = lu_numeric_factorize(lu, B, lu->sym_num_identity, lu->sym_k);
    if (num_result < 0) {
        lu->sym_valid = 0;  /* Invalidate on numeric failure */
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
