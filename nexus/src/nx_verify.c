/*
 * nx_verify.c - Stage V: in-process faithfulness verification
 *
 * See nx_verify.h. Does input<->output property checks (provenance,
 * conservation, lossless direct-mapping) without re-running the transform
 * grammar, so it stays independent of the transform code it checks.
 */
#include "nx_verify.h"
#include "sh_json.h"
#include "sh_arena.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#define NX_VERIFY_ARENA (16 * 1024 * 1024)
#define DEFAULT_NUM_REL_TOL 1e-9   /* tighter than %g round-trip noise, catches any real precision loss */

typedef enum { T_STRING, T_INT, T_DOUBLE, T_BOOL } ColType;

void nx_verify_options_default(NxVerifyOptions *opts)
{
    if (opts) opts->num_rel_tol = DEFAULT_NUM_REL_TOL;
}

const char *nx_verify_status_str(NxVerifyStatus s)
{
    switch (s) {
        case NX_VERIFY_OK:        return "ok";
        case NX_VERIFY_ERR_PARSE: return "parse error";
        case NX_VERIFY_ERR_ARENA: return "arena error";
        default:                  return "unknown";
    }
}

int nx_verify_clean(const NxVerifyReport *r)
{
    if (!r) return 0;
    return r->provenance_ok && r->fields_lossy == 0 &&
           r->fields_mismatch == 0 && r->rows_unmatched == 0;
}

static ColType parse_type(const char *s)
{
    if (strcmp(s, "int") == 0)    return T_INT;
    if (strcmp(s, "double") == 0) return T_DOUBLE;
    if (strcmp(s, "bool") == 0)   return T_BOOL;
    return T_STRING;
}

/* True if transforms is absent/empty or contains only the "trim" op — the only
 * column transform we replicate (whitespace strip, unambiguous). Anything else
 * (replace, unknown) makes the column not directly verifiable. */
static int transforms_trim_only(ShJsonValue *transforms)
{
    if (!transforms || sh_json_type(transforms) != SH_JSON_ARRAY) return 1;
    size_t n = sh_json_array_len(transforms);
    for (size_t i = 0; i < n; i++) {
        ShJsonValue *t = sh_json_array_get(transforms, i);
        if (sh_json_type(t) != SH_JSON_STRING) return 0;          /* object (replace) */
        if (strcmp(sh_json_as_string(t, ""), "trim") != 0) return 0;
    }
    return 1;
}

/* Trim to a [start,len) view of str (isspace both ends), matching nx_xform. */
static const char *trim_view(const char *str, size_t *out_len)
{
    const char *p = str ? str : "";
    while (*p && isspace((unsigned char)*p)) p++;
    const char *e = p + strlen(p);
    while (e > p && isspace((unsigned char)e[-1])) e--;
    *out_len = (size_t)(e - p);
    return p;
}

