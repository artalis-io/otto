/*
 * nx_pdf.h - PDF Table Reconstructor for Nexus Ingestion Pipeline
 *
 * Reconstructs tables from positioned text runs extracted by the
 * pdf-to-text-json.py preprocessor. Uses x/y coordinate clustering
 * to identify rows and columns.
 *
 * Input: Intermediate JSON with text runs + (x, y, w, h) coordinates
 * Output: Raw rows JSON (same nx_raw format as XLSX parser)
 */

#ifndef NX_PDF_H
#define NX_PDF_H

#include "sh_arena.h"
#include "nx_issue.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Status Codes
 * ============================================================================ */

typedef enum {
    NX_PDF_OK = 0,
    NX_PDF_ERR_NULL,           /* NULL input */
    NX_PDF_ERR_JSON,           /* Invalid JSON input */
    NX_PDF_ERR_NO_TEXT,        /* No text runs found */
    NX_PDF_ERR_ARENA           /* Arena allocation failure */
} NxPdfStatus;

/* ============================================================================
 * Configuration
 * ============================================================================ */

typedef struct {
    double row_tolerance;      /* Y-distance to group into same row (default: 3.0) */
    double col_gap_min;        /* Min X-gap to split columns (default: 10.0) */
    int header_row;            /* Row index to use as headers (-1 = auto) */
} NxPdfOptions;

#define NX_PDF_DEFAULT_OPTIONS { 3.0, 10.0, -1 }
#define NX_PDF_AUTO_OPTIONS    { -1.0, -1.0, -1 }  /* Auto-detect from text heights */

/* ============================================================================
 * API
 * ============================================================================ */

/*
 * Reconstruct tables from PDF text-run JSON.
 *
 * Input JSON format (from pdf-to-text-json.py):
 * {
 *   "pages": [
 *     {
 *       "page": 1,
 *       "width": 595.0,
 *       "height": 842.0,
 *       "texts": [
 *         {"text": "Budapest", "x": 50.0, "y": 100.0, "w": 80.0, "h": 12.0}
 *       ]
 *     }
 *   ]
 * }
 *
 * @param json_data  Text-run JSON from PDF preprocessor
 * @param json_len   Length of JSON
 * @param opts       Clustering options (NULL for auto-detect). If auto values
 *                   (negative) are used, resolved values are written back.
 * @param filename   Original PDF filename (for metadata)
 * @param arena      Arena for intermediate allocations
 * @param issues     Issue list for structured reporting (NULL to skip)
 * @param out_json   Output: heap-allocated raw rows JSON (caller must free)
 * @param out_len    Output: length of JSON
 * @return NX_PDF_OK on success
 */
NxPdfStatus nx_pdf_extract_tables(const char *json_data, size_t json_len,
                                  NxPdfOptions *opts, const char *filename,
                                  SHArena *arena, NxIssueList *issues,
                                  char **out_json, size_t *out_len);

/*
 * Get human-readable error message.
 */
const char *nx_pdf_status_str(NxPdfStatus status);

#ifdef __cplusplus
}
#endif

#endif /* NX_PDF_H */
