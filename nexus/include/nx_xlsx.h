/*
 * nx_xlsx.h - XLSX Parser for Nexus Ingestion Pipeline
 *
 * Parses XLSX files (ZIP containers of XML) into raw rows JSON.
 * Uses miniz for ZIP extraction and sh_xml for XML parsing.
 *
 * XLSX files contain:
 *   xl/sharedStrings.xml  - String table referenced by index
 *   xl/workbook.xml       - Sheet names and order
 *   xl/worksheets/sheet*.xml - Cell data with row/col references
 *
 * All cell values are extracted as strings (no type interpretation).
 * Output matches the nx_raw JSON format for Stage B transform.
 */

#ifndef NX_XLSX_H
#define NX_XLSX_H

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
    NX_XLSX_OK = 0,
    NX_XLSX_ERR_NULL,           /* NULL input */
    NX_XLSX_ERR_ZIP,            /* Invalid ZIP / not an XLSX */
    NX_XLSX_ERR_NO_SHEETS,      /* No worksheets found */
    NX_XLSX_ERR_XML,            /* XML parse error */
    NX_XLSX_ERR_ARENA,          /* Arena allocation failure */
    NX_XLSX_ERR_LIMITS,         /* Exceeds configured limits */
    NX_XLSX_ERR_EMPTY           /* File parsed but contains no data rows */
} NxXlsxStatus;

/* ============================================================================
 * Configuration
 * ============================================================================ */

typedef struct {
    int max_rows;           /* Max rows per sheet (0 = unlimited) */
    int max_cols;           /* Max columns per sheet (0 = unlimited) */
    int max_sheets;         /* Max sheets to parse (0 = unlimited) */
    size_t max_file_size;   /* Max input file size (0 = unlimited) */
    int max_shared_strings; /* Max shared string table entries (0 = unlimited) */
} NxXlsxLimits;

/* Default limits for safety */
#define NX_XLSX_DEFAULT_LIMITS { 100000, 1000, 100, 100 * 1024 * 1024, 1000000 }

/* ============================================================================
 * API
 * ============================================================================ */

/*
 * Parse XLSX file bytes into raw rows JSON.
 *
 * Output JSON follows the nx_raw format:
 * {"nx_raw":1,"source":{"filename":"...","sha256":"...","format":"xlsx"},
 *  "tables":[{"name":"Sheet1","index":0,"headers":[...],"rows":[...]}]}
 *
 * @param data      XLSX file bytes
 * @param len       Length of data
 * @param limits    Parsing limits (NULL for defaults)
 * @param filename  Original filename (for metadata, may be NULL)
 * @param arena     Arena for intermediate allocations
 * @param issues    Issue list for structured reporting (NULL to skip)
 * @param out_json  Output: heap-allocated JSON string (caller must free)
 * @param out_len   Output: length of JSON string
 * @return NX_XLSX_OK on success
 */
NxXlsxStatus nx_xlsx_parse(const void *data, size_t len,
                           const NxXlsxLimits *limits, const char *filename,
                           SHArena *arena, NxIssueList *issues,
                           char **out_json, size_t *out_len);

/*
 * Get human-readable error message.
 */
const char *nx_xlsx_status_str(NxXlsxStatus status);

#ifdef __cplusplus
}
#endif

#endif /* NX_XLSX_H */
