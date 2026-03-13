/*
 * Ralph - A Linear Programming and Mixed Integer Programming Solver
 *
 * Internal Core API Header
 *
 * This header is internal-only. External consumers must use:
 * - ralph_lp.h
 * - ralph_mip.h
 */

#ifndef RALPH_CORE_H
#define RALPH_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Solution status codes */
typedef enum {
    RALPH_STATUS_UNKNOWN = 0,
    RALPH_STATUS_OPTIMAL = 1,
    RALPH_STATUS_INFEASIBLE = 2,
    RALPH_STATUS_UNBOUNDED = 3,
    RALPH_STATUS_INF_OR_UNBD = 4,
    RALPH_STATUS_ITERATION_LIMIT = 5,
    RALPH_STATUS_TIME_LIMIT = 6,
    RALPH_STATUS_NODE_LIMIT = 7,
    RALPH_STATUS_IMPRECISE = 8,
    RALPH_STATUS_OBJ_LIMIT = 9,
    RALPH_STATUS_ERROR = -1
} RalphStatus;

#ifndef RALPH_API_ERROR_TYPES_DEFINED
#define RALPH_API_ERROR_TYPES_DEFINED
#define RALPH_API_ERROR_MESSAGE_MAX 160

/* High-level error domain for structured API diagnostics. */
typedef enum {
    RALPH_ERROR_DOMAIN_NONE = 0,
    RALPH_ERROR_DOMAIN_ARGUMENT = 1,
    RALPH_ERROR_DOMAIN_STATE = 2,
    RALPH_ERROR_DOMAIN_RANGE = 3,
    RALPH_ERROR_DOMAIN_PARAMETER = 4,
    RALPH_ERROR_DOMAIN_MEMORY = 5,
    RALPH_ERROR_DOMAIN_IO = 6,
    RALPH_ERROR_DOMAIN_PARSE = 7,
    RALPH_ERROR_DOMAIN_SOLVER = 8,
    RALPH_ERROR_DOMAIN_EXTERNAL = 9,
    RALPH_ERROR_DOMAIN_INTERNAL = 10
} RalphErrorDomain;

/* Stable machine-readable error code. */
typedef enum {
    RALPH_ERROR_CODE_NONE = 0,
    RALPH_ERROR_CODE_INVALID_ARGUMENT = 1,
    RALPH_ERROR_CODE_NULL_POINTER = 2,
    RALPH_ERROR_CODE_OUT_OF_RANGE = 3,
    RALPH_ERROR_CODE_NOT_AVAILABLE = 4,
    RALPH_ERROR_CODE_UNKNOWN_PARAMETER = 5,
    RALPH_ERROR_CODE_PARAMETER_SCOPE_MISMATCH = 6,
    RALPH_ERROR_CODE_PARAMETER_VALUE_INVALID = 7,
    RALPH_ERROR_CODE_ALLOCATION_FAILED = 8,
    RALPH_ERROR_CODE_IO_OPEN_FAILED = 9,
    RALPH_ERROR_CODE_IO_READ_FAILED = 10,
    RALPH_ERROR_CODE_IO_WRITE_FAILED = 11,
    RALPH_ERROR_CODE_PARSE_FAILED = 12,
    RALPH_ERROR_CODE_SOLVE_FAILED = 13,
    RALPH_ERROR_CODE_EXTERNAL_DISPATCH_FAILED = 14,
    RALPH_ERROR_CODE_EXTERNAL_EXECUTION_FAILED = 15,
    RALPH_ERROR_CODE_NUMERICAL_FAILURE = 16,
    RALPH_ERROR_CODE_LIMIT_REACHED = 17,
    RALPH_ERROR_CODE_INTERNAL_FAILURE = 18
} RalphErrorCode;

/* API call family that raised the error. */
typedef enum {
    RALPH_ERROR_API_NONE = 0,
    RALPH_ERROR_API_LIFECYCLE = 1,
    RALPH_ERROR_API_MODEL_BUILD = 2,
    RALPH_ERROR_API_MODEL_EDIT = 3,
    RALPH_ERROR_API_SOLVE = 4,
    RALPH_ERROR_API_SOLUTION_QUERY = 5,
    RALPH_ERROR_API_PARAMETER = 6,
    RALPH_ERROR_API_BASIS = 7,
    RALPH_ERROR_API_IO = 8,
    RALPH_ERROR_API_EXTERNAL = 9,
    RALPH_ERROR_API_PARSE = 10,
    RALPH_ERROR_API_BENDERS = 11
} RalphErrorAPIId;

/* Last API error snapshot.
 * `detail_i0/detail_i1` are optional context integers (index/size/errno-like). */
typedef struct {
    RalphErrorDomain domain;
    RalphErrorCode code;
    RalphStatus status_hint;
    RalphErrorAPIId api_id;
    int detail_i0;
    int detail_i1;
    char message[RALPH_API_ERROR_MESSAGE_MAX];
} RalphAPIError;
#endif /* RALPH_API_ERROR_TYPES_DEFINED */

/* Variable types */
typedef enum {
    RALPH_CONTINUOUS = 'C',
    RALPH_INTEGER = 'I',
    RALPH_BINARY = 'B'
} RalphVarType;

/* Constraint sense */
typedef enum {
    RALPH_LESS_EQUAL = 'L',
    RALPH_EQUAL = 'E',
    RALPH_GREATER_EQUAL = 'G'
} RalphSense;

/* Objective sense */
typedef enum {
    RALPH_MINIMIZE = 1,
    RALPH_MAXIMIZE = -1
} RalphObjSense;

#ifndef RALPH_LP_ALGO_BASE_TYPES_DEFINED
/* LP algorithm selection API surface.
 * Internal-first policy:
 * - PRIMAL/DUAL/AUTO route to internal simplex backends.
 * - *_EXTERNAL modes require lp_external_provider + registered adapter match.
 * Barrier backends are capability-gated. */
typedef enum {
    RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX = 0,
    RALPH_LP_ALGORITHM_DUAL_SIMPLEX = 1,
    RALPH_LP_ALGORITHM_AUTO = 2,
    RALPH_LP_ALGORITHM_BARRIER = 3,
    RALPH_LP_ALGORITHM_PRIMAL_SIMPLEX_EXTERNAL = 4,
    RALPH_LP_ALGORITHM_DUAL_SIMPLEX_EXTERNAL = 5,
    RALPH_LP_ALGORITHM_BARRIER_EXTERNAL = 6
} RalphLPAlgorithm;

/* External LP backend provider IDs (used by lp_external_provider parameter). */
typedef enum {
    RALPH_LP_EXTERNAL_PROVIDER_NONE = 0,
    RALPH_LP_EXTERNAL_PROVIDER_GLPK = 1,
    RALPH_LP_EXTERNAL_PROVIDER_HIGHS = 2,
    RALPH_LP_EXTERNAL_PROVIDER_CLP = 3,
    RALPH_LP_EXTERNAL_PROVIDER_CPLEX = 4,
    RALPH_LP_EXTERNAL_PROVIDER_GUROBI = 5,
    RALPH_LP_EXTERNAL_PROVIDER_GLOP = 6
} RalphLPExternalProvider;

typedef enum {
    RALPH_LP_CROSSOVER_AUTO = 0,
    RALPH_LP_CROSSOVER_OFF = 1,
    RALPH_LP_CROSSOVER_ON = 2
} RalphLPCrossoverMode;

typedef enum {
    RALPH_LP_FALLBACK_NONE = 0,
    RALPH_LP_FALLBACK_BARRIER_UNAVAILABLE = 1,
    RALPH_LP_FALLBACK_CROSSOVER_UNAVAILABLE = 2,
    RALPH_LP_FALLBACK_EXTERNAL_UNAVAILABLE = 3
} RalphLPFallbackReason;
#define RALPH_LP_ALGO_BASE_TYPES_DEFINED
#endif /* RALPH_LP_ALGO_BASE_TYPES_DEFINED */

/* Constants */
#define RALPH_INFINITY 1e30

/* Branch direction hints */
typedef enum {
    RALPH_BRANCH_AUTO = 0,   /* Solver chooses direction */
    RALPH_BRANCH_DOWN = -1,  /* Prefer branching down (x <= floor(val)) */
    RALPH_BRANCH_UP = 1      /* Prefer branching up (x >= ceil(val)) */
} RalphBranchDir;

/* MIP-start status for incumbent warm-start hints */
typedef enum {
    RALPH_MIP_START_NONE = 0,      /* No MIP start is staged */
    RALPH_MIP_START_PENDING = 1,   /* Start is staged and will be tried on next MIP solve */
    RALPH_MIP_START_ACCEPTED = 2,  /* Last attempted start was feasible and accepted */
    RALPH_MIP_START_REJECTED = 3   /* Last attempted start was infeasible or incompatible */
} RalphMIPStartStatus;

/* MIP-start repair policy */
typedef enum {
    RALPH_MIP_START_REPAIR_STRICT = 0,            /* Reject if out-of-bounds/non-integral */
    RALPH_MIP_START_REPAIR_PROJECT_BOUNDS = 1,    /* Clamp to [lb, ub] then validate */
    RALPH_MIP_START_REPAIR_PROJECT_AND_ROUND = 2  /* Clamp + round integer vars then validate */
} RalphMIPStartRepairMode;

/* Explicit LP basis status (row/column) for warm-start interoperability. */
typedef enum {
    RALPH_BASIS_STATUS_BASIC = 0,
    RALPH_BASIS_STATUS_AT_LOWER = 1,
    RALPH_BASIS_STATUS_AT_UPPER = 2,
    RALPH_BASIS_STATUS_FREE = 3,
    RALPH_BASIS_STATUS_FIXED = 4
} RalphBasisStatus;

/* Opaque model handle */
typedef struct RalphModel RalphModel;

/* Model creation and destruction */
RalphModel* ralph_core_create(void);
void ralph_core_free(RalphModel *model);

/* Model building */
int ralph_core_set_obj_sense(RalphModel *model, RalphObjSense sense);
int ralph_core_add_var(RalphModel *model, double lb, double ub, double obj, RalphVarType type);
int ralph_core_add_vars(RalphModel *model, int count, const double *lb, const double *ub,
                   const double *obj, const RalphVarType *types);
int ralph_core_add_constraint(RalphModel *model, int nnz, const int *indices,
                         const double *values, RalphSense sense, double rhs);

/* Model modification */
int ralph_core_set_var_bounds(RalphModel *model, int var, double lb, double ub);
int ralph_core_set_var_type(RalphModel *model, int var, RalphVarType type);
int ralph_core_set_obj_coef(RalphModel *model, int var, double coef);
int ralph_core_set_obj_offset(RalphModel *model, double offset);
double ralph_core_get_obj_offset(const RalphModel *model);

/* Model queries */
int ralph_core_get_num_vars(const RalphModel *model);
int ralph_core_get_num_cons(const RalphModel *model);
int ralph_core_get_num_integers(const RalphModel *model);
int ralph_core_is_mip(const RalphModel *model);

/* Solving */
int ralph_core_optimize(RalphModel *model);
int ralph_core_optimize_lp(RalphModel *model);
int ralph_core_optimize_mip(RalphModel *model);

