# Ralph LP Benchmark Skill

Debug and improve Ralph's LP simplex solver by comparing against GLPK on NETLIB benchmark problems.

**Trigger:** Use when asked to improve Ralph LP solver performance, debug numerical issues, compare against GLPK, or analyze simplex algorithm behavior.

## Quick Start

```bash
cd /Users/mark/Desktop/work/artalis-io/otto/ralph

# 1. Build the benchmark tool
make build-ralph-benchmark

# 2. Download NETLIB problems (first time only)
./ralph-benchmark --download-netlib

# 3. Run quick smoke test
./ralph-benchmark --suite tiny -v

# 4. Run single problem for detailed analysis
./ralph-benchmark --netlib afiro -v
```

## Workflow

### Phase 1: Identify Performance Issues

1. **Run benchmark suite and save results:**
   ```bash
   ./ralph-benchmark --suite small > results.json 2>progress.log
   ```

2. **Analyze JSON output for issues:**
   - `validation.solution_valid == false`: Solution is wrong
   - `validation.objective_match == false`: Objective differs from GLPK
   - `validation.numerically_stable == false`: NaN/Inf in solution
   - `performance.ralph_vs_glpk_time > 10`: Significantly slower
   - `performance.ralph_vs_glpk_iters > 2`: Many more iterations

3. **Document the worst cases:**
   - Which problems fail validation?
   - Which problems are slowest relative to GLPK?
   - Are there patterns (problem size, density, structure)?

### Phase 2: Diagnose Root Cause

**Key source files by symptom:**

| Symptom | Primary File | Secondary Files |
|---------|--------------|-----------------|
| Wrong objective | `src/simplex.c` | `src/lu.c` |
| Constraint violation | `src/simplex.c:ratio_test()` | `src/model.c` |
| Cycling/timeout | `src/simplex.c:perturb()` | `src/lu.c` |
| NaN/Inf in solution | `src/lu.c:lu_update()` | `src/simplex.c` |
| Slow per-iteration | `src/lu.c:lu_solve()` | `src/simplex.c:pricing` |
| Too many iterations | `src/simplex.c:select_entering()` | `src/simplex.c:steepest_edge()` |
| Scaling issues | `src/simplex.c:apply_scaling()` | `src/lu.c` |

**Simplex algorithm components:**

| Component | Location | Description |
|-----------|----------|-------------|
| Pricing | `simplex.c:select_entering_variable()` | Choose entering variable (Dantzig, Steepest Edge, Devex) |
| Ratio test | `simplex.c:ratio_test()` | Choose leaving variable |
| Pivot | `simplex.c:simplex_pivot()` | Update basis |
| LU factorization | `lu.c:lu_factorize()` | Initial factorization |
| LU solve | `lu.c:lu_solve()` | Solve Bx=b |
| LU update | `lu.c:lu_update()` | Forrest-Tomlin updates |
| Scaling | `simplex.c:apply_scaling()` | Geometric mean scaling |
| Anti-cycling | `simplex.c:perturb()` | Perturbation method |

### Phase 3: Fix the Bug

**Before writing code, read the file:**
```bash
# Example: debugging LU factorization
cat src/lu.c | head -200
```

**Common fixes by issue type:**

**1. Numerical instability (NaN/Inf):**
- Check pivot tolerance in `lu.c` (RALPH_PIVOT_TOL)
- Check for division by zero
- Add bounds checking on matrix operations

**2. Wrong solution (objective mismatch):**
- Verify reduced cost calculation in `simplex.c`
- Check sign conventions for maximize vs minimize
- Verify scaling/unscaling in `unscale_solution()`

**3. Too many iterations:**
- Check steepest edge weight updates
- Verify Devex pricing implementation
- Consider switching pricing strategy

**4. Slow per-iteration time:**
- Profile LU solve and update operations
- Check for inefficient loops in sparse operations
- Consider refactorization threshold

### Phase 4: Verify the Fix

1. **Rebuild:**
   ```bash
   make clean && make build-ralph-benchmark
   ```

2. **Re-run the failing problem:**
   ```bash
   ./ralph-benchmark --netlib <problem> -v
   ```

3. **Run full test suite:**
   ```bash
   make test       # 73 LP tests
   make test-lap   # 358 LAP tests
   ```

4. **Run benchmark suite to check for regressions:**
   ```bash
   ./ralph-benchmark --suite small -v
   ```

## Benchmark Problems by Category

### Tiny (< 100 vars) - Quick tests
| Problem | Vars | Cons | Notes |
|---------|------|------|-------|
| afiro | 32 | 27 | Simplest NETLIB problem |
| sc50a | 48 | 50 | Small, dense |
| sc50b | 48 | 50 | Similar to sc50a |
| kb2 | 41 | 43 | Small, sparse |

### Small (100-500 vars) - Fast iteration
| Problem | Vars | Cons | Notes |
|---------|------|------|-------|
| blend | 83 | 74 | Blending problem |
| adlittle | 97 | 56 | |
| share2b | 79 | 96 | |
| stocfor1 | 111 | 117 | Stochastic programming |

