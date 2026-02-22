/*
 * Ralph - Main Public API Implementation
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include "ralph.h"
#include "lp.h"
#include "mip.h"
#include "presolve.h"
#include "detect.h"
#include "benders.h"

#define RALPH_VERSION "0.1.0"

/* Forward declarations */
int lp_model_finalize(LPModel *model);

/* ============================================================================
 * Internal Model Structure
 * ============================================================================ */

struct RalphModel {
    LPModel *lp_model;
    SimplexSolver *lp_solver;
    MIPSolver *mip_solver;

    /* Parameters */
    int max_iterations;
    double time_limit;
    int presolve;
    int verbose;
    int telemetry;  /* 1=collect LP/LU telemetry counters/timers */
    double mip_gap;
    int max_nodes;
    int max_cut_rounds;
    int method;  /* 0=primal simplex, 1=dual simplex, 2=auto */
    int pricing; /* 0=Dantzig, 1=Steepest edge, 2=Devex (default), 3=Partial */
    int detect_special; /* 1=detect LAP/network structure, 0=disable */
    int node_pool_capacity; /* Pre-allocated B&B node pool size (default 1024) */
    int node_select;  /* 0=best-first, 1=DFS, 2=best-estimate, 3=hybrid (default) */
    unsigned int presolve_mask; /* Bitmask controlling presolve techniques (default PRESOLVE_SAFE=0x110F) */
    int force_two_phase; /* 1=force two-phase simplex for clean Farkas duals */
    int trace_phase1; /* 1=emit deterministic Phase-1 failure trace */
    int scaling;            /* 0=off, 1=single-round (default), N=N geo rounds + equilibrium */
    int crash;              /* 0=off, 1=triangular crash basis */
    int verify;             /* 0=off, 1=post-solve verification */
    double objective_limit; /* Early-exit obj limit (user space) */
    int phase1_pricing;     /* Override pricing for Phase 1: 0=Dantzig, -1=disabled */
    int dual_bound_flip;    /* -1=default(on), 0=off, 1=on */
    int dual_steepest_edge; /* -1=default(on), 0=off, 1=on */
    int var_select;         /* -1=default, 0=most_infeas, 1=pseudo_cost, 2=strong, 3=reliability */
    int lu_supernode;       /* 0=off (default), 1=enable supernodal LU factorization (T2.1) */

    /* Solution */
    RalphStatus status;
    double obj_value;
    double *solution;
    double *dual_solution;
    double *reduced_costs;
    double *unbounded_ray;
    int unbounded_ray_valid;

    /* MIP-specific */
    double best_bound;
    int node_count;
    double *mip_start;
    int *mip_start_mask;
    int mip_start_n;
    int mip_start_nnz;
    RalphMIPStartStatus mip_start_status;
    RalphMIPStartRepairMode mip_start_repair_mode;

    /* Branching control (stored until MIP solver is created) */
    int *branch_priorities;
    int *branch_directions;

    /* Cut callback (stored until MIP solver is created) */
    RalphCutCallback cut_callback;
    int has_cut_callback;

    /* Branch callback (stored until MIP solver is created) */
    RalphBranchCallback branch_callback;
    int has_branch_callback;

    /* LP-only progress/cancel callbacks (never propagated to MIP callbacks) */
    RalphLPProgressCallback lp_progress_callback;
    int has_lp_progress_callback;
    RalphLPCancelCallback lp_cancel_callback;
    int has_lp_cancel_callback;

    /* Statistics */
    int iteration_count;
    RalphPresolveReport last_presolve_report;

    /* Basis staged before first optimize() (applied when simplex tableau is created) */
    int staged_basis_m;
    int staged_basis_n;
    int *staged_basis;
    VarStatus *staged_var_status;
};

/* Basis representation for warm start */
struct RalphBasis {
    int m;              /* Number of constraints */
    int n;              /* Number of extended variables */
    int *basis;         /* Basic variable indices (size m) */
    VarStatus *var_status;  /* Variable status array (size n) */
};

static void ralph_clear_staged_basis(RalphModel *model) {
    if (!model) return;
    free(model->staged_basis);
    free(model->staged_var_status);
    model->staged_basis = NULL;
    model->staged_var_status = NULL;
    model->staged_basis_m = 0;
    model->staged_basis_n = 0;
}

static void ralph_clear_mip_start_internal(RalphModel *model) {
    if (!model) return;
    free(model->mip_start);
    free(model->mip_start_mask);
    model->mip_start = NULL;
    model->mip_start_mask = NULL;
    model->mip_start_n = 0;
    model->mip_start_nnz = 0;
    model->mip_start_status = RALPH_MIP_START_NONE;
}

static void ralph_reset_presolve_report(RalphModel *model) {
    if (!model) return;
    memset(&model->last_presolve_report, 0, sizeof(model->last_presolve_report));
}

static int ralph_is_valid_sense(RalphSense sense) {
    return sense == RALPH_LESS_EQUAL || sense == RALPH_EQUAL || sense == RALPH_GREATER_EQUAL;
}

static int ralph_basis_status_from_internal(VarStatus st, RalphBasisStatus *out) {
    if (!out) return -1;
    switch (st) {
        case RALPH_BASIC:
            *out = RALPH_BASIS_STATUS_BASIC;
            return 0;
        case RALPH_NONBASIC_LOWER:
            *out = RALPH_BASIS_STATUS_AT_LOWER;
            return 0;
        case RALPH_NONBASIC_UPPER:
            *out = RALPH_BASIS_STATUS_AT_UPPER;
            return 0;
        case RALPH_NONBASIC_FREE:
            *out = RALPH_BASIS_STATUS_FREE;
            return 0;
        case RALPH_FIXED:
            *out = RALPH_BASIS_STATUS_FIXED;
            return 0;
        default:
            return -1;
    }
}

static int ralph_basis_status_to_internal(RalphBasisStatus in, VarStatus *out) {
    if (!out) return -1;
    switch (in) {
        case RALPH_BASIS_STATUS_BASIC:
            *out = RALPH_BASIC;
            return 0;
        case RALPH_BASIS_STATUS_AT_LOWER:
            *out = RALPH_NONBASIC_LOWER;
            return 0;
        case RALPH_BASIS_STATUS_AT_UPPER:
            *out = RALPH_NONBASIC_UPPER;
            return 0;
        case RALPH_BASIS_STATUS_FREE:
            *out = RALPH_NONBASIC_FREE;
            return 0;
        case RALPH_BASIS_STATUS_FIXED:
            *out = RALPH_FIXED;
            return 0;
        default:
            return -1;
    }
}

static int ralph_build_row_primary_aux_map(const SimplexTableau *tab, int *row_aux) {
    if (!tab || !row_aux || !tab->model) return -1;

    int m = tab->m;
    int num_struct = tab->model->num_vars;
    if (num_struct < 0 || num_struct > tab->n) return -1;

    for (int i = 0; i < m; i++) row_aux[i] = -1;
    if (m == 0) return 0;

    if (!tab->aux_row || tab->num_aux < m) return -1;

    for (int k = 0; k < tab->num_aux; k++) {
        int row = tab->aux_row[k];
        if (row < 0 || row >= m) continue;
        if (row_aux[row] >= 0) continue;

        int var = num_struct + k;
        if (var < 0 || var >= tab->n) continue;
        row_aux[row] = var;
    }

    for (int i = 0; i < m; i++) {
        if (row_aux[i] < 0 || row_aux[i] >= tab->n) return -1;
    }

    return 0;
}

static int ralph_probe_lp_status(const RalphModel *model,
                                 LPModel *probe_model,
                                 RalphStatus *status_out) {
    if (!model || !probe_model || !status_out) return -1;

    SimplexSolver *probe = simplex_create(probe_model);
    if (!probe) return -1;

    probe->max_iterations = (model->max_iterations > 0) ? model->max_iterations : RALPH_DEFAULT_MAX_ITER;
    probe->time_limit = (model->time_limit > 0.0) ? model->time_limit : RALPH_DEFAULT_TIME_LIMIT;
    probe->verbose = 0;
    probe->telemetry_enabled = 0;
    probe->presolve = 0;
    probe->pricing_strategy = model->pricing;
    probe->scaling = 0;
    probe->crash = model->crash;
    probe->verify = 0;
    probe->phase1_pricing = model->phase1_pricing;
    probe->objective_limit = RALPH_INFINITY;
    probe->force_two_phase = model->force_two_phase;
    probe->trace_phase1 = 0;
    probe->method = 0;  /* Use primal for robust infeasibility checks */

    (void)simplex_solve(probe);
    *status_out = probe->status;
    simplex_free(probe);
    return 0;
}

static int ralph_set_mip_start_copy(RalphModel *model, const double *x,
                                    const int *mask, int n) {
    if (!model || !x || n <= 0) return -1;
    if ((size_t)n > SIZE_MAX / sizeof(double)) return -1;

    double *copy = (double*)malloc((size_t)n * sizeof(double));
    int *mask_copy = (int*)calloc((size_t)n, sizeof(int));
    if (!copy) return -1;
    if (!mask_copy) {
        free(copy);
        return -1;
    }
    memcpy(copy, x, (size_t)n * sizeof(double));

    int nnz = 0;
    if (mask) {
        for (int j = 0; j < n; j++) {
            mask_copy[j] = mask[j] ? 1 : 0;
            nnz += mask_copy[j];
        }
    } else {
        for (int j = 0; j < n; j++) mask_copy[j] = 1;
        nnz = n;
    }

    free(model->mip_start);
    free(model->mip_start_mask);
    model->mip_start = copy;
    model->mip_start_mask = mask_copy;
    model->mip_start_n = n;
    model->mip_start_nnz = nnz;
    model->mip_start_status = RALPH_MIP_START_PENDING;
    return 0;
}

static int ralph_stage_basis_copy(RalphModel *model, const RalphBasis *basis) {
    if (!model || !basis || !basis->basis || !basis->var_status) return -1;

    int *basis_copy = NULL;
    VarStatus *status_copy = NULL;

    if (basis->m > 0) {
        basis_copy = (int*)malloc((size_t)basis->m * sizeof(int));
        if (!basis_copy) return -1;
        memcpy(basis_copy, basis->basis, (size_t)basis->m * sizeof(int));
    }

    if (basis->n > 0) {
        status_copy = (VarStatus*)malloc((size_t)basis->n * sizeof(VarStatus));
        if (!status_copy) {
            free(basis_copy);
            return -1;
        }
        memcpy(status_copy, basis->var_status, (size_t)basis->n * sizeof(VarStatus));
    }

    ralph_clear_staged_basis(model);
    model->staged_basis = basis_copy;
    model->staged_var_status = status_copy;
    model->staged_basis_m = basis->m;
    model->staged_basis_n = basis->n;
    return 0;
}

/* Invalidate any cached solve state after model edits. */
static void ralph_invalidate_solve_state(RalphModel *model) {
    if (!model) return;

    simplex_free(model->lp_solver);
    model->lp_solver = NULL;
    mip_free(model->mip_solver);
    model->mip_solver = NULL;

    free(model->solution);
    model->solution = NULL;
    free(model->dual_solution);
    model->dual_solution = NULL;
    free(model->reduced_costs);
    model->reduced_costs = NULL;
    free(model->unbounded_ray);
    model->unbounded_ray = NULL;
    model->unbounded_ray_valid = 0;
    ralph_clear_staged_basis(model);
    if (model->mip_start) {
        if (model->lp_model && model->mip_start_n == model->lp_model->num_vars) {
            model->mip_start_status = RALPH_MIP_START_PENDING;
        } else {
            ralph_clear_mip_start_internal(model);
        }
    }

    model->status = RALPH_STATUS_UNKNOWN;
    model->obj_value = 0.0;
    model->best_bound = 0.0;
    model->node_count = 0;
    model->iteration_count = 0;
    ralph_reset_presolve_report(model);
}

/* ============================================================================
 * Model Creation/Destruction
 * ============================================================================ */

RalphModel* ralph_create(void) {
    RalphModel *model = (RalphModel*)calloc(1, sizeof(RalphModel));
    if (!model) return NULL;

    model->lp_model = lp_model_create();
    if (!model->lp_model) {
        free(model);
        return NULL;
    }

    /* Default parameters */
    model->max_iterations = RALPH_DEFAULT_MAX_ITER;
    model->time_limit = RALPH_DEFAULT_TIME_LIMIT;
    model->presolve = 0;  /* Disabled by default - adds overhead on random LPs */
    model->presolve_mask = PRESOLVE_SAFE;
    model->verbose = 0;
    model->telemetry = 1;
    model->mip_gap = RALPH_DEFAULT_MIP_GAP;
    model->max_nodes = RALPH_DEFAULT_NODE_LIMIT;
    model->max_cut_rounds = 0;  /* Disabled by default */
    model->method = 0;  /* Default: primal simplex */
    model->pricing = 2; /* Default: Devex */
    model->scaling = 1;    /* Default: single-round geometric mean */
    model->crash = 0;      /* Default: off (all-slack basis) */
    model->verify = 0;     /* Default: off (no post-solve verification) */
    model->objective_limit = RALPH_INFINITY; /* Default: no limit */
    model->phase1_pricing = -1; /* Default: disabled (use solver pricing) */
    model->detect_special = 0; /* Default: disabled for fair benchmarking */
    model->node_pool_capacity = 1024; /* Default B&B node pool size */
    model->node_select = 3; /* Default: hybrid */
    model->trace_phase1 = 0;
    model->dual_bound_flip = -1;    /* -1 = use default (on) */
    model->dual_steepest_edge = -1; /* -1 = use default (on) */
    model->var_select = -1;         /* -1 = use MIP solver default */

    model->status = RALPH_STATUS_UNKNOWN;
    model->mip_start = NULL;
    model->mip_start_mask = NULL;
    model->mip_start_n = 0;
    model->mip_start_nnz = 0;
    model->mip_start_status = RALPH_MIP_START_NONE;
    model->mip_start_repair_mode = RALPH_MIP_START_REPAIR_STRICT;
    ralph_reset_presolve_report(model);

    return model;
}

