/*
 * Ralph - Main Public API Implementation
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include "ralph_core.h"
#include "lp.h"
#include "mip.h"
#include "presolve.h"
#include "detect.h"
#include "benders.h"
#include "lp_conflict.h"
#include "lp_error.h"
#include "lp_backend.h"
#include "lp_dispatch.h"
#include "lp_external_adapter.h"
#include "lp_external_glpk_oop.h"
#include "lp_policy_glpk_compat.h"

#define RALPH_VERSION "0.1.0"

/* Forward declarations */
int lp_model_finalize(LPModel *model);

/* ============================================================================
 * Internal Model Structure
 * ============================================================================ */

struct RalphModel {
    LPModel *lp_model;
    SimplexSolver *lp_solver;
    MIPSolver *mip_solver;

    /* Parameters */
    int max_iterations;
    double time_limit;
    int presolve;
    int verbose;
    int telemetry;  /* 1=collect LP/LU telemetry counters/timers */
    double mip_gap;
    int max_nodes;
    int max_cut_rounds;
    int method;  /* 0=primal simplex, 1=dual simplex, 2=auto */
    int lp_algorithm;      /* Requested LP algorithm (extends method with barrier value). */
    int barrier_crossover; /* Requested barrier crossover mode (API-level capability gate). */
    int lp_external_provider; /* Requested external LP provider for explicit external algorithms. */
    int lp_external_strict;   /* 1=error on explicit external request when provider/backend unavailable */
    int pricing; /* 0=Dantzig, 1=Steepest edge, 2=Devex (default), 3=Partial */
    int detect_special; /* 1=detect LAP/network structure, 0=disable */
    int node_pool_capacity; /* Pre-allocated B&B node pool size (default 1024) */
    int node_select;  /* 0=best-first, 1=DFS, 2=best-estimate, 3=hybrid (default) */
    unsigned int presolve_mask; /* Bitmask controlling presolve techniques (default PRESOLVE_SAFE=0x110F) */
    int force_two_phase; /* 1=force two-phase simplex for clean Farkas duals */
    int trace_phase1; /* 1=emit deterministic Phase-1 failure trace */
    int scaling;            /* 0=off, 1=single-round (default), N=N geo rounds + equilibrium */
    int crash;              /* 0=off, 1=triangular crash basis */
    int verify;             /* 0=off, 1=post-solve verification */
    double objective_limit; /* Early-exit obj limit (user space) */
    int phase1_pricing;     /* Override pricing for Phase 1: 0=Dantzig, -1=disabled */
    int dual_bound_flip;    /* -1=default(on), 0=off, 1=on */
    int dual_steepest_edge; /* -1=default(on), 0=off, 1=on */
    int var_select;         /* -1=default, 0=most_infeas, 1=pseudo_cost, 2=strong, 3=reliability */
    int lu_supernode;       /* 0=off (default), 1=enable supernodal LU factorization (T2.1) */
    int deterministic;      /* 1=enforce deterministic LP runtime policy */
    int random_seed;        /* deterministic LP seed for anti-cycling perturbation offsets */
    int lp_threads;         /* LP thread policy (0=auto; deterministic mode defaults to 1) */
    int lp_basis_governor_mode; /* 0=off, 1=shadow, 2=control_phase2 */
    int lp_policy_profile;  /* 0=default, 1=glpk_compat */
    int glpk_smcp_method;   /* 0=auto, 1=primal, 2=dual */
    int glpk_smcp_pricing;  /* 0=standard, 1=steep */
    int glpk_smcp_ratio;    /* 0=standard, 1=harris */
    int glpk_smcp_flip;     /* 0=off, 1=on */
    int glpk_smcp_basis;    /* 0=adv, 1=std */
    int glpk_smcp_presolve; /* 0=auto, 1=off, 2=on */
    int glpk_bfcp_backend;  /* 0=luf_ft, 1=cbg, 2=cgr */
    int glpk_bfcp_update_limit; /* -1=auto */
    double glpk_bfcp_pivot_tol; /* <=0=auto */
    double glpk_bfcp_growth_guard; /* <=0=auto */

    /* Solution */
    RalphStatus status;
    double obj_value;
    double *solution;
    double *dual_solution;
    double *reduced_costs;
    double *unbounded_ray;
    int unbounded_ray_valid;

    /* MIP-specific */
    double best_bound;
    int node_count;
    double *mip_start;
    int *mip_start_mask;
    int mip_start_n;
    int mip_start_nnz;
    RalphMIPStartStatus mip_start_status;
    RalphMIPStartRepairMode mip_start_repair_mode;

    /* Branching control (stored until MIP solver is created) */
    int *branch_priorities;
    int *branch_directions;

    /* Cut callback (stored until MIP solver is created) */
    RalphCutCallback cut_callback;
    int has_cut_callback;

    /* Branch callback (stored until MIP solver is created) */
    RalphBranchCallback branch_callback;
    int has_branch_callback;

    /* LP-only progress/cancel callbacks (never propagated to MIP callbacks) */
    RalphLPProgressCallback lp_progress_callback;
    int has_lp_progress_callback;
    RalphLPCancelCallback lp_cancel_callback;
    int has_lp_cancel_callback;

    /* Statistics */
    int iteration_count;
    RalphPresolveReport last_presolve_report;
    RalphLPSolveAlgorithmReport last_lp_algorithm_report;
    int last_lp_algorithm_report_valid;
    RalphLPExternalFailureReport last_lp_external_failure_report;
    int last_lp_external_failure_report_valid;

    /* Basis staged before first optimize() (applied when simplex tableau is created) */
    int staged_basis_m;
    int staged_basis_n;
    int *staged_basis;
    VarStatus *staged_var_status;

    /* Structured API error state (per-model). */
    LPAPIErrorState error_state;
};

/* Basis representation for warm start */
struct RalphBasis {
    int m;              /* Number of constraints */
    int n;              /* Number of extended variables */
    int *basis;         /* Basic variable indices (size m) */
    VarStatus *var_status;  /* Variable status array (size n) */
};

typedef struct {
    RalphLPExternalAdapter adapter;
} RalphLPExternalAdapterBridgeEntry;

static int ralph_lp_external_provider_valid_public(RalphLPExternalProvider provider) {
    return provider >= RALPH_LP_EXTERNAL_PROVIDER_GLPK &&
           provider <= RALPH_LP_EXTERNAL_PROVIDER_GLOP;
}

static LPExternalProvider ralph_lp_external_provider_to_internal(
    RalphLPExternalProvider provider) {
    switch (provider) {
        case RALPH_LP_EXTERNAL_PROVIDER_GLPK:
            return LP_EXTERNAL_PROVIDER_GLPK;
        case RALPH_LP_EXTERNAL_PROVIDER_HIGHS:
            return LP_EXTERNAL_PROVIDER_HIGHS;
        case RALPH_LP_EXTERNAL_PROVIDER_CLP:
            return LP_EXTERNAL_PROVIDER_CLP;
        case RALPH_LP_EXTERNAL_PROVIDER_CPLEX:
            return LP_EXTERNAL_PROVIDER_CPLEX;
        case RALPH_LP_EXTERNAL_PROVIDER_GUROBI:
            return LP_EXTERNAL_PROVIDER_GUROBI;
        case RALPH_LP_EXTERNAL_PROVIDER_GLOP:
            return LP_EXTERNAL_PROVIDER_GLOP;
        case RALPH_LP_EXTERNAL_PROVIDER_NONE:
        default:
            return LP_EXTERNAL_PROVIDER_NONE;
    }
}

static RalphLPExternalProvider ralph_lp_external_provider_from_internal(
    LPExternalProvider provider) {
    switch (provider) {
        case LP_EXTERNAL_PROVIDER_GLPK:
            return RALPH_LP_EXTERNAL_PROVIDER_GLPK;
        case LP_EXTERNAL_PROVIDER_HIGHS:
            return RALPH_LP_EXTERNAL_PROVIDER_HIGHS;
        case LP_EXTERNAL_PROVIDER_CLP:
            return RALPH_LP_EXTERNAL_PROVIDER_CLP;
        case LP_EXTERNAL_PROVIDER_CPLEX:
            return RALPH_LP_EXTERNAL_PROVIDER_CPLEX;
        case LP_EXTERNAL_PROVIDER_GUROBI:
            return RALPH_LP_EXTERNAL_PROVIDER_GUROBI;
        case LP_EXTERNAL_PROVIDER_GLOP:
            return RALPH_LP_EXTERNAL_PROVIDER_GLOP;
        case LP_EXTERNAL_PROVIDER_NONE:
        default:
            return RALPH_LP_EXTERNAL_PROVIDER_NONE;
    }
}

static RalphLPExternalBackendKind ralph_lp_external_backend_from_internal(
    LPExternalBackendKind backend) {
    switch (backend) {
        case LP_EXTERNAL_BACKEND_DUAL_SIMPLEX:
            return RALPH_LP_EXTERNAL_BACKEND_DUAL_SIMPLEX;
        case LP_EXTERNAL_BACKEND_BARRIER:
            return RALPH_LP_EXTERNAL_BACKEND_BARRIER;
        case LP_EXTERNAL_BACKEND_SIMPLEX:
        default:
            return RALPH_LP_EXTERNAL_BACKEND_SIMPLEX;
    }
}

static int ralph_lp_external_backend_from_algorithm(RalphLPAlgorithm algorithm,
                                                    RalphLPExternalBackendKind *backend_out) {
    if (!backend_out) return -1;
    switch (algorithm) {
        case RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL:
            *backend_out = RALPH_LP_EXTERNAL_BACKEND_SIMPLEX;
            return 0;
        case RALPH_LP_ALGORITHM_DUAL_SIMPLEX_EXTERNAL:
            *backend_out = RALPH_LP_EXTERNAL_BACKEND_DUAL_SIMPLEX;
            return 0;
        case RALPH_LP_ALGORITHM_BARRIER_EXTERNAL:
            *backend_out = RALPH_LP_EXTERNAL_BACKEND_BARRIER;
            return 0;
        default:
            return -1;
    }
}

static int ralph_lp_dispatch_backend_is_external(LPDispatchBackend backend) {
    return backend == LP_DISPATCH_BACKEND_SIMPLEX_EXTERNAL ||
           backend == LP_DISPATCH_BACKEND_DUAL_SIMPLEX_EXTERNAL ||
           backend == LP_DISPATCH_BACKEND_BARRIER_EXTERNAL;
}

static int ralph_lp_external_backend_from_dispatch_backend(
    LPDispatchBackend backend,
    RalphLPExternalBackendKind *backend_out) {
    if (!backend_out) return -1;
    switch (backend) {
        case LP_DISPATCH_BACKEND_SIMPLEX_EXTERNAL:
            *backend_out = RALPH_LP_EXTERNAL_BACKEND_SIMPLEX;
            return 0;
        case LP_DISPATCH_BACKEND_DUAL_SIMPLEX_EXTERNAL:
            *backend_out = RALPH_LP_EXTERNAL_BACKEND_DUAL_SIMPLEX;
            return 0;
        case LP_DISPATCH_BACKEND_BARRIER_EXTERNAL:
            *backend_out = RALPH_LP_EXTERNAL_BACKEND_BARRIER;
            return 0;
        default:
            return -1;
    }
}

static void ralph_set_api_error(const RalphModel *model,
                                RalphErrorDomain domain,
                                RalphErrorCode code,
                                RalphStatus status_hint,
                                RalphErrorAPIId api_id,
                                int detail_i0,
                                int detail_i1,
                                const char *message) {
    if (model) {
        lp_error_state_set(&((RalphModel*)model)->error_state,
                           domain,
                           code,
                           status_hint,
                           api_id,
                           detail_i0,
                           detail_i1,
                           message);
    } else {
        lp_error_tls_set(domain,
                         code,
                         status_hint,
                         api_id,
                         detail_i0,
                         detail_i1,
                         message);
    }
}

static void ralph_clear_api_error(const RalphModel *model) {
    if (model) {
        lp_error_state_clear(&((RalphModel*)model)->error_state);
    } else {
        lp_error_tls_clear();
    }
}

#define RALPH_CLEAR_API_ERROR(model_ptr) \
    ralph_clear_api_error((const RalphModel*)(model_ptr))

#define RALPH_FAIL_API(model_ptr, domain, code, status_hint, api_id, detail0, detail1, msg) \
    do { \
        ralph_set_api_error((const RalphModel*)(model_ptr), \
                            (domain), \
                            (code), \
                            (status_hint), \
                            (api_id), \
                            (detail0), \
                            (detail1), \
                            (msg)); \
        return -1; \
    } while (0)

#define RALPH_FAIL_API_PTR(model_ptr, domain, code, status_hint, api_id, detail0, detail1, msg) \
    do { \
        ralph_set_api_error((const RalphModel*)(model_ptr), \
                            (domain), \
                            (code), \
                            (status_hint), \
                            (api_id), \
                            (detail0), \
                            (detail1), \
                            (msg)); \
        return NULL; \
    } while (0)

static int ralph_lp_external_bridge_get_capabilities(LPExternalCapabilities *caps,
                                                      void *user_data) {
    RalphLPExternalAdapterBridgeEntry *entry =
        (RalphLPExternalAdapterBridgeEntry*)user_data;
    RalphLPExternalCapabilities public_caps;

    if (!entry || !entry->adapter.get_capabilities || !caps) return -1;
    memset(&public_caps, 0, sizeof(public_caps));
    if (entry->adapter.get_capabilities(&public_caps, entry->adapter.user_data) != 0) return -1;

    caps->supports_simplex = public_caps.supports_simplex ? 1 : 0;
    caps->supports_dual_simplex = public_caps.supports_dual_simplex ? 1 : 0;
    caps->supports_barrier = public_caps.supports_barrier ? 1 : 0;
    caps->supports_crossover = public_caps.supports_crossover ? 1 : 0;
    return 0;
}

static int ralph_lp_external_bridge_solve(LPExternalBackendKind backend,
                                          SimplexSolver *solver,
                                          void *user_data) {
    RalphLPExternalAdapterBridgeEntry *entry =
        (RalphLPExternalAdapterBridgeEntry*)user_data;
    RalphLPExternalBackendKind public_backend =
        ralph_lp_external_backend_from_internal(backend);

    if (!entry || !entry->adapter.solve) return -1;
    return entry->adapter.solve(public_backend, (void*)solver, entry->adapter.user_data);
}

static void ralph_lp_external_bridge_entry_destroy(void *user_data) {
    free(user_data);
}

static void ralph_clear_staged_basis(RalphModel *model) {
    if (!model) return;
    free(model->staged_basis);
    free(model->staged_var_status);
    model->staged_basis = NULL;
    model->staged_var_status = NULL;
    model->staged_basis_m = 0;
    model->staged_basis_n = 0;
}

static void ralph_clear_mip_start_internal(RalphModel *model) {
    if (!model) return;
    free(model->mip_start);
    free(model->mip_start_mask);
    model->mip_start = NULL;
    model->mip_start_mask = NULL;
    model->mip_start_n = 0;
    model->mip_start_nnz = 0;
    model->mip_start_status = RALPH_MIP_START_NONE;
}

static void ralph_reset_presolve_report(RalphModel *model) {
    if (!model) return;
    memset(&model->last_presolve_report, 0, sizeof(model->last_presolve_report));
}

static int ralph_set_requested_lp_algorithm_internal(RalphModel *model, int value) {
    int normalized_algorithm = 0;
    int legacy_method = 0;
    if (!model) return -1;
    if (lp_dispatch_set_requested_algorithm(value,
                                            &normalized_algorithm,
                                            &legacy_method) != 0) {
        return -1;
    }
    model->lp_algorithm = normalized_algorithm;
    model->method = legacy_method;
    return 0;
}

static int ralph_set_requested_barrier_crossover_internal(RalphModel *model, int value) {
    int normalized_crossover = 0;
    if (!model) return -1;
    if (lp_dispatch_set_requested_crossover(value, &normalized_crossover) != 0) {
        return -1;
    }
    model->barrier_crossover = normalized_crossover;
    return 0;
}

static int ralph_set_requested_lp_external_provider_internal(RalphModel *model, int value) {
    int normalized_external_provider = 0;
    if (!model) return -1;
    if (lp_dispatch_set_requested_external_provider(value,
                                                    &normalized_external_provider) != 0) {
        return -1;
    }
    model->lp_external_provider = normalized_external_provider;
    return 0;
}

static void ralph_glpk_policy_config_from_model(const RalphModel *model,
                                                LPGLPKCompatConfig *cfg) {
    if (!cfg) return;
    lp_policy_glpk_compat_init(cfg);
    if (!model) return;

    cfg->lp_policy_profile = model->lp_policy_profile;
    cfg->glpk_smcp_method = model->glpk_smcp_method;
    cfg->glpk_smcp_pricing = model->glpk_smcp_pricing;
    cfg->glpk_smcp_ratio = model->glpk_smcp_ratio;
    cfg->glpk_smcp_flip = model->glpk_smcp_flip;
    cfg->glpk_smcp_basis = model->glpk_smcp_basis;
    cfg->glpk_smcp_presolve = model->glpk_smcp_presolve;
    cfg->glpk_bfcp_backend = model->glpk_bfcp_backend;
    cfg->glpk_bfcp_update_limit = model->glpk_bfcp_update_limit;
    cfg->glpk_bfcp_pivot_tol = model->glpk_bfcp_pivot_tol;
    cfg->glpk_bfcp_growth_guard = model->glpk_bfcp_growth_guard;
}

static void ralph_glpk_policy_config_to_model(RalphModel *model,
                                              const LPGLPKCompatConfig *cfg) {
    if (!model || !cfg) return;
    model->lp_policy_profile = cfg->lp_policy_profile;
    model->glpk_smcp_method = cfg->glpk_smcp_method;
    model->glpk_smcp_pricing = cfg->glpk_smcp_pricing;
    model->glpk_smcp_ratio = cfg->glpk_smcp_ratio;
    model->glpk_smcp_flip = cfg->glpk_smcp_flip;
    model->glpk_smcp_basis = cfg->glpk_smcp_basis;
    model->glpk_smcp_presolve = cfg->glpk_smcp_presolve;
    model->glpk_bfcp_backend = cfg->glpk_bfcp_backend;
    model->glpk_bfcp_update_limit = cfg->glpk_bfcp_update_limit;
    model->glpk_bfcp_pivot_tol = cfg->glpk_bfcp_pivot_tol;
    model->glpk_bfcp_growth_guard = cfg->glpk_bfcp_growth_guard;
}

static int ralph_set_glpk_policy_int_param(RalphModel *model,
                                           RalphParamId param,
                                           int value) {
    LPGLPKCompatConfig cfg;
    ralph_glpk_policy_config_from_model(model, &cfg);

    switch (param) {
        case RALPH_PARAM_LP_POLICY_PROFILE:
            cfg.lp_policy_profile = value;
            lp_policy_glpk_compat_apply_profile_defaults(&cfg);
            break;
        case RALPH_PARAM_GLPK_SMCP_METHOD:
            cfg.glpk_smcp_method = value;
            break;
        case RALPH_PARAM_GLPK_SMCP_PRICING:
            cfg.glpk_smcp_pricing = value;
            break;
        case RALPH_PARAM_GLPK_SMCP_RATIO:
            cfg.glpk_smcp_ratio = value;
            break;
        case RALPH_PARAM_GLPK_SMCP_FLIP:
            cfg.glpk_smcp_flip = value;
            break;
        case RALPH_PARAM_GLPK_SMCP_BASIS:
            cfg.glpk_smcp_basis = value;
            break;
        case RALPH_PARAM_GLPK_SMCP_PRESOLVE:
            cfg.glpk_smcp_presolve = value;
            break;
        case RALPH_PARAM_GLPK_BFCP_BACKEND:
            cfg.glpk_bfcp_backend = value;
            break;
        case RALPH_PARAM_GLPK_BFCP_UPDATE_LIMIT:
            cfg.glpk_bfcp_update_limit = value;
            break;
        default:
            return -1;
    }

    if (!lp_policy_glpk_compat_validate(&cfg)) return -1;
    ralph_glpk_policy_config_to_model(model, &cfg);
    return 0;
}

