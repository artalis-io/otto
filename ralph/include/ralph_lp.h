/*
 * Ralph LP Public API (modular header)
 *
 * This header exposes LP-focused APIs only.
 * MIP-specific APIs are declared in ralph_mip.h.
 */

#ifndef RALPH_LP_PUBLIC_H
#define RALPH_LP_PUBLIC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LP constants */
#define RALPH_LP_INFINITY 1e30

/* LP status codes */
typedef enum {
    RALPH_LP_STATUS_UNKNOWN = 0,
    RALPH_LP_STATUS_OPTIMAL = 1,
    RALPH_LP_STATUS_INFEASIBLE = 2,
    RALPH_LP_STATUS_UNBOUNDED = 3,
    RALPH_LP_STATUS_INF_OR_UNBD = 4,
    RALPH_LP_STATUS_ITERATION_LIMIT = 5,
    RALPH_LP_STATUS_TIME_LIMIT = 6,
    RALPH_LP_STATUS_NODE_LIMIT = 7,
    RALPH_LP_STATUS_IMPRECISE = 8,
    RALPH_LP_STATUS_OBJ_LIMIT = 9,
    RALPH_LP_STATUS_ERROR = -1
} RalphLPStatus;

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
    RalphLPStatus status_hint;
    RalphErrorAPIId api_id;
    int detail_i0;
    int detail_i1;
    char message[RALPH_API_ERROR_MESSAGE_MAX];
} RalphAPIError;
#endif /* RALPH_API_ERROR_TYPES_DEFINED */

/* LP variable types (integer/binary allowed for model portability). */
typedef enum {
    RALPH_LP_VAR_CONTINUOUS = 'C',
    RALPH_LP_VAR_INTEGER = 'I',
    RALPH_LP_VAR_BINARY = 'B'
} RalphLPVarType;

/* LP row sense */
typedef enum {
    RALPH_LP_SENSE_LESS_EQUAL = 'L',
    RALPH_LP_SENSE_EQUAL = 'E',
    RALPH_LP_SENSE_GREATER_EQUAL = 'G'
} RalphLPSense;

/* LP objective sense */
typedef enum {
    RALPH_LP_OBJ_MINIMIZE = 1,
    RALPH_LP_OBJ_MAXIMIZE = -1
} RalphLPObjSense;

/* LP basis status for warm-start interop. */
typedef enum {
    RALPH_LP_BASIS_BASIC = 0,
    RALPH_LP_BASIS_AT_LOWER = 1,
    RALPH_LP_BASIS_AT_UPPER = 2,
    RALPH_LP_BASIS_FREE = 3,
    RALPH_LP_BASIS_FIXED = 4
} RalphLPBasisStatus;

/* LP algorithm selection API surface.
 * Internal-first policy:
 * - PRIMAL/DUAL/AUTO route to internal simplex backends.
 * - *_EXTERNAL modes require lp_external_provider + registered adapter match.
 * Barrier backends are capability-gated. */
#ifndef RALPH_LP_ALGO_BASE_TYPES_DEFINED
#define RALPH_LP_ALGO_BASE_TYPES_DEFINED
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
#endif /* RALPH_LP_ALGO_BASE_TYPES_DEFINED */

#ifndef RALPH_LP_EXT_REPORT_TYPES_DEFINED
#define RALPH_LP_EXT_REPORT_TYPES_DEFINED
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
 * - Any non-zero value means solve failure; unmapped values are treated as generic failures. */
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
    RalphLPStatus mapped_status;
    int fatal;
} RalphLPExternalFailureReport;
#endif /* RALPH_LP_EXT_REPORT_TYPES_DEFINED */

/* Opaque LP model/basis handles (same underlying engine types). */
typedef struct RalphModel RalphLPModel;
typedef struct RalphBasis RalphLPBasis;

/* Lifecycle */
RalphLPModel* ralph_lp_create(void);
void ralph_lp_free(RalphLPModel *model);

/* Model building */
int ralph_lp_set_obj_sense(RalphLPModel *model, RalphLPObjSense sense);
int ralph_lp_add_var(RalphLPModel *model, double lb, double ub, double obj, RalphLPVarType type);
int ralph_lp_add_vars(RalphLPModel *model, int count, const double *lb, const double *ub,
                      const double *obj, const RalphLPVarType *types);
int ralph_lp_add_constraint(RalphLPModel *model, int nnz, const int *indices,
                            const double *values, RalphLPSense sense, double rhs);

/* Model modification */
int ralph_lp_set_var_bounds(RalphLPModel *model, int var, double lb, double ub);
int ralph_lp_set_var_type(RalphLPModel *model, int var, RalphLPVarType type);
int ralph_lp_set_obj_coef(RalphLPModel *model, int var, double coef);
int ralph_lp_set_obj_offset(RalphLPModel *model, double offset);
double ralph_lp_get_obj_offset(const RalphLPModel *model);
int ralph_lp_set_var_name(RalphLPModel *model, int var, const char *name);
int ralph_lp_set_con_name(RalphLPModel *model, int con, const char *name);
const char* ralph_lp_get_var_name(const RalphLPModel *model, int var);
const char* ralph_lp_get_con_name(const RalphLPModel *model, int con);

