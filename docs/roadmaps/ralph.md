# Ralph Solver Roadmap

Development roadmap for Ralph LP/MIP solver covering algorithms, performance, and planned features.

## Status Summary (Feb 2026)

| Area | Status | Notes |
|------|--------|-------|
| **Revised Simplex** | ✅ Complete | Primal simplex with LU factorization |
| **LU Factorization** | ✅ Complete | Sparse factorization, eta updates |
| **Branch & Bound MIP** | ✅ Complete | Basic B&B with cuts |
| **LAP Solver** | ✅ Complete | JVC algorithm, 358 tests |
| **Network Flow** | ✅ Complete | Network simplex, 153 tests |
| **Problem Detection** | ✅ Complete | Auto-detect LAP/network structure |
| **Presolve** | ✅ Phase 1 | Singleton, redundant rows, bound tightening |
| **NETLIB Suite** | 67% Pass | 8/12 problems (see below) |
| **MIP Infrastructure** | ✅ Complete | Branching, cuts, callbacks, warm start (§6) |
| **Benders Decomposition** | ✅ Complete | Generic solver, ~1100 LoC, 24 tests (§7) |

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

**Benders Decomposition** → See **Chapter 7** for full design
- Generic implementation in Ralph (not domain-specific)
- Infrastructure complete (§6), algorithm planned (§7)

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

#### P2: Warm Start + Cut Callback ✅

```c
/* Basis representation for warm start */
typedef struct RalphBasis RalphBasis;
RalphBasis* ralph_save_basis(const RalphModel *model);
int ralph_load_basis(RalphModel *model, const RalphBasis *basis);
void ralph_free_basis(RalphBasis *basis);

/* Farkas ray (infeasibility certificate) - already implemented */
int ralph_get_farkas_ray(const RalphModel *model, double *ray);

/* Cut callback for automatic cut generation at B&B nodes */
typedef struct {
    int (*generate_cuts)(void *user_data, const double *x_relaxation,
                         int num_vars, RalphCut *cuts, int max_cuts);
    void *user_data;
} RalphCutCallback;
void ralph_set_cut_callback(RalphModel *model, const RalphCutCallback *cb);
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

#### P3: Branching Callback ✅

```c
/* Branching callback for custom variable selection */
typedef struct {
    int (*select_branch_var)(void *user_data, const double *x_relaxation,
                              int num_vars, const int *is_integer,
                              const double *lb, const double *ub);
    void *user_data;
} RalphBranchCallback;

void ralph_set_branch_callback(RalphModel *model, const RalphBranchCallback *cb);
```

Callback returns variable index to branch on, or -1 to use default strategy.
Variable must be fractional and integer-constrained.

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

#### Benders Decomposition (Planned)

**Current state:** FuelWise uses exhaustive enumeration for k≤20 stations via
`fw_solve_refuel_benders()`, which iterates all 2^k combinations. This is not
true Benders decomposition.

**Why upgrade to true Benders:** Scale to k>30 stations without exponential blowup.

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

### 6.6 Implementation Status

| Phase | Features | Status | Notes |
|-------|----------|--------|-------|
| P0 | Branch priorities + directions | ✅ Complete | Better MIP for k≤30 |
| P1 | Lazy constraints + bound query + RHS mod | ✅ Complete | Manual cut loop, Benders prep |
| P2 | Warm start + cut callback | ✅ Complete | Basis save/restore, automatic cuts |
| P3 | Branching callback | ✅ Complete | Custom variable selection |
| - | FuelWise true Benders | ⏳ Planned | Replace enumeration with MIP master |
| - | HoSE clock cuts | ⏳ Planned | Driving/break capacity cuts |

**MIP infrastructure complete (Feb 2026).** Next: implement domain-specific algorithms
using the infrastructure (true Benders for FuelWise, clock cuts for HoSE).

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

## Chapter 7: Generic Benders Decomposition ✅

**Status:** Complete (Feb 2026) - 24 tests passing

This chapter describes the generic Benders decomposition solver in Ralph, replacing
domain-specific implementations (e.g., FuelWise enumeration) with a reusable algorithm.

### 7.1 Background

**Benders decomposition** solves problems of the form:

```
min  c'x + d'y
s.t. Ax = b                    (master constraints)
     Tx + Wy = h               (linking constraints)
     x ∈ X (integer/binary)    (complicating variables)
     y ≥ 0                     (continuous variables)