/* Structured API error diagnostics (model-local with TLS fallback when model==NULL). */
int ralph_core_get_last_error(const RalphModel *model, RalphAPIError *out);
int ralph_core_clear_error(RalphModel *model);
const char* ralph_core_error_domain_string(RalphErrorDomain domain);
const char* ralph_core_error_code_string(RalphErrorCode code);
const char* ralph_core_error_api_string(RalphErrorAPIId api_id);
const char* ralph_core_error_message(const RalphAPIError *error);

/* Solution retrieval */
RalphStatus ralph_core_get_status(const RalphModel *model);
double ralph_core_get_objval(const RalphModel *model);
int ralph_core_get_solution(const RalphModel *model, double *x);
int ralph_core_get_dual_solution(const RalphModel *model, double *y);
int ralph_core_get_reduced_costs(const RalphModel *model, double *rc);

#ifndef RALPH_LP_EXT_REPORT_TYPES_DEFINED
typedef struct {
    int supports_primal_simplex;
    int supports_dual_simplex;
    int supports_barrier;
    int supports_crossover;
} RalphLPCapabilities;

typedef struct {
    RalphLPAlgorithm requested_algorithm;
    RalphLPAlgorithm effective_algorithm;
    RalphLPCrossoverMode requested_crossover;
    RalphLPCrossoverMode effective_crossover;
    int fallback_applied;
    RalphLPFallbackReason fallback_reason;
} RalphLPSolveAlgorithmReport;

typedef enum {
    RALPH_LP_EXTERNAL_BACKEND_SIMPLEX = 0,
    RALPH_LP_EXTERNAL_BACKEND_DUAL_SIMPLEX = 1,
    RALPH_LP_EXTERNAL_BACKEND_BARRIER = 2
} RalphLPExternalBackendKind;

/* Recommended external adapter solve return codes.
 * Contract:
 * - `RALPH_LP_EXTERNAL_ADAPTER_RC_OK` means success.
 * - Any non-zero value means solve failure; unmapped values are treated as generic failures.
 */
typedef enum {
    RALPH_LP_EXTERNAL_ADAPTER_RC_OK = 0,
    RALPH_LP_EXTERNAL_ADAPTER_RC_ERROR = -1,
    RALPH_LP_EXTERNAL_ADAPTER_RC_NUMERICAL_FAILURE = -2,
    RALPH_LP_EXTERNAL_ADAPTER_RC_TIME_LIMIT = -3,
    RALPH_LP_EXTERNAL_ADAPTER_RC_ITERATION_LIMIT = -4
} RalphLPExternalAdapterResult;

typedef enum {
    RALPH_LP_EXTERNAL_FAILURE_STAGE_NONE = 0,
    RALPH_LP_EXTERNAL_FAILURE_STAGE_DISPATCH = 1,
    RALPH_LP_EXTERNAL_FAILURE_STAGE_EXECUTION = 2
} RalphLPExternalFailureStage;

typedef enum {
    RALPH_LP_EXTERNAL_FAILURE_NONE = 0,
    RALPH_LP_EXTERNAL_FAILURE_PROVIDER_REQUIRED = 1,
    RALPH_LP_EXTERNAL_FAILURE_PROVIDER_UNREGISTERED = 2,
    RALPH_LP_EXTERNAL_FAILURE_BACKEND_UNSUPPORTED = 3,
    RALPH_LP_EXTERNAL_FAILURE_CAPABILITY_QUERY_FAILED = 4,
    RALPH_LP_EXTERNAL_FAILURE_ADAPTER_FAILED = 5,
    RALPH_LP_EXTERNAL_FAILURE_ADAPTER_NUMERICAL_FAILURE = 6,
    RALPH_LP_EXTERNAL_FAILURE_ADAPTER_TIME_LIMIT = 7,
    RALPH_LP_EXTERNAL_FAILURE_ADAPTER_ITERATION_LIMIT = 8
} RalphLPExternalFailureReason;

typedef struct {
    int supports_simplex;
    int supports_dual_simplex;
    int supports_barrier;
    int supports_crossover;
} RalphLPExternalCapabilities;

#define RALPH_LP_EXTERNAL_ADAPTER_ABI_VERSION 1

typedef struct {
    int abi_version;
    RalphLPExternalProvider provider;
    const char *provider_name;  /* Optional override; may be NULL. */
    int (*get_capabilities)(RalphLPExternalCapabilities *caps, void *user_data);
    int (*solve)(RalphLPExternalBackendKind backend, void *solver_handle, void *user_data);
    void *user_data;
} RalphLPExternalAdapter;

typedef struct {
    RalphLPExternalFailureStage stage;
    RalphLPExternalFailureReason reason;
    RalphLPAlgorithm requested_algorithm;
    RalphLPAlgorithm effective_algorithm;
    RalphLPExternalProvider requested_provider;
    RalphLPExternalProvider effective_provider;
    RalphLPExternalBackendKind backend;
    RalphLPFallbackReason fallback_reason;
    int adapter_return_code;
    RalphStatus mapped_status;
    int fatal;
} RalphLPExternalFailureReport;
#define RALPH_LP_EXT_REPORT_TYPES_DEFINED
#endif /* RALPH_LP_EXT_REPORT_TYPES_DEFINED */

/* LP capabilities are compile/runtime feature flags independent of model instance.
 * Returns 0 on success, -1 on invalid args. */
int ralph_core_get_lp_capabilities(RalphLPCapabilities *caps);

/* Last LP algorithm execution report (LP-only).
 * Contract:
 * - returns -1 for MIP models or if no LP solve has been run since invalidation.
 * - report describes requested vs effective algorithm/crossover and fallback reason.
 */
int ralph_core_get_last_lp_algorithm_report(const RalphModel *model,
                                       RalphLPSolveAlgorithmReport *report);

/* Last external LP failure report for the most recent LP solve.
 * Returns 0 when an external-dispatch or external-execution failure was recorded,
 * and -1 when unavailable (no failure report, invalid args, or MIP model). */
int ralph_core_get_last_lp_external_failure_report(const RalphModel *model,
                                              RalphLPExternalFailureReport *report);

/* External adapter registration/query API.
 * Contract:
 * - Multiple providers may be registered concurrently.
 * - External LP dispatch still requires explicit external algorithm request +
 *   matching lp_external_provider parameter.
 */
const char* ralph_core_get_lp_external_provider_name(RalphLPExternalProvider provider);
int ralph_core_get_lp_external_provider_capabilities(RalphLPExternalProvider provider,
                                                RalphLPExternalCapabilities *caps);
int ralph_core_get_lp_external_registered_providers(RalphLPExternalProvider *providers,
                                               int capacity,
                                               int *count);
int ralph_core_register_lp_external_adapter(const RalphLPExternalAdapter *adapter);
/* Register GLPK as an out-of-process adapter via `glpsol` (no in-process libglpk link).
 * Pass NULL to use `glpsol` from PATH, or an absolute/relative executable path. */
int ralph_core_register_lp_external_glpk_oop(const char *glpsol_path);
int ralph_core_unregister_lp_external_adapter(RalphLPExternalProvider provider);
int ralph_core_unregister_lp_external_glpk_oop(void);
void ralph_core_unregister_all_lp_external_adapters(void);
int ralph_core_is_lp_external_adapter_registered(RalphLPExternalProvider provider);

/* Infeasibility certificate (Farkas ray)
 * Returns 0 on success, -1 if not available (problem not infeasible or no certificate)
 * The ray y satisfies: y'A >= 0 and y'b < 0, proving infeasibility
 * Array must be pre-allocated with size >= ralph_core_get_num_cons(model) */
int ralph_core_get_farkas_ray(const RalphModel *model, double *ray);

/* Unboundedness certificate (primal ray)
 * Returns 0 on success, -1 if not available (problem not unbounded or no ray).
 * The ray d has size num_vars and points in an objective-improving direction
 * that preserves feasibility for sufficiently large steps. */
int ralph_core_get_unbounded_ray(const RalphModel *model, double *ray);

typedef enum {
    RALPH_CONFLICT_MEMBER_ROW = 0,
    RALPH_CONFLICT_MEMBER_VAR_LB = 1,
    RALPH_CONFLICT_MEMBER_VAR_UB = 2
} RalphConflictMemberType;

typedef struct {
    RalphConflictMemberType type;  /* row, variable lower bound, or variable upper bound */
    int index;                     /* constraint or variable index depending on type */
} RalphConflictMember;

typedef struct {
    int include_bounds;   /* 1 = include finite var bounds in conflict candidates */
    int use_farkas_seed;  /* 1 = prune row candidates using Farkas support before shrink */
} RalphConflictOptions;

typedef struct {
    int probes;            /* Number of LP probes executed */
    int dropped;           /* Candidates removed by deletion filter */
    int initial_size;      /* Candidate count before optional seed pruning */
    int final_size;        /* Final conflict size */
    int used_farkas_seed;  /* 1 if seed pruning was accepted */
    int seeded_rows;       /* Number of rows removed by accepted seed pruning */
} RalphConflictReport;

/* Compute a minimal LP conflict set over rows and optional variable bounds.
 *
 * Contract:
 * - LP-only API: returns -1 for MIP models.
 * - Requires model status == INFEASIBLE from the most recent solve.
 * - Set `options->include_bounds=0` for row-only IIS behavior.
 * - `members` may be NULL only when `capacity==0` (size query).
 * - `count` receives required/returned member count.
 *
 * Returns 0 on success, -1 on invalid args/unavailable/insufficient capacity.
 */
int ralph_core_compute_lp_conflict(const RalphModel *model,
                              const RalphConflictOptions *options,
                              RalphConflictMember *members,
                              int capacity,
                              int *count,
                              RalphConflictReport *report);

/* Compute a minimal irreducible infeasible subsystem (IIS) over LP rows.
 *
 * Contract:
 * - LP-only API: returns -1 for MIP models.
 * - Requires model status == INFEASIBLE from the most recent solve.
 * - row_flags must have size >= num_cons; row_flags[i]=1 iff row i is in IIS.
 *
 * This first version computes an irreducible row set by iterative deletion.
 *
 * @param model     The model
 * @param row_flags Output bitmap over constraints (size=num_cons)
 * @param iis_size  Output count of IIS rows (may be NULL)
 * @return 0 on success, -1 on error/unavailable
 */
int ralph_core_compute_lp_iis(const RalphModel *model, int *row_flags, int *iis_size);

/* Statistics */
int ralph_core_get_iterations(const RalphModel *model);

/* Presolve report (from the most recent solve call) */
typedef struct {
    int used;                   /* 1 if presolve executed, 0 otherwise */
    unsigned int mask;          /* Presolve mask used for the solve */
    int rounds;                 /* Presolve rounds executed */
    int vars_removed;           /* Variables removed by presolve */
    int cons_removed;           /* Constraints removed by presolve */
    int bounds_tightened;       /* Bound tightenings applied */
    int matrix_rank;            /* Computed matrix rank (0 if not computed) */
    int redundant_rows_found;   /* Redundant rows removed */
    double presolve_time_ms;    /* Wall-clock time spent in presolve */
} RalphPresolveReport;

/* Get presolve report from the most recent solve.
 * Returns 0 on success, -1 on invalid arguments. */
int ralph_core_get_last_presolve_report(const RalphModel *model, RalphPresolveReport *report);

