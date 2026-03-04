#ifndef LP_BFCP_POLICY_H
#define LP_BFCP_POLICY_H

#include "lp_policy_glpk_compat.h"

/* BFCP policy module: normalize requested BFCP controls into effective
 * runtime overrides consumed by the solver/LU layer. */
typedef struct {
    int requested_backend;         /* LP_GLPK_BFCP_BACKEND_* */
    int requested_update_limit;    /* <=0 => auto/default */
    double requested_pivot_tol;    /* <=0/non-finite => auto/default */
    double requested_growth_guard; /* <=0/non-finite => auto/default */
} LPBFCPPolicyRequest;

typedef struct {
    int effective_backend;      /* currently clamped to LUF_FT */
    int backend_supported;      /* 1 if requested backend is available */
    int update_limit_override;  /* <=0 => use LU default */
    double pivot_tol_override;  /* <=0 => use LU default */
    double growth_guard_override; /* <=0 => use LU default */
} LPBFCPPolicyEffective;

void lp_bfcp_policy_request_init(LPBFCPPolicyRequest *req);
void lp_bfcp_policy_effective_init(LPBFCPPolicyEffective *eff);

/* Returns 0 on success, -1 on invalid pointers. */
int lp_bfcp_policy_compute(const LPBFCPPolicyRequest *req,
                           LPBFCPPolicyEffective *eff);

#endif /* LP_BFCP_POLICY_H */
