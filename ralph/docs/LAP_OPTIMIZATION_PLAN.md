# LAP Solver Performance Optimization Plan

## Current Performance Baseline
- n=500: ~2ms
- n=1000: ~15ms
- n=2000: ~72ms
- Complexity: O(n³) theoretical, practical performance depends on problem structure

## Identified Hot Spots

| Phase | Description | Complexity | Cache Pattern | SIMD Potential |
|-------|-------------|------------|---------------|----------------|
| Phase 1 | Column Reduction | O(n²) | Column-major (bad) | High |
| Phase 2 | Reduction Transfer | O(n²) worst | Row-major (good) | Medium |
| Phase 3 | Auction | O(n² × iters) | Row-major (good) | High |
| Phase 4 | Dijkstra | O(n²) per free row | Mixed | Medium |

---

## Priority 1: SIMD Vectorization (High Impact, Low Risk)

### 1.1 Vectorize Min-Finding in Phase 1
**Location:** Lines 128-134 (finding column minimum)
**Current:**
```c
for (i = 1; i < n; i++) {
    double c = work_cost[i * n + j];
    if (c < min_cost) {
        min_cost = c;
        min_row = i;
    }
}
```
**Optimization:**
```c
#pragma omp simd reduction(min:min_cost)
for (i = 1; i < n; i++) {
    double c = work_cost[i * n + j];
    if (c < min_cost) min_cost = c;
}
// Second pass to find index (or use manual SIMD with index tracking)
```
**Expected Gain:** 2-4x for this phase
**Effort:** Low

### 1.2 Vectorize Min/Second-Min in Phase 3
**Location:** Lines 208-219 (auction bidding)
**Current:** Sequential scan finding min and second-min
**Optimization:**
- Use SIMD to compute all reduced costs at once
- Use horizontal min operations
- Track indices separately
```c
double reduced[n];  // Pre-compute all reduced costs
#pragma omp simd
for (j = 0; j < n; j++) {
    reduced[j] = work_cost[i * n + j] - col_price[j];
}
// Then find min/second-min from reduced[]
```
**Expected Gain:** 2-3x for auction phase
**Effort:** Medium

### 1.3 Vectorize Reduction Transfer (Phase 2)
**Location:** Lines 175-181
**Optimization:** Similar to 1.2, pre-compute reduced costs with SIMD
**Expected Gain:** 1.5-2x for this phase
**Effort:** Low

---

## Priority 2: Memory Layout & Cache Optimization (High Impact, Medium Risk)

### 2.1 Consolidated Memory Allocation
**Current:** 11 separate malloc/calloc calls
**Optimization:** Single aligned allocation with pointer arithmetic
```c
typedef struct {
    double *work_cost;    // n×n, 64-byte aligned
    double *col_price;    // n
    double *row_price;    // n
    double *dist;         // n
    int *row_assign;      // n
    int *col_assign;      // n
    int *matches;         // n
    int *free_rows;       // 2n
    int *pred;            // n
    int *col_list;        // n
    int *in_free_list;    // n
} LapWorkspace;

LapWorkspace* lap_workspace_create(int n);
void lap_workspace_free(LapWorkspace *ws);
```
**Benefits:**
- Single allocation reduces overhead
- Better cache locality
- Enables workspace reuse for repeated solves
**Expected Gain:** 5-10% overall, more for small n
**Effort:** Medium

### 2.2 Transpose Cost Matrix for Phase 1
**Current:** Phase 1 accesses columns (stride = n)
**Optimization:**
- Option A: Store transposed copy for Phase 1
- Option B: Process in cache-friendly blocks
```c
// Blocked access pattern
#define BLOCK_SIZE 64
for (jb = 0; jb < n; jb += BLOCK_SIZE) {
    for (ib = 0; ib < n; ib += BLOCK_SIZE) {
        // Process block [ib:ib+BS, jb:jb+BS]
    }
}
```
**Expected Gain:** 20-50% for Phase 1 on large problems
**Effort:** Medium

### 2.3 Aligned Allocations for SIMD
**Current:** Standard malloc (may not be aligned)
**Optimization:**
```c
#ifdef _WIN32
    work_cost = _aligned_malloc(n * n * sizeof(double), 64);
#else
    posix_memalign((void**)&work_cost, 64, n * n * sizeof(double));
#endif
```
**Expected Gain:** Enables full SIMD performance
**Effort:** Low

---

## Priority 3: Algorithm Improvements (Medium Impact, Medium Risk)

### 3.1 Heap-Based Dijkstra in Phase 4
**Current:** Linear scan to find minimum distance (O(n) per iteration)
**Optimization:** Use binary heap or bucket queue
```c
typedef struct {
    int col;
    double dist;
} HeapNode;

// Min-heap operations
void heap_push(HeapNode *heap, int *size, int col, double dist);
HeapNode heap_pop(HeapNode *heap, int *size);
```
**Trade-off:**
- Better asymptotic: O(n log n) vs O(n²) per augmentation
- But constant factor is higher
- Only beneficial when many augmentations needed
**Expected Gain:** 10-30% for problems with many free rows after auction
**Effort:** High

