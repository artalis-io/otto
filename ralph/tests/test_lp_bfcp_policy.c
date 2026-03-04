#include <math.h>
#include <stdio.h>
#include "lp_bfcp_policy.h"

#define TEST(cond, msg) \
    do { \
        total++; \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s\n", (msg)); \
        } else { \
            pass++; \
        } \
    } while (0)

int main(void) {
    int pass = 0;
    int total = 0;
    LPBFCPPolicyRequest req;
    LPBFCPPolicyEffective eff;
    int rc;

    printf("=== LP BFCP Policy Tests ===\n");

    rc = lp_bfcp_policy_compute(NULL, &eff);
    TEST(rc == -1, "compute: null request rejected");
    rc = lp_bfcp_policy_compute(&req, NULL);
    TEST(rc == -1, "compute: null effective rejected");

    lp_bfcp_policy_request_init(&req);
    TEST(req.requested_backend == LP_GLPK_BFCP_BACKEND_LUF_FT,
         "request init: backend luf_ft");
    TEST(req.requested_update_limit == -1,
         "request init: update limit auto");
    TEST(req.requested_pivot_tol == 0.0,
         "request init: pivot tol auto");
    TEST(req.requested_growth_guard == 0.0,
         "request init: growth guard auto");

    lp_bfcp_policy_effective_init(&eff);
    TEST(eff.effective_backend == LP_GLPK_BFCP_BACKEND_LUF_FT,
         "effective init: backend luf_ft");
    TEST(eff.backend_supported == 1,
         "effective init: backend supported");
    TEST(eff.update_limit_override == -1,
         "effective init: update limit auto");
    TEST(eff.pivot_tol_override == 0.0,
         "effective init: pivot tol auto");
    TEST(eff.growth_guard_override == 0.0,
         "effective init: growth guard auto");

    req.requested_backend = LP_GLPK_BFCP_BACKEND_LUF_FT;
    req.requested_update_limit = 80;
    req.requested_pivot_tol = 1e-8;
    req.requested_growth_guard = 1e6;
    rc = lp_bfcp_policy_compute(&req, &eff);
    TEST(rc == 0, "compute: valid request");
    TEST(eff.backend_supported == 1, "compute: luf_ft supported");
    TEST(eff.effective_backend == LP_GLPK_BFCP_BACKEND_LUF_FT,
         "compute: luf_ft remains effective backend");
    TEST(eff.update_limit_override == 80,
         "compute: update limit propagated");
    TEST(fabs(eff.pivot_tol_override - 1e-8) <= 1e-16,
         "compute: pivot tol propagated");
    TEST(fabs(eff.growth_guard_override - 1e6) <= 1e-6,
         "compute: growth guard propagated");

    req.requested_backend = LP_GLPK_BFCP_BACKEND_CGR;
    req.requested_update_limit = -1;
    req.requested_pivot_tol = 0.0;
    req.requested_growth_guard = 0.0;
    rc = lp_bfcp_policy_compute(&req, &eff);
    TEST(rc == 0, "compute: unsupported backend request accepted");
    TEST(eff.backend_supported == 0,
         "compute: cgr marked unsupported");
    TEST(eff.effective_backend == LP_GLPK_BFCP_BACKEND_LUF_FT,
         "compute: unsupported backend clamped to luf_ft");
    TEST(eff.update_limit_override == -1,
         "compute: auto update limit preserved");

    req.requested_backend = 99;
    req.requested_update_limit = 0;
    req.requested_pivot_tol = INFINITY;
    req.requested_growth_guard = NAN;
    rc = lp_bfcp_policy_compute(&req, &eff);
    TEST(rc == 0, "compute: out-of-range backend request accepted");
    TEST(eff.backend_supported == 0,
         "compute: out-of-range backend marked unsupported");
    TEST(eff.effective_backend == LP_GLPK_BFCP_BACKEND_LUF_FT,
         "compute: out-of-range backend clamped to luf_ft");
    TEST(eff.update_limit_override == -1,
         "compute: non-positive update limit normalized to auto");
    TEST(eff.pivot_tol_override == 0.0,
         "compute: non-finite pivot tol normalized to auto");
    TEST(eff.growth_guard_override == 0.0,
         "compute: non-finite growth guard normalized to auto");

    printf("Passed %d/%d bfcp-policy tests\n", pass, total);
    return (pass == total) ? 0 : 1;
}
