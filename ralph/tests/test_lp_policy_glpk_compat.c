#include <stdio.h>
#include <math.h>
#include <string.h>
#include "lp.h"
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

#define ASSERT_DBL_NEAR(a, b, tol, msg) do { \
    tests_run++; \
    if (fabs((a) - (b)) <= (tol)) { \
        tests_passed++; \
    } else { \
        printf("  FAIL: %s (%g != %g)\n", msg, (double)(a), (double)(b)); \
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
    ASSERT_DBL_NEAR(cfg.glpk_smcp_tol_bnd, 1e-7, 1e-16,
                    "init: default tol_bnd");
    ASSERT_DBL_NEAR(cfg.glpk_smcp_tol_dj, 1e-7, 1e-16,
                    "init: default tol_dj");
    ASSERT_DBL_NEAR(cfg.glpk_smcp_tol_piv, 1e-9, 1e-18,
                    "init: default tol_piv");
    ASSERT_INT_EQ(cfg.glpk_smcp_excl, LP_GLPK_SMCP_EXCL_ON,
                  "init: default excl");
    ASSERT_INT_EQ(cfg.glpk_smcp_shift, LP_GLPK_SMCP_SHIFT_ON,
                  "init: default shift");
    ASSERT_INT_EQ(cfg.glpk_smcp_aorn, LP_GLPK_SMCP_AORN_USE_NT,
                  "init: default aorn");
    ASSERT_INT_EQ(cfg.glpk_bfcp_factorization, LP_GLPK_BFCP_FACTORIZATION_LUF,
                  "init: default factorization");
    ASSERT_INT_EQ(cfg.glpk_bfcp_update_limit, -1,
                  "init: default update limit");
    ASSERT_INT_EQ(cfg.glpk_bfcp_pivot_limit, -1,
                  "init: default pivot limit");
    ASSERT_INT_EQ(cfg.glpk_bfcp_suhl, LP_GLPK_BFCP_SUHL_AUTO,
                  "init: default suhl");
    ASSERT_DBL_NEAR(cfg.glpk_bfcp_eps_tol, 0.0, 1e-16,
                    "init: default eps_tol");
    ASSERT_INT_EQ(cfg.glpk_bfcp_nfs_max, -1,
                  "init: default nfs_max");
    ASSERT_INT_EQ(cfg.glpk_bfcp_nrs_max, -1,
                  "init: default nrs_max");
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
    ASSERT_DBL_NEAR(cfg.glpk_smcp_tol_bnd, 1e-7, 1e-16,
                    "profile: tol_bnd");
    ASSERT_DBL_NEAR(cfg.glpk_smcp_tol_dj, 1e-7, 1e-16,
                    "profile: tol_dj");
    ASSERT_DBL_NEAR(cfg.glpk_smcp_tol_piv, 1e-9, 1e-18,
                    "profile: tol_piv");
    ASSERT_INT_EQ(cfg.glpk_smcp_excl, LP_GLPK_SMCP_EXCL_ON,
                  "profile: excl");
    ASSERT_INT_EQ(cfg.glpk_smcp_shift, LP_GLPK_SMCP_SHIFT_ON,
                  "profile: shift");
    ASSERT_INT_EQ(cfg.glpk_smcp_aorn, LP_GLPK_SMCP_AORN_USE_NT,
                  "profile: aorn");
    ASSERT_INT_EQ(cfg.glpk_bfcp_backend, LP_GLPK_BFCP_BACKEND_LUF_FT,
                  "profile: backend");
    ASSERT_INT_EQ(cfg.glpk_bfcp_factorization, LP_GLPK_BFCP_FACTORIZATION_LUF,
                  "profile: factorization remains luf");
    ASSERT_INT_EQ(cfg.glpk_bfcp_update_limit, 100,
                  "profile: update limit");
    ASSERT_INT_EQ(cfg.glpk_bfcp_pivot_limit, -1,
                  "profile: pivot limit remains auto");
    ASSERT_INT_EQ(cfg.glpk_bfcp_suhl, LP_GLPK_BFCP_SUHL_AUTO,
                  "profile: suhl remains auto");
}

