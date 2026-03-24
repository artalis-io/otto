#include "ralph_test_mod_api.h"

#include "ralph_lp.h"
#include "ralph_mip.h"

static int ralph_test_model_is_mip(const RalphModel *model) {
    return ralph_lp_get_num_integer_vars((const RalphLPModel *)model) > 0;
}

RalphModel* ralph_test_create(void) {
    return (RalphModel *)ralph_lp_create();
}

void ralph_test_free(RalphModel *model) {
    ralph_lp_free((RalphLPModel *)model);
}

int ralph_test_set_obj_sense(RalphModel *model, RalphObjSense sense) {
    return ralph_lp_set_obj_sense((RalphLPModel *)model, (RalphLPObjSense)sense);
}

int ralph_test_add_var(RalphModel *model, double lb, double ub, double obj, RalphVarType type) {
    return ralph_lp_add_var((RalphLPModel *)model, lb, ub, obj, (RalphLPVarType)type);
}

int ralph_test_add_vars(RalphModel *model, int count, const double *lb, const double *ub,
                        const double *obj, const RalphVarType *types) {
    return ralph_lp_add_vars((RalphLPModel *)model,
                             count,
                             lb,
                             ub,
                             obj,
                             (const RalphLPVarType *)types);
}

int ralph_test_add_constraint(RalphModel *model, int nnz, const int *indices,
                              const double *values, RalphSense sense, double rhs) {
    return ralph_lp_add_constraint((RalphLPModel *)model,
                                   nnz,
                                   indices,
                                   values,
                                   (RalphLPSense)sense,
                                   rhs);
}

int ralph_test_set_var_bounds(RalphModel *model, int var, double lb, double ub) {
    return ralph_lp_set_var_bounds((RalphLPModel *)model, var, lb, ub);
}

int ralph_test_set_var_type(RalphModel *model, int var, RalphVarType type) {
    return ralph_lp_set_var_type((RalphLPModel *)model, var, (RalphLPVarType)type);
}

int ralph_test_set_obj_coef(RalphModel *model, int var, double coef) {
    return ralph_lp_set_obj_coef((RalphLPModel *)model, var, coef);
}

int ralph_test_set_obj_offset(RalphModel *model, double offset) {
    return ralph_lp_set_obj_offset((RalphLPModel *)model, offset);
}

double ralph_test_get_obj_offset(const RalphModel *model) {
    return ralph_lp_get_obj_offset((const RalphLPModel *)model);
}

int ralph_test_set_var_name(RalphModel *model, int var, const char *name) {
    return ralph_lp_set_var_name((RalphLPModel *)model, var, name);
}

int ralph_test_set_con_name(RalphModel *model, int con, const char *name) {
    return ralph_lp_set_con_name((RalphLPModel *)model, con, name);
}

const char* ralph_test_get_var_name(const RalphModel *model, int var) {
    return ralph_lp_get_var_name((const RalphLPModel *)model, var);
}

const char* ralph_test_get_con_name(const RalphModel *model, int con) {
    return ralph_lp_get_con_name((const RalphLPModel *)model, con);
}

int ralph_test_get_num_vars(const RalphModel *model) {
    return ralph_lp_get_num_vars((const RalphLPModel *)model);
}

int ralph_test_get_num_cons(const RalphModel *model) {
    return ralph_lp_get_num_cons((const RalphLPModel *)model);
}

int ralph_test_get_num_integers(const RalphModel *model) {
    return ralph_lp_get_num_integer_vars((const RalphLPModel *)model);
}

int ralph_test_is_mip(const RalphModel *model) {
    return ralph_test_model_is_mip(model) ? 1 : 0;
}

int ralph_test_get_var_bounds(const RalphModel *model, int var, double *lb, double *ub) {
    return ralph_lp_get_var_bounds((const RalphLPModel *)model, var, lb, ub);
}

int ralph_test_optimize(RalphModel *model) {
    if (ralph_test_model_is_mip(model)) {
        return ralph_mip_optimize((RalphMIPModel *)model);
    }
    return ralph_lp_optimize((RalphLPModel *)model);
}

int ralph_test_optimize_lp(RalphModel *model) {
    return ralph_lp_optimize((RalphLPModel *)model);
}

int ralph_test_optimize_mip(RalphModel *model) {
    return ralph_mip_optimize((RalphMIPModel *)model);
}

RalphStatus ralph_test_get_status(const RalphModel *model) {
    if (ralph_test_model_is_mip(model)) {
        return (RalphStatus)ralph_mip_get_status((const RalphMIPModel *)model);
    }
    return (RalphStatus)ralph_lp_get_status((const RalphLPModel *)model);
}

