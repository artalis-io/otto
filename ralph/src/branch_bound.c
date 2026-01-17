/*
 * Ralph - Branch and Bound Implementation
 *
 * Implements a Branch and Bound framework for MIP solving with:
 * - Multiple node selection strategies (best-first, depth-first, hybrid)
 * - Multiple variable selection strategies (most infeasible, pseudo-cost, strong branching)
 * - Node pruning and warm starting
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include "mip.h"

/* ============================================================================
 * Node Priority Queue
 * ============================================================================ */

NodeQueue* node_queue_create(int capacity, NodeSelectStrategy strategy, int obj_sense) {
    NodeQueue *queue = (NodeQueue*)calloc(1, sizeof(NodeQueue));
    if (!queue) return NULL;

    queue->capacity = capacity > 0 ? capacity : 1024;
    queue->size = 0;
    queue->strategy = strategy;
    queue->obj_sense = obj_sense;

    queue->nodes = (BBNode**)malloc(queue->capacity * sizeof(BBNode*));
    if (!queue->nodes) {
        free(queue);
        return NULL;
    }

    return queue;
}

void node_queue_free(NodeQueue *queue) {
    if (!queue) return;

    for (int i = 0; i < queue->size; i++) {
        bb_node_free(queue->nodes[i]);
    }
    free(queue->nodes);
    free(queue);
}

/* Compare nodes based on strategy */
static int node_compare(const BBNode *a, const BBNode *b, NodeSelectStrategy strategy, int obj_sense) {
    switch (strategy) {
        case NODE_SELECT_BEST_FIRST:
            /* Lower bound is better for minimization */
            if (obj_sense == 1) {
                return (a->lp_bound < b->lp_bound) ? -1 : 1;
            } else {
                return (a->lp_bound > b->lp_bound) ? -1 : 1;
            }

        case NODE_SELECT_DEPTH_FIRST:
            /* Deeper nodes first */
            return (a->depth > b->depth) ? -1 : 1;

        case NODE_SELECT_BEST_ESTIMATE:
            /* Use estimate based on pseudo-costs */
            if (obj_sense == 1) {
                return (a->estimate < b->estimate) ? -1 : 1;
            } else {
                return (a->estimate > b->estimate) ? -1 : 1;
            }

        case NODE_SELECT_HYBRID:
            /* Depth-first until first solution, then best-first */
            /* Handled in pop by checking has_incumbent */
            if (a->depth != b->depth) {
                return (a->depth > b->depth) ? -1 : 1;
            }
            if (obj_sense == 1) {
                return (a->lp_bound < b->lp_bound) ? -1 : 1;
            } else {
                return (a->lp_bound > b->lp_bound) ? -1 : 1;
            }

        default:
            return 0;
    }
}

/* Heapify up */
static void heapify_up(NodeQueue *queue, int idx) {
    while (idx > 0) {
        int parent = (idx - 1) / 2;
        if (node_compare(queue->nodes[idx], queue->nodes[parent],
                        queue->strategy, queue->obj_sense) < 0) {
            BBNode *tmp = queue->nodes[idx];
            queue->nodes[idx] = queue->nodes[parent];
            queue->nodes[parent] = tmp;
            idx = parent;
        } else {
            break;
        }
    }
}

/* Heapify down */
static void heapify_down(NodeQueue *queue, int idx) {
    int size = queue->size;
    while (1) {
        int smallest = idx;
        int left = 2 * idx + 1;
        int right = 2 * idx + 2;

        if (left < size &&
            node_compare(queue->nodes[left], queue->nodes[smallest],
                        queue->strategy, queue->obj_sense) < 0) {
            smallest = left;
        }
        if (right < size &&
            node_compare(queue->nodes[right], queue->nodes[smallest],
                        queue->strategy, queue->obj_sense) < 0) {
            smallest = right;
        }

        if (smallest != idx) {
            BBNode *tmp = queue->nodes[idx];
            queue->nodes[idx] = queue->nodes[smallest];
            queue->nodes[smallest] = tmp;
            idx = smallest;
        } else {
            break;
        }
    }
}

