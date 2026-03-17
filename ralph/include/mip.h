/*
 * Ralph - MIP Solver Internals
 */

#ifndef RALPH_MIP_H
#define RALPH_MIP_H

#include "lp.h"
#include "detect.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default MIP parameters */
#define RALPH_DEFAULT_NODE_LIMIT 1000000
#define RALPH_DEFAULT_MIP_GAP 0.0001    /* 0.01% relative gap */
#define RALPH_DEFAULT_ABS_MIP_GAP 1e-6  /* 1e-6 absolute gap */
#define RALPH_DEFAULT_CUTOFF RALPH_INFINITY

/* Reliability branching parameters */
#define MIP_RELIABILITY_THRESHOLD   4    /* Strong-branch until this many observations */
#define MIP_RELIABILITY_MAX_STRONG  2    /* Max strong-branch evaluations per node */
#define MIP_RELIABILITY_PIVOT_BUDGET 64  /* Dual pivots per strong-branch probe */
#define MIP_RELIABILITY_NO_INCUMBENT_TAPER_AFTER 1024   /* Explored nodes before pre-incumbent taper */
#define MIP_RELIABILITY_NO_INCUMBENT_DISABLE_AFTER 4096 /* Explored nodes before pre-incumbent strong-branch off */
#define MIP_RELIABILITY_NO_INCUMBENT_PIVOT_BUDGET 48    /* Reduced pivot budget in tapered pre-incumbent mode */
#define MIP_RELIABILITY_POST_INCUMBENT_PROBE_NODES 64   /* Probe only during early post-incumbent bootstrap */
#define MIP_RELIABILITY_POST_INCUMBENT_MAX_STRONG 1     /* Keep post-incumbent probing cheap */
#define MIP_RELIABILITY_POST_INCUMBENT_PIVOT_BUDGET 32  /* Keep post-incumbent probes short */
#define MIP_PROBE_HANDOFF_NO_INCUMBENT_PROBE_NODES 128  /* Only keep child probe states during early tree bootstrap */
#define MIP_PROBE_HANDOFF_NO_INCUMBENT_TAPER_AFTER 64   /* Shrink handoff probe budget after early nodes */
#define MIP_PROBE_HANDOFF_PIVOT_BUDGET 16               /* Child-state handoff only needs a short dual polish */
#define MIP_PROBE_HANDOFF_TAPERED_PIVOT_BUDGET 8        /* Tighter cap once the tree is established */
#define MIP_PROBE_HANDOFF_POST_INCUMBENT_PROBE_NODES 32 /* Very limited handoff after an incumbent exists */
#define MIP_PROBE_HANDOFF_POST_INCUMBENT_PIVOT_BUDGET 8 /* Keep post-incumbent handoff probes minimal */
#define MIP_RELIABILITY_CANDIDATE_LIMIT 8               /* Only probe the top pseudo-cost candidates */
#define MIP_NON_ROOT_CUT_MAX_DEPTH 4                    /* Only separate early/shallow nodes */
#define MIP_NON_ROOT_CUT_MAX_NODES 128                  /* Stop non-root separation after early tree bootstrap */
#define MIP_NON_ROOT_CUT_MAX_APPLY 8                    /* Keep each non-root cut pass small */

/* Cut quality filter parameters */
#define MIP_CUT_MIN_VIOLATION  1e-4   /* Minimum violation to apply a cut */
#define MIP_CUT_MAX_DYNAMISM   1e6    /* Max coefficient ratio max|a|/min|a| */
#define MIP_CUT_PARALLEL_TOL   0.999  /* Cosine similarity threshold for parallel cuts */

/* Root cut-family mask bits */
#define MIP_ROOT_CUT_GOMORY_MASK (1 << 0)
#define MIP_ROOT_CUT_MIR_MASK    (1 << 1)
#define MIP_ROOT_CUT_COVER_MASK  (1 << 2)
#define MIP_ROOT_CUT_SCP_MASK    (1 << 3)
#define MIP_ROOT_CUT_ALL_MASK \
    (MIP_ROOT_CUT_GOMORY_MASK | MIP_ROOT_CUT_MIR_MASK | \
     MIP_ROOT_CUT_COVER_MASK | MIP_ROOT_CUT_SCP_MASK)