/* LP solver telemetry snapshot (from the most recent solve call). */
typedef struct {
    double perf_primal_setup_ms;
    double perf_dual_ms;
    double perf_phase1_ms;
    double perf_transition_ms;
    double perf_phase2_ms;
    double perf_pricing_ms;
    double perf_ratio_ms;
    double perf_pivot_ms;
    double perf_refactor_ms;
    double perf_ftran_ms;
    double perf_btran_ms;
    double perf_ftran_base_ms;
    double perf_ftran_update_apply_ms;
    int perf_ftran_update_apply_calls;
    double perf_btran_base_ms;
    double perf_btran_update_apply_ms;
    int perf_btran_update_apply_calls;
    double perf_lu_update_ms;
    double perf_compute_solution_ms;
    double perf_compute_rc_ms;
    double perf_refactor_all_ms;
    int perf_refactor_count;
    double perf_refactor_last_ms;
    double perf_refactor_max_ms;
    int perf_refactor_last_reason;
    int perf_refactor_next_reason;
    int perf_refactor_reason_setup;
    int perf_refactor_reason_transition;
    int perf_refactor_reason_periodic;
    int perf_refactor_reason_ratio_recovery;
    int perf_refactor_reason_pivot_recovery;
    int perf_refactor_reason_forced_small_pivot;
    int perf_refactor_reason_update_recovery;
    int perf_refactor_reason_direction_stabilize;
    int perf_refactor_reason_infeas_cleanup;
    int perf_refactor_reason_other;
    int perf_refactor_periodic_policy;
    int perf_refactor_periodic_lu_health;
    int perf_refactor_safety_forced;
    int perf_basis_fastpath_hits;
    int perf_basis_cols_rewritten;
    unsigned long long perf_basis_tail_shift_bytes;
    int perf_refactor_last_m;
    int perf_refactor_last_k;
    int perf_refactor_last_nnz_B;

    double perf_phase1_pricing_ms;
    double perf_phase1_ratio_ms;
    double perf_phase1_pivot_ms;
    double perf_phase1_refactor_ms;
    double perf_phase1_compute_solution_ms;
    double perf_phase1_compute_rc_ms;
    int perf_phase1_pricing_calls;
    int perf_phase1_ratio_calls;
    int perf_phase1_pivot_calls;
    int perf_phase1_refactor_calls;
    int perf_phase1_compute_solution_calls;
    int perf_phase1_compute_rc_calls;
    int perf_phase1_compute_solution_ctx_other;
    int perf_phase1_compute_solution_ctx_recompute_full;
    int perf_phase1_compute_solution_ctx_recompute_guard_forced_full;
    int perf_phase1_compute_solution_ctx_init;
    int perf_phase1_compute_solution_ctx_no_entering_cleanup;
    int perf_phase1_compute_solution_ctx_infeas_cleanup;
    int perf_phase1_compute_solution_ctx_refactor_fail_continue;
    int perf_phase1_compute_solution_ctx_refactor_failure_recovery;
    int perf_phase1_compute_solution_ctx_refactor_success;
    int perf_phase1_compute_solution_ctx_drift_refresh;
    int perf_phase1_compute_solution_ctx_dual_rescue;
    int perf_phase1_compute_rc_ctx_other;
    int perf_phase1_compute_rc_ctx_recompute_full;
    int perf_phase1_compute_rc_ctx_recompute_rc_only;
    int perf_phase1_compute_rc_ctx_recompute_guard_forced_full;
    int perf_phase1_compute_rc_ctx_init;
    int perf_phase1_compute_rc_ctx_infeas_cleanup;
    int perf_phase1_compute_rc_ctx_refactor_fail_continue;
    int perf_phase1_compute_rc_ctx_refactor_failure_recovery;
    int perf_phase1_compute_rc_ctx_refactor_success;
    int perf_phase1_compute_rc_ctx_drift_refresh;
    int perf_phase1_compute_rc_ctx_dual_rescue;
    int perf_phase1_entering_exclusions;
    int perf_phase1_entering_exclusion_repeats;
    int perf_phase1_entering_exclusion_hits;
    int perf_phase1_entering_exclusion_reroutes;
    int perf_phase1_entering_exclusion_no_alt;
    int perf_phase1_refactor_periodic_policy;
    int perf_phase1_refactor_periodic_lu_health;
    int perf_phase1_refactor_safety_forced;
    int perf_phase1_dir_stabilize_force_extreme_dir;
    int perf_phase1_dir_stabilize_force_lu_health;
    int perf_phase1_dir_stabilize_cooldown_candidates;
    int perf_phase1_dir_stabilize_ratio_le_3;
    int perf_phase1_dir_stabilize_ratio_le_10;
    int perf_phase1_dir_stabilize_ratio_le_30;
    int perf_phase1_dir_stabilize_ratio_le_100;
    int perf_phase1_dir_stabilize_ratio_gt_100;
    int perf_phase1_dir_stabilize_ratio_gt_300;
    int perf_phase1_dir_stabilize_ratio_gt_1000;
    int perf_phase1_dir_stabilize_skip_rc_only;
    int perf_phase1_dir_stabilize_skip_full;
    int perf_phase1_dir_stabilize_skip_no_recompute;
    int perf_phase1_dir_stabilize_skip_guard_refresh;
    int perf_phase1_dir_stabilize_escape_gate_triggers;
    int perf_phase1_dir_stabilize_escape_gate_suppressed_lu_health;
    int perf_phase1_dir_stabilize_escape_gate_suppressed_force_pivot_mode;
    int perf_phase1_dir_stabilize_escape_gate_hard_bypass;
    int perf_phase1_dir_stabilize_refactor_from_no_pivot_force;
    int perf_phase1_dir_stabilize_refactor_from_force_extreme_dir;
    int perf_phase1_dir_stabilize_refactor_from_force_lu_health;
    int perf_phase1_dir_stabilize_refactor_from_force_pivot_mode;
    int perf_phase1_dir_stabilize_refactor_from_ladder_force;
    int perf_phase1_force_pivot_budget_dir_event_seen;
    int perf_phase1_force_pivot_budget_pivot_spend;
    int perf_phase1_force_pivot_relax_applied;
    int perf_phase1_force_extreme_relax_applied;
    int perf_phase1_force_extreme_tiny_theta_relax_applied;
    int perf_phase1_recompute_after_ratio_breakdown;
    int perf_phase1_recompute_after_dir_skip;
    int perf_phase1_recompute_after_dir_refactor;
    int perf_phase1_recompute_after_pivot_fail_recovery;
    int perf_phase1_recompute_after_perturb;
    int perf_phase1_recompute_rc_only_calls;
    int perf_phase1_recompute_rc_guard_forced_full;
    int perf_phase1_ratio_breakdown_retries;
    int perf_phase1_ratio_breakdown_escalations;
    int perf_phase1_pivot_fail_recovery_exclusions;
    int perf_phase1_no_pivot_events;
    int perf_phase1_no_pivot_forced_refactor;
    int perf_phase1_no_pivot_forced_ratio_breakdown;
    int perf_phase1_no_pivot_forced_dir_skip;
    int perf_phase1_no_pivot_forced_pivot_fail;
    int perf_phase1_no_pivot_events_ratio_breakdown;
    int perf_phase1_no_pivot_events_dir_skip;
    int perf_phase1_no_pivot_events_pivot_fail;
    int perf_phase1_no_pivot_no_progress_events;
    int perf_phase1_no_pivot_ladder_retry_defers;
    int perf_phase1_no_pivot_ladder_retry_ratio_breakdown;
    int perf_phase1_no_pivot_ladder_retry_dir_skip;
    int perf_phase1_no_pivot_ladder_retry_pivot_fail;
    int perf_phase1_no_pivot_ladder_dual_rescue_attempts;
    int perf_phase1_no_pivot_ladder_dual_rescue_successes;
    int perf_phase1_no_pivot_ladder_dual_rescue_failures;
    int perf_phase1_no_pivot_ladder_dual_rescue_attempts_ratio_breakdown;
    int perf_phase1_no_pivot_ladder_dual_rescue_attempts_dir_skip;
    int perf_phase1_no_pivot_ladder_dual_rescue_attempts_pivot_fail;
    int perf_phase1_no_pivot_ladder_forced_refactors;
    int perf_phase1_no_pivot_ladder_forced_refactors_ratio_breakdown;
    int perf_phase1_no_pivot_ladder_forced_refactors_dir_skip;
    int perf_phase1_no_pivot_ladder_forced_refactors_pivot_fail;
    int perf_phase1_no_pivot_ladder_rescue_guard_cooldown_blocks;
    int perf_phase1_no_pivot_ladder_rescue_guard_fail_cap_forces;
    int perf_phase1_direct_dual_rescue_attempts;
    int perf_phase1_direct_dual_rescue_successes;
    int perf_phase1_direct_dual_rescue_failures;
    int perf_phase1_direct_dual_rescue_guard_cooldown_blocks;
    int perf_phase1_direct_dual_rescue_guard_fail_cap_blocks;
    int perf_phase1_soft_lu_policy_cooldown_defers;
    int perf_phase1_dir_skip_same_entering_repeats;
    int perf_phase1_dir_skip_same_entering_max_streak;
    int perf_phase1_failed_stabilize_events;
    int perf_phase1_failed_stabilize_primary_failures;
    int perf_phase1_failed_stabilize_alternate_failures;
    int perf_phase1_failed_stabilize_same_entering_repeats;
    int perf_phase1_failed_stabilize_same_entering_max_streak;
    int perf_phase1_failed_stabilize_retry_penalty_arms;
    int perf_phase1_failed_stabilize_retry_penalty_alt_found;
    int perf_phase1_failed_stabilize_retry_penalty_no_alt;
    int perf_phase1_failed_stabilize_retry_penalty_alt_stabilized;
    int perf_phase1_failed_stabilize_retry_penalty_alt_failed;
    int perf_phase1_failed_stabilize_retry_penalty_same_alt_repeats;
    int perf_phase1_failed_stabilize_retry_penalty_same_alt_max_streak;
    int perf_phase1_failed_stabilize_retry_local_memory_arms;
    int perf_phase1_failed_stabilize_retry_local_memory_alt_found;
    int perf_phase1_failed_stabilize_retry_local_memory_no_alt;
    int perf_phase1_failed_stabilize_retry_local_memory_fallback_same_alt;
    int perf_phase1_failed_stabilize_retry_local_memory_alt_stabilized;
    int perf_phase1_failed_stabilize_retry_local_memory_alt_failed;
    int perf_phase1_failed_stabilize_retry_pool_samples;
    int perf_phase1_failed_stabilize_retry_pool_eligible_total;
    int perf_phase1_failed_stabilize_retry_pool_eligible_max;
    int perf_phase1_failed_stabilize_retry_pool_singleton_samples;
    int perf_phase1_failed_stabilize_retry_pool_best_differs_samples;
    int perf_phase1_failed_stabilize_retry_selector_eval_samples;
    int perf_phase1_failed_stabilize_retry_selector_eval_best_differs_samples;
    double perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_total;
    double perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_max;
    int perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_2;
    int perf_phase1_failed_stabilize_retry_selector_eval_score_ratio_ge_4;
    int perf_phase1_failed_stabilize_retry_shadow_samples;
    int perf_phase1_failed_stabilize_retry_shadow_ratio_failed;
    int perf_phase1_failed_stabilize_retry_shadow_dir_stable;
    int perf_phase1_failed_stabilize_retry_shadow_dir_failed;
    int perf_phase1_failed_stabilize_retry_shadow_dir_nnz_total;
    int perf_phase1_failed_stabilize_retry_shadow_dir_nnz_max;
    double perf_phase1_failed_stabilize_retry_shadow_dir_inf_total;
    double perf_phase1_failed_stabilize_retry_shadow_dir_inf_max;
    double perf_phase1_failed_stabilize_retry_shadow_pivot_abs_total;
    double perf_phase1_failed_stabilize_retry_shadow_pivot_abs_max;
    int perf_phase1_failed_stabilize_retry_shadow_guard_arms;
    int perf_phase1_failed_stabilize_retry_shadow_guard_original_exclusions;
    int perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_retry;
    int perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_dual_rescue;
    int perf_phase1_failed_stabilize_retry_shadow_post_dir_skip_forced_refactor;
    int perf_phase1_failed_stabilize_retry_shadow_next_failed_stabilize;
    int perf_phase1_failed_stabilize_retry_shadow_next_ratio_breakdown;
    int perf_phase1_failed_stabilize_retry_shadow_next_pivot_fail;
    int perf_phase1_failed_stabilize_retry_shadow_next_pivot_success;
    int perf_phase1_force_extreme_followup_stabilized;
    int perf_phase1_force_extreme_followup_ratio_breakdown;
    int perf_phase1_force_extreme_followup_failed_stabilize;
    int perf_phase1_force_extreme_followup_post_dir_skip_retry;
    int perf_phase1_force_extreme_followup_post_dir_skip_dual_rescue;
    int perf_phase1_force_extreme_followup_post_dir_skip_forced_refactor;
    int perf_phase1_force_extreme_followup_next_failed_stabilize;
    int perf_phase1_force_extreme_followup_next_ratio_breakdown;
    int perf_phase1_force_extreme_followup_next_pivot_fail;
    int perf_phase1_force_extreme_followup_next_pivot_success;
    int perf_phase1_force_extreme_followup_dir_samples;
    int perf_phase1_force_extreme_followup_dir_bound_geometry;
    int perf_phase1_force_extreme_followup_dir_bound_flip;
    int perf_phase1_force_extreme_followup_dir_tiny_theta;
    int perf_phase1_force_extreme_followup_dir_weak_leaving;
    int perf_phase1_force_extreme_followup_dir_ftran_shape;
    int perf_phase1_force_extreme_followup_dir_nnz_total;
    int perf_phase1_force_extreme_followup_dir_nnz_max;
    double perf_phase1_force_extreme_followup_dir_inf_total;
    double perf_phase1_force_extreme_followup_dir_inf_max;
    double perf_phase1_force_extreme_followup_pivot_abs_total;
    double perf_phase1_force_extreme_followup_pivot_abs_max;
    double perf_phase1_force_extreme_followup_theta_total;
    double perf_phase1_force_extreme_followup_theta_max;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_samples;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_bound_geometry;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_bound_flip;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_tiny_theta;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_weak_leaving;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_ftran_shape;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_nnz_total;
    int perf_phase1_failed_stabilize_retry_shadow_followup_dir_nnz_max;
    double perf_phase1_failed_stabilize_retry_shadow_followup_dir_inf_total;
    double perf_phase1_failed_stabilize_retry_shadow_followup_dir_inf_max;
    double perf_phase1_failed_stabilize_retry_shadow_followup_pivot_abs_total;
    double perf_phase1_failed_stabilize_retry_shadow_followup_pivot_abs_max;
    double perf_phase1_failed_stabilize_retry_shadow_followup_theta_total;
    double perf_phase1_failed_stabilize_retry_shadow_followup_theta_max;
    int perf_phase1_failed_stabilize_retry_selector_bland_arms;
    int perf_phase1_failed_stabilize_retry_selector_guarded_arms;
    int perf_phase1_failed_stabilize_retry_selector_guarded_eligible_total;
    int perf_phase1_failed_stabilize_retry_selector_guarded_eligible_max;
    int perf_phase1_failed_stabilize_retry_selector_bland_alt_stabilized;
    int perf_phase1_failed_stabilize_retry_selector_bland_alt_failed;
    int perf_phase1_failed_stabilize_retry_selector_bland_ratio_failed;
    int perf_phase1_failed_stabilize_retry_selector_bland_dir_failed;
    int perf_phase1_failed_stabilize_retry_selector_guarded_alt_stabilized;
    int perf_phase1_failed_stabilize_retry_selector_guarded_alt_failed;
    int perf_phase1_failed_stabilize_retry_selector_guarded_ratio_failed;
    int perf_phase1_failed_stabilize_retry_selector_guarded_dir_failed;
    int perf_phase1_failed_stabilize_retry_selector_guarded_fallback_to_bland;
    int perf_phase1_failed_stabilize_retry_dir_fail_shape_samples;
    int perf_phase1_failed_stabilize_retry_dir_fail_nnz_total;
    int perf_phase1_failed_stabilize_retry_dir_fail_nnz_max;
    double perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_total;
    double perf_phase1_failed_stabilize_retry_dir_fail_dir_inf_max;
    double perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_total;
    double perf_phase1_failed_stabilize_retry_dir_fail_pivot_abs_max;
    int perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_30;
    int perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_100;
    int perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_le_1000;
    int perf_phase1_failed_stabilize_retry_dir_fail_inf_ratio_gt_1000;
    int perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_8;
    int perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_6;
    int perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_le_1e_4;
    int perf_phase1_failed_stabilize_retry_dir_fail_pivot_ratio_gt_1e_4;
    int perf_phase1_failed_stabilize_retry_dir_second_chance_arms;
    int perf_phase1_failed_stabilize_retry_dir_second_chance_no_alt;
    int perf_phase1_failed_stabilize_retry_dir_second_chance_stabilized;
    int perf_phase1_failed_stabilize_retry_dir_second_chance_failed;
    int perf_phase1_failed_stabilize_retry_dir_guard_arms;
    int perf_phase1_failed_stabilize_retry_dir_guard_original_exclusions;
    int perf_phase1_window_pressure_windows_started;
    int perf_phase1_window_pressure_progress_resets;
    int perf_phase1_window_pressure_force_pivot_arms;
    int perf_phase1_window_pressure_force_pivot_blocked_pending;
    int perf_phase1_window_pressure_force_pivot_blocked_budget;
    int perf_phase1_window_pressure_force_pivot_reject_under_trigger;
    int perf_phase1_window_pressure_force_pivot_reject_failed_share;
    int perf_phase1_window_pressure_force_pivot_reject_dir_skip_share;
    int perf_phase1_window_pressure_force_pivot_reject_local_fail;
    int perf_phase1_window_pressure_force_pivot_reject_alternation;
    int perf_phase1_window_pressure_event_total;
    int perf_phase1_window_pressure_failed_stabilize_total;
    int perf_phase1_window_pressure_dir_skip_total;
    int perf_phase1_window_pressure_local_memory_fail_total;
    int perf_phase1_window_pressure_alternation_total;
    int perf_phase1_window_pressure_event_max;
    int perf_phase1_window_pressure_failed_stabilize_max;
    int perf_phase1_window_pressure_dir_skip_max;
    int perf_phase1_window_pressure_local_memory_fail_max;
    int perf_phase1_window_pressure_alternation_max;

    double perf_phase2_pricing_ms;
    double perf_phase2_ratio_ms;
    double perf_phase2_pivot_ms;
    double perf_phase2_refactor_ms;
    double perf_phase2_compute_solution_ms;
    double perf_phase2_compute_rc_ms;
    int perf_phase2_pricing_calls;
    int perf_phase2_ratio_calls;
    int perf_phase2_pivot_calls;
    int perf_phase2_refactor_calls;
    int perf_phase2_compute_solution_calls;
    int perf_phase2_compute_rc_calls;
    int perf_phase2_refactor_periodic_policy;
    int perf_phase2_refactor_periodic_lu_health;
    int perf_phase2_refactor_safety_forced;
    int perf_phase2_degenerate_episodes;
    int perf_phase2_degenerate_streak_max;
    int perf_phase2_theta_le_1e_9;
    int perf_phase2_theta_le_1e_6;
    int perf_phase2_theta_le_1e_3;
    int perf_phase2_theta_gt_1e_3;
    int perf_phase2_weak_pivot_samples;
    double perf_phase2_weak_pivot_ratio_total;
    double perf_phase2_weak_pivot_ratio_min;
    int perf_phase2_weak_pivot_ratio_le_1e_8;
    int perf_phase2_weak_pivot_ratio_le_1e_6;
    int perf_phase2_weak_pivot_ratio_le_1e_4;
    int perf_phase2_weak_pivot_ratio_gt_1e_4;
    int perf_phase2_repeat_entering_events;
    int perf_phase2_repeat_entering_max_streak;
    int perf_phase2_repeat_leaving_events;
    int perf_phase2_repeat_leaving_max_streak;
    int perf_phase2_bland_pricing_iters;
    int perf_phase2_adaptive_devex_partial_iters;
    int perf_phase2_bland_enter_episodes;
    int perf_phase2_bland_exit_episodes;
    int perf_phase2_perturb_applied;
    int perf_phase2_devex_reset_count;
    int perf_phase2_devex_age_max;
    int perf_phase2_degen_refactor_calls;
    int perf_phase2_degen_refactor_ratio_recovery;
    int perf_phase2_degen_refactor_pivot_recovery;
    int perf_phase2_degen_refactor_periodic_policy;
    int perf_phase2_degen_refactor_periodic_lu_health;
    int perf_phase2_degen_refactor_safety_forced;
    int perf_phase2_degen_escape_triggers;
    int perf_dual_ratio_no_entering;
    int perf_dual_theta_nonpositive;
    int perf_dual_pivot_reject_small;
    int perf_dual_bound_flip_applied;
    int perf_dual_bound_flip_startup;
    int perf_dual_bound_flip_iterative;
    int perf_dual_lu_hard_trigger;

    double periodic_feedback_bias_phase1;
    double periodic_feedback_bias_phase2;
    int periodic_feedback_last_reason_phase1;
    int periodic_feedback_last_reason_phase2;
    int periodic_feedback_last_interval_phase1;
    int periodic_feedback_last_interval_phase2;
    int periodic_feedback_hint_interval_phase1;
    int periodic_feedback_hint_interval_phase2;
    double periodic_feedback_hint_pressure_phase1;
    double periodic_feedback_hint_pressure_phase2;
    int soft_lu_cost_gate_enabled;
    int soft_lu_cost_gate_defers_phase1;
    int soft_lu_cost_gate_defers_phase2;
    int soft_lu_consecutive_defers_phase1;
    int soft_lu_consecutive_defers_phase2;
    int soft_lu_defer_cap_forced_phase1;
    int soft_lu_defer_cap_forced_phase2;
    int periodic_cost_gate_enabled;
    int periodic_cost_gate_defers_phase1;
    int periodic_cost_gate_defers_phase2;
    int periodic_cost_consecutive_defers_phase1;
    int periodic_cost_consecutive_defers_phase2;
    int periodic_cost_defer_cap_forced_phase1;
    int periodic_cost_defer_cap_forced_phase2;
    int periodic_cost_gate_checks_phase1;
    int periodic_cost_gate_checks_phase2;
    int periodic_cost_gate_block_small_m_phase1;
    int periodic_cost_gate_block_small_m_phase2;
    int periodic_cost_gate_block_invalid_inputs_phase1;
    int periodic_cost_gate_block_invalid_inputs_phase2;
    int periodic_cost_gate_block_warmup_phase1;
    int periodic_cost_gate_block_warmup_phase2;
    int periodic_cost_gate_block_invalid_cost_phase1;
    int periodic_cost_gate_block_invalid_cost_phase2;
    int periodic_cost_gate_block_ratio_phase1;
    int periodic_cost_gate_block_ratio_phase2;
    int periodic_cost_gate_block_update_reserve_phase1;
    int periodic_cost_gate_block_update_reserve_phase2;
    int periodic_cost_gate_last_reason_phase1;
    int periodic_cost_gate_last_reason_phase2;
    int periodic_cost_iter_samples_phase1;
    int periodic_cost_iter_samples_phase2;
    int periodic_cost_refactor_samples_phase1;
    int periodic_cost_refactor_samples_phase2;
    double soft_lu_refactor_cost_ewma_phase1;
    double soft_lu_refactor_cost_ewma_phase2;
    double soft_lu_iter_cost_ewma_phase1;
    double soft_lu_iter_cost_ewma_phase2;
    int basis_governor_mode;
    int shadow_refactor_yes_phase1;
    int shadow_refactor_yes_phase2;
    int shadow_refactor_yes_dual;
    int shadow_refactor_no_phase1;
    int shadow_refactor_no_phase2;
    int shadow_refactor_no_dual;
    int shadow_backend_pick_markowitz;
    int shadow_backend_pick_supernode;
    int shadow_backend_pick_dense;
    int shadow_disagree_primal_refactor;
    int shadow_disagree_dual_refactor;
    int shadow_disagree_lu_backend;
} RalphLPSolverTelemetry;

