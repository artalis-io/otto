# Ralph LP Solver Performance Improvement Plan

## Goal
Match GLPK performance (currently ~8x faster on large problems).

## Current Benchmark (1000x500 LP) - Updated January 2026
| Solver | Time | Iterations | Per-Iteration |
|--------|------|------------|---------------|
| Ralph  | 0.95s | 2476       | 0.385ms       |
| GLPK   | 0.12s| 2826       | 0.044ms       |

**Progress**: Per-iteration time reduced from 0.58ms to 0.385ms (33% improvement).
**Remaining gap**: 8.8x per-iteration (down from 13x).

---

## Profiling Results (500x250 LP, 20 trials)

**Total samples in simplex_solve: 1859**

| Function | Samples | % | Notes |
|----------|---------|---|-------|
| `apply_ft_spikes_backward` | 461 | 24.8% | BTRAN spike application |
| `apply_ft_spikes_forward` | 362 | 19.5% | FTRAN spike application |
| `compact_ft_spikes` | 112 | 6.0% | Spike compaction |
| `lu_solve_transpose_sparse` | 417 | 22.4% | Sparse BTRAN |
| `lu_solve_transpose` | 400 | 21.5% | Dense BTRAN |
| `compute_reach_L/U` | 155 | 8.3% | Sparse triangular reach |
| `sparse_dot_column` | 42 | 2.3% | Reduced cost calculation |
| `lu_factorize` | 19 | 1.0% | Refactorization |

**Key finding**: Forrest-Tomlin spike application is **50% of solve time**.

---

## Completed Optimizations ✓

### ✓ Contiguous Spike Storage (Phase 2.1)
Changed from pointer-of-pointers to contiguous pool with offset tracking.
**Result**: ~20% improvement in per-iteration time.

### ✓ Eliminate sqrt() in Steepest Edge (Phase 1.1)
Use squared ratios for comparison.
**Result**: Minimal impact (Devex already used squared ratios).

### ✓ Merge Ratio Test Phases (Phase 1.2)
Single-pass Harris ratio test.
**Result**: Minimal impact (LU dominates).

### ✓ Use Cached U Diagonal (Phase 1.3)
O(1) diagonal lookup instead of O(nnz_col) search.
**Result**: Minimal impact (not on hot path).

### ✓ OpenMP SIMD (Phase 2.4 partial)
Added `#pragma omp simd` to sparse_dot_column and matrix operations.
**Result**: ~17% improvement in per-iteration time.

### ✓ Partial Pricing with Candidate List (Phase 3.1)
Hot set of promising variables + partial scan.
**Result**: Available via pricing_strategy=3.

---

## Next Priorities (Based on Profiling)

### Priority 1: Reduce Spike Application Cost (50% of time)
The FT spike application dominates. Options:
1. **More frequent refactorization** - Reduce spike count
2. **SIMD in spike application** - Vectorize inner loops
3. **Adaptive refactor threshold** - Tune based on problem size

### Priority 2: BTRAN Result Caching
`lu_solve_transpose` called twice per pivot (pricing + update).
Opportunity to cache and reuse.

### Priority 3: Smarter Compaction
`compact_ft_spikes` at 6% - may be triggered too often.

---

## Phase 1: Quick Wins (Est. 20-30% speedup)

### 1.1 Eliminate sqrt() in Steepest Edge Pricing
**Location**: `simplex.c:870-874`
**Current**:
```c
ratio = (-rc) / sqrt(weight);  // sqrt() is expensive
```
**Fix**: Use squared ratios for comparison:
```c
// Compare rc²/weight instead of |rc|/√weight
ratio_sq = (rc * rc) / weight;
if (ratio_sq > best_ratio_sq) { ... }
```
**Impact**: Eliminates n sqrt() calls per iteration

