/*
 * nx_issue.c - Dynamic Issue Tracker Implementation
 *
 * Growable issue list using realloc doubling. Must survive across stage
 * boundaries (each stage creates/destroys its own arena), so we use
 * heap allocation rather than arena.
 */

#include "nx_issue.h"
#include "sh_json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define NX_ISSUE_MIN_CAP 32

void nx_issue_list_init(NxIssueList *list)
{
    list->items    = NULL;
    list->count    = 0;
    list->capacity = 0;
}

void nx_issue_list_free(NxIssueList *list)
{
    free(list->items);
    list->items    = NULL;
    list->count    = 0;
    list->capacity = 0;
}

static int ensure_capacity(NxIssueList *list)
{
    if (list->count < list->capacity)
        return 0;

    int new_cap = list->capacity * 2;
    if (new_cap < NX_ISSUE_MIN_CAP)
        new_cap = NX_ISSUE_MIN_CAP;

    NxIssue *new_items = realloc(list->items, (size_t)new_cap * sizeof(NxIssue));
    if (!new_items)
        return -1;

    list->items    = new_items;
    list->capacity = new_cap;
    return 0;
}

int nx_issue_add(NxIssueList *list, NxStage stage, NxIssueSeverity severity,
                 int row, const char *field, const char *code,
                 const char *message)
{
    if (!list) return -1;
    if (ensure_capacity(list) != 0) return -1;

    NxIssue *issue = &list->items[list->count++];
    issue->stage    = stage;
    issue->severity = severity;
    issue->row      = row;

    snprintf(issue->field, sizeof(issue->field), "%s", field ? field : "");
    snprintf(issue->code, sizeof(issue->code), "%s", code ? code : "");
    snprintf(issue->message, sizeof(issue->message), "%s", message ? message : "");

    return 0;
}

int nx_issue_addf(NxIssueList *list, NxStage stage, NxIssueSeverity severity,
                  int row, const char *field, const char *code,
                  const char *fmt, ...)
{
    if (!list) return -1;
    if (ensure_capacity(list) != 0) return -1;

    NxIssue *issue = &list->items[list->count++];
    issue->stage    = stage;
    issue->severity = severity;
    issue->row      = row;

    snprintf(issue->field, sizeof(issue->field), "%s", field ? field : "");
    snprintf(issue->code, sizeof(issue->code), "%s", code ? code : "");

    va_list ap;
    va_start(ap, fmt);
    if (fmt)
        vsnprintf(issue->message, sizeof(issue->message), fmt, ap);
    else
        issue->message[0] = '\0';
    va_end(ap);

    return 0;
}

int nx_issue_count(const NxIssueList *list, int stage, int severity)
{
    if (!list) return 0;

    int n = 0;
    for (int i = 0; i < list->count; i++) {
        if (stage >= 0 && (int)list->items[i].stage != stage)
            continue;
        if (severity >= 0 && (int)list->items[i].severity != severity)
            continue;
        n++;
    }
    return n;
}

void nx_issue_write_json(const NxIssueList *list, int stage_filter,
                         ShJsonWriter *w)
{
    if (!list || !w) return;

    sh_json_write_array_start(w);

    for (int i = 0; i < list->count; i++) {
        const NxIssue *issue = &list->items[i];

        if (stage_filter >= 0 && (int)issue->stage != stage_filter)
            continue;

        sh_json_write_object_start(w);
        sh_json_write_kv_string(w, "stage", nx_stage_str(issue->stage));
        sh_json_write_kv_string(w, "severity", nx_issue_severity_str(issue->severity));
        sh_json_write_kv_int(w, "row", issue->row);
        if (issue->field[0])
            sh_json_write_kv_string(w, "field", issue->field);
        sh_json_write_kv_string(w, "code", issue->code);
        sh_json_write_kv_string(w, "message", issue->message);
        sh_json_write_object_end(w);
    }

    sh_json_write_array_end(w);
}

const char *nx_stage_str(NxStage stage)
{
    switch (stage) {
        case NX_STAGE_A: return "A";
        case NX_STAGE_M: return "M";
        case NX_STAGE_B: return "B";
        case NX_STAGE_X: return "X";
        case NX_STAGE_D: return "D";
        default:         return "?";
    }
}

const char *nx_issue_severity_str(NxIssueSeverity severity)
{
    switch (severity) {
        case NX_ISSUE_INFO:    return "info";
        case NX_ISSUE_WARNING: return "warning";
        case NX_ISSUE_ERROR:   return "error";
        default:               return "unknown";
    }
}

char nx_stage_tag(NxStage stage)
{
    switch (stage) {
        case NX_STAGE_A: return 'A';
        case NX_STAGE_M: return 'M';
        case NX_STAGE_B: return 'B';
        case NX_STAGE_X: return 'X';
        case NX_STAGE_D: return 'D';
        default:         return '?';
    }
}
