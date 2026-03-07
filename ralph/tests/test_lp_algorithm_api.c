/*
 * Tests for LP algorithm capability/fallback API surface.
 *
 * This module is intentionally separate from telemetry/logging tests.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "ralph_test_mod_api.h"

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

static RalphModel* build_small_lp(void) {
    RalphModel *model = ralph_test_create();
    if (!model) return NULL;

    ralph_test_set_obj_sense(model, RALPH_MINIMIZE);
    ralph_test_set_int_param(model, "detect_special", 0);
    ralph_test_set_int_param(model, "presolve", 0);

    /* min x, s.t. x >= 1, x >= 0 */
    ralph_test_add_var(model, 0.0, RALPH_INFINITY, 1.0, RALPH_CONTINUOUS);
    {
        int idx[] = {0};
        double val[] = {1.0};
        ralph_test_add_constraint(model, 1, idx, val, RALPH_GREATER_EQUAL, 1.0);
    }
    return model;
}

static void test_lp_capabilities(void) {
    RalphLPCapabilities caps;

    ASSERT_INT_EQ(ralph_lp_get_capabilities(NULL), -1,
                  "capabilities: NULL output rejected");

    memset(&caps, 0, sizeof(caps));
    ASSERT_INT_EQ(ralph_lp_get_capabilities(&caps), 0,
                  "capabilities: getter succeeds");
    ASSERT_INT_EQ(caps.supports_primal_simplex, 1,
                  "capabilities: primal supported");
    ASSERT_INT_EQ(caps.supports_dual_simplex, 1,
                  "capabilities: dual supported");
    ASSERT_INT_EQ(caps.supports_barrier, 0,
                  "capabilities: barrier unsupported");
    ASSERT_INT_EQ(caps.supports_crossover, 0,
                  "capabilities: crossover unsupported");
}