### 3.2 Better Auction Tie-Breaking
**Current:** When u1 ≈ u2, simple column switching
**Optimization:** Use ε-scaling to guarantee convergence
```c
double epsilon = max_cost / n;  // Initial epsilon
while (epsilon > 1.0 / n) {
    run_auction_with_epsilon(epsilon);
    epsilon /= 2;
}
```
**Expected Gain:** More predictable convergence, fewer iterations
**Effort:** High

### 3.3 Early Termination in Phase 3
**Current:** Always runs 2 full passes
**Optimization:** Exit early if all rows assigned
```c
if (num_free == 0) break;  // Already have this, but could be more aggressive
```
**Expected Gain:** Problem-dependent, up to 50% on easy instances
**Effort:** Low

---

## Priority 4: OpenMP Parallelization (High Impact, High Risk)

### 4.1 Parallel Matrix Copy/Negate
**Location:** Lines 111-117
**Optimization:**
```c
#pragma omp parallel for simd
for (i = 0; i < n * n; i++) {
    work_cost[i] = -cost[i];
}
```
**Expected Gain:** Near-linear speedup for large n
**Effort:** Low

### 4.2 Parallel Column Reduction (Phase 1)
**Current:** Sequential column processing
**Optimization:**
```c
#pragma omp parallel for schedule(dynamic, 16)
for (j = n - 1; j >= 0; j--) {
    // Find column minimum (no dependencies between columns)
    double local_min = work_cost[0 * n + j];
    int local_min_row = 0;
    for (i = 1; i < n; i++) {
        if (work_cost[i * n + j] < local_min) {
            local_min = work_cost[i * n + j];
            local_min_row = i;
        }
    }
    col_price[j] = local_min;
    // matches[] and assignments need synchronization
    #pragma omp critical
    {
        matches[local_min_row]++;
        // ... update assignments
    }
}
```
**Challenge:** Assignment updates have dependencies
**Expected Gain:** 2-4x on multicore for large n
**Effort:** High (need careful synchronization)

### 4.3 Parallel Dijkstra Augmentations (Phase 4)
**Challenge:** Augmentations can overlap - need conflict detection
**Approach:**
- Find augmenting paths in parallel (read-only phase)
- Apply non-conflicting augmentations
- Retry conflicting ones sequentially
**Expected Gain:** 1.5-2x (limited by conflicts)
**Effort:** Very High

---

## Priority 5: API Enhancements (Low Impact, Low Risk)

### 5.1 Workspace Reuse API
```c
LapWorkspace* ralph_lap_workspace_create(int max_n);
void ralph_lap_workspace_free(LapWorkspace *ws);

RalphLapStatus ralph_lap_solve_with_workspace(
    int n,
    const double *cost,
    RalphLapObjective objective,
    int *row_sol,
    LapWorkspace *ws,  // Pre-allocated workspace
    ...
);
```
**Benefit:** Amortize allocation cost over multiple solves
**Expected Gain:** 10-20% for repeated solves
**Effort:** Medium

### 5.2 In-Place Solve Option
```c
RalphLapStatus ralph_lap_solve_inplace(
    int n,
    double *cost,  // Modified in place (negated for max)
    ...
);
```
**Benefit:** Avoid O(n²) copy for callers who don't need original
**Expected Gain:** 5-10%
**Effort:** Low

---

## Implementation Roadmap

### Phase A: Quick Wins (1-2 days)
1. [P1.1] SIMD min-finding in Phase 1
2. [P1.3] SIMD reduction transfer
3. [P2.3] Aligned allocations
4. [P4.1] Parallel matrix copy
5. [P3.3] Early termination improvements

**Expected Total Gain:** 30-50%

### Phase B: Core Optimizations (3-5 days)
1. [P1.2] SIMD auction phase
2. [P2.1] Consolidated memory allocation
3. [P2.2] Cache-friendly Phase 1

**Expected Total Gain:** Additional 40-60%

### Phase C: Advanced Optimizations (5-10 days)
1. [P4.2] Parallel column reduction
2. [P3.1] Heap-based Dijkstra
3. [P5.1] Workspace reuse API

**Expected Total Gain:** Additional 20-40%

### Phase D: Research Items (ongoing)
1. [P3.2] ε-scaling auction
2. [P4.3] Parallel augmentations
3. Sparse-native JVC implementation

---

## Benchmarking Strategy

### Micro-benchmarks (per optimization)
```c
void bench_phase1_original(int n, int trials);
void bench_phase1_simd(int n, int trials);
void bench_phase1_parallel(int n, int trials);
```

### Problem Classes
1. **Random uniform**: Baseline case
2. **Geometric (TSP-like)**: Structured costs
3. **Sparse**: Many infinite costs
4. **Worst-case**: Designed to stress auction phase

