/*
 * nx_diff.c - Change Detection Between Runs
 *
 * Compares two canonical JSON documents by matching records on "id"
 * field and detecting content changes via FNV-1a hashing.
 *
 * Algorithm:
 * 1. Parse old canonical JSON, extract records array
 * 2. Build SHHashmapI64: FNV-1a(id) → record index
 * 3. Compute content hash for each old record
 * 4. Parse new canonical JSON, extract records array
 * 5. For each new record: look up id in old hashmap
 *    - Not found: added
 *    - Found, same content hash: unchanged
 *    - Found, different content hash: modified
 * 6. Old records not seen: removed
 * 7. Emit diff JSON
 */

#include "nx_diff.h"
#include "sh_json.h"
#include "sh_hashmap.h"
#include "sh_hash.h"
#include <string.h>
#include <stdlib.h>

/* Hash a string to int64_t key (avoid 0 sentinel for SHHashmapI64) */
static int64_t hash_str_key(const char *s)
{
    uint64_t h = sh_fnv1a_64_str(s);
    int64_t key = (int64_t)(h | 1); /* Ensure non-zero */
    return key;
}

/* Hash a JSON value recursively for content comparison */
static uint64_t hash_json_value(const ShJsonValue *v)
{
    if (!v) return SH_FNV1A_64_OFFSET;
    uint64_t h = SH_FNV1A_64_OFFSET;
    uint8_t tag = (uint8_t)v->type;
    h ^= tag;
    h *= SH_FNV1A_64_PRIME;

    switch (v->type) {
    case SH_JSON_NULL:
        break;
    case SH_JSON_BOOL:
        h ^= v->u.bool_val ? 1u : 0u;
        h *= SH_FNV1A_64_PRIME;
        break;
    case SH_JSON_NUMBER: {
        uint64_t bits;
        memcpy(&bits, &v->u.num_val, sizeof(bits));
        h ^= sh_fnv1a_64(&bits, sizeof(bits));
        break;
    }
    case SH_JSON_STRING:
        h ^= sh_fnv1a_64(v->u.string_val.str, v->u.string_val.len);
        break;
    case SH_JSON_ARRAY:
        for (size_t i = 0; i < v->u.array_val.count; i++) {
            h ^= hash_json_value(v->u.array_val.items[i]);
            h *= SH_FNV1A_64_PRIME;
        }
        break;
    case SH_JSON_OBJECT:
        for (size_t i = 0; i < v->u.object_val.count; i++) {
            h ^= sh_fnv1a_64(v->u.object_val.members[i].key,
                        v->u.object_val.members[i].key_len);
            h *= SH_FNV1A_64_PRIME;
            h ^= hash_json_value(v->u.object_val.members[i].value);
            h *= SH_FNV1A_64_PRIME;
        }
        break;
    }
    return h;
}

/* Hash a record's content, excluding the "id" field */
static uint64_t hash_record_content(const ShJsonValue *record)
{
    if (!record || record->type != SH_JSON_OBJECT) return 0;
    uint64_t h = SH_FNV1A_64_OFFSET;
    for (size_t i = 0; i < record->u.object_val.count; i++) {
        ShJsonMember *m = &record->u.object_val.members[i];
        if (m->key_len == 2 && memcmp(m->key, "id", 2) == 0) continue;
        h ^= sh_fnv1a_64(m->key, m->key_len);
        h *= SH_FNV1A_64_PRIME;
        h ^= hash_json_value(m->value);
        h *= SH_FNV1A_64_PRIME;
    }
    return h;
}

/* ============================================================================
 * JSON Value Serializer (ShJsonValue → ShJsonWriter)
 * ============================================================================ */