int node_queue_push(NodeQueue *queue, BBNode *node) {
    if (!queue || !node) return -1;

    /* Expand if needed */
    if (queue->size >= queue->capacity) {
        int new_cap = queue->capacity * 2;
        BBNode **new_nodes = (BBNode**)realloc(queue->nodes, new_cap * sizeof(BBNode*));
        if (!new_nodes) return -1;
        queue->nodes = new_nodes;
        queue->capacity = new_cap;
    }

    queue->nodes[queue->size] = node;
    heapify_up(queue, queue->size);
    queue->size++;

    return 0;
}

BBNode* node_queue_pop(NodeQueue *queue) {
    if (!queue || queue->size == 0) return NULL;

    BBNode *node = queue->nodes[0];
    queue->size--;

    if (queue->size > 0) {
        queue->nodes[0] = queue->nodes[queue->size];
        heapify_down(queue, 0);
    }

    return node;
}

int node_queue_is_empty(const NodeQueue *queue) {
    return !queue || queue->size == 0;
}

/* Remove nodes with bound worse than cutoff */
void node_queue_update_bound(NodeQueue *queue, double cutoff) {
    if (!queue) return;

    int write_idx = 0;
    for (int i = 0; i < queue->size; i++) {
        BBNode *node = queue->nodes[i];
        int prune = 0;

        if (queue->obj_sense == 1) {  /* Minimize */
            if (node->lp_bound >= cutoff - RALPH_OPT_TOL) prune = 1;
        } else {  /* Maximize */
            if (node->lp_bound <= cutoff + RALPH_OPT_TOL) prune = 1;
        }

        if (prune) {
            bb_node_free(node);
        } else {
            queue->nodes[write_idx++] = node;
        }
    }
    queue->size = write_idx;

    /* Rebuild heap */
    for (int i = queue->size / 2 - 1; i >= 0; i--) {
        heapify_down(queue, i);
    }
}

/* Get best LP bound from open nodes in queue */
double node_queue_best_bound(const NodeQueue *queue) {
    if (!queue || queue->size == 0) {
        return (queue && queue->obj_sense == 1) ? RALPH_INFINITY : -RALPH_INFINITY;
    }

    double best = queue->nodes[0]->lp_bound;
    for (int i = 1; i < queue->size; i++) {
        if (queue->obj_sense == 1) {  /* Minimize */
            if (queue->nodes[i]->lp_bound < best) {
                best = queue->nodes[i]->lp_bound;
            }
        } else {  /* Maximize */
            if (queue->nodes[i]->lp_bound > best) {
                best = queue->nodes[i]->lp_bound;
            }
        }
    }
    return best;
}

/* ============================================================================
 * Branch and Bound Node
 * ============================================================================ */

BBNode* bb_node_create(int num_vars) {
    BBNode *node = (BBNode*)calloc(1, sizeof(BBNode));
    if (!node) return NULL;

    node->lb = (double*)malloc(num_vars * sizeof(double));
    node->ub = (double*)malloc(num_vars * sizeof(double));

    if (!node->lb || !node->ub) {
        bb_node_free(node);
        return NULL;
    }

    node->id = -1;
    node->depth = 0;
    node->parent_id = -1;
    node->branch_var = -1;
    node->lp_bound = -RALPH_INFINITY;
    node->estimate = -RALPH_INFINITY;

    return node;
}

void bb_node_free(BBNode *node) {
    if (!node) return;

    free(node->lb);
    free(node->ub);
    free(node->basis);
    free(node->var_status);
    free(node);
}

BBNode* bb_node_copy(const BBNode *src, int num_vars) {
    if (!src) return NULL;

    BBNode *dst = bb_node_create(num_vars);
    if (!dst) return NULL;

    dst->id = src->id;
    dst->depth = src->depth;
    dst->parent_id = src->parent_id;
    dst->branch_dir = src->branch_dir;
    dst->branch_var = src->branch_var;
    dst->branch_val = src->branch_val;
    dst->lp_bound = src->lp_bound;
    dst->lp_status = src->lp_status;
    dst->estimate = src->estimate;

    memcpy(dst->lb, src->lb, num_vars * sizeof(double));
    memcpy(dst->ub, src->ub, num_vars * sizeof(double));

    /* Copy basis information for warm starting */
    if (src->basis && src->var_status && src->basis_size > 0 && src->var_status_size > 0) {
        dst->basis = (int*)malloc(src->basis_size * sizeof(int));
        dst->var_status = (VarStatus*)malloc(src->var_status_size * sizeof(VarStatus));
        if (dst->basis && dst->var_status) {
            memcpy(dst->basis, src->basis, src->basis_size * sizeof(int));
            memcpy(dst->var_status, src->var_status, src->var_status_size * sizeof(VarStatus));
            dst->basis_size = src->basis_size;
            dst->var_status_size = src->var_status_size;
        }
    }

    return dst;
}

