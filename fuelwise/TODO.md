# FuelWise TODO

## Benders Decomposition Solver

**Status**: Currently uses simplified enumeration, not true Benders decomposition.

### Current Implementation

The `fw_solve_refuel_benders()` function currently uses brute-force enumeration of all 2^k stop combinations (for k stations) rather than true Benders decomposition with optimality/feasibility cuts. This works correctly for small problems (k ≤ 20) but is inefficient.

**Limitations**:
- Exponential complexity O(2^k) instead of polynomial
- Falls back to MILP for k > 20 stations
- Does not leverage the decomposition structure

### Proper Benders Implementation Needed

A proper implementation would:

1. **Master Problem**: Binary MIP with z[i] variables (stop decisions) and theta (cost estimate)
   ```
   minimize: stop_cost * sum(z) + theta
   subject to: optimality cuts
               feasibility cuts
   ```

2. **Subproblem**: LP for fuel purchases given fixed z
   ```
   minimize: sum(price[i] * x[i])
   subject to: fuel balance, tank capacity, minimum fuel constraints
   ```

3. **Cut Generation**:
   - **Optimality cuts**: `theta >= sub_obj + dual' * (z - z_fixed)` using subproblem duals
   - **Feasibility cuts**: `0 >= ray' * (h - T*z)` using Farkas rays from infeasible subproblems

### Dependencies

Requires Ralph solver enhancements:
- Reliable dual solution extraction from LP subproblems
- Working Farkas ray extraction for infeasibility certificates
- Stable MIP solver for master problem with accumulated cuts

See `ralph/docs/TODO_FEATURES.md` Section 4 (Benders Decomposition) for Ralph-side requirements.

### References

- Benders (1962): "Partitioning procedures for solving mixed-variables programming problems"
- Magnanti & Wong (1981): "Accelerating Benders decomposition"
