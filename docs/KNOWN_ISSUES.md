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

### c-MIR Back-Substitution Sign Errors (cuts.c:cmir_build_cut)
- **Severity**: High — produces trivially infeasible cuts on pure binary/integer problems
- **Trigger**: Binary knapsack or all-integer MIP problems where c-MIR generates cuts
- **Root cause**: `cmir_build_cut()` had two sign errors in the back-substitution logic that converts from the complemented/bound-substituted space back to original variables. The complementation constant and upper-bound substitution constant were added instead of subtracted. Additionally, there may be a remaining issue producing cuts with impossibly negative RHS values on all-integer problems (partially fixed).
- **Partial fix applied**: Two sign errors corrected in back-substitution. Safety check added to reject trivially infeasible cuts (where minimum possible LHS exceeds RHS given variable bounds).
- **Impact**: Without the safety check, the MIP solver reports `RALPH_STATUS_INFEASIBLE` on valid problems. With the safety check, invalid cuts are silently rejected and the solver finds the correct optimal.
- **Reproducer**: 10-variable binary knapsack with 3 constraints and `max_cut_rounds=5` (see `test_cmir_binary_knapsack_regression` in test_main.c).
- **Remaining work**: Find root cause of why c-MIR produces negative RHS with all-positive coefficients on pure binary problems. The safety check masks this but doesn't fix it.

### No Recovery When LP Becomes Infeasible After Cuts (mip.c:1146-1154)
- **Severity**: Medium — compounds the above bug
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