### Numerically Challenging
| Problem | Vars | Cons | Notes |
|---------|------|------|-------|
| brandy | 220 | 194 | Known numerical issues |
| degen2 | 534 | 444 | Highly degenerate |
| degen3 | 1503 | 1503 | Highly degenerate |
| pilot4 | 1000 | 410 | Numerical difficulties |
| bore3d | 83 | 233 | Ill-conditioned |

### Large (2000+ vars) - Stress tests
| Problem | Vars | Cons | Notes |
|---------|------|------|-------|
| fit1d | 1026 | 24 | Wide, few constraints |
| fit1p | 1677 | 627 | |
| woodw | 8405 | 1098 | Very large |

## JSON Output Fields

```json
{
  "problem": {
    "name": "afiro",       // Problem name
    "vars": 32,            // Number of variables
    "cons": 27,            // Number of constraints
    "is_mip": false        // LP or MIP
  },
  "glpk": {
    "status": "optimal",   // Reference status
    "objective": -464.75,  // Reference objective
    "time_ms": 1.2,        // Reference time
    "iterations": 12       // Reference iterations
  },
  "ralph": {
    "status": "optimal",   // Ralph status
    "objective": -464.75,  // Ralph objective
    "time_ms": 3.4,        // Ralph time
    "iterations": 18       // Ralph iterations
  },
  "validation": {
    "solution_valid": true,      // Overall validity
    "objective_match": true,     // Obj within tolerance
    "objective_rel_error": 1e-15,// Relative error
    "numerically_stable": true   // No NaN/Inf
  },
  "performance": {
    "ralph_vs_glpk_time": 2.83,  // Time ratio
    "ralph_vs_glpk_iters": 1.50, // Iteration ratio
    "ralph_ms_per_iter": 0.189,  // Per-iter time
    "glpk_ms_per_iter": 0.100    // GLPK per-iter time
  },
  "diagnosis": {
    "issues": "",                // Issue description
    "recommendations": []        // Suggested files to check
  }
}
```

## CLI Reference

```bash
# Run single problem
./ralph-benchmark problem.mps            # From file
./ralph-benchmark --netlib afiro         # From NETLIB

# Run test suites
./ralph-benchmark --suite tiny           # 5 problems, ~5 sec
./ralph-benchmark --suite small          # 15 problems, ~30 sec
./ralph-benchmark --suite medium         # 40 problems, ~2 min
./ralph-benchmark --suite all            # All problems

# Options
-v, --verbose           # Progress to stderr
--time-mult N           # Ralph time = N * GLPK time (default: 20)
--hard-cap SEC          # Max time per problem (default: 60)
--lp-only               # Only LP problems (default)
--mip-only              # Only MIP problems
--all-types             # Both LP and MIP

# Tolerances
--obj-rel-tol TOL       # Objective relative tolerance (default: 1e-6)
--obj-abs-tol TOL       # Objective absolute tolerance (default: 1e-8)
--feas-tol TOL          # Feasibility tolerance (default: 1e-6)

# Utility
--list                  # List available NETLIB problems
--download-netlib       # Download NETLIB problems
--help                  # Show help
```

## Performance Targets

Target: Achieve parity with GLPK on LP problems.

| Metric | Target | Notes |
|--------|--------|-------|
| Correctness | 100% | All solutions match GLPK |
| Feasibility | 0 violations >1e-9 | No constraint violations |
| Speed | <5x GLPK time | Acceptable overhead |
| Iterations | <2x GLPK iterations | Efficient pivoting |

## Regression Test Requirement

**IMPORTANT: When a fix is made for a benchmark failure, add a regression test.**

If Claude identifies and fixes a bug discovered through benchmarking:

1. **Create a minimal test case** in `tests/test_main.c`:
   ```c
   static void test_regression_<issue_name>(void) {
       printf("\n=== Test: Regression - <description> ===\n");

       RalphModel *model = ralph_create();
       // ... reproduce the bug scenario ...
       ralph_optimize(model);

       ASSERT(ralph_get_status(model) == RALPH_STATUS_OPTIMAL,
              "Should find optimal solution");
       ASSERT_EQ_DBL(ralph_get_objval(model), expected_obj,
              "Objective should match");

       ralph_free(model);
   }
   ```

2. **Register the test** in `main()`:
   ```c
   test_regression_<issue_name>();
   ```

3. **Run the test** to verify:
   ```bash
   make test
   ```

This ensures the bug cannot regress silently.

## Known Limitations

**NETLIB Download**: The original NETLIB collection uses compressed "emps" format which
requires special decompression. The included test files are simplified LP format versions.
For full NETLIB testing, consider using COIN-OR's pre-converted files or the MIPLIB
collection.

## Checklist Before Committing

- [ ] Ran `/c-audit` on modified files
- [ ] `make test` passes (73+ LP tests including any new regression tests)
- [ ] `./ralph-benchmark --suite tiny` shows no regressions
- [ ] No new compiler warnings with `-Wall -Wextra`
- [ ] Added regression test for any bug fixes
- [ ] Documented the fix in commit message

## Related Skills

- `/c-audit` - Security and safety audit for C code
- Check `ralph/CLAUDE.md` for solver architecture details
