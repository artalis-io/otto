/*
 * Ralph - A Linear Programming and Mixed Integer Programming Solver
 *
 * Public API Header
 */

#ifndef RALPH_H
#define RALPH_H

#include <stddef.h>

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
    RALPH_STATUS_ERROR = -1
} RalphStatus;

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

/* Opaque model handle */
typedef struct RalphModel RalphModel;

/* Model creation and destruction */
RalphModel* ralph_create(void);
void ralph_free(RalphModel *model);

/* Model building */
int ralph_set_obj_sense(RalphModel *model, RalphObjSense sense);
int ralph_add_var(RalphModel *model, double lb, double ub, double obj, RalphVarType type);
int ralph_add_vars(RalphModel *model, int count, const double *lb, const double *ub,
                   const double *obj, const RalphVarType *types);
int ralph_add_constraint(RalphModel *model, int nnz, const int *indices,
                         const double *values, RalphSense sense, double rhs);

/* Model modification */
int ralph_set_var_bounds(RalphModel *model, int var, double lb, double ub);
int ralph_set_var_type(RalphModel *model, int var, RalphVarType type);
int ralph_set_obj_coef(RalphModel *model, int var, double coef);

/* Model queries */
int ralph_get_num_vars(const RalphModel *model);
int ralph_get_num_cons(const RalphModel *model);
int ralph_get_num_integers(const RalphModel *model);
int ralph_is_mip(const RalphModel *model);

/* Solving */
int ralph_optimize(RalphModel *model);

/* Solution retrieval */
RalphStatus ralph_get_status(const RalphModel *model);
double ralph_get_objval(const RalphModel *model);
int ralph_get_solution(const RalphModel *model, double *x);
int ralph_get_dual_solution(const RalphModel *model, double *y);
int ralph_get_reduced_costs(const RalphModel *model, double *rc);

/* MIP-specific */
double ralph_get_best_bound(const RalphModel *model);
double ralph_get_mip_gap(const RalphModel *model);
int ralph_get_node_count(const RalphModel *model);

/* Parameters */
int ralph_set_int_param(RalphModel *model, const char *name, int value);
int ralph_set_dbl_param(RalphModel *model, const char *name, double value);
int ralph_get_int_param(const RalphModel *model, const char *name, int *value);
int ralph_get_dbl_param(const RalphModel *model, const char *name, double *value);

/* File I/O */
int ralph_read_mps(RalphModel *model, const char *filename);
int ralph_write_mps(const RalphModel *model, const char *filename);
int ralph_write_solution(const RalphModel *model, const char *filename);

/* Utility */
const char* ralph_status_string(RalphStatus status);
const char* ralph_version(void);

#ifdef __cplusplus
}
#endif

#endif /* RALPH_H */
