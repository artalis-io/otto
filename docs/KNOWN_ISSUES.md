# Known Issues and Limitations

## Performance Issues

### LP Solver
- **GLPK comparison**: Ralph is 2-13x slower than GLPK per iteration on larger problems (500+ variables)
- **Dual simplex stability**: Dual simplex can accumulate reduced cost errors, sometimes falling back to primal simplex (see `dual_simplex.c`)

### MIP Solver

Measured against GLPK 5.0 with `bench_mip`, every objective matching:

| problem | Ralph vs GLPK |
|---|---|
| Knapsack (single row, strongly correlated) | GLPK 33x faster |
| MultiKnapsack | GLPK 8-15x faster |
| FacilityLocation | GLPK 5x faster |
| SetPartitioning, SetCovering, LinearAssignment, NetworkFlow | parity |

- **Knapsack was the outlier, and is fixed.** The generator builds strongly
  correlated instances (profit tracks weight) at the classic hard capacity
  ratio. Ralph explored hundreds of thousands of nodes where GLPK needed
  milliseconds. The node count was indeed the whole story -- strong branching
  is under 5% of solve time -- but the cause was not bounding strength, which
  is what this entry used to say. The root LP bound is 0.96% from the optimum
  on the 32-item instance; the bound was never the problem.

  Every objective coefficient in these models is an integer on an integer
  variable, so every feasible objective is an integer, and a node bound can be
  rounded towards the incumbent before it is compared. Ralph compared the raw
  bound, so a node at -1028.4 under an incumbent of -1028 survived along with
  its whole subtree. With the rounding: 32 items 14,705 nodes -> 339; 56
  1,123 -> 5; 90 2,493 -> 33; 110 100,000 (the node limit, never proving
  optimality) -> 7. Against GLPK the 32-item case went from 50x slower to
  1.8x, and 110 items from not finishing to 180x faster.

  Guarded by `ralph/tests/test_obj_integrality_prune.c`.
- **Two earlier entries here were stale** and are removed rather than carried
  forward. "Set partitioning: 37s vs 0.0002s on 30-variable problems" and
  "Large facility location: times out at 210 variables" were both written on
  2026-01-20, before the `optim/spp_*` work landed. Re-measured at exactly those
  sizes: set partitioning is at parity, and the 210-variable facility location
  solves in 0.014s with a matching objective.
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

### No Recovery When LP Becomes Infeasible After Cuts -- FIXED

- **Status**: Fixed, and the entry above had gone stale rather than describing
  live behaviour. It was written in `c67e27c0` (2026-02-12); the recovery landed
  in `74bd4386` (2026-02-22), ten days later, and nothing updated the record.
- **What it claimed**: that `mip.c` propagated `solver->lp_solver->status`
  (INFEASIBLE) and returned without checking `solver->has_incumbent`, so a MIP
  holding a valid incumbent from the diving heuristic could still be reported
  infeasible.
- **What the code does now**: the root cut loop treats a non-optimal LP after
  cuts as a reason to *discard the cuts*, not to give up -- it logs
  `LP non-optimal after cuts (status=%d), discarding cuts`, calls
  `mip_recover_root_relaxation()` to rebuild the relaxation from the original
  model, and breaks out of the cut rounds. It never assigns INFEASIBLE there.
  That is exactly the "optionally attempt recovery by removing the last batch
  of cuts" the entry proposed.
- **The incumbent check exists too**: final status resolution prefers OPTIMAL
  whenever `has_incumbent` is set, and only reports INFEASIBLE when the node
  queue is empty *and* no incumbent was ever found. The two remaining sites
  that do propagate an infeasible LP status both run before any heuristic has
  had a chance to produce an incumbent (SPP propagation, and the root LP itself),
  so there is no path that reports INFEASIBLE while holding a solution.
- No regression test was added: the recovery branch needs a model whose LP goes
  infeasible specifically as a result of cut addition, and a test that did not
  actually drive that path would be worse than none.

### GMI Slack Variable Projection
- GMI cuts with significant positive slack coefficients (>0.1) are rejected to prevent cutting off integer feasible points
- This makes GMI cuts mostly ineffective for >= constraints
- See `cuts.c:generate_gmi_cut()` for the rejection logic

### A sanitizer build of libshared.a is silently reused by later builds -- FIXED

