/*
 * ralph_params.c - parameter metadata, lookup and get/set dispatch.
 *
 * Moved verbatim out of ralph.c, which was 8,225 lines and at least seven
 * separate concerns. This is the largest of them: a static specification
 * table and the dispatchers over it, with no solver state of its own and
 * no public symbol that changes. Nothing here was rewritten -- the point
 * was to move it, not to improve it, so that a later change to either half
 * is readable on its own.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ralph_core.h"
#include "ralph_internal.h"
#include "ralph_params.h"
#include "lp.h"
#include "mip.h"
#include "presolve.h"
#include "lp_log.h"

/* ============================================================================
 * Parameters
 * ============================================================================ */

typedef struct {
    RalphParamId id;
    const char *name;
    RalphParamScope scope;
    RalphParamValueType value_type;
    double default_value;
    int has_min;
    double min_value;
    int has_max;
    double max_value;
    const char *aliases[4];
    int alias_count;
} RalphParamSpec;

static int ralph_param_name_eq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

static const RalphParamSpec* ralph_param_specs(void) {
    static const RalphParamSpec specs[RALPH_PARAM_COUNT] = {
        [RALPH_PARAM_MAX_ITERATIONS] = {
            .id = RALPH_PARAM_MAX_ITERATIONS,
            .name = "max_iterations",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)RALPH_DEFAULT_MAX_ITER,
            .aliases = {"IterationLimit"},
            .alias_count = 1
        },
        [RALPH_PARAM_PRESOLVE] = {
            .id = RALPH_PARAM_PRESOLVE,
            .name = "presolve",
            .scope = RALPH_PARAM_SCOPE_SHARED,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = -1.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 1.0,
            .aliases = {"Presolve"},
            .alias_count = 1
        },
        [RALPH_PARAM_VERBOSE] = {
            .id = RALPH_PARAM_VERBOSE,
            .name = "verbose",
            .scope = RALPH_PARAM_SCOPE_SHARED,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .aliases = {"OutputFlag"},
            .alias_count = 1
        },
        [RALPH_PARAM_TELEMETRY] = {
            .id = RALPH_PARAM_TELEMETRY,
            .name = "telemetry",
            .scope = RALPH_PARAM_SCOPE_SHARED,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 1.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 1.0,
            .aliases = {"Telemetry"},
            .alias_count = 1
        },
        [RALPH_PARAM_MAX_NODES] = {
            .id = RALPH_PARAM_MAX_NODES,
            .name = "max_nodes",
            .scope = RALPH_PARAM_SCOPE_MIP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)RALPH_DEFAULT_NODE_LIMIT,
            .aliases = {"NodeLimit"},
            .alias_count = 1
        },
        [RALPH_PARAM_MAX_CUT_ROUNDS] = {
            .id = RALPH_PARAM_MAX_CUT_ROUNDS,
            .name = "max_cut_rounds",
            .scope = RALPH_PARAM_SCOPE_MIP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .aliases = {"CutRounds"},
            .alias_count = 1
        },
        [RALPH_PARAM_METHOD] = {
            .id = RALPH_PARAM_METHOD,
            .name = "method",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 2.0,
            .aliases = {"Method"},
            .alias_count = 1
        },
        [RALPH_PARAM_PRICING] = {
            .id = RALPH_PARAM_PRICING,
            .name = "pricing",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 2.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 5.0,
            .aliases = {"Pricing"},
            .alias_count = 1
        },
        [RALPH_PARAM_DETECT_SPECIAL] = {
            .id = RALPH_PARAM_DETECT_SPECIAL,
            .name = "detect_special",
            .scope = RALPH_PARAM_SCOPE_SHARED,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 1.0,
            .aliases = {"DetectSpecial"},
            .alias_count = 1
        },
        [RALPH_PARAM_NODE_POOL_CAPACITY] = {
            .id = RALPH_PARAM_NODE_POOL_CAPACITY,
            .name = "node_pool_capacity",
            .scope = RALPH_PARAM_SCOPE_MIP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 1024.0,
            .has_min = 1,
            .min_value = 1.0,
            .aliases = {"PoolCapacity"},
            .alias_count = 1
        },
        [RALPH_PARAM_NODE_SELECT] = {
            .id = RALPH_PARAM_NODE_SELECT,
            .name = "node_select",
            .scope = RALPH_PARAM_SCOPE_MIP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 3.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 3.0,
            .aliases = {"NodeSelect"},
            .alias_count = 1
        },
        [RALPH_PARAM_FORCE_TWO_PHASE] = {
            .id = RALPH_PARAM_FORCE_TWO_PHASE,
            .name = "force_two_phase",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 1.0,
            .aliases = {"TwoPhase"},
            .alias_count = 1
        },
        [RALPH_PARAM_TRACE_PHASE1] = {
            .id = RALPH_PARAM_TRACE_PHASE1,
            .name = "trace_phase1",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 1.0,
            .aliases = {"TracePhase1"},
            .alias_count = 1
        },
        [RALPH_PARAM_PRESOLVE_MASK] = {
            .id = RALPH_PARAM_PRESOLVE_MASK,
            .name = "presolve_mask",
            .scope = RALPH_PARAM_SCOPE_SHARED,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)PRESOLVE_SAFE,
            .has_min = 1,
            .min_value = 0.0,
            .aliases = {"PresolveMask"},
            .alias_count = 1
        },
        [RALPH_PARAM_DUAL_BOUND_FLIP] = {
            .id = RALPH_PARAM_DUAL_BOUND_FLIP,
            .name = "dual_bound_flip",
            .scope = RALPH_PARAM_SCOPE_SHARED,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = -1.0,
            .has_min = 1,
            .min_value = -1.0,
            .has_max = 1,
            .max_value = 1.0,
            .aliases = {"DualBoundFlip"},
            .alias_count = 1
        },
        [RALPH_PARAM_DUAL_STEEPEST_EDGE] = {
            .id = RALPH_PARAM_DUAL_STEEPEST_EDGE,
            .name = "dual_steepest_edge",
            .scope = RALPH_PARAM_SCOPE_SHARED,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = -1.0,
            .has_min = 1,
            .min_value = -1.0,
            .has_max = 1,
            .max_value = 1.0,
            .aliases = {"DualSteepestEdge"},
            .alias_count = 1
        },
        [RALPH_PARAM_SCALING] = {
            .id = RALPH_PARAM_SCALING,
            .name = "scaling",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 1.0,
            .has_min = 1,
            .min_value = 0.0,
            .aliases = {"Scaling", "scaling_rounds", "ScalingRounds"},
            .alias_count = 3
        },
        [RALPH_PARAM_CRASH] = {
            .id = RALPH_PARAM_CRASH,
            .name = "crash",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 1.0,
            .aliases = {"Crash"},
            .alias_count = 1
        },
        [RALPH_PARAM_VERIFY] = {
            .id = RALPH_PARAM_VERIFY,
            .name = "verify",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 1.0,
            .aliases = {"Verify"},
            .alias_count = 1
        },
        [RALPH_PARAM_PHASE1_PRICING] = {
            .id = RALPH_PARAM_PHASE1_PRICING,
            .name = "phase1_pricing",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = -1.0,
            .has_min = 1,
            .min_value = -1.0,
            .aliases = {"Phase1Pricing"},
            .alias_count = 1
        },
        [RALPH_PARAM_VAR_SELECT] = {
            .id = RALPH_PARAM_VAR_SELECT,
            .name = "var_select",
            .scope = RALPH_PARAM_SCOPE_MIP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = -1.0,
            .has_min = 1,
            .min_value = -1.0,
            .has_max = 1,
            .max_value = 4.0,
            .aliases = {"VarSelect"},
            .alias_count = 1
        },
        [RALPH_PARAM_LU_SUPERNODE] = {
            .id = RALPH_PARAM_LU_SUPERNODE,
            .name = "lu_supernode",
            .scope = RALPH_PARAM_SCOPE_SHARED,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 1.0,
            .aliases = {"LuSupernode"},
            .alias_count = 1
        },
        [RALPH_PARAM_DETERMINISTIC] = {
            .id = RALPH_PARAM_DETERMINISTIC,
            .name = "deterministic",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 1.0,
            .aliases = {"Deterministic"},
            .alias_count = 1
        },
        [RALPH_PARAM_RANDOM_SEED] = {
            .id = RALPH_PARAM_RANDOM_SEED,
            .name = "random_seed",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .has_min = 1,
            .min_value = 0.0,
            .aliases = {"RandomSeed"},
            .alias_count = 1
        },
        [RALPH_PARAM_LP_THREADS] = {
            .id = RALPH_PARAM_LP_THREADS,
            .name = "lp_threads",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .has_min = 1,
            .min_value = 0.0,
            .aliases = {"LPThreads"},
            .alias_count = 1
        },
        [RALPH_PARAM_LP_ALGORITHM] = {
            .id = RALPH_PARAM_LP_ALGORITHM,
            .name = "lp_algorithm",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)RALPH_LP_ALGORITHM_AUTO,
            .has_min = 1,
            .min_value = (double)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
            .has_max = 1,
            .max_value = (double)RALPH_LP_ALGORITHM_BARRIER_EXTERNAL,
            .aliases = {"LPAlgorithm"},
            .alias_count = 1
        },
        [RALPH_PARAM_BARRIER_CROSSOVER] = {
            .id = RALPH_PARAM_BARRIER_CROSSOVER,
            .name = "barrier_crossover",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)RALPH_LP_CROSSOVER_AUTO,
            .has_min = 1,
            .min_value = (double)RALPH_LP_CROSSOVER_AUTO,
            .has_max = 1,
            .max_value = (double)RALPH_LP_CROSSOVER_ON,
            .aliases = {"BarrierCrossover"},
            .alias_count = 1
        },
        [RALPH_PARAM_LP_EXTERNAL_PROVIDER] = {
            .id = RALPH_PARAM_LP_EXTERNAL_PROVIDER,
            .name = "lp_external_provider",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)RALPH_LP_EXTERNAL_PROVIDER_NONE,
            .has_min = 1,
            .min_value = (double)RALPH_LP_EXTERNAL_PROVIDER_NONE,
            .has_max = 1,
            .max_value = (double)RALPH_LP_EXTERNAL_PROVIDER_GLOP,
            .aliases = {"LPExternalProvider"},
            .alias_count = 1
        },
        [RALPH_PARAM_LP_EXTERNAL_STRICT] = {
            .id = RALPH_PARAM_LP_EXTERNAL_STRICT,
            .name = "lp_external_strict",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 1.0,
            .aliases = {"LPExternalStrict"},
            .alias_count = 1
        },
        [RALPH_PARAM_LP_BASIS_GOVERNOR_MODE] = {
            .id = RALPH_PARAM_LP_BASIS_GOVERNOR_MODE,
            .name = "lp_basis_governor_mode",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)LP_BASIS_GOV_MODE_OFF,
            .has_min = 1,
            .min_value = (double)LP_BASIS_GOV_MODE_OFF,
            .has_max = 1,
            .max_value = (double)LP_BASIS_GOV_MODE_CONTROL_PHASE2,
            .aliases = {"LPBasisGovernorMode"},
            .alias_count = 1
        },
        [RALPH_PARAM_LP_REINVERT_CONTROLLER_MODE] = {
            .id = RALPH_PARAM_LP_REINVERT_CONTROLLER_MODE,
            .name = "lp_reinvert_controller_mode",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)LP_REINVERT_MODE_SHADOW,
            .has_min = 1,
            .min_value = (double)LP_REINVERT_MODE_OFF,
            .has_max = 1,
            .max_value = (double)LP_REINVERT_MODE_CONTROL_ALL,
            .aliases = {"LPReinvertControllerMode"},
            .alias_count = 1
        },
        [RALPH_PARAM_LP_POLICY_PROFILE] = {
            .id = RALPH_PARAM_LP_POLICY_PROFILE,
            .name = "lp_policy_profile",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)RALPH_LP_POLICY_PROFILE_DEFAULT,
            .has_min = 1,
            .min_value = (double)RALPH_LP_POLICY_PROFILE_DEFAULT,
            .has_max = 1,
            .max_value = (double)RALPH_LP_POLICY_PROFILE_GLPK_LEGACY,
            .aliases = {"LPPolicyProfile"},
            .alias_count = 1
        },
        [RALPH_PARAM_GLPK_SMCP_METHOD] = {
            .id = RALPH_PARAM_GLPK_SMCP_METHOD,
            .name = "glpk_smcp_method",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)RALPH_LP_GLPK_SMCP_METHOD_AUTO,
            .has_min = 1,
            .min_value = (double)RALPH_LP_GLPK_SMCP_METHOD_AUTO,
            .has_max = 1,
            .max_value = (double)RALPH_LP_GLPK_SMCP_METHOD_DUAL,
            .aliases = {"GLPKSMCPMethod"},
            .alias_count = 1
        },
        [RALPH_PARAM_GLPK_SMCP_PRICING] = {
            .id = RALPH_PARAM_GLPK_SMCP_PRICING,
            .name = "glpk_smcp_pricing",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)RALPH_LP_GLPK_SMCP_PRICING_STEEP,
            .has_min = 1,
            .min_value = (double)RALPH_LP_GLPK_SMCP_PRICING_STANDARD,
            .has_max = 1,
            .max_value = (double)RALPH_LP_GLPK_SMCP_PRICING_STEEP,
            .aliases = {"GLPKSMCPPricing"},
            .alias_count = 1
        },
        [RALPH_PARAM_GLPK_SMCP_RATIO] = {
            .id = RALPH_PARAM_GLPK_SMCP_RATIO,
            .name = "glpk_smcp_ratio",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)RALPH_LP_GLPK_SMCP_RATIO_HARRIS,
            .has_min = 1,
            .min_value = (double)RALPH_LP_GLPK_SMCP_RATIO_STANDARD,
            .has_max = 1,
            .max_value = (double)RALPH_LP_GLPK_SMCP_RATIO_HARRIS,
            .aliases = {"GLPKSMCPRatio"},
            .alias_count = 1
        },
        [RALPH_PARAM_GLPK_SMCP_FLIP] = {
            .id = RALPH_PARAM_GLPK_SMCP_FLIP,
            .name = "glpk_smcp_flip",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)RALPH_LP_GLPK_SMCP_FLIP_OFF,
            .has_min = 1,
            .min_value = (double)RALPH_LP_GLPK_SMCP_FLIP_OFF,
            .has_max = 1,
            .max_value = (double)RALPH_LP_GLPK_SMCP_FLIP_ON,
            .aliases = {"GLPKSMCPFlip"},
            .alias_count = 1
        },
        [RALPH_PARAM_GLPK_SMCP_BASIS] = {
            .id = RALPH_PARAM_GLPK_SMCP_BASIS,
            .name = "glpk_smcp_basis",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)RALPH_LP_GLPK_SMCP_BASIS_ADV,
            .has_min = 1,
            .min_value = (double)RALPH_LP_GLPK_SMCP_BASIS_ADV,
            .has_max = 1,
            .max_value = (double)RALPH_LP_GLPK_SMCP_BASIS_INI,
            .aliases = {"GLPKSMCPBasis"},
            .alias_count = 1
        },
        [RALPH_PARAM_GLPK_SMCP_PRESOLVE] = {
            .id = RALPH_PARAM_GLPK_SMCP_PRESOLVE,
            .name = "glpk_smcp_presolve",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)RALPH_LP_GLPK_SMCP_PRESOLVE_AUTO,
            .has_min = 1,
            .min_value = (double)RALPH_LP_GLPK_SMCP_PRESOLVE_AUTO,
            .has_max = 1,
            .max_value = (double)RALPH_LP_GLPK_SMCP_PRESOLVE_ON,
            .aliases = {"GLPKSMCPPresolve"},
            .alias_count = 1
        },
        [RALPH_PARAM_GLPK_SMCP_TOL_BND] = {
            .id = RALPH_PARAM_GLPK_SMCP_TOL_BND,
            .name = "glpk_smcp_tol_bnd",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_DOUBLE,
            .default_value = 1e-7,
            .has_min = 1,
            .min_value = 0.0,
            .aliases = {"GLPKSMCPTolBnd"},
            .alias_count = 1
        },
        [RALPH_PARAM_GLPK_SMCP_TOL_DJ] = {
            .id = RALPH_PARAM_GLPK_SMCP_TOL_DJ,
            .name = "glpk_smcp_tol_dj",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_DOUBLE,
            .default_value = 1e-7,
            .has_min = 1,
            .min_value = 0.0,
            .aliases = {"GLPKSMCPTolDj"},
            .alias_count = 1
        },
        [RALPH_PARAM_GLPK_SMCP_TOL_PIV] = {
            .id = RALPH_PARAM_GLPK_SMCP_TOL_PIV,
            .name = "glpk_smcp_tol_piv",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_DOUBLE,
            .default_value = 1e-9,
            .has_min = 1,
            .min_value = 0.0,
            .aliases = {"GLPKSMCPTolPiv"},
            .alias_count = 1
        },
        [RALPH_PARAM_GLPK_SMCP_EXCL] = {
            .id = RALPH_PARAM_GLPK_SMCP_EXCL,
            .name = "glpk_smcp_excl",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)RALPH_LP_GLPK_SMCP_EXCL_ON,
            .has_min = 1,
            .min_value = (double)RALPH_LP_GLPK_SMCP_EXCL_OFF,
            .has_max = 1,
            .max_value = (double)RALPH_LP_GLPK_SMCP_EXCL_ON,
            .aliases = {"GLPKSMCPExcl"},
            .alias_count = 1
        },
        [RALPH_PARAM_GLPK_SMCP_SHIFT] = {
            .id = RALPH_PARAM_GLPK_SMCP_SHIFT,
            .name = "glpk_smcp_shift",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)RALPH_LP_GLPK_SMCP_SHIFT_ON,
            .has_min = 1,
            .min_value = (double)RALPH_LP_GLPK_SMCP_SHIFT_OFF,
            .has_max = 1,
            .max_value = (double)RALPH_LP_GLPK_SMCP_SHIFT_ON,
            .aliases = {"GLPKSMCPShift"},
            .alias_count = 1
        },
        [RALPH_PARAM_GLPK_SMCP_AORN] = {
            .id = RALPH_PARAM_GLPK_SMCP_AORN,
            .name = "glpk_smcp_aorn",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)RALPH_LP_GLPK_SMCP_AORN_USE_NT,
            .has_min = 1,
            .min_value = (double)RALPH_LP_GLPK_SMCP_AORN_USE_AT,
            .has_max = 1,
            .max_value = (double)RALPH_LP_GLPK_SMCP_AORN_USE_NT,
            .aliases = {"GLPKSMCPAorn"},
            .alias_count = 1
        },
        [RALPH_PARAM_GLPK_BFCP_FACTORIZATION] = {
            .id = RALPH_PARAM_GLPK_BFCP_FACTORIZATION,
            .name = "glpk_bfcp_factorization",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)RALPH_LP_GLPK_BFCP_FACTORIZATION_LUF,
            .has_min = 1,
            .min_value = (double)RALPH_LP_GLPK_BFCP_FACTORIZATION_LUF,
            .has_max = 1,
            .max_value = (double)RALPH_LP_GLPK_BFCP_FACTORIZATION_BTF,
            .aliases = {"GLPKBFCPFactorization"},
            .alias_count = 1
        },
        [RALPH_PARAM_GLPK_BFCP_BACKEND] = {
            .id = RALPH_PARAM_GLPK_BFCP_BACKEND,
            .name = "glpk_bfcp_backend",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)RALPH_LP_GLPK_BFCP_BACKEND_LUF_FT,
            .has_min = 1,
            .min_value = (double)RALPH_LP_GLPK_BFCP_BACKEND_LUF_FT,
            .has_max = 1,
            .max_value = (double)RALPH_LP_GLPK_BFCP_BACKEND_CGR,
            .aliases = {"GLPKBFCPBackend"},
            .alias_count = 1
        },
        [RALPH_PARAM_GLPK_BFCP_UPDATE_LIMIT] = {
            .id = RALPH_PARAM_GLPK_BFCP_UPDATE_LIMIT,
            .name = "glpk_bfcp_update_limit",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = -1.0,
            .has_min = 1,
            .min_value = -1.0,
            .aliases = {"GLPKBFCPUpdateLimit"},
            .alias_count = 1
        },
        [RALPH_PARAM_GLPK_BFCP_PIVOT_LIMIT] = {
            .id = RALPH_PARAM_GLPK_BFCP_PIVOT_LIMIT,
            .name = "glpk_bfcp_pivot_limit",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = -1.0,
            .has_min = 1,
            .min_value = -1.0,
            .aliases = {"GLPKBFCPPivotLimit"},
            .alias_count = 1
        },
        [RALPH_PARAM_GLPK_BFCP_SUHL] = {
            .id = RALPH_PARAM_GLPK_BFCP_SUHL,
            .name = "glpk_bfcp_suhl",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)RALPH_LP_GLPK_BFCP_SUHL_AUTO,
            .has_min = 1,
            .min_value = (double)RALPH_LP_GLPK_BFCP_SUHL_AUTO,
            .has_max = 1,
            .max_value = (double)RALPH_LP_GLPK_BFCP_SUHL_ON,
            .aliases = {"GLPKBFCPSuhl"},
            .alias_count = 1
        },
        [RALPH_PARAM_GLPK_BFCP_PIVOT_TOL] = {
            .id = RALPH_PARAM_GLPK_BFCP_PIVOT_TOL,
            .name = "glpk_bfcp_pivot_tol",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_DOUBLE,
            .default_value = 0.0,
            .aliases = {"GLPKBFCPPivotTol"},
            .alias_count = 1
        },
        [RALPH_PARAM_GLPK_BFCP_EPS_TOL] = {
            .id = RALPH_PARAM_GLPK_BFCP_EPS_TOL,
            .name = "glpk_bfcp_eps_tol",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_DOUBLE,
            .default_value = 0.0,
            .has_min = 1,
            .min_value = 0.0,
            .aliases = {"GLPKBFCPEpsTol"},
            .alias_count = 1
        },
        [RALPH_PARAM_GLPK_BFCP_GROWTH_GUARD] = {
            .id = RALPH_PARAM_GLPK_BFCP_GROWTH_GUARD,
            .name = "glpk_bfcp_growth_guard",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_DOUBLE,
            .default_value = 0.0,
            .aliases = {"GLPKBFCPGrowthGuard"},
            .alias_count = 1
        },
        [RALPH_PARAM_GLPK_BFCP_NFS_MAX] = {
            .id = RALPH_PARAM_GLPK_BFCP_NFS_MAX,
            .name = "glpk_bfcp_nfs_max",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = -1.0,
            .has_min = 1,
            .min_value = -1.0,
            .aliases = {"GLPKBFCPNfsMax"},
            .alias_count = 1
        },
        [RALPH_PARAM_GLPK_BFCP_NRS_MAX] = {
            .id = RALPH_PARAM_GLPK_BFCP_NRS_MAX,
            .name = "glpk_bfcp_nrs_max",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = -1.0,
            .has_min = 1,
            .min_value = -1.0,
            .aliases = {"GLPKBFCPNrsMax"},
            .alias_count = 1
        },
        [RALPH_PARAM_REFACTOR_MIN_INTERVAL] = {
            .id = RALPH_PARAM_REFACTOR_MIN_INTERVAL,
            .name = "refactor_min_interval",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 10.0,
            .has_min = 1,
            .min_value = 1.0,
            .aliases = {"RefactorMinInterval"},
            .alias_count = 1
        },
        [RALPH_PARAM_REFACTOR_MAX_INTERVAL] = {
            .id = RALPH_PARAM_REFACTOR_MAX_INTERVAL,
            .name = "refactor_max_interval",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 80.0,
            .has_min = 1,
            .min_value = 1.0,
            .aliases = {"RefactorMaxInterval"},
            .alias_count = 1
        },
        [RALPH_PARAM_DEGEN_ESCAPE_MIN_M] = {
            .id = RALPH_PARAM_DEGEN_ESCAPE_MIN_M,
            .name = "degen_escape_min_m",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 1200.0,
            .has_min = 1,
            .min_value = 1.0,
            .aliases = {"DegenEscapeMinM"},
            .alias_count = 1
        },
        [RALPH_PARAM_LU_COST_EWMA_ALPHA] = {
            .id = RALPH_PARAM_LU_COST_EWMA_ALPHA,
            .name = "lu_cost_ewma_alpha",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_DOUBLE,
            .default_value = 0.20,
            .has_min = 1,
            .min_value = 0.0,
            .aliases = {"LUCostEWMAAlpha"},
            .alias_count = 1
        },
        [RALPH_PARAM_LU_SPIKE_WARN_PCT] = {
            .id = RALPH_PARAM_LU_SPIKE_WARN_PCT,
            .name = "lu_spike_warn_pct",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 85.0,
            .has_min = 1,
            .min_value = 1.0,
            .aliases = {"LUSpikeWarnPct"},
            .alias_count = 1
        },
        [RALPH_PARAM_TIME_LIMIT] = {
            .id = RALPH_PARAM_TIME_LIMIT,
            .name = "time_limit",
            .scope = RALPH_PARAM_SCOPE_SHARED,
            .value_type = RALPH_PARAM_VALUE_DOUBLE,
            .default_value = RALPH_DEFAULT_TIME_LIMIT,
            .has_min = 1,
            .min_value = 0.0,
            .aliases = {"TimeLimit"},
            .alias_count = 1
        },
        [RALPH_PARAM_MIP_GAP] = {
            .id = RALPH_PARAM_MIP_GAP,
            .name = "mip_gap",
            .scope = RALPH_PARAM_SCOPE_MIP,
            .value_type = RALPH_PARAM_VALUE_DOUBLE,
            .default_value = RALPH_DEFAULT_MIP_GAP,
            .has_min = 1,
            .min_value = 0.0,
            .aliases = {"MIPGap"},
            .alias_count = 1
        },
        [RALPH_PARAM_OBJ_LIMIT] = {
            .id = RALPH_PARAM_OBJ_LIMIT,
            .name = "obj_limit",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_DOUBLE,
            .default_value = RALPH_INFINITY,
            .aliases = {"ObjLimit"},
            .alias_count = 1
        },
        [RALPH_PARAM_FEAS_TOL] = {
            .id = RALPH_PARAM_FEAS_TOL,
            .name = "feas_tol",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_DOUBLE,
            .default_value = RALPH_FEAS_TOL,
            .has_min = 1,
            .min_value = 0.0
        },
        [RALPH_PARAM_OPT_TOL] = {
            .id = RALPH_PARAM_OPT_TOL,
            .name = "opt_tol",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_DOUBLE,
            .default_value = RALPH_OPT_TOL,
            .has_min = 1,
            .min_value = 0.0
        },
        [RALPH_PARAM_PIVOT_TOL] = {
            .id = RALPH_PARAM_PIVOT_TOL,
            .name = "pivot_tol",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_DOUBLE,
            .default_value = RALPH_PIVOT_TOL,
            .has_min = 1,
            .min_value = 0.0
        }
    };
    return specs;
}

