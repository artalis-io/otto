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

### Root Gomory Cuts Substituted Artificial Variables (cuts.c) — FIXED

- **Severity**: High — Ralph reported a suboptimal solution as OPTIMAL.
- **Status**: Fixed. Artificial variables are now skipped by GMI and c-MIR
  source-row extraction.
- **Trigger**: any model where a `>=` row (including a previously added `>=`
  cut row) contributes an artificial variable to the tableau, from the second
  root cut round onward.
- **Root cause**: GMI cut generation substituted every auxiliary variable out
  of the cut with

  ```
  s = aux_coef * (b - A x)
  ```

  That identity holds only when the auxiliary is its row's *only* one. In
  non-dual mode a `>=` row carries both a surplus (coef -1) and an artificial
  (coef +1):

  ```
  A x - surplus + artificial = b
  ```

  so the artificial is `b - A x + surplus`, not `b - A x`. Substituting it
  dropped the surplus and injected a spurious linear term into the cut. The
  same applies to the artificial a `<=` row gains when it starts infeasible.
- **Why it took until round 2**: round 1 has no `>=` cut rows in the tableau,
  so no such artificial is nonbasic yet. GMI cuts are themselves `>=` rows, so
  once round 1's cuts are applied, round 2 has them.
- **Why nothing downstream flagged it**: the bad term is invisible at the LP
  point the cut is generated from. Every nonbasic deviation is zero there, so
  the cut still satisfies `violation == f_0` exactly — the natural
  self-consistency check passes. The term only bites at other points, where it
  removes integer optima.
- **Evidence**: on `./ralph/repro_cfl_crash cfl 6 40`, 38 of 40 objectives
  matched GLPK and two did not — 154 vs 152 (trial 1) and 174 vs 173
  (trial 26), both minimisations, so Ralph was returning a feasible but worse
  solution and calling it optimal. Instrumenting the cut loop showed the
  offending nonbasic to be the artificial of a cut row, with a *negative*
  deviation at the true optimum (`t = -2.142857`); GMI requires every nonbasic
  deviation to be non-negative.
- **Fix**: `generate_gmi_cut_from_row()` and `cmir_extract_source_row()` skip
  `tab->is_artificial_var[j]`. An artificial is fixed at zero in any feasible
  LP, so its term is identically zero — dropping it is exact, not merely
  conservative.
- **Result**: all 40 objectives now match GLPK, with c-MIR both off (default)
  and on.
- **Regression test**: `ralph/tests/test_gmi_cut_validity.c`, wired in as
  `make -C ralph test-gmi-cut-validity` and part of `make -C ralph test`. It
  solves the two instances above and asserts the GLPK-verified optima. Against
  the unfixed generator it fails with 154 and 174.

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

### MPS Writer Emits Invalid Fixed-Format MPS

- `ralph_lp_write_mps()` starts `ROWS` entries in column 1 and writes an
  `OBJSENSE` section. GLPK 5.0 rejects both, in fixed and free format alike
  (`invalid indicator record`), so written models cannot be handed to it
  without post-processing. Found while cross-checking MIP objectives against
  GLPK; not yet fixed.

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

## MIP: repeated capacitated-facility-location solves crash on Windows — FIXED

- **Severity**: was High. A wrong optimal objective was returned before the crash.
- **Status**: Fixed. Root cause was undefined pointer arithmetic in
  `bb_node_pool_return()` (`branch_bound.c`).

Solving many capacitated facility location (CFL) instances in one process
corrupted memory on Windows. The crash landed in `bb_node_pool_copy()` reached
from `compute_branch_children()`, and was preceded by at least one silently
wrong answer.

### Root cause

`bb_node_pool_get()` falls back to a standalone `bb_node_create()` whenever the
tree outgrows the pool. Those standalone nodes come back through
`bb_node_pool_return()`, which decided pool membership like this:

