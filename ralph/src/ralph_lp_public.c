/*
 * Ralph LP modular public API wrappers.
 *
 * These wrappers expose a stable LP-only symbol namespace (`ralph_lp_*`)
 * while reusing the existing core implementation.
 */

#include <stdlib.h>

#include "ralph_core.h"
#include "ralph_lp.h"

static int ralph_lp_convert_var_types(int count,
                                      const RalphLPVarType *src,
                                      RalphVarType **dst_out) {
    int i;
    RalphVarType *dst;

    if (!dst_out) return -1;
    *dst_out = NULL;
    if (!src) return 0;
    if (count <= 0) return -1;

    dst = (RalphVarType *)malloc((size_t)count * sizeof(RalphVarType));
    if (!dst) return -1;

    for (i = 0; i < count; i++) {
        dst[i] = (RalphVarType)src[i];
    }
    *dst_out = dst;
    return 0;
}

RalphLPModel* ralph_lp_create(void) {
    return (RalphLPModel *)ralph_create();
}

void ralph_lp_free(RalphLPModel *model) {
    ralph_free((RalphModel *)model);
}

int ralph_lp_set_obj_sense(RalphLPModel *model, RalphLPObjSense sense) {
    return ralph_set_obj_sense((RalphModel *)model, (RalphObjSense)sense);
}

int ralph_lp_add_var(RalphLPModel *model, double lb, double ub, double obj, RalphLPVarType type) {
    return ralph_add_var((RalphModel *)model, lb, ub, obj, (RalphVarType)type);
}

int ralph_lp_add_vars(RalphLPModel *model, int count, const double *lb, const double *ub,
                      const double *obj, const RalphLPVarType *types) {
    RalphVarType *types_conv = NULL;
    int ret;

    if (ralph_lp_convert_var_types(count, types, &types_conv) != 0) return -1;
    ret = ralph_add_vars((RalphModel *)model, count, lb, ub, obj, types_conv);
    free(types_conv);
    return ret;
}

int ralph_lp_add_constraint(RalphLPModel *model, int nnz, const int *indices,
                            const double *values, RalphLPSense sense, double rhs) {
    return ralph_add_constraint((RalphModel *)model, nnz, indices, values, (RalphSense)sense, rhs);
}

int ralph_lp_set_var_bounds(RalphLPModel *model, int var, double lb, double ub) {
    return ralph_set_var_bounds((RalphModel *)model, var, lb, ub);
}

int ralph_lp_set_var_type(RalphLPModel *model, int var, RalphLPVarType type) {
    return ralph_set_var_type((RalphModel *)model, var, (RalphVarType)type);
}

int ralph_lp_set_obj_coef(RalphLPModel *model, int var, double coef) {
    return ralph_set_obj_coef((RalphModel *)model, var, coef);
}

int ralph_lp_set_obj_offset(RalphLPModel *model, double offset) {
    return ralph_set_obj_offset((RalphModel *)model, offset);
}

double ralph_lp_get_obj_offset(const RalphLPModel *model) {
    return ralph_get_obj_offset((const RalphModel *)model);
}

int ralph_lp_set_var_name(RalphLPModel *model, int var, const char *name) {
    return ralph_set_var_name((RalphModel *)model, var, name);
}

int ralph_lp_set_con_name(RalphLPModel *model, int con, const char *name) {
    return ralph_set_con_name((RalphModel *)model, con, name);
}

const char* ralph_lp_get_var_name(const RalphLPModel *model, int var) {
    return ralph_get_var_name((const RalphModel *)model, var);
}

const char* ralph_lp_get_con_name(const RalphLPModel *model, int con) {
    return ralph_get_con_name((const RalphModel *)model, con);
}

int ralph_lp_get_num_vars(const RalphLPModel *model) {
    return ralph_get_num_vars((const RalphModel *)model);
}

int ralph_lp_get_num_cons(const RalphLPModel *model) {
    return ralph_get_num_cons((const RalphModel *)model);
}

int ralph_lp_get_num_integer_vars(const RalphLPModel *model) {
    return ralph_get_num_integers((const RalphModel *)model);
}

int ralph_lp_get_var_bounds(const RalphLPModel *model, int var, double *lb, double *ub) {
    return ralph_get_var_bounds((const RalphModel *)model, var, lb, ub);
}

int ralph_lp_optimize(RalphLPModel *model) {
    return ralph_optimize_lp((RalphModel *)model);
}

RalphLPStatus ralph_lp_get_status(const RalphLPModel *model) {
    return (RalphLPStatus)ralph_get_status((const RalphModel *)model);
}

double ralph_lp_get_objval(const RalphLPModel *model) {
    return ralph_get_objval((const RalphModel *)model);
}

int ralph_lp_get_solution(const RalphLPModel *model, double *x) {
    return ralph_get_solution((const RalphModel *)model, x);
}

int ralph_lp_get_dual_solution(const RalphLPModel *model, double *y) {
    return ralph_get_dual_solution((const RalphModel *)model, y);
}

int ralph_lp_get_reduced_costs(const RalphLPModel *model, double *rc) {
    return ralph_get_reduced_costs((const RalphModel *)model, rc);
}

int ralph_lp_get_iterations(const RalphLPModel *model) {
    return ralph_get_iterations((const RalphModel *)model);
}

RalphLPBasis* ralph_lp_save_basis(const RalphLPModel *model) {
    return (RalphLPBasis *)ralph_save_basis((const RalphModel *)model);
}

