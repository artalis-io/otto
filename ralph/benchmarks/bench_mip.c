/*
 * Ralph MIP Benchmark Suite
 *
 * Tests Ralph's MIP solver on classic combinatorial optimization problems:
 * - Set Covering
 * - Set Partitioning
 * - Linear Assignment
 * - Minimum Cost Network Flow
 * - Uncapacitated Facility Location
 *
 * Build:
 *   make bench-mip
 *
 * Run:
 *   ./bench_mip [--quick] [--problem NAME] [--size N]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* Ralph headers */
#include "ralph.h"

/* GLPK header */
#include <glpk.h>

/* ============================================================================
 * Random number generator (deterministic for reproducibility)
 * ============================================================================ */

static unsigned int g_seed = 42;

static void seed_random(unsigned int seed) {
    g_seed = seed;
}

static double rand_double(double min, double max) {
    g_seed = g_seed * 1103515245 + 12345;
    double r = (double)(g_seed % 100000) / 100000.0;
    return min + r * (max - min);
}

static int rand_int(int min, int max) {
    g_seed = g_seed * 1103515245 + 12345;
    return min + (g_seed % (max - min + 1));
}

/* ============================================================================
 * Problem data structure
 * ============================================================================ */

typedef struct {
    char name[64];
    int num_vars;
    int num_cons;
    int num_integers;

    /* Objective */
    double *obj;        /* [num_vars] */
    RalphObjSense sense;

    /* Variable bounds and types */
    double *lb;         /* [num_vars] */
    double *ub;         /* [num_vars] */
    char *vtype;        /* [num_vars] 'C', 'I', 'B' */

    /* Constraints in triplet format */
    int *con_row;
    int *con_col;
    double *con_val;
    int nnz;
    int nnz_alloc;

    double *rhs;        /* [num_cons] */
    char *sense_con;    /* [num_cons] 'L', 'E', 'G' */
} MIPProblem;

static MIPProblem* mip_create(const char *name, int num_vars, int num_cons, int est_nnz) {
    MIPProblem *prob = (MIPProblem*)calloc(1, sizeof(MIPProblem));
    if (!prob) return NULL;

    strncpy(prob->name, name, sizeof(prob->name) - 1);
    prob->num_vars = num_vars;
    prob->num_cons = num_cons;
    prob->sense = RALPH_MINIMIZE;

    prob->obj = (double*)calloc(num_vars, sizeof(double));
    prob->lb = (double*)calloc(num_vars, sizeof(double));
    prob->ub = (double*)calloc(num_vars, sizeof(double));
    prob->vtype = (char*)calloc(num_vars, sizeof(char));

    prob->nnz_alloc = est_nnz > 0 ? est_nnz : num_vars * num_cons / 2;
    prob->con_row = (int*)malloc(prob->nnz_alloc * sizeof(int));
    prob->con_col = (int*)malloc(prob->nnz_alloc * sizeof(int));
    prob->con_val = (double*)malloc(prob->nnz_alloc * sizeof(double));
    prob->nnz = 0;

    prob->rhs = (double*)calloc(num_cons, sizeof(double));
    prob->sense_con = (char*)calloc(num_cons, sizeof(char));

    /* Default bounds and types */
    for (int j = 0; j < num_vars; j++) {
        prob->lb[j] = 0.0;
        prob->ub[j] = RALPH_INFINITY;
        prob->vtype[j] = 'C';
    }

    for (int i = 0; i < num_cons; i++) {
        prob->sense_con[i] = 'L';
    }

    return prob;
}

static void mip_add_coef(MIPProblem *prob, int row, int col, double val) {
    if (prob->nnz >= prob->nnz_alloc) {
        prob->nnz_alloc *= 2;
        prob->con_row = (int*)realloc(prob->con_row, prob->nnz_alloc * sizeof(int));
        prob->con_col = (int*)realloc(prob->con_col, prob->nnz_alloc * sizeof(int));
        prob->con_val = (double*)realloc(prob->con_val, prob->nnz_alloc * sizeof(double));
    }
    prob->con_row[prob->nnz] = row;
    prob->con_col[prob->nnz] = col;
    prob->con_val[prob->nnz] = val;
    prob->nnz++;
}

