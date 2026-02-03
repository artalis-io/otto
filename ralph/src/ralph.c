/*
 * Ralph - Main Public API Implementation
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "ralph.h"
#include "lp.h"
#include "mip.h"
#include "presolve.h"
#include "detect.h"

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
    double mip_gap;
    int max_nodes;
    int max_cut_rounds;
    int method;  /* 0=primal simplex, 1=dual simplex, 2=auto */
    int pricing; /* 0=Dantzig, 1=Steepest edge, 2=Devex (default), 3=Partial */
    int detect_special; /* 1=detect LAP/network structure, 0=disable */

    /* Solution */
    RalphStatus status;
    double obj_value;
    double *solution;
    double *dual_solution;
    double *reduced_costs;

    /* MIP-specific */
    double best_bound;
    int node_count;

    /* Statistics */
    int iteration_count;
};

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
    model->verbose = 0;
    model->mip_gap = RALPH_DEFAULT_MIP_GAP;
    model->max_nodes = RALPH_DEFAULT_NODE_LIMIT;
    model->max_cut_rounds = 0;  /* Disabled by default */
    model->method = 0;  /* Default: primal simplex */
    model->pricing = 2; /* Default: Devex */
    model->detect_special = 0; /* Default: disabled for fair benchmarking */

    model->status = RALPH_STATUS_UNKNOWN;

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
    free(model);
}

/* ============================================================================
 * Model Building
 * ============================================================================ */

int ralph_set_obj_sense(RalphModel *model, RalphObjSense sense) {
    if (!model || !model->lp_model) return -1;
    model->lp_model->obj_sense = (int)sense;
    return 0;
}

int ralph_add_var(RalphModel *model, double lb, double ub, double obj, RalphVarType type) {
    if (!model || !model->lp_model) return -1;
    return lp_model_add_var(model->lp_model, lb, ub, obj, (char)type);
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

    return 0;
}

int ralph_add_constraint(RalphModel *model, int nnz, const int *indices,
                         const double *values, RalphSense sense, double rhs) {
    if (!model || !model->lp_model) return -1;
    return lp_model_add_constraint(model->lp_model, nnz, indices, values, (char)sense, rhs);
}

/* ============================================================================
 * Model Modification
 * ============================================================================ */

int ralph_set_var_bounds(RalphModel *model, int var, double lb, double ub) {
    if (!model || !model->lp_model) return -1;
    if (var < 0 || var >= model->lp_model->num_vars) return -1;

    model->lp_model->lb[var] = lb;
    model->lp_model->ub[var] = ub;
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

    return 0;
}

