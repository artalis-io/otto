# Benders Debug Map

## System Call Graph

```
fw_solve_refuel_benders() [fuelwise/src/fw_refuel.c:541]
    │
    ├── Build full MILP model with variables:
    │     x[0..k-1]     - fuel purchased (continuous, SUBPROBLEM)
    │     y[k..2k-1]    - cumulative fuel (continuous, SUBPROBLEM)
    │     z[2k..3k-1]   - stop decisions (binary, MASTER)
    │     θ = 3k        - recourse cost (continuous, MASTER)
    │
    └── ralph_solve_benders() [ralph/src/ralph.c → benders.c:1206]
            │
            ├── benders_create() [benders.c:39]
            │     └── Allocate context, cuts, mappings
            │
            ├── benders_partition() [benders.c:165]
            │     ├── Mark master vars (z[i], θ)
            │     ├── Build variable mappings (orig_to_master, orig_to_sub)
            │     ├── Classify constraints:
            │     │     0 = master-only, 1 = sub-only, 2 = linking
            │     └── Build LinkingConstraint array with T coefficients
            │
            ├── benders_build_master() [benders.c:338]
            │     ├── Create master model with z vars + θ
            │     └── Add master-only constraints
            │
            ├── benders_build_subproblem() [benders.c:430]
            │     ├── Create sub model with x,y vars (relaxed to continuous)
            │     ├── Add sub-only constraints (fuel balance, min fuel, tank cap)
            │     └── Add linking constraints with ORIGINAL RHS
            │
            └── benders_solve_classic() [benders.c:967]
                    │
                    └── ITERATION LOOP [benders.c:997]
                          │
                          ├── mip_solve(master) [benders.c:1005]
                          │     └── Get z̄, θ̄ from master_solution
                          │
                          ├── FOR each scenario:
                          │     │
                          │     ├── benders_update_subproblem_rhs() [benders.c:514]
                          │     │     └── sub->b[k] = h_orig - T * z̄
                          │     │
                          │     ├── benders_solve_subproblem() [benders.c:542]
                          │     │     ├── simplex_solve()
                          │     │     ├── Extract duals[] for linking rows
                          │     │     └── Extract farkas[] if infeasible
                          │     │
                          │     └── IF feasible AND sub_obj > θ̄ + tol:
                          │           benders_add_optimality_cut() [benders.c:654]
                          │               ├── Build master_coeffs[j] = Σ_k π_k * T_kj
                          │               ├── constant = sub_obj + Σ π_k * T_kj * z̄_j
                          │               └── Cut: θ + Σ coeff*z >= constant
                          │
                          ├── benders_check_convergence() [benders.c:931]
                          │
                          └── benders_apply_cuts_to_master() [benders.c:876]
                                └── Re-finalize master model, recreate solver
```

## Key Locations for Logging/Asserts

### 1. Iteration Bounds (benders.c:997-1106)
```c
// After line 1027 (master solve):
LOG: iter, LB (master_obj), θ̄, z̄ hash

// After line 1064 (subproblem solve):
LOG: scenario, is_feasible, sub_obj, Q(z̄)
ASSERT: sub_obj <= UB (if tracking true UB)
```

### 2. Cut Generation (benders.c:654-783)
```c
// After line 762 (cut built):
LOG: cut constant, num_terms, coefficients
ASSERT: cut_value(z̄) ≈ sub_obj (tight at generation point)
ASSERT: cut_value(z̄) = constant - Σ coeff*z̄ = sub_obj
```

### 3. Dual Extraction (benders.c:574-581)
```c
// After line 579 (duals copied):
ASSERT: |π_k| < 1e6 (no BigM contamination)
LOG: all linking duals
```

### 4. Subproblem RHS Update (benders.c:522-532)
```c
// After line 532:
LOG: constraint k, original_rhs, updated_rhs, T*z̄ contribution
```

## x-Dependence Classification for FuelWise

### Linking Constraints (x in bounds via z)

| Constraint | Form | T coefficient | Sense |
|------------|------|---------------|-------|
| Upper bound | `x[i] - M*z[i] <= 0` | T = -M (tank_capacity) | <= |
| Lower bound | `x[i] - m*z[i] >= 0` | T = -m (min_purchase) | >= |

### Sub-only Constraints (no z dependence)

