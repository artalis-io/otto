/*
 * nx_validate.c - Semantic Validation Engine (Stage X)
 *
 * Post-transform validation that runs on canonical JSON. Applies rules
 * from schema's "validate" section and produces validation audit.
 */

#include "nx_validate.h"
#include "sh_json.h"
#include "sh_arena.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <regex.h>
#include <ctype.h>
#include <stdint.h>

/* ============================================================================
 * Configuration Limits
 * ============================================================================ */

#define NX_MAX_VALIDATION_RULES   32
#define NX_MAX_VALIDATION_DETAILS 1024
#define NX_MAX_UNIQUE_FIELDS      8
#define NX_MAX_FIELD_NAME_LEN     64
#define NX_MAX_PATTERN_LEN        256
#define NX_MAX_MESSAGE_LEN        512
#define NX_MAX_VALUE_LEN          256

/* ============================================================================
 * Validation Rule Types
 * ============================================================================ */

typedef enum {
    NX_RULE_GEO_BOUNDS,
    NX_RULE_FORMAT,
    NX_RULE_UNIQUE,
    NX_RULE_OUTLIER
} NxRuleType;

typedef enum {
    NX_SEVERITY_ERROR,
    NX_SEVERITY_WARNING
} NxSeverity;

/* Parsed validation rule */
typedef struct {
    NxRuleType type;
    NxSeverity severity;

    /* geo_bounds */
    char lat_field[NX_MAX_FIELD_NAME_LEN];
    char lon_field[NX_MAX_FIELD_NAME_LEN];
    double min_lat, max_lat, min_lon, max_lon;

    /* format */
    char format_field[NX_MAX_FIELD_NAME_LEN];
    char pattern[NX_MAX_PATTERN_LEN];
    char message[NX_MAX_MESSAGE_LEN];
    regex_t regex;
    int regex_compiled;

    /* unique */
    char unique_fields[NX_MAX_UNIQUE_FIELDS][NX_MAX_FIELD_NAME_LEN];
    int unique_field_count;

    /* outlier */
    char outlier_field[NX_MAX_FIELD_NAME_LEN];
    double outlier_factor;
} NxValidationRule;

/* Validation detail for audit */
typedef struct {
    int row_index;
    const char *rule_name;
    const char *field_name;
    NxSeverity severity;
    char message[NX_MAX_MESSAGE_LEN];
} NxValidationDetail;

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static const char *severity_str(NxSeverity sev) {
    return sev == NX_SEVERITY_ERROR ? "error" : "warning";
}

static const char *rule_type_str(NxRuleType type) {
    switch (type) {
        case NX_RULE_GEO_BOUNDS: return "geo_bounds";
        case NX_RULE_FORMAT: return "format";
        case NX_RULE_UNIQUE: return "unique";
        case NX_RULE_OUTLIER: return "outlier";
        default: return "unknown";
    }
}

/* Parse severity from JSON value */
static NxSeverity parse_severity(const char *str) {
    if (str && strcmp(str, "warning") == 0) {
        return NX_SEVERITY_WARNING;
    }
    return NX_SEVERITY_ERROR;
}

/* ============================================================================
 * Rule Parsing
 * ============================================================================ */

