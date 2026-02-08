# Ralph Solver Roadmap

Development roadmap for Ralph LP/MIP solver covering algorithms, performance, and planned features.

## Status Summary (Feb 2026)

| Area | Status | Tests |
|------|--------|-------|
| **Revised Simplex** | ✅ Complete | Primal simplex with LU factorization |
| **LU Factorization** | ✅ Complete | Sparse factorization, eta updates |
| **Branch & Bound MIP** | ✅ Complete | Basic B&B with cuts |
| **LAP Solver** | ✅ Complete | JVC algorithm, 358 tests |
| **Network Flow** | ✅ Complete | Network simplex, 153 tests |
| **Problem Detection** | ✅ Complete | Auto-detect LAP/network structure |
| **Presolve** | ✅ Phase 1 | Singleton, redundant rows, bound tightening |
| **NETLIB Suite** | 67% Pass | 8/12 problems (see below) |

---

## Chapter 1: LP Solver Performance

### 1.1 Current Benchmarks

**1000x500 LP (15% dense)**
| Solver | Time | Per-Iteration | Gap |
|--------|------|---------------|-----|
| Ralph  | 0.95s | 0.385ms | - |
| GLPK   | 0.12s | 0.044ms | 8.8x |

**Per-Iteration Breakdown:**
- 42% LU factorization (refactorization)
- 27% BTRAN (eta updates backward)
- 26% FTRAN (eta updates forward)
- 5% Other (pricing, ratio test)

### 1.2 NETLIB Benchmark Results

| Problem | Status | Notes |
|---------|--------|-------|
| adlittle | ✅ PASS | Ralph 9.6x faster than GLPK |
| share2b | ✅ PASS | Ralph 6.8x faster than GLPK |
| kb2, sc50a, sc50b | ✅ PASS | Small dense problems |
| grow7, israel | ✅ PASS | Comparable to GLPK |
| stocfor1 | ✅ PASS | Stochastic programming |
| bnl1 | ✅ PASS | 2503 iters (degeneracy) |
| brandy | ✅ PASS | 133 iters |
| degen2 | ✅ PASS | 2333 iters (degeneracy) |
| bandm | ❌ FAIL | Numerical instability |
| beaconfd | ❌ FAIL | LU update threshold issue |
| blend | ❌ FAIL | Returns INFEASIBLE incorrectly |
| lotfi | ❌ FAIL | Numerical instability |

### 1.3 Performance TODO

| Priority | Task | Expected Impact |
|----------|------|-----------------|
| **High** | Supernodal factorization | 3-5x factorization speedup |
| **High** | Symbolic analysis phase | 1.5-2x for repeated factorization |
| **Medium** | Better fill-reducing ordering (COLAMD) | 1.2-1.5x from reduced fill-in |
| **Medium** | Compressed sparse storage | Eliminate linked-list overhead |
| **Low** | Parallel pricing | Multi-core utilization |

### 1.4 Numerical Stability TODO

| Task | Status | Notes |
|------|--------|-------|
| Two-Phase Simplex | ✅ Complete | For >80% equality problems |
| Equilibration Scaling | ✅ Complete | Geometric mean scaling |
| Iterative Refinement | ✅ Complete | Residual correction |
| Threshold Pivoting | ✅ Complete | In factorization + updates |
| Harris Ratio Test | ⏳ TODO | Needed for beaconfd |
| Bound Perturbation | ⏳ TODO | Anti-degeneracy |

---

## Chapter 2: LAP Solver

### 2.1 Current Implementation ✅

**Algorithm**: Jonker-Volgenant-Castanon (JVC)
- O(n³) worst-case, often O(n²) in practice
- SIMD-optimized dense solver
- Native sparse JVC for <30% density

**Performance:**
| Size | Dense | Sparse (10%) | Warm Start |
|------|-------|--------------|------------|
| n=500 | 1.5ms | 1.1ms | 0.3ms (5x) |
| n=1000 | 10ms | 6.7ms | 1.5ms (7x) |
| n=2000 | 42ms | - | 6ms (7x) |

### 2.2 LAP Features ✅

| Feature | API | Notes |
|---------|-----|-------|
| Dense LAP | `ralph_lap_solve()` | Standard n×n |
| Sparse LAP | `ralph_lap_solve_sparse()` | CSR format |
| Rectangular | `ralph_lap_solve_rect()` | m×n problems |
| Warm start | `ralph_lap_solve_warm()` | Reuse duals |
| k-Best | `ralph_lap_solve_k_best()` | Murty's algorithm |
| Bottleneck | `ralph_lap_solve_ex()` | Minimax assignment |
| Priority | `ralph_lap_solve_ex()` | Unbalanced LAP |
| Cardinality | `ralph_lap_solve_ex()` | Min/max bounds |
| Qualification | `ralph_lap_solve_ex()` | Allow-list subsets |

---

## Chapter 3: Network Flow Solver

### 3.1 Current Implementation ✅

**Algorithm**: Network Simplex with candidate list pricing
- Standard MCNF problems
- Warm start for re-optimization
- Flow decomposition into paths

### 3.2 Network Flow Features ✅

| Feature | API | Notes |
|---------|-----|-------|
| Standard MCNF | `ralph_netflow_solve()` | Candidate list pricing |
| Unified API | `ralph_netflow_solve_ex()` | All algorithm variants |
| Flow decomposition | `ralph_netflow_decompose()` | Extract paths |
| Bottleneck | `algorithm = BOTTLENECK` | Minimize max arc cost |
| Warm start | `options.warm_start = 1` | Reuse basis |
| Cost scaling | `options.cost_scaling = 1` | For degeneracy |

---

## Chapter 4: Planned Features

### 4.1 Decomposition Methods

**Benders Decomposition** (Planned)
- For mixed-integer stochastic programs
- Master problem (integer) + subproblems (LP)
- Useful for: fleet optimization, network design

**Dantzig-Wolfe Decomposition** (Planned)
- For block-angular structure
- Column generation approach
- Useful for: crew scheduling, cutting stock

### 4.2 External Solver Backends

**HiGHS Integration** (Planned)
- Open-source LP/MIP solver
- Fallback for hard problems
- Interface: `ralph_set_backend(RALPH_BACKEND_HIGHS)`

**GLPK Integration** (Planned)
- GPL-licensed reference solver
- Benchmarking and validation
- Interface: `ralph_set_backend(RALPH_BACKEND_GLPK)`

### 4.3 MIP Improvements

| Task | Priority | Notes |
|------|----------|-------|
| Pseudocost branching | High | Better variable selection |
| Diving heuristics | High | Faster incumbent finding |
| Node presolve | Medium | Bound tightening, probing |
| Clique detection | Medium | From set-packing constraints |
| Cut pool management | Low | Reuse cuts across nodes |

---

## Technical Reference

For deep-dive documentation, see [docs/internals/](../internals/):
- [ralph-architecture.md](../internals/ralph-architecture.md) - Solver architecture
- [lu-factorization.md](../internals/lu-factorization.md) - LU decomposition details
- [simplex.md](../internals/simplex.md) - Revised simplex implementation

## Related Files

| File | Purpose |
|------|---------|
| `ralph/CLAUDE.md` | Development guide, API reference |
| `ralph/include/ralph.h` | Public API |
| `ralph/include/lap.h` | LAP solver API |
| `ralph/include/netflow.h` | Network flow API |
