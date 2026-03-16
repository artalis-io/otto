#ifdef SG_HAS_THREADS

#include "sg_internal.h"
#include "sg_parallel.h"
#include "sh_worker_pool.h"
#include "sh_completion.h"
#include "sh_dist.h"

#include <unistd.h>

/* Population pool constants */
#define SG_POP_DEFAULT_POOL_SIZE   25  /* S27b: enlarged from 6 for BPD diversity */
#define SG_POP_FEASIBLE_CAP        15  /* S27f: max feasible members */
#define SG_POP_INFEASIBLE_CAP      10  /* S27f: max infeasible members */

/* ============================================================================
 * Internal types
 * ============================================================================ */

typedef struct {
    SGContext clone;                   /* Shallow clone (embedded by value) */
    _Atomic uint8_t *master_cancel;  /* Points to original ctx->cancel_requested */
    uint64_t seed;
    int use_deterministic;
    SGStatus result;
    ShCompletion completion;
    uint8_t owns_warm_start;          /* 1 = SREX-built, needs free */
} SGParallelWorkItem;

typedef struct {
    uint32_t *vehicle_ids;       /* [num_routes] */
    uint32_t *route_lengths;     /* [num_routes] */
    uint32_t *request_ids;       /* [total_requests] flat */
    uint32_t num_routes;
    uint32_t total_requests;
    /* Cached quality for sorting */
    uint32_t num_unassigned;
    uint32_t vehicles_used;
    double total_distance;
    /* S27b: Broken-pairs next-request map for diversity */
    uint32_t *next_request;      /* [num_requests] next in same route, UINT32_MAX if last */
    double biased_fitness;       /* Computed on demand */
    /* S27f: Feasibility classification */
    double total_violation;      /* Sum of constraint violations; 0 = feasible */
} SGPopulationMember;

/* ============================================================================
 * Clone lifecycle
 * ============================================================================ */

static SGStatus sg_context_clone_init(SGContext *clone, const SGContext *src) {
    memcpy(clone, src, sizeof(SGContext));

    /* Fresh RNG for this clone */
    clone->op_rng = sh_rng_create_default();
    if (!clone->op_rng) return SG_STATUS_OUT_OF_MEMORY;

    /* Clear solver-scoped mutable state */
    clone->final_solution = NULL;
    clone->active_solution = NULL;
    clone->destroy_op_stats = NULL;
    clone->repair_op_stats = NULL;
    clone->num_destroy_ops = 0;
    clone->num_repair_ops = 0;
    memset(&clone->stats, 0, sizeof(SGStats));
    memset(&clone->scratch, 0, sizeof(SGScratchBuffers));
    clone->solution_arena_size = 0;
    atomic_store(&clone->cancel_requested, 0);

    /* Penalty manager: clear so each clone's sg_solve() initializes its own.
     * The memcpy above copied the original's penalty.state pointer — we must
     * not share it (mutable feasible_count/total_count per constraint). */
    memset(&clone->penalty, 0, sizeof(SGPenaltyManager));

    /* Convergence: each clone must NOT write to the master's ring buffer.
     * sg_solve() checks convergence_buffer != NULL before writing, so
     * clearing these fields disables convergence recording in clones. */
    clone->convergence_buffer = NULL;
    clone->convergence_callback = NULL;
    clone->convergence_callback_data = NULL;
    clone->convergence_capacity = 0;
    clone->convergence_count = 0;
    clone->convergence_write_pos = 0;

    /* tune_params: shared read-only pointer to master's params — safe for
     * concurrent reads since sg_tune_d()/sg_tune_i() are pure accessors
     * and tune_params is never modified during solving. */

    /* Modified-vehicles bitset: each clone's sg_solve() allocates its own. */
    clone->modified_vehicles = NULL;
    clone->modified_vehicles_words = 0;

    /* Clear inherited warm start (population search injects its own) */
    clone->initial_route_vehicle_ids = NULL;
    clone->initial_route_request_ids = NULL;
    clone->initial_route_lengths = NULL;
    clone->num_initial_routes = 0;
    clone->total_initial_requests = 0;

    /* Progress callback replaced with cancel forwarder in worker callback */
    clone->progress_callback = NULL;
    clone->progress_callback_data = NULL;

    return SG_STATUS_OK;
}

static void sg_context_clone_free(SGContext *clone) {
    sh_rng_free(clone->op_rng);
    clone->op_rng = NULL;

    if (clone->final_solution) {
        sg_route_solution_free(clone->final_solution, NULL);
        clone->final_solution = NULL;
    }

    free(clone->destroy_op_stats);
    clone->destroy_op_stats = NULL;
    free(clone->repair_op_stats);
    clone->repair_op_stats = NULL;

    /* Free penalty state if sg_solve() allocated one but didn't clean up
     * (e.g., early error exit). Normal path: sg_solve_route_model() already
     * calls sg_penalty_free(), so this is a no-op (state == NULL). */
    sg_penalty_free(&clone->penalty);

    /* scratch is freed inside sg_solve_route_model already */
    /* Do NOT free model data (depots, vehicles, requests, matrices, etc.) */
    /* Do NOT free warm start pointers — they're borrowed from population pool */
}

/* ============================================================================
 * Worker callback
 * ============================================================================ */

static int sg_parallel_cancel_forwarder(const SGStats *stats, void *user_data) {
    SGParallelWorkItem *item = (SGParallelWorkItem *)user_data;
    (void)stats;
    if (atomic_load(item->master_cancel)) {
        atomic_store(&item->clone.cancel_requested, 1);
        return 1;
    }
    return 0;
}

