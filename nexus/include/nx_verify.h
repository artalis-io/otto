/*
 * nx_verify.h - Stage V: in-process faithfulness verification
 *
 * Cross-checks canonical output against the raw input WITHOUT re-running the
 * transform grammar (that would be tautological in the same process). It does
 * input<->output property checks that are independent of the transform code:
 *
 *   - provenance : canonical source_sha256 matches the raw source sha256
 *   - conservation: rows in == records out + Stage-B rejects (no silent drops)
 *   - lossless mapping: for columns that map a raw cell directly (source is a
 *     real column, transforms are trim-only), the emitted value must faithfully
 *     represent the raw cell. For numeric types this is a value round-trip that
 *     catches lossy casts (e.g. an int column truncating "599.9" -> 599, or a
 *     precision clamp turning 8769.627 -> 8769.63).
 *
 * Columns whose source is a multi-transform virtual column, or whose transforms
 * are not trim-only, are reported as "unverified" (never as passed) — the
 * external reconcile.py remains the full independent grammar re-execution.
 *
 * Findings are appended to the issue list under NX_STAGE_V.
 */
#ifndef NX_VERIFY_H
#define NX_VERIFY_H

#include "nx_issue.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NX_VERIFY_OK = 0,       /* checks ran; see report for pass/fail counts */
    NX_VERIFY_ERR_PARSE,    /* raw/canonical/schema JSON did not parse */
    NX_VERIFY_ERR_ARENA     /* allocation failure */
} NxVerifyStatus;

typedef struct {
    double num_rel_tol;     /* numeric round-trip tolerance; 0.0 = exact */
} NxVerifyOptions;

typedef struct {
    int provenance_ok;      /* 1 = sha matched (or absent on both) */
    int rows_checked;       /* raw rows paired with a record */
    int rows_unmatched;     /* conservation gap (kept rows != record_count) */
    int fields_verified;    /* direct 1:1 / numeric round-trip checks that passed */
    int fields_lossy;       /* numeric value lost information vs the raw cell */
    int fields_mismatch;    /* string/bool value differs from the raw cell */
    int fields_unverified;  /* transform-derived or non-trim; not checkable here */
} NxVerifyReport;

/* Set default options. */
void nx_verify_options_default(NxVerifyOptions *opts);

/*
 * Verify canonical output against raw input under a schema. Appends issues
 * (NX_STAGE_V) and fills report. Returns NX_VERIFY_OK when the checks ran (a
 * clean pass has report->fields_lossy == fields_mismatch == rows_unmatched == 0
 * and provenance_ok == 1).
 */
NxVerifyStatus nx_verify(const char *raw_json, size_t raw_len,
                         const char *canonical_json, size_t canonical_len,
                         const char *schema_json, size_t schema_len,
                         const NxVerifyOptions *opts,
                         NxIssueList *issues, NxVerifyReport *report);

/* 1 if the report represents a clean pass (no faithfulness failures). */
int nx_verify_clean(const NxVerifyReport *report);

const char *nx_verify_status_str(NxVerifyStatus s);

#ifdef __cplusplus
}
#endif

#endif /* NX_VERIFY_H */
