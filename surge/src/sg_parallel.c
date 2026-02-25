#ifdef SG_HAS_THREADS

#include "sg_internal.h"
#include "sg_parallel.h"
#include "sh_worker_pool.h"
#include "sh_completion.h"
#include "sh_dist.h"

#include <unistd.h>

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
    memset(m, 0, sizeof(*m));
}

/* Forward declaration — defined below with SREX helpers */
static double sg_population_similarity(const SGPopulationMember *a,
                                        const SGPopulationMember *b,
                                        uint32_t num_requests);

static void sg_population_insert_ex(SGPopulationMember *pool, uint32_t *pool_size,
                                     uint32_t pool_capacity, SGPopulationMember *candidate,
                                     uint32_t num_requests) {
    double cand_score = sg_route_objective_cost(candidate->num_unassigned,
                                                 candidate->vehicles_used,
                                                 candidate->total_distance);
    uint32_t sz = *pool_size;
    uint32_t insert_pos, i;

    /* Duplicate detection: skip if same (vehicles_used, total_distance) */
    for (i = 0; i < sz; i++) {
        if (pool[i].vehicles_used == candidate->vehicles_used &&
            fabs(pool[i].total_distance - candidate->total_distance) < 1e-9 &&
            pool[i].num_unassigned == candidate->num_unassigned) {
            sg_population_member_free(candidate);
            return;
        }
    }

    /* Diversity filter: if >90% similar to any existing member AND not strictly better, reject */
    if (num_requests > 0) {
        for (i = 0; i < sz; i++) {
            double sim = sg_population_similarity(&pool[i], candidate, num_requests);
            if (sim > 0.90) {
                double existing_score = sg_route_objective_cost(pool[i].num_unassigned,
                                                                pool[i].vehicles_used,
                                                                pool[i].total_distance);
                if (cand_score >= existing_score - 1e-9) {
                    sg_population_member_free(candidate);
                    return;
                }
            }
        }
    }

    /* Find insertion position (ascending by score) */
    insert_pos = sz;
    for (i = 0; i < sz; i++) {
        double s = sg_route_objective_cost(pool[i].num_unassigned,
                                            pool[i].vehicles_used,
                                            pool[i].total_distance);
        if (cand_score < s) {
            insert_pos = i;
            break;
        }
    }

    if (sz < pool_capacity) {
        /* Room in pool — shift right and insert */
        if (insert_pos < sz) {
            memmove(&pool[insert_pos + 1], &pool[insert_pos],
                    (size_t)(sz - insert_pos) * sizeof(SGPopulationMember));
        }
        pool[insert_pos] = *candidate;
        *pool_size = sz + 1;
    } else if (insert_pos < pool_capacity) {
        /* Pool full — evict worst (last), shift right, insert */
        sg_population_member_free(&pool[pool_capacity - 1]);
        if (insert_pos < pool_capacity - 1) {
            memmove(&pool[insert_pos + 1], &pool[insert_pos],
                    (size_t)(pool_capacity - 1 - insert_pos) * sizeof(SGPopulationMember));
        }
        pool[insert_pos] = *candidate;
    } else {
        /* Candidate is worse than all in full pool */
        sg_population_member_free(candidate);
    }
}