double ralph_test_get_objval(const RalphModel *model) {
    if (ralph_test_model_is_mip(model)) {
        return ralph_mip_get_objval((const RalphMIPModel *)model);
    }
    return ralph_lp_get_objval((const RalphLPModel *)model);
}

int ralph_test_get_solution(const RalphModel *model, double *x) {
    if (ralph_test_model_is_mip(model)) {
        return ralph_mip_get_solution((const RalphMIPModel *)model, x);
    }
    return ralph_lp_get_solution((const RalphLPModel *)model, x);
}

int ralph_test_get_dual_solution(const RalphModel *model, double *y) {
    return ralph_lp_get_dual_solution((const RalphLPModel *)model, y);
}

int ralph_test_get_reduced_costs(const RalphModel *model, double *rc) {
    return ralph_lp_get_reduced_costs((const RalphLPModel *)model, rc);
}

int ralph_test_get_iterations(const RalphModel *model) {
    return ralph_lp_get_iterations((const RalphLPModel *)model);
}

RalphBasis* ralph_test_save_basis(const RalphModel *model) {
    return (RalphBasis *)ralph_lp_save_basis((const RalphLPModel *)model);
}

int ralph_test_load_basis(RalphModel *model, const RalphBasis *basis) {
    return ralph_lp_load_basis((RalphLPModel *)model, (const RalphLPBasis *)basis);
}

int ralph_test_get_basis_status(const RalphModel *model,
                                RalphBasisStatus *col_status,
                                RalphBasisStatus *row_status) {
    return ralph_lp_get_basis_status((const RalphLPModel *)model,
                                     (RalphLPBasisStatus *)col_status,
                                     (RalphLPBasisStatus *)row_status);
}

int ralph_test_set_basis_status(RalphModel *model,
                                const RalphBasisStatus *col_status,
                                const RalphBasisStatus *row_status) {
    return ralph_lp_set_basis_status((RalphLPModel *)model,
                                     (const RalphLPBasisStatus *)col_status,
                                     (const RalphLPBasisStatus *)row_status);
}

void ralph_test_free_basis(RalphBasis *basis) {
    ralph_lp_free_basis((RalphLPBasis *)basis);
}

int ralph_test_write_basis_file(const RalphBasis *basis, const char *filename) {
    return ralph_lp_write_basis_file((const RalphLPBasis *)basis, filename);
}

RalphBasis* ralph_test_read_basis_file(const char *filename) {
    return (RalphBasis *)ralph_lp_read_basis_file(filename);
}

int ralph_test_set_int_param(RalphModel *model, const char *name, int value) {
    int rc = ralph_lp_set_int_param((RalphLPModel *)model, name, value);
    if (rc == 0) return 0;
    return ralph_mip_set_int_param((RalphMIPModel *)model, name, value);
}

int ralph_test_set_dbl_param(RalphModel *model, const char *name, double value) {
    int rc = ralph_lp_set_dbl_param((RalphLPModel *)model, name, value);
    if (rc == 0) return 0;
    return ralph_mip_set_dbl_param((RalphMIPModel *)model, name, value);
}

int ralph_test_get_int_param(const RalphModel *model, const char *name, int *value) {
    int rc = ralph_lp_get_int_param((const RalphLPModel *)model, name, value);
    if (rc == 0) return 0;
    return ralph_mip_get_int_param((const RalphMIPModel *)model, name, value);
}

int ralph_test_get_dbl_param(const RalphModel *model, const char *name, double *value) {
    int rc = ralph_lp_get_dbl_param((const RalphLPModel *)model, name, value);
    if (rc == 0) return 0;
    return ralph_mip_get_dbl_param((const RalphMIPModel *)model, name, value);
}

int ralph_test_set_lp_int_param(RalphModel *model, const char *name, int value) {
    return ralph_lp_set_int_param((RalphLPModel *)model, name, value);
}

int ralph_test_set_lp_dbl_param(RalphModel *model, const char *name, double value) {
    return ralph_lp_set_dbl_param((RalphLPModel *)model, name, value);
}

int ralph_test_get_lp_int_param(const RalphModel *model, const char *name, int *value) {
    return ralph_lp_get_int_param((const RalphLPModel *)model, name, value);
}

int ralph_test_get_lp_dbl_param(const RalphModel *model, const char *name, double *value) {
    return ralph_lp_get_dbl_param((const RalphLPModel *)model, name, value);
}

int ralph_test_set_mip_int_param(RalphModel *model, const char *name, int value) {
    return ralph_mip_set_int_param((RalphMIPModel *)model, name, value);
}