static void sg_parallel_worker_callback(ShWorkItem *wi, void *pool_ctx) {
    SGParallelWorkItem *item = (SGParallelWorkItem *)wi->user_ctx;
    (void)pool_ctx;

    if (sh_completion_is_cancelled(&item->completion)) {
        sh_completion_signal(&item->completion);
        sh_workqueue_item_free(wi);
        return;
    }

    /* Set seed and deterministic mode */
    item->clone.config.seed = item->seed;
    if (item->use_deterministic)
        item->clone.config.deterministic = 1;

    /* Install cancel forwarder as progress callback */
    item->clone.progress_callback = sg_parallel_cancel_forwarder;
    item->clone.progress_callback_data = item;

    item->result = sg_solve(&item->clone);

    sh_completion_signal(&item->completion);
    sh_workqueue_item_free(wi);
}

/* ============================================================================
 * Population helpers
 * ============================================================================ */

static SGStatus sg_extract_routes(const SGRouteSolution *sol,
                                   SGPopulationMember *member) {
    uint32_t v, num_routes = 0, total_reqs = 0, offset;

    if (!sol || !member) return SG_STATUS_INVALID_ARG;

    memset(member, 0, sizeof(*member));
    member->num_unassigned = sol->base.num_unassigned;
    member->vehicles_used = sol->vehicles_used;
    member->total_distance = sol->total_distance;
    member->total_violation = sg_solution_total_violation(sol);

    /* Count non-empty routes */
    for (v = 0; v < sol->num_vehicles; v++) {
        if (sol->route_lengths[v] > 0) {
            num_routes++;
            total_reqs += sol->route_lengths[v];
        }
    }

    if (num_routes == 0) return SG_STATUS_OK;

    member->vehicle_ids = (uint32_t *)malloc((size_t)num_routes * sizeof(uint32_t));
    member->route_lengths = (uint32_t *)malloc((size_t)num_routes * sizeof(uint32_t));
    member->request_ids = (uint32_t *)malloc((size_t)total_reqs * sizeof(uint32_t));
    if (!member->vehicle_ids || !member->route_lengths || !member->request_ids) {
        free(member->vehicle_ids);
        free(member->route_lengths);
        free(member->request_ids);
        memset(member, 0, sizeof(*member));
        return SG_STATUS_OUT_OF_MEMORY;
    }

    offset = 0;
    num_routes = 0;
    for (v = 0; v < sol->num_vehicles; v++) {
        uint32_t len = sol->route_lengths[v];
        if (len == 0) continue;
        member->vehicle_ids[num_routes] = v;
        member->route_lengths[num_routes] = len;
        memcpy(&member->request_ids[offset],
               sg_route_vehicle_ptr_const(sol, v),
               (size_t)len * sizeof(uint32_t));
        offset += len;
        num_routes++;
    }

    member->num_routes = num_routes;
    member->total_requests = total_reqs;
    return SG_STATUS_OK;
}

static void sg_population_member_free(SGPopulationMember *m) {
    if (!m) return;
    free(m->vehicle_ids);
    free(m->route_lengths);
    free(m->request_ids);
    free(m->next_request);
    memset(m, 0, sizeof(*m));
}

/* S27b: Build next-request map for BPD computation.
   next_request[r] = request ID that immediately follows r in same route,
   or UINT32_MAX if r is last in its route or unassigned. */
static void sg_population_build_next_map(SGPopulationMember *m, uint32_t num_requests) {
    uint32_t ri, i, offset;

    free(m->next_request);
    m->next_request = (uint32_t *)malloc((size_t)num_requests * sizeof(uint32_t));
    if (!m->next_request) return;
    memset(m->next_request, 0xFF, (size_t)num_requests * sizeof(uint32_t));

    offset = 0;
    for (ri = 0; ri < m->num_routes; ri++) {
        uint32_t rlen = m->route_lengths[ri];
        for (i = 0; i < rlen; i++) {
            uint32_t rid = m->request_ids[offset + i];
            if (rid < num_requests) {
                m->next_request[rid] = (i + 1 < rlen) ? m->request_ids[offset + i + 1] : UINT32_MAX;
            }
        }
        offset += rlen;
    }
}

/* S27b: Broken Pairs Distance. Fraction of requests where next differs. O(n). */
static double sg_broken_pairs_distance(const SGPopulationMember *a,
                                        const SGPopulationMember *b,
                                        uint32_t num_requests) {
    uint32_t i, different = 0, counted = 0;

    if (!a->next_request || !b->next_request || num_requests == 0) return 1.0;

    for (i = 0; i < num_requests; i++) {
        /* Only count if assigned in both */
        if (a->next_request[i] == UINT32_MAX && b->next_request[i] == UINT32_MAX) continue;
        counted++;
        if (a->next_request[i] != b->next_request[i]) different++;
    }
    return counted > 0 ? (double)different / (double)counted : 1.0;
}

/* S27b: Compute biased fitness for all pool members.
   fitness = quality_rank + (1 - n_elite/pool_size) * diversity_rank
   Lower = better. n_elite = pool_size / 5 (PyVRP convention). */
