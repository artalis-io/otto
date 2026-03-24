/*
 * Ralph MIP modular public API wrappers.
 *
 * This module depends on the LP public model handle and extends it with
 * MIP-only controls and search statistics.
 */

#include <stdlib.h>

#include "ralph_core.h"
#include "ralph_mip.h"
#include "mip.h"

/* Internal helper exposed by ralph.c for diagnostics. */
MIPSolver* ralph_get_mip_solver(const RalphModel *model);

RalphMIPModel* ralph_mip_create(void) {
    return (RalphMIPModel *)ralph_core_create();
}

void ralph_mip_free(RalphMIPModel *model) {
    ralph_core_free((RalphModel *)model);
}

int ralph_mip_optimize(RalphMIPModel *model) {
    return ralph_core_optimize_mip((RalphModel *)model);
}

int ralph_mip_get_last_error(const RalphMIPModel *model, RalphAPIError *out) {
    return ralph_core_get_last_error((const RalphModel *)model, out);
}

int ralph_mip_clear_error(RalphMIPModel *model) {
    return ralph_core_clear_error((RalphModel *)model);
}

RalphLPStatus ralph_mip_get_status(const RalphMIPModel *model) {
    return (RalphLPStatus)ralph_core_get_status((const RalphModel *)model);
}

double ralph_mip_get_objval(const RalphMIPModel *model) {
    return ralph_core_get_objval((const RalphModel *)model);
}

int ralph_mip_get_solution(const RalphMIPModel *model, double *x) {
    return ralph_core_get_solution((const RalphModel *)model, x);
}

double ralph_mip_get_best_bound(const RalphMIPModel *model) {
    return ralph_core_get_best_bound((const RalphModel *)model);
}

double ralph_mip_get_gap(const RalphMIPModel *model) {
    return ralph_core_get_mip_gap((const RalphModel *)model);
}

int ralph_mip_get_node_count(const RalphMIPModel *model) {
    return ralph_core_get_node_count((const RalphModel *)model);
}

void ralph_mip_print_stats(const RalphMIPModel *model) {
    mip_print_stats(ralph_get_mip_solver((const RalphModel *)model));
}

int ralph_mip_set_branch_priorities(RalphMIPModel *model, const int *priorities) {
    return ralph_core_set_branch_priorities((RalphModel *)model, priorities);
}

int ralph_mip_set_branch_directions(RalphMIPModel *model,
                                    const RalphMIPBranchDirection *directions) {
    int *tmp = NULL;
    int n;
    int i;
    int ret;

    if (!model) return -1;
    if (!directions) return ralph_core_set_branch_directions((RalphModel *)model, NULL);

    n = ralph_core_get_num_vars((const RalphModel *)model);
    if (n <= 0) return -1;

    tmp = (int *)malloc((size_t)n * sizeof(int));
    if (!tmp) return -1;
    for (i = 0; i < n; i++) tmp[i] = (int)directions[i];

    ret = ralph_core_set_branch_directions((RalphModel *)model, tmp);
    free(tmp);
    return ret;
}

void ralph_mip_set_cut_callback(RalphMIPModel *model, const RalphMIPCutCallback *callback) {
    ralph_core_set_cut_callback((RalphModel *)model, (const RalphCutCallback *)callback);
}

void ralph_mip_set_branch_callback(RalphMIPModel *model, const RalphMIPBranchCallback *callback) {
    ralph_core_set_branch_callback((RalphModel *)model, (const RalphBranchCallback *)callback);
}

int ralph_mip_set_start(RalphMIPModel *model, const double *x) {
    return ralph_core_set_mip_start((RalphModel *)model, x);
}

int ralph_mip_set_start_sparse(RalphMIPModel *model, int count,
                               const int *indices, const double *values) {
    return ralph_core_set_mip_start_sparse((RalphModel *)model, count, indices, values);
}

void ralph_mip_clear_start(RalphMIPModel *model) {
    ralph_core_clear_mip_start((RalphModel *)model);
}

RalphMIPStartState ralph_mip_get_start_status(const RalphMIPModel *model) {
    return (RalphMIPStartState)ralph_core_get_mip_start_status((const RalphModel *)model);
}

