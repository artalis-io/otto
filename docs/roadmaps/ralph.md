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
| **REST API** | ✅ Complete | HTTP + WASM, LP/MPS input (§5) |
| **MIP Infrastructure** | ✅ Complete | Branching priorities, cut callbacks, Benders (WIP) |

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
- **See §6.3** for FuelWise-specific Benders implementation

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

**Note:** For domain-specific MIP improvements targeting FuelWise and HoSE, see **§6**.
The domain-specific approach (branching priorities, reach cuts, clock cuts) provides
better performance than generic improvements for these structured problem classes.

---

## Technical Reference

For deep-dive documentation, see [docs/internals/](../internals/):
- [ralph-architecture.md](../internals/ralph-architecture.md) - Solver architecture
- [lu-factorization.md](../internals/lu-factorization.md) - LU decomposition details
- [simplex.md](../internals/simplex.md) - Revised simplex implementation

---

## Chapter 5: REST API & WASM Demo ✅

Transport-agnostic API for solving small LP/MIP problems, primarily for WASM demos
on the documentation site. Follows patterns from Carta, Velo, Locus.

**Status: Complete (Feb 2026)**

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

---

---

## Chapter 6: FuelWise & HoSE MIP Infrastructure

This chapter covers MIP infrastructure needed for FuelWise (refueling optimization) and HoSE
(Hours of Service engine). Both modules require domain-specific cuts and branching control
to achieve competitive solve times.

**Key insight:** Domain-specific cuts are the equalizer that makes Ralph competitive with
commercial solvers for structured problems like FuelWise and HoSE.

### 6.1 Overview

| Module | Problem Structure | Binary Vars | Key Challenge |
|--------|-------------------|-------------|---------------|
| **FuelWise** | Path with k stations | k (stop decisions) | Scale to k>30 stations |
| **HoSE** | Time-indexed schedule | ~6000 (state vars) | 1-week horizon at 5-min granularity |

Both benefit from:
- Branching priorities (cheaper/earlier stations first)
- Domain-specific cuts (reach cuts, driving capacity cuts)
- Warm start for iterative solving

### 6.2 New Ralph API

#### P0: Branching Control (~100 LoC)

```c
/*
 * Set branching priority for each variable.
 * Higher priority = branch first. Default = 0.
 */
void ralph_set_branch_priorities(RalphModel *model, const int *priorities);

/*
 * Set preferred branching direction for each variable.
 * 0 = down first (try x=0), 1 = up first (try x=1), -1 = auto
 */
void ralph_set_branch_directions(RalphModel *model, const int *directions);
```

**Implementation:**
- Store arrays in `RalphModel` struct
- Modify `select_branching_variable()` in `branch_bound.c`
- Use direction hint when creating child nodes

**Use cases:**
- FuelWise: Branch on cheaper stations first, prefer stopping at cheap stations
- HoSE: Branch on earlier periods first, prefer OFF states to explore "rest early"

#### P1: Constraint Modification + Bounds Query (~50 LoC)

```c
/*
 * Modify RHS of existing constraint (for Benders iterations).
 */
int ralph_set_constraint_rhs(RalphModel *model, int constraint, double rhs);

/*
 * Query variable bounds (for cut generators).
 */
int ralph_get_var_bounds(RalphModel *model, int var, double *lb, double *ub);
```

#### P1: Lazy Constraints (~150 LoC)

```c
/*
 * Add constraint(s) and re-solve with warm start.
 * User controls the cut loop externally.
 */
int ralph_add_lazy_constraint(RalphModel *model, const RalphCut *cut);
int ralph_add_lazy_constraints(RalphModel *model, const RalphCut *cuts, int count);
```

**Usage pattern:**
```c
while (1) {
    ralph_optimize(model);
    if (ralph_get_status(model) != RALPH_STATUS_OPTIMAL) break;

    double *x = ralph_get_solution(model);
    RalphCut cuts[10];
    int n = my_generate_cuts(x, cuts, 10);  /* User's domain logic */

    if (n == 0) break;  /* No violations, done */
    ralph_add_lazy_constraints(model, cuts, n);
}
```

