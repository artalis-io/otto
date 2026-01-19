# Ralph Performance Optimization Roadmap

## Current State (January 2026)

**Benchmark: 500×250 LP, 15% dense, Dantzig pricing**

| Solver | Time | Per-Iteration | Iterations |
|--------|------|---------------|------------|
| Ralph  | 4.4s | 0.77 ms | 5,685 |
| GLPK   | 0.087s | 0.015 ms | ~5,700 |
| **Gap** | **51×** | **51×** | Same |

**Profile Breakdown (current):**
- 42% `lu_factorize_sparse` - Initial/refactorization (2 calls)
- 27% `apply_ft_spikes_backward` - BTRAN update application
- 26% `apply_ft_spikes_forward` - FTRAN update application
- 5% Other (solve_L, solve_U, pricing, ratio test)

---

## Priority 1: LU Factorization (High Impact)

### 1.1 Supernodal Factorization

**Current:** Each pivot is processed individually with linked-list operations.

**Improvement:** Group columns with similar sparsity patterns into "supernodes" and factorize using dense BLAS.

```
Before: 250 individual pivots, each with linked-list ops
After:  ~50 supernodes, each using dense matrix multiply
```

**Implementation:**
1. Build elimination tree during symbolic analysis
2. Identify supernodes (consecutive columns with same row structure)
3. Allocate dense blocks for each supernode
4. Use BLAS dgemm/dtrsm for dense operations within supernodes

**Expected speedup:** 3-5× for factorization phase

**Reference:** CHOLMOD, SuperLU algorithms

### 1.2 Symbolic Analysis Phase

**Current:** Column ordering computed during numerical factorization.

**Improvement:** Separate symbolic and numerical phases.

```c
// New API
LUSymbolic* lu_symbolic_analyze(const SparseMatrix *pattern);
int lu_numeric_factorize(LUFactorization *lu, const LUSymbolic *sym, const SparseMatrix *B);
```

**Benefits:**
- Reuse symbolic analysis across multiple factorizations with same pattern
- Pre-allocate exact memory needed (no reallocs)
- Better cache utilization from known access patterns

**Expected speedup:** 1.5-2× for repeated factorizations (common in simplex)

### 1.3 Better Fill-Reducing Ordering

**Current:** LP-specific ordering (singletons last) + simplified AMD.

**Improvements:**
1. **COLAMD** - Better minimum degree approximation
2. **Nested Dissection** - Better for larger matrices (>1000 rows)
3. **Hybrid** - Use structure-specific ordering for LP bases

```c
typedef enum {
    LU_ORDER_NATURAL,
    LU_ORDER_AMD,
    LU_ORDER_COLAMD,
    LU_ORDER_NESTED_DISSECTION,
    LU_ORDER_LP_SPECIFIC
} LUOrderingMethod;
```

**Expected speedup:** 1.2-1.5× from reduced fill-in

### 1.4 Compressed Sparse Storage

**Current:** Linked lists for working matrix during factorization.

**Problem:**
- Each `alloc_entry()` call has overhead
- Poor cache locality (entries scattered in memory)
- O(nnz) insertion in sorted lists

**Improvement:** Use compressed column/row format with position arrays.

```c
typedef struct {
    // Column storage (CSC-like)
    int *col_ptr;        // col_ptr[j] = start of column j
    int *col_idx;        // Row indices
    double *col_val;     // Values
    int *col_pos;        // col_pos[i*m + j] = position of (i,j) in column j, or -1

    // Row storage (CSR-like)
    int *row_ptr;
    int *row_idx;
    double *row_val;
    int *row_pos;        // row_pos[i*m + j] = position of (i,j) in row i, or -1
} CompressedWorkMatrix;
```

**Benefits:**
- O(1) lookup via position arrays
- Better cache locality
- No allocation during elimination (pre-allocated with fill estimate)

**Expected speedup:** 2-3× for factorization inner loop

---

## Priority 2: FT Update Application (High Impact)

### 2.1 Spike Compaction

**Current:** Each basis change adds a new spike. With 3000 max_updates, we accumulate up to 3000 spikes.

