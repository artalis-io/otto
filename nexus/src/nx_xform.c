/*
 * nx_xform.c - Schema-Driven Transform Engine (Stage B)
 *
 * Takes raw rows JSON + transform schema → canonical output JSON.
 *
 * Schema format:
 * {
 *   "nx_schema": 1,
 *   "version": "gls-hu-automata-v1",
 *   "output_type": "facility",
 *   "table_selector": {"index": 0},
 *   "skip_rows": 0,
 *   "columns": [
 *     {"source": 0, "target": "city", "type": "string",
 *      "transforms": ["trim"], "required": true}
 *   ],
 *   "derived": [
 *     {"target": "facility_type", "value": "parcel_automata"}
 *   ],
 *   "row_id": {"template": "gls-hu-{city}-{name}", "slugify": true}
 * }
 */

#include "nx_xform.h"
#include "nx_slug.h"
#include "sh_json.h"
#include "sh_arena.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

/* ============================================================================
 * Configuration Limits
 * ============================================================================ */

#define MAX_COLUMNS   64
#define MAX_DERIVED   32
#define MAX_TRANSFORMS 8
#define MAX_FIELD_LEN  256
#define MAX_ID_LEN     512

/* ============================================================================
 * Schema Structures (parsed from JSON)
 * ============================================================================ */

typedef enum {
    XFORM_TYPE_STRING,
    XFORM_TYPE_INT,
    XFORM_TYPE_DOUBLE,
    XFORM_TYPE_BOOL
} XformType;

typedef enum {
    XFORM_TRIM,
    XFORM_LOWERCASE,
    XFORM_UPPERCASE
} XformTransform;

typedef struct {
    int source_col;            /* Column index in raw data */
    char target[MAX_FIELD_LEN]; /* Output field name */
    XformType type;
    int precision;             /* For doubles */
    int required;
    double validate_min;
    double validate_max;
    int has_validate_min;
    int has_validate_max;
    char default_val[MAX_FIELD_LEN];
    int has_default;
    XformTransform transforms[MAX_TRANSFORMS];
    int transform_count;
} ColumnMapping;

typedef struct {
    char target[MAX_FIELD_LEN];
    char value[MAX_FIELD_LEN];
} DerivedField;

typedef struct {
    char version[MAX_FIELD_LEN];
    char output_type[MAX_FIELD_LEN];
    int table_index;
    int skip_rows;

    ColumnMapping columns[MAX_COLUMNS];
    int column_count;

    DerivedField derived[MAX_DERIVED];
    int derived_count;

    char id_template[MAX_ID_LEN];
    int id_slugify;
} XformSchema;

/* ============================================================================
 * Rejection Tracking
 * ============================================================================ */

typedef struct {
    int row;
    char field[MAX_FIELD_LEN];
    char reason[MAX_FIELD_LEN];
} Rejection;

#define MAX_REJECTIONS 1024

typedef struct {
    int rows_processed;
    int rows_accepted;
    int rows_rejected;
    Rejection rejections[MAX_REJECTIONS];
    int rejection_count;
} AuditTrail;

static void audit_reject(AuditTrail *audit, int row,
                         const char *field, const char *reason)
{
    audit->rows_rejected++;
    if (audit->rejection_count < MAX_REJECTIONS) {
        Rejection *r = &audit->rejections[audit->rejection_count++];
        r->row = row;
        snprintf(r->field, MAX_FIELD_LEN, "%s", field);
        snprintf(r->reason, MAX_FIELD_LEN, "%s", reason);
    }
}

/* ============================================================================
 * Schema Parsing
 * ============================================================================ */

static XformType parse_type(const char *s)
{
    if (!s) return XFORM_TYPE_STRING;
    if (strcmp(s, "int") == 0) return XFORM_TYPE_INT;
    if (strcmp(s, "double") == 0) return XFORM_TYPE_DOUBLE;
    if (strcmp(s, "bool") == 0) return XFORM_TYPE_BOOL;
    return XFORM_TYPE_STRING;
}

static XformTransform parse_transform(const char *s)
{
    if (!s) return XFORM_TRIM;
    if (strcmp(s, "lowercase") == 0) return XFORM_LOWERCASE;
    if (strcmp(s, "uppercase") == 0) return XFORM_UPPERCASE;
    return XFORM_TRIM;
}