/* Reduced-cost fixing parameters */
#define MIP_RC_FIX_MIN_GAP     1e-4   /* Don't fix when gap is numerically tiny */

/* RINS (Relaxation Induced Neighborhood Search) parameters */
#define MIP_RINS_INTERVAL       100   /* Nodes between RINS calls */
#define MIP_RINS_INTERVAL_SMALL  50   /* For problems with < 50 integers */
#define MIP_RINS_MAX_DIVE        30   /* Max dive depth in RINS */
#define MIP_RINS_MIN_FREE_PCT   0.1   /* Skip if < 10% integers free */
#define MIP_RINS_LP_ITER_LIMIT  200   /* LP iterations per RINS dive step */

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
    VAR_SELECT_RELIABILITY = 3,
    VAR_SELECT_SCP = 4          /* SCP-specific constraint branching */
} VarSelectStrategy;

/* Branch direction */
typedef enum {
    BRANCH_DOWN = 0,
    BRANCH_UP = 1
} BranchDir;

/* Saved-basis provenance for node hot starts. */
typedef enum {
    NODE_BASIS_SOURCE_NONE = 0,
    NODE_BASIS_SOURCE_RELAXATION = 1,
    NODE_BASIS_SOURCE_STRONG_PROBE = 2
} NodeBasisSource;

/* Cut types */
typedef enum {
    CUT_GOMORY = 0,
    CUT_MIR = 1,
    CUT_CLIQUE = 2,
    CUT_KNAPSACK = 3,
    CUT_ODD_HOLE = 4,
    CUT_LIFTED_COVER = 5
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
    int basis_source;       /* NodeBasisSource */

} BBNode;

/* Node pool for efficient allocation/deallocation of B&B nodes.
 * Pre-allocates a block of nodes to reduce malloc overhead in deep trees. */
typedef struct {
    int capacity;           /* Total nodes in pool */
    int num_vars;           /* Size of lb/ub arrays per node */
    BBNode *nodes;          /* Pre-allocated node structures */
    double *lb_pool;        /* Contiguous lb arrays for all nodes */
    double *ub_pool;        /* Contiguous ub arrays for all nodes */
    int *free_list;         /* Stack of free node indices */
    int free_count;         /* Number of free nodes */
    int nodes_allocated;    /* High-water mark for stats */
} BBNodePool;

