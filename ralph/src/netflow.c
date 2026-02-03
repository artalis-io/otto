/*
 * netflow.c - Network Simplex Solver for Minimum Cost Network Flow
 *
 * Implementation of the network simplex algorithm with:
 * - Artificial arc method for Phase 1 (initial BFS)
 * - Candidate list pricing (Mulvey's method)
 * - Threaded tree representation for O(subtree) updates
 */

#include "netflow.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

/* Memory alignment for cache efficiency */
#define NETFLOW_ALIGNMENT 64

/* ============================================================================
 * Aligned Allocation Helpers
 * ============================================================================ */

static void* netflow_aligned_alloc(size_t size) {
#ifdef _WIN32
    return _aligned_malloc(size, NETFLOW_ALIGNMENT);
#else
    void *ptr = NULL;
    if (posix_memalign(&ptr, NETFLOW_ALIGNMENT, size) != 0) {
        return NULL;
    }
    return ptr;
#endif
}

static void netflow_aligned_free(void *ptr) {
#ifdef _WIN32
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

/* Align size to boundary */
static inline size_t align_size(size_t size) {
    return ((size + NETFLOW_ALIGNMENT - 1) / NETFLOW_ALIGNMENT) * NETFLOW_ALIGNMENT;
}

/* ============================================================================
 * Workspace Structure
 * ============================================================================ */

struct RalphNetflowWorkspace {
    int max_nodes;
    int max_arcs;

    /* Single memory block */
    void *memory_block;
    size_t block_size;

    /* Problem data (augmented with artificial arcs) */
    int *aug_tail;              /* Tail of augmented arcs [max_arcs + max_nodes] */
    int *aug_head;              /* Head of augmented arcs [max_arcs + max_nodes] */
    double *aug_cost;           /* Cost of augmented arcs [max_arcs + max_nodes] */
    double *aug_capacity;       /* Capacity of augmented arcs [max_arcs + max_nodes] */
    double *aug_lower;          /* Lower bound of augmented arcs [max_arcs + max_nodes] */

    /* Primal solution */
    double *flow;               /* Arc flows [max_arcs + max_nodes] */
    int *state;                 /* Arc state: AT_LOWER, BASIC, AT_UPPER [max_arcs + max_nodes] */

    /* Dual solution */
    double *potential;          /* Node potentials [max_nodes + 1] (includes root) */

    /* Spanning tree structure */
    int *parent;                /* parent[i] = parent node (-1 for root) [max_nodes + 1] */
    int *pred_arc;              /* pred_arc[i] = arc connecting i to parent [max_nodes + 1] */
    int *depth;                 /* depth[i] = tree depth (root = 0) [max_nodes + 1] */
    int *thread;                /* Preorder traversal: next node [max_nodes + 1] */
    int *size;                  /* Subtree size [max_nodes + 1] */

    /* Working arrays */
    int *cycle_nodes;           /* Nodes in cycle [max_nodes + 2] */
    int *cycle_arcs;            /* Arcs in cycle [max_nodes + 1] */
    int *cycle_dir;             /* Direction of flow on arc (+1/-1) [max_nodes + 1] */
    int *mark;                  /* Node marking [max_nodes + 1] */

    /* Candidate list for pricing */
    int *candidates;            /* Candidate arcs [max_arcs + max_nodes] */
    int num_candidates;
    int next_arc;               /* For round-robin scanning */
    int64_t pivots_since_rebuild;

    /* Statistics */
    int64_t total_pivots;
    int64_t degenerate_pivots;

    /* Warm start data */
    int warm_start_valid;           /* 1 if warm start data is valid */
    int warm_start_nodes;           /* Number of nodes in warm start */
    int warm_start_arcs;            /* Number of arcs in warm start */
};

/* ============================================================================
 * Workspace Management
 * ============================================================================ */

size_t ralph_netflow_workspace_size(int max_nodes, int max_arcs) {
    if (max_nodes <= 0 || max_arcs < 0) {
        return 0;
    }

    /* Check against maximum bounds (DoS protection) */
    if (max_nodes > RALPH_NETFLOW_MAX_NODES || max_arcs > RALPH_NETFLOW_MAX_ARCS) {
        return 0;
    }

    /* Check for overflow: max_arcs + max_nodes */
    if ((size_t)max_arcs > SIZE_MAX - (size_t)max_nodes) {
        return 0;
    }

    size_t n = (size_t)max_nodes + 1;       /* +1 for artificial root */
    size_t m = (size_t)max_arcs + (size_t)max_nodes;  /* +num_nodes artificial arcs */

    /* Check for overflow in array size calculations */
    if (m > SIZE_MAX / sizeof(double)) {
        return 0;
    }
    if (n > SIZE_MAX / sizeof(double)) {
        return 0;
    }

    /* Problem data */
    size_t aug_tail_size = align_size(m * sizeof(int));
    size_t aug_head_size = align_size(m * sizeof(int));
    size_t aug_cost_size = align_size(m * sizeof(double));
    size_t aug_capacity_size = align_size(m * sizeof(double));
    size_t aug_lower_size = align_size(m * sizeof(double));

    /* Primal */
    size_t flow_size = align_size(m * sizeof(double));
    size_t state_size = align_size(m * sizeof(int));

    /* Dual */
    size_t potential_size = align_size(n * sizeof(double));

    /* Tree structure */
    size_t parent_size = align_size(n * sizeof(int));
    size_t pred_arc_size = align_size(n * sizeof(int));
    size_t depth_size = align_size(n * sizeof(int));
    size_t thread_size = align_size(n * sizeof(int));
    size_t subtree_size = align_size(n * sizeof(int));

    /* Working arrays */
    size_t cycle_nodes_size = align_size((n + 1) * sizeof(int));
    size_t cycle_arcs_size = align_size(n * sizeof(int));
    size_t cycle_dir_size = align_size(n * sizeof(int));
    size_t mark_size = align_size(n * sizeof(int));

    /* Candidate list */
    size_t candidates_size = align_size(m * sizeof(int));

    return aug_tail_size + aug_head_size + aug_cost_size + aug_capacity_size +
           aug_lower_size + flow_size + state_size + potential_size +
           parent_size + pred_arc_size + depth_size + thread_size + subtree_size +
           cycle_nodes_size + cycle_arcs_size + cycle_dir_size + mark_size +
           candidates_size;
}

RalphNetflowWorkspace* ralph_netflow_workspace_create(int max_nodes, int max_arcs) {
    if (max_nodes <= 0 || max_arcs < 0) {
        return NULL;
    }

    /* Check against maximum bounds (DoS protection) */
    if (max_nodes > RALPH_NETFLOW_MAX_NODES || max_arcs > RALPH_NETFLOW_MAX_ARCS) {
        return NULL;
    }

    /* Get required size (includes overflow checks) */
    size_t block_size = ralph_netflow_workspace_size(max_nodes, max_arcs);
    if (block_size == 0) {
        return NULL;  /* Overflow or invalid input */
    }

    RalphNetflowWorkspace *ws = (RalphNetflowWorkspace *)malloc(sizeof(RalphNetflowWorkspace));
    if (!ws) {
        return NULL;
    }

    ws->max_nodes = max_nodes;
    ws->max_arcs = max_arcs;

    ws->block_size = block_size;
    ws->memory_block = netflow_aligned_alloc(ws->block_size);
    if (!ws->memory_block) {
        free(ws);
        return NULL;
    }

    /* Set up pointers */
    size_t n = (size_t)max_nodes + 1;
    size_t m = (size_t)max_arcs + (size_t)max_nodes;

    char *ptr = (char *)ws->memory_block;

    ws->aug_tail = (int *)ptr;        ptr += align_size(m * sizeof(int));
    ws->aug_head = (int *)ptr;        ptr += align_size(m * sizeof(int));
    ws->aug_cost = (double *)ptr;     ptr += align_size(m * sizeof(double));
    ws->aug_capacity = (double *)ptr; ptr += align_size(m * sizeof(double));
    ws->aug_lower = (double *)ptr;    ptr += align_size(m * sizeof(double));

    ws->flow = (double *)ptr;         ptr += align_size(m * sizeof(double));
    ws->state = (int *)ptr;           ptr += align_size(m * sizeof(int));

    ws->potential = (double *)ptr;    ptr += align_size(n * sizeof(double));

    ws->parent = (int *)ptr;          ptr += align_size(n * sizeof(int));
    ws->pred_arc = (int *)ptr;        ptr += align_size(n * sizeof(int));
    ws->depth = (int *)ptr;           ptr += align_size(n * sizeof(int));
    ws->thread = (int *)ptr;          ptr += align_size(n * sizeof(int));
    ws->size = (int *)ptr;            ptr += align_size(n * sizeof(int));

    ws->cycle_nodes = (int *)ptr;     ptr += align_size((n + 1) * sizeof(int));
    ws->cycle_arcs = (int *)ptr;      ptr += align_size(n * sizeof(int));
    ws->cycle_dir = (int *)ptr;       ptr += align_size(n * sizeof(int));
    ws->mark = (int *)ptr;            ptr += align_size(n * sizeof(int));

    ws->candidates = (int *)ptr;

    ws->num_candidates = 0;
    ws->next_arc = 0;
    ws->pivots_since_rebuild = 0;
    ws->total_pivots = 0;
    ws->degenerate_pivots = 0;

    /* Initialize warm start fields */
    ws->warm_start_valid = 0;
    ws->warm_start_nodes = 0;
    ws->warm_start_arcs = 0;

    return ws;
}

void ralph_netflow_workspace_free(RalphNetflowWorkspace *ws) {
    if (ws) {
        netflow_aligned_free(ws->memory_block);
        free(ws);
    }
}

int ralph_netflow_workspace_max_nodes(const RalphNetflowWorkspace *ws) {
    return ws ? ws->max_nodes : 0;
}

int ralph_netflow_workspace_max_arcs(const RalphNetflowWorkspace *ws) {
    return ws ? ws->max_arcs : 0;
}

/* ============================================================================
 * Warm Start API
 * ============================================================================ */

RalphNetflowStatus ralph_netflow_warm_start(
    RalphNetflowWorkspace *ws,
    int num_nodes,
    int num_arcs,
    const double *potential,
    const double *flow,
    const int *arc_state
) {
    if (!ws || !potential) {
        return RALPH_NETFLOW_INVALID_INPUT;
    }

    if (num_nodes <= 0 || num_arcs < 0) {
        return RALPH_NETFLOW_INVALID_INPUT;
    }

    if (num_nodes > ws->max_nodes || num_arcs > ws->max_arcs) {
        return RALPH_NETFLOW_INVALID_INPUT;
    }

    /* Store node potentials */
    memcpy(ws->potential, potential, num_nodes * sizeof(double));

    /* Store flow if provided */
    if (flow) {
        memcpy(ws->flow, flow, num_arcs * sizeof(double));
    }

    /* Store arc states if provided */
    if (arc_state) {
        memcpy(ws->state, arc_state, num_arcs * sizeof(int));
    }

    /* Mark warm start as valid */
    ws->warm_start_valid = 1;
    ws->warm_start_nodes = num_nodes;
    ws->warm_start_arcs = num_arcs;

    return RALPH_NETFLOW_OPTIMAL;
}

void ralph_netflow_warm_start_clear(RalphNetflowWorkspace *ws) {
    if (ws) {
        ws->warm_start_valid = 0;
        ws->warm_start_nodes = 0;
        ws->warm_start_arcs = 0;
    }
}

int ralph_netflow_warm_start_valid(const RalphNetflowWorkspace *ws, int num_nodes, int num_arcs) {
    if (!ws || !ws->warm_start_valid) {
        return 0;
    }

    /* Warm start is only valid if dimensions match */
    return (ws->warm_start_nodes == num_nodes && ws->warm_start_arcs == num_arcs);
}

/* ============================================================================
 * Status String
 * ============================================================================ */

const char* ralph_netflow_status_string(RalphNetflowStatus status) {
    switch (status) {
        case RALPH_NETFLOW_OPTIMAL:        return "Optimal";
        case RALPH_NETFLOW_INFEASIBLE:     return "Infeasible";
        case RALPH_NETFLOW_UNBOUNDED:      return "Unbounded";
        case RALPH_NETFLOW_MAX_ITERATIONS: return "Max iterations reached";
        case RALPH_NETFLOW_INVALID_INPUT:  return "Invalid input";
        case RALPH_NETFLOW_OUT_OF_MEMORY:  return "Out of memory";
        default:                           return "Unknown status";
    }
}

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

/* Check for infinity */
static inline int is_infinite(double val) {
    return val >= RALPH_NETFLOW_INFINITY * RALPH_NETFLOW_INF_THRESHOLD;
}

/* Check for invalid floating point values */
static inline int is_valid_double(double val) {
    return !isnan(val) && !isinf(val);
}

/* Tolerance-based comparisons */
static inline int approx_zero(double val) {
    return fabs(val) < RALPH_NETFLOW_TOLERANCE;
}

static inline int approx_negative(double val) {
    return val < -RALPH_NETFLOW_TOLERANCE;
}

static inline int approx_positive(double val) {
    return val > RALPH_NETFLOW_TOLERANCE;
}

/* ============================================================================
 * Tree Operations
 * ============================================================================ */

/*
 * Compute reduced cost for an arc.
 * rc[a] = cost[a] - potential[tail[a]] + potential[head[a]]
 */
static inline double reduced_cost(const RalphNetflowWorkspace *ws, int arc) {
    int t = ws->aug_tail[arc];
    int h = ws->aug_head[arc];
    return ws->aug_cost[arc] - ws->potential[t] + ws->potential[h];
}

/*
 * Build initial tree structure using DFS from root.
 */
static void build_thread_order(RalphNetflowWorkspace *ws, int num_nodes) {
    int root = num_nodes;

    /* Simple initial structure: root has all real nodes as children */
    int prev = root;
    for (int i = 0; i < num_nodes; i++) {
        ws->thread[prev] = i;
        prev = i;
        ws->size[i] = 1;
    }
    ws->thread[prev] = root;  /* Back to root */
    ws->size[root] = num_nodes + 1;
}

/*
 * Find the LCA and build the cycle when entering_arc is added to the tree.
 * Returns cycle length.
 */
static int find_cycle(
    RalphNetflowWorkspace *ws,
    int entering_arc,
    int num_nodes,
    int *lca
) {
    int u = ws->aug_tail[entering_arc];
    int v = ws->aug_head[entering_arc];

    /* Clear marks */
    memset(ws->mark, 0, (num_nodes + 1) * sizeof(int));

    /* Mark path from u to root */
    for (int node = u; node >= 0; node = ws->parent[node]) {
        ws->mark[node] = 1;
    }

    /* Find LCA from v */
    int lca_node = v;
    while (ws->mark[lca_node] == 0) {
        lca_node = ws->parent[lca_node];
    }
    *lca = lca_node;

    /* Build cycle path
     *
     * The cycle is: u --entering_arc--> v --> ... --> LCA --> ... --> u
     *
     * When entering_dir = +1 (pushing flow from u to v on entering arc):
     * - For arcs on u->LCA path: we traverse FROM u TOWARD LCA
     *   - If arc goes node->parent (toward LCA): flow DECREASES (direction = -1)
     *   - If arc goes parent->node (away from LCA): flow INCREASES (direction = +1)
     * - For arcs on LCA->v path: we traverse FROM LCA TOWARD v
     *   - If arc goes parent->node (toward v): flow INCREASES (direction = +1)
     *   - If arc goes node->parent (away from v): flow DECREASES (direction = -1)
     */
    int len = 0;

    /* Path from u to LCA */
    for (int node = u; node != lca_node; node = ws->parent[node]) {
        ws->cycle_nodes[len] = node;
        ws->cycle_arcs[len] = ws->pred_arc[node];
        int arc = ws->pred_arc[node];
        int parent = ws->parent[node];
        /* If arc tail is current node: arc goes node->parent (toward LCA) */
        /* In cycle direction, this arc's flow DECREASES when entering arc increases */
        if (ws->aug_tail[arc] == node && ws->aug_head[arc] == parent) {
            ws->cycle_dir[len] = -1;  /* Flow decreases */
        } else {
            ws->cycle_dir[len] = +1;  /* Flow increases */
        }
        len++;
    }

    /* Path from v to LCA (we'll reverse it to get LCA->v) */
    int v_start = len;
    for (int node = v; node != lca_node; node = ws->parent[node]) {
        ws->cycle_nodes[len] = node;
        ws->cycle_arcs[len] = ws->pred_arc[node];
        int arc = ws->pred_arc[node];
        int parent = ws->parent[node];
        /* We're building v->LCA but will reverse to get LCA->v */
        /* After reversal, if arc originally went node->parent (toward LCA) */
        /* In LCA->v direction, that's opposite, so flow INCREASES */
        if (ws->aug_tail[arc] == node && ws->aug_head[arc] == parent) {
            ws->cycle_dir[len] = +1;  /* After reversal: flow increases */
        } else {
            ws->cycle_dir[len] = -1;  /* After reversal: flow decreases */
        }
        len++;
    }

    /* Reverse v-path portion */
    for (int i = v_start, j = len - 1; i < j; i++, j--) {
        int tmp_node = ws->cycle_nodes[i];
        int tmp_arc = ws->cycle_arcs[i];
        int tmp_dir = ws->cycle_dir[i];

        ws->cycle_nodes[i] = ws->cycle_nodes[j];
        ws->cycle_arcs[i] = ws->cycle_arcs[j];
        ws->cycle_dir[i] = ws->cycle_dir[j];

        ws->cycle_nodes[j] = tmp_node;
        ws->cycle_arcs[j] = tmp_arc;
        ws->cycle_dir[j] = tmp_dir;
    }

    return len;
}

/*
 * Ratio test: find the maximum flow change and leaving arc.
 *
 * entering_dir: +1 if we push flow from tail to head of entering arc
 *
 * Returns leaving arc index (or entering_arc if it blocks itself).
 */
static int ratio_test(
    RalphNetflowWorkspace *ws,
    int entering_arc,
    int entering_dir,
    int cycle_len,
    double *delta_flow,
    int *leaving_at_lower
) {
    double min_delta = RALPH_NETFLOW_INFINITY;
    int leaving_arc = entering_arc;
    int at_lower = 1;

    /* Check entering arc limit */
    if (entering_dir > 0) {
        /* Increasing flow: limited by capacity - flow */
        double delta = ws->aug_capacity[entering_arc] - ws->flow[entering_arc];
        if (delta < min_delta) {
            min_delta = delta;
            leaving_arc = entering_arc;
            at_lower = 0;  /* Will be at upper bound */
        }
    } else {
        /* Decreasing flow: limited by flow - lower */
        double delta = ws->flow[entering_arc] - ws->aug_lower[entering_arc];
        if (delta < min_delta) {
            min_delta = delta;
            leaving_arc = entering_arc;
            at_lower = 1;  /* Will be at lower bound */
        }
    }

    /* Check tree arcs */
    for (int i = 0; i < cycle_len; i++) {
        int arc = ws->cycle_arcs[i];
        int dir = ws->cycle_dir[i] * entering_dir;
        double delta;

        if (dir > 0) {
            /* Flow increases: limited by capacity */
            delta = ws->aug_capacity[arc] - ws->flow[arc];
        } else {
            /* Flow decreases: limited by lower bound */
            delta = ws->flow[arc] - ws->aug_lower[arc];
        }

        if (delta < min_delta || (approx_zero(delta - min_delta) && leaving_arc == entering_arc)) {
            min_delta = delta;
            leaving_arc = arc;
            at_lower = (dir < 0);  /* If decreasing, will be at lower */
        }
    }

    *delta_flow = min_delta;
    *leaving_at_lower = at_lower;
    return leaving_arc;
}

/*
 * Update flows along the cycle.
 */
static void update_flows(
    RalphNetflowWorkspace *ws,
    int entering_arc,
    int entering_dir,
    int cycle_len,
    double delta
) {
    /* Update entering arc */
    ws->flow[entering_arc] += entering_dir * delta;

    /* Update tree arcs */
    for (int i = 0; i < cycle_len; i++) {
        int arc = ws->cycle_arcs[i];
        int dir = ws->cycle_dir[i] * entering_dir;
        ws->flow[arc] += dir * delta;
    }
}

/*
 * Update the tree structure after pivot.
 * This is the complex part - we need to:
 * 1. Remove leaving arc from tree
 * 2. Add entering arc to tree
 * 3. Reroot the moved subtree
 * 4. Update depths and thread order
 */
static void update_tree(
    RalphNetflowWorkspace *ws,
    int entering_arc,
    int leaving_arc,
    int num_nodes
) {
    if (entering_arc == leaving_arc) {
        /* No tree change, just state change */
        return;
    }

    int root = num_nodes;

    /* Find the endpoints */
    int u = ws->aug_tail[entering_arc];
    int v = ws->aug_head[entering_arc];

    /* Find the child of the leaving arc (the one further from root) */
    int leaving_u = ws->aug_tail[leaving_arc];
    int leaving_v = ws->aug_head[leaving_arc];

    int subtree_root;  /* Root of the subtree being moved */
    if (ws->depth[leaving_u] > ws->depth[leaving_v]) {
        subtree_root = leaving_u;
    } else {
        subtree_root = leaving_v;
    }

    /* Determine which endpoint of entering arc is in the moving subtree */
    int new_parent, new_child;
    int node = u;
    int u_in_subtree = 0;
    while (node >= 0 && node <= root) {
        if (node == subtree_root) {
            u_in_subtree = 1;
            break;
        }
        node = ws->parent[node];
    }

    if (u_in_subtree) {
        new_child = u;
        new_parent = v;
    } else {
        new_child = v;
        new_parent = u;
    }

    /* Reverse the path from new_child to subtree_root */
    /* This makes new_child the new root of the subtree, connected to new_parent */

    int current = new_child;
    int prev_node = new_parent;
    int prev_arc = entering_arc;

    while (current != subtree_root) {
        int next_node = ws->parent[current];
        int next_arc = ws->pred_arc[current];

        ws->parent[current] = prev_node;
        ws->pred_arc[current] = prev_arc;

        prev_node = current;
        prev_arc = next_arc;
        current = next_node;
    }

    /* Connect subtree_root to prev_node */
    ws->parent[subtree_root] = prev_node;
    ws->pred_arc[subtree_root] = prev_arc;

    /* Recalculate depths for moved subtree
     *
     * Note: Without maintaining children pointers, we use an iterative approach
     * that is O(n * subtree_height) in worst case. For most practical networks,
     * the tree is relatively balanced and this is efficient. A fully O(n)
     * solution would require maintaining bidirectional tree links.
     */
    ws->depth[new_child] = ws->depth[new_parent] + 1;

    /* Mark nodes that have been updated */
    memset(ws->mark, 0, ((size_t)num_nodes + 1) * sizeof(int));
    ws->mark[new_child] = 1;
    ws->mark[root] = 1;  /* Root never changes */

    /* Propagate depth updates through the subtree */
    int remaining = num_nodes;  /* Upper bound on unprocessed nodes */
    while (remaining > 0) {
        int updated = 0;
        for (int i = 0; i <= num_nodes; i++) {
            if (ws->mark[i]) continue;  /* Already processed */
            int p = ws->parent[i];
            if (p >= 0 && ws->mark[p]) {
                /* Parent is processed, so we can update this node */
                ws->depth[i] = ws->depth[p] + 1;
                ws->mark[i] = 1;
                updated++;
            }
        }
        if (updated == 0) break;  /* No progress = all reachable nodes done */
        remaining -= updated;
    }

    /* Update arc states */
    ws->state[entering_arc] = RALPH_NETFLOW_BASIC;
    if (approx_zero(ws->flow[leaving_arc] - ws->aug_lower[leaving_arc])) {
        ws->state[leaving_arc] = RALPH_NETFLOW_AT_LOWER;
    } else {
        ws->state[leaving_arc] = RALPH_NETFLOW_AT_UPPER;
    }
}

/*
 * Recalculate all node potentials from scratch using tree structure.
 */
static void recalculate_potentials(RalphNetflowWorkspace *ws, int num_nodes) {
    int root = num_nodes;
    ws->potential[root] = 0.0;

    /* Iterate until all potentials are set */
    /* Mark which are done */
    memset(ws->mark, 0, (num_nodes + 1) * sizeof(int));
    ws->mark[root] = 1;

    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < num_nodes; i++) {
            if (ws->mark[i]) continue;

            int p = ws->parent[i];
            if (p < 0 || !ws->mark[p]) continue;

            /* Calculate potential based on parent and connecting arc */
            int arc = ws->pred_arc[i];

            /* For basic arc: cost - pot[tail] + pot[head] = 0 */
            /* So: if arc goes i->p: pot[i] = pot[p] + cost */
            /* If arc goes p->i: pot[i] = pot[p] - cost */
            if (ws->aug_tail[arc] == i) {
                /* Arc goes i -> p */
                ws->potential[i] = ws->potential[p] + ws->aug_cost[arc];
            } else {
                /* Arc goes p -> i */
                ws->potential[i] = ws->potential[p] - ws->aug_cost[arc];
            }

            ws->mark[i] = 1;
            changed = 1;
        }
    }
}