/* ============================================================================
 * Variable Selection for Branching
 * ============================================================================ */

/* Most infeasible variable selection */
static int select_most_infeasible(MIPSolver *solver, const double *solution) {
    int best_var = -1;
    double best_infeas = RALPH_INT_TOL;

    for (int k = 0; k < solver->num_integers; k++) {
        int j = solver->integer_vars[k];
        double val = solution[j];
        double frac = val - floor(val);
        double infeas = fmin(frac, 1.0 - frac);

        if (infeas > best_infeas) {
            best_infeas = infeas;
            best_var = j;
        }
    }

    return best_var;
}

/* Pseudo-cost based variable selection */
static int select_pseudo_cost(MIPSolver *solver, const double *solution) {
    int best_var = -1;
    double best_score = -1.0;

    for (int k = 0; k < solver->num_integers; k++) {
        int j = solver->integer_vars[k];
        double val = solution[j];
        double frac = val - floor(val);

        if (frac < RALPH_INT_TOL || frac > 1.0 - RALPH_INT_TOL) continue;

        /* Estimate degradation */
        double down_est = frac * solver->pseudo_cost_down[j];
        double up_est = (1.0 - frac) * solver->pseudo_cost_up[j];

        /* Score function: product of estimates */
        double score = fmax(down_est, RALPH_ZERO_TOL) * fmax(up_est, RALPH_ZERO_TOL);

        if (score > best_score) {
            best_score = score;
            best_var = j;
        }
    }

    /* Fall back to most infeasible if no good pseudo-costs */
    if (best_var < 0) {
        best_var = select_most_infeasible(solver, solution);
    }

    return best_var;
}

/* Strong branching - solve LP relaxations to evaluate branching choices */
int strong_branch(MIPSolver *solver, int var, double val,
                  double *down_obj, double *up_obj, int max_iter) {
    SimplexSolver *lp = solver->lp_solver;
    SimplexTableau *tab = lp->tableau;

    double orig_lb = tab->lb_ext[var];
    double orig_ub = tab->ub_ext[var];

    /* Try branching down */
    tab->ub_ext[var] = floor(val);
    tableau_compute_solution(tab);

    int save_max_iter = lp->max_iterations;
    lp->max_iterations = max_iter;

    dual_simplex_solve(lp);
    *down_obj = (lp->status == RALPH_STATUS_OPTIMAL) ? lp->obj_value : RALPH_INFINITY;

    /* Restore and try branching up */
    tab->ub_ext[var] = orig_ub;
    tab->lb_ext[var] = ceil(val);
    tableau_compute_solution(tab);

    dual_simplex_solve(lp);
    *up_obj = (lp->status == RALPH_STATUS_OPTIMAL) ? lp->obj_value : RALPH_INFINITY;

    /* Restore original bounds */
    tab->lb_ext[var] = orig_lb;
    tab->ub_ext[var] = orig_ub;
    lp->max_iterations = save_max_iter;

    return 0;
}

