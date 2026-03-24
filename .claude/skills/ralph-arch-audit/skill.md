# Ralph Architecture Audit Skill

Detect architectural decay, leaky abstractions, monkey-patching, and non-orthogonal design in Ralph's LP/MIP solver. Benchmarked against design practices from GLPK, HiGHS, CPLEX, and Gurobi.

**Trigger:** Use when asked to audit Ralph's architecture, find design issues, check for monkey-patching, verify solver layering, or assess abstraction quality.

```
/ralph-arch-audit              # Full audit
/ralph-arch-audit simplex      # Audit simplex only
/ralph-arch-audit mip          # Audit MIP only
/ralph-arch-audit lu           # Audit LU factorization only
/ralph-arch-audit coupling     # Cross-module coupling analysis only
```

## What This Skill Tests

This skill detects seven classes of architectural defect that cause long-term maintenance burden, correctness risk, and performance regressions in LP/MIP solvers.

---

## Category 1: Leaky Abstractions

**Principle (HiGHS/GLPK):** Modules communicate through narrow, documented interfaces. The simplex never reaches into LU struct fields; MIP never reaches into SimplexTableau fields.

### 1.1 Simplex-to-LU Field Access

**What to check:** Search `simplex.c` and `dual_simplex.c` for direct reads/writes of `LUFactorization` struct fields (anything matching `tab->lu->` or `lu->` followed by a field name rather than a function call).

**Allowed:** Calls to `lu_*()` functions (proper API).
**Disallowed:** Direct access to `lu->num_updates`, `lu->growth_factor`, `lu->cond_estimate`, `lu->pivot_tol`, `lu->redundant_rows`, `lu->last_failure_reason`, etc.

**How to test:**
```bash
# Count direct LU field accesses from simplex (should be 0 in ideal design)
grep -nE '(tab->lu|->lu)->[a-z_]+[^(]' ralph/src/simplex.c ralph/src/dual_simplex.c | \
  grep -vE 'lu_[a-z_]+\(' | wc -l
```

**Production solver comparison:**
- **GLPK:** `spx_primal.c` never touches `bflib` internals. All access via `bfd_ftran()`, `bfd_btran()`, `bfd_factorize()`, `bfd_update()`.
- **HiGHS:** `HSimplexNla` wraps all LU access. Simplex calls `simplex_nla.ftran()`, never `factor.L_start[k]`.

**Known violations (baseline to track):**
- `simplex.c` reads `tab->lu->num_updates`, `tab->lu->growth_factor`, `tab->lu->cond_estimate` for telemetry and refactoring decisions
- `simplex.c` writes `tab->lu->redundant_rows`, `tab->lu->allow_regularization`, `tab->lu->max_regularizations`, `tab->lu->pivot_tol`
- Each of these should be wrapped in a setter/getter function

**Severity:** Medium. Coupling is read-heavy (telemetry), not algorithmic. But any new LU struct field change requires auditing all simplex.c access points.

### 1.2 MIP-to-Tableau Field Access

**What to check:** Search `mip.c`, `branch_bound.c`, `cuts.c`, `mip_lp_adapter.h` for direct access to `SimplexTableau` fields.

**Allowed:** Calls through `mip_lp_adapter.h` inline functions.
**Disallowed:** Direct `solver->lp_solver->tableau->basis[i]`, `solver->lp_solver->tableau->x[j]`, etc.

**How to test:**
```bash
# Direct tableau access from MIP code (should go through adapter)
grep -nE '->tableau->' ralph/src/mip.c ralph/src/branch_bound.c ralph/src/cuts.c | wc -l
```

**Production solver comparison:**
- **GLPK:** MIP (`glpios*.c`) uses `glp_get_*()` API to read LP solutions, never touches simplex internals.
- **HiGHS:** `HighsMip` accesses LP through `HighsLpSolver` public interface.

**Known violations:** `mip_lp_adapter.h` is the coupling point — it directly reads `lp->tableau->basis`, `lp->tableau->var_status`, etc. The adapter is thin but not opaque.

### 1.3 Model State Leaking Into Solver

**What to check:** After `simplex_create(model)`, does the solver hold a pointer to the model, or does it copy what it needs?

**Ideal:** Solver copies the problem data. Model can be modified without affecting a running solve.
**Current:** `SimplexSolver` holds a pointer to `LPModel`. Changes to the model during solve would corrupt solver state.