static const RalphParamSpec* ralph_param_spec_by_id(RalphParamId param) {
    if (param < 0 || param >= RALPH_PARAM_COUNT) return NULL;
    return &ralph_param_specs()[param];
}

static int ralph_param_scope_allows_lp(RalphParamScope scope) {
    return scope == RALPH_PARAM_SCOPE_SHARED || scope == RALPH_PARAM_SCOPE_LP;
}

static int ralph_param_scope_allows_mip(RalphParamScope scope) {
    return scope == RALPH_PARAM_SCOPE_SHARED || scope == RALPH_PARAM_SCOPE_MIP;
}

int ralph_core_get_param_count(void) {
    return RALPH_PARAM_COUNT;
}

int ralph_core_get_param_meta(RalphParamId param, RalphParamMeta *meta) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (!meta) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "metadata output pointer is null");
    }
    RALPH_CLEAR_API_ERROR(NULL);
    if (!spec) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       RALPH_PARAM_COUNT,
                       "unknown parameter id");
    }

    meta->id = spec->id;
    meta->name = spec->name;
    meta->scope = spec->scope;
    meta->value_type = spec->value_type;
    meta->default_value = spec->default_value;
    meta->has_min = spec->has_min;
    meta->min_value = spec->min_value;
    meta->has_max = spec->has_max;
    meta->max_value = spec->max_value;
    return 0;
}

