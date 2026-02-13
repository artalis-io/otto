/*
 * nx_ingest.c - Pipeline Orchestrator
 *
 * Runs the two-stage pipeline: Stage A (extraction) → Stage B (transform).
 */

#include "nx_ingest.h"
#include "nx_xlsx.h"
#include "nx_xform.h"
#include "sh_arena.h"
#include <stdlib.h>
#include <string.h>

/* Arena size for ingestion (4 MB) */
#define INGEST_ARENA_SIZE (4 * 1024 * 1024)

const char *nx_ingest_status_str(NxIngestStatus status)
{
    switch (status) {
        case NX_INGEST_OK:          return "OK";
        case NX_INGEST_ERR_NULL:    return "NULL input";
        case NX_INGEST_ERR_FORMAT:  return "Unsupported format";
        case NX_INGEST_ERR_STAGE_A: return "Stage A (extraction) failed";
        case NX_INGEST_ERR_STAGE_B: return "Stage B (transform) failed";
        case NX_INGEST_ERR_ARENA:   return "Arena allocation failure";
        default:                    return "Unknown error";
    }
}

NxIngestStatus nx_ingest(const void *data, size_t len,
                         NxIngestFormat format, const char *filename,
                         const char *schema_json, size_t schema_len,
                         char **out_raw, size_t *out_raw_len,
                         char **out_canon, size_t *out_canon_len)
{
    char *raw_json = NULL;
    size_t raw_len = 0;

    if (!data || !out_canon || !out_canon_len)
        return NX_INGEST_ERR_NULL;

    if (out_raw) *out_raw = NULL;
    if (out_raw_len) *out_raw_len = 0;
    *out_canon = NULL;
    *out_canon_len = 0;

    /* Stage A: Extract raw rows */
    SHArena *arena_a = sh_arena_create(INGEST_ARENA_SIZE);
    if (!arena_a) return NX_INGEST_ERR_ARENA;

    switch (format) {
    case NX_FORMAT_XLSX: {
        NxXlsxStatus xs = nx_xlsx_parse(data, len, NULL, filename,
                                         arena_a, &raw_json, &raw_len);
        if (xs != NX_XLSX_OK) {
            sh_arena_free(arena_a);
            return NX_INGEST_ERR_STAGE_A;
        }
        break;
    }
    case NX_FORMAT_PDF_JSON:
        /* PDF text-run JSON is already in raw-ish format;
         * for now, pass through to Stage B */
        raw_json = (char *)malloc(len + 1);
        if (!raw_json) {
            sh_arena_free(arena_a);
            return NX_INGEST_ERR_ARENA;
        }
        memcpy(raw_json, data, len);
        raw_json[len] = '\0';
        raw_len = len;
        break;
    default:
        sh_arena_free(arena_a);
        return NX_INGEST_ERR_FORMAT;
    }

    sh_arena_free(arena_a); /* Stage A arena freed before Stage B */

    /* Stage B: Transform */
    if (schema_json && schema_len > 0) {
        SHArena *arena_b = sh_arena_create(INGEST_ARENA_SIZE);
        if (!arena_b) {
            free(raw_json);
            return NX_INGEST_ERR_ARENA;
        }

        NxXformStatus ts = nx_xform_apply(raw_json, raw_len,
                                           schema_json, schema_len,
                                           arena_b, out_canon, out_canon_len);
        sh_arena_free(arena_b);

        if (ts != NX_XFORM_OK) {
            free(raw_json);
            return NX_INGEST_ERR_STAGE_B;
        }
    }

    /* Return raw JSON if requested */
    if (out_raw && out_raw_len) {
        *out_raw = raw_json;
        *out_raw_len = raw_len;
    } else {
        free(raw_json);
    }

    return NX_INGEST_OK;
}