static void test_param_metadata_and_scope(void) {
    RalphModel *model = build_small_lp();
    RalphParamMeta meta;
    RalphParamId pid = RALPH_PARAM_COUNT;
    int value = -999;
    double dvalue = -1.0;

    ASSERT_TRUE(model != NULL, "params: model created");
    if (!model) return;

    memset(&meta, 0, sizeof(meta));
    ASSERT_INT_EQ(ralph_core_get_param_meta(RALPH_PARAM_LP_ALGORITHM, &meta), 0,
                  "params: metadata for lp_algorithm");
    ASSERT_TRUE(strcmp(meta.name, "lp_algorithm") == 0,
                "params: lp_algorithm canonical name");
    ASSERT_INT_EQ((int)meta.scope, (int)RALPH_PARAM_SCOPE_LP,
                  "params: lp_algorithm LP scope");
    ASSERT_INT_EQ((int)meta.value_type, (int)RALPH_PARAM_VALUE_INT,
                  "params: lp_algorithm int type");
    ASSERT_INT_EQ(meta.has_min, 1, "params: lp_algorithm has min");
    ASSERT_INT_EQ(meta.has_max, 1, "params: lp_algorithm has max");
    ASSERT_INT_EQ((int)meta.min_value, (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                  "params: lp_algorithm min");
    ASSERT_INT_EQ((int)meta.max_value, (int)RALPH_LP_ALGORITHM_BARRIER_EXTERNAL,
                  "params: lp_algorithm max");

    memset(&meta, 0, sizeof(meta));
    ASSERT_INT_EQ(ralph_core_get_param_meta(RALPH_PARAM_BARRIER_CROSSOVER, &meta), 0,
                  "params: metadata for barrier_crossover");
    ASSERT_TRUE(strcmp(meta.name, "barrier_crossover") == 0,
                "params: barrier_crossover canonical name");
    ASSERT_INT_EQ((int)meta.scope, (int)RALPH_PARAM_SCOPE_LP,
                  "params: barrier_crossover LP scope");
    ASSERT_INT_EQ(meta.has_min, 1, "params: barrier_crossover has min");
    ASSERT_INT_EQ(meta.has_max, 1, "params: barrier_crossover has max");
    ASSERT_INT_EQ((int)meta.min_value, (int)RALPH_LP_CROSSOVER_AUTO,
                  "params: barrier_crossover min");
    ASSERT_INT_EQ((int)meta.max_value, (int)RALPH_LP_CROSSOVER_ON,
                  "params: barrier_crossover max");

    memset(&meta, 0, sizeof(meta));
    ASSERT_INT_EQ(ralph_core_get_param_meta(RALPH_PARAM_LP_EXTERNAL_PROVIDER, &meta), 0,
                  "params: metadata for lp_external_provider");
    ASSERT_TRUE(strcmp(meta.name, "lp_external_provider") == 0,
                "params: lp_external_provider canonical name");
    ASSERT_INT_EQ((int)meta.scope, (int)RALPH_PARAM_SCOPE_LP,
                  "params: lp_external_provider LP scope");
    ASSERT_INT_EQ(meta.has_min, 1, "params: lp_external_provider has min");
    ASSERT_INT_EQ(meta.has_max, 1, "params: lp_external_provider has max");
    ASSERT_INT_EQ((int)meta.min_value, (int)RALPH_LP_EXTERNAL_PROVIDER_NONE,
                  "params: lp_external_provider min");
    ASSERT_INT_EQ((int)meta.max_value, (int)RALPH_LP_EXTERNAL_PROVIDER_GLOP,
                  "params: lp_external_provider max");

    memset(&meta, 0, sizeof(meta));
    ASSERT_INT_EQ(ralph_core_get_param_meta(RALPH_PARAM_LP_EXTERNAL_STRICT, &meta), 0,
                  "params: metadata for lp_external_strict");
    ASSERT_TRUE(strcmp(meta.name, "lp_external_strict") == 0,
                "params: lp_external_strict canonical name");
    ASSERT_INT_EQ((int)meta.scope, (int)RALPH_PARAM_SCOPE_LP,
                  "params: lp_external_strict LP scope");
    ASSERT_INT_EQ(meta.has_min, 1, "params: lp_external_strict has min");
    ASSERT_INT_EQ(meta.has_max, 1, "params: lp_external_strict has max");
    ASSERT_INT_EQ((int)meta.min_value, 0,
                  "params: lp_external_strict min");
    ASSERT_INT_EQ((int)meta.max_value, 1,
                  "params: lp_external_strict max");

    memset(&meta, 0, sizeof(meta));
    ASSERT_INT_EQ(ralph_core_get_param_meta(RALPH_PARAM_LP_BASIS_GOVERNOR_MODE, &meta), 0,
                  "params: metadata for lp_basis_governor_mode");
    ASSERT_TRUE(strcmp(meta.name, "lp_basis_governor_mode") == 0,
                "params: lp_basis_governor_mode canonical name");
    ASSERT_INT_EQ((int)meta.scope, (int)RALPH_PARAM_SCOPE_LP,
                  "params: lp_basis_governor_mode LP scope");
    ASSERT_INT_EQ(meta.has_min, 1, "params: lp_basis_governor_mode has min");
    ASSERT_INT_EQ(meta.has_max, 1, "params: lp_basis_governor_mode has max");
    ASSERT_INT_EQ((int)meta.min_value, 0,
                  "params: lp_basis_governor_mode min");
    ASSERT_INT_EQ((int)meta.max_value, 2,
                  "params: lp_basis_governor_mode max");

    memset(&meta, 0, sizeof(meta));
    ASSERT_INT_EQ(ralph_core_get_param_meta(RALPH_PARAM_LP_REINVERT_CONTROLLER_MODE, &meta), 0,
                  "params: metadata for lp_reinvert_controller_mode");
    ASSERT_TRUE(strcmp(meta.name, "lp_reinvert_controller_mode") == 0,
                "params: lp_reinvert_controller_mode canonical name");
    ASSERT_INT_EQ((int)meta.scope, (int)RALPH_PARAM_SCOPE_LP,
                  "params: lp_reinvert_controller_mode LP scope");
    ASSERT_INT_EQ(meta.has_min, 1, "params: lp_reinvert_controller_mode has min");
    ASSERT_INT_EQ(meta.has_max, 1, "params: lp_reinvert_controller_mode has max");
    ASSERT_INT_EQ((int)meta.min_value, (int)RALPH_LP_REINVERT_CONTROLLER_MODE_OFF,
                  "params: lp_reinvert_controller_mode min");
    ASSERT_INT_EQ((int)meta.max_value, (int)RALPH_LP_REINVERT_CONTROLLER_MODE_CONTROL_ALL,
                  "params: lp_reinvert_controller_mode max");

    memset(&meta, 0, sizeof(meta));
    ASSERT_INT_EQ(ralph_core_get_param_meta(RALPH_PARAM_LP_POLICY_PROFILE, &meta), 0,
                  "params: metadata for lp_policy_profile");
    ASSERT_TRUE(strcmp(meta.name, "lp_policy_profile") == 0,
                "params: lp_policy_profile canonical name");
    ASSERT_INT_EQ((int)meta.scope, (int)RALPH_PARAM_SCOPE_LP,
                  "params: lp_policy_profile LP scope");
    ASSERT_INT_EQ(meta.has_min, 1, "params: lp_policy_profile has min");
    ASSERT_INT_EQ(meta.has_max, 1, "params: lp_policy_profile has max");
    ASSERT_INT_EQ((int)meta.min_value, (int)RALPH_LP_POLICY_PROFILE_DEFAULT,
                  "params: lp_policy_profile min");
    ASSERT_INT_EQ((int)meta.max_value, (int)RALPH_LP_POLICY_PROFILE_GLPK_LEGACY,
                  "params: lp_policy_profile max");

    memset(&meta, 0, sizeof(meta));
    ASSERT_INT_EQ(ralph_core_get_param_meta(RALPH_PARAM_GLPK_SMCP_METHOD, &meta), 0,
                  "params: metadata for glpk_smcp_method");
    ASSERT_TRUE(strcmp(meta.name, "glpk_smcp_method") == 0,
                "params: glpk_smcp_method canonical name");
    ASSERT_INT_EQ((int)meta.scope, (int)RALPH_PARAM_SCOPE_LP,
                  "params: glpk_smcp_method LP scope");
    ASSERT_INT_EQ(meta.has_min, 1, "params: glpk_smcp_method has min");
    ASSERT_INT_EQ(meta.has_max, 1, "params: glpk_smcp_method has max");
    ASSERT_INT_EQ((int)meta.min_value, (int)RALPH_LP_GLPK_SMCP_METHOD_AUTO,
                  "params: glpk_smcp_method min");
    ASSERT_INT_EQ((int)meta.max_value, (int)RALPH_LP_GLPK_SMCP_METHOD_DUAL,
                  "params: glpk_smcp_method max");

    memset(&meta, 0, sizeof(meta));
    ASSERT_INT_EQ(ralph_core_get_param_meta(RALPH_PARAM_GLPK_SMCP_BASIS, &meta), 0,
                  "params: metadata for glpk_smcp_basis");
    ASSERT_TRUE(strcmp(meta.name, "glpk_smcp_basis") == 0,
                "params: glpk_smcp_basis canonical name");
    ASSERT_INT_EQ((int)meta.scope, (int)RALPH_PARAM_SCOPE_LP,
                  "params: glpk_smcp_basis LP scope");
    ASSERT_INT_EQ(meta.has_min, 1, "params: glpk_smcp_basis has min");
    ASSERT_INT_EQ(meta.has_max, 1, "params: glpk_smcp_basis has max");
    ASSERT_INT_EQ((int)meta.min_value, (int)RALPH_LP_GLPK_SMCP_BASIS_ADV,
                  "params: glpk_smcp_basis min");
    ASSERT_INT_EQ((int)meta.max_value, (int)RALPH_LP_GLPK_SMCP_BASIS_INI,
                  "params: glpk_smcp_basis max");

    memset(&meta, 0, sizeof(meta));
    ASSERT_INT_EQ(ralph_core_get_param_meta(RALPH_PARAM_GLPK_BFCP_FACTORIZATION, &meta), 0,
                  "params: metadata for glpk_bfcp_factorization");
    ASSERT_TRUE(strcmp(meta.name, "glpk_bfcp_factorization") == 0,
                "params: glpk_bfcp_factorization canonical name");
    ASSERT_INT_EQ((int)meta.value_type, (int)RALPH_PARAM_VALUE_INT,
                  "params: glpk_bfcp_factorization int type");

    memset(&meta, 0, sizeof(meta));
    ASSERT_INT_EQ(ralph_core_get_param_meta(RALPH_PARAM_GLPK_BFCP_PIVOT_LIMIT, &meta), 0,
                  "params: metadata for glpk_bfcp_pivot_limit");
    ASSERT_TRUE(strcmp(meta.name, "glpk_bfcp_pivot_limit") == 0,
                "params: glpk_bfcp_pivot_limit canonical name");
    ASSERT_INT_EQ((int)meta.value_type, (int)RALPH_PARAM_VALUE_INT,
                  "params: glpk_bfcp_pivot_limit int type");

    memset(&meta, 0, sizeof(meta));
    ASSERT_INT_EQ(ralph_core_get_param_meta(RALPH_PARAM_GLPK_BFCP_SUHL, &meta), 0,
                  "params: metadata for glpk_bfcp_suhl");
    ASSERT_TRUE(strcmp(meta.name, "glpk_bfcp_suhl") == 0,
                "params: glpk_bfcp_suhl canonical name");
    ASSERT_INT_EQ((int)meta.value_type, (int)RALPH_PARAM_VALUE_INT,
                  "params: glpk_bfcp_suhl int type");

    memset(&meta, 0, sizeof(meta));
    ASSERT_INT_EQ(ralph_core_get_param_meta(RALPH_PARAM_GLPK_BFCP_PIVOT_TOL, &meta), 0,
                  "params: metadata for glpk_bfcp_pivot_tol");
    ASSERT_TRUE(strcmp(meta.name, "glpk_bfcp_pivot_tol") == 0,
                "params: glpk_bfcp_pivot_tol canonical name");
    ASSERT_INT_EQ((int)meta.value_type, (int)RALPH_PARAM_VALUE_DOUBLE,
                  "params: glpk_bfcp_pivot_tol double type");

    memset(&meta, 0, sizeof(meta));
    ASSERT_INT_EQ(ralph_core_get_param_meta(RALPH_PARAM_GLPK_BFCP_EPS_TOL, &meta), 0,
                  "params: metadata for glpk_bfcp_eps_tol");
    ASSERT_TRUE(strcmp(meta.name, "glpk_bfcp_eps_tol") == 0,
                "params: glpk_bfcp_eps_tol canonical name");
    ASSERT_INT_EQ((int)meta.value_type, (int)RALPH_PARAM_VALUE_DOUBLE,
                  "params: glpk_bfcp_eps_tol double type");

    memset(&meta, 0, sizeof(meta));
    ASSERT_INT_EQ(ralph_core_get_param_meta(RALPH_PARAM_GLPK_BFCP_NFS_MAX, &meta), 0,
                  "params: metadata for glpk_bfcp_nfs_max");
    ASSERT_TRUE(strcmp(meta.name, "glpk_bfcp_nfs_max") == 0,
                "params: glpk_bfcp_nfs_max canonical name");
    ASSERT_INT_EQ((int)meta.value_type, (int)RALPH_PARAM_VALUE_INT,
                  "params: glpk_bfcp_nfs_max int type");

    memset(&meta, 0, sizeof(meta));
    ASSERT_INT_EQ(ralph_core_get_param_meta(RALPH_PARAM_GLPK_BFCP_NRS_MAX, &meta), 0,
                  "params: metadata for glpk_bfcp_nrs_max");
    ASSERT_TRUE(strcmp(meta.name, "glpk_bfcp_nrs_max") == 0,
                "params: glpk_bfcp_nrs_max canonical name");
    ASSERT_INT_EQ((int)meta.value_type, (int)RALPH_PARAM_VALUE_INT,
                  "params: glpk_bfcp_nrs_max int type");

    memset(&meta, 0, sizeof(meta));
    ASSERT_INT_EQ(ralph_core_get_param_meta(RALPH_PARAM_GLPK_SMCP_TOL_BND, &meta), 0,
                  "params: metadata for glpk_smcp_tol_bnd");
    ASSERT_TRUE(strcmp(meta.name, "glpk_smcp_tol_bnd") == 0,
                "params: glpk_smcp_tol_bnd canonical name");
    ASSERT_INT_EQ((int)meta.value_type, (int)RALPH_PARAM_VALUE_DOUBLE,
                  "params: glpk_smcp_tol_bnd double type");

    memset(&meta, 0, sizeof(meta));
    ASSERT_INT_EQ(ralph_core_get_param_meta(RALPH_PARAM_GLPK_SMCP_EXCL, &meta), 0,
                  "params: metadata for glpk_smcp_excl");
    ASSERT_TRUE(strcmp(meta.name, "glpk_smcp_excl") == 0,
                "params: glpk_smcp_excl canonical name");
    ASSERT_INT_EQ((int)meta.min_value, (int)RALPH_LP_GLPK_SMCP_EXCL_OFF,
                  "params: glpk_smcp_excl min");
    ASSERT_INT_EQ((int)meta.max_value, (int)RALPH_LP_GLPK_SMCP_EXCL_ON,
                  "params: glpk_smcp_excl max");

    ASSERT_INT_EQ(ralph_core_find_param_by_name("lp_algorithm", &pid), 0,
                  "params: find lp_algorithm canonical");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_LP_ALGORITHM,
                  "params: lp_algorithm canonical id");
    ASSERT_INT_EQ(ralph_core_find_param_by_name("LPAlgorithm", &pid), 0,
                  "params: find lp_algorithm alias");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_LP_ALGORITHM,
                  "params: lp_algorithm alias id");
    ASSERT_INT_EQ(ralph_core_find_param_by_name("lp_external_provider", &pid), 0,
                  "params: find lp_external_provider canonical");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                  "params: lp_external_provider canonical id");
    ASSERT_INT_EQ(ralph_core_find_param_by_name("LPExternalProvider", &pid), 0,
                  "params: find lp_external_provider alias");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                  "params: lp_external_provider alias id");
    ASSERT_INT_EQ(ralph_core_find_param_by_name("lp_external_strict", &pid), 0,
                  "params: find lp_external_strict canonical");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_LP_EXTERNAL_STRICT,
                  "params: lp_external_strict canonical id");
    ASSERT_INT_EQ(ralph_core_find_param_by_name("LPExternalStrict", &pid), 0,
                  "params: find lp_external_strict alias");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_LP_EXTERNAL_STRICT,
                  "params: lp_external_strict alias id");
    ASSERT_INT_EQ(ralph_core_find_param_by_name("lp_basis_governor_mode", &pid), 0,
                  "params: find lp_basis_governor_mode canonical");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_LP_BASIS_GOVERNOR_MODE,
                  "params: lp_basis_governor_mode canonical id");
    ASSERT_INT_EQ(ralph_core_find_param_by_name("LPBasisGovernorMode", &pid), 0,
                  "params: find lp_basis_governor_mode alias");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_LP_BASIS_GOVERNOR_MODE,
                  "params: lp_basis_governor_mode alias id");
    ASSERT_INT_EQ(ralph_core_find_param_by_name("lp_reinvert_controller_mode", &pid), 0,
                  "params: find lp_reinvert_controller_mode canonical");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_LP_REINVERT_CONTROLLER_MODE,
                  "params: lp_reinvert_controller_mode canonical id");
    ASSERT_INT_EQ(ralph_core_find_param_by_name("LPReinvertControllerMode", &pid), 0,
                  "params: find lp_reinvert_controller_mode alias");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_LP_REINVERT_CONTROLLER_MODE,
                  "params: lp_reinvert_controller_mode alias id");
    ASSERT_INT_EQ(ralph_core_find_param_by_name("lp_policy_profile", &pid), 0,
                  "params: find lp_policy_profile canonical");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_LP_POLICY_PROFILE,
                  "params: lp_policy_profile canonical id");
    ASSERT_INT_EQ(ralph_core_find_param_by_name("LPPolicyProfile", &pid), 0,
                  "params: find lp_policy_profile alias");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_LP_POLICY_PROFILE,
                  "params: lp_policy_profile alias id");
    ASSERT_INT_EQ(ralph_core_find_param_by_name("GLPKSMCPExcl", &pid), 0,
                  "params: find glpk_smcp_excl alias");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_GLPK_SMCP_EXCL,
                  "params: glpk_smcp_excl alias id");
    ASSERT_INT_EQ(ralph_core_find_param_by_name("GLPKBFCPFactorization", &pid), 0,
                  "params: find glpk_bfcp_factorization alias");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_GLPK_BFCP_FACTORIZATION,
                  "params: glpk_bfcp_factorization alias id");
    ASSERT_INT_EQ(ralph_core_find_param_by_name("GLPKBFCPPivotLimit", &pid), 0,
                  "params: find glpk_bfcp_pivot_limit alias");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_GLPK_BFCP_PIVOT_LIMIT,
                  "params: glpk_bfcp_pivot_limit alias id");
    ASSERT_INT_EQ(ralph_core_find_param_by_name("GLPKBFCPSuhl", &pid), 0,
                  "params: find glpk_bfcp_suhl alias");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_GLPK_BFCP_SUHL,
                  "params: glpk_bfcp_suhl alias id");
    ASSERT_INT_EQ(ralph_core_find_param_by_name("glpk_bfcp_growth_guard", &pid), 0,
                  "params: find glpk_bfcp_growth_guard canonical");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_GLPK_BFCP_GROWTH_GUARD,
                  "params: glpk_bfcp_growth_guard canonical id");
    ASSERT_INT_EQ(ralph_core_find_param_by_name("GLPKBFCPGrowthGuard", &pid), 0,
                  "params: find glpk_bfcp_growth_guard alias");
    ASSERT_INT_EQ((int)pid, (int)RALPH_PARAM_GLPK_BFCP_GROWTH_GUARD,
                  "params: glpk_bfcp_growth_guard alias id");

    ASSERT_INT_EQ(ralph_core_set_mip_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                             (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX),
                  -1,
                  "params: MIP strict rejects LP algorithm id");
    ASSERT_INT_EQ(ralph_core_set_mip_int_param_id(model, RALPH_PARAM_BARRIER_CROSSOVER,
                                             (int)RALPH_LP_CROSSOVER_AUTO),
                  -1,
                  "params: MIP strict rejects barrier crossover id");
    ASSERT_INT_EQ(ralph_core_set_mip_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                             (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                  -1,
                  "params: MIP strict rejects lp_external_provider id");
    ASSERT_INT_EQ(ralph_core_set_mip_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_STRICT, 1),
                  -1,
                  "params: MIP strict rejects lp_external_strict id");
    ASSERT_INT_EQ(ralph_core_set_mip_int_param_id(model, RALPH_PARAM_LP_BASIS_GOVERNOR_MODE, 1),
                  -1,
                  "params: MIP strict rejects lp_basis_governor_mode id");
    ASSERT_INT_EQ(ralph_core_set_mip_int_param_id(model, RALPH_PARAM_LP_REINVERT_CONTROLLER_MODE, 1),
                  -1,
                  "params: MIP strict rejects lp_reinvert_controller_mode id");
    ASSERT_INT_EQ(ralph_core_set_mip_int_param_id(model, RALPH_PARAM_LP_POLICY_PROFILE,
                                             (int)RALPH_LP_POLICY_PROFILE_GLPK_COMPAT),
                  -1,
                  "params: MIP strict rejects lp_policy_profile id");
    ASSERT_INT_EQ(ralph_core_set_mip_dbl_param_id(model, RALPH_PARAM_GLPK_BFCP_PIVOT_TOL, 1e-8),
                  -1,
                  "params: MIP strict rejects glpk_bfcp_pivot_tol id");

    ASSERT_INT_EQ(ralph_core_set_lp_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                            (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX),
                  0,
                  "params: LP strict accepts lp_algorithm");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_METHOD, &value), 0,
                  "params: read legacy method after lp_algorithm");
    ASSERT_INT_EQ(value, 1,
                  "params: method syncs with lp_algorithm<=auto");

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_METHOD, 0), 0,
                  "params: set legacy method");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_LP_ALGORITHM, &value), 0,
                  "params: read lp_algorithm after method");
    ASSERT_INT_EQ(value, (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                  "params: lp_algorithm syncs from method");

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_BARRIER),
                  0,
                  "params: barrier algorithm request accepted");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_METHOD, &value), 0,
                  "params: read method after barrier request");
    ASSERT_INT_EQ(value, (int)RALPH_LP_ALGORITHM_AUTO,
                  "params: method remains auto when barrier requested");

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                  0,
                  "params: external primal algorithm request accepted");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_METHOD, &value), 0,
                  "params: read method after external primal request");
    ASSERT_INT_EQ(value, (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                  "params: method maps to primal for external primal");

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX_EXTERNAL),
                  0,
                  "params: external dual algorithm request accepted");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_METHOD, &value), 0,
                  "params: read method after external dual request");
    ASSERT_INT_EQ(value, (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX,
                  "params: method maps to dual for external dual");

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                  0,
                  "params: set lp_external_provider by id");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER, &value), 0,
                  "params: get lp_external_provider by id");
    ASSERT_INT_EQ(value, (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK,
                  "params: lp_external_provider set/get consistent");

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM, 7), -1,
                  "params: reject lp_algorithm out of range");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_BARRIER_CROSSOVER, 3), -1,
                  "params: reject barrier_crossover out of range");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER, 7), -1,
                  "params: reject lp_external_provider out of range");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_STRICT, 2), -1,
                  "params: reject lp_external_strict out of range");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_BASIS_GOVERNOR_MODE, 3), -1,
                  "params: reject lp_basis_governor_mode out of range");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_REINVERT_CONTROLLER_MODE, 4), -1,
                  "params: reject lp_reinvert_controller_mode out of range");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_POLICY_PROFILE, 4), -1,
                  "params: reject lp_policy_profile out of range");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_SMCP_METHOD, 4), -1,
                  "params: reject glpk_smcp_method out of range");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_SMCP_BASIS, 4), -1,
                  "params: reject glpk_smcp_basis out of range");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_SMCP_EXCL, 2), -1,
                  "params: reject glpk_smcp_excl out of range");
    ASSERT_INT_EQ(ralph_core_set_dbl_param_id(model, RALPH_PARAM_GLPK_SMCP_TOL_BND, 0.0), -1,
                  "params: reject glpk_smcp_tol_bnd non-positive");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_BFCP_FACTORIZATION, 2), -1,
                  "params: reject glpk_bfcp_factorization out of range");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_BFCP_UPDATE_LIMIT, -2), -1,
                  "params: reject glpk_bfcp_update_limit out of range");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_BFCP_PIVOT_LIMIT, -2), -1,
                  "params: reject glpk_bfcp_pivot_limit out of range");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_BFCP_SUHL, 2), -1,
                  "params: reject glpk_bfcp_suhl out of range");
    ASSERT_INT_EQ(ralph_core_set_dbl_param_id(model, RALPH_PARAM_GLPK_BFCP_PIVOT_TOL, INFINITY), -1,
                  "params: reject glpk_bfcp_pivot_tol non-finite");
    ASSERT_INT_EQ(ralph_core_set_dbl_param_id(model, RALPH_PARAM_GLPK_BFCP_EPS_TOL, INFINITY), -1,
                  "params: reject glpk_bfcp_eps_tol non-finite");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_BFCP_NFS_MAX, -2), -1,
                  "params: reject glpk_bfcp_nfs_max out of range");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_BFCP_NRS_MAX, -2), -1,
                  "params: reject glpk_bfcp_nrs_max out of range");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_BASIS_GOVERNOR_MODE, 2), 0,
                  "params: set lp_basis_governor_mode by id");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_LP_BASIS_GOVERNOR_MODE, &value), 0,
                  "params: get lp_basis_governor_mode by id");
    ASSERT_INT_EQ(value, 2,
                  "params: lp_basis_governor_mode set/get consistent");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_REINVERT_CONTROLLER_MODE, 3), 0,
                  "params: set lp_reinvert_controller_mode by id");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_LP_REINVERT_CONTROLLER_MODE, &value), 0,
                  "params: get lp_reinvert_controller_mode by id");
    ASSERT_INT_EQ(value, 3,
                  "params: lp_reinvert_controller_mode set/get consistent");

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_POLICY_PROFILE,
                                         (int)RALPH_LP_POLICY_PROFILE_GLPK_COMPAT),
                  0,
                  "params: set lp_policy_profile=glpk_compat");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_GLPK_SMCP_METHOD, &value), 0,
                  "params: get glpk_smcp_method after profile apply");
    ASSERT_INT_EQ(value, (int)RALPH_LP_GLPK_SMCP_METHOD_PRIMAL,
                  "params: profile defaults glpk_smcp_method=primal");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_GLPK_SMCP_PRICING, &value), 0,
                  "params: get glpk_smcp_pricing after profile apply");
    ASSERT_INT_EQ(value, (int)RALPH_LP_GLPK_SMCP_PRICING_STEEP,
                  "params: profile defaults glpk_smcp_pricing=steep");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_GLPK_SMCP_PRESOLVE, &value), 0,
                  "params: get glpk_smcp_presolve after profile apply");
    ASSERT_INT_EQ(value, (int)RALPH_LP_GLPK_SMCP_PRESOLVE_ON,
                  "params: profile defaults glpk_smcp_presolve=on");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_GLPK_BFCP_UPDATE_LIMIT, &value), 0,
                  "params: get glpk_bfcp_update_limit after profile apply");
    ASSERT_INT_EQ(value, 100,
                  "params: profile defaults glpk_bfcp_update_limit=100 when auto");

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_SMCP_METHOD,
                                         (int)RALPH_LP_GLPK_SMCP_METHOD_DUALP),
                  0,
                  "params: override glpk_smcp_method=dualp after profile");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_GLPK_SMCP_METHOD, &value), 0,
                  "params: get glpk_smcp_method dualp override");
    ASSERT_INT_EQ(value, (int)RALPH_LP_GLPK_SMCP_METHOD_DUALP,
                  "params: explicit dualp override persists after profile");

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_SMCP_METHOD,
                                         (int)RALPH_LP_GLPK_SMCP_METHOD_DUAL),
                  0,
                  "params: override glpk_smcp_method after profile");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_GLPK_SMCP_METHOD, &value), 0,
                  "params: get glpk_smcp_method override");
    ASSERT_INT_EQ(value, (int)RALPH_LP_GLPK_SMCP_METHOD_DUAL,
                  "params: explicit override persists after profile");

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_SMCP_BASIS,
                                         (int)RALPH_LP_GLPK_SMCP_BASIS_BIB),
                  0,
                  "params: set glpk_smcp_basis=bib");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_GLPK_SMCP_BASIS, &value), 0,
                  "params: get glpk_smcp_basis=bib");
    ASSERT_INT_EQ(value, (int)RALPH_LP_GLPK_SMCP_BASIS_BIB,
                  "params: glpk_smcp_basis=bib persists");

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_SMCP_BASIS,
                                         (int)RALPH_LP_GLPK_SMCP_BASIS_INI),
                  0,
                  "params: set glpk_smcp_basis=ini");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_GLPK_SMCP_BASIS, &value), 0,
                  "params: get glpk_smcp_basis=ini");
    ASSERT_INT_EQ(value, (int)RALPH_LP_GLPK_SMCP_BASIS_INI,
                  "params: glpk_smcp_basis=ini persists");

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_BFCP_UPDATE_LIMIT, -1), 0,
                  "params: set glpk_bfcp_update_limit auto");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_GLPK_BFCP_UPDATE_LIMIT, &value), 0,
                  "params: get glpk_bfcp_update_limit auto");
    ASSERT_INT_EQ(value, -1,
                  "params: glpk_bfcp_update_limit auto set/get");

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_BFCP_FACTORIZATION,
                                         (int)RALPH_LP_GLPK_BFCP_FACTORIZATION_BTF),
                  0,
                  "params: set glpk_bfcp_factorization");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_GLPK_BFCP_FACTORIZATION, &value), 0,
                  "params: get glpk_bfcp_factorization");
    ASSERT_INT_EQ(value, (int)RALPH_LP_GLPK_BFCP_FACTORIZATION_BTF,
                  "params: glpk_bfcp_factorization set/get consistent");

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_BFCP_PIVOT_LIMIT, 4), 0,
                  "params: set glpk_bfcp_pivot_limit");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_GLPK_BFCP_PIVOT_LIMIT, &value), 0,
                  "params: get glpk_bfcp_pivot_limit");
    ASSERT_INT_EQ(value, 4,
                  "params: glpk_bfcp_pivot_limit set/get consistent");

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_BFCP_SUHL,
                                         (int)RALPH_LP_GLPK_BFCP_SUHL_ON),
                  0,
                  "params: set glpk_bfcp_suhl");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_GLPK_BFCP_SUHL, &value), 0,
                  "params: get glpk_bfcp_suhl");
    ASSERT_INT_EQ(value, (int)RALPH_LP_GLPK_BFCP_SUHL_ON,
                  "params: glpk_bfcp_suhl set/get consistent");

    ASSERT_INT_EQ(ralph_core_set_dbl_param_id(model, RALPH_PARAM_GLPK_BFCP_PIVOT_TOL, 1e-8), 0,
                  "params: set glpk_bfcp_pivot_tol");
    ASSERT_INT_EQ(ralph_core_get_dbl_param_id(model, RALPH_PARAM_GLPK_BFCP_PIVOT_TOL, &dvalue), 0,
                  "params: get glpk_bfcp_pivot_tol");
    ASSERT_TRUE(fabs(dvalue - 1e-8) < 1e-14,
                "params: glpk_bfcp_pivot_tol set/get consistent");

    ASSERT_INT_EQ(ralph_core_set_dbl_param_id(model, RALPH_PARAM_GLPK_BFCP_GROWTH_GUARD, 1e6), 0,
                  "params: set glpk_bfcp_growth_guard");
    ASSERT_INT_EQ(ralph_core_get_dbl_param_id(model, RALPH_PARAM_GLPK_BFCP_GROWTH_GUARD, &dvalue), 0,
                  "params: get glpk_bfcp_growth_guard");
    ASSERT_TRUE(fabs(dvalue - 1e6) < 1e-6,
                "params: glpk_bfcp_growth_guard set/get consistent");

    ASSERT_INT_EQ(ralph_core_set_dbl_param_id(model, RALPH_PARAM_GLPK_BFCP_EPS_TOL, 1e-15), 0,
                  "params: set glpk_bfcp_eps_tol");
    ASSERT_INT_EQ(ralph_core_get_dbl_param_id(model, RALPH_PARAM_GLPK_BFCP_EPS_TOL, &dvalue), 0,
                  "params: get glpk_bfcp_eps_tol");
    ASSERT_TRUE(fabs(dvalue - 1e-15) < 1e-21,
                "params: glpk_bfcp_eps_tol set/get consistent");

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_BFCP_NFS_MAX, 50), 0,
                  "params: set glpk_bfcp_nfs_max");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_GLPK_BFCP_NFS_MAX, &value), 0,
                  "params: get glpk_bfcp_nfs_max");
    ASSERT_INT_EQ(value, 50,
                  "params: glpk_bfcp_nfs_max set/get consistent");

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_BFCP_NRS_MAX, 60), 0,
                  "params: set glpk_bfcp_nrs_max");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_GLPK_BFCP_NRS_MAX, &value), 0,
                  "params: get glpk_bfcp_nrs_max");
    ASSERT_INT_EQ(value, 60,
                  "params: glpk_bfcp_nrs_max set/get consistent");

    ASSERT_INT_EQ(ralph_core_set_dbl_param_id(model, RALPH_PARAM_GLPK_SMCP_TOL_BND, 2e-7), 0,
                  "params: set glpk_smcp_tol_bnd");
    ASSERT_INT_EQ(ralph_core_get_dbl_param_id(model, RALPH_PARAM_GLPK_SMCP_TOL_BND, &dvalue), 0,
                  "params: get glpk_smcp_tol_bnd");
    ASSERT_TRUE(fabs(dvalue - 2e-7) < 1e-16,
                "params: glpk_smcp_tol_bnd set/get consistent");

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_SMCP_EXCL,
                                         (int)RALPH_LP_GLPK_SMCP_EXCL_OFF),
                  0,
                  "params: set glpk_smcp_excl");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_GLPK_SMCP_EXCL, &value), 0,
                  "params: get glpk_smcp_excl");
    ASSERT_INT_EQ(value, (int)RALPH_LP_GLPK_SMCP_EXCL_OFF,
                  "params: glpk_smcp_excl set/get consistent");

    ASSERT_INT_EQ(ralph_test_set_int_param(model, "barrier_crossover",
                                      (int)RALPH_LP_CROSSOVER_ON),
                  0,
                  "params: set barrier_crossover by string");
    ASSERT_INT_EQ(ralph_core_get_int_param_id(model, RALPH_PARAM_BARRIER_CROSSOVER, &value), 0,
                  "params: get barrier_crossover by id");
    ASSERT_INT_EQ(value, (int)RALPH_LP_CROSSOVER_ON,
                  "params: barrier_crossover set/get consistent");

    ralph_test_free(model);
}

