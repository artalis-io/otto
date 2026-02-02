/*
 * lc_serialize.h - Binary Index Serialization
 *
 * Save/load geocoding indexes to/from binary format for fast startup.
 * Supports mmap for near-instant loading of pre-built indexes.
 */

#ifndef LC_SERIALIZE_H
#define LC_SERIALIZE_H

#include "lc_types.h"
#include "lc_index.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* File format magic and version */
#define LC_BINARY_MAGIC    0x4C4F4355  /* "LOCU" */
#define LC_BINARY_VERSION  4           /* v4: zero-copy with geometry/ngrams */

/* ============================================================================
 * Serialization API
 * ============================================================================ */

/*
 * Save index to binary file.
 * Returns LC_OK on success.
 */
LCStatus lc_index_save(const LCIndex *index, const char *path);

/*
 * Load index from binary file.
 * Returns NULL on error.
 */
LCIndex *lc_index_load(const char *path);

/*
 * Load index using mmap for fast startup.
 * The returned index holds a reference to the mapped file.
 * Call lc_index_free() to unmap when done.
 */
LCIndex *lc_index_mmap(const char *path);

/*
 * Check if file is a valid Locus binary index.
 */
int lc_is_binary_index(const char *path);

/*
 * Get binary format version from file.
 * Returns 0 if not a valid index file.
 */
uint32_t lc_binary_version(const char *path);

/* ============================================================================
 * mmap'd Index Search (for v3/v4 indexes)
 * ============================================================================ */

/*
 * Search mmap'd trie for exact match.
 * Returns number of results written to results array.
 */
size_t lc_mmap_trie_search_exact(void *mmap_ctx, const char *name,
                                  size_t max_results, uint32_t *results);

/*
 * Search mmap'd trie for prefix match.
 * Returns number of results written to results array.
 */
size_t lc_mmap_trie_search_prefix(void *mmap_ctx, const char *prefix,
                                   size_t max_results, uint32_t *results);

/*
 * Query mmap'd grid for entities near a point.
 * Returns entity IDs in the grid cell containing coord.
 */
size_t lc_mmap_grid_query_point(void *mmap_ctx, SHCoord coord,
                                 size_t max_results, uint32_t *results);

/*
 * Find nearest entities to a coordinate (for reverse geocoding).
 * Returns number of results, sorted by distance ascending.
 */
size_t lc_mmap_grid_find_nearest(void *mmap_ctx, const LCEntityStore *store,
                                  SHCoord coord, size_t max_results,
                                  LCNearestResult *results);

/*
 * Check if index was loaded via mmap (v3 format).
 */
int lc_index_is_mmap(const LCIndex *index);

#ifdef __cplusplus
}
#endif

#endif /* LC_SERIALIZE_H */