### 1.2 Merge Ratio Test Phases
**Location**: `simplex.c:1047-1128`
**Current**: Two passes over m constraints (Phase 1 finds theta_max, Phase 2 selects)
**Fix**: Single pass that tracks both simultaneously:
```c
for (int i = 0; i < m; i++) {
    double dk = d[i];
    if (fabs(dk) < PIVOT_TOL) continue;

    double ratio = compute_ratio(x[basis[i]], dk, lb, ub);

    // Track Harris theta_max
    if (ratio < theta_max + HARRIS_TOL) {
        theta_max = fmin(theta_max, ratio);
    }

    // Track best leaving (combines Phase 2)
    if (ratio <= theta_max + HARRIS_TOL && fabs(dk) > best_pivot) {
        best_leaving = i;
        best_pivot = fabs(dk);
    }
}
```
**Impact**: Reduces m×2 to m operations per iteration

### 1.3 Use Cached U Diagonal in Sparse Solve
**Location**: `lu.c:825-830`
**Current**: Linear search for diagonal element
```c
for (int p = lu->U_colptr[j]; p < lu->U_colptr[j+1]; p++) {
    if (lu->U_rowidx[p] == j) { diag = lu->U_values[p]; break; }
}
```
**Fix**: Use pre-cached U_diag:
```c
double diag = lu->U_diag[j];  // Already allocated at lu.c:79
```
**Impact**: Eliminates O(nnz_col) search per column in backward solve

### 1.4 Cache Column Norms for SE Reset
**Location**: `simplex.c:1261-1269`
**Current**: Recomputes ||A_j||² every 2n iterations
**Fix**: Cache norms at tableau creation, reuse:
```c
// In SimplexTableau struct:
double *col_norms;  // ||A_j||² for each column

// At creation (tableau_create):
for (int j = 0; j < n; j++) {
    col_norms[j] = sparse_column_norm_sq(A_ext, j);
}

// At SE reset:
for (int j = 0; j < n; j++) {
    if (var_status[j] != RALPH_BASIC) {
        se_weights[j] = col_norms[j];
    }
}
```
**Impact**: Eliminates O(n×nnz) recomputation every 2n iterations

---

## Phase 2: LU Factorization Speedup (Est. 30-40% speedup)

### 2.1 Contiguous Spike Storage (CRITICAL - No Pointer Chasing)
**Location**: `include/lp.h:101-107`, `lu.c:1543-1586`
**Current Problem**: Pointer-of-pointers causes cache misses
```c
int **ft_spike_idx;    // Array of pointers to index arrays
double **ft_spike_val; // Array of pointers to value arrays
```
**Fix**: Use contiguous pool with offset tracking:
```c
// Contiguous storage (already partially done at lines 129-133)
typedef struct {
    int *all_indices;      // Contiguous array for all spike indices
    double *all_values;    // Contiguous array for all spike values
    int *spike_start;      // Start offset for spike k
    int *spike_nnz;        // Number of nonzeros in spike k
    double *spike_diag;    // Diagonal values (for branchless apply)
    int num_spikes;
    int total_capacity;
} SpikePool;

// Access pattern becomes cache-friendly:
for (int k = 0; k < num_spikes; k++) {
    int start = pool->spike_start[k];
    int nnz = pool->spike_nnz[k];
    double diag = pool->spike_diag[k];

    // Sequential memory access:
    for (int p = 0; p < nnz; p++) {
        int row = pool->all_indices[start + p];
        double val = pool->all_values[start + p];
        x[row] -= val * x_pivot;
    }
    x[col] *= diag;  // Apply diagonal
}
```
**Impact**: Eliminates double indirection, improves cache hit rate by 50%+

### 2.2 Single-Pass Reach Computation
**Location**: `lu.c:975-1045`
**Current**: DFS to find reach, then sort for topological order
**Fix**: Output directly in topological order during DFS:
```c
// Use reverse post-order from DFS (natural topological order)
static int reach_dfs(const LUFactorization *lu, int j,
                     int *marked, int *stack, int *reach, int *reach_count) {
    if (marked[j]) return;
    marked[j] = 1;

    // Visit children first (columns that j depends on)
    for (int p = lu->L_colptr[j]; p < lu->L_colptr[j+1]; p++) {
        int child = lu->L_rowidx[p];
        reach_dfs(lu, child, marked, stack, reach, reach_count);
    }

    // Post-order: add j after all dependencies
    reach[(*reach_count)++] = j;
}
// Result is already in topological order - no sort needed!
```
**Impact**: Eliminates O(reach × log(reach)) sort overhead