static void mip_free(MIPProblem *prob) {
    if (!prob) return;
    free(prob->obj);
    free(prob->lb);
    free(prob->ub);
    free(prob->vtype);
    free(prob->con_row);
    free(prob->con_col);
    free(prob->con_val);
    free(prob->rhs);
    free(prob->sense_con);
    free(prob);
}

/* ============================================================================
 * Set Covering Problem Generator
 *
 * Minimize sum(c[j] * x[j]) for j in subsets
 * Subject to: sum(a[i,j] * x[j]) >= 1 for each element i
 *             x[j] in {0,1}
 *
 * a[i,j] = 1 if subset j covers element i
 * ============================================================================ */

static MIPProblem* generate_set_covering(int num_elements, int num_subsets,
                                          double density, unsigned int seed) {
    seed_random(seed);

    MIPProblem *prob = mip_create("SetCovering", num_subsets, num_elements,
                                  (int)(num_elements * num_subsets * density * 1.5));
    if (!prob) return NULL;

    prob->num_integers = num_subsets;

    /* Variables: x[j] = 1 if subset j is selected */
    for (int j = 0; j < num_subsets; j++) {
        prob->obj[j] = rand_double(1.0, 10.0);  /* Cost of subset j */
        prob->lb[j] = 0.0;
        prob->ub[j] = 1.0;
        prob->vtype[j] = 'B';  /* Binary */
    }

    /* Constraints: each element must be covered at least once */
    for (int i = 0; i < num_elements; i++) {
        int covers = 0;
        for (int j = 0; j < num_subsets; j++) {
            if (rand_double(0, 1) < density) {
                mip_add_coef(prob, i, j, 1.0);
                covers++;
            }
        }
        /* Ensure at least one subset covers this element */
        if (covers == 0) {
            int j = rand_int(0, num_subsets - 1);
            mip_add_coef(prob, i, j, 1.0);
        }
        prob->rhs[i] = 1.0;
        prob->sense_con[i] = 'G';  /* >= 1 */
    }

    return prob;
}

/* ============================================================================
 * Set Partitioning Problem Generator
 *
 * Same as set covering but each element must be covered exactly once.
 * ============================================================================ */

static MIPProblem* generate_set_partitioning(int num_elements, int num_subsets,
                                              double density, unsigned int seed) {
    seed_random(seed);

    MIPProblem *prob = mip_create("SetPartitioning", num_subsets, num_elements,
                                  (int)(num_elements * num_subsets * density * 1.5));
    if (!prob) return NULL;

    prob->num_integers = num_subsets;

    /* Variables: x[j] = 1 if subset j is selected */
    for (int j = 0; j < num_subsets; j++) {
        prob->obj[j] = rand_double(1.0, 10.0);
        prob->lb[j] = 0.0;
        prob->ub[j] = 1.0;
        prob->vtype[j] = 'B';
    }

    /* Constraints: each element must be covered exactly once */
    for (int i = 0; i < num_elements; i++) {
        int covers = 0;
        for (int j = 0; j < num_subsets; j++) {
            if (rand_double(0, 1) < density) {
                mip_add_coef(prob, i, j, 1.0);
                covers++;
            }
        }
        /* Ensure at least two subsets cover this element (for partitioning feasibility) */
        while (covers < 2) {
            int j = rand_int(0, num_subsets - 1);
            mip_add_coef(prob, i, j, 1.0);
            covers++;
        }
        prob->rhs[i] = 1.0;
        prob->sense_con[i] = 'E';  /* = 1 */
    }

    return prob;
}

/* ============================================================================
 * Linear Assignment Problem Generator
 *
 * Assign n workers to n jobs to minimize total cost.
 *
 * Minimize sum(c[i,j] * x[i,j])
 * Subject to: sum_j(x[i,j]) = 1 for each worker i
 *             sum_i(x[i,j]) = 1 for each job j
 *             x[i,j] in {0,1}
 * ============================================================================ */