int ralph_core_find_param_by_name(const char *name, RalphParamId *param) {
    if (!name || !param) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "name or output pointer is null");
    }
    RALPH_CLEAR_API_ERROR(NULL);

    const RalphParamSpec *specs = ralph_param_specs();
    for (int i = 0; i < RALPH_PARAM_COUNT; i++) {
        const RalphParamSpec *spec = &specs[i];
        if (ralph_param_name_eq(name, spec->name)) {
            *param = spec->id;
            return 0;
        }
        for (int k = 0; k < spec->alias_count; k++) {
            if (ralph_param_name_eq(name, spec->aliases[k])) {
                *param = spec->id;
                return 0;
            }
        }
    }
    RALPH_FAIL_API(NULL,
                   RALPH_ERROR_DOMAIN_PARAMETER,
                   RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                   RALPH_STATUS_UNKNOWN,
                   RALPH_ERROR_API_PARAMETER,
                   0,
                   0,
                   "unknown parameter name");
}

int ralph_core_set_int_param_id(RalphModel *model, RalphParamId param, int value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (!model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    if (!spec) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       RALPH_PARAM_COUNT,
                       "unknown parameter id");
    }
    if (spec->value_type != RALPH_PARAM_VALUE_INT) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       spec->value_type,
                       "parameter is not integer-valued");
    }

    switch (param) {
        case RALPH_PARAM_MAX_ITERATIONS:
            model->max_iterations = value;
            break;
        case RALPH_PARAM_PRESOLVE:
            model->presolve = value ? 1 : -1;  /* -1 = explicitly off */
            break;
        case RALPH_PARAM_VERBOSE:
            model->verbose = value;
            break;
        case RALPH_PARAM_TELEMETRY:
            model->telemetry = value ? 1 : 0;
            break;
        case RALPH_PARAM_MAX_NODES:
            model->max_nodes = value;
            break;
        case RALPH_PARAM_MAX_CUT_ROUNDS:
            model->max_cut_rounds = value;
            break;
        case RALPH_PARAM_METHOD:
            if (value < (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX ||
                value > (int)RALPH_LP_ALGORITHM_AUTO) {
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_PARAMETER,
                               RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_PARAMETER,
                               param,
                               value,
                               "method value is out of range");
            }
            if (ralph_set_requested_lp_algorithm_internal(model, value) != 0) {
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_PARAMETER,
                               RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_PARAMETER,
                               param,
                               value,
                               "invalid LP algorithm value");
            }
            break;
        case RALPH_PARAM_PRICING:
            model->pricing = value;
            break;
        case RALPH_PARAM_DETECT_SPECIAL:
            model->detect_special = value;
            break;
        case RALPH_PARAM_NODE_POOL_CAPACITY:
            model->node_pool_capacity = (value > 0) ? value : 1024;
            break;
        case RALPH_PARAM_NODE_SELECT:
            if (value < 0 || value > 3) {
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_PARAMETER,
                               RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_PARAMETER,
                               param,
                               value,
                               "node_select is out of range");
            }
            model->node_select = value;
            break;
        case RALPH_PARAM_FORCE_TWO_PHASE:
            model->force_two_phase = value;
            break;
        case RALPH_PARAM_TRACE_PHASE1:
            model->trace_phase1 = value;
            break;
        case RALPH_PARAM_PRESOLVE_MASK:
            model->presolve_mask = (unsigned int)value;
            break;
        case RALPH_PARAM_DUAL_BOUND_FLIP:
            model->dual_bound_flip = value ? 1 : 0;
            break;
        case RALPH_PARAM_DUAL_STEEPEST_EDGE:
            model->dual_steepest_edge = value ? 1 : 0;
            break;
        case RALPH_PARAM_SCALING:
            model->scaling = (value >= 0) ? value : 0;
            break;
        case RALPH_PARAM_CRASH:
            model->crash = value ? 1 : 0;
            break;
        case RALPH_PARAM_VERIFY:
            model->verify = value ? 1 : 0;
            break;
        case RALPH_PARAM_PHASE1_PRICING:
            model->phase1_pricing = (value >= 0) ? value : -1;
            break;
        case RALPH_PARAM_VAR_SELECT:
            if (value < 0 || value > 4) {
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_PARAMETER,
                               RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_PARAMETER,
                               param,
                               value,
                               "var_select is out of range");
            }
            model->var_select = value;
            break;
        case RALPH_PARAM_LU_SUPERNODE:
            model->lu_supernode = value ? 1 : 0;
            break;
        case RALPH_PARAM_DETERMINISTIC:
            model->deterministic = value ? 1 : 0;
            break;
        case RALPH_PARAM_RANDOM_SEED:
            if (value < 0) {
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_PARAMETER,
                               RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_PARAMETER,
                               param,
                               value,
                               "random_seed must be non-negative");
            }
            model->random_seed = value;
            break;
        case RALPH_PARAM_LP_THREADS:
            if (value < 0) {
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_PARAMETER,
                               RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_PARAMETER,
                               param,
                               value,
                               "lp_threads must be non-negative");
            }
            model->lp_threads = value;
            break;
        case RALPH_PARAM_LP_ALGORITHM:
            if (ralph_set_requested_lp_algorithm_internal(model, value) != 0) {
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_PARAMETER,
                               RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_PARAMETER,
                               param,
                               value,
                               "invalid lp_algorithm value");
            }
            break;
        case RALPH_PARAM_BARRIER_CROSSOVER:
            if (ralph_set_requested_barrier_crossover_internal(model, value) != 0) {
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_PARAMETER,
                               RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_PARAMETER,
                               param,
                               value,
                               "invalid barrier_crossover value");
            }
            break;
        case RALPH_PARAM_LP_EXTERNAL_PROVIDER:
            if (ralph_set_requested_lp_external_provider_internal(model, value) != 0) {
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_PARAMETER,
                               RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_PARAMETER,
                               param,
                               value,
                               "invalid lp_external_provider value");
            }
            break;
        case RALPH_PARAM_LP_EXTERNAL_STRICT:
            if (value < 0 || value > 1) {
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_PARAMETER,
                               RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_PARAMETER,
                               param,
                               value,
                               "lp_external_strict must be 0 or 1");
            }
            model->lp_external_strict = value;
            break;
        case RALPH_PARAM_LP_BASIS_GOVERNOR_MODE:
            if (!lp_basis_governor_mode_is_valid(value)) {
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_PARAMETER,
                               RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_PARAMETER,
                               param,
                               value,
                               "lp_basis_governor_mode must be 0(off), 1(shadow), or 2(control_phase2)");
            }
            model->lp_basis_governor_mode = value;
            break;
        case RALPH_PARAM_LP_REINVERT_CONTROLLER_MODE:
            if (!lp_reinvert_controller_mode_is_valid(value)) {
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_PARAMETER,
                               RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_PARAMETER,
                               param,
                               value,
                               "lp_reinvert_controller_mode must be 0(off), 1(shadow), 2(control_phase1), or 3(control_all)");
            }
            model->lp_reinvert_controller_mode = value;
            break;
        case RALPH_PARAM_LP_POLICY_PROFILE:
        case RALPH_PARAM_GLPK_SMCP_METHOD:
        case RALPH_PARAM_GLPK_SMCP_PRICING:
        case RALPH_PARAM_GLPK_SMCP_RATIO:
        case RALPH_PARAM_GLPK_SMCP_FLIP:
        case RALPH_PARAM_GLPK_SMCP_BASIS:
        case RALPH_PARAM_GLPK_SMCP_PRESOLVE:
        case RALPH_PARAM_GLPK_SMCP_EXCL:
        case RALPH_PARAM_GLPK_SMCP_SHIFT:
        case RALPH_PARAM_GLPK_SMCP_AORN:
        case RALPH_PARAM_GLPK_BFCP_FACTORIZATION:
        case RALPH_PARAM_GLPK_BFCP_BACKEND:
        case RALPH_PARAM_GLPK_BFCP_UPDATE_LIMIT:
        case RALPH_PARAM_GLPK_BFCP_PIVOT_LIMIT:
        case RALPH_PARAM_GLPK_BFCP_SUHL:
        case RALPH_PARAM_GLPK_BFCP_NFS_MAX:
        case RALPH_PARAM_GLPK_BFCP_NRS_MAX:
            if (ralph_set_glpk_policy_int_param(model, param, value) != 0) {
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_PARAMETER,
                               RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_PARAMETER,
                               param,
                               value,
                               "invalid glpk policy integer parameter value");
            }
            break;
        case RALPH_PARAM_REFACTOR_MIN_INTERVAL:
            model->refactor_min_interval = (value >= 1) ? value : 1;
            break;
        case RALPH_PARAM_REFACTOR_MAX_INTERVAL:
            model->refactor_max_interval = (value >= 1) ? value : 1;
            break;
        case RALPH_PARAM_DEGEN_ESCAPE_MIN_M:
            model->degen_escape_min_m = (value >= 1) ? value : 1;
            break;
        case RALPH_PARAM_LU_SPIKE_WARN_PCT:
            model->lu_spike_warn_pct = (value >= 1) ? value : 1;
            break;
        default:
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_PARAMETER,
                           RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_PARAMETER,
                           param,
                           0,
                           "unsupported parameter id");
    }

    return 0;
}

