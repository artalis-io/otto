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
#include <regex.h>
#include "nx_compute.h"

/* ============================================================================
 * Configuration Limits
 * ============================================================================ */

#define MAX_COLUMNS          64
#define MAX_DERIVED          32
#define MAX_TRANSFORMS       8
#define MAX_FIELD_LEN        256
#define MAX_ID_LEN           512
#define MAX_VIRTUAL_COLS     32
#define MAX_MULTI_TRANSFORMS 16
#define MAX_MULTI_TARGETS    8
#define MAX_MULTI_SOURCES    8
#define MAX_CONDITIONS       8

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
    XFORM_UPPERCASE,
    XFORM_REPLACE
} XformTransform;

#define XFORM_PARAM_LEN 64

typedef struct {
    XformTransform type;
    char param_a[XFORM_PARAM_LEN]; /* For replace: "from" string */
    char param_b[XFORM_PARAM_LEN]; /* For replace: "to" string */
} XformTransformEntry;

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
    XformTransformEntry transforms[MAX_TRANSFORMS];
    int transform_count;
} ColumnMapping;

typedef struct {
    char target[MAX_FIELD_LEN];
    char value[MAX_FIELD_LEN];
} DerivedField;

/* ============================================================================
 * Multi-Transform Structures (split, merge, regex, compute, conditional)
 * ============================================================================ */

typedef enum {
    MULTI_SPLIT,
    MULTI_MERGE,
    MULTI_REGEX,
    MULTI_COMPUTE,
    MULTI_CONDITIONAL
} MultiTransformType;

typedef struct {
    char field[MAX_FIELD_LEN];
    int index;         /* Split: piece index; Regex: capture group */
    XformType type;
    int precision;
    XformTransformEntry transforms[MAX_TRANSFORMS];
    int transform_count;
} MultiTarget;

typedef struct {
    char match[MAX_FIELD_LEN];    /* Regex pattern */
    char field[MAX_FIELD_LEN];    /* Target field name */
    char value[MAX_FIELD_LEN];    /* Value (may contain {0}) */
} Condition;

typedef struct {
    MultiTransformType type;

    /* Single source (split, regex, conditional) */
    int source;

    /* Multiple sources (merge, compute) */
    int sources[MAX_MULTI_SOURCES];
    int source_count;

    /* Split */
    char delimiter[64];
    int widths[MAX_MULTI_TARGETS];
    int width_count;
    int mode_fixed_width;

    /* Merge */
    char separator[64];
    char merge_template[MAX_FIELD_LEN];
    int has_template;

    /* Regex */
    char pattern[MAX_FIELD_LEN];

    /* Compute */
    char function[MAX_FIELD_LEN];

    /* Conditional */
    Condition conditions[MAX_CONDITIONS];
    int condition_count;

    /* Targets */
    MultiTarget targets[MAX_MULTI_TARGETS];
    int target_count;

    /* Virtual column base index (assigned during parsing) */
    int virtual_base;
} MultiTransform;