static MIPProblem* generate_linear_assignment(int n, unsigned int seed) {
    seed_random(seed);

    int num_vars = n * n;  /* x[i,j] for worker i, job j */
    int num_cons = 2 * n;  /* n worker constraints + n job constraints */

    MIPProblem *prob = mip_create("LinearAssignment", num_vars, num_cons, 2 * num_vars);
    if (!prob) return NULL;

    prob->num_integers = num_vars;

    /* Variables: x[i*n + j] = 1 if worker i assigned to job j */
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            int var = i * n + j;
            prob->obj[var] = rand_double(1.0, 100.0);  /* Assignment cost */
            prob->lb[var] = 0.0;
            prob->ub[var] = 1.0;
            prob->vtype[var] = 'B';
        }
    }

    /* Worker constraints: each worker assigned to exactly one job */
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            int var = i * n + j;
            mip_add_coef(prob, i, var, 1.0);
        }
        prob->rhs[i] = 1.0;
        prob->sense_con[i] = 'E';
    }

    /* Job constraints: each job assigned to exactly one worker */
    for (int j = 0; j < n; j++) {
        int con = n + j;
        for (int i = 0; i < n; i++) {
            int var = i * n + j;
            mip_add_coef(prob, con, var, 1.0);
        }
        prob->rhs[con] = 1.0;
        prob->sense_con[con] = 'E';
    }

    return prob;
}

/* ============================================================================
 * Minimum Cost Network Flow Problem Generator
 *
 * Creates a layered network with source(s) -> transit nodes -> sink(s)
 * Structure guarantees feasibility by having high-capacity paths from
 * sources to sinks.
 *
 * Variables: f[i,j] = flow on arc (i,j)
 * Minimize: sum(cost[i,j] * f[i,j])
 * Subject to:
 *   sum(f[i,j] for j out) - sum(f[k,i] for k in) = supply[i] for each node i
 *   0 <= f[i,j] <= capacity[i,j]
 * ============================================================================ */