static int parse_validation_rules(ShJsonValue *validate_array, SHArena *arena,
                                   NxValidationRule **out_rules, int *out_count) {
    if (!validate_array || sh_json_type(validate_array) != SH_JSON_ARRAY) {
        *out_rules = NULL;
        *out_count = 0;
        return 1; /* No validation rules - not an error */
    }

    size_t count = sh_json_array_len(validate_array);
    if (count == 0 || count > NX_MAX_VALIDATION_RULES) {
        *out_rules = NULL;
        *out_count = 0;
        return 1;
    }

    NxValidationRule *rules = sh_arena_calloc(arena, count, sizeof(NxValidationRule));
    if (!rules) return 0;

    int parsed = 0;
    for (size_t i = 0; i < count; i++) {
        ShJsonValue *rule_obj = sh_json_array_get(validate_array, i);
        if (!rule_obj || sh_json_type(rule_obj) != SH_JSON_OBJECT) continue;

        const char *type_str = sh_json_as_string(sh_json_get(rule_obj, "type"), "");
        const char *sev_str = sh_json_as_string(sh_json_get(rule_obj, "severity"), "error");

        NxValidationRule *rule = &rules[parsed];
        rule->severity = parse_severity(sev_str);
        rule->regex_compiled = 0;

        if (strcmp(type_str, "geo_bounds") == 0) {
            rule->type = NX_RULE_GEO_BOUNDS;

            const char *lat_field = sh_json_as_string(sh_json_get(rule_obj, "lat_field"), "");
            const char *lon_field = sh_json_as_string(sh_json_get(rule_obj, "lon_field"), "");
            snprintf(rule->lat_field, sizeof(rule->lat_field), "%s", lat_field);
            snprintf(rule->lon_field, sizeof(rule->lon_field), "%s", lon_field);

            ShJsonValue *bounds = sh_json_get(rule_obj, "bounds");
            if (bounds) {
                rule->min_lat = sh_json_as_double(sh_json_get(bounds, "min_lat"), -90.0);
                rule->max_lat = sh_json_as_double(sh_json_get(bounds, "max_lat"), 90.0);
                rule->min_lon = sh_json_as_double(sh_json_get(bounds, "min_lon"), -180.0);
                rule->max_lon = sh_json_as_double(sh_json_get(bounds, "max_lon"), 180.0);
            }
            parsed++;

        } else if (strcmp(type_str, "format") == 0) {
            rule->type = NX_RULE_FORMAT;

            const char *field = sh_json_as_string(sh_json_get(rule_obj, "field"), "");
            const char *pattern = sh_json_as_string(sh_json_get(rule_obj, "pattern"), "");
            const char *message = sh_json_as_string(sh_json_get(rule_obj, "message"), "Format validation failed");

            snprintf(rule->format_field, sizeof(rule->format_field), "%s", field);
            snprintf(rule->pattern, sizeof(rule->pattern), "%s", pattern);
            snprintf(rule->message, sizeof(rule->message), "%s", message);

            /* Compile regex */
            if (regcomp(&rule->regex, rule->pattern, REG_EXTENDED | REG_NOSUB) == 0) {
                rule->regex_compiled = 1;
                parsed++;
            }

        } else if (strcmp(type_str, "unique") == 0) {
            rule->type = NX_RULE_UNIQUE;

            ShJsonValue *fields = sh_json_get(rule_obj, "fields");
            if (fields && sh_json_type(fields) == SH_JSON_ARRAY) {
                size_t field_count = sh_json_array_len(fields);
                rule->unique_field_count = 0;

                for (size_t j = 0; j < field_count && j < NX_MAX_UNIQUE_FIELDS; j++) {
                    ShJsonValue *field_val = sh_json_array_get(fields, j);
                    const char *field_name = sh_json_as_string(field_val, "");
                    if (field_name[0]) {
                        snprintf(rule->unique_fields[rule->unique_field_count],
                                NX_MAX_FIELD_NAME_LEN, "%s", field_name);
                        rule->unique_field_count++;
                    }
                }

                if (rule->unique_field_count > 0) {
                    parsed++;
                }
            }

        } else if (strcmp(type_str, "outlier") == 0) {
            rule->type = NX_RULE_OUTLIER;

            const char *field = sh_json_as_string(sh_json_get(rule_obj, "field"), "");
            snprintf(rule->outlier_field, sizeof(rule->outlier_field), "%s", field);

            rule->outlier_factor = sh_json_as_double(sh_json_get(rule_obj, "factor"), 1.5);
            parsed++;
        }
    }

    *out_rules = rules;
    *out_count = parsed;
    return 1;
}

/* ============================================================================
 * Rule Validation Functions
 * ============================================================================ */

/* Validate geo_bounds rule for a record */
static int validate_geo_bounds(const NxValidationRule *rule, ShJsonValue *record,
                               NxValidationDetail *detail) {
    ShJsonValue *lat_val = sh_json_get(record, rule->lat_field);
    ShJsonValue *lon_val = sh_json_get(record, rule->lon_field);

    if (!lat_val || !lon_val) return 1; /* Skip if fields missing */

    double lat = sh_json_as_double(lat_val, 0.0);
    double lon = sh_json_as_double(lon_val, 0.0);

    if (lat < rule->min_lat || lat > rule->max_lat) {
        snprintf(detail->message, sizeof(detail->message),
                "%s %.6f outside bounds [%.6f, %.6f]",
                rule->lat_field, lat, rule->min_lat, rule->max_lat);
        detail->field_name = rule->lat_field;
        return 0;
    }

    if (lon < rule->min_lon || lon > rule->max_lon) {
        snprintf(detail->message, sizeof(detail->message),
                "%s %.6f outside bounds [%.6f, %.6f]",
                rule->lon_field, lon, rule->min_lon, rule->max_lon);
        detail->field_name = rule->lon_field;
        return 0;
    }

    return 1;
}