/* Reliability branching - hybrid of pseudo-cost and strong branching */
static int select_reliability_branch(MIPSolver *solver, const double *solution) {
    int best_var = -1;
    double best_score = -1.0;
    int reliability_threshold = 8;  /* Strong branch until this many observations */
    int max_strong = 5;             /* Max strong branching evaluations per node */

    int strong_count = 0;

    for (int k = 0; k < solver->num_integers; k++) {
        int j = solver->integer_vars[k];
        double val = solution[j];
        double frac = val - floor(val);

        if (frac < RALPH_INT_TOL || frac > 1.0 - RALPH_INT_TOL) continue;

        double down_est, up_est;

        /* Check if we need strong branching */
        int need_strong = (solver->pseudo_count_down[j] < reliability_threshold ||
                          solver->pseudo_count_up[j] < reliability_threshold);

        if (need_strong && strong_count < max_strong) {
            double down_obj, up_obj;
            strong_branch(solver, j, val, &down_obj, &up_obj, 100);

            /* Update pseudo-costs */
            double parent_obj = solver->lp_solver->obj_value;
            if (down_obj < RALPH_INFINITY/2) {
                update_pseudo_costs(solver, j, val, parent_obj, down_obj, BRANCH_DOWN);
            }
            if (up_obj < RALPH_INFINITY/2) {
                update_pseudo_costs(solver, j, val, parent_obj, up_obj, BRANCH_UP);
            }

            down_est = frac * solver->pseudo_cost_down[j];
            up_est = (1.0 - frac) * solver->pseudo_cost_up[j];
            strong_count++;
        } else {
            down_est = frac * solver->pseudo_cost_down[j];
            up_est = (1.0 - frac) * solver->pseudo_cost_up[j];
        }

        double score = fmax(down_est, RALPH_ZERO_TOL) * fmax(up_est, RALPH_ZERO_TOL);

        if (score > best_score) {
            best_score = score;
            best_var = j;
        }
    }

    if (best_var < 0) {
        best_var = select_most_infeasible(solver, solution);
    }

    return best_var;
}

int select_branch_variable(MIPSolver *solver, const double *solution, int *branch_var) {
    switch (solver->var_select) {
        case VAR_SELECT_MAX_INFEAS:
            *branch_var = select_most_infeasible(solver, solution);
            break;
        case VAR_SELECT_PSEUDO_COST:
            *branch_var = select_pseudo_cost(solver, solution);
            break;
        case VAR_SELECT_STRONG_BRANCH:
        case VAR_SELECT_RELIABILITY:
            *branch_var = select_reliability_branch(solver, solution);
            break;
        default:
            *branch_var = select_most_infeasible(solver, solution);
    }

    return (*branch_var >= 0) ? 0 : -1;
}

/* ============================================================================
 * Pseudo-Cost Management
 * ============================================================================ */

void update_pseudo_costs(MIPSolver *solver, int var, double val,
                        double parent_obj, double child_obj, BranchDir dir) {
    double delta_obj = child_obj - parent_obj;
    if (delta_obj < 0) delta_obj = 0;  /* Should not decrease */

    double frac = val - floor(val);

    if (dir == BRANCH_DOWN) {
        double delta_x = frac;
        if (delta_x > RALPH_ZERO_TOL) {
            double new_pseudo = delta_obj / delta_x;
            int count = solver->pseudo_count_down[var];
            solver->pseudo_cost_down[var] = (solver->pseudo_cost_down[var] * count + new_pseudo) / (count + 1);
            solver->pseudo_count_down[var]++;
        }
    } else {
        double delta_x = 1.0 - frac;
        if (delta_x > RALPH_ZERO_TOL) {
            double new_pseudo = delta_obj / delta_x;
            int count = solver->pseudo_count_up[var];
            solver->pseudo_cost_up[var] = (solver->pseudo_cost_up[var] * count + new_pseudo) / (count + 1);
            solver->pseudo_count_up[var]++;
        }
    }
}

double estimate_branch_obj(MIPSolver *solver, int var, double val, BranchDir dir) {
    double frac = val - floor(val);

    if (dir == BRANCH_DOWN) {
        return solver->lp_solver->obj_value + frac * solver->pseudo_cost_down[var];
    } else {
        return solver->lp_solver->obj_value + (1.0 - frac) * solver->pseudo_cost_up[var];
    }
}

/* ============================================================================
 * Create Child Nodes
 * ============================================================================ */