**How to test:**
```bash
# Check if solver stores model pointer vs copy
grep -n 'solver->model\|solver->lp_model' ralph/src/simplex.c ralph/src/ralph.c | head -20
```

---

## Category 2: Phase 1/Phase 2 Non-Orthogonality

**Principle (GLPK/HiGHS):** Phase 1 and Phase 2 of the simplex should be the same algorithm with a different objective vector. Crisis-recovery logic should be shared, not phase-specific.

### 2.1 Phase-Specific Crisis Recovery

**What to check:** Count functions/code paths that exist only for Phase 1 but not Phase 2 (or vice versa).

**How to test:**
```bash
# Phase 1 specific functions (should be minimal)
grep -c 'phase1_' ralph/src/simplex.c
grep -c 'phase1_' ralph/include/lp.h
grep -c 'PHASE1_' ralph/src/simplex.c

# Phase 2 specific functions
grep -c 'phase2_' ralph/src/simplex.c
grep -c 'PHASE2_' ralph/src/simplex.c

# Ratio of phase-specific to shared code (lower is better)
```

**What's problematic:**
- Phase 1 has its own pricing override logic (auto-switch to Devex)
- Phase 1 has dedicated crisis machinery: no-pivot ladder rescue, direction stabilization, window pressure tracking
- Phase 1 has ~80 local state variables for crisis detection vs Phase 2's much simpler loop
- Phase 1 perturbation logic is separate from Phase 2 perturbation logic