/* Validate format rule for a record */
static int validate_format(const NxValidationRule *rule, ShJsonValue *record,
                          NxValidationDetail *detail) {
    if (!rule->regex_compiled) return 1;

    ShJsonValue *field_val = sh_json_get(record, rule->format_field);
    if (!field_val) return 1; /* Skip if field missing */

    const char *str = sh_json_as_string(field_val, "");
    if (!str[0]) return 1; /* Skip empty strings */

    if (regexec(&rule->regex, str, 0, NULL, 0) != 0) {
        snprintf(detail->message, sizeof(detail->message), "%s", rule->message);
        detail->field_name = rule->format_field;
        return 0;
    }

    return 1;
}

/* Check if two records match on unique fields */
static int records_match_unique_fields(const NxValidationRule *rule,
                                       ShJsonValue *rec1, ShJsonValue *rec2) {
    for (int i = 0; i < rule->unique_field_count; i++) {
        const char *field = rule->unique_fields[i];
        ShJsonValue *v1 = sh_json_get(rec1, field);
        ShJsonValue *v2 = sh_json_get(rec2, field);

        if (!v1 || !v2) return 0;

        /* Compare as strings */
        const char *s1 = sh_json_as_string(v1, "");
        const char *s2 = sh_json_as_string(v2, "");

        if (strcmp(s1, s2) != 0) return 0;
    }

    return 1; /* All fields match */
}

/* Validate unique rule - returns array of duplicate indices */
static int *validate_unique(const NxValidationRule *rule, ShJsonValue *records_array,
                           SHArena *arena, int *dup_count) {
    size_t record_count = sh_json_array_len(records_array);
    if (record_count == 0) {
        *dup_count = 0;
        return NULL;
    }

    int *duplicates = sh_arena_calloc(arena, record_count, sizeof(int));
    if (!duplicates) {
        *dup_count = 0;
        return NULL;
    }

    int dup_idx = 0;

    /* O(n²) uniqueness check - first occurrence kept, duplicates marked */
    for (size_t i = 0; i < record_count; i++) {
        ShJsonValue *rec_i = sh_json_array_get(records_array, i);
        if (!rec_i) continue;

        /* Check if this record is a duplicate of any earlier record */
        for (size_t j = 0; j < i; j++) {
            ShJsonValue *rec_j = sh_json_array_get(records_array, j);
            if (!rec_j) continue;

            if (records_match_unique_fields(rule, rec_i, rec_j)) {
                duplicates[dup_idx++] = (int)i;
                break;
            }
        }
    }

    *dup_count = dup_idx;
    return duplicates;
}