int ralph_set_obj_coef(RalphModel *model, int var, double coef) {
    if (!model || !model->lp_model) return -1;
    if (var < 0 || var >= model->lp_model->num_vars) return -1;

    model->lp_model->c[var] = coef;
    return 0;
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

int ralph_optimize(RalphModel *model) {
    if (!model || !model->lp_model) return -1;

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
    model->solution = NULL;
    model->dual_solution = NULL;
    model->reduced_costs = NULL;

    int n_orig = model->lp_model->num_vars;
    int m_orig = model->lp_model->num_cons;

    /* Apply presolve if enabled */
    PresolveResult *presolved = NULL;
    LPModel *solve_model = model->lp_model;

    if (model->presolve) {
        presolved = presolve(model->lp_model);
        if (presolved && presolved->reduced_model) {
            solve_model = presolved->reduced_model;
            if (model->verbose) {
                printf("Presolve: %d vars removed, %d cons removed, %d bounds tightened\n",
                       presolved->vars_removed, presolved->cons_removed,
                       presolved->bounds_tightened);
            }
        }
    }

    /* Try LAP detection for pure LP (not MIP) */
    if (!ralph_is_mip(model) && model->detect_special && ralph_get_detect_lap()) {
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

    /* Try network detection for pure LP (not MIP) */
    if (!ralph_is_mip(model) && model->detect_special && ralph_get_detect_network()) {
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

    if (ralph_is_mip(model)) {
        /* MIP solve - use presolved model if available */
        model->mip_solver = mip_create(solve_model, model->detect_special);
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
        model->mip_solver->max_cut_rounds = model->max_cut_rounds;

        /* Solve */
        mip_solve(model->mip_solver);

        model->status = model->mip_solver->status;

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
        model->lp_solver->presolve = 0;  /* Already done */
        model->lp_solver->pricing_strategy = model->pricing;

        /* Solve using selected method */
        if (model->method == 1) {
            /* Dual simplex - true dual phase 1 */
            dual_simplex_solve_from_scratch(model->lp_solver);
        } else if (model->method == 2) {
            /* Auto: use dual for all-<= constraints, primal otherwise */
            int use_dual = 1;
            for (int i = 0; i < solve_model->num_cons; i++) {
                if (solve_model->sense[i] != 'L') {
                    use_dual = 0;
                    break;
                }
            }
            if (use_dual) {
                dual_simplex_solve_from_scratch(model->lp_solver);
            } else {
                simplex_solve(model->lp_solver);
            }
        } else {
            /* Default: primal simplex */
            simplex_solve(model->lp_solver);
        }

        model->status = model->lp_solver->status;
        model->iteration_count = model->lp_solver->iterations;

        if (model->status == RALPH_STATUS_OPTIMAL) {
            model->obj_value = model->lp_solver->obj_value;

            /* Allocate solution arrays for original problem size */
            model->solution = (double*)calloc(n_orig, sizeof(double));
            model->dual_solution = (double*)calloc(m_orig, sizeof(double));
            model->reduced_costs = (double*)calloc(n_orig, sizeof(double));

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
        }
    }

    /* Free presolve result */
    if (presolved) {
        presolve_free(presolved);
    }

    return 0;
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

/* ============================================================================
 * Parameters
 * ============================================================================ */

int ralph_set_int_param(RalphModel *model, const char *name, int value) {
    if (!model || !name) return -1;

    if (strcmp(name, "max_iterations") == 0 || strcmp(name, "IterationLimit") == 0) {
        model->max_iterations = value;
    } else if (strcmp(name, "presolve") == 0 || strcmp(name, "Presolve") == 0) {
        model->presolve = value;
    } else if (strcmp(name, "verbose") == 0 || strcmp(name, "OutputFlag") == 0) {
        model->verbose = value;
    } else if (strcmp(name, "max_nodes") == 0 || strcmp(name, "NodeLimit") == 0) {
        model->max_nodes = value;
    } else if (strcmp(name, "max_cut_rounds") == 0 || strcmp(name, "CutRounds") == 0) {
        model->max_cut_rounds = value;
    } else if (strcmp(name, "method") == 0 || strcmp(name, "Method") == 0) {
        /* 0=primal simplex, 1=dual simplex, 2=auto */
        model->method = value;
    } else if (strcmp(name, "pricing") == 0 || strcmp(name, "Pricing") == 0) {
        /* 0=Dantzig, 1=Steepest edge, 2=Devex, 3=Partial */
        model->pricing = value;
    } else if (strcmp(name, "detect_special") == 0 || strcmp(name, "DetectSpecial") == 0) {
        /* 1=detect LAP/network structure, 0=disable */
        model->detect_special = value;
    } else {
        return -1;  /* Unknown parameter */
    }

    return 0;
}

int ralph_set_dbl_param(RalphModel *model, const char *name, double value) {
    if (!model || !name) return -1;

    if (strcmp(name, "time_limit") == 0 || strcmp(name, "TimeLimit") == 0) {
        model->time_limit = value;
    } else if (strcmp(name, "mip_gap") == 0 || strcmp(name, "MIPGap") == 0) {
        model->mip_gap = value;
    } else {
        return -1;  /* Unknown parameter */
    }

    return 0;
}

int ralph_get_int_param(const RalphModel *model, const char *name, int *value) {
    if (!model || !name || !value) return -1;

    if (strcmp(name, "max_iterations") == 0) {
        *value = model->max_iterations;
    } else if (strcmp(name, "presolve") == 0) {
        *value = model->presolve;
    } else if (strcmp(name, "verbose") == 0) {
        *value = model->verbose;
    } else if (strcmp(name, "max_nodes") == 0) {
        *value = model->max_nodes;
    } else if (strcmp(name, "max_cut_rounds") == 0) {
        *value = model->max_cut_rounds;
    } else if (strcmp(name, "method") == 0) {
        *value = model->method;
    } else {
        return -1;
    }

    return 0;
}

int ralph_get_dbl_param(const RalphModel *model, const char *name, double *value) {
    if (!model || !name || !value) return -1;

    if (strcmp(name, "time_limit") == 0) {
        *value = model->time_limit;
    } else if (strcmp(name, "mip_gap") == 0) {
        *value = model->mip_gap;
    } else {
        return -1;
    }

    return 0;
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
        case RALPH_STATUS_ERROR:          return "ERROR";
        default:                          return "UNKNOWN";
    }
}

const char* ralph_version(void) {
    return RALPH_VERSION;
}