**Improvement:** Periodically compact spikes into the factorization.

```c
// Every N updates (e.g., N=100), compact recent spikes
void lu_compact_spikes(LUFactorization *lu, int num_to_compact);
```

**Algorithm:**
1. Take last K spikes
2. Apply them to a copy of L and U
3. Store compacted L, U
4. Remove compacted spikes from list

**Trade-off:** Compaction cost vs. reduced spike application cost

**Expected speedup:** 1.5-2× for spike application

### 2.2 Sparse Spike Application

**Current:** `apply_ft_spikes_forward` iterates through all spikes, checking if x[col] is non-zero.

**Improvement:** Track which spikes affect which positions.

```c
typedef struct {
    int *spike_affects;      // spike_affects[k] = list of positions affected by spike k
    int *position_spikes;    // position_spikes[i] = list of spikes affecting position i
} SpikeIndex;
```

**For sparse RHS:** Only apply spikes that can affect non-zero positions.

**Expected speedup:** 2-3× for sparse solves (common in simplex)

### 2.3 Lazy Spike Application

**Current:** All spikes applied on every solve.

**Improvement:** Delay spike application until needed.

```c
typedef struct {
    double *cached_solution;  // Last computed solution
    int valid_through_spike;  // Spikes 0..N already applied
    int *dirty_positions;     // Positions modified since cache
} LazySolveCache;
```

**For consecutive solves with same RHS:** Reuse cached result.

**Expected speedup:** Variable, depends on access patterns

### 2.4 SIMD Vectorization

**Current:** Scalar operations in spike application loop.

```c
for (int p = 0; p < nnz; p++) {
    x[idx[p]] += val[p] * xc;
}
```

**Improvement:** Use AVX2/AVX-512 for gather-scatter operations.

```c
// AVX2 version (4 doubles at a time)
for (int p = 0; p < nnz - 3; p += 4) {
    __m256i indices = _mm256_loadu_si256((__m256i*)&idx[p]);
    __m256d vals = _mm256_loadu_pd(&val[p]);
    __m256d xc_vec = _mm256_set1_pd(xc);
    __m256d products = _mm256_mul_pd(vals, xc_vec);

    // Gather current x values
    __m256d current = _mm256_i64gather_pd(x, indices, 8);
    __m256d updated = _mm256_add_pd(current, products);

    // Scatter back (requires manual unrolling or AVX-512)
    // ...
}
```

**Expected speedup:** 2-4× for dense spike application

---

## Priority 3: Simplex Algorithm (Medium Impact)

### 3.1 Steepest Edge Pricing

**Current:** Dantzig pricing (most negative reduced cost) - O(n) per iteration.

**Problem:** Dantzig often takes more iterations than steepest edge.

**Improvement:** Implement Devex or exact steepest edge.

```c
// Devex approximate steepest edge
typedef struct {
    double *weights;      // Reference frame weights
    int *weight_age;      // When weight was last updated
} DevexPricing;

// Selection: minimize rc[j]^2 / weight[j]
```

**Expected impact:**
- 20-40% fewer iterations
- Small per-iteration overhead for weight updates

### 3.2 Partial Pricing

**Current:** Scan all non-basic variables for pricing.

**Improvement:** Only scan a subset, cycling through.

```c
typedef struct {
    int section_size;     // Variables per section
    int current_section;  // Which section to scan
    int *promising;       // Variables that were close to selection
} PartialPricing;
```

**Expected speedup:** 2-3× for pricing phase on large problems

### 3.3 Bound Flipping Ratio Test

**Current:** Standard ratio test with Harris tolerance.

**Improvement:** BFRT (Bound Flipping Ratio Test) - allows variables to flip bounds during ratio test.

```c
// Can reduce iterations by allowing multiple bound changes per pivot
typedef struct {
    int *flipped_vars;
    double *flip_amounts;
    int num_flipped;
} BFRTResult;
```

**Expected impact:** 5-15% fewer iterations on degenerate problems

### 3.4 Hyper-Sparse Simplex

**Current:** Dense vectors for basic solution and reduced costs.

**Improvement:** Maintain sparse representations when sparsity allows.

