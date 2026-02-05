# Set Covering/Partitioning MIP Performance Plan

**Date:** 2026-02-05
**Status:** Planned
**Priority:** High for trucking/logistics applications

## Executive Summary

Ralph has solid MIP foundations (B&B, Gomory/MIR cuts, pseudo-costs) but treats set covering/partitioning (SCP/SPP) as generic MIPs. This plan adds **structure detection**, **specialized cuts**, **problem-specific heuristics**, and **improved branching** to achieve 5-50x speedups on these problem classes.

## Problem Definitions

| Problem | Formulation | Constraint Sense | Common Applications |
|---------|-------------|------------------|---------------------|
| **Set Covering (SCP)** | min c'x : Ax >= 1, x in {0,1} | >= | Facility location, crew scheduling |
| **Set Partitioning (SPP)** | min c'x : Ax = 1, x in {0,1} | = | Airline crew pairing, vehicle routing |
| **Set Packing** | max c'x : Ax <= 1, x in {0,1} | <= | Independent set, scheduling |

**Key structural properties:**
- Constraint matrix A is 0-1 (each element covered or not)
- Each constraint has RHS = 1
- Variables are binary
- Columns represent "sets" that cover subsets of elements

## Current State Analysis

### What Ralph Has

| Component | Status | Location |
|-----------|--------|----------|
| Branch & Bound | 4 node selection, 4 variable selection strategies | `branch_bound.c` |
| Gomory Cuts | Full implementation | `cuts.c:215-518` |
| MIR Cuts | Full implementation | `cuts.c:543-738` |
| Knapsack Cover Cuts | Simple detection (only <= constraints) | `cuts.c:752-947` |
| Pseudo-costs | Generic implementation | `branch_bound.c:745-780` |
| Presolve | 7 operations + probing | `presolve.c` |
| LAP Detection | Full, with specialized solver | `detect.c:62-310` |
| Network Detection | Full, with specialized solver | `detect.c:598-886` |

### What's Missing for SCP/SPP

- No structure detection for set covering/partitioning
- No clique cuts (placeholder only in CutType enum)
- No odd-hole cuts
- No problem-specific heuristics
- No constraint branching
- No dominance-based preprocessing
- No Lagrangian relaxation

---

## Phase 1: Structure Detection (HIGH PRIORITY)

### 1.1 Add SCP/SPP Detection

**File:** `ralph/src/detect.c`
**New types:**

```c
typedef enum {
    SETCOVER_NONE = 0,
    SETCOVER_COVERING,      /* Ax >= 1, x binary */
    SETCOVER_PARTITIONING,  /* Ax = 1, x binary */
    SETCOVER_PACKING,       /* Ax <= 1, x binary */
    SETCOVER_MIXED          /* Mix of above */
} SetCoverType;

typedef struct {
    SetCoverType type;
    int num_elements;       /* Number of constraints (elements to cover) */
    int num_sets;           /* Number of variables (sets) */
    int *set_size;          /* Number of elements in each set */
    int *element_freq;      /* Number of sets covering each element */
    double density;         /* nnz / (m * n) */
    int *conflict_degree;   /* Number of conflicting sets per set */
} SetCoverSignature;

int detect_set_cover(const LPModel *model, SetCoverSignature *sig);
```

**Detection algorithm:**
1. Check all variables are binary
2. Check all RHS values are 1.0
3. Check all coefficients are 0 or 1
4. Classify constraint senses (>=, =, <=)
5. Compute structural statistics

**Estimated effort:** 150-200 lines

### 1.2 Integrate Detection into MIP Solver

**File:** `ralph/src/mip.c`

```c
/* At root node, after LP solve */
SetCoverSignature scp_sig;
if (detect_set_cover(model, &scp_sig)) {
    solver->problem_class = scp_sig.type;
    solver->scp_signature = &scp_sig;
    /* Enable specialized strategies */
}
```

---

## Phase 2: Specialized Preprocessing (MEDIUM PRIORITY)

### 2.1 Row Dominance Reduction

**File:** `ralph/src/presolve.c`
**New function:** `presolve_row_dominance()`