static int ralph_set_glpk_policy_double_param(RalphModel *model,
                                              RalphParamId param,
                                              double value) {
    LPGLPKCompatConfig cfg;
    ralph_glpk_policy_config_from_model(model, &cfg);

    switch (param) {
        case RALPH_PARAM_GLPK_BFCP_PIVOT_TOL:
            cfg.glpk_bfcp_pivot_tol = value;
            break;
        case RALPH_PARAM_GLPK_BFCP_GROWTH_GUARD:
            cfg.glpk_bfcp_growth_guard = value;
            break;
        default:
            return -1;
    }

    if (!lp_policy_glpk_compat_validate(&cfg)) return -1;
    ralph_glpk_policy_config_to_model(model, &cfg);
    return 0;
}

static void ralph_reset_lp_algorithm_report(RalphModel *model) {
    if (!model) return;
    memset(&model->last_lp_algorithm_report, 0, sizeof(model->last_lp_algorithm_report));
    model->last_lp_algorithm_report.requested_algorithm = RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX;
    model->last_lp_algorithm_report.effective_algorithm = RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX;
    model->last_lp_algorithm_report.requested_crossover = RALPH_LP_CROSSOVER_AUTO;
    model->last_lp_algorithm_report.effective_crossover = RALPH_LP_CROSSOVER_AUTO;
    model->last_lp_algorithm_report.fallback_applied = 0;
    model->last_lp_algorithm_report.fallback_reason = RALPH_LP_FALLBACK_NONE;
    model->last_lp_algorithm_report_valid = 0;
}

static void ralph_store_lp_algorithm_report(RalphModel *model,
                                            const RalphLPSolveAlgorithmReport *report) {
    if (!model || !report) return;
    model->last_lp_algorithm_report = *report;
    model->last_lp_algorithm_report_valid = 1;
}

static void ralph_reset_lp_external_failure_report(RalphModel *model) {
    if (!model) return;
    memset(&model->last_lp_external_failure_report, 0,
           sizeof(model->last_lp_external_failure_report));
    model->last_lp_external_failure_report.stage = RALPH_LP_EXTERNAL_FAILURE_STAGE_NONE;
    model->last_lp_external_failure_report.reason = RALPH_LP_EXTERNAL_FAILURE_NONE;
    model->last_lp_external_failure_report.requested_algorithm = RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX;
    model->last_lp_external_failure_report.effective_algorithm = RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX;
    model->last_lp_external_failure_report.requested_provider = RALPH_LP_EXTERNAL_PROVIDER_NONE;
    model->last_lp_external_failure_report.effective_provider = RALPH_LP_EXTERNAL_PROVIDER_NONE;
    model->last_lp_external_failure_report.backend = RALPH_LP_EXTERNAL_BACKEND_SIMPLEX;
    model->last_lp_external_failure_report.fallback_reason = RALPH_LP_FALLBACK_NONE;
    model->last_lp_external_failure_report.adapter_return_code = 0;
    model->last_lp_external_failure_report.mapped_status = RALPH_STATUS_UNKNOWN;
    model->last_lp_external_failure_report.fatal = 0;
    model->last_lp_external_failure_report_valid = 0;
}

static void ralph_store_lp_external_failure_report(
    RalphModel *model,
    const RalphLPExternalFailureReport *report) {
    if (!model || !report) return;
    model->last_lp_external_failure_report = *report;
    model->last_lp_external_failure_report_valid = 1;
}

static int ralph_lp_external_caps_support_backend(
    const LPExternalCapabilities *caps,
    RalphLPExternalBackendKind backend) {
    if (!caps) return 0;
    switch (backend) {
        case RALPH_LP_EXTERNAL_BACKEND_SIMPLEX:
            return caps->supports_simplex ? 1 : 0;
        case RALPH_LP_EXTERNAL_BACKEND_DUAL_SIMPLEX:
            return caps->supports_dual_simplex ? 1 : 0;
        case RALPH_LP_EXTERNAL_BACKEND_BARRIER:
            return caps->supports_barrier ? 1 : 0;
        default:
            return 0;
    }
}

static RalphLPExternalFailureReason ralph_classify_external_dispatch_unavailable(
    RalphLPAlgorithm requested_algorithm,
    RalphLPExternalProvider requested_provider,
    RalphLPExternalBackendKind backend) {
    LPExternalProvider provider_internal;
    LPExternalCapabilities caps;

    if (requested_provider == RALPH_LP_EXTERNAL_PROVIDER_NONE) {
        return RALPH_LP_EXTERNAL_FAILURE_PROVIDER_REQUIRED;
    }
    if (!ralph_lp_external_provider_valid_public(requested_provider)) {
        return RALPH_LP_EXTERNAL_FAILURE_PROVIDER_UNREGISTERED;
    }

    provider_internal = ralph_lp_external_provider_to_internal(requested_provider);
    if (provider_internal == LP_EXTERNAL_PROVIDER_NONE) {
        return RALPH_LP_EXTERNAL_FAILURE_PROVIDER_UNREGISTERED;
    }
    if (lp_external_adapter_is_registered(provider_internal) != 1) {
        return RALPH_LP_EXTERNAL_FAILURE_PROVIDER_UNREGISTERED;
    }

    memset(&caps, 0, sizeof(caps));
    if (lp_external_adapter_get_capabilities(provider_internal, &caps) != 0) {
        return RALPH_LP_EXTERNAL_FAILURE_CAPABILITY_QUERY_FAILED;
    }
    if (!ralph_lp_external_caps_support_backend(&caps, backend)) {
        return RALPH_LP_EXTERNAL_FAILURE_BACKEND_UNSUPPORTED;
    }

    (void)requested_algorithm;
    return RALPH_LP_EXTERNAL_FAILURE_CAPABILITY_QUERY_FAILED;
}

static void ralph_map_external_adapter_rc(int adapter_rc,
                                          RalphLPExternalFailureReason *reason_out,
                                          RalphStatus *status_out) {
    RalphLPExternalFailureReason reason = RALPH_LP_EXTERNAL_FAILURE_ADAPTER_FAILED;
    RalphStatus status = RALPH_STATUS_ERROR;

    switch ((RalphLPExternalAdapterResult)adapter_rc) {
        case RALPH_LP_EXTERNAL_ADAPTER_RC_NUMERICAL_FAILURE:
            reason = RALPH_LP_EXTERNAL_FAILURE_ADAPTER_NUMERICAL_FAILURE;
            status = RALPH_STATUS_ERROR;
            break;
        case RALPH_LP_EXTERNAL_ADAPTER_RC_TIME_LIMIT:
            reason = RALPH_LP_EXTERNAL_FAILURE_ADAPTER_TIME_LIMIT;
            status = RALPH_STATUS_TIME_LIMIT;
            break;
        case RALPH_LP_EXTERNAL_ADAPTER_RC_ITERATION_LIMIT:
            reason = RALPH_LP_EXTERNAL_FAILURE_ADAPTER_ITERATION_LIMIT;
            status = RALPH_STATUS_ITERATION_LIMIT;
            break;
        case RALPH_LP_EXTERNAL_ADAPTER_RC_ERROR:
        default:
            reason = RALPH_LP_EXTERNAL_FAILURE_ADAPTER_FAILED;
            status = RALPH_STATUS_ERROR;
            break;
    }

    if (reason_out) *reason_out = reason;
    if (status_out) *status_out = status;
}

static void ralph_record_external_dispatch_failure(RalphModel *model,
                                                   const LPDispatchPlan *plan,
                                                   int fatal) {
    RalphLPExternalFailureReport report;
    RalphLPExternalBackendKind backend = RALPH_LP_EXTERNAL_BACKEND_SIMPLEX;

    if (!model || !plan) return;
    if (ralph_lp_external_backend_from_algorithm(plan->requested_algorithm, &backend) != 0) return;

    memset(&report, 0, sizeof(report));
    report.stage = RALPH_LP_EXTERNAL_FAILURE_STAGE_DISPATCH;
    report.reason = ralph_classify_external_dispatch_unavailable(
        plan->requested_algorithm,
        plan->requested_external_provider,
        backend);
    report.requested_algorithm = plan->requested_algorithm;
    report.effective_algorithm = plan->effective_algorithm;
    report.requested_provider = plan->requested_external_provider;
    report.effective_provider = plan->effective_external_provider;
    report.backend = backend;
    report.fallback_reason = plan->fallback_reason;
    report.adapter_return_code = 0;
    report.mapped_status = fatal ? RALPH_STATUS_ERROR : RALPH_STATUS_UNKNOWN;
    report.fatal = fatal ? 1 : 0;
    ralph_store_lp_external_failure_report(model, &report);
}

static void ralph_record_external_execution_failure(RalphModel *model,
                                                    const LPDispatchPlan *plan,
                                                    int adapter_rc) {
    RalphLPExternalFailureReport report;
    RalphLPExternalFailureReason reason = RALPH_LP_EXTERNAL_FAILURE_ADAPTER_FAILED;
    RalphStatus mapped_status = RALPH_STATUS_ERROR;
    RalphLPExternalBackendKind backend = RALPH_LP_EXTERNAL_BACKEND_SIMPLEX;

    if (!model || !plan) return;
    if (ralph_lp_external_backend_from_dispatch_backend(plan->effective_backend, &backend) != 0) {
        return;
    }

    ralph_map_external_adapter_rc(adapter_rc, &reason, &mapped_status);

    memset(&report, 0, sizeof(report));
    report.stage = RALPH_LP_EXTERNAL_FAILURE_STAGE_EXECUTION;
    report.reason = reason;
    report.requested_algorithm = plan->requested_algorithm;
    report.effective_algorithm = plan->effective_algorithm;
    report.requested_provider = plan->requested_external_provider;
    report.effective_provider = plan->effective_external_provider;
    report.backend = backend;
    report.fallback_reason = plan->fallback_reason;
    report.adapter_return_code = adapter_rc;
    report.mapped_status = mapped_status;
    report.fatal = 1;
    ralph_store_lp_external_failure_report(model, &report);
}

static int ralph_is_valid_sense(RalphSense sense) {
    return sense == RALPH_LESS_EQUAL || sense == RALPH_EQUAL || sense == RALPH_GREATER_EQUAL;
}

static int ralph_basis_status_from_internal(VarStatus st, RalphBasisStatus *out) {
    if (!out) return -1;
    switch (st) {
        case RALPH_BASIC:
            *out = RALPH_BASIS_STATUS_BASIC;
            return 0;
        case RALPH_NONBASIC_LOWER:
            *out = RALPH_BASIS_STATUS_AT_LOWER;
            return 0;
        case RALPH_NONBASIC_UPPER:
            *out = RALPH_BASIS_STATUS_AT_UPPER;
            return 0;
        case RALPH_NONBASIC_FREE:
            *out = RALPH_BASIS_STATUS_FREE;
            return 0;
        case RALPH_FIXED:
            *out = RALPH_BASIS_STATUS_FIXED;
            return 0;
        default:
            return -1;
    }
}

static int ralph_basis_status_to_internal(RalphBasisStatus in, VarStatus *out) {
    if (!out) return -1;
    switch (in) {
        case RALPH_BASIS_STATUS_BASIC:
            *out = RALPH_BASIC;
            return 0;
        case RALPH_BASIS_STATUS_AT_LOWER:
            *out = RALPH_NONBASIC_LOWER;
            return 0;
        case RALPH_BASIS_STATUS_AT_UPPER:
            *out = RALPH_NONBASIC_UPPER;
            return 0;
        case RALPH_BASIS_STATUS_FREE:
            *out = RALPH_NONBASIC_FREE;
            return 0;
        case RALPH_BASIS_STATUS_FIXED:
            *out = RALPH_FIXED;
            return 0;
        default:
            return -1;
    }
}

static int ralph_build_row_primary_aux_map(const SimplexTableau *tab, int *row_aux) {
    if (!tab || !row_aux || !tab->model) return -1;

    int m = tab->m;
    int num_struct = tab->model->num_vars;
    if (num_struct < 0 || num_struct > tab->n) return -1;

    for (int i = 0; i < m; i++) row_aux[i] = -1;
    if (m == 0) return 0;

    if (!tab->aux_row || tab->num_aux < m) return -1;

    for (int k = 0; k < tab->num_aux; k++) {
        int row = tab->aux_row[k];
        if (row < 0 || row >= m) continue;
        if (row_aux[row] >= 0) continue;

        int var = num_struct + k;
        if (var < 0 || var >= tab->n) continue;
        row_aux[row] = var;
    }

    for (int i = 0; i < m; i++) {
        if (row_aux[i] < 0 || row_aux[i] >= tab->n) return -1;
    }

    return 0;
}

static int ralph_probe_lp_status(const RalphModel *model,
                                 LPModel *probe_model,
                                 RalphStatus *status_out) {
    if (!model || !probe_model || !status_out) return -1;

    LPGLPKCompatConfig glpk_policy_cfg;
    int probe_pricing = model->pricing;
    int probe_phase1_pricing = model->phase1_pricing;

    ralph_glpk_policy_config_from_model(model, &glpk_policy_cfg);
    if (!lp_policy_glpk_compat_validate(&glpk_policy_cfg)) return -1;
    lp_policy_glpk_compat_apply_runtime(&glpk_policy_cfg,
                                        NULL,
                                        &probe_pricing,
                                        &probe_phase1_pricing,
                                        NULL);

    SimplexSolver *probe = simplex_create(probe_model);
    if (!probe) return -1;

    probe->max_iterations = (model->max_iterations > 0) ? model->max_iterations : RALPH_DEFAULT_MAX_ITER;
    probe->time_limit = (model->time_limit > 0.0) ? model->time_limit : RALPH_DEFAULT_TIME_LIMIT;
    probe->verbose = 0;
    probe->telemetry_enabled = 0;
    probe->presolve = 0;
    probe->pricing_strategy = probe_pricing;
    probe->scaling = 0;
    probe->crash = model->crash;
    probe->verify = 0;
    probe->phase1_pricing = probe_phase1_pricing;
    probe->objective_limit = RALPH_INFINITY;
    /* Probe solves prioritize robust infeasibility checks over caller mode. */
    probe->force_two_phase = 1;
    probe->trace_phase1 = 0;
    probe->deterministic = model->deterministic ? 1 : 0;
    probe->random_seed = (model->random_seed >= 0) ? (unsigned int)model->random_seed : 0U;
    probe->lp_threads = (model->lp_threads >= 0) ? model->lp_threads : 0;
    probe->policy.basis_governor_mode = model->lp_basis_governor_mode;
    lp_basis_governor_set_mode(&probe->policy.basis_governor,
                               probe->policy.basis_governor_mode);
    probe->method = 0;  /* Use primal for robust infeasibility checks */

    (void)simplex_solve(probe);
    *status_out = probe->status;
    simplex_free(probe);
    return 0;
}

static int ralph_probe_lp_status_cb(void *ctx, LPModel *probe_model, RalphStatus *status_out) {
    return ralph_probe_lp_status((const RalphModel*)ctx, probe_model, status_out);
}

static int ralph_set_mip_start_copy(RalphModel *model, const double *x,
                                    const int *mask, int n) {
    if (!model || !x || n <= 0) return -1;
    if ((size_t)n > SIZE_MAX / sizeof(double)) return -1;

    double *copy = (double*)malloc((size_t)n * sizeof(double));
    int *mask_copy = (int*)calloc((size_t)n, sizeof(int));
    if (!copy) return -1;
    if (!mask_copy) {
        free(copy);
        return -1;
    }
    memcpy(copy, x, (size_t)n * sizeof(double));

    int nnz = 0;
    if (mask) {
        for (int j = 0; j < n; j++) {
            mask_copy[j] = mask[j] ? 1 : 0;
            nnz += mask_copy[j];
        }
    } else {
        for (int j = 0; j < n; j++) mask_copy[j] = 1;
        nnz = n;
    }

    free(model->mip_start);
    free(model->mip_start_mask);
    model->mip_start = copy;
    model->mip_start_mask = mask_copy;
    model->mip_start_n = n;
    model->mip_start_nnz = nnz;
    model->mip_start_status = RALPH_MIP_START_PENDING;
    return 0;
}

static int ralph_stage_basis_copy(RalphModel *model, const RalphBasis *basis) {
    if (!model || !basis || !basis->basis || !basis->var_status) return -1;

    int *basis_copy = NULL;
    VarStatus *status_copy = NULL;

    if (basis->m > 0) {
        basis_copy = (int*)malloc((size_t)basis->m * sizeof(int));
        if (!basis_copy) return -1;
        memcpy(basis_copy, basis->basis, (size_t)basis->m * sizeof(int));
    }

    if (basis->n > 0) {
        status_copy = (VarStatus*)malloc((size_t)basis->n * sizeof(VarStatus));
        if (!status_copy) {
            free(basis_copy);
            return -1;
        }
        memcpy(status_copy, basis->var_status, (size_t)basis->n * sizeof(VarStatus));
    }

    ralph_clear_staged_basis(model);
    model->staged_basis = basis_copy;
    model->staged_var_status = status_copy;
    model->staged_basis_m = basis->m;
    model->staged_basis_n = basis->n;
    return 0;
}

/* Invalidate any cached solve state after model edits. */
static void ralph_invalidate_solve_state(RalphModel *model) {
    if (!model) return;

    simplex_free(model->lp_solver);
    model->lp_solver = NULL;
    mip_free(model->mip_solver);
    model->mip_solver = NULL;

    free(model->solution);
    model->solution = NULL;
    free(model->dual_solution);
    model->dual_solution = NULL;
    free(model->reduced_costs);
    model->reduced_costs = NULL;
    free(model->unbounded_ray);
    model->unbounded_ray = NULL;
    model->unbounded_ray_valid = 0;
    ralph_clear_staged_basis(model);
    if (model->mip_start) {
        if (model->lp_model && model->mip_start_n == model->lp_model->num_vars) {
            model->mip_start_status = RALPH_MIP_START_PENDING;
        } else {
            ralph_clear_mip_start_internal(model);
        }
    }

    model->status = RALPH_STATUS_UNKNOWN;
    model->obj_value = 0.0;
    model->best_bound = 0.0;
    model->node_count = 0;
    model->iteration_count = 0;
    ralph_reset_presolve_report(model);
    ralph_reset_lp_algorithm_report(model);
    ralph_reset_lp_external_failure_report(model);
}

/* ============================================================================
 * Model Creation/Destruction
 * ============================================================================ */