/* Model queries */
int ralph_lp_get_num_vars(const RalphLPModel *model);
int ralph_lp_get_num_cons(const RalphLPModel *model);
int ralph_lp_get_num_integer_vars(const RalphLPModel *model);
int ralph_lp_get_var_bounds(const RalphLPModel *model, int var, double *lb, double *ub);

/* Solve */
int ralph_lp_optimize(RalphLPModel *model);

/* Structured API error diagnostics (model-local with TLS fallback when model==NULL).
 * Contract:
 * - returns 0 when an error snapshot is available, -1 when unavailable/invalid args.
 * - `ralph_lp_clear_error(model)` clears model-local error state.
 * - pass `model==NULL` to query/clear TLS fallback state used by no-model entry points. */
int ralph_lp_get_last_error(const RalphLPModel *model, RalphAPIError *out);
int ralph_lp_clear_error(RalphLPModel *model);
const char* ralph_lp_error_domain_string(RalphErrorDomain domain);
const char* ralph_lp_error_code_string(RalphErrorCode code);
const char* ralph_lp_error_api_string(RalphErrorAPIId api_id);
const char* ralph_lp_error_message(const RalphAPIError *error);

/* LP algorithm/capability and external-adapter reports. */
int ralph_lp_get_capabilities(RalphLPCapabilities *caps);
int ralph_lp_get_last_algorithm_report(const RalphLPModel *model,
                                       RalphLPSolveAlgorithmReport *report);
int ralph_lp_get_last_external_failure_report(const RalphLPModel *model,
                                              RalphLPExternalFailureReport *report);

/* External adapter registration/query API.
 * Contract:
 * - Multiple providers may be registered concurrently.
 * - External LP dispatch still requires explicit external algorithm request +
 *   matching lp_external_provider parameter. */
const char* ralph_lp_external_provider_name(RalphLPExternalProvider provider);
int ralph_lp_external_provider_capabilities(RalphLPExternalProvider provider,
                                            RalphLPExternalCapabilities *caps);
int ralph_lp_external_registered_providers(RalphLPExternalProvider *providers,
                                           int capacity,
                                           int *count_out);
int ralph_lp_external_register_adapter(const RalphLPExternalAdapter *adapter);
/* Register GLPK as an out-of-process adapter via `glpsol` (no in-process libglpk link).
 * `glpsol_path == NULL` means PATH lookup. */
int ralph_lp_external_register_glpk_oop(const char *glpsol_path);
int ralph_lp_external_unregister_adapter(RalphLPExternalProvider provider);
int ralph_lp_external_unregister_glpk_oop(void);
void ralph_lp_external_unregister_all_adapters(void);
int ralph_lp_external_is_adapter_registered(RalphLPExternalProvider provider);

/* Solution queries */
RalphLPStatus ralph_lp_get_status(const RalphLPModel *model);
double ralph_lp_get_objval(const RalphLPModel *model);
int ralph_lp_get_solution(const RalphLPModel *model, double *x);
int ralph_lp_get_dual_solution(const RalphLPModel *model, double *y);
int ralph_lp_get_reduced_costs(const RalphLPModel *model, double *rc);
int ralph_lp_get_iterations(const RalphLPModel *model);

/* Basis warm-start */
RalphLPBasis* ralph_lp_save_basis(const RalphLPModel *model);
int ralph_lp_load_basis(RalphLPModel *model, const RalphLPBasis *basis);
int ralph_lp_get_basis_status(const RalphLPModel *model,
                              RalphLPBasisStatus *col_status,
                              RalphLPBasisStatus *row_status);
int ralph_lp_set_basis_status(RalphLPModel *model,
                              const RalphLPBasisStatus *col_status,
                              const RalphLPBasisStatus *row_status);
void ralph_lp_free_basis(RalphLPBasis *basis);
int ralph_lp_write_basis_file(const RalphLPBasis *basis, const char *filename);
RalphLPBasis* ralph_lp_read_basis_file(const char *filename);

/* LP-scoped parameter APIs */
int ralph_lp_set_int_param(RalphLPModel *model, const char *name, int value);
int ralph_lp_set_dbl_param(RalphLPModel *model, const char *name, double value);
int ralph_lp_get_int_param(const RalphLPModel *model, const char *name, int *value);
int ralph_lp_get_dbl_param(const RalphLPModel *model, const char *name, double *value);

/* File I/O */
int ralph_lp_read_mps(RalphLPModel *model, const char *filename);
int ralph_lp_write_mps(const RalphLPModel *model, const char *filename);
int ralph_lp_read_lp(RalphLPModel *model, const char *filename);
int ralph_lp_write_lp(const RalphLPModel *model, const char *filename);
int ralph_lp_write_solution(const RalphLPModel *model, const char *filename);
int ralph_lp_write_solution_buf(const RalphLPModel *model, char *buf, size_t buf_size);

/* Utility */
const char* ralph_lp_status_string(RalphLPStatus status);
const char* ralph_lp_version(void);

#ifdef __cplusplus
}
#endif

#endif /* RALPH_LP_PUBLIC_H */