void ralph_free(RalphModel *model) {
    if (!model) return;

    lp_model_free(model->lp_model);
    simplex_free(model->lp_solver);
    mip_free(model->mip_solver);
    free(model->solution);
    free(model->dual_solution);
    free(model->reduced_costs);
    free(model->unbounded_ray);
    ralph_clear_staged_basis(model);
    ralph_clear_mip_start_internal(model);
    free(model->branch_priorities);
    free(model->branch_directions);
    free(model);
}

/* ============================================================================
 * Model Building
 * ============================================================================ */

int ralph_set_obj_sense(RalphModel *model, RalphObjSense sense) {
    if (!model || !model->lp_model) return -1;
    model->lp_model->obj_sense = (int)sense;
    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_add_var(RalphModel *model, double lb, double ub, double obj, RalphVarType type) {
    if (!model || !model->lp_model) return -1;
    int rc = lp_model_add_var(model->lp_model, lb, ub, obj, (char)type);
    if (rc < 0) return rc;
    ralph_invalidate_solve_state(model);
    return rc;
}

int ralph_add_vars(RalphModel *model, int count, const double *lb, const double *ub,
                   const double *obj, const RalphVarType *types) {
    if (!model || !model->lp_model || count <= 0) return -1;

    for (int i = 0; i < count; i++) {
        double l = lb ? lb[i] : 0.0;
        double u = ub ? ub[i] : RALPH_INFINITY;
        double o = obj ? obj[i] : 0.0;
        RalphVarType t = types ? types[i] : RALPH_CONTINUOUS;

        if (lp_model_add_var(model->lp_model, l, u, o, (char)t) < 0) {
            return -1;
        }
    }

    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_add_constraint(RalphModel *model, int nnz, const int *indices,
                         const double *values, RalphSense sense, double rhs) {
    if (!model || !model->lp_model) return -1;
    int rc = lp_model_add_constraint(model->lp_model, nnz, indices, values, (char)sense, rhs);
    if (rc < 0) return -1;
    ralph_invalidate_solve_state(model);
    return rc;
}

/* ============================================================================
 * Model Modification
 * ============================================================================ */

int ralph_set_var_bounds(RalphModel *model, int var, double lb, double ub) {
    if (!model || !model->lp_model) return -1;
    if (var < 0 || var >= model->lp_model->num_vars) return -1;

    model->lp_model->lb[var] = lb;
    model->lp_model->ub[var] = ub;
    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_set_var_type(RalphModel *model, int var, RalphVarType type) {
    if (!model || !model->lp_model) return -1;
    if (var < 0 || var >= model->lp_model->num_vars) return -1;

    char old_type = model->lp_model->var_type[var];
    model->lp_model->var_type[var] = (char)type;

    /* Update integer counts */
    if ((old_type == 'I' || old_type == 'B') && type == RALPH_CONTINUOUS) {
        model->lp_model->num_integers--;
        if (old_type == 'B') model->lp_model->num_binary--;
    } else if (old_type == 'C' && (type == RALPH_INTEGER || type == RALPH_BINARY)) {
        model->lp_model->num_integers++;
        if (type == RALPH_BINARY) model->lp_model->num_binary++;
    }

    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_set_obj_coef(RalphModel *model, int var, double coef) {
    if (!model || !model->lp_model) return -1;
    if (var < 0 || var >= model->lp_model->num_vars) return -1;

    model->lp_model->c[var] = coef;
    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_set_obj_offset(RalphModel *model, double offset) {
    if (!model || !model->lp_model) return -1;
    model->lp_model->obj_offset = offset;
    ralph_invalidate_solve_state(model);
    return 0;
}

double ralph_get_obj_offset(const RalphModel *model) {
    if (!model || !model->lp_model) return 0.0;
    return model->lp_model->obj_offset;
}

/* ============================================================================
 * Model Queries
 * ============================================================================ */

int ralph_get_num_vars(const RalphModel *model) {
    return model && model->lp_model ? model->lp_model->num_vars : 0;
}

int ralph_get_num_cons(const RalphModel *model) {
    return model && model->lp_model ? model->lp_model->num_cons : 0;
}

int ralph_get_num_integers(const RalphModel *model) {
    return model && model->lp_model ? model->lp_model->num_integers : 0;
}

int ralph_is_mip(const RalphModel *model) {
    return model && model->lp_model && model->lp_model->num_integers > 0;
}

/* ============================================================================
 * Solving
 * ============================================================================ */

typedef enum {
    RALPH_SOLVE_AUTO = 0,
    RALPH_SOLVE_LP_ONLY = 1,
    RALPH_SOLVE_MIP_ONLY = 2
} RalphSolveMode;

static int ralph_optimize_with_mode(RalphModel *model, RalphSolveMode mode) {
    if (!model || !model->lp_model) return -1;

    int model_is_mip = ralph_is_mip(model);
    if (mode == RALPH_SOLVE_LP_ONLY && model_is_mip) {
        model->status = RALPH_STATUS_ERROR;
        return -1;
    }
    if (mode == RALPH_SOLVE_MIP_ONLY && !model_is_mip) {
        model->status = RALPH_STATUS_ERROR;
        return -1;
    }
    int solve_as_mip = (mode == RALPH_SOLVE_MIP_ONLY) ? 1 :
                       (mode == RALPH_SOLVE_LP_ONLY) ? 0 : model_is_mip;

    /* Finalize model if needed */
    if (!model->lp_model->A) {
        if (lp_model_finalize(model->lp_model) != 0) {
            model->status = RALPH_STATUS_ERROR;
            return -1;
        }
    }

    /* Free previous solution */
    free(model->solution);
    free(model->dual_solution);
    free(model->reduced_costs);
    free(model->unbounded_ray);
    model->solution = NULL;
    model->dual_solution = NULL;
    model->reduced_costs = NULL;
    model->unbounded_ray = NULL;
    model->unbounded_ray_valid = 0;
    ralph_reset_presolve_report(model);

    int n_orig = model->lp_model->num_vars;
    int m_orig = model->lp_model->num_cons;

    /*
     * Try special structure detection BEFORE presolve.
     * LAP and network problems have tight structure that presolve can't simplify,
     * and presolve is expensive for large problems. Detecting structure first
     * and solving directly saves the presolve overhead.
     */

    /* Try LAP detection first (most specific) */
    if (model->detect_special && ralph_get_detect_lap()) {
        LAPSignature lap_sig;
        if (detect_lap(model->lp_model, &lap_sig)) {
            if (model->verbose) {
                printf("Detected LAP structure: %dx%d assignment problem\n",
                       lap_sig.n, lap_sig.n);
                printf("Solving directly with JVC (skipping presolve)\n");
            }

            /* Solve as LAP - bypasses presolve and MIP infrastructure */
            model->solution = (double*)calloc(n_orig, sizeof(double));
            if (model->solution) {
                double lap_obj;
                if (solve_as_lap(&lap_sig, model->solution, &lap_obj) == 0) {
                    model->status = RALPH_STATUS_OPTIMAL;
                    model->obj_value = lap_obj;
                    model->best_bound = lap_obj;
                    model->node_count = 0;
                    model->iteration_count = 0;

                    detect_lap_free(&lap_sig);
                    return 0;
                }
                free(model->solution);
                model->solution = NULL;
            }
            detect_lap_free(&lap_sig);
            /* Fall through to normal path if LAP solve failed */
        }
    }

    /* Try network detection (more general than LAP) */
    if (model->detect_special && ralph_get_detect_network()) {
        NetworkSignature net_sig;
        if (detect_network(model->lp_model, &net_sig)) {
            RalphNetworkType type = detect_network_type(&net_sig);
            if (model->verbose) {
                const char *type_str = "general";
                if (type == RALPH_NETWORK_ASSIGNMENT) type_str = "assignment";
                else if (type == RALPH_NETWORK_TRANSPORTATION) type_str = "transportation";
                else if (type == RALPH_NETWORK_SHORTEST_PATH) type_str = "shortest path";
                printf("Detected network structure: %d nodes, %d arcs (%s)\n",
                       net_sig.num_nodes, net_sig.num_arcs, type_str);
                printf("Solving with network simplex (skipping presolve)\n");
            }

            /* Solve as network flow - bypasses presolve and MIP infrastructure */
            model->solution = (double*)calloc(n_orig, sizeof(double));
            if (model->solution) {
                double net_obj;
                if (solve_as_network(&net_sig, model->solution, &net_obj) == 0) {
                    model->status = RALPH_STATUS_OPTIMAL;
                    model->obj_value = net_obj;
                    model->best_bound = net_obj;
                    model->node_count = 0;
                    model->iteration_count = 0;

                    detect_network_free(&net_sig);
                    return 0;
                }
                free(model->solution);
                model->solution = NULL;
            }
            detect_network_free(&net_sig);
            /* Fall through to normal path if network solve failed */
        }
    }

    /* Apply presolve if enabled (special structure already handled above).
     * For MIP: auto-enable lightweight presolve (0x110F) unless user
     * explicitly disabled it. Avoids SINGLETON_COLS, PROBING, and
     * PROPORTIONAL_ROWS which interact badly in B&B. */
    PresolveResult *presolved = NULL;
    LPModel *solve_model = model->lp_model;
    int use_presolve = model->presolve;
    unsigned int use_mask = model->presolve_mask;

    if (!use_presolve && solve_as_mip && model->presolve != -1) {
        use_presolve = 1;
        use_mask = 0x110F;  /* FIXED+EMPTY+SINGL_ROW+BOUND_TIGHT+SHIFT */
    }

    if (use_presolve > 0) {
        clock_t presolve_start = clock();
        presolved = presolve_with_mask(model->lp_model, use_mask);
        clock_t presolve_end = clock();
        model->last_presolve_report.used = 1;
        model->last_presolve_report.mask = use_mask;
        model->last_presolve_report.presolve_time_ms =
            1000.0 * (double)(presolve_end - presolve_start) / CLOCKS_PER_SEC;
        if (presolved) {
            model->last_presolve_report.rounds = presolved->rounds;
            model->last_presolve_report.vars_removed = presolved->vars_removed;
            model->last_presolve_report.cons_removed = presolved->cons_removed;
            model->last_presolve_report.bounds_tightened = presolved->bounds_tightened;
            model->last_presolve_report.matrix_rank = presolved->matrix_rank;
            model->last_presolve_report.redundant_rows_found = presolved->redundant_rows_found;
        }
        if (presolved && presolved->reduced_model) {
            solve_model = presolved->reduced_model;
            if (model->verbose) {
                printf("Presolve: %d vars removed, %d cons removed, %d bounds tightened",
                       presolved->vars_removed, presolved->cons_removed,
                       presolved->bounds_tightened);
                if (presolved->matrix_rank > 0) {
                    printf(", matrix rank=%d", presolved->matrix_rank);
                }
                printf("\n");
            }
        }
    }

    /* Handle trivially solved model: presolve eliminated all constraints.
     * With 0 constraints, the optimal is determined by bounds alone:
     * set each variable to its best bound for the objective. */
    if (solve_model->num_cons == 0 && solve_model->num_vars > 0) {
        double *trivial_sol = (double*)calloc(solve_model->num_vars, sizeof(double));
        if (trivial_sol) {
            double obj = solve_model->obj_offset;
            for (int j = 0; j < solve_model->num_vars; j++) {
                double c_eff = solve_model->c[j] * solve_model->obj_sense;
                if (c_eff > RALPH_ZERO_TOL) {
                    trivial_sol[j] = solve_model->lb[j];
                } else if (c_eff < -RALPH_ZERO_TOL) {
                    trivial_sol[j] = solve_model->ub[j];
                } else {
                    trivial_sol[j] = solve_model->lb[j] > -RALPH_INFINITY/2 ?
                                     solve_model->lb[j] : 0.0;
                }
                obj += solve_model->c[j] * trivial_sol[j];
            }

            model->status = RALPH_STATUS_OPTIMAL;
            model->obj_value = obj;
            model->iteration_count = 0;

            model->solution = (double*)calloc(n_orig, sizeof(double));
            if (model->solution) {
                if (presolved && presolved->reduced_model) {
                    postsolve(presolved, trivial_sol, model->solution);
                } else {
                    memcpy(model->solution, trivial_sol, solve_model->num_vars * sizeof(double));
                }
            }
            free(trivial_sol);
            if (presolved) presolve_free(presolved);
            return 0;
        }
    }

    /* Handle completely empty model: 0 vars, 0 constraints */
    if (solve_model->num_vars == 0) {
        model->status = RALPH_STATUS_OPTIMAL;
        model->obj_value = solve_model->obj_offset;
        model->iteration_count = 0;

        model->solution = (double*)calloc(n_orig, sizeof(double));
        if (model->solution && presolved && presolved->reduced_model) {
            double empty = 0.0;
            postsolve(presolved, &empty, model->solution);
        }
        if (presolved) presolve_free(presolved);
        return 0;
    }

    /* Try LAP detection for pure LP (not MIP) - post-presolve fallback.
     * This is a backup in case presolve reveals LAP structure that wasn't
     * detected in the original model (rare but possible). */
    if (!solve_as_mip && model->detect_special && ralph_get_detect_lap()) {
        LAPSignature lap_sig;
        if (detect_lap(solve_model, &lap_sig)) {
            if (model->verbose) {
                printf("Detected LAP structure: %dx%d assignment problem\n",
                       lap_sig.n, lap_sig.n);
            }

            /* Solve as LAP */
            model->solution = (double*)calloc(n_orig, sizeof(double));
            if (model->solution) {
                double lap_obj;
                if (solve_as_lap(&lap_sig, model->solution, &lap_obj) == 0) {
                    model->status = RALPH_STATUS_OPTIMAL;
                    model->obj_value = lap_obj;
                    model->iteration_count = 0;

                    /* Postsolve if presolve was applied */
                    if (presolved && presolved->reduced_model) {
                        double *presolved_sol = model->solution;
                        model->solution = (double*)calloc(n_orig, sizeof(double));
                        if (model->solution) {
                            postsolve(presolved, presolved_sol, model->solution);
                        }
                        free(presolved_sol);
                    }

                    detect_lap_free(&lap_sig);
                    if (presolved) presolve_free(presolved);
                    return 0;
                }
                free(model->solution);
                model->solution = NULL;
            }
            detect_lap_free(&lap_sig);
        }
    }

    /* Try network detection for pure LP (not MIP) - post-presolve fallback.
     * This is a backup in case presolve reveals network structure that wasn't
     * detected in the original model (rare but possible). */
    if (!solve_as_mip && model->detect_special && ralph_get_detect_network()) {
        NetworkSignature net_sig;
        if (detect_network(solve_model, &net_sig)) {
            if (model->verbose) {
                RalphNetworkType type = detect_network_type(&net_sig);
                const char *type_str = "general";
                if (type == RALPH_NETWORK_ASSIGNMENT) type_str = "assignment";
                else if (type == RALPH_NETWORK_TRANSPORTATION) type_str = "transportation";
                else if (type == RALPH_NETWORK_SHORTEST_PATH) type_str = "shortest path";
                printf("Detected network structure: %d nodes, %d arcs (%s)\n",
                       net_sig.num_nodes, net_sig.num_arcs, type_str);
            }

            /* Solve as network flow */
            model->solution = (double*)calloc(n_orig, sizeof(double));
            if (model->solution) {
                double net_obj;
                if (solve_as_network(&net_sig, model->solution, &net_obj) == 0) {
                    model->status = RALPH_STATUS_OPTIMAL;
                    model->obj_value = net_obj;
                    model->iteration_count = 0;

                    /* Postsolve if presolve was applied */
                    if (presolved && presolved->reduced_model) {
                        double *presolved_sol = model->solution;
                        model->solution = (double*)calloc(n_orig, sizeof(double));
                        if (model->solution) {
                            postsolve(presolved, presolved_sol, model->solution);
                        }
                        free(presolved_sol);
                    }

                    detect_network_free(&net_sig);
                    if (presolved) presolve_free(presolved);
                    return 0;
                }
                free(model->solution);
                model->solution = NULL;
            }
            detect_network_free(&net_sig);
        }
    }

    if (solve_as_mip) {
        /* MIP solve - LAP and network problems were already handled above before presolve */
        model->mip_solver = mip_create(solve_model, model->detect_special, model->node_pool_capacity);
        if (!model->mip_solver) {
            if (presolved) presolve_free(presolved);
            model->status = RALPH_STATUS_ERROR;
            return -1;
        }

        /* Set parameters */
        model->mip_solver->max_nodes = model->max_nodes;
        model->mip_solver->time_limit = model->time_limit;
        model->mip_solver->mip_gap = model->mip_gap;
        model->mip_solver->verbose = model->verbose;
        model->mip_solver->telemetry = model->telemetry ? 1 : 0;
        model->mip_solver->max_cut_rounds = model->max_cut_rounds;
        model->mip_solver->dual_bound_flip = model->dual_bound_flip;
        model->mip_solver->dual_steepest_edge = model->dual_steepest_edge;
        model->mip_solver->lu_supernode = model->lu_supernode;
        model->mip_solver->mip_start_repair_mode = (int)model->mip_start_repair_mode;

        /* Set node selection strategy */
        model->mip_solver->node_select = (NodeSelectStrategy)model->node_select;
        if (model->mip_solver->node_queue) {
            model->mip_solver->node_queue->strategy = (NodeSelectStrategy)model->node_select;
        }

        /* Set variable selection strategy (overrides default if explicitly set) */
        if (model->var_select >= 0) {
            model->mip_solver->var_select = (VarSelectStrategy)model->var_select;
        }

        /* Pass branching control data to MIP solver.
         * When presolve is active, remap from original to presolved indices
         * using var_map[reduced_j] → original_j. */
        if (model->branch_priorities) {
            int n_solve = solve_model->num_vars;
            model->mip_solver->branch_priorities = (int*)malloc(n_solve * sizeof(int));
            if (model->mip_solver->branch_priorities) {
                if (presolved && presolved->var_map) {
                    for (int j = 0; j < n_solve; j++) {
                        int orig = presolved->var_map[j];
                        model->mip_solver->branch_priorities[j] =
                            (orig >= 0) ? model->branch_priorities[orig] : 0;
                    }
                } else {
                    memcpy(model->mip_solver->branch_priorities, model->branch_priorities,
                           n_solve * sizeof(int));
                }
            }
        }
        if (model->branch_directions) {
            int n_solve = solve_model->num_vars;
            model->mip_solver->branch_directions = (int*)malloc(n_solve * sizeof(int));
            if (model->mip_solver->branch_directions) {
                if (presolved && presolved->var_map) {
                    for (int j = 0; j < n_solve; j++) {
                        int orig = presolved->var_map[j];
                        model->mip_solver->branch_directions[j] =
                            (orig >= 0) ? model->branch_directions[orig] : 0;
                    }
                } else {
                    memcpy(model->mip_solver->branch_directions, model->branch_directions,
                           n_solve * sizeof(int));
                }
            }
        }

        /* Pass cut callback to MIP solver */
        if (model->has_cut_callback) {
            model->mip_solver->cut_callback = model->cut_callback;
            model->mip_solver->has_cut_callback = 1;
        }

        /* Pass branch callback to MIP solver */
        if (model->has_branch_callback) {
            model->mip_solver->branch_callback = model->branch_callback;
            model->mip_solver->has_branch_callback = 1;
        }

        /* Pass staged MIP start to the (possibly presolved) MIP model. */
        if (model->mip_start) {
            int n_solve = solve_model->num_vars;
            int mapped_ok = (model->mip_start_n == n_orig && n_solve > 0);
            double *solve_start = NULL;
            int *solve_mask = NULL;

            if (mapped_ok) {
                solve_start = (double*)malloc((size_t)n_solve * sizeof(double));
                solve_mask = (int*)calloc((size_t)n_solve, sizeof(int));
                if (!solve_start) {
                    if (presolved) presolve_free(presolved);
                    model->status = RALPH_STATUS_ERROR;
                    return -1;
                }
                if (!solve_mask) {
                    free(solve_start);
                    if (presolved) presolve_free(presolved);
                    model->status = RALPH_STATUS_ERROR;
                    return -1;
                }

                if (presolved && presolved->var_map) {
                    for (int j = 0; j < n_solve; j++) {
                        int orig = presolved->var_map[j];
                        if (orig < 0 || orig >= model->mip_start_n) {
                            mapped_ok = 0;
                            break;
                        }
                        solve_start[j] = model->mip_start[orig];
                        solve_mask[j] = (model->mip_start_mask && model->mip_start_mask[orig]) ? 1 : 0;
                    }
                } else {
                    memcpy(solve_start, model->mip_start, (size_t)n_solve * sizeof(double));
                    if (model->mip_start_mask) {
                        memcpy(solve_mask, model->mip_start_mask, (size_t)n_solve * sizeof(int));
                    } else {
                        for (int j = 0; j < n_solve; j++) solve_mask[j] = 1;
                    }
                }
            }

            if (mapped_ok &&
                mip_set_start_ex(model->mip_solver, solve_start, solve_mask,
                                 n_solve, (int)model->mip_start_repair_mode) == 0) {
                model->mip_start_status = RALPH_MIP_START_PENDING;
            } else {
                model->mip_start_status = RALPH_MIP_START_REJECTED;
            }
            free(solve_start);
            free(solve_mask);
        }

        /* Solve */
        mip_solve(model->mip_solver);

        model->status = model->mip_solver->status;
        if (model->mip_start && model->mip_solver->mip_start_attempted > 0) {
            model->mip_start_status = (model->mip_solver->mip_start_accepted > 0) ?
                                      RALPH_MIP_START_ACCEPTED :
                                      RALPH_MIP_START_REJECTED;
        }

        if (model->mip_solver->has_incumbent) {
            model->obj_value = model->mip_solver->best_obj;
            model->best_bound = model->mip_solver->best_bound;
            model->node_count = model->mip_solver->nodes_explored;

            /* Copy solution - postsolve if presolve was applied */
            model->solution = (double*)calloc(n_orig, sizeof(double));
            if (model->solution) {
                if (presolved && presolved->reduced_model) {
                    /* Postsolve: recover original solution from presolved */
                    postsolve(presolved, model->mip_solver->best_solution, model->solution);
                } else {
                    memcpy(model->solution, model->mip_solver->best_solution, n_orig * sizeof(double));
                }
            }
        }

        /* Free presolve result */
        if (presolved) {
            presolve_free(presolved);
            presolved = NULL;
        }
    } else {
        /* LP solve */
        model->lp_solver = simplex_create(solve_model);
        if (!model->lp_solver) {
            if (presolved) presolve_free(presolved);
            model->status = RALPH_STATUS_ERROR;
            return -1;
        }

        /* Set parameters */
        model->lp_solver->max_iterations = model->max_iterations;
        model->lp_solver->time_limit = model->time_limit;
        model->lp_solver->verbose = model->verbose;
        model->lp_solver->telemetry_enabled = model->telemetry ? 1 : 0;
        model->lp_solver->presolve = 0;  /* Already done */
        model->lp_solver->pricing_strategy = model->pricing;
        model->lp_solver->scaling = model->scaling;
        model->lp_solver->crash = model->crash;
        model->lp_solver->verify = model->verify;
        model->lp_solver->phase1_pricing = model->phase1_pricing;
        /* Convert objective limit from user space to internal minimization space */
        if (model->objective_limit < RALPH_INFINITY) {
            model->lp_solver->objective_limit = model->objective_limit * solve_model->obj_sense;
        } else {
            model->lp_solver->objective_limit = RALPH_INFINITY;
        }
        model->lp_solver->force_two_phase = model->force_two_phase;
        model->lp_solver->trace_phase1 = model->trace_phase1;
        model->lp_solver->method = model->method;
        model->lp_solver->lp_progress_callback = model->lp_progress_callback;
        model->lp_solver->has_lp_progress_callback = model->has_lp_progress_callback;
        model->lp_solver->lp_cancel_callback = model->lp_cancel_callback;
        model->lp_solver->has_lp_cancel_callback = model->has_lp_cancel_callback;
        if (model->dual_bound_flip >= 0)
            model->lp_solver->use_dual_bound_flip = model->dual_bound_flip;
        if (model->dual_steepest_edge >= 0)
            model->lp_solver->use_dual_steepest_edge = model->dual_steepest_edge;
        model->lp_solver->lu_supernode = model->lu_supernode;

        if (model->staged_basis && model->staged_var_status) {
            if (simplex_set_warm_basis(model->lp_solver,
                                       model->staged_basis_m,
                                       model->staged_basis_n,
                                       model->staged_basis,
                                       model->staged_var_status) != 0) {
                if (presolved) presolve_free(presolved);
                model->status = RALPH_STATUS_ERROR;
                return -1;
            }
            ralph_clear_staged_basis(model);
        }

        /* Solve — method dispatch (primal/dual/auto) is handled inside simplex_solve */
        simplex_solve(model->lp_solver);

        model->status = model->lp_solver->status;
        model->iteration_count = model->lp_solver->iterations;
        model->unbounded_ray_valid = 0;

        if (model->status == RALPH_STATUS_OPTIMAL ||
            model->status == RALPH_STATUS_IMPRECISE ||
            model->status == RALPH_STATUS_OBJ_LIMIT) {
            model->obj_value = model->lp_solver->obj_value;

            /* Allocate solution arrays for original problem size */
            model->solution = (double*)calloc(n_orig, sizeof(double));
            model->dual_solution = (double*)calloc(m_orig, sizeof(double));
            model->reduced_costs = (double*)calloc(n_orig, sizeof(double));

            if (!model->solution || !model->dual_solution || !model->reduced_costs) {
                free(model->solution); model->solution = NULL;
                free(model->dual_solution); model->dual_solution = NULL;
                free(model->reduced_costs); model->reduced_costs = NULL;
                if (presolved) presolve_free(presolved);
                model->status = RALPH_STATUS_ERROR;
                return -1;
            }

            if (presolved && presolved->reduced_model) {
                /* Postsolve: recover original solution */
                if (model->solution && model->lp_solver->solution) {
                    postsolve(presolved, model->lp_solver->solution, model->solution);
                }
                /* Dual values need postsolve too - for now copy what we have */
                int m_reduced = solve_model->num_cons;
                if (model->dual_solution && model->lp_solver->dual_solution) {
                    for (int i = 0; i < m_reduced && i < m_orig; i++) {
                        int orig_con = presolved->con_map ? presolved->con_map[i] : i;
                        if (orig_con >= 0 && orig_con < m_orig) {
                            model->dual_solution[orig_con] = model->lp_solver->dual_solution[i];
                        }
                    }
                }
            } else {
                /* No presolve - direct copy */
                if (model->solution && model->lp_solver->solution) {
                    memcpy(model->solution, model->lp_solver->solution, n_orig * sizeof(double));
                }
                if (model->dual_solution && model->lp_solver->dual_solution) {
                    memcpy(model->dual_solution, model->lp_solver->dual_solution, m_orig * sizeof(double));
                }
                if (model->reduced_costs && model->lp_solver->reduced_costs) {
                    memcpy(model->reduced_costs, model->lp_solver->reduced_costs, n_orig * sizeof(double));
                }
            }
        } else if (model->status == RALPH_STATUS_UNBOUNDED &&
                   model->lp_solver->unbounded_valid &&
                   model->lp_solver->unbounded_ray) {
            model->unbounded_ray = (double*)calloc((size_t)n_orig, sizeof(double));
            if (model->unbounded_ray) {
                int mapped = 0;
                if (presolved && presolved->reduced_model && presolved->var_map) {
                    int n_reduced = solve_model->num_vars;
                    for (int j = 0; j < n_reduced; j++) {
                        int orig = presolved->var_map[j];
                        if (orig >= 0 && orig < n_orig) {
                            model->unbounded_ray[orig] = model->lp_solver->unbounded_ray[j];
                        }
                    }
                    mapped = 1;
                } else if (solve_model->num_vars == n_orig) {
                    memcpy(model->unbounded_ray, model->lp_solver->unbounded_ray,
                           (size_t)n_orig * sizeof(double));
                    mapped = 1;
                }
                if (mapped) {
                    model->unbounded_ray_valid = 1;
                } else {
                    free(model->unbounded_ray);
                    model->unbounded_ray = NULL;
                }
            }
        }
    }

    /* Free presolve result */
    if (presolved) {
        presolve_free(presolved);
    }

    return 0;
}

int ralph_optimize(RalphModel *model) {
    return ralph_optimize_with_mode(model, RALPH_SOLVE_AUTO);
}

int ralph_optimize_lp(RalphModel *model) {
    return ralph_optimize_with_mode(model, RALPH_SOLVE_LP_ONLY);
}

int ralph_optimize_mip(RalphModel *model) {
    return ralph_optimize_with_mode(model, RALPH_SOLVE_MIP_ONLY);
}

/* ============================================================================
 * Solution Retrieval
 * ============================================================================ */

RalphStatus ralph_get_status(const RalphModel *model) {
    return model ? model->status : RALPH_STATUS_UNKNOWN;
}

double ralph_get_objval(const RalphModel *model) {
    return model ? model->obj_value : 0.0;
}

int ralph_get_solution(const RalphModel *model, double *x) {
    if (!model || !x || !model->solution) return -1;

    int n = ralph_get_num_vars(model);
    memcpy(x, model->solution, n * sizeof(double));
    return 0;
}

int ralph_get_dual_solution(const RalphModel *model, double *y) {
    if (!model || !y || !model->dual_solution) return -1;

    int m = ralph_get_num_cons(model);
    memcpy(y, model->dual_solution, m * sizeof(double));
    return 0;
}

int ralph_get_reduced_costs(const RalphModel *model, double *rc) {
    if (!model || !rc || !model->reduced_costs) return -1;

    int n = ralph_get_num_vars(model);
    memcpy(rc, model->reduced_costs, n * sizeof(double));
    return 0;
}

int ralph_get_farkas_ray(const RalphModel *model, double *ray) {
    if (!model || !ray) return -1;

    /* Check if status is infeasible and we have a valid Farkas ray */
    if (model->status != RALPH_STATUS_INFEASIBLE) return -1;

    /* For LP problems, get the ray from the simplex solver */
    if (model->lp_solver && model->lp_solver->farkas_valid && model->lp_solver->farkas_ray) {
        int m = ralph_get_num_cons(model);
        memcpy(ray, model->lp_solver->farkas_ray, m * sizeof(double));
        return 0;
    }

    /* No valid Farkas ray available */
    return -1;
}

int ralph_get_unbounded_ray(const RalphModel *model, double *ray) {
    if (!model || !ray) return -1;
    if (model->status != RALPH_STATUS_UNBOUNDED) return -1;

    int n = ralph_get_num_vars(model);
    if (n <= 0) return -1;

    if (model->unbounded_ray_valid && model->unbounded_ray) {
        memcpy(ray, model->unbounded_ray, (size_t)n * sizeof(double));
        return 0;
    }

    if (model->lp_solver &&
        model->lp_solver->unbounded_valid &&
        model->lp_solver->unbounded_ray &&
        model->lp_solver->model &&
        model->lp_solver->model->num_vars == n) {
        memcpy(ray, model->lp_solver->unbounded_ray, (size_t)n * sizeof(double));
        return 0;
    }

    return -1;
}

int ralph_compute_lp_iis(const RalphModel *model, int *row_flags, int *iis_size) {
    if (!model || !model->lp_model || !row_flags) return -1;
    if (iis_size) *iis_size = 0;
    if (ralph_is_mip(model)) return -1;
    if (model->status != RALPH_STATUS_INFEASIBLE) return -1;

    int m = model->lp_model->num_cons;
    if (m <= 0) return -1;
    memset(row_flags, 0, (size_t)m * sizeof(int));

    LPModel *work = lp_model_copy(model->lp_model);
    if (!work) return -1;

    int *active_rows = (int*)malloc((size_t)m * sizeof(int));
    if (!active_rows) {
        lp_model_free(work);
        return -1;
    }
    for (int i = 0; i < m; i++) active_rows[i] = i;
    int active_count = m;

    int pos = 0;
    while (pos < active_count) {
        LPModel *test = lp_model_copy(work);
        if (!test) {
            free(active_rows);
            lp_model_free(work);
            return -1;
        }

        if (lp_model_delete_constraint(test, pos) != 0) {
            lp_model_free(test);
            free(active_rows);
            lp_model_free(work);
            return -1;
        }

        RalphStatus probe_status = RALPH_STATUS_ERROR;
        if (ralph_probe_lp_status(model, test, &probe_status) != 0) {
            lp_model_free(test);
            free(active_rows);
            lp_model_free(work);
            return -1;
        }
        lp_model_free(test);

        if (probe_status == RALPH_STATUS_INFEASIBLE) {
            /* Row at this position is redundant for infeasibility; drop it. */
            if (lp_model_delete_constraint(work, pos) != 0) {
                free(active_rows);
                lp_model_free(work);
                return -1;
            }
            if (lp_model_finalize(work) != 0) {
                free(active_rows);
                lp_model_free(work);
                return -1;
            }
            for (int k = pos + 1; k < active_count; k++) {
                active_rows[k - 1] = active_rows[k];
            }
            active_count--;
            continue;
        }

        if (probe_status == RALPH_STATUS_ERROR ||
            probe_status == RALPH_STATUS_TIME_LIMIT ||
            probe_status == RALPH_STATUS_ITERATION_LIMIT) {
            free(active_rows);
            lp_model_free(work);
            return -1;
        }

        pos++;
    }

    for (int i = 0; i < active_count; i++) {
        int row = active_rows[i];
        if (row >= 0 && row < m) row_flags[row] = 1;
    }
    if (iis_size) *iis_size = active_count;

    free(active_rows);
    lp_model_free(work);
    return 0;
}

/* ============================================================================
 * MIP-Specific
 * ============================================================================ */

double ralph_get_best_bound(const RalphModel *model) {
    return model ? model->best_bound : 0.0;
}

double ralph_get_mip_gap(const RalphModel *model) {
    if (!model || !ralph_is_mip(model)) return 0.0;

    double gap = fabs(model->obj_value - model->best_bound);
    return gap / (fabs(model->obj_value) + 1e-10);
}

int ralph_get_node_count(const RalphModel *model) {
    return model ? model->node_count : 0;
}

int ralph_get_iterations(const RalphModel *model) {
    return model ? model->iteration_count : 0;
}

static const SimplexSolver* ralph_get_last_lp_solver_for_reports(const RalphModel *model) {
    if (!model) return NULL;
    if (model->lp_solver) return model->lp_solver;
    if (model->mip_solver && model->mip_solver->lp_solver) {
        return model->mip_solver->lp_solver;
    }
    return NULL;
}

int ralph_get_last_presolve_report(const RalphModel *model, RalphPresolveReport *report) {
    if (!model || !report) return -1;
    *report = model->last_presolve_report;
    return 0;
}

int ralph_get_last_lp_telemetry(const RalphModel *model, RalphLPSolverTelemetry *telemetry) {
    if (!model || !telemetry) return -1;

    memset(telemetry, 0, sizeof(*telemetry));
    const SimplexSolver *lp = ralph_get_last_lp_solver_for_reports(model);
    if (!lp) return 0;

    LPSolverTelemetrySnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    lp_telemetry_snapshot_solver(lp, &snapshot);

    if (sizeof(*telemetry) != sizeof(snapshot)) return -1;
    memcpy(telemetry, &snapshot, sizeof(*telemetry));
    return 0;
}

int ralph_get_last_lu_telemetry(const RalphModel *model, RalphLUTelemetry *telemetry) {
    if (!model || !telemetry) return -1;

    memset(telemetry, 0, sizeof(*telemetry));
    const SimplexSolver *lp = ralph_get_last_lp_solver_for_reports(model);
    if (!lp || !lp->tableau || !lp->tableau->lu) return 0;

    LUTelemetrySnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    lp_telemetry_snapshot_lu(lp->tableau->lu, &snapshot);

    if (sizeof(*telemetry) != sizeof(snapshot)) return -1;
    memcpy(telemetry, &snapshot, sizeof(*telemetry));
    return 0;
}

int ralph_get_solution_quality(const RalphModel *model, RalphSolutionQuality *quality) {
    if (!model || !quality) return -1;

    memset(quality, 0, sizeof(*quality));
    quality->status = model->status;

    const SimplexSolver *lp = ralph_get_last_lp_solver_for_reports(model);
    if (!lp) {
        quality->verify_enabled = model->verify ? 1 : 0;
        return 0;
    }

    quality->verify_enabled = lp->verify ? 1 : 0;
    if (!quality->verify_enabled) return 0;

    if (model->status != RALPH_STATUS_OPTIMAL &&
        model->status != RALPH_STATUS_IMPRECISE &&
        model->status != RALPH_STATUS_OBJ_LIMIT) {
        return 0;
    }

    quality->available = 1;
    quality->primal_infeas = lp->verify_primal_infeas;
    quality->bound_infeas = lp->verify_bound_infeas;
    quality->dual_infeas = lp->verify_dual_infeas;
    quality->comp_slack = lp->verify_comp_slack;
    quality->obj_error = lp->verify_obj_error;
    quality->cond_estimate = lp->verify_cond_estimate;
    return 0;
}

/* ============================================================================
 * Branching Control
 * ============================================================================ */

int ralph_set_branch_priorities(RalphModel *model, const int *priorities) {
    if (!model) return -1;

    /* Free existing priorities */
    free(model->branch_priorities);
    model->branch_priorities = NULL;

    if (!priorities) return 0;  /* Clear priorities */

    int n = ralph_get_num_vars(model);
    if (n <= 0) return -1;

    model->branch_priorities = (int*)malloc(n * sizeof(int));
    if (!model->branch_priorities) return -1;

    memcpy(model->branch_priorities, priorities, n * sizeof(int));
    return 0;
}

int ralph_set_branch_directions(RalphModel *model, const int *directions) {
    if (!model) return -1;

    /* Free existing directions */
    free(model->branch_directions);
    model->branch_directions = NULL;

    if (!directions) return 0;  /* Clear directions */

    int n = ralph_get_num_vars(model);
    if (n <= 0) return -1;

    model->branch_directions = (int*)malloc(n * sizeof(int));
    if (!model->branch_directions) return -1;

    memcpy(model->branch_directions, directions, n * sizeof(int));
    return 0;
}

int ralph_set_mip_start(RalphModel *model, const double *x) {
    if (!model || !model->lp_model || !x) return -1;

    int n = model->lp_model->num_vars;
    if (n <= 0) return -1;
    if (ralph_set_mip_start_copy(model, x, NULL, n) != 0) return -1;
    return 0;
}

int ralph_set_mip_start_sparse(RalphModel *model, int count,
                               const int *indices, const double *values) {
    if (!model || !model->lp_model) return -1;
    if (count < 0) return -1;
    if (count > 0 && (!indices || !values)) return -1;

    int n = model->lp_model->num_vars;
    if (n <= 0) return -1;

    if (!model->mip_start || model->mip_start_n != n || !model->mip_start_mask) {
        double *dense = (double*)malloc((size_t)n * sizeof(double));
        int *mask = (int*)calloc((size_t)n, sizeof(int));
        if (!dense || !mask) {
            free(dense);
            free(mask);
            return -1;
        }
        for (int j = 0; j < n; j++) dense[j] = model->lp_model->lb[j];
        free(model->mip_start);
        free(model->mip_start_mask);
        model->mip_start = dense;
        model->mip_start_mask = mask;
        model->mip_start_n = n;
        model->mip_start_nnz = 0;
    }

    for (int k = 0; k < count; k++) {
        int j = indices[k];
        if (j < 0 || j >= n) return -1;
        model->mip_start[j] = values[k];
        if (!model->mip_start_mask[j]) {
            model->mip_start_mask[j] = 1;
            model->mip_start_nnz++;
        }
    }

    model->mip_start_status = RALPH_MIP_START_PENDING;
    return 0;
}

void ralph_clear_mip_start(RalphModel *model) {
    if (!model) return;
    ralph_clear_mip_start_internal(model);
}

RalphMIPStartStatus ralph_get_mip_start_status(const RalphModel *model) {
    if (!model) return RALPH_MIP_START_NONE;
    return model->mip_start_status;
}

int ralph_set_mip_start_repair_mode(RalphModel *model, RalphMIPStartRepairMode mode) {
    if (!model) return -1;
    if (mode < RALPH_MIP_START_REPAIR_STRICT ||
        mode > RALPH_MIP_START_REPAIR_PROJECT_AND_ROUND) {
        return -1;
    }
    model->mip_start_repair_mode = mode;
    return 0;
}

RalphMIPStartRepairMode ralph_get_mip_start_repair_mode(const RalphModel *model) {
    if (!model) return RALPH_MIP_START_REPAIR_STRICT;
    return model->mip_start_repair_mode;
}

/* ============================================================================
 * Constraint Modification
 * ============================================================================ */

int ralph_set_constraint_rhs(RalphModel *model, int constraint, double rhs) {
    if (!model || !model->lp_model) return -1;
    if (constraint < 0 || constraint >= model->lp_model->num_cons) return -1;

    model->lp_model->b[constraint] = rhs;
    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_set_constraint_sense(RalphModel *model, int constraint, RalphSense sense) {
    if (!model || !model->lp_model) return -1;
    if (constraint < 0 || constraint >= model->lp_model->num_cons) return -1;
    if (!ralph_is_valid_sense(sense)) return -1;

    model->lp_model->sense[constraint] = (char)sense;
    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_set_constraint_coef(RalphModel *model, int constraint, int var, double coef) {
    if (!model || !model->lp_model) return -1;
    if (lp_model_set_coefficient(model->lp_model, constraint, var, coef) != 0) return -1;
    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_set_constraint_coefs(RalphModel *model, int count,
                               const int *constraints, const int *vars,
                               const double *coefs) {
    if (!model || !model->lp_model) return -1;
    if (lp_model_set_coefficients(model->lp_model, count, constraints, vars, coefs) != 0) return -1;
    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_set_constraint_rhs_batch(RalphModel *model, int count,
                                   const int *constraints, const double *rhs_values) {
    if (!model || !model->lp_model || count < 0) return -1;
    if (count == 0) return 0;
    if (!constraints || !rhs_values) return -1;

    int m = model->lp_model->num_cons;
    for (int i = 0; i < count; i++) {
        if (constraints[i] < 0 || constraints[i] >= m) return -1;
    }

    for (int i = 0; i < count; i++) {
        model->lp_model->b[constraints[i]] = rhs_values[i];
    }
    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_set_constraint_sense_batch(RalphModel *model, int count,
                                     const int *constraints, const RalphSense *senses) {
    if (!model || !model->lp_model || count < 0) return -1;
    if (count == 0) return 0;
    if (!constraints || !senses) return -1;

    int m = model->lp_model->num_cons;
    for (int i = 0; i < count; i++) {
        if (constraints[i] < 0 || constraints[i] >= m) return -1;
        if (!ralph_is_valid_sense(senses[i])) return -1;
    }

    for (int i = 0; i < count; i++) {
        model->lp_model->sense[constraints[i]] = (char)senses[i];
    }
    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_get_constraint_rhs(const RalphModel *model, int constraint, double *rhs) {
    if (!model || !model->lp_model || !rhs) return -1;
    if (constraint < 0 || constraint >= model->lp_model->num_cons) return -1;
    *rhs = model->lp_model->b[constraint];
    return 0;
}

int ralph_get_constraint_sense(const RalphModel *model, int constraint, RalphSense *sense) {
    if (!model || !model->lp_model || !sense) return -1;
    if (constraint < 0 || constraint >= model->lp_model->num_cons) return -1;
    char s = model->lp_model->sense[constraint];
    if (s != 'L' && s != 'E' && s != 'G') return -1;
    *sense = (RalphSense)s;
    return 0;
}

int ralph_get_constraint_coef(const RalphModel *model, int constraint, int var, double *coef) {
    if (!model || !model->lp_model) return -1;
    return lp_model_get_coefficient(model->lp_model, constraint, var, coef);
}

int ralph_delete_constraint(RalphModel *model, int constraint) {
    if (!model || !model->lp_model) return -1;
    if (lp_model_delete_constraint(model->lp_model, constraint) != 0) return -1;
    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_delete_var(RalphModel *model, int var) {
    if (!model || !model->lp_model) return -1;
    if (lp_model_delete_var(model->lp_model, var) != 0) return -1;
    ralph_invalidate_solve_state(model);
    return 0;
}

int ralph_get_var_bounds(const RalphModel *model, int var, double *lb, double *ub) {
    if (!model || !model->lp_model) return -1;
    if (var < 0 || var >= model->lp_model->num_vars) return -1;

    if (lb) *lb = model->lp_model->lb[var];
    if (ub) *ub = model->lp_model->ub[var];

    return 0;
}

int ralph_add_lazy_constraint(RalphModel *model, const RalphCut *cut) {
    if (!model || !model->lp_model || !cut) return -1;

    /* Add the constraint to the model */
    int result = lp_model_add_constraint(model->lp_model,
                                          cut->num_vars,
                                          cut->indices,
                                          cut->coeffs,
                                          (char)cut->sense,
                                          cut->rhs);
    if (result < 0) return -1;

    /* Invalidate solver state to force re-solve (will use warm start if available) */
    /* Note: For true warm start, we keep the LP solver but invalidate MIP solver */
    mip_free(model->mip_solver);
    model->mip_solver = NULL;

    /* Keep LP solver for potential warm start, but invalidate cached solution */
    if (model->lp_solver) {
        model->lp_solver->status = RALPH_STATUS_UNKNOWN;
    }
    if (model->mip_start && model->mip_start_n == model->lp_model->num_vars) {
        model->mip_start_status = RALPH_MIP_START_PENDING;
    }

    return 0;
}

int ralph_add_lazy_constraints(RalphModel *model, const RalphCut *cuts, int count) {
    if (!model || !cuts) return -1;
    if (count <= 0) return 0;

    for (int i = 0; i < count; i++) {
        if (ralph_add_lazy_constraint(model, &cuts[i]) < 0) {
            return -1;
        }
    }

    return 0;
}

/* ============================================================================
 * Warm Start (Basis Save/Restore)
 * ============================================================================ */

RalphBasis* ralph_save_basis(const RalphModel *model) {
    if (!model || !model->lp_solver || !model->lp_solver->tableau) {
        return NULL;
    }

    SimplexTableau *tab = model->lp_solver->tableau;
    if (!tab->basis || !tab->var_status) {
        return NULL;
    }

    RalphBasis *basis = (RalphBasis*)calloc(1, sizeof(RalphBasis));
    if (!basis) return NULL;

    basis->m = tab->m;
    basis->n = tab->n;

    /* Copy basis array (use calloc for overflow-safe size calculation) */
    basis->basis = (int*)calloc(tab->m, sizeof(int));
    if (!basis->basis) {
        free(basis);
        return NULL;
    }
    memcpy(basis->basis, tab->basis, (size_t)tab->m * sizeof(int));

    /* Copy variable status array (use calloc for overflow-safe size calculation) */
    basis->var_status = (VarStatus*)calloc(tab->n, sizeof(VarStatus));
    if (!basis->var_status) {
        free(basis->basis);
        free(basis);
        return NULL;
    }
    memcpy(basis->var_status, tab->var_status, (size_t)tab->n * sizeof(VarStatus));

    return basis;
}

int ralph_load_basis(RalphModel *model, const RalphBasis *basis) {
    if (!model || !basis) return -1;
    if (!basis->basis || !basis->var_status) return -1;

    /* Load directly into an existing live simplex tableau when available. */
    if (model->lp_solver && model->lp_solver->tableau) {
        SimplexTableau *tab = model->lp_solver->tableau;

        /* Check dimension compatibility */
        if (tab->m != basis->m || tab->n != basis->n) {
            return -1;  /* Dimensions don't match */
        }

        /* Save backup before modification so we can restore on refactorize failure */
        int *orig_basis = (int*)calloc(tab->m, sizeof(int));
        VarStatus *orig_status = (VarStatus*)calloc(tab->n, sizeof(VarStatus));
        if (!orig_basis || !orig_status) {
            free(orig_basis);
            free(orig_status);
            return -1;
        }
        memcpy(orig_basis, tab->basis, (size_t)tab->m * sizeof(int));
        memcpy(orig_status, tab->var_status, (size_t)tab->n * sizeof(VarStatus));

        if (tableau_apply_warm_basis(tab, basis->m, basis->n, basis->basis, basis->var_status) != 0 ||
            tableau_refactorize(tab) != 0) {
            /* Restore original basis to avoid corrupted state */
            (void)tableau_apply_warm_basis(tab, tab->m, tab->n, orig_basis, orig_status);
            (void)tableau_refactorize(tab);
            free(orig_basis);
            free(orig_status);
            return -1;
        }

        free(orig_basis);
        free(orig_status);

        return 0;
    }

    /* No live solver yet: stage basis and apply on next optimize() call. */
    if (model->lp_model && basis->m != model->lp_model->num_cons) {
        return -1;
    }
    return ralph_stage_basis_copy(model, basis);
}

int ralph_get_basis_status(const RalphModel *model,
                           RalphBasisStatus *col_status,
                           RalphBasisStatus *row_status) {
    if (!model || !model->lp_model) return -1;
    if (!col_status && !row_status) return -1;
    if (!model->lp_solver || !model->lp_solver->tableau) return -1;

    const SimplexTableau *tab = model->lp_solver->tableau;
    if (!tab || !tab->basis || !tab->var_status) return -1;

    int num_vars = model->lp_model->num_vars;
    int num_cons = model->lp_model->num_cons;
    if (num_vars < 0 || num_cons < 0) return -1;
    if (tab->m != num_cons || num_vars > tab->n) return -1;

    if (col_status) {
        for (int j = 0; j < num_vars; j++) {
            if (ralph_basis_status_from_internal(tab->var_status[j], &col_status[j]) != 0) {
                return -1;
            }
        }
    }

    if (row_status && num_cons > 0) {
        int *row_aux = (int*)calloc((size_t)num_cons, sizeof(int));
        if (!row_aux) return -1;

        if (ralph_build_row_primary_aux_map(tab, row_aux) != 0) {
            free(row_aux);
            return -1;
        }

        for (int i = 0; i < num_cons; i++) {
            int row_var = row_aux[i];
            if (ralph_basis_status_from_internal(tab->var_status[row_var], &row_status[i]) != 0) {
                free(row_aux);
                return -1;
            }
        }
        free(row_aux);
    }

    return 0;
}

int ralph_set_basis_status(RalphModel *model,
                           const RalphBasisStatus *col_status,
                           const RalphBasisStatus *row_status) {
    if (!model || !model->lp_model) return -1;
    if (!col_status && !row_status) return -1;
    if (!model->lp_solver || !model->lp_solver->tableau) return -1;

    SimplexTableau *tab = model->lp_solver->tableau;
    if (!tab || !tab->basis || !tab->var_status) return -1;

    int num_vars = model->lp_model->num_vars;
    int num_cons = model->lp_model->num_cons;
    int n = tab->n;
    if (num_vars < 0 || num_cons < 0 || n < 0) return -1;
    if (tab->m != num_cons || num_vars > n) return -1;

    int *row_aux = NULL;
    if (row_status && num_cons > 0) {
        row_aux = (int*)calloc((size_t)num_cons, sizeof(int));
        if (!row_aux) return -1;
        if (ralph_build_row_primary_aux_map(tab, row_aux) != 0) {
            free(row_aux);
            return -1;
        }
    }

    int *basis_copy = NULL;
    VarStatus *status_copy = NULL;
    char *used = NULL;
    int ret = -1;

    if (num_cons > 0) {
        basis_copy = (int*)calloc((size_t)num_cons, sizeof(int));
        if (!basis_copy) goto cleanup;
    }
    if (n > 0) {
        status_copy = (VarStatus*)calloc((size_t)n, sizeof(VarStatus));
        if (!status_copy) goto cleanup;
        used = (char*)calloc((size_t)n, sizeof(char));
        if (!used) goto cleanup;
    }

    if (num_cons > 0) memcpy(basis_copy, tab->basis, (size_t)num_cons * sizeof(int));
    if (n > 0) memcpy(status_copy, tab->var_status, (size_t)n * sizeof(VarStatus));

    if (col_status) {
        for (int j = 0; j < num_vars; j++) {
            if (ralph_basis_status_to_internal(col_status[j], &status_copy[j]) != 0) {
                goto cleanup;
            }
        }
    }

    if (row_status) {
        for (int i = 0; i < num_cons; i++) {
            int row_var = row_aux[i];
            if (row_var < 0 || row_var >= n) goto cleanup;
            if (ralph_basis_status_to_internal(row_status[i], &status_copy[row_var]) != 0) {
                goto cleanup;
            }
        }
    }

    int basic_count = 0;
    for (int j = 0; j < n; j++) {
        if (status_copy[j] == RALPH_BASIC) basic_count++;
    }
    if (basic_count != num_cons) goto cleanup;

    for (int i = 0; i < num_cons; i++) {
        int old_var = tab->basis[i];
        if (old_var >= 0 && old_var < n &&
            status_copy[old_var] == RALPH_BASIC &&
            !used[old_var]) {
            basis_copy[i] = old_var;
            used[old_var] = 1;
        } else {
            basis_copy[i] = -1;
        }
    }

    int fill = 0;
    for (int j = 0; j < n; j++) {
        if (status_copy[j] != RALPH_BASIC || used[j]) continue;
        while (fill < num_cons && basis_copy[fill] >= 0) fill++;
        if (fill >= num_cons) goto cleanup;
        basis_copy[fill] = j;
        used[j] = 1;
    }

    for (int i = 0; i < num_cons; i++) {
        if (basis_copy[i] < 0) goto cleanup;
    }

    RalphBasis staged;
    memset(&staged, 0, sizeof(staged));
    staged.m = num_cons;
    staged.n = n;
    staged.basis = basis_copy;
    staged.var_status = status_copy;

    ret = ralph_load_basis(model, &staged);

cleanup:
    free(row_aux);
    free(basis_copy);
    free(status_copy);
    free(used);
    return ret;
}

void ralph_free_basis(RalphBasis *basis) {
    if (!basis) return;
    free(basis->basis);
    free(basis->var_status);
    free(basis);
}

int ralph_write_basis_file(const RalphBasis *basis, const char *filename) {
    if (!basis || !filename || !basis->basis || !basis->var_status) return -1;
    if (basis->m < 0 || basis->n < 0) return -1;

    FILE *fp = fopen(filename, "w");
    if (!fp) return -1;

    if (fprintf(fp, "RALPH_BASIS_V1 %d %d\n", basis->m, basis->n) < 0) {
        fclose(fp);
        return -1;
    }

    for (int i = 0; i < basis->m; i++) {
        if (fprintf(fp, "%d%c", basis->basis[i], (i + 1 == basis->m) ? '\n' : ' ') < 0) {
            fclose(fp);
            return -1;
        }
    }
    if (basis->m == 0 && fprintf(fp, "\n") < 0) {
        fclose(fp);
        return -1;
    }

    for (int j = 0; j < basis->n; j++) {
        if (fprintf(fp, "%d%c", (int)basis->var_status[j], (j + 1 == basis->n) ? '\n' : ' ') < 0) {
            fclose(fp);
            return -1;
        }
    }
    if (basis->n == 0 && fprintf(fp, "\n") < 0) {
        fclose(fp);
        return -1;
    }

    if (fclose(fp) != 0) return -1;
    return 0;
}

RalphBasis* ralph_read_basis_file(const char *filename) {
    if (!filename) return NULL;

    FILE *fp = fopen(filename, "r");
    if (!fp) return NULL;

    char magic[32] = {0};
    int m = 0, n = 0;
    if (fscanf(fp, "%31s %d %d", magic, &m, &n) != 3) {
        fclose(fp);
        return NULL;
    }
    if (strcmp(magic, "RALPH_BASIS_V1") != 0 || m < 0 || n < 0) {
        fclose(fp);
        return NULL;
    }

    RalphBasis *basis = (RalphBasis*)calloc(1, sizeof(RalphBasis));
    if (!basis) {
        fclose(fp);
        return NULL;
    }
    basis->m = m;
    basis->n = n;

    if (m > 0) {
        basis->basis = (int*)calloc((size_t)m, sizeof(int));
        if (!basis->basis) {
            ralph_free_basis(basis);
            fclose(fp);
            return NULL;
        }
    }
    if (n > 0) {
        basis->var_status = (VarStatus*)calloc((size_t)n, sizeof(VarStatus));
        if (!basis->var_status) {
            ralph_free_basis(basis);
            fclose(fp);
            return NULL;
        }
    }

    for (int i = 0; i < m; i++) {
        if (fscanf(fp, "%d", &basis->basis[i]) != 1) {
            ralph_free_basis(basis);
            fclose(fp);
            return NULL;
        }
    }
    for (int j = 0; j < n; j++) {
        int v = 0;
        if (fscanf(fp, "%d", &v) != 1) {
            ralph_free_basis(basis);
            fclose(fp);
            return NULL;
        }
        if (v < (int)RALPH_BASIC || v > (int)RALPH_FIXED) {
            ralph_free_basis(basis);
            fclose(fp);
            return NULL;
        }
        basis->var_status[j] = (VarStatus)v;
    }

    fclose(fp);
    return basis;
}

int ralph_write_mip_start_file(const RalphModel *model, const char *filename) {
    if (!model || !model->lp_model || !filename) return -1;

    int n = model->lp_model->num_vars;
    if (n <= 0) return -1;

    const double *start = NULL;
    const int *mask = NULL;
    int nnz = 0;

    if (model->mip_start && model->mip_start_n == n) {
        start = model->mip_start;
        mask = model->mip_start_mask;
        nnz = model->mip_start_nnz;
    } else if (model->solution && ralph_is_mip(model)) {
        start = model->solution;
    }

    if (!start) return -1;

    FILE *fp = fopen(filename, "w");
    if (!fp) return -1;

    if (fprintf(fp, "RALPH_MIPSTART_V1 %d %d %d\n", n,
                (int)model->mip_start_repair_mode, nnz) < 0) {
        fclose(fp);
        return -1;
    }

    for (int j = 0; j < n; j++) {
        if (fprintf(fp, "%.17g%c", start[j], (j + 1 == n) ? '\n' : ' ') < 0) {
            fclose(fp);
            return -1;
        }
    }

    for (int j = 0; j < n; j++) {
        int bit = mask ? (mask[j] ? 1 : 0) : 1;
        if (fprintf(fp, "%d%c", bit, (j + 1 == n) ? '\n' : ' ') < 0) {
            fclose(fp);
            return -1;
        }
    }

    if (fclose(fp) != 0) return -1;
    return 0;
}

int ralph_read_mip_start_file(RalphModel *model, const char *filename) {
    if (!model || !model->lp_model || !filename) return -1;

    FILE *fp = fopen(filename, "r");
    if (!fp) return -1;

    char magic[32] = {0};
    int n = 0;
    int repair = 0;
    int nnz = 0;
    if (fscanf(fp, "%31s %d %d %d", magic, &n, &repair, &nnz) != 4) {
        fclose(fp);
        return -1;
    }
    if (strcmp(magic, "RALPH_MIPSTART_V1") != 0 || n <= 0 ||
        n != model->lp_model->num_vars) {
        fclose(fp);
        return -1;
    }

    double *start = (double*)malloc((size_t)n * sizeof(double));
    int *mask = (int*)calloc((size_t)n, sizeof(int));
    if (!start || !mask) {
        free(start);
        free(mask);
        fclose(fp);
        return -1;
    }

    for (int j = 0; j < n; j++) {
        if (fscanf(fp, "%lf", &start[j]) != 1) {
            free(start);
            free(mask);
            fclose(fp);
            return -1;
        }
    }
    int counted = 0;
    for (int j = 0; j < n; j++) {
        int bit = 0;
        if (fscanf(fp, "%d", &bit) != 1) {
            free(start);
            free(mask);
            fclose(fp);
            return -1;
        }
        mask[j] = bit ? 1 : 0;
        counted += mask[j];
    }

    fclose(fp);

    if (ralph_set_mip_start_copy(model, start, mask, n) != 0) {
        free(start);
        free(mask);
        return -1;
    }
    model->mip_start_nnz = counted;

    if (repair >= (int)RALPH_MIP_START_REPAIR_STRICT &&
        repair <= (int)RALPH_MIP_START_REPAIR_PROJECT_AND_ROUND) {
        model->mip_start_repair_mode = (RalphMIPStartRepairMode)repair;
    } else {
        model->mip_start_repair_mode = RALPH_MIP_START_REPAIR_STRICT;
    }

    free(start);
    free(mask);
    return 0;
}

/* ============================================================================
 * Cut Callback
 * ============================================================================ */

void ralph_set_cut_callback(RalphModel *model, const RalphCutCallback *callback) {
    if (!model) return;

    if (callback) {
        model->cut_callback = *callback;
        model->has_cut_callback = 1;
    } else {
        memset(&model->cut_callback, 0, sizeof(RalphCutCallback));
        model->has_cut_callback = 0;
    }
}

/* ============================================================================
 * Branching Callback
 * ============================================================================ */

void ralph_set_branch_callback(RalphModel *model, const RalphBranchCallback *callback) {
    if (!model) return;

    if (callback) {
        model->branch_callback = *callback;
        model->has_branch_callback = 1;
    } else {
        memset(&model->branch_callback, 0, sizeof(RalphBranchCallback));
        model->has_branch_callback = 0;
    }
}

void ralph_set_lp_progress_callback(RalphModel *model,
                                    const RalphLPProgressCallback *callback) {
    if (!model) return;

    if (callback) {
        model->lp_progress_callback = *callback;
        model->has_lp_progress_callback = (callback->on_progress != NULL) ? 1 : 0;
    } else {
        memset(&model->lp_progress_callback, 0, sizeof(RalphLPProgressCallback));
        model->has_lp_progress_callback = 0;
    }
}

void ralph_set_lp_cancel_callback(RalphModel *model,
                                  const RalphLPCancelCallback *callback) {
    if (!model) return;

    if (callback) {
        model->lp_cancel_callback = *callback;
        model->has_lp_cancel_callback = (callback->should_cancel != NULL) ? 1 : 0;
    } else {
        memset(&model->lp_cancel_callback, 0, sizeof(RalphLPCancelCallback));
        model->has_lp_cancel_callback = 0;
    }
}

/* ============================================================================
 * Benders Decomposition
 * ============================================================================ */

int ralph_solve_benders(
    RalphModel *model,
    const RalphBendersConfig *config,
    double *x,
    RalphBendersResult *result)
{
    if (!model || !config) return -1;
    if (!model->lp_model) return -1;

    /* Finalize model if needed (builds sparse matrix A) */
    if (!model->lp_model->A) {
        if (lp_model_finalize(model->lp_model) != 0) {
            return -1;
        }
    }

    /* Delegate to internal Benders solver */
    return benders_solve(model->lp_model, config, x, result);
}

/* ============================================================================
 * Parameters
 * ============================================================================ */

typedef struct {
    RalphParamId id;
    const char *name;
    RalphParamScope scope;
    RalphParamValueType value_type;
    double default_value;
    int has_min;
    double min_value;
    int has_max;
    double max_value;
    const char *aliases[4];
    int alias_count;
} RalphParamSpec;

static int ralph_param_name_eq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

static const RalphParamSpec* ralph_param_specs(void) {
    static const RalphParamSpec specs[RALPH_PARAM_COUNT] = {
        [RALPH_PARAM_MAX_ITERATIONS] = {
            .id = RALPH_PARAM_MAX_ITERATIONS,
            .name = "max_iterations",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)RALPH_DEFAULT_MAX_ITER,
            .aliases = {"IterationLimit"},
            .alias_count = 1
        },
        [RALPH_PARAM_PRESOLVE] = {
            .id = RALPH_PARAM_PRESOLVE,
            .name = "presolve",
            .scope = RALPH_PARAM_SCOPE_SHARED,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 1.0,
            .aliases = {"Presolve"},
            .alias_count = 1
        },
        [RALPH_PARAM_VERBOSE] = {
            .id = RALPH_PARAM_VERBOSE,
            .name = "verbose",
            .scope = RALPH_PARAM_SCOPE_SHARED,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .aliases = {"OutputFlag"},
            .alias_count = 1
        },
        [RALPH_PARAM_TELEMETRY] = {
            .id = RALPH_PARAM_TELEMETRY,
            .name = "telemetry",
            .scope = RALPH_PARAM_SCOPE_SHARED,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 1.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 1.0,
            .aliases = {"Telemetry"},
            .alias_count = 1
        },
        [RALPH_PARAM_MAX_NODES] = {
            .id = RALPH_PARAM_MAX_NODES,
            .name = "max_nodes",
            .scope = RALPH_PARAM_SCOPE_MIP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)RALPH_DEFAULT_NODE_LIMIT,
            .aliases = {"NodeLimit"},
            .alias_count = 1
        },
        [RALPH_PARAM_MAX_CUT_ROUNDS] = {
            .id = RALPH_PARAM_MAX_CUT_ROUNDS,
            .name = "max_cut_rounds",
            .scope = RALPH_PARAM_SCOPE_MIP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .aliases = {"CutRounds"},
            .alias_count = 1
        },
        [RALPH_PARAM_METHOD] = {
            .id = RALPH_PARAM_METHOD,
            .name = "method",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 2.0,
            .aliases = {"Method"},
            .alias_count = 1
        },
        [RALPH_PARAM_PRICING] = {
            .id = RALPH_PARAM_PRICING,
            .name = "pricing",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 2.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 5.0,
            .aliases = {"Pricing"},
            .alias_count = 1
        },
        [RALPH_PARAM_DETECT_SPECIAL] = {
            .id = RALPH_PARAM_DETECT_SPECIAL,
            .name = "detect_special",
            .scope = RALPH_PARAM_SCOPE_SHARED,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 1.0,
            .aliases = {"DetectSpecial"},
            .alias_count = 1
        },
        [RALPH_PARAM_NODE_POOL_CAPACITY] = {
            .id = RALPH_PARAM_NODE_POOL_CAPACITY,
            .name = "node_pool_capacity",
            .scope = RALPH_PARAM_SCOPE_MIP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 1024.0,
            .has_min = 1,
            .min_value = 1.0,
            .aliases = {"PoolCapacity"},
            .alias_count = 1
        },
        [RALPH_PARAM_NODE_SELECT] = {
            .id = RALPH_PARAM_NODE_SELECT,
            .name = "node_select",
            .scope = RALPH_PARAM_SCOPE_MIP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 3.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 3.0,
            .aliases = {"NodeSelect"},
            .alias_count = 1
        },
        [RALPH_PARAM_FORCE_TWO_PHASE] = {
            .id = RALPH_PARAM_FORCE_TWO_PHASE,
            .name = "force_two_phase",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 1.0,
            .aliases = {"TwoPhase"},
            .alias_count = 1
        },
        [RALPH_PARAM_TRACE_PHASE1] = {
            .id = RALPH_PARAM_TRACE_PHASE1,
            .name = "trace_phase1",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 1.0,
            .aliases = {"TracePhase1"},
            .alias_count = 1
        },
        [RALPH_PARAM_PRESOLVE_MASK] = {
            .id = RALPH_PARAM_PRESOLVE_MASK,
            .name = "presolve_mask",
            .scope = RALPH_PARAM_SCOPE_SHARED,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = (double)PRESOLVE_SAFE,
            .has_min = 1,
            .min_value = 0.0,
            .aliases = {"PresolveMask"},
            .alias_count = 1
        },
        [RALPH_PARAM_DUAL_BOUND_FLIP] = {
            .id = RALPH_PARAM_DUAL_BOUND_FLIP,
            .name = "dual_bound_flip",
            .scope = RALPH_PARAM_SCOPE_SHARED,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = -1.0,
            .has_min = 1,
            .min_value = -1.0,
            .has_max = 1,
            .max_value = 1.0,
            .aliases = {"DualBoundFlip"},
            .alias_count = 1
        },
        [RALPH_PARAM_DUAL_STEEPEST_EDGE] = {
            .id = RALPH_PARAM_DUAL_STEEPEST_EDGE,
            .name = "dual_steepest_edge",
            .scope = RALPH_PARAM_SCOPE_SHARED,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = -1.0,
            .has_min = 1,
            .min_value = -1.0,
            .has_max = 1,
            .max_value = 1.0,
            .aliases = {"DualSteepestEdge"},
            .alias_count = 1
        },
        [RALPH_PARAM_SCALING] = {
            .id = RALPH_PARAM_SCALING,
            .name = "scaling",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 1.0,
            .has_min = 1,
            .min_value = 0.0,
            .aliases = {"Scaling", "scaling_rounds", "ScalingRounds"},
            .alias_count = 3
        },
        [RALPH_PARAM_CRASH] = {
            .id = RALPH_PARAM_CRASH,
            .name = "crash",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 1.0,
            .aliases = {"Crash"},
            .alias_count = 1
        },
        [RALPH_PARAM_VERIFY] = {
            .id = RALPH_PARAM_VERIFY,
            .name = "verify",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 1.0,
            .aliases = {"Verify"},
            .alias_count = 1
        },
        [RALPH_PARAM_PHASE1_PRICING] = {
            .id = RALPH_PARAM_PHASE1_PRICING,
            .name = "phase1_pricing",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = -1.0,
            .has_min = 1,
            .min_value = -1.0,
            .aliases = {"Phase1Pricing"},
            .alias_count = 1
        },
        [RALPH_PARAM_VAR_SELECT] = {
            .id = RALPH_PARAM_VAR_SELECT,
            .name = "var_select",
            .scope = RALPH_PARAM_SCOPE_MIP,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = -1.0,
            .has_min = 1,
            .min_value = -1.0,
            .has_max = 1,
            .max_value = 4.0,
            .aliases = {"VarSelect"},
            .alias_count = 1
        },
        [RALPH_PARAM_LU_SUPERNODE] = {
            .id = RALPH_PARAM_LU_SUPERNODE,
            .name = "lu_supernode",
            .scope = RALPH_PARAM_SCOPE_SHARED,
            .value_type = RALPH_PARAM_VALUE_INT,
            .default_value = 0.0,
            .has_min = 1,
            .min_value = 0.0,
            .has_max = 1,
            .max_value = 1.0,
            .aliases = {"LuSupernode"},
            .alias_count = 1
        },
        [RALPH_PARAM_TIME_LIMIT] = {
            .id = RALPH_PARAM_TIME_LIMIT,
            .name = "time_limit",
            .scope = RALPH_PARAM_SCOPE_SHARED,
            .value_type = RALPH_PARAM_VALUE_DOUBLE,
            .default_value = RALPH_DEFAULT_TIME_LIMIT,
            .has_min = 1,
            .min_value = 0.0,
            .aliases = {"TimeLimit"},
            .alias_count = 1
        },
        [RALPH_PARAM_MIP_GAP] = {
            .id = RALPH_PARAM_MIP_GAP,
            .name = "mip_gap",
            .scope = RALPH_PARAM_SCOPE_MIP,
            .value_type = RALPH_PARAM_VALUE_DOUBLE,
            .default_value = RALPH_DEFAULT_MIP_GAP,
            .has_min = 1,
            .min_value = 0.0,
            .aliases = {"MIPGap"},
            .alias_count = 1
        },
        [RALPH_PARAM_OBJ_LIMIT] = {
            .id = RALPH_PARAM_OBJ_LIMIT,
            .name = "obj_limit",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_DOUBLE,
            .default_value = RALPH_INFINITY,
            .aliases = {"ObjLimit"},
            .alias_count = 1
        },
        [RALPH_PARAM_FEAS_TOL] = {
            .id = RALPH_PARAM_FEAS_TOL,
            .name = "feas_tol",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_DOUBLE,
            .default_value = RALPH_FEAS_TOL,
            .has_min = 1,
            .min_value = 0.0
        },
        [RALPH_PARAM_OPT_TOL] = {
            .id = RALPH_PARAM_OPT_TOL,
            .name = "opt_tol",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_DOUBLE,
            .default_value = RALPH_OPT_TOL,
            .has_min = 1,
            .min_value = 0.0
        },
        [RALPH_PARAM_PIVOT_TOL] = {
            .id = RALPH_PARAM_PIVOT_TOL,
            .name = "pivot_tol",
            .scope = RALPH_PARAM_SCOPE_LP,
            .value_type = RALPH_PARAM_VALUE_DOUBLE,
            .default_value = RALPH_PIVOT_TOL,
            .has_min = 1,
            .min_value = 0.0
        }
    };
    return specs;
}

static const RalphParamSpec* ralph_param_spec_by_id(RalphParamId param) {
    if (param < 0 || param >= RALPH_PARAM_COUNT) return NULL;
    return &ralph_param_specs()[param];
}

static int ralph_param_scope_allows_lp(RalphParamScope scope) {
    return scope == RALPH_PARAM_SCOPE_SHARED || scope == RALPH_PARAM_SCOPE_LP;
}

static int ralph_param_scope_allows_mip(RalphParamScope scope) {
    return scope == RALPH_PARAM_SCOPE_SHARED || scope == RALPH_PARAM_SCOPE_MIP;
}

int ralph_get_param_count(void) {
    return RALPH_PARAM_COUNT;
}

int ralph_get_param_meta(RalphParamId param, RalphParamMeta *meta) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (!spec || !meta) return -1;

    meta->id = spec->id;
    meta->name = spec->name;
    meta->scope = spec->scope;
    meta->value_type = spec->value_type;
    meta->default_value = spec->default_value;
    meta->has_min = spec->has_min;
    meta->min_value = spec->min_value;
    meta->has_max = spec->has_max;
    meta->max_value = spec->max_value;
    return 0;
}

int ralph_find_param_by_name(const char *name, RalphParamId *param) {
    if (!name || !param) return -1;

    const RalphParamSpec *specs = ralph_param_specs();
    for (int i = 0; i < RALPH_PARAM_COUNT; i++) {
        const RalphParamSpec *spec = &specs[i];
        if (ralph_param_name_eq(name, spec->name)) {
            *param = spec->id;
            return 0;
        }
        for (int k = 0; k < spec->alias_count; k++) {
            if (ralph_param_name_eq(name, spec->aliases[k])) {
                *param = spec->id;
                return 0;
            }
        }
    }
    return -1;
}

int ralph_set_int_param_id(RalphModel *model, RalphParamId param, int value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (!model || !spec || spec->value_type != RALPH_PARAM_VALUE_INT) return -1;

    switch (param) {
        case RALPH_PARAM_MAX_ITERATIONS:
            model->max_iterations = value;
            break;
        case RALPH_PARAM_PRESOLVE:
            model->presolve = value ? 1 : -1;  /* -1 = explicitly off */
            break;
        case RALPH_PARAM_VERBOSE:
            model->verbose = value;
            break;
        case RALPH_PARAM_TELEMETRY:
            model->telemetry = value ? 1 : 0;
            break;
        case RALPH_PARAM_MAX_NODES:
            model->max_nodes = value;
            break;
        case RALPH_PARAM_MAX_CUT_ROUNDS:
            model->max_cut_rounds = value;
            break;
        case RALPH_PARAM_METHOD:
            model->method = value;
            break;
        case RALPH_PARAM_PRICING:
            model->pricing = value;
            break;
        case RALPH_PARAM_DETECT_SPECIAL:
            model->detect_special = value;
            break;
        case RALPH_PARAM_NODE_POOL_CAPACITY:
            model->node_pool_capacity = (value > 0) ? value : 1024;
            break;
        case RALPH_PARAM_NODE_SELECT:
            if (value < 0 || value > 3) return -1;
            model->node_select = value;
            break;
        case RALPH_PARAM_FORCE_TWO_PHASE:
            model->force_two_phase = value;
            break;
        case RALPH_PARAM_TRACE_PHASE1:
            model->trace_phase1 = value;
            break;
        case RALPH_PARAM_PRESOLVE_MASK:
            model->presolve_mask = (unsigned int)value;
            break;
        case RALPH_PARAM_DUAL_BOUND_FLIP:
            model->dual_bound_flip = value ? 1 : 0;
            break;
        case RALPH_PARAM_DUAL_STEEPEST_EDGE:
            model->dual_steepest_edge = value ? 1 : 0;
            break;
        case RALPH_PARAM_SCALING:
            model->scaling = (value >= 0) ? value : 0;
            break;
        case RALPH_PARAM_CRASH:
            model->crash = value ? 1 : 0;
            break;
        case RALPH_PARAM_VERIFY:
            model->verify = value ? 1 : 0;
            break;
        case RALPH_PARAM_PHASE1_PRICING:
            model->phase1_pricing = (value >= 0) ? value : -1;
            break;
        case RALPH_PARAM_VAR_SELECT:
            if (value < 0 || value > 4) return -1;
            model->var_select = value;
            break;
        case RALPH_PARAM_LU_SUPERNODE:
            model->lu_supernode = value ? 1 : 0;
            break;
        default:
            return -1;
    }

    return 0;
}

int ralph_set_dbl_param_id(RalphModel *model, RalphParamId param, double value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (!model || !spec || spec->value_type != RALPH_PARAM_VALUE_DOUBLE) return -1;
    if (!model->lp_model) return -1;

    switch (param) {
        case RALPH_PARAM_TIME_LIMIT:
            model->time_limit = value;
            break;
        case RALPH_PARAM_MIP_GAP:
            model->mip_gap = value;
            break;
        case RALPH_PARAM_OBJ_LIMIT:
            model->objective_limit = value;
            break;
        case RALPH_PARAM_FEAS_TOL:
            if (value > 0.0) model->lp_model->feas_tol = value;
            break;
        case RALPH_PARAM_OPT_TOL:
            if (value > 0.0) model->lp_model->opt_tol = value;
            break;
        case RALPH_PARAM_PIVOT_TOL:
            if (value > 0.0) model->lp_model->pivot_tol = value;
            break;
        default:
            return -1;
    }

    return 0;
}

int ralph_get_int_param_id(const RalphModel *model, RalphParamId param, int *value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (!model || !value || !spec || spec->value_type != RALPH_PARAM_VALUE_INT) return -1;

    switch (param) {
        case RALPH_PARAM_MAX_ITERATIONS:
            *value = model->max_iterations;
            break;
        case RALPH_PARAM_PRESOLVE:
            *value = (model->presolve > 0) ? 1 : 0;
            break;
        case RALPH_PARAM_VERBOSE:
            *value = model->verbose;
            break;
        case RALPH_PARAM_TELEMETRY:
            *value = model->telemetry;
            break;
        case RALPH_PARAM_MAX_NODES:
            *value = model->max_nodes;
            break;
        case RALPH_PARAM_MAX_CUT_ROUNDS:
            *value = model->max_cut_rounds;
            break;
        case RALPH_PARAM_METHOD:
            *value = model->method;
            break;
        case RALPH_PARAM_PRICING:
            *value = model->pricing;
            break;
        case RALPH_PARAM_DETECT_SPECIAL:
            *value = model->detect_special;
            break;
        case RALPH_PARAM_NODE_POOL_CAPACITY:
            *value = model->node_pool_capacity;
            break;
        case RALPH_PARAM_NODE_SELECT:
            *value = model->node_select;
            break;
        case RALPH_PARAM_FORCE_TWO_PHASE:
            *value = model->force_two_phase;
            break;
        case RALPH_PARAM_TRACE_PHASE1:
            *value = model->trace_phase1;
            break;
        case RALPH_PARAM_PRESOLVE_MASK:
            *value = (int)model->presolve_mask;
            break;
        case RALPH_PARAM_DUAL_BOUND_FLIP:
            *value = model->dual_bound_flip;
            break;
        case RALPH_PARAM_DUAL_STEEPEST_EDGE:
            *value = model->dual_steepest_edge;
            break;
        case RALPH_PARAM_SCALING:
            *value = model->scaling;
            break;
        case RALPH_PARAM_CRASH:
            *value = model->crash;
            break;
        case RALPH_PARAM_VERIFY:
            *value = model->verify;
            break;
        case RALPH_PARAM_PHASE1_PRICING:
            *value = model->phase1_pricing;
            break;
        case RALPH_PARAM_VAR_SELECT:
            *value = model->var_select;
            break;
        case RALPH_PARAM_LU_SUPERNODE:
            *value = model->lu_supernode;
            break;
        default:
            return -1;
    }

    return 0;
}

int ralph_get_dbl_param_id(const RalphModel *model, RalphParamId param, double *value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (!model || !value || !spec || spec->value_type != RALPH_PARAM_VALUE_DOUBLE) return -1;
    if (!model->lp_model) return -1;

    switch (param) {
        case RALPH_PARAM_TIME_LIMIT:
            *value = model->time_limit;
            break;
        case RALPH_PARAM_MIP_GAP:
            *value = model->mip_gap;
            break;
        case RALPH_PARAM_OBJ_LIMIT:
            *value = model->objective_limit;
            break;
        case RALPH_PARAM_FEAS_TOL:
            *value = model->lp_model->feas_tol;
            break;
        case RALPH_PARAM_OPT_TOL:
            *value = model->lp_model->opt_tol;
            break;
        case RALPH_PARAM_PIVOT_TOL:
            *value = model->lp_model->pivot_tol;
            break;
        default:
            return -1;
    }

    return 0;
}

int ralph_set_lp_int_param_id(RalphModel *model, RalphParamId param, int value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (!spec || spec->value_type != RALPH_PARAM_VALUE_INT) return -1;
    if (!ralph_param_scope_allows_lp(spec->scope)) return -1;
    return ralph_set_int_param_id(model, param, value);
}

int ralph_set_lp_dbl_param_id(RalphModel *model, RalphParamId param, double value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (!spec || spec->value_type != RALPH_PARAM_VALUE_DOUBLE) return -1;
    if (!ralph_param_scope_allows_lp(spec->scope)) return -1;
    return ralph_set_dbl_param_id(model, param, value);
}

int ralph_get_lp_int_param_id(const RalphModel *model, RalphParamId param, int *value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (!spec || spec->value_type != RALPH_PARAM_VALUE_INT) return -1;
    if (!ralph_param_scope_allows_lp(spec->scope)) return -1;
    return ralph_get_int_param_id(model, param, value);
}

int ralph_get_lp_dbl_param_id(const RalphModel *model, RalphParamId param, double *value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (!spec || spec->value_type != RALPH_PARAM_VALUE_DOUBLE) return -1;
    if (!ralph_param_scope_allows_lp(spec->scope)) return -1;
    return ralph_get_dbl_param_id(model, param, value);
}

int ralph_set_mip_int_param_id(RalphModel *model, RalphParamId param, int value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (!spec || spec->value_type != RALPH_PARAM_VALUE_INT) return -1;
    if (!ralph_param_scope_allows_mip(spec->scope)) return -1;
    return ralph_set_int_param_id(model, param, value);
}

int ralph_set_mip_dbl_param_id(RalphModel *model, RalphParamId param, double value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (!spec || spec->value_type != RALPH_PARAM_VALUE_DOUBLE) return -1;
    if (!ralph_param_scope_allows_mip(spec->scope)) return -1;
    return ralph_set_dbl_param_id(model, param, value);
}

int ralph_get_mip_int_param_id(const RalphModel *model, RalphParamId param, int *value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (!spec || spec->value_type != RALPH_PARAM_VALUE_INT) return -1;
    if (!ralph_param_scope_allows_mip(spec->scope)) return -1;
    return ralph_get_int_param_id(model, param, value);
}

int ralph_get_mip_dbl_param_id(const RalphModel *model, RalphParamId param, double *value) {
    const RalphParamSpec *spec = ralph_param_spec_by_id(param);
    if (!spec || spec->value_type != RALPH_PARAM_VALUE_DOUBLE) return -1;
    if (!ralph_param_scope_allows_mip(spec->scope)) return -1;
    return ralph_get_dbl_param_id(model, param, value);
}

int ralph_set_int_param(RalphModel *model, const char *name, int value) {
    RalphParamId param;
    if (!model || !name) return -1;
    if (ralph_find_param_by_name(name, &param) != 0) return -1;
    return ralph_set_int_param_id(model, param, value);
}

int ralph_set_dbl_param(RalphModel *model, const char *name, double value) {
    RalphParamId param;
    if (!model || !name) return -1;
    if (ralph_find_param_by_name(name, &param) != 0) return -1;
    return ralph_set_dbl_param_id(model, param, value);
}

int ralph_get_int_param(const RalphModel *model, const char *name, int *value) {
    RalphParamId param;
    if (!model || !name || !value) return -1;
    if (ralph_find_param_by_name(name, &param) != 0) return -1;
    return ralph_get_int_param_id(model, param, value);
}

int ralph_get_dbl_param(const RalphModel *model, const char *name, double *value) {
    RalphParamId param;
    if (!model || !name || !value) return -1;
    if (ralph_find_param_by_name(name, &param) != 0) return -1;
    return ralph_get_dbl_param_id(model, param, value);
}

int ralph_set_lp_int_param(RalphModel *model, const char *name, int value) {
    RalphParamId param;
    if (!name) return -1;
    if (ralph_find_param_by_name(name, &param) != 0) return -1;
    return ralph_set_lp_int_param_id(model, param, value);
}

int ralph_set_lp_dbl_param(RalphModel *model, const char *name, double value) {
    RalphParamId param;
    if (!name) return -1;
    if (ralph_find_param_by_name(name, &param) != 0) return -1;
    return ralph_set_lp_dbl_param_id(model, param, value);
}

int ralph_get_lp_int_param(const RalphModel *model, const char *name, int *value) {
    RalphParamId param;
    if (!name) return -1;
    if (ralph_find_param_by_name(name, &param) != 0) return -1;
    return ralph_get_lp_int_param_id(model, param, value);
}

int ralph_get_lp_dbl_param(const RalphModel *model, const char *name, double *value) {
    RalphParamId param;
    if (!name) return -1;
    if (ralph_find_param_by_name(name, &param) != 0) return -1;
    return ralph_get_lp_dbl_param_id(model, param, value);
}

int ralph_set_mip_int_param(RalphModel *model, const char *name, int value) {
    RalphParamId param;
    if (!name) return -1;
    if (ralph_find_param_by_name(name, &param) != 0) return -1;
    return ralph_set_mip_int_param_id(model, param, value);
}

int ralph_set_mip_dbl_param(RalphModel *model, const char *name, double value) {
    RalphParamId param;
    if (!name) return -1;
    if (ralph_find_param_by_name(name, &param) != 0) return -1;
    return ralph_set_mip_dbl_param_id(model, param, value);
}

int ralph_get_mip_int_param(const RalphModel *model, const char *name, int *value) {
    RalphParamId param;
    if (!name) return -1;
    if (ralph_find_param_by_name(name, &param) != 0) return -1;
    return ralph_get_mip_int_param_id(model, param, value);
}

int ralph_get_mip_dbl_param(const RalphModel *model, const char *name, double *value) {
    RalphParamId param;
    if (!name) return -1;
    if (ralph_find_param_by_name(name, &param) != 0) return -1;
    return ralph_get_mip_dbl_param_id(model, param, value);
}

/* ============================================================================
 * Utility
 * ============================================================================ */

const char* ralph_status_string(RalphStatus status) {
    switch (status) {
        case RALPH_STATUS_UNKNOWN:        return "UNKNOWN";
        case RALPH_STATUS_OPTIMAL:        return "OPTIMAL";
        case RALPH_STATUS_INFEASIBLE:     return "INFEASIBLE";
        case RALPH_STATUS_UNBOUNDED:      return "UNBOUNDED";
        case RALPH_STATUS_INF_OR_UNBD:    return "INF_OR_UNBOUNDED";
        case RALPH_STATUS_ITERATION_LIMIT: return "ITERATION_LIMIT";
        case RALPH_STATUS_TIME_LIMIT:     return "TIME_LIMIT";
        case RALPH_STATUS_NODE_LIMIT:     return "NODE_LIMIT";
        case RALPH_STATUS_IMPRECISE:     return "IMPRECISE";
        case RALPH_STATUS_OBJ_LIMIT:    return "OBJ_LIMIT";
        case RALPH_STATUS_ERROR:          return "ERROR";
        default:                          return "UNKNOWN";
    }
}

const char* ralph_version(void) {
    return RALPH_VERSION;
}

/* ============================================================================
 * Name Management
 * ============================================================================ */

const char* ralph_get_var_name(const RalphModel *model, int var) {
    if (!model || !model->lp_model) return NULL;
    return lp_model_get_var_name(model->lp_model, var);
}

const char* ralph_get_con_name(const RalphModel *model, int con) {
    if (!model || !model->lp_model) return NULL;
    return lp_model_get_con_name(model->lp_model, con);
}

int ralph_set_var_name(RalphModel *model, int var, const char *name) {
    if (!model || !model->lp_model) return -1;
    return lp_model_set_var_name(model->lp_model, var, name);
}

int ralph_set_con_name(RalphModel *model, int con, const char *name) {
    if (!model || !model->lp_model) return -1;
    return lp_model_set_con_name(model->lp_model, con, name);
}

const char* ralph_get_problem_name(const RalphModel *model) {
    if (!model || !model->lp_model) return NULL;
    return lp_model_get_name(model->lp_model);
}

int ralph_set_problem_name(RalphModel *model, const char *name) {
    if (!model || !model->lp_model) return -1;
    return lp_model_set_name(model->lp_model, name);
}

/* Internal helper for LP writer - provides access to LPModel */
LPModel* ralph_get_lp_model(const RalphModel *model) {
    return model ? model->lp_model : NULL;
}

/* Internal helper for benchmark diagnostics */
SimplexSolver* ralph_get_lp_solver(const RalphModel *model) {
    return model ? model->lp_solver : NULL;
}

/* Internal helper for integration tests and benchmark diagnostics */
MIPSolver* ralph_get_mip_solver(const RalphModel *model) {
    return model ? model->mip_solver : NULL;
}