static int parse_schema(const char *json, size_t len,
                        SHArena *arena, XformSchema *schema)
{
    ShJsonValue *root = NULL;
    if (sh_json_parse(json, len, arena, &root) != SH_JSON_OK)
        return -1;

    /* Version */
    const char *ver = sh_json_as_string(sh_json_get(root, "version"), "");
    snprintf(schema->version, MAX_FIELD_LEN, "%s", ver);

    /* Output type */
    const char *otype = sh_json_as_string(sh_json_get(root, "output_type"), "record");
    snprintf(schema->output_type, MAX_FIELD_LEN, "%s", otype);

    /* Table selector */
    ShJsonValue *sel = sh_json_get(root, "table_selector");
    schema->table_index = sh_json_as_int(sh_json_get(sel, "index"), 0);

    /* Skip rows */
    schema->skip_rows = sh_json_as_int(sh_json_get(root, "skip_rows"), 0);

    /* Columns */
    ShJsonValue *cols = sh_json_get(root, "columns");
    schema->column_count = 0;
    if (cols) {
        int n = (int)sh_json_array_len(cols);
        if (n > MAX_COLUMNS) n = MAX_COLUMNS;
        for (int i = 0; i < n; i++) {
            ShJsonValue *col = sh_json_array_get(cols, (size_t)i);
            ColumnMapping *cm = &schema->columns[schema->column_count++];
            memset(cm, 0, sizeof(*cm));

            cm->source_col = sh_json_as_int(sh_json_get(col, "source"), i);
            const char *target = sh_json_as_string(sh_json_get(col, "target"), "");
            snprintf(cm->target, MAX_FIELD_LEN, "%s", target);
            cm->type = parse_type(sh_json_as_string(sh_json_get(col, "type"), "string"));
            cm->precision = sh_json_as_int(sh_json_get(col, "precision"), 6);
            cm->required = sh_json_as_bool(sh_json_get(col, "required"), 0);

            /* Default */
            ShJsonValue *def = sh_json_get(col, "default");
            if (def && !sh_json_is_null(def)) {
                cm->has_default = 1;
                snprintf(cm->default_val, MAX_FIELD_LEN, "%s",
                         sh_json_as_string(def, ""));
            }

            /* Validation */
            ShJsonValue *validate = sh_json_get(col, "validate");
            if (validate) {
                ShJsonValue *vmin = sh_json_get(validate, "min");
                ShJsonValue *vmax = sh_json_get(validate, "max");
                if (vmin && sh_json_type(vmin) == SH_JSON_NUMBER) {
                    cm->has_validate_min = 1;
                    cm->validate_min = sh_json_as_double(vmin, 0);
                }
                if (vmax && sh_json_type(vmax) == SH_JSON_NUMBER) {
                    cm->has_validate_max = 1;
                    cm->validate_max = sh_json_as_double(vmax, 0);
                }
            }

            /* Transforms */
            ShJsonValue *xforms = sh_json_get(col, "transforms");
            cm->transform_count = 0;
            if (xforms) {
                int tn = (int)sh_json_array_len(xforms);
                if (tn > MAX_TRANSFORMS) tn = MAX_TRANSFORMS;
                for (int j = 0; j < tn; j++) {
                    const char *ts = sh_json_as_string(
                        sh_json_array_get(xforms, (size_t)j), "");
                    cm->transforms[cm->transform_count++] = parse_transform(ts);
                }
            }
        }
    }

    /* Derived fields */
    ShJsonValue *derived = sh_json_get(root, "derived");
    schema->derived_count = 0;
    if (derived) {
        int n = (int)sh_json_array_len(derived);
        if (n > MAX_DERIVED) n = MAX_DERIVED;
        for (int i = 0; i < n; i++) {
            ShJsonValue *d = sh_json_array_get(derived, (size_t)i);
            DerivedField *df = &schema->derived[schema->derived_count++];
            snprintf(df->target, MAX_FIELD_LEN, "%s",
                     sh_json_as_string(sh_json_get(d, "target"), ""));
            snprintf(df->value, MAX_FIELD_LEN, "%s",
                     sh_json_as_string(sh_json_get(d, "value"), ""));
        }
    }

    /* Row ID */
    ShJsonValue *row_id = sh_json_get(root, "row_id");
    schema->id_template[0] = '\0';
    schema->id_slugify = 0;
    if (row_id) {
        snprintf(schema->id_template, MAX_ID_LEN, "%s",
                 sh_json_as_string(sh_json_get(row_id, "template"), ""));
        schema->id_slugify = sh_json_as_bool(sh_json_get(row_id, "slugify"), 0);
    }

    return 0;
}