A row i dominates row j if every set covering element i also covers element j.
For SCP, dominated rows can be removed (they're weaker constraints).

```c
/* For set covering: if row i superset of row j, remove row i */
for (int i = 0; i < m; i++) {
    for (int j = 0; j < m; j++) {
        if (i != j && row_dominates(A, i, j)) {
            mark_row_redundant(i);
        }
    }
}
```

**Complexity:** O(m^2 * avg_row_nnz) - use sparse intersection

### 2.2 Column Dominance Reduction

**New function:** `presolve_column_dominance()`

Column j dominates column k if set j covers everything set k covers and c[j] <= c[k].
Dominated columns can be fixed to 0.

```c
/* If col j superset of col k and c[j] <= c[k], fix x[k] = 0 */
for (int j = 0; j < n; j++) {
    for (int k = 0; k < n; k++) {
        if (j != k && col_dominates(A, j, k) && c[j] <= c[k]) {
            fix_variable(k, 0);
        }
    }
}
```

### 2.3 Essential Set Detection

Sets that are the only ones covering some element must be selected:

```c
/* If element i covered by only one set j, fix x[j] = 1 */
for (int i = 0; i < m; i++) {
    if (element_freq[i] == 1) {
        int j = get_single_covering_set(i);
        fix_variable(j, 1);
        /* Propagate: remove all elements covered by set j */
    }
}
```

**Estimated effort:** 200-250 lines total

---

## Phase 3: Specialized Cutting Planes (HIGH PRIORITY)

### 3.1 Clique Cuts

**File:** `ralph/src/cuts.c`

Two sets **conflict** if they both cover some element (in SPP, at most one can be selected).

```c
typedef struct {
    int *adj_list;      /* Flattened adjacency lists */
    int *adj_ptr;       /* Pointers into adj_list */
    int num_vars;
} ConflictGraph;

ConflictGraph *build_conflict_graph(const LPModel *model, const SetCoverSignature *sig);

/* Clique inequality: sum(x_j : j in clique) <= 1 */
int generate_clique_cuts(MIPSolver *solver, CutPool *pool);
```

**Clique finding algorithm:**
1. Build conflict graph from constraint matrix
2. Find maximal cliques using greedy extension
3. For each clique C with LP violation > threshold:
   - Add cut: sum(x_j) <= 1 for j in C

**Estimated effort:** 250-300 lines

### 3.2 Odd-Hole Cuts

For SPP, odd cycles in the conflict graph yield strong cuts:

```c
/* Odd hole: x_1 + x_2 + ... + x_{2k+1} <= k */
int generate_odd_hole_cuts(MIPSolver *solver, CutPool *pool);
```

**Algorithm:**
1. Find odd cycles in conflict graph via BFS
2. Generate cut for violated cycles

**Estimated effort:** 150 lines

### 3.3 Lifted Cover Inequalities

Strengthen existing cover cuts for SCP:

```c
/* Standard cover: sum(x_j : j in C) <= |C| - 1
 * Lifted: sum(a_j * x_j : j in C) + sum(b_j * x_j : j not in C) <= |C| - 1
 * where a_j, b_j computed via sequential lifting */
int generate_lifted_cover_cuts(MIPSolver *solver, CutPool *pool);
```

**Estimated effort:** 200 lines

---

## Phase 4: Problem-Specific Heuristics (HIGH PRIORITY)

### 4.1 Greedy Set Cover Heuristic

**File:** `ralph/src/mip.c`
**New function:** `heuristic_greedy_set_cover()`

```c
int heuristic_greedy_set_cover(MIPSolver *solver, double *solution) {
    /* Classic greedy: repeatedly select set with best cost/coverage ratio */
    int *uncovered = copy_all_elements();

    while (has_uncovered(uncovered)) {
        int best_set = -1;
        double best_ratio = INFINITY;

        for (int j = 0; j < n; j++) {
            if (solution[j] > 0.5) continue;  /* Already selected */
            int covers = count_newly_covered(j, uncovered);
            if (covers > 0) {
                double ratio = c[j] / covers;
                if (ratio < best_ratio) {
                    best_ratio = ratio;
                    best_set = j;
                }
            }
        }

        if (best_set < 0) return -1;  /* Infeasible */
        solution[best_set] = 1.0;
        mark_covered(best_set, uncovered);
    }
    return 0;  /* Success */
}
```

**Properties:**
- O(m * n) per complete solution
- Approximation ratio: O(log m) for SCP
- Run at root and periodically during B&B

**Estimated effort:** 100 lines

### 4.2 LP-Guided Greedy

Modify greedy to prefer sets with high LP values:

```c
/* Ratio = c[j] / (covers * (1 + lp_value[j])) */
/* Biases toward sets the LP "wants" to select */
```

### 4.3 Local Search Improvement

After greedy, try 1-opt and 2-opt moves:

```c
int heuristic_local_search(MIPSolver *solver, double *solution) {
    /* 1-opt: try removing each selected set */
    for (int j = 0; j < n; j++) {
        if (solution[j] > 0.5) {
            if (still_feasible_without(j, solution)) {
                solution[j] = 0.0;  /* Remove redundant set */
            }
        }
    }

    /* 2-opt: try replacing one set with a cheaper one */
    for (int j = 0; j < n; j++) {
        if (solution[j] > 0.5) {
            for (int k = 0; k < n; k++) {
                if (solution[k] < 0.5 && can_replace(j, k, solution)) {
                    if (c[k] < c[j]) {
                        solution[j] = 0.0;
                        solution[k] = 1.0;
                    }
                }
            }
        }
    }
    return 0;
}
```

**Estimated effort:** 150 lines

---

## Phase 5: Improved Branching (MEDIUM PRIORITY)

### 5.1 Constraint Branching

Instead of branching on variables, branch on constraints:

```c
/* For uncovered element i with fractional coverage:
 * Left branch: element i covered by set j (x_j = 1)
 * Right branch: element i NOT covered by set j (x_j = 0)
 * Select j with highest LP value among sets covering i */

int select_constraint_branch(MIPSolver *solver, int *element, int *set) {
    double max_frac = 0;
    int best_elem = -1;

    for (int i = 0; i < m; i++) {
        double coverage = compute_coverage(i, solver->lp_solution);
        double frac = fabs(coverage - round(coverage));
        if (frac > max_frac) {
            max_frac = frac;
            best_elem = i;
        }
    }

    /* Find best set to branch on for this element */
    *element = best_elem;
    *set = find_best_covering_set(best_elem, solver->lp_solution);
    return 0;
}
```

### 5.2 Pseudo-Cost Initialization

Initialize pseudo-costs using problem structure:

```c
void init_pseudo_costs_scp(MIPSolver *solver) {
    for (int j = 0; j < n; j++) {
        /* Down cost: losing coverage of set_size[j] elements */
        solver->pseudo_cost_down[j] = c[j] / set_size[j];

        /* Up cost: paying c[j] for set_size[j] elements */
        solver->pseudo_cost_up[j] = c[j] / set_size[j];
    }
}
```

### 5.3 SOS1 Branching for SPP

For set partitioning, each element defines an SOS1 constraint:

```c
/* Element i: exactly one of covering sets selected
 * Branch: partition covering sets into two groups */
int branch_sos1_element(MIPSolver *solver, int element) {
    /* Split covering sets by LP value */
    /* Left: sets with lp_value > median, Right: others */
}
```

**Estimated effort:** 200 lines total

---

## Phase 6: Lagrangian Relaxation (LOW PRIORITY)

For very large instances, use Lagrangian bounds:

```c
/* Relax covering constraints with multipliers lambda_i
 * L(lambda) = min c'x + lambda'(1 - Ax) = lambda'1 + min (c - A'lambda)'x
 * Subproblem: trivial (select sets with negative reduced cost)
 */

double lagrangian_bound(MIPSolver *solver, double *lambda) {
    double bound = 0;
    for (int i = 0; i < m; i++) {
        bound += lambda[i];
    }

    for (int j = 0; j < n; j++) {
        double reduced_cost = c[j];
        for (int i : sets_covering(j)) {
            reduced_cost -= lambda[i];
        }
        if (reduced_cost < 0) {
            bound += reduced_cost;  /* Select this set */
        }
    }
    return bound;
}

/* Subgradient optimization to find best lambda */
void lagrangian_dual(MIPSolver *solver) {
    double lambda[m] = {0};
    for (int iter = 0; iter < max_iter; iter++) {
        double bound = lagrangian_bound(solver, lambda);
        /* Update lambda using subgradient */
    }
}
```

**Estimated effort:** 300 lines

---

## Implementation Order & Estimates

| Phase | Component | Priority | Effort | Impact |
|-------|-----------|----------|--------|--------|
| 1.1 | SCP/SPP detection | HIGH | 200 lines | Enables all other phases |
| 4.1 | Greedy heuristic | HIGH | 100 lines | Fast incumbents, better pruning |
| 3.1 | Clique cuts | HIGH | 300 lines | Tighter LP bounds |
| 2.1-2.3 | Preprocessing | MEDIUM | 250 lines | Smaller problems |
| 5.1 | Constraint branching | MEDIUM | 150 lines | Better branch decisions |
| 3.2 | Odd-hole cuts | MEDIUM | 150 lines | Stronger for SPP |
| 4.3 | Local search | MEDIUM | 150 lines | Better solutions |
| 5.2-5.3 | Pseudo-cost init, SOS1 | LOW | 100 lines | Marginal improvement |
| 6 | Lagrangian | LOW | 300 lines | Large instances only |

**Total estimated effort:** ~1,700 lines of new code

---

## Testing Strategy

### New Test Cases (tests/test_mip.c)

```c
/* Small SCP instances */
test_set_cover_5x10();       /* 5 elements, 10 sets */
test_set_cover_detection();  /* Verify detection works */

/* Small SPP instances */
test_set_partition_5x10();
test_set_partition_crew();   /* Crew scheduling example */

/* Benchmark instances */
test_scp_rail507();          /* OR-Library benchmark */
test_scp_rail516();
test_spp_aa01();             /* Airline crew pairing */

/* Cutting plane tests */
test_clique_cut_generation();
test_odd_hole_detection();

/* Heuristic tests */
test_greedy_set_cover();
test_local_search_improvement();
```

### Performance Benchmarks

Compare before/after on:
1. OR-Library SCP instances (scp41-scp410, rail507, rail516)
2. MIPLIB set partitioning instances
3. Synthetic instances with varying density

---

## API Extensions

### New Public Functions (ralph/include/ralph.h)

```c
/* Query detected problem structure */
int ralph_get_problem_class(RalphModel *model);
const char *ralph_problem_class_name(int class_id);

/* Enable/disable specialized strategies */
void ralph_set_scp_heuristics(RalphModel *model, int enabled);
void ralph_set_scp_cuts(RalphModel *model, int enabled);
void ralph_set_constraint_branching(RalphModel *model, int enabled);
```

### New Parameters

```c
ralph_set_int_param(model, "scp_greedy_heuristic", 1);
ralph_set_int_param(model, "scp_clique_cuts", 1);
ralph_set_int_param(model, "scp_constraint_branching", 1);
ralph_set_int_param(model, "scp_local_search", 1);
ralph_set_int_param(model, "scp_presolve_dominance", 1);
```

---

## Expected Performance Improvements

| Problem Type | Current | Expected | Speedup Factor |
|--------------|---------|----------|----------------|
| Small SCP (m<100) | 1x | 5-10x | Heuristics + cuts |
| Medium SCP (m<500) | 1x | 10-20x | All phases |
| Large SCP (m>1000) | Often timeout | Solvable | Detection + Lagrangian |
| SPP (any size) | 1x | 10-50x | Clique cuts very effective |

---

## Dependencies

- Phase 1 (detection) required for all other phases
- Phase 4 (heuristics) independent, can implement first
- Phase 3 (cuts) requires conflict graph from Phase 1
- Phase 5 (branching) requires Phase 1 signature

---

## File Modifications Summary

| File | Changes |
|------|---------|
| `ralph/include/detect.h` | Add SetCoverSignature, detection API |
| `ralph/include/mip.h` | Add SCP parameters, problem_class field |
| `ralph/src/detect.c` | Add detect_set_cover() (~200 lines) |
| `ralph/src/presolve.c` | Add dominance reduction (~250 lines) |
| `ralph/src/cuts.c` | Add clique, odd-hole, lifted cover cuts (~500 lines) |
| `ralph/src/mip.c` | Add heuristics, integrate detection (~400 lines) |
| `ralph/src/branch_bound.c` | Add constraint branching (~200 lines) |
| `ralph/tests/test_mip.c` | Add SCP/SPP test cases (~300 lines) |
| `ralph/CLAUDE.md` | Document new features |

---

## References

1. Balas, E., & Padberg, M. W. (1976). Set partitioning: A survey.
2. Caprara, A., Fischetti, M., & Toth, P. (1999). A heuristic method for the set covering problem.
3. Hoffman, K. L., & Padberg, M. (1993). Solving airline crew scheduling problems by branch-and-cut.
4. Nemhauser, G. L., & Wolsey, L. A. (1988). Integer and Combinatorial Optimization, Ch. II.2.