/* ============================================================================
 * Pricing Strategies
 * ============================================================================ */

/*
 * Rebuild candidate list by scanning all non-basic arcs.
 */
static void rebuild_candidate_list(
    RalphNetflowWorkspace *ws,
    int num_arcs_total,
    int list_size
) {
    ws->num_candidates = 0;

    for (int a = 0; a < num_arcs_total; a++) {
        if (ws->state[a] == RALPH_NETFLOW_BASIC) continue;

        double rc = reduced_cost(ws, a);

        int eligible = 0;
        if (ws->state[a] == RALPH_NETFLOW_AT_LOWER && approx_negative(rc)) {
            eligible = 1;
        } else if (ws->state[a] == RALPH_NETFLOW_AT_UPPER && approx_positive(rc)) {
            eligible = 1;
        }

        if (eligible) {
            ws->candidates[ws->num_candidates++] = a;
            if (ws->num_candidates >= list_size) {
                break;
            }
        }
    }

    ws->pivots_since_rebuild = 0;
    ws->next_arc = 0;
}

/*
 * Find entering arc using candidate list pricing.
 * Returns arc index, or -1 if optimal.
 */
/*
 * Find entering arc from candidate list with epsilon-optimal pricing.
 * Only returns arcs with |reduced_cost| > epsilon.
 */
