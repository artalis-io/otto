/*
 * nx_csv.h - CSV Parser for Nexus Ingestion Pipeline (Stage 2)
 *
 * Parses CSV/TSV data into raw rows JSON (nx_raw format).
 * Uses sh_csv for RFC 4180 parsing and sh_json for output.
 *
 * Output matches the same nx_raw JSON format as nx_xlsx and nx_pdf.
 */

#ifndef NX_CSV_H
#define NX_CSV_H

#include "sh_arena.h"
#include "sh_csv.h"
#include "nx_issue.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Status Codes
 * ============================================================================ */

typedef enum {
    NX_CSV_OK = 0,
    NX_CSV_ERR_NULL,        /* NULL input */
    NX_CSV_ERR_PARSE,       /* CSV parse error */
    NX_CSV_ERR_NO_DATA,     /* No data rows found */
    NX_CSV_ERR_ARENA        /* Arena allocation failure */
} NxCsvStatus;

/* ============================================================================
 * Configuration
 * ============================================================================ */

typedef struct {
    int max_rows;            /* Max data rows (0 = unlimited) */
    int max_cols;            /* Max columns (0 = unlimited) */
} NxCsvLimits;

#define NX_CSV_DEFAULT_LIMITS { 100000, 1000 }

/* ============================================================================
 * API
 * ============================================================================ */

/*
 * Parse CSV data into raw rows JSON.
 *
 * Output JSON follows the nx_raw format:
 * {"nx_raw":1,"source":{"filename":"...","sha256":"...","format":"csv"},
 *  "tables":[{"name":"Sheet1","index":0,"headers":[...],"rows":[...]}]}
 *
 * @param data      CSV text data
 * @param len       Length of data
 * @param csv_opts  CSV parser options (NULL for defaults with auto-detect)
 * @param limits    Row/column limits (NULL for defaults)
 * @param filename  Original filename (for metadata, may be NULL)
 * @param arena     Arena for intermediate allocations
 * @param issues    Issue list for structured reporting (NULL to skip)
 * @param out_json  Output: heap-allocated JSON string (caller must free)
 * @param out_len   Output: length of JSON string
 * @return NX_CSV_OK on success
 */
NxCsvStatus nx_csv_parse(const char *data, size_t len,
                          const ShCsvOpts *csv_opts,
                          const NxCsvLimits *limits,
                          const char *filename,
                          SHArena *arena, NxIssueList *issues,
                          char **out_json, size_t *out_len);

/*
 * Get human-readable error message.
 */
const char *nx_csv_status_str(NxCsvStatus status);

#ifdef __cplusplus
}
#endif

#endif /* NX_CSV_H */
