#include <math.h>
#include "lp_bfcp_policy.h"

static int bfcp_backend_supported(int backend) {
    return backend == LP_GLPK_BFCP_BACKEND_LUF_FT;
}

void lp_bfcp_policy_request_init(LPBFCPPolicyRequest *req) {
    if (!req) return;
    req->requested_backend = LP_GLPK_BFCP_BACKEND_LUF_FT;
    req->requested_update_limit = -1;
    req->requested_pivot_tol = 0.0;
    req->requested_growth_guard = 0.0;
}

void lp_bfcp_policy_effective_init(LPBFCPPolicyEffective *eff) {
    if (!eff) return;
    eff->effective_backend = LP_GLPK_BFCP_BACKEND_LUF_FT;
    eff->backend_supported = 1;
    eff->update_limit_override = -1;
    eff->pivot_tol_override = 0.0;
    eff->growth_guard_override = 0.0;
}

int lp_bfcp_policy_compute(const LPBFCPPolicyRequest *req,
                           LPBFCPPolicyEffective *eff) {
    int requested_backend = LP_GLPK_BFCP_BACKEND_LUF_FT;

    if (!req || !eff) return -1;
    lp_bfcp_policy_effective_init(eff);

    requested_backend = req->requested_backend;
    eff->backend_supported = bfcp_backend_supported(requested_backend);
    eff->effective_backend = LP_GLPK_BFCP_BACKEND_LUF_FT;

    if (req->requested_update_limit > 0) {
        eff->update_limit_override = req->requested_update_limit;
    }
    if (isfinite(req->requested_pivot_tol) && req->requested_pivot_tol > 0.0) {
        eff->pivot_tol_override = req->requested_pivot_tol;
    }
    if (isfinite(req->requested_growth_guard) && req->requested_growth_guard > 0.0) {
        eff->growth_guard_override = req->requested_growth_guard;
    }

    return 0;
}
