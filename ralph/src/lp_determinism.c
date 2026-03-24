/*
 * Ralph - LP Determinism Helpers
 *
 * Runtime policy for reproducible LP solves. Kept separate from telemetry/logging.
 */

#include <stdint.h>
#include "lp.h"

#ifdef _OPENMP
#include <omp.h>
/*
 * Process-wide default for OMP thread count (-1 = use OMP default).
 *
 * THREAD SAFETY: Plain int, but only written by lp_determinism_set_threads()
 * which should be called at startup before any solving. Per-solve control is
 * available via solver->lp_threads (see lp_determinism_effective_threads()).
 */
static int lp_default_omp_threads = -1;
#endif

/* Lightweight integer mixer (SplitMix-style finalizer) for seeded offsets. */
static uint32_t lp_determinism_mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

int lp_determinism_effective_threads(const SimplexSolver *solver) {
    if (!solver) return 0;
    if (solver->lp_threads > 0) return solver->lp_threads;
    if (solver->deterministic) return 1;
    return 0;
}

void lp_determinism_apply_runtime(SimplexSolver *solver) {
    if (!solver) return;

    int threads = lp_determinism_effective_threads(solver);
    solver->determinism_effective_threads = threads;

#ifdef _OPENMP
    if (lp_default_omp_threads <= 0) {
        int max_threads = omp_get_max_threads();
        if (max_threads > 0) lp_default_omp_threads = max_threads;
    }

    if (threads > 0) {
        omp_set_dynamic(0);
        omp_set_num_threads(threads);
    } else if (lp_default_omp_threads > 0) {
        omp_set_num_threads(lp_default_omp_threads);
    }
#else
    (void)threads;
#endif
}

unsigned int lp_determinism_seed_offset(const SimplexSolver *solver,
                                        int key,
                                        unsigned int modulus) {
    if (!solver || !solver->deterministic || solver->random_seed == 0U || modulus == 0U) {
        return 0U;
    }
    uint32_t mixed = lp_determinism_mix32((uint32_t)key ^ solver->random_seed);
    return (unsigned int)(mixed % modulus);
}