static void sg_population_compute_fitness(SGPopulationMember *pool, uint32_t pool_size,
                                           uint32_t num_requests) {
    uint32_t i, j;
    double *avg_bpd;       /* average BPD to k nearest neighbors */
    uint32_t *div_rank;
    uint32_t n_elite;
    uint32_t k_near = pool_size < 5 ? pool_size : 5;

    if (pool_size <= 1) {
        if (pool_size == 1) pool[0].biased_fitness = 0.0;
        return;
    }

    avg_bpd = (double *)calloc((size_t)pool_size, sizeof(double));
    div_rank = (uint32_t *)calloc((size_t)pool_size, sizeof(uint32_t));
    if (!avg_bpd || !div_rank) {
        free(avg_bpd);
        free(div_rank);
        /* Fallback: use quality rank only */
        for (i = 0; i < pool_size; i++) pool[i].biased_fitness = (double)i;
        return;
    }

    /* Compute average BPD to k nearest neighbors for each member */
    for (i = 0; i < pool_size; i++) {
        double *distances = (double *)malloc((size_t)pool_size * sizeof(double));
        if (!distances) { avg_bpd[i] = 0.0; continue; }

        for (j = 0; j < pool_size; j++) {
            distances[j] = (i == j) ? 999.0 : sg_broken_pairs_distance(&pool[i], &pool[j], num_requests);
        }
        /* Partial sort to find k smallest (simple selection) */
        {
            uint32_t k2;
            double sum = 0.0;
            for (k2 = 0; k2 < k_near; k2++) {
                uint32_t min_idx = k2;
                for (j = k2 + 1; j < pool_size; j++) {
                    if (distances[j] < distances[min_idx]) min_idx = j;
                }
                if (min_idx != k2) {
                    double tmp = distances[k2]; distances[k2] = distances[min_idx]; distances[min_idx] = tmp;
                }
                sum += distances[k2];
            }
            avg_bpd[i] = sum / (double)k_near;
        }
        free(distances);
    }

    /* Rank by diversity (higher avg_bpd = more diverse = lower rank = better) */
    for (i = 0; i < pool_size; i++) div_rank[i] = 0;
    for (i = 0; i < pool_size; i++) {
        for (j = 0; j < pool_size; j++) {
            if (j != i && avg_bpd[j] > avg_bpd[i]) div_rank[i]++;
        }
    }

    /* Biased fitness: quality_rank + weight * diversity_rank
       Pool is sorted by quality, so quality_rank = i.
       n_elite = max(1, pool_size / 5). */
    n_elite = pool_size / 5;
    if (n_elite < 1) n_elite = 1;
    {
        double div_weight = 1.0 - (double)n_elite / (double)pool_size;
        for (i = 0; i < pool_size; i++) {
            pool[i].biased_fitness = (double)i + div_weight * (double)div_rank[i];
        }
    }

    free(avg_bpd);
    free(div_rank);
}

/* S27b: PyVRP-style population insert with biased fitness survivor selection.
   Pool is kept sorted by quality (ascending cost). When full, remove member
   with worst biased fitness (combines quality rank + diversity rank). */
static void sg_population_insert_ex(SGPopulationMember *pool, uint32_t *pool_size,
                                     uint32_t pool_capacity, SGPopulationMember *candidate,
                                     uint32_t num_requests) {
    uint32_t sz = *pool_size;
    uint32_t i;

    /* Build next-request map for BPD */
    sg_population_build_next_map(candidate, num_requests);

    /* Duplicate detection: skip if same (vehicles_used, total_distance) */
    for (i = 0; i < sz; i++) {
        if (pool[i].vehicles_used == candidate->vehicles_used &&
            fabs(pool[i].total_distance - candidate->total_distance) < 1e-9 &&
            pool[i].num_unassigned == candidate->num_unassigned) {
            sg_population_member_free(candidate);
            return;
        }
    }

    if (sz < pool_capacity) {
        /* Room in pool — append */
        pool[sz] = *candidate;
        *pool_size = sz + 1;
    } else {
        /* Pool full — S27f dual-pool survivor selection.
           Evict from the appropriate subpool to maintain balance. */
        uint32_t nf = 0, victim_idx = UINT32_MAX;
        double worst_fitness = -1e30;
        int cand_feasible = (candidate->total_violation < 1e-9);
        int evict_feasible;

        for (i = 0; i < sz; i++) {
            if (pool[i].total_violation < 1e-9) nf++;
        }

        /* Decide which subpool to evict from:
           - Candidate feasible + feasible at cap → evict feasible
           - Candidate infeasible + infeasible at cap → evict infeasible
           - Otherwise evict from the opposite subpool to make room */
        evict_feasible = (cand_feasible && nf >= SG_POP_FEASIBLE_CAP) ||
                         (!cand_feasible && (sz - nf) < SG_POP_INFEASIBLE_CAP);

        sg_population_compute_fitness(pool, sz, num_requests);

        /* Find worst biased fitness in target subpool */
        for (i = 0; i < sz; i++) {
            int is_feas = (pool[i].total_violation < 1e-9);
            if (is_feas != evict_feasible) continue;
            if (pool[i].biased_fitness > worst_fitness) {
                worst_fitness = pool[i].biased_fitness;
                victim_idx = i;
            }
        }

        /* Fallback: evict worst from any subpool */
        if (victim_idx == UINT32_MAX) {
            for (i = 0; i < sz; i++) {
                if (pool[i].biased_fitness > worst_fitness) {
                    worst_fitness = pool[i].biased_fitness;
                    victim_idx = i;
                }
            }
        }

        if (victim_idx == UINT32_MAX) {
            sg_population_member_free(candidate);
            return;
        }

        /* Accept candidate if it has better cost OR adds diversity */
        {
            double cand_cost = sg_route_objective_cost(candidate->num_unassigned,
                candidate->vehicles_used, candidate->total_distance);
            double victim_cost = sg_route_objective_cost(pool[victim_idx].num_unassigned,
                pool[victim_idx].vehicles_used, pool[victim_idx].total_distance);
            double bpd = (sz > 0) ? sg_broken_pairs_distance(&pool[0], candidate,
                                                              num_requests) : 1.0;

            if (cand_cost < victim_cost || bpd > 0.2) {
                sg_population_member_free(&pool[victim_idx]);
                pool[victim_idx] = *candidate;
            } else {
                sg_population_member_free(candidate);
            }
        }
    }
}

