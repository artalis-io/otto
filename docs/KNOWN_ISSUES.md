# Known Issues and Limitations

## Performance Issues

### LP Solver
- **GLPK comparison**: Ralph is 2-13x slower than GLPK per iteration on larger problems (500+ variables)
- **Dual simplex stability**: Dual simplex can accumulate reduced cost errors, sometimes falling back to primal simplex (see `dual_simplex.c`)

### MIP Solver
- **Set partitioning problems**: Very slow on equality-constrained MIP problems (e.g., 37s vs 0.0002s for GLPK on 30-variable problems)
- **Large facility location**: Times out on medium-sized problems (210 variables, 10 integer)
- **Limited presolve for MIP**: Lightweight presolve (mask 0x110F) is enabled by default for MIP but aggressive techniques (singleton cols, probing, proportional rows) cause slowdowns

## Cut Generation Bugs

### c-MIR Cuts From Purely Continuous Rows (cuts.c:cmir_build_cut) — FIXED

- **Severity**: High — generated invalid cuts that removed the true integer optimum
- **Status**: Fixed. `cmir_row_has_integer_support()` now gates cut construction.
- **Trigger**: Any model where a c-MIR candidate row has no integer variable in
  its support, including after aggregation.
- **Root cause**: The MIR derivation strengthens `sum a_j x_j <= b` into
  `... <= floor(b)`. That rounding is justified only by the integrality of at
  least one variable in the row. `cmir_build_cut()` applied it unconditionally,
  so on a purely continuous row it tightened the right-hand side by
  `f_0 ∈ (0,1)` with nothing to justify it — an inequality that removes
  feasible points rather than fractional ones.
- **Evidence**: On `fw_bench_config_milp_100()` seed 125 with
  `RALPH_ENABLE_MIR_ROOT_CUTS=1`, all 10 generated cuts came from rows with
  zero integer variables, and 8 of the 10 excluded the true optimum. Ralph
  reported 1595.124441 as OPTIMAL against a true optimum of 1585.247322 —
  feasible, validating, and wrong, so nothing downstream flagged it.
- **Fix**: Rows with no integer support are skipped before the `(C, delta)`
  separation search, counted in `root_mir_rows_no_integer`, and rejected again
  inside `cmir_build_cut()` (the function that performs the rounding).
- **Side effect**: The invalid cuts were also driving a branch-and-bound
  explosion. The seed=125 MIR-on solve went from roughly five minutes to
  0.1 seconds.
- **Regression test**: `test_raw_milp100_seed125_shifted_presolve_mir_optin()`
  in `fuelwise/bench/test_validator.c`, gating in CI via `test-c`.
- **Earlier work on this entry**: two back-substitution sign errors (the
  complementation and upper-bound constants were added rather than subtracted)
  were corrected previously, along with a safety check rejecting cuts whose
  minimum possible LHS exceeds their RHS. Both are still in place. The check is
  a cheap sanity net, not the fix — it never caught this family, because a
  cut over continuous variables with finite bounds looks perfectly satisfiable.
- **Still open**: `generate_mir_cuts()` also carries a narrower heuristic that
  drops all-positive cuts with exactly one integer term when a shifted pivot
  was used. It is unrelated to the bug above and no test currently requires it;
  it should be re-examined on its own terms.
- **Note**: c-MIR remains opt-in behind `RALPH_ENABLE_MIR_ROOT_CUTS`.

### No Recovery When LP Becomes Infeasible After Cuts (mip.c:1146-1154)
- **Severity**: Medium — compounded the c-MIR bug above; still worth fixing on its own
- **Root cause**: When the LP becomes infeasible after adding cuts, `mip.c` blindly sets `solver->status = solver->lp_solver->status` (INFEASIBLE) and returns immediately, without checking `solver->has_incumbent`. If the diving heuristic already found a valid integer solution, the solver should report OPTIMAL.
- **Fix**: Check for existing incumbent before propagating LP infeasibility. Optionally attempt recovery by removing the last batch of cuts.

### GMI Slack Variable Projection
- GMI cuts with significant positive slack coefficients (>0.1) are rejected to prevent cutting off integer feasible points
- This makes GMI cuts mostly ineffective for >= constraints
- See `cuts.c:generate_gmi_cut()` for the rejection logic

## Numerical Issues

### Artificial Variable Residuals
- After Big-M phase 1, artificial variables may have tiny non-zero values (1e-5 to 1e-6)
- These are numerical noise but were previously causing incorrect objective reporting
- Fixed in commit `eb50d7e` by computing objective from original variables only

### LU Factorization
- LU update accumulates numerical errors over many pivots
- Refactorization every 50 iterations helps but may not be sufficient for ill-conditioned problems
- Sparse triangular solves use reach-based algorithms which can accumulate rounding errors

## API Issues

### Return Value Confusion
- `ralph_optimize()` returns 0 for success, -1 for error (not the solve status)
- Use `ralph_get_status()` to get the actual solve status
- This was a source of bugs in benchmark code (fixed in commit `eb50d7e`)

