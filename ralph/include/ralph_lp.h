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