/* Node queue (priority queue) */
typedef struct {
    int capacity;
    int size;
    BBNode **nodes;
    NodeSelectStrategy strategy;
    int obj_sense;      /* For comparison direction */
    int has_incumbent;  /* For HYBRID: 0=depth-first phase, 1=best-first phase */
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

    /* User-provided MIP incumbent start (full vector, original model space) */
    double *mip_start;
    int *mip_start_mask;     /* 1=specified by user, 0=imputed/default */
    int mip_start_n;
    int mip_start_nnz;
    int mip_start_repair_mode; /* RalphMIPStartRepairMode */
    int mip_start_attempted;
    int mip_start_accepted;

    /* Node management */
    NodeQueue *node_queue;
    BBNodePool *node_pool;  /* Memory pool for node allocation */
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

    /* User-provided branching control */
    int *branch_priorities;     /* Priority for each variable (higher = branch first) */
    int *branch_directions;     /* Preferred direction: -1=down, 0=auto, 1=up */

    /* User-provided cut callback */
    RalphCutCallback cut_callback;
    int has_cut_callback;

    /* User-provided branching callback */
    RalphBranchCallback branch_callback;
    int has_branch_callback;

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
    int root_cut_mask;
    int verbose;
    int telemetry;          /* 1 = collect LP/LU telemetry in node relaxations */
    int dual_bound_flip;    /* -1=default(on), 0=off, 1=on */
    int dual_steepest_edge; /* -1=default(on), 0=off, 1=on */
    int lu_supernode;       /* 0=off, 1=enable supernodal LU (T2.1) */

    /* Statistics */
    RalphStatus status;
    double root_bound;
    int root_iterations;
    double solve_time;
    double root_lp_time_ms;
    double node_lp_time_ms;
    double node_lp_warm_time_ms;
    double node_lp_cold_time_ms;
    double strong_branch_time_ms;
    double root_cut_time_ms;
    double non_root_cut_time_ms;

    /* LAP-based solving (for assignment MIPs) */
    int use_lap_solver;          /* 1 if LAP structure detected and enabled */
    MIPLAPSignature *lap_sig;    /* LAP signature for LP relaxations */
    int lap_nodes_solved;        /* Number of nodes solved with LAP */
    int simplex_nodes_solved;    /* Number of nodes solved with simplex */
    int last_solved_node_id;     /* ID of last node whose LP was solved (for child detection) */
    int node_basis_warm_attempts; /* Live node-basis warm restore attempts */
    int node_basis_warm_applied;  /* Live node-basis warm restores applied */
    int node_basis_warm_rejected; /* Live node-basis restores rejected/fallback */
    int node_basis_staged;        /* Cold starts that staged node basis via LP warm API */
    int node_basis_stage_cooldown; /* Nodes to skip staged warm-basis after repeated rejection */
    int node_lp_cold_starts;      /* Cold-start node LP solves/re-solves */
    int probe_child_snapshots_saved;     /* Child probe bases captured after final kept probe */
    int probe_child_warm_applied;        /* Probe-sourced bases accepted by node LP reuse */
    int relaxation_basis_warm_applied;   /* Relaxation-sourced bases accepted by node LP reuse */
    int saved_basis_live_restore_attempted;      /* Saved-basis nodes that attempted live restore */
    int saved_basis_warm_reopt_succeeded;        /* Saved-basis nodes that finished on warm dual reopt */
    int saved_basis_fallback_no_tableau;         /* Saved-basis fallbacks with no live tableau */
    int saved_basis_fallback_artificial_skip;    /* Saved-basis fallbacks skipped due to artificial rows */
    int saved_basis_fallback_size_mismatch;      /* Saved-basis fallbacks skipped due to snapshot/tableau size mismatch */
    int saved_basis_fallback_live_restore_not_attempted; /* Residual saved-basis fallbacks that never attempted live restore */
    int cold_start_no_saved_basis;       /* Cold starts with no saved node basis */
    int cold_start_saved_basis_fallback; /* Saved basis existed, but node still cold-started */
    int cold_start_probe_restore_failure;/* Probe-sourced live restore failed */
    int cold_start_live_restore_failure; /* Generic live restore failed */
    int cold_start_warm_reopt_failure;   /* Warm dual reopt fell back to cold start */
    int cold_start_stage_retry;          /* Extra cold start after staged-warm rejection */
    int cold_start_branch_recovery;      /* Cold start used to recover stale branch-selector LP state */
    int warm_reject_invalid_snapshot;      /* Saved basis snapshot failed sanity checks */
    int warm_reject_restore_failure;       /* Live restore rejected by LP warm-basis API */
    int warm_reject_stage_failure;         /* Staged warm basis rejected before cold-start solve */
    int warm_reject_stage_solve_rejected;  /* Staged warm basis rejected during cold-start solve */
    int warm_reject_stage_solution_invalid;/* Staged warm solve produced invalid node solution */
    int strong_branch_probes;      /* Strong-branch probe calls */
    int strong_branch_failures;    /* Strong-branch probe calls that failed */
    int strong_branch_recoveries;  /* Failed probes that recovered LP state */
    int cut_recovery_attempts;     /* Root cut-loop LP recovery attempts */
    int cut_recovery_success;      /* Root cut-loop recoveries that succeeded */
    int cut_recovery_failures;     /* Root cut-loop recoveries that failed */
    int root_cut_rounds;           /* Root cut rounds attempted */
    int root_cuts_generated;       /* Root cuts generated */
    int root_cuts_applied;         /* Root cuts applied */
    int non_root_cut_rounds;       /* Non-root cut passes attempted */
    int non_root_cuts_generated;   /* Non-root cuts generated (user + built-in) */
    int non_root_cuts_applied;     /* Non-root cuts applied */
    int fathom_lp_infeasible;      /* Nodes pruned after non-optimal LP relaxation */
    int fathom_bound;              /* Nodes pruned by incumbent bound */
    int fathom_integral;           /* Nodes closed by integer-feasible LP solution */
    int fathom_no_branch_var;      /* Fractional nodes pruned after branch-var selection failed */

    /* Reduced-cost fixing + RINS statistics */
    int rc_fixings;              /* Total variables fixed by reduced-cost fixing */
    int rins_calls;              /* Number of RINS heuristic invocations */
    int rins_found;              /* Number of incumbents found by RINS */

    /* SCP-specific optimizations (for set covering/partitioning MIPs) */
    int use_scp_solver;          /* 1 if SCP structure detected and enabled */
    int scp_cuts_generated;      /* Number of SCP-specific cuts generated */
    double lagrangian_bound;     /* Best Lagrangian dual bound (if computed) */

} MIPSolver;