static void test_algorithm_report_guards_and_invalidation(void) {
    RalphModel *model = build_small_lp();
    RalphLPSolveAlgorithmReport report;

    ASSERT_TRUE(model != NULL, "report: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_lp_get_last_algorithm_report(model, &report), -1,
                  "report: unavailable before solve");

    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0,
                  "report: LP optimize succeeds");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_OPTIMAL,
                  "report: LP status optimal");

    ASSERT_INT_EQ(ralph_lp_get_last_algorithm_report(model, &report), 0,
                  "report: available after LP solve");
    ASSERT_INT_EQ((int)report.requested_algorithm,
                  (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                  "report: default requested algorithm");
    ASSERT_INT_EQ((int)report.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                  "report: default effective algorithm");
    ASSERT_INT_EQ(report.fallback_applied, 0,
                  "report: no fallback for default algorithm");

    ASSERT_INT_EQ(ralph_test_set_obj_coef(model, 0, 2.0), 0,
                  "report: mutate model invalidates state");
    ASSERT_INT_EQ(ralph_lp_get_last_algorithm_report(model, &report), -1,
                  "report: unavailable after invalidation");

    ralph_test_free(model);
}

static void test_barrier_fallback_report(void) {
    RalphModel *model = build_small_lp();
    RalphLPSolveAlgorithmReport report;

    ASSERT_TRUE(model != NULL, "barrier: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_BARRIER),
                  0,
                  "barrier: request barrier algorithm");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_BARRIER_CROSSOVER,
                                         (int)RALPH_LP_CROSSOVER_ON),
                  0,
                  "barrier: request crossover ON");

    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0,
                  "barrier: LP optimize succeeds");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_OPTIMAL,
                  "barrier: LP status optimal");
    ASSERT_INT_EQ(ralph_lp_get_last_algorithm_report(model, &report), 0,
                  "barrier: report available");

    ASSERT_INT_EQ((int)report.requested_algorithm,
                  (int)RALPH_LP_ALGORITHM_BARRIER,
                  "barrier: requested algorithm captured");
    ASSERT_INT_EQ((int)report.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_AUTO,
                  "barrier: effective algorithm fallback to auto");
    ASSERT_INT_EQ((int)report.requested_crossover,
                  (int)RALPH_LP_CROSSOVER_ON,
                  "barrier: requested crossover captured");
    ASSERT_INT_EQ((int)report.effective_crossover,
                  (int)RALPH_LP_CROSSOVER_AUTO,
                  "barrier: effective crossover fallback to auto");
    ASSERT_INT_EQ(report.fallback_applied, 1,
                  "barrier: fallback marked as applied");
    ASSERT_INT_EQ((int)report.fallback_reason,
                  (int)RALPH_LP_FALLBACK_BARRIER_UNAVAILABLE,
                  "barrier: fallback reason is barrier unavailable");

    ralph_test_free(model);
}

