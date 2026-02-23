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

#ifdef __cplusplus
}
#endif

#endif /* SG_HAS_THREADS */
#endif /* SG_PARALLEL_H */
