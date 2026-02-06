/*
 * ct_serialize.h - Binary Index Serialization
 *
 * Save/load Carta indexes to/from binary format for fast startup.
 * Supports mmap for near-instant loading of pre-built indexes.
 */

#ifndef CT_SERIALIZE_H
#define CT_SERIALIZE_H

#include "ct_types.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* File format magic and version */
#define CT_BINARY_MAGIC    0x43525441  /* "CRTA" */
#define CT_BINARY_VERSION  5  /* v5: Added boundary relations */

/* ============================================================================
 * Serialization API
 * ============================================================================ */

/*
 * Save PBF context (ways + R-tree) to binary file.
 * Returns CT_OK on success.
 */
CTStatus ct_index_save(const CTPBFContext *ctx, const char *path);

/*
 * Load index using mmap for fast startup.
 * The returned context holds a reference to the mapped file.
 * Call ct_free_pbf_context() to unmap when done.
 */
CTPBFContext *ct_index_mmap(const char *path);

/*
 * Check if file is a valid Carta binary index.
 */
int ct_is_binary_index(const char *path);

/*
 * Get binary format version from file.
 * Returns 0 if not a valid index file.
 */
uint32_t ct_binary_version(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* CT_SERIALIZE_H */