### Sizes to Test
- Small: n = 50, 100, 200 (cache-resident)
- Medium: n = 500, 1000 (L3 cache pressure)
- Large: n = 2000, 5000, 10000 (memory-bound)

---

## Risk Assessment

| Optimization | Risk | Mitigation |
|--------------|------|------------|
| SIMD | Low | Portable pragmas, fallback scalar |
| Memory layout | Medium | Extensive testing, workspace validation |
| OpenMP | High | Race condition testing, sequential fallback |
| Algorithm changes | High | Cross-validation with LP solver |

---

## Success Criteria

| Size | Current | Target (Phase A) | Target (Phase B) | Target (Phase C) |
|------|---------|------------------|------------------|------------------|
| n=500 | 2ms | 1.2ms | 0.8ms | 0.5ms |
| n=1000 | 15ms | 9ms | 5ms | 3ms |
| n=2000 | 72ms | 45ms | 25ms | 15ms |
| n=5000 | - | - | 150ms | 80ms |

---

## Implementation Status

### Phase A (Completed)
- [x] Cache-optimized Phase 1 (row-major access pattern)
- [x] Aligned memory allocations (64-byte for AVX-512)
- [x] Parallel matrix copy with OpenMP SIMD
- [x] SIMD pragmas in output computation

**Results:**
- n=500: 1.30ms (35% improvement from 2ms baseline)
- n=2000: 39.2ms (46% improvement from 72ms baseline)

### Phase B (Completed)
- [x] Consolidated workspace allocation (single aligned malloc)
- [x] `RalphLapWorkspace` struct for memory management
- [x] `ralph_lap_workspace_create/free/max_n` API
- [x] `ralph_lap_solve_with_workspace` for repeated solves
- [x] Heap-based Dijkstra for Phase 4 (threshold n >= 3000)
- [x] Binary heap with decrease-key operation

**Results (2026-01-29):**
- n=500: 1.56ms
- n=1000: 10.6ms
- n=2000: 44.5ms
- n=5000: 876ms

**Notes:**
- Heap-based Dijkstra only beneficial for n >= 3000 due to higher constant factors
- Linear scan version retained for smaller problems (better cache utilization)
- Workspace reuse shows ~5% speedup for repeated solves of same size
- Total test count: 81 tests (100% passing)

### Phase C (Completed)
- [x] Parallel column reduction with column-block processing
- [x] SIMD-optimized reduced cost computation in auction phase
- [x] Parallel Phase 2 reduction transfer
- [x] Separate SIMD threshold (n >= 100) from parallel threshold (n >= 5000)

**Implementation Details:**
- Added `LAP_SIMD_THRESHOLD` (100) for SIMD optimizations
- Added `LAP_PARALLEL_THRESHOLD` (5000) for OpenMP threading
- Column-block processing in Phase 1 for cache-friendly parallel access
- Pre-computed reduced costs with SIMD in Phase 3 auction
- Parallel reduction transfer in Phase 2 (no race conditions)

**Results (2026-01-29):**
- n=200: 0.16ms (was 0.21ms, 24% improvement)
- n=500: 1.51ms
- n=1000: 9.58ms
- n=2000: 41.7ms
- n=5000: 901ms

**Key Findings:**
- SIMD provides consistent benefit for n >= 100
- OpenMP threading only beneficial for very large problems (n >= 5000)
- Thread overhead causes variance for medium problems - disabled for n < 5000
- Test count at Phase C completion: 81 LAP tests + 65 Ralph tests

### Phase D (Completed)
- [x] Sparse-native JVC implementation
- [x] SIMD optimizations for sparse solver
- [x] Sparse vs dense benchmarks
- [x] Maximization support for all LAP variants (dense, rect, sparse)
- [x] INFINITY handling fix for maximization with forbidden edges
- [x] ε-scaling auction for tie-breaking convergence
- [ ] AVX-512 intrinsics for critical loops (if needed)

**Sparse Implementation Details (2026-01-29):**
- Native sparse JVC for density < 30%, dense fallback for higher density
- Three-state Dijkstra (not reached/in queue/finalized) for correct infeasibility detection
- SIMD pragmas in initialization loops and minimum-finding
- Benchmarks: 1.2-1.6x speedup at low densities vs dense conversion

**ε-Scaling Auction (2026-01-29):**
- Optional feature enabled via `ralph_lap_set_epsilon_scaling(1)`
- Adds small epsilon to Phase 3 price adjustments for tie-breaking
- Epsilon = max_cost / (n² * factor) where factor defaults to 4.0
- Helps convergence on degenerate problems with many tied costs
- Does not affect optimality (Phase 4 Dijkstra still guarantees optimal)
- API: `ralph_lap_set_epsilon_scaling()`, `ralph_lap_set_epsilon_factor()`

**Test Coverage:**
- Total LAP tests: 123 (100% passing)
- Total Ralph tests: 65 (100% passing)
- Tests cover: dense, rectangular, sparse, minimization, maximization, infeasibility, epsilon scaling