static void test_profile_defaults_glpk_strict(void) {
    LPGLPKCompatConfig cfg;
    lp_policy_glpk_compat_init(&cfg);

    cfg.lp_policy_profile = LP_POLICY_PROFILE_GLPK_STRICT;
    lp_policy_glpk_compat_apply_profile_defaults(&cfg);

    ASSERT_INT_EQ(cfg.glpk_smcp_method, LP_GLPK_SMCP_METHOD_PRIMAL,
                  "strict profile: method");
    ASSERT_INT_EQ(cfg.glpk_smcp_pricing, LP_GLPK_SMCP_PRICING_STEEP,
                  "strict profile: pricing");
    ASSERT_INT_EQ(cfg.glpk_smcp_ratio, LP_GLPK_SMCP_RATIO_HARRIS,
                  "strict profile: ratio");
    ASSERT_INT_EQ(cfg.glpk_smcp_flip, LP_GLPK_SMCP_FLIP_OFF,
                  "strict profile: flip");
    ASSERT_INT_EQ(cfg.glpk_smcp_basis, LP_GLPK_SMCP_BASIS_ADV,
                  "strict profile: basis");
    ASSERT_INT_EQ(cfg.glpk_smcp_presolve, LP_GLPK_SMCP_PRESOLVE_ON,
                  "strict profile: presolve");
    ASSERT_DBL_NEAR(cfg.glpk_smcp_tol_bnd, 1e-7, 1e-16,
                    "strict profile: tol_bnd");
    ASSERT_DBL_NEAR(cfg.glpk_smcp_tol_dj, 1e-7, 1e-16,
                    "strict profile: tol_dj");
    ASSERT_DBL_NEAR(cfg.glpk_smcp_tol_piv, 1e-9, 1e-18,
                    "strict profile: tol_piv");
    ASSERT_INT_EQ(cfg.glpk_smcp_excl, LP_GLPK_SMCP_EXCL_ON,
                  "strict profile: excl");
    ASSERT_INT_EQ(cfg.glpk_smcp_shift, LP_GLPK_SMCP_SHIFT_ON,
                  "strict profile: shift");
    ASSERT_INT_EQ(cfg.glpk_smcp_aorn, LP_GLPK_SMCP_AORN_USE_NT,
                  "strict profile: aorn");
    ASSERT_INT_EQ(cfg.glpk_bfcp_backend, LP_GLPK_BFCP_BACKEND_LUF_FT,
                  "strict profile: backend");
    ASSERT_INT_EQ(cfg.glpk_bfcp_update_limit, 100,
                  "strict profile: update limit");
}

static void test_profile_defaults_glpk_legacy(void) {
    LPGLPKCompatConfig cfg;
    lp_policy_glpk_compat_init(&cfg);

    cfg.lp_policy_profile = LP_POLICY_PROFILE_GLPK_LEGACY;
    lp_policy_glpk_compat_apply_profile_defaults(&cfg);

    ASSERT_INT_EQ(cfg.glpk_smcp_method, LP_GLPK_SMCP_METHOD_PRIMAL,
                  "legacy profile: method");
    ASSERT_INT_EQ(cfg.glpk_smcp_pricing, LP_GLPK_SMCP_PRICING_STEEP,
                  "legacy profile: pricing");
    ASSERT_INT_EQ(cfg.glpk_smcp_ratio, LP_GLPK_SMCP_RATIO_HARRIS,
                  "legacy profile: ratio");
    ASSERT_INT_EQ(cfg.glpk_smcp_flip, LP_GLPK_SMCP_FLIP_OFF,
                  "legacy profile: flip");
    ASSERT_INT_EQ(cfg.glpk_bfcp_backend, LP_GLPK_BFCP_BACKEND_LUF_FT,
                  "legacy profile: backend");
    ASSERT_TRUE(lp_policy_glpk_compat_validate(&cfg) == 1,
                "legacy profile: config validates");
}