/* MIP solver functions */
MIPSolver* mip_create(LPModel *model, int detect_special, int pool_capacity);
void mip_free(MIPSolver *solver);
int mip_solve(MIPSolver *solver);
int mip_set_start(MIPSolver *solver, const double *x, int n);
int mip_set_start_ex(MIPSolver *solver, const double *x, const int *mask,
                     int n, int repair_mode);

/* Recover root LP relaxation state from the original model.
 * Contract:
 * - Rebuilds working model from original model copy.
 * - Recreates and resolves LP solver on rebuilt working model.
 * - Returns 0 only if rebuilt root LP is OPTIMAL and LP state is usable. */
int mip_recover_root_relaxation(MIPSolver *solver);

/* Node management */
NodeQueue* node_queue_create(int capacity, NodeSelectStrategy strategy, int obj_sense);
void node_queue_free(NodeQueue *queue);
void node_queue_free_with_pool(NodeQueue *queue, BBNodePool *pool);
int node_queue_push(NodeQueue *queue, BBNode *node);
BBNode* node_queue_pop(NodeQueue *queue);
int node_queue_is_empty(const NodeQueue *queue);
void node_queue_update_bound(NodeQueue *queue, double cutoff);
void node_queue_update_bound_with_pool(NodeQueue *queue, double cutoff, BBNodePool *pool);
double node_queue_best_bound(const NodeQueue *queue);
void node_queue_set_incumbent_found(NodeQueue *queue);

BBNode* bb_node_create(int num_vars);
void bb_node_free(BBNode *node);
BBNode* bb_node_copy(const BBNode *node, int num_vars);

/* Node pool for efficient allocation */
BBNodePool* bb_node_pool_create(int capacity, int num_vars);
void bb_node_pool_free(BBNodePool *pool);
BBNode* bb_node_pool_get(BBNodePool *pool);
void bb_node_pool_return(BBNodePool *pool, BBNode *node);
BBNode* bb_node_pool_copy(BBNodePool *pool, const BBNode *src, int num_vars);

/* Branching */
int select_branch_variable(MIPSolver *solver, const double *solution, int *branch_var);
void compute_branch_children(MIPSolver *solver, BBNode *parent, int branch_var,
                            BBNode **child_down, BBNode **child_up);

/* Strong branching */
int strong_branch(MIPSolver *solver, int var, double val,
                  double *down_obj, double *up_obj, int max_iter,
                  BBNode *down_node, BBNode *up_node);

/* Pseudo-cost branching */
void update_pseudo_costs(MIPSolver *solver, int var, double val,
                        double parent_obj, double child_obj, BranchDir dir);
double estimate_branch_obj(MIPSolver *solver, int var, double val, BranchDir dir);

/* SCP-specific branching (Phase 5) */

/*
 * Initialize pseudo-costs using SCP structure.
 *
 * For SCP, use cost/coverage ratio as initial estimate:
 *   pseudo_cost_down[j] = c[j] / set_size[j]  (cost per element lost)
 *   pseudo_cost_up[j] = c[j] / set_size[j]    (cost per element gained)
 *
 * This provides better initial estimates than default 1.0 values.
 *
 * Parameters:
 *   solver - MIP solver with SCP structure
 *
 * Returns:
 *   0 on success, -1 if not an SCP model.
 */
int init_pseudo_costs_scp(MIPSolver *solver);

/*
 * Select branching variable using SCP constraint branching.
 *
 * Instead of branching on the most fractional variable, this:
 * 1. Finds the element (constraint) with most fractional coverage
 * 2. Returns the covering set with highest LP value
 *
 * This tends to make better branching decisions for SCP because:
 * - Elements with fractional coverage are the "bottleneck"
 * - Branching on high LP-value sets has more impact
 *
 * Parameters:
 *   solver   - MIP solver
 *   solution - Current LP solution
 *   element  - Output: element with most fractional coverage (-1 if none)
 *   set      - Output: best set to branch on
 *
 * Returns:
 *   0 on success with valid branching decision, -1 if no branching needed.
 */