static void write_json_value(ShJsonWriter *w, const ShJsonValue *v)
{
    if (!v || v->type == SH_JSON_NULL) {
        sh_json_write_null(w);
        return;
    }
    switch (v->type) {
    case SH_JSON_BOOL:
        sh_json_write_bool(w, v->u.bool_val);
        break;
    case SH_JSON_NUMBER:
        sh_json_write_double(w, v->u.num_val);
        break;
    case SH_JSON_STRING:
        sh_json_write_string(w, v->u.string_val.str);
        break;
    case SH_JSON_ARRAY:
        sh_json_write_array_start(w);
        for (size_t i = 0; i < v->u.array_val.count; i++)
            write_json_value(w, v->u.array_val.items[i]);
        sh_json_write_array_end(w);
        break;
    case SH_JSON_OBJECT:
        sh_json_write_object_start(w);
        for (size_t i = 0; i < v->u.object_val.count; i++) {
            sh_json_write_key(w, v->u.object_val.members[i].key);
            write_json_value(w, v->u.object_val.members[i].value);
        }
        sh_json_write_object_end(w);
        break;
    default:
        sh_json_write_null(w);
        break;
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

const char *nx_diff_status_str(NxDiffStatus status)
{
    switch (status) {
    case NX_DIFF_OK:        return "OK";
    case NX_DIFF_ERR_NULL:  return "NULL input";
    case NX_DIFF_ERR_PARSE: return "JSON parse error";
    case NX_DIFF_ERR_ARENA: return "Arena allocation failure";
    default:                return "Unknown error";
    }
}

NxDiffStatus nx_diff(const char *old_json, size_t old_len,
                     const char *new_json, size_t new_len,
                     SHArena *arena,
                     char **out_json, size_t *out_len)
{
    if (!old_json || !new_json || !out_json || !out_len)
        return NX_DIFF_ERR_NULL;
    if (!arena) return NX_DIFF_ERR_ARENA;

    *out_json = NULL;
    *out_len = 0;

    /* Parse both canonical JSONs */
    ShJsonValue *old_root = NULL, *new_root = NULL;
    if (sh_json_parse(old_json, old_len, arena, &old_root) != SH_JSON_OK)
        return NX_DIFF_ERR_PARSE;
    if (sh_json_parse(new_json, new_len, arena, &new_root) != SH_JSON_OK)
        return NX_DIFF_ERR_PARSE;

    ShJsonValue *old_records = sh_json_get(old_root, "records");
    ShJsonValue *new_records = sh_json_get(new_root, "records");
    if (!old_records || !new_records) return NX_DIFF_ERR_PARSE;

    size_t old_count = sh_json_array_len(old_records);
    size_t new_count = sh_json_array_len(new_records);

    /* Build hashmap from old records: FNV-1a(id) → index */
    SHHashmapI64 *old_map = sh_hashmap_i64_create(old_count > 0 ? old_count : 1);
    if (!old_map) return NX_DIFF_ERR_ARENA;

    /* Content hashes and seen flags for old records */
    uint64_t *old_hashes = NULL;
    uint8_t *old_seen = NULL;
    if (old_count > 0) {
        old_hashes = (uint64_t *)sh_arena_alloc(arena,
            old_count * sizeof(uint64_t));
        old_seen = (uint8_t *)sh_arena_calloc(arena,
            old_count, sizeof(uint8_t));
        if (!old_hashes || !old_seen) {
            sh_hashmap_i64_free(old_map);
            return NX_DIFF_ERR_ARENA;
        }
    }

    for (size_t i = 0; i < old_count; i++) {
        ShJsonValue *rec = sh_json_array_get(old_records, i);
        const char *id = sh_json_as_string(sh_json_get(rec, "id"), "");
        if (!id[0]) continue;

        int64_t key = hash_str_key(id);
        old_hashes[i] = hash_record_content(rec);
        sh_hashmap_i64_insert(old_map, key, i);
    }

    /* Build diff arrays in separate buffers */
    ShJsonBuf added_buf, removed_buf, modified_buf;
    sh_json_buf_init(&added_buf);
    sh_json_buf_init(&removed_buf);
    sh_json_buf_init(&modified_buf);

    ShJsonWriter aw, rw, mw;
    sh_json_writer_init(&aw, sh_json_buf_write, &added_buf);
    sh_json_writer_init(&rw, sh_json_buf_write, &removed_buf);
    sh_json_writer_init(&mw, sh_json_buf_write, &modified_buf);

    sh_json_write_array_start(&aw);
    sh_json_write_array_start(&rw);
    sh_json_write_array_start(&mw);

    int n_added = 0, n_modified = 0, n_unchanged = 0;

    /* Process new records */
    for (size_t i = 0; i < new_count; i++) {
        ShJsonValue *rec = sh_json_array_get(new_records, i);
        const char *id = sh_json_as_string(sh_json_get(rec, "id"), "");
        if (!id[0]) continue;

        int64_t key = hash_str_key(id);
        size_t old_idx = sh_hashmap_i64_lookup(old_map, key);

        if (old_idx == SIZE_MAX) {
            /* Added */
            write_json_value(&aw, rec);
            n_added++;
        } else {
            old_seen[old_idx] = 1;
            uint64_t new_hash = hash_record_content(rec);

            if (new_hash == old_hashes[old_idx]) {
                n_unchanged++;
            } else {
                /* Modified */
                ShJsonValue *old_rec = sh_json_array_get(old_records, old_idx);
                sh_json_write_object_start(&mw);
                sh_json_write_kv_string(&mw, "id", id);
                sh_json_write_key(&mw, "old");
                write_json_value(&mw, old_rec);
                sh_json_write_key(&mw, "new");
                write_json_value(&mw, rec);
                sh_json_write_object_end(&mw);
                n_modified++;
            }
        }
    }

    /* Find removed records */
    int n_removed = 0;
    for (size_t i = 0; i < old_count; i++) {
        if (old_seen && !old_seen[i]) {
            ShJsonValue *rec = sh_json_array_get(old_records, i);
            const char *id = sh_json_as_string(sh_json_get(rec, "id"), "");
            if (!id[0]) continue;
            write_json_value(&rw, rec);
            n_removed++;
        }
    }

    sh_json_write_array_end(&aw);
    sh_json_write_array_end(&rw);
    sh_json_write_array_end(&mw);

    /* Build output JSON */
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_json_write_object_start(&w);
    sh_json_write_kv_int(&w, "nx_diff", 1);

    sh_json_write_key(&w, "summary");
    sh_json_write_object_start(&w);
    sh_json_write_kv_int(&w, "added", n_added);
    sh_json_write_kv_int(&w, "removed", n_removed);
    sh_json_write_kv_int(&w, "modified", n_modified);
    sh_json_write_kv_int(&w, "unchanged", n_unchanged);
    sh_json_write_object_end(&w);

    sh_json_write_key(&w, "added");
    if (added_buf.buf)
        sh_json_write_raw(&w, added_buf.buf, added_buf.len);
    else {
        sh_json_write_array_start(&w);
        sh_json_write_array_end(&w);
    }

    sh_json_write_key(&w, "removed");
    if (removed_buf.buf)
        sh_json_write_raw(&w, removed_buf.buf, removed_buf.len);
    else {
        sh_json_write_array_start(&w);
        sh_json_write_array_end(&w);
    }

    sh_json_write_key(&w, "modified");
    if (modified_buf.buf)
        sh_json_write_raw(&w, modified_buf.buf, modified_buf.len);
    else {
        sh_json_write_array_start(&w);
        sh_json_write_array_end(&w);
    }

    sh_json_write_object_end(&w);

    /* Cleanup */
    sh_json_buf_free(&added_buf);
    sh_json_buf_free(&removed_buf);
    sh_json_buf_free(&modified_buf);
    sh_hashmap_i64_free(old_map);

    if (sh_json_writer_error(&w) || !jb.buf) {
        sh_json_buf_free(&jb);
        return NX_DIFF_ERR_ARENA;
    }

    *out_json = sh_json_buf_take(&jb);
    if (*out_json)
        *out_len = strlen(*out_json);

    return NX_DIFF_OK;
}