**Production solver comparison:**
- **GLPK** (`spx_primal.c`): Single `spx_primal()` function handles both phases with a phase flag. Anti-cycling (perturbation, Bland's rule) is shared infrastructure.
- **HiGHS** (`HEkkPrimal.cpp`): `solve()` method dispatches to `solvePhase1()` and `solvePhase2()`, but both call the same `iterate()` method. Crisis recovery is in `iterate()`, not duplicated per phase.

**Metric:** Count #defines containing "PHASE1" vs "PHASE2" in `simplex.c`:
- If PHASE1 count > 3x PHASE2 count, Phase 1 has accumulated disproportionate special-casing.

### 2.2 Duplicate Iteration Loops

**What to check:** Are there separate `for`/`while` loops for Phase 1 and Phase 2, or does one loop handle both with a phase flag?

**How to test:**
```bash
# Separate phase functions (current design: 2 separate loops)
grep -n 'simplex_phase[12]' ralph/src/simplex.c | head -10
```

**Assessment criteria:**
- Two separate functions with duplicated pivot/pricing/ratio-test logic = non-orthogonal
- One function with phase flag = orthogonal (GLPK pattern)
- Two functions that call shared `iterate()` = acceptable (HiGHS pattern)

### 2.3 Phase Transition Fragility

**What to check:** What happens at the Phase 1 → Phase 2 transition? Is state cleanly handed off?

```bash
grep -n 'transition_phase2\|simplex_transition' ralph/src/simplex.c
```

**Risk areas:**
- Artificial variables removed (must update all basis/nonbasis arrays)
- Pricing weights reset or carried forward (inconsistent → wrong directions)
- Perturbations removed or kept (affects cycling protection)
- LU state preserved or refactored (stale factors → numerical errors)

---

## Category 3: Algorithmic Monkey-Patching

**Principle:** Solver behavior should derive from general algorithmic principles, not problem-size-specific thresholds or instance-specific heuristics.

### 3.1 Problem-Size-Dependent Thresholds

**What to check:** Search for constants that gate behavior on problem dimensions (m, n). These are often added to fix a specific failing problem.

**How to test:**
```bash
# Find problem-size gates (pattern: comparing m or n against constants)
grep -nE '(tab->m|tab->n|solver->m|num_vars|num_cons)\s*(>|<|>=|<=)\s*[0-9]{2,}' \
  ralph/src/simplex.c ralph/src/lp_refactor_policy.c | head -30
```

**Known violations:**
- `PHASE2_DEGEN_ESCAPE_MIN_M = 1200` — degeneracy escape only kicks in for large problems
- `PHASE1_AUTO_DANTZIG_MIN_M = 700` — auto Dantzig pricing only for m >= 700
- `RALPH_SPIKE_DENSE_REJECT_M_MIN = 300` — dense spike guard skipped for small problems
- `SN_MIN_K = 64` — supernodal LU only for k >= 64

**Assessment:**
- Size-dependent gates for algorithmic selection (e.g., "use supernodal if k >= 64") = acceptable (cache behavior genuinely changes with size)
- Size-dependent gates for recovery/crisis logic = monkey-patch (e.g., "only escape degeneracy if m >= 1200")

**Production solver comparison:**
- **GLPK:** Anti-cycling uses Harris ratio test unconditionally — no size gates.
- **HiGHS:** Pricing strategy selection is parameter-driven, not size-gated. Edge-weight initialization adapts, but via continuous formulas, not step thresholds.

### 3.2 Hardcoded Recovery Constants

**What to check:** Count `#define` constants in `simplex.c` and `lp_refactor_policy.c` that control behavior.

**How to test:**
```bash
# Count hardcoded tuning constants (should be minimized, rest should be runtime params)
grep -c '^#define' ralph/src/simplex.c
grep -c '^#define' ralph/src/lp_refactor_policy.c

# Compare: how many of these are exposed as runtime parameters?
grep -c 'set_int_param\|set_double_param' ralph/src/ralph.c
```

**Red flags:**
- Constants with names like `*_RETRY_*`, `*_RESCUE_*`, `*_ESCAPE_*`, `*_FALLBACK_*` — these suggest reactive patches
- Constants that combine multiple conditions (e.g., `PHASE1_FAILED_STABILIZE_RETRY_SHADOW_GUARD_MIN_DIR_INF_RATIO`) — over-specific recovery paths
- More than 50 tuning constants in a single file with no runtime override

**Production solver comparison:**
- **CPLEX:** All behavioral parameters in `CPXsetintparam()`/`CPXsetdblparam()`. Users can tune everything.
- **Gurobi:** All tuning via `GRBsetintparam()`/`GRBsetdblparam()`. No compile-time behavioral constants.
- **HiGHS:** `HighsOptions` struct has ~100 runtime-tunable parameters. Compile-time constants are only for true invariants (array sizes, mathematical constants).

### 3.3 Crisis State Machine Complexity

**What to check:** In Phase 1's main loop, how many distinct "crisis" or "recovery" states exist? Each one likely represents a specific problem that broke the solver.

**How to test:**
```bash
# Count crisis/recovery state variables in Phase 1
grep -nE '(streak|crisis|rescue|escape|ladder|force|stall|stagnation|breakdown)' \
  ralph/src/simplex.c | wc -l
```

**Assessment:**
- 0-5 recovery mechanisms = healthy (standard: Bland's rule, perturbation, refactorization)
- 6-10 = moderate accumulation
- 11+ = algorithmic monkey-patching (each added for a specific failing instance)

**Standard recovery mechanisms in production solvers:**
1. Bland's anti-cycling rule
2. Bound perturbation / shift
3. Basis refactorization
4. Scaling adjustment
That's it. Four mechanisms, not fifteen.

---

## Category 4: Module Coupling Metrics

### 4.1 Fan-Out Analysis

**What to check:** How many other modules does each source file call into?

**How to test:**
```bash
# simplex.c fan-out (unique non-standard includes)
grep '#include' ralph/src/simplex.c | grep -v '<' | sort -u | wc -l

# Number of distinct function prefixes called from simplex.c
grep -oE '[a-z_]+\(' ralph/src/simplex.c | sed 's/($//' | \
  grep -E '^(lu_|lp_|mip_|pricing_|tableau_|ralph_)' | \
  sed 's/_[a-z_]*$//' | sort -u | wc -l
```

**Assessment:**
- Fan-out > 8 distinct module prefixes from a single file = high coupling
- `simplex.c` likely calls into: `lu_*`, `lp_refactor_policy_*`, `lp_reinvert_controller_*`, `lp_basis_governor_*`, `lp_telemetry_*`, `lp_glpk_strict_*`, `tableau_*`, `pricing_*` — that's 8+ modules

**Production solver comparison:**
- **GLPK** `spx_primal.c`: Calls `bfd_*` (LU), `spx_chuzc*` (pricing), `spx_chuzr*` (ratio test), `spx_update*` (basis). Four module interfaces.
- **HiGHS** `HEkkPrimal.cpp`: Calls `simplex_nla` (LU), `devex` or `steepest_edge` (pricing), `HighsSimplexInfo` (state). Three module interfaces.

### 4.2 File Size Analysis

**What to check:** Oversized files indicate accumulated responsibilities.

**How to test:**
```bash
wc -l ralph/src/simplex.c ralph/src/dual_simplex.c ralph/src/lu.c \
     ralph/src/lu_sparse.c ralph/src/mip.c ralph/src/branch_bound.c \
     ralph/src/lp_refactor_policy.c ralph/src/model.c ralph/src/ralph.c \
     ralph/src/presolve.c
```

**Assessment:**
- < 2000 lines: healthy for a core algorithm file
- 2000-5000 lines: needs extraction of sub-concerns
- 5000-10000 lines: significant refactoring needed
- > 10000 lines: monolith — multiple algorithms mixed in one file

**Production solver comparison:**
- **GLPK** `spx_primal.c`: ~900 lines. `spx_dual.c`: ~800 lines. `bfd_*.c`: ~300-500 lines each.
- **HiGHS** `HEkkPrimal.cpp`: ~1200 lines. `HEkkDual.cpp`: ~1500 lines. `HFactor.cpp`: ~800 lines.

### 4.3 Struct Size Analysis

**What to check:** How many fields does each key struct have? Large structs indicate accumulated state and mixed concerns.

**How to test:**
```bash
# Count fields in key structs (approximate via member declarations)
# SimplexTableau fields
awk '/^typedef struct/,/SimplexTableau;/' ralph/include/lp.h | grep -cE '^\s+(int|double|char|size_t|void|const|SparseMatrix|LU|Arena)\s'

# SimplexSolver fields
awk '/^typedef struct/,/SimplexSolver;/' ralph/include/lp.h | grep -cE '^\s+(int|double|char|size_t|void|const)\s'

# LUFactorization fields
awk '/^typedef struct/,/LUFactorization;/' ralph/include/lp.h | grep -cE '^\s+(int|double|char|size_t|void|const)\s'
```

**Assessment:**
- < 30 fields: healthy
- 30-60 fields: consider extracting sub-structs (pricing state, LU config, telemetry)
- 60+ fields: mixed concerns — multiple sub-systems packed into one struct

**Production solver comparison:**
- **GLPK** `SPXLP`: ~15 fields (dimensions, matrix, bounds, solution, basis)
- **HiGHS** `HighsSimplexInfo`: ~40 fields but grouped into named sub-structs

---

## Category 5: Error Domain Separation

**Principle:** Errors from LU (numerical), simplex (algorithmic), and MIP (structural) should be typed differently and propagated cleanly.

### 5.1 Error Type Mixing

**What to check:** Does `lu_update()` return the same error code (-1) for "singular pivot" and "out of memory"?

```bash
grep -n 'return -1' ralph/src/lu.c | head -20
```

**Assessment:** If a function returns -1 for multiple failure modes, the caller must inspect side-channel state (`lu->last_failure_reason`). This is error-prone.

**Production comparison:**
- **HiGHS:** Returns typed enum `HighsStatus`. Each failure mode has its own value.
- **GLPK:** Returns specific error codes (GLP_EBADB, GLP_ESING, GLP_EBOUND, etc.)

### 5.2 Status Propagation Chain

**What to check:** How does an LU failure propagate up to the user?

```
LU failure → simplex detects → sets solver status → ralph extracts → user sees RalphStatus
```

Each transition is a potential information loss point. Check that failure reasons survive.

---

## Category 6: Solver State Machine Explicitness

**Principle (HiGHS):** The solver should have explicit, enumerated states with well-defined transitions.

### 6.1 Implicit State

**What to check:** Is the solver's current state (initializing, Phase 1, transitioning, Phase 2, done, error) represented as an enum, or is it implicit in which function is executing?

```bash
grep -n 'typedef enum.*State\|typedef enum.*Phase\|typedef enum.*Status' ralph/include/lp.h
```

**Assessment:**
- Explicit state enum with transition validation = good
- State implicit in call stack = fragile (can't inspect, can't serialize for debugging)

**Production comparison:**
- **HiGHS:** `SimplexAlgorithm` enum + `HighsSimplexStatus` struct with explicit phase tracking
- **GLPK:** Phase tracked via `lp->phase` field (1 or 2)

---

## Category 7: Parameter System Completeness

**Principle (CPLEX/Gurobi):** Every behavioral constant that could reasonably vary between problem classes should be a runtime parameter, not a compile-time `#define`.

### 7.1 Compile-Time vs Runtime Parameters

**What to check:** Ratio of `#define` behavioral constants to `ralph_set_*_param()` entries.

**How to test:**
```bash
# Compile-time behavioral constants
grep -cE '^#define\s+(RALPH_|PHASE[12]_|MIP_|LP_)' \
  ralph/src/simplex.c ralph/src/lp_refactor_policy.c ralph/src/mip.c ralph/src/cuts.c

# Runtime parameters
grep -c 'set_int_param\|set_double_param\|set_string_param' ralph/include/ralph_lp.h ralph/include/ralph_mip.h
```

**Assessment:**
- Ratio > 5:1 (compile:runtime) = most behavior is baked in
- Ratio 2:1 - 5:1 = moderate configurability
- Ratio < 2:1 = good (approaching CPLEX/Gurobi level)

### 7.2 Missing Runtime Parameters

Cross-reference compile-time constants against what production solvers expose:

| Parameter | CPLEX | Gurobi | HiGHS | Ralph |
|-----------|-------|--------|-------|-------|
| Feasibility tolerance | `CPX_PARAM_EPRHS` | `FeasibilityTol` | `primal_feasibility_tolerance` | `feas_tol` (runtime) |
| Optimality tolerance | `CPX_PARAM_EPOPT` | `OptimalityTol` | `dual_feasibility_tolerance` | `opt_tol` (runtime) |
| Pivot tolerance | `CPX_PARAM_EPMRK` | `MarkowitzTol` | `factor_pivot_tolerance` | `pivot_tol` (runtime) |
| Refactorization interval | `CPX_PARAM_REINV` | N/A | `simplex_update_limit` | `max_updates` (compile) |
| Perturbation amount | `CPX_PARAM_EPPER` | `PerturbValue` | `simplex_perturbation_limit` | #define (compile) |
| Degeneracy tolerance | part of `EPRHS` | part of `FeasibilityTol` | `primal_simplex_bound_perturbation_multiplier` | #define (compile) |
| Growth factor threshold | N/A | N/A | N/A | #define (compile) |
| Pricing strategy | `CPX_PARAM_PPRIIND` | `SimplexPricing` | `simplex_strategy` | `pricing` (runtime) |
| Scaling | `CPX_PARAM_SCAIND` | `ScaleFlag` | `simplex_scale_strategy` | `scaling` (runtime) |

---

## Report Format

```markdown
## Ralph Architecture Audit Report

**Date:** YYYY-MM-DD
**Scope:** {full | simplex | mip | lu | coupling}

### Scores

| Category | Score | Grade | Notes |
|----------|-------|-------|-------|
| 1. Leaky Abstractions | N direct accesses | A-F | |
| 2. Phase Orthogonality | ratio P1:P2 specific | A-F | |
| 3. Monkey-Patching | N crisis mechanisms | A-F | |
| 4. Module Coupling | fan-out count | A-F | |
| 5. Error Separation | N mixed returns | A-F | |
| 6. State Machine | explicit/implicit | A-F | |
| 7. Parameter System | compile:runtime ratio | A-F | |

### Grading Scale

- **A:** Production solver quality (CPLEX/Gurobi level)
- **B:** Mature open-source quality (HiGHS level)
- **C:** Functional but has accumulated design debt (GLPK level)
- **D:** Significant architectural issues affecting maintainability
- **F:** Fundamental design problems blocking correctness

### Detailed Findings

[Per-category findings with file:line, severity, and fix suggestions]

### Recommendations (Prioritized)

1. **Quick wins** (< 1 day each, no algorithmic risk)
2. **Medium effort** (1-3 days, requires test coverage)
3. **Major refactors** (1+ weeks, requires planning phase)
```

## Audit Procedure

When `/ralph-arch-audit` is invoked:

1. **Run automated checks** — Execute the grep/wc commands from each category
2. **Read key files** — simplex.c (Phase 1 loop, Phase 2 loop, pivot function), lp.h (struct definitions), mip.c (node solve, branching dispatch)
3. **Score each category** — Using the metrics defined above
4. **Compare against baseline** — Previous audit results (if any) stored in `docs/roadmaps/ralph.md`
5. **Generate recommendations** — Prioritized by impact/effort ratio

## What This Skill Does NOT Test

- **Correctness** — Use `make test` and NETLIB benchmarks for that
- **Performance** — Use `/ralph-benchmark` for that
- **Memory safety** — Use `/c-audit` for that
- **API contract** — Use `make test-api` for that

This skill tests **design quality** — whether the code is structured to remain correct and performant as features are added.