static int find_entering_arc_candidate_list(
    RalphNetflowWorkspace *ws,
    int *entering_dir,
    double epsilon
) {
    double best_violation = epsilon;  /* Only accept violations > epsilon */
    int best_arc = -1;
    int best_dir = 0;

    int i = 0;
    while (i < ws->num_candidates) {
        int a = ws->candidates[i];

        if (ws->state[a] == RALPH_NETFLOW_BASIC) {
            ws->candidates[i] = ws->candidates[--ws->num_candidates];
            continue;
        }

        double rc = reduced_cost(ws, a);
        double violation = 0;
        int dir = 0;

        if (ws->state[a] == RALPH_NETFLOW_AT_LOWER && rc < -epsilon) {
            violation = -rc;
            dir = +1;
        } else if (ws->state[a] == RALPH_NETFLOW_AT_UPPER && rc > epsilon) {
            violation = rc;
            dir = -1;
        } else {
            /* Arc is epsilon-optimal, remove from candidates */
            if (fabs(rc) <= epsilon) {
                ws->candidates[i] = ws->candidates[--ws->num_candidates];
                continue;
            }
            i++;
            continue;
        }

        if (violation > best_violation) {
            best_violation = violation;
            best_arc = a;
            best_dir = dir;
        }

        i++;
    }

    if (best_arc >= 0) {
        *entering_dir = best_dir;
    }

    return best_arc;
}