```c
typedef struct {
    // Sparse basic solution
    int *x_idx;
    double *x_val;
    int x_nnz;

    // Sparse reduced costs
    int *rc_idx;
    double *rc_val;
    int rc_nnz;
} HyperSparseTableau;
```

**Threshold:** Switch to dense when nnz > n/10.

**Expected speedup:** 2-5× on problems with sparse optimal basis

---

## Priority 4: Memory and Cache (Medium Impact)

### 4.1 Memory Pool Allocator

**Current:** Standard malloc/free for sparse entries.

**Improvement:** Arena allocator with bulk deallocation.

```c
typedef struct {
    char *arena;
    size_t used;
    size_t capacity;
} MemoryArena;

void* arena_alloc(MemoryArena *arena, size_t size);
void arena_reset(MemoryArena *arena);  // O(1) "free all"
```

**Benefits:**
- Faster allocation (bump pointer)
- Better cache locality (sequential allocation)
- O(1) cleanup at end of factorization

**Expected speedup:** 1.2-1.5× for factorization

### 4.2 Cache-Oblivious Algorithms

**Current:** Column-major access patterns.

**Improvement:** Block the factorization for cache efficiency.

```c
// Process columns in blocks that fit in L2 cache
#define BLOCK_SIZE 64  // Tune for target cache size

for (int jb = 0; jb < m; jb += BLOCK_SIZE) {
    int je = min(jb + BLOCK_SIZE, m);
    // Factor columns jb..je together
    factor_block(work, jb, je);
}
```

**Expected speedup:** 1.3-1.5× for larger matrices

### 4.3 Prefetching

**Current:** No explicit prefetching.

**Improvement:** Prefetch next iteration's data.

```c
for (int k = 0; k < lu->ft_num_updates; k++) {
    // Prefetch next spike's data
    if (k + 1 < lu->ft_num_updates) {
        __builtin_prefetch(lu->ft_spike_idx[k+1], 0, 3);
        __builtin_prefetch(lu->ft_spike_val[k+1], 0, 3);
    }

    // Process current spike
    // ...
}
```

**Expected speedup:** 1.1-1.2× for spike application

---

## Priority 5: Parallelization (Lower Priority)

### 5.1 Parallel Triangular Solves

**Current:** Sequential forward/backward substitution.

**Improvement:** Level-set parallelization.

```c
// Compute level sets during symbolic analysis
typedef struct {
    int num_levels;
    int *level_start;     // level_start[l] = first column in level l
    int *level_cols;      // Columns grouped by level
} LevelSets;

// Parallel solve: process each level in parallel
#pragma omp parallel for
for (int i = level_start[l]; i < level_start[l+1]; i++) {
    int j = level_cols[i];
    // Process column j (independent of other columns in this level)
}
```

**Expected speedup:** 2-4× on multi-core (depends on level structure)

### 5.2 Parallel Spike Application

**Current:** Sequential spike application.

**Improvement:** Parallelize across spikes (need conflict detection).

```c
// Group spikes by affected columns
// Apply non-conflicting spikes in parallel
```

**Challenge:** Spikes can conflict (write same x[i]).

**Expected speedup:** 1.5-2× with careful implementation

### 5.3 GPU Offload for Large Problems

**Current:** CPU only.

**Improvement:** Offload dense operations to GPU.

**Candidates:**
- Supernodal dense factorization
- Large matrix-vector products
- Batch spike application

**Threshold:** Only beneficial for m > 5000 typically.

**Expected speedup:** 5-10× for large problems, overhead for small

---

## Priority 6: Algorithmic Improvements (Research)

### 6.1 LU Update Alternatives

**Current:** Forrest-Tomlin updates with spike accumulation.

**Alternatives:**
1. **Bartels-Golub** - Classic method, stable but O(m²) worst case
2. **Fletcher-Matthews** - Q-form, good for sparse
3. **Suhl-Suhl** - Hybrid approach used in CPLEX

### 6.2 Basis Representation

**Current:** Product form of inverse (eta-file / FT spikes).

**Alternative:** Maintain explicit B^{-1} for small bases.