static MIPProblem* generate_network_flow(int num_nodes, int num_arcs_approx,
                                          unsigned int seed) {
    seed_random(seed);

    /* Simple layered network:
     * Layer 0: source (node 0)
     * Layer 1: transit nodes (nodes 1 to num_nodes-2)
     * Layer 2: sink (node num_nodes-1)
     */
    if (num_nodes < 3) num_nodes = 3;

    int source = 0;
    int sink = num_nodes - 1;
    int num_transit = num_nodes - 2;

    /* Estimate arcs: source->transit + transit->transit + transit->sink */
    int max_arcs = num_transit + num_transit * (num_transit - 1) / 2 + num_transit;
    int alloc_arcs = max_arcs > num_arcs_approx ? max_arcs : num_arcs_approx;

    int *arc_from = (int*)malloc(alloc_arcs * sizeof(int));
    int *arc_to = (int*)malloc(alloc_arcs * sizeof(int));
    double *arc_cost = (double*)malloc(alloc_arcs * sizeof(double));
    double *arc_cap = (double*)malloc(alloc_arcs * sizeof(double));

    int num_arcs = 0;

    /* Total flow to route */
    double total_flow = rand_double(50.0, 200.0);

    /* Arcs from source to transit nodes */
    for (int t = 1; t <= num_transit; t++) {
        arc_from[num_arcs] = source;
        arc_to[num_arcs] = t;
        arc_cost[num_arcs] = rand_double(1.0, 10.0);
        arc_cap[num_arcs] = total_flow;  /* Large enough capacity */
        num_arcs++;
    }

    /* Some arcs between transit nodes (for routing flexibility) */
    for (int t1 = 1; t1 <= num_transit && num_arcs < alloc_arcs - num_transit; t1++) {
        for (int t2 = t1 + 1; t2 <= num_transit; t2++) {
            if (rand_double(0, 1) < 0.3) {
                arc_from[num_arcs] = t1;
                arc_to[num_arcs] = t2;
                arc_cost[num_arcs] = rand_double(1.0, 15.0);
                arc_cap[num_arcs] = rand_double(total_flow / 4, total_flow);
                num_arcs++;
            }
        }
    }

    /* Arcs from transit nodes to sink */
    for (int t = 1; t <= num_transit; t++) {
        arc_from[num_arcs] = t;
        arc_to[num_arcs] = sink;
        arc_cost[num_arcs] = rand_double(1.0, 10.0);
        arc_cap[num_arcs] = total_flow;  /* Large enough capacity */
        num_arcs++;
    }

    /* Create supply/demand: source has positive supply, sink has negative */
    double *supply = (double*)calloc(num_nodes, sizeof(double));
    supply[source] = total_flow;
    supply[sink] = -total_flow;
    /* Transit nodes have zero supply (transshipment) */

    /* Create MIP problem */
    MIPProblem *prob = mip_create("NetworkFlow", num_arcs, num_nodes, 2 * num_arcs);
    if (!prob) {
        free(arc_from); free(arc_to); free(arc_cost); free(arc_cap); free(supply);
        return NULL;
    }

    prob->num_integers = 0;  /* Network flow is LP (continuous) */

    /* Variables: f[a] = flow on arc a */
    for (int a = 0; a < num_arcs; a++) {
        prob->obj[a] = arc_cost[a];
        prob->lb[a] = 0.0;
        prob->ub[a] = arc_cap[a];
        prob->vtype[a] = 'C';
    }

    /* Flow balance constraints for each node */
    for (int i = 0; i < num_nodes; i++) {
        /* sum(outflow) - sum(inflow) = supply[i] */
        for (int a = 0; a < num_arcs; a++) {
            if (arc_from[a] == i) {
                mip_add_coef(prob, i, a, 1.0);  /* Outflow */
            }
            if (arc_to[a] == i) {
                mip_add_coef(prob, i, a, -1.0);  /* Inflow */
            }
        }
        prob->rhs[i] = supply[i];
        prob->sense_con[i] = 'E';
    }

    free(arc_from); free(arc_to); free(arc_cost); free(arc_cap); free(supply);
    return prob;
}

/* ============================================================================
 * Uncapacitated Facility Location Problem Generator
 *
 * Given potential facility locations and customer demands, decide which
 * facilities to open and how to assign customers to minimize total cost.
 *
 * Variables:
 *   y[j] = 1 if facility j is opened
 *   x[i,j] = fraction of customer i's demand served by facility j
 *
 * Minimize: sum(f[j] * y[j]) + sum(c[i,j] * d[i] * x[i,j])
 * Subject to:
 *   sum_j(x[i,j]) = 1 for each customer i (all demand must be met)
 *   x[i,j] <= y[j] for each i,j (can only serve from open facilities)
 *   y[j] in {0,1}
 *   x[i,j] >= 0
 * ============================================================================ */

