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
#include <stdint.h>
#include <limits.h>
#include "mip.h"
#include "mip_lp_adapter.h"

static double mip_branch_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

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
    queue->has_incumbent = 0;

    queue->nodes = (BBNode**)calloc(queue->capacity, sizeof(BBNode*));
    if (!queue->nodes) {
        free(queue);
        return NULL;
    }

    return queue;
}

void node_queue_free(NodeQueue *queue) {
    node_queue_free_with_pool(queue, NULL);
}

void node_queue_free_with_pool(NodeQueue *queue, BBNodePool *pool) {
    if (!queue) return;

    for (int i = 0; i < queue->size; i++) {
        bb_node_pool_return(pool, queue->nodes[i]);
    }
    free(queue->nodes);
    free(queue);
}

/* Compare nodes based on strategy */
static int node_compare(const BBNode *a, const BBNode *b,
                        NodeSelectStrategy strategy, int obj_sense,
                        int has_incumbent) {
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
            if (!has_incumbent) {
                /* Pre-incumbent: depth-first to find feasible solution fast */
                if (a->depth != b->depth)
                    return (a->depth > b->depth) ? -1 : 1;
                /* Tiebreak by LP bound */
                if (obj_sense == 1)
                    return (a->lp_bound < b->lp_bound) ? -1 : 1;
                else
                    return (a->lp_bound > b->lp_bound) ? -1 : 1;
            }
            /* Post-incumbent: best-first to close gap efficiently */
            if (obj_sense == 1)
                return (a->lp_bound < b->lp_bound) ? -1 : 1;
            else
                return (a->lp_bound > b->lp_bound) ? -1 : 1;

        default:
            return 0;
    }
}