#### P2: Warm Start + Farkas Ray (~350 LoC)

```c
/*
 * Save/restore LP basis for warm start between solves.
 */
int ralph_save_basis(RalphModel *model, int **basis);
int ralph_load_basis(RalphModel *model, const int *basis);

/*
 * Extract Farkas ray (infeasibility certificate) for Benders feasibility cuts.
 * Returns ray such that: ray'b < 0 while ray'A >= 0, proving infeasibility.
 */
int ralph_get_farkas_ray(RalphModel *model, double *ray);
```

**Use case:** Benders decomposition for FuelWise with k>30 stations. See §4.1 for
decomposition method background. The Farkas ray proves "these stations must have
at least one stop" when subproblem is infeasible.

#### P2: Cut Callback (~400 LoC)

```c
/* Cut representation */
typedef struct {
    int *indices;       /* Variable indices */
    double *coeffs;     /* Coefficients */
    int num_vars;       /* Number of variables in cut */
    char sense;         /* 'L' (<=), 'G' (>=), 'E' (=) */
    double rhs;         /* Right-hand side */
} RalphCut;

/* Cut callback signature */
typedef struct {
    /*
     * Called at each B&B node after LP relaxation solved.
     * x_relaxation: current LP solution (may be fractional)
     * Returns number of cuts added (0 = no cuts found)
     */
    int (*generate_cuts)(
        void *user_data,
        const double *x_relaxation,
        int num_vars,
        RalphCut *cuts,         /* Output: array of cuts */
        int max_cuts            /* Max cuts to generate */
    );
    void *user_data;
} RalphCutCallback;

void ralph_set_cut_callback(RalphModel *model, RalphCutCallback *cb);
```

**Implementation notes:**
- Invoke after each node LP solve
- Manage cut pool (avoid duplicates, limit total cuts)
- Age out ineffective cuts
- Generate at most 10-20 cuts per callback (diminishing returns)

#### P3: Branching Callback (~200 LoC)

```c
/* Branching decision */
typedef struct {
    int var_index;      /* Variable to branch on (-1 = use default) */
    double branch_point;/* Value to branch at (usually 0.5 for binary) */
    int direction;      /* 0 = down first, 1 = up first */
} RalphBranchDecision;

typedef struct {
    RalphBranchDecision (*select_branch)(
        void *user_data,
        const double *x_relaxation,
        const int *fractional_vars,
        const double *fractional_vals,
        int num_fractional
    );
    void *user_data;
} RalphBranchCallback;

void ralph_set_branch_callback(RalphModel *model, RalphBranchCallback *cb);
```

### 6.3 FuelWise-Specific Cuts

#### Reach Cuts

"Must stop at least once in [i, j] to have enough fuel to reach station j+1"

These are analogous to subtour elimination cuts in TSP. Extremely effective for
FuelWise's path structure.