/* LU telemetry snapshot (from the most recent solve call). */
typedef struct {
    int mkz_enabled;
    int sn_enabled;
    int mkz_calls;
    int mkz_successes;
    int mkz_failures;
    int mkz_last_failure;
    int mkz_dense_fallbacks;
    int mkz_fail_workspace;
    int mkz_fail_pool;
    int mkz_fail_singular;
    int mkz_fail_capacity;
    int mkz_singular_retry_attempts;
    int mkz_singular_retry_successes;
    int mkz_singular_retry_failures;
    int mkz_reserved_fallback_attempts;
    int mkz_reserved_fallback_accepts;
    int mkz_reserved_fallback_rejects;
    int mkz_circuit_trips;
    int mkz_circuit_skips;
    int mkz_circuit_resets;
    uint64_t mkz_primary_scan_entries;
    uint64_t mkz_rescue_scan_entries;
    uint64_t mkz_reserved_scan_entries;
    uint64_t mkz_update_existing_entries;
    uint64_t mkz_update_fill_candidates;
    uint64_t mkz_hint_fallback_scans;
    uint64_t mkz_hint_fallback_scan_entries;
    uint64_t mkz_affected_columns_total;
    uint64_t mkz_affected_columns_max;
    uint64_t mkz_col_max_scan_entries;

    int sparse_dense_fallbacks;
    int used_dense_fallback_last;
    int sparse_fallback_last_reason;
    int sparse_fallback_reason_small_matrix;
    int sparse_fallback_reason_symbolic;
    int sparse_fallback_reason_numeric;
    int identity_sep_failures;
    int symbolic_failures;
    int symbolic_fail_workspace;
    int symbolic_fail_unmatched_no_reserved;
    int symbolic_fail_inconsistent_identity;
    int symbolic_full_retry_attempts;
    int symbolic_full_retry_successes;
    int symbolic_full_retry_numeric_failures;
    int symbolic_full_retry_mkz_attempts;
    int symbolic_full_retry_mkz_successes;
    int symbolic_full_retry_mkz_failures;
    int numeric_backend_markowitz;
    int numeric_backend_supernode;
    int numeric_backend_dense_ge;
    int backend_policy_luf_ft;
    int backend_policy_cbg;
    int backend_policy_cgr;
    int backend_policy_last;
    int update_path_ft;
    int update_path_eta;
    int update_path_bg_compat;
    int update_path_gr_compat;
    int identity_sep_retry_lane_dense_chosen;
    int identity_sep_retry_lane_supernode_chosen;
    int identity_sep_retry_lane_dense_successes;
    int identity_sep_retry_lane_supernode_successes;

    int sn_calls;
    int sn_successes;
    int num_updates;
    int max_updates;
    int last_failure_reason;

    int perf_factorize_calls;
    int perf_last_basis_nnz;
    int perf_last_m;
    int perf_last_k;
    int perf_symbolic_calls;
    int perf_symbolic_cache_hits;
    int perf_symbolic_cache_misses;
    double perf_last_symbolic_ms;
    double perf_last_sparse_numeric_ms;
    double perf_last_dense_ge_numeric_ms;
    double perf_last_supernode_numeric_ms;
    double perf_last_dense_factorize_ms;
    double perf_last_a_struct_build_ms;
    double perf_last_markowitz_numeric_ms;
    double perf_last_identity_placement_ms;
    double perf_last_coo_to_csc_ms;
    double perf_total_symbolic_ms;
    double perf_total_sparse_numeric_ms;
    double perf_total_dense_ge_numeric_ms;
    double perf_total_supernode_numeric_ms;
    double perf_total_dense_factorize_ms;
    double perf_total_a_struct_build_ms;
    double perf_total_markowitz_numeric_ms;
    double perf_total_identity_placement_ms;
    double perf_total_coo_to_csc_ms;
    int perf_update_apply_forward_calls;
    int perf_update_apply_backward_calls;
    int perf_compact_factor_calls;
    int perf_compact_solve_calls;
    double perf_total_update_apply_forward_ms;
    double perf_total_update_apply_backward_ms;
    double perf_total_compact_factor_ms;
    double perf_total_compact_solve_ms;
    uint64_t perf_sn_phase_samples;
    double perf_sn_panel_factor_ms;
    double perf_sn_panel_pivot_search_ms;
    double perf_sn_panel_swap_scatter_ms;
    double perf_sn_panel_eliminate_ms;
    double perf_sn_u_emit_ms;
    double perf_sn_active_set_ms;
    double perf_sn_pack_blocks_ms;
    double perf_sn_full_update_ms;
    double perf_sn_compact_update_ms;
    uint64_t perf_sn_active_row_scan_entries;
    uint64_t perf_sn_active_col_scan_entries;
    uint64_t perf_sn_trailing_rows_total;
    uint64_t perf_sn_trailing_cols_total;
    uint64_t perf_sn_active_rows_total;
    uint64_t perf_sn_active_cols_total;
    uint64_t perf_sn_pack_l_entries_total;
    uint64_t perf_sn_pack_u_entries_total;
    uint64_t perf_sn_dense_triplets_total;
    uint64_t perf_sn_compact_triplets_total;
    uint64_t perf_sn_full_update_calls;
    uint64_t perf_sn_compact_update_calls;
    uint64_t perf_sn_skipped_update_calls;
    uint64_t perf_sn_compact_cols1_calls;
    uint64_t perf_sn_compact_cols1_rows_total;
    double perf_sn_compact_cols1_ms;
    uint64_t perf_sn_compact_cols2_calls;
    uint64_t perf_sn_compact_cols2_rows_total;
    double perf_sn_compact_cols2_ms;
    uint64_t perf_sn_compact_cols3_calls;
    uint64_t perf_sn_compact_cols3_rows_total;
    double perf_sn_compact_cols3_ms;
    uint64_t perf_sn_compact_cols4_calls;
    uint64_t perf_sn_compact_cols4_rows_total;
    double perf_sn_compact_cols4_ms;
    uint64_t perf_sn_compact_cols5p_calls;
    uint64_t perf_sn_compact_cols5p_rows_total;
    double perf_sn_compact_cols5p_ms;
} RalphLUTelemetry;

