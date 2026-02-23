/*
 * Ralph test/benchmark modular API adapter.
 *
 * This header is test/bench-only glue. It remaps high-traffic legacy
 * `ralph_*` calls to adapter functions implemented in a separate compilation
 * unit (`src/ralph_test_mod_api.c`) that route through `ralph_lp_*` /
 * `ralph_mip_*`.
 */

#ifndef RALPH_TEST_MOD_API_H
#define RALPH_TEST_MOD_API_H

#include "ralph_core.h"

#ifdef __cplusplus
extern "C" {
#endif

RalphModel* ralph_test_create(void);
void ralph_test_free(RalphModel *model);

int ralph_test_set_obj_sense(RalphModel *model, RalphObjSense sense);
int ralph_test_add_var(RalphModel *model, double lb, double ub, double obj, RalphVarType type);
int ralph_test_add_vars(RalphModel *model, int count, const double *lb, const double *ub,
                        const double *obj, const RalphVarType *types);
int ralph_test_add_constraint(RalphModel *model, int nnz, const int *indices,
                              const double *values, RalphSense sense, double rhs);
int ralph_test_set_var_bounds(RalphModel *model, int var, double lb, double ub);
int ralph_test_set_var_type(RalphModel *model, int var, RalphVarType type);
int ralph_test_set_obj_coef(RalphModel *model, int var, double coef);
int ralph_test_set_obj_offset(RalphModel *model, double offset);
double ralph_test_get_obj_offset(const RalphModel *model);
int ralph_test_set_var_name(RalphModel *model, int var, const char *name);
int ralph_test_set_con_name(RalphModel *model, int con, const char *name);
const char* ralph_test_get_var_name(const RalphModel *model, int var);
const char* ralph_test_get_con_name(const RalphModel *model, int con);

int ralph_test_get_num_vars(const RalphModel *model);
int ralph_test_get_num_cons(const RalphModel *model);
int ralph_test_get_num_integers(const RalphModel *model);
int ralph_test_is_mip(const RalphModel *model);
int ralph_test_get_var_bounds(const RalphModel *model, int var, double *lb, double *ub);

int ralph_test_optimize(RalphModel *model);
int ralph_test_optimize_lp(RalphModel *model);
int ralph_test_optimize_mip(RalphModel *model);

RalphStatus ralph_test_get_status(const RalphModel *model);
double ralph_test_get_objval(const RalphModel *model);
int ralph_test_get_solution(const RalphModel *model, double *x);
int ralph_test_get_dual_solution(const RalphModel *model, double *y);
int ralph_test_get_reduced_costs(const RalphModel *model, double *rc);
int ralph_test_get_iterations(const RalphModel *model);

RalphBasis* ralph_test_save_basis(const RalphModel *model);
int ralph_test_load_basis(RalphModel *model, const RalphBasis *basis);
int ralph_test_get_basis_status(const RalphModel *model,
                                RalphBasisStatus *col_status,
                                RalphBasisStatus *row_status);
int ralph_test_set_basis_status(RalphModel *model,
                                const RalphBasisStatus *col_status,
                                const RalphBasisStatus *row_status);
void ralph_test_free_basis(RalphBasis *basis);
int ralph_test_write_basis_file(const RalphBasis *basis, const char *filename);
RalphBasis* ralph_test_read_basis_file(const char *filename);

int ralph_test_set_int_param(RalphModel *model, const char *name, int value);
int ralph_test_set_dbl_param(RalphModel *model, const char *name, double value);
int ralph_test_get_int_param(const RalphModel *model, const char *name, int *value);
int ralph_test_get_dbl_param(const RalphModel *model, const char *name, double *value);
int ralph_test_set_lp_int_param(RalphModel *model, const char *name, int value);
int ralph_test_set_lp_dbl_param(RalphModel *model, const char *name, double value);
int ralph_test_get_lp_int_param(const RalphModel *model, const char *name, int *value);
int ralph_test_get_lp_dbl_param(const RalphModel *model, const char *name, double *value);
int ralph_test_set_mip_int_param(RalphModel *model, const char *name, int value);
int ralph_test_set_mip_dbl_param(RalphModel *model, const char *name, double value);
int ralph_test_get_mip_int_param(const RalphModel *model, const char *name, int *value);
int ralph_test_get_mip_dbl_param(const RalphModel *model, const char *name, double *value);

int ralph_test_read_mps(RalphModel *model, const char *filename);
int ralph_test_write_mps(const RalphModel *model, const char *filename);
int ralph_test_read_lp(RalphModel *model, const char *filename);
int ralph_test_write_lp(const RalphModel *model, const char *filename);
int ralph_test_write_solution(const RalphModel *model, const char *filename);
int ralph_test_write_solution_buf(const RalphModel *model, char *buf, size_t buf_size);

double ralph_test_get_best_bound(const RalphModel *model);
double ralph_test_get_mip_gap(const RalphModel *model);
int ralph_test_get_node_count(const RalphModel *model);
int ralph_test_set_branch_priorities(RalphModel *model, const int *priorities);
int ralph_test_set_branch_directions(RalphModel *model, const int *directions);
int ralph_test_set_mip_start(RalphModel *model, const double *x);
int ralph_test_set_mip_start_sparse(RalphModel *model, int count,
                                    const int *indices, const double *values);
void ralph_test_clear_mip_start(RalphModel *model);
RalphMIPStartStatus ralph_test_get_mip_start_status(const RalphModel *model);
int ralph_test_set_mip_start_repair_mode(RalphModel *model, RalphMIPStartRepairMode mode);
RalphMIPStartRepairMode ralph_test_get_mip_start_repair_mode(const RalphModel *model);
int ralph_test_write_mip_start_file(const RalphModel *model, const char *filename);
int ralph_test_read_mip_start_file(RalphModel *model, const char *filename);
void ralph_test_set_cut_callback(RalphModel *model, const RalphCutCallback *callback);
void ralph_test_set_branch_callback(RalphModel *model, const RalphBranchCallback *callback);
int ralph_test_solve_benders(RalphModel *model,
                             const RalphBendersConfig *config,
                             double *x,
                             RalphBendersResult *result);

const char* ralph_test_status_string(RalphStatus status);
const char* ralph_test_version(void);

#ifdef __cplusplus
}
#endif

#endif /* RALPH_TEST_MOD_API_H */