```c
/*
 * Generate reach cuts for FuelWise MILP.
 *
 * Scans for intervals where LP relaxation shows insufficient stops
 * to maintain minimum fuel level.
 */
int fw_generate_reach_cuts(
    const FWRefuelProblem *problem,
    const double *z_relaxation,  /* Fractional z[i] values */
    int z_var_offset,
    RalphCut *cuts,
    int max_cuts
) {
    int num_cuts = 0;
    double fuel = problem->tank_capacity;
    int segment_start = 0;

    for (int i = 0; i < problem->num_stations && num_cuts < max_cuts; i++) {
        double consumed = fw_calc_fuel_consumed(problem,
            (i == 0) ? 0 : problem->stations[i-1].distance_from_start,
            problem->stations[i].distance_from_start);

        fuel -= consumed;

        /* Check if we can reach station i+1 without refueling */
        double next_consumed = fw_calc_fuel_consumed(problem,
            problem->stations[i].distance_from_start,
            (i+1 < problem->num_stations)
                ? problem->stations[i+1].distance_from_start
                : problem->total_distance);

        if (fuel - next_consumed < problem->minimum_fuel) {
            /* Must stop somewhere in [segment_start, i] */
            double z_sum = 0;
            for (int j = segment_start; j <= i; j++) {
                z_sum += z_relaxation[z_var_offset + j];
            }

            if (z_sum < 1.0 - 1e-6) {
                /* Violation: add cut Σ z[j] >= 1 */
                int cut_size = i - segment_start + 1;
                cuts[num_cuts].indices = malloc(cut_size * sizeof(int));
                cuts[num_cuts].coeffs = malloc(cut_size * sizeof(double));
                cuts[num_cuts].num_vars = cut_size;

                for (int j = 0; j < cut_size; j++) {
                    cuts[num_cuts].indices[j] = z_var_offset + segment_start + j;
                    cuts[num_cuts].coeffs[j] = 1.0;
                }
                cuts[num_cuts].sense = 'G';
                cuts[num_cuts].rhs = 1.0;
                num_cuts++;
            }

            fuel = problem->tank_capacity;
            segment_start = i + 1;
        }
    }
    return num_cuts;
}
```

#### Benders Decomposition

See §4.1 for general Benders background. For FuelWise specifically:

**Master problem (MIP):** Choose which stations to stop at (z variables)
```
min  stop_cost·Σz[i] + θ
s.t. z[i] ∈ {0, 1}
     feasibility cuts (from Farkas rays)
     optimality cuts (from LP duals)
     θ ≥ lower_bound
```

**Subproblem (LP, given z_fixed):** Optimize fuel purchases
```
min  Σ price[i]·x[i]
s.t. flow balance constraints
     y[i] ≥ min_fuel
     y[i] + x[i] ≤ tank_capacity + consumption[i]
     x[i] ≤ tank_capacity · z_fixed[i]    (RHS depends on z)
     x[i] ≥ min_purchase · z_fixed[i]
```

**Why FuelWise is ideal for Benders:**
1. z affects only RHS of linking constraints
2. Subproblem is trivially fast (simple LP with k variables)
3. Infeasibility has clear meaning: "can't reach station j+1"
4. Cuts accumulate, tightening master over iterations

**Network structure:** The subproblem has path/network structure:
- Nodes: origin, stations, destination
- Arcs: road segments (consume fuel) and refueling (add fuel)
- Flow conservation at each node

This means the subproblem can be solved via network simplex (see §3) instead of
general simplex, providing:
- O(n²) vs O(n³) complexity
- Integral solutions guaranteed (no basis degeneracy)
- Warm start between Benders iterations via `options.warm_start`

**Implementation:** Ralph already has problem detection for LAP/network structure
(see Status Summary). Simply enable detection for FuelWise subproblems—when
`ralph_optimize()` sees the network structure, it automatically dispatches to
`ralph_netflow_solve()` internally. No FuelWise code changes needed.

### 6.4 HoSE-Specific Cuts

See [hose.md](./hose.md) for complete MIP formulation details. Key cuts:

#### Driving Capacity Cuts (11h limit)