/* Get LP solver telemetry snapshot from the most recent solve.
 * Returns 0 on success, -1 on invalid arguments. */
int ralph_core_get_last_lp_telemetry(const RalphModel *model, RalphLPSolverTelemetry *telemetry);

/* Get LU telemetry snapshot from the most recent solve.
 * Returns 0 on success, -1 on invalid arguments. */
int ralph_core_get_last_lu_telemetry(const RalphModel *model, RalphLUTelemetry *telemetry);

/* Solution-quality snapshot (KKT/verification metrics).
 *
 * Availability contract:
 * - available=1 only when LP verification was enabled for the solve and the
 *   resulting status is OPTIMAL, IMPRECISE, or OBJ_LIMIT.
 * - available=0 otherwise (metrics are left as 0).
 */
typedef struct {
    int available;              /* 1 if metrics are available for this solve */
    int verify_enabled;         /* 1 if LP verification was enabled in solver path */
    RalphStatus status;         /* Solve status corresponding to this snapshot */
    double primal_infeas;       /* ||Ax - b||_inf feasibility violation */
    double bound_infeas;        /* Max bound violation */
    double dual_infeas;         /* Max dual-feasibility violation */
    double comp_slack;          /* Max complementary slackness residual */
    double obj_error;           /* Relative objective recomputation error */
    double cond_estimate;       /* Basis condition estimate from LU */
} RalphSolutionQuality;

/* Get solution-quality metrics from the most recent solve.
 * Returns 0 on success, -1 on invalid arguments. */
int ralph_core_get_solution_quality(const RalphModel *model, RalphSolutionQuality *quality);

/* Fixed-basis sensitivity/ranging interval. */
typedef struct {
    double current;  /* Current coefficient/RHS value */
    double lower;    /* Smallest value preserving current basis optimality/feasibility */
    double upper;    /* Largest value preserving current basis optimality/feasibility */
} RalphSensitivityRange;

/* Fixed-basis bound ranging intervals for one variable. */
typedef struct {
    double lower_current;  /* Current lower bound */
    double lower_min;      /* Minimum lower bound preserving current basis */
    double lower_max;      /* Maximum lower bound preserving current basis */
    double upper_current;  /* Current upper bound */
    double upper_min;      /* Minimum upper bound preserving current basis */
    double upper_max;      /* Maximum upper bound preserving current basis */
} RalphBoundSensitivityRange;

/* LP fixed-basis sensitivity APIs.
 *
 * Contract:
 * - LP-only (returns -1 for MIP models).
 * - Requires most recent LP solve status == OPTIMAL.
 * - Requires a live LP tableau matching original model dimensions
 *   (v1 returns -1 when presolve reduced dimensions).
 * - Ranges are fixed-basis intervals in current simplex basis.
 *
 * Returns 0 on success, -1 on invalid args/unavailable state.
 */
int ralph_core_get_constraint_rhs_range(const RalphModel *model,
                                   int constraint,
                                   RalphSensitivityRange *range);
