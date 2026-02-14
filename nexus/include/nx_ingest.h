/*
 * nx_ingest.h - Nexus Document Ingestion Pipeline
 *
 * Five-stage pipeline for extracting tabular data from documents:
 *   Stage A: Document → Raw rows JSON (format-specific parser)
 *   Stage M: Merge continuation rows (PDF only, if schema has "row_merge")
 *   Stage B: Raw rows JSON → Canonical JSON (schema-driven transform)
 *   Stage X: Semantic validation (geo_bounds, format, unique, outlier)
 *
 * Supports XLSX, CSV, and PDF (via pre-processed text-run JSON).
 */

#ifndef NX_INGEST_H
#define NX_INGEST_H

#include <stddef.h>
#include "nx_issue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

typedef enum {
    NX_FORMAT_XLSX,
    NX_FORMAT_PDF_JSON,  /* Pre-processed PDF text-run JSON */
    NX_FORMAT_CSV        /* CSV/TSV text data */
} NxIngestFormat;

typedef enum {
    NX_INGEST_OK = 0,
    NX_INGEST_ERR_NULL,
    NX_INGEST_ERR_FORMAT,        /* Unknown/unsupported format */
    NX_INGEST_ERR_STAGE_A,       /* Raw extraction failed */
    NX_INGEST_ERR_STAGE_B,       /* Transform failed */
    NX_INGEST_ERR_STAGE_X,       /* Validation failed */
    NX_INGEST_ERR_ARENA
} NxIngestStatus;

/* ============================================================================
 * API
 * ============================================================================ */

/*
 * Run full ingestion pipeline: document → raw JSON → canonical JSON.
 *
 * @param data          Document bytes (XLSX), text-run JSON (PDF), or CSV text
 * @param len           Length of data
 * @param format        Document format
 * @param filename      Original filename (for metadata)
 * @param schema_json   Transform schema JSON (Stage B config)
 * @param schema_len    Length of schema JSON
 * @param out_raw       Output: raw rows JSON (caller must free, may be NULL to skip)
 * @param out_raw_len   Output: length of raw JSON
 * @param out_canon     Output: canonical JSON (caller must free)
 * @param out_canon_len Output: length of canonical JSON
 * @param issues        Optional issue list for cross-stage tracking (NULL = ignore)
 * @return NX_INGEST_OK on success
 */
NxIngestStatus nx_ingest(const void *data, size_t len,
                         NxIngestFormat format, const char *filename,
                         const char *schema_json, size_t schema_len,
                         char **out_raw, size_t *out_raw_len,
                         char **out_canon, size_t *out_canon_len,
                         NxIssueList *issues);

/*
 * Get human-readable error message.
 */
const char *nx_ingest_status_str(NxIngestStatus status);

#ifdef __cplusplus
}
#endif

#endif /* NX_INGEST_H */