```c
ptrdiff_t offset = node - pool->nodes;
if (offset < 0 || offset >= pool->capacity) { bb_node_free(node); return; }
```

Subtracting two pointers that do not point into the same array is undefined
behaviour. The compiler is entitled to assume the subtraction is well defined —
that `node` really is in `pool->nodes[]` — and therefore that `offset` is
already in range. At `-O3` GCC used that licence and deleted the `offset < 0`
half of the check. Instrumentation caught it directly:

```
[DBG] POOL EXHAUSTED #1 (cap=1024)
[DBG] EXTERNAL RETURN #1 offset=27401
[DBG] EXTERNAL RETURN #2 offset=6148914691236544597
[DBG] OFFSET CHECK MISMATCH truly_in=0 passes=1 offset=-6148914691236489781 cap=1024
[DBG] BAD GET idx=-1431628341 free_count=1 cap=1024
```

A standalone node therefore pushed a garbage index onto `free_list`. A later
`bb_node_pool_get()` popped that index and returned `&pool->nodes[garbage]` —
a wild pointer, dereferenced in `bb_node_pool_copy()`. Where the garbage index
happened to land inside the pool, a live node was handed out a second time,
which is where the wrong objectives came from.

This also explains the properties that made the bug look mysterious:

- **Only CFL.** It is the only one of the three generated classes whose trees
  outgrow the 1024-node pool, so it is the only one that ever creates a
  standalone node. Instrumented, `setcover` reports zero pool exhaustions.
  The equality rows and negative capacity coefficients were a red herring.
- **The sequence matters.** Whether the garbage offset lands in `[0, capacity)`
  depends on where malloc puts the standalone node relative to the pool block,
  which depends on heap history.
- **Sanitizers hide it.** ASan changes allocation layout and adds redzones, so
  the garbage offset falls outside the range and the elided check stops
  mattering. The overflow is also intra-allocation once the bogus index is in
  range, which ASan cannot see at all.

### Fix

`bb_node_pool_return()` now decides membership by comparing addresses as
`uintptr_t`, never by forming a pointer difference, and additionally checks
that the address is correctly aligned within the pool block. The push onto
`free_list` is refused when the list is already full, which closes the
unbounded `pool->free_list[pool->free_count++]` write that was flagged
separately.

`node_queue_free()` was deleted. It called
`node_queue_free_with_pool(queue, NULL)`, and a NULL pool sent every pooled
node to `bb_node_free()`, which would have freed three interior pointers.
Nothing called it.

### Regression test

`ralph/tests/test_bb_node_pool.c`, wired in as `make -C ralph test-bb-node-pool`
and part of `make -C ralph test`. It exercises the standalone-return path
directly and then replays 23 CFL solves in one process. Against the old
`bb_node_pool_return()` the suite segfaults; against the fix it passes.

The standalone reproduction is still available as
`make -C ralph repro-cfl-crash`; `./ralph/repro_cfl_crash cfl 6 40` now
completes 40 of 40.

## Platform-Specific Issues

### macOS

Built and tested on every push by the **macOS Core** job (`macos-latest`,
Apple Silicon): `shared`, `ralph`, `velo`, `carta`, `locus`, `fuelwise`,
`surge` and `nexus`. No known platform-specific issues.

### Linux

The primary CI platform. Every module suite runs here on each push, alongside
the API tests, the sanitiser jobs and the WASM build.

### Windows

A first-class target, built and tested on every push by four jobs.

- **Windows Core** (MinGW/UCRT64): `shared` (including the PAL suite), `velo`,
  `carta`, `locus`, `nexus`, `ralph` (main suite plus detect, lap, netflow and
  netlib), and the FuelWise and Surge transport tests. It also *builds* all
  five API servers -- ralph, fuelwise, surge, velo, carta -- which is what
  proves Keel and the Keel-side shared helpers link here.