/*
 * Find entering arc using first eligible rule.
 */
/*
 * Find first eligible entering arc with epsilon-optimal pricing.
 * Only returns arcs with |reduced_cost| > epsilon.
 */
static int find_entering_arc_first_eligible(
    RalphNetflowWorkspace *ws,
    int num_arcs_total,
    int *entering_dir,
    double epsilon
) {
    for (int a = ws->next_arc; a < num_arcs_total; a++) {
        if (ws->state[a] == RALPH_NETFLOW_BASIC) continue;

        double rc = reduced_cost(ws, a);

        if (ws->state[a] == RALPH_NETFLOW_AT_LOWER && rc < -epsilon) {
            *entering_dir = +1;
            ws->next_arc = a + 1;
            return a;
        }
        if (ws->state[a] == RALPH_NETFLOW_AT_UPPER && rc > epsilon) {
            *entering_dir = -1;
            ws->next_arc = a + 1;
            return a;
        }
    }

    for (int a = 0; a < ws->next_arc; a++) {
        if (ws->state[a] == RALPH_NETFLOW_BASIC) continue;

        double rc = reduced_cost(ws, a);

        if (ws->state[a] == RALPH_NETFLOW_AT_LOWER && rc < -epsilon) {
            *entering_dir = +1;
            ws->next_arc = a + 1;
            return a;
        }
        if (ws->state[a] == RALPH_NETFLOW_AT_UPPER && rc > epsilon) {
            *entering_dir = -1;
            ws->next_arc = a + 1;
            return a;
        }
    }

    return -1;
}

/* ============================================================================
 * Initial Basic Feasible Solution
 * ============================================================================ */

static RalphNetflowStatus setup_initial_solution(
    RalphNetflowWorkspace *ws,
    const RalphNetflowProblem *problem,
    double cost_multiplier
) {
    int num_nodes = problem->num_nodes;
    int num_arcs = problem->num_arcs;
    int root = num_nodes;

    /* Copy and transform problem data */
    for (int a = 0; a < num_arcs; a++) {
        ws->aug_tail[a] = problem->tail[a];
        ws->aug_head[a] = problem->head[a];
        ws->aug_cost[a] = problem->cost[a] * cost_multiplier;
        ws->aug_capacity[a] = problem->capacity ? problem->capacity[a] : RALPH_NETFLOW_INFINITY;
        ws->aug_lower[a] = problem->lower ? problem->lower[a] : 0.0;
        ws->flow[a] = ws->aug_lower[a];
        ws->state[a] = RALPH_NETFLOW_AT_LOWER;
    }

    /* Compute residual supply (after accounting for lower bounds) */
    /* Check for overflow before allocation */
    if ((size_t)num_nodes > (SIZE_MAX / sizeof(double)) - 1) {
        return RALPH_NETFLOW_OUT_OF_MEMORY;
    }
    double *residual = (double *)malloc(((size_t)num_nodes + 1) * sizeof(double));
    if (!residual) return RALPH_NETFLOW_OUT_OF_MEMORY;

    for (int i = 0; i < num_nodes; i++) {
        residual[i] = problem->supply[i];
    }

    for (int a = 0; a < num_arcs; a++) {
        double low = ws->aug_lower[a];
        if (low > RALPH_NETFLOW_TOLERANCE) {
            residual[ws->aug_tail[a]] -= low;
            residual[ws->aug_head[a]] += low;
        }
    }

    /* Create artificial arcs */
    for (int i = 0; i < num_nodes; i++) {
        int a = num_arcs + i;
        double sup = residual[i];

        if (sup >= -RALPH_NETFLOW_TOLERANCE) {
            /* Supply >= 0: arc i -> root */
            ws->aug_tail[a] = i;
            ws->aug_head[a] = root;
            ws->aug_cost[a] = RALPH_NETFLOW_BIG_M;
            ws->aug_capacity[a] = RALPH_NETFLOW_INFINITY;
            ws->aug_lower[a] = 0.0;
            ws->flow[a] = (sup > 0) ? sup : 0.0;
        } else {
            /* Supply < 0 (demand): arc root -> i */
            ws->aug_tail[a] = root;
            ws->aug_head[a] = i;
            ws->aug_cost[a] = RALPH_NETFLOW_BIG_M;
            ws->aug_capacity[a] = RALPH_NETFLOW_INFINITY;
            ws->aug_lower[a] = 0.0;
            ws->flow[a] = -sup;
        }
        ws->state[a] = RALPH_NETFLOW_BASIC;
    }

    free(residual);

    /* Initialize tree: root is tree root, all nodes are direct children */
    ws->parent[root] = -1;
    ws->pred_arc[root] = -1;
    ws->depth[root] = 0;

    for (int i = 0; i < num_nodes; i++) {
        ws->parent[i] = root;
        ws->pred_arc[i] = num_arcs + i;
        ws->depth[i] = 1;
    }

    build_thread_order(ws, num_nodes);

    /* Initialize potentials */
    ws->potential[root] = 0.0;
    for (int i = 0; i < num_nodes; i++) {
        int a = num_arcs + i;
        /* rc = cost - pot[tail] + pot[head] = 0 for basic arc */
        if (ws->aug_tail[a] == i) {
            /* Arc i -> root */
            ws->potential[i] = ws->aug_cost[a] + ws->potential[root];
        } else {
            /* Arc root -> i */
            ws->potential[i] = ws->potential[root] - ws->aug_cost[a];
        }
    }

    return RALPH_NETFLOW_OPTIMAL;
}

/* ============================================================================
 * Warm Start Solution Setup
 *
 * Reuses the basis (tree structure) from a previous solve to skip Phase 1.
 * Requires: same graph structure (arcs), same supply/demand.
 * Supports: different costs (potentials are recomputed).
 * ============================================================================ */