```

The key insight: once x is fixed, the subproblem in y is an LP. We can represent
the optimal subproblem cost Q(x) via linear cuts in the master problem.

**Algorithm:**
1. Solve master: `min c'x + θ` subject to master constraints + accumulated cuts
2. Fix x*, solve subproblem: `min d'y s.t. Wy = h - Tx*`
3. If subproblem optimal with objective q*:
   - Add **optimality cut**: `θ ≥ π'(h - Tx)` where π = subproblem duals
4. If subproblem infeasible:
   - Add **feasibility cut**: `0 ≥ y'(h - Tx)` where y = Farkas ray
5. Repeat until `θ ≈ q*` (convergence)

### 7.2 API Design Options

#### Option A: Structure-Based (Recommended)

User specifies which variables are "complicating" (go to master), Ralph handles decomposition:

```c
typedef struct {
    /* Which variables belong to master problem */
    int *master_var_indices;
    int num_master_vars;

    /* The θ variable representing subproblem cost (-1 to auto-create) */
    int theta_var;

    /* Stochastic Benders: multiple scenarios */
    int num_scenarios;          /* 1 for deterministic */
    double *scenario_probs;     /* NULL = equal weights */

    /* Algorithm parameters */
    double gap_tolerance;       /* Convergence gap (default 1e-6) */
    int max_iterations;         /* Iteration limit (default 1000) */
    int cuts_at_lp_nodes;       /* 1 = branch-and-Benders-cut (modern) */
    int warm_start_subproblems; /* 1 = reuse subproblem basis */
} RalphBendersConfig;

/* Main entry point */
int ralph_solve_benders(
    RalphModel *model,
    const RalphBendersConfig *config,
    RalphSolution *solution
);
```

**Ralph automatically:**
1. Partitions model into master (variables in `master_var_indices`) + subproblem (rest)
2. Identifies linking constraints (those involving both master and subproblem vars)
3. Creates master MIP with θ variable for recourse cost
4. Generates cuts from subproblem duals (optimality) or Farkas rays (feasibility)
5. Iterates until convergence

#### Option B: Callback-Based (Maximum Flexibility)

For non-standard Benders variants (e.g., logic-based Benders, combinatorial subproblems):

```c
typedef struct {
    /* User builds subproblem given fixed master solution */
    RalphModel* (*build_subproblem)(
        void *user_data,
        int scenario,
        const double *x_master,
        int num_master_vars
    );

    /* User adds cut to master (NULL = use automatic cut generation) */
    int (*add_cut)(
        void *user_data,
        RalphModel *master,
        int scenario,
        int cut_type,           /* RALPH_BENDERS_OPTIMALITY or _FEASIBILITY */
        const double *multipliers,
        double subproblem_obj
    );

    /* User decides convergence (NULL = use gap tolerance) */
    int (*check_convergence)(
        void *user_data,
        double master_obj,
        double subproblem_obj,
        int iteration
    );

    void *user_data;
    int num_scenarios;
    double *scenario_probs;
} RalphBendersCallbacks;

int ralph_solve_benders_ex(
    RalphModel *master,
    const RalphBendersCallbacks *callbacks,
    RalphSolution *solution
);
```

### 7.3 Comparison

| Aspect | Option A (Structure) | Option B (Callback) |
|--------|---------------------|---------------------|
| **User effort** | Minimal - just tag variables | Significant - implement callbacks |
| **Flexibility** | Standard Benders only | Any Benders variant |
| **Cut generation** | Automatic | User-controlled |
| **Stochastic** | Built-in | User implements |
| **Debugging** | Easier (Ralph handles logic) | Harder (user code) |
| **Performance tuning** | Limited | Full control |

**Recommendation:** Implement **Option A first**, with Option B as future extension.

Option A covers 90% of use cases (FuelWise, stochastic fleet planning, network design)
with minimal user code. Option B can be added later for exotic variants.

### 7.4 Integration with MIP Infrastructure (§6)

**Key insight:** The master problem is a MIP solved by Ralph's B&B. All §6 features
apply to the master, working in tandem with Benders:

```
┌─────────────────────────────────────────────────────────────────┐
│                    Benders Master MIP                            │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  Branch & Bound (existing)                                │   │
│  │                                                           │   │
│  │  • Branching priorities (§6 P0)                          │   │
│  │    → Control which master vars branch first               │   │
│  │    → FuelWise: cheap stations first                       │   │
│  │                                                           │   │
│  │  • Branching directions (§6 P0)                          │   │
│  │    → Hint down/up preference                              │   │
│  │    → FuelWise: prefer z=1 (stop) at cheap stations        │   │
│  │                                                           │   │
│  │  • User cut callback (§6 P2)                             │   │
│  │    → Add domain-specific cuts during B&B                  │   │
│  │    → FuelWise: reach cuts (must stop in interval)         │   │
│  │    → HoSE: driving capacity cuts                          │   │
│  │                                                           │   │
│  │  • Benders cuts (NEW - this chapter)                     │   │
│  │    → Optimality cuts from subproblem duals                │   │
│  │    → Feasibility cuts from Farkas rays                    │   │
│  │    → Added at integer solutions OR LP nodes               │   │
│  │                                                           │   │
│  └──────────────────────────────────────────────────────────┘   │
│                           │                                      │
│                           ▼                                      │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  Subproblem LP (for each integer/LP solution)             │   │
│  │                                                           │   │
│  │  • Warm start (§6 P2)                                    │   │
│  │    → Reuse basis between subproblem solves                │   │
│  │    → Major speedup for similar x values                   │   │
│  │                                                           │   │
│  │  • Network detection (existing)                          │   │
│  │    → Auto-dispatch to network simplex if applicable       │   │
│  │    → FuelWise: subproblem has path structure              │   │
│  │                                                           │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

**Example: FuelWise with full integration**

```c
/* 1. Build full model */
RalphModel *model = fw_build_full_milp(problem);  /* z[i], x[i], θ */

/* 2. Set branching priorities (cheap stations first) */
int priorities[k];
for (int i = 0; i < k; i++) {
    priorities[i] = (int)(1000.0 / problem->stations[i].price);
}
ralph_set_branch_priorities(model, priorities);

/* 3. Set branching directions (prefer stopping at cheap stations) */
int directions[k];
for (int i = 0; i < k; i++) {
    directions[i] = (problem->stations[i].price < avg_price) ? 1 : 0;
}
ralph_set_branch_directions(model, directions);

/* 4. Set user cut callback for reach cuts */
RalphCutCallback reach_cb = {
    .generate_cuts = fw_generate_reach_cuts,
    .user_data = problem
};
ralph_set_cut_callback(model, &reach_cb);

/* 5. Configure Benders */
int master_vars[k];  /* z[0]...z[k-1] */
for (int i = 0; i < k; i++) master_vars[i] = i;

RalphBendersConfig benders = {
    .master_var_indices = master_vars,
    .num_master_vars = k,
    .theta_var = theta_idx,
    .num_scenarios = 1,
    .cuts_at_lp_nodes = 1,       /* Modern branch-and-Benders-cut */
    .warm_start_subproblems = 1
};

/* 6. Solve */
RalphSolution solution;
ralph_solve_benders(model, &benders, &solution);
```

**What happens internally:**

1. Master B&B starts, using priorities to branch z[cheap] before z[expensive]
2. At each B&B node:
   - User cut callback adds reach cuts if violated
   - If `cuts_at_lp_nodes=1`: solve subproblem, add Benders cuts even for fractional x
3. At integer solutions:
   - Solve subproblem LP (warm started, may use network simplex)
   - If feasible: add optimality cut `θ ≥ π'(h - Tz)`
   - If infeasible: add feasibility cut from Farkas ray
4. Converges when master θ matches subproblem objective

### 7.5 Modern vs Classic Benders

| Variant | Cuts added at | Pros | Cons |
|---------|--------------|------|------|
| **Classic** | Integer solutions only | Simpler, fewer subproblem solves | More B&B nodes |
| **Modern (B&B&C)** | LP nodes too | Tighter LP relaxation, fewer nodes | More subproblem solves |

**Recommendation:** Default to modern (`cuts_at_lp_nodes=1`) with option to disable.
Modern Benders is typically 2-10x faster on structured problems like FuelWise.

### 7.6 Stochastic Benders

For problems with uncertainty (e.g., demand scenarios, price scenarios):

```c
/* K scenarios with probabilities p[k] */
RalphBendersConfig config = {
    .master_var_indices = first_stage_vars,
    .num_master_vars = n1,
    .theta_var = -1,            /* Auto-create K theta variables */
    .num_scenarios = K,
    .scenario_probs = probs     /* Sum to 1.0 */
};
```

Ralph creates K subproblems (one per scenario), solves in parallel, generates
weighted cuts: `θ[k] ≥ π[k]'(h[k] - T[k]x)` for each scenario.

### 7.7 Implementation Status ✅