static void test_crossover_only_fallback_report(void) {
    RalphModel *model = build_small_lp();
    RalphLPSolveAlgorithmReport report;

    ASSERT_TRUE(model != NULL, "crossover: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX),
                  0,
                  "crossover: primal algorithm requested");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_BARRIER_CROSSOVER,
                                         (int)RALPH_LP_CROSSOVER_ON),
                  0,
                  "crossover: request crossover ON");

    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0,
                  "crossover: LP optimize succeeds");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_OPTIMAL,
                  "crossover: LP status optimal");
    ASSERT_INT_EQ(ralph_lp_get_last_algorithm_report(model, &report), 0,
                  "crossover: report available");

    ASSERT_INT_EQ((int)report.requested_algorithm,
                  (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                  "crossover: requested algorithm captured");
    ASSERT_INT_EQ((int)report.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                  "crossover: effective algorithm unchanged");
    ASSERT_INT_EQ((int)report.requested_crossover,
                  (int)RALPH_LP_CROSSOVER_ON,
                  "crossover: requested crossover captured");
    ASSERT_INT_EQ((int)report.effective_crossover,
                  (int)RALPH_LP_CROSSOVER_AUTO,
                  "crossover: effective crossover fallback to auto");
    ASSERT_INT_EQ(report.fallback_applied, 1,
                  "crossover: fallback marked as applied");
    ASSERT_INT_EQ((int)report.fallback_reason,
                  (int)RALPH_LP_FALLBACK_CROSSOVER_UNAVAILABLE,
                  "crossover: fallback reason is crossover unavailable");

    ralph_test_free(model);
}