`make -C shared test-asan` is `clean test`, so it rebuilds `libshared.a`
with `DEBUG_CFLAGS`: `/Zi /Od`, `-DDEBUG` and the sanitizer. A subsequent
`make -C shared lib` then sees an archive that exists, with no source newer
than it, calls it up to date, and leaves the debug build in place. Anything
linked afterwards gets it.

The symptom was remote from the cause: the three solver-backed API servers
(ralph, surge, fuelwise) answered a solve request with
`{"status":"error","iterations":0}` -- zero iterations on a two-variable
problem -- while velo, locus and carta were unaffected. Nothing was wrong
with the solvers, and nothing was wrong with `/Od`.

Proved by isolation: with the server still built at `/Od` with ASan, but
`libshared.a` rebuilt clean at `/O2`, all three pass. The Windows MSVC ASan
job now does `clean lib` rather than `lib` for exactly this reason.

Same shape as the stale `libkeel.a` in the API Makefiles (fixed separately):
make treats an archive as current because the file is there, having no notion
of the flags it was built with.

**The hazard itself is now closed.** `mk/flagstamp.mk` records the compiler and
flags each module was last built with, and discards objects built with
different ones. It covers every variant, not just the sanitizer: release to
debug, and GCC to MSVC, which previously needed a manual `make clean` that
`CLAUDE.md` told the reader to remember. The explicit `clean` this job briefly
carried has been removed, which is what demonstrates the stamp works.
### Ralph's solver core leaked on every re-solve  (fixed)

`make -C ralph test-asan` builds the core under the sanitizers. The first
reported figure was 754,436 bytes in 327 allocations, attributed to the
lazy-constraint path. That turned out to be one of **five** leaks and not the
largest; the attribution was an artifact of LeakSanitizer aborting at the
first test that leaked.

All five are the same defect: **a pointer field on a long-lived object,
assigned on a path that runs more than once, with the free only in the
destructor.** Found one at a time, because `halt_on_error` stops at the first
finding and each fix let the suite run further:

| # | site | orphaned on |
|---|------|-------------|
| 1 | `ralph.c` — `model->lp_solver`, `model->mip_solver` | every `optimize()` after the first |
| 2 | `simplex.c` — `setup_primal_tableau` | a second `simplex_solve()`, or `prepare` after a solve |
| 3 | `mip.c` — `generate_node_cuts` success path | every node that applied a cut |
| 4 | `simplex_scaling.c` — `apply_scaling` | every Benders subproblem re-solve |
| 5 | `benders.c` — `ctx->num_cuts = 0` | every cut, in every Benders round |

Only #1 was specific to anything resembling the lazy-constraint path, and even
that one is really the ordinary re-solve shape: any second `optimize()` on a
model leaked. #3 and #5 scale with how *well* the solver works -- more cuts
means more leak.

A sixth was in a test rather than the library: the lifted-cover validity
harness leaked its LP point once per instance, 2.6 MB over 50,000 instances.

One UB finding surfaced on the way, unrelated to the leaks and older than all
of them: `tableau_clone_with_augmented_rows` passed a null `artificial_vars`
to `memcpy` with a zero count, which is undefined. The first sanitizer run
aborted there before leak detection ran at all.

`ralph_core_add_lazy_constraint` also claimed it kept the LP solver "for
potential warm start". Nothing read it -- the next solve called
`simplex_create` unconditionally -- so the retention bought no warm start and
guaranteed leak #1. Comment corrected.

None of this was visible on Windows: LeakSanitizer is Linux-only.

**The `Sanitize Ralph Core` job is wired up and green**, which was the stated
condition for closing this.

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

### MPS Writer Emits Invalid Fixed-Format MPS -- FIXED

