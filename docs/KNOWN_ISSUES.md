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
- **Status**: Root cause identified in Phase 1. Not fixed.

**The behaviour.** Same source, same solver parameters, same machine, one
NETLIB problem:

| build | iterations | time | status |
|---|---|---|---|
| `-march=native -ffast-math` (OTTO default) | **45** | 15.1 ms | OPTIMAL, 6.2e-12 |
| drop `-march=native` only | 45 | 13.6 ms | OPTIMAL |
| drop fast-math only | 45 | 15.1 ms | OPTIMAL |
| **drop both** | **20000** (cap) | 72.8 s | iteration limit |
| MSVC 19.44, any `/fp:` setting | **20000** (cap) | 78.5 s | iteration limit |

Either flag alone masks it. Removing both exposes it, and MSVC has neither.

**What it actually is: the primal solution drifts, and nothing notices.**

Phase 1 maintains `x` incrementally across pivots rather than re-solving
`B x_B = b` every iteration. On this problem, under strict IEEE, that
incremental `x` drifts catastrophically far from the true basic solution, and
the drift is invisible until something forces a full recompute.

Measured, on the failing build:

| event | artificial sum before | after full recompute | after refactorize + recompute |
|---|---|---|---|
| first  | 30.1187 | **5194.73** | 5194.73 |
| second | 19823.9 | **4638608.7** | 4638608.7 |

The solver believed Phase 1 infeasibility was 30. It was 5195 -- a factor of 172.
Refactorizing the basis first changes the recomputed value not at all, which is
the whole point: the recompute is not corrupting anything, it is *revealing* a
point the incremental updates had already lost track of. From there the run
never recovers; the artificial sum peaks near 3.2e9 and Phase 1 never completes.
The passing build never exceeds 45.05 and ends at 6.1e-21.

**Why the existing guard cannot see it.** The scaled-direction acceptance path
in `simplex_phase1_zones.c` is guarded by
`p1_engine_direction_preserves_artificial_progress()`, which compares a
*predicted* artificial sum against the current one. Both are computed from the
same drifted `x`, so the guard is self-consistent and wrong about reality. It
passes every time while the true infeasibility climbs.

**Where the drift comes from.** The verbose log shows Phase 1 repeatedly pivoting
on directions with an infinity-norm of 1e6 to 1e9:

    Large direction norm 1.76e+09 at iter 294 (entering=179), re-factorizing before pivot
    Skipping unstable entering column after stabilization attempts (iter=294, entering=180, dir_inf=1.75e+09)
    Large direction norm 1.01e+06 at iter 290 (entering=179), skipping direction-stabilize refactor (cooldown=7)

Two paths let such a pivot through:

- **The cooldown bypass.** `skipping direction-stabilize refactor (cooldown=N)`
  suppresses the stabilizing refactorization precisely when the direction norm
  says it is most needed.
- **The force-pivot budget.** In the dir-stabilize retry loop it sets
  `stabilized = 1` on `refactored_pivot_abs >= RALPH_PIVOT_TOL` alone, skipping
  the `dir_inf <= RALPH_PHASE1_DIR_INF_REFACTOR_TRIGGER` bound that the very next
  block enforces.

Each such pivot injects error into the incrementally-maintained `x`.

**Ruled out, with evidence:**

- *Not the pivot application.* Instrumenting `simplex_pivot()` inside
  `p1_zone_pivot()` over 304 Phase 1 pivots found **zero** that grew the
  artificial sum by even 2x. The blow-up is never in the pivot itself.
- *Not recompute corruption.* Forcing a fresh `tableau_refactorize_with_reason()`
  before recomputing reproduces the same value to all printed digits, and does
  not fix the run (still 20000 iterations).
- *Not the optimiser.* `-O0` and `-O2` both stall once the two flags are gone.
- *Not floating-point mode.* `/fp:precise`, `/fp:fast` and `/fp:strict` are
  identical on MSVC.
- *Not OpenMP.* Building without `-fopenmp`, so the `#pragma omp simd`
  directives are ignored exactly as MSVC ignores them, still converges in 45.
- *Not `max_iterations`.* The passing build returns 45 iterations at every cap
  from 500 to 20000; the parameter is a ceiling and nothing more.

**Presolve is in the loop.** With `presolve=0` the failing build errors out in
125 ms instead of stalling. Presolve alone does not explain it -- the passing
builds run the same presolve -- but the stall needs it.

**Why the flags hide it.** Both builds are bit-identical for the first 107 Phase
1 iterations. FMA contraction and reassociation then nudge pricing and ratio
decisions just enough that the passing build never selects the entering column
that produces the 1e9 direction; it reaches a fourth stall re-perturbation
(`scale 9.0`) and completes Phase 1 at iteration 350. The failing build reaches
only three perturbations before losing the point.

**A related gap, worth fixing regardless.** `p1_zone_stall_detect()` computes an
artificial-sum progress window via `p1_progress_window_update()`, but the whole
block sits behind `if (tab->m < 1000) { ... return P1_ZONE_PROCEED; }`. For any
problem with fewer than 1000 rows -- bore3d has 228 -- that window is dead code,
and stall detection falls back to the change in `tab->obj_value`, which is the
*perturbed* objective and is computed from the same drifted `x`.

**Reproduce, with GCC, no MSVC required:**

    make -C ralph clean
    make -C ralph lib CC_ARCH= CC_FP_FASTMATH= CC_FP_KEEP_NONFINITE=
    # then run bore3d through the NETLIB harness

`make -C ralph clean` is not optional. `make lib` after changing those variables
rebuilds only what is out of date, leaving an archive of mixed-flag objects that
behaves like neither build.

The parameters the harness uses for this case, captured rather than guessed:

    verbose=0  max_iterations=10000000  presolve=1  verify=1
    method=2   random_seed=0  lp_basis_governor_mode=0
    lp_reinvert_controller_mode=1

**Where a fix should go.** Not at the recompute -- that one is the messenger.
Either stop taking pivots whose direction norm implies the incremental update
will destroy `x` (close the cooldown and force-pivot bypasses so
`RALPH_PHASE1_DIR_INF_REFACTOR_TRIGGER` actually binds), or periodically
validate the incremental `x` against a fresh solve and treat a large discrepancy
as a basis failure rather than a new starting point. The second is cheaper to
make safe and would catch the whole class; the first addresses the cause.

Any change here must clear `ralph/benchmarks/netlib_regression_gate.sh` against
its baselines -- this path is shared by every LP Ralph solves.

**A note for whoever picks this up.** The NETLIB harness runs each problem in a
child process, and on Windows that child is created with `bInheritHandles=FALSE`
-- anything it writes to stderr is lost. Diagnostics added inside
`solve_with_ralph` will not appear until the isolation is bypassed by calling
`test_run_job()` directly, or by driving `ralph_test_optimize()` from a small
in-process driver, which is how the numbers above were taken.
