#ifndef LP_GLPK_STRICT_BFCP_H
#define LP_GLPK_STRICT_BFCP_H

#include "lp_policy_glpk_compat.h"

/* Centralized strict-mode LU/BFCP dispatch plan.
 *
 * This keeps strict-lane LU execution choices inspectable and orthogonal to
 * Ralph's default LU orchestration path. */

typedef struct {
    int strict_mode;          /* 0=default lane, 1=strict GLPK-like lane */
    int factorization;        /* LP_GLPK_BFCP_FACTORIZATION_* */
    int backend;              /* LP_GLPK_BFCP_BACKEND_* */
} LPGLPKStrictBFCPRequest;

typedef struct {
    int strict_lane_active;             /* 1 when strict lane is selected */
    int effective_factorization;        /* LP_GLPK_BFCP_FACTORIZATION_* */
    int effective_backend;              /* LP_GLPK_BFCP_BACKEND_* */
    int use_btf;                        /* 1 when BTF factorization requested */
    int prefer_dense_ge_numeric;        /* 1 when strict lane should skip Markowitz */
    int allow_supernode_lane;           /* 0 in strict lane */
    int allow_symbolic_full_retry;      /* 0 in strict lane */
    int allow_top_level_dense_fallback; /* 1 while strict lane still uses dense safety fallback */
} LPGLPKStrictBFCPPlan;

void lp_glpk_strict_bfcp_request_init(LPGLPKStrictBFCPRequest *req);
void lp_glpk_strict_bfcp_plan_init(LPGLPKStrictBFCPPlan *plan);
int lp_glpk_strict_bfcp_build_plan(const LPGLPKStrictBFCPRequest *req,
                                   LPGLPKStrictBFCPPlan *plan);

#endif /* LP_GLPK_STRICT_BFCP_H */