```c
// For m < 200, dense inverse may be faster
if (m < DENSE_THRESHOLD) {
    // Use dense B^{-1}, update with Sherman-Morrison
} else {
    // Use sparse LU with FT updates
}
```

### 6.3 Preconditioning

**Current:** No preconditioning.

**Improvement:** Scale matrix before factorization.

```c
// Equilibration scaling
void equilibrate(SparseMatrix *B, double *row_scale, double *col_scale);
```

**Benefits:**
- Better numerical stability
- Reduced fill-in in some cases
- Better pivot selection

---

## Implementation Roadmap

### Phase 1: Quick Wins (1-2 weeks)
- [ ] Implement spike compaction (§2.1)
- [ ] Add SIMD to spike application (§2.4)
- [ ] Memory pool allocator (§4.1)

**Expected combined speedup:** 2-3×

### Phase 2: Core Improvements (2-4 weeks)
- [ ] Compressed sparse storage for factorization (§1.4)
- [ ] Symbolic analysis separation (§1.2)
- [ ] Sparse spike application (§2.2)

**Expected combined speedup:** 2-4×

### Phase 3: Algorithm Enhancements (4-8 weeks)
- [ ] Supernodal factorization (§1.1)
- [ ] Steepest edge pricing (§3.1)
- [ ] Hyper-sparse simplex (§3.4)

**Expected combined speedup:** 2-3×

### Phase 4: Advanced (8+ weeks)
- [ ] Parallelization (§5)
- [ ] COLAMD/Nested dissection (§1.3)
- [ ] GPU support (§5.3)

---

## Measuring Progress

### Benchmark Suite

```bash
# Standard benchmarks
./bench_lp netlib/afiro.mps      # Small (27×51)
./bench_lp netlib/blend.mps      # Medium (74×114)
./bench_lp netlib/adlittle.mps   # Medium (56×138)
./bench_lp netlib/share2b.mps    # Larger (96×162)
./bench_lp netlib/stocfor1.mps   # Larger (117×165)

# Stress tests
./bench_lp random_500x250.lp     # Current benchmark
./bench_lp random_1000x500.lp    # Scale test
./bench_lp random_2000x1000.lp   # Large scale
```

### Key Metrics

1. **Total solve time** - Primary metric
2. **Iteration count** - Algorithm efficiency
3. **Time per iteration** - Implementation efficiency
4. **Factorization time** - LU performance
5. **Update time** - FT spike overhead
6. **Memory usage** - Scalability

### Comparison Targets

| Problem Size | Current | Target | GLPK |
|--------------|---------|--------|------|
| 100×50 | ~0.1s | ~0.01s | 0.005s |
| 500×250 | ~4.4s | ~0.5s | 0.087s |
| 1000×500 | ~40s | ~2s | 0.5s |
| 2000×1000 | ? | ~10s | 2s |

**Goal:** Within 5× of GLPK for medium problems, within 10× for large.

---

## References

### Papers
1. Duff, Erisman, Reid - "Direct Methods for Sparse Matrices" (1986)
2. Davis - "Direct Methods for Sparse Linear Systems" (2006)
3. Suhl, Suhl - "Computing Sparse LU Factorizations for LP" (1990)
4. Forrest, Tomlin - "Updated Triangular Factors of the Basis" (1972)
5. Harris - "Pivot Selection Methods of the Devex LP Code" (1973)

### Open Source Implementations
1. **GLPK** - GNU Linear Programming Kit (C, public domain)
2. **CLP** - COIN-OR LP solver (C++, EPL)
3. **SoPlex** - Sequential object-oriented simPlex (C++, ZIB academic)
4. **HiGHS** - High performance LP solver (C++, MIT)
5. **SuiteSparse** - Sparse matrix library with UMFPACK, CHOLMOD (C, various)

### Profiling Tools
```bash
# CPU profiling
gprof ./bench_lp gmon.out
perf record ./bench_lp && perf report

# Memory profiling
valgrind --tool=massif ./bench_lp
heaptrack ./bench_lp

# Cache analysis
valgrind --tool=cachegrind ./bench_lp
perf stat -e cache-misses,cache-references ./bench_lp
```
