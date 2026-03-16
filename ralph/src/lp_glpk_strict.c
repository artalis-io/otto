#include "lp_glpk_strict.h"

int lp_glpk_strict_mode_enabled(int glpk_strict_mode) {
    return glpk_strict_mode ? 1 : 0;
}

int lp_glpk_strict_allow_phase1_stagnation_escape(int glpk_strict_mode) {
    return !lp_glpk_strict_mode_enabled(glpk_strict_mode);
}

int lp_glpk_strict_allow_phase1_no_pivot_force(int glpk_strict_mode) {
    return !lp_glpk_strict_mode_enabled(glpk_strict_mode);
}

int lp_glpk_strict_allow_phase1_no_pivot_ladder(int glpk_strict_mode) {
    return !lp_glpk_strict_mode_enabled(glpk_strict_mode);
}

int lp_glpk_strict_allow_phase1_dual_rescue(int glpk_strict_mode) {
    return !lp_glpk_strict_mode_enabled(glpk_strict_mode);
}

int lp_glpk_strict_allow_phase1_dir_stabilize_force(int glpk_strict_mode) {
    return !lp_glpk_strict_mode_enabled(glpk_strict_mode);
}

int lp_glpk_strict_allow_phase1_force_pivot_mode(int glpk_strict_mode) {
    return !lp_glpk_strict_mode_enabled(glpk_strict_mode);
}

int lp_glpk_strict_allow_dual_startup_bound_flip(int glpk_strict_mode) {
    return !lp_glpk_strict_mode_enabled(glpk_strict_mode);
}

int lp_glpk_strict_allow_dual_one_shot_recovery(int glpk_strict_mode) {
    return !lp_glpk_strict_mode_enabled(glpk_strict_mode);
}

int lp_glpk_strict_use_dual_adaptive_ratio_thresholds(int glpk_strict_mode) {
    return !lp_glpk_strict_mode_enabled(glpk_strict_mode);
}

int lp_glpk_strict_allow_bfcp_adaptive_reasons(int glpk_strict_mode) {
    return !lp_glpk_strict_mode_enabled(glpk_strict_mode);
}

int lp_glpk_strict_allow_lu_update_adaptive_thresholds(int glpk_strict_mode) {
    return !lp_glpk_strict_mode_enabled(glpk_strict_mode);
}

int lp_glpk_strict_allow_lu_sparse_skip_heuristics(int glpk_strict_mode) {
    return !lp_glpk_strict_mode_enabled(glpk_strict_mode);
}