int ralph_core_set_dbl_param_id(RalphModel *model, RalphParamId param, double value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    if (!spec) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       RALPH_PARAM_COUNT,
                       "unknown parameter id");
    }
    if (spec->value_type != RALPH_PARAM_VALUE_DOUBLE) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       spec->value_type,
                       "parameter is not double-valued");
    }

    switch (param) {
        case RALPH_PARAM_TIME_LIMIT:
            model->time_limit = value;
            break;
        case RALPH_PARAM_MIP_GAP:
            model->mip_gap = value;
            break;
        case RALPH_PARAM_OBJ_LIMIT:
            model->objective_limit = value;
            break;
        case RALPH_PARAM_FEAS_TOL:
            if (value <= 0.0) {
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_PARAMETER,
                               RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_PARAMETER,
                               param,
                               0,
                               "feas_tol must be positive");
            }
            model->lp_model->feas_tol = value;
            break;
        case RALPH_PARAM_OPT_TOL:
            if (value <= 0.0) {
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_PARAMETER,
                               RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_PARAMETER,
                               param,
                               0,
                               "opt_tol must be positive");
            }
            model->lp_model->opt_tol = value;
            break;
        case RALPH_PARAM_PIVOT_TOL:
            if (value <= 0.0) {
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_PARAMETER,
                               RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_PARAMETER,
                               param,
                               0,
                               "pivot_tol must be positive");
            }
            model->lp_model->pivot_tol = value;
            break;
        case RALPH_PARAM_GLPK_SMCP_TOL_BND:
        case RALPH_PARAM_GLPK_SMCP_TOL_DJ:
        case RALPH_PARAM_GLPK_SMCP_TOL_PIV:
        case RALPH_PARAM_GLPK_BFCP_PIVOT_TOL:
        case RALPH_PARAM_GLPK_BFCP_EPS_TOL:
        case RALPH_PARAM_GLPK_BFCP_GROWTH_GUARD:
            if (!isfinite(value)) {
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_PARAMETER,
                               RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_PARAMETER,
                               param,
                               0,
                               "glpk policy float parameter must be finite");
            }
            if (ralph_set_glpk_policy_double_param(model, param, value) != 0) {
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_PARAMETER,
                               RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_PARAMETER,
                               param,
                               0,
                               "invalid glpk policy float parameter value");
            }
            break;
        case RALPH_PARAM_LU_COST_EWMA_ALPHA:
            model->lu_cost_ewma_alpha = (value >= 0.0) ? value : 0.0;
            break;
        default:
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_PARAMETER,
                           RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_PARAMETER,
                           param,
                           0,
                           "unsupported parameter id");
    }

    return 0;
}