static MIPProblem* generate_facility_location(int num_customers, int num_facilities,
                                               unsigned int seed) {
    seed_random(seed);

    int num_vars = num_facilities + num_customers * num_facilities;
    /* y[j] for j=0..F-1, then x[i*F + j] for i=0..C-1, j=0..F-1 */
    int num_cons = num_customers + num_customers * num_facilities;
    /* Demand constraints + linking constraints */

    MIPProblem *prob = mip_create("FacilityLocation", num_vars, num_cons,
                                  num_customers * num_facilities * 3);
    if (!prob) return NULL;

    prob->num_integers = num_facilities;

    /* Generate facility fixed costs and customer-facility service costs */
    double *fixed_cost = (double*)malloc(num_facilities * sizeof(double));
    double *demand = (double*)malloc(num_customers * sizeof(double));

    for (int j = 0; j < num_facilities; j++) {
        fixed_cost[j] = rand_double(100.0, 500.0);
    }
    for (int i = 0; i < num_customers; i++) {
        demand[i] = rand_double(1.0, 10.0);
    }

    /* Variables y[j]: facility opening decisions */
    for (int j = 0; j < num_facilities; j++) {
        prob->obj[j] = fixed_cost[j];
        prob->lb[j] = 0.0;
        prob->ub[j] = 1.0;
        prob->vtype[j] = 'B';
    }

    /* Variables x[i,j]: assignment fractions */
    for (int i = 0; i < num_customers; i++) {
        for (int j = 0; j < num_facilities; j++) {
            int var = num_facilities + i * num_facilities + j;
            /* Service cost: distance-based */
            double dist = rand_double(1.0, 50.0);  /* Random "distance" */
            prob->obj[var] = dist * demand[i];
            prob->lb[var] = 0.0;
            prob->ub[var] = 1.0;
            prob->vtype[var] = 'C';
        }
    }

    /* Demand constraints: sum_j(x[i,j]) = 1 */
    int con = 0;
    for (int i = 0; i < num_customers; i++) {
        for (int j = 0; j < num_facilities; j++) {
            int var = num_facilities + i * num_facilities + j;
            mip_add_coef(prob, con, var, 1.0);
        }
        prob->rhs[con] = 1.0;
        prob->sense_con[con] = 'E';
        con++;
    }

    /* Linking constraints: x[i,j] <= y[j] */
    for (int i = 0; i < num_customers; i++) {
        for (int j = 0; j < num_facilities; j++) {
            int x_var = num_facilities + i * num_facilities + j;
            int y_var = j;
            mip_add_coef(prob, con, x_var, 1.0);
            mip_add_coef(prob, con, y_var, -1.0);
            prob->rhs[con] = 0.0;
            prob->sense_con[con] = 'L';
            con++;
        }
    }

    free(fixed_cost);
    free(demand);
    return prob;
}

/* ============================================================================
 * Solver interfaces
 * ============================================================================ */

typedef struct {
    double solve_time;
    double objective;
    int status;  /* 0 = optimal, 1 = infeasible, 2 = other */
    int nodes;
    int iterations;
} SolveResult;

static SolveResult solve_with_ralph(MIPProblem *prob, double time_limit) {
    SolveResult result = {0};

    RalphModel *model = ralph_create();
    if (!model) {
        result.status = 2;
        return result;
    }

    ralph_set_int_param(model, "verbose", 0);
    ralph_set_dbl_param(model, "time_limit", time_limit);
    ralph_set_int_param(model, "max_nodes", 100000);
    ralph_set_obj_sense(model, prob->sense);

    /* Add variables */
    for (int j = 0; j < prob->num_vars; j++) {
        ralph_add_var(model, prob->lb[j], prob->ub[j], prob->obj[j], prob->vtype[j]);
    }

    /* Build constraint arrays per row */
    int *row_start = (int*)calloc(prob->num_cons + 1, sizeof(int));
    for (int k = 0; k < prob->nnz; k++) {
        row_start[prob->con_row[k] + 1]++;
    }
    for (int i = 0; i < prob->num_cons; i++) {
        row_start[i + 1] += row_start[i];
    }

    int *row_idx = (int*)malloc(prob->nnz * sizeof(int));
    double *row_val = (double*)malloc(prob->nnz * sizeof(double));
    int *row_pos = (int*)malloc(prob->num_cons * sizeof(int));
    memcpy(row_pos, row_start, prob->num_cons * sizeof(int));

    for (int k = 0; k < prob->nnz; k++) {
        int i = prob->con_row[k];
        int pos = row_pos[i]++;
        row_idx[pos] = prob->con_col[k];
        row_val[pos] = prob->con_val[k];
    }

    /* Add constraints */
    for (int i = 0; i < prob->num_cons; i++) {
        int nnz = row_start[i + 1] - row_start[i];
        ralph_add_constraint(model, nnz, &row_idx[row_start[i]],
                            &row_val[row_start[i]], prob->sense_con[i], prob->rhs[i]);
    }

    free(row_start);
    free(row_idx);
    free(row_val);
    free(row_pos);

    /* Solve and time */
    clock_t start = clock();
    int status = ralph_optimize(model);
    clock_t end = clock();

    result.solve_time = (double)(end - start) / CLOCKS_PER_SEC;
    result.objective = ralph_get_objval(model);
    result.nodes = ralph_get_node_count(model);
    result.iterations = ralph_get_iterations(model);

    if (status == RALPH_STATUS_OPTIMAL) {
        result.status = 0;
    } else if (status == RALPH_STATUS_INFEASIBLE) {
        result.status = 1;
    } else {
        result.status = 2;
    }

    ralph_free(model);
    return result;
}

