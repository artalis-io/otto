/*
 * nx_diff.h - Change Detection Between Runs
 *
 * Compares two canonical JSON documents (nx_canonical format) and
 * produces a structured diff report. Records are matched by their
 * "id" field (from row_id template).
 *
 * Output categories:
 *   added:     id exists in new but not old
 *   removed:   id exists in old but not new
 *   modified:  id exists in both but content differs
 *   unchanged: id exists in both with identical content
 */

#ifndef NX_DIFF_H
#define NX_DIFF_H

#include "sh_arena.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NX_DIFF_OK = 0,
    NX_DIFF_ERR_NULL,
    NX_DIFF_ERR_PARSE,
    NX_DIFF_ERR_ARENA
} NxDiffStatus;

/*
 * Compare two canonical JSON documents and produce a diff report.
 *
 * Output JSON format:
 *   {
 *     "nx_diff": 1,
 *     "summary": {"added": N, "removed": N, "modified": N, "unchanged": N},
 *     "added":    [{...record...}, ...],
 *     "removed":  [{...record...}, ...],
 *     "modified": [{"id": "...", "old": {...}, "new": {...}}, ...]
 *   }
 *
 * @param old_json  Previous run's canonical JSON
 * @param old_len   Length of old JSON
 * @param new_json  Current run's canonical JSON
 * @param new_len   Length of new JSON
 * @param arena     Arena for intermediate allocations
 * @param out_json  Output diff JSON (malloc'd, caller frees)
 * @param out_len   Output JSON length
 */
NxDiffStatus nx_diff(const char *old_json, size_t old_len,
                     const char *new_json, size_t new_len,
                     SHArena *arena,
                     char **out_json, size_t *out_len);

const char *nx_diff_status_str(NxDiffStatus status);

#ifdef __cplusplus
}
#endif

#endif /* NX_DIFF_H */