static RalphNetflowStatus setup_warm_start_solution(
    RalphNetflowWorkspace *ws,
    const RalphNetflowProblem *problem,
    double cost_multiplier
) {
    int num_nodes = problem->num_nodes;
    int num_arcs = problem->num_arcs;
    int root = num_nodes;

    /* Copy and transform problem data (costs may have changed) */
    for (int a = 0; a < num_arcs; a++) {
        ws->aug_tail[a] = problem->tail[a];
        ws->aug_head[a] = problem->head[a];
        ws->aug_cost[a] = problem->cost[a] * cost_multiplier;
        ws->aug_capacity[a] = problem->capacity ? problem->capacity[a] : RALPH_NETFLOW_INFINITY;
        ws->aug_lower[a] = problem->lower ? problem->lower[a] : 0.0;
        /* flow[] and state[] are preserved from previous solve */
    }

    /* Create artificial arcs (needed for algorithm structure) */
    /* These should all be non-basic with zero flow in a warm start from feasible solution */
    for (int i = 0; i < num_nodes; i++) {
        int a = num_arcs + i;
        double sup = problem->supply[i];

        if (sup >= -RALPH_NETFLOW_TOLERANCE) {
            /* Supply >= 0: arc i -> root */
            ws->aug_tail[a] = i;
            ws->aug_head[a] = root;
        } else {
            /* Supply < 0 (demand): arc root -> i */
            ws->aug_tail[a] = root;
            ws->aug_head[a] = i;
        }
        ws->aug_cost[a] = RALPH_NETFLOW_BIG_M;
        ws->aug_capacity[a] = RALPH_NETFLOW_INFINITY;
        ws->aug_lower[a] = 0.0;
        /* flow[a] should be 0 from previous feasible solution */
        /* state[a] preserved from previous solve (may be BASIC in degenerate case) */
    }

    /* Reconstruct tree root */
    ws->parent[root] = -1;
    ws->pred_arc[root] = -1;
    ws->depth[root] = 0;

    /* Recalculate depth for all nodes using iterative propagation
     * (parent[] and pred_arc[] are preserved from previous solve) */
    memset(ws->mark, 0, ((size_t)num_nodes + 1) * sizeof(int));
    ws->mark[root] = 1;

    int remaining = num_nodes;
    while (remaining > 0) {
        int updated = 0;
        for (int i = 0; i < num_nodes; i++) {
            if (ws->mark[i]) continue;
            int p = ws->parent[i];
            if (p >= 0 && ws->mark[p]) {
                ws->depth[i] = ws->depth[p] + 1;
                ws->mark[i] = 1;
                updated++;
            }
        }
        if (updated == 0) break;
        remaining -= updated;
    }

    /* Rebuild thread order (simple linear order, not optimized) */
    build_thread_order(ws, num_nodes);

    /* Recompute potentials for new costs */
    recalculate_potentials(ws, num_nodes);

    return RALPH_NETFLOW_OPTIMAL;
}

/* ============================================================================
 * Main Solver
 * ============================================================================ */

static RalphNetflowStatus netflow_solve_internal(
    const RalphNetflowProblem *problem,
    const RalphNetflowOptions *options,
    RalphNetflowResult *result,
    RalphNetflowWorkspace *ws
) {
    int num_nodes = problem->num_nodes;
    int num_arcs = problem->num_arcs;
    int num_arcs_total = num_arcs + num_nodes;

    double cost_multiplier = (problem->objective == RALPH_NETFLOW_MAXIMIZE) ? -1.0 : 1.0;

    /* Initialize: use warm start if requested and valid, otherwise cold start */
    RalphNetflowStatus status;
    if (options->warm_start &&
        ralph_netflow_warm_start_valid(ws, num_nodes, num_arcs)) {
        status = setup_warm_start_solution(ws, problem, cost_multiplier);
    } else {
        status = setup_initial_solution(ws, problem, cost_multiplier);
    }
    if (status != RALPH_NETFLOW_OPTIMAL) {
        result->status = status;
        return status;
    }

    /* Pricing setup based on options */
    int use_candidate_list = (options->pricing == RALPH_NETFLOW_PRICING_CANDIDATE);
    int list_size = num_arcs_total / RALPH_NETFLOW_LIST_FACTOR;
    if (list_size < RALPH_NETFLOW_MIN_LIST_SIZE) list_size = RALPH_NETFLOW_MIN_LIST_SIZE;
    if (list_size > RALPH_NETFLOW_MAX_LIST_SIZE) list_size = RALPH_NETFLOW_MAX_LIST_SIZE;
    int rebuild_freq = list_size * RALPH_NETFLOW_REBUILD_FACTOR;

    if (use_candidate_list) {
        rebuild_candidate_list(ws, num_arcs_total, list_size);
    }

    /* Iteration limit */
    int64_t max_iter = options->max_iterations;
    if (max_iter <= 0) {
        max_iter = (int64_t)num_nodes * (int64_t)num_arcs_total * RALPH_NETFLOW_ITER_MULTIPLIER;
        if (max_iter < RALPH_NETFLOW_MIN_ITERATIONS) max_iter = RALPH_NETFLOW_MIN_ITERATIONS;
    }

    /* Epsilon scaling setup */
    double epsilon = 0.0;  /* No scaling by default */
    double epsilon_factor = options->epsilon_factor;
    if (epsilon_factor <= 1.0) epsilon_factor = 4.0;  /* Default factor */

    if (options->cost_scaling) {
        /* Compute initial epsilon based on maximum cost */
        double max_cost = 0.0;
        for (int a = 0; a < num_arcs; a++) {
            double c = fabs(ws->aug_cost[a]);
            if (c > max_cost && c < RALPH_NETFLOW_BIG_M * 0.5) {
                max_cost = c;
            }
        }
        /* Start with epsilon = max_cost, will reduce by factor each phase */
        epsilon = max_cost;
        if (epsilon < RALPH_NETFLOW_TOLERANCE) {
            epsilon = 0.0;  /* All costs are zero, no scaling needed */
        }
    }

    /* Main loop with epsilon scaling */
    ws->total_pivots = 0;
    ws->degenerate_pivots = 0;

    int epsilon_phases = 0;
    const int max_epsilon_phases = 20;  /* Safety limit */

    while (ws->total_pivots < max_iter) {
        /* Find entering arc with epsilon-optimal pricing */
        int entering_dir;
        int entering_arc;

        if (use_candidate_list) {
            if (ws->num_candidates == 0 || ws->pivots_since_rebuild >= rebuild_freq) {
                rebuild_candidate_list(ws, num_arcs_total, list_size);
            }
            entering_arc = find_entering_arc_candidate_list(ws, &entering_dir, epsilon);

            if (entering_arc < 0 && ws->pivots_since_rebuild > 0) {
                rebuild_candidate_list(ws, num_arcs_total, list_size);
                entering_arc = find_entering_arc_candidate_list(ws, &entering_dir, epsilon);
            }
        } else {
            entering_arc = find_entering_arc_first_eligible(ws, num_arcs_total, &entering_dir, epsilon);
        }

        if (entering_arc < 0) {
            /* Epsilon-optimal - check if we need to reduce epsilon */
            if (epsilon > RALPH_NETFLOW_TOLERANCE && epsilon_phases < max_epsilon_phases) {
                epsilon /= epsilon_factor;
                if (epsilon < RALPH_NETFLOW_TOLERANCE) {
                    epsilon = 0.0;
                }
                epsilon_phases++;
                /* Rebuild candidate list for new epsilon */
                if (use_candidate_list) {
                    rebuild_candidate_list(ws, num_arcs_total, list_size);
                }
                ws->next_arc = 0;  /* Reset for first-eligible */
                continue;
            }
            /* Truly optimal */
            break;
        }

        /* Find cycle */
        int lca;
        int cycle_len = find_cycle(ws, entering_arc, num_nodes, &lca);

        /* Ratio test */
        double delta;
        int leaving_at_lower;
        int leaving_arc = ratio_test(ws, entering_arc, entering_dir, cycle_len, &delta, &leaving_at_lower);

        /* Check unbounded */
        if (is_infinite(delta)) {
            result->status = RALPH_NETFLOW_UNBOUNDED;
            return RALPH_NETFLOW_UNBOUNDED;
        }

        /* Update flows */
        update_flows(ws, entering_arc, entering_dir, cycle_len, delta);

        if (approx_zero(delta)) {
            ws->degenerate_pivots++;
        }

        /* Update tree */
        if (entering_arc == leaving_arc) {
            /* Entering arc blocks itself - just update state */
            if (leaving_at_lower) {
                ws->state[entering_arc] = RALPH_NETFLOW_AT_LOWER;
            } else {
                ws->state[entering_arc] = RALPH_NETFLOW_AT_UPPER;
            }
        } else {
            update_tree(ws, entering_arc, leaving_arc, num_nodes);
            recalculate_potentials(ws, num_nodes);
        }

        ws->total_pivots++;
        ws->pivots_since_rebuild++;
    }

    /* Check for infeasibility (artificial arcs with positive flow) */
    for (int i = 0; i < num_nodes; i++) {
        int a = num_arcs + i;
        if (ws->flow[a] > RALPH_NETFLOW_TOLERANCE) {
            result->status = RALPH_NETFLOW_INFEASIBLE;
            return RALPH_NETFLOW_INFEASIBLE;
        }
    }

    if (ws->total_pivots >= max_iter) {
        result->status = RALPH_NETFLOW_MAX_ITERATIONS;
        return RALPH_NETFLOW_MAX_ITERATIONS;
    }

    /* Copy solution */
    double obj = 0.0;
    for (int a = 0; a < num_arcs; a++) {
        result->flow[a] = ws->flow[a];
        obj += problem->cost[a] * ws->flow[a];
    }

    /* Handle single vs multiple objectives (for k-best compatibility) */
    if (result->objectives) {
        result->objectives[0] = obj;
    }
    result->objective = obj;
    result->num_found = 1;

    /* Copy dual variables if requested */
    if (result->potential) {
        for (int i = 0; i < num_nodes; i++) {
            result->potential[i] = ws->potential[i] * cost_multiplier;
        }
    }

    /* Copy arc states if requested (for warm start) */
    if (result->arc_state) {
        for (int a = 0; a < num_arcs; a++) {
            result->arc_state[a] = ws->state[a];
        }
    }

    /* Save warm start data if requested */
    if (options->save_warm_start) {
        ws->warm_start_valid = 1;
        ws->warm_start_nodes = num_nodes;
        ws->warm_start_arcs = num_arcs;
        /* flow, potential, and state are already in workspace */
    }

    result->iterations = ws->total_pivots;
    result->degenerate_pivots = ws->degenerate_pivots;
    result->status = RALPH_NETFLOW_OPTIMAL;

    return RALPH_NETFLOW_OPTIMAL;
}