static SolveResult solve_with_glpk(MIPProblem *prob, double time_limit) {
    SolveResult result = {0};

    glp_prob *lp = glp_create_prob();
    if (!lp) {
        result.status = 2;
        return result;
    }

    glp_set_obj_dir(lp, prob->sense == RALPH_MINIMIZE ? GLP_MIN : GLP_MAX);

    /* Add rows (constraints) */
    if (prob->num_cons > 0) {
        glp_add_rows(lp, prob->num_cons);
        for (int i = 0; i < prob->num_cons; i++) {
            if (prob->sense_con[i] == 'L') {
                glp_set_row_bnds(lp, i + 1, GLP_UP, 0.0, prob->rhs[i]);
            } else if (prob->sense_con[i] == 'G') {
                glp_set_row_bnds(lp, i + 1, GLP_LO, prob->rhs[i], 0.0);
            } else {
                glp_set_row_bnds(lp, i + 1, GLP_FX, prob->rhs[i], prob->rhs[i]);
            }
        }
    }

    /* Add columns (variables) */
    glp_add_cols(lp, prob->num_vars);
    for (int j = 0; j < prob->num_vars; j++) {
        double lb = prob->lb[j];
        double ub = prob->ub[j];

        if (lb <= -1e29 && ub >= 1e29) {
            glp_set_col_bnds(lp, j + 1, GLP_FR, 0.0, 0.0);
        } else if (lb <= -1e29) {
            glp_set_col_bnds(lp, j + 1, GLP_UP, 0.0, ub);
        } else if (ub >= 1e29) {
            glp_set_col_bnds(lp, j + 1, GLP_LO, lb, 0.0);
        } else if (fabs(lb - ub) < 1e-9) {
            glp_set_col_bnds(lp, j + 1, GLP_FX, lb, ub);
        } else {
            glp_set_col_bnds(lp, j + 1, GLP_DB, lb, ub);
        }

        glp_set_obj_coef(lp, j + 1, prob->obj[j]);

        if (prob->vtype[j] == 'B') {
            glp_set_col_kind(lp, j + 1, GLP_BV);
        } else if (prob->vtype[j] == 'I') {
            glp_set_col_kind(lp, j + 1, GLP_IV);
        }
    }

    /* Load constraint matrix */
    if (prob->nnz > 0) {
        int *ia = (int*)malloc((prob->nnz + 1) * sizeof(int));
        int *ja = (int*)malloc((prob->nnz + 1) * sizeof(int));
        double *ar = (double*)malloc((prob->nnz + 1) * sizeof(double));

        for (int k = 0; k < prob->nnz; k++) {
            ia[k + 1] = prob->con_row[k] + 1;
            ja[k + 1] = prob->con_col[k] + 1;
            ar[k + 1] = prob->con_val[k];
        }

        glp_load_matrix(lp, prob->nnz, ia, ja, ar);

        free(ia);
        free(ja);
        free(ar);
    }

    /* Solve */
    clock_t start = clock();

    /* First solve LP relaxation */
    glp_smcp sparm;
    glp_init_smcp(&sparm);
    sparm.msg_lev = GLP_MSG_OFF;
    glp_simplex(lp, &sparm);

    /* Then solve MIP if there are integers */
    int glp_status;
    if (prob->num_integers > 0) {
        glp_iocp iparm;
        glp_init_iocp(&iparm);
        iparm.msg_lev = GLP_MSG_OFF;
        iparm.tm_lim = (int)(time_limit * 1000);
        iparm.presolve = GLP_ON;

        glp_intopt(lp, &iparm);
        glp_status = glp_mip_status(lp);
        result.objective = glp_mip_obj_val(lp);
    } else {
        glp_status = glp_get_status(lp);
        result.objective = glp_get_obj_val(lp);
    }

    clock_t end = clock();
    result.solve_time = (double)(end - start) / CLOCKS_PER_SEC;

    if (glp_status == GLP_OPT) {
        result.status = 0;
    } else if (glp_status == GLP_INFEAS || glp_status == GLP_NOFEAS) {
        result.status = 1;
    } else {
        result.status = 2;
    }

    glp_delete_prob(lp);
    return result;
}

