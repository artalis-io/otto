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
} SGParallelWorkItem;

/* ============================================================================
 * Clone lifecycle
 * ============================================================================ */

static SGStatus sg_context_clone_init(SGParallelWorkItem *item, const SGContext *src) {
    memcpy(&item->clone, src, sizeof(SGContext));

    /* Fresh RNG for this clone */
    item->clone.op_rng = sh_rng_create_default();
    if (!item->clone.op_rng) return SG_STATUS_OUT_OF_MEMORY;

    /* Clear solver-scoped mutable state */
    item->clone.final_solution = NULL;
    item->clone.active_solution = NULL;
    item->clone.destroy_op_stats = NULL;
    item->clone.repair_op_stats = NULL;
    item->clone.num_destroy_ops = 0;
    item->clone.num_repair_ops = 0;
    memset(&item->clone.stats, 0, sizeof(SGStats));
    memset(&item->clone.scratch, 0, sizeof(SGScratchBuffers));
    item->clone.solution_arena_size = 0;
    atomic_store(&item->clone.cancel_requested, 0);

    /* Progress callback replaced with cancel forwarder in worker callback */
    item->clone.progress_callback = NULL;
    item->clone.progress_callback_data = NULL;

    return SG_STATUS_OK;
}

static void sg_context_clone_free(SGParallelWorkItem *item) {
    SGContext *clone = &item->clone;

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

        status = sg_context_clone_init(&items[i], ctx);
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
        sg_context_clone_free(&items[i]);
    }
    free(items);

    return status;
}

#endif /* SG_HAS_THREADS */