RalphModel* ralph_core_create(void) {
    RalphModel *model = (RalphModel*)calloc(1, sizeof(RalphModel));
    if (!model) {
        lp_error_tls_set(RALPH_ERROR_DOMAIN_MEMORY,
                         RALPH_ERROR_CODE_ALLOCATION_FAILED,
                         RALPH_STATUS_ERROR,
                         RALPH_ERROR_API_LIFECYCLE,
                         0,
                         0,
                         "failed to allocate model");
        return NULL;
    }

    lp_error_state_init(&model->error_state);

    model->lp_model = lp_model_create();
    if (!model->lp_model) {
        lp_error_tls_set(RALPH_ERROR_DOMAIN_MEMORY,
                         RALPH_ERROR_CODE_ALLOCATION_FAILED,
                         RALPH_STATUS_ERROR,
                         RALPH_ERROR_API_LIFECYCLE,
                         0,
                         0,
                         "failed to allocate lp model");
        free(model);
        return NULL;
    }

    /* Default parameters */
    model->max_iterations = RALPH_DEFAULT_MAX_ITER;
    model->time_limit = RALPH_DEFAULT_TIME_LIMIT;
    model->presolve = 0;  /* Disabled by default - adds overhead on random LPs */
    model->presolve_mask = PRESOLVE_SAFE;
    model->verbose = 0;
    model->telemetry = 1;
    model->mip_gap = RALPH_DEFAULT_MIP_GAP;
    model->max_nodes = RALPH_DEFAULT_NODE_LIMIT;
    model->max_cut_rounds = 0;  /* Disabled by default */
    model->method = 0;  /* Default: primal simplex */
    model->lp_algorithm = (int)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX;
    model->barrier_crossover = (int)RALPH_LP_CROSSOVER_AUTO;
    model->lp_external_provider = (int)RALPH_LP_EXTERNAL_PROVIDER_NONE;
    model->lp_external_strict = 0;
    model->pricing = 2; /* Default: Devex */
    model->scaling = 1;    /* Default: single-round geometric mean */
    model->crash = 0;      /* Default: off (all-slack basis) */
    model->verify = 0;     /* Default: off (no post-solve verification) */
    model->objective_limit = RALPH_INFINITY; /* Default: no limit */
    model->phase1_pricing = -1; /* Default: disabled (use solver pricing) */
    model->detect_special = 0; /* Default: disabled for fair benchmarking */
    model->node_pool_capacity = 1024; /* Default B&B node pool size */
    model->node_select = 3; /* Default: hybrid */
    model->trace_phase1 = 0;
    model->dual_bound_flip = -1;    /* -1 = use default (on) */
    model->dual_steepest_edge = -1; /* -1 = use default (on) */
    model->var_select = -1;         /* -1 = use MIP solver default */
    model->deterministic = 0;
    model->random_seed = 0;
    model->lp_threads = 0;
    model->lp_basis_governor_mode = LP_BASIS_GOV_MODE_OFF;
    {
        LPGLPKCompatConfig cfg;
        lp_policy_glpk_compat_init(&cfg);
        ralph_glpk_policy_config_to_model(model, &cfg);
    }

    model->status = RALPH_STATUS_UNKNOWN;
    model->mip_start = NULL;
    model->mip_start_mask = NULL;
    model->mip_start_n = 0;
    model->mip_start_nnz = 0;
    model->mip_start_status = RALPH_MIP_START_NONE;
    model->mip_start_repair_mode = RALPH_MIP_START_REPAIR_STRICT;
    ralph_reset_presolve_report(model);
    ralph_reset_lp_algorithm_report(model);
    ralph_reset_lp_external_failure_report(model);
    lp_error_tls_clear();

    return model;
}

void ralph_core_free(RalphModel *model) {
    if (!model) return;

    lp_model_free(model->lp_model);
    simplex_free(model->lp_solver);
    mip_free(model->mip_solver);
    free(model->solution);
    free(model->dual_solution);
    free(model->reduced_costs);
    free(model->unbounded_ray);
    ralph_clear_staged_basis(model);
    ralph_clear_mip_start_internal(model);
    free(model->branch_priorities);
    free(model->branch_directions);
    free(model);
}

/* ============================================================================
 * Model Building
 * ============================================================================ */

int ralph_core_set_obj_sense(RalphModel *model, RalphObjSense sense) {
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_BUILD,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    model->lp_model->obj_sense = (int)sense;
    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_core_add_var(RalphModel *model, double lb, double ub, double obj, RalphVarType type) {
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_BUILD,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    int rc = lp_model_add_var(model->lp_model, lb, ub, obj, (char)type);
    if (rc < 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_SOLVER,
                       RALPH_ERROR_CODE_INVALID_ARGUMENT,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_BUILD,
                       rc,
                       0,
                       "failed to add variable");
    }
    ralph_invalidate_solve_state(model);
    return rc;
}

int ralph_core_add_vars(RalphModel *model, int count, const double *lb, const double *ub,
                   const double *obj, const RalphVarType *types) {
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_BUILD,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    if (count <= 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_BUILD,
                       count,
                       0,
                       "count must be positive");
    }

    for (int i = 0; i < count; i++) {
        double l = lb ? lb[i] : 0.0;
        double u = ub ? ub[i] : RALPH_INFINITY;
        double o = obj ? obj[i] : 0.0;
        RalphVarType t = types ? types[i] : RALPH_CONTINUOUS;

        if (lp_model_add_var(model->lp_model, l, u, o, (char)t) < 0) {
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_SOLVER,
                           RALPH_ERROR_CODE_INVALID_ARGUMENT,
                           RALPH_STATUS_ERROR,
                           RALPH_ERROR_API_MODEL_BUILD,
                           i,
                           count,
                           "failed to add variable in batch");
        }
    }

    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_core_add_constraint(RalphModel *model, int nnz, const int *indices,
                         const double *values, RalphSense sense, double rhs) {
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_BUILD,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    int rc = lp_model_add_constraint(model->lp_model, nnz, indices, values, (char)sense, rhs);
    if (rc < 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_SOLVER,
                       RALPH_ERROR_CODE_INVALID_ARGUMENT,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_BUILD,
                       rc,
                       nnz,
                       "failed to add constraint");
    }
    ralph_invalidate_solve_state(model);
    return rc;
}

/* ============================================================================
 * Model Modification
 * ============================================================================ */

int ralph_core_set_var_bounds(RalphModel *model, int var, double lb, double ub) {
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    if (var < 0 || var >= model->lp_model->num_vars) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       var,
                       model->lp_model->num_vars,
                       "variable index out of range");
    }

    model->lp_model->lb[var] = lb;
    model->lp_model->ub[var] = ub;
    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_core_set_var_type(RalphModel *model, int var, RalphVarType type) {
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    if (var < 0 || var >= model->lp_model->num_vars) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       var,
                       model->lp_model->num_vars,
                       "variable index out of range");
    }

    char old_type = model->lp_model->var_type[var];
    model->lp_model->var_type[var] = (char)type;

    /* Update integer counts */
    if ((old_type == 'I' || old_type == 'B') && type == RALPH_CONTINUOUS) {
        model->lp_model->num_integers--;
        if (old_type == 'B') model->lp_model->num_binary--;
    } else if (old_type == 'C' && (type == RALPH_INTEGER || type == RALPH_BINARY)) {
        model->lp_model->num_integers++;
        if (type == RALPH_BINARY) model->lp_model->num_binary++;
    }

    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_core_set_obj_coef(RalphModel *model, int var, double coef) {
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    if (var < 0 || var >= model->lp_model->num_vars) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       var,
                       model->lp_model->num_vars,
                       "variable index out of range");
    }

    model->lp_model->c[var] = coef;
    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_core_set_obj_offset(RalphModel *model, double offset) {
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    model->lp_model->obj_offset = offset;
    ralph_invalidate_solve_state(model);
    return 0;
}

double ralph_core_get_obj_offset(const RalphModel *model) {
    if (!model || !model->lp_model) return 0.0;
    return model->lp_model->obj_offset;
}

/* ============================================================================
 * Model Queries
 * ============================================================================ */

int ralph_core_get_num_vars(const RalphModel *model) {
    return model && model->lp_model ? model->lp_model->num_vars : 0;
}

int ralph_core_get_num_cons(const RalphModel *model) {
    return model && model->lp_model ? model->lp_model->num_cons : 0;
}

int ralph_core_get_num_integers(const RalphModel *model) {
    return model && model->lp_model ? model->lp_model->num_integers : 0;
}

int ralph_core_is_mip(const RalphModel *model) {
    return model && model->lp_model && model->lp_model->num_integers > 0;
}

/* ============================================================================
 * Solving
 * ============================================================================ */

typedef enum {
    RALPH_SOLVE_AUTO = 0,
    RALPH_SOLVE_LP_ONLY = 1,
    RALPH_SOLVE_MIP_ONLY = 2
} RalphSolveMode;