typedef struct {
    char version[MAX_FIELD_LEN];
    char output_type[MAX_FIELD_LEN];
    int table_index;
    int skip_rows;

    ColumnMapping columns[MAX_COLUMNS];
    int column_count;

    DerivedField derived[MAX_DERIVED];
    int derived_count;

    MultiTransform multi[MAX_MULTI_TRANSFORMS];
    int multi_count;
    int virtual_col_count;

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

/* Forward declaration (defined in String Transforms section below) */
static void apply_transforms(char *buf, size_t *len,
                             const XformTransformEntry *transforms, int count);

/* ============================================================================
 * Multi-Transform Parsing
 * ============================================================================ */

static int parse_multi_target_transforms(ShJsonValue *t, MultiTarget *tgt)
{
    ShJsonValue *txf = sh_json_get(t, "transforms");
    if (!txf) return 0;
    int tn = (int)sh_json_array_len(txf);
    if (tn > MAX_TRANSFORMS) tn = MAX_TRANSFORMS;
    for (int k = 0; k < tn; k++) {
        ShJsonValue *tv = sh_json_array_get(txf, (size_t)k);
        if (sh_json_type(tv) == SH_JSON_STRING) {
            tgt->transforms[tgt->transform_count].type =
                parse_transform(sh_json_as_string(tv, ""));
            tgt->transform_count++;
        }
    }
    return tgt->transform_count;
}

static void parse_multi_targets(ShJsonValue *mt, MultiTransform *m)
{
    ShJsonValue *targets = sh_json_get(mt, "targets");
    if (!targets) return;
    m->target_count = (int)sh_json_array_len(targets);
    if (m->target_count > MAX_MULTI_TARGETS)
        m->target_count = MAX_MULTI_TARGETS;
    for (int j = 0; j < m->target_count; j++) {
        ShJsonValue *t = sh_json_array_get(targets, (size_t)j);
        MultiTarget *tgt = &m->targets[j];
        memset(tgt, 0, sizeof(*tgt));
        snprintf(tgt->field, MAX_FIELD_LEN, "%s",
                 sh_json_as_string(sh_json_get(t, "field"), ""));
        tgt->index = sh_json_as_int(sh_json_get(t, "index"), j);
        /* "group" for regex targets (overrides index) */
        ShJsonValue *grp = sh_json_get(t, "group");
        if (grp) tgt->index = sh_json_as_int(grp, j);
        tgt->type = parse_type(
            sh_json_as_string(sh_json_get(t, "type"), "string"));
        tgt->precision = sh_json_as_int(sh_json_get(t, "precision"), 6);
        parse_multi_target_transforms(t, tgt);
    }
}

static int parse_multi_transforms(ShJsonValue *arr, XformSchema *schema)
{
    if (!arr) return 0;
    int n = (int)sh_json_array_len(arr);
    if (n > MAX_MULTI_TRANSFORMS) n = MAX_MULTI_TRANSFORMS;

    int virtual_base = 0;

    for (int i = 0; i < n; i++) {
        ShJsonValue *mt = sh_json_array_get(arr, (size_t)i);
        MultiTransform *m = &schema->multi[schema->multi_count];
        memset(m, 0, sizeof(*m));

        const char *type_str = sh_json_as_string(sh_json_get(mt, "type"), "");

        if (strcmp(type_str, "split") == 0) {
            m->type = MULTI_SPLIT;
            m->source = sh_json_as_int(sh_json_get(mt, "source"), 0);
            const char *mode = sh_json_as_string(
                sh_json_get(mt, "mode"), "delimiter");
            if (strcmp(mode, "fixed_width") == 0) {
                m->mode_fixed_width = 1;
                ShJsonValue *widths = sh_json_get(mt, "widths");
                if (widths) {
                    m->width_count = (int)sh_json_array_len(widths);
                    if (m->width_count > MAX_MULTI_TARGETS)
                        m->width_count = MAX_MULTI_TARGETS;
                    for (int j = 0; j < m->width_count; j++)
                        m->widths[j] = sh_json_as_int(
                            sh_json_array_get(widths, (size_t)j), 0);
                }
            } else {
                snprintf(m->delimiter, sizeof(m->delimiter), "%s",
                         sh_json_as_string(sh_json_get(mt, "delimiter"), ","));
            }
        } else if (strcmp(type_str, "merge") == 0) {
            m->type = MULTI_MERGE;
            ShJsonValue *sources = sh_json_get(mt, "sources");
            if (sources) {
                m->source_count = (int)sh_json_array_len(sources);
                if (m->source_count > MAX_MULTI_SOURCES)
                    m->source_count = MAX_MULTI_SOURCES;
                for (int j = 0; j < m->source_count; j++)
                    m->sources[j] = sh_json_as_int(
                        sh_json_array_get(sources, (size_t)j), 0);
            }
            snprintf(m->separator, sizeof(m->separator), "%s",
                     sh_json_as_string(sh_json_get(mt, "separator"), " "));
            ShJsonValue *tmpl = sh_json_get(mt, "template");
            if (tmpl && sh_json_type(tmpl) == SH_JSON_STRING) {
                m->has_template = 1;
                snprintf(m->merge_template, MAX_FIELD_LEN, "%s",
                         sh_json_as_string(tmpl, ""));
            }
        } else if (strcmp(type_str, "regex") == 0) {
            m->type = MULTI_REGEX;
            m->source = sh_json_as_int(sh_json_get(mt, "source"), 0);
            snprintf(m->pattern, MAX_FIELD_LEN, "%s",
                     sh_json_as_string(sh_json_get(mt, "pattern"), ""));
        } else if (strcmp(type_str, "compute") == 0) {
            m->type = MULTI_COMPUTE;
            snprintf(m->function, MAX_FIELD_LEN, "%s",
                     sh_json_as_string(sh_json_get(mt, "function"), ""));
            ShJsonValue *sources = sh_json_get(mt, "sources");
            if (sources) {
                m->source_count = (int)sh_json_array_len(sources);
                if (m->source_count > MAX_MULTI_SOURCES)
                    m->source_count = MAX_MULTI_SOURCES;
                for (int j = 0; j < m->source_count; j++)
                    m->sources[j] = sh_json_as_int(
                        sh_json_array_get(sources, (size_t)j), 0);
            }
        } else if (strcmp(type_str, "conditional") == 0) {
            m->type = MULTI_CONDITIONAL;
            m->source = sh_json_as_int(sh_json_get(mt, "source"), 0);
            ShJsonValue *conds = sh_json_get(mt, "conditions");
            if (conds) {
                m->condition_count = (int)sh_json_array_len(conds);
                if (m->condition_count > MAX_CONDITIONS)
                    m->condition_count = MAX_CONDITIONS;
                for (int j = 0; j < m->condition_count; j++) {
                    ShJsonValue *cond = sh_json_array_get(conds, (size_t)j);
                    Condition *c = &m->conditions[j];
                    snprintf(c->match, MAX_FIELD_LEN, "%s",
                             sh_json_as_string(sh_json_get(cond, "match"), ".*"));
                    ShJsonValue *set = sh_json_get(cond, "set");
                    if (set) {
                        snprintf(c->field, MAX_FIELD_LEN, "%s",
                                 sh_json_as_string(sh_json_get(set, "field"), ""));
                        snprintf(c->value, MAX_FIELD_LEN, "%s",
                                 sh_json_as_string(sh_json_get(set, "value"), ""));
                    }
                }
            }
        } else {
            continue; /* Unknown type, skip */
        }

        /* Parse targets array */
        parse_multi_targets(mt, m);

        /* For merge without explicit targets: single output from "target" key */
        if (m->type == MULTI_MERGE && m->target_count == 0) {
            m->target_count = 1;
            snprintf(m->targets[0].field, MAX_FIELD_LEN, "%s",
                     sh_json_as_string(sh_json_get(mt, "target"), ""));
            m->targets[0].type = parse_type(
                sh_json_as_string(sh_json_get(mt, "target_type"), "string"));
        }

        /* For conditional without explicit targets: single output */
        if (m->type == MULTI_CONDITIONAL && m->target_count == 0) {
            m->target_count = 1;
            if (m->condition_count > 0)
                snprintf(m->targets[0].field, MAX_FIELD_LEN, "%s",
                         m->conditions[0].field);
        }

        m->virtual_base = virtual_base;
        virtual_base += m->target_count;
        schema->multi_count++;
    }

    schema->virtual_col_count = virtual_base;
    return 0;
}

/* ============================================================================
 * Multi-Transform Execution
 * ============================================================================ */

/*
 * Get cell value from original cells or prior virtual columns.
 */
static const char *get_cell(const char **cells, int ncells,
                            char virtual_vals[][MAX_FIELD_LEN],
                            int total_virtual, int idx)
{
    if (idx < ncells) return cells[idx];
    int vi = idx - ncells;
    if (vi >= 0 && vi < total_virtual) return virtual_vals[vi];
    return "";
}

/*
 * Execute all multi-transforms on a row, producing virtual column values.
 *
 * @param multis        Array of multi-transforms from schema
 * @param multi_count   Number of multi-transforms
 * @param cells         Original cell string pointers
 * @param ncells        Number of original cells
 * @param virtual_vals  Output: virtual column values [MAX_VIRTUAL_COLS][MAX_FIELD_LEN]
 * @return Number of virtual columns produced
 */
static int execute_multi_transforms(const MultiTransform *multis, int multi_count,
                                     const char **cells, int ncells,
                                     char virtual_vals[][MAX_FIELD_LEN])
{
    int total_virtual = 0;

    for (int mi = 0; mi < multi_count; mi++) {
        const MultiTransform *mt = &multis[mi];

        switch (mt->type) {
        case MULTI_SPLIT: {
            const char *src = get_cell(cells, ncells, virtual_vals,
                                       total_virtual, mt->source);

            if (mt->mode_fixed_width) {
                /* Fixed-width split */
                size_t offset = 0;
                size_t slen = strlen(src);
                for (int t = 0; t < mt->target_count; t++) {
                    int vi = mt->virtual_base + t;
                    if (vi >= MAX_VIRTUAL_COLS) break;

                    int w = (t < mt->width_count) ? mt->widths[t] : 0;
                    if (w <= 0 || offset >= slen) {
                        virtual_vals[vi][0] = '\0';
                    } else {
                        size_t copy_len = (size_t)w;
                        if (offset + copy_len > slen)
                            copy_len = slen - offset;
                        if (copy_len >= MAX_FIELD_LEN)
                            copy_len = MAX_FIELD_LEN - 1;
                        memcpy(virtual_vals[vi], src + offset, copy_len);
                        virtual_vals[vi][copy_len] = '\0';
                    }
                    offset += (size_t)w;

                    /* Apply per-target transforms */
                    size_t vlen = strlen(virtual_vals[vi]);
                    apply_transforms(virtual_vals[vi], &vlen,
                                     mt->targets[t].transforms,
                                     mt->targets[t].transform_count);
                }
            } else {
                /* Delimiter split */
                size_t dlen = strlen(mt->delimiter);
                const char *p = src;
                int piece = 0;

                /* Initialize all targets to empty */
                for (int t = 0; t < mt->target_count; t++) {
                    int vi = mt->virtual_base + t;
                    if (vi < MAX_VIRTUAL_COLS)
                        virtual_vals[vi][0] = '\0';
                }

                while (*p) {
                    const char *next = (dlen > 0) ? strstr(p, mt->delimiter) : NULL;
                    size_t frag_len = next ? (size_t)(next - p) : strlen(p);

                    /* Find target that wants this piece */
                    for (int t = 0; t < mt->target_count; t++) {
                        if (mt->targets[t].index == piece) {
                            int vi = mt->virtual_base + t;
                            if (vi < MAX_VIRTUAL_COLS) {
                                if (frag_len >= MAX_FIELD_LEN)
                                    frag_len = MAX_FIELD_LEN - 1;
                                memcpy(virtual_vals[vi], p, frag_len);
                                virtual_vals[vi][frag_len] = '\0';

                                /* Apply per-target transforms */
                                size_t vlen = frag_len;
                                apply_transforms(virtual_vals[vi], &vlen,
                                                 mt->targets[t].transforms,
                                                 mt->targets[t].transform_count);
                            }
                            break;
                        }
                    }

                    piece++;
                    if (!next) break;
                    p = next + dlen;
                }
            }
            break;
        }

        case MULTI_MERGE: {
            int vi = mt->virtual_base;
            if (vi >= MAX_VIRTUAL_COLS) break;

            if (mt->has_template) {
                /* Template merge: replace {0}, {1}, ... with source values */
                char result[MAX_FIELD_LEN];
                size_t w = 0;
                const char *p = mt->merge_template;
                while (*p && w < MAX_FIELD_LEN - 1) {
                    if (*p == '{' && p[1] >= '0' && p[1] <= '9') {
                        int idx = p[1] - '0';
                        p += 2;
                        if (*p == '}') p++;

                        const char *val = "";
                        if (idx < mt->source_count)
                            val = get_cell(cells, ncells, virtual_vals,
                                           total_virtual, mt->sources[idx]);
                        size_t vl = strlen(val);
                        if (w + vl >= MAX_FIELD_LEN) vl = MAX_FIELD_LEN - 1 - w;
                        memcpy(result + w, val, vl);
                        w += vl;
                    } else {
                        result[w++] = *p++;
                    }
                }
                result[w] = '\0';
                snprintf(virtual_vals[vi], MAX_FIELD_LEN, "%s", result);
            } else {
                /* Separator merge */
                size_t w = 0;
                size_t sep_len = strlen(mt->separator);
                for (int s = 0; s < mt->source_count; s++) {
                    const char *val = get_cell(cells, ncells, virtual_vals,
                                               total_virtual, mt->sources[s]);
                    size_t vl = strlen(val);
                    if (s > 0 && w + sep_len < MAX_FIELD_LEN - 1) {
                        memcpy(virtual_vals[vi] + w, mt->separator, sep_len);
                        w += sep_len;
                    }
                    if (w + vl >= MAX_FIELD_LEN) vl = MAX_FIELD_LEN - 1 - w;
                    memcpy(virtual_vals[vi] + w, val, vl);
                    w += vl;
                }
                virtual_vals[vi][w] = '\0';
            }
            break;
        }

        case MULTI_REGEX: {
            const char *src = get_cell(cells, ncells, virtual_vals,
                                       total_virtual, mt->source);

            /* Initialize targets to empty */
            for (int t = 0; t < mt->target_count; t++) {
                int vi = mt->virtual_base + t;
                if (vi < MAX_VIRTUAL_COLS)
                    virtual_vals[vi][0] = '\0';
            }

            regex_t re;
            if (regcomp(&re, mt->pattern, REG_EXTENDED) == 0) {
                regmatch_t matches[10];
                if (regexec(&re, src, 10, matches, 0) == 0) {
                    for (int t = 0; t < mt->target_count; t++) {
                        int grp = mt->targets[t].index;
                        if (grp >= 0 && grp < 10 && matches[grp].rm_so >= 0) {
                            int vi = mt->virtual_base + t;
                            if (vi >= MAX_VIRTUAL_COLS) continue;

                            size_t mlen = (size_t)(matches[grp].rm_eo -
                                                   matches[grp].rm_so);
                            if (mlen >= MAX_FIELD_LEN) mlen = MAX_FIELD_LEN - 1;
                            memcpy(virtual_vals[vi],
                                   src + matches[grp].rm_so, mlen);
                            virtual_vals[vi][mlen] = '\0';

                            /* Apply per-target transforms */
                            apply_transforms(virtual_vals[vi], &mlen,
                                             mt->targets[t].transforms,
                                             mt->targets[t].transform_count);
                        }
                    }
                }
                regfree(&re);
            }
            break;
        }

        case MULTI_COMPUTE: {
            /* Gather source values */
            const char *src_vals[MAX_MULTI_SOURCES];
            for (int s = 0; s < mt->source_count; s++)
                src_vals[s] = get_cell(cells, ncells, virtual_vals,
                                       total_virtual, mt->sources[s]);

            /* Initialize targets to empty */
            for (int t = 0; t < mt->target_count; t++) {
                int vi = mt->virtual_base + t;
                if (vi < MAX_VIRTUAL_COLS)
                    virtual_vals[vi][0] = '\0';
            }

            NxComputeFunc fn = nx_compute_find(mt->function);
            if (fn) {
                char outputs[MAX_MULTI_TARGETS][256];
                int nout = fn(src_vals, mt->source_count,
                              outputs, mt->target_count);
                for (int t = 0; t < nout && t < mt->target_count; t++) {
                    int vi = mt->virtual_base + t;
                    if (vi < MAX_VIRTUAL_COLS)
                        snprintf(virtual_vals[vi], MAX_FIELD_LEN,
                                 "%s", outputs[t]);
                }
            }
            break;
        }

        case MULTI_CONDITIONAL: {
            int vi = mt->virtual_base;
            if (vi >= MAX_VIRTUAL_COLS) break;
            virtual_vals[vi][0] = '\0';

            const char *src = get_cell(cells, ncells, virtual_vals,
                                       total_virtual, mt->source);

            for (int c = 0; c < mt->condition_count; c++) {
                regex_t re;
                if (regcomp(&re, mt->conditions[c].match,
                            REG_EXTENDED | REG_NOSUB) == 0) {
                    int matched = (regexec(&re, src, 0, NULL, 0) == 0);
                    regfree(&re);

                    if (matched) {
                        const char *val = mt->conditions[c].value;
                        if (strstr(val, "{0}")) {
                            /* Replace {0} with source value */
                            char result[MAX_FIELD_LEN];
                            size_t w = 0;
                            const char *p = val;
                            while (*p && w < MAX_FIELD_LEN - 1) {
                                if (p[0] == '{' && p[1] == '0' && p[2] == '}') {
                                    size_t slen = strlen(src);
                                    if (w + slen >= MAX_FIELD_LEN)
                                        slen = MAX_FIELD_LEN - 1 - w;
                                    memcpy(result + w, src, slen);
                                    w += slen;
                                    p += 3;
                                } else {
                                    result[w++] = *p++;
                                }
                            }
                            result[w] = '\0';
                            snprintf(virtual_vals[vi], MAX_FIELD_LEN,
                                     "%s", result);
                        } else {
                            snprintf(virtual_vals[vi], MAX_FIELD_LEN,
                                     "%s", val);
                        }
                        break;
                    }
                }
            }
            break;
        }
        }

        total_virtual = mt->virtual_base + mt->target_count;
    }

    return total_virtual;
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

            /* Transforms (string or object) */
            ShJsonValue *xforms = sh_json_get(col, "transforms");
            cm->transform_count = 0;
            if (xforms) {
                int tn = (int)sh_json_array_len(xforms);
                if (tn > MAX_TRANSFORMS) tn = MAX_TRANSFORMS;
                for (int j = 0; j < tn; j++) {
                    ShJsonValue *tv = sh_json_array_get(xforms, (size_t)j);
                    XformTransformEntry *te = &cm->transforms[cm->transform_count];
                    memset(te, 0, sizeof(*te));

                    if (sh_json_type(tv) == SH_JSON_STRING) {
                        /* Simple string transform: "trim", "lowercase", "uppercase" */
                        te->type = parse_transform(sh_json_as_string(tv, ""));
                        cm->transform_count++;
                    } else if (sh_json_type(tv) == SH_JSON_OBJECT) {
                        /* Parameterized transform: {"replace": ["~", ""]} */
                        ShJsonValue *rep = sh_json_get(tv, "replace");
                        if (rep && sh_json_array_len(rep) >= 2) {
                            te->type = XFORM_REPLACE;
                            snprintf(te->param_a, XFORM_PARAM_LEN, "%s",
                                     sh_json_as_string(sh_json_array_get(rep, 0), ""));
                            snprintf(te->param_b, XFORM_PARAM_LEN, "%s",
                                     sh_json_as_string(sh_json_array_get(rep, 1), ""));
                            cm->transform_count++;
                        }
                    }
                }
            }
        }
    }

    /* Multi-transforms (Phase 2A) */
    schema->multi_count = 0;
    schema->virtual_col_count = 0;
    parse_multi_transforms(sh_json_get(root, "multi_transforms"), schema);

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
                             const XformTransformEntry *transforms, int count)
{
    for (int t = 0; t < count; t++) {
        switch (transforms[t].type) {
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
        case XFORM_REPLACE: {
            const char *from = transforms[t].param_a;
            const char *to = transforms[t].param_b;
            size_t from_len = strlen(from);
            size_t to_len = strlen(to);
            if (from_len == 0) break;

            /* Simple in-place replace (first occurrence only for safety) */
            char *pos = strstr(buf, from);
            int replace_iter = 0;
            while (pos && replace_iter < 1000) {
                replace_iter++;
                size_t offset = (size_t)(pos - buf);
                size_t tail_len = *len - offset - from_len;

                if (to_len != from_len) {
                    /* Check buffer overflow */
                    if (*len - from_len + to_len >= MAX_FIELD_LEN) break;
                    memmove(pos + to_len, pos + from_len, tail_len + 1);
                }
                memcpy(pos, to, to_len);
                *len = *len - from_len + to_len;
                buf[*len] = '\0';
                pos = strstr(pos + to_len, from);
            }
            break;
        }
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

    /* Per-row buffers hoisted outside loop to reduce stack pressure (~24 KB) */
    char virtual_vals[MAX_VIRTUAL_COLS][MAX_FIELD_LEN];
    char field_bufs[MAX_COLUMNS][MAX_FIELD_LEN];
    const char *field_names[MAX_COLUMNS];
    const char *field_vals[MAX_COLUMNS];
    const char *cell_strs[MAX_COLUMNS + MAX_VIRTUAL_COLS];

    for (int i = 0; i < nrows; i++) {
        if (i < schema.skip_rows) continue;

        ShJsonValue *row = sh_json_array_get(rows, (size_t)i);
        ShJsonValue *cells = sh_json_get(row, "cells");
        int row_num = sh_json_as_int(sh_json_get(row, "row"), i);
        int ncells = (int)sh_json_array_len(cells);

        audit.rows_processed++;

        /* Build flat array of original cell strings */
        int total_cols = ncells;
        if (total_cols > MAX_COLUMNS) total_cols = MAX_COLUMNS;
        for (int j = 0; j < total_cols; j++)
            cell_strs[j] = sh_json_as_string(
                sh_json_array_get(cells, (size_t)j), "");

        /* Execute multi-transforms → virtual columns */
        if (schema.multi_count > 0) {
            int nv = execute_multi_transforms(
                schema.multi, schema.multi_count,
                cell_strs, total_cols, virtual_vals);
            for (int v = 0; v < nv &&
                 total_cols + v < MAX_COLUMNS + MAX_VIRTUAL_COLS; v++)
                cell_strs[total_cols + v] = virtual_vals[v];
            total_cols += nv;
        }

        /* Extract and validate all column values */
        int valid = 1;

        for (int c = 0; c < schema.column_count; c++) {
            ColumnMapping *cm = &schema.columns[c];
            field_names[c] = cm->target;

            /* Get raw cell value (from original or virtual columns) */
            const char *raw_val = "";
            if (cm->source_col < total_cols) {
                raw_val = cell_strs[cm->source_col];
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
