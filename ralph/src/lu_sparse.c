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
#include "lp.h"

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

static AMDWorkspace* amd_workspace_create(int n) {
    AMDWorkspace *amd = (AMDWorkspace*)malloc(sizeof(AMDWorkspace));
    if (!amd) return NULL;

    amd->n = n;
    amd->head = (int*)malloc((n + 1) * sizeof(int));
    amd->next = (int*)malloc(n * sizeof(int));
    amd->prev = (int*)malloc(n * sizeof(int));
    amd->degree = (int*)malloc(n * sizeof(int));

    if (!amd->head || !amd->next || !amd->prev || !amd->degree) {
        free(amd->head);
        free(amd->next);
        free(amd->prev);
        free(amd->degree);
        free(amd);
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

    int *perm = (int*)malloc(n * sizeof(int));
    int *eliminated = (int*)calloc(n, sizeof(int));
    int *marker = (int*)malloc(n * sizeof(int));  /* For counting unique neighbors */

    if (!perm || !eliminated || !marker) {
        free(perm);
        free(eliminated);
        free(marker);
        return NULL;
    }

    for (int j = 0; j < n; j++) marker[j] = -1;

    /* Build row-to-column adjacency */
    int *row_ptr = (int*)calloc(m + 1, sizeof(int));
    int *row_cols = (int*)malloc(B->nnz * sizeof(int));
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
    int *col_element = (int*)malloc(n * sizeof(int));

    /* For each element, track its member columns as a simple list */
    int *element_head = (int*)malloc(n * sizeof(int));
    int *element_next = (int*)malloc(n * sizeof(int));

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
    int *adj_cols = (int*)malloc(n * sizeof(int));
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
    int *perm = (int*)malloc(n * sizeof(int));
    if (!perm) return NULL;

    /* Classify columns: identity (singleton ±1) vs structural */
    int *is_identity = (int*)calloc(n, sizeof(int));
    int *col_counts = (int*)malloc(n * sizeof(int));

    if (!is_identity || !col_counts) {
        free(perm);
        free(is_identity);
        free(col_counts);
        return NULL;
    }

    int num_identity = 0;
    for (int j = 0; j < n; j++) {
        int nnz = B->colptr[j + 1] - B->colptr[j];
        col_counts[j] = nnz;

        /* Check if column is identity (exactly 1 nonzero with value ±1) */
        if (nnz == 1) {
            int p = B->colptr[j];
            double val = B->values[p];
            if (fabs(fabs(val) - 1.0) < 1e-10) {
                is_identity[j] = 1;
                num_identity++;
            }
        }
    }

    /* Sort structural columns by column count (insertion sort for simplicity) */
    int *structural = (int*)malloc(n * sizeof(int));
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
    work->chunks[0] = (SparseEntry*)malloc(work->chunk_size * sizeof(SparseEntry));
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
    work->col_perm = (int*)malloc(m * sizeof(int));
    work->row_perm = (int*)malloc(m * sizeof(int));
    work->col_perm_inv = (int*)malloc(m * sizeof(int));
    work->row_perm_inv = (int*)malloc(m * sizeof(int));
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
            work->chunks[work->num_chunks] = (SparseEntry*)malloc(
                                                work->chunk_size * sizeof(SparseEntry));
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

/* Efficient row update using scatter-gather pattern
 * Updates row i by subtracting mult * pivot_row
 * This is O(nnz_row + nnz_pivot_row) instead of O(nnz_row * nnz_pivot_row)
 */
static void update_row_scatter_gather(SparseLUWork *work, int i, int pivot_row,
                                       double mult, int pivot_col) {
    double *dense = work->work_dense;
    int *marker = work->work_marker;
    int m = work->m;

    /* Step 1: Scatter row i to dense array */
    int *nz_cols = (int*)alloca(m * sizeof(int));  /* Stack allocation for speed */
    int nnz = 0;

    for (SparseEntry *re = work->rows[i]; re; re = re->next) {
        int j = re->idx;
        if (!work->col_done[j]) {
            dense[j] = re->val;
            marker[j] = 1;
            nz_cols[nnz++] = j;
        }
    }

    /* Step 2: Update using pivot row entries */
    for (SparseEntry *pe = work->rows[pivot_row]; pe; pe = pe->next) {
        int j = pe->idx;
        if (work->col_done[j]) continue;

        double delta = mult * pe->val;
        dense[j] -= delta;

        if (!marker[j]) {
            /* New fill-in */
            marker[j] = 1;
            nz_cols[nnz++] = j;
        }
    }

    /* Step 3: Gather back - update sparse row and column structures */
    /* First, remove old row entries from column lists */
    for (SparseEntry *re = work->rows[i]; re; re = re->next) {
        int j = re->idx;
        if (!work->col_done[j]) {
            /* Remove from column j */
            SparseEntry **pp = &work->cols[j];
            while (*pp && (*pp)->idx != i) {
                pp = &(*pp)->next;
            }
            if (*pp && (*pp)->idx == i) {
                *pp = (*pp)->next;
                work->col_nnz[j]--;
            }
        }
    }

    /* Clear old row list */
    work->rows[i] = NULL;
    work->row_nnz[i] = 0;

    /* Rebuild row from dense values */
    for (int k = 0; k < nnz; k++) {
        int j = nz_cols[k];
        double val = dense[j];

        /* Clear dense array and marker for next use */
        dense[j] = 0.0;
        marker[j] = 0;

        if (fabs(val) < RALPH_ZERO_TOL) continue;  /* Skip zeros */

        /* Add to column list */
        SparseEntry *col_entry = alloc_entry(work);
        if (!col_entry) continue;
        col_entry->idx = i;
        col_entry->val = val;

        /* Insert in sorted order into column */
        SparseEntry **pp = &work->cols[j];
        while (*pp && (*pp)->idx < i) {
            pp = &(*pp)->next;
        }
        col_entry->next = *pp;
        *pp = col_entry;
        work->col_nnz[j]++;

        /* Add to row list */
        SparseEntry *row_entry = alloc_entry(work);
        if (!row_entry) continue;
        row_entry->idx = j;
        row_entry->val = val;

        /* Insert in sorted order into row */
        SparseEntry **rp = &work->rows[i];
        while (*rp && (*rp)->idx < j) {
            rp = &(*rp)->next;
        }
        row_entry->next = *rp;
        *rp = row_entry;
        work->row_nnz[i]++;
    }
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
        col_order = (int*)malloc(m * sizeof(int));
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
    int *L_i = (int*)malloc(L_cap * sizeof(int));
    int *L_j = (int*)malloc(L_cap * sizeof(int));
    double *L_v = (double*)malloc(L_cap * sizeof(double));
    int *U_i = (int*)malloc(U_cap * sizeof(int));
    int *U_j = (int*)malloc(U_cap * sizeof(int));
    double *U_v = (double*)malloc(U_cap * sizeof(double));
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
    lu->L_colptr = (int*)malloc((m + 1) * sizeof(int));
    lu->L_rowidx = (int*)malloc(L_nnz * sizeof(int));
    lu->L_values = (double*)malloc(L_nnz * sizeof(double));
    lu->U_colptr = (int*)malloc((m + 1) * sizeof(int));
    lu->U_rowidx = (int*)malloc(U_nnz * sizeof(int));
    lu->U_values = (double*)malloc(U_nnz * sizeof(double));

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
    lu->ft_num_compacted = 0;
    lu->ft_compact_valid = 0;
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

    lp->identity_cols = (int*)malloc(m * sizeof(int));
    lp->identity_rows = (int*)malloc(m * sizeof(int));
    lp->identity_vals = (double*)malloc(m * sizeof(double));
    lp->structural_cols = (int*)malloc(m * sizeof(int));
    lp->row_is_identity = (int*)calloc(m, sizeof(int));
    lp->row_to_sub = (int*)malloc(m * sizeof(int));
    lp->sub_to_row = (int*)malloc(m * sizeof(int));
    lp->id_to_step = (int*)malloc(m * sizeof(int));

    /* Initial capacity for cross-terms */
    lp->B21_cap = B->nnz / 4 + 16;
    lp->B21_col = (int*)malloc(lp->B21_cap * sizeof(int));
    lp->B21_row = (int*)malloc(lp->B21_cap * sizeof(int));
    lp->B21_val = (double*)malloc(lp->B21_cap * sizeof(double));
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
    for (int jj = 0; jj < lp->num_structural; jj++) {
        int j = lp->structural_cols[jj];
        for (int p = B->colptr[j]; p < B->colptr[j + 1]; p++) {
            int row = B->rowidx[p];
            double val = B->values[p];
            if (lp->row_is_identity[row] && fabs(val) > RALPH_ZERO_TOL) {
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
            }
        }
    }

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
 * LP-Aware LU Factorization
 *
 * Algorithm:
 * 1. Analyze basis to identify identity vs structural columns
 * 2. Extract k×k submatrix from structural columns × non-identity rows
 * 3. Factorize the small submatrix with partial pivoting
 * 4. Build full L/U by combining:
 *    - Trivial parts from identity columns (L[i,j]=1, U[j,j]=±1)
 *    - Dense factorization of structural submatrix
 */
int lu_factorize_sparse_efficient(LUFactorization *lu, const SparseMatrix *B) {
    if (!lu || !B) return -1;
    if (B->nrows != B->ncols || B->nrows != lu->m) return -1;

    int m = lu->m;

    /* For small matrices, dense is faster due to overhead */
    if (m < 30) {
        return lu_factorize_dense(lu, B);
    }

    /* Analyze LP basis structure */
    LPBasisStructure *lp = analyze_lp_basis(B);
    if (!lp) {
        return lu_factorize_dense(lu, B);
    }

    int k = lp->num_structural;    /* Submatrix dimension */
    int num_id = lp->num_identity; /* Number of identity columns */

    /* If very few identity columns, fall back to dense (not worth the overhead) */
    if (num_id < m / 4) {
        free_lp_basis_structure(lp);
        return lu_factorize_dense(lu, B);
    }

    /* Special case: pure identity matrix */
    if (k == 0) {
        /* L and U are both identity (with sign from identity_vals) */
        free(lu->L_colptr); free(lu->L_rowidx); free(lu->L_values);
        free(lu->U_colptr); free(lu->U_rowidx); free(lu->U_values);

        lu->L_colptr = (int*)malloc((m + 1) * sizeof(int));
        lu->L_rowidx = (int*)malloc(m * sizeof(int));
        lu->L_values = (double*)malloc(m * sizeof(double));
        lu->U_colptr = (int*)malloc((m + 1) * sizeof(int));
        lu->U_rowidx = (int*)malloc(m * sizeof(int));
        lu->U_values = (double*)malloc(m * sizeof(double));

        /* Build identity L and U using column order from identity analysis */
        for (int step = 0; step < m; step++) {
            int j = lp->identity_cols[step];
            int row = lp->identity_rows[step];
            double val = lp->identity_vals[step];

            lu->perm[step] = row;
            lu->perm_inv[row] = step;
            lu->col_perm[step] = j;
            lu->col_perm_inv[j] = step;

            lu->L_colptr[step] = step;
            lu->L_rowidx[step] = step;
            lu->L_values[step] = 1.0;

            lu->U_colptr[step] = step;
            lu->U_rowidx[step] = step;
            lu->U_values[step] = val;
        }
        lu->L_colptr[m] = m;
        lu->U_colptr[m] = m;
        lu->nnz_L = m;
        lu->nnz_U = m;

        /* Set U diagonals - all ±1 for identity matrix */
        for (int step = 0; step < m; step++) {
            lu->U_diag[step] = lp->identity_vals[step];
        }

        /* Clear update structures */
        lu->num_updates = 0;
        lu->ft_num_updates = 0;
        lu->ft_num_compacted = 0;
        lu->ft_compact_valid = 0;
        lu->spike_pool_used = 0;
        for (int i = 0; i < m; i++) {
            lu->ft_col_order[i] = i;
            lu->ft_col_order_inv[i] = i;
        }

        lu->min_diag_U = 1.0;
        lu->max_diag_U = 1.0;
        lu->cond_estimate = 1.0;
        lu->growth_factor = 1.0;

        free_lp_basis_structure(lp);
        return 0;
    }

    /* Extract k×k structural submatrix */
    /* A_sub[ii + jj*k] = B[sub_to_row[ii], structural_cols[jj]] */
    double *A_sub = (double*)calloc((size_t)k * k, sizeof(double));
    int *sub_perm = (int*)malloc(k * sizeof(int));        /* Row permutation within submatrix */
    int *sub_perm_inv = (int*)malloc(k * sizeof(int));

    if (!A_sub || !sub_perm || !sub_perm_inv) {
        free(A_sub); free(sub_perm); free(sub_perm_inv);
        free_lp_basis_structure(lp);
        return lu_factorize_dense(lu, B);
    }

    /* Initialize submatrix permutation */
    for (int i = 0; i < k; i++) {
        sub_perm[i] = i;
        sub_perm_inv[i] = i;
    }

    /* Fill submatrix from structural columns */
    for (int jj = 0; jj < k; jj++) {
        int j = lp->structural_cols[jj];
        for (int p = B->colptr[j]; p < B->colptr[j + 1]; p++) {
            int row = B->rowidx[p];
            int ii = lp->row_to_sub[row];
            if (ii >= 0) {
                A_sub[ii + jj * k] = B->values[p];
            }
        }
    }

    /* Dense LU on k×k submatrix with partial pivoting */
    for (int step = 0; step < k; step++) {
        /* Find pivot: max |A_sub[i, step]| for i >= step */
        int pivot_idx = -1;
        double max_val = 0.0;

        for (int ii = step; ii < k; ii++) {
            int orig_ii = sub_perm[ii];
            double val = fabs(A_sub[orig_ii + step * k]);
            if (val > max_val) {
                max_val = val;
                pivot_idx = ii;
            }
        }

        if (max_val < RALPH_PIVOT_TOL) {
            /* Singular submatrix - fall back to dense */
            free(A_sub); free(sub_perm); free(sub_perm_inv);
            free_lp_basis_structure(lp);
            return lu_factorize_dense(lu, B);
        }

        /* Swap rows in permutation */
        if (pivot_idx != step) {
            int tmp = sub_perm[step];
            sub_perm[step] = sub_perm[pivot_idx];
            sub_perm[pivot_idx] = tmp;
        }

        int piv = sub_perm[step];
        double pivot_val = A_sub[piv + step * k];

        /* Eliminate: compute multipliers and update submatrix */
        for (int ii = step + 1; ii < k; ii++) {
            int row_ii = sub_perm[ii];
            double a_ik = A_sub[row_ii + step * k];

            if (fabs(a_ik) < RALPH_ZERO_TOL) continue;

            double mult = a_ik / pivot_val;
            A_sub[row_ii + step * k] = mult;  /* Store L multiplier */

            for (int jj = step + 1; jj < k; jj++) {
                A_sub[row_ii + jj * k] -= mult * A_sub[piv + jj * k];
            }
        }
    }

    /* Build inverse permutation */
    for (int i = 0; i < k; i++) {
        sub_perm_inv[sub_perm[i]] = i;
    }

    /* Now build full L and U in CSC format */
    /*
     * Column ordering: structural columns first (in order), then identity columns
     * Row ordering: follows pivot selection
     *
     * For step s:
     *   If s < k: pivot from structural submatrix
     *     - col_perm[s] = structural_cols[s]
     *     - perm[s] = sub_to_row[sub_perm[s]]  (original row of s-th pivot)
     *   If s >= k: identity column
     *     - col_perm[s] = identity_cols[s-k]
     *     - perm[s] = identity_rows[s-k]
     */

    /* Free old storage */
    free(lu->L_colptr); free(lu->L_rowidx); free(lu->L_values);
    free(lu->U_colptr); free(lu->U_rowidx); free(lu->U_values);

    /* Estimate L/U sizes:
     * - Structural part: up to k² entries
     * - Identity part: m-k diagonal entries each
     * - Cross terms: structural cols may have entries in identity rows (U only)
     */
    int L_cap = k * k + (m - k) + m;
    int U_cap = k * k + (m - k) + B->nnz;

    int *L_row_arr = (int*)malloc(L_cap * sizeof(int));
    int *L_col_arr = (int*)malloc(L_cap * sizeof(int));
    double *L_val_arr = (double*)malloc(L_cap * sizeof(double));
    int *U_row_arr = (int*)malloc(U_cap * sizeof(int));
    int *U_col_arr = (int*)malloc(U_cap * sizeof(int));
    double *U_val_arr = (double*)malloc(U_cap * sizeof(double));

    if (!L_row_arr || !L_col_arr || !L_val_arr ||
        !U_row_arr || !U_col_arr || !U_val_arr) {
        free(L_row_arr); free(L_col_arr); free(L_val_arr);
        free(U_row_arr); free(U_col_arr); free(U_val_arr);
        free(A_sub); free(sub_perm); free(sub_perm_inv);
        free_lp_basis_structure(lp);
        return -1;
    }

    int L_nnz = 0, U_nnz = 0;

    /* Build permutations */
    for (int step = 0; step < k; step++) {
        lu->col_perm[step] = lp->structural_cols[step];
        lu->col_perm_inv[lp->structural_cols[step]] = step;

        int sub_row = sub_perm[step];
        int orig_row = lp->sub_to_row[sub_row];
        lu->perm[step] = orig_row;
        lu->perm_inv[orig_row] = step;
    }

    for (int i = 0; i < num_id; i++) {
        int step = k + i;
        lu->col_perm[step] = lp->identity_cols[i];
        lu->col_perm_inv[lp->identity_cols[i]] = step;

        int orig_row = lp->identity_rows[i];
        lu->perm[step] = orig_row;
        lu->perm_inv[orig_row] = step;
    }

    /* Build L entries (COO format) */
    /* Structural part: L is lower triangular in permuted order */
    for (int step = 0; step < k; step++) {
        /* Diagonal: L[step, step] = 1 */
        L_row_arr[L_nnz] = step;
        L_col_arr[L_nnz] = step;
        L_val_arr[L_nnz] = 1.0;
        L_nnz++;

        /* Sub-diagonal: L[ii, step] = multipliers from A_sub */
        for (int ii = step + 1; ii < k; ii++) {
            int sub_row = sub_perm[ii];
            double mult = A_sub[sub_row + step * k];
            if (fabs(mult) > RALPH_ZERO_TOL) {
                L_row_arr[L_nnz] = ii;
                L_col_arr[L_nnz] = step;
                L_val_arr[L_nnz] = mult;
                L_nnz++;
            }
        }
    }

    /* Identity part: L diagonal = 1 */
    for (int i = 0; i < num_id; i++) {
        int step = k + i;
        L_row_arr[L_nnz] = step;
        L_col_arr[L_nnz] = step;
        L_val_arr[L_nnz] = 1.0;
        L_nnz++;
    }

    /* Schur complement: Lower-left block L21 = B21 * U11^{-1}
     * For each identity row with cross-terms, solve U11^T * y = b
     * where b contains the cross-term entries, then L21[row,:] = y
     */
    if (lp->B21_nnz > 0) {
        /* Allocate workspace for triangular solve */
        double *b_vec = (double*)calloc(k, sizeof(double));
        double *y_vec = (double*)calloc(k, sizeof(double));
        int *has_entry = (int*)calloc(m, sizeof(int));  /* Track which identity rows have cross-terms */

        if (b_vec && y_vec && has_entry) {
            /* Mark identity rows that have cross-terms */
            for (int i = 0; i < lp->B21_nnz; i++) {
                has_entry[lp->B21_row[i]] = 1;
            }

            /* Process each identity row with cross-terms */
            for (int id_idx = 0; id_idx < num_id; id_idx++) {
                int row_step = k + id_idx;
                if (!has_entry[row_step]) continue;

                /* Gather B21 entries for this row into b_vec */
                memset(b_vec, 0, k * sizeof(double));
                for (int i = 0; i < lp->B21_nnz; i++) {
                    if (lp->B21_row[i] == row_step) {
                        b_vec[lp->B21_col[i]] = lp->B21_val[i];
                    }
                }

                /* Solve U11^T * y = b (forward substitution since U11^T is lower triangular)
                 * U11 is stored in A_sub: U11[step, col] = A_sub[sub_perm[step] + col*k] for col >= step
                 * U11^T[col, step] = U11[step, col]
                 */
                memset(y_vec, 0, k * sizeof(double));
                for (int col = 0; col < k; col++) {
                    /* y[col] = (b[col] - sum_{j<col} U11^T[col,j]*y[j]) / U11^T[col,col] */
                    double sum = b_vec[col];
                    for (int j = 0; j < col; j++) {
                        /* U11^T[col, j] = U11[j, col] = A_sub[sub_perm[j] + col*k] (if col >= j) */
                        sum -= A_sub[sub_perm[j] + col * k] * y_vec[j];
                    }
                    /* U11^T[col, col] = U11[col, col] = A_sub[sub_perm[col] + col*k] */
                    double diag = A_sub[sub_perm[col] + col * k];
                    if (fabs(diag) > RALPH_ZERO_TOL) {
                        y_vec[col] = sum / diag;
                    }
                }

                /* Store L entries for this identity row */
                for (int col = 0; col < k; col++) {
                    if (fabs(y_vec[col]) > RALPH_ZERO_TOL) {
                        /* Ensure capacity */
                        if (L_nnz >= L_cap) {
                            int new_cap = L_cap * 2;
                            int *tmp_row = (int*)realloc(L_row_arr, new_cap * sizeof(int));
                            int *tmp_col = (int*)realloc(L_col_arr, new_cap * sizeof(int));
                            double *tmp_val = (double*)realloc(L_val_arr, new_cap * sizeof(double));
                            if (!tmp_row || !tmp_col || !tmp_val) {
                                free(tmp_row ? tmp_row : L_row_arr);
                                free(tmp_col ? tmp_col : L_col_arr);
                                free(tmp_val ? tmp_val : L_val_arr);
                                free(U_row_arr); free(U_col_arr); free(U_val_arr);
                                free(A_sub); free(sub_perm); free(sub_perm_inv);
                                free(b_vec); free(y_vec); free(has_entry);
                                free_lp_basis_structure(lp);
                                return -1;
                            }
                            L_row_arr = tmp_row;
                            L_col_arr = tmp_col;
                            L_val_arr = tmp_val;
                            L_cap = new_cap;
                        }
                        L_row_arr[L_nnz] = row_step;
                        L_col_arr[L_nnz] = col;
                        L_val_arr[L_nnz] = y_vec[col];
                        L_nnz++;
                    }
                }
            }
        }
        free(b_vec);
        free(y_vec);
        free(has_entry);
    }

    /* Build U entries (COO format) */
    /* Structural part: U is upper triangular in permuted order */
    /* With Schur complement, U has block structure: [U11, 0; 0, D] */
    for (int step = 0; step < k; step++) {
        int piv_sub = sub_perm[step];

        /* U[step, jj] for jj >= step comes from A_sub[piv_sub, jj] */
        for (int jj = step; jj < k; jj++) {
            double val = A_sub[piv_sub + jj * k];
            if (fabs(val) > RALPH_ZERO_TOL || jj == step) {
                U_row_arr[U_nnz] = step;
                U_col_arr[U_nnz] = jj;
                U_val_arr[U_nnz] = val;
                U_nnz++;
            }
        }
        /* Note: Cross-terms (structural cols in identity rows) are handled in L
         * via the Schur complement L21 = B21 * U11^{-1}, so U is block diagonal */
    }

    /* Identity part: U diagonal = ±1 */
    for (int i = 0; i < num_id; i++) {
        int step = k + i;
        U_row_arr[U_nnz] = step;
        U_col_arr[U_nnz] = step;
        U_val_arr[U_nnz] = lp->identity_vals[i];
        U_nnz++;
    }

    /* Convert L from COO to CSC */
    lu->L_colptr = (int*)calloc(m + 1, sizeof(int));
    lu->L_rowidx = (int*)malloc(L_nnz * sizeof(int));
    lu->L_values = (double*)malloc(L_nnz * sizeof(double));

    for (int i = 0; i < L_nnz; i++) {
        lu->L_colptr[L_col_arr[i] + 1]++;
    }
    for (int j = 0; j < m; j++) {
        lu->L_colptr[j + 1] += lu->L_colptr[j];
    }

    int *L_pos = (int*)calloc(m, sizeof(int));
    for (int i = 0; i < L_nnz; i++) {
        int col = L_col_arr[i];
        int pos = lu->L_colptr[col] + L_pos[col]++;
        lu->L_rowidx[pos] = L_row_arr[i];
        lu->L_values[pos] = L_val_arr[i];
    }
    free(L_pos);
    lu->nnz_L = L_nnz;

    /* Convert U from COO to CSC */
    lu->U_colptr = (int*)calloc(m + 1, sizeof(int));
    lu->U_rowidx = (int*)malloc(U_nnz * sizeof(int));
    lu->U_values = (double*)malloc(U_nnz * sizeof(double));

    for (int i = 0; i < U_nnz; i++) {
        lu->U_colptr[U_col_arr[i] + 1]++;
    }
    for (int j = 0; j < m; j++) {
        lu->U_colptr[j + 1] += lu->U_colptr[j];
    }

    int *U_pos = (int*)calloc(m, sizeof(int));
    for (int i = 0; i < U_nnz; i++) {
        int col = U_col_arr[i];
        int pos = lu->U_colptr[col] + U_pos[col]++;
        lu->U_rowidx[pos] = U_row_arr[i];
        lu->U_values[pos] = U_val_arr[i];
    }
    free(U_pos);
    lu->nnz_U = U_nnz;

    /* Clear eta/FT structures - contiguous pool storage, just reset counters */
    for (int i = 0; i < lu->ft_num_updates; i++) {
        lu->ft_spike_nnz[i] = 0;
        lu->ft_spike_diag[i] = 0.0;
        lu->ft_spike_start[i] = 0;
    }
    lu->ft_num_updates = 0;
    lu->ft_num_compacted = 0;
    lu->ft_compact_valid = 0;
    lu->spike_pool_used = 0;  /* Reset contiguous pool */
    for (int i = 0; i < m; i++) {
        lu->ft_col_order[i] = i;
        lu->ft_col_order_inv[i] = i;
    }
    lu->num_updates = 0;
    lu->num_eta = 0;

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
    free(L_row_arr); free(L_col_arr); free(L_val_arr);
    free(U_row_arr); free(U_col_arr); free(U_val_arr);
    free(A_sub); free(sub_perm); free(sub_perm_inv);
    free_lp_basis_structure(lp);

    return 0;
}