static int ralph_optimize_with_mode(RalphModel *model, RalphSolveMode mode) {
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_SOLVE,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);

    int model_is_mip = ralph_core_is_mip(model);
    if (mode == RALPH_SOLVE_LP_ONLY && model_is_mip) {
        model->status = RALPH_STATUS_ERROR;
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_STATE,
                       RALPH_ERROR_CODE_NOT_AVAILABLE,
                       model->status,
                       RALPH_ERROR_API_SOLVE,
                       (int)mode,
                       1,
                       "LP-only solve requested for MIP model");
    }
    if (mode == RALPH_SOLVE_MIP_ONLY && !model_is_mip) {
        model->status = RALPH_STATUS_ERROR;
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_STATE,
                       RALPH_ERROR_CODE_NOT_AVAILABLE,
                       model->status,
                       RALPH_ERROR_API_SOLVE,
                       (int)mode,
                       0,
                       "MIP-only solve requested for LP model");
    }
    int solve_as_mip = (mode == RALPH_SOLVE_MIP_ONLY) ? 1 :
                       (mode == RALPH_SOLVE_LP_ONLY) ? 0 : model_is_mip;
    LPDispatchPlan lp_dispatch_plan;
    RalphLPSolveAlgorithmReport lp_algorithm_report;
    int lp_algorithm_report_ready = 0;
    int lp_simplex_method = model->method;
    int lp_pricing_strategy = model->pricing;
    int lp_phase1_pricing = model->phase1_pricing;
    int use_presolve = model->presolve;
    LPDispatchBackend lp_effective_backend = LP_DISPATCH_BACKEND_SIMPLEX;
    LPExternalProvider lp_effective_provider = LP_EXTERNAL_PROVIDER_NONE;
    LPGLPKCompatConfig glpk_policy_cfg;

    ralph_glpk_policy_config_from_model(model, &glpk_policy_cfg);
    if (!lp_policy_glpk_compat_validate(&glpk_policy_cfg)) {
        model->status = RALPH_STATUS_ERROR;
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                       model->status,
                       RALPH_ERROR_API_SOLVE,
                       0,
                       0,
                       "invalid glpk-compatible policy configuration");
    }

    ralph_reset_lp_algorithm_report(model);
    ralph_reset_lp_external_failure_report(model);
    if (!solve_as_mip) {
        if (lp_dispatch_build_plan(model->lp_algorithm,
                                   model->lp_external_provider,
                                   model->barrier_crossover,
                                   &lp_dispatch_plan) != 0) {
            model->status = RALPH_STATUS_ERROR;
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_EXTERNAL,
                           RALPH_ERROR_CODE_EXTERNAL_DISPATCH_FAILED,
                           model->status,
                           RALPH_ERROR_API_SOLVE,
                           model->lp_algorithm,
                           model->lp_external_provider,
                           "failed to build LP dispatch plan");
        }
        lp_dispatch_plan_to_report(&lp_dispatch_plan, &lp_algorithm_report);
        lp_simplex_method = lp_dispatch_plan.simplex_method;
        lp_effective_backend = lp_dispatch_plan.effective_backend;
        lp_effective_provider = ralph_lp_external_provider_to_internal(
            lp_dispatch_plan.effective_external_provider);
        if (lp_dispatch_algorithm_is_external(model->lp_algorithm) &&
            lp_dispatch_plan.fallback_applied &&
            lp_dispatch_plan.fallback_reason == RALPH_LP_FALLBACK_EXTERNAL_UNAVAILABLE) {
            ralph_record_external_dispatch_failure(model,
                                                  &lp_dispatch_plan,
                                                  model->lp_external_strict ? 1 : 0);
        }
        if (model->lp_external_strict &&
            lp_dispatch_algorithm_is_external(model->lp_algorithm) &&
            lp_dispatch_plan.fallback_applied &&
            lp_dispatch_plan.fallback_reason == RALPH_LP_FALLBACK_EXTERNAL_UNAVAILABLE) {
            model->status = RALPH_STATUS_ERROR;
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_EXTERNAL,
                           RALPH_ERROR_CODE_EXTERNAL_DISPATCH_FAILED,
                           model->status,
                           RALPH_ERROR_API_SOLVE,
                           lp_dispatch_plan.requested_external_provider,
                           lp_dispatch_plan.fallback_reason,
                           "strict external dispatch failed");
        }
        lp_policy_glpk_compat_apply_runtime(&glpk_policy_cfg,
                                            &lp_simplex_method,
                                            &lp_pricing_strategy,
                                            &lp_phase1_pricing,
                                            &use_presolve);
        lp_algorithm_report_ready = 1;
    }

    /* Finalize model if needed */
    if (!model->lp_model->A) {
        if (lp_model_finalize(model->lp_model) != 0) {
            model->status = RALPH_STATUS_ERROR;
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_SOLVER,
                           RALPH_ERROR_CODE_SOLVE_FAILED,
                           model->status,
                           RALPH_ERROR_API_SOLVE,
                           0,
                           0,
                           "failed to finalize model");
        }
    }

    /* Free previous solution */
    free(model->solution);
    free(model->dual_solution);
    free(model->reduced_costs);
    free(model->unbounded_ray);
    model->solution = NULL;
    model->dual_solution = NULL;
    model->reduced_costs = NULL;
    model->unbounded_ray = NULL;
    model->unbounded_ray_valid = 0;
    ralph_reset_presolve_report(model);

    int n_orig = model->lp_model->num_vars;
    int m_orig = model->lp_model->num_cons;

    /*
     * Try special structure detection BEFORE presolve.
     * LAP and network problems have tight structure that presolve can't simplify,
     * and presolve is expensive for large problems. Detecting structure first
     * and solving directly saves the presolve overhead.
     */

    /* Try LAP detection first (most specific) */
    if (model->detect_special && ralph_get_detect_lap()) {
        LAPSignature lap_sig;
        if (detect_lap(model->lp_model, &lap_sig)) {
            if (model->verbose) {
                printf("Detected LAP structure: %dx%d assignment problem\n",
                       lap_sig.n, lap_sig.n);
                printf("Solving directly with JVC (skipping presolve)\n");
            }

            /* Solve as LAP - bypasses presolve and MIP infrastructure */
            model->solution = (double*)calloc(n_orig, sizeof(double));
            if (model->solution) {
                double lap_obj;
                if (solve_as_lap(&lap_sig, model->solution, &lap_obj) == 0) {
                    model->status = RALPH_STATUS_OPTIMAL;
                    model->obj_value = lap_obj;
                    model->best_bound = lap_obj;
                    model->node_count = 0;
                    model->iteration_count = 0;
                    if (lp_algorithm_report_ready) {
                        ralph_store_lp_algorithm_report(model, &lp_algorithm_report);
                    }

                    detect_lap_free(&lap_sig);
                    return 0;
                }
                free(model->solution);
                model->solution = NULL;
            }
            detect_lap_free(&lap_sig);
            /* Fall through to normal path if LAP solve failed */
        }
    }

    /* Try network detection (more general than LAP) */
    if (model->detect_special && ralph_get_detect_network()) {
        NetworkSignature net_sig;
        if (detect_network(model->lp_model, &net_sig)) {
            RalphNetworkType type = detect_network_type(&net_sig);
            if (model->verbose) {
                const char *type_str = "general";
                if (type == RALPH_NETWORK_ASSIGNMENT) type_str = "assignment";
                else if (type == RALPH_NETWORK_TRANSPORTATION) type_str = "transportation";
                else if (type == RALPH_NETWORK_SHORTEST_PATH) type_str = "shortest path";
                printf("Detected network structure: %d nodes, %d arcs (%s)\n",
                       net_sig.num_nodes, net_sig.num_arcs, type_str);
                printf("Solving with network simplex (skipping presolve)\n");
            }

            /* Solve as network flow - bypasses presolve and MIP infrastructure */
            model->solution = (double*)calloc(n_orig, sizeof(double));
            if (model->solution) {
                double net_obj;
                if (solve_as_network(&net_sig, model->solution, &net_obj) == 0) {
                    model->status = RALPH_STATUS_OPTIMAL;
                    model->obj_value = net_obj;
                    model->best_bound = net_obj;
                    model->node_count = 0;
                    model->iteration_count = 0;
                    if (lp_algorithm_report_ready) {
                        ralph_store_lp_algorithm_report(model, &lp_algorithm_report);
                    }

                    detect_network_free(&net_sig);
                    return 0;
                }
                free(model->solution);
                model->solution = NULL;
            }
            detect_network_free(&net_sig);
            /* Fall through to normal path if network solve failed */
        }
    }

    /* Apply presolve if enabled (special structure already handled above).
     * For MIP: auto-enable lightweight presolve (0x110F) unless user
     * explicitly disabled it. Avoids SINGLETON_COLS, PROBING, and
     * PROPORTIONAL_ROWS which interact badly in B&B. */
    PresolveResult *presolved = NULL;
    LPModel *solve_model = model->lp_model;
    unsigned int use_mask = model->presolve_mask;

    if (!use_presolve && solve_as_mip && model->presolve != -1) {
        use_presolve = 1;
        use_mask = 0x110F;  /* FIXED+EMPTY+SINGL_ROW+BOUND_TIGHT+SHIFT */
    }

    if (use_presolve > 0) {
        clock_t presolve_start = clock();
        presolved = presolve_with_mask(model->lp_model, use_mask);
        clock_t presolve_end = clock();
        model->last_presolve_report.used = 1;
        model->last_presolve_report.mask = use_mask;
        model->last_presolve_report.presolve_time_ms =
            1000.0 * (double)(presolve_end - presolve_start) / CLOCKS_PER_SEC;
        if (presolved) {
            model->last_presolve_report.rounds = presolved->rounds;
            model->last_presolve_report.vars_removed = presolved->vars_removed;
            model->last_presolve_report.cons_removed = presolved->cons_removed;
            model->last_presolve_report.bounds_tightened = presolved->bounds_tightened;
            model->last_presolve_report.matrix_rank = presolved->matrix_rank;
            model->last_presolve_report.redundant_rows_found = presolved->redundant_rows_found;
        }
        if (presolved && presolved->reduced_model) {
            solve_model = presolved->reduced_model;
            if (model->verbose) {
                printf("Presolve: %d vars removed, %d cons removed, %d bounds tightened",
                       presolved->vars_removed, presolved->cons_removed,
                       presolved->bounds_tightened);
                if (presolved->matrix_rank > 0) {
                    printf(", matrix rank=%d", presolved->matrix_rank);
                }
                printf("\n");
            }
        }
    }

    /* Handle trivially solved model: presolve eliminated all constraints.
     * With 0 constraints, the optimal is determined by bounds alone:
     * set each variable to its best bound for the objective. */
    if (solve_model->num_cons == 0 && solve_model->num_vars > 0) {
        double *trivial_sol = (double*)calloc(solve_model->num_vars, sizeof(double));
        if (trivial_sol) {
            double obj = solve_model->obj_offset;
            for (int j = 0; j < solve_model->num_vars; j++) {
                double c_eff = solve_model->c[j] * solve_model->obj_sense;
                if (c_eff > RALPH_ZERO_TOL) {
                    trivial_sol[j] = solve_model->lb[j];
                } else if (c_eff < -RALPH_ZERO_TOL) {
                    trivial_sol[j] = solve_model->ub[j];
                } else {
                    trivial_sol[j] = solve_model->lb[j] > -RALPH_INFINITY/2 ?
                                     solve_model->lb[j] : 0.0;
                }
                obj += solve_model->c[j] * trivial_sol[j];
            }

            model->status = RALPH_STATUS_OPTIMAL;
            model->obj_value = obj;
            model->iteration_count = 0;

            model->solution = (double*)calloc(n_orig, sizeof(double));
            if (model->solution) {
                if (presolved && presolved->reduced_model) {
                    postsolve(presolved, trivial_sol, model->solution);
                } else {
                    memcpy(model->solution, trivial_sol, solve_model->num_vars * sizeof(double));
                }
            }
            free(trivial_sol);
            if (presolved) presolve_free(presolved);
            if (lp_algorithm_report_ready) {
                ralph_store_lp_algorithm_report(model, &lp_algorithm_report);
            }
            return 0;
        }
    }

    /* Handle completely empty model: 0 vars, 0 constraints */
    if (solve_model->num_vars == 0) {
        model->status = RALPH_STATUS_OPTIMAL;
        model->obj_value = solve_model->obj_offset;
        model->iteration_count = 0;

        model->solution = (double*)calloc(n_orig, sizeof(double));
        if (model->solution && presolved && presolved->reduced_model) {
            double empty = 0.0;
            postsolve(presolved, &empty, model->solution);
        }
        if (presolved) presolve_free(presolved);
        if (lp_algorithm_report_ready) {
            ralph_store_lp_algorithm_report(model, &lp_algorithm_report);
        }
        return 0;
    }

    /* Try LAP detection for pure LP (not MIP) - post-presolve fallback.
     * This is a backup in case presolve reveals LAP structure that wasn't
     * detected in the original model (rare but possible). */
    if (!solve_as_mip && model->detect_special && ralph_get_detect_lap()) {
        LAPSignature lap_sig;
        if (detect_lap(solve_model, &lap_sig)) {
            if (model->verbose) {
                printf("Detected LAP structure: %dx%d assignment problem\n",
                       lap_sig.n, lap_sig.n);
            }

            /* Solve as LAP */
            model->solution = (double*)calloc(n_orig, sizeof(double));
            if (model->solution) {
                double lap_obj;
                if (solve_as_lap(&lap_sig, model->solution, &lap_obj) == 0) {
                    model->status = RALPH_STATUS_OPTIMAL;
                    model->obj_value = lap_obj;
                    model->iteration_count = 0;

                    /* Postsolve if presolve was applied */
                    if (presolved && presolved->reduced_model) {
                        double *presolved_sol = model->solution;
                        model->solution = (double*)calloc(n_orig, sizeof(double));
                        if (model->solution) {
                            postsolve(presolved, presolved_sol, model->solution);
                        }
                        free(presolved_sol);
                    }

                    detect_lap_free(&lap_sig);
                    if (presolved) presolve_free(presolved);
                    if (lp_algorithm_report_ready) {
                        ralph_store_lp_algorithm_report(model, &lp_algorithm_report);
                    }
                    return 0;
                }
                free(model->solution);
                model->solution = NULL;
            }
            detect_lap_free(&lap_sig);
        }
    }

    /* Try network detection for pure LP (not MIP) - post-presolve fallback.
     * This is a backup in case presolve reveals network structure that wasn't
     * detected in the original model (rare but possible). */
    if (!solve_as_mip && model->detect_special && ralph_get_detect_network()) {
        NetworkSignature net_sig;
        if (detect_network(solve_model, &net_sig)) {
            if (model->verbose) {
                RalphNetworkType type = detect_network_type(&net_sig);
                const char *type_str = "general";
                if (type == RALPH_NETWORK_ASSIGNMENT) type_str = "assignment";
                else if (type == RALPH_NETWORK_TRANSPORTATION) type_str = "transportation";
                else if (type == RALPH_NETWORK_SHORTEST_PATH) type_str = "shortest path";
                printf("Detected network structure: %d nodes, %d arcs (%s)\n",
                       net_sig.num_nodes, net_sig.num_arcs, type_str);
            }

            /* Solve as network flow */
            model->solution = (double*)calloc(n_orig, sizeof(double));
            if (model->solution) {
                double net_obj;
                if (solve_as_network(&net_sig, model->solution, &net_obj) == 0) {
                    model->status = RALPH_STATUS_OPTIMAL;
                    model->obj_value = net_obj;
                    model->iteration_count = 0;

                    /* Postsolve if presolve was applied */
                    if (presolved && presolved->reduced_model) {
                        double *presolved_sol = model->solution;
                        model->solution = (double*)calloc(n_orig, sizeof(double));
                        if (model->solution) {
                            postsolve(presolved, presolved_sol, model->solution);
                        }
                        free(presolved_sol);
                    }

                    detect_network_free(&net_sig);
                    if (presolved) presolve_free(presolved);
                    if (lp_algorithm_report_ready) {
                        ralph_store_lp_algorithm_report(model, &lp_algorithm_report);
                    }
                    return 0;
                }
                free(model->solution);
                model->solution = NULL;
            }
            detect_network_free(&net_sig);
        }
    }

    if (solve_as_mip) {
        /* MIP solve - LAP and network problems were already handled above before presolve */
        model->mip_solver = mip_create(solve_model, model->detect_special, model->node_pool_capacity);
        if (!model->mip_solver) {
            if (presolved) presolve_free(presolved);
            model->status = RALPH_STATUS_ERROR;
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_MEMORY,
                           RALPH_ERROR_CODE_ALLOCATION_FAILED,
                           model->status,
                           RALPH_ERROR_API_SOLVE,
                           0,
                           0,
                           "failed to allocate MIP solver");
        }

        /* Set parameters */
        model->mip_solver->max_nodes = model->max_nodes;
        model->mip_solver->time_limit = model->time_limit;
        model->mip_solver->mip_gap = model->mip_gap;
        model->mip_solver->verbose = model->verbose;
        model->mip_solver->telemetry = model->telemetry ? 1 : 0;
        model->mip_solver->max_cut_rounds = model->max_cut_rounds;
        model->mip_solver->dual_bound_flip = model->dual_bound_flip;
        model->mip_solver->dual_steepest_edge = model->dual_steepest_edge;
        model->mip_solver->lu_supernode = model->lu_supernode;
        model->mip_solver->mip_start_repair_mode = (int)model->mip_start_repair_mode;

        /* Set node selection strategy */
        model->mip_solver->node_select = (NodeSelectStrategy)model->node_select;
        if (model->mip_solver->node_queue) {
            model->mip_solver->node_queue->strategy = (NodeSelectStrategy)model->node_select;
        }

        /* Set variable selection strategy (overrides default if explicitly set) */
        if (model->var_select >= 0) {
            model->mip_solver->var_select = (VarSelectStrategy)model->var_select;
        }

        /* Pass branching control data to MIP solver.
         * When presolve is active, remap from original to presolved indices
         * using var_map[reduced_j] → original_j. */
        if (model->branch_priorities) {
            int n_solve = solve_model->num_vars;
            model->mip_solver->branch_priorities = (int*)malloc(n_solve * sizeof(int));
            if (model->mip_solver->branch_priorities) {
                if (presolved && presolved->var_map) {
                    for (int j = 0; j < n_solve; j++) {
                        int orig = presolved->var_map[j];
                        model->mip_solver->branch_priorities[j] =
                            (orig >= 0) ? model->branch_priorities[orig] : 0;
                    }
                } else {
                    memcpy(model->mip_solver->branch_priorities, model->branch_priorities,
                           n_solve * sizeof(int));
                }
            }
        }
        if (model->branch_directions) {
            int n_solve = solve_model->num_vars;
            model->mip_solver->branch_directions = (int*)malloc(n_solve * sizeof(int));
            if (model->mip_solver->branch_directions) {
                if (presolved && presolved->var_map) {
                    for (int j = 0; j < n_solve; j++) {
                        int orig = presolved->var_map[j];
                        model->mip_solver->branch_directions[j] =
                            (orig >= 0) ? model->branch_directions[orig] : 0;
                    }
                } else {
                    memcpy(model->mip_solver->branch_directions, model->branch_directions,
                           n_solve * sizeof(int));
                }
            }
        }

        /* Pass cut callback to MIP solver */
        if (model->has_cut_callback) {
            model->mip_solver->cut_callback = model->cut_callback;
            model->mip_solver->has_cut_callback = 1;
        }

        /* Pass branch callback to MIP solver */
        if (model->has_branch_callback) {
            model->mip_solver->branch_callback = model->branch_callback;
            model->mip_solver->has_branch_callback = 1;
        }

        /* Pass staged MIP start to the (possibly presolved) MIP model. */
        if (model->mip_start) {
            int n_solve = solve_model->num_vars;
            int mapped_ok = (model->mip_start_n == n_orig && n_solve > 0);
            double *solve_start = NULL;
            int *solve_mask = NULL;

            if (mapped_ok) {
                solve_start = (double*)malloc((size_t)n_solve * sizeof(double));
                solve_mask = (int*)calloc((size_t)n_solve, sizeof(int));
                if (!solve_start) {
                    if (presolved) presolve_free(presolved);
                    model->status = RALPH_STATUS_ERROR;
                    RALPH_FAIL_API(model,
                                   RALPH_ERROR_DOMAIN_MEMORY,
                                   RALPH_ERROR_CODE_ALLOCATION_FAILED,
                                   model->status,
                                   RALPH_ERROR_API_SOLVE,
                                   0,
                                   n_solve,
                                   "failed to allocate mapped MIP start values");
                }
                if (!solve_mask) {
                    free(solve_start);
                    if (presolved) presolve_free(presolved);
                    model->status = RALPH_STATUS_ERROR;
                    RALPH_FAIL_API(model,
                                   RALPH_ERROR_DOMAIN_MEMORY,
                                   RALPH_ERROR_CODE_ALLOCATION_FAILED,
                                   model->status,
                                   RALPH_ERROR_API_SOLVE,
                                   1,
                                   n_solve,
                                   "failed to allocate mapped MIP start mask");
                }

                if (presolved && presolved->var_map) {
                    for (int j = 0; j < n_solve; j++) {
                        int orig = presolved->var_map[j];
                        if (orig < 0 || orig >= model->mip_start_n) {
                            mapped_ok = 0;
                            break;
                        }
                        solve_start[j] = model->mip_start[orig];
                        solve_mask[j] = (model->mip_start_mask && model->mip_start_mask[orig]) ? 1 : 0;
                    }
                } else {
                    memcpy(solve_start, model->mip_start, (size_t)n_solve * sizeof(double));
                    if (model->mip_start_mask) {
                        memcpy(solve_mask, model->mip_start_mask, (size_t)n_solve * sizeof(int));
                    } else {
                        for (int j = 0; j < n_solve; j++) solve_mask[j] = 1;
                    }
                }
            }

            if (mapped_ok &&
                mip_set_start_ex(model->mip_solver, solve_start, solve_mask,
                                 n_solve, (int)model->mip_start_repair_mode) == 0) {
                model->mip_start_status = RALPH_MIP_START_PENDING;
            } else {
                model->mip_start_status = RALPH_MIP_START_REJECTED;
            }
            free(solve_start);
            free(solve_mask);
        }

        /* Solve */
        mip_solve(model->mip_solver);

        model->status = model->mip_solver->status;
        if (model->mip_start && model->mip_solver->mip_start_attempted > 0) {
            model->mip_start_status = (model->mip_solver->mip_start_accepted > 0) ?
                                      RALPH_MIP_START_ACCEPTED :
                                      RALPH_MIP_START_REJECTED;
        }

        if (model->mip_solver->has_incumbent) {
            model->obj_value = model->mip_solver->best_obj;
            model->best_bound = model->mip_solver->best_bound;
            model->node_count = model->mip_solver->nodes_explored;

            /* Copy solution - postsolve if presolve was applied */
            model->solution = (double*)calloc(n_orig, sizeof(double));
            if (model->solution) {
                if (presolved && presolved->reduced_model) {
                    /* Postsolve: recover original solution from presolved */
                    postsolve(presolved, model->mip_solver->best_solution, model->solution);
                } else {
                    memcpy(model->solution, model->mip_solver->best_solution, n_orig * sizeof(double));
                }
            }
        }

        /* Free presolve result */
        if (presolved) {
            presolve_free(presolved);
            presolved = NULL;
        }
    } else {
        /* LP solve */
        model->lp_solver = simplex_create(solve_model);
        if (!model->lp_solver) {
            if (presolved) presolve_free(presolved);
            model->status = RALPH_STATUS_ERROR;
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_MEMORY,
                           RALPH_ERROR_CODE_ALLOCATION_FAILED,
                           model->status,
                           RALPH_ERROR_API_SOLVE,
                           0,
                           0,
                           "failed to allocate LP solver");
        }

        /* Set parameters */
        model->lp_solver->max_iterations = model->max_iterations;
        model->lp_solver->time_limit = model->time_limit;
        model->lp_solver->verbose = model->verbose;
        model->lp_solver->telemetry_enabled = model->telemetry ? 1 : 0;
        model->lp_solver->presolve = 0;  /* Already done */
        model->lp_solver->pricing_strategy = lp_pricing_strategy;
        model->lp_solver->scaling = model->scaling;
        model->lp_solver->crash = model->crash;
        model->lp_solver->verify = model->verify;
        model->lp_solver->phase1_pricing = lp_phase1_pricing;
        /* Convert objective limit from user space to internal minimization space */
        if (model->objective_limit < RALPH_INFINITY) {
            model->lp_solver->objective_limit = model->objective_limit * solve_model->obj_sense;
        } else {
            model->lp_solver->objective_limit = RALPH_INFINITY;
        }
        model->lp_solver->force_two_phase = model->force_two_phase;
        model->lp_solver->trace_phase1 = model->trace_phase1;
        model->lp_solver->method = lp_simplex_method;
        model->lp_solver->lp_progress_callback = model->lp_progress_callback;
        model->lp_solver->has_lp_progress_callback = model->has_lp_progress_callback;
        model->lp_solver->lp_cancel_callback = model->lp_cancel_callback;
        model->lp_solver->has_lp_cancel_callback = model->has_lp_cancel_callback;
        if (model->dual_bound_flip >= 0)
            model->lp_solver->use_dual_bound_flip = model->dual_bound_flip;
        if (model->dual_steepest_edge >= 0)
            model->lp_solver->use_dual_steepest_edge = model->dual_steepest_edge;
        model->lp_solver->lu_supernode = model->lu_supernode;
        model->lp_solver->deterministic = model->deterministic ? 1 : 0;
        model->lp_solver->random_seed = (model->random_seed >= 0) ? (unsigned int)model->random_seed : 0U;
        model->lp_solver->lp_threads = (model->lp_threads >= 0) ? model->lp_threads : 0;
        model->lp_solver->policy.basis_governor_mode = model->lp_basis_governor_mode;
        lp_basis_governor_set_mode(&model->lp_solver->policy.basis_governor,
                                   model->lp_basis_governor_mode);

        if (model->staged_basis && model->staged_var_status) {
            if (simplex_set_warm_basis(model->lp_solver,
                                       model->staged_basis_m,
                                       model->staged_basis_n,
                                       model->staged_basis,
                                       model->staged_var_status) != 0) {
                if (presolved) presolve_free(presolved);
                model->status = RALPH_STATUS_ERROR;
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_STATE,
                               RALPH_ERROR_CODE_NOT_AVAILABLE,
                               model->status,
                               RALPH_ERROR_API_SOLVE,
                               model->staged_basis_m,
                               model->staged_basis_n,
                               "failed to apply staged basis");
            }
            ralph_clear_staged_basis(model);
        }

        /* Solve through backend runtime (simplex today, barrier/external later). */
        {
            int backend_rc = lp_backend_run(lp_effective_backend,
                                            lp_effective_provider,
                                            model->lp_solver);
            if (backend_rc != 0) {
                if (ralph_lp_dispatch_backend_is_external(lp_effective_backend)) {
                    RalphStatus mapped_status = RALPH_STATUS_ERROR;
                    ralph_record_external_execution_failure(model,
                                                           &lp_dispatch_plan,
                                                           backend_rc);
                    if (model->last_lp_external_failure_report_valid) {
                        mapped_status = model->last_lp_external_failure_report.mapped_status;
                    }
                    model->status = mapped_status;
                } else {
                    model->status = RALPH_STATUS_ERROR;
                }
                if (presolved) presolve_free(presolved);
                if (ralph_lp_dispatch_backend_is_external(lp_effective_backend)) {
                    RALPH_FAIL_API(model,
                                   RALPH_ERROR_DOMAIN_EXTERNAL,
                                   RALPH_ERROR_CODE_EXTERNAL_EXECUTION_FAILED,
                                   model->status,
                                   RALPH_ERROR_API_SOLVE,
                                   backend_rc,
                                   (int)lp_effective_backend,
                                   "external LP backend execution failed");
                }
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_SOLVER,
                               RALPH_ERROR_CODE_SOLVE_FAILED,
                               model->status,
                               RALPH_ERROR_API_SOLVE,
                               backend_rc,
                               (int)lp_effective_backend,
                               "internal LP backend execution failed");
            }
        }

        model->status = model->lp_solver->status;
        model->iteration_count = model->lp_solver->iterations;
        model->unbounded_ray_valid = 0;

        if (model->status == RALPH_STATUS_OPTIMAL ||
            model->status == RALPH_STATUS_IMPRECISE ||
            model->status == RALPH_STATUS_OBJ_LIMIT) {
            model->obj_value = model->lp_solver->obj_value;

            /* Allocate solution arrays for original problem size */
            model->solution = (double*)calloc(n_orig, sizeof(double));
            model->dual_solution = (double*)calloc(m_orig, sizeof(double));
            model->reduced_costs = (double*)calloc(n_orig, sizeof(double));

            if (!model->solution || !model->dual_solution || !model->reduced_costs) {
                free(model->solution); model->solution = NULL;
                free(model->dual_solution); model->dual_solution = NULL;
                free(model->reduced_costs); model->reduced_costs = NULL;
                if (presolved) presolve_free(presolved);
                model->status = RALPH_STATUS_ERROR;
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_MEMORY,
                               RALPH_ERROR_CODE_ALLOCATION_FAILED,
                               model->status,
                               RALPH_ERROR_API_SOLVE,
                               n_orig,
                               m_orig,
                               "failed to allocate solution vectors");
            }

            if (presolved && presolved->reduced_model) {
                /* Postsolve: recover original solution */
                if (model->solution && model->lp_solver->solution) {
                    postsolve(presolved, model->lp_solver->solution, model->solution);
                }
                /* Dual values need postsolve too - for now copy what we have */
                int m_reduced = solve_model->num_cons;
                if (model->dual_solution && model->lp_solver->dual_solution) {
                    for (int i = 0; i < m_reduced && i < m_orig; i++) {
                        int orig_con = presolved->con_map ? presolved->con_map[i] : i;
                        if (orig_con >= 0 && orig_con < m_orig) {
                            model->dual_solution[orig_con] = model->lp_solver->dual_solution[i];
                        }
                    }
                }
            } else {
                /* No presolve - direct copy */
                if (model->solution && model->lp_solver->solution) {
                    memcpy(model->solution, model->lp_solver->solution, n_orig * sizeof(double));
                }
                if (model->dual_solution && model->lp_solver->dual_solution) {
                    memcpy(model->dual_solution, model->lp_solver->dual_solution, m_orig * sizeof(double));
                }
                if (model->reduced_costs && model->lp_solver->reduced_costs) {
                    memcpy(model->reduced_costs, model->lp_solver->reduced_costs, n_orig * sizeof(double));
                }
            }
        } else if (model->status == RALPH_STATUS_UNBOUNDED &&
                   model->lp_solver->unbounded_valid &&
                   model->lp_solver->unbounded_ray) {
            model->unbounded_ray = (double*)calloc((size_t)n_orig, sizeof(double));
            if (model->unbounded_ray) {
                int mapped = 0;
                if (presolved && presolved->reduced_model && presolved->var_map) {
                    int n_reduced = solve_model->num_vars;
                    for (int j = 0; j < n_reduced; j++) {
                        int orig = presolved->var_map[j];
                        if (orig >= 0 && orig < n_orig) {
                            model->unbounded_ray[orig] = model->lp_solver->unbounded_ray[j];
                        }
                    }
                    mapped = 1;
                } else if (solve_model->num_vars == n_orig) {
                    memcpy(model->unbounded_ray, model->lp_solver->unbounded_ray,
                           (size_t)n_orig * sizeof(double));
                    mapped = 1;
                }
                if (mapped) {
                    model->unbounded_ray_valid = 1;
                } else {
                    free(model->unbounded_ray);
                    model->unbounded_ray = NULL;
                }
            }
        }
    }

    /* Free presolve result */
    if (presolved) {
        presolve_free(presolved);
    }

    if (lp_algorithm_report_ready) {
        ralph_store_lp_algorithm_report(model, &lp_algorithm_report);
    }

    return 0;
}