/* ============================================================================
 * Benchmark runner
 * ============================================================================ */

static void print_result(const char *solver, SolveResult *res) {
    const char *status_str = "???";
    if (res->status == 0) status_str = "OPT";
    else if (res->status == 1) status_str = "INF";
    else status_str = "LIM";

    printf("  %-8s %-8s %12.2f %10.4f\n",
           solver, status_str, res->objective, res->solve_time);
}

static void run_mip_benchmark(MIPProblem *prob, double time_limit) {
    printf("\n");
    printf("Problem: %s\n", prob->name);
    printf("  %d variables (%d integer), %d constraints, %d non-zeros\n",
           prob->num_vars, prob->num_integers, prob->num_cons, prob->nnz);
    printf("  %-8s %-8s %12s %10s\n", "Solver", "Status", "Objective", "Time(s)");
    printf("  %-8s %-8s %12s %10s\n", "------", "------", "---------", "-------");

    SolveResult ralph_res = solve_with_ralph(prob, time_limit);
    print_result("Ralph", &ralph_res);

    SolveResult glpk_res = solve_with_glpk(prob, time_limit);
    print_result("GLPK", &glpk_res);

    /* Compare objectives if both optimal */
    if (ralph_res.status == 0 && glpk_res.status == 0) {
        double diff = fabs(ralph_res.objective - glpk_res.objective);
        double scale = fmax(1.0, fabs(glpk_res.objective));
        if (diff / scale > 1e-4) {
            printf("  WARNING: Objective mismatch (diff = %.4f)\n", diff);
        } else {
            printf("  Objectives match (OK)\n");
        }
        if (ralph_res.solve_time > 0 && glpk_res.solve_time > 0) {
            printf("  Speedup: GLPK %.1fx faster\n",
                   ralph_res.solve_time / glpk_res.solve_time);
        }
    }
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_header(void) {
    printf("\n");
    printf("================================================================================\n");
    printf("                    Ralph MIP Benchmark Suite                                   \n");
    printf("================================================================================\n");
    printf("\n");
    printf("Comparing Ralph %s against GLPK %d.%d on classic MIP problems\n",
           ralph_version(), GLP_MAJOR_VERSION, GLP_MINOR_VERSION);
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("\nOptions:\n");
    printf("  --quick             Run quick benchmarks (smaller sizes)\n");
    printf("  --problem NAME      Run only specified problem type:\n");
    printf("                      setcover, setpart, assignment, netflow, facility\n");
    printf("  --size N            Override problem size\n");
    printf("  --time-limit T      Set time limit in seconds (default: 60)\n");
    printf("  --help              Show this help\n");
}

int main(int argc, char **argv) {
    print_header();

    /* Parse command line */
    int quick_mode = 0;
    const char *problem_filter = NULL;
    int size_override = 0;
    double time_limit = 60.0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--quick") == 0) {
            quick_mode = 1;
        } else if (strcmp(argv[i], "--problem") == 0 && i + 1 < argc) {
            problem_filter = argv[++i];
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            size_override = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--time-limit") == 0 && i + 1 < argc) {
            time_limit = atof(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    /* Problem sizes */
    int small_size = quick_mode ? 10 : 20;
    int medium_size = quick_mode ? 20 : 50;
    int large_size = quick_mode ? 30 : 100;

    if (size_override > 0) {
        small_size = size_override;
        medium_size = size_override;
        large_size = size_override;
    }

    printf("\nTime limit: %.0f seconds per problem\n", time_limit);
    if (quick_mode) printf("Quick mode: using smaller problem sizes\n");

    /* Run benchmarks */
    MIPProblem *prob;

    /* Set Covering */
    if (!problem_filter || strstr(problem_filter, "setcover")) {
        printf("\n");
        printf("--------------------------------------------------------------------------------\n");
        printf("  SET COVERING PROBLEMS\n");
        printf("--------------------------------------------------------------------------------\n");

        prob = generate_set_covering(small_size, small_size * 2, 0.3, 42);
        run_mip_benchmark(prob, time_limit);
        mip_free(prob);

        prob = generate_set_covering(medium_size, medium_size * 2, 0.25, 123);
        run_mip_benchmark(prob, time_limit);
        mip_free(prob);

        if (!quick_mode) {
            prob = generate_set_covering(large_size, large_size * 2, 0.2, 456);
            run_mip_benchmark(prob, time_limit);
            mip_free(prob);
        }
    }

    /* Set Partitioning */
    if (!problem_filter || strstr(problem_filter, "setpart")) {
        printf("\n");
        printf("--------------------------------------------------------------------------------\n");
        printf("  SET PARTITIONING PROBLEMS\n");
        printf("--------------------------------------------------------------------------------\n");

        prob = generate_set_partitioning(small_size, small_size * 3, 0.35, 42);
        run_mip_benchmark(prob, time_limit);
        mip_free(prob);

        prob = generate_set_partitioning(medium_size, medium_size * 3, 0.30, 123);
        run_mip_benchmark(prob, time_limit);
        mip_free(prob);
    }

    /* Linear Assignment */
    if (!problem_filter || strstr(problem_filter, "assignment")) {
        printf("\n");
        printf("--------------------------------------------------------------------------------\n");
        printf("  LINEAR ASSIGNMENT PROBLEMS\n");
        printf("--------------------------------------------------------------------------------\n");

        prob = generate_linear_assignment(small_size, 42);
        run_mip_benchmark(prob, time_limit);
        mip_free(prob);

        prob = generate_linear_assignment(medium_size, 123);
        run_mip_benchmark(prob, time_limit);
        mip_free(prob);

        if (!quick_mode) {
            prob = generate_linear_assignment(large_size, 456);
            run_mip_benchmark(prob, time_limit);
            mip_free(prob);
        }
    }

    /* Network Flow (LP, not MIP) */
    if (!problem_filter || strstr(problem_filter, "netflow")) {
        printf("\n");
        printf("--------------------------------------------------------------------------------\n");
        printf("  MINIMUM COST NETWORK FLOW PROBLEMS (LP)\n");
        printf("--------------------------------------------------------------------------------\n");

        prob = generate_network_flow(small_size * 2, small_size * 4, 42);
        run_mip_benchmark(prob, time_limit);
        mip_free(prob);

        prob = generate_network_flow(medium_size * 2, medium_size * 4, 123);
        run_mip_benchmark(prob, time_limit);
        mip_free(prob);

        if (!quick_mode) {
            prob = generate_network_flow(large_size * 2, large_size * 4, 456);
            run_mip_benchmark(prob, time_limit);
            mip_free(prob);
        }
    }

    /* Facility Location */
    if (!problem_filter || strstr(problem_filter, "facility")) {
        printf("\n");
        printf("--------------------------------------------------------------------------------\n");
        printf("  UNCAPACITATED FACILITY LOCATION PROBLEMS\n");
        printf("--------------------------------------------------------------------------------\n");

        prob = generate_facility_location(small_size, small_size / 2, 42);
        run_mip_benchmark(prob, time_limit);
        mip_free(prob);

        prob = generate_facility_location(medium_size, medium_size / 2, 123);
        run_mip_benchmark(prob, time_limit);
        mip_free(prob);

        if (!quick_mode) {
            prob = generate_facility_location(large_size, large_size / 2, 456);
            run_mip_benchmark(prob, time_limit);
            mip_free(prob);
        }
    }

    printf("\n");
    printf("================================================================================\n");
    printf("  MIP Benchmark Complete\n");
    printf("================================================================================\n\n");

    return 0;
}
