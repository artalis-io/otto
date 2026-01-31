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
#define LC_BINARY_VERSION  2           /* v2: mmap-friendly format */

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

#ifdef __cplusplus
}
#endif

#endif /* LC_SERIALIZE_H */
