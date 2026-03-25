/*
 * simplex_internal.h - Internal declarations shared between simplex sub-modules.
 *
 * These functions are defined in simplex.c but needed by extracted sub-modules
 * (simplex_pricing.c, simplex_ratio.c, simplex_perturb.c).
 *
 * Functions already declared in lp.h (tableau_compute_solution,
 * tableau_compute_reduced_costs) are not repeated here.
 */

#ifndef SIMPLEX_INTERNAL_H
#define SIMPLEX_INTERNAL_H

#include "lp.h"

/* Variable eligibility check (accounts for GLPK-compat exclusion rules) */
int simplex_smcp_excl_skip_var(const SimplexTableau *tab, int j);

/* Dual computation */
int tableau_compute_duals(SimplexTableau *tab);

/* Lazy single reduced cost computation */
double tableau_get_rc(SimplexTableau *tab, int j);

/* Phase 1 helpers needed by simplex_phase1_recovery.c */
double phase1_artificial_abs_sum(const SimplexTableau *tab);
void phase1_recompute_full_with_reason(SimplexSolver *solver,
                                       SimplexTableau *tab,
                                       int *rc_only_streak,
                                       LPPhase1RecomputeReason reason);
void phase1_trace_emit_summary(SimplexSolver *solver, RalphStatus phase1_status);

#endif /* SIMPLEX_INTERNAL_H */