/* ============================================================================
 * Bottleneck Network Flow
 *
 * Minimizes the maximum arc cost used in the flow (minimax objective).
 * Uses binary search over arc costs with feasibility checks.
 * ============================================================================ */

/* Compare doubles for qsort */
static int compare_doubles(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

/*
 * Check if a feasible flow exists using only arcs with cost <= threshold.
 * Returns 1 if feasible, 0 otherwise. Also returns the flow if feasible.
 */
static int bottleneck_feasibility_check(
    const RalphNetflowProblem *problem,
    double threshold,
    double *flow_out,
    RalphNetflowWorkspace *ws
) {
    int num_nodes = problem->num_nodes;
    int num_arcs = problem->num_arcs;

    /* Create modified capacity array: zero capacity for arcs above threshold */
    double *modified_cap = (double *)malloc(num_arcs * sizeof(double));
    if (!modified_cap) return 0;

    for (int a = 0; a < num_arcs; a++) {
        if (problem->cost[a] <= threshold + RALPH_NETFLOW_TOLERANCE) {
            /* Arc is allowed - use original capacity */
            modified_cap[a] = problem->capacity ? problem->capacity[a] : RALPH_NETFLOW_INFINITY;
        } else {
            /* Arc cost exceeds threshold - disable it */
            modified_cap[a] = 0.0;
        }
    }

    /* Create modified problem */
    RalphNetflowProblem mod_problem = {
        .num_nodes = num_nodes,
        .num_arcs = num_arcs,
        .tail = problem->tail,
        .head = problem->head,
        .cost = problem->cost,  /* Costs don't matter for feasibility */
        .capacity = modified_cap,
        .lower = problem->lower,
        .supply = problem->supply,
        .objective = RALPH_NETFLOW_MINIMIZE
    };

    /* Solve with default options */
    RalphNetflowOptions opts = RALPH_NETFLOW_OPTIONS_DEFAULT;
    RalphNetflowResult result = {.flow = flow_out};

    RalphNetflowStatus status = netflow_solve_internal(&mod_problem, &opts, &result, ws);

    free(modified_cap);

    return (status == RALPH_NETFLOW_OPTIMAL);
}

/*
 * Solve bottleneck network flow: minimize the maximum arc cost used.
 *
 * Algorithm:
 * 1. Extract and sort unique arc costs
 * 2. Binary search on threshold
 * 3. For each threshold, check if feasible flow exists using only arcs with cost <= threshold
 * 4. Return minimum threshold that allows feasible flow
 */
static RalphNetflowStatus solve_bottleneck_internal(
    const RalphNetflowProblem *problem,
    const RalphNetflowOptions *options,
    RalphNetflowResult *result,
    RalphNetflowWorkspace *ws
) {
    (void)options;  /* Unused for now */

    int num_nodes = problem->num_nodes;
    int num_arcs = problem->num_arcs;

    if (num_arcs == 0) {
        /* No arcs - check if any flow is needed */
        int needs_flow = 0;
        for (int i = 0; i < num_nodes; i++) {
            if (fabs(problem->supply[i]) > RALPH_NETFLOW_TOLERANCE) {
                needs_flow = 1;
                break;
            }
        }

        if (needs_flow) {
            /* Supply/demand exists but no arcs to route it */
            result->status = RALPH_NETFLOW_INFEASIBLE;
            return RALPH_NETFLOW_INFEASIBLE;
        }

        /* No flow needed - trivially optimal */
        result->objective = 0.0;
        result->num_found = 1;
        result->status = RALPH_NETFLOW_OPTIMAL;
        result->iterations = 0;
        result->degenerate_pivots = 0;
        return RALPH_NETFLOW_OPTIMAL;
    }

    /* Extract arc costs and sort them */
    double *costs = (double *)malloc(num_arcs * sizeof(double));
    if (!costs) {
        result->status = RALPH_NETFLOW_OUT_OF_MEMORY;
        return RALPH_NETFLOW_OUT_OF_MEMORY;
    }

    for (int a = 0; a < num_arcs; a++) {
        costs[a] = problem->cost[a];
    }
    qsort(costs, num_arcs, sizeof(double), compare_doubles);

    /* Remove duplicates to get unique thresholds */
    int num_unique = 1;
    for (int i = 1; i < num_arcs; i++) {
        if (costs[i] > costs[num_unique - 1] + RALPH_NETFLOW_TOLERANCE) {
            costs[num_unique++] = costs[i];
        }
    }

    /* Binary search for minimum threshold */
    int lo = 0;
    int hi = num_unique - 1;
    int best = -1;

    /* Allocate temp flow array for feasibility checks */
    double *temp_flow = (double *)malloc(num_arcs * sizeof(double));
    if (!temp_flow) {
        free(costs);
        result->status = RALPH_NETFLOW_OUT_OF_MEMORY;
        return RALPH_NETFLOW_OUT_OF_MEMORY;
    }

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        double threshold = costs[mid];

        if (bottleneck_feasibility_check(problem, threshold, temp_flow, ws)) {
            /* Feasible with this threshold - try lower */
            best = mid;
            /* Copy the feasible flow to result */
            memcpy(result->flow, temp_flow, num_arcs * sizeof(double));
            hi = mid - 1;
        } else {
            /* Not feasible - need higher threshold */
            lo = mid + 1;
        }
    }

    free(temp_flow);
    free(costs);

    if (best < 0) {
        /* No feasible flow exists */
        result->status = RALPH_NETFLOW_INFEASIBLE;
        return RALPH_NETFLOW_INFEASIBLE;
    }

    /* Return the bottleneck cost (maximum arc cost used) */
    /* Verify by finding the actual maximum cost used */
    double actual_max_cost = 0.0;
    for (int a = 0; a < num_arcs; a++) {
        if (result->flow[a] > RALPH_NETFLOW_TOLERANCE) {
            if (problem->cost[a] > actual_max_cost) {
                actual_max_cost = problem->cost[a];
            }
        }
    }

    result->objective = actual_max_cost;
    result->num_found = 1;
    result->status = RALPH_NETFLOW_OPTIMAL;
    result->iterations = 0;  /* Not tracked for bottleneck */
    result->degenerate_pivots = 0;

    if (result->objectives) {
        result->objectives[0] = actual_max_cost;
    }

    return RALPH_NETFLOW_OPTIMAL;
}

/* ============================================================================
 * Unified API Implementation
 * ============================================================================ */

/*
 * Internal validation helper.
 * Returns RALPH_NETFLOW_OPTIMAL if valid, error code otherwise.
 */