/* Heapify up */
static void heapify_up(NodeQueue *queue, int idx) {
    while (idx > 0) {
        int parent = (idx - 1) / 2;
        if (node_compare(queue->nodes[idx], queue->nodes[parent],
                        queue->strategy, queue->obj_sense,
                        queue->has_incumbent) < 0) {
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
                        queue->strategy, queue->obj_sense,
                        queue->has_incumbent) < 0) {
            smallest = left;
        }
        if (right < size &&
            node_compare(queue->nodes[right], queue->nodes[smallest],
                        queue->strategy, queue->obj_sense,
                        queue->has_incumbent) < 0) {
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
    node_queue_update_bound_with_pool(queue, cutoff, NULL);
}

void node_queue_update_bound_with_pool(NodeQueue *queue, double cutoff, BBNodePool *pool) {
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
            bb_node_pool_return(pool, node);
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

/* Notify queue that an incumbent was found (for HYBRID strategy switch) */
void node_queue_set_incumbent_found(NodeQueue *queue) {
    if (!queue || queue->has_incumbent) return;
    queue->has_incumbent = 1;
    /* HYBRID ordering changes — rebuild heap */
    if (queue->strategy == NODE_SELECT_HYBRID && queue->size > 1) {
        for (int i = queue->size / 2 - 1; i >= 0; i--)
            heapify_down(queue, i);
    }
}

/* ============================================================================
 * Branch and Bound Node
 * ============================================================================ */

BBNode* bb_node_create(int num_vars) {
    BBNode *node = (BBNode*)calloc(1, sizeof(BBNode));
    if (!node) return NULL;

    node->lb = (double*)calloc(num_vars, sizeof(double));
    node->ub = (double*)calloc(num_vars, sizeof(double));

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
    if (num_vars < 0) return NULL;

    /* Check for integer overflow in memcpy size calculation */
    if ((size_t)num_vars > SIZE_MAX / sizeof(double)) {
        return NULL;
    }

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

    memcpy(dst->lb, src->lb, (size_t)num_vars * sizeof(double));
    memcpy(dst->ub, src->ub, (size_t)num_vars * sizeof(double));

    /* Copy basis information for warm starting */
    if (src->basis && src->var_status && src->basis_size > 0 && src->var_status_size > 0) {
        dst->basis = (int*)calloc(src->basis_size, sizeof(int));
        dst->var_status = (VarStatus*)calloc(src->var_status_size, sizeof(VarStatus));
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
 * BBNode Memory Pool
 *
 * Pre-allocates a block of nodes to reduce malloc overhead in deep B&B trees.
 * Benefits:
 * - Single allocation instead of per-node mallocs
 * - Contiguous memory for better cache behavior
 * - O(1) allocation/deallocation (stack-based free list)
 * - Reduces memory fragmentation in long-running MIP solves
 * ============================================================================ */

BBNodePool* bb_node_pool_create(int capacity, int num_vars) {
    if (capacity <= 0 || num_vars <= 0) return NULL;

    BBNodePool *pool = (BBNodePool*)calloc(1, sizeof(BBNodePool));
    if (!pool) return NULL;

    pool->capacity = capacity;
    pool->num_vars = num_vars;

    /* Allocate node structures in single block */
    pool->nodes = (BBNode*)calloc(capacity, sizeof(BBNode));
    if (!pool->nodes) {
        free(pool);
        return NULL;
    }

    /* Check for overflow before allocation */
    if ((size_t)capacity > SIZE_MAX / (size_t)num_vars / sizeof(double)) {
        free(pool->nodes);
        free(pool);
        return NULL;  /* Would overflow */
    }

    /* Allocate contiguous lb/ub arrays for all nodes */
    size_t array_size = (size_t)capacity * (size_t)num_vars;
    pool->lb_pool = (double*)calloc(array_size, sizeof(double));
    pool->ub_pool = (double*)calloc(array_size, sizeof(double));
    if (!pool->lb_pool || !pool->ub_pool) {
        free(pool->lb_pool);
        free(pool->ub_pool);
        free(pool->nodes);
        free(pool);
        return NULL;
    }

    /* Allocate free list (stack of available indices) */
    pool->free_list = (int*)calloc(capacity, sizeof(int));
    if (!pool->free_list) {
        free(pool->lb_pool);
        free(pool->ub_pool);
        free(pool->nodes);
        free(pool);
        return NULL;
    }

    /* Initialize: all nodes are free, point lb/ub into pools */
    for (int i = 0; i < capacity; i++) {
        pool->nodes[i].lb = pool->lb_pool + (size_t)i * num_vars;
        pool->nodes[i].ub = pool->ub_pool + (size_t)i * num_vars;
        pool->nodes[i].id = -1;  /* Mark as unused */
        pool->nodes[i].basis = NULL;
        pool->nodes[i].var_status = NULL;
        pool->free_list[i] = capacity - 1 - i;  /* Stack: top = 0 */
    }
    pool->free_count = capacity;
    pool->nodes_allocated = 0;

    return pool;
}

void bb_node_pool_free(BBNodePool *pool) {
    if (!pool) return;

    /* Free any basis/var_status arrays allocated on nodes */
    for (int i = 0; i < pool->capacity; i++) {
        free(pool->nodes[i].basis);
        free(pool->nodes[i].var_status);
    }

    free(pool->lb_pool);
    free(pool->ub_pool);
    free(pool->free_list);
    free(pool->nodes);
    free(pool);
}

BBNode* bb_node_pool_get(BBNodePool *pool) {
    if (!pool) return NULL;

    /* If pool exhausted, fall back to regular allocation */
    if (pool->free_count == 0) {
        return bb_node_create(pool->num_vars);
    }

    /* Pop from free list */
    int idx = pool->free_list[--pool->free_count];
    BBNode *node = &pool->nodes[idx];

    /* Initialize node (lb/ub already point to pool arrays) */
    node->id = -1;
    node->depth = 0;
    node->parent_id = -1;
    node->branch_var = -1;
    node->branch_val = 0.0;
    node->branch_dir = BRANCH_DOWN;
    node->lp_bound = -RALPH_INFINITY;
    node->lp_status = 0;
    node->lp_iterations = 0;
    node->estimate = -RALPH_INFINITY;
    /* basis/var_status may have data from previous use - leave for caller */

    /* Track high-water mark */
    int in_use = pool->capacity - pool->free_count;
    if (in_use > pool->nodes_allocated) {
        pool->nodes_allocated = in_use;
    }

    return node;
}

void bb_node_pool_return(BBNodePool *pool, BBNode *node) {
    if (!node) return;

    /* If no pool, use regular free */
    if (!pool) {
        bb_node_free(node);
        return;
    }

    /* Verify node belongs to this pool */
    ptrdiff_t offset = node - pool->nodes;
    if (offset < 0 || offset >= pool->capacity) {
        /* Node not from this pool - fall back to regular free */
        bb_node_free(node);
        return;
    }

    /* Free basis info (not pooled - varies per node) */
    free(node->basis);
    free(node->var_status);
    node->basis = NULL;
    node->var_status = NULL;
    node->basis_size = 0;
    node->var_status_size = 0;

    /* Push to free list */
    pool->free_list[pool->free_count++] = (int)offset;
}

/* Copy a node using pool if available, falling back to regular allocation */
BBNode* bb_node_pool_copy(BBNodePool *pool, const BBNode *src, int num_vars) {
    if (!src) return NULL;

    BBNode *dst = pool ? bb_node_pool_get(pool) : bb_node_create(num_vars);
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
        dst->basis = (int*)calloc(src->basis_size, sizeof(int));
        dst->var_status = (VarStatus*)calloc(src->var_status_size, sizeof(VarStatus));
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

/* Forward declarations for priority-aware variants */
static int select_most_infeasible_with_priority(MIPSolver *solver, const double *solution, int max_prio);
static int select_pseudo_cost_with_priority(MIPSolver *solver, const double *solution, int max_prio);

/*
 * Most infeasible variable selection.
 *
 * Optimization: Use restrict pointers and avoid redundant floor() calls.
 * The infeasibility is |frac - 0.5| distance from 0.5, maximized when frac = 0.5.
 */
static int select_most_infeasible(MIPSolver *solver, const double *solution) {
    int best_var = -1;
    double best_infeas = RALPH_INT_TOL;

    const int * restrict int_vars = solver->integer_vars;
    const int num_int = solver->num_integers;
    const LPModel *wm = solver->working_model;
    const double *lb = wm ? wm->lb : NULL;
    const double *ub = wm ? wm->ub : NULL;

    for (int k = 0; k < num_int; k++) {
        int j = int_vars[k];
        if (lb && ub && ub[j] - lb[j] <= RALPH_INT_TOL) continue;
        double val = solution[j];
        /* Use subtraction from truncated value - faster than floor() on some systems */
        double frac = val - (double)(long)val;
        if (frac < 0.0) frac += 1.0;  /* Handle negative values */
        /* Infeasibility: distance from nearest integer = min(frac, 1-frac) */
        double infeas = (frac <= 0.5) ? frac : (1.0 - frac);

        if (infeas > best_infeas) {
            best_infeas = infeas;
            best_var = j;
        }
    }

    return best_var;
}

/*
 * Pseudo-cost based variable selection.
 *
 * Optimization: Use restrict pointers for better aliasing hints.
 */
static int select_pseudo_cost(MIPSolver *solver, const double *solution) {
    int best_var = -1;
    double best_score = -1.0;

    const int * restrict int_vars = solver->integer_vars;
    const double * restrict pc_down = solver->pseudo_cost_down;
    const double * restrict pc_up = solver->pseudo_cost_up;
    const int num_int = solver->num_integers;
    const LPModel *wm = solver->working_model;
    const double *lb = wm ? wm->lb : NULL;
    const double *ub = wm ? wm->ub : NULL;

    for (int k = 0; k < num_int; k++) {
        int j = int_vars[k];
        if (lb && ub && ub[j] - lb[j] <= RALPH_INT_TOL) continue;
        double val = solution[j];
        double frac = val - (double)(long)val;
        if (frac < 0.0) frac += 1.0;

        if (frac < RALPH_INT_TOL || frac > 1.0 - RALPH_INT_TOL) continue;

        /* Estimate degradation */
        double down_est = frac * pc_down[j];
        double up_est = (1.0 - frac) * pc_up[j];

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

/* Strong branching - solve LP relaxations to evaluate branching choices.
 * Uses the MIP/LP adapter so probing/recovery flows through one LP-state API.
 */
int strong_branch(MIPSolver *solver, int var, double val,
                  double *down_obj, double *up_obj, int max_iter) {
    int recovered = 0;

    *down_obj = RALPH_INFINITY;
    *up_obj = RALPH_INFINITY;

    if (!solver || !solver->working_model || !solver->lp_solver || !solver->lp_solver->tableau) {
        return -1;
    }

    SimplexSolver *lp = solver->lp_solver;
    SimplexTableau *tab = lp->tableau;
    int num_struct = solver->working_model->num_vars;
    if (var < 0 || var >= num_struct || num_struct <= 0) return -1;
    if (!tab->lb_ext || !tab->ub_ext || !tab->basis || !tab->var_status) return -1;
    solver->strong_branch_probes++;
    double probe_start_ms = mip_branch_now_ms();

    int m = tab->m;
    int n = tab->n;
    int *save_basis = (int*)calloc((size_t)m, sizeof(int));
    VarStatus *save_var_status = (VarStatus*)calloc((size_t)n, sizeof(VarStatus));
    double *probe_lb = (double*)calloc((size_t)num_struct, sizeof(double));
    double *probe_ub = (double*)calloc((size_t)num_struct, sizeof(double));
    if (!save_basis || !save_var_status || !probe_lb || !probe_ub) {
        free(save_basis);
        free(save_var_status);
        free(probe_lb);
        free(probe_ub);
        return -1;
    }

    memcpy(save_basis, tab->basis, (size_t)m * sizeof(int));
    memcpy(save_var_status, tab->var_status, (size_t)n * sizeof(VarStatus));
    memcpy(probe_lb, tab->lb_ext, (size_t)num_struct * sizeof(double));
    memcpy(probe_ub, tab->ub_ext, (size_t)num_struct * sizeof(double));
    double orig_lb = probe_lb[var];
    double orig_ub = probe_ub[var];

    /* Try branching down */
    probe_ub[var] = floor(val);
    if (mip_lp_apply_structural_bounds(tab, num_struct, probe_lb, probe_ub) != 0 ||
        mip_lp_recompute(tab) != 0 ||
        mip_lp_dual_reopt(lp, max_iter, NULL) != 0 ||
        !lp->tableau || !lp->solution || lp->tableau != tab) {
        goto strong_fail;
    }
    *down_obj = (lp->status == RALPH_STATUS_OPTIMAL) ? lp->obj_value : RALPH_INFINITY;

    if (mip_lp_restore_warm_basis(lp, m, n, save_basis, save_var_status) != 0 ||
        !lp->tableau || !lp->solution || lp->tableau != tab) {
        goto strong_fail;
    }

    /* Try branching up */
    probe_ub[var] = orig_ub;
    probe_lb[var] = ceil(val);
    if (mip_lp_apply_structural_bounds(tab, num_struct, probe_lb, probe_ub) != 0 ||
        mip_lp_recompute(tab) != 0 ||
        mip_lp_dual_reopt(lp, max_iter, NULL) != 0 ||
        !lp->tableau || !lp->solution || lp->tableau != tab) {
        goto strong_fail;
    }
    *up_obj = (lp->status == RALPH_STATUS_OPTIMAL) ? lp->obj_value : RALPH_INFINITY;

    /* Restore original bounds and basis */
    probe_lb[var] = orig_lb;
    probe_ub[var] = orig_ub;
    if (mip_lp_apply_structural_bounds(tab, num_struct, probe_lb, probe_ub) != 0 ||
        mip_lp_restore_warm_basis(lp, m, n, save_basis, save_var_status) != 0 ||
        !lp->tableau || !lp->solution || lp->tableau != tab) {
        goto strong_fail;
    }
    lp->status = RALPH_STATUS_OPTIMAL;

    free(save_basis);
    free(save_var_status);
    free(probe_lb);
    free(probe_ub);
    solver->strong_branch_time_ms += mip_branch_now_ms() - probe_start_ms;
    return 0;

strong_fail:
    /* Explicit LP-state recovery contract for probing:
     * leave the caller with a usable (tableau+solution) LP state. */
    solver->strong_branch_failures++;
    if (lp->tableau) {
        probe_lb[var] = orig_lb;
        probe_ub[var] = orig_ub;
        if (mip_lp_apply_structural_bounds(lp->tableau, num_struct, probe_lb, probe_ub) == 0 &&
            mip_lp_restore_warm_basis(lp, m, n, save_basis, save_var_status) == 0 &&
            lp->tableau && lp->solution) {
            recovered = 1;
        }
    }
    if (!recovered && mip_lp_recover_state(lp) == 0 && lp->tableau && lp->solution) {
        recovered = 1;
    }
    if (recovered) solver->strong_branch_recoveries++;

    free(save_basis);
    free(save_var_status);
    free(probe_lb);
    free(probe_ub);
    solver->strong_branch_time_ms += mip_branch_now_ms() - probe_start_ms;
    return -1;
}

/* Adaptive reliability probing for deep no-incumbent trees.
 * Early search keeps full probing quality; late no-incumbent search
 * downshifts probe cost to avoid spending most wall time on probing. */
static int reliability_strong_probe_limit(const MIPSolver *solver) {
    if (!solver) return MIP_RELIABILITY_MAX_STRONG;
    if (solver->has_incumbent) {
        if (solver->nodes_explored >= MIP_RELIABILITY_POST_INCUMBENT_PROBE_NODES) return 0;
        return MIP_RELIABILITY_POST_INCUMBENT_MAX_STRONG;
    }
    if (solver->nodes_explored >= MIP_RELIABILITY_NO_INCUMBENT_DISABLE_AFTER) return 0;
    if (solver->nodes_explored >= MIP_RELIABILITY_NO_INCUMBENT_TAPER_AFTER) return 1;
    return MIP_RELIABILITY_MAX_STRONG;
}

static int reliability_probe_pivot_budget(const MIPSolver *solver) {
    if (!solver) return MIP_RELIABILITY_PIVOT_BUDGET;
    if (solver->has_incumbent) return MIP_RELIABILITY_POST_INCUMBENT_PIVOT_BUDGET;
    if (solver->nodes_explored >= MIP_RELIABILITY_NO_INCUMBENT_TAPER_AFTER) {
        return MIP_RELIABILITY_NO_INCUMBENT_PIVOT_BUDGET;
    }
    return MIP_RELIABILITY_PIVOT_BUDGET;
}

static void reliability_shortlist_insert(int *vars, double *vals, double *fracs,
                                         double *scores, int *count,
                                         int var, double val, double frac, double score) {
    int limit = MIP_RELIABILITY_CANDIDATE_LIMIT;
    if (!vars || !vals || !fracs || !scores || !count || limit <= 0) return;
    if (*count >= limit && score <= scores[limit - 1]) return;

    int pos = (*count < limit) ? (*count)++ : limit - 1;
    while (pos > 0 && score > scores[pos - 1]) {
        vars[pos] = vars[pos - 1];
        vals[pos] = vals[pos - 1];
        fracs[pos] = fracs[pos - 1];
        scores[pos] = scores[pos - 1];
        pos--;
    }

    vars[pos] = var;
    vals[pos] = val;
    fracs[pos] = frac;
    scores[pos] = score;
}

/*
 * Reliability branching core - hybrid of pseudo-cost and strong branching.
 *
 * If max_prio > INT_MIN and priorities are set, only considers variables
 * at the maximum priority level (matching select_pseudo_cost_with_priority).
 * Otherwise, considers all fractional integer variables.
 */
static int select_reliability_branch_impl(MIPSolver *solver, const double *solution,
                                           int use_priorities, int max_prio) {
    int best_var = -1;
    double best_score = -1.0;
    int strong_count = 0;
    int strong_limit = reliability_strong_probe_limit(solver);
    int strong_pivot_budget = reliability_probe_pivot_budget(solver);
    int strong_failed = 0;  /* Stop strong branching if LP state corrupted */
    int strong_vars[MIP_RELIABILITY_CANDIDATE_LIMIT];
    double strong_vals[MIP_RELIABILITY_CANDIDATE_LIMIT];
    double strong_fracs[MIP_RELIABILITY_CANDIDATE_LIMIT];
    double strong_scores[MIP_RELIABILITY_CANDIDATE_LIMIT];
    int strong_candidates = 0;

    const int * restrict int_vars = solver->integer_vars;
    const int * restrict prios = solver->branch_priorities;
    const int num_int = solver->num_integers;
    const LPModel *wm = solver->working_model;
    const double *lb = wm ? wm->lb : NULL;
    const double *ub = wm ? wm->ub : NULL;

    if (strong_limit <= 0) {
        if (use_priorities) return select_pseudo_cost_with_priority(solver, solution, max_prio);
        return select_pseudo_cost(solver, solution);
    }

    for (int k = 0; k < num_int; k++) {
        if (solver->lp_solver && solver->lp_solver->solution) {
            solution = solver->lp_solver->solution;
        }
        if (!solution) break;

        int j = int_vars[k];
        if (lb && ub && ub[j] - lb[j] <= RALPH_INT_TOL) continue;

        /* Priority filter: skip if not at max priority */
        if (use_priorities && prios && prios[j] < max_prio) continue;

        double val = solution[j];
        double frac = val - floor(val);

        if (frac < RALPH_INT_TOL || frac > 1.0 - RALPH_INT_TOL) continue;

        double down_est, up_est;

        /* Check if we need strong branching */
        int need_strong = !strong_failed &&
                          (solver->pseudo_count_down[j] < MIP_RELIABILITY_THRESHOLD ||
                           solver->pseudo_count_up[j] < MIP_RELIABILITY_THRESHOLD);

        down_est = frac * solver->pseudo_cost_down[j];
        up_est = (1.0 - frac) * solver->pseudo_cost_up[j];

        if (need_strong && solver->lp_solver && solver->lp_solver->tableau) {
            double score = fmax(down_est, RALPH_ZERO_TOL) * fmax(up_est, RALPH_ZERO_TOL);
            reliability_shortlist_insert(strong_vars, strong_vals, strong_fracs,
                                         strong_scores, &strong_candidates,
                                         j, val, frac, score);
        }

        double score = fmax(down_est, RALPH_ZERO_TOL) * fmax(up_est, RALPH_ZERO_TOL);

        if (score > best_score) {
            best_score = score;
            best_var = j;
        }
    }

    for (int c = 0; c < strong_candidates && strong_count < strong_limit; c++) {
        int j = strong_vars[c];
        double val = strong_vals[c];
        double frac = strong_fracs[c];
        double down_obj, up_obj;

        int sb_result = strong_branch(solver, j, val, &down_obj, &up_obj,
                                      strong_pivot_budget);

        /* Re-read solution pointer: strong_branch() may recover LP state. */
        if (solver->lp_solver) {
            solution = solver->lp_solver->solution;
        }
        if (!solution) break;  /* LP state lost, recovery handled below */

        if (sb_result == 0) {
            double parent_obj = solver->lp_solver->obj_value;
            if (down_obj < RALPH_INFINITY / 2) {
                update_pseudo_costs(solver, j, val, parent_obj, down_obj, BRANCH_DOWN);
            }
            if (up_obj < RALPH_INFINITY / 2) {
                update_pseudo_costs(solver, j, val, parent_obj, up_obj, BRANCH_UP);
            }
            strong_count++;
        } else {
            strong_failed = 1;
            break;
        }

        double down_est = frac * solver->pseudo_cost_down[j];
        double up_est = (1.0 - frac) * solver->pseudo_cost_up[j];
        double score = fmax(down_est, RALPH_ZERO_TOL) * fmax(up_est, RALPH_ZERO_TOL);
        if (score > best_score) {
            best_score = score;
            best_var = j;
        }
    }

    /* If strong branching corrupted the LP state, re-solve to restore it.
     * This ensures the solution array is valid for compute_branch_children. */
    if (strong_failed && solver->lp_solver) {
        if (!solver->lp_solver->solution || !solver->lp_solver->tableau)
            (void)mip_lp_recover_state(solver->lp_solver);
        /* Refresh solution pointer after recovery */
        solution = solver->lp_solver->solution;
    }

    if (best_var < 0 && solution) {
        if (use_priorities) {
            best_var = select_most_infeasible_with_priority(solver, solution, max_prio);
        } else {
            best_var = select_most_infeasible(solver, solution);
        }
    }

    return best_var;
}

/* Reliability branching without priority filtering */
static int select_reliability_branch(MIPSolver *solver, const double *solution) {
    return select_reliability_branch_impl(solver, solution, 0, 0);
}

/* Reliability branching with priority filtering */
static int select_reliability_branch_with_priority(MIPSolver *solver, const double *solution,
                                                     int max_prio) {
    return select_reliability_branch_impl(solver, solution, 1, max_prio);
}

/*
 * Find the maximum priority among fractional integer variables.
 * Returns the max priority, or 0 if no fractional variables exist.
 */
static int find_max_priority(MIPSolver *solver, const double *solution) {
    if (!solver->branch_priorities) return 0;  /* All equal priority */

    int max_prio = INT_MIN;
    const int * restrict int_vars = solver->integer_vars;
    const int num_int = solver->num_integers;
    const int * restrict prios = solver->branch_priorities;
    const int num_vars = solver->original_model->num_vars;
    const LPModel *wm = solver->working_model;
    const double *lb = wm ? wm->lb : NULL;
    const double *ub = wm ? wm->ub : NULL;

    for (int k = 0; k < num_int; k++) {
        int j = int_vars[k];
        if (j < 0 || j >= num_vars) continue;
        if (lb && ub && ub[j] - lb[j] <= RALPH_INT_TOL) continue;
        double val = solution[j];
        double frac = val - (double)(long)val;
        if (frac < 0.0) frac += 1.0;

        if (frac > RALPH_INT_TOL && frac < 1.0 - RALPH_INT_TOL) {
            if (prios[j] > max_prio) {
                max_prio = prios[j];
            }
        }
    }
    return (max_prio == INT_MIN) ? 0 : max_prio;
}

/*
 * Most infeasible selection with priority filtering.
 * Only considers variables at the maximum priority level.
 */
static int select_most_infeasible_with_priority(MIPSolver *solver, const double *solution, int max_prio) {
    int best_var = -1;
    double best_infeas = RALPH_INT_TOL;

    const int * restrict int_vars = solver->integer_vars;
    const int num_int = solver->num_integers;
    const int * restrict prios = solver->branch_priorities;
    const LPModel *wm = solver->working_model;
    const double *lb = wm ? wm->lb : NULL;
    const double *ub = wm ? wm->ub : NULL;

    for (int k = 0; k < num_int; k++) {
        int j = int_vars[k];

        /* Skip if not at max priority */
        if (prios && prios[j] < max_prio) continue;
        if (lb && ub && ub[j] - lb[j] <= RALPH_INT_TOL) continue;

        double val = solution[j];
        double frac = val - (double)(long)val;
        if (frac < 0.0) frac += 1.0;
        double infeas = (frac <= 0.5) ? frac : (1.0 - frac);

        if (infeas > best_infeas) {
            best_infeas = infeas;
            best_var = j;
        }
    }

    return best_var;
}

/*
 * Pseudo-cost selection with priority filtering.
 */
static int select_pseudo_cost_with_priority(MIPSolver *solver, const double *solution, int max_prio) {
    int best_var = -1;
    double best_score = -1.0;

    const int * restrict int_vars = solver->integer_vars;
    const double * restrict pc_down = solver->pseudo_cost_down;
    const double * restrict pc_up = solver->pseudo_cost_up;
    const int num_int = solver->num_integers;
    const int * restrict prios = solver->branch_priorities;
    const int num_vars = solver->original_model->num_vars;
    const LPModel *wm = solver->working_model;
    const double *lb = wm ? wm->lb : NULL;
    const double *ub = wm ? wm->ub : NULL;

    for (int k = 0; k < num_int; k++) {
        int j = int_vars[k];
        if (j < 0 || j >= num_vars) continue;
        if (lb && ub && ub[j] - lb[j] <= RALPH_INT_TOL) continue;

        /* Skip if not at max priority */
        if (prios && prios[j] < max_prio) continue;

        double val = solution[j];
        double frac = val - (double)(long)val;
        if (frac < 0.0) frac += 1.0;

        if (frac < RALPH_INT_TOL || frac > 1.0 - RALPH_INT_TOL) continue;

        double down_est = frac * pc_down[j];
        double up_est = (1.0 - frac) * pc_up[j];
        double score = fmax(down_est, RALPH_ZERO_TOL) * fmax(up_est, RALPH_ZERO_TOL);

        if (score > best_score) {
            best_score = score;
            best_var = j;
        }
    }

    if (best_var < 0) {
        best_var = select_most_infeasible_with_priority(solver, solution, max_prio);
    }

    return best_var;
}

int select_branch_variable(MIPSolver *solver, const double *solution, int *branch_var) {
    /* Try user-provided branching callback first */
    if (solver->has_branch_callback && solver->branch_callback.select_branch_var) {
        LPModel *model = solver->original_model;
        int user_var = solver->branch_callback.select_branch_var(
            solver->branch_callback.user_data,
            solution,
            model->num_vars,
            solver->is_integer,
            model->lb,
            model->ub
        );

        /* If user returns valid variable index, use it */
        if (user_var >= 0 && user_var < model->num_vars) {
            LPModel *wm = solver->working_model ? solver->working_model : model;
            /* Verify it's actually fractional and branchable at current node */
            if (solver->is_integer[user_var] &&
                wm->ub[user_var] - wm->lb[user_var] > RALPH_INT_TOL) {
                double val = solution[user_var];
                double frac = val - floor(val);
                if (frac > RALPH_INT_TOL && frac < 1.0 - RALPH_INT_TOL) {
                    *branch_var = user_var;
                    return 0;
                }
            }
        }
        /* If user returns -1 or invalid variable, fall through to default */
    }

    /* Find max priority among fractional variables */
    int max_prio = find_max_priority(solver, solution);

    switch (solver->var_select) {
        case VAR_SELECT_MAX_INFEAS:
            if (solver->branch_priorities) {
                *branch_var = select_most_infeasible_with_priority(solver, solution, max_prio);
            } else {
                *branch_var = select_most_infeasible(solver, solution);
            }
            break;
        case VAR_SELECT_PSEUDO_COST:
            if (solver->branch_priorities) {
                *branch_var = select_pseudo_cost_with_priority(solver, solution, max_prio);
            } else {
                *branch_var = select_pseudo_cost(solver, solution);
            }
            break;
        case VAR_SELECT_STRONG_BRANCH:
        case VAR_SELECT_RELIABILITY:
            if (solver->branch_priorities) {
                *branch_var = select_reliability_branch_with_priority(solver, solution, max_prio);
            } else {
                *branch_var = select_reliability_branch(solver, solution);
            }
            break;
        case VAR_SELECT_SCP: {
            /* SCP constraint branching */
            int element, set;
            if (select_scp_branch(solver, solution, &element, &set) == 0) {
                *branch_var = set;
            } else {
                /* Fall back to most infeasible if SCP branching fails */
                *branch_var = select_most_infeasible(solver, solution);
            }
            break;
        }
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
    if (!solver->lp_solver || !solver->lp_solver->solution) {
        *child_down = NULL;
        *child_up = NULL;
        return;
    }

    if (branch_var < 0 || branch_var >= num_vars) {
        *child_down = NULL;
        *child_up = NULL;
        return;
    }

    double val = solver->lp_solver->solution[branch_var];
    double down_ub = floor(val);
    double up_lb = ceil(val);

    int can_down = (down_ub < parent->ub[branch_var] - RALPH_INT_TOL) &&
                   (down_ub >= parent->lb[branch_var] - RALPH_INT_TOL);
    int can_up = (up_lb > parent->lb[branch_var] + RALPH_INT_TOL) &&
                 (up_lb <= parent->ub[branch_var] + RALPH_INT_TOL);

    *child_down = NULL;
    *child_up = NULL;
    if (!can_down && !can_up) return;

    /* Create down child (x <= floor(val)) using pool if available */
    if (can_down) *child_down = bb_node_pool_copy(solver->node_pool, parent, num_vars);
    if (can_down && *child_down) {
        (*child_down)->depth = parent->depth + 1;
        (*child_down)->parent_id = parent->id;
        (*child_down)->branch_var = branch_var;
        (*child_down)->branch_val = val;
        (*child_down)->branch_dir = BRANCH_DOWN;
        (*child_down)->ub[branch_var] = down_ub;
        (*child_down)->estimate = estimate_branch_obj(solver, branch_var, val, BRANCH_DOWN);
    }

    /* Create up child (x >= ceil(val)) using pool if available */
    if (can_up) *child_up = bb_node_pool_copy(solver->node_pool, parent, num_vars);
    if (can_up && *child_up) {
        (*child_up)->depth = parent->depth + 1;
        (*child_up)->parent_id = parent->id;
        (*child_up)->branch_var = branch_var;
        (*child_up)->branch_val = val;
        (*child_up)->branch_dir = BRANCH_UP;
        (*child_up)->lb[branch_var] = up_lb;
        (*child_up)->estimate = estimate_branch_obj(solver, branch_var, val, BRANCH_UP);
    }

    /* Apply preferred branch direction by swapping if needed.
     * The first child (child_down in original output slot) is explored first
     * in depth-first search. By swapping, we control which direction is tried first.
     */
    if (solver->branch_directions &&
        branch_var >= 0 && branch_var < solver->original_model->num_vars) {
        int pref = solver->branch_directions[branch_var];
        if (pref > 0) {  /* RALPH_BRANCH_UP: prefer up first */
            BBNode *tmp = *child_down;
            *child_down = *child_up;
            *child_up = tmp;
        }
        /* pref < 0 (RALPH_BRANCH_DOWN) or pref == 0 (AUTO): keep default order */
    }
}

/* ============================================================================
 * Solution Checking
 * ============================================================================ */

int check_integer_feasibility(MIPSolver *solver, const double *solution) {
    const LPModel *model;

    if (!solver || !solution) return 0;
    model = solver->working_model ? solver->working_model : solver->original_model;

    for (int k = 0; k < solver->num_integers; k++) {
        int j = solver->integer_vars[k];
        double val = solution[j];
        if (!isfinite(val)) {
            return 0;
        }
        if (model &&
            (val < model->lb[j] - RALPH_FEAS_TOL || val > model->ub[j] + RALPH_FEAS_TOL)) {
            return 0;  /* Violates variable bounds */
        }
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
        double *ax = (double*)calloc(model->num_cons, sizeof(double));
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
 * SCP-Specific Heuristics (Phase 4)
 * ============================================================================ */

/*
 * Check if model has SCP structure (for heuristic applicability).
 * Lightweight check without full detection overhead.
 */
int is_scp_model(const LPModel *model) {
    if (!model || model->num_vars == 0 || model->num_cons == 0) return 0;

    /* All variables must be binary */
    for (int j = 0; j < model->num_vars; j++) {
        if (model->var_type[j] != 'B') return 0;
    }

    /* All constraints must be >= or = with positive RHS */
    for (int i = 0; i < model->num_cons; i++) {
        if (model->sense[i] != 'G' && model->sense[i] != 'E') return 0;
        if (model->b[i] < RALPH_ZERO_TOL) return 0;
    }

    /* All coefficients must be 0 or 1 */
    if (!model->A) return 0;
    SparseMatrix *A = model->A;
    for (int p = 0; p < A->colptr[model->num_vars]; p++) {
        double v = A->values[p];
        if (fabs(v) > RALPH_ZERO_TOL && fabs(v - 1.0) > RALPH_ZERO_TOL) {
            return 0;
        }
    }

    return 1;
}

/*
 * Build coverage data structure for SCP heuristics.
 * Returns arrays indicating which elements each set covers and vice versa.
 */
static int build_coverage_data(const LPModel *model,
                                int ***set_covers,      /* set_covers[j] = array of elements */
                                int **set_cover_count,  /* set_cover_count[j] = count */
                                int ***element_sets,    /* element_sets[i] = array of sets */
                                int **element_set_count) {
    int n = model->num_vars;
    int m = model->num_cons;
    SparseMatrix *A = model->A;

    /* Allocate arrays */
    *set_covers = (int **)calloc(n, sizeof(int *));
    *set_cover_count = (int *)calloc(n, sizeof(int));
    *element_sets = (int **)calloc(m, sizeof(int *));
    *element_set_count = (int *)calloc(m, sizeof(int));

    if (!*set_covers || !*set_cover_count || !*element_sets || !*element_set_count) {
        free(*set_covers); free(*set_cover_count);
        free(*element_sets); free(*element_set_count);
        return -1;
    }

    /* Count coverage from sparse matrix */
    for (int j = 0; j < n; j++) {
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            if (fabs(A->values[p] - 1.0) < RALPH_ZERO_TOL) {
                (*set_cover_count)[j]++;
                int row = A->rowidx[p];
                if (row < m) {
                    (*element_set_count)[row]++;
                }
            }
        }
    }

    /* Allocate individual arrays */
    for (int j = 0; j < n; j++) {
        if ((*set_cover_count)[j] > 0) {
            (*set_covers)[j] = (int *)calloc((*set_cover_count)[j], sizeof(int));
            if (!(*set_covers)[j]) goto error;
        }
        (*set_cover_count)[j] = 0;  /* Reset for filling */
    }
    for (int i = 0; i < m; i++) {
        if ((*element_set_count)[i] > 0) {
            (*element_sets)[i] = (int *)calloc((*element_set_count)[i], sizeof(int));
            if (!(*element_sets)[i]) goto error;
        }
        (*element_set_count)[i] = 0;  /* Reset for filling */
    }

    /* Fill arrays */
    for (int j = 0; j < n; j++) {
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            if (fabs(A->values[p] - 1.0) < RALPH_ZERO_TOL) {
                int row = A->rowidx[p];
                if (row < m) {
                    (*set_covers)[j][(*set_cover_count)[j]++] = row;
                    (*element_sets)[row][(*element_set_count)[row]++] = j;
                }
            }
        }
    }

    return 0;

error:
    for (int j = 0; j < n; j++) free((*set_covers)[j]);
    for (int i = 0; i < m; i++) free((*element_sets)[i]);
    free(*set_covers); free(*set_cover_count);
    free(*element_sets); free(*element_set_count);
    return -1;
}

static void free_coverage_data(int n, int m, int **set_covers, int *set_cover_count,
                                int **element_sets, int *element_set_count) {
    if (set_covers) {
        for (int j = 0; j < n; j++) free(set_covers[j]);
        free(set_covers);
    }
    free(set_cover_count);
    if (element_sets) {
        for (int i = 0; i < m; i++) free(element_sets[i]);
        free(element_sets);
    }
    free(element_set_count);
}

/*
 * Greedy set cover heuristic with incremental coverage tracking.
 *
 * Optimization: Instead of recomputing coverage for all sets each iteration O(mn),
 * we maintain a coverage_count[] array and only update counts for sets that share
 * elements with the selected set. This reduces per-iteration cost from O(n) to
 * O(avg_sets_per_element * avg_elements_per_set).
 */
int heuristic_greedy_set_cover(MIPSolver *solver, double *solution) {
    if (!solver || !solution) return -1;

    LPModel *model = solver->original_model;
    if (!is_scp_model(model)) return -1;

    int n = model->num_vars;
    int m = model->num_cons;

    /* Build coverage data */
    int **set_covers, *set_cover_count;
    int **element_sets, *element_set_count;
    if (build_coverage_data(model, &set_covers, &set_cover_count,
                            &element_sets, &element_set_count) != 0) {
        return -1;
    }

    /* Initialize solution to all zeros */
    memset(solution, 0, n * sizeof(double));

    /* Track uncovered elements */
    int *uncovered = (int *)calloc(m, sizeof(int));
    if (!uncovered) {
        free_coverage_data(n, m, set_covers, set_cover_count, element_sets, element_set_count);
        return -1;
    }

    /* Track coverage count for each set (how many uncovered elements it would cover) */
    int *coverage_count = (int *)malloc(n * sizeof(int));
    if (!coverage_count) {
        free(uncovered);
        free_coverage_data(n, m, set_covers, set_cover_count, element_sets, element_set_count);
        return -1;
    }

    int num_uncovered = 0;
    for (int i = 0; i < m; i++) {
        /* Check RHS - need to cover at least b[i] times */
        uncovered[i] = (int)(model->b[i] + 0.5);  /* Usually 1 */
        num_uncovered += uncovered[i];
    }

    /* Initialize coverage counts - each set can cover all its elements initially */
    for (int j = 0; j < n; j++) {
        int count = 0;
        for (int k = 0; k < set_cover_count[j]; k++) {
            int elem = set_covers[j][k];
            count += uncovered[elem];
        }
        coverage_count[j] = count;
    }

    /* Greedy selection with incremental updates */
    while (num_uncovered > 0) {
        int best_set = -1;
        double best_ratio = RALPH_INFINITY;

        /* Find best cost/coverage ratio using cached coverage counts */
        for (int j = 0; j < n; j++) {
            if (solution[j] > 0.5) continue;  /* Already selected */

            int covers = coverage_count[j];
            if (covers > 0) {
                double ratio = model->c[j] / (double)covers;
                if (ratio < best_ratio) {
                    best_ratio = ratio;
                    best_set = j;
                }
            }
        }

        if (best_set < 0) {
            /* No set can cover remaining elements - infeasible */
            free(coverage_count);
            free(uncovered);
            free_coverage_data(n, m, set_covers, set_cover_count, element_sets, element_set_count);
            return -1;
        }

        /* Select best set */
        solution[best_set] = 1.0;
        coverage_count[best_set] = 0;  /* No longer contributes */

        /* Mark elements as covered and update affected sets' coverage counts */
        for (int k = 0; k < set_cover_count[best_set]; k++) {
            int elem = set_covers[best_set][k];
            if (uncovered[elem] > 0) {
                /* Decrement coverage count for ALL sets that cover this element */
                for (int s = 0; s < element_set_count[elem]; s++) {
                    int other_set = element_sets[elem][s];
                    if (solution[other_set] < 0.5 && coverage_count[other_set] > 0) {
                        coverage_count[other_set]--;
                    }
                }
                uncovered[elem]--;
                num_uncovered--;
            }
        }
    }

    free(coverage_count);
    free(uncovered);
    free_coverage_data(n, m, set_covers, set_cover_count, element_sets, element_set_count);
    return 0;
}

/*
 * LP-guided greedy heuristic with incremental coverage tracking.
 *
 * Same incremental optimization as heuristic_greedy_set_cover,
 * but biases selection toward sets with high LP relaxation values.
 */
int heuristic_lp_guided_greedy(MIPSolver *solver, const double *lp_solution, double *solution) {
    if (!solver || !solution) return -1;
    if (!lp_solution) {
        /* Fall back to pure greedy if no LP solution */
        return heuristic_greedy_set_cover(solver, solution);
    }

    LPModel *model = solver->original_model;
    if (!is_scp_model(model)) return -1;

    int n = model->num_vars;
    int m = model->num_cons;

    /* Build coverage data */
    int **set_covers, *set_cover_count;
    int **element_sets, *element_set_count;
    if (build_coverage_data(model, &set_covers, &set_cover_count,
                            &element_sets, &element_set_count) != 0) {
        return -1;
    }

    /* Initialize solution to all zeros */
    memset(solution, 0, n * sizeof(double));

    /* Track uncovered elements */
    int *uncovered = (int *)calloc(m, sizeof(int));
    if (!uncovered) {
        free_coverage_data(n, m, set_covers, set_cover_count, element_sets, element_set_count);
        return -1;
    }

    /* Track coverage count for each set (how many uncovered elements it would cover) */
    int *coverage_count = (int *)malloc(n * sizeof(int));
    if (!coverage_count) {
        free(uncovered);
        free_coverage_data(n, m, set_covers, set_cover_count, element_sets, element_set_count);
        return -1;
    }

    int num_uncovered = 0;
    for (int i = 0; i < m; i++) {
        uncovered[i] = (int)(model->b[i] + 0.5);
        num_uncovered += uncovered[i];
    }

    /* Initialize coverage counts */
    for (int j = 0; j < n; j++) {
        int count = 0;
        for (int k = 0; k < set_cover_count[j]; k++) {
            int elem = set_covers[j][k];
            count += uncovered[elem];
        }
        coverage_count[j] = count;
    }

    /* LP-guided greedy selection with incremental updates */
    while (num_uncovered > 0) {
        int best_set = -1;
        double best_ratio = RALPH_INFINITY;

        for (int j = 0; j < n; j++) {
            if (solution[j] > 0.5) continue;

            int covers = coverage_count[j];
            if (covers > 0) {
                /* LP-guided ratio: bias toward high LP values */
                double lp_boost = 1.0 + lp_solution[j];  /* Range [1, 2] */
                double ratio = model->c[j] / ((double)covers * lp_boost);
                if (ratio < best_ratio) {
                    best_ratio = ratio;
                    best_set = j;
                }
            }
        }

        if (best_set < 0) {
            free(coverage_count);
            free(uncovered);
            free_coverage_data(n, m, set_covers, set_cover_count, element_sets, element_set_count);
            return -1;
        }

        /* Select best set */
        solution[best_set] = 1.0;
        coverage_count[best_set] = 0;

        /* Mark elements as covered and update affected sets' coverage counts */
        for (int k = 0; k < set_cover_count[best_set]; k++) {
            int elem = set_covers[best_set][k];
            if (uncovered[elem] > 0) {
                /* Decrement coverage count for ALL sets that cover this element */
                for (int s = 0; s < element_set_count[elem]; s++) {
                    int other_set = element_sets[elem][s];
                    if (solution[other_set] < 0.5 && coverage_count[other_set] > 0) {
                        coverage_count[other_set]--;
                    }
                }
                uncovered[elem]--;
                num_uncovered--;
            }
        }
    }

    free(coverage_count);
    free(uncovered);
    free_coverage_data(n, m, set_covers, set_cover_count, element_sets, element_set_count);
    return 0;
}

/*
 * Check if solution remains feasible without a given set.
 */
static int is_feasible_without(const LPModel *model, const double *solution,
                                int exclude_set, int **set_covers, int *set_cover_count) {
    int m = model->num_cons;
    int n = model->num_vars;

    /* Compute coverage for each element without exclude_set */
    int *coverage = (int *)calloc(m, sizeof(int));
    if (!coverage) return 0;

    for (int j = 0; j < n; j++) {
        if (j == exclude_set) continue;
        if (solution[j] < 0.5) continue;

        for (int k = 0; k < set_cover_count[j]; k++) {
            int elem = set_covers[j][k];
            if (elem < m) coverage[elem]++;
        }
    }

    /* Check if all elements still covered */
    int feasible = 1;
    for (int i = 0; i < m; i++) {
        int required = (int)(model->b[i] + 0.5);
        if (coverage[i] < required) {
            feasible = 0;
            break;
        }
    }

    free(coverage);
    return feasible;
}

/*
 * Check if set k can replace set j in the solution.
 */
static int can_replace(const LPModel *model, const double *solution,
                        int j, int k, int **set_covers, int *set_cover_count) {
    int m = model->num_cons;
    int n = model->num_vars;

    /* Compute coverage with k instead of j */
    int *coverage = (int *)calloc(m, sizeof(int));
    if (!coverage) return 0;

    for (int s = 0; s < n; s++) {
        if (s == j) continue;  /* Exclude j */
        int is_selected = (s == k) || (solution[s] > 0.5);
        if (!is_selected) continue;

        for (int p = 0; p < set_cover_count[s]; p++) {
            int elem = set_covers[s][p];
            if (elem < m) coverage[elem]++;
        }
    }

    /* Check feasibility */
    int feasible = 1;
    for (int i = 0; i < m; i++) {
        int required = (int)(model->b[i] + 0.5);
        if (coverage[i] < required) {
            feasible = 0;
            break;
        }
    }

    free(coverage);
    return feasible;
}

/*
 * Local search improvement for SCP.
 */
int heuristic_local_search_scp(MIPSolver *solver, double *solution) {
    if (!solver || !solution) return -1;

    LPModel *model = solver->original_model;
    if (!is_scp_model(model)) return -1;

    int n = model->num_vars;
    int m = model->num_cons;

    /* Build coverage data */
    int **set_covers, *set_cover_count;
    int **element_sets, *element_set_count;
    if (build_coverage_data(model, &set_covers, &set_cover_count,
                            &element_sets, &element_set_count) != 0) {
        return -1;
    }

    int improvements = 0;
    int changed = 1;

    while (changed) {
        changed = 0;

        /* 1-opt: Try removing redundant sets */
        for (int j = 0; j < n; j++) {
            if (solution[j] < 0.5) continue;

            if (is_feasible_without(model, solution, j, set_covers, set_cover_count)) {
                solution[j] = 0.0;
                improvements++;
                changed = 1;
            }
        }

        /* 2-opt: Try replacing a set with a cheaper one */
        for (int j = 0; j < n; j++) {
            if (solution[j] < 0.5) continue;

            for (int k = 0; k < n; k++) {
                if (k == j) continue;
                if (solution[k] > 0.5) continue;
                if (model->c[k] >= model->c[j]) continue;  /* k must be cheaper */

                if (can_replace(model, solution, j, k, set_covers, set_cover_count)) {
                    solution[j] = 0.0;
                    solution[k] = 1.0;
                    improvements++;
                    changed = 1;
                    break;  /* Restart from this j */
                }
            }
            if (changed) break;
        }
    }

    free_coverage_data(n, m, set_covers, set_cover_count, element_sets, element_set_count);
    return improvements;
}

/*
 * Combined SCP heuristic.
 */
int heuristic_scp(MIPSolver *solver, const double *lp_solution, double *solution) {
    if (!solver || !solution) return -1;

    /* Try LP-guided greedy (or pure greedy if no LP solution) */
    int result;
    if (lp_solution) {
        result = heuristic_lp_guided_greedy(solver, lp_solution, solution);
    } else {
        result = heuristic_greedy_set_cover(solver, solution);
    }

    if (result != 0) return -1;

    /* Improve with local search */
    heuristic_local_search_scp(solver, solution);

    return 0;
}

/* ============================================================================
 * SCP-Specific Branching (Phase 5)
 * ============================================================================ */

/*
 * Initialize pseudo-costs for SCP using cost/coverage ratio.
 */
int init_pseudo_costs_scp(MIPSolver *solver) {
    if (!solver) return -1;

    LPModel *model = solver->original_model;
    if (!is_scp_model(model)) return -1;

    int n = model->num_vars;
    SparseMatrix *A = model->A;

    /* Compute set sizes (number of elements each set covers) */
    for (int j = 0; j < n; j++) {
        int set_size = 0;
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            if (fabs(A->values[p] - 1.0) < RALPH_ZERO_TOL) {
                set_size++;
            }
        }

        if (set_size > 0) {
            /* Cost per element: good estimate for branching impact */
            double cost_per_elem = model->c[j] / (double)set_size;
            solver->pseudo_cost_down[j] = cost_per_elem;
            solver->pseudo_cost_up[j] = cost_per_elem;
        } else {
            /* Empty set - use cost directly */
            solver->pseudo_cost_down[j] = model->c[j];
            solver->pseudo_cost_up[j] = model->c[j];
        }

        /* Mark as initialized */
        solver->pseudo_count_down[j] = 1;
        solver->pseudo_count_up[j] = 1;
    }

    return 0;
}

/*
 * Compute coverage of an element by current LP solution.
 * Returns sum of x_j for all sets j covering element i.
 */
static double compute_element_coverage(const LPModel *model, int element,
                                        const double *solution) {
    double coverage = 0.0;
    SparseMatrix *A = model->A;
    int n = model->num_vars;

    /* Scan columns for sets covering this element */
    for (int j = 0; j < n; j++) {
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            if (A->rowidx[p] == element && fabs(A->values[p] - 1.0) < RALPH_ZERO_TOL) {
                coverage += solution[j];
                break;
            }
        }
    }

    return coverage;
}

/*
 * Find the set with highest LP value among those covering an element.
 */
static int find_best_covering_set(const LPModel *model, int element,
                                   const double *solution) {
    int best_set = -1;
    double best_val = -1.0;
    SparseMatrix *A = model->A;
    int n = model->num_vars;

    for (int j = 0; j < n; j++) {
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            if (A->rowidx[p] == element && fabs(A->values[p] - 1.0) < RALPH_ZERO_TOL) {
                /* Set j covers element - check LP value and fractionality */
                double val = solution[j];
                double frac = val - floor(val);
                /* Prefer fractional variables with higher LP values */
                if (frac > RALPH_INT_TOL && frac < 1.0 - RALPH_INT_TOL) {
                    if (val > best_val) {
                        best_val = val;
                        best_set = j;
                    }
                }
                break;
            }
        }
    }

    return best_set;
}

/*
 * SCP constraint branching: branch on element with most fractional coverage.
 */
int select_scp_branch(MIPSolver *solver, const double *solution,
                      int *element, int *set) {
    if (!solver || !solution || !element || !set) return -1;

    LPModel *model = solver->original_model;
    if (!is_scp_model(model)) return -1;

    int m = model->num_cons;

    *element = -1;
    *set = -1;

    double max_frac = 0.0;
    int best_elem = -1;

    /* Find element with most fractional coverage */
    for (int i = 0; i < m; i++) {
        double coverage = compute_element_coverage(model, i, solution);
        double required = model->b[i];

        /* Fractionality: how far from being satisfied integrally */
        double frac = fabs(coverage - round(coverage));

        /* Also penalize under-coverage (coverage < required) */
        if (coverage < required - RALPH_ZERO_TOL) {
            frac += (required - coverage);  /* Boost priority */
        }

        if (frac > max_frac + RALPH_ZERO_TOL) {
            max_frac = frac;
            best_elem = i;
        }
    }

    if (best_elem < 0 || max_frac < RALPH_INT_TOL) {
        return -1;  /* No fractional element found */
    }

    *element = best_elem;

    /* Find best set to branch on for this element */
    *set = find_best_covering_set(model, best_elem, solution);

    if (*set < 0) {
        /* No fractional covering set found - find any covering set */
        SparseMatrix *A = model->A;
        int n = model->num_vars;
        for (int j = 0; j < n; j++) {
            for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
                if (A->rowidx[p] == best_elem && fabs(A->values[p] - 1.0) < RALPH_ZERO_TOL) {
                    double frac = solution[j] - floor(solution[j]);
                    if (frac > RALPH_INT_TOL && frac < 1.0 - RALPH_INT_TOL) {
                        *set = j;
                        return 0;
                    }
                    break;
                }
            }
        }
        return -1;  /* No valid branching variable */
    }

    return 0;
}

/*
 * SOS1 branching for set partitioning problems.
 *
 * For SPP, each element has exactly one covering set selected.
 * This partitions the covering sets and branches on the median.
 */
int select_sos1_branch_spp(MIPSolver *solver, const double *solution, int *set) {
    if (!solver || !solution || !set) return -1;

    LPModel *model = solver->original_model;
    if (!is_scp_model(model)) return -1;

    /* Check if this is set partitioning (all = constraints) */
    int m = model->num_cons;
    for (int i = 0; i < m; i++) {
        if (model->sense[i] != 'E') return -1;
    }

    int n = model->num_vars;
    SparseMatrix *A = model->A;

    *set = -1;
    double max_frac = 0.0;
    int best_elem = -1;

    /* Find element with most fractional coverage */
    for (int i = 0; i < m; i++) {
        double coverage = compute_element_coverage(model, i, solution);
        double frac = fabs(coverage - 1.0);  /* Should be exactly 1 for SPP */

        if (frac > max_frac + RALPH_ZERO_TOL && frac > RALPH_INT_TOL) {
            max_frac = frac;
            best_elem = i;
        }
    }

    if (best_elem < 0) return -1;

    /* Collect covering sets for this element */
    int *covering_sets = (int *)calloc(n, sizeof(int));
    double *lp_values = (double *)calloc(n, sizeof(double));
    if (!covering_sets || !lp_values) {
        free(covering_sets);
        free(lp_values);
        return -1;
    }

    int num_covering = 0;
    for (int j = 0; j < n; j++) {
        for (int p = A->colptr[j]; p < A->colptr[j + 1]; p++) {
            if (A->rowidx[p] == best_elem && fabs(A->values[p] - 1.0) < RALPH_ZERO_TOL) {
                covering_sets[num_covering] = j;
                lp_values[num_covering] = solution[j];
                num_covering++;
                break;
            }
        }
    }

    if (num_covering < 2) {
        free(covering_sets);
        free(lp_values);
        return -1;
    }

    /* Sort covering sets by LP value (descending) */
    for (int i = 0; i < num_covering - 1; i++) {
        for (int j = i + 1; j < num_covering; j++) {
            if (lp_values[j] > lp_values[i]) {
                double tmp_val = lp_values[i];
                lp_values[i] = lp_values[j];
                lp_values[j] = tmp_val;
                int tmp_set = covering_sets[i];
                covering_sets[i] = covering_sets[j];
                covering_sets[j] = tmp_set;
            }
        }
    }

    /* Find the set at the partition boundary (where cumulative LP ~ 0.5) */
    double cumsum = 0.0;
    int partition_idx = 0;
    for (int i = 0; i < num_covering; i++) {
        cumsum += lp_values[i];
        if (cumsum >= 0.5) {
            partition_idx = i;
            break;
        }
    }

    /* Branch on the set at the partition point */
    *set = covering_sets[partition_idx];

    /* Verify it's fractional */
    double frac = solution[*set] - floor(solution[*set]);
    if (frac < RALPH_INT_TOL || frac > 1.0 - RALPH_INT_TOL) {
        /* Not fractional - find next fractional one */
        for (int i = 0; i < num_covering; i++) {
            frac = solution[covering_sets[i]] - floor(solution[covering_sets[i]]);
            if (frac > RALPH_INT_TOL && frac < 1.0 - RALPH_INT_TOL) {
                *set = covering_sets[i];
                break;
            }
        }
    }

    free(covering_sets);
    free(lp_values);

    return (*set >= 0) ? 0 : -1;
}

/* ============================================================================
 * Utility
 * ============================================================================ */

void mip_print_node_info(const MIPSolver *solver, const BBNode *node) {
    (void)solver;  /* Reserved for future use (e.g., printing solver state) */
    printf("Node %d: depth=%d, bound=%.4f, status=%d\n",
           node->id, node->depth, node->lp_bound, node->lp_status);
}

/* ============================================================================
 * Lagrangian Relaxation for SCP (Phase 6)
 * ============================================================================
 * Implementation moved to src/optim/lagrangian_scp.c using the generic
 * Lagrangian framework from optim_lagrangian.h.
 *
 * The following functions are now provided by lagrangian_scp.c:
 *   - lagrangian_create()
 *   - lagrangian_free()
 *   - lagrangian_bound()
 *   - lagrangian_step()
 *   - lagrangian_optimize()
 *   - lagrangian_repair()
 *   - lagrangian_solve_scp()
 * ============================================================================ */
