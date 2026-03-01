#include "lp_policy_glpk_compat.h"
#include <math.h>

static int lp_policy_profile_valid(int value) {
    return value >= LP_POLICY_PROFILE_DEFAULT &&
           value <= LP_POLICY_PROFILE_GLPK_COMPAT;
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

static int lp_glpk_bfcp_backend_valid(int value) {
    return value >= LP_GLPK_BFCP_BACKEND_LUF_FT &&
           value <= LP_GLPK_BFCP_BACKEND_CGR;
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
    if (!lp_glpk_bfcp_backend_valid(cfg->glpk_bfcp_backend)) return 0;
    if (cfg->glpk_bfcp_update_limit < -1) return 0;
    if (!isfinite(cfg->glpk_bfcp_pivot_tol)) return 0;
    if (!isfinite(cfg->glpk_bfcp_growth_guard)) return 0;
    return 1;
}

void lp_policy_glpk_compat_apply_profile_defaults(LPGLPKCompatConfig *cfg) {
    if (!cfg) return;
    if (cfg->lp_policy_profile != LP_POLICY_PROFILE_GLPK_COMPAT) return;

    /* GLPK defaults: primal simplex, steep pricing, Harris ratio, no flip,
     * advanced basis, presolve enabled, LUF+FT backend. */
    cfg->glpk_smcp_method = LP_GLPK_SMCP_METHOD_PRIMAL;
    cfg->glpk_smcp_pricing = LP_GLPK_SMCP_PRICING_STEEP;
    cfg->glpk_smcp_ratio = LP_GLPK_SMCP_RATIO_HARRIS;
    cfg->glpk_smcp_flip = LP_GLPK_SMCP_FLIP_OFF;
    cfg->glpk_smcp_basis = LP_GLPK_SMCP_BASIS_ADV;
    cfg->glpk_smcp_presolve = LP_GLPK_SMCP_PRESOLVE_ON;
    cfg->glpk_bfcp_backend = LP_GLPK_BFCP_BACKEND_LUF_FT;
    cfg->glpk_bfcp_update_limit = 100;
    cfg->glpk_bfcp_pivot_tol = 0.0;
    cfg->glpk_bfcp_growth_guard = 0.0;
}

void lp_policy_glpk_compat_apply_runtime(const LPGLPKCompatConfig *cfg,
                                         int *method_io,
                                         int *pricing_io,
                                         int *phase1_pricing_io,
                                         int *presolve_io) {
    if (!cfg) return;
    if (cfg->lp_policy_profile != LP_POLICY_PROFILE_GLPK_COMPAT) return;

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
}
