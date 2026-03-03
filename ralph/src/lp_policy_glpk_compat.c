#include "lp_policy_glpk_compat.h"
#include "lp.h"
#include <math.h>

static int lp_policy_profile_valid(int value) {
    return value >= LP_POLICY_PROFILE_DEFAULT &&
           value <= LP_POLICY_PROFILE_GLPK_LEGACY;
}

static int lp_glpk_smcp_method_valid(int value) {
    return value >= LP_GLPK_SMCP_METHOD_AUTO &&
           value <= LP_GLPK_SMCP_METHOD_DUAL;
}

static int lp_glpk_smcp_pricing_valid(int value) {
    return value >= LP_GLPK_SMCP_PRICING_STANDARD &&
           value <= LP_GLPK_SMCP_PRICING_STEEP;
}

static int lp_glpk_smcp_ratio_valid(int value) {
    return value >= LP_GLPK_SMCP_RATIO_STANDARD &&
           value <= LP_GLPK_SMCP_RATIO_HARRIS;
}

static int lp_glpk_smcp_flip_valid(int value) {
    return value >= LP_GLPK_SMCP_FLIP_OFF &&
           value <= LP_GLPK_SMCP_FLIP_ON;
}

static int lp_glpk_smcp_basis_valid(int value) {
    return value >= LP_GLPK_SMCP_BASIS_ADV &&
           value <= LP_GLPK_SMCP_BASIS_STD;
}

static int lp_glpk_smcp_presolve_valid(int value) {
    return value >= LP_GLPK_SMCP_PRESOLVE_AUTO &&
           value <= LP_GLPK_SMCP_PRESOLVE_ON;
}

static int lp_glpk_smcp_excl_valid(int value) {
    return value >= LP_GLPK_SMCP_EXCL_OFF &&
           value <= LP_GLPK_SMCP_EXCL_ON;
}

static int lp_glpk_smcp_shift_valid(int value) {
    return value >= LP_GLPK_SMCP_SHIFT_OFF &&
           value <= LP_GLPK_SMCP_SHIFT_ON;
}

static int lp_glpk_smcp_aorn_valid(int value) {
    return value >= LP_GLPK_SMCP_AORN_USE_AT &&
           value <= LP_GLPK_SMCP_AORN_USE_NT;
}

static int lp_glpk_bfcp_backend_valid(int value) {
    return value >= LP_GLPK_BFCP_BACKEND_LUF_FT &&
           value <= LP_GLPK_BFCP_BACKEND_CGR;
}

static double lp_glpk_working_fixed_width_tol(int smcp_shift, double tol_bnd) {
    const double strict_tol = 1e-12;
    const double shifted_tol_default = 1e-7;
    const double shifted_tol_cap = 1e-7;
    if (smcp_shift != 0) {
        if (isfinite(tol_bnd) && tol_bnd > 0.0) {
            return (tol_bnd < shifted_tol_cap) ? tol_bnd : shifted_tol_cap;
        }
        return shifted_tol_default;
    }
    if (isfinite(tol_bnd) && tol_bnd > 0.0 && tol_bnd < strict_tol) {
        return tol_bnd;
    }
    return strict_tol;
}

int lp_policy_glpk_working_exclude_nonbasic(int smcp_excl,
                                            int smcp_shift,
                                            int var_status,
                                            double lb,
                                            double ub,
                                            double tol_bnd) {
    double fixed_tol = lp_glpk_working_fixed_width_tol(smcp_shift, tol_bnd);
    if (var_status == (int)RALPH_FIXED) return 1;
    if (smcp_excl == LP_GLPK_SMCP_EXCL_OFF) return 0;
    if (var_status != (int)RALPH_NONBASIC_LOWER &&
        var_status != (int)RALPH_NONBASIC_UPPER) {
        return 0;
    }
    if (lb <= -RALPH_INFINITY / 2.0 || ub >= RALPH_INFINITY / 2.0) {
        return 0;
    }
    return fabs(ub - lb) <= fixed_tol;
}