static void test_external_fallback_report(void) {
    RalphModel *model = build_small_lp();
    RalphLPSolveAlgorithmReport report;

    ASSERT_TRUE(model != NULL, "external-fallback: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                  0,
                  "external-fallback: request external primal algorithm");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                  0,
                  "external-fallback: request external provider GLPK");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_BARRIER_CROSSOVER,
                                         (int)RALPH_LP_CROSSOVER_ON),
                  0,
                  "external-fallback: request crossover ON");

    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0,
                  "external-fallback: LP optimize succeeds");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_OPTIMAL,
                  "external-fallback: LP status optimal");
    ASSERT_INT_EQ(ralph_lp_get_last_algorithm_report(model, &report), 0,
                  "external-fallback: report available");

    ASSERT_INT_EQ((int)report.requested_algorithm,
                  (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL,
                  "external-fallback: requested algorithm captured");
    ASSERT_INT_EQ((int)report.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
                  "external-fallback: effective algorithm fallback to internal primal");
    ASSERT_INT_EQ((int)report.requested_crossover,
                  (int)RALPH_LP_CROSSOVER_ON,
                  "external-fallback: requested crossover captured");
    ASSERT_INT_EQ((int)report.effective_crossover,
                  (int)RALPH_LP_CROSSOVER_AUTO,
                  "external-fallback: effective crossover fallback to auto");
    ASSERT_INT_EQ(report.fallback_applied, 1,
                  "external-fallback: fallback applied");
    ASSERT_INT_EQ((int)report.fallback_reason,
                  (int)RALPH_LP_FALLBACK_EXTERNAL_UNAVAILABLE,
                  "external-fallback: fallback reason external unavailable");

    ralph_test_free(model);
}

