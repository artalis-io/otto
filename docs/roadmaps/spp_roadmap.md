# SPP Roadmap

## Purpose

Add a set-partitioning-specific acceleration path for Ralph that remains
orthogonal to the generic MIP stack.

The generic branch-and-bound engine in `ralph/src/mip.c` remains the proof
engine for optimality and infeasibility. The new SPP path only contributes:
- incumbent heuristics
- valid cutting planes

## Constraints

- Specialized logic must activate only for exact-cover set partitioning models.
- The SPP modules must be separately testable without running full MIP solve.
- Integration points into generic MIP must stay thin and root-focused first.
- Objective/status parity with GLPK on the current benchmark seeds must be
  preserved after each slice.

## Architecture

### 1. SPP Context

Add a reusable exact-cover context:
- `ralph/include/spp.h`
- `ralph/src/optim/spp_context.c`

Context responsibilities:
- validate exact-cover partitioning structure
- own row-to-set incidence
- own set-to-row incidence
- own costs and deterministic ordering
- own conflict graph for exact-cover rows

The context does not own `MIPSolver` or `SimplexSolver` state.

### 2. SPP Primal Heuristic

Add a standalone heuristic module:
- `ralph/src/optim/spp_heuristic.c`

Planned API:
- `spp_heuristic_run(ctx, lp_x, sol_out, obj_out, stats)`
- `spp_solution_check(ctx, sol)`

Planned behavior:
- LP-guided seed
- forced-row / singleton pass
- exact-cover repair
- bounded exchange improvement

### 3. SPP Cut Separator

Add a standalone separator:
- `ralph/src/optim/spp_cuts.c`

Planned API:
- `spp_separate(ctx, lp_x, cut_sink, params, stats)`

Initial cut families:
- clique cuts
- odd-cycle / odd-hole cuts

The separator should accept only context plus LP values and emit cuts. It
should not know about the MIP root loop directly.

### 4. Thin MIP Integration

Integrate only through `ralph/src/mip.c`:
- build/free `SPPContext`
- run SPP heuristic after root LP
- run SPP separator in the root cut loop

No first-pass changes to:
- branching policy
- probing
- node LP solve logic
- generic SCP paths

## Testing

Dedicated tests:
- `ralph/tests/test_spp_context.c`
- `ralph/tests/test_spp_heuristic.c`
- `ralph/tests/test_spp_cuts.c`
- `ralph/tests/test_spp_integration.c`

Required gates:
- every heuristic solution must satisfy exact cover and binary constraints
- every emitted cut must be valid on brute-force tiny instances
- `SetPartitioning 30x10` must remain `OPT 12.40383`
- `SetPartitioning 60x20` must remain `INF`
- quick Ralph vs GLPK MIP comparison must keep objective/status parity

## Implementation Order

1. SPP context module and dedicated test harness
2. SPP primal heuristic module
3. SPP cut separator module
4. Thin root-only MIP integration
5. Benchmark and GLPK parity validation

## Status

- Slice 1 complete: `SPPContext` + `test_spp_context`
- Slice 2 complete: standalone `SPP` primal heuristic + `test_spp_heuristic`
- Slice 3 partially complete: standalone clique separator + `test_spp_cuts`
- Slice 4 partially complete: thin root-only heuristic and separator hooks in `ralph/src/mip.c`
- Generic MIP now always builds `SPPContext` for exact-cover models; this path no longer depends on the broader `detect_special` flag
- Slice 3 pending remainder: odd-cycle / odd-hole separation
- Slice 4 pending remainder: understand why current quick seeds never enter the root cut loop
- Latest benchmark status:
  - `SetPartitioning 30x10`: objective/status parity retained vs GLPK, improved from about `0.0035s` to about `0.0022s` in generic MIP after enabling the standalone SPP path
  - `SetPartitioning 60x20`: infeasibility parity retained vs GLPK, still about `0.012s` and the root cut loop is not exercised on the quick seed