NxVerifyStatus nx_verify(const char *raw_json, size_t raw_len,
                         const char *canonical_json, size_t canonical_len,
                         const char *schema_json, size_t schema_len,
                         const NxVerifyOptions *opts,
                         NxIssueList *issues, NxVerifyReport *report)
{
    NxVerifyReport rep;
    memset(&rep, 0, sizeof(rep));
    rep.provenance_ok = 1;
    double tol = (opts && opts->num_rel_tol > 0) ? opts->num_rel_tol : DEFAULT_NUM_REL_TOL;

    SHArena *arena = sh_arena_create(NX_VERIFY_ARENA);
    if (!arena) return NX_VERIFY_ERR_ARENA;

    ShJsonValue *raw = NULL, *canon = NULL, *schema = NULL;
    if (sh_json_parse(raw_json, raw_len, arena, &raw) != SH_JSON_OK ||
        sh_json_parse(canonical_json, canonical_len, arena, &canon) != SH_JSON_OK ||
        sh_json_parse(schema_json, schema_len, arena, &schema) != SH_JSON_OK) {
        sh_arena_free(arena);
        return NX_VERIFY_ERR_PARSE;
    }

    /* --- provenance: canonical source_sha256 == raw source.sha256 --- */
    const char *raw_sha = sh_json_as_string(
        sh_json_get(sh_json_get(raw, "source"), "sha256"), "");
    const char *can_sha = sh_json_as_string(sh_json_get(canon, "source_sha256"), "");
    if (raw_sha[0] && can_sha[0]) {
        if (strcmp(raw_sha, can_sha) != 0) {
            rep.provenance_ok = 0;
            nx_issue_add(issues, NX_STAGE_V, NX_ISSUE_ERROR, -1, "", "provenance",
                         "canonical source_sha256 does not match raw input");
        }
    } else {
        nx_issue_add(issues, NX_STAGE_V, NX_ISSUE_INFO, -1, "", "provenance",
                     "source sha256 absent; provenance not checked");
    }

    ShJsonValue *tables = sh_json_get(raw, "tables");
    ShJsonValue *t0 = tables ? sh_json_array_get(tables, 0) : NULL;
    ShJsonValue *rows = t0 ? sh_json_get(t0, "rows") : NULL;
    ShJsonValue *records = sh_json_get(canon, "records");
    ShJsonValue *columns = sh_json_get(schema, "columns");
    if (!rows || !records || !columns) {
        if (report) *report = rep;
        sh_arena_free(arena);
        return NX_VERIFY_OK;   /* nothing to pair; provenance still reported */
    }

    /* number of real source columns (virtual multi-transform columns are >= this) */
    ShJsonValue *headers = t0 ? sh_json_get(t0, "headers") : NULL;
    int ncols = headers ? (int)sh_json_array_len(headers) : 0;
    if (ncols == 0 && sh_json_array_len(rows) > 0) {
        ShJsonValue *c0 = sh_json_get(sh_json_array_get(rows, 0), "cells");
        ncols = c0 ? (int)sh_json_array_len(c0) : 0;
    }

    /* --- reject set (Stage-B rejections carry the raw row number) --- */
    ShJsonValue *rej = sh_json_get(sh_json_get(canon, "audit"), "rejections");
    size_t nrej = rej ? sh_json_array_len(rej) : 0;
    int *reject_rows = NULL;
    if (nrej > 0) {
        reject_rows = sh_arena_calloc(arena, nrej, sizeof(int));
        for (size_t i = 0; i < nrej && reject_rows; i++)
            reject_rows[i] = sh_json_as_int(sh_json_get(sh_json_array_get(rej, i), "row"), -1);
    }

    /* --- pre-classify columns: which are directly verifiable --- */
    size_t ncol = sh_json_array_len(columns);
    for (size_t c = 0; c < ncol; c++) {
        ShJsonValue *col = sh_json_array_get(columns, c);
        int src = sh_json_as_int(sh_json_get(col, "source"), -1);
        const char *target = sh_json_as_string(sh_json_get(col, "target"), "");
        int verifiable = (src >= 0 && src < ncols) &&
                         transforms_trim_only(sh_json_get(col, "transforms"));
        if (!verifiable) {
            rep.fields_unverified++;
            nx_issue_addf(issues, NX_STAGE_V, NX_ISSUE_INFO, -1, target, "unverified",
                          "%s not independently verified (transform-derived or non-trim mapping)",
                          target[0] ? target : "(field)");
        }
    }

    /* --- pair kept raw rows with records (in order) and check each field --- */
    size_t nrows = sh_json_array_len(rows);
    size_t nrec = sh_json_array_len(records);
    size_t rec_i = 0;
    for (size_t r = 0; r < nrows && rec_i < nrec; r++) {
        ShJsonValue *row = sh_json_array_get(rows, r);
        int rownum = sh_json_as_int(sh_json_get(row, "row"), (int)r);

        int rejected = 0;
        for (size_t k = 0; k < nrej; k++)
            if (reject_rows && reject_rows[k] == rownum) { rejected = 1; break; }
        if (rejected) continue;

        ShJsonValue *cells = sh_json_get(row, "cells");
        ShJsonValue *record = sh_json_array_get(records, rec_i);
        rec_i++;
        rep.rows_checked++;

        for (size_t c = 0; c < ncol; c++) {
            ShJsonValue *col = sh_json_array_get(columns, c);
            int src = sh_json_as_int(sh_json_get(col, "source"), -1);
            if (src < 0 || src >= ncols) continue;                 /* virtual: unverified */
            if (!transforms_trim_only(sh_json_get(col, "transforms"))) continue;

            const char *target = sh_json_as_string(sh_json_get(col, "target"), "");
            ColType type = parse_type(sh_json_as_string(sh_json_get(col, "type"), "string"));

            const char *raw_cell = sh_json_as_string(
                cells ? sh_json_array_get(cells, (size_t)src) : NULL, "");
            size_t vlen = 0;
            const char *v = trim_view(raw_cell, &vlen);
            ShJsonValue *got = sh_json_get(record, target);

            if (type == T_INT || type == T_DOUBLE) {
                double expect = 0.0;
                if (vlen > 0) {
                    char buf[64];
                    if (vlen >= sizeof(buf)) continue;             /* implausible number; skip */
                    memcpy(buf, v, vlen); buf[vlen] = '\0';
                    char *endp = NULL;
                    expect = strtod(buf, &endp);
                    if (endp == buf) continue;                     /* unparseable; nx would have rejected */
                }
                double gv = sh_json_as_double(got, NAN);
                if (isnan(gv)) {
                    rep.fields_mismatch++;
                    nx_issue_addf(issues, NX_STAGE_V, NX_ISSUE_ERROR, rownum, target,
                                  "missing", "%s: numeric field missing in output", target);
                    continue;
                }
                double lim = tol * fmax(1.0, fabs(expect));
                if (fabs(gv - expect) > lim) {
                    rep.fields_lossy++;
                    nx_issue_addf(issues, NX_STAGE_V, NX_ISSUE_ERROR, rownum, target,
                                  "lossy", "%s: emitted %.10g loses raw %.10g", target, gv, expect);
                } else {
                    rep.fields_verified++;
                }
            } else if (type == T_BOOL) {
                int expect = (vlen == 4 && strncmp(v, "true", 4) == 0) ||
                             (vlen == 1 && v[0] == '1') ||
                             (vlen == 3 && strncmp(v, "yes", 3) == 0);
                int gb = sh_json_as_bool(got, false) ? 1 : 0;
                if (gb != expect) {
                    rep.fields_mismatch++;
                    nx_issue_addf(issues, NX_STAGE_V, NX_ISSUE_ERROR, rownum, target,
                                  "mismatch", "%s: emitted bool differs from raw", target);
                } else {
                    rep.fields_verified++;
                }
            } else { /* string */
                const char *gs = sh_json_as_string(got, NULL);
                if (!gs) {
                    /* an empty-string default is legitimate; only flag a non-empty raw */
                    if (vlen > 0) {
                        rep.fields_mismatch++;
                        nx_issue_addf(issues, NX_STAGE_V, NX_ISSUE_ERROR, rownum, target,
                                      "missing", "%s: string field missing in output", target);
                    } else {
                        rep.fields_verified++;
                    }
                } else if (strlen(gs) == vlen && memcmp(gs, v, vlen) == 0) {
                    rep.fields_verified++;
                } else {
                    rep.fields_mismatch++;
                    nx_issue_addf(issues, NX_STAGE_V, NX_ISSUE_ERROR, rownum, target,
                                  "mismatch", "%s: emitted %.80s != raw cell", target, gs);
                }
            }
        }
    }

    /* --- conservation: kept raw rows should equal the record count --- */
    if (rec_i != nrec) {
        rep.rows_unmatched = (int)(nrec > rec_i ? nrec - rec_i : rec_i - nrec);
        nx_issue_addf(issues, NX_STAGE_V, NX_ISSUE_WARNING, -1, "", "conservation",
                      "row/record count mismatch: paired %zu, records %zu "
                      "(Stage X may have removed records)", rec_i, nrec);
    }

    if (report) *report = rep;
    sh_arena_free(arena);
    return NX_VERIFY_OK;
}