/* S27b+S27f: Tournament selection on biased fitness with cross-pool draw.
   70% of the time draw from feasible subpool, 30% from infeasible.
   Falls back to any member if the preferred subpool is empty. */
static uint32_t sg_population_tournament_select(SGPopulationMember *pool,
                                                  uint32_t pool_size, SHRng *rng,
                                                  uint32_t num_requests) {
    uint32_t a, b;
    int prefer_feasible, attempt;

    sg_population_compute_fitness(pool, pool_size, num_requests);

    prefer_feasible = (sh_rng_next_u32(rng) % 10) < 7;

    /* Draw two candidates, preferring the target subpool (best-effort) */
    a = sh_rng_next_u32(rng) % pool_size;
    b = sh_rng_next_u32(rng) % pool_size;
    for (attempt = 0; attempt < 3; attempt++) {
        int a_ok = prefer_feasible ? (pool[a].total_violation < 1e-9)
                                   : (pool[a].total_violation >= 1e-9);
        if (a_ok) break;
        a = sh_rng_next_u32(rng) % pool_size;
    }
    for (attempt = 0; attempt < 3; attempt++) {
        int b_ok = prefer_feasible ? (pool[b].total_violation < 1e-9)
                                   : (pool[b].total_violation >= 1e-9);
        if (b_ok) break;
        b = sh_rng_next_u32(rng) % pool_size;
    }

    return pool[a].biased_fitness <= pool[b].biased_fitness ? a : b;
}

/* ============================================================================
 * SREX crossover: build merged warm-start from two parents
 * ============================================================================ */

static void sg_srex_build_warm_start(const SGPopulationMember *p1,
                                      const SGPopulationMember *p2,
                                      SHRng *rng, SGContext *clone) {
    uint8_t *placed = NULL;
    uint32_t *ws_vehicle_ids = NULL;
    uint32_t *ws_route_lengths = NULL;
    uint32_t *ws_request_ids = NULL;
    uint32_t ws_num_routes = 0;
    uint32_t ws_total_requests = 0;
    uint32_t max_routes, max_requests;
    uint32_t k, i, j, offset;

    if (!p1 || !p2 || p1->num_routes == 0 || p2->num_routes == 0) return;

    /* k = number of routes to take from P1 */
    k = 1 + sh_rng_next_u32(rng) % (p1->num_routes < 3 ? p1->num_routes : 3);

    placed = (uint8_t *)calloc(clone->num_requests, sizeof(uint8_t));
    if (!placed) return;

    /* Max possible routes and requests */
    max_routes = p1->num_routes + p2->num_routes;
    max_requests = p1->total_requests + p2->total_requests;
    ws_vehicle_ids = (uint32_t *)malloc((size_t)max_routes * sizeof(uint32_t));
    ws_route_lengths = (uint32_t *)malloc((size_t)max_routes * sizeof(uint32_t));
    ws_request_ids = (uint32_t *)malloc((size_t)max_requests * sizeof(uint32_t));
    if (!ws_vehicle_ids || !ws_route_lengths || !ws_request_ids) {
        free(placed);
        free(ws_vehicle_ids);
        free(ws_route_lengths);
        free(ws_request_ids);
        return;
    }

    /* Step 1: S27c quality-weighted route selection from P1.
       Compute per-route distance from request centroids.  Routes with lower
       per-request distance (better quality) get higher selection probability. */
    {
        uint32_t *perm = (uint32_t *)malloc((size_t)p1->num_routes * sizeof(uint32_t));
        double *weights = (double *)malloc((size_t)p1->num_routes * sizeof(double));
        if (!perm || !weights) {
            free(perm);
            free(weights);
            free(placed);
            free(ws_vehicle_ids);
            free(ws_route_lengths);
            free(ws_request_ids);
            return;
        }

        /* Compute per-route cost and inverse weights */
        {
            uint32_t off = 0;
            for (i = 0; i < p1->num_routes; i++) {
                double cost = 0.0, px = 0, py = 0;
                int have_prev = 0;
                perm[i] = i;
                for (j = 0; j < p1->route_lengths[i]; j++) {
                    double cx, cy;
                    if (sg_request_centroid(clone, p1->request_ids[off + j],
                                            &cx, &cy)) {
                        if (have_prev) cost += sg_euclid(px, py, cx, cy);
                        px = cx; py = cy; have_prev = 1;
                    }
                }
                weights[i] = 1.0 / (cost / (p1->route_lengths[i] > 1
                                    ? (double)p1->route_lengths[i] : 1.0) + 1.0);
                off += p1->route_lengths[i];
            }
        }

        /* Weighted selection without replacement (roulette) */
        for (i = 0; i < k && i < p1->num_routes; i++) {
            double total_w = 0.0, cumul = 0.0, threshold;
            uint32_t j2, sel = i;
            for (j2 = i; j2 < p1->num_routes; j2++)
                total_w += weights[perm[j2]];
            threshold = (sh_rng_next_u32(rng) / 4294967295.0) * total_w;
            for (j2 = i; j2 < p1->num_routes; j2++) {
                cumul += weights[perm[j2]];
                if (cumul >= threshold) { sel = j2; break; }
            }
            { uint32_t tmp = perm[i]; perm[i] = perm[sel]; perm[sel] = tmp; }
        }

        /* Copy selected P1 routes */
        for (i = 0; i < k; i++) {
            uint32_t ri = perm[i];
            uint32_t p1_offset = 0;
            uint32_t rlen;

            for (j = 0; j < ri; j++) p1_offset += p1->route_lengths[j];
            rlen = p1->route_lengths[ri];

            ws_vehicle_ids[ws_num_routes] = p1->vehicle_ids[ri];
            ws_route_lengths[ws_num_routes] = rlen;
            for (j = 0; j < rlen; j++) {
                uint32_t rid = p1->request_ids[p1_offset + j];
                ws_request_ids[ws_total_requests++] = rid;
                if (rid < clone->num_requests) placed[rid] = 1;
            }
            ws_num_routes++;
        }
        free(perm);
        free(weights);
    }

    /* Step 2: For each P2 route, include requests NOT in placed set.
       S27c: Skip singleton P2 fragments — let construction place them
       optimally instead of inheriting weak 1-request routes. */
    offset = 0;
    for (i = 0; i < p2->num_routes; i++) {
        uint32_t rlen = p2->route_lengths[i];
        uint32_t vid = p2->vehicle_ids[i];
        uint32_t route_count = 0;
        uint32_t route_start = ws_total_requests;

        for (j = 0; j < rlen; j++) {
            uint32_t rid = p2->request_ids[offset + j];
            if (rid < clone->num_requests && !placed[rid]) {
                ws_request_ids[ws_total_requests++] = rid;
                placed[rid] = 1;
                route_count++;
            }
        }
        offset += rlen;

        if (route_count > 1) {
            ws_vehicle_ids[ws_num_routes] = vid;
            ws_route_lengths[ws_num_routes] = route_count;
            ws_num_routes++;
        } else {
            /* Singleton or empty — un-place and let construction handle */
            for (j = 0; j < route_count; j++)
                placed[ws_request_ids[route_start + j]] = 0;
            ws_total_requests = route_start;
        }
    }

    /* Set warm start on clone */
    clone->initial_route_vehicle_ids = ws_vehicle_ids;
    clone->initial_route_request_ids = ws_request_ids;
    clone->initial_route_lengths = ws_route_lengths;
    clone->num_initial_routes = ws_num_routes;
    clone->total_initial_requests = ws_total_requests;

    free(placed);
    /* Note: ws_vehicle_ids, ws_route_lengths, ws_request_ids are now owned by clone
       and must be freed after the solve completes. */
}