int lp_policy_glpk_working_use_at_kernel(int smcp_aorn, int has_row_scatter) {
    if (smcp_aorn != LP_GLPK_SMCP_AORN_USE_AT) return 0;
    return has_row_scatter ? 1 : 0;
}

int lp_policy_glpk_perturb_next_state(int state, int event, int *next_state_out) {
    int next_state = state;
    int valid = 1;

    switch ((LPGLPKPerturbState)state) {
        case LP_GLPK_PERTURB_STATE_OFF:
            switch ((LPGLPKPerturbEvent)event) {
                case LP_GLPK_PERTURB_EVENT_ENABLE:
                case LP_GLPK_PERTURB_EVENT_REAPPLY:
                    next_state = LP_GLPK_PERTURB_STATE_ACTIVE;
                    break;
                case LP_GLPK_PERTURB_EVENT_DISABLE:
                    next_state = LP_GLPK_PERTURB_STATE_OFF;
                    break;
                case LP_GLPK_PERTURB_EVENT_BEGIN_CLEANUP:
                    valid = 0;
                    break;
                default:
                    valid = 0;
                    break;
            }
            break;
        case LP_GLPK_PERTURB_STATE_ACTIVE:
            switch ((LPGLPKPerturbEvent)event) {
                case LP_GLPK_PERTURB_EVENT_ENABLE:
                case LP_GLPK_PERTURB_EVENT_REAPPLY:
                    next_state = LP_GLPK_PERTURB_STATE_ACTIVE;
                    break;
                case LP_GLPK_PERTURB_EVENT_BEGIN_CLEANUP:
                    next_state = LP_GLPK_PERTURB_STATE_CLEANUP;
                    break;
                case LP_GLPK_PERTURB_EVENT_DISABLE:
                    next_state = LP_GLPK_PERTURB_STATE_OFF;
                    break;
                default:
                    valid = 0;
                    break;
            }
            break;
        case LP_GLPK_PERTURB_STATE_CLEANUP:
            switch ((LPGLPKPerturbEvent)event) {
                case LP_GLPK_PERTURB_EVENT_DISABLE:
                    next_state = LP_GLPK_PERTURB_STATE_OFF;
                    break;
                case LP_GLPK_PERTURB_EVENT_ENABLE:
                case LP_GLPK_PERTURB_EVENT_REAPPLY:
                    next_state = LP_GLPK_PERTURB_STATE_ACTIVE;
                    break;
                case LP_GLPK_PERTURB_EVENT_BEGIN_CLEANUP:
                    next_state = LP_GLPK_PERTURB_STATE_CLEANUP;
                    break;
                default:
                    valid = 0;
                    break;
            }
            break;
        default:
            valid = 0;
            break;
    }

    if (next_state_out) {
        *next_state_out = valid ? next_state : state;
    }
    return valid;
}

void lp_policy_glpk_compat_init(LPGLPKCompatConfig *cfg) {
    if (!cfg) return;
    cfg->lp_policy_profile = LP_POLICY_PROFILE_DEFAULT;
    cfg->glpk_smcp_method = LP_GLPK_SMCP_METHOD_AUTO;
    cfg->glpk_smcp_pricing = LP_GLPK_SMCP_PRICING_STEEP;
    cfg->glpk_smcp_ratio = LP_GLPK_SMCP_RATIO_HARRIS;
    cfg->glpk_smcp_flip = LP_GLPK_SMCP_FLIP_OFF;
    cfg->glpk_smcp_basis = LP_GLPK_SMCP_BASIS_ADV;
    cfg->glpk_smcp_presolve = LP_GLPK_SMCP_PRESOLVE_AUTO;
    cfg->glpk_smcp_tol_bnd = 1e-7;
    cfg->glpk_smcp_tol_dj = 1e-7;
    cfg->glpk_smcp_tol_piv = 1e-9;
    cfg->glpk_smcp_excl = LP_GLPK_SMCP_EXCL_ON;
    cfg->glpk_smcp_shift = LP_GLPK_SMCP_SHIFT_ON;
    cfg->glpk_smcp_aorn = LP_GLPK_SMCP_AORN_USE_NT;
    cfg->glpk_bfcp_backend = LP_GLPK_BFCP_BACKEND_LUF_FT;
    cfg->glpk_bfcp_update_limit = -1;
    cfg->glpk_bfcp_pivot_tol = 0.0;
    cfg->glpk_bfcp_growth_guard = 0.0;
}