static void test_basis_helpers(void) {
    int crash = -1;

    ASSERT_TRUE(lp_policy_glpk_basis_crash_mode(LP_GLPK_SMCP_BASIS_ADV, &crash) == 1,
                "basis helper: adv crash mode valid");
    ASSERT_INT_EQ(crash, 1, "basis helper: adv maps to crash on");
    ASSERT_TRUE(lp_policy_glpk_basis_crash_mode(LP_GLPK_SMCP_BASIS_STD, &crash) == 1,
                "basis helper: std crash mode valid");
    ASSERT_INT_EQ(crash, 0, "basis helper: std maps to crash off");
    ASSERT_TRUE(lp_policy_glpk_basis_crash_mode(LP_GLPK_SMCP_BASIS_BIB, &crash) == 1,
                "basis helper: bib crash mode valid");
    ASSERT_INT_EQ(crash, 1, "basis helper: bib maps to crash-like mode");
    ASSERT_TRUE(lp_policy_glpk_basis_crash_mode(LP_GLPK_SMCP_BASIS_INI, &crash) == 1,
                "basis helper: ini crash mode valid");
    ASSERT_INT_EQ(crash, 0, "basis helper: ini maps to crash off");

    ASSERT_INT_EQ(lp_policy_glpk_basis_requires_staged_basis(LP_GLPK_SMCP_BASIS_ADV), 0,
                  "basis helper: adv does not require staged basis");
    ASSERT_INT_EQ(lp_policy_glpk_basis_requires_staged_basis(LP_GLPK_SMCP_BASIS_INI), 1,
                  "basis helper: ini requires staged basis");

    ASSERT_INT_EQ(lp_policy_glpk_basis_supports_current_runtime(LP_GLPK_SMCP_BASIS_ADV), 1,
                  "basis helper: adv supported");
    ASSERT_INT_EQ(lp_policy_glpk_basis_supports_current_runtime(LP_GLPK_SMCP_BASIS_STD), 1,
                  "basis helper: std supported");
    ASSERT_INT_EQ(lp_policy_glpk_basis_supports_current_runtime(LP_GLPK_SMCP_BASIS_BIB), 0,
                  "basis helper: bib not yet supported");
    ASSERT_INT_EQ(lp_policy_glpk_basis_supports_current_runtime(LP_GLPK_SMCP_BASIS_INI), 1,
                  "basis helper: ini supported");

    ASSERT_TRUE(strcmp(lp_policy_glpk_basis_name(LP_GLPK_SMCP_BASIS_ADV), "adv") == 0,
                "basis helper: adv name");
    ASSERT_TRUE(strcmp(lp_policy_glpk_basis_name(LP_GLPK_SMCP_BASIS_BIB), "bib") == 0,
                "basis helper: bib name");
    ASSERT_TRUE(strcmp(lp_policy_glpk_basis_name(LP_GLPK_SMCP_BASIS_INI), "ini") == 0,
                "basis helper: ini name");
}

static void test_bfcp_runtime_support_helpers(void) {
    LPGLPKCompatConfig cfg;
    const char *unsupported = "sentinel";

    lp_policy_glpk_compat_init(&cfg);
    ASSERT_INT_EQ(lp_policy_glpk_bfcp_supports_current_runtime(&cfg, &unsupported), 1,
                  "bfcp helper: defaults supported");
    ASSERT_TRUE(unsupported == NULL,
                "bfcp helper: defaults keep unsupported parameter null");

    cfg.glpk_bfcp_factorization = LP_GLPK_BFCP_FACTORIZATION_BTF;
    ASSERT_INT_EQ(lp_policy_glpk_bfcp_supports_current_runtime(&cfg, &unsupported), 1,
                  "bfcp helper: btf supported");
    ASSERT_TRUE(unsupported == NULL,
                "bfcp helper: btf keeps unsupported parameter null");

    lp_policy_glpk_compat_init(&cfg);
    cfg.glpk_bfcp_pivot_limit = 4;
    ASSERT_INT_EQ(lp_policy_glpk_bfcp_supports_current_runtime(&cfg, &unsupported), 0,
                  "bfcp helper: pivot limit unsupported");
    ASSERT_TRUE(strcmp(unsupported, "glpk_bfcp_pivot_limit") == 0,
                "bfcp helper: pivot limit name");

    lp_policy_glpk_compat_init(&cfg);
    cfg.glpk_bfcp_suhl = LP_GLPK_BFCP_SUHL_ON;
    ASSERT_INT_EQ(lp_policy_glpk_bfcp_supports_current_runtime(&cfg, &unsupported), 0,
                  "bfcp helper: suhl unsupported");
    ASSERT_TRUE(strcmp(unsupported, "glpk_bfcp_suhl") == 0,
                "bfcp helper: suhl name");

    lp_policy_glpk_compat_init(&cfg);
    cfg.glpk_bfcp_eps_tol = 1e-15;
    ASSERT_INT_EQ(lp_policy_glpk_bfcp_supports_current_runtime(&cfg, &unsupported), 0,
                  "bfcp helper: eps tol unsupported");
    ASSERT_TRUE(strcmp(unsupported, "glpk_bfcp_eps_tol") == 0,
                "bfcp helper: eps tol name");
}