int select_scp_branch(MIPSolver *solver, const double *solution,
                      int *element, int *set);

/*
 * Select branching variable using SOS1 structure for SPP.
 *
 * For set partitioning (Ax = 1), each element defines an SOS1 constraint:
 * exactly one of the covering sets must be selected.
 *
 * This function:
 * 1. Finds an element with fractional coverage
 * 2. Partitions covering sets into two groups by LP value
 * 3. Returns the set at the partition boundary
 *
 * Parameters:
 *   solver   - MIP solver with SPP structure
 *   solution - Current LP solution
 *   set      - Output: set to branch on
 *
 * Returns:
 *   0 on success, -1 if not applicable or no branching needed.
 */
int select_sos1_branch_spp(MIPSolver *solver, const double *solution, int *set);

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
int generate_cover_cuts(MIPSolver *solver, CutPool *pool);
int apply_cuts(MIPSolver *solver, CutPool *pool, int max_cuts);

/* SCP-specific cutting planes (Phase 3) */

/*
 * Conflict graph for set covering/partitioning problems.
 * Two sets (variables) conflict if they both cover the same element.
 * In SPP (=), at most one can be selected; in SCP (>=), cliques still valid.
 */
typedef struct {
    int num_vars;           /* Number of variables (sets) */
    int *adj_ptr;           /* CSR pointers into adj_list (size num_vars+1) */
    int *adj_list;          /* Flattened adjacency lists */
    int num_edges;          /* Total edges in graph */
} ConflictGraph;

ConflictGraph *conflict_graph_create(const LPModel *model, const SetCoverSignature *sig);
void conflict_graph_free(ConflictGraph *graph);

/*
 * Generate clique cuts from the conflict graph.
 * A clique C has the property that at most one variable can be 1.
 * Cut: sum(x_j : j in C) <= 1
 *
 * Uses greedy clique extension starting from violated edges.
 * Returns number of cuts added to pool.
 */
int generate_clique_cuts(MIPSolver *solver, CutPool *pool, const ConflictGraph *graph);

/*
 * Generate odd-hole cuts from the conflict graph.
 * An odd hole is an odd cycle (length 2k+1) in the conflict graph.
 * Cut: sum(x_j : j in cycle) <= k
 *
 * Uses BFS to find shortest odd cycles.
 * Returns number of cuts added to pool.
 */
int generate_odd_hole_cuts(MIPSolver *solver, CutPool *pool, const ConflictGraph *graph);

/*
 * Generate lifted cover inequalities for SCP.
 * Strengthens basic cover cuts via sequential lifting.
 *
 * Basic cover: sum(x_j : j in C) <= |C| - 1
 * Lifted: sum(a_j * x_j) <= |C| - 1  where a_j >= 1 computed via lifting
 *
 * Returns number of cuts added to pool.
 */
int generate_lifted_cover_cuts(MIPSolver *solver, CutPool *pool);

/*
 * Combined SCP cut generation.
 * Detects SCP structure, builds conflict graph, generates all SCP cuts.
 * Returns total cuts added.
 */
int generate_scp_cuts(MIPSolver *solver, CutPool *pool);

/* Primal heuristics */
int heuristic_rounding(MIPSolver *solver, const double *lp_solution, double *int_solution);
int heuristic_feasibility_pump(MIPSolver *solver, double *solution);

/* SCP-specific primal heuristics (Phase 4) */

/*
 * Greedy set cover heuristic.
 *
 * Classic greedy algorithm: repeatedly select the set with the best
 * cost-to-coverage ratio until all elements are covered.
 *
 * Approximation ratio: O(log m) for set covering problems.
 * Complexity: O(m * n) per complete solution.
 *
 * Parameters:
 *   solver   - MIP solver with SCP structure
 *   solution - Output: binary solution (caller allocates, size = num_vars)
 *
 * Returns:
 *   0 on success (feasible solution found), -1 on failure.
 */
int heuristic_greedy_set_cover(MIPSolver *solver, double *solution);