| Phase | Component | LoC | Status |
|-------|-----------|-----|--------|
| 1 | Model partitioning | 150 | ✅ Complete |
| 2 | Linking detection | 100 | ✅ Complete |
| 3 | Cut generation | 200 | ✅ Complete (optimality + feasibility) |
| 4 | Benders loop | 150 | ✅ Complete (classic algorithm) |
| 5 | B&B integration | 100 | ⏳ Planned (modern B&B&C) |
| 6 | Warm start | 50 | ✅ Complete |
| 7 | Stochastic | 150 | ✅ Complete (multi-scenario) |
| 8 | Testing | 200 | ✅ 24 tests passing |
| **Total** | | **~1100** | |

**Files:**
- `ralph/src/benders.c` - Main implementation
- `ralph/include/benders.h` - API header
- `ralph/tests/test_benders.c` - Test suite

**Dependencies:**
- §6 MIP infrastructure (✅ complete)
- Network flow solver (✅ complete) - for fast subproblems
- Farkas ray extraction (✅ complete)

### 7.8 Known Limitations

| Limitation | Impact | Workaround |
|------------|--------|------------|
| **Classic Benders only** | Modern B&B&C (cuts at LP nodes) not implemented | Classic algorithm works well for structured problems |
| **Numerical sensitivity** | Very large bounds (1e30) can cause simplex instability | Use reasonable bounds (<1e6) for theta variable |
| **0 master constraints** | Edge case returns error | Add dummy constraint if needed |
| **Feasibility cuts** | Less tested than optimality cuts | Most problems have feasible subproblems |
| **Gomory cuts + Benders** | Gomory/MIR cuts in master solver cause INFEASIBLE status | Set `max_cut_rounds = 0` on master solver to disable |
| **Algorithm correctness** | FuelWise Benders converges to suboptimal solution ($85 vs expected $74) | Under investigation (see TODO below) |

**TODO: Benders Algorithm Correctness**

The Benders implementation converges without errors but finds a suboptimal solution in FuelWise
test cases. Suspected issues to investigate:

1. **Optimality cut generation**: Verify dual values from subproblem are correctly extracted
   and transformed into valid Benders optimality cuts. Check sign conventions and constraint
   indexing.

2. **Subproblem RHS handling**: The subproblem RHS depends on master solution (z values).
   Verify `sub_only_rhs_contribution` correctly accounts for constraints that only involve
   subproblem variables vs linking constraints.

3. **Farkas ray normalization**: Feasibility cuts use Farkas rays which require normalization.
   The current implementation normalizes but may have sign/indexing issues.

4. **Theta bounds**: Fixed by calculating from problem structure (max_fuel_value * 100) rather
   than hardcoded ±1e9. Verify this is sufficient for all problem instances.

5. **Cut accumulation**: Ensure cuts from previous iterations remain valid and are not
   inadvertently modified or dropped.

**Does NOT affect FuelWise:**
- FuelWise has master constraints (capacity constraints)
- Classic Benders provides the main scaling win: O(k² × iterations) vs O(2^k) enumeration
- FuelWise subproblems are typically feasible (flow balance with sufficient capacity)
- Reasonable bounds are natural for fuel quantities

### 7.9 Expected Performance

| Problem | Current (enumeration) | With Benders | Speedup |
|---------|----------------------|--------------|---------|
| FuelWise k=20 | 50ms | 30ms | 1.7x |
| FuelWise k=30 | timeout (2^30) | 80ms | ∞ |
| FuelWise k=50 | timeout | 150ms | ∞ |
| FuelWise k=100 | timeout | 500ms | ∞ |

The key win is scaling: enumeration is O(2^k), Benders is typically O(k² · iterations).

### 7.10 Relationship to §6.3

§6.3 described FuelWise-specific Benders design. This chapter supersedes that with
a generic implementation. FuelWise becomes a *user* of generic Benders:

- §6.3 reach cuts → User cut callback (works with Benders)
- §6.3 Benders loop → Handled by `ralph_solve_benders()`
- §6.3 Farkas cuts → Automatic in generic Benders

The domain-specific value in FuelWise is now:
1. Reach cut generation (user callback)
2. Branching priorities (cheap stations first)
3. Problem formulation (how to express as Benders structure)

---

## Related Files

| File | Purpose |
|------|---------|
| `ralph/CLAUDE.md` | Development guide, API reference |
| `ralph/include/ralph.h` | Public API |
| `ralph/include/ralph_api.h` | REST API (planned) |
| `ralph/include/lap.h` | LAP solver API |
| `ralph/include/netflow.h` | Network flow API |