static void test_external_strict_mode_error(void) {
    RalphModel *model = build_small_lp();

    ASSERT_TRUE(model != NULL, "external-strict: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_ALGORITHM,
                                         (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL),
                  0,
                  "external-strict: request external primal algorithm");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_PROVIDER,
                                         (int)RALPH_LP_EXTERNAL_PROVIDER_GLPK),
                  0,
                  "external-strict: request external provider GLPK");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_EXTERNAL_STRICT, 1),
                  0,
                  "external-strict: enable strict mode");
    ASSERT_INT_EQ(ralph_test_optimize_lp(model), -1,
                  "external-strict: optimize fails when external backend unavailable");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_ERROR,
                  "external-strict: status is ERROR");

    ralph_test_free(model);
}

static void test_legacy_method_dispatch_report(void) {
    RalphModel *model = build_small_lp();
    RalphLPSolveAlgorithmReport report;

    ASSERT_TRUE(model != NULL, "legacy-dispatch: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_test_set_int_param(model, "method", 1), 0,
                  "legacy-dispatch: set method=dual");
    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0,
                  "legacy-dispatch: solve with dual method succeeds");
    ASSERT_INT_EQ(ralph_lp_get_last_algorithm_report(model, &report), 0,
                  "legacy-dispatch: report available for dual method");
    ASSERT_INT_EQ((int)report.requested_algorithm,
                  (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX,
                  "legacy-dispatch: requested algorithm reflects dual method");
    ASSERT_INT_EQ((int)report.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_DUAL_SIMPLEX,
                  "legacy-dispatch: effective algorithm remains dual");
    ASSERT_INT_EQ(report.fallback_applied, 0,
                  "legacy-dispatch: no fallback for dual method");

    ASSERT_INT_EQ(ralph_test_set_int_param(model, "method", 2), 0,
                  "legacy-dispatch: set method=auto");
    ASSERT_INT_EQ(ralph_test_optimize_lp(model), 0,
                  "legacy-dispatch: solve with auto method succeeds");
    ASSERT_INT_EQ(ralph_lp_get_last_algorithm_report(model, &report), 0,
                  "legacy-dispatch: report available for auto method");
    ASSERT_INT_EQ((int)report.requested_algorithm,
                  (int)RALPH_LP_ALGORITHM_AUTO,
                  "legacy-dispatch: requested algorithm reflects auto method");
    ASSERT_INT_EQ((int)report.effective_algorithm,
                  (int)RALPH_LP_ALGORITHM_AUTO,
                  "legacy-dispatch: effective algorithm remains auto");
    ASSERT_INT_EQ(report.fallback_applied, 0,
                  "legacy-dispatch: no fallback for auto method");

    ralph_test_free(model);
}