int ralph_core_get_obj_coef_range(const RalphModel *model,
                             int var,
                             RalphSensitivityRange *range);
int ralph_core_get_var_bound_range(const RalphModel *model,
                              int var,
                              RalphBoundSensitivityRange *range);

/* LP progress callback phase code. */
typedef enum {
    RALPH_LP_PROGRESS_PHASE_DUAL = 0,
    RALPH_LP_PROGRESS_PHASE_1 = 1,
    RALPH_LP_PROGRESS_PHASE_2 = 2
} RalphLPProgressPhase;

/* LP progress snapshot emitted during simplex iterations.
 *
 * Notes:
 * - `objective` is the current tableau objective mapped to user-space sense.
 * - `quality_*` fields are populated only when `quality_available=1`.
 */
typedef struct {
    RalphLPProgressPhase phase;  /* Current LP phase (dual/phase1/phase2) */
    int iteration;               /* Current LP iteration index */
    double elapsed_time_sec;     /* Elapsed solve time in seconds */
    RalphStatus status;          /* Current solver status */
    double objective;            /* Current objective estimate (user space) */
    int quality_available;       /* 1 if quality metrics are available */
    double primal_infeas;        /* Verification primal infeasibility */
    double bound_infeas;         /* Verification bound infeasibility */
    double dual_infeas;          /* Verification dual infeasibility */
    double comp_slack;           /* Verification complementary slackness */
    double obj_error;            /* Verification relative objective error */
    double cond_estimate;        /* Basis condition estimate */
} RalphLPProgressInfo;

/* LP progress callback configuration.
 *
 * Return non-zero from `on_progress` to request early termination. This maps to
 * `RALPH_STATUS_TIME_LIMIT` and causes solve to stop safely.
 */
typedef struct {
    int (*on_progress)(void *user_data, const RalphLPProgressInfo *info);
    void *user_data;
    int every_n_iterations;      /* Callback cadence; <=0 defaults to 1 */
} RalphLPProgressCallback;

/* LP cancellation poll callback.
 *
 * If `should_cancel` returns non-zero, solve terminates early with
 * `RALPH_STATUS_TIME_LIMIT`.
 */
typedef struct {
    int (*should_cancel)(void *user_data);
    void *user_data;
} RalphLPCancelCallback;

/* Set LP progress callback (LP solve path only). Pass NULL to disable. */
void ralph_core_set_lp_progress_callback(RalphModel *model,
                                    const RalphLPProgressCallback *callback);

/* Set LP cancellation poll callback (LP solve path only). Pass NULL to disable. */
void ralph_core_set_lp_cancel_callback(RalphModel *model,
                                  const RalphLPCancelCallback *callback);

/* MIP-specific */
double ralph_core_get_best_bound(const RalphModel *model);
double ralph_core_get_mip_gap(const RalphModel *model);
int ralph_core_get_node_count(const RalphModel *model);

/* Branching control (MIP)
 *
 * These functions control variable selection during branch and bound.
 * Must be called before ralph_core_optimize(). Arrays are copied internally.
 */

/* Set branching priorities for integer variables.
 * Higher priority variables are branched on first.
 * @param model      The model
 * @param priorities Array of priorities (size = num_vars). NULL to clear.
 * @return 0 on success, -1 on error
 */
int ralph_core_set_branch_priorities(RalphModel *model, const int *priorities);

/* Set preferred branch direction for integer variables.
 * @param model      The model
 * @param directions Array of RalphBranchDir values (size = num_vars). NULL to clear.
 * @return 0 on success, -1 on error
 */
int ralph_core_set_branch_directions(RalphModel *model, const int *directions);

/* MIP incumbent warm start (MIP start).
 *
 * A staged MIP start is checked for bounds, integrality, and constraint
 * feasibility at solve time. If valid, it seeds the incumbent and can improve
 * pruning without affecting correctness.
 */

/* Set full-length MIP start vector (size = num_vars).
 * The vector is copied internally and remains active across solves until cleared.
 * @param model The model
 * @param x     Candidate incumbent values in original variable space
 * @return 0 on success, -1 on error
 */
int ralph_core_set_mip_start(RalphModel *model, const double *x);

/* Set sparse/partial MIP start entries.
 *
 * If no start is staged yet, unspecified variables are initialized to their
 * lower bounds. Subsequent sparse calls update only listed variables.
 *
 * @param model   The model
 * @param count   Number of sparse entries
 * @param indices Variable indices (size=count)
 * @param values  Entry values (size=count)
 * @return 0 on success, -1 on error
 */
int ralph_core_set_mip_start_sparse(RalphModel *model, int count,
                               const int *indices, const double *values);

/* Clear staged MIP start.
 * @param model The model
 */
void ralph_core_clear_mip_start(RalphModel *model);

/* Get staged/last-attempted MIP-start status.
 * @param model The model
 * @return RalphMIPStartStatus
 */
RalphMIPStartStatus ralph_core_get_mip_start_status(const RalphModel *model);

/* Set/get MIP-start repair mode. */
int ralph_core_set_mip_start_repair_mode(RalphModel *model, RalphMIPStartRepairMode mode);
RalphMIPStartRepairMode ralph_core_get_mip_start_repair_mode(const RalphModel *model);

/* Constraint modification (for Benders decomposition, cut loops)
 *
 * These functions allow modifying the model between solves.
 * After modification, call ralph_core_optimize() to re-solve.
 *
 * Modification semantics:
 * - Current solve status/solution is invalidated immediately
 * - Existing LP/MIP solver state is discarded
 * - A subsequent ralph_core_optimize() rebuilds solver state from the modified model
 */

/* Modify RHS of existing constraint.
 * @param model      The model
 * @param constraint Constraint index (0 to num_cons-1)
 * @param rhs        New right-hand side value
 * @return 0 on success, -1 on error (invalid constraint index)
 */
int ralph_core_set_constraint_rhs(RalphModel *model, int constraint, double rhs);

/* Modify one matrix coefficient A[constraint, var].
 * Setting coef to 0 removes the entry from the sparse matrix pattern.
 *
 * @param model      The model
 * @param constraint Constraint index (0 to num_cons-1)
 * @param var        Variable index (0 to num_vars-1)
 * @param coef       New coefficient value
 * @return 0 on success, -1 on error
 */
int ralph_core_set_constraint_coef(RalphModel *model, int constraint, int var, double coef);

/* Modify multiple matrix coefficients in one call.
 * Each update sets A[constraints[i], vars[i]] = coefs[i].
 * Setting coefs[i] to 0 removes that entry.
 *
 * @param model       The model
 * @param count       Number of updates
 * @param constraints Constraint indices (size=count)
 * @param vars        Variable indices (size=count)
 * @param coefs       Coefficients (size=count)
 * @return 0 on success, -1 on error
 */
int ralph_core_set_constraint_coefs(RalphModel *model, int count,
                               const int *constraints, const int *vars,
                               const double *coefs);

/* Modify constraint sense of an existing row.
 * @param model      The model
 * @param constraint Constraint index (0 to num_cons-1)
 * @param sense      New constraint sense (L/E/G)
 * @return 0 on success, -1 on error
 */
int ralph_core_set_constraint_sense(RalphModel *model, int constraint, RalphSense sense);

/* Batch modify RHS values (all-or-nothing validation).
 * Applies rhs_values[i] to constraints[i] when all indices are valid.
 *
 * @param model       The model
 * @param count       Number of updates
 * @param constraints Constraint indices (size=count)
 * @param rhs_values  RHS values (size=count)
 * @return 0 on success, -1 on error
 */
int ralph_core_set_constraint_rhs_batch(RalphModel *model, int count,
                                   const int *constraints, const double *rhs_values);

/* Batch modify constraint senses (all-or-nothing validation).
 * Applies senses[i] to constraints[i] when all indices/senses are valid.
 *
 * @param model       The model
 * @param count       Number of updates
 * @param constraints Constraint indices (size=count)
 * @param senses      New row senses (size=count)
 * @return 0 on success, -1 on error
 */
int ralph_core_set_constraint_sense_batch(RalphModel *model, int count,
                                     const int *constraints, const RalphSense *senses);

/* Query RHS of a constraint row.
 * @param model      The model
 * @param constraint Constraint index (0 to num_cons-1)
 * @param rhs        Output RHS (non-NULL)
 * @return 0 on success, -1 on error
 */
int ralph_core_get_constraint_rhs(const RalphModel *model, int constraint, double *rhs);

/* Query sense of a constraint row.
 * @param model      The model
 * @param constraint Constraint index (0 to num_cons-1)
 * @param sense      Output row sense (non-NULL)
 * @return 0 on success, -1 on error
 */
int ralph_core_get_constraint_sense(const RalphModel *model, int constraint, RalphSense *sense);

/* Query matrix coefficient A[constraint, var].
 * Returns 0 for structurally absent coefficients.
 *
 * @param model      The model
 * @param constraint Constraint index (0 to num_cons-1)
 * @param var        Variable index (0 to num_vars-1)
 * @param coef       Output coefficient value (non-NULL)
 * @return 0 on success, -1 on error
 */
int ralph_core_get_constraint_coef(const RalphModel *model, int constraint, int var, double *coef);

/* Delete one constraint row.
 *
 * Invalidation semantics:
 * - Current solve status/solution is invalidated.
 * - Existing LP/MIP solver state is discarded.
 * - MIP starts are kept only when still dimension-compatible.
 *
 * @param model      The model
 * @param constraint Constraint index (0 to num_cons-1)
 * @return 0 on success, -1 on error
 */
int ralph_core_delete_constraint(RalphModel *model, int constraint);

/* Delete one structural variable column.
 *
 * Invalidation semantics:
 * - Current solve status/solution is invalidated.
 * - Existing LP/MIP solver state is discarded.
 * - MIP starts are dropped when dimension no longer matches.
 *
 * @param model The model
 * @param var   Variable index (0 to num_vars-1)
 * @return 0 on success, -1 on error
 */
int ralph_core_delete_var(RalphModel *model, int var);

/* Query variable bounds.
 * @param model The model
 * @param var   Variable index (0 to num_vars-1)
 * @param lb    Output: lower bound (may be NULL if not needed)
 * @param ub    Output: upper bound (may be NULL if not needed)
 * @return 0 on success, -1 on error (invalid variable index)
 */
int ralph_core_get_var_bounds(const RalphModel *model, int var, double *lb, double *ub);

/* Cut representation for lazy constraints and callbacks */
typedef struct {
    const int *indices;     /* Variable indices */
    const double *coeffs;   /* Coefficients */
    int num_vars;           /* Number of variables in cut */
    RalphSense sense;       /* 'L' (<=), 'G' (>=), 'E' (=) */
    double rhs;             /* Right-hand side */
} RalphCut;

/* Add a lazy constraint to the model.
 * User controls the cut loop externally. After adding constraints, call
 * ralph_core_optimize() to re-solve. The solver will attempt warm start.
 *
 * @param model The model
 * @param cut   The cut to add
 * @return 0 on success, -1 on error
 */
int ralph_core_add_lazy_constraint(RalphModel *model, const RalphCut *cut);

/* Add multiple lazy constraints to the model.
 * @param model The model
 * @param cuts  Array of cuts to add
 * @param count Number of cuts
 * @return 0 on success, -1 on error
 */