/* (S27b: old vehicle-assignment similarity removed, replaced by BPD above) */

/* ============================================================================
 * Public API
 * ============================================================================ */

SGStatus sg_solve_parallel(SGContext *ctx, uint32_t num_threads) {
    SGParallelWorkItem *items = NULL;
    ShWorkQueue *queue = NULL;
    ShWorkerPool *pool = NULL;
    ShWorkerPoolConfig pool_cfg;
    SGStatus status = SG_STATUS_OK;
    int best_idx = -1;
    uint32_t i;

    if (!ctx) return SG_STATUS_INVALID_ARG;

    /* Auto-detect thread count */
    if (num_threads == 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        num_threads = (n > 0 && n <= 64) ? (uint32_t)n : 4;
    }

    /* Trivial case: single thread = just call sg_solve */
    if (num_threads == 1) return sg_solve(ctx);

    /* Prepare travel on the original BEFORE cloning (one-time model mutation) */
    status = sg_prepare_travel(ctx);
    if (status != SG_STATUS_OK) return status;

    /* Validate model on the original */
    status = sg_validate_model(ctx);
    if (status != SG_STATUS_OK) return status;

    /* Reset cancel */
    atomic_store(&ctx->cancel_requested, 0);

    /* Allocate work items */
    items = (SGParallelWorkItem *)calloc(num_threads, sizeof(SGParallelWorkItem));
    if (!items) return SG_STATUS_OUT_OF_MEMORY;

    /* Create work queue and worker pool */
    queue = sh_workqueue_create(num_threads, 0);
    if (!queue) { free(items); return SG_STATUS_OUT_OF_MEMORY; }

    memset(&pool_cfg, 0, sizeof(pool_cfg));
    pool_cfg.queue = queue;
    pool_cfg.callback = sg_parallel_worker_callback;
    pool_cfg.ctx = NULL;
    pool_cfg.poll_timeout_ms = 100;

    pool = sh_worker_pool_create((int)num_threads, &pool_cfg);
    if (!pool) {
        sh_workqueue_free(queue);
        free(items);
        return SG_STATUS_OUT_OF_MEMORY;
    }

    /* Initialize clones and push work items */
    for (i = 0; i < num_threads; i++) {
        ShWorkItem wi;

        items[i].master_cancel = &ctx->cancel_requested;
        items[i].seed = ctx->config.seed + i;
        items[i].use_deterministic = ctx->config.deterministic;
        items[i].result = SG_STATUS_ERROR;

        status = sg_context_clone_init(&items[i].clone, ctx);
        if (status != SG_STATUS_OK) goto cleanup;

        sh_completion_init(&items[i].completion);

        memset(&wi, 0, sizeof(wi));
        wi.user_ctx = &items[i];
        if (!sh_workqueue_push(queue, &wi)) {
            status = SG_STATUS_ERROR;
            goto cleanup;
        }
    }

    /* Wait for all completions */
    for (i = 0; i < num_threads; i++) {
        sh_completion_wait(&items[i].completion, 0);
    }

    /* Find best result */
    for (i = 0; i < num_threads; i++) {
        if (items[i].result != SG_STATUS_OK && items[i].result != SG_STATUS_LIMIT) continue;
        if (!items[i].clone.final_solution) continue;

        if (best_idx < 0 ||
            sg_route_solution_is_better(items[i].clone.final_solution,
                                        items[best_idx].clone.final_solution, ctx)) {
            best_idx = (int)i;
        }
    }

    /* Copy best solution back to original context */
    if (best_idx >= 0) {
        if (ctx->final_solution) {
            sg_route_solution_free(ctx->final_solution, NULL);
            ctx->final_solution = NULL;
        }

        /* Enable fast arena-copy path (clone computed this, original didn't) */
        ctx->solution_arena_size = items[best_idx].clone.solution_arena_size;

        ctx->final_solution = (SGRouteSolution *)sg_route_solution_copy(
            items[best_idx].clone.final_solution, ctx);

        /* Aggregate stats: total iterations from all threads, solution from winner */
        ctx->stats = items[best_idx].clone.stats;
        {
            uint32_t total_iters = 0;
            for (i = 0; i < num_threads; i++) {
                total_iters += items[i].clone.stats.iterations;
            }
            ctx->stats.iterations = total_iters;
        }

        /* Transfer operator telemetry from winner (avoids copy) */
        free(ctx->destroy_op_stats);
        free(ctx->repair_op_stats);
        ctx->destroy_op_stats = items[best_idx].clone.destroy_op_stats;
        ctx->repair_op_stats = items[best_idx].clone.repair_op_stats;
        ctx->num_destroy_ops = items[best_idx].clone.num_destroy_ops;
        ctx->num_repair_ops = items[best_idx].clone.num_repair_ops;
        items[best_idx].clone.destroy_op_stats = NULL;
        items[best_idx].clone.repair_op_stats = NULL;

        status = items[best_idx].result;
    } else {
        status = SG_STATUS_ERROR;
    }

cleanup:
    /* Shut down pool */
    sh_worker_pool_stop(pool);
    sh_worker_pool_join(pool);
    sh_worker_pool_free(pool);
    sh_workqueue_free(queue);

    /* Free clones and completions */
    for (i = 0; i < num_threads; i++) {
        sh_completion_cleanup(&items[i].completion);
        sg_context_clone_free(&items[i].clone);
    }
    free(items);

    return status;
}