static uint32_t sg_population_tournament_select(SGPopulationMember *pool,
                                                  uint32_t pool_size, SHRng *rng) {
    uint32_t a = sh_rng_next_u32(rng) % pool_size;
    uint32_t b = sh_rng_next_u32(rng) % pool_size;
    double sa = sg_route_objective_cost(pool[a].num_unassigned,
                                         pool[a].vehicles_used,
                                         pool[a].total_distance);
    double sb = sg_route_objective_cost(pool[b].num_unassigned,
                                         pool[b].vehicles_used,
                                         pool[b].total_distance);
    return sa <= sb ? a : b;
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

    /* Step 1: Select k random routes from P1 */
    {
        uint32_t *perm = (uint32_t *)malloc((size_t)p1->num_routes * sizeof(uint32_t));
        if (!perm) {
            free(placed);
            free(ws_vehicle_ids);
            free(ws_route_lengths);
            free(ws_request_ids);
            return;
        }
        for (i = 0; i < p1->num_routes; i++) perm[i] = i;
        /* Fisher-Yates partial shuffle for k elements */
        for (i = 0; i < k && i < p1->num_routes; i++) {
            uint32_t j2 = i + sh_rng_next_u32(rng) % (p1->num_routes - i);
            uint32_t tmp = perm[i]; perm[i] = perm[j2]; perm[j2] = tmp;
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
    }

    /* Step 2: For each P2 route, include requests NOT in placed set */
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

        if (route_count > 0) {
            ws_vehicle_ids[ws_num_routes] = vid;
            ws_route_lengths[ws_num_routes] = route_count;
            ws_num_routes++;
        } else {
            /* Roll back if no requests were added */
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

/* ============================================================================
 * Population diversity: request-to-vehicle similarity
 * ============================================================================ */

static double sg_population_similarity(const SGPopulationMember *a,
                                        const SGPopulationMember *b,
                                        uint32_t num_requests) {
    uint32_t *map_a = NULL;
    uint32_t *map_b = NULL;
    uint32_t same = 0, total_assigned = 0;
    uint32_t ri, offset, i;

    if (!a || !b || num_requests == 0) return 0.0;

    map_a = (uint32_t *)malloc((size_t)num_requests * sizeof(uint32_t));
    map_b = (uint32_t *)malloc((size_t)num_requests * sizeof(uint32_t));
    if (!map_a || !map_b) {
        free(map_a);
        free(map_b);
        return 0.0;
    }
    memset(map_a, 0xFF, (size_t)num_requests * sizeof(uint32_t));
    memset(map_b, 0xFF, (size_t)num_requests * sizeof(uint32_t));

    /* Build request→vehicle map for A */
    offset = 0;
    for (ri = 0; ri < a->num_routes; ri++) {
        uint32_t vid = a->vehicle_ids[ri];
        for (i = 0; i < a->route_lengths[ri]; i++) {
            uint32_t rid = a->request_ids[offset + i];
            if (rid < num_requests) map_a[rid] = vid;
        }
        offset += a->route_lengths[ri];
    }

    /* Build request→vehicle map for B */
    offset = 0;
    for (ri = 0; ri < b->num_routes; ri++) {
        uint32_t vid = b->vehicle_ids[ri];
        for (i = 0; i < b->route_lengths[ri]; i++) {
            uint32_t rid = b->request_ids[offset + i];
            if (rid < num_requests) map_b[rid] = vid;
        }
        offset += b->route_lengths[ri];
    }

    /* Count requests on same vehicle in both */
    for (i = 0; i < num_requests; i++) {
        if (map_a[i] != UINT32_MAX && map_b[i] != UINT32_MAX) {
            total_assigned++;
            if (map_a[i] == map_b[i]) same++;
        }
    }

    free(map_a);
    free(map_b);
    return total_assigned > 0 ? (double)same / (double)total_assigned : 0.0;
}

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

#define SG_POP_DEFAULT_POOL_SIZE   6
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
    if (num_gens == 0) num_gens = SG_POP_DEFAULT_GENERATIONS;

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

            /* Set per-generation budget */
            items[i].clone.config.max_iterations = iter_per_gen;
            if (time_per_gen > 0) {
                items[i].clone.config.max_time_seconds = time_per_gen;
            }

            /* Warm start from population pool (generations > 0 only) */
            items[i].owns_warm_start = 0;
            if (g > 0 && pop_pool_size > 0) {
                uint32_t crossover_cutoff = (uint32_t)(xover_frac * (double)num_threads);
                uint32_t parent = sg_population_tournament_select(
                    pop_pool, pop_pool_size, select_rng);

                if (i < crossover_cutoff && pop_pool_size >= 2) {
                    /* SREX crossover: merge two tournament-selected parents */
                    uint32_t parent2 = sg_population_tournament_select(
                        pop_pool, pop_pool_size, select_rng);
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

        /* Harvest results */
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
                /* Free previous global best if we own it */
                if (global_best) {
                    sg_route_solution_free(global_best, NULL);
                }
                global_best = (SGRouteSolution *)sg_route_solution_copy(
                    items[i].clone.final_solution, ctx);
                status = items[i].result;
            }

            /* Extract route structure into population pool */
            if (sg_extract_routes(items[i].clone.final_solution, &candidate) == SG_STATUS_OK) {
                sg_population_insert_ex(pop_pool, &pop_pool_size, pop_size,
                                        &candidate, ctx->num_requests);
            }

            /* Accumulate iterations */
            total_iterations += (uint32_t)items[i].clone.stats.iterations;
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