| Constraint | Variables | Notes |
|------------|-----------|-------|
| Fuel balance | y[i], x[0..i-1] | Equality, k constraints |
| Min fuel at arrival | y[i] | Inequality >= |
| Tank capacity | y[i], x[i] | Inequality <= |
| Reach destination | x[0..k-1] | Inequality >= |

### Master-only Constraints

| Constraint | Variables | Notes |
|------------|-----------|-------|
| Trivial sum(z) >= 0 | z[0..k-1] | Added to ensure master has constraints |

## Cut Formula Verification

**Standard Benders optimality cut (minimization):**
```
θ >= π'h - π'Tz
```

Where:
- π = dual solution (all constraints)
- h = original RHS vector
- T = technology matrix (coefficients of master vars in linking constraints)
- z = master variables

**Rearranged as >= constraint:**
```
θ + π'T z >= π'h
```

**Code computes:**
```c
constant = sub_obj + Σ_k (π_link_k * T_kj * z̄_j)   // = π'h_orig
coeff[j] = Σ_k (π_link_k * T_kj)                    // = (π'T)_j
```

**Validity check at z̄:**
```
LHS = θ̄ + Σ (coeff[j] * z̄[j])
RHS = constant = sub_obj + Σ (coeff[j] * z̄[j])

LHS - RHS = θ̄ - sub_obj

If θ̄ < sub_obj: cut is violated (will cut off z̄) ✓
If θ̄ = sub_obj: cut is tight (correct) ✓
If θ̄ > sub_obj: cut already satisfied (shouldn't add)
```

## FIXED: BigM Dual Contamination (Feb 2026)

### Root Cause
The simplex solver uses BigM method when equalities < 80% of constraints.
FuelWise subproblem has ~33% equalities → uses BigM.

When artificials with cost 1e8 remain in basis (degenerate or nearly so),
duals become contaminated: π values include 1e8 terms.

**Symptom:** Cut coefficients ~1e8 or ~-2e9, master obj going to 1e30.

### Fix Applied (2 changes)

**1. Force two-phase simplex for Benders subproblems:**
```c
// In benders.c benders_solve_subproblem():
ctx->sub_solvers[scenario]->force_two_phase = 1;
```

**2. Extract Farkas ray in two-phase infeasibility path:**
```c
// In simplex.c Phase 1 infeasibility detection:
extract_farkas_ray(solver);  // Added before setting INFEASIBLE status
```

### Files Changed
- `ralph/include/lp.h`: Added `force_two_phase` field to SimplexSolver
- `ralph/src/simplex.c`: Added `tableau_create_ex()` that respects flag, added Farkas extraction
- `ralph/src/benders.c`: Set `force_two_phase=1` for subproblem solvers

### Verification
- FuelWise Benders test: $74 optimal (was $72-$85 before fix)
- All Ralph tests pass: 125 main, 358 LAP, 153 netflow, 194 detect

## Farkas Ray Validation Invariants

For a valid Farkas certificate of infeasibility, `extract_farkas_ray()` validates:

1. **Nontrivial ray**: `||y||_∞ > 1e-9`
   - Zero ray indicates extraction from wrong state or c_ext costs incorrect

2. **Infeasibility certificate**: `y'b_norm < -1e-9`
   - Must account for constraint sense: `b_norm[i] = -b[i]` for >= constraints
   - This proves no feasible solution exists

3. **Dual feasibility (spot-check)**: `y'a_j >= -1e-6` for sampled columns
   - Check first 10 structural columns
   - Catches "wrong vector" bugs where tab->y isn't row duals

### Key Implementation Notes

- Farkas ray must be extracted **during Phase 1** before restoring Phase 2 costs
- `tab->y` contains row duals `y = c_B' * B^{-1}` computed by `tableau_compute_reduced_costs()`
- Phase 1 costs: 0 for structural/slack, 1 for artificial variables
- Row ordering is stable: row i in `solver->farkas_ray` = original constraint i

### Benders Row Indexing

Subproblem constraints are ordered:
- Rows 0..`num_sub_cons-1`: sub-only constraints
- Rows `num_sub_cons`..`num_sub_cons+num_linking-1`: linking constraints

Both optimality and feasibility cuts read duals/rays starting at `ctx->num_sub_cons`.