/* Compare function for qsort */
static int compare_doubles(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

/* Validate outlier rule - returns array of outlier indices */
static int *validate_outlier(const NxValidationRule *rule, ShJsonValue *records_array,
                            SHArena *arena, int *outlier_count,
                            double *out_q1, double *out_q3) {
    size_t record_count = sh_json_array_len(records_array);
    if (record_count == 0) {
        *outlier_count = 0;
        return NULL;
    }

    /* Collect numeric values */
    double *values = sh_arena_alloc(arena, record_count * sizeof(double));
    if (!values) {
        *outlier_count = 0;
        return NULL;
    }

    size_t value_count = 0;
    for (size_t i = 0; i < record_count; i++) {
        ShJsonValue *rec = sh_json_array_get(records_array, i);
        if (!rec) continue;

        ShJsonValue *field = sh_json_get(rec, rule->outlier_field);
        if (!field || sh_json_type(field) != SH_JSON_NUMBER) continue;

        values[value_count++] = sh_json_as_double(field, 0.0);
    }

    if (value_count < 4) {
        *outlier_count = 0;
        return NULL; /* Need at least 4 values for IQR */
    }

    /* Sort values */
    qsort(values, value_count, sizeof(double), compare_doubles);

    /* Compute Q1, Q3, IQR */
    size_t q1_idx = value_count / 4;
    size_t q3_idx = (3 * value_count) / 4;
    double q1 = values[q1_idx];
    double q3 = values[q3_idx];
    double iqr = q3 - q1;

    *out_q1 = q1;
    *out_q3 = q3;

    if (iqr <= 0.0) {
        *outlier_count = 0;
        return NULL; /* No spread */
    }

    double lower_bound = q1 - rule->outlier_factor * iqr;
    double upper_bound = q3 + rule->outlier_factor * iqr;

    /* Find outliers */
    int *outliers = sh_arena_calloc(arena, record_count, sizeof(int));
    if (!outliers) {
        *outlier_count = 0;
        return NULL;
    }

    int out_idx = 0;
    for (size_t i = 0; i < record_count; i++) {
        ShJsonValue *rec = sh_json_array_get(records_array, i);
        if (!rec) continue;

        ShJsonValue *field = sh_json_get(rec, rule->outlier_field);
        if (!field || sh_json_type(field) != SH_JSON_NUMBER) continue;

        double val = sh_json_as_double(field, 0.0);
        if (val < lower_bound || val > upper_bound) {
            outliers[out_idx++] = (int)i;
        }
    }

    *outlier_count = out_idx;
    return outliers;
}

/* ============================================================================
 * Main Validation Function
 * ============================================================================ */

NxValidateStatus nx_validate(const char *canonical_json, size_t canon_len,
                             const char *schema_json, size_t schema_len,
                             SHArena *arena,
                             char **out_json, size_t *out_len) {
    if (!canonical_json || !schema_json || !arena || !out_json || !out_len) {
        return NX_VALIDATE_ERR_NULL;
    }

    *out_json = NULL;
    *out_len = 0;

    /* Parse canonical JSON */
    ShJsonValue *canon_root = NULL;
    if (sh_json_parse(canonical_json, canon_len, arena, &canon_root) != SH_JSON_OK || !canon_root) {
        return NX_VALIDATE_ERR_JSON;
    }

    /* Parse schema JSON */
    ShJsonValue *schema_root = NULL;
    if (sh_json_parse(schema_json, schema_len, arena, &schema_root) != SH_JSON_OK || !schema_root) {
        return NX_VALIDATE_ERR_JSON;
    }

    /* Parse validation rules from schema */
    ShJsonValue *validate_array = sh_json_get(schema_root, "validate");
    NxValidationRule *rules = NULL;
    int rule_count = 0;

    if (!parse_validation_rules(validate_array, arena, &rules, &rule_count)) {
        return NX_VALIDATE_ERR_ARENA;
    }

    /* If no validation rules, pass through unchanged */
    if (rule_count == 0) {
        *out_json = malloc(canon_len + 1);
        if (!*out_json) return NX_VALIDATE_ERR_ARENA;
        memcpy(*out_json, canonical_json, canon_len);
        (*out_json)[canon_len] = '\0';
        *out_len = canon_len;
        return NX_VALIDATE_OK;
    }

    /* Get records array */
    ShJsonValue *records_array = sh_json_get(canon_root, "records");
    if (!records_array || sh_json_type(records_array) != SH_JSON_ARRAY) {
        return NX_VALIDATE_ERR_JSON;
    }

    size_t record_count = sh_json_array_len(records_array);

    /* Track validation details */
    NxValidationDetail *details = sh_arena_calloc(arena, NX_MAX_VALIDATION_DETAILS,
                                                   sizeof(NxValidationDetail));
    if (!details) return NX_VALIDATE_ERR_ARENA;

    int detail_count = 0;
    int error_count = 0;
    int warning_count = 0;

    /* Track which records to remove (error-severity failures) */
    int *records_to_remove = sh_arena_calloc(arena, record_count, sizeof(int));
    if (!records_to_remove) return NX_VALIDATE_ERR_ARENA;
    int remove_count = 0;

    /* Apply each rule */
    for (int r = 0; r < rule_count; r++) {
        NxValidationRule *rule = &rules[r];

        if (rule->type == NX_RULE_UNIQUE) {
            /* Unique rule - check all records at once */
            int dup_count = 0;
            int *duplicates = validate_unique(rule, records_array, arena, &dup_count);

            for (int d = 0; d < dup_count && detail_count < NX_MAX_VALIDATION_DETAILS; d++) {
                int dup_idx = duplicates[d];

                NxValidationDetail *detail = &details[detail_count++];
                detail->row_index = dup_idx;
                detail->rule_name = "unique";
                detail->field_name = rule->unique_fields[0];
                detail->severity = rule->severity;
                snprintf(detail->message, sizeof(detail->message),
                        "Duplicate value for unique field(s)");

                if (rule->severity == NX_SEVERITY_ERROR) {
                    records_to_remove[remove_count++] = dup_idx;
                    error_count++;
                } else {
                    warning_count++;
                }
            }

        } else if (rule->type == NX_RULE_OUTLIER) {
            /* Outlier rule - check all records at once */
            int outlier_count = 0;
            double q1 = 0.0, q3 = 0.0;
            int *outliers = validate_outlier(rule, records_array, arena, &outlier_count, &q1, &q3);

            for (int o = 0; o < outlier_count && detail_count < NX_MAX_VALIDATION_DETAILS; o++) {
                int out_idx = outliers[o];
                ShJsonValue *rec = sh_json_array_get(records_array, out_idx);
                ShJsonValue *field = sh_json_get(rec, rule->outlier_field);
                double val = sh_json_as_double(field, 0.0);

                NxValidationDetail *detail = &details[detail_count++];
                detail->row_index = out_idx;
                detail->rule_name = "outlier";
                detail->field_name = rule->outlier_field;
                detail->severity = rule->severity;
                snprintf(detail->message, sizeof(detail->message),
                        "%s %.6f is outlier (Q1=%.6f, Q3=%.6f, IQR factor=%.1f)",
                        rule->outlier_field, val, q1, q3, rule->outlier_factor);

                if (rule->severity == NX_SEVERITY_ERROR) {
                    records_to_remove[remove_count++] = out_idx;
                    error_count++;
                } else {
                    warning_count++;
                }
            }

        } else {
            /* Per-record rules (geo_bounds, format) */
            for (size_t i = 0; i < record_count && detail_count < NX_MAX_VALIDATION_DETAILS; i++) {
                ShJsonValue *record = sh_json_array_get(records_array, i);
                if (!record) continue;

                NxValidationDetail detail_buf = {0};
                detail_buf.row_index = (int)i;
                detail_buf.rule_name = rule_type_str(rule->type);
                detail_buf.severity = rule->severity;

                int passed = 1;
                if (rule->type == NX_RULE_GEO_BOUNDS) {
                    passed = validate_geo_bounds(rule, record, &detail_buf);
                } else if (rule->type == NX_RULE_FORMAT) {
                    passed = validate_format(rule, record, &detail_buf);
                }

                if (!passed) {
                    details[detail_count++] = detail_buf;

                    if (rule->severity == NX_SEVERITY_ERROR) {
                        records_to_remove[remove_count++] = (int)i;
                        error_count++;
                    } else {
                        warning_count++;
                    }
                }
            }
        }
    }

    /* Build output JSON with validation section and filtered records */
    ShJsonBuf jb;
    sh_json_buf_init(&jb);

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    sh_json_write_object_start(&w);

    /* Copy top-level fields */
    sh_json_write_kv_int(&w, "nx_canonical", sh_json_as_int(sh_json_get(canon_root, "nx_canonical"), 1));

    const char *schema_version = sh_json_as_string(sh_json_get(canon_root, "schema_version"), "");
    if (schema_version[0]) {
        sh_json_write_kv_string(&w, "schema_version", schema_version);
    }

    const char *source_sha = sh_json_as_string(sh_json_get(canon_root, "source_sha256"), "");
    if (source_sha[0]) {
        sh_json_write_kv_string(&w, "source_sha256", source_sha);
    }

    const char *output_type = sh_json_as_string(sh_json_get(canon_root, "output_type"), "");
    if (output_type[0]) {
        sh_json_write_kv_string(&w, "output_type", output_type);
    }

    /* Write filtered records array */
    sh_json_write_key(&w, "records");
    sh_json_write_array_start(&w);

    int written_count = 0;
    for (size_t i = 0; i < record_count; i++) {
        /* Check if this record should be removed */
        int should_remove = 0;
        for (int r = 0; r < remove_count; r++) {
            if (records_to_remove[r] == (int)i) {
                should_remove = 1;
                break;
            }
        }

        if (!should_remove) {
            ShJsonValue *record = sh_json_array_get(records_array, i);
            if (record && sh_json_type(record) == SH_JSON_OBJECT) {
                written_count++;

                /* Write record object with all fields */
                sh_json_write_object_start(&w);

                /* Iterate through all members of the object */
                size_t member_count = sh_json_object_len(record);
                for (size_t m = 0; m < member_count; m++) {
                    /* Access member directly from internal structure */
                    /* Note: This relies on ShJsonValue's internal layout */
                    ShJsonMember *member = &record->u.object_val.members[m];
                    const char *key = member->key;
                    ShJsonValue *value = member->value;

                    if (!key || !value) continue;

                    /* Write key-value pair based on value type */
                    switch (sh_json_type(value)) {
                        case SH_JSON_NULL:
                            sh_json_write_kv_null(&w, key);
                            break;
                        case SH_JSON_BOOL:
                            sh_json_write_kv_bool(&w, key, sh_json_as_bool(value, false));
                            break;
                        case SH_JSON_NUMBER:
                            /* Check if it looks like an integer */
                            {
                                double num = sh_json_as_double(value, 0.0);
                                if (floor(num) == num && fabs(num) < 1e10) {
                                    sh_json_write_kv_int(&w, key, (int64_t)num);
                                } else {
                                    sh_json_write_kv_double_fmt(&w, key, num, 6);
                                }
                            }
                            break;
                        case SH_JSON_STRING:
                            sh_json_write_kv_string(&w, key, sh_json_as_string(value, ""));
                            break;
                        case SH_JSON_ARRAY:
                        case SH_JSON_OBJECT:
                            /* Skip nested structures for now - not expected in flat records */
                            break;
                    }
                }

                sh_json_write_object_end(&w);
            }
        }
    }

    sh_json_write_array_end(&w);

    sh_json_write_kv_int(&w, "record_count", written_count);

    /* Write audit section with validation */
    sh_json_write_key(&w, "audit");
    sh_json_write_object_start(&w);

    ShJsonValue *orig_audit = sh_json_get(canon_root, "audit");
    int rows_processed = sh_json_as_int(sh_json_get(orig_audit, "rows_processed"), 0);

    sh_json_write_kv_int(&w, "rows_processed", rows_processed);
    sh_json_write_kv_int(&w, "rows_accepted", written_count);
    sh_json_write_kv_int(&w, "rows_rejected", (int)record_count - written_count);

    /* Copy rejections array if present */
    sh_json_write_key(&w, "rejections");
    sh_json_write_array_start(&w);
    sh_json_write_array_end(&w);

    /* Write validation section */
    sh_json_write_key(&w, "validation");
    sh_json_write_object_start(&w);

    sh_json_write_kv_int(&w, "rules_applied", rule_count);
    sh_json_write_kv_int(&w, "errors", error_count);
    sh_json_write_kv_int(&w, "warnings", warning_count);

    sh_json_write_key(&w, "details");
    sh_json_write_array_start(&w);

    for (int d = 0; d < detail_count; d++) {
        NxValidationDetail *detail = &details[d];

        sh_json_write_object_start(&w);
        sh_json_write_kv_int(&w, "row", detail->row_index);
        sh_json_write_kv_string(&w, "rule", detail->rule_name);
        sh_json_write_kv_string(&w, "field", detail->field_name ? detail->field_name : "");
        sh_json_write_kv_string(&w, "severity", severity_str(detail->severity));
        sh_json_write_kv_string(&w, "message", detail->message);
        sh_json_write_object_end(&w);
    }

    sh_json_write_array_end(&w);

    sh_json_write_object_end(&w); /* Close validation */

    sh_json_write_object_end(&w); /* Close audit */

    sh_json_write_object_end(&w); /* Close root */

    /* Clean up regex */
    for (int r = 0; r < rule_count; r++) {
        if (rules[r].regex_compiled) {
            regfree(&rules[r].regex);
        }
    }

    if (sh_json_writer_error(&w) || !jb.buf) {
        sh_json_buf_free(&jb);
        return NX_VALIDATE_ERR_ARENA;
    }

    *out_json = jb.buf;
    *out_len = jb.len;
    return NX_VALIDATE_OK;
}

/* ============================================================================
 * Status String
 * ============================================================================ */

const char *nx_validate_status_str(NxValidateStatus status) {
    switch (status) {
        case NX_VALIDATE_OK: return "OK";
        case NX_VALIDATE_ERR_NULL: return "NULL input";
        case NX_VALIDATE_ERR_JSON: return "Invalid JSON";
        case NX_VALIDATE_ERR_ARENA: return "Arena allocation failure";
        default: return "Unknown error";
    }
}