int ralph_core_get_int_param_id(const RalphModel *model, RalphParamId param, int *value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!model || !value) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "model or output pointer is null");
    }
    if (!spec) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       RALPH_PARAM_COUNT,
                       "unknown parameter id");
    }
    if (spec->value_type != RALPH_PARAM_VALUE_INT) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       spec->value_type,
                       "parameter is not integer-valued");
    }

    switch (param) {
        case RALPH_PARAM_MAX_ITERATIONS:
            *value = model->max_iterations;
            break;
        case RALPH_PARAM_PRESOLVE:
            *value = (model->presolve > 0) ? 1 : 0;
            break;
        case RALPH_PARAM_VERBOSE:
            *value = model->verbose;
            break;
        case RALPH_PARAM_TELEMETRY:
            *value = model->telemetry;
            break;
        case RALPH_PARAM_MAX_NODES:
            *value = model->max_nodes;
            break;
        case RALPH_PARAM_MAX_CUT_ROUNDS:
            *value = model->max_cut_rounds;
            break;
        case RALPH_PARAM_METHOD:
            *value = model->method;
            break;
        case RALPH_PARAM_PRICING:
            *value = model->pricing;
            break;
        case RALPH_PARAM_DETECT_SPECIAL:
            *value = model->detect_special;
            break;
        case RALPH_PARAM_NODE_POOL_CAPACITY:
            *value = model->node_pool_capacity;
            break;
        case RALPH_PARAM_NODE_SELECT:
            *value = model->node_select;
            break;
        case RALPH_PARAM_FORCE_TWO_PHASE:
            *value = model->force_two_phase;
            break;
        case RALPH_PARAM_TRACE_PHASE1:
            *value = model->trace_phase1;
            break;
        case RALPH_PARAM_PRESOLVE_MASK:
            *value = (int)model->presolve_mask;
            break;
        case RALPH_PARAM_DUAL_BOUND_FLIP:
            *value = model->dual_bound_flip;
            break;
        case RALPH_PARAM_DUAL_STEEPEST_EDGE:
            *value = model->dual_steepest_edge;
            break;
        case RALPH_PARAM_SCALING:
            *value = model->scaling;
            break;
        case RALPH_PARAM_CRASH:
            *value = model->crash;
            break;
        case RALPH_PARAM_VERIFY:
            *value = model->verify;
            break;
        case RALPH_PARAM_PHASE1_PRICING:
            *value = model->phase1_pricing;
            break;
        case RALPH_PARAM_VAR_SELECT:
            *value = model->var_select;
            break;
        case RALPH_PARAM_LU_SUPERNODE:
            *value = model->lu_supernode;
            break;
        case RALPH_PARAM_DETERMINISTIC:
            *value = model->deterministic;
            break;
        case RALPH_PARAM_RANDOM_SEED:
            *value = model->random_seed;
            break;
        case RALPH_PARAM_LP_THREADS:
            *value = model->lp_threads;
            break;
        case RALPH_PARAM_LP_ALGORITHM:
            *value = model->lp_algorithm;
            break;
        case RALPH_PARAM_BARRIER_CROSSOVER:
            *value = model->barrier_crossover;
            break;
        case RALPH_PARAM_LP_EXTERNAL_PROVIDER:
            *value = model->lp_external_provider;
            break;
        case RALPH_PARAM_LP_EXTERNAL_STRICT:
            *value = model->lp_external_strict;
            break;
        case RALPH_PARAM_LP_BASIS_GOVERNOR_MODE:
            *value = model->lp_basis_governor_mode;
            break;
        case RALPH_PARAM_LP_REINVERT_CONTROLLER_MODE:
            *value = model->lp_reinvert_controller_mode;
            break;
        case RALPH_PARAM_LP_POLICY_PROFILE:
            *value = model->lp_policy_profile;
            break;
        case RALPH_PARAM_GLPK_SMCP_METHOD:
            *value = model->glpk_smcp_method;
            break;
        case RALPH_PARAM_GLPK_SMCP_PRICING:
            *value = model->glpk_smcp_pricing;
            break;
        case RALPH_PARAM_GLPK_SMCP_RATIO:
            *value = model->glpk_smcp_ratio;
            break;
        case RALPH_PARAM_GLPK_SMCP_FLIP:
            *value = model->glpk_smcp_flip;
            break;
        case RALPH_PARAM_GLPK_SMCP_BASIS:
            *value = model->glpk_smcp_basis;
            break;
        case RALPH_PARAM_GLPK_SMCP_PRESOLVE:
            *value = model->glpk_smcp_presolve;
            break;
        case RALPH_PARAM_GLPK_SMCP_EXCL:
            *value = model->glpk_smcp_excl;
            break;
        case RALPH_PARAM_GLPK_SMCP_SHIFT:
            *value = model->glpk_smcp_shift;
            break;
        case RALPH_PARAM_GLPK_SMCP_AORN:
            *value = model->glpk_smcp_aorn;
            break;
        case RALPH_PARAM_GLPK_BFCP_FACTORIZATION:
            *value = model->glpk_bfcp_factorization;
            break;
        case RALPH_PARAM_GLPK_BFCP_BACKEND:
            *value = model->glpk_bfcp_backend;
            break;
        case RALPH_PARAM_GLPK_BFCP_UPDATE_LIMIT:
            *value = model->glpk_bfcp_update_limit;
            break;
        case RALPH_PARAM_GLPK_BFCP_PIVOT_LIMIT:
            *value = model->glpk_bfcp_pivot_limit;
            break;
        case RALPH_PARAM_GLPK_BFCP_SUHL:
            *value = model->glpk_bfcp_suhl;
            break;
        case RALPH_PARAM_GLPK_BFCP_NFS_MAX:
            *value = model->glpk_bfcp_nfs_max;
            break;
        case RALPH_PARAM_GLPK_BFCP_NRS_MAX:
            *value = model->glpk_bfcp_nrs_max;
            break;
        case RALPH_PARAM_REFACTOR_MIN_INTERVAL:
            *value = model->refactor_min_interval;
            break;
        case RALPH_PARAM_REFACTOR_MAX_INTERVAL:
            *value = model->refactor_max_interval;
            break;
        case RALPH_PARAM_DEGEN_ESCAPE_MIN_M:
            *value = model->degen_escape_min_m;
            break;
        case RALPH_PARAM_LU_SPIKE_WARN_PCT:
            *value = model->lu_spike_warn_pct;
            break;
        default:
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_PARAMETER,
                           RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_PARAMETER,
                           param,
                           0,
                           "unsupported parameter id");
    }

    return 0;
}

