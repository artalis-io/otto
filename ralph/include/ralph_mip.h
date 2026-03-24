/*
 * Ralph MIP Public API (modular header)
 *
 * MIP extends the LP API. Include this header for branch-and-bound controls,
 * MIP warm-starts, and node-bound telemetry.
 */

#ifndef RALPH_MIP_PUBLIC_H
#define RALPH_MIP_PUBLIC_H

#include "ralph_lp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MIP branch direction hints */
typedef enum {
    RALPH_MIP_BRANCH_AUTO = 0,
    RALPH_MIP_BRANCH_DOWN = -1,
    RALPH_MIP_BRANCH_UP = 1
} RalphMIPBranchDirection;

/* MIP-start status */
typedef enum {
    RALPH_MIP_START_STATUS_NONE = 0,
    RALPH_MIP_START_STATUS_PENDING = 1,
    RALPH_MIP_START_STATUS_ACCEPTED = 2,
    RALPH_MIP_START_STATUS_REJECTED = 3
} RalphMIPStartState;

/* MIP-start repair policy */
typedef enum {
    RALPH_MIP_START_REPAIR_MODE_STRICT = 0,
    RALPH_MIP_START_REPAIR_MODE_PROJECT_BOUNDS = 1,
    RALPH_MIP_START_REPAIR_MODE_PROJECT_AND_ROUND = 2
} RalphMIPStartRepairPolicy;

/* Cut representation for MIP callbacks and lazy cuts. */
typedef struct {
    const int *indices;
    const double *coeffs;
    int num_vars;
    RalphLPSense sense;
    double rhs;
} RalphMIPCut;

/* User cut callback (called at MIP nodes on LP relaxations). */
typedef struct {
    int (*generate_cuts)(
        void *user_data,
        const double *x_relaxation,
        int num_vars,
        RalphMIPCut *cuts,
        int max_cuts
    );
    void *user_data;
} RalphMIPCutCallback;

/* User branching callback (optional custom variable selection). */
typedef struct {
    int (*select_branch_var)(
        void *user_data,
        const double *x_relaxation,
        int num_vars,
        const int *is_integer,
        const double *lb,
        const double *ub
    );
    void *user_data;
} RalphMIPBranchCallback;

/* Benders decomposition configuration (MIP-level API). */
typedef struct {
    const int *master_var_indices;
    int num_master_vars;
    int theta_var;
    int num_scenarios;
    const double *scenario_probs;
    double gap_tolerance;
    int max_iterations;
    int cuts_at_lp_nodes;
    int warm_start_master;
    int warm_start_subproblems;
    const double *initial_master_solution;
    int strict_farkas;
    int verbose;
    const int *branch_priorities;
    const RalphMIPBranchDirection *branch_directions;
} RalphMIPBendersConfig;

#define RALPH_MIP_BENDERS_CONFIG_DEFAULT { \
    .master_var_indices = NULL,            \
    .num_master_vars = 0,                  \
    .theta_var = -1,                       \
    .num_scenarios = 1,                    \
    .scenario_probs = NULL,                \
    .gap_tolerance = 1e-6,                 \
    .max_iterations = 1000,                \
    .cuts_at_lp_nodes = 1,                 \
    .warm_start_master = 1,                \
    .warm_start_subproblems = 1,           \
    .initial_master_solution = NULL,       \
    .strict_farkas = 0,                    \
    .verbose = 0,                          \
    .branch_priorities = NULL,             \
    .branch_directions = NULL              \
}

/* Benders result statistics. */
typedef struct {
    RalphLPStatus status;
    double objective;
    double master_obj;
    double subproblem_obj;
    double gap;
    int iterations;
    int optimality_cuts;
    int feasibility_cuts;
    int nodes_explored;
    int subproblems_solved;
    int master_warm_starts_attempted;
    int master_warm_starts_accepted;
    int subproblem_warm_starts;
    int subproblem_cold_starts;
    double solve_time;
} RalphMIPBendersResult;

/* Opaque MIP model handle (same underlying engine model type). */
typedef RalphLPModel RalphMIPModel;

/* Lifecycle */
RalphMIPModel* ralph_mip_create(void);
void ralph_mip_free(RalphMIPModel *model);

/* Solve */
int ralph_mip_optimize(RalphMIPModel *model);

/* Structured API error diagnostics (same contract as LP API). */
int ralph_mip_get_last_error(const RalphMIPModel *model, RalphAPIError *out);
int ralph_mip_clear_error(RalphMIPModel *model);

/* Solution and MIP search metrics */
RalphLPStatus ralph_mip_get_status(const RalphMIPModel *model);
double ralph_mip_get_objval(const RalphMIPModel *model);
int ralph_mip_get_solution(const RalphMIPModel *model, double *x);
double ralph_mip_get_best_bound(const RalphMIPModel *model);
double ralph_mip_get_gap(const RalphMIPModel *model);
int ralph_mip_get_node_count(const RalphMIPModel *model);
void ralph_mip_print_stats(const RalphMIPModel *model);

/* Branching controls */
int ralph_mip_set_branch_priorities(RalphMIPModel *model, const int *priorities);
int ralph_mip_set_branch_directions(RalphMIPModel *model,
                                    const RalphMIPBranchDirection *directions);
void ralph_mip_set_cut_callback(RalphMIPModel *model, const RalphMIPCutCallback *callback);
void ralph_mip_set_branch_callback(RalphMIPModel *model, const RalphMIPBranchCallback *callback);

/* MIP-start controls */
int ralph_mip_set_start(RalphMIPModel *model, const double *x);
int ralph_mip_set_start_sparse(RalphMIPModel *model, int count,
                               const int *indices, const double *values);
void ralph_mip_clear_start(RalphMIPModel *model);
RalphMIPStartState ralph_mip_get_start_status(const RalphMIPModel *model);
int ralph_mip_set_start_repair_mode(RalphMIPModel *model, RalphMIPStartRepairPolicy mode);
RalphMIPStartRepairPolicy ralph_mip_get_start_repair_mode(const RalphMIPModel *model);

/* MIP-scoped parameter APIs */
int ralph_mip_set_int_param(RalphMIPModel *model, const char *name, int value);
int ralph_mip_set_dbl_param(RalphMIPModel *model, const char *name, double value);
int ralph_mip_get_int_param(const RalphMIPModel *model, const char *name, int *value);
int ralph_mip_get_dbl_param(const RalphMIPModel *model, const char *name, double *value);

/* MIP-start serialization */
int ralph_mip_write_start_file(const RalphMIPModel *model, const char *filename);
int ralph_mip_read_start_file(RalphMIPModel *model, const char *filename);

/* Benders decomposition solve entry point. */
int ralph_mip_solve_benders(RalphMIPModel *model,
                            const RalphMIPBendersConfig *config,
                            double *x,
                            RalphMIPBendersResult *result);

#ifdef __cplusplus
}
#endif

#endif /* RALPH_MIP_PUBLIC_H */
