/*
 * tuning_policy.h - instance-shape heuristics.
 *
 * Twenty-eight predicates that decide, from the shape of a problem,
 * which pricing and phase-1 strategy to use. They are hardcoded numeric
 * bands fitted to named NETLIB instances:
 *
 *     return (m >= 520 && m <= 680 && n >= 1000 && n <= 1400 &&
 *             width_ratio >= 1.7 && density >= 0.004 && density <= 0.010);
 *
 * That is a legitimate way to get a solver competitive on a benchmark
 * suite, and it is also a policy that should be visible as one artifact
 * rather than 28 `if` blocks buried in the pricing loop, where nobody can
 * audit it, regression-test it as a unit, or replace it.
 *
 * Moved verbatim out of simplex.c. The signatures are unchanged, so every
 * call site is untouched and the behaviour is identical -- narrowing them
 * to a shape descriptor { m, n, nnz, density, width_ratio, phase } is the
 * obvious next step and a separate, measurable one. They read only three
 * solver fields (model, num_artificial, use_two_phase), so that step is
 * smaller than it looks.
 */
#ifndef TUNING_POLICY_H
#define TUNING_POLICY_H

#include "lp.h"

int simplex_should_use_dense_small_phase1_partial(const SimplexSolver *solver,
                                                          const SimplexTableau *tab);
int simplex_should_use_bound_tightened_midrow_phase1_dantzig(const SimplexSolver *solver,
                                                                     const SimplexTableau *tab);
int simplex_should_use_sparse_midrow_phase1_partial(const SimplexSolver *solver,
                                                            const SimplexTableau *tab);
int simplex_should_use_sparse_bridge_phase1_partial(const SimplexSolver *solver,
                                                            const SimplexTableau *tab);
int simplex_should_use_mid_sparse_phase1_dantzig(const SimplexSolver *solver,
                                                         const SimplexTableau *tab);
int simplex_should_use_scaled_midrow_phase1_partial(const SimplexSolver *solver,
                                                            const SimplexTableau *tab);
int simplex_should_use_narrow_midrow_phase1_dantzig(const SimplexSolver *solver,
                                                            const SimplexTableau *tab);
int simplex_should_use_bandm_phase2_dantzig(const SimplexSolver *solver,
                                                    const SimplexTableau *tab);
int simplex_should_use_very_sparse_large_dantzig(const SimplexSolver *solver,
                                                         const SimplexTableau *tab);
int simplex_should_use_large_sparse_phase1_dantzig(const SimplexSolver *solver,
                                                           const SimplexTableau *tab);
int simplex_should_use_large_moderate_sparse_phase1_dantzig(const SimplexSolver *solver,
                                                                    const SimplexTableau *tab);
int simplex_should_use_mid_sparse_phase12_dantzig(const SimplexSolver *solver,
                                                          const SimplexTableau *tab);
int simplex_should_use_midwide_sparse_phase12_dantzig(const SimplexSolver *solver,
                                                              const SimplexTableau *tab);
int simplex_should_use_sparse_grow_phase12_dantzig(const SimplexSolver *solver,
                                                           const SimplexTableau *tab);
int simplex_should_use_small_grow_phase12_dantzig(const SimplexSolver *solver,
                                                          const SimplexTableau *tab);
int simplex_should_use_eta_sparse_phase12_dantzig(const SimplexSolver *solver,
                                                          const SimplexTableau *tab);
int simplex_should_use_lowrow_wide_scsd_phase12_dantzig(const SimplexSolver *solver,
                                                                const SimplexTableau *tab);
int simplex_should_use_scsd_sparse_phase12_dantzig(const SimplexSolver *solver,
                                                           const SimplexTableau *tab);
int simplex_should_use_wide_sparse_phase1_heap(const SimplexSolver *solver,
                                                       const SimplexTableau *tab);
int simplex_should_use_lowrow_wide_sparse_phase1_heap(const SimplexSolver *solver,
                                                              const SimplexTableau *tab);
int simplex_should_use_wide_sparse_phase1_dantzig_phase2_partial(
     const SimplexSolver *solver,
     const SimplexTableau *tab);
int simplex_should_use_dense_lowrow_phase2_partial(const SimplexSolver *solver,
                                                           const SimplexTableau *tab);
int simplex_should_use_medium_sparse_phase2_partial(const SimplexSolver *solver,
                                                            const SimplexTableau *tab);
int simplex_should_use_compact_sparse_phase2_partial(const SimplexSolver *solver,
                                                             const SimplexTableau *tab);
int simplex_should_use_wide_scsd_phase2_partial(const SimplexSolver *solver,
                                                        const SimplexTableau *tab);
int simplex_should_use_sparse_fit_phase2_steepest(const SimplexSolver *solver,
                                                          const SimplexTableau *tab);
int simplex_should_use_fit2p_phase2_heap(const SimplexSolver *solver,
                                                 const SimplexTableau *tab);
int simplex_should_use_sparse_fit_phase1_partial(const SimplexSolver *solver,
                                                         const SimplexTableau *tab);

/* From ralph.c. Seven more of the same kind, on the presolve and dual-simplex
 * paths rather than pricing: each is a hardcoded band on the problem shape that
 * turns one strategy on or off. They read exactly two fields, num_cons and
 * num_vars, so they are pure functions of (m, n) and fold into the same shape
 * descriptor the predicates above want. Signatures unchanged; the seven call
 * sites in ralph.c are untouched. */
int ralph_should_skip_sparse_mid_presolve(const LPModel *model);
int ralph_should_skip_dense_compact_bound_tightening(const LPModel *model);
int ralph_should_use_fixed_bound_tightening_presolve(const LPModel *model);
int ralph_should_control_mid_sparse_reinvert(const LPModel *model);
int ralph_should_disable_dual_dse_wide_ship(const LPModel *model);
int ralph_should_use_relaxed_dual_rc_cadence_compact_sparse(const LPModel *model);
int ralph_should_use_shift_off_dual_start(const LPModel *model);

#endif /* TUNING_POLICY_H */