/* ============================================================================
 * Population-based search
 * ============================================================================ */

#define SG_POP_DEFAULT_GENERATIONS 3

SGStatus sg_solve_population(SGContext *ctx, const SGPopulationConfig *cfg) {
    uint32_t num_threads, pop_size, num_gens;
    double xover_frac;
    SGPopulationMember *pop_pool = NULL;
    uint32_t pop_pool_size = 0;
    SGParallelWorkItem *items = NULL;
    ShWorkQueue *queue = NULL;
    ShWorkerPool *pool = NULL;
    ShWorkerPoolConfig pool_cfg;
    SHRng *select_rng = NULL;
    SGStatus status = SG_STATUS_OK;
    SGRouteSolution *global_best = NULL;
    uint32_t total_iterations = 0;
    int orig_max_iterations;
    int orig_max_time_seconds;
    int iter_per_gen;
    int time_per_gen;
    double penalty_scale = 1.0;  /* S27f: dynamic penalty multiplier */
    uint32_t g, i;

    if (!ctx) return SG_STATUS_INVALID_ARG;

    /* Apply defaults */
    num_threads = cfg ? cfg->num_threads : 0;
    pop_size = cfg ? cfg->population_size : 0;
    num_gens = cfg ? cfg->num_generations : 0;
    xover_frac = cfg ? cfg->crossover_fraction : 0.0;
    if (xover_frac <= 0.0 || xover_frac > 1.0) xover_frac = 0.5;
    if (num_threads == 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        num_threads = (n > 0 && n <= 64) ? (uint32_t)n : 4;
    }
    if (pop_size == 0) pop_size = SG_POP_DEFAULT_POOL_SIZE;
    if (num_gens == 0) {
        /* S27e: More generations for large instances — bigger pool needs more
           iterations to explore diversity, and per-generation iteration cost
           is lower relative to total budget at large scale. */
        SGScale scale = sg_scale_from_count(ctx->num_requests);
        num_gens = (scale >= SG_SCALE_XLARGE) ? 5 : SG_POP_DEFAULT_GENERATIONS;
    }

    /* Single generation = plain parallel solve */
    if (num_gens == 1) {
        return sg_solve_parallel(ctx, num_threads);
    }

    /* Prepare travel + validate on original BEFORE cloning */
    status = sg_prepare_travel(ctx);
    if (status != SG_STATUS_OK) return status;

    status = sg_validate_model(ctx);
    if (status != SG_STATUS_OK) return status;

    /* Reset cancel */
    atomic_store(&ctx->cancel_requested, 0);

    /* Compute per-generation budget */
    orig_max_iterations = ctx->config.max_iterations;
    orig_max_time_seconds = ctx->config.max_time_seconds;
    iter_per_gen = orig_max_iterations / (int)num_gens;
    if (iter_per_gen < 1) iter_per_gen = 1;
    time_per_gen = orig_max_time_seconds > 0 ? orig_max_time_seconds / (int)num_gens : 0;

    /* Allocate population pool */
    pop_pool = (SGPopulationMember *)calloc((size_t)pop_size, sizeof(SGPopulationMember));
    if (!pop_pool) return SG_STATUS_OUT_OF_MEMORY;

    /* RNG for tournament selection */
    select_rng = sh_rng_create_default();
    if (!select_rng) { free(pop_pool); return SG_STATUS_OUT_OF_MEMORY; }
    sh_rng_seed(select_rng, ctx->config.seed + 0xBEEF);

    /* Create worker pool (persists across generations) */
    queue = sh_workqueue_create(num_threads, 0);
    if (!queue) { status = SG_STATUS_OUT_OF_MEMORY; goto pop_cleanup; }

    memset(&pool_cfg, 0, sizeof(pool_cfg));
    pool_cfg.queue = queue;
    pool_cfg.callback = sg_parallel_worker_callback;
    pool_cfg.ctx = NULL;
    pool_cfg.poll_timeout_ms = 100;

    pool = sh_worker_pool_create((int)num_threads, &pool_cfg);
    if (!pool) { status = SG_STATUS_OUT_OF_MEMORY; goto pop_cleanup; }

    /* Allocate work items (reused each generation) */
    items = (SGParallelWorkItem *)calloc(num_threads, sizeof(SGParallelWorkItem));
    if (!items) { status = SG_STATUS_OUT_OF_MEMORY; goto pop_cleanup; }

    /* Pre-compute feature-based strategy order for Gen 0 (once, before loop) */
    SGInstanceFeatures gen0_features;
    SGConstructMethod gen0_order[SG_CONSTRUCT_COUNT];
    sg_compute_instance_features(ctx, &gen0_features);
    sg_feature_strategy_order(&gen0_features, gen0_order);

    /* --- Generation loop --- */
    for (g = 0; g < num_gens; g++) {

        /* Check cancellation between generations */
        if (atomic_load(&ctx->cancel_requested)) {
            status = global_best ? SG_STATUS_LIMIT : SG_STATUS_ERROR;
            break;
        }

        /* Initialize clones and push work items */
        for (i = 0; i < num_threads; i++) {
            ShWorkItem wi;

            items[i].master_cancel = &ctx->cancel_requested;
            items[i].seed = ctx->config.seed + (uint64_t)g * num_threads + i;
            items[i].use_deterministic = ctx->config.deterministic;
            items[i].result = SG_STATUS_ERROR;

            status = sg_context_clone_init(&items[i].clone, ctx);
            if (status != SG_STATUS_OK) goto gen_cleanup;

            /* S27f: Scale penalty weights for dynamic penalty adaptation */
            if (penalty_scale != 1.0) {
                int k;
                for (k = 0; k < SG_PENALTY_COUNT; k++)
                    items[i].clone.penalty.weight[k] *= penalty_scale;
            }

            /* Set per-generation budget */
            items[i].clone.config.max_iterations = iter_per_gen;
            if (time_per_gen > 0) {
                items[i].clone.config.max_time_seconds = time_per_gen;
            }

            /* Generation 0: feature-informed round-robin for diversity.
               Generations > 0 use warm start, so construct_method stays default. */
            if (g == 0) {
                items[i].clone.construct_method = gen0_order[i % SG_CONSTRUCT_COUNT];
            }

            /* Apply generation reheat for generations > 0 */
            if (g > 0 && ctx->tune_params) {
                items[i].clone.gen_reheat_ratio = sg_tune_d(ctx,
                    ctx->tune_params->gen_reheat_ratio, 1.0);
                items[i].clone.gen_cooling_stretch = sg_tune_d(ctx,
                    ctx->tune_params->gen_cooling_stretch, 1.0);
            } else {
                items[i].clone.gen_reheat_ratio = 1.0;
                items[i].clone.gen_cooling_stretch = 1.0;
            }

            /* Warm start from population pool (generations > 0 only) */
            items[i].owns_warm_start = 0;
            if (g > 0 && pop_pool_size > 0) {
                uint32_t crossover_cutoff = (uint32_t)(xover_frac * (double)num_threads);
                uint32_t parent = sg_population_tournament_select(
                    pop_pool, pop_pool_size, select_rng, ctx->num_requests);

                if (i < crossover_cutoff && pop_pool_size >= 2) {
                    /* SREX crossover: merge two tournament-selected parents */
                    uint32_t parent2 = sg_population_tournament_select(
                        pop_pool, pop_pool_size, select_rng, ctx->num_requests);
                    /* Ensure different parents */
                    if (parent2 == parent) {
                        parent2 = (parent + 1) % pop_pool_size;
                    }
                    sg_srex_build_warm_start(&pop_pool[parent], &pop_pool[parent2],
                                              select_rng, &items[i].clone);
                    items[i].owns_warm_start = 1;
                } else {
                    /* Single-parent warm-start (current behavior) */
                    items[i].clone.initial_route_vehicle_ids = pop_pool[parent].vehicle_ids;
                    items[i].clone.initial_route_request_ids = pop_pool[parent].request_ids;
                    items[i].clone.initial_route_lengths = pop_pool[parent].route_lengths;
                    items[i].clone.num_initial_routes = pop_pool[parent].num_routes;
                    items[i].clone.total_initial_requests = pop_pool[parent].total_requests;
                }
            }

            sh_completion_init(&items[i].completion);

            memset(&wi, 0, sizeof(wi));
            wi.user_ctx = &items[i];
            if (!sh_workqueue_push(queue, &wi)) {
                status = SG_STATUS_ERROR;
                goto gen_cleanup;
            }
        }

        /* Wait for all completions */
        for (i = 0; i < num_threads; i++) {
            sh_completion_wait(&items[i].completion, 0);
        }

        /* Harvest results + S27f feasibility tracking */
        {
            uint32_t gen_feasible = 0, gen_total = 0;

            for (i = 0; i < num_threads; i++) {
                SGPopulationMember candidate;

                if (items[i].result != SG_STATUS_OK && items[i].result != SG_STATUS_LIMIT)
                    continue;
                if (!items[i].clone.final_solution) continue;

                /* Enable fast arena-copy path (clone computed this, original didn't) */
                if (ctx->solution_arena_size == 0) {
                    ctx->solution_arena_size = items[i].clone.solution_arena_size;
                }

                /* Track global best */
                if (!global_best ||
                    sg_route_solution_is_better(items[i].clone.final_solution,
                                                global_best, ctx)) {
                    if (global_best) {
                        sg_route_solution_free(global_best, NULL);
                    }
                    global_best = (SGRouteSolution *)sg_route_solution_copy(
                        items[i].clone.final_solution, ctx);
                    status = items[i].result;
                }

                /* S27f: Track feasibility of offspring */
                gen_total++;
                if (sg_solution_total_violation(items[i].clone.final_solution) < 1e-9)
                    gen_feasible++;

                /* Extract route structure into population pool */
                if (sg_extract_routes(items[i].clone.final_solution, &candidate) == SG_STATUS_OK) {
                    sg_population_insert_ex(pop_pool, &pop_pool_size, pop_size,
                                            &candidate, ctx->num_requests);
                }

                /* Accumulate iterations */
                total_iterations += (uint32_t)items[i].clone.stats.iterations;
            }

            /* S27f: Dynamic penalty adaptation between generations.
               Target 43% feasible offspring (PyVRP default).
               Increase penalties when too few feasible, decrease when too many. */
            if (gen_total > 0 && g + 1 < num_gens) {
                double feas_frac = (double)gen_feasible / (double)gen_total;
                if (feas_frac < 0.43) {
                    penalty_scale *= 1.2;
                    if (penalty_scale > 10.0) penalty_scale = 10.0;
                } else if (feas_frac > 0.43) {
                    penalty_scale *= 0.85;
                    if (penalty_scale < 0.1) penalty_scale = 0.1;
                }
            }
        }

gen_cleanup:
        /* Clear warm start pointers before freeing clones */
        for (i = 0; i < num_threads; i++) {
            if (items[i].owns_warm_start) {
                /* SREX-built arrays are owned by us */
                free(items[i].clone.initial_route_vehicle_ids);
                free(items[i].clone.initial_route_request_ids);
                free(items[i].clone.initial_route_lengths);
            }
            items[i].clone.initial_route_vehicle_ids = NULL;
            items[i].clone.initial_route_request_ids = NULL;
            items[i].clone.initial_route_lengths = NULL;
            items[i].clone.num_initial_routes = 0;
            items[i].clone.total_initial_requests = 0;
            items[i].owns_warm_start = 0;
            sh_completion_cleanup(&items[i].completion);
            sg_context_clone_free(&items[i].clone);
        }
        memset(items, 0, (size_t)num_threads * sizeof(SGParallelWorkItem));

        if (status != SG_STATUS_OK && status != SG_STATUS_LIMIT) break;
    }

    /* Transfer global best to original context */
    if (global_best) {
        if (ctx->final_solution) {
            sg_route_solution_free(ctx->final_solution, NULL);
        }
        ctx->final_solution = global_best;
        global_best = NULL;

        /* Stats: total iterations across all generations, metrics from best */
        ctx->stats.iterations = total_iterations;
        ctx->stats.total_distance = ctx->final_solution->total_distance;
        ctx->stats.vehicles_used = ctx->final_solution->vehicles_used;
        ctx->stats.unassigned = ctx->final_solution->base.num_unassigned;
        ctx->stats.total_cost = sg_route_solution_cost(ctx->final_solution, ctx);

        /* Compute aggregate stats from final solution */
        {
            uint32_t v;
            double total_waiting = 0.0, total_overtime = 0.0, total_tw_penalty = 0.0;
            for (v = 0; v < ctx->final_solution->num_vehicles; v++) {
                if (ctx->final_solution->route_waiting)
                    total_waiting += ctx->final_solution->route_waiting[v];
                if (ctx->final_solution->route_overtime)
                    total_overtime += ctx->final_solution->route_overtime[v];
                if (ctx->final_solution->route_tw_penalty)
                    total_tw_penalty += ctx->final_solution->route_tw_penalty[v];
            }
            ctx->stats.total_waiting = total_waiting;
            ctx->stats.total_overtime = total_overtime;
            ctx->stats.total_tw_penalty = total_tw_penalty;
        }

        /* Clear operator telemetry (aggregation across generations not meaningful) */
        free(ctx->destroy_op_stats);
        free(ctx->repair_op_stats);
        ctx->destroy_op_stats = NULL;
        ctx->repair_op_stats = NULL;
        ctx->num_destroy_ops = 0;
        ctx->num_repair_ops = 0;
    } else {
        status = SG_STATUS_ERROR;
    }

pop_cleanup:
    /* Shut down pool */
    if (pool) {
        sh_worker_pool_stop(pool);
        sh_worker_pool_join(pool);
        sh_worker_pool_free(pool);
    }
    if (queue) sh_workqueue_free(queue);

    /* Free population pool members */
    for (i = 0; i < pop_pool_size; i++) {
        sg_population_member_free(&pop_pool[i]);
    }
    free(pop_pool);

    if (global_best) sg_route_solution_free(global_best, NULL);
    sh_rng_free(select_rng);
    free(items);

    return status;
}

#endif /* SG_HAS_THREADS */