int ralph_core_get_dbl_param_id(const RalphModel *model, RalphParamId param, double *value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!model || !value || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "model or output pointer is null");
    }
    if (!spec) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       RALPH_PARAM_COUNT,
                       "unknown parameter id");
    }
    if (spec->value_type != RALPH_PARAM_VALUE_DOUBLE) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       spec->value_type,
                       "parameter is not double-valued");
    }

    switch (param) {
        case RALPH_PARAM_TIME_LIMIT:
            *value = model->time_limit;
            break;
        case RALPH_PARAM_MIP_GAP:
            *value = model->mip_gap;
            break;
        case RALPH_PARAM_OBJ_LIMIT:
            *value = model->objective_limit;
            break;
        case RALPH_PARAM_FEAS_TOL:
            *value = model->lp_model->feas_tol;
            break;
        case RALPH_PARAM_OPT_TOL:
            *value = model->lp_model->opt_tol;
            break;
        case RALPH_PARAM_PIVOT_TOL:
            *value = model->lp_model->pivot_tol;
            break;
        case RALPH_PARAM_GLPK_SMCP_TOL_BND:
            *value = model->glpk_smcp_tol_bnd;
            break;
        case RALPH_PARAM_GLPK_SMCP_TOL_DJ:
            *value = model->glpk_smcp_tol_dj;
            break;
        case RALPH_PARAM_GLPK_SMCP_TOL_PIV:
            *value = model->glpk_smcp_tol_piv;
            break;
        case RALPH_PARAM_GLPK_BFCP_PIVOT_TOL:
            *value = model->glpk_bfcp_pivot_tol;
            break;
        case RALPH_PARAM_GLPK_BFCP_EPS_TOL:
            *value = model->glpk_bfcp_eps_tol;
            break;
        case RALPH_PARAM_GLPK_BFCP_GROWTH_GUARD:
            *value = model->glpk_bfcp_growth_guard;
            break;
        case RALPH_PARAM_LU_COST_EWMA_ALPHA:
            *value = model->lu_cost_ewma_alpha;
            break;
        default:
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_PARAMETER,
                           RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_PARAMETER,
                           param,
                           0,
                           "unsupported parameter id");
    }

    return 0;
}

