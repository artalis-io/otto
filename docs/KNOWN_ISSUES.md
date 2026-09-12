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

### bore3d does not converge under plain IEEE arithmetic -- FIXED

- **Status**: Fixed. Kept here because the diagnosis took three attempts and
  two of them were wrong in instructive ways.

**The symptom.** Any build without `-march=native` or `-ffast-math` -- a plain
`gcc -O2`, a distro package, MSVC, which has neither -- failed to solve a
NETLIB LP that takes 45 iterations with them, running to the 20000 iteration
cap in 76 seconds instead.

**The cause.** Phase 1 would occasionally reach a basis so ill-conditioned that
`B^-1 b` is meaningless, and a DIR_SKIP full recompute would then *adopt* that
point -- taking artificial feasibility from 30.1 to 5194.7 in one step, and
from there to 3.2e9 without recovering.

**The fix.** `simplex_phase1_zones.c` declines the recompute when the basis
condition estimate exceeds `RALPH_PHASE1_RECOMPUTE_COND_LIMIT` (1e4), keeping
the incrementally maintained `x` and leaving the existing stall machinery to
make progress. bore3d now solves in 39 iterations under strict IEEE and 45
with the default flags, to the same objective either way.

**What made it measurable.** Probing every Phase 1 iteration -- snapshot,
strictly refactorize, recompute, compare, restore -- over the first 250
iterations of the failing run:

| | |
|---|---|
| incremental and true artificial sum agree to 9 decimal places | 227 |
| agree within 1% | 17 |
| differ by more than 1% | 6 |

and on those six the condition estimate runs 4.9e4 to 1.9e5 against a median of
7.3 everywhere else. The threshold sits below all six. It also declines on 14
of the 244 good iterations, which costs nothing: on those the two values agree
to within 1% anyway, so keeping the incremental one loses nothing. The
asymmetry is the point -- refusing a good recompute keeps a correct `x`,
adopting a bad one loses the solve.

**Two wrong answers first, recorded so nobody retraces them.**

- *"The primal solution drifts."* It does not. The measurement above says the
  incremental `x` is accurate on 244 of 250 iterations. The failure is six
  isolated bad bases, not accumulated error. A fix built on the drift theory
  (refactorize before accepting a worsened recompute) reproduces the same
  value to every printed digit and does not help: better arithmetic on a bad
  basis is still a bad basis.
- *"The force-pivot budget lets the bad pivot through."* Gating it on
  `p1_candidate_basis_refactorable()` -- which applies the candidate basis,
  strictly refactorizes and compares the true artificial sum -- also does not
  help. The bad bases are reached through ordinary pivots too.

**Reproduce the old failure** (on a commit before the fix):

    make -C ralph clean
    make -C ralph lib CC_ARCH= CC_FP_FASTMATH= CC_FP_KEEP_NONFINITE=

`make -C ralph clean` is not optional: `make lib` after changing those
variables rebuilds only what is out of date, leaving an archive of mixed-flag
objects that behaves like neither build.

### The NETLIB regression gate could not run on Windows -- FIXED

- **Severity**: was Medium. **Status**: fixed; root cause was a broken
  availability probe, not the solver.

This entry previously said three problems -- `25fv47`, `bandm`, `scagr25` --
"do not meet its checks on Windows", and speculated about per-platform
baselines. That was wrong, and the shape of it should have been the clue:
those three are the baseline's *entire* `required_pass` set, so all three
failing together was systematic rather than three numerical coincidences.

They were never solved at all. `ralph-benchmark` probed for its reference
oracle with:

```c
system("which glpsol >/dev/null 2>&1")
```

On Windows `system()` runs `cmd.exe`, which has neither `which` nor
`/dev/null` -- it reads the redirect as a path and fails with "The system
cannot find the path specified" whether or not glpsol is installed. GLPK is
required outside `--test` mode, so the harness exited 1 before solving
anything, and the gate recorded three command failures. Every mismatch
artifact it wrote was empty, which is what gave it away.

Six other call sites had copied the same spelling, including
`test_lp_external_glpk_oop_integration.c`, whose "glpsol not in PATH" skip
was therefore unconditional on Windows rather than a real dependency check.
All seven now use `sh_pal_program_on_path()`, which asks `where` on Windows
and `which` elsewhere.

**Result on Windows once it could run.** The full 84-problem gate:

| category | count |
|---|---|
| objective mismatches | 0 |
| status mismatches | 0 |
| invalid solutions | 0 |
| command failures | 0 |
| dense-fallback violations | 0 |
| unexpected timeouts | 1 (`bnl1.mps`) |

So the solver agrees with GLPK on every problem it finishes, and the
per-platform-baseline theory was unnecessary.

### Two things this turned up, still open

**`bnl1.mps` overruns its own cap.** It is not in the baseline's
`known_timeouts`, nor in `required_pass` or `required_coverage`. It was killed
by the *external* 50s wrapper (`time_ms: 50000`, exit 124) rather than
stopping at the 20s hard cap, so the internal cap did not take effect. Whether
that is Windows-specific is unmeasured -- the gate has never been run on Linux
either.

**Nothing runs the gate.** No workflow invokes
`ralph/benchmarks/netlib_regression_gate.sh`; CI runs `make -C ralph
test-netlib`, which is `--test` mode against a table of reference optimals and
needs no GLPK. A full gate run takes upwards of twenty minutes, so putting it
in CI is a scheduling decision rather than a free win.

**Two traps for whoever runs it by hand on Windows.** The gate shells out to
`make`, so it needs `--no-build` with a `mingw32-make`-built binary --
`/usr/bin/make` strips `TMPDIR` and dies with "Cannot create temporary file in
C:\WINDOWS\". And it must run under MSYS2's UCRT64 shell, not Git Bash:
MSYS2's `jq.exe` segfaults there because it loads Git Bash's `msys-2.0.dll`
instead of its own.

**A note for whoever works in this area.** The NETLIB harness runs each problem
in a child process, and on Windows that child is created with
`bInheritHandles=FALSE` -- anything it writes to stderr is lost. Diagnostics
added inside `solve_with_ralph` will not appear until the isolation is bypassed
by calling `test_run_job()` directly, or by driving `ralph_test_optimize()`
from a small in-process driver.