- **Windows Suites** (MinGW/UCRT64): the full Surge suite and the FuelWise
  bench regression.
- **Windows Suites MSVC** (`CC=cl`): the same two suites under cl.exe. Both are
  numerical rather than structural, and MSVC's libm and FP codegen are not
  MinGW's, so the pair of jobs is what would catch the two compilers
  disagreeing on an objective.
- **Windows MSVC** (`CC=cl`): twenty targets, the above plus `arbor` and four
  ClayShards ones.

The nightly NETLIB regression gate runs on Linux and on Windows under both
compilers -- `gate-windows` (gcc) and `gate-windows-msvc` (cl). All 84 problems
agree with the shared baseline under MSVC: no status, objective or solution
validity differences.

All six API servers' live-server suites run everywhere the servers are built:
`Windows Core` (MinGW), `Windows MSVC` (cl), `macOS Core` (clang) and the six
Linux `Test * API` jobs. Until recently they ran on no Windows or macOS job
under any compiler -- those jobs built the servers and stopped, so "it links"
was the whole claim. Each suite starts a real server, drives it with curl and
exits non-zero on failure; about 110 assertions in 25 seconds per platform.

Every Windows job duplicates a Monaco fixture step to get there. That looked
like a reason to skip it, but it measures 6.2s end to end -- download and
graph build included -- which is cheaper than any scheme for sharing it.

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
| unexpected timeouts | 0 |

So the solver agrees with GLPK on every problem, and the
per-platform-baseline theory was unnecessary.

### bnl1 did not converge on some CPUs -- FIXED

- **Severity**: was Medium. **Status**: fixed by removing `-ffast-math`.

`bnl1.mps` sometimes never finished. Measured, same source, same flags:

| machine | `-ffast-math` | strict IEEE |
|---|---|---|
| AMD EPYC 9V74 80-core | **never converges** | optimal 264 ms, 439 it |
| AMD EPYC 7763 64-core | **never converges** | optimal 247 ms, 439 it |
| Intel Xeon 6973P-C | optimal 201 ms, 328 it | optimal 202 ms, 439 it |
| Intel Xeon Platinum 8573C | optimal 268 ms, 328 it | optimal 286 ms, 439 it |

Under strace the hang was 44 seconds of pure userspace work with zero
syscalls, and glpsol was never spawned -- the simplex spinning, not I/O, not
the harness, and not the reference solver, which does bnl1 alone in 0.035 s.

The mechanism is the one the bore3d entry above describes. `-ffast-math` let
the compiler use whatever the host CPU offered and reassociate freely; the
resulting floating-point differences changed pivot selection, and a solve near
the edge landed on a different side of it per machine. Note the iteration
counts: under `-ffast-math` the solve took a different path on every CPU,
while strict IEEE gives an identical 439 everywhere.

Fixed by dropping `-ffast-math` and turning off FP contraction across the
build (`mk/toolchain.mk`), which cost nothing: over the 26 problems in
`required_coverage`, total solve time went 651 ms to 635 ms for 2.8% more
iterations and zero status changes.

**Still open, and separate.** `--hard-cap 20` did not stop the runaway solve;
it ran until the external 50-second wrapper killed it. Strict FP means Ralph
no longer takes that path on bnl1, but a time limit that does not fire is a
live defect on whatever path goes bad next.

### The gate runs nightly

`.github/workflows/netlib-nightly.yml` runs the full 84-problem gate at 04:00
UTC on Linux and Windows, and on pull requests that touch the gate script, its
baseline manifest or the workflow itself. Per-push CI still runs `make -C ralph
test-netlib`, which is `--test` mode against a table of reference optimals and
needs no GLPK -- the cheap check that catches the common case.

It is nightly rather than per-push because a full run takes upwards of twenty
minutes and wants an otherwise idle machine: the limit handed to Ralph is
derived from GLPK's measured time on the same problem, so contention inflates
the reference and the budget together.

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