static void test_runtime_mapping_noop_under_default_profile(void) {
    LPGLPKCompatConfig cfg;
    int method = 2;
    int pricing = 2;
    int phase1_pricing = -1;
    int presolve = 0;
    int ratio = 1;
    int dual_ratio = LP_DUAL_RATIO_TEST_HARRIS;
    int dual_bound_flip = 1;
    double tol_bnd = 1.0;
    double tol_dj = 2.0;
    double tol_piv = 3.0;
    int excl = 9;
    int shift = 8;
    int aorn = 7;
    int crash = 0;
    int backend = -1;
    int backend_supported = -1;
    int update_limit = -1;
    double pivot_tol = 0.0;
    double growth_guard = 0.0;
    int dual_refactor_base_interval = -1;
    int dual_rc_recompute_interval = -1;
    int soft_gate = 1;
    int periodic_gate = 1;

    lp_policy_glpk_compat_init(&cfg);
    cfg.glpk_smcp_method = LP_GLPK_SMCP_METHOD_DUAL;
    cfg.glpk_smcp_pricing = LP_GLPK_SMCP_PRICING_STANDARD;
    cfg.glpk_smcp_presolve = LP_GLPK_SMCP_PRESOLVE_ON;
    lp_policy_glpk_compat_apply_runtime(&cfg,
                                        &method,
                                        &pricing,
                                        &phase1_pricing,
                                        &presolve,
                                        &ratio,
                                        &dual_ratio,
                                        &dual_bound_flip,
                                        &tol_bnd,
                                        &tol_dj,
                                        &tol_piv,
                                        &excl,
                                        &shift,
                                        &aorn,
                                        &crash,
                                        &backend,
                                        &backend_supported,
                                        &update_limit,
                                        &pivot_tol,
                                        &growth_guard,
                                        &dual_refactor_base_interval,
                                        &dual_rc_recompute_interval,
                                        &soft_gate,
                                        &periodic_gate);

    ASSERT_INT_EQ(method, 2, "runtime default profile: method unchanged");
    ASSERT_INT_EQ(pricing, 2, "runtime default profile: pricing unchanged");
    ASSERT_INT_EQ(phase1_pricing, -1, "runtime default profile: phase1 pricing unchanged");
    ASSERT_INT_EQ(presolve, 0, "runtime default profile: presolve unchanged");
    ASSERT_INT_EQ(ratio, 1, "runtime default profile: ratio unchanged");
    ASSERT_INT_EQ(dual_ratio, LP_DUAL_RATIO_TEST_HARRIS,
                  "runtime default profile: dual ratio unchanged");
    ASSERT_INT_EQ(dual_bound_flip, 1, "runtime default profile: flip unchanged");
    ASSERT_DBL_NEAR(tol_bnd, 1.0, 1e-16,
                    "runtime default profile: tol_bnd unchanged");
    ASSERT_DBL_NEAR(tol_dj, 2.0, 1e-16,
                    "runtime default profile: tol_dj unchanged");
    ASSERT_DBL_NEAR(tol_piv, 3.0, 1e-16,
                    "runtime default profile: tol_piv unchanged");
    ASSERT_INT_EQ(excl, 9, "runtime default profile: excl unchanged");
    ASSERT_INT_EQ(shift, 8, "runtime default profile: shift unchanged");
    ASSERT_INT_EQ(aorn, 7, "runtime default profile: aorn unchanged");
    ASSERT_INT_EQ(crash, 0, "runtime default profile: crash unchanged");
    ASSERT_INT_EQ(backend, -1, "runtime default profile: backend unchanged");
    ASSERT_INT_EQ(backend_supported, -1, "runtime default profile: backend support unchanged");
    ASSERT_INT_EQ(update_limit, -1, "runtime default profile: update limit unchanged");
    ASSERT_INT_EQ(dual_refactor_base_interval, -1,
                  "runtime default profile: dual refactor interval unchanged");
    ASSERT_INT_EQ(dual_rc_recompute_interval, -1,
                  "runtime default profile: dual rc interval unchanged");
    ASSERT_INT_EQ(soft_gate, 1, "runtime default profile: soft gate unchanged");
    ASSERT_INT_EQ(periodic_gate, 1, "runtime default profile: periodic gate unchanged");
}