int lp_policy_glpk_compat_validate(const LPGLPKCompatConfig *cfg) {
    if (!cfg) return 0;
    if (!lp_policy_profile_valid(cfg->lp_policy_profile)) return 0;
    if (!lp_glpk_smcp_method_valid(cfg->glpk_smcp_method)) return 0;
    if (!lp_glpk_smcp_pricing_valid(cfg->glpk_smcp_pricing)) return 0;
    if (!lp_glpk_smcp_ratio_valid(cfg->glpk_smcp_ratio)) return 0;
    if (!lp_glpk_smcp_flip_valid(cfg->glpk_smcp_flip)) return 0;
    if (!lp_glpk_smcp_basis_valid(cfg->glpk_smcp_basis)) return 0;
    if (!lp_glpk_smcp_presolve_valid(cfg->glpk_smcp_presolve)) return 0;
    if (!lp_glpk_smcp_excl_valid(cfg->glpk_smcp_excl)) return 0;
    if (!lp_glpk_smcp_shift_valid(cfg->glpk_smcp_shift)) return 0;
    if (!lp_glpk_smcp_aorn_valid(cfg->glpk_smcp_aorn)) return 0;
    if (!isfinite(cfg->glpk_smcp_tol_bnd) || cfg->glpk_smcp_tol_bnd <= 0.0) return 0;
    if (!isfinite(cfg->glpk_smcp_tol_dj) || cfg->glpk_smcp_tol_dj <= 0.0) return 0;
    if (!isfinite(cfg->glpk_smcp_tol_piv) || cfg->glpk_smcp_tol_piv <= 0.0) return 0;
    if (!lp_glpk_bfcp_backend_valid(cfg->glpk_bfcp_backend)) return 0;
    if (cfg->glpk_bfcp_update_limit < -1) return 0;
    if (!isfinite(cfg->glpk_bfcp_pivot_tol)) return 0;
    if (!isfinite(cfg->glpk_bfcp_growth_guard)) return 0;
    return 1;
}

void lp_policy_glpk_compat_apply_profile_defaults(LPGLPKCompatConfig *cfg) {
    if (!cfg) return;
    if (cfg->lp_policy_profile != LP_POLICY_PROFILE_GLPK_COMPAT &&
        cfg->lp_policy_profile != LP_POLICY_PROFILE_GLPK_STRICT &&
        cfg->lp_policy_profile != LP_POLICY_PROFILE_GLPK_LEGACY) {
        return;
    }

    /* GLPK defaults: primal simplex, steep pricing, Harris ratio, no flip,
     * advanced basis, presolve enabled, LUF+FT backend. */
    cfg->glpk_smcp_method = LP_GLPK_SMCP_METHOD_PRIMAL;
    cfg->glpk_smcp_pricing = LP_GLPK_SMCP_PRICING_STEEP;
    cfg->glpk_smcp_ratio = LP_GLPK_SMCP_RATIO_HARRIS;
    cfg->glpk_smcp_flip = LP_GLPK_SMCP_FLIP_OFF;
    cfg->glpk_smcp_basis = LP_GLPK_SMCP_BASIS_ADV;
    cfg->glpk_smcp_presolve = LP_GLPK_SMCP_PRESOLVE_ON;
    cfg->glpk_smcp_tol_bnd = 1e-7;
    cfg->glpk_smcp_tol_dj = 1e-7;
    cfg->glpk_smcp_tol_piv = 1e-9;
    cfg->glpk_smcp_excl = LP_GLPK_SMCP_EXCL_ON;
    cfg->glpk_smcp_shift = LP_GLPK_SMCP_SHIFT_ON;
    cfg->glpk_smcp_aorn = LP_GLPK_SMCP_AORN_USE_NT;
    cfg->glpk_bfcp_backend = LP_GLPK_BFCP_BACKEND_LUF_FT;
    cfg->glpk_bfcp_update_limit = 100;
    cfg->glpk_bfcp_pivot_tol = 0.0;
    cfg->glpk_bfcp_growth_guard = 0.0;
}

