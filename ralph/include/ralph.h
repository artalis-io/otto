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

/* Constants */
#define RALPH_INFINITY 1e30

/* Branch direction hints */
typedef enum {
    RALPH_BRANCH_AUTO = 0,   /* Solver chooses direction */
    RALPH_BRANCH_DOWN = -1,  /* Prefer branching down (x <= floor(val)) */
    RALPH_BRANCH_UP = 1      /* Prefer branching up (x >= ceil(val)) */
} RalphBranchDir;

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

/* Infeasibility certificate (Farkas ray)
 * Returns 0 on success, -1 if not available (problem not infeasible or no certificate)
 * The ray y satisfies: y'A >= 0 and y'b < 0, proving infeasibility
 * Array must be pre-allocated with size >= ralph_get_num_cons(model) */
int ralph_get_farkas_ray(const RalphModel *model, double *ray);

/* Statistics */
int ralph_get_iterations(const RalphModel *model);

/* MIP-specific */
double ralph_get_best_bound(const RalphModel *model);
double ralph_get_mip_gap(const RalphModel *model);
int ralph_get_node_count(const RalphModel *model);

/* Branching control (MIP)
 *
 * These functions control variable selection during branch and bound.
 * Must be called before ralph_optimize(). Arrays are copied internally.
 */

/* Set branching priorities for integer variables.
 * Higher priority variables are branched on first.
 * @param model      The model
 * @param priorities Array of priorities (size = num_vars). NULL to clear.
 * @return 0 on success, -1 on error
 */
int ralph_set_branch_priorities(RalphModel *model, const int *priorities);

/* Set preferred branch direction for integer variables.
 * @param model      The model
 * @param directions Array of RalphBranchDir values (size = num_vars). NULL to clear.
 * @return 0 on success, -1 on error
 */
int ralph_set_branch_directions(RalphModel *model, const int *directions);

/* Constraint modification (for Benders decomposition, cut loops)
 *
 * These functions allow modifying the model between solves. After modification,
 * call ralph_optimize() to re-solve. The solver will attempt warm start.
 */

/* Modify RHS of existing constraint.
 * @param model      The model
 * @param constraint Constraint index (0 to num_cons-1)
 * @param rhs        New right-hand side value
 * @return 0 on success, -1 on error (invalid constraint index)
 */
int ralph_set_constraint_rhs(RalphModel *model, int constraint, double rhs);

/* Query variable bounds.
 * @param model The model
 * @param var   Variable index (0 to num_vars-1)
 * @param lb    Output: lower bound (may be NULL if not needed)
 * @param ub    Output: upper bound (may be NULL if not needed)
 * @return 0 on success, -1 on error (invalid variable index)
 */
int ralph_get_var_bounds(const RalphModel *model, int var, double *lb, double *ub);

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
 * ralph_optimize() to re-solve. The solver will attempt warm start.
 *
 * @param model The model
 * @param cut   The cut to add
 * @return 0 on success, -1 on error
 */
int ralph_add_lazy_constraint(RalphModel *model, const RalphCut *cut);

/* Add multiple lazy constraints to the model.
 * @param model The model
 * @param cuts  Array of cuts to add
 * @param count Number of cuts
 * @return 0 on success, -1 on error
 */
int ralph_add_lazy_constraints(RalphModel *model, const RalphCut *cuts, int count);

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
 * @return Basis handle, or NULL on error. Caller must free with ralph_free_basis().
 */
RalphBasis* ralph_save_basis(const RalphModel *model);

/* Load a previously saved basis for warm start.
 * @param model The model
 * @param basis The basis to load
 * @return 0 on success, -1 on error (e.g., dimensions mismatch)
 */
int ralph_load_basis(RalphModel *model, const RalphBasis *basis);

/* Free a saved basis.
 * @param basis The basis to free (may be NULL)
 */
void ralph_free_basis(RalphBasis *basis);

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
void ralph_set_cut_callback(RalphModel *model, const RalphCutCallback *callback);

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
void ralph_set_branch_callback(RalphModel *model, const RalphBranchCallback *callback);

/* Parameters */
int ralph_set_int_param(RalphModel *model, const char *name, int value);
int ralph_set_dbl_param(RalphModel *model, const char *name, double value);
int ralph_get_int_param(const RalphModel *model, const char *name, int *value);
int ralph_get_dbl_param(const RalphModel *model, const char *name, double *value);

/* File I/O */
int ralph_read_mps(RalphModel *model, const char *filename);
int ralph_write_mps(const RalphModel *model, const char *filename);
int ralph_read_lp(RalphModel *model, const char *filename);
int ralph_write_lp(const RalphModel *model, const char *filename);
int ralph_write_solution(const RalphModel *model, const char *filename);

/* Write solution to buffer in SOL format.
 * @param model    Solved model
 * @param buf      Output buffer
 * @param buf_size Size of buffer
 * @return Number of bytes written (not including null terminator), or -1 on error.
 *         If return value >= buf_size, output was truncated.
 */
int ralph_write_solution_buf(const RalphModel *model, char *buf, size_t buf_size);

/* Name management */
const char* ralph_get_var_name(const RalphModel *model, int var);
const char* ralph_get_con_name(const RalphModel *model, int con);
int ralph_set_var_name(RalphModel *model, int var, const char *name);
int ralph_set_con_name(RalphModel *model, int con, const char *name);
const char* ralph_get_problem_name(const RalphModel *model);
int ralph_set_problem_name(RalphModel *model, const char *name);

/* Utility */
const char* ralph_status_string(RalphStatus status);
const char* ralph_version(void);

#ifdef __cplusplus
}
#endif

#endif /* RALPH_H */