void compute_branch_children(MIPSolver *solver, BBNode *parent, int branch_var,
                            BBNode **child_down, BBNode **child_up) {
    int num_vars = solver->original_model->num_vars;
    double val = solver->lp_solver->solution[branch_var];

    /* Create down child (x <= floor(val)) */
    *child_down = bb_node_copy(parent, num_vars);
    if (*child_down) {
        (*child_down)->depth = parent->depth + 1;
        (*child_down)->parent_id = parent->id;
        (*child_down)->branch_var = branch_var;
        (*child_down)->branch_val = val;
        (*child_down)->branch_dir = BRANCH_DOWN;
        (*child_down)->ub[branch_var] = floor(val);
        (*child_down)->estimate = estimate_branch_obj(solver, branch_var, val, BRANCH_DOWN);
    }

    /* Create up child (x >= ceil(val)) */
    *child_up = bb_node_copy(parent, num_vars);
    if (*child_up) {
        (*child_up)->depth = parent->depth + 1;
        (*child_up)->parent_id = parent->id;
        (*child_up)->branch_var = branch_var;
        (*child_up)->branch_val = val;
        (*child_up)->branch_dir = BRANCH_UP;
        (*child_up)->lb[branch_var] = ceil(val);
        (*child_up)->estimate = estimate_branch_obj(solver, branch_var, val, BRANCH_UP);
    }
}

/* ============================================================================
 * Solution Checking
 * ============================================================================ */

int check_integer_feasibility(MIPSolver *solver, const double *solution) {
    for (int k = 0; k < solver->num_integers; k++) {
        int j = solver->integer_vars[k];
        double val = solution[j];
        double frac = val - floor(val);

        if (frac > RALPH_INT_TOL && frac < 1.0 - RALPH_INT_TOL) {
            return 0;  /* Not integer feasible */
        }
    }
    return 1;  /* Integer feasible */
}

double compute_integrality_violation(MIPSolver *solver, const double *solution) {
    double total_viol = 0.0;

    for (int k = 0; k < solver->num_integers; k++) {
        int j = solver->integer_vars[k];
        double val = solution[j];
        double frac = val - floor(val);
        total_viol += fmin(frac, 1.0 - frac);
    }

    return total_viol;
}

/* ============================================================================
 * Primal Heuristics
 * ============================================================================ */

int heuristic_rounding(MIPSolver *solver, const double *lp_solution, double *int_solution) {
    LPModel *model = solver->original_model;

    /* Copy LP solution */
    memcpy(int_solution, lp_solution, model->num_vars * sizeof(double));

    /* Round integer variables */
    for (int k = 0; k < solver->num_integers; k++) {
        int j = solver->integer_vars[k];
        double val = lp_solution[j];

        /* Round to nearest integer within bounds */
        double rounded = round(val);
        rounded = fmax(rounded, model->lb[j]);
        rounded = fmin(rounded, model->ub[j]);
        int_solution[j] = rounded;
    }

    /* Check integer feasibility */
    if (!check_integer_feasibility(solver, int_solution)) {
        return -1;
    }

    /* Check constraint feasibility (Ax sense b) */
    if (model->A && model->num_cons > 0) {
        double *ax = (double*)malloc(model->num_cons * sizeof(double));
        if (ax) {
            sparse_matvec(model->A, int_solution, ax);

            for (int i = 0; i < model->num_cons; i++) {
                double lhs = ax[i];
                double rhs = model->b[i];
                char sense = model->sense[i];

                int violated = 0;
                if (sense == 'L' && lhs > rhs + RALPH_FEAS_TOL) {
                    violated = 1;  /* ax > b for <= constraint */
                } else if (sense == 'G' && lhs < rhs - RALPH_FEAS_TOL) {
                    violated = 1;  /* ax < b for >= constraint */
                } else if (sense == 'E' && fabs(lhs - rhs) > RALPH_FEAS_TOL) {
                    violated = 1;  /* ax != b for = constraint */
                }

                if (violated) {
                    free(ax);
                    return -1;
                }
            }
            free(ax);
        }
    }

    return 0;
}

/* ============================================================================
 * Utility
 * ============================================================================ */

void mip_print_node_info(const MIPSolver *solver, const BBNode *node) {
    printf("Node %d: depth=%d, bound=%.4f, status=%d\n",
           node->id, node->depth, node->lp_bound, node->lp_status);
}
