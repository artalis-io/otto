#ifndef RALPH_LP_GLPK_STRICT_H
#define RALPH_LP_GLPK_STRICT_H

/* Centralized strict-mode gates.
 *
 * In GLPK strict mode, Ralph-local rescue/adaptation heuristics should be
 * bypassed so solver decisions come from the explicit control plane and hard
 * numerical safety only. */

int lp_glpk_strict_mode_enabled(int glpk_strict_mode);

int lp_glpk_strict_allow_phase1_stagnation_escape(int glpk_strict_mode);
int lp_glpk_strict_allow_phase1_no_pivot_force(int glpk_strict_mode);
int lp_glpk_strict_allow_phase1_no_pivot_ladder(int glpk_strict_mode);
int lp_glpk_strict_allow_phase1_dual_rescue(int glpk_strict_mode);
int lp_glpk_strict_allow_phase1_dir_stabilize_force(int glpk_strict_mode);
int lp_glpk_strict_allow_phase1_force_pivot_mode(int glpk_strict_mode);

int lp_glpk_strict_allow_dual_startup_bound_flip(int glpk_strict_mode);
int lp_glpk_strict_allow_dual_one_shot_recovery(int glpk_strict_mode);
int lp_glpk_strict_use_dual_adaptive_ratio_thresholds(int glpk_strict_mode);

int lp_glpk_strict_allow_bfcp_adaptive_reasons(int glpk_strict_mode);
int lp_glpk_strict_allow_lu_update_adaptive_thresholds(int glpk_strict_mode);
int lp_glpk_strict_allow_lu_sparse_skip_heuristics(int glpk_strict_mode);

#endif /* RALPH_LP_GLPK_STRICT_H */
