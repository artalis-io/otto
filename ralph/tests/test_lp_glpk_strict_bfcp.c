#include <stdio.h>
#include "lp_glpk_strict_bfcp.h"

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
    LPGLPKStrictBFCPRequest req;
    LPGLPKStrictBFCPPlan plan;

    printf("=== LP GLPK Strict BFCP Dispatch Tests ===\n");

    lp_glpk_strict_bfcp_request_init(&req);
    TEST(req.strict_mode == 0, "strict bfcp: request init strict off");
    TEST(req.factorization == LP_GLPK_BFCP_FACTORIZATION_LUF,
         "strict bfcp: request init factorization luf");
    TEST(req.backend == LP_GLPK_BFCP_BACKEND_LUF_FT,
         "strict bfcp: request init backend luf_ft");

    TEST(lp_glpk_strict_bfcp_build_plan(&req, &plan) == 0,
         "strict bfcp: default plan build");
    TEST(plan.strict_lane_active == 0,
         "strict bfcp: default plan inactive");
    TEST(plan.allow_supernode_lane == 1,
         "strict bfcp: default plan keeps supernode lane");
    TEST(plan.allow_symbolic_full_retry == 1,
         "strict bfcp: default plan keeps symbolic full retry");
    TEST(plan.allow_top_level_dense_fallback == 1,
         "strict bfcp: default plan keeps dense fallback");

    req.strict_mode = 1;
    req.factorization = LP_GLPK_BFCP_FACTORIZATION_LUF;
    req.backend = LP_GLPK_BFCP_BACKEND_CGR;
    TEST(lp_glpk_strict_bfcp_build_plan(&req, &plan) == 0,
         "strict bfcp: strict luf plan build");
    TEST(plan.strict_lane_active == 1,
         "strict bfcp: strict lane active");
    TEST(plan.effective_factorization == LP_GLPK_BFCP_FACTORIZATION_LUF,
         "strict bfcp: strict plan keeps luf");
    TEST(plan.effective_backend == LP_GLPK_BFCP_BACKEND_CGR,
         "strict bfcp: strict plan keeps backend");
    TEST(plan.use_btf == 0,
         "strict bfcp: strict luf plan not btf");
    TEST(plan.allow_supernode_lane == 0,
         "strict bfcp: strict plan disables supernode");
    TEST(plan.allow_symbolic_full_retry == 0,
         "strict bfcp: strict plan disables symbolic full retry");
    TEST(plan.allow_top_level_dense_fallback == 1,
         "strict bfcp: strict plan keeps dense fallback safety");

    req.factorization = LP_GLPK_BFCP_FACTORIZATION_BTF;
    req.backend = LP_GLPK_BFCP_BACKEND_CBG;
    TEST(lp_glpk_strict_bfcp_build_plan(&req, &plan) == 0,
         "strict bfcp: strict btf plan build");
    TEST(plan.use_btf == 1,
         "strict bfcp: strict btf plan marks btf");
    TEST(plan.effective_backend == LP_GLPK_BFCP_BACKEND_CBG,
         "strict bfcp: strict btf plan keeps backend");

    printf("Passed %d/%d glpk-strict-bfcp tests\n", pass, total);
    return (pass == total) ? 0 : 1;
}