static void test_runtime_mapping_glpk_profile(void) {
    LPGLPKCompatConfig cfg;
    int method = 2;
    int pricing = 2;
    int phase1_pricing = -1;
    int presolve = 0;
    int ratio = -1;
    int dual_ratio = -1;
    int dual_bound_flip = -1;
    double tol_bnd = -1.0;
    double tol_dj = -1.0;
    double tol_piv = -1.0;
    int excl = -1;
    int shift = -1;
    int aorn = -1;
    int crash = -1;
    int backend = -1;
    int backend_supported = -1;
    int update_limit = -1;
    double pivot_tol = -1.0;
    double growth_guard = -1.0;
    int dual_refactor_base_interval = -1;
    int dual_rc_recompute_interval = -1;
    int soft_gate = 1;
    int periodic_gate = 1;

    lp_policy_glpk_compat_init(&cfg);
    cfg.lp_policy_profile = LP_POLICY_PROFILE_GLPK_COMPAT;
    cfg.glpk_smcp_method = LP_GLPK_SMCP_METHOD_DUALP;
    cfg.glpk_smcp_pricing = LP_GLPK_SMCP_PRICING_STANDARD;
    cfg.glpk_smcp_presolve = LP_GLPK_SMCP_PRESOLVE_OFF;
    cfg.glpk_smcp_ratio = LP_GLPK_SMCP_RATIO_STANDARD;
    cfg.glpk_smcp_flip = LP_GLPK_SMCP_FLIP_ON;
    cfg.glpk_smcp_basis = LP_GLPK_SMCP_BASIS_STD;
    cfg.glpk_smcp_tol_bnd = 2e-7;
    cfg.glpk_smcp_tol_dj = 3e-7;
    cfg.glpk_smcp_tol_piv = 4e-9;
    cfg.glpk_smcp_excl = LP_GLPK_SMCP_EXCL_OFF;
    cfg.glpk_smcp_shift = LP_GLPK_SMCP_SHIFT_OFF;
    cfg.glpk_smcp_aorn = LP_GLPK_SMCP_AORN_USE_AT;
    cfg.glpk_bfcp_backend = LP_GLPK_BFCP_BACKEND_CGR;
    cfg.glpk_bfcp_update_limit = 77;
    cfg.glpk_bfcp_pivot_tol = 1e-8;
    cfg.glpk_bfcp_growth_guard = 1e6;
    lp_policy_glpk_compat_apply_runtime(&cfg,
                                        &method,
                                        &pricing,
                                        &phase1_pricing,
                                        &presolve,
                                        &ratio,
                                        &dual_ratio,
                                        &dual_bound_flip,
                                        &tol_bnd,
                                        &tol_dj,
                                        &tol_piv,
                                        &excl,
                                        &shift,
                                        &aorn,
                                        &crash,
                                        &backend,
                                        &backend_supported,
                                        &update_limit,
                                        &pivot_tol,
                                        &growth_guard,
                                        &dual_refactor_base_interval,
                                        &dual_rc_recompute_interval,
                                        &soft_gate,
                                        &periodic_gate);

    ASSERT_INT_EQ(method, 2, "runtime glpk profile: dualp method mapped");
    ASSERT_INT_EQ(pricing, 0, "runtime glpk profile: standard pricing mapped");
    ASSERT_INT_EQ(phase1_pricing, 0, "runtime glpk profile: phase1 pricing mapped");
    ASSERT_INT_EQ(presolve, -1, "runtime glpk profile: presolve off mapped");
    ASSERT_INT_EQ(ratio, 0, "runtime glpk profile: standard ratio mapped");
    ASSERT_INT_EQ(dual_ratio, LP_DUAL_RATIO_TEST_FLIP,
                  "runtime glpk profile: flip maps to dual flip mode");
    ASSERT_INT_EQ(dual_bound_flip, 1,
                  "runtime glpk profile: strict-default enables bound flip");
    ASSERT_DBL_NEAR(tol_bnd, 2e-7, 1e-16,
                    "runtime glpk profile: tol_bnd mapped");
    ASSERT_DBL_NEAR(tol_dj, 3e-7, 1e-16,
                    "runtime glpk profile: tol_dj mapped");
    ASSERT_DBL_NEAR(tol_piv, 4e-9, 1e-18,
                    "runtime glpk profile: tol_piv mapped");
    ASSERT_INT_EQ(excl, LP_GLPK_SMCP_EXCL_OFF,
                  "runtime glpk profile: excl mapped");
    ASSERT_INT_EQ(shift, LP_GLPK_SMCP_SHIFT_OFF,
                  "runtime glpk profile: shift mapped");
    ASSERT_INT_EQ(aorn, LP_GLPK_SMCP_AORN_USE_AT,
                  "runtime glpk profile: aorn mapped");
    ASSERT_INT_EQ(crash, 0, "runtime glpk profile: std basis mapped to crash off");
    ASSERT_INT_EQ(backend, LP_GLPK_BFCP_BACKEND_CGR,
                  "runtime glpk profile: backend preserved");
    ASSERT_INT_EQ(backend_supported, 1,
                  "runtime glpk profile: cgr backend reported supported");
    ASSERT_INT_EQ(update_limit, 77, "runtime glpk profile: update limit mapped");
    ASSERT_DBL_NEAR(pivot_tol, 1e-8, 1e-14,
                    "runtime glpk profile: pivot tol mapped");
    ASSERT_DBL_NEAR(growth_guard, 1e6, 1e-6,
                    "runtime glpk profile: growth guard mapped");
    ASSERT_INT_EQ(dual_refactor_base_interval, 38,
                  "runtime glpk profile: dual refactor cadence mapped from update limit");
    ASSERT_INT_EQ(dual_rc_recompute_interval, 19,
                  "runtime glpk profile: dual rc cadence mapped from update limit");
    ASSERT_INT_EQ(soft_gate, 0, "runtime glpk profile: soft cost gate disabled");
    ASSERT_INT_EQ(periodic_gate, 0, "runtime glpk profile: periodic cost gate disabled");

    cfg.glpk_smcp_flip = LP_GLPK_SMCP_FLIP_OFF;
    cfg.glpk_smcp_ratio = LP_GLPK_SMCP_RATIO_HARRIS;
    dual_ratio = -1;
    lp_policy_glpk_compat_apply_runtime(&cfg,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        &dual_ratio,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL);
    ASSERT_INT_EQ(dual_ratio, LP_DUAL_RATIO_TEST_HARRIS,
                  "runtime glpk profile: dual ratio falls back to Harris when flip off");

    cfg.glpk_smcp_method = LP_GLPK_SMCP_METHOD_DUAL;
    method = -1;
    lp_policy_glpk_compat_apply_runtime(&cfg,
                                        &method,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL);
    ASSERT_INT_EQ(method, 1, "runtime glpk profile: dual-only method mapped");
}