int ralph_lp_load_basis(RalphLPModel *model, const RalphLPBasis *basis) {
    return ralph_load_basis((RalphModel *)model, (const RalphBasis *)basis);
}

int ralph_lp_get_basis_status(const RalphLPModel *model,
                              RalphLPBasisStatus *col_status,
                              RalphLPBasisStatus *row_status) {
    RalphBasisStatus *tmp_col = NULL;
    RalphBasisStatus *tmp_row = NULL;
    int n;
    int m;
    int i;
    int ret;

    if (!model) return -1;
    if (!col_status && !row_status) return -1;

    n = ralph_get_num_vars((const RalphModel *)model);
    m = ralph_get_num_cons((const RalphModel *)model);
    if (n < 0 || m < 0) return -1;

    if (col_status) {
        if (n > 0) {
            tmp_col = (RalphBasisStatus *)malloc((size_t)n * sizeof(RalphBasisStatus));
            if (!tmp_col) return -1;
        }
    }
    if (row_status) {
        if (m > 0) {
            tmp_row = (RalphBasisStatus *)malloc((size_t)m * sizeof(RalphBasisStatus));
            if (!tmp_row) {
                free(tmp_col);
                return -1;
            }
        }
    }

    ret = ralph_get_basis_status((const RalphModel *)model, tmp_col, tmp_row);
    if (ret == 0) {
        if (col_status) {
            for (i = 0; i < n; i++) col_status[i] = (RalphLPBasisStatus)tmp_col[i];
        }
        if (row_status) {
            for (i = 0; i < m; i++) row_status[i] = (RalphLPBasisStatus)tmp_row[i];
        }
    }

    free(tmp_col);
    free(tmp_row);
    return ret;
}

int ralph_lp_set_basis_status(RalphLPModel *model,
                              const RalphLPBasisStatus *col_status,
                              const RalphLPBasisStatus *row_status) {
    RalphBasisStatus *tmp_col = NULL;
    RalphBasisStatus *tmp_row = NULL;
    int n;
    int m;
    int i;
    int ret;

    if (!model) return -1;
    if (!col_status && !row_status) return -1;

    n = ralph_get_num_vars((const RalphModel *)model);
    m = ralph_get_num_cons((const RalphModel *)model);
    if (n < 0 || m < 0) return -1;

    if (col_status) {
        if (n > 0) {
            tmp_col = (RalphBasisStatus *)malloc((size_t)n * sizeof(RalphBasisStatus));
            if (!tmp_col) return -1;
            for (i = 0; i < n; i++) tmp_col[i] = (RalphBasisStatus)col_status[i];
        }
    }
    if (row_status) {
        if (m > 0) {
            tmp_row = (RalphBasisStatus *)malloc((size_t)m * sizeof(RalphBasisStatus));
            if (!tmp_row) {
                free(tmp_col);
                return -1;
            }
            for (i = 0; i < m; i++) tmp_row[i] = (RalphBasisStatus)row_status[i];
        }
    }

    ret = ralph_set_basis_status((RalphModel *)model, tmp_col, tmp_row);
    free(tmp_col);
    free(tmp_row);
    return ret;
}

void ralph_lp_free_basis(RalphLPBasis *basis) {
    ralph_free_basis((RalphBasis *)basis);
}

int ralph_lp_write_basis_file(const RalphLPBasis *basis, const char *filename) {
    return ralph_write_basis_file((const RalphBasis *)basis, filename);
}

RalphLPBasis* ralph_lp_read_basis_file(const char *filename) {
    return (RalphLPBasis *)ralph_read_basis_file(filename);
}

int ralph_lp_set_int_param(RalphLPModel *model, const char *name, int value) {
    return ralph_set_lp_int_param((RalphModel *)model, name, value);
}

int ralph_lp_set_dbl_param(RalphLPModel *model, const char *name, double value) {
    return ralph_set_lp_dbl_param((RalphModel *)model, name, value);
}

int ralph_lp_get_int_param(const RalphLPModel *model, const char *name, int *value) {
    return ralph_get_lp_int_param((const RalphModel *)model, name, value);
}

int ralph_lp_get_dbl_param(const RalphLPModel *model, const char *name, double *value) {
    return ralph_get_lp_dbl_param((const RalphModel *)model, name, value);
}

int ralph_lp_read_mps(RalphLPModel *model, const char *filename) {
    return ralph_read_mps((RalphModel *)model, filename);
}

int ralph_lp_write_mps(const RalphLPModel *model, const char *filename) {
    return ralph_write_mps((const RalphModel *)model, filename);
}

int ralph_lp_read_lp(RalphLPModel *model, const char *filename) {
    return ralph_read_lp((RalphModel *)model, filename);
}

int ralph_lp_write_lp(const RalphLPModel *model, const char *filename) {
    return ralph_write_lp((const RalphModel *)model, filename);
}

int ralph_lp_write_solution(const RalphLPModel *model, const char *filename) {
    return ralph_write_solution((const RalphModel *)model, filename);
}

int ralph_lp_write_solution_buf(const RalphLPModel *model, char *buf, size_t buf_size) {
    return ralph_write_solution_buf((const RalphModel *)model, buf, buf_size);
}

const char* ralph_lp_status_string(RalphLPStatus status) {
    return ralph_status_string((RalphStatus)status);
}

const char* ralph_lp_version(void) {
    return ralph_version();
}