static void test_lp_report_rejects_mip_models(void) {
    RalphModel *mip = ralph_test_create();
    RalphLPSolveAlgorithmReport report;

    ASSERT_TRUE(mip != NULL, "mip-guard: model created");
    if (!mip) return;

    ralph_test_set_obj_sense(mip, RALPH_MAXIMIZE);
    ralph_test_add_var(mip, 0.0, 1.0, 1.0, RALPH_BINARY);

    ASSERT_INT_EQ(ralph_lp_get_last_algorithm_report(mip, &report), -1,
                  "mip-guard: report rejected for MIP model before solve");
    ASSERT_INT_EQ(ralph_test_optimize_mip(mip), 0,
                  "mip-guard: optimize_mip succeeds");
    ASSERT_INT_EQ(ralph_lp_get_last_algorithm_report(mip, &report), -1,
                  "mip-guard: report rejected for MIP model after solve");

    ralph_test_free(mip);
}

static void test_glpk_basis_ini_requires_staged_basis(void) {
    RalphModel *model = build_small_lp();
    RalphAPIError err;

    ASSERT_TRUE(model != NULL, "glpk-basis-ini: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_POLICY_PROFILE,
                                         (int)RALPH_LP_POLICY_PROFILE_GLPK_COMPAT),
                  0,
                  "glpk-basis-ini: set glpk compat profile");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_SMCP_BASIS,
                                         (int)RALPH_LP_GLPK_SMCP_BASIS_INI),
                  0,
                  "glpk-basis-ini: request ini basis");
    ASSERT_INT_EQ(ralph_test_optimize_lp(model), -1,
                  "glpk-basis-ini: solve fails without staged basis");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_ERROR,
                  "glpk-basis-ini: status is error");
    ASSERT_INT_EQ(ralph_lp_get_last_error((const RalphLPModel *)model, &err), 0,
                  "glpk-basis-ini: last error available");
    ASSERT_INT_EQ((int)err.domain, (int)RALPH_ERROR_DOMAIN_STATE,
                  "glpk-basis-ini: last error domain");
    ASSERT_INT_EQ((int)err.code, (int)RALPH_ERROR_CODE_NOT_AVAILABLE,
                  "glpk-basis-ini: last error code");

    ralph_test_free(model);
}