static void test_runtime_mapping_glpk_strict_profile(void) {
    LPGLPKCompatConfig cfg;
    int dual_ratio = -1;
    int dual_bound_flip = -1;

    lp_policy_glpk_compat_init(&cfg);
    cfg.lp_policy_profile = LP_POLICY_PROFILE_GLPK_STRICT;
    cfg.glpk_smcp_ratio = LP_GLPK_SMCP_RATIO_STANDARD;
    cfg.glpk_smcp_flip = LP_GLPK_SMCP_FLIP_OFF;
    lp_policy_glpk_compat_apply_runtime(&cfg,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        &dual_ratio,
                                        &dual_bound_flip,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL);
    ASSERT_INT_EQ(dual_ratio, LP_DUAL_RATIO_TEST_STANDARD,
                  "runtime strict profile: standard ratio mapped");
    ASSERT_INT_EQ(dual_bound_flip, 0,
                  "runtime strict profile: flip off disables bound flip");

    cfg.glpk_smcp_flip = LP_GLPK_SMCP_FLIP_ON;
    dual_ratio = -1;
    dual_bound_flip = -1;
    lp_policy_glpk_compat_apply_runtime(&cfg,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        &dual_ratio,
                                        &dual_bound_flip,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL,
                                        NULL);
    ASSERT_INT_EQ(dual_ratio, LP_DUAL_RATIO_TEST_FLIP,
                  "runtime strict profile: flip on selects dual flip ratio");
    ASSERT_INT_EQ(dual_bound_flip, 1,
                  "runtime strict profile: flip on enables bound flip");
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
    cfg.glpk_smcp_tol_bnd = 0.0;
    ASSERT_TRUE(lp_policy_glpk_compat_validate(&cfg) == 0,
                "validate: rejects non-positive tol_bnd");

    lp_policy_glpk_compat_init(&cfg);
    cfg.glpk_smcp_excl = 2;
    ASSERT_TRUE(lp_policy_glpk_compat_validate(&cfg) == 0,
                "validate: rejects invalid smcp_excl");

    lp_policy_glpk_compat_init(&cfg);
    cfg.glpk_bfcp_pivot_tol = INFINITY;
    ASSERT_TRUE(lp_policy_glpk_compat_validate(&cfg) == 0,
                "validate: rejects non-finite pivot tol");

    lp_policy_glpk_compat_init(&cfg);
    cfg.glpk_bfcp_factorization = 2;
    ASSERT_TRUE(lp_policy_glpk_compat_validate(&cfg) == 0,
                "validate: rejects invalid factorization");

    lp_policy_glpk_compat_init(&cfg);
    cfg.glpk_bfcp_pivot_limit = -2;
    ASSERT_TRUE(lp_policy_glpk_compat_validate(&cfg) == 0,
                "validate: rejects pivot_limit < -1");

    lp_policy_glpk_compat_init(&cfg);
    cfg.glpk_bfcp_suhl = 2;
    ASSERT_TRUE(lp_policy_glpk_compat_validate(&cfg) == 0,
                "validate: rejects invalid suhl");

    lp_policy_glpk_compat_init(&cfg);
    cfg.glpk_bfcp_eps_tol = INFINITY;
    ASSERT_TRUE(lp_policy_glpk_compat_validate(&cfg) == 0,
                "validate: rejects non-finite eps tol");

    lp_policy_glpk_compat_init(&cfg);
    cfg.glpk_bfcp_nfs_max = -2;
    ASSERT_TRUE(lp_policy_glpk_compat_validate(&cfg) == 0,
                "validate: rejects nfs_max < -1");

    lp_policy_glpk_compat_init(&cfg);
    cfg.glpk_bfcp_nrs_max = -2;
    ASSERT_TRUE(lp_policy_glpk_compat_validate(&cfg) == 0,
                "validate: rejects nrs_max < -1");
}

