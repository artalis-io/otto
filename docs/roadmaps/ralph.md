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

---

## Chapter 5: REST API & WASM Demo

Transport-agnostic API for solving small LP/MIP problems, primarily for WASM demos
on the documentation site. Follows patterns from Carta, Velo, Locus.

### 5.1 Scope

| Use Case | Supported | Notes |
|----------|-----------|-------|
| WASM demo on docs | ✅ | Primary goal |
| Small LP/MIP via curl | ✅ | Testing, learning |
| MPS/LP format input | ✅ | Embedded in JSON |
| Large production problems | ❌ | Embed library directly |
| Warm starts | ❌ | Stateful, use library |

### 5.2 API Design

**Port**: 8084

**Endpoints**:
```
POST /api/v1/solve      - Solve LP/MIP from JSON-embedded problem
GET  /api/v1/health     - Health check
GET  /api/v1/formats    - List supported input formats
```

**Request** (POST /api/v1/solve):
```json
{
  "format": "lp",
  "problem": "max: 5x + 3y; 2x + 4y <= 40; x >= 0; y >= 0;",
  "timeout_ms": 5000
}
```

**Response**:
```json
{
  "status": "optimal",
  "objective": 42.5,
  "variables": {"x": 10.0, "y": 5.5},
  "solve_time_ms": 12,
  "iterations": 23
}
```

**Size Limits**: 100 vars/constraints (LP), 50 vars/constraints (MIP), 5s default timeout.

### 5.3 File Structure

```
ralph/
├── include/
│   └── ralph_api.h          # Transport-agnostic API handler
├── src/
│   ├── ralph_api.c          # API handler implementation
│   └── ralph_parse_lp.c     # LP format parser
├── api/
│   └── src/main.c           # Mongoose wrapper (port 8084)
└── wasm/
    └── ralph_wasm_api.c     # WASM wrapper
```

### 5.4 LP Format

```
/* Production Planning Example */
max: 5 chairs + 3 tables;

wood:  2 chairs + 4 tables <= 40;
labor: 3 chairs + 2 tables <= 24;

chairs >= 0;
tables >= 0;
```

Supports: `min`/`max` objective, `<=`/`>=`/`=` constraints, named constraints, bounds, comments.

### 5.5 Implementation TODOs

| Phase | Duration | Deliverables |
|-------|----------|--------------|
| 1. Handler | 2-3 days | `ralph_api.h`, `ralph_api.c`, `ralph_parse_lp.c` |
| 2. HTTP Server | 1 day | `ralph/api/src/main.c`, Makefile |
| 3. WASM | 1 day | `ralph/wasm/ralph_wasm_api.c`, Makefile |
| 4. JS Handler | 1 day | `site/js/handlers/ralph.js`, api-config.json |
| 5. Demo | 0.5 day | Example LP, documentation |
| 6. Testing | 1 day | Integration, api-docs, test script |

**Total: ~6-7 days**

---

## Related Files

| File | Purpose |
|------|---------|
| `ralph/CLAUDE.md` | Development guide, API reference |
| `ralph/include/ralph.h` | Public API |
| `ralph/include/ralph_api.h` | REST API (planned) |
| `ralph/include/lap.h` | LAP solver API |
| `ralph/include/netflow.h` | Network flow API |
