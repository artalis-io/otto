/*
 * Ralph - MIP Solver Internals
 */

#ifndef RALPH_MIP_H
#define RALPH_MIP_H

#include "lp.h"

/* Default MIP parameters */
#define RALPH_DEFAULT_NODE_LIMIT 1000000
#define RALPH_DEFAULT_MIP_GAP 0.0001    /* 0.01% relative gap */
#define RALPH_DEFAULT_ABS_MIP_GAP 1e-6  /* 1e-6 absolute gap */
#define RALPH_DEFAULT_CUTOFF RALPH_INFINITY

/* Node selection strategy */
typedef enum {
    NODE_SELECT_BEST_FIRST = 0,
    NODE_SELECT_DEPTH_FIRST = 1,
    NODE_SELECT_BEST_ESTIMATE = 2,
    NODE_SELECT_HYBRID = 3
} NodeSelectStrategy;

/* Variable selection strategy */
typedef enum {
    VAR_SELECT_MAX_INFEAS = 0,
    VAR_SELECT_PSEUDO_COST = 1,
    VAR_SELECT_STRONG_BRANCH = 2,
    VAR_SELECT_RELIABILITY = 3
} VarSelectStrategy;

/* Branch direction */
typedef enum {
    BRANCH_DOWN = 0,
    BRANCH_UP = 1
} BranchDir;

/* Cut types */
typedef enum {
    CUT_GOMORY = 0,
    CUT_MIR = 1,
    CUT_CLIQUE = 2,
    CUT_KNAPSACK = 3
} CutType;

/* A cutting plane */
typedef struct {
    int nnz;
    int capacity;       /* Allocated capacity */
    int *indices;
    double *values;
    char sense;
    double rhs;
    CutType type;
    double violation;
    int age;            /* Rounds since last binding */
} Cut;

/* Cut pool */
typedef struct {
    int capacity;
    int count;
    Cut **cuts;
} CutPool;

/* Branch and bound node */
typedef struct BBNode {
    int id;
    int depth;
    int parent_id;
    BranchDir branch_dir;
    int branch_var;
    double branch_val;

    /* Node bounds */
    double *lb;         /* Variable lower bounds at this node */
    double *ub;         /* Variable upper bounds at this node */

    /* LP relaxation info */
    double lp_bound;
    int lp_status;
    int lp_iterations;

    /* For best estimate node selection */
    double estimate;

    /* Basis information for warm start */
    int *basis;
    VarStatus *var_status;
    int basis_size;         /* Size of basis array (m = num constraints) */
    int var_status_size;    /* Size of var_status array (n = extended vars) */

} BBNode;

/* Node queue (priority queue) */
typedef struct {
    int capacity;
    int size;
    BBNode **nodes;
    NodeSelectStrategy strategy;
    int obj_sense;      /* For comparison direction */
} NodeQueue;

/* MIP solver state */
typedef struct {
    LPModel *original_model;
    LPModel *working_model;
    SimplexSolver *lp_solver;

    /* Integer variable information */
    int num_integers;
    int *integer_vars;  /* Indices of integer variables */
    int *is_integer;    /* Boolean array */

    /* Best solutions */
    double best_bound;      /* Best dual bound (LP relaxation) */
    double best_obj;        /* Best primal bound (incumbent) */
    double *best_solution;  /* Best integer solution found */
    int has_incumbent;

    /* Node management */
    NodeQueue *node_queue;
    int node_count;
    int nodes_explored;
    int max_depth;

    /* Cut pool */
    CutPool *cut_pool;
    int cuts_generated;
    int cuts_applied;

    /* Pseudo-costs for branching */
    double *pseudo_cost_down;
    double *pseudo_cost_up;
    int *pseudo_count_down;
    int *pseudo_count_up;

    /* Parameters */
    int max_nodes;
    double time_limit;
    double mip_gap;
    double abs_mip_gap;
    double cutoff;
    NodeSelectStrategy node_select;
    VarSelectStrategy var_select;
    int max_cuts_per_round;
    int max_cut_rounds;
    int verbose;

    /* Statistics */
    RalphStatus status;
    double root_bound;
    int root_iterations;
    double solve_time;

} MIPSolver;

/* MIP solver functions */
MIPSolver* mip_create(LPModel *model);
void mip_free(MIPSolver *solver);
int mip_solve(MIPSolver *solver);

/* Node management */
NodeQueue* node_queue_create(int capacity, NodeSelectStrategy strategy, int obj_sense);
void node_queue_free(NodeQueue *queue);
int node_queue_push(NodeQueue *queue, BBNode *node);
BBNode* node_queue_pop(NodeQueue *queue);
int node_queue_is_empty(const NodeQueue *queue);
void node_queue_update_bound(NodeQueue *queue, double cutoff);
double node_queue_best_bound(const NodeQueue *queue);

BBNode* bb_node_create(int num_vars);
void bb_node_free(BBNode *node);
BBNode* bb_node_copy(const BBNode *node, int num_vars);

/* Branching */
int select_branch_variable(MIPSolver *solver, const double *solution, int *branch_var);
void compute_branch_children(MIPSolver *solver, BBNode *parent, int branch_var,
                            BBNode **child_down, BBNode **child_up);

/* Strong branching */
int strong_branch(MIPSolver *solver, int var, double val,
                  double *down_obj, double *up_obj, int max_iter);

/* Pseudo-cost branching */
void update_pseudo_costs(MIPSolver *solver, int var, double val,
                        double parent_obj, double child_obj, BranchDir dir);
double estimate_branch_obj(MIPSolver *solver, int var, double val, BranchDir dir);

/* Cutting planes */
CutPool* cut_pool_create(int capacity);
void cut_pool_free(CutPool *pool);
Cut* cut_create(int max_nnz);
void cut_free(Cut *cut);
int cut_pool_add(CutPool *pool, Cut *cut);

/* Cut pool management */
void cut_pool_cleanup(CutPool *pool, int max_age);
void cut_pool_age(CutPool *pool);
int cut_pool_update_efficacy(CutPool *pool, const double *x, int n);
void cut_pool_clear(CutPool *pool);

/* Cut generation */
int generate_gomory_cuts(MIPSolver *solver, CutPool *pool);
int generate_mir_cuts(MIPSolver *solver, CutPool *pool);
int apply_cuts(MIPSolver *solver, CutPool *pool, int max_cuts);

/* Primal heuristics */
int heuristic_rounding(MIPSolver *solver, const double *lp_solution, double *int_solution);
int heuristic_feasibility_pump(MIPSolver *solver, double *solution);

/* Solution checking */
int check_integer_feasibility(MIPSolver *solver, const double *solution);
double compute_integrality_violation(MIPSolver *solver, const double *solution);

/* Probing */
int probing_bound_tightening(MIPSolver *solver);

/* Utility */
void mip_print_stats(const MIPSolver *solver);
void mip_print_node_info(const MIPSolver *solver, const BBNode *node);

#endif /* RALPH_MIP_H */