### 2.3 Replace AMD Linked Lists with Bitset Degree Tracking
**Location**: `lu_sparse.c:243-350`
**Current Problem**: Linked list traversal for degree updates
**Fix**: Use dense bitset for neighbor tracking:
```c
// Bitset for tracking neighbors (64 columns per uint64_t)
uint64_t *neighbor_bits;  // Size: (m + 63) / 64

// Degree update becomes O(1) bit operations:
void add_neighbor(uint64_t *bits, int col) {
    bits[col / 64] |= (1ULL << (col % 64));
}

int count_neighbors(uint64_t *bits, int m) {
    int count = 0;
    int nwords = (m + 63) / 64;
    for (int i = 0; i < nwords; i++) {
        count += __builtin_popcountll(bits[i]);  // Hardware popcount
    }
    return count;
}
```
**Impact**: O(1) degree queries instead of O(degree) linked list traversal

### 2.4 Vectorized L/U Triangular Solves
**Location**: `lu.c:801-850`
**Current**: Scalar operations in inner loop
**Fix**: SIMD for inner product computation:
```c
#include <immintrin.h>  // For AVX

// Vectorized sparse dot product for L solve
double sparse_dot_avx(const double *values, const int *indices,
                      int nnz, const double *x) {
    __m256d sum = _mm256_setzero_pd();
    int i = 0;

    // Process 4 elements at a time
    for (; i + 4 <= nnz; i += 4) {
        __m256d v = _mm256_loadu_pd(&values[i]);
        __m256d xv = _mm256_set_pd(
            x[indices[i+3]], x[indices[i+2]],
            x[indices[i+1]], x[indices[i]]
        );
        sum = _mm256_fmadd_pd(v, xv, sum);
    }

    // Horizontal sum
    double result[4];
    _mm256_storeu_pd(result, sum);
    double total = result[0] + result[1] + result[2] + result[3];

    // Handle remainder
    for (; i < nnz; i++) {
        total += values[i] * x[indices[i]];
    }
    return total;
}
```
**Impact**: 2-4x speedup on L/U solve inner loops

---

## Phase 3: Simplex Algorithm Improvements (Est. 15-20% speedup)

### 3.1 Partial Pricing with Candidate List
**Location**: `simplex.c:800-920`
**Current**: Full scan of n variables every iteration
**Fix**: Maintain sorted candidate list, scan subset:
```c
#define CANDIDATE_SIZE 100

typedef struct {
    int *candidates;      // Variable indices
    double *priorities;   // |rc|/sqrt(weight) or similar
    int count;
    int scan_start;       // For round-robin partial scan
} CandidateList;

// Pricing scans candidates first, then partial scan of remaining
int select_entering_partial(SimplexTableau *tab, CandidateList *cand) {
    int best = -1;
    double best_ratio = 0;

    // Phase 1: Check candidates (hot set)
    for (int i = 0; i < cand->count; i++) {
        int j = cand->candidates[i];
        if (var_status[j] == RALPH_BASIC) continue;
        double ratio = compute_ratio(rc[j], se_weights[j]);
        if (ratio > best_ratio) {
            best_ratio = ratio;
            best = j;
        }
    }

    // Phase 2: Partial scan of remainder (cold set)
    int scan_size = (n - m) / 4;  // Scan 25% of non-basic
    for (int i = 0; i < scan_size; i++) {
        int j = (cand->scan_start + i) % n;
        // ... check and possibly update candidates
    }
    cand->scan_start = (cand->scan_start + scan_size) % n;

    return best;
}
```
**Impact**: Reduces O(n) to O(100 + n/4) per iteration

### 3.2 Bound Flipping for Degenerate Pivots
**Location**: `simplex.c:1150-1200`
**Current**: Degenerate pivots (theta=0) waste iterations
**Fix**: Implement bound flipping ratio test:
```c
// When theta=0, flip variable to opposite bound instead of pivoting
if (theta < RALPH_FEAS_TOL && has_finite_bounds(entering)) {
    // Flip entering variable to opposite bound
    if (var_status[entering] == RALPH_NONBASIC_LOWER) {
        x[entering] = ub[entering];
        var_status[entering] = RALPH_NONBASIC_UPPER;
    } else {
        x[entering] = lb[entering];
        var_status[entering] = RALPH_NONBASIC_LOWER;
    }
    // Update reduced costs without basis change
    update_rc_for_flip(tab, entering);
    continue;  // Skip pivot, try again
}
```
**Impact**: Reduces degenerate iterations by 30-50%