int ralph_core_optimize(RalphModel *model) {
    return ralph_optimize_with_mode(model, RALPH_SOLVE_AUTO);
}

int ralph_core_optimize_lp(RalphModel *model) {
    return ralph_optimize_with_mode(model, RALPH_SOLVE_LP_ONLY);
}

int ralph_core_optimize_mip(RalphModel *model) {
    return ralph_optimize_with_mode(model, RALPH_SOLVE_MIP_ONLY);
}

int ralph_core_get_last_error(const RalphModel *model, RalphAPIError *out) {
    if (!out) return -1;
    if (model) return lp_error_state_get(&model->error_state, out);
    return lp_error_tls_get(out);
}

int ralph_core_clear_error(RalphModel *model) {
    if (model) {
        lp_error_state_clear(&model->error_state);
    } else {
        lp_error_tls_clear();
    }
    return 0;
}

const char* ralph_core_error_domain_string(RalphErrorDomain domain) {
    return lp_error_domain_string(domain);
}

const char* ralph_core_error_code_string(RalphErrorCode code) {
    return lp_error_code_string(code);
}

const char* ralph_core_error_api_string(RalphErrorAPIId api_id) {
    return lp_error_api_string(api_id);
}

const char* ralph_core_error_message(const RalphAPIError *error) {
    return lp_error_message(error);
}

/* ============================================================================
 * Solution Retrieval
 * ============================================================================ */

RalphStatus ralph_core_get_status(const RalphModel *model) {
    return model ? model->status : RALPH_STATUS_UNKNOWN;
}

double ralph_core_get_objval(const RalphModel *model) {
    return model ? model->obj_value : 0.0;
}

int ralph_core_get_solution(const RalphModel *model, double *x) {
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!model || !x) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_SOLUTION_QUERY,
                       0,
                       0,
                       "model or output buffer is null");
    }
    if (!model->solution) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_STATE,
                       RALPH_ERROR_CODE_NOT_AVAILABLE,
                       model->status,
                       RALPH_ERROR_API_SOLUTION_QUERY,
                       0,
                       0,
                       "primal solution is unavailable");
    }

    int n = ralph_core_get_num_vars(model);
    memcpy(x, model->solution, n * sizeof(double));
    return 0;
}

int ralph_core_get_dual_solution(const RalphModel *model, double *y) {
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!model || !y) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_SOLUTION_QUERY,
                       0,
                       0,
                       "model or output buffer is null");
    }
    if (!model->dual_solution) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_STATE,
                       RALPH_ERROR_CODE_NOT_AVAILABLE,
                       model->status,
                       RALPH_ERROR_API_SOLUTION_QUERY,
                       1,
                       0,
                       "dual solution is unavailable");
    }

    int m = ralph_core_get_num_cons(model);
    memcpy(y, model->dual_solution, m * sizeof(double));
    return 0;
}

int ralph_core_get_reduced_costs(const RalphModel *model, double *rc) {
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!model || !rc) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_SOLUTION_QUERY,
                       0,
                       0,
                       "model or output buffer is null");
    }
    if (!model->reduced_costs) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_STATE,
                       RALPH_ERROR_CODE_NOT_AVAILABLE,
                       model->status,
                       RALPH_ERROR_API_SOLUTION_QUERY,
                       2,
                       0,
                       "reduced costs are unavailable");
    }

    int n = ralph_core_get_num_vars(model);
    memcpy(rc, model->reduced_costs, n * sizeof(double));
    return 0;
}

int ralph_core_get_lp_capabilities(RalphLPCapabilities *caps) {
    if (!caps) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_EXTERNAL,
                       0,
                       0,
                       "output pointer is null");
    }
    RALPH_CLEAR_API_ERROR(NULL);
    lp_dispatch_get_capabilities(caps);
    return 0;
}

int ralph_core_get_last_lp_algorithm_report(const RalphModel *model,
                                       RalphLPSolveAlgorithmReport *report) {
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!model || !report) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_SOLVE,
                       0,
                       0,
                       "model or report output is null");
    }
    if (ralph_core_is_mip(model)) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_STATE,
                       RALPH_ERROR_CODE_NOT_AVAILABLE,
                       model->status,
                       RALPH_ERROR_API_SOLVE,
                       0,
                       0,
                       "LP algorithm report is unavailable for MIP models");
    }
    if (!model->last_lp_algorithm_report_valid) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_STATE,
                       RALPH_ERROR_CODE_NOT_AVAILABLE,
                       model->status,
                       RALPH_ERROR_API_SOLVE,
                       0,
                       0,
                       "LP algorithm report is unavailable");
    }
    *report = model->last_lp_algorithm_report;
    return 0;
}

int ralph_core_get_last_lp_external_failure_report(const RalphModel *model,
                                              RalphLPExternalFailureReport *report) {
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!model || !report) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_EXTERNAL,
                       0,
                       0,
                       "model or report output is null");
    }
    if (ralph_core_is_mip(model)) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_STATE,
                       RALPH_ERROR_CODE_NOT_AVAILABLE,
                       model->status,
                       RALPH_ERROR_API_EXTERNAL,
                       0,
                       0,
                       "external failure report is unavailable for MIP models");
    }
    if (!model->last_lp_external_failure_report_valid) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_STATE,
                       RALPH_ERROR_CODE_NOT_AVAILABLE,
                       model->status,
                       RALPH_ERROR_API_EXTERNAL,
                       0,
                       0,
                       "external failure report is unavailable");
    }
    *report = model->last_lp_external_failure_report;
    return 0;
}

const char* ralph_core_get_lp_external_provider_name(RalphLPExternalProvider provider) {
    RALPH_CLEAR_API_ERROR(NULL);
    if (provider == RALPH_LP_EXTERNAL_PROVIDER_NONE) {
        return lp_external_provider_name(LP_EXTERNAL_PROVIDER_NONE);
    }
    if (!ralph_lp_external_provider_valid_public(provider)) {
        RALPH_FAIL_API_PTR(NULL,
                           RALPH_ERROR_DOMAIN_RANGE,
                           RALPH_ERROR_CODE_OUT_OF_RANGE,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_EXTERNAL,
                           provider,
                           0,
                           "external provider id is out of range");
    }
    return lp_external_provider_name(ralph_lp_external_provider_to_internal(provider));
}

int ralph_core_get_lp_external_provider_capabilities(RalphLPExternalProvider provider,
                                                RalphLPExternalCapabilities *caps) {
    LPExternalCapabilities internal_caps;
    LPExternalProvider internal_provider;

    if (!caps) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_EXTERNAL,
                       0,
                       0,
                       "output pointer is null");
    }
    RALPH_CLEAR_API_ERROR(NULL);
    memset(caps, 0, sizeof(*caps));
    if (!ralph_lp_external_provider_valid_public(provider)) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_EXTERNAL,
                       (int)provider,
                       0,
                       "external provider id is out of range");
    }

    internal_provider = ralph_lp_external_provider_to_internal(provider);
    if (internal_provider == LP_EXTERNAL_PROVIDER_NONE) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_EXTERNAL,
                       RALPH_ERROR_CODE_EXTERNAL_DISPATCH_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_EXTERNAL,
                       (int)provider,
                       0,
                       "provider NONE has no capabilities");
    }
    if (lp_external_adapter_get_capabilities(internal_provider, &internal_caps) != 0) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_EXTERNAL,
                       RALPH_ERROR_CODE_EXTERNAL_EXECUTION_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_EXTERNAL,
                       (int)provider,
                       0,
                       "failed to query external provider capabilities");
    }

    caps->supports_simplex = internal_caps.supports_simplex ? 1 : 0;
    caps->supports_dual_simplex = internal_caps.supports_dual_simplex ? 1 : 0;
    caps->supports_barrier = internal_caps.supports_barrier ? 1 : 0;
    caps->supports_crossover = internal_caps.supports_crossover ? 1 : 0;
    return 0;
}

int ralph_core_get_lp_external_registered_providers(RalphLPExternalProvider *providers,
                                               int capacity,
                                               int *count) {
    LPExternalProvider internal_list[(int)LP_EXTERNAL_PROVIDER_GLOP + 1];
    int internal_capacity = capacity;
    int needed = 0;
    int rc;

    if (!count) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_EXTERNAL,
                       0,
                       0,
                       "count output is null");
    }
    RALPH_CLEAR_API_ERROR(NULL);
    if (capacity < 0) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_EXTERNAL,
                       capacity,
                       0,
                       "capacity must be non-negative");
    }
    if (!providers && capacity > 0) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_EXTERNAL,
                       capacity,
                       0,
                       "providers buffer is null");
    }
    if (internal_capacity > (int)LP_EXTERNAL_PROVIDER_GLOP) {
        internal_capacity = (int)LP_EXTERNAL_PROVIDER_GLOP;
    }
    if (internal_capacity < 0) internal_capacity = 0;

    rc = lp_external_adapter_list_registered(
        (internal_capacity > 0) ? internal_list : NULL,
        internal_capacity,
        &needed);
    if (count) *count = needed;
    if (rc != 0) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_EXTERNAL,
                       RALPH_ERROR_CODE_EXTERNAL_EXECUTION_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_EXTERNAL,
                       rc,
                       0,
                       "failed to list registered external providers");
    }

    if (providers && capacity > 0) {
        int to_copy = needed;
        if (to_copy > capacity) to_copy = capacity;
        for (int i = 0; i < to_copy; i++) {
            providers[i] = ralph_lp_external_provider_from_internal(internal_list[i]);
        }
    }
    return 0;
}

int ralph_core_register_lp_external_adapter(const RalphLPExternalAdapter *adapter) {
    RalphLPExternalAdapterBridgeEntry *entry = NULL;
    LPExternalAdapter internal_adapter;
    LPExternalProvider provider;

    if (!adapter) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_EXTERNAL,
                       0,
                       0,
                       "adapter pointer is null");
    }
    RALPH_CLEAR_API_ERROR(NULL);
    if (adapter->abi_version != RALPH_LP_EXTERNAL_ADAPTER_ABI_VERSION) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_EXTERNAL,
                       RALPH_ERROR_CODE_INVALID_ARGUMENT,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_EXTERNAL,
                       adapter->abi_version,
                       RALPH_LP_EXTERNAL_ADAPTER_ABI_VERSION,
                       "external adapter ABI version mismatch");
    }
    if (!ralph_lp_external_provider_valid_public(adapter->provider)) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_EXTERNAL,
                       adapter->provider,
                       0,
                       "external provider id is out of range");
    }
    if (!adapter->get_capabilities || !adapter->solve) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_EXTERNAL,
                       0,
                       0,
                       "adapter callbacks are required");
    }

    provider = ralph_lp_external_provider_to_internal(adapter->provider);
    if (provider == LP_EXTERNAL_PROVIDER_NONE) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_EXTERNAL,
                       RALPH_ERROR_CODE_EXTERNAL_DISPATCH_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_EXTERNAL,
                       adapter->provider,
                       0,
                       "external provider NONE cannot be registered");
    }

    entry = (RalphLPExternalAdapterBridgeEntry*)calloc(1, sizeof(*entry));
    if (!entry) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_MEMORY,
                       RALPH_ERROR_CODE_ALLOCATION_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_EXTERNAL,
                       0,
                       0,
                       "failed to allocate adapter bridge entry");
    }
    entry->adapter = *adapter;

    memset(&internal_adapter, 0, sizeof(internal_adapter));
    internal_adapter.abi_version = LP_EXTERNAL_ADAPTER_ABI_VERSION;
    internal_adapter.provider = provider;
    internal_adapter.provider_name = adapter->provider_name;
    internal_adapter.get_capabilities = ralph_lp_external_bridge_get_capabilities;
    internal_adapter.solve = ralph_lp_external_bridge_solve;
    internal_adapter.user_data = entry;
    internal_adapter.destroy_user_data = ralph_lp_external_bridge_entry_destroy;

    if (lp_external_adapter_register(&internal_adapter) != 0) {
        free(entry);
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_EXTERNAL,
                       RALPH_ERROR_CODE_EXTERNAL_EXECUTION_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_EXTERNAL,
                       adapter->provider,
                       0,
                       "failed to register external adapter");
    }
    return 0;
}

int ralph_core_register_lp_external_glpk_oop(const char *glpsol_path) {
    RALPH_CLEAR_API_ERROR(NULL);
    if (lp_external_glpk_oop_register(glpsol_path) != 0) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_EXTERNAL,
                       RALPH_ERROR_CODE_EXTERNAL_EXECUTION_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_EXTERNAL,
                       0,
                       0,
                       "failed to register GLPK OOP adapter");
    }
    return 0;
}

int ralph_core_unregister_lp_external_adapter(RalphLPExternalProvider provider) {
    RALPH_CLEAR_API_ERROR(NULL);
    if (!ralph_lp_external_provider_valid_public(provider)) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_EXTERNAL,
                       provider,
                       0,
                       "external provider id is out of range");
    }
    if (lp_external_adapter_unregister(ralph_lp_external_provider_to_internal(provider)) != 0) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_EXTERNAL,
                       RALPH_ERROR_CODE_EXTERNAL_EXECUTION_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_EXTERNAL,
                       provider,
                       0,
                       "failed to unregister external adapter");
    }
    return 0;
}

int ralph_core_unregister_lp_external_glpk_oop(void) {
    RALPH_CLEAR_API_ERROR(NULL);
    if (lp_external_glpk_oop_unregister() != 0) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_EXTERNAL,
                       RALPH_ERROR_CODE_EXTERNAL_EXECUTION_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_EXTERNAL,
                       0,
                       0,
                       "failed to unregister GLPK OOP adapter");
    }
    return 0;
}

void ralph_core_unregister_all_lp_external_adapters(void) {
    lp_external_adapter_unregister_all();
}

int ralph_core_is_lp_external_adapter_registered(RalphLPExternalProvider provider) {
    RALPH_CLEAR_API_ERROR(NULL);
    if (provider == RALPH_LP_EXTERNAL_PROVIDER_NONE) {
        return lp_external_adapter_is_registered(LP_EXTERNAL_PROVIDER_NONE);
    }
    if (!ralph_lp_external_provider_valid_public(provider)) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_EXTERNAL,
                       provider,
                       0,
                       "external provider id is out of range");
    }
    return lp_external_adapter_is_registered(ralph_lp_external_provider_to_internal(provider));
}

int ralph_core_get_farkas_ray(const RalphModel *model, double *ray) {
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!model || !ray) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_SOLUTION_QUERY,
                       0,
                       0,
                       "model or output buffer is null");
    }

    /* Check if status is infeasible and we have a valid Farkas ray */
    if (model->status != RALPH_STATUS_INFEASIBLE) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_STATE,
                       RALPH_ERROR_CODE_NOT_AVAILABLE,
                       model->status,
                       RALPH_ERROR_API_SOLUTION_QUERY,
                       model->status,
                       RALPH_STATUS_INFEASIBLE,
                       "Farkas ray only available for infeasible status");
    }

    /* For LP problems, get the ray from the simplex solver */
    if (model->lp_solver && model->lp_solver->farkas_valid && model->lp_solver->farkas_ray) {
        int m = ralph_core_get_num_cons(model);
        memcpy(ray, model->lp_solver->farkas_ray, m * sizeof(double));
        return 0;
    }

    /* No valid Farkas ray available */
    RALPH_FAIL_API(model,
                   RALPH_ERROR_DOMAIN_STATE,
                   RALPH_ERROR_CODE_NOT_AVAILABLE,
                   model->status,
                   RALPH_ERROR_API_SOLUTION_QUERY,
                   0,
                   0,
                   "Farkas ray is unavailable");
}

int ralph_core_get_unbounded_ray(const RalphModel *model, double *ray) {
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!model || !ray) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_SOLUTION_QUERY,
                       0,
                       0,
                       "model or output buffer is null");
    }
    if (model->status != RALPH_STATUS_UNBOUNDED) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_STATE,
                       RALPH_ERROR_CODE_NOT_AVAILABLE,
                       model->status,
                       RALPH_ERROR_API_SOLUTION_QUERY,
                       model->status,
                       RALPH_STATUS_UNBOUNDED,
                       "unbounded ray only available for unbounded status");
    }

    int n = ralph_core_get_num_vars(model);
    if (n <= 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_STATE,
                       RALPH_ERROR_CODE_NOT_AVAILABLE,
                       model->status,
                       RALPH_ERROR_API_SOLUTION_QUERY,
                       n,
                       0,
                       "unbounded ray unavailable for empty model");
    }

    if (model->unbounded_ray_valid && model->unbounded_ray) {
        memcpy(ray, model->unbounded_ray, (size_t)n * sizeof(double));
        return 0;
    }

    if (model->lp_solver &&
        model->lp_solver->unbounded_valid &&
        model->lp_solver->unbounded_ray &&
        model->lp_solver->model &&
        model->lp_solver->model->num_vars == n) {
        memcpy(ray, model->lp_solver->unbounded_ray, (size_t)n * sizeof(double));
        return 0;
    }

    RALPH_FAIL_API(model,
                   RALPH_ERROR_DOMAIN_STATE,
                   RALPH_ERROR_CODE_NOT_AVAILABLE,
                   model->status,
                   RALPH_ERROR_API_SOLUTION_QUERY,
                   0,
                   0,
                   "unbounded ray is unavailable");
}

