/*
 * ralph_internal.h - the model struct and API-error macros, shared
 * between the translation units that make up the Ralph facade.
 *
 * Private to ralph/src. RalphModel is deliberately opaque in the public
 * headers (ralph_core.h:204) and must stay that way; this is the view
 * ralph.c and ralph_params.c need of each other, and nothing outside
 * ralph/src includes it.
 *
 * It exists because extracting the parameter subsystem out of ralph.c
 * turned out not to be the pure table-and-dispatch move it looked like:
 * the dispatchers read and write model fields, so the struct had to
 * become visible to two files rather than one.
 */
#ifndef RALPH_INTERNAL_H
#define RALPH_INTERNAL_H

#include <stdint.h>
#include <time.h>

#include "ralph_core.h"
#include "lp.h"
#include "mip.h"
#include "presolve.h"
#include "detect.h"
#include "benders.h"
#include "lp_error.h"

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
    int method;  /* 0=primal simplex, 1=dual simplex, 2=dual-first fallback */
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
    int dual_steepest_edge; /* -1=solver default, 0=off, 1=on */
    int var_select;         /* -1=default, 0=most_infeas, 1=pseudo_cost, 2=strong, 3=reliability */
    int lu_supernode;       /* 0=off (default), 1=enable supernodal LU factorization (T2.1) */
    int deterministic;      /* 1=enforce deterministic LP runtime policy */
    int random_seed;        /* deterministic LP seed for anti-cycling perturbation offsets */
    int lp_threads;         /* LP thread policy (0=auto; deterministic mode defaults to 1) */
    int lp_basis_governor_mode; /* 0=off, 1=shadow, 2=control_phase2 */
    int lp_reinvert_controller_mode; /* 0=off, 1=shadow, 2=control_phase1, 3=control_all */
    int lp_policy_profile;  /* 0=default, 1=glpk_compat, 2=glpk_strict, 3=glpk_legacy */
    int glpk_smcp_method;   /* 0=auto, 1=primal, 2=dualp, 3=dual */
    int glpk_smcp_pricing;  /* 0=standard, 1=steep */
    int glpk_smcp_ratio;    /* 0=standard, 1=harris */
    int glpk_smcp_flip;     /* 0=off, 1=on */
    int glpk_smcp_basis;    /* 0=adv, 1=std, 2=bib, 3=ini */
    int glpk_smcp_presolve; /* 0=auto, 1=off, 2=on */
    double glpk_smcp_tol_bnd; /* >0 */
    double glpk_smcp_tol_dj;  /* >0 */
    double glpk_smcp_tol_piv; /* >0 */
    int glpk_smcp_excl;       /* 0=off, 1=on */
    int glpk_smcp_shift;      /* 0=off, 1=on */
    int glpk_smcp_aorn;       /* 1=use A^T, 2=use N^T */
    int glpk_bfcp_factorization; /* 0=luf, 1=btf */
    int glpk_bfcp_backend;  /* 0=luf_ft, 1=cbg, 2=cgr */
    int glpk_bfcp_update_limit; /* -1=auto */
    int glpk_bfcp_pivot_limit; /* -1=auto */
    int glpk_bfcp_suhl; /* -1=auto, 0=off, 1=on */
    double glpk_bfcp_pivot_tol; /* <=0=auto */
    double glpk_bfcp_eps_tol; /* <=0=auto */
    double glpk_bfcp_growth_guard; /* <=0=auto */
    int glpk_bfcp_nfs_max; /* -1=auto */
    int glpk_bfcp_nrs_max; /* -1=auto */

    /* Refactoring policy overrides (R2) */
    int refactor_min_interval;      /* Phase 2 periodic min interval (default 10) */
    int refactor_max_interval;      /* Phase 2 periodic max interval (default 80) */
    int degen_escape_min_m;         /* Min m for degen escape (default 1200) */
    double lu_cost_ewma_alpha;      /* EWMA alpha for LU cost tracking (default 0.20) */
    int lu_spike_warn_pct;          /* Spike pool warning pct (default 85) */

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
    void *owned_cut_callback_ctx;
    void (*free_owned_cut_callback_ctx)(void *);

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

/* Used by the RALPH_FAIL_API macros below, so both translation units that
 * expand them need to see these. */
void ralph_set_api_error(const RalphModel *model,
                         RalphErrorDomain domain,
                         RalphErrorCode code,
                         RalphStatus status_hint,
                         RalphErrorAPIId api_id,
                         int detail_i0,
                         int detail_i1,
                         const char *message);
void ralph_clear_api_error(const RalphModel *model);
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

#endif /* RALPH_INTERNAL_H */
