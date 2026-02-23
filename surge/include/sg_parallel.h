#ifndef SG_PARALLEL_H
#define SG_PARALLEL_H

#ifdef SG_HAS_THREADS

#include "surge.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Solve the model using multiple independent runs with different seeds.
 * Each run gets seed = config.seed + i (i = 0..num_threads-1).
 * The best solution (lexicographic: unassigned -> vehicles -> distance) is
 * retained on ctx, accessible via the normal sg_solution_get_* API.
 *
 * @param ctx         Fully built model context
 * @param num_threads Number of parallel threads (0 = auto-detect CPU count)
 * @return SG_STATUS_OK, SG_STATUS_LIMIT, or error
 *
 * Thread safety: ctx must not be accessed from other threads during this call.
 * Cancel: sg_cancel(ctx) from another thread cancels all parallel runs.
 * Progress: The user's progress callback is NOT called during parallel solve.
 *           After return, stats contain aggregate iterations from all threads
 *           plus solution metrics from the winning run.
 */
SGStatus sg_solve_parallel(SGContext *ctx, uint32_t num_threads);

typedef struct {
    uint32_t num_threads;       /* 0 = auto-detect */
    uint32_t population_size;   /* elite pool capacity (0 = default 6) */
    uint32_t num_generations;   /* 0 = default 3 */
} SGPopulationConfig;

/*
 * Population-based search: runs multiple generations of parallel ALNS.
 * Each generation warm-starts from elite solutions found in prior generations.
 * Same total compute budget as sg_solve_parallel (iterations/time split across
 * generations), but guided search typically finds better solutions.
 *
 * @param ctx  Fully built model context
 * @param cfg  Population config (NULL = all defaults: auto threads, pool=6, gen=3)
 * @return SG_STATUS_OK, SG_STATUS_LIMIT, or error
 *
 * Thread safety / cancel / progress: same as sg_solve_parallel.
 */
SGStatus sg_solve_population(SGContext *ctx, const SGPopulationConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif /* SG_HAS_THREADS */
#endif /* SG_PARALLEL_H */