/*
 * LP-guided greedy heuristic.
 *
 * Modified greedy that biases toward sets with high LP relaxation values.
 * Uses ratio = cost / (coverage * (1 + lp_value)) to combine:
 * - Classic greedy cost-effectiveness
 * - LP relaxation guidance
 *
 * Parameters:
 *   solver      - MIP solver with SCP structure
 *   lp_solution - Current LP relaxation solution
 *   solution    - Output: binary solution
 *
 * Returns:
 *   0 on success, -1 on failure.
 */
int heuristic_lp_guided_greedy(MIPSolver *solver, const double *lp_solution, double *solution);

/*
 * Check if model has SCP (Set Covering Problem) structure.
 *
 * An SCP has:
 * - All binary variables
 * - All >= or = constraints
 * - All 0-1 coefficients
 * - Positive RHS
 *
 * Parameters:
 *   model - LP model to check
 *
 * Returns:
 *   1 if SCP, 0 otherwise.
 */
int is_scp_model(const LPModel *model);

/*
 * Local search improvement for SCP solutions.
 *
 * Improves a feasible SCP solution via:
 * - 1-opt: Remove redundant sets that aren't needed for coverage
 * - 2-opt: Replace a set with a cheaper one that covers the same elements
 *
 * Parameters:
 *   solver   - MIP solver with SCP structure
 *   solution - In/out: feasible solution to improve
 *
 * Returns:
 *   Number of improvements made (>= 0), -1 on error.
 */
int heuristic_local_search_scp(MIPSolver *solver, double *solution);

/*
 * Combined SCP heuristic.
 *
 * Runs greedy (or LP-guided greedy if LP solution available),
 * followed by local search improvement.
 *
 * Parameters:
 *   solver      - MIP solver
 *   lp_solution - Current LP solution (can be NULL for pure greedy)
 *   solution    - Output: best solution found
 *
 * Returns:
 *   0 on success with feasible solution, -1 on failure.
 */
int heuristic_scp(MIPSolver *solver, const double *lp_solution, double *solution);

/* Solution checking */
int check_integer_feasibility(MIPSolver *solver, const double *solution);
double compute_integrality_violation(MIPSolver *solver, const double *solution);

/* Probing */
int probing_bound_tightening(MIPSolver *solver);

/* Utility */
void mip_print_stats(const MIPSolver *solver);
void mip_print_node_info(const MIPSolver *solver, const BBNode *node);

/* ============================================================================
 * Lagrangian Relaxation for SCP (Phase 6)
 * ============================================================================ */

/*
 * Lagrangian relaxation context for Set Covering Problems.
 *
 * The Lagrangian relaxation of SCP:
 *   min c'x  s.t. Ax >= 1, x in {0,1}
 *
 * With Lagrangian multipliers lambda >= 0:
 *   L(lambda) = min { c'x + lambda'(1 - Ax) : x in {0,1} }
 *             = sum(lambda) + min { (c - A'lambda)'x : x in {0,1} }
 *
 * The inner minimization is trivial: for each j, x_j = 1 if (c_j - sum(lambda_i : i in S_j)) < 0.
 *
 * Properties:
 * - L(lambda) <= optimal value (valid dual bound)
 * - max_lambda L(lambda) = Lagrangian dual bound
 * - Often tighter than LP relaxation for SCP
 * - Much faster to compute than LP for large instances
 */
typedef struct {
    int num_elements;           /* m: number of constraints (elements) */
    int num_sets;               /* n: number of variables (sets) */

    /* Lagrangian multipliers (dual variables) */
    double *lambda;             /* Multipliers for covering constraints (size m) */

    /* Problem data (pointers to model data, not owned) */
    const double *costs;        /* Set costs c[j] */
    const int *col_ptr;         /* CSC column pointers */
    const int *row_idx;         /* CSC row indices */

    /* Subgradient optimization state */
    double *subgradient;        /* Current subgradient (size m) */
    double *best_lambda;        /* Best multipliers found (size m) */
    double best_bound;          /* Best Lagrangian bound found */
    double ub;                  /* Best known upper bound (from heuristic) */

    /* Parameters */
    int max_iterations;         /* Maximum subgradient iterations */
    double step_factor;         /* Step size factor (typically 2.0, halved on no improvement) */
    double min_step_factor;     /* Minimum step factor before stopping */
    int no_improve_limit;       /* Iterations without improvement before halving step */

    /* Statistics */
    int iterations;             /* Total iterations performed */
    int bound_improvements;     /* Number of times bound improved */

    /* Solution from Lagrangian subproblem */
    double *x_lagrangian;       /* Binary solution from subproblem (size n) */

} LagrangianContext;