static void test_working_lp_helpers(void) {
    int skip_shift_off = lp_policy_glpk_working_exclude_nonbasic(
        LP_GLPK_SMCP_EXCL_ON, LP_GLPK_SMCP_SHIFT_OFF,
        (int)RALPH_NONBASIC_LOWER, 5.0, 5.0 + 5e-8, 1e-7);
    int skip_shift_on = lp_policy_glpk_working_exclude_nonbasic(
        LP_GLPK_SMCP_EXCL_ON, LP_GLPK_SMCP_SHIFT_ON,
        (int)RALPH_NONBASIC_LOWER, 5.0, 5.0 + 5e-8, 1e-7);
    int skip_fixed = lp_policy_glpk_working_exclude_nonbasic(
        LP_GLPK_SMCP_EXCL_OFF, LP_GLPK_SMCP_SHIFT_OFF,
        (int)RALPH_FIXED, 2.0, 2.0, 1e-7);
    int skip_inf = lp_policy_glpk_working_exclude_nonbasic(
        LP_GLPK_SMCP_EXCL_ON, LP_GLPK_SMCP_SHIFT_ON,
        (int)RALPH_NONBASIC_LOWER, -RALPH_INFINITY, 10.0, 1e-7);
    int skip_tol_cap = lp_policy_glpk_working_exclude_nonbasic(
        LP_GLPK_SMCP_EXCL_ON, LP_GLPK_SMCP_SHIFT_ON,
        (int)RALPH_NONBASIC_LOWER, 1.0, 1.0 + 5e-6, 1e-3);
    int aorn_nt = lp_policy_glpk_working_use_at_kernel(LP_GLPK_SMCP_AORN_USE_NT, 1);
    int aorn_at_no_rows = lp_policy_glpk_working_use_at_kernel(LP_GLPK_SMCP_AORN_USE_AT, 0);
    int aorn_at_rows = lp_policy_glpk_working_use_at_kernel(LP_GLPK_SMCP_AORN_USE_AT, 1);
    double tol_shift_off = lp_policy_glpk_working_fixed_width_tol(
        LP_GLPK_SMCP_SHIFT_OFF, 1e-7);
    double tol_shift_on = lp_policy_glpk_working_fixed_width_tol(
        LP_GLPK_SMCP_SHIFT_ON, 1e-7);
    double tol_shift_on_cap = lp_policy_glpk_working_fixed_width_tol(
        LP_GLPK_SMCP_SHIFT_ON, 1e-3);

    ASSERT_INT_EQ(skip_shift_off, 0,
                  "working lp helper: shift off keeps narrow boxed var active");
    ASSERT_INT_EQ(skip_shift_on, 1,
                  "working lp helper: shift on excludes narrow boxed var");
    ASSERT_INT_EQ(skip_fixed, 1,
                  "working lp helper: fixed var excluded regardless of excl toggle");
    ASSERT_INT_EQ(skip_inf, 0,
                  "working lp helper: infinite-bound var remains in working LP");
    ASSERT_INT_EQ(skip_tol_cap, 0,
                  "working lp helper: shifted exclusion tolerance is capped");
    ASSERT_DBL_NEAR(tol_shift_off, 1e-12, 1e-18,
                    "working lp helper: shift-off fixed width tolerance is strict");
    ASSERT_DBL_NEAR(tol_shift_on, 1e-7, 1e-15,
                    "working lp helper: shift-on uses bound tolerance");
    ASSERT_DBL_NEAR(tol_shift_on_cap, 1e-7, 1e-15,
                    "working lp helper: shift-on fixed width tolerance is capped");
    ASSERT_INT_EQ(aorn_nt, 0,
                  "working lp helper: N^T selects column kernel");
    ASSERT_INT_EQ(aorn_at_no_rows, 0,
                  "working lp helper: A^T falls back without row scatter");
    ASSERT_INT_EQ(aorn_at_rows, 1,
                  "working lp helper: A^T selects row kernel when available");
}