int ralph_core_add_lazy_constraints(RalphModel *model, const RalphCut *cuts, int count);

/* Warm start support (basis save/restore)
 *
 * Save and restore LP basis between solves for warm start. Particularly
 * useful for Benders decomposition where the subproblem changes slightly
 * between iterations.
 */

/* Opaque basis handle */
typedef struct RalphBasis RalphBasis;

/* Save the current LP basis.
 * @param model The model (must have been solved)
 * @return Basis handle, or NULL on error. Caller must free with ralph_core_free_basis().
 */
RalphBasis* ralph_core_save_basis(const RalphModel *model);

/* Load a previously saved basis for warm start.
 * @param model The model
 * @param basis The basis to load
 *
 * Behavior:
 * - If an LP solver/tableau already exists, applies immediately.
 * - If called before the first ralph_core_optimize() (or after solver invalidation),
 *   the basis is staged and applied on the next LP optimize() call.
 * - Basis must be dimension-compatible with the model/solver state.
 *
 * @return 0 on success, -1 on error (e.g., dimensions mismatch)
 */
int ralph_core_load_basis(RalphModel *model, const RalphBasis *basis);

/* Query explicit LP basis statuses for structural columns and logical rows.
 *
 * Contract:
 * - Requires a live LP tableau (model must have been LP-optimized successfully).
 * - `col_status` has size >= num_vars when non-NULL.
 * - `row_status` has size >= num_cons when non-NULL.
 * - At least one of `col_status` or `row_status` must be non-NULL.
 *
 * @return 0 on success, -1 on error/unavailable.
 */
int ralph_core_get_basis_status(const RalphModel *model,
                           RalphBasisStatus *col_status,
                           RalphBasisStatus *row_status);

/* Apply explicit LP basis statuses for structural columns and logical rows.
 *
 * Contract:
 * - Requires a live LP tableau (model must have been LP-optimized successfully).
 * - `col_status` has size >= num_vars when non-NULL.
 * - `row_status` has size >= num_cons when non-NULL.
 * - At least one of `col_status` or `row_status` must be non-NULL.
 * - Statuses must define a valid basis; invalid/singular combinations return -1.
 *
 * @return 0 on success, -1 on error.
 */
int ralph_core_set_basis_status(RalphModel *model,
                           const RalphBasisStatus *col_status,
                           const RalphBasisStatus *row_status);

/* Free a saved basis.
 * @param basis The basis to free (may be NULL)
 */
void ralph_core_free_basis(RalphBasis *basis);

/* Serialize/deserialize warm-start artifacts for checkpoint/restart. */
int ralph_core_write_basis_file(const RalphBasis *basis, const char *filename);
RalphBasis* ralph_core_read_basis_file(const char *filename);
int ralph_core_write_mip_start_file(const RalphModel *model, const char *filename);
int ralph_core_read_mip_start_file(RalphModel *model, const char *filename);

/* Cut callback (for automatic cut generation during MIP solving)
 *
 * The callback is invoked at each B&B node after the LP relaxation is solved.
 * The user can generate domain-specific cuts based on the fractional solution.
 */
typedef struct {
    /*
     * Called at each B&B node after LP relaxation solved.
     * @param user_data   User-provided context pointer
     * @param x_relaxation Current LP solution (may be fractional)
     * @param num_vars    Number of variables
     * @param cuts        Output array for generated cuts
     * @param max_cuts    Maximum number of cuts to generate
     * @return Number of cuts added (0 = no cuts found), or -1 on error
     */
    int (*generate_cuts)(
        void *user_data,
        const double *x_relaxation,
        int num_vars,
        RalphCut *cuts,
        int max_cuts
    );
    void *user_data;    /* User-provided context (passed to generate_cuts) */
} RalphCutCallback;

/* Set cut callback for automatic cut generation during MIP solving.
 * @param model    The model
 * @param callback The callback (NULL to disable)
 */
void ralph_core_set_cut_callback(RalphModel *model, const RalphCutCallback *callback);

/* Branching callback (for custom variable selection during MIP solving)
 *
 * The callback is invoked when the MIP solver needs to select a branching
 * variable. The user can examine the fractional LP solution and choose
 * which variable to branch on, or return -1 to use the default strategy.
 */
typedef struct {
    /*
     * Called when MIP solver needs to select a branching variable.
     * @param user_data    User-provided context pointer
     * @param x_relaxation Current LP solution (may have fractional integer vars)
     * @param num_vars     Number of variables
     * @param is_integer   Boolean array: is_integer[j]=1 if var j is integer
     * @param lb           Current lower bounds
     * @param ub           Current upper bounds
     * @return Variable index to branch on (0 to num_vars-1), or -1 to use default
     */
    int (*select_branch_var)(
        void *user_data,
        const double *x_relaxation,
        int num_vars,
        const int *is_integer,
        const double *lb,
        const double *ub
    );
    void *user_data;    /* User-provided context (passed to select_branch_var) */
} RalphBranchCallback;

/* Set branching callback for custom variable selection during MIP solving.
 * @param model    The model
 * @param callback The callback (NULL to disable, uses default strategy)
 */
void ralph_core_set_branch_callback(RalphModel *model, const RalphBranchCallback *callback);

/* ============================================================================
 * Benders Decomposition
 * ============================================================================
 *
 * Generic Benders decomposition solver for problems of the form:
 *   min c'x + d'y
 *   s.t. Ax = b               (master constraints)
 *        Tx + Wy = h          (linking constraints)
 *        x ∈ X (integer)      (complicating variables)
 *        y ≥ 0                (continuous recourse)
 *
 * User specifies which variables are "complicating" (go to master problem).
 * Ralph automatically:
 *   1. Partitions model into master (MIP) + subproblem (LP)
 *   2. Detects linking constraints
 *   3. Generates optimality cuts from subproblem duals
 *   4. Generates feasibility cuts from Farkas rays
 *   5. Iterates until convergence
 */

/* Benders configuration */
typedef struct {
    /* Which variables belong to master problem (complicating variables) */
    const int *master_var_indices;
    int num_master_vars;

    /* The θ variable representing subproblem cost (-1 to auto-create) */
    int theta_var;

    /* Stochastic Benders: multiple scenarios (deterministic = 1) */
    int num_scenarios;
    const double *scenario_probs;   /* NULL = equal weights */

    /* Algorithm parameters */
    double gap_tolerance;           /* Convergence gap (default 1e-6) */
    int max_iterations;             /* Iteration limit (default 1000) */
    int cuts_at_lp_nodes;           /* 1 = modern branch-and-Benders-cut */
    int warm_start_master;          /* 1 = seed master MIP from previous master incumbent */
    int warm_start_subproblems;     /* 1 = reuse subproblem basis/tableau, 0 = cold-start each sub solve */
    const double *initial_master_solution; /* Optional initial master start in original variable space (size=num_vars) */
    int strict_farkas;              /* 1 = require strict Farkas validation */
    int verbose;                    /* Verbosity level (0-2) */

    /* Branching hints (original variable space, applied to master MIP) */
    const int *branch_priorities;   /* NULL = no priorities */
    const int *branch_directions;   /* NULL = no directions */
} RalphBendersConfig;

/* Default configuration */
#define RALPH_BENDERS_CONFIG_DEFAULT { \
    .master_var_indices = NULL,        \
    .num_master_vars = 0,              \
    .theta_var = -1,                   \
    .num_scenarios = 1,                \
    .scenario_probs = NULL,            \
    .gap_tolerance = 1e-6,             \
    .max_iterations = 1000,            \
    .cuts_at_lp_nodes = 1,             \
    .warm_start_master = 1,            \
    .warm_start_subproblems = 1,       \
    .initial_master_solution = NULL,   \
    .strict_farkas = 0,                \
    .verbose = 0,                      \
    .branch_priorities = NULL,         \
    .branch_directions = NULL          \
}

/* Benders result information */
typedef struct {
    RalphStatus status;         /* Solution status */
    double objective;           /* Optimal objective value */
    double master_obj;          /* Master objective (c'x + θ) */
    double subproblem_obj;      /* Subproblem objective (d'y) */
    double gap;                 /* Final optimality gap */
    int iterations;             /* Number of Benders iterations */
    int optimality_cuts;        /* Number of optimality cuts added */
    int feasibility_cuts;       /* Number of feasibility cuts added */
    int nodes_explored;         /* B&B nodes (if cuts_at_lp_nodes) */
    int subproblems_solved;     /* Total subproblem solves across all scenarios */
    int master_warm_starts_attempted; /* Master MIP start attempts */
    int master_warm_starts_accepted;  /* Master MIP starts accepted */
    int subproblem_warm_starts; /* Subproblem solves that reused an existing tableau */
    int subproblem_cold_starts; /* Subproblem solves that started from a fresh tableau */
    double solve_time;          /* Total solve time in seconds */
} RalphBendersResult;

/*
 * Solve a model using Benders decomposition.
 *
 * @param model   The full model containing all variables and constraints
 * @param config  Benders configuration specifying master variables
 * @param x       Output: optimal solution (size = num_vars). May be NULL.
 * @param result  Output: detailed result info. May be NULL.
 * @return 0 on success, -1 on error
 *
 * The model is partitioned into:
 *   - Master: variables in master_var_indices + theta variable
 *   - Subproblem: all other variables
 *
 * Constraints are classified as:
 *   - Master-only: only involve master variables → go to master
 *   - Subproblem-only: only involve subproblem variables → go to subproblem
 *   - Linking: involve both → RHS depends on master solution
 */
int ralph_core_solve_benders(
    RalphModel *model,
    const RalphBendersConfig *config,
    double *x,
    RalphBendersResult *result
);

/* Typed parameter metadata/introspection */
typedef enum {
    RALPH_PARAM_SCOPE_SHARED = 0,
    RALPH_PARAM_SCOPE_LP = 1,
    RALPH_PARAM_SCOPE_MIP = 2
} RalphParamScope;

typedef enum {
    RALPH_PARAM_VALUE_INT = 0,
    RALPH_PARAM_VALUE_DOUBLE = 1
} RalphParamValueType;

typedef enum {
    RALPH_LP_POLICY_PROFILE_DEFAULT = 0,
    RALPH_LP_POLICY_PROFILE_GLPK_COMPAT = 1,
    RALPH_LP_POLICY_PROFILE_GLPK_STRICT = 2,
    RALPH_LP_POLICY_PROFILE_GLPK_LEGACY = 3
} RalphLPPolicyProfile;

typedef enum {
    RALPH_LP_REINVERT_CONTROLLER_MODE_OFF = 0,
    RALPH_LP_REINVERT_CONTROLLER_MODE_SHADOW = 1,
    RALPH_LP_REINVERT_CONTROLLER_MODE_CONTROL_PHASE1 = 2,
    RALPH_LP_REINVERT_CONTROLLER_MODE_CONTROL_ALL = 3
} RalphLPReinvertControllerMode;

typedef enum {
    RALPH_LP_GLPK_SMCP_METHOD_AUTO = 0,
    RALPH_LP_GLPK_SMCP_METHOD_PRIMAL = 1,
    RALPH_LP_GLPK_SMCP_METHOD_DUALP = 2,
    RALPH_LP_GLPK_SMCP_METHOD_DUAL = 3
} RalphLPGLPKSMCPMethod;