int ralph_core_set_lp_int_param_id(RalphModel *model, RalphParamId param, int value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!spec) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       RALPH_PARAM_COUNT,
                       "unknown parameter id");
    }
    if (spec->value_type != RALPH_PARAM_VALUE_INT) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       spec->value_type,
                       "parameter is not integer-valued");
    }
    if (!ralph_param_scope_allows_lp(spec->scope)) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_PARAMETER_SCOPE_MISMATCH,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       spec->scope,
                       "parameter is not in LP scope");
    }
    return ralph_core_set_int_param_id(model, param, value);
}

int ralph_core_set_lp_dbl_param_id(RalphModel *model, RalphParamId param, double value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!spec) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       RALPH_PARAM_COUNT,
                       "unknown parameter id");
    }
    if (spec->value_type != RALPH_PARAM_VALUE_DOUBLE) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       spec->value_type,
                       "parameter is not double-valued");
    }
    if (!ralph_param_scope_allows_lp(spec->scope)) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_PARAMETER_SCOPE_MISMATCH,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       spec->scope,
                       "parameter is not in LP scope");
    }
    return ralph_core_set_dbl_param_id(model, param, value);
}

int ralph_core_get_lp_int_param_id(const RalphModel *model, RalphParamId param, int *value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!spec) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       RALPH_PARAM_COUNT,
                       "unknown parameter id");
    }
    if (spec->value_type != RALPH_PARAM_VALUE_INT) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       spec->value_type,
                       "parameter is not integer-valued");
    }
    if (!ralph_param_scope_allows_lp(spec->scope)) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_PARAMETER_SCOPE_MISMATCH,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       spec->scope,
                       "parameter is not in LP scope");
    }
    return ralph_core_get_int_param_id(model, param, value);
}

