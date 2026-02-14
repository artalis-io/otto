#ifndef NX_COMPUTE_H
#define NX_COMPUTE_H

/**
 * @file nx_compute.h
 * @brief Compute function registry for advanced transform engine
 *
 * Static registry of compute functions used by transform rules.
 * Each function takes N source columns (as strings) and produces M target values.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Compute function signature.
 *
 * @param sources Array of source column values (NULL-terminated strings)
 * @param nsources Number of source columns
 * @param outputs Array of output buffers (each 256 bytes)
 * @param max_outputs Maximum number of outputs allowed
 * @return Number of outputs produced, or -1 on error
 */
typedef int (*NxComputeFunc)(const char **sources, int nsources,
                             char outputs[][256], int max_outputs);

/**
 * Find a compute function by name.
 *
 * @param name Function name (e.g., "eov_to_wgs84", "coalesce")
 * @return Function pointer, or NULL if not found
 */
NxComputeFunc nx_compute_find(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* NX_COMPUTE_H */