typedef enum {
    RALPH_LP_GLPK_SMCP_PRICING_STANDARD = 0,
    RALPH_LP_GLPK_SMCP_PRICING_STEEP = 1
} RalphLPGLPKSMCPPricing;

typedef enum {
    RALPH_LP_GLPK_SMCP_RATIO_STANDARD = 0,
    RALPH_LP_GLPK_SMCP_RATIO_HARRIS = 1
} RalphLPGLPKSMCPRatio;

typedef enum {
    RALPH_LP_GLPK_SMCP_FLIP_OFF = 0,
    RALPH_LP_GLPK_SMCP_FLIP_ON = 1
} RalphLPGLPKSMCPFlip;

typedef enum {
    RALPH_LP_GLPK_SMCP_BASIS_ADV = 0,
    RALPH_LP_GLPK_SMCP_BASIS_STD = 1,
    RALPH_LP_GLPK_SMCP_BASIS_BIB = 2,
    RALPH_LP_GLPK_SMCP_BASIS_INI = 3
} RalphLPGLPKSMCPBasis;

typedef enum {
    RALPH_LP_GLPK_SMCP_PRESOLVE_AUTO = 0,
    RALPH_LP_GLPK_SMCP_PRESOLVE_OFF = 1,
    RALPH_LP_GLPK_SMCP_PRESOLVE_ON = 2
} RalphLPGLPKSMCPPresolve;

typedef enum {
    RALPH_LP_GLPK_SMCP_EXCL_OFF = 0,
    RALPH_LP_GLPK_SMCP_EXCL_ON = 1
} RalphLPGLPKSMCPExcl;

typedef enum {
    RALPH_LP_GLPK_SMCP_SHIFT_OFF = 0,
    RALPH_LP_GLPK_SMCP_SHIFT_ON = 1
} RalphLPGLPKSMCPShift;

typedef enum {
    RALPH_LP_GLPK_SMCP_AORN_USE_AT = 1,
    RALPH_LP_GLPK_SMCP_AORN_USE_NT = 2
} RalphLPGLPKSMCPAorn;

typedef enum {
    RALPH_LP_GLPK_BFCP_FACTORIZATION_LUF = 0,
    RALPH_LP_GLPK_BFCP_FACTORIZATION_BTF = 1
} RalphLPGLPKBFCPFactorization;

typedef enum {
    RALPH_LP_GLPK_BFCP_BACKEND_LUF_FT = 0,
    RALPH_LP_GLPK_BFCP_BACKEND_CBG = 1,
    RALPH_LP_GLPK_BFCP_BACKEND_CGR = 2
} RalphLPGLPKBFCPBackend;

typedef enum {
    RALPH_LP_GLPK_BFCP_SUHL_AUTO = -1,
    RALPH_LP_GLPK_BFCP_SUHL_OFF = 0,
    RALPH_LP_GLPK_BFCP_SUHL_ON = 1
} RalphLPGLPKBFCPSuhl;

typedef enum {
    RALPH_PARAM_MAX_ITERATIONS = 0,
    RALPH_PARAM_PRESOLVE,
    RALPH_PARAM_VERBOSE,
    RALPH_PARAM_TELEMETRY,
    RALPH_PARAM_MAX_NODES,
    RALPH_PARAM_MAX_CUT_ROUNDS,
    RALPH_PARAM_METHOD,
    RALPH_PARAM_PRICING,
    RALPH_PARAM_DETECT_SPECIAL,
    RALPH_PARAM_NODE_POOL_CAPACITY,
    RALPH_PARAM_NODE_SELECT,
    RALPH_PARAM_FORCE_TWO_PHASE,
    RALPH_PARAM_TRACE_PHASE1,
    RALPH_PARAM_PRESOLVE_MASK,
    RALPH_PARAM_DUAL_BOUND_FLIP,
    RALPH_PARAM_DUAL_STEEPEST_EDGE,
    RALPH_PARAM_SCALING,
    RALPH_PARAM_CRASH,
    RALPH_PARAM_VERIFY,
    RALPH_PARAM_PHASE1_PRICING,
    RALPH_PARAM_VAR_SELECT,
    RALPH_PARAM_LU_SUPERNODE,
    RALPH_PARAM_DETERMINISTIC,
    RALPH_PARAM_RANDOM_SEED,
    RALPH_PARAM_LP_THREADS,
    RALPH_PARAM_TIME_LIMIT,
    RALPH_PARAM_MIP_GAP,
    RALPH_PARAM_OBJ_LIMIT,
    RALPH_PARAM_FEAS_TOL,
    RALPH_PARAM_OPT_TOL,
    RALPH_PARAM_PIVOT_TOL,
    RALPH_PARAM_LP_ALGORITHM,
    RALPH_PARAM_BARRIER_CROSSOVER,
    RALPH_PARAM_LP_EXTERNAL_PROVIDER,
    RALPH_PARAM_LP_EXTERNAL_STRICT,
    RALPH_PARAM_LP_BASIS_GOVERNOR_MODE,
    RALPH_PARAM_LP_REINVERT_CONTROLLER_MODE,
    RALPH_PARAM_LP_POLICY_PROFILE,
    RALPH_PARAM_GLPK_SMCP_METHOD,
    RALPH_PARAM_GLPK_SMCP_PRICING,
    RALPH_PARAM_GLPK_SMCP_RATIO,
    RALPH_PARAM_GLPK_SMCP_FLIP,
    RALPH_PARAM_GLPK_SMCP_BASIS,
    RALPH_PARAM_GLPK_SMCP_PRESOLVE,
    RALPH_PARAM_GLPK_SMCP_TOL_BND,
    RALPH_PARAM_GLPK_SMCP_TOL_DJ,
    RALPH_PARAM_GLPK_SMCP_TOL_PIV,
    RALPH_PARAM_GLPK_SMCP_EXCL,
    RALPH_PARAM_GLPK_SMCP_SHIFT,
    RALPH_PARAM_GLPK_SMCP_AORN,
    RALPH_PARAM_GLPK_BFCP_FACTORIZATION,
    RALPH_PARAM_GLPK_BFCP_BACKEND,
    RALPH_PARAM_GLPK_BFCP_UPDATE_LIMIT,
    RALPH_PARAM_GLPK_BFCP_PIVOT_LIMIT,
    RALPH_PARAM_GLPK_BFCP_SUHL,
    RALPH_PARAM_GLPK_BFCP_PIVOT_TOL,
    RALPH_PARAM_GLPK_BFCP_EPS_TOL,
    RALPH_PARAM_GLPK_BFCP_GROWTH_GUARD,
    RALPH_PARAM_GLPK_BFCP_NFS_MAX,
    RALPH_PARAM_GLPK_BFCP_NRS_MAX,
    RALPH_PARAM_COUNT
} RalphParamId;

typedef struct {
    RalphParamId id;
    const char *name;              /* Canonical string name */
    RalphParamScope scope;         /* SHARED / LP / MIP */
    RalphParamValueType value_type;/* INT / DOUBLE */
    double default_value;          /* Numeric default (int values represented exactly) */
    int has_min;
    double min_value;
    int has_max;
    double max_value;
} RalphParamMeta;

/* Parameters */
int ralph_core_set_int_param(RalphModel *model, const char *name, int value);
int ralph_core_set_dbl_param(RalphModel *model, const char *name, double value);
int ralph_core_get_int_param(const RalphModel *model, const char *name, int *value);
int ralph_core_get_dbl_param(const RalphModel *model, const char *name, double *value);

/* Strict parameter APIs.
 * LP strict APIs reject MIP-only knobs.
 * MIP strict APIs reject LP-only knobs. */
int ralph_core_set_lp_int_param(RalphModel *model, const char *name, int value);
int ralph_core_set_lp_dbl_param(RalphModel *model, const char *name, double value);
int ralph_core_get_lp_int_param(const RalphModel *model, const char *name, int *value);
int ralph_core_get_lp_dbl_param(const RalphModel *model, const char *name, double *value);

int ralph_core_set_mip_int_param(RalphModel *model, const char *name, int value);
int ralph_core_set_mip_dbl_param(RalphModel *model, const char *name, double value);
int ralph_core_get_mip_int_param(const RalphModel *model, const char *name, int *value);
int ralph_core_get_mip_dbl_param(const RalphModel *model, const char *name, double *value);

/* Typed parameter APIs (ID-based). */
int ralph_core_set_int_param_id(RalphModel *model, RalphParamId param, int value);
int ralph_core_set_dbl_param_id(RalphModel *model, RalphParamId param, double value);
int ralph_core_get_int_param_id(const RalphModel *model, RalphParamId param, int *value);
int ralph_core_get_dbl_param_id(const RalphModel *model, RalphParamId param, double *value);

/* Typed strict parameter APIs (ID-based scope enforcement). */
int ralph_core_set_lp_int_param_id(RalphModel *model, RalphParamId param, int value);
int ralph_core_set_lp_dbl_param_id(RalphModel *model, RalphParamId param, double value);
int ralph_core_get_lp_int_param_id(const RalphModel *model, RalphParamId param, int *value);
int ralph_core_get_lp_dbl_param_id(const RalphModel *model, RalphParamId param, double *value);

int ralph_core_set_mip_int_param_id(RalphModel *model, RalphParamId param, int value);
int ralph_core_set_mip_dbl_param_id(RalphModel *model, RalphParamId param, double value);
int ralph_core_get_mip_int_param_id(const RalphModel *model, RalphParamId param, int *value);
int ralph_core_get_mip_dbl_param_id(const RalphModel *model, RalphParamId param, double *value);

/* Parameter metadata/introspection APIs. */
int ralph_core_get_param_count(void);
int ralph_core_get_param_meta(RalphParamId param, RalphParamMeta *meta);
int ralph_core_find_param_by_name(const char *name, RalphParamId *param);

/* File I/O */
int ralph_core_read_mps(RalphModel *model, const char *filename);
int ralph_core_write_mps(const RalphModel *model, const char *filename);
int ralph_core_read_lp(RalphModel *model, const char *filename);
int ralph_core_write_lp(const RalphModel *model, const char *filename);
int ralph_core_write_solution(const RalphModel *model, const char *filename);

/* Write solution to buffer in SOL format.
 * @param model    Solved model
 * @param buf      Output buffer
 * @param buf_size Size of buffer
 * @return Number of bytes written (not including null terminator), or -1 on error.
 *         If return value >= buf_size, output was truncated.
 */
int ralph_core_write_solution_buf(const RalphModel *model, char *buf, size_t buf_size);

/* Name management */
const char* ralph_core_get_var_name(const RalphModel *model, int var);
const char* ralph_core_get_con_name(const RalphModel *model, int con);
int ralph_core_set_var_name(RalphModel *model, int var, const char *name);
int ralph_core_set_con_name(RalphModel *model, int con, const char *name);
const char* ralph_core_get_problem_name(const RalphModel *model);
int ralph_core_set_problem_name(RalphModel *model, const char *name);

/* Utility */
const char* ralph_core_status_string(RalphStatus status);
const char* ralph_core_version(void);

#ifdef __cplusplus
}
#endif

#endif /* RALPH_CORE_H */