int ralph_core_compute_lp_conflict(const RalphModel *model,
                              const RalphConflictOptions *options,
                              RalphConflictMember *members,
                              int capacity,
                              int *count,
                              RalphConflictReport *report) {
    LPConflictOptionsInternal internal_opts;
    LPConflictMemberInternal *internal_members = NULL;
    LPConflictReportInternal internal_report;
    int internal_count = 0;

    if (!model || !model->lp_model || !count) return -1;
    if (capacity < 0) return -1;
    if (!members && capacity > 0) return -1;
    *count = 0;
    if (report) memset(report, 0, sizeof(*report));

    if (ralph_core_is_mip(model)) return -1;
    if (model->status != RALPH_STATUS_INFEASIBLE) return -1;

    internal_opts.include_bounds = 1;
    internal_opts.use_farkas_seed = 0;
    internal_opts.farkas_ray = NULL;
    internal_opts.farkas_valid = 0;
    if (options) {
        internal_opts.include_bounds = options->include_bounds ? 1 : 0;
        internal_opts.use_farkas_seed = options->use_farkas_seed ? 1 : 0;
    }
    if (model->lp_solver && model->lp_solver->farkas_valid && model->lp_solver->farkas_ray) {
        internal_opts.farkas_valid = 1;
        internal_opts.farkas_ray = model->lp_solver->farkas_ray;
    }

    memset(&internal_report, 0, sizeof(internal_report));
    if (lp_conflict_compute(model->lp_model, &internal_opts,
                            ralph_probe_lp_status_cb, (void*)model,
                            &internal_members, &internal_count,
                            &internal_report) != 0) {
        return -1;
    }

    *count = internal_count;
    if (capacity < internal_count) {
        lp_conflict_free_members(internal_members);
        return -1;
    }

    for (int i = 0; i < internal_count; i++) {
        if (internal_members[i].type == LP_CONFLICT_MEMBER_ROW) {
            members[i].type = RALPH_CONFLICT_MEMBER_ROW;
        } else if (internal_members[i].type == LP_CONFLICT_MEMBER_VAR_LB) {
            members[i].type = RALPH_CONFLICT_MEMBER_VAR_LB;
        } else if (internal_members[i].type == LP_CONFLICT_MEMBER_VAR_UB) {
            members[i].type = RALPH_CONFLICT_MEMBER_VAR_UB;
        } else {
            lp_conflict_free_members(internal_members);
            return -1;
        }
        members[i].index = internal_members[i].index;
    }

    if (report) {
        report->probes = internal_report.probes;
        report->dropped = internal_report.dropped;
        report->initial_size = internal_report.initial_size;
        report->final_size = internal_report.final_size;
        report->used_farkas_seed = internal_report.used_farkas_seed;
        report->seeded_rows = internal_report.seeded_rows;
    }

    lp_conflict_free_members(internal_members);
    return 0;
}

int ralph_core_compute_lp_iis(const RalphModel *model, int *row_flags, int *iis_size) {
    LPConflictOptionsInternal opts;
    LPConflictMemberInternal *members = NULL;
    LPConflictReportInternal report;
    int count = 0;
    int rows = 0;

    if (!model || !model->lp_model || !row_flags) return -1;
    if (iis_size) *iis_size = 0;
    if (ralph_core_is_mip(model)) return -1;
    if (model->status != RALPH_STATUS_INFEASIBLE) return -1;

    int m = model->lp_model->num_cons;
    if (m <= 0) return -1;
    memset(row_flags, 0, (size_t)m * sizeof(int));

    opts.include_bounds = 0;
    opts.use_farkas_seed = 0;
    opts.farkas_ray = NULL;
    opts.farkas_valid = 0;

    memset(&report, 0, sizeof(report));
    if (lp_conflict_compute(model->lp_model, &opts,
                            ralph_probe_lp_status_cb, (void*)model,
                            &members, &count, &report) != 0) {
        return -1;
    }

    for (int i = 0; i < count; i++) {
        if (members[i].type != LP_CONFLICT_MEMBER_ROW) {
            lp_conflict_free_members(members);
            return -1;
        }
        if (members[i].index < 0 || members[i].index >= m) {
            lp_conflict_free_members(members);
            return -1;
        }
        row_flags[members[i].index] = 1;
        rows++;
    }
    if (iis_size) *iis_size = rows;

    lp_conflict_free_members(members);
    return 0;
}

/* ============================================================================
 * MIP-Specific
 * ============================================================================ */

double ralph_core_get_best_bound(const RalphModel *model) {
    return model ? model->best_bound : 0.0;
}

double ralph_core_get_mip_gap(const RalphModel *model) {
    if (!model || !ralph_core_is_mip(model)) return 0.0;

    double gap = fabs(model->obj_value - model->best_bound);
    return gap / (fabs(model->obj_value) + 1e-10);
}

int ralph_core_get_node_count(const RalphModel *model) {
    return model ? model->node_count : 0;
}

int ralph_core_get_iterations(const RalphModel *model) {
    return model ? model->iteration_count : 0;
}

static const SimplexSolver* ralph_get_last_lp_solver_for_reports(const RalphModel *model) {
    if (!model) return NULL;
    if (model->lp_solver) return model->lp_solver;
    if (model->mip_solver && model->mip_solver->lp_solver) {
        return model->mip_solver->lp_solver;
    }
    return NULL;
}

int ralph_core_get_last_presolve_report(const RalphModel *model, RalphPresolveReport *report) {
    if (!model || !report) return -1;
    *report = model->last_presolve_report;
    return 0;
}

int ralph_core_get_last_lp_telemetry(const RalphModel *model, RalphLPSolverTelemetry *telemetry) {
    if (!model || !telemetry) return -1;

    memset(telemetry, 0, sizeof(*telemetry));
    const SimplexSolver *lp = ralph_get_last_lp_solver_for_reports(model);
    if (!lp) return 0;

    LPSolverTelemetrySnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    lp_telemetry_snapshot_solver(lp, &snapshot);

    if (sizeof(*telemetry) != sizeof(snapshot)) return -1;
    memcpy(telemetry, &snapshot, sizeof(*telemetry));
    return 0;
}

int ralph_core_get_last_lu_telemetry(const RalphModel *model, RalphLUTelemetry *telemetry) {
    if (!model || !telemetry) return -1;

    memset(telemetry, 0, sizeof(*telemetry));
    const SimplexSolver *lp = ralph_get_last_lp_solver_for_reports(model);
    if (!lp || !lp->tableau || !lp->tableau->lu) return 0;

    LUTelemetrySnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    lp_telemetry_snapshot_lu(lp->tableau->lu, &snapshot);

    if (sizeof(*telemetry) != sizeof(snapshot)) return -1;
    memcpy(telemetry, &snapshot, sizeof(*telemetry));
    return 0;
}

int ralph_core_get_solution_quality(const RalphModel *model, RalphSolutionQuality *quality) {
    if (!model || !quality) return -1;

    memset(quality, 0, sizeof(*quality));
    quality->status = model->status;

    const SimplexSolver *lp = ralph_get_last_lp_solver_for_reports(model);
    if (!lp) {
        quality->verify_enabled = model->verify ? 1 : 0;
        return 0;
    }

    quality->verify_enabled = lp->verify ? 1 : 0;
    if (!quality->verify_enabled) return 0;

    if (model->status != RALPH_STATUS_OPTIMAL &&
        model->status != RALPH_STATUS_IMPRECISE &&
        model->status != RALPH_STATUS_OBJ_LIMIT) {
        return 0;
    }

    quality->available = 1;
    quality->primal_infeas = lp->verify_primal_infeas;
    quality->bound_infeas = lp->verify_bound_infeas;
    quality->dual_infeas = lp->verify_dual_infeas;
    quality->comp_slack = lp->verify_comp_slack;
    quality->obj_error = lp->verify_obj_error;
    quality->cond_estimate = lp->verify_cond_estimate;
    return 0;
}

static int ralph_get_lp_tableau_for_sensitivity(const RalphModel *model,
                                                const SimplexTableau **tab_out) {
    const SimplexTableau *tab = NULL;
    if (!model || !tab_out || !model->lp_model) return -1;
    if (ralph_core_is_mip(model)) return -1;
    if (model->status != RALPH_STATUS_OPTIMAL) return -1;
    if (!model->lp_solver || !model->lp_solver->tableau) return -1;

    tab = model->lp_solver->tableau;
    if (!tab || tab->phase != 2) return -1;
    if (!tab->lu || !tab->basis || !tab->basis_pos || !tab->var_status) return -1;

    /* v1 contract: unavailable when presolve solved a reduced-dimension model. */
    if (tab->model != model->lp_model) return -1;
    if (tab->m != model->lp_model->num_cons) return -1;
    if (model->lp_model->num_vars > tab->n) return -1;

    *tab_out = tab;
    return 0;
}

int ralph_core_get_constraint_rhs_range(const RalphModel *model,
                                   int constraint,
                                   RalphSensitivityRange *range) {
    const SimplexTableau *tab = NULL;
    double rhs_min_internal = 0.0;
    double rhs_max_internal = 0.0;
    double sign = 1.0;
    double a = 0.0;
    double b = 0.0;
    double lo = 0.0;
    double hi = 0.0;

    if (!model || !range || !model->lp_model) return -1;
    if (ralph_get_lp_tableau_for_sensitivity(model, &tab) != 0) return -1;
    if (constraint < 0 || constraint >= model->lp_model->num_cons) return -1;

    if (lp_sensitivity_rhs_range_internal(tab, constraint,
                                          &rhs_min_internal,
                                          &rhs_max_internal) != 0) {
        return -1;
    }

    if (tab->row_sign && tab->m > constraint) {
        sign = (tab->row_sign[constraint] < 0.0) ? -1.0 : 1.0;
    }

    a = rhs_min_internal * sign;
    b = rhs_max_internal * sign;
    lo = fmin(a, b);
    hi = fmax(a, b);

    /* Keep row-sign normalization unchanged in v1 fixed-basis contract. */
    if (sign > 0.0 && lo < 0.0) lo = 0.0;
    if (sign < 0.0 && hi > 0.0) hi = 0.0;
    if (lo > hi + 10.0 * RALPH_FEAS_TOL) return -1;

    range->current = model->lp_model->b[constraint];
    range->lower = lo;
    range->upper = hi;
    return 0;
}

int ralph_core_get_obj_coef_range(const RalphModel *model,
                             int var,
                             RalphSensitivityRange *range) {
    const SimplexTableau *tab = NULL;
    double min_internal = 0.0;
    double max_internal = 0.0;
    double sense = 1.0;
    double a = 0.0;
    double b = 0.0;

    if (!model || !range || !model->lp_model) return -1;
    if (ralph_get_lp_tableau_for_sensitivity(model, &tab) != 0) return -1;
    if (var < 0 || var >= model->lp_model->num_vars) return -1;

    if (lp_sensitivity_obj_coef_range_internal(tab, var,
                                               &min_internal,
                                               &max_internal) != 0) {
        return -1;
    }

    sense = (double)model->lp_model->obj_sense;  /* user_coef = internal_coef * sense */
    a = min_internal * sense;
    b = max_internal * sense;

    range->current = model->lp_model->c[var];
    range->lower = fmin(a, b);
    range->upper = fmax(a, b);
    return 0;
}

int ralph_core_get_var_bound_range(const RalphModel *model,
                              int var,
                              RalphBoundSensitivityRange *range) {
    const SimplexTableau *tab = NULL;
    LPBoundRangeInternal internal;
    if (!model || !range || !model->lp_model) return -1;
    if (ralph_get_lp_tableau_for_sensitivity(model, &tab) != 0) return -1;
    if (var < 0 || var >= model->lp_model->num_vars) return -1;

    if (lp_sensitivity_var_bound_range_internal(tab, var, &internal) != 0) {
        return -1;
    }

    range->lower_current = model->lp_model->lb[var];
    range->lower_min = internal.lower_min;
    range->lower_max = internal.lower_max;
    range->upper_current = model->lp_model->ub[var];
    range->upper_min = internal.upper_min;
    range->upper_max = internal.upper_max;
    return 0;
}

/* ============================================================================
 * Branching Control
 * ============================================================================ */

int ralph_core_set_branch_priorities(RalphModel *model, const int *priorities) {
    if (!model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);

    /* Free existing priorities */
    free(model->branch_priorities);
    model->branch_priorities = NULL;

    if (!priorities) return 0;  /* Clear priorities */

    int n = ralph_core_get_num_vars(model);
    if (n <= 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_STATE,
                       RALPH_ERROR_CODE_NOT_AVAILABLE,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       n,
                       0,
                       "no variables available for branch priorities");
    }

    model->branch_priorities = (int*)malloc(n * sizeof(int));
    if (!model->branch_priorities) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_MEMORY,
                       RALPH_ERROR_CODE_ALLOCATION_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       n,
                       0,
                       "failed to allocate branch priorities");
    }

    memcpy(model->branch_priorities, priorities, n * sizeof(int));
    return 0;
}

int ralph_core_set_branch_directions(RalphModel *model, const int *directions) {
    if (!model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);

    /* Free existing directions */
    free(model->branch_directions);
    model->branch_directions = NULL;

    if (!directions) return 0;  /* Clear directions */

    int n = ralph_core_get_num_vars(model);
    if (n <= 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_STATE,
                       RALPH_ERROR_CODE_NOT_AVAILABLE,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       n,
                       0,
                       "no variables available for branch directions");
    }

    model->branch_directions = (int*)malloc(n * sizeof(int));
    if (!model->branch_directions) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_MEMORY,
                       RALPH_ERROR_CODE_ALLOCATION_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       n,
                       0,
                       "failed to allocate branch directions");
    }

    memcpy(model->branch_directions, directions, n * sizeof(int));
    return 0;
}

int ralph_core_set_mip_start(RalphModel *model, const double *x) {
    if (!model || !model->lp_model || !x) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model or start vector is null");
    }
    RALPH_CLEAR_API_ERROR(model);

    int n = model->lp_model->num_vars;
    if (n <= 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_STATE,
                       RALPH_ERROR_CODE_NOT_AVAILABLE,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       n,
                       0,
                       "no variables available for MIP start");
    }
    if (ralph_set_mip_start_copy(model, x, NULL, n) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_MEMORY,
                       RALPH_ERROR_CODE_ALLOCATION_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       n,
                       0,
                       "failed to copy MIP start");
    }
    return 0;
}

int ralph_core_set_mip_start_sparse(RalphModel *model, int count,
                               const int *indices, const double *values) {
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    if (count < 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       count,
                       0,
                       "count must be non-negative");
    }
    if (count > 0 && (!indices || !values)) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       count,
                       0,
                       "sparse start arrays are null");
    }

    int n = model->lp_model->num_vars;
    if (n <= 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_STATE,
                       RALPH_ERROR_CODE_NOT_AVAILABLE,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       n,
                       0,
                       "no variables available for sparse MIP start");
    }

    if (!model->mip_start || model->mip_start_n != n || !model->mip_start_mask) {
        double *dense = (double*)malloc((size_t)n * sizeof(double));
        int *mask = (int*)calloc((size_t)n, sizeof(int));
        if (!dense || !mask) {
            free(dense);
            free(mask);
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_MEMORY,
                           RALPH_ERROR_CODE_ALLOCATION_FAILED,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_MODEL_EDIT,
                           n,
                           0,
                           "failed to allocate sparse MIP start buffers");
        }
        for (int j = 0; j < n; j++) dense[j] = model->lp_model->lb[j];
        free(model->mip_start);
        free(model->mip_start_mask);
        model->mip_start = dense;
        model->mip_start_mask = mask;
        model->mip_start_n = n;
        model->mip_start_nnz = 0;
    }

    for (int k = 0; k < count; k++) {
        int j = indices[k];
        if (j < 0 || j >= n) {
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_RANGE,
                           RALPH_ERROR_CODE_OUT_OF_RANGE,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_MODEL_EDIT,
                           j,
                           n,
                           "MIP start index out of range");
        }
        model->mip_start[j] = values[k];
        if (!model->mip_start_mask[j]) {
            model->mip_start_mask[j] = 1;
            model->mip_start_nnz++;
        }
    }

    model->mip_start_status = RALPH_MIP_START_PENDING;
    return 0;
}

void ralph_core_clear_mip_start(RalphModel *model) {
    if (!model) return;
    ralph_clear_mip_start_internal(model);
}

RalphMIPStartStatus ralph_core_get_mip_start_status(const RalphModel *model) {
    if (!model) return RALPH_MIP_START_NONE;
    return model->mip_start_status;
}

int ralph_core_set_mip_start_repair_mode(RalphModel *model, RalphMIPStartRepairMode mode) {
    if (!model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    if (mode < RALPH_MIP_START_REPAIR_STRICT ||
        mode > RALPH_MIP_START_REPAIR_PROJECT_AND_ROUND) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARAMETER,
                       RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       mode,
                       0,
                       "repair mode is out of range");
    }
    model->mip_start_repair_mode = mode;
    return 0;
}

RalphMIPStartRepairMode ralph_core_get_mip_start_repair_mode(const RalphModel *model) {
    if (!model) return RALPH_MIP_START_REPAIR_STRICT;
    return model->mip_start_repair_mode;
}

/* ============================================================================
 * Constraint Modification
 * ============================================================================ */

int ralph_core_set_constraint_rhs(RalphModel *model, int constraint, double rhs) {
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    if (constraint < 0 || constraint >= model->lp_model->num_cons) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       constraint,
                       model->lp_model->num_cons,
                       "constraint index out of range");
    }

    model->lp_model->b[constraint] = rhs;
    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_core_set_constraint_sense(RalphModel *model, int constraint, RalphSense sense) {
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    if (constraint < 0 || constraint >= model->lp_model->num_cons) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       constraint,
                       model->lp_model->num_cons,
                       "constraint index out of range");
    }
    if (!ralph_is_valid_sense(sense)) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_INVALID_ARGUMENT,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       sense,
                       0,
                       "invalid constraint sense");
    }

    model->lp_model->sense[constraint] = (char)sense;
    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_core_set_constraint_coef(RalphModel *model, int constraint, int var, double coef) {
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    if (lp_model_set_coefficient(model->lp_model, constraint, var, coef) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       constraint,
                       var,
                       "constraint/variable index out of range");
    }
    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_core_set_constraint_coefs(RalphModel *model, int count,
                               const int *constraints, const int *vars,
                               const double *coefs) {
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    if (lp_model_set_coefficients(model->lp_model, count, constraints, vars, coefs) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_INVALID_ARGUMENT,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       count,
                       0,
                       "invalid batch coefficients");
    }
    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_core_set_constraint_rhs_batch(RalphModel *model, int count,
                                   const int *constraints, const double *rhs_values) {
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    if (count < 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       count,
                       0,
                       "count must be non-negative");
    }
    if (count == 0) return 0;
    if (!constraints || !rhs_values) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       count,
                       0,
                       "batch arrays are null");
    }

    int m = model->lp_model->num_cons;
    for (int i = 0; i < count; i++) {
        if (constraints[i] < 0 || constraints[i] >= m) {
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_RANGE,
                           RALPH_ERROR_CODE_OUT_OF_RANGE,
                           RALPH_STATUS_ERROR,
                           RALPH_ERROR_API_MODEL_EDIT,
                           constraints[i],
                           m,
                           "constraint index out of range");
        }
    }

    for (int i = 0; i < count; i++) {
        model->lp_model->b[constraints[i]] = rhs_values[i];
    }
    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_core_set_constraint_sense_batch(RalphModel *model, int count,
                                     const int *constraints, const RalphSense *senses) {
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    if (count < 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       count,
                       0,
                       "count must be non-negative");
    }
    if (count == 0) return 0;
    if (!constraints || !senses) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       count,
                       0,
                       "batch arrays are null");
    }

    int m = model->lp_model->num_cons;
    for (int i = 0; i < count; i++) {
        if (constraints[i] < 0 || constraints[i] >= m) {
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_RANGE,
                           RALPH_ERROR_CODE_OUT_OF_RANGE,
                           RALPH_STATUS_ERROR,
                           RALPH_ERROR_API_MODEL_EDIT,
                           constraints[i],
                           m,
                           "constraint index out of range");
        }
        if (!ralph_is_valid_sense(senses[i])) {
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_ARGUMENT,
                           RALPH_ERROR_CODE_INVALID_ARGUMENT,
                           RALPH_STATUS_ERROR,
                           RALPH_ERROR_API_MODEL_EDIT,
                           senses[i],
                           i,
                           "invalid constraint sense");
        }
    }

    for (int i = 0; i < count; i++) {
        model->lp_model->sense[constraints[i]] = (char)senses[i];
    }
    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_core_get_constraint_rhs(const RalphModel *model, int constraint, double *rhs) {
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!model || !model->lp_model || !rhs) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model or rhs output is null");
    }
    if (constraint < 0 || constraint >= model->lp_model->num_cons) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       constraint,
                       model->lp_model->num_cons,
                       "constraint index out of range");
    }
    *rhs = model->lp_model->b[constraint];
    return 0;
}