- **Status**: Fixed. `ralph_lp_write_mps()` wrote every data record starting in
  column 1, where MPS reserves space for section indicators, and emitted an
  `OBJSENSE` section. GLPK 5.0 rejected the file on the second line
  (`invalid indicator record`) before reading any data, so written models could
  not be handed to it without post-processing.

  Three things were wrong, and only the first two were in the original report:

  1. Data records began in column 1. They now sit at the fixed-format field
     columns (field 1 at 2-3, field 2 at 5-12, field 3 at 15-22, field 4 at
     25-36), which is simultaneously valid free-format.
  2. `OBJSENSE` is a CPLEX extension GLPK rejects in every spelling tried --
     as a section, with the value on the following line, and inline. It is now
     written only for a MAX model, where the alternative is silently turning
     the model into its own opposite; a MIN model, the default, comes out as
     plain MPS.
  3. Not in the report and found while verifying: `%.17g` runs to 24
     characters, overflowing field 4 into columns 37-39, which glpsol rejects
     with `positions 37-39 must be blank`. Values are now written at the
     shortest precision that reads back bit-identical, which is a few
     characters for ordinary coefficients and never rounds one away.

  Verified by reading each NETLIB problem with Ralph, writing it back out, and
  having glpsol solve both: **82 of 84 are accepted in strict fixed format**
  and agree with the original objective to every digit printed. The two
  exceptions are separate defects, below.

  `test_write_mps_format` and `test_write_mps_max_sense` in `ralph/tests/test_lp.c`
  assert the format properties directly, so the guarantee does not depend on
  having glpsol installed. Both were confirmed to fail against the old writer.

### MPS Writer Collides Names Containing Spaces -- FIXED

- **Status**: Fixed. Fixed-format MPS delimits fields by column, so a name may
  contain spaces, and real models use that: `forplan.mps` has columns named
  `M012T1 #`, `M012T1 )`, `M012T1 +` and four more. The writer replaced every
  character outside `[A-Za-z0-9_.$]` with `_`, so all seven became `M012T1__`
  and their coefficients merged -- forplan's 421 distinct column names came out
  as 408, across 3 colliding groups. glpsol refused the result
  (`duplicate coefficient in row 'OBJ'`), which made it loud, but Ralph's own
  reader would have accepted the merged model as if it were the real one.

  Names are now allocated as a set rather than sanitised one at a time, against
  an open-addressed table of everything already issued. Two details matter:

  - Uniqueness is decided against **every** name handed out so far, not within
    a group of identical ones. A disambiguated `M012T1_1` can collide with a
    source column that was genuinely called that, and resolving each group on
    its own regenerates the same string indefinitely.
  - The suffix **replaces the tail** rather than extending it. Field 2 is 8
    columns wide, so appending to an already-8-character name just moves the
    complaint to `positions 13-14 must be blank`. Names that were longer than
    the field to begin with are widened freely, since such a model was never
    fixed-format representable anyway.

  forplan now reads back as 163 rows and **421 columns**, matching its source
  name count exactly.

### MPS Writer Cannot Express Some Values in Fixed Format -- FIXED

- **Status**: Fixed, by removing the cause rather than the symptom.

  `nesm.mps` produced a right-hand side of `25.699996999999996`, which needs 18
  characters to read back as the same double. Field 4 of a fixed-format record
  is 12 columns, and **no shorter decimal maps to that double** -- checked, not
  assumed -- so the file could not be made valid by any change to how the value
  was printed.

  The value was never in nesm. `mps_reader.c` expands a ranged row into two
  constraints, `Ax >= b - |r|` and `Ax <= b`, because the model carries no range
  concept; nesm's `AP1P21` has `b = 58.799988` and `r = 33.099991`, and
  `58.799988 - 33.099991` is exactly `25.699996999999996`. Writing the expansion
  back out is correct but lossy: nesm became 751 rows instead of 663, and the
  computed difference was unrepresentable.

  The writer now folds that pair back into one row plus a `RANGES` entry. Two
  properties make it safe:

  - **Merge only when it round-trips exactly.** The pair is recognised by
    adjacency, opposite senses and an identical coefficient vector, and merged
    only if `hi - |hi - lo|` reproduces `lo` bit-for-bit. Floating point does
    not guarantee that in general, and a merge that shifted a constraint by an
    ulp would be a worse defect than a long line. Where it fails, both rows are
    written as before.
  - **The range is searched, not computed.** Only `hi - |r| == lo` has to hold,
    and many decimals satisfy it. The raw difference can itself need 18
    characters (nesm has ranges that come out as `50.800003000000004`) where
    the shortest qualifying decimal is the source's own `50.800003`.

  nesm now writes 663 rows -- matching its source exactly -- and the whole
  NETLIB set round-trips: **84 of 84 accepted in strict fixed format, 84 of 84
  objectives identical**. `test_write_mps_ranges` pins the behaviour and was
  checked against the previous writer, where it fails.

### Return Value Confusion
- `ralph_core_optimize()` returns 0 for success, -1 for error (not the solve status)
- Use `ralph_core_get_status()` to get the actual solve status
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
- Ralph version (`ralph_core_version()`)
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
