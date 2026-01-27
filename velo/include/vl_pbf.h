/*
 * vl_pbf.h - PBF parsing API
 *
 * Functions for parsing OpenStreetMap PBF files.
 */

#ifndef VL_PBF_H
#define VL_PBF_H

#include "vl_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Create a new PBF parsing context.
 * Returns NULL on allocation failure.
 */
VLPBFContext *vl_pbf_context_create(void);

/*
 * Free a PBF parsing context and all parsed data.
 */
void vl_pbf_context_free(VLPBFContext *ctx);

/*
 * Parse PBF data from memory.
 *
 * data: pointer to PBF data
 * len: length of data in bytes
 *
 * Returns VL_OK on success.
 */
VLStatus vl_pbf_parse(VLPBFContext *ctx, const uint8_t *data, size_t len);

/*
 * Parse PBF data from a file.
 * Uses mmap on POSIX systems for efficiency.
 *
 * filename: path to PBF file
 *
 * Returns VL_OK on success.
 */
VLStatus vl_pbf_parse_file(VLPBFContext *ctx, const char *filename);

#ifdef __cplusplus
}
#endif

#endif /* VL_PBF_H */