### 3.3 Dense Column Detection for Pricing Skip
**Current**: Price every column regardless of density
**Fix**: Skip very dense columns in early iterations:
```c
// At tableau creation, mark dense columns
int *is_dense_col;  // 1 if column has > m/2 nonzeros

// In pricing, skip dense columns early (they rarely enter)
if (tab->iterations < 100 && is_dense_col[j]) continue;
```
**Impact**: Small but consistent speedup on problems with dense columns

---

## Phase 4: Memory Layout Optimization (Est. 10-15% speedup)

### 4.1 Cache-Aligned Allocations
```c
// Use aligned allocation for hot arrays
#define CACHE_LINE 64

double *alloc_aligned(size_t n) {
    void *ptr;
    posix_memalign(&ptr, CACHE_LINE, n * sizeof(double));
    return (double*)ptr;
}

// Apply to: work1, work2, work3, x, y, rc, se_weights
```

### 4.2 Structure-of-Arrays for Basis Info
**Current**:
```c
int *basis;        // Which var is basic in each row
VarStatus *status; // Status of each var
int *basis_pos;    // Position in basis (-1 if non-basic)
```
**Fix**: Pack together for better locality:
```c
typedef struct {
    int basis_var;      // Variable index
    VarStatus status;   // Its status
    double value;       // Current value (avoid x[basis[i]] indirection)
} BasisEntry;

BasisEntry *basis_info;  // Single array, sequential access
```

### 4.3 Prefetching in Hot Loops
```c
// In sparse matvec:
for (int p = colptr[j]; p < colptr[j+1]; p++) {
    __builtin_prefetch(&values[p + 8], 0, 1);  // Prefetch ahead
    result[rowidx[p]] += values[p] * xj;
}
```

---

## Implementation Priority (by impact/effort)

| Priority | Task | Est. Speedup | Effort |
|----------|------|--------------|--------|
| 1 | Contiguous spike storage | 15-20% | 2 days |
| 2 | Eliminate SE sqrt() | 8-10% | 0.5 days |
| 3 | Merge ratio test phases | 5-8% | 1 day |
| 4 | Use U_diag cache | 5-7% | 0.5 days |
| 5 | Single-pass reach | 5-8% | 1 day |
| 6 | Partial pricing | 10-15% | 2 days |
| 7 | SIMD L/U solves | 10-15% | 2 days |
| 8 | Bitset AMD | 3-5% | 1 day |
| 9 | Bound flipping | 5-10% | 1 day |
| 10 | Cache alignment | 3-5% | 0.5 days |

**Cumulative Expected Speedup**: 50-70% (bringing Ralph within 2x of GLPK)

---

## Data Structures Summary

### AVOID (Slow)
- Linked lists for anything in hot path
- Pointer-of-pointers arrays (double indirection)
- malloc/free in per-iteration code
- Unsorted sparse indices requiring search

### USE (Fast)
- Contiguous arrays with offset tracking
- Pre-allocated pools with bump allocation
- CSC/CSR with sorted indices
- Bitsets for set operations
- Cache-aligned allocations
- SIMD intrinsics for inner loops

---

## Verification Strategy

1. **Correctness**: All 59 existing tests must pass after each change
2. **Performance**: Run `bench_vs_glpk` before/after each phase
3. **Profiling**: Use `perf record` / `perf report` to verify hotspot reduction
4. **Memory**: Use `valgrind --tool=cachegrind` to verify cache improvement

---

## Files to Modify

| File | Changes |
|------|---------|
| `include/lp.h` | Add SpikePool struct, col_norms, candidate list |
| `src/lu.c` | Contiguous spikes, U_diag usage, reach optimization |
| `src/lu_sparse.c` | Bitset AMD, vectorized solves |
| `src/simplex.c` | SE sqrt fix, ratio test merge, partial pricing |
| `src/sparse.c` | SIMD matvec, prefetching |