/* ============================================================================
 * String Transforms
 * ============================================================================ */

static void apply_transforms(char *buf, size_t *len,
                             const XformTransform *transforms, int count)
{
    for (int t = 0; t < count; t++) {
        switch (transforms[t]) {
        case XFORM_TRIM: {
            /* Trim leading whitespace */
            size_t start = 0;
            while (start < *len && isspace((unsigned char)buf[start]))
                start++;
            if (start > 0) {
                memmove(buf, buf + start, *len - start + 1);
                *len -= start;
            }
            /* Trim trailing whitespace */
            while (*len > 0 && isspace((unsigned char)buf[*len - 1]))
                (*len)--;
            buf[*len] = '\0';
            break;
        }
        case XFORM_LOWERCASE:
            for (size_t i = 0; i < *len; i++)
                buf[i] = (char)tolower((unsigned char)buf[i]);
            break;
        case XFORM_UPPERCASE:
            for (size_t i = 0; i < *len; i++)
                buf[i] = (char)toupper((unsigned char)buf[i]);
            break;
        }
    }
}

/* ============================================================================
 * ID Template Expansion
 * ============================================================================ */

/* Expand template like "gls-hu-{city}-{name}" using field values */
static void expand_template(const char *tmpl,
                            const char **field_names, const char **field_vals,
                            int field_count, int do_slugify,
                            char *out, size_t out_cap)
{
    size_t w = 0;
    const char *p = tmpl;

    while (*p && w < out_cap - 1) {
        if (*p == '{') {
            /* Find closing brace */
            const char *end = strchr(p + 1, '}');
            if (!end) {
                out[w++] = *p++;
                continue;
            }
            size_t name_len = (size_t)(end - p - 1);

            /* Look up field value */
            const char *val = "";
            for (int i = 0; i < field_count; i++) {
                if (strlen(field_names[i]) == name_len &&
                    memcmp(field_names[i], p + 1, name_len) == 0) {
                    val = field_vals[i];
                    break;
                }
            }

            /* Copy value */
            size_t vlen = strlen(val);
            if (w + vlen >= out_cap - 1) vlen = out_cap - 1 - w;
            memcpy(out + w, val, vlen);
            w += vlen;
            p = end + 1;
        } else {
            out[w++] = *p++;
        }
    }
    out[w] = '\0';

    /* Slugify if requested */
    if (do_slugify && w > 0) {
        char tmp[MAX_ID_LEN];
        size_t slug_len = nx_slugify(out, w, tmp, sizeof(tmp));
        if (slug_len < out_cap) {
            memcpy(out, tmp, slug_len + 1);
        }
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

const char *nx_xform_status_str(NxXformStatus status)
{
    switch (status) {
        case NX_XFORM_OK:        return "OK";
        case NX_XFORM_ERR_NULL:  return "NULL input";
        case NX_XFORM_ERR_SCHEMA: return "Invalid schema";
        case NX_XFORM_ERR_RAW:   return "Invalid raw JSON";
        case NX_XFORM_ERR_NO_TABLE: return "Selected table not found";
        case NX_XFORM_ERR_ARENA: return "Arena allocation failure";
        default:                 return "Unknown error";
    }
}

NxXformStatus nx_xform_apply(const char *raw_json, size_t raw_len,
                             const char *schema_json, size_t schema_len,
                             SHArena *arena, char **out_json, size_t *out_len)
{
    if (!raw_json || !schema_json || !out_json || !out_len)
        return NX_XFORM_ERR_NULL;
    if (!arena) return NX_XFORM_ERR_ARENA;

    *out_json = NULL;
    *out_len = 0;

    /* Parse schema */
    XformSchema schema;
    memset(&schema, 0, sizeof(schema));
    if (parse_schema(schema_json, schema_len, arena, &schema) < 0)
        return NX_XFORM_ERR_SCHEMA;

    /* Parse raw JSON */
    ShJsonValue *raw_root = NULL;
    if (sh_json_parse(raw_json, raw_len, arena, &raw_root) != SH_JSON_OK)
        return NX_XFORM_ERR_RAW;

    /* Get source SHA-256 */
    const char *source_sha = sh_json_as_string(
        sh_json_get(sh_json_get(raw_root, "source"), "sha256"), "");

    /* Select table */
    ShJsonValue *tables = sh_json_get(raw_root, "tables");
    if (!tables || sh_json_array_len(tables) == 0)
        return NX_XFORM_ERR_RAW;

    ShJsonValue *table = NULL;
    size_t ntables = sh_json_array_len(tables);
    for (size_t i = 0; i < ntables; i++) {
        ShJsonValue *t = sh_json_array_get(tables, i);
        if (sh_json_as_int(sh_json_get(t, "index"), -1) == schema.table_index) {
            table = t;
            break;
        }
    }
    if (!table) {
        /* Fall back to first table */
        table = sh_json_array_get(tables, 0);
    }
    if (!table) return NX_XFORM_ERR_NO_TABLE;

    ShJsonValue *rows = sh_json_get(table, "rows");
    if (!rows) return NX_XFORM_ERR_RAW;

    int nrows = (int)sh_json_array_len(rows);

    /* Initialize audit trail */
    AuditTrail audit;
    memset(&audit, 0, sizeof(audit));

    /* Build output JSON */
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_json_write_object_start(&w);
    sh_json_write_kv_int(&w, "nx_canonical", 1);
    sh_json_write_kv_string(&w, "schema_version", schema.version);
    sh_json_write_kv_string(&w, "source_sha256", source_sha);
    sh_json_write_kv_string(&w, "output_type", schema.output_type);

    /* Process rows → records */
    /* First pass: build records into a temporary buffer, track rejections */
    ShJsonBuf records_buf;
    sh_json_buf_init(&records_buf);
    ShJsonWriter rw;
    sh_json_writer_init(&rw, sh_json_buf_write, &records_buf);
    sh_json_write_array_start(&rw);

    int records_written = 0;

    for (int i = 0; i < nrows; i++) {
        if (i < schema.skip_rows) continue;

        ShJsonValue *row = sh_json_array_get(rows, (size_t)i);
        ShJsonValue *cells = sh_json_get(row, "cells");
        int row_num = sh_json_as_int(sh_json_get(row, "row"), i);
        int ncells = (int)sh_json_array_len(cells);

        audit.rows_processed++;

        /* Extract and validate all column values */
        char field_bufs[MAX_COLUMNS][MAX_FIELD_LEN];
        const char *field_names[MAX_COLUMNS];
        const char *field_vals[MAX_COLUMNS];
        int valid = 1;

        for (int c = 0; c < schema.column_count; c++) {
            ColumnMapping *cm = &schema.columns[c];
            field_names[c] = cm->target;

            /* Get raw cell value */
            const char *raw_val = "";
            if (cm->source_col < ncells) {
                raw_val = sh_json_as_string(
                    sh_json_array_get(cells, (size_t)cm->source_col), "");
            }

            /* Copy to mutable buffer */
            snprintf(field_bufs[c], MAX_FIELD_LEN, "%s", raw_val);
            size_t flen = strlen(field_bufs[c]);

            /* Apply transforms */
            apply_transforms(field_bufs[c], &flen,
                             cm->transforms, cm->transform_count);

            /* Check required */
            if (cm->required && flen == 0) {
                if (cm->has_default) {
                    snprintf(field_bufs[c], MAX_FIELD_LEN, "%s", cm->default_val);
                    flen = strlen(field_bufs[c]);
                } else {
                    audit_reject(&audit, row_num, cm->target,
                                 "required field empty");
                    valid = 0;
                    break;
                }
            }

            /* Apply default for empty non-required fields */
            if (flen == 0 && cm->has_default) {
                snprintf(field_bufs[c], MAX_FIELD_LEN, "%s", cm->default_val);
                flen = strlen(field_bufs[c]);
            }

            /* Type validation for numerics */
            if (flen > 0 && (cm->type == XFORM_TYPE_DOUBLE ||
                             cm->type == XFORM_TYPE_INT)) {
                char *endp;
                double dval = strtod(field_bufs[c], &endp);
                if (endp == field_bufs[c]) {
                    audit_reject(&audit, row_num, cm->target,
                                 "invalid number");
                    valid = 0;
                    break;
                }
                if (cm->has_validate_min && dval < cm->validate_min) {
                    audit_reject(&audit, row_num, cm->target,
                                 "below minimum");
                    valid = 0;
                    break;
                }
                if (cm->has_validate_max && dval > cm->validate_max) {
                    audit_reject(&audit, row_num, cm->target,
                                 "above maximum");
                    valid = 0;
                    break;
                }
            }

            field_vals[c] = field_bufs[c];
        }

        if (!valid) continue;

        audit.rows_accepted++;

        /* Write record */
        sh_json_write_object_start(&rw);

        /* Generate ID if template is set */
        if (schema.id_template[0]) {
            char id_buf[MAX_ID_LEN];
            expand_template(schema.id_template,
                            field_names, field_vals,
                            schema.column_count, schema.id_slugify,
                            id_buf, sizeof(id_buf));
            sh_json_write_kv_string(&rw, "id", id_buf);
        }

        /* Write column fields */
        for (int c = 0; c < schema.column_count; c++) {
            ColumnMapping *cm = &schema.columns[c];
            const char *val = field_vals[c];

            switch (cm->type) {
            case XFORM_TYPE_STRING:
                sh_json_write_kv_string(&rw, cm->target, val);
                break;
            case XFORM_TYPE_INT: {
                int64_t ival = (int64_t)strtol(val, NULL, 10);
                sh_json_write_kv_int(&rw, cm->target, ival);
                break;
            }
            case XFORM_TYPE_DOUBLE: {
                double dval = strtod(val, NULL);
                sh_json_write_kv_double_fmt(&rw, cm->target, dval, cm->precision);
                break;
            }
            case XFORM_TYPE_BOOL: {
                int bval = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0 ||
                            strcmp(val, "yes") == 0);
                sh_json_write_kv_bool(&rw, cm->target, bval);
                break;
            }
            }
        }

        /* Write derived fields */
        for (int d = 0; d < schema.derived_count; d++) {
            sh_json_write_kv_string(&rw, schema.derived[d].target,
                                     schema.derived[d].value);
        }

        sh_json_write_object_end(&rw);
        records_written++;
    }

    sh_json_write_array_end(&rw);

    /* Write records to main output */
    sh_json_write_key(&w, "records");
    if (records_buf.buf) {
        sh_json_write_raw(&w, records_buf.buf, records_buf.len);
    } else {
        sh_json_write_array_start(&w);
        sh_json_write_array_end(&w);
    }
    sh_json_buf_free(&records_buf);

    sh_json_write_kv_int(&w, "record_count", records_written);

    /* Audit */
    sh_json_write_key(&w, "audit");
    sh_json_write_object_start(&w);
    sh_json_write_kv_int(&w, "rows_processed", audit.rows_processed);
    sh_json_write_kv_int(&w, "rows_accepted", audit.rows_accepted);
    sh_json_write_kv_int(&w, "rows_rejected", audit.rows_rejected);

    sh_json_write_key(&w, "rejections");
    sh_json_write_array_start(&w);
    for (int i = 0; i < audit.rejection_count; i++) {
        Rejection *r = &audit.rejections[i];
        sh_json_write_object_start(&w);
        sh_json_write_kv_int(&w, "row", r->row);
        sh_json_write_kv_string(&w, "field", r->field);
        sh_json_write_kv_string(&w, "reason", r->reason);
        sh_json_write_object_end(&w);
    }
    sh_json_write_array_end(&w);

    sh_json_write_key(&w, "warnings");
    sh_json_write_array_start(&w);
    sh_json_write_array_end(&w);

    sh_json_write_object_end(&w); /* audit */

    sh_json_write_object_end(&w); /* root */

    if (sh_json_writer_error(&w) || !jb.buf) {
        sh_json_buf_free(&jb);
        return NX_XFORM_ERR_ARENA;
    }

    *out_json = sh_json_buf_take(&jb);
    if (*out_json)
        *out_len = strlen(*out_json);

    return NX_XFORM_OK;
}
