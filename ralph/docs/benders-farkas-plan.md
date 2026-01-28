# Benders feasibility cuts with Farkas ray

This plan describes how to integrate Benders feasibility cuts using Ralph's Farkas ray API for the FuelWise refuel MILP split.

## Subproblem formulation (explicit constraints)

Build the LP subproblem with fixed z values from the master. Use explicit constraints for all z-dependent bounds so duals map to cut coefficients.

Variables (same as MILP):
- x[i]: fuel purchased at station i (continuous)
- y[i]: cumulative fuel before arriving at station i (continuous)

Constraints (index in the exact order added; store rhs[k]):
1. Fuel balance (each i)
   y[i] - sum_{j<i} x[j] = current_fuel
2. Minimum fuel at arrival (each i)
   y[i] >= minimum_fuel + consumed(0, dist_i)
3. Tank capacity after refuel (each i)
   y[i] + x[i] <= tank_capacity + consumed(0, dist_i)
4. Link purchase to stop (upper) (each i)
   x[i] - tank_capacity * z[i] <= 0
5. Minimum purchase (lower) (each i, if min_purchase > 0)
   x[i] - min_purchase * z[i] >= 0
6. Reach destination
   sum_i x[i] >= total_consumed + min_end - current_fuel

Objective (subproblem):
- sum_i x[i] * (price_i - remaining_fuel_value)

## Farkas ray handling

If subproblem is infeasible:
- call ralph_get_farkas_ray(model, ray) where ray length = num_constraints
- ray is aligned to the constraints in the order added

## Feasibility cut derivation

We need y' * (b + D z) <= 0 for feasibility. When infeasible, add a cut:
- sum_i (cut_coeff[i] * z[i]) <= cut_rhs

Only constraints 4 and 5 depend on z, so only those contribute to D.

For each station i:
- Constraint 4 dual y_upper[i] gives contribution: -tank_capacity * y_upper[i]
- Constraint 5 dual y_lower[i] gives contribution: +min_purchase * y_lower[i]

So:
- cut_coeff[i] = (-tank_capacity * y_upper[i]) + (min_purchase * y_lower[i])
- cut_rhs = - sum_k (ray[k] * rhs[k])

Add to master:
- sum_i cut_coeff[i] * z[i] <= cut_rhs

## Required bookkeeping

While building the subproblem, store:
- upper_bound_con_idx[i] for constraint 4
- lower_bound_con_idx[i] for constraint 5 (if present)
- rhs[k] for every constraint index

These arrays let you compute cut_coeff and cut_rhs from the ray quickly.

## Robustness

- If ralph_get_farkas_ray fails, add a no-good cut to exclude the current z:
  sum_i (z[i] if z[i]=1 else (1-z[i])) <= k-1
- If cut_rhs is slightly positive due to tolerance, relax with a small epsilon.
- Remaining fuel credit affects objective only; it does not change feasibility cuts.