## Missing Features

### LP Features
- Barrier/interior point method
- Network simplex for pure network problems
- Basis crash procedures

### MIP Features
- Primal heuristics (RINS, feasibility pump)
- Conflict analysis and learning
- Dual steepest edge pricing
- Parallel branch and bound

## Test Coverage Gaps

### Untested Scenarios
- Very sparse problems (density < 5%)
- Highly degenerate problems
- Problems with many equality constraints
- Problems requiring many branching iterations
- Numerical edge cases (very large/small coefficients)

## Platform-Specific Issues

### macOS
- No known platform-specific issues

### Linux
- Not actively tested but should work

### Windows
- Not tested, may need build system adjustments

## Workarounds

### For slow MIP problems
1. Try adding cuts manually if you know the problem structure
2. Use `max_nodes` parameter to limit exploration
3. Use `time_limit` parameter to cap solve time
4. Consider reformulating with fewer equality constraints

### For numerical issues
1. Scale your problem data to avoid very large/small coefficients
2. Reduce iteration count between refactorizations if needed
3. Use `verbose=2` to see iteration details

## Reporting Issues

File issues at: https://github.com/ottofleet/otto/issues

Include:
- Problem description (ideally MPS or C code to reproduce)
- Expected vs actual behavior
- Ralph version (`ralph_version()`)
- Platform and compiler

## Compiler-Dependent Behaviour

### bore3d does not converge under plain IEEE arithmetic

- **Severity**: High. This is a solver bug, not a build problem, and OTTO's own
  flags are what hide it. Any build without `-march=native` or `-ffast-math` --
  a plain `gcc -O2`, a distro package, another compiler -- fails to solve a
  NETLIB LP that takes 45 iterations here.
- **Status**: Diagnosed, not fixed.
- **Found**: While bringing Ralph up on MSVC, which has no equivalent of either
  flag. It looked like an MSVC problem for exactly as long as it took to
  reproduce it under GCC.

**The behaviour.** Same source, same solver parameters, same machine, one
NETLIB problem:

| build | iterations | time | status |
|---|---|---|---|
| `-march=native -ffast-math` (OTTO default) | **45** | 15.1 ms | OPTIMAL, 6.2e-12 |
| drop `-march=native` only | 45 | 13.6 ms | OPTIMAL |
| drop fast-math only | 45 | 15.1 ms | OPTIMAL |
| **drop both** | **20000** (cap) | 75.9 s | iteration limit |
| MSVC 19.44, any `/fp:` setting | **20000** (cap) | 78.5 s | iteration limit |

Either flag alone masks it. Removing both exposes it, and MSVC has neither.

**It is not the optimiser and not fast-math semantics.** `-O0` and `-O2` both
stall, at 20000 iterations, once the two flags are gone. What the flags change
is the arithmetic -- FMA contraction from `-march=native`, reassociation from
`-ffast-math` -- and either perturbation is enough to knock the pivot sequence
off whatever degenerate path strict IEEE evaluation walks into.

**It is not floating-point mode.** `/fp:precise`, `/fp:fast` and `/fp:strict`
are identical on MSVC. All three differ from the GCC flags, and none reproduces
GCC's result.

**It is not OpenMP.** Building the GCC side without `-fopenmp`, so the 29
`#pragma omp simd` directives are ignored exactly as MSVC ignores them, still
converges in 45 iterations.

**Presolve is in the loop.** With `presolve=0` the failing build errors out in
125 ms instead of stalling. Presolve alone does not explain it -- the passing
builds run the same presolve -- but the stall needs it.

**Reproduce, with GCC, no MSVC required:**

    make -C ralph clean
    make -C ralph lib CC_ARCH= CC_FP_FASTMATH= CC_FP_KEEP_NONFINITE=
    # then run bore3d through the NETLIB harness

The parameters the harness uses for this case, captured rather than guessed:

    verbose=0  max_iterations=10000000  presolve=1  verify=1
    method=2   random_seed=0  lp_basis_governor_mode=0
    lp_reinvert_controller_mode=1

`max_iterations=10000000` is why an untouched run burns its whole wall-clock cap
instead of reporting an iteration limit; clamp it to see the real outcome.

**Where to look.** 20000 iterations on a 315x233 LP, with time per iteration
roughly 11x the healthy build's, is what a basis degrading into constant
refactorization looks like. Ralph's anti-cycling machinery -- Bland,
perturbation, the stabilise and rescue ladders -- is either not triggering on
this path or not helping. That machinery has only ever been exercised against
arithmetic that carries FMA or reassociation, which is the gap this exposes.

**A note for whoever picks this up.** The NETLIB harness runs each problem in a
child process, and on Windows that child is created with `bInheritHandles=FALSE`
-- anything it writes to stderr is lost. Diagnostics added inside
`solve_with_ralph` will not appear until the isolation is bypassed by calling
`test_run_job()` directly.