void lp_policy_glpk_compat_apply_runtime(const LPGLPKCompatConfig *cfg,
                                         int *method_io,
                                         int *pricing_io,
                                         int *phase1_pricing_io,
                                         int *presolve_io,
                                         int *ratio_io,
                                         int *dual_ratio_io,
                                         int *dual_bound_flip_io,
                                         double *smcp_tol_bnd_io,
                                         double *smcp_tol_dj_io,
                                         double *smcp_tol_piv_io,
                                         int *smcp_excl_io,
                                         int *smcp_shift_io,
                                         int *smcp_aorn_io,
                                         int *crash_io,
                                         int *bfcp_backend_io,
                                         int *bfcp_backend_supported_io,
                                         int *bfcp_update_limit_io,
                                         double *bfcp_pivot_tol_io,
                                         double *bfcp_growth_guard_io,
                                         int *dual_refactor_base_interval_io,
                                         int *dual_rc_recompute_interval_io,
                                         int *soft_lu_cost_gate_enabled_io,
                                         int *periodic_cost_gate_enabled_io) {
    int active_profile = 0;
    int strict_profile = 0;
    int dual_refactor_base_interval = 50;
    int dual_rc_recompute_interval = 20;

    if (!cfg) return;
    active_profile = (cfg->lp_policy_profile == LP_POLICY_PROFILE_GLPK_COMPAT ||
                      cfg->lp_policy_profile == LP_POLICY_PROFILE_GLPK_STRICT ||
                      cfg->lp_policy_profile == LP_POLICY_PROFILE_GLPK_LEGACY);
    strict_profile = (cfg->lp_policy_profile == LP_POLICY_PROFILE_GLPK_COMPAT ||
                      cfg->lp_policy_profile == LP_POLICY_PROFILE_GLPK_STRICT);
    if (!active_profile) {
        return;
    }

    if (cfg->glpk_smcp_method == LP_GLPK_SMCP_METHOD_PRIMAL) {
        if (method_io) *method_io = 0;
    } else if (cfg->glpk_smcp_method == LP_GLPK_SMCP_METHOD_DUAL) {
        if (method_io) *method_io = 1;
    }

    if (pricing_io) {
        if (cfg->glpk_smcp_pricing == LP_GLPK_SMCP_PRICING_STEEP) {
            *pricing_io = 1; /* Steepest edge */
        } else {
            *pricing_io = 0; /* Standard pricing (Dantzig) */
        }
    }

    if (phase1_pricing_io) {
        if (cfg->glpk_smcp_pricing == LP_GLPK_SMCP_PRICING_STEEP) {
            *phase1_pricing_io = 1;
        } else {
            *phase1_pricing_io = 0;
        }
    }

    if (presolve_io) {
        if (cfg->glpk_smcp_presolve == LP_GLPK_SMCP_PRESOLVE_ON) {
            *presolve_io = 1;
        } else if (cfg->glpk_smcp_presolve == LP_GLPK_SMCP_PRESOLVE_OFF) {
            *presolve_io = -1;
        }
    }

    if (ratio_io) {
        *ratio_io = (cfg->glpk_smcp_ratio == LP_GLPK_SMCP_RATIO_HARRIS) ? 1 : 0;
    }

    if (dual_ratio_io) {
        if (cfg->glpk_smcp_flip == LP_GLPK_SMCP_FLIP_ON) {
            *dual_ratio_io = LP_DUAL_RATIO_TEST_FLIP;
        } else {
            *dual_ratio_io = (cfg->glpk_smcp_ratio == LP_GLPK_SMCP_RATIO_HARRIS)
                                 ? LP_DUAL_RATIO_TEST_HARRIS
                                 : LP_DUAL_RATIO_TEST_STANDARD;
        }
    }

    /* GLPK compat/strict profiles map flip directly to startup/iterative
     * bound-flip enable. Legacy profile keeps historical behavior. */
    if (dual_bound_flip_io && strict_profile) {
        *dual_bound_flip_io = (cfg->glpk_smcp_flip == LP_GLPK_SMCP_FLIP_ON) ? 1 : 0;
    }
    if (smcp_tol_bnd_io) *smcp_tol_bnd_io = cfg->glpk_smcp_tol_bnd;
    if (smcp_tol_dj_io) *smcp_tol_dj_io = cfg->glpk_smcp_tol_dj;
    if (smcp_tol_piv_io) *smcp_tol_piv_io = cfg->glpk_smcp_tol_piv;
    if (smcp_excl_io) *smcp_excl_io = cfg->glpk_smcp_excl;
    if (smcp_shift_io) *smcp_shift_io = cfg->glpk_smcp_shift;
    if (smcp_aorn_io) *smcp_aorn_io = cfg->glpk_smcp_aorn;

    if (crash_io) {
        *crash_io = (cfg->glpk_smcp_basis == LP_GLPK_SMCP_BASIS_ADV) ? 1 : 0;
    }

    if (cfg->glpk_bfcp_update_limit > 0) {
        dual_refactor_base_interval = cfg->glpk_bfcp_update_limit / 2;
    }
    if (dual_refactor_base_interval < 8) dual_refactor_base_interval = 8;
    if (dual_refactor_base_interval > 128) dual_refactor_base_interval = 128;
    dual_rc_recompute_interval = dual_refactor_base_interval / 2;
    if (dual_rc_recompute_interval < 10) dual_rc_recompute_interval = 10;
    if (dual_rc_recompute_interval > 64) dual_rc_recompute_interval = 64;

    if (bfcp_backend_io) {
        /* Until true BG/GR implementations are added, keep runtime backend on
         * LUF+FT to avoid misleading pseudo-mapping in simplex configuration. */
        *bfcp_backend_io = LP_GLPK_BFCP_BACKEND_LUF_FT;
    }
    if (bfcp_backend_supported_io) {
        *bfcp_backend_supported_io =
            (cfg->glpk_bfcp_backend == LP_GLPK_BFCP_BACKEND_LUF_FT) ? 1 : 0;
    }
    if (bfcp_update_limit_io) {
        *bfcp_update_limit_io = cfg->glpk_bfcp_update_limit;
    }
    if (bfcp_pivot_tol_io) {
        *bfcp_pivot_tol_io = cfg->glpk_bfcp_pivot_tol;
    }
    if (bfcp_growth_guard_io) {
        *bfcp_growth_guard_io = cfg->glpk_bfcp_growth_guard;
    }
    if (dual_refactor_base_interval_io) {
        *dual_refactor_base_interval_io = dual_refactor_base_interval;
    }
    if (dual_rc_recompute_interval_io) {
        *dual_rc_recompute_interval_io = dual_rc_recompute_interval;
    }
    if (soft_lu_cost_gate_enabled_io) {
        *soft_lu_cost_gate_enabled_io = 0;
    }
    if (periodic_cost_gate_enabled_io) {
        *periodic_cost_gate_enabled_io = 0;
    }
}