static void test_glpk_basis_ini_accepts_staged_basis(void) {
    RalphModel *source = build_small_lp();
    RalphModel *target = build_small_lp();
    RalphBasis *basis = NULL;

    ASSERT_TRUE(source != NULL, "glpk-basis-ini-load: source model created");
    ASSERT_TRUE(target != NULL, "glpk-basis-ini-load: target model created");
    if (!source || !target) {
        ralph_test_free(source);
        ralph_test_free(target);
        return;
    }

    ASSERT_INT_EQ(ralph_test_optimize_lp(source), 0,
                  "glpk-basis-ini-load: source solve succeeds");
    basis = ralph_test_save_basis(source);
    ASSERT_TRUE(basis != NULL, "glpk-basis-ini-load: source basis saved");
    if (!basis) {
        ralph_test_free(source);
        ralph_test_free(target);
        return;
    }

    ASSERT_INT_EQ(ralph_core_set_int_param_id(target, RALPH_PARAM_LP_POLICY_PROFILE,
                                         (int)RALPH_LP_POLICY_PROFILE_GLPK_COMPAT),
                  0,
                  "glpk-basis-ini-load: set glpk compat profile");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(target, RALPH_PARAM_GLPK_SMCP_BASIS,
                                         (int)RALPH_LP_GLPK_SMCP_BASIS_INI),
                  0,
                  "glpk-basis-ini-load: request ini basis");
    ASSERT_INT_EQ(ralph_test_load_basis(target, basis), 0,
                  "glpk-basis-ini-load: stage basis");
    ASSERT_INT_EQ(ralph_test_optimize_lp(target), 0,
                  "glpk-basis-ini-load: solve succeeds with staged basis");
    ASSERT_INT_EQ((int)ralph_test_get_status(target), (int)RALPH_STATUS_OPTIMAL,
                  "glpk-basis-ini-load: target status optimal");

    ralph_test_free_basis(basis);
    ralph_test_free(source);
    ralph_test_free(target);
}

static void test_glpk_basis_bib_reports_not_available(void) {
    RalphModel *model = build_small_lp();
    RalphAPIError err;

    ASSERT_TRUE(model != NULL, "glpk-basis-bib: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_POLICY_PROFILE,
                                         (int)RALPH_LP_POLICY_PROFILE_GLPK_COMPAT),
                  0,
                  "glpk-basis-bib: set glpk compat profile");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_SMCP_BASIS,
                                         (int)RALPH_LP_GLPK_SMCP_BASIS_BIB),
                  0,
                  "glpk-basis-bib: request bib basis");
    ASSERT_INT_EQ(ralph_test_optimize_lp(model), -1,
                  "glpk-basis-bib: solve fails cleanly");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_ERROR,
                  "glpk-basis-bib: status is error");
    ASSERT_INT_EQ(ralph_lp_get_last_error((const RalphLPModel *)model, &err), 0,
                  "glpk-basis-bib: last error available");
    ASSERT_INT_EQ((int)err.domain, (int)RALPH_ERROR_DOMAIN_STATE,
                  "glpk-basis-bib: last error domain");
    ASSERT_INT_EQ((int)err.code, (int)RALPH_ERROR_CODE_NOT_AVAILABLE,
                  "glpk-basis-bib: last error code");

    ralph_test_free(model);
}

static void test_glpk_bfcp_unsupported_extra_control_reports_not_available(void) {
    RalphModel *model = build_small_lp();
    RalphAPIError err;

    ASSERT_TRUE(model != NULL, "glpk-bfcp-extra: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_POLICY_PROFILE,
                                         (int)RALPH_LP_POLICY_PROFILE_GLPK_COMPAT),
                  0,
                  "glpk-bfcp-extra: set glpk compat profile");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_BFCP_PIVOT_LIMIT, 4),
                  0,
                  "glpk-bfcp-extra: request pivot limit");
    ASSERT_INT_EQ(ralph_test_optimize_lp(model), -1,
                  "glpk-bfcp-extra: solve fails cleanly");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_ERROR,
                  "glpk-bfcp-extra: status is error");
    ASSERT_INT_EQ(ralph_lp_get_last_error((const RalphLPModel *)model, &err), 0,
                  "glpk-bfcp-extra: last error available");
    ASSERT_INT_EQ((int)err.domain, (int)RALPH_ERROR_DOMAIN_STATE,
                  "glpk-bfcp-extra: last error domain");
    ASSERT_INT_EQ((int)err.code, (int)RALPH_ERROR_CODE_NOT_AVAILABLE,
                  "glpk-bfcp-extra: last error code");

    ralph_test_free(model);
}

static void test_glpk_bfcp_btf_reports_not_available(void) {
    RalphModel *model = build_small_lp();
    RalphAPIError err;

    ASSERT_TRUE(model != NULL, "glpk-bfcp-btf: model created");
    if (!model) return;

    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_LP_POLICY_PROFILE,
                                         (int)RALPH_LP_POLICY_PROFILE_GLPK_COMPAT),
                  0,
                  "glpk-bfcp-btf: set glpk compat profile");
    ASSERT_INT_EQ(ralph_core_set_int_param_id(model, RALPH_PARAM_GLPK_BFCP_FACTORIZATION,
                                         (int)RALPH_LP_GLPK_BFCP_FACTORIZATION_BTF),
                  0,
                  "glpk-bfcp-btf: request btf factorization");
    ASSERT_INT_EQ(ralph_test_optimize_lp(model), -1,
                  "glpk-bfcp-btf: solve fails cleanly");
    ASSERT_INT_EQ((int)ralph_test_get_status(model), (int)RALPH_STATUS_ERROR,
                  "glpk-bfcp-btf: status is error");
    ASSERT_INT_EQ(ralph_lp_get_last_error((const RalphLPModel *)model, &err), 0,
                  "glpk-bfcp-btf: last error available");
    ASSERT_INT_EQ((int)err.domain, (int)RALPH_ERROR_DOMAIN_STATE,
                  "glpk-bfcp-btf: last error domain");
    ASSERT_INT_EQ((int)err.code, (int)RALPH_ERROR_CODE_NOT_AVAILABLE,
                  "glpk-bfcp-btf: last error code");

    ralph_test_free(model);
}

int main(void) {
    printf("=== LP Algorithm API Tests ===\n");

    test_lp_capabilities();
    test_param_metadata_and_scope();
    test_algorithm_report_guards_and_invalidation();
    test_barrier_fallback_report();
    test_crossover_only_fallback_report();
    test_external_fallback_report();
    test_external_strict_mode_error();
    test_legacy_method_dispatch_report();
    test_lp_report_rejects_mip_models();
    test_glpk_basis_ini_requires_staged_basis();
    test_glpk_basis_ini_accepts_staged_basis();
    test_glpk_basis_bib_reports_not_available();
    test_glpk_bfcp_btf_reports_not_available();
    test_glpk_bfcp_unsupported_extra_control_reports_not_available();

    printf("Passed %d/%d tests\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
