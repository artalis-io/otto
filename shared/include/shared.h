/*
 * shared.h - OTTO Platform Shared Utilities
 *
 * Common utilities used across velo, carta, and ralph modules:
 * - Geographic calculations (haversine, coordinates)
 * - Bounding box operations
 * - Coordinate projections
 * - Protocol buffer encoding/decoding
 * - Zlib compression/decompression
 * - OSM PBF parsing utilities
 * - Arena and pool memory allocators
 * - Rate limiting and work queue management
 * - API client resilience (circuit breaker, backoff, retry)
 * - CORS header utilities
 */

#ifndef SHARED_H
#define SHARED_H

#include <stdint.h>
#include <stdlib.h>

#include "sh_geo.h"
#include "sh_protobuf.h"
#include "sh_inflate.h"
#include "sh_pbf.h"
#include "sh_arena.h"
#include "sh_pool.h"
#include "sh_ratelimit.h"
#include "sh_workqueue.h"
#include "sh_capacity.h"
#include "sh_args.h"
#include "sh_circuit.h"
#include "sh_backoff.h"
#include "sh_retry.h"
#include "sh_cors.h"

/* Library version */
#define SHARED_VERSION_MAJOR 1
#define SHARED_VERSION_MINOR 0
#define SHARED_VERSION_PATCH 0

/* ============================================================================
 * Memory Safety Macros
 * ============================================================================ */

/*
 * SAFE_FREE - Free pointer and set to NULL to prevent use-after-free.
 *
 * Usage:
 *   SAFE_FREE(ptr);  // Equivalent to: free(ptr); ptr = NULL;
 *
 * Benefits:
 * - Prevents use-after-free bugs (dereferencing freed pointer)
 * - Prevents double-free bugs (second free becomes free(NULL), which is safe)
 * - Makes memory state explicit to code reviewers
 */
#define SAFE_FREE(p) do { free(p); (p) = NULL; } while(0)

/*
 * SH_ARRAY_LEN - Get the number of elements in a statically-allocated array.
 *
 * WARNING: Only works with actual arrays, not pointers.
 */
#define SH_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/*
 * sh_safe_mul_size - Overflow-safe size multiplication.
 *
 * Multiplies two size_t values and stores result, checking for overflow.
 * Use this before malloc(a * b) to prevent integer overflow vulnerabilities.
 *
 * Returns: 1 on success, 0 if multiplication would overflow
 *
 * Usage:
 *   size_t alloc_size;
 *   if (!sh_safe_mul_size(count, sizeof(element), &alloc_size)) {
 *       return ERROR_OUT_OF_MEMORY;  // Would overflow
 *   }
 *   void *ptr = malloc(alloc_size);
 */
static inline int sh_safe_mul_size(size_t a, size_t b, size_t *result)
{
    if (a > 0 && b > SIZE_MAX / a) {
        return 0;  /* Overflow */
    }
    *result = a * b;
    return 1;
}

/* Version string */
const char *sh_version(void);

#endif /* SHARED_H */