static RalphNetflowStatus validate_problem(
    const RalphNetflowProblem *problem,
    RalphNetflowResult *result
) {
    if (!problem || !result) {
        if (result) result->status = RALPH_NETFLOW_INVALID_INPUT;
        return RALPH_NETFLOW_INVALID_INPUT;
    }

    /* flow array required only if num_arcs > 0 */
    if (problem->num_arcs > 0 && !result->flow) {
        result->status = RALPH_NETFLOW_INVALID_INPUT;
        return RALPH_NETFLOW_INVALID_INPUT;
    }

    if (problem->num_nodes <= 0 || problem->num_arcs < 0) {
        result->status = RALPH_NETFLOW_INVALID_INPUT;
        return RALPH_NETFLOW_INVALID_INPUT;
    }

    /* Check required arrays (NULL ok if num_arcs == 0) */
    if (!problem->supply) {
        result->status = RALPH_NETFLOW_INVALID_INPUT;
        return RALPH_NETFLOW_INVALID_INPUT;
    }
    if (problem->num_arcs > 0 && (!problem->tail || !problem->head || !problem->cost)) {
        result->status = RALPH_NETFLOW_INVALID_INPUT;
        return RALPH_NETFLOW_INVALID_INPUT;
    }

    /* Validate arc endpoints and data */
    for (int a = 0; a < problem->num_arcs; a++) {
        if (problem->tail[a] < 0 || problem->tail[a] >= problem->num_nodes ||
            problem->head[a] < 0 || problem->head[a] >= problem->num_nodes) {
            result->status = RALPH_NETFLOW_INVALID_INPUT;
            return RALPH_NETFLOW_INVALID_INPUT;
        }

        /* Check for NaN/Inf in costs */
        if (!is_valid_double(problem->cost[a])) {
            result->status = RALPH_NETFLOW_INVALID_INPUT;
            return RALPH_NETFLOW_INVALID_INPUT;
        }

        /* Validate capacity and lower bounds */
        if (problem->capacity) {
            if (!is_valid_double(problem->capacity[a]) || problem->capacity[a] < 0) {
                result->status = RALPH_NETFLOW_INVALID_INPUT;
                return RALPH_NETFLOW_INVALID_INPUT;
            }
        }
        if (problem->lower) {
            if (!is_valid_double(problem->lower[a]) || problem->lower[a] < 0) {
                result->status = RALPH_NETFLOW_INVALID_INPUT;
                return RALPH_NETFLOW_INVALID_INPUT;
            }
        }

        /* Check capacity >= lower */
        if (problem->capacity && problem->lower) {
            if (problem->capacity[a] < problem->lower[a] - RALPH_NETFLOW_TOLERANCE) {
                result->status = RALPH_NETFLOW_INVALID_INPUT;
                return RALPH_NETFLOW_INVALID_INPUT;
            }
        }
    }

    /* Validate supply values */
    for (int i = 0; i < problem->num_nodes; i++) {
        if (!is_valid_double(problem->supply[i])) {
            result->status = RALPH_NETFLOW_INVALID_INPUT;
            return RALPH_NETFLOW_INVALID_INPUT;
        }
    }

    /* Check supply balance */
    double total_supply = 0.0;
    for (int i = 0; i < problem->num_nodes; i++) {
        total_supply += problem->supply[i];
    }
    if (fabs(total_supply) > RALPH_NETFLOW_TOLERANCE * problem->num_nodes) {
        result->status = RALPH_NETFLOW_INFEASIBLE;
        return RALPH_NETFLOW_INFEASIBLE;
    }

    return RALPH_NETFLOW_OPTIMAL;
}

/*
 * Unified API - main entry point with algorithm dispatch.
 */
RalphNetflowStatus ralph_netflow_solve_ex(
    const RalphNetflowProblem *problem,
    const RalphNetflowOptions *options,
    RalphNetflowResult *result,
    RalphNetflowWorkspace *workspace
) {
    /* Validate inputs */
    RalphNetflowStatus status = validate_problem(problem, result);
    if (status != RALPH_NETFLOW_OPTIMAL) {
        return status;
    }

    /* Default options */
    RalphNetflowOptions default_opts = RALPH_NETFLOW_OPTIONS_DEFAULT;
    const RalphNetflowOptions *opts = options ? options : &default_opts;

    /* Workspace management */
    RalphNetflowWorkspace *ws = workspace;
    int allocated_ws = 0;

    if (!ws) {
        ws = ralph_netflow_workspace_create(problem->num_nodes, problem->num_arcs);
        if (!ws) {
            result->status = RALPH_NETFLOW_OUT_OF_MEMORY;
            return RALPH_NETFLOW_OUT_OF_MEMORY;
        }
        allocated_ws = 1;
    } else {
        if (problem->num_nodes > ws->max_nodes || problem->num_arcs > ws->max_arcs) {
            result->status = RALPH_NETFLOW_INVALID_INPUT;
            return RALPH_NETFLOW_INVALID_INPUT;
        }
    }

    /* Initialize result */
    result->status = RALPH_NETFLOW_OPTIMAL;
    result->num_found = 0;

    /* Dispatch based on algorithm */
    switch (opts->algorithm) {
        case RALPH_NETFLOW_ALG_K_BEST:
            if (opts->k <= 0) {
                status = RALPH_NETFLOW_INVALID_INPUT;
            } else {
                /* Solve standard problem first */
                status = netflow_solve_internal(problem, opts, result, ws);
                if (status == RALPH_NETFLOW_OPTIMAL && opts->k > 1) {
                    /* Decompose flow into up to k paths */
                    if (result->paths) {
                        int num_paths = 0;
                        RalphNetflowStatus decomp_status = ralph_netflow_decompose(
                            problem, result->flow, opts->k, result->paths, &num_paths);
                        if (decomp_status == RALPH_NETFLOW_OPTIMAL) {
                            result->num_found = num_paths;
                            /* Fill objectives array if provided */
                            if (result->objectives) {
                                for (int i = 0; i < num_paths; i++) {
                                    result->objectives[i] = result->paths[i].cost;
                                }
                            }
                        }
                    }
                }
            }
            break;

        case RALPH_NETFLOW_ALG_BOTTLENECK:
            status = solve_bottleneck_internal(problem, opts, result, ws);
            break;

        case RALPH_NETFLOW_ALG_STANDARD:
        default:
            status = netflow_solve_internal(problem, opts, result, ws);
            break;
    }

    result->status = status;

    if (allocated_ws) {
        ralph_netflow_workspace_free(ws);
    }

    return status;
}

/*
 * Original API - wrapper around unified API.
 */
RalphNetflowStatus ralph_netflow_solve(
    const RalphNetflowProblem *problem,
    const RalphNetflowOptions *options,
    RalphNetflowResult *result,
    RalphNetflowWorkspace *workspace
) {
    return ralph_netflow_solve_ex(problem, options, result, workspace);
}

RalphNetflowStatus ralph_mcnf_solve(
    int num_nodes,
    int num_arcs,
    const int *tail,
    const int *head,
    const double *cost,
    const double *capacity,
    const double *supply,
    double *flow,
    double *objective
) {
    RalphNetflowProblem problem = {
        .num_nodes = num_nodes,
        .num_arcs = num_arcs,
        .tail = tail,
        .head = head,
        .cost = cost,
        .capacity = capacity,
        .lower = NULL,
        .supply = supply,
        .objective = RALPH_NETFLOW_MINIMIZE
    };

    RalphNetflowResult result = {
        .flow = flow,
        .potential = NULL
    };

    RalphNetflowStatus status = ralph_netflow_solve(&problem, NULL, &result, NULL);

    if (objective) {
        *objective = result.objective;
    }

    return status;
}

/* ============================================================================
 * Verification
 * ============================================================================ */

int ralph_netflow_verify(
    const RalphNetflowProblem *problem,
    const double *flow,
    double *objective,
    double *max_violation
) {
    if (!problem || !flow) {
        return 0;
    }

    int num_nodes = problem->num_nodes;
    int num_arcs = problem->num_arcs;
    double max_viol = 0.0;

    /* Check flow conservation */
    for (int i = 0; i < num_nodes; i++) {
        double net_flow = problem->supply[i];
        for (int a = 0; a < num_arcs; a++) {
            if (problem->tail[a] == i) {
                net_flow -= flow[a];
            }
            if (problem->head[a] == i) {
                net_flow += flow[a];
            }
        }
        double viol = fabs(net_flow);
        if (viol > max_viol) max_viol = viol;
    }

    /* Check capacity bounds */
    for (int a = 0; a < num_arcs; a++) {
        double lower = problem->lower ? problem->lower[a] : 0.0;
        double upper = problem->capacity ? problem->capacity[a] : RALPH_NETFLOW_INFINITY;

        if (flow[a] < lower - RALPH_NETFLOW_TOLERANCE) {
            double viol = lower - flow[a];
            if (viol > max_viol) max_viol = viol;
        }
        if (flow[a] > upper + RALPH_NETFLOW_TOLERANCE && !is_infinite(upper)) {
            double viol = flow[a] - upper;
            if (viol > max_viol) max_viol = viol;
        }
    }

    if (objective) {
        double obj = 0.0;
        for (int a = 0; a < num_arcs; a++) {
            obj += problem->cost[a] * flow[a];
        }
        *objective = obj;
    }

    if (max_violation) {
        *max_violation = max_viol;
    }

    return max_viol <= RALPH_NETFLOW_TOLERANCE * num_nodes;
}

