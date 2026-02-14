/*
 * nx_issue.h - Dynamic Issue Tracker for Nexus Pipeline
 *
 * Growable issue list threaded through all pipeline stages for structured
 * error reporting. No hard caps — grows without bound via realloc.
 *
 * All stage functions accept an optional NxIssueList *issues parameter.
 * When NULL, behavior is identical to the non-instrumented version.
 * When non-NULL, issues are appended with stage/severity/row/field metadata.
 */

#ifndef NX_ISSUE_H
#define NX_ISSUE_H

#include <stddef.h>
#include "sh_json.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================ */

typedef enum {
    NX_ISSUE_INFO,
    NX_ISSUE_WARNING,
    NX_ISSUE_ERROR
} NxIssueSeverity;

typedef enum {
    NX_STAGE_A,   /* Extract (XLSX/PDF/CSV parsers) */
    NX_STAGE_M,   /* Merge (continuation rows) */
    NX_STAGE_B,   /* Transform (schema-driven) */
    NX_STAGE_X,   /* Validate (semantic rules) */
    NX_STAGE_D    /* Emit (GeoJSON/CSV output) */
} NxStage;

typedef struct {
    NxStage          stage;
    NxIssueSeverity  severity;
    int              row;           /* -1 if not row-specific */
    char             field[64];     /* "" if not field-specific */
    char             code[64];      /* Machine-readable: "cell_truncated" */
    char             message[256];  /* Human-readable description */
} NxIssue;

typedef struct {
    NxIssue *items;     /* Heap-allocated, realloc double-on-grow */
    int      count;
    int      capacity;
} NxIssueList;

/* ============================================================================
 * API
 * ============================================================================ */

/* Initialize an issue list (zero state, no allocation). */
void nx_issue_list_init(NxIssueList *list);

/* Free all memory held by the issue list. */
void nx_issue_list_free(NxIssueList *list);

/* Append an issue. Returns 0 on success, -1 on OOM. */
int nx_issue_add(NxIssueList *list, NxStage stage, NxIssueSeverity severity,
                 int row, const char *field, const char *code,
                 const char *message);

/* Append an issue with printf-style message. Returns 0 on success, -1 on OOM. */
int nx_issue_addf(NxIssueList *list, NxStage stage, NxIssueSeverity severity,
                  int row, const char *field, const char *code,
                  const char *fmt, ...) __attribute__((format(printf, 7, 8)));

/*
 * Count issues matching filters. Pass -1 for stage or severity to match any.
 */
int nx_issue_count(const NxIssueList *list, int stage, int severity);

/*
 * Write issues as JSON array using ShJsonWriter.
 * If stage_filter >= 0, only writes issues for that stage.
 */
void nx_issue_write_json(const NxIssueList *list, int stage_filter,
                         ShJsonWriter *w);

/* String conversion helpers */
const char *nx_stage_str(NxStage stage);
const char *nx_issue_severity_str(NxIssueSeverity severity);
char        nx_stage_tag(NxStage stage);

#ifdef __cplusplus
}
#endif

#endif /* NX_ISSUE_H */
