#include <stdio.h>
#include <math.h>
#include "lp_policy_glpk_compat.h"

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT_TRUE(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s\n", msg); \
    } \
} while (0)

#define ASSERT_INT_EQ(a, b, msg) do { \
    tests_run++; \
    if ((a) == (b)) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%d != %d)\n", msg, (int)(a), (int)(b)); \
    } \
} while (0)

static void test_init_and_validate(void) {
    LPGLPKCompatConfig cfg;
    lp_policy_glpk_compat_init(&cfg);

    ASSERT_INT_EQ(cfg.lp_policy_profile, LP_POLICY_PROFILE_DEFAULT,
                  "init: default profile");
    ASSERT_INT_EQ(cfg.glpk_smcp_method, LP_GLPK_SMCP_METHOD_AUTO,
                  "init: default method");
    ASSERT_INT_EQ(cfg.glpk_smcp_pricing, LP_GLPK_SMCP_PRICING_STEEP,
                  "init: default pricing");
    ASSERT_INT_EQ(cfg.glpk_bfcp_update_limit, -1,
                  "init: default update limit");
    ASSERT_TRUE(lp_policy_glpk_compat_validate(&cfg) == 1,
                "init: default config validates");
}

static void test_profile_defaults(void) {
    LPGLPKCompatConfig cfg;
    lp_policy_glpk_compat_init(&cfg);

    cfg.lp_policy_profile = LP_POLICY_PROFILE_GLPK_COMPAT;
    lp_policy_glpk_compat_apply_profile_defaults(&cfg);

    ASSERT_INT_EQ(cfg.glpk_smcp_method, LP_GLPK_SMCP_METHOD_PRIMAL,
                  "profile: method");
    ASSERT_INT_EQ(cfg.glpk_smcp_pricing, LP_GLPK_SMCP_PRICING_STEEP,
                  "profile: pricing");
    ASSERT_INT_EQ(cfg.glpk_smcp_ratio, LP_GLPK_SMCP_RATIO_HARRIS,
                  "profile: ratio");
    ASSERT_INT_EQ(cfg.glpk_smcp_flip, LP_GLPK_SMCP_FLIP_OFF,
                  "profile: flip");
    ASSERT_INT_EQ(cfg.glpk_smcp_basis, LP_GLPK_SMCP_BASIS_ADV,
                  "profile: basis");
    ASSERT_INT_EQ(cfg.glpk_smcp_presolve, LP_GLPK_SMCP_PRESOLVE_ON,
                  "profile: presolve");
    ASSERT_INT_EQ(cfg.glpk_bfcp_backend, LP_GLPK_BFCP_BACKEND_LUF_FT,
                  "profile: backend");
    ASSERT_INT_EQ(cfg.glpk_bfcp_update_limit, 100,
                  "profile: update limit");
}

static void test_runtime_mapping_noop_under_default_profile(void) {
    LPGLPKCompatConfig cfg;
    int method = 2;
    int pricing = 2;
    int phase1_pricing = -1;
    int presolve = 0;

    lp_policy_glpk_compat_init(&cfg);
    cfg.glpk_smcp_method = LP_GLPK_SMCP_METHOD_DUAL;
    cfg.glpk_smcp_pricing = LP_GLPK_SMCP_PRICING_STANDARD;
    cfg.glpk_smcp_presolve = LP_GLPK_SMCP_PRESOLVE_ON;
    lp_policy_glpk_compat_apply_runtime(&cfg,
                                        &method,
                                        &pricing,
                                        &phase1_pricing,
                                        &presolve);

    ASSERT_INT_EQ(method, 2, "runtime default profile: method unchanged");
    ASSERT_INT_EQ(pricing, 2, "runtime default profile: pricing unchanged");
    ASSERT_INT_EQ(phase1_pricing, -1, "runtime default profile: phase1 pricing unchanged");
    ASSERT_INT_EQ(presolve, 0, "runtime default profile: presolve unchanged");
}

static void test_runtime_mapping_glpk_profile(void) {
    LPGLPKCompatConfig cfg;
    int method = 2;
    int pricing = 2;
    int phase1_pricing = -1;
    int presolve = 0;

    lp_policy_glpk_compat_init(&cfg);
    cfg.lp_policy_profile = LP_POLICY_PROFILE_GLPK_COMPAT;
    cfg.glpk_smcp_method = LP_GLPK_SMCP_METHOD_DUAL;
    cfg.glpk_smcp_pricing = LP_GLPK_SMCP_PRICING_STANDARD;
    cfg.glpk_smcp_presolve = LP_GLPK_SMCP_PRESOLVE_OFF;
    lp_policy_glpk_compat_apply_runtime(&cfg,
                                        &method,
                                        &pricing,
                                        &phase1_pricing,
                                        &presolve);

    ASSERT_INT_EQ(method, 1, "runtime glpk profile: dual method mapped");
    ASSERT_INT_EQ(pricing, 0, "runtime glpk profile: standard pricing mapped");
    ASSERT_INT_EQ(phase1_pricing, 0, "runtime glpk profile: phase1 pricing mapped");
    ASSERT_INT_EQ(presolve, -1, "runtime glpk profile: presolve off mapped");
}

static void test_validation_rejects_invalid_values(void) {
    LPGLPKCompatConfig cfg;
    lp_policy_glpk_compat_init(&cfg);

    cfg.glpk_bfcp_update_limit = -2;
    ASSERT_TRUE(lp_policy_glpk_compat_validate(&cfg) == 0,
                "validate: rejects update_limit < -1");

    lp_policy_glpk_compat_init(&cfg);
    cfg.lp_policy_profile = 7;
    ASSERT_TRUE(lp_policy_glpk_compat_validate(&cfg) == 0,
                "validate: rejects invalid profile");

    lp_policy_glpk_compat_init(&cfg);
    cfg.glpk_bfcp_pivot_tol = INFINITY;
    ASSERT_TRUE(lp_policy_glpk_compat_validate(&cfg) == 0,
                "validate: rejects non-finite pivot tol");
}

int main(void) {
    printf("=== LP GLPK-Compat Policy Tests ===\n");

    test_init_and_validate();
    test_profile_defaults();
    test_runtime_mapping_noop_under_default_profile();
    test_runtime_mapping_glpk_profile();
    test_validation_rejects_invalid_values();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