int ralph_netflow_check_optimality(
    const RalphNetflowProblem *problem,
    const double *flow,
    const double *potential,
    double *max_violation
) {
    if (!problem || !flow || !potential) {
        return 0;
    }

    int num_arcs = problem->num_arcs;
    double max_viol = 0.0;
    double mult = (problem->objective == RALPH_NETFLOW_MAXIMIZE) ? -1.0 : 1.0;

    for (int a = 0; a < num_arcs; a++) {
        double lower = problem->lower ? problem->lower[a] : 0.0;
        double upper = problem->capacity ? problem->capacity[a] : RALPH_NETFLOW_INFINITY;

        double rc = problem->cost[a] * mult -
                    potential[problem->tail[a]] +
                    potential[problem->head[a]];

        if (flow[a] < upper - RALPH_NETFLOW_TOLERANCE && rc < -RALPH_NETFLOW_TOLERANCE) {
            double viol = -rc;
            if (viol > max_viol) max_viol = viol;
        }
        if (flow[a] > lower + RALPH_NETFLOW_TOLERANCE && rc > RALPH_NETFLOW_TOLERANCE) {
            double viol = rc;
            if (viol > max_viol) max_viol = viol;
        }
    }

    if (max_violation) {
        *max_violation = max_viol;
    }

    return max_viol <= RALPH_NETFLOW_TOLERANCE * RALPH_NETFLOW_OPT_TOL_MULTIPLIER;
}

/* ============================================================================
 * Flow Decomposition
 * ============================================================================ */

void ralph_netflow_path_free(RalphNetflowPath *path) {
    if (path && path->arcs) {
        free(path->arcs);
        path->arcs = NULL;
        path->num_arcs = 0;
    }
}

/* Compare paths by unit cost for sorting */
static int compare_paths_by_unit_cost(const void *a, const void *b) {
    const RalphNetflowPath *pa = (const RalphNetflowPath *)a;
    const RalphNetflowPath *pb = (const RalphNetflowPath *)b;
    if (pa->unit_cost < pb->unit_cost) return -1;
    if (pa->unit_cost > pb->unit_cost) return 1;
    return 0;
}

RalphNetflowStatus ralph_netflow_decompose(
    const RalphNetflowProblem *problem,
    const double *flow,
    int max_paths,
    RalphNetflowPath *paths,
    int *num_paths
) {
    if (!problem || !flow || !paths || !num_paths) {
        return RALPH_NETFLOW_INVALID_INPUT;
    }

    int num_nodes = problem->num_nodes;
    int num_arcs = problem->num_arcs;
    *num_paths = 0;

    if (num_arcs == 0) {
        return RALPH_NETFLOW_OPTIMAL;
    }

    /* Allocate working arrays */
    double *residual_flow = (double *)malloc(num_arcs * sizeof(double));
    double *residual_supply = (double *)malloc(num_nodes * sizeof(double));
    int *path_arcs = (int *)malloc(num_nodes * sizeof(int));  /* Max path length */
    int *visited = (int *)calloc(num_nodes, sizeof(int));

    if (!residual_flow || !residual_supply || !path_arcs || !visited) {
        free(residual_flow);
        free(residual_supply);
        free(path_arcs);
        free(visited);
        return RALPH_NETFLOW_OUT_OF_MEMORY;
    }

    /* Initialize residuals */
    memcpy(residual_flow, flow, num_arcs * sizeof(double));
    memcpy(residual_supply, problem->supply, num_nodes * sizeof(double));

    /* Build adjacency list: for each node, list of outgoing arcs */
    int *arc_start = (int *)malloc((num_nodes + 1) * sizeof(int));
    int *arc_list = (int *)malloc(num_arcs * sizeof(int));

    if (!arc_start || !arc_list) {
        free(residual_flow);
        free(residual_supply);
        free(path_arcs);
        free(visited);
        free(arc_start);
        free(arc_list);
        return RALPH_NETFLOW_OUT_OF_MEMORY;
    }

    /* Count outgoing arcs per node */
    memset(arc_start, 0, (num_nodes + 1) * sizeof(int));
    for (int a = 0; a < num_arcs; a++) {
        arc_start[problem->tail[a] + 1]++;
    }
    for (int i = 1; i <= num_nodes; i++) {
        arc_start[i] += arc_start[i - 1];
    }

    /* Fill arc list */
    int *arc_pos = (int *)malloc(num_nodes * sizeof(int));
    if (!arc_pos) {
        free(residual_flow);
        free(residual_supply);
        free(path_arcs);
        free(visited);
        free(arc_start);
        free(arc_list);
        return RALPH_NETFLOW_OUT_OF_MEMORY;
    }
    memcpy(arc_pos, arc_start, num_nodes * sizeof(int));

    for (int a = 0; a < num_arcs; a++) {
        int tail = problem->tail[a];
        arc_list[arc_pos[tail]++] = a;
    }
    free(arc_pos);

    /* Decompose flow into paths */
    int path_count = 0;
    int max_path_count = (max_paths > 0) ? max_paths : num_arcs;  /* Upper bound */

    while (path_count < max_path_count) {
        /* Find a source with positive residual supply */
        int source = -1;
        for (int i = 0; i < num_nodes; i++) {
            if (residual_supply[i] > RALPH_NETFLOW_TOLERANCE) {
                source = i;
                break;
            }
        }
        if (source < 0) break;  /* No more flow to decompose */

        /* DFS to find path from source to sink */
        memset(visited, 0, num_nodes * sizeof(int));
        int path_len = 0;
        int current = source;
        double min_flow = residual_supply[source];
        visited[current] = 1;

        while (residual_supply[current] >= -RALPH_NETFLOW_TOLERANCE) {
            /* Current is not a sink, find outgoing arc with positive flow */
            int found_arc = -1;
            for (int idx = arc_start[current]; idx < arc_start[current + 1]; idx++) {
                int a = arc_list[idx];
                if (residual_flow[a] > RALPH_NETFLOW_TOLERANCE) {
                    int next = problem->head[a];
                    if (!visited[next]) {
                        found_arc = a;
                        break;
                    }
                }
            }

            if (found_arc < 0) {
                /* No outgoing arc found - this shouldn't happen for valid flow */
                break;
            }

            path_arcs[path_len++] = found_arc;
            if (residual_flow[found_arc] < min_flow) {
                min_flow = residual_flow[found_arc];
            }

            current = problem->head[found_arc];
            visited[current] = 1;

            /* Check if reached a sink */
            if (residual_supply[current] < -RALPH_NETFLOW_TOLERANCE) {
                if (-residual_supply[current] < min_flow) {
                    min_flow = -residual_supply[current];
                }
                break;
            }
        }

        if (path_len == 0 || min_flow <= RALPH_NETFLOW_TOLERANCE) {
            /* No valid path found, stop */
            break;
        }

        /* Record the path */
        RalphNetflowPath *p = &paths[path_count];
        p->arcs = (int *)malloc(path_len * sizeof(int));
        if (!p->arcs) {
            /* Clean up previously allocated paths */
            for (int i = 0; i < path_count; i++) {
                ralph_netflow_path_free(&paths[i]);
            }
            free(residual_flow);
            free(residual_supply);
            free(path_arcs);
            free(visited);
            free(arc_start);
            free(arc_list);
            return RALPH_NETFLOW_OUT_OF_MEMORY;
        }

        memcpy(p->arcs, path_arcs, path_len * sizeof(int));
        p->num_arcs = path_len;
        p->source = source;
        p->sink = current;
        p->flow = min_flow;

        /* Calculate path cost */
        double unit_cost = 0.0;
        for (int i = 0; i < path_len; i++) {
            unit_cost += problem->cost[p->arcs[i]];
        }
        p->unit_cost = unit_cost;
        p->cost = unit_cost * min_flow;

        /* Update residuals */
        residual_supply[source] -= min_flow;
        residual_supply[current] += min_flow;
        for (int i = 0; i < path_len; i++) {
            residual_flow[p->arcs[i]] -= min_flow;
        }

        path_count++;
    }

    /* Sort paths by unit cost */
    if (path_count > 1) {
        qsort(paths, path_count, sizeof(RalphNetflowPath), compare_paths_by_unit_cost);
    }

    *num_paths = path_count;

    free(residual_flow);
    free(residual_supply);
    free(path_arcs);
    free(visited);
    free(arc_start);
    free(arc_list);

    return RALPH_NETFLOW_OPTIMAL;
}
