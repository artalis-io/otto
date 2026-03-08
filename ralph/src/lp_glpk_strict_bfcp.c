#include "lp_glpk_strict_bfcp.h"

static int lp_glpk_strict_bfcp_valid_factorization(int factorization) {
    return factorization >= LP_GLPK_BFCP_FACTORIZATION_LUF &&
           factorization <= LP_GLPK_BFCP_FACTORIZATION_BTF;
}

static int lp_glpk_strict_bfcp_valid_backend(int backend) {
    return backend >= LP_GLPK_BFCP_BACKEND_LUF_FT &&
           backend <= LP_GLPK_BFCP_BACKEND_CGR;
}

void lp_glpk_strict_bfcp_request_init(LPGLPKStrictBFCPRequest *req) {
    if (!req) return;
    req->strict_mode = 0;
    req->factorization = LP_GLPK_BFCP_FACTORIZATION_LUF;
    req->backend = LP_GLPK_BFCP_BACKEND_LUF_FT;
}

void lp_glpk_strict_bfcp_plan_init(LPGLPKStrictBFCPPlan *plan) {
    if (!plan) return;
    plan->strict_lane_active = 0;
    plan->effective_factorization = LP_GLPK_BFCP_FACTORIZATION_LUF;
    plan->effective_backend = LP_GLPK_BFCP_BACKEND_LUF_FT;
    plan->use_btf = 0;
    plan->prefer_dense_ge_numeric = 0;
    plan->allow_supernode_lane = 1;
    plan->allow_symbolic_full_retry = 1;
    plan->allow_top_level_dense_fallback = 1;
}

int lp_glpk_strict_bfcp_build_plan(const LPGLPKStrictBFCPRequest *req,
                                   LPGLPKStrictBFCPPlan *plan) {
    int factorization;
    int backend;

    if (!req || !plan) return -1;
    lp_glpk_strict_bfcp_plan_init(plan);

    factorization = lp_glpk_strict_bfcp_valid_factorization(req->factorization)
        ? req->factorization
        : LP_GLPK_BFCP_FACTORIZATION_LUF;
    backend = lp_glpk_strict_bfcp_valid_backend(req->backend)
        ? req->backend
        : LP_GLPK_BFCP_BACKEND_LUF_FT;

    plan->effective_factorization = factorization;
    plan->effective_backend = backend;
    plan->use_btf = (factorization == LP_GLPK_BFCP_FACTORIZATION_BTF) ? 1 : 0;
    plan->prefer_dense_ge_numeric = (backend == LP_GLPK_BFCP_BACKEND_CBG) ? 1 : 0;

    if (req->strict_mode) {
        plan->strict_lane_active = 1;
        plan->allow_supernode_lane = 0;
        plan->allow_symbolic_full_retry = 0;
        plan->allow_top_level_dense_fallback = 1;
    }

    return 0;
}