int ralph_core_get_lp_dbl_param_id(const RalphModel *model, RalphParamId param, double *value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!spec) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       RALPH_PARAM_COUNT,
                       "unknown parameter id");
    }
    if (spec->value_type != RALPH_PARAM_VALUE_DOUBLE) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       spec->value_type,
                       "parameter is not double-valued");
    }
    if (!ralph_param_scope_allows_lp(spec->scope)) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_PARAMETER_SCOPE_MISMATCH,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       spec->scope,
                       "parameter is not in LP scope");
    }
    return ralph_core_get_dbl_param_id(model, param, value);
}

int ralph_core_set_mip_int_param_id(RalphModel *model, RalphParamId param, int value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!spec) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       RALPH_PARAM_COUNT,
                       "unknown parameter id");
    }
    if (spec->value_type != RALPH_PARAM_VALUE_INT) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       spec->value_type,
                       "parameter is not integer-valued");
    }
    if (!ralph_param_scope_allows_mip(spec->scope)) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_PARAMETER_SCOPE_MISMATCH,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       spec->scope,
                       "parameter is not in MIP scope");
    }
    return ralph_core_set_int_param_id(model, param, value);
}

int ralph_core_set_mip_dbl_param_id(RalphModel *model, RalphParamId param, double value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!spec) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       RALPH_PARAM_COUNT,
                       "unknown parameter id");
    }
    if (spec->value_type != RALPH_PARAM_VALUE_DOUBLE) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       spec->value_type,
                       "parameter is not double-valued");
    }
    if (!ralph_param_scope_allows_mip(spec->scope)) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_PARAMETER_SCOPE_MISMATCH,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       spec->scope,
                       "parameter is not in MIP scope");
    }
    return ralph_core_set_dbl_param_id(model, param, value);
}

int ralph_core_get_mip_int_param_id(const RalphModel *model, RalphParamId param, int *value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!spec) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       RALPH_PARAM_COUNT,
                       "unknown parameter id");
    }
    if (spec->value_type != RALPH_PARAM_VALUE_INT) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       spec->value_type,
                       "parameter is not integer-valued");
    }
    if (!ralph_param_scope_allows_mip(spec->scope)) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_PARAMETER_SCOPE_MISMATCH,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       spec->scope,
                       "parameter is not in MIP scope");
    }
    return ralph_core_get_int_param_id(model, param, value);
}

int ralph_core_get_mip_dbl_param_id(const RalphModel *model, RalphParamId param, double *value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!spec) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       RALPH_PARAM_COUNT,
                       "unknown parameter id");
    }
    if (spec->value_type != RALPH_PARAM_VALUE_DOUBLE) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       spec->value_type,
                       "parameter is not double-valued");
    }
    if (!ralph_param_scope_allows_mip(spec->scope)) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_PARAMETER_SCOPE_MISMATCH,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       param,
                       spec->scope,
                       "parameter is not in MIP scope");
    }
    return ralph_core_get_dbl_param_id(model, param, value);
}

int ralph_core_set_int_param(RalphModel *model, const char *name, int value) {
    RalphParamId param;
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!model || !name) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "model or name is null");
    }
    if (ralph_core_find_param_by_name(name, &param) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "unknown parameter name");
    }
    return ralph_core_set_int_param_id(model, param, value);
}

int ralph_core_set_dbl_param(RalphModel *model, const char *name, double value) {
    RalphParamId param;
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!model || !name) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "model or name is null");
    }
    if (ralph_core_find_param_by_name(name, &param) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "unknown parameter name");
    }
    return ralph_core_set_dbl_param_id(model, param, value);
}

int ralph_core_get_int_param(const RalphModel *model, const char *name, int *value) {
    RalphParamId param;
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!model || !name || !value) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "model, name, or output pointer is null");
    }
    if (ralph_core_find_param_by_name(name, &param) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "unknown parameter name");
    }
    return ralph_core_get_int_param_id(model, param, value);
}

int ralph_core_get_dbl_param(const RalphModel *model, const char *name, double *value) {
    RalphParamId param;
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!model || !name || !value) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "model, name, or output pointer is null");
    }
    if (ralph_core_find_param_by_name(name, &param) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "unknown parameter name");
    }
    return ralph_core_get_dbl_param_id(model, param, value);
}

int ralph_core_set_lp_int_param(RalphModel *model, const char *name, int value) {
    RalphParamId param;
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!name) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "name is null");
    }
    if (ralph_core_find_param_by_name(name, &param) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "unknown parameter name");
    }
    return ralph_core_set_lp_int_param_id(model, param, value);
}

int ralph_core_set_lp_dbl_param(RalphModel *model, const char *name, double value) {
    RalphParamId param;
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!name) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "name is null");
    }
    if (ralph_core_find_param_by_name(name, &param) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "unknown parameter name");
    }
    return ralph_core_set_lp_dbl_param_id(model, param, value);
}

int ralph_core_get_lp_int_param(const RalphModel *model, const char *name, int *value) {
    RalphParamId param;
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!name || !value) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "name or output pointer is null");
    }
    if (ralph_core_find_param_by_name(name, &param) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "unknown parameter name");
    }
    return ralph_core_get_lp_int_param_id(model, param, value);
}

int ralph_core_get_lp_dbl_param(const RalphModel *model, const char *name, double *value) {
    RalphParamId param;
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!name || !value) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "name or output pointer is null");
    }
    if (ralph_core_find_param_by_name(name, &param) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "unknown parameter name");
    }
    return ralph_core_get_lp_dbl_param_id(model, param, value);
}

int ralph_core_set_mip_int_param(RalphModel *model, const char *name, int value) {
    RalphParamId param;
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!name) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "name is null");
    }
    if (ralph_core_find_param_by_name(name, &param) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "unknown parameter name");
    }
    return ralph_core_set_mip_int_param_id(model, param, value);
}

int ralph_core_set_mip_dbl_param(RalphModel *model, const char *name, double value) {
    RalphParamId param;
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!name) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "name is null");
    }
    if (ralph_core_find_param_by_name(name, &param) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "unknown parameter name");
    }
    return ralph_core_set_mip_dbl_param_id(model, param, value);
}

int ralph_core_get_mip_int_param(const RalphModel *model, const char *name, int *value) {
    RalphParamId param;
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!name || !value) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "name or output pointer is null");
    }
    if (ralph_core_find_param_by_name(name, &param) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "unknown parameter name");
    }
    return ralph_core_get_mip_int_param_id(model, param, value);
}

int ralph_core_get_mip_dbl_param(const RalphModel *model, const char *name, double *value) {
    RalphParamId param;
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!name || !value) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "name or output pointer is null");
    }
    if (ralph_core_find_param_by_name(name, &param) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_UNKNOWN_PARAMETER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_PARAMETER,
                       0,
                       0,
                       "unknown parameter name");
    }
    return ralph_core_get_mip_dbl_param_id(model, param, value);
}

/* ============================================================================
 * Utility
 * ============================================================================ */