int ralph_core_get_constraint_sense(const RalphModel *model, int constraint, RalphSense *sense) {
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!model || !model->lp_model || !sense) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model or sense output is null");
    }
    if (constraint < 0 || constraint >= model->lp_model->num_cons) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       constraint,
                       model->lp_model->num_cons,
                       "constraint index out of range");
    }
    char s = model->lp_model->sense[constraint];
    if (s != 'L' && s != 'E' && s != 'G') {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_STATE,
                       RALPH_ERROR_CODE_NOT_AVAILABLE,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       s,
                       constraint,
                       "constraint sense is invalid");
    }
    *sense = (RalphSense)s;
    return 0;
}

int ralph_core_get_constraint_coef(const RalphModel *model, int constraint, int var, double *coef) {
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!model || !model->lp_model || !coef) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model or output is null");
    }
    if (lp_model_get_coefficient(model->lp_model, constraint, var, coef) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       constraint,
                       var,
                       "constraint/variable index out of range");
    }
    return 0;
}

int ralph_core_delete_constraint(RalphModel *model, int constraint) {
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    if (lp_model_delete_constraint(model->lp_model, constraint) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       constraint,
                       model->lp_model->num_cons,
                       "constraint index out of range");
    }
    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_core_delete_var(RalphModel *model, int var) {
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    if (lp_model_delete_var(model->lp_model, var) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       var,
                       model->lp_model->num_vars,
                       "variable index out of range");
    }
    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_core_get_var_bounds(const RalphModel *model, int var, double *lb, double *ub) {
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model is null");
    }
    if (var < 0 || var >= model->lp_model->num_vars) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       var,
                       model->lp_model->num_vars,
                       "variable index out of range");
    }

    if (lb) *lb = model->lp_model->lb[var];
    if (ub) *ub = model->lp_model->ub[var];

    return 0;
}

int ralph_core_add_lazy_constraint(RalphModel *model, const RalphCut *cut) {
    if (!model || !model->lp_model || !cut) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model or cut is null");
    }
    RALPH_CLEAR_API_ERROR(model);

    /* Add the constraint to the model */
    int result = lp_model_add_constraint(model->lp_model,
                                          cut->num_vars,
                                          cut->indices,
                                          cut->coeffs,
                                          (char)cut->sense,
                                          cut->rhs);
    if (result < 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_INVALID_ARGUMENT,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       result,
                       cut->num_vars,
                       "failed to add lazy constraint");
    }

    /* Invalidate solver state to force re-solve (will use warm start if available) */
    /* Note: For true warm start, we keep the LP solver but invalidate MIP solver */
    mip_free(model->mip_solver);
    model->mip_solver = NULL;

    /* Keep LP solver for potential warm start, but invalidate cached solution */
    if (model->lp_solver) {
        model->lp_solver->status = RALPH_STATUS_UNKNOWN;
    }
    if (model->mip_start && model->mip_start_n == model->lp_model->num_vars) {
        model->mip_start_status = RALPH_MIP_START_PENDING;
    }

    return 0;
}

int ralph_core_add_lazy_constraints(RalphModel *model, const RalphCut *cuts, int count) {
    if (!model || !cuts) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model or cuts is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    if (count <= 0) return 0;

    for (int i = 0; i < count; i++) {
        if (ralph_core_add_lazy_constraint(model, &cuts[i]) < 0) {
            return -1;
        }
    }

    return 0;
}

/* ============================================================================
 * Warm Start (Basis Save/Restore)
 * ============================================================================ */

RalphBasis* ralph_core_save_basis(const RalphModel *model) {
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!model || !model->lp_solver || !model->lp_solver->tableau) {
        RALPH_FAIL_API_PTR(model,
                           RALPH_ERROR_DOMAIN_STATE,
                           RALPH_ERROR_CODE_NOT_AVAILABLE,
                           model ? model->status : RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_BASIS,
                           0,
                           0,
                           "basis is unavailable (no active tableau)");
    }

    SimplexTableau *tab = model->lp_solver->tableau;
    if (!tab->basis || !tab->var_status) {
        RALPH_FAIL_API_PTR(model,
                           RALPH_ERROR_DOMAIN_STATE,
                           RALPH_ERROR_CODE_NOT_AVAILABLE,
                           model->status,
                           RALPH_ERROR_API_BASIS,
                           1,
                           0,
                           "basis vectors are unavailable");
    }

    RalphBasis *basis = (RalphBasis*)calloc(1, sizeof(RalphBasis));
    if (!basis) {
        RALPH_FAIL_API_PTR(model,
                           RALPH_ERROR_DOMAIN_MEMORY,
                           RALPH_ERROR_CODE_ALLOCATION_FAILED,
                           model->status,
                           RALPH_ERROR_API_BASIS,
                           0,
                           0,
                           "failed to allocate basis object");
    }

    basis->m = tab->m;
    basis->n = tab->n;

    /* Copy basis array (use calloc for overflow-safe size calculation) */
    basis->basis = (int*)calloc(tab->m, sizeof(int));
    if (!basis->basis) {
        free(basis);
        RALPH_FAIL_API_PTR(model,
                           RALPH_ERROR_DOMAIN_MEMORY,
                           RALPH_ERROR_CODE_ALLOCATION_FAILED,
                           model->status,
                           RALPH_ERROR_API_BASIS,
                           tab->m,
                           0,
                           "failed to allocate basis indices");
    }
    memcpy(basis->basis, tab->basis, (size_t)tab->m * sizeof(int));

    /* Copy variable status array (use calloc for overflow-safe size calculation) */
    basis->var_status = (VarStatus*)calloc(tab->n, sizeof(VarStatus));
    if (!basis->var_status) {
        free(basis->basis);
        free(basis);
        RALPH_FAIL_API_PTR(model,
                           RALPH_ERROR_DOMAIN_MEMORY,
                           RALPH_ERROR_CODE_ALLOCATION_FAILED,
                           model->status,
                           RALPH_ERROR_API_BASIS,
                           tab->n,
                           0,
                           "failed to allocate basis status array");
    }
    memcpy(basis->var_status, tab->var_status, (size_t)tab->n * sizeof(VarStatus));

    return basis;
}

int ralph_core_load_basis(RalphModel *model, const RalphBasis *basis) {
    if (!model || !basis) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_BASIS,
                       0,
                       0,
                       "model or basis is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    if (!basis->basis || !basis->var_status) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_BASIS,
                       1,
                       0,
                       "basis arrays are null");
    }

    /* Load directly into an existing live simplex tableau when available. */
    if (model->lp_solver && model->lp_solver->tableau) {
        SimplexTableau *tab = model->lp_solver->tableau;

        /* Check dimension compatibility */
        if (tab->m != basis->m || tab->n != basis->n) {
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_RANGE,
                           RALPH_ERROR_CODE_OUT_OF_RANGE,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_BASIS,
                           basis->m,
                           basis->n,
                           "basis dimensions do not match live tableau");
        }

        /* Save backup before modification so we can restore on refactorize failure */
        int *orig_basis = (int*)calloc(tab->m, sizeof(int));
        VarStatus *orig_status = (VarStatus*)calloc(tab->n, sizeof(VarStatus));
        if (!orig_basis || !orig_status) {
            free(orig_basis);
            free(orig_status);
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_MEMORY,
                           RALPH_ERROR_CODE_ALLOCATION_FAILED,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_BASIS,
                           tab->m,
                           tab->n,
                           "failed to allocate basis backup");
        }
        memcpy(orig_basis, tab->basis, (size_t)tab->m * sizeof(int));
        memcpy(orig_status, tab->var_status, (size_t)tab->n * sizeof(VarStatus));

        if (tableau_apply_warm_basis(tab, basis->m, basis->n, basis->basis, basis->var_status) != 0 ||
            tableau_refactorize(tab) != 0) {
            /* Restore original basis to avoid corrupted state */
            (void)tableau_apply_warm_basis(tab, tab->m, tab->n, orig_basis, orig_status);
            (void)tableau_refactorize(tab);
            free(orig_basis);
            free(orig_status);
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_SOLVER,
                           RALPH_ERROR_CODE_NUMERICAL_FAILURE,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_BASIS,
                           basis->m,
                           basis->n,
                           "basis application/refactorization failed");
        }

        free(orig_basis);
        free(orig_status);

        return 0;
    }

    /* No live solver yet: stage basis and apply on next optimize() call. */
    if (model->lp_model && basis->m != model->lp_model->num_cons) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_BASIS,
                       basis->m,
                       model->lp_model->num_cons,
                       "staged basis row count does not match model");
    }
    if (ralph_stage_basis_copy(model, basis) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_MEMORY,
                       RALPH_ERROR_CODE_ALLOCATION_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_BASIS,
                       basis->m,
                       basis->n,
                       "failed to stage basis copy");
    }
    return 0;
}

int ralph_core_get_basis_status(const RalphModel *model,
                           RalphBasisStatus *col_status,
                           RalphBasisStatus *row_status) {
    if (!model || !model->lp_model) return -1;
    if (!col_status && !row_status) return -1;
    if (!model->lp_solver || !model->lp_solver->tableau) return -1;

    const SimplexTableau *tab = model->lp_solver->tableau;
    if (!tab || !tab->basis || !tab->var_status) return -1;

    int num_vars = model->lp_model->num_vars;
    int num_cons = model->lp_model->num_cons;
    if (num_vars < 0 || num_cons < 0) return -1;
    if (tab->m != num_cons || num_vars > tab->n) return -1;

    if (col_status) {
        for (int j = 0; j < num_vars; j++) {
            if (ralph_basis_status_from_internal(tab->var_status[j], &col_status[j]) != 0) {
                return -1;
            }
        }
    }

    if (row_status && num_cons > 0) {
        int *row_aux = (int*)calloc((size_t)num_cons, sizeof(int));
        if (!row_aux) return -1;

        if (ralph_build_row_primary_aux_map(tab, row_aux) != 0) {
            free(row_aux);
            return -1;
        }

        for (int i = 0; i < num_cons; i++) {
            int row_var = row_aux[i];
            if (ralph_basis_status_from_internal(tab->var_status[row_var], &row_status[i]) != 0) {
                free(row_aux);
                return -1;
            }
        }
        free(row_aux);
    }

    return 0;
}

int ralph_core_set_basis_status(RalphModel *model,
                           const RalphBasisStatus *col_status,
                           const RalphBasisStatus *row_status) {
    if (!model || !model->lp_model) return -1;
    if (!col_status && !row_status) return -1;
    if (!model->lp_solver || !model->lp_solver->tableau) return -1;

    SimplexTableau *tab = model->lp_solver->tableau;
    if (!tab || !tab->basis || !tab->var_status) return -1;

    int num_vars = model->lp_model->num_vars;
    int num_cons = model->lp_model->num_cons;
    int n = tab->n;
    if (num_vars < 0 || num_cons < 0 || n < 0) return -1;
    if (tab->m != num_cons || num_vars > n) return -1;

    int *row_aux = NULL;
    if (row_status && num_cons > 0) {
        row_aux = (int*)calloc((size_t)num_cons, sizeof(int));
        if (!row_aux) return -1;
        if (ralph_build_row_primary_aux_map(tab, row_aux) != 0) {
            free(row_aux);
            return -1;
        }
    }

    int *basis_copy = NULL;
    VarStatus *status_copy = NULL;
    char *used = NULL;
    int ret = -1;

    if (num_cons > 0) {
        basis_copy = (int*)calloc((size_t)num_cons, sizeof(int));
        if (!basis_copy) goto cleanup;
    }
    if (n > 0) {
        status_copy = (VarStatus*)calloc((size_t)n, sizeof(VarStatus));
        if (!status_copy) goto cleanup;
        used = (char*)calloc((size_t)n, sizeof(char));
        if (!used) goto cleanup;
    }

    if (num_cons > 0) memcpy(basis_copy, tab->basis, (size_t)num_cons * sizeof(int));
    if (n > 0) memcpy(status_copy, tab->var_status, (size_t)n * sizeof(VarStatus));

    if (col_status) {
        for (int j = 0; j < num_vars; j++) {
            if (ralph_basis_status_to_internal(col_status[j], &status_copy[j]) != 0) {
                goto cleanup;
            }
        }
    }

    if (row_status) {
        for (int i = 0; i < num_cons; i++) {
            int row_var = row_aux[i];
            if (row_var < 0 || row_var >= n) goto cleanup;
            if (ralph_basis_status_to_internal(row_status[i], &status_copy[row_var]) != 0) {
                goto cleanup;
            }
        }
    }

    int basic_count = 0;
    for (int j = 0; j < n; j++) {
        if (status_copy[j] == RALPH_BASIC) basic_count++;
    }
    if (basic_count != num_cons) goto cleanup;

    for (int i = 0; i < num_cons; i++) {
        int old_var = tab->basis[i];
        if (old_var >= 0 && old_var < n &&
            status_copy[old_var] == RALPH_BASIC &&
            !used[old_var]) {
            basis_copy[i] = old_var;
            used[old_var] = 1;
        } else {
            basis_copy[i] = -1;
        }
    }

    int fill = 0;
    for (int j = 0; j < n; j++) {
        if (status_copy[j] != RALPH_BASIC || used[j]) continue;
        while (fill < num_cons && basis_copy[fill] >= 0) fill++;
        if (fill >= num_cons) goto cleanup;
        basis_copy[fill] = j;
        used[j] = 1;
    }

    for (int i = 0; i < num_cons; i++) {
        if (basis_copy[i] < 0) goto cleanup;
    }

    RalphBasis staged;
    memset(&staged, 0, sizeof(staged));
    staged.m = num_cons;
    staged.n = n;
    staged.basis = basis_copy;
    staged.var_status = status_copy;

    ret = ralph_core_load_basis(model, &staged);

cleanup:
    free(row_aux);
    free(basis_copy);
    free(status_copy);
    free(used);
    return ret;
}

void ralph_core_free_basis(RalphBasis *basis) {
    if (!basis) return;
    free(basis->basis);
    free(basis->var_status);
    free(basis);
}

int ralph_core_write_basis_file(const RalphBasis *basis, const char *filename) {
    if (!basis || !filename || !basis->basis || !basis->var_status) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "basis or filename is null");
    }
    RALPH_CLEAR_API_ERROR(NULL);
    if (basis->m < 0 || basis->n < 0) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_IO,
                       basis->m,
                       basis->n,
                       "basis dimensions are invalid");
    }

    FILE *fp = fopen(filename, "w");
    if (!fp) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_IO,
                       RALPH_ERROR_CODE_IO_OPEN_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "failed to open basis file for write");
    }

    if (fprintf(fp, "RALPH_BASIS_V1 %d %d\n", basis->m, basis->n) < 0) {
        fclose(fp);
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_IO,
                       RALPH_ERROR_CODE_IO_WRITE_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "failed to write basis header");
    }

    for (int i = 0; i < basis->m; i++) {
        if (fprintf(fp, "%d%c", basis->basis[i], (i + 1 == basis->m) ? '\n' : ' ') < 0) {
            fclose(fp);
            RALPH_FAIL_API(NULL,
                           RALPH_ERROR_DOMAIN_IO,
                           RALPH_ERROR_CODE_IO_WRITE_FAILED,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_IO,
                           i,
                           basis->m,
                           "failed to write basis indices");
        }
    }
    if (basis->m == 0 && fprintf(fp, "\n") < 0) {
        fclose(fp);
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_IO,
                       RALPH_ERROR_CODE_IO_WRITE_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "failed to write empty basis row");
    }

    for (int j = 0; j < basis->n; j++) {
        if (fprintf(fp, "%d%c", (int)basis->var_status[j], (j + 1 == basis->n) ? '\n' : ' ') < 0) {
            fclose(fp);
            RALPH_FAIL_API(NULL,
                           RALPH_ERROR_DOMAIN_IO,
                           RALPH_ERROR_CODE_IO_WRITE_FAILED,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_IO,
                           j,
                           basis->n,
                           "failed to write basis status");
        }
    }
    if (basis->n == 0 && fprintf(fp, "\n") < 0) {
        fclose(fp);
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_IO,
                       RALPH_ERROR_CODE_IO_WRITE_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "failed to write empty basis status row");
    }

    if (fclose(fp) != 0) {
        RALPH_FAIL_API(NULL,
                       RALPH_ERROR_DOMAIN_IO,
                       RALPH_ERROR_CODE_IO_WRITE_FAILED,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "failed to close basis file");
    }
    return 0;
}