int ralph_mip_set_start_repair_mode(RalphMIPModel *model, RalphMIPStartRepairPolicy mode) {
    return ralph_core_set_mip_start_repair_mode((RalphModel *)model, (RalphMIPStartRepairMode)mode);
}

RalphMIPStartRepairPolicy ralph_mip_get_start_repair_mode(const RalphMIPModel *model) {
    return (RalphMIPStartRepairPolicy)ralph_core_get_mip_start_repair_mode((const RalphModel *)model);
}

int ralph_mip_set_int_param(RalphMIPModel *model, const char *name, int value) {
    return ralph_core_set_mip_int_param((RalphModel *)model, name, value);
}

int ralph_mip_set_dbl_param(RalphMIPModel *model, const char *name, double value) {
    return ralph_core_set_mip_dbl_param((RalphModel *)model, name, value);
}

int ralph_mip_get_int_param(const RalphMIPModel *model, const char *name, int *value) {
    return ralph_core_get_mip_int_param((const RalphModel *)model, name, value);
}

int ralph_mip_get_dbl_param(const RalphMIPModel *model, const char *name, double *value) {
    return ralph_core_get_mip_dbl_param((const RalphModel *)model, name, value);
}

int ralph_mip_write_start_file(const RalphMIPModel *model, const char *filename) {
    return ralph_core_write_mip_start_file((const RalphModel *)model, filename);
}

int ralph_mip_read_start_file(RalphMIPModel *model, const char *filename) {
    return ralph_core_read_mip_start_file((RalphModel *)model, filename);
}

int ralph_mip_solve_benders(RalphMIPModel *model,
                            const RalphMIPBendersConfig *config,
                            double *x,
                            RalphMIPBendersResult *result) {
    RalphBendersConfig legacy_cfg = RALPH_BENDERS_CONFIG_DEFAULT;
    RalphBendersResult legacy_result;
    RalphBendersResult *legacy_result_ptr = NULL;
    int rc;

    if (!model || !config) return -1;

    legacy_cfg.master_var_indices = config->master_var_indices;
    legacy_cfg.num_master_vars = config->num_master_vars;
    legacy_cfg.theta_var = config->theta_var;
    legacy_cfg.num_scenarios = config->num_scenarios;
    legacy_cfg.scenario_probs = config->scenario_probs;
    legacy_cfg.gap_tolerance = config->gap_tolerance;
    legacy_cfg.max_iterations = config->max_iterations;
    legacy_cfg.cuts_at_lp_nodes = config->cuts_at_lp_nodes;
    legacy_cfg.warm_start_master = config->warm_start_master;
    legacy_cfg.warm_start_subproblems = config->warm_start_subproblems;
    legacy_cfg.initial_master_solution = config->initial_master_solution;
    legacy_cfg.strict_farkas = config->strict_farkas;
    legacy_cfg.verbose = config->verbose;
    legacy_cfg.branch_priorities = config->branch_priorities;
    legacy_cfg.branch_directions = (const int *)config->branch_directions;

    if (result) {
        legacy_result_ptr = &legacy_result;
    }

    rc = ralph_core_solve_benders((RalphModel *)model, &legacy_cfg, x, legacy_result_ptr);
    if (rc != 0 || !result) return rc;

    result->status = (RalphLPStatus)legacy_result.status;
    result->objective = legacy_result.objective;
    result->master_obj = legacy_result.master_obj;
    result->subproblem_obj = legacy_result.subproblem_obj;
    result->gap = legacy_result.gap;
    result->iterations = legacy_result.iterations;
    result->optimality_cuts = legacy_result.optimality_cuts;
    result->feasibility_cuts = legacy_result.feasibility_cuts;
    result->nodes_explored = legacy_result.nodes_explored;
    result->subproblems_solved = legacy_result.subproblems_solved;
    result->master_warm_starts_attempted = legacy_result.master_warm_starts_attempted;
    result->master_warm_starts_accepted = legacy_result.master_warm_starts_accepted;
    result->subproblem_warm_starts = legacy_result.subproblem_warm_starts;
    result->subproblem_cold_starts = legacy_result.subproblem_cold_starts;
    result->solve_time = legacy_result.solve_time;
    return 0;
}
