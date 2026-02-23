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

#ifndef RALPH_TEST_MOD_API_IMPL

#define ralph_create ralph_test_create
#define ralph_free ralph_test_free

#define ralph_set_obj_sense ralph_test_set_obj_sense
#define ralph_add_var ralph_test_add_var
#define ralph_add_vars ralph_test_add_vars
#define ralph_add_constraint ralph_test_add_constraint
#define ralph_set_var_bounds ralph_test_set_var_bounds
#define ralph_set_var_type ralph_test_set_var_type
#define ralph_set_obj_coef ralph_test_set_obj_coef
#define ralph_set_obj_offset ralph_test_set_obj_offset
#define ralph_get_obj_offset ralph_test_get_obj_offset
#define ralph_set_var_name ralph_test_set_var_name
#define ralph_set_con_name ralph_test_set_con_name
#define ralph_get_var_name ralph_test_get_var_name
#define ralph_get_con_name ralph_test_get_con_name

#define ralph_get_num_vars ralph_test_get_num_vars
#define ralph_get_num_cons ralph_test_get_num_cons
#define ralph_get_num_integers ralph_test_get_num_integers
#define ralph_is_mip ralph_test_is_mip
#define ralph_get_var_bounds ralph_test_get_var_bounds

#define ralph_optimize ralph_test_optimize
#define ralph_optimize_lp ralph_test_optimize_lp
#define ralph_optimize_mip ralph_test_optimize_mip

#define ralph_get_status ralph_test_get_status
#define ralph_get_objval ralph_test_get_objval
#define ralph_get_solution ralph_test_get_solution
#define ralph_get_dual_solution ralph_test_get_dual_solution
#define ralph_get_reduced_costs ralph_test_get_reduced_costs
#define ralph_get_iterations ralph_test_get_iterations

#define ralph_save_basis ralph_test_save_basis
#define ralph_load_basis ralph_test_load_basis
#define ralph_get_basis_status ralph_test_get_basis_status
#define ralph_set_basis_status ralph_test_set_basis_status
#define ralph_free_basis ralph_test_free_basis
#define ralph_write_basis_file ralph_test_write_basis_file
#define ralph_read_basis_file ralph_test_read_basis_file

#define ralph_set_int_param ralph_test_set_int_param
#define ralph_set_dbl_param ralph_test_set_dbl_param
#define ralph_get_int_param ralph_test_get_int_param
#define ralph_get_dbl_param ralph_test_get_dbl_param
#define ralph_set_lp_int_param ralph_test_set_lp_int_param
#define ralph_set_lp_dbl_param ralph_test_set_lp_dbl_param
#define ralph_get_lp_int_param ralph_test_get_lp_int_param
#define ralph_get_lp_dbl_param ralph_test_get_lp_dbl_param
#define ralph_set_mip_int_param ralph_test_set_mip_int_param
#define ralph_set_mip_dbl_param ralph_test_set_mip_dbl_param
#define ralph_get_mip_int_param ralph_test_get_mip_int_param
#define ralph_get_mip_dbl_param ralph_test_get_mip_dbl_param

#define ralph_read_mps ralph_test_read_mps
#define ralph_write_mps ralph_test_write_mps
#define ralph_read_lp ralph_test_read_lp
#define ralph_write_lp ralph_test_write_lp
#define ralph_write_solution ralph_test_write_solution
#define ralph_write_solution_buf ralph_test_write_solution_buf

#define ralph_get_best_bound ralph_test_get_best_bound
#define ralph_get_mip_gap ralph_test_get_mip_gap
#define ralph_get_node_count ralph_test_get_node_count
#define ralph_set_branch_priorities ralph_test_set_branch_priorities
#define ralph_set_branch_directions ralph_test_set_branch_directions
#define ralph_set_mip_start ralph_test_set_mip_start
#define ralph_set_mip_start_sparse ralph_test_set_mip_start_sparse
#define ralph_clear_mip_start ralph_test_clear_mip_start
#define ralph_get_mip_start_status ralph_test_get_mip_start_status
#define ralph_set_mip_start_repair_mode ralph_test_set_mip_start_repair_mode
#define ralph_get_mip_start_repair_mode ralph_test_get_mip_start_repair_mode
#define ralph_write_mip_start_file ralph_test_write_mip_start_file
#define ralph_read_mip_start_file ralph_test_read_mip_start_file
#define ralph_set_cut_callback ralph_test_set_cut_callback
#define ralph_set_branch_callback ralph_test_set_branch_callback
#define ralph_solve_benders ralph_test_solve_benders

#define ralph_status_string ralph_test_status_string
#define ralph_version ralph_test_version

#endif /* RALPH_TEST_MOD_API_IMPL */

#endif /* RALPH_TEST_MOD_API_H */
