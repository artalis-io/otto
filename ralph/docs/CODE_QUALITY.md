# Ralph Code Quality Issues and Recommendations

This document tracks code quality issues, security concerns, and architectural recommendations identified through code review.

**Last Updated:** 2026-02-04

---

## Table of Contents

1. [Security Issues](#security-issues)
2. [Performance Issues](#performance-issues)
3. [Code Style Issues](#code-style-issues)
4. [Arena Allocator Recommendation](#arena-allocator-recommendation)

---

## Security Issues

### Issue 1: Integer Overflow Risk in Spike Pool Sizing (HIGH)

**File:** `src/lu.c:127`

**Problem:**
```c
lu->spike_pool_capacity = lu->max_updates * (m / 2 + 20);
lu->spike_pool_idx = (int*)malloc(lu->spike_pool_capacity * sizeof(int));
```

If `max_updates * (m / 2 + 20)` overflows `int`, this becomes a small allocation. Subsequent accesses would write out of bounds.

**Status:** FIXED (2026-02-04)

**Fix:**
```c
size_t pool_size = (size_t)lu->max_updates * ((size_t)m / 2 + 20);
if (pool_size > INT_MAX) {
    pool_size = INT_MAX;  /* Cap to prevent overflow */
}
lu->spike_pool_capacity = (int)pool_size;
```

---

### Issue 2: Missing Bounds Check in Triplet Sorting (MEDIUM)

**File:** `src/sparse.c:175-185`

**Problem:**
```c
for (int i = 0; i < trips->nnz; i++) {
    keys[i].col = trips->col[i];  // trips->col[i] not validated
```

If `trips->col[i]` contains an out-of-range column index, the subsequent CSC conversion at line 202 will access `col_counts[trips->col[i]]` out of bounds.

**Status:** FIXED (2026-02-04)

**Fix:**
```c
for (int i = 0; i < trips->nnz; i++) {
    if (trips->row[i] < 0 || trips->row[i] >= trips->nrows ||
        trips->col[i] < 0 || trips->col[i] >= trips->ncols) {
        free(keys);
        return NULL;  /* Invalid index */
    }
    keys[i].col = trips->col[i];
    keys[i].row = trips->row[i];
    keys[i].orig_idx = i;
}
```

---

### Existing Strengths

**Bounds Checking:**
- `sparse.c:287-288` - Good bounds validation on column access
- `sparse.c:342-345` - Element access with full bounds check

**SAFE_FREE Macro (`lp.h:19`):**
```c
#define SAFE_FREE(p) do { free(p); (p) = NULL; } while(0)
```
Prevents double-free and use-after-free. Used consistently throughout.

**Realloc Safety (`model.c:125-127`):**
```c
double *new_c = (double*)realloc(model->c, new_capacity * sizeof(double));
if (!new_c) return -1;  /* Preserves original on failure */
model->c = new_c;
```

---

## Performance Issues

### Issue 1: Repeated Malloc in Hot Path (MEDIUM)

**File:** `src/simplex.c:714-715`

**Problem:**
```c
int *cb_idx = (int*)malloc(nnz_cb * sizeof(int));
double *cb_val = (double*)malloc(nnz_cb * sizeof(double));
```

These allocations happen per `tableau_compute_reduced_costs` call. In the simplex main loop, this is called frequently.

**Status:** FIXED (2026-02-04)

**Fix:** Pre-allocate `cb_sparse_idx` and `cb_sparse_val` arrays in `SimplexTableau` structure.

---

### Issue 2: Dense Matrix Allocation in LU Fallback (LOW)

**File:** `src/lu.c:261`

**Problem:**
```c
double *A = (double*)calloc(m * m, sizeof(double));  /* O(m^2) space */
```

This fallback path allocates O(m^2) memory. For large m, this is expensive.

**Status:** FIXED (2026-02-04)

**Fix:** Pre-allocated `dense_work` field in `LUFactorization` struct. The m×m matrix is allocated once in `lu_create()` and reused across all factorizations.

---

### Existing Strengths

**Hyper-Sparse Operations (`lu.c:914-1049`):**
Reach computation for sparse triangular solves is O(|reach|) instead of O(m).

**Contiguous Spike Pool (`lu.c:104-135`):**
Forrest-Tomlin spikes stored contiguously for cache efficiency. Comments cite 15-20% speedup.

**SIMD Hints (`sparse.c:256-262`):**
```c
#pragma omp simd reduction(+:sum)
for (int p = start; p < end; p++) {
    sum += values[p] * x[rowidx[p]];
}
```

**Lazy Reduced Cost Computation (`simplex.c:829-844`):**
On-demand computation with caching for future use.

---

## Code Style Issues

### Issue 1: Magic Numbers (LOW)

**File:** `src/simplex.c:412`

**Problem:**
```c
tab->c_ext[artificial_idx] = 1e8;  /* Big-M cost */
```

Should use `RALPH_BIG_M` constant.

**Status:** FIXED (2026-02-04)

---

### Issue 2: Long Functions (LOW)

**File:** `src/simplex.c` - `tableau_create()`

**Problem:** Function is 340+ lines, doing multiple distinct operations.

**Status:** FIXED (2026-02-04)

**Fix:** Extracted helper functions:
- `tableau_alloc_arrays()` - allocates all workspace arrays via arena
- `tableau_init_weights()` - initializes steepest edge/Devex weights

---

### Existing Strengths

**Consistent Style:**
- 4-space indentation
- `snake_case` functions
- `SCREAMING_CASE` constants
- Comments on non-obvious logic

**Clear Module Boundaries:**
- `ralph.c` - Public API
- `model.c` - Model building
- `simplex.c` - LP algorithm
- `lu.c` - Numerical core
- `sparse.c` - Matrix operations

**Good Documentation:** Function headers explain purpose and parameters.

---

## Arena Allocator Recommendation

### Current Memory Pattern

Ralph uses three distinct allocation patterns:

| Phase | Pattern | Arena Suitability |
|-------|---------|-------------------|
| Model Building | Incremental realloc | Poor |
| Solve (LP/MIP) | Bulk allocation + workspace reuse | Excellent |
| LU Factorization | Pre-allocated pools + updates | Good |

### Where Arena Allocation Would Work Well

#### 1. SimplexTableau (`simplex.c:223-556`)

The tableau allocates ~20 arrays at creation that all share the same lifetime:

```c
tab->c_ext = (double*)calloc(tab->n, sizeof(double));
tab->lb_ext = (double*)calloc(tab->n, sizeof(double));
tab->basis = (int*)malloc(tab->m * sizeof(int));
// ... 15+ more allocations
```

**Benefit:** Eliminates 20+ allocations per tableau, single reset instead of 20+ frees.

#### 2. LU Factorization Workspace (`lu.c:33-170`)

Already follows an arena-like pattern with pre-allocated workspaces:

```c
lu->hs_work1 = (double*)calloc(m, sizeof(double));
lu->hs_work2 = (double*)calloc(m, sizeof(double));
```

The spike pool already uses contiguous allocation. Converting to arena would simplify cleanup but provide modest performance gains.

#### 3. MIP Branch-and-Bound Nodes

BBNodes have bounded lifetimes. A per-node arena would allow all temporary allocations to be freed in bulk.

### Where Arena Won't Work

**Model Building (`model.c`):**

Uses incremental `realloc`:
```c
double *new_c = (double*)realloc(model->c, new_capacity * sizeof(double));
```

This is fundamentally incompatible with arena allocation. The build state pattern is correct for this use case.

### Recommended Arena API

```c
/* Minimal arena for Ralph solve phase */
typedef struct {
    char *buffer;
    size_t capacity;
    size_t used;
} RalphArena;

static inline void* ralph_arena_alloc(RalphArena *a, size_t size) {
    size = (size + 7) & ~7;  /* 8-byte alignment */
    if (a->used + size > a->capacity) return NULL;
    void *ptr = a->buffer + a->used;
    a->used += size;
    return ptr;
}

static inline void ralph_arena_reset(RalphArena *a) {
    a->used = 0;
}
```

### Estimated Impact

- Reduce tableau creation allocations: 20+ to 1
- Eliminate fragmentation in long-running MIP solves
- Simplify cleanup in error paths

### Status

IMPLEMENTED (2026-02-04)

**Implementation:**
- Added `RalphArena` struct to `include/lp.h` with inline functions
- Modified `tableau_alloc_arrays()` to allocate from arena
- Modified `tableau_free()` to free arena in single call
- Reduced 21 allocations to 1 per tableau creation

---

## BBNode Memory Pool

### Problem

MIP Branch-and-Bound creates many BBNode structures, each with malloc'd lb/ub arrays. In deep trees this causes:
- Thousands of small allocations per solve
- Memory fragmentation over time
- Poor cache locality due to scattered node data

### Solution

Added `BBNodePool` to pre-allocate nodes with contiguous lb/ub arrays:

```c
typedef struct {
    int capacity;           /* Total nodes in pool */
    int num_vars;           /* Size of lb/ub arrays per node */
    BBNode *nodes;          /* Pre-allocated node structures */
    double *lb_pool;        /* Contiguous lb arrays for all nodes */
    double *ub_pool;        /* Contiguous ub arrays for all nodes */
    int *free_list;         /* Stack of free node indices */
    int free_count;         /* Number of free nodes */
    int nodes_allocated;    /* High-water mark for stats */
} BBNodePool;
```

### API

```c
BBNodePool* bb_node_pool_create(int capacity, int num_vars);
void bb_node_pool_free(BBNodePool *pool);
BBNode* bb_node_pool_get(BBNodePool *pool);
void bb_node_pool_return(BBNodePool *pool, BBNode *node);
```

### Benefits

- **O(1) allocation/deallocation**: Stack-based free list
- **Contiguous memory**: Better cache behavior for lb/ub arrays
- **Reduced fragmentation**: Single allocation for all node data
- **Safe fallback**: `bb_node_pool_return()` handles non-pool nodes gracefully

### Configuration

Pool capacity is configurable via the `node_pool_capacity` parameter:

```c
ralph_set_int_param(model, "node_pool_capacity", 4096);  /* Large MIPs */
ralph_set_int_param(model, "node_pool_capacity", 256);   /* Memory-constrained */
```

| Value | Use Case |
|-------|----------|
| 256 | Embedded/memory-constrained systems |
| 1024 | Default - suitable for most problems |
| 4096+ | Large MIPs with weak LP relaxations |

### Status

IMPLEMENTED (2026-02-04)

**Implementation:**
- Added `BBNodePool` struct to `include/mip.h`
- Implemented pool functions in `src/branch_bound.c`
- Integrated pool into `MIPSolver` (created in `mip_create()`, freed in `mip_free()`)
- Pool capacity configurable via `node_pool_capacity` parameter (default 1024)
- Pool is optional - NULL pool falls back to individual allocations
- Automatic fallback to malloc when pool exhausted

---

## Summary

| Issue | Severity | Status |
|-------|----------|--------|
| Integer overflow in spike pool | HIGH | FIXED |
| Integer overflow in node pool | MEDIUM | FIXED |
| Missing triplet bounds check | MEDIUM | FIXED |
| Malloc in simplex hot path | MEDIUM | FIXED |
| Magic numbers | LOW | FIXED |
| Dense LU fallback allocation | LOW | FIXED |
| Long functions | LOW | FIXED |
| Arena allocator (SimplexTableau) | Enhancement | IMPLEMENTED |
| Arena allocator (LUFactorization) | Enhancement | IMPLEMENTED |
| BBNode memory pool | Enhancement | IMPLEMENTED |