/*
 * Create Lagrangian relaxation context for SCP.
 *
 * Parameters:
 *   solver - MIP solver with SCP structure
 *
 * Returns:
 *   Lagrangian context, or NULL if not an SCP or allocation failed.
 *   Caller must free with lagrangian_free().
 */
LagrangianContext *lagrangian_create(MIPSolver *solver);

/*
 * Free Lagrangian context.
 */
void lagrangian_free(LagrangianContext *ctx);

/*
 * Compute Lagrangian bound for current multipliers.
 *
 * Solves the Lagrangian subproblem:
 *   L(lambda) = sum(lambda) + sum { min(0, c_j - sum(lambda_i : i covers j)) }
 *
 * Also computes the subgradient g_i = 1 - sum(x_j : j covers i).
 *
 * Parameters:
 *   ctx - Lagrangian context
 *
 * Returns:
 *   Lagrangian bound L(lambda).
 */
double lagrangian_bound(LagrangianContext *ctx);

/*
 * Perform one subgradient update step.
 *
 * Updates lambda using:
 *   lambda_i = max(0, lambda_i + step * g_i)
 *
 * where step = factor * (ub - L(lambda)) / ||g||^2
 *
 * Parameters:
 *   ctx - Lagrangian context
 *
 * Returns:
 *   New Lagrangian bound after update.
 */
double lagrangian_step(LagrangianContext *ctx);

/*
 * Run subgradient optimization to find best Lagrangian bound.
 *
 * Iterates until:
 * - max_iterations reached
 * - step_factor falls below min_step_factor
 * - gap between ub and bound is within tolerance
 *
 * Parameters:
 *   ctx - Lagrangian context
 *
 * Returns:
 *   Best Lagrangian bound found (also stored in ctx->best_bound).
 */
double lagrangian_optimize(LagrangianContext *ctx);

/*
 * Convert Lagrangian solution to feasible SCP solution.
 *
 * The Lagrangian subproblem solution x_lagrangian may be infeasible
 * (some elements not covered). This function repairs it using greedy.
 *
 * Parameters:
 *   ctx      - Lagrangian context with x_lagrangian populated
 *   solver   - MIP solver (for costs and coverage data)
 *   solution - Output: feasible binary solution (size n)
 *
 * Returns:
 *   Objective value of feasible solution, or RALPH_INFINITY if failed.
 */
double lagrangian_repair(LagrangianContext *ctx, MIPSolver *solver, double *solution);

/*
 * Full Lagrangian-based solve for SCP.
 *
 * Combines:
 * 1. Greedy heuristic for initial upper bound
 * 2. Subgradient optimization for lower bound
 * 3. Lagrangian repair for improved solutions
 *
 * Parameters:
 *   solver      - MIP solver with SCP structure
 *   solution    - Output: best solution found
 *   lower_bound - Output: best lower bound (can be NULL)
 *
 * Returns:
 *   0 on success with solution, -1 if not SCP or error.
 */
int lagrangian_solve_scp(MIPSolver *solver, double *solution, double *lower_bound);

/*
 * Solve SCP using generic Lagrangian framework (no MIPSolver dependency).
 *
 * This is the low-level function that operates directly on problem data.
 * Use lagrangian_solve_scp() for the MIPSolver-integrated version.
 *
 * Parameters:
 *   m, n       - Problem dimensions (elements, sets)
 *   costs      - Set costs [n]
 *   rhs        - RHS values [m] (typically all 1.0 for SCP)
 *   col_ptr    - CSC column pointers [n+1]
 *   row_idx    - CSC row indices
 *   initial_ub - Initial upper bound from heuristic (HUGE_VAL if none)
 *   solution   - Output: best solution found [n]
 *   lower_bound - Output: best lower bound (can be NULL)
 *
 * Returns:
 *   0 on success, -1 on error.
 */
int ralph_lagrangian_solve_scp_ex(
    int m, int n,
    const double *costs,
    const double *rhs,
    const int *col_ptr,
    const int *row_idx,
    double initial_ub,
    double *solution,
    double *lower_bound
);

#ifdef __cplusplus
}
#endif

#endif /* RALPH_MIP_H */