```c
/*
 * Generate driving capacity cuts.
 * Detects intervals where fractional driving exceeds 11h without a reset.
 */
int hs_generate_driving_cuts(
    const double *x,
    int num_periods,
    double delta,
    int drive_var_offset,
    int reset_var_offset,
    RalphCut *cuts,
    int max_cuts
) {
    int num_cuts = 0;
    int last_reset = -1;
    double driving_sum = 0.0;

    for (int t = 0; t < num_periods && num_cuts < max_cuts; t++) {
        double x_drive = x[drive_var_offset + t];
        double x_reset = x[reset_var_offset + t];

        driving_sum += x_drive * delta;

        if (x_reset > 0.5) {
            last_reset = t;
            driving_sum = 0.0;
            continue;
        }

        if (driving_sum > 11.0 + 1e-6) {
            /* Violation: add cut Σ x[τ,DRIVE] ≤ floor(11/δ) */
            int t0 = last_reset + 1;
            int t1 = t;
            int cut_size = t1 - t0 + 1;

            cuts[num_cuts].indices = malloc(cut_size * sizeof(int));
            cuts[num_cuts].coeffs = malloc(cut_size * sizeof(double));
            cuts[num_cuts].num_vars = cut_size;

            for (int j = 0; j < cut_size; j++) {
                cuts[num_cuts].indices[j] = drive_var_offset + t0 + j;
                cuts[num_cuts].coeffs[j] = 1.0;
            }
            cuts[num_cuts].sense = 'L';
            cuts[num_cuts].rhs = floor(11.0 / delta);
            num_cuts++;

            driving_sum = 0.0;
            last_reset = t;
        }
    }
    return num_cuts;
}
```

#### Mandatory Break Cuts (8h limit)

Similar structure to driving capacity, but tracks driving since last 30-min break.
Per FMCSA 2020 rule, both OFF and WORK (on-duty/not-driving) qualify as break time.

#### Cut Strength Analysis

| Cut Type | Strength | When to Generate | Cost |
|----------|----------|------------------|------|
| **FuelWise reach** | Very strong | Always | O(k) scan |
| **Driving capacity (11h)** | Strong | Always | O(H) scan |
| **Mandatory break (8h)** | Strong | Always | O(H) scan |
| **14h window** | Medium | Preprocessing preferred | O(H) |
| **70h cycle** | Weak | Rarely needed | O(H) |

### 6.5 Expected Performance

#### FuelWise MILP

| Stations (k) | Without Cuts | With Reach Cuts | With Benders |
|--------------|--------------|-----------------|--------------|
| k=20 | 1s | 50ms | 30ms |
| k=30 | timeout | 100ms | 50ms |
| k=50 | timeout | 300ms | 100ms |
| k=100 | timeout | timeout | 1s |

#### HoSE MIP (1-week horizon, δ=5 min)

| Scenario | Without Cuts | With Cuts |
|----------|--------------|-----------|
| Fresh driver, 1 task | 2-10s | <1s |
| Exhausted clocks, 1 task | 5-30s | 2-10s |
| Chain of 3 tasks | 30-120s | 10-30s |
| Chain of 5 tasks | 2-10 min | 30-120s |

### 6.6 Implementation Timeline

| Week | Features | LoC | Enables |
|------|----------|-----|---------|
| 1 | Branch priorities + directions | 100 | Better MIP for k≤30 |
| 2 | Lazy constraints + bound query + RHS mod | 200 | Manual cut loop, Benders prep |
| 3 | Warm start + Farkas ray | 350 | Full Benders for k>30 |
| 4 | Cut callback | 400 | Automatic domain cuts during B&B |
| 5 | Integration + testing | - | FuelWise reach cuts, HoSE clock cuts |

**Total: ~1050 LoC** for complete MIP infrastructure.

### 6.7 Relationship to §4.3 MIP Improvements

This chapter implements several items from §4.3:

| §4.3 Item | This Chapter |
|-----------|--------------|
| Pseudocost branching | P0 branching priorities (simpler, domain-specific) |
| Diving heuristics | Not needed (structure provides good incumbents) |
| Node presolve | P1 bound query enables domain presolve |
| Cut pool management | P2 cut callback |

The domain-specific approach here is more targeted than generic MIP improvements
and provides better performance for FuelWise/HoSE problem classes.

---

## Related Files

| File | Purpose |
|------|---------|
| `ralph/CLAUDE.md` | Development guide, API reference |
| `ralph/include/ralph.h` | Public API |
| `ralph/include/ralph_api.h` | REST API |
| `ralph/include/lap.h` | LAP solver API |
| `ralph/include/netflow.h` | Network flow API |