int ralph_test_set_mip_dbl_param(RalphModel *model, const char *name, double value) {
    return ralph_mip_set_dbl_param((RalphMIPModel *)model, name, value);
}

int ralph_test_get_mip_int_param(const RalphModel *model, const char *name, int *value) {
    return ralph_mip_get_int_param((const RalphMIPModel *)model, name, value);
}

int ralph_test_get_mip_dbl_param(const RalphModel *model, const char *name, double *value) {
    return ralph_mip_get_dbl_param((const RalphMIPModel *)model, name, value);
}

int ralph_test_read_mps(RalphModel *model, const char *filename) {
    return ralph_lp_read_mps((RalphLPModel *)model, filename);
}

int ralph_test_write_mps(const RalphModel *model, const char *filename) {
    return ralph_lp_write_mps((const RalphLPModel *)model, filename);
}

int ralph_test_read_lp(RalphModel *model, const char *filename) {
    return ralph_lp_read_lp((RalphLPModel *)model, filename);
}

int ralph_test_write_lp(const RalphModel *model, const char *filename) {
    return ralph_lp_write_lp((const RalphLPModel *)model, filename);
}

int ralph_test_write_solution(const RalphModel *model, const char *filename) {
    return ralph_lp_write_solution((const RalphLPModel *)model, filename);
}

int ralph_test_write_solution_buf(const RalphModel *model, char *buf, size_t buf_size) {
    return ralph_lp_write_solution_buf((const RalphLPModel *)model, buf, buf_size);
}

double ralph_test_get_best_bound(const RalphModel *model) {
    return ralph_mip_get_best_bound((const RalphMIPModel *)model);
}

double ralph_test_get_mip_gap(const RalphModel *model) {
    return ralph_mip_get_gap((const RalphMIPModel *)model);
}

int ralph_test_get_node_count(const RalphModel *model) {
    return ralph_mip_get_node_count((const RalphMIPModel *)model);
}

int ralph_test_set_branch_priorities(RalphModel *model, const int *priorities) {
    return ralph_mip_set_branch_priorities((RalphMIPModel *)model, priorities);
}

int ralph_test_set_branch_directions(RalphModel *model, const int *directions) {
    return ralph_mip_set_branch_directions((RalphMIPModel *)model,
                                           (const RalphMIPBranchDirection *)directions);
}

int ralph_test_set_mip_start(RalphModel *model, const double *x) {
    return ralph_mip_set_start((RalphMIPModel *)model, x);
}

int ralph_test_set_mip_start_sparse(RalphModel *model, int count,
                                    const int *indices, const double *values) {
    return ralph_mip_set_start_sparse((RalphMIPModel *)model, count, indices, values);
}

void ralph_test_clear_mip_start(RalphModel *model) {
    ralph_mip_clear_start((RalphMIPModel *)model);
}

RalphMIPStartStatus ralph_test_get_mip_start_status(const RalphModel *model) {
    return (RalphMIPStartStatus)ralph_mip_get_start_status((const RalphMIPModel *)model);
}

int ralph_test_set_mip_start_repair_mode(RalphModel *model, RalphMIPStartRepairMode mode) {
    return ralph_mip_set_start_repair_mode((RalphMIPModel *)model,
                                           (RalphMIPStartRepairPolicy)mode);
}

RalphMIPStartRepairMode ralph_test_get_mip_start_repair_mode(const RalphModel *model) {
    return (RalphMIPStartRepairMode)ralph_mip_get_start_repair_mode((const RalphMIPModel *)model);
}

int ralph_test_write_mip_start_file(const RalphModel *model, const char *filename) {
    return ralph_mip_write_start_file((const RalphMIPModel *)model, filename);
}

int ralph_test_read_mip_start_file(RalphModel *model, const char *filename) {
    return ralph_mip_read_start_file((RalphMIPModel *)model, filename);
}

void ralph_test_set_cut_callback(RalphModel *model, const RalphCutCallback *callback) {
    ralph_core_set_cut_callback(model, callback);
}

void ralph_test_set_branch_callback(RalphModel *model, const RalphBranchCallback *callback) {
    ralph_core_set_branch_callback(model, callback);
}

int ralph_test_solve_benders(RalphModel *model,
                             const RalphBendersConfig *config,
                             double *x,
                             RalphBendersResult *result) {
    return ralph_mip_solve_benders((RalphMIPModel *)model,
                                   (const RalphMIPBendersConfig *)config,
                                   x,
                                   (RalphMIPBendersResult *)result);
}

const char* ralph_test_status_string(RalphStatus status) {
    return ralph_lp_status_string((RalphLPStatus)status);
}

const char* ralph_test_version(void) {
    return ralph_lp_version();
}