static void test_perturb_state_machine_helpers(void) {
    int next = -1;
    int ok = lp_policy_glpk_perturb_next_state(LP_GLPK_PERTURB_STATE_OFF,
                                               LP_GLPK_PERTURB_EVENT_ENABLE,
                                               &next);
    ASSERT_INT_EQ(ok, 1, "perturb fsm: off->enable is valid");
    ASSERT_INT_EQ(next, LP_GLPK_PERTURB_STATE_ACTIVE,
                  "perturb fsm: off + enable => active");

    ok = lp_policy_glpk_perturb_next_state(next,
                                           LP_GLPK_PERTURB_EVENT_BEGIN_CLEANUP,
                                           &next);
    ASSERT_INT_EQ(ok, 1, "perturb fsm: active->begin cleanup is valid");
    ASSERT_INT_EQ(next, LP_GLPK_PERTURB_STATE_CLEANUP,
                  "perturb fsm: active + begin cleanup => cleanup");

    ok = lp_policy_glpk_perturb_next_state(next,
                                           LP_GLPK_PERTURB_EVENT_DISABLE,
                                           &next);
    ASSERT_INT_EQ(ok, 1, "perturb fsm: cleanup->disable is valid");
    ASSERT_INT_EQ(next, LP_GLPK_PERTURB_STATE_OFF,
                  "perturb fsm: cleanup + disable => off");

    ok = lp_policy_glpk_perturb_next_state(LP_GLPK_PERTURB_STATE_OFF,
                                           LP_GLPK_PERTURB_EVENT_BEGIN_CLEANUP,
                                           &next);
    ASSERT_INT_EQ(ok, 0, "perturb fsm: off->begin cleanup is invalid");
    ASSERT_INT_EQ(next, LP_GLPK_PERTURB_STATE_OFF,
                  "perturb fsm: invalid transition keeps state");

    ok = lp_policy_glpk_perturb_next_state(99,
                                           LP_GLPK_PERTURB_EVENT_ENABLE,
                                           &next);
    ASSERT_INT_EQ(ok, 0, "perturb fsm: invalid state rejected");
}

int main(void) {
    printf("=== LP GLPK-Compat Policy Tests ===\n");

    test_init_and_validate();
    test_profile_defaults();
    test_profile_defaults_glpk_strict();
    test_profile_defaults_glpk_legacy();
    test_basis_helpers();
    test_bfcp_runtime_support_helpers();
    test_runtime_mapping_noop_under_default_profile();
    test_runtime_mapping_glpk_profile();
    test_runtime_mapping_glpk_strict_profile();
    test_validation_rejects_invalid_values();
    test_working_lp_helpers();
    test_perturb_state_machine_helpers();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