RalphBasis* ralph_core_read_basis_file(const char *filename) {
    if (!filename) {
        RALPH_FAIL_API_PTR(NULL,
                           RALPH_ERROR_DOMAIN_ARGUMENT,
                           RALPH_ERROR_CODE_NULL_POINTER,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_IO,
                           0,
                           0,
                           "filename is null");
    }
    RALPH_CLEAR_API_ERROR(NULL);

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        RALPH_FAIL_API_PTR(NULL,
                           RALPH_ERROR_DOMAIN_IO,
                           RALPH_ERROR_CODE_IO_OPEN_FAILED,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_IO,
                           0,
                           0,
                           "failed to open basis file for read");
    }

    char magic[32] = {0};
    int m = 0, n = 0;
    if (fscanf(fp, "%31s %d %d", magic, &m, &n) != 3) {
        fclose(fp);
        RALPH_FAIL_API_PTR(NULL,
                           RALPH_ERROR_DOMAIN_PARSE,
                           RALPH_ERROR_CODE_PARSE_FAILED,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_IO,
                           0,
                           0,
                           "failed to parse basis header");
    }
    if (strcmp(magic, "RALPH_BASIS_V1") != 0 || m < 0 || n < 0) {
        fclose(fp);
        RALPH_FAIL_API_PTR(NULL,
                           RALPH_ERROR_DOMAIN_PARSE,
                           RALPH_ERROR_CODE_PARSE_FAILED,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_IO,
                           m,
                           n,
                           "invalid basis header");
    }

    RalphBasis *basis = (RalphBasis*)calloc(1, sizeof(RalphBasis));
    if (!basis) {
        fclose(fp);
        RALPH_FAIL_API_PTR(NULL,
                           RALPH_ERROR_DOMAIN_MEMORY,
                           RALPH_ERROR_CODE_ALLOCATION_FAILED,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_IO,
                           0,
                           0,
                           "failed to allocate basis object");
    }
    basis->m = m;
    basis->n = n;

    if (m > 0) {
        basis->basis = (int*)calloc((size_t)m, sizeof(int));
        if (!basis->basis) {
            ralph_core_free_basis(basis);
            fclose(fp);
            RALPH_FAIL_API_PTR(NULL,
                               RALPH_ERROR_DOMAIN_MEMORY,
                               RALPH_ERROR_CODE_ALLOCATION_FAILED,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_IO,
                               m,
                               0,
                               "failed to allocate basis indices");
        }
    }
    if (n > 0) {
        basis->var_status = (VarStatus*)calloc((size_t)n, sizeof(VarStatus));
        if (!basis->var_status) {
            ralph_core_free_basis(basis);
            fclose(fp);
            RALPH_FAIL_API_PTR(NULL,
                               RALPH_ERROR_DOMAIN_MEMORY,
                               RALPH_ERROR_CODE_ALLOCATION_FAILED,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_IO,
                               n,
                               0,
                               "failed to allocate basis status");
        }
    }

    for (int i = 0; i < m; i++) {
        if (fscanf(fp, "%d", &basis->basis[i]) != 1) {
            ralph_core_free_basis(basis);
            fclose(fp);
            RALPH_FAIL_API_PTR(NULL,
                               RALPH_ERROR_DOMAIN_PARSE,
                               RALPH_ERROR_CODE_PARSE_FAILED,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_IO,
                               i,
                               m,
                               "failed to parse basis index");
        }
    }
    for (int j = 0; j < n; j++) {
        int v = 0;
        if (fscanf(fp, "%d", &v) != 1) {
            ralph_core_free_basis(basis);
            fclose(fp);
            RALPH_FAIL_API_PTR(NULL,
                               RALPH_ERROR_DOMAIN_PARSE,
                               RALPH_ERROR_CODE_PARSE_FAILED,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_IO,
                               j,
                               n,
                               "failed to parse basis status");
        }
        if (v < (int)RALPH_BASIC || v > (int)RALPH_FIXED) {
            ralph_core_free_basis(basis);
            fclose(fp);
            RALPH_FAIL_API_PTR(NULL,
                               RALPH_ERROR_DOMAIN_PARSE,
                               RALPH_ERROR_CODE_PARSE_FAILED,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_IO,
                               v,
                               j,
                               "basis status value out of range");
        }
        basis->var_status[j] = (VarStatus)v;
    }

    fclose(fp);
    return basis;
}

int ralph_core_write_mip_start_file(const RalphModel *model, const char *filename) {
    if (!model || !model->lp_model || !filename) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "model or filename is null");
    }
    RALPH_CLEAR_API_ERROR(model);

    int n = model->lp_model->num_vars;
    if (n <= 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_STATE,
                       RALPH_ERROR_CODE_NOT_AVAILABLE,
                       model->status,
                       RALPH_ERROR_API_IO,
                       n,
                       0,
                       "no variables available for MIP start serialization");
    }

    const double *start = NULL;
    const int *mask = NULL;
    int nnz = 0;

    if (model->mip_start && model->mip_start_n == n) {
        start = model->mip_start;
        mask = model->mip_start_mask;
        nnz = model->mip_start_nnz;
    } else if (model->solution && ralph_core_is_mip(model)) {
        start = model->solution;
    }

    if (!start) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_STATE,
                       RALPH_ERROR_CODE_NOT_AVAILABLE,
                       model->status,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "no MIP start or incumbent solution available");
    }

    FILE *fp = fopen(filename, "w");
    if (!fp) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_IO,
                       RALPH_ERROR_CODE_IO_OPEN_FAILED,
                       model->status,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "failed to open MIP start file for write");
    }

    if (fprintf(fp, "RALPH_MIPSTART_V1 %d %d %d\n", n,
                (int)model->mip_start_repair_mode, nnz) < 0) {
        fclose(fp);
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_IO,
                       RALPH_ERROR_CODE_IO_WRITE_FAILED,
                       model->status,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "failed to write MIP start header");
    }

    for (int j = 0; j < n; j++) {
        if (fprintf(fp, "%.17g%c", start[j], (j + 1 == n) ? '\n' : ' ') < 0) {
            fclose(fp);
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_IO,
                           RALPH_ERROR_CODE_IO_WRITE_FAILED,
                           model->status,
                           RALPH_ERROR_API_IO,
                           j,
                           n,
                           "failed to write MIP start values");
        }
    }

    for (int j = 0; j < n; j++) {
        int bit = mask ? (mask[j] ? 1 : 0) : 1;
        if (fprintf(fp, "%d%c", bit, (j + 1 == n) ? '\n' : ' ') < 0) {
            fclose(fp);
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_IO,
                           RALPH_ERROR_CODE_IO_WRITE_FAILED,
                           model->status,
                           RALPH_ERROR_API_IO,
                           j,
                           n,
                           "failed to write MIP start mask");
        }
    }

    if (fclose(fp) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_IO,
                       RALPH_ERROR_CODE_IO_WRITE_FAILED,
                       model->status,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "failed to close MIP start file");
    }
    return 0;
}

int ralph_core_read_mip_start_file(RalphModel *model, const char *filename) {
    if (!model || !model->lp_model || !filename) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "model or filename is null");
    }
    RALPH_CLEAR_API_ERROR(model);

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_IO,
                       RALPH_ERROR_CODE_IO_OPEN_FAILED,
                       model->status,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "failed to open MIP start file for read");
    }

    char magic[32] = {0};
    int n = 0;
    int repair = 0;
    int nnz = 0;
    if (fscanf(fp, "%31s %d %d %d", magic, &n, &repair, &nnz) != 4) {
        fclose(fp);
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARSE,
                       RALPH_ERROR_CODE_PARSE_FAILED,
                       model->status,
                       RALPH_ERROR_API_IO,
                       0,
                       0,
                       "failed to parse MIP start header");
    }
    if (strcmp(magic, "RALPH_MIPSTART_V1") != 0 || n <= 0 ||
        n != model->lp_model->num_vars) {
        fclose(fp);
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_PARSE,
                       RALPH_ERROR_CODE_PARSE_FAILED,
                       model->status,
                       RALPH_ERROR_API_IO,
                       n,
                       model->lp_model->num_vars,
                       "invalid MIP start header");
    }

    double *start = (double*)malloc((size_t)n * sizeof(double));
    int *mask = (int*)calloc((size_t)n, sizeof(int));
    if (!start || !mask) {
        free(start);
        free(mask);
        fclose(fp);
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_MEMORY,
                       RALPH_ERROR_CODE_ALLOCATION_FAILED,
                       model->status,
                       RALPH_ERROR_API_IO,
                       n,
                       0,
                       "failed to allocate MIP start buffers");
    }

    for (int j = 0; j < n; j++) {
        if (fscanf(fp, "%lf", &start[j]) != 1) {
            free(start);
            free(mask);
            fclose(fp);
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_PARSE,
                           RALPH_ERROR_CODE_PARSE_FAILED,
                           model->status,
                           RALPH_ERROR_API_IO,
                           j,
                           n,
                           "failed to parse MIP start value");
        }
    }
    int counted = 0;
    for (int j = 0; j < n; j++) {
        int bit = 0;
        if (fscanf(fp, "%d", &bit) != 1) {
            free(start);
            free(mask);
            fclose(fp);
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_PARSE,
                           RALPH_ERROR_CODE_PARSE_FAILED,
                           model->status,
                           RALPH_ERROR_API_IO,
                           j,
                           n,
                           "failed to parse MIP start mask");
        }
        mask[j] = bit ? 1 : 0;
        counted += mask[j];
    }

    fclose(fp);

    if (ralph_set_mip_start_copy(model, start, mask, n) != 0) {
        free(start);
        free(mask);
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_MEMORY,
                       RALPH_ERROR_CODE_ALLOCATION_FAILED,
                       model->status,
                       RALPH_ERROR_API_IO,
                       n,
                       0,
                       "failed to stage MIP start");
    }
    model->mip_start_nnz = counted;

    if (repair >= (int)RALPH_MIP_START_REPAIR_STRICT &&
        repair <= (int)RALPH_MIP_START_REPAIR_PROJECT_AND_ROUND) {
        model->mip_start_repair_mode = (RalphMIPStartRepairMode)repair;
    } else {
        model->mip_start_repair_mode = RALPH_MIP_START_REPAIR_STRICT;
    }

    free(start);
    free(mask);
    return 0;
}

/* ============================================================================
 * Cut Callback
 * ============================================================================ */

void ralph_core_set_cut_callback(RalphModel *model, const RalphCutCallback *callback) {
    if (!model) return;

    if (callback) {
        model->cut_callback = *callback;
        model->has_cut_callback = 1;
    } else {
        memset(&model->cut_callback, 0, sizeof(RalphCutCallback));
        model->has_cut_callback = 0;
    }
}

/* ============================================================================
 * Branching Callback
 * ============================================================================ */

void ralph_core_set_branch_callback(RalphModel *model, const RalphBranchCallback *callback) {
    if (!model) return;

    if (callback) {
        model->branch_callback = *callback;
        model->has_branch_callback = 1;
    } else {
        memset(&model->branch_callback, 0, sizeof(RalphBranchCallback));
        model->has_branch_callback = 0;
    }
}

void ralph_core_set_lp_progress_callback(RalphModel *model,
                                    const RalphLPProgressCallback *callback) {
    if (!model) return;

    if (callback) {
        model->lp_progress_callback = *callback;
        model->has_lp_progress_callback = (callback->on_progress != NULL) ? 1 : 0;
    } else {
        memset(&model->lp_progress_callback, 0, sizeof(RalphLPProgressCallback));
        model->has_lp_progress_callback = 0;
    }
}

void ralph_core_set_lp_cancel_callback(RalphModel *model,
                                  const RalphLPCancelCallback *callback) {
    if (!model) return;

    if (callback) {
        model->lp_cancel_callback = *callback;
        model->has_lp_cancel_callback = (callback->should_cancel != NULL) ? 1 : 0;
    } else {
        memset(&model->lp_cancel_callback, 0, sizeof(RalphLPCancelCallback));
        model->has_lp_cancel_callback = 0;
    }
}

/* ============================================================================
 * Benders Decomposition
 * ============================================================================ */

int ralph_core_solve_benders(
    RalphModel *model,
    const RalphBendersConfig *config,
    double *x,
    RalphBendersResult *result)
{
    if (!model || !config || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_BENDERS,
                       0,
                       0,
                       "model or benders config is null");
    }
    RALPH_CLEAR_API_ERROR(model);

    /* Finalize model if needed (builds sparse matrix A) */
    if (!model->lp_model->A) {
        if (lp_model_finalize(model->lp_model) != 0) {
            RALPH_FAIL_API(model,
                           RALPH_ERROR_DOMAIN_SOLVER,
                           RALPH_ERROR_CODE_SOLVE_FAILED,
                           RALPH_STATUS_ERROR,
                           RALPH_ERROR_API_BENDERS,
                           0,
                           0,
                           "failed to finalize model before Benders solve");
        }
    }

    /* Delegate to internal Benders solver */
    if (benders_solve(model->lp_model, config, x, result) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_SOLVER,
                       RALPH_ERROR_CODE_SOLVE_FAILED,
                       RALPH_STATUS_ERROR,
                       RALPH_ERROR_API_BENDERS,
                       0,
                       0,
                       "Benders solve failed");
    }
    return 0;
}

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
            .default_value = 0.0,
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
            .default_value = (double)RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX,
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
        [RALPH_PARAM_LP_POLICY_PROFILE] = {
            .id = RALPH_PARAM_LP_POLICY_PROFILE,
            .name = "lp_policy_profile",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)RALPH_LP_POLICY_PROFILE_DEFAULT,
            .has_min = 1,
            .min_value = (double)RALPH_LP_POLICY_PROFILE_DEFAULT,
            .has_max = 1,
            .max_value = (double)RALPH_LP_POLICY_PROFILE_GLPK_COMPAT,
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
            .max_value = (double)RALPH_LP_GLPK_SMCP_BASIS_STD,
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
        [RALPH_PARAM_GLPK_BFCP_PIVOT_TOL] = {
            .id = RALPH_PARAM_GLPK_BFCP_PIVOT_TOL,
            .name = "glpk_bfcp_pivot_tol",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_DOUBLE,
            .default_value = 0.0,
            .aliases = {"GLPKBFCPPivotTol"},
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
        case RALPH_PARAM_LP_POLICY_PROFILE:
        case RALPH_PARAM_GLPK_SMCP_METHOD:
        case RALPH_PARAM_GLPK_SMCP_PRICING:
        case RALPH_PARAM_GLPK_SMCP_RATIO:
        case RALPH_PARAM_GLPK_SMCP_FLIP:
        case RALPH_PARAM_GLPK_SMCP_BASIS:
        case RALPH_PARAM_GLPK_SMCP_PRESOLVE:
        case RALPH_PARAM_GLPK_BFCP_BACKEND:
        case RALPH_PARAM_GLPK_BFCP_UPDATE_LIMIT:
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
        case RALPH_PARAM_GLPK_BFCP_PIVOT_TOL:
        case RALPH_PARAM_GLPK_BFCP_GROWTH_GUARD:
            if (!isfinite(value)) {
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_PARAMETER,
                               RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_PARAMETER,
                               param,
                               0,
                               "glpk bfcp float parameter must be finite");
            }
            if (ralph_set_glpk_policy_double_param(model, param, value) != 0) {
                RALPH_FAIL_API(model,
                               RALPH_ERROR_DOMAIN_PARAMETER,
                               RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID,
                               RALPH_STATUS_UNKNOWN,
                               RALPH_ERROR_API_PARAMETER,
                               param,
                               0,
                               "invalid glpk bfcp float parameter value");
            }
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
        case RALPH_PARAM_GLPK_BFCP_BACKEND:
            *value = model->glpk_bfcp_backend;
            break;
        case RALPH_PARAM_GLPK_BFCP_UPDATE_LIMIT:
            *value = model->glpk_bfcp_update_limit;
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
        case RALPH_PARAM_GLPK_BFCP_PIVOT_TOL:
            *value = model->glpk_bfcp_pivot_tol;
            break;
        case RALPH_PARAM_GLPK_BFCP_GROWTH_GUARD:
            *value = model->glpk_bfcp_growth_guard;
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

const char* ralph_core_status_string(RalphStatus status) {
    switch (status) {
        case RALPH_STATUS_UNKNOWN:        return "UNKNOWN";
        case RALPH_STATUS_OPTIMAL:        return "OPTIMAL";
        case RALPH_STATUS_INFEASIBLE:     return "INFEASIBLE";
        case RALPH_STATUS_UNBOUNDED:      return "UNBOUNDED";
        case RALPH_STATUS_INF_OR_UNBD:    return "INF_OR_UNBOUNDED";
        case RALPH_STATUS_ITERATION_LIMIT: return "ITERATION_LIMIT";
        case RALPH_STATUS_TIME_LIMIT:     return "TIME_LIMIT";
        case RALPH_STATUS_NODE_LIMIT:     return "NODE_LIMIT";
        case RALPH_STATUS_IMPRECISE:     return "IMPRECISE";
        case RALPH_STATUS_OBJ_LIMIT:    return "OBJ_LIMIT";
        case RALPH_STATUS_ERROR:          return "ERROR";
        default:                          return "UNKNOWN";
    }
}

const char* ralph_core_version(void) {
    return RALPH_VERSION;
}

/* ============================================================================
 * Name Management
 * ============================================================================ */

const char* ralph_core_get_var_name(const RalphModel *model, int var) {
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!model || !model->lp_model) {
        RALPH_FAIL_API_PTR(model,
                           RALPH_ERROR_DOMAIN_ARGUMENT,
                           RALPH_ERROR_CODE_NULL_POINTER,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_MODEL_EDIT,
                           0,
                           0,
                           "model is null");
    }
    return lp_model_get_var_name(model->lp_model, var);
}

const char* ralph_core_get_con_name(const RalphModel *model, int con) {
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!model || !model->lp_model) {
        RALPH_FAIL_API_PTR(model,
                           RALPH_ERROR_DOMAIN_ARGUMENT,
                           RALPH_ERROR_CODE_NULL_POINTER,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_MODEL_EDIT,
                           0,
                           0,
                           "model is null");
    }
    return lp_model_get_con_name(model->lp_model, con);
}

int ralph_core_set_var_name(RalphModel *model, int var, const char *name) {
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    if (lp_model_set_var_name(model->lp_model, var, name) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       var,
                       model->lp_model->num_vars,
                       "failed to set variable name");
    }
    return 0;
}

int ralph_core_set_con_name(RalphModel *model, int con, const char *name) {
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    if (lp_model_set_con_name(model->lp_model, con, name) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_RANGE,
                       RALPH_ERROR_CODE_OUT_OF_RANGE,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       con,
                       model->lp_model->num_cons,
                       "failed to set constraint name");
    }
    return 0;
}

const char* ralph_core_get_problem_name(const RalphModel *model) {
    if (model) RALPH_CLEAR_API_ERROR(model);
    if (!model || !model->lp_model) {
        RALPH_FAIL_API_PTR(model,
                           RALPH_ERROR_DOMAIN_ARGUMENT,
                           RALPH_ERROR_CODE_NULL_POINTER,
                           RALPH_STATUS_UNKNOWN,
                           RALPH_ERROR_API_MODEL_EDIT,
                           0,
                           0,
                           "model is null");
    }
    return lp_model_get_name(model->lp_model);
}

int ralph_core_set_problem_name(RalphModel *model, const char *name) {
    if (!model || !model->lp_model) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_NULL_POINTER,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "model is null");
    }
    RALPH_CLEAR_API_ERROR(model);
    if (lp_model_set_name(model->lp_model, name) != 0) {
        RALPH_FAIL_API(model,
                       RALPH_ERROR_DOMAIN_ARGUMENT,
                       RALPH_ERROR_CODE_INVALID_ARGUMENT,
                       RALPH_STATUS_UNKNOWN,
                       RALPH_ERROR_API_MODEL_EDIT,
                       0,
                       0,
                       "failed to set problem name");
    }
    return 0;
}

/* Internal helper for LP writer - provides access to LPModel */
LPModel* ralph_get_lp_model(const RalphModel *model) {
    return model ? model->lp_model : NULL;
}

/* Internal helper for benchmark diagnostics */
SimplexSolver* ralph_get_lp_solver(const RalphModel *model) {
    return model ? model->lp_solver : NULL;
}

/* Internal helper for integration tests and benchmark diagnostics */
MIPSolver* ralph_get_mip_solver(const RalphModel *model) {
    return model ? model->mip_solver : NULL;
}
