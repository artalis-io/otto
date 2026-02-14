/*
 * nx_xlsx.c - XLSX Parser (Stage A)
 *
 * Parses XLSX files (ZIP of XML) into deterministic raw rows JSON.
 *
 * XLSX structure:
 *   xl/sharedStrings.xml - Shared string table (strings referenced by index)
 *   xl/workbook.xml - Sheet names
 *   xl/worksheets/sheet1.xml, sheet2.xml, ... - Cell data
 *
 * Cell reference format: "A1", "B2", "AA100" etc.
 * Cell types: s=shared string, n=number (default), str=inline string, b=boolean
 */

#include "nx_xlsx.h"
#include "sh_xml.h"
#include "sh_json.h"
#include "sh_hash_sha256.h"
#include "miniz.h"
#include "miniz_zip.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Internal Constants
 * ============================================================================ */

#define MAX_SHARED_STRINGS 65536
#define MAX_CELL_REF_LEN   16
#define MAX_SHEET_NAME_LEN 256
#define INITIAL_ROW_CAP    256
#define INITIAL_COL_CAP    64

/* ============================================================================
 * Column Reference Parsing
 * ============================================================================ */

/* Parse cell reference "AB12" -> (col_index, row_index) both 0-based */
static void parse_cell_ref(const char *ref, int *col, int *row)
{
    int c = 0, r = 0;
    const char *p = ref;

    /* Parse column letters */
    while (*p >= 'A' && *p <= 'Z') {
        c = c * 26 + (*p - 'A' + 1);
        p++;
    }
    *col = c > 0 ? c - 1 : 0;

    /* Parse row number */
    while (*p >= '0' && *p <= '9') {
        r = r * 10 + (*p - '0');
        p++;
    }
    *row = r > 0 ? r - 1 : 0;
}

/* ============================================================================
 * Shared Strings
 * ============================================================================ */

typedef struct {
    const char **strings;
    int count;
    int capacity;
} SharedStrings;

static int parse_shared_strings(const char *xml, size_t xml_len,
                                SHArena *arena, NxIssueList *issues,
                                SharedStrings *ss)
{
    ShXmlReader r;
    ShXmlToken tok;
    sh_xml_init(&r, xml, xml_len, arena);

    ss->count = 0;
    ss->capacity = 1024;
    ss->strings = (const char **)sh_arena_alloc(arena,
        (size_t)ss->capacity * sizeof(char *));
    if (!ss->strings) return -1;

    int in_si = 0, in_t = 0;
    /* Buffer for concatenating <t> segments within one <si> */
    char concat_buf[4096];
    int concat_len = 0;

    while (sh_xml_next(&r, &tok) != SH_XML_EOF) {
        if (tok.type == SH_XML_ERROR) return -1;

        if (tok.type == SH_XML_START_TAG) {
            if (tok.name_len == 2 && memcmp(tok.name, "si", 2) == 0) {
                in_si = 1;
                concat_len = 0;
            } else if (in_si && tok.name_len == 1 && tok.name[0] == 't') {
                in_t = 1;
            }
        } else if (tok.type == SH_XML_TEXT && in_t) {
            /* Append text to concat buffer */
            int avail = (int)sizeof(concat_buf) - concat_len - 1;
            int copy_len = (int)tok.text_len;
            if (copy_len > avail) copy_len = avail;
            if (copy_len > 0) {
                memcpy(concat_buf + concat_len, tok.text, (size_t)copy_len);
                concat_len += copy_len;
            }
        } else if (tok.type == SH_XML_END_TAG) {
            if (tok.name_len == 1 && tok.name[0] == 't') {
                in_t = 0;
            } else if (tok.name_len == 2 && memcmp(tok.name, "si", 2) == 0) {
                /* End of <si>: store concatenated string */
                concat_buf[concat_len] = '\0';
                if (ss->count < MAX_SHARED_STRINGS) {
                    if (ss->count >= ss->capacity) {
                        /* Out of pre-allocated space */
                        return -1;
                    }
                    char *s = (char *)sh_arena_alloc(arena, (size_t)concat_len + 1);
                    if (!s) return -1;
                    memcpy(s, concat_buf, (size_t)concat_len + 1);
                    ss->strings[ss->count++] = s;
                } else {
                    if (issues && ss->count == MAX_SHARED_STRINGS)
                        nx_issue_addf(issues, NX_STAGE_A, NX_ISSUE_WARNING,
                                      -1, "", "shared_string_limit",
                                      "Hit MAX_SHARED_STRINGS=%d cap",
                                      MAX_SHARED_STRINGS);
                }
                in_si = 0;
                concat_len = 0;
            }
        }
    }
    return 0;
}

/* ============================================================================
 * Sheet Names
 * ============================================================================ */

typedef struct {
    char name[MAX_SHEET_NAME_LEN];
} SheetInfo;

static int parse_workbook(const char *xml, size_t xml_len,
                          SHArena *arena, SheetInfo *sheets, int max_sheets)
{
    ShXmlReader r;
    ShXmlToken tok;
    sh_xml_init(&r, xml, xml_len, arena);

    int count = 0;
    while (sh_xml_next(&r, &tok) != SH_XML_EOF) {
        if (tok.type == SH_XML_ERROR) break;
        if ((tok.type == SH_XML_START_TAG || tok.type == SH_XML_SELF_CLOSE) &&
            tok.name_len == 5 && memcmp(tok.name, "sheet", 5) == 0) {
            const char *name = sh_xml_attr(&tok, "name");
            if (name && count < max_sheets) {
                snprintf(sheets[count].name, MAX_SHEET_NAME_LEN, "%s", name);
                count++;
            }
        }
    }
    return count;
}

/* ============================================================================
 * Worksheet Cell Parsing
 * ============================================================================ */

typedef struct {
    char **cells;    /* Array of cell strings, indexed by column */
    int col_count;   /* Highest column index + 1 */
    int row_num;     /* 0-based row number */
} ParsedRow;

typedef struct {
    ParsedRow *rows;
    int row_count;
    int row_cap;
    int max_cols;    /* Widest row seen */
} ParsedSheet;

static int ensure_row_cap(ParsedSheet *sheet, SHArena *arena)
{
    if (sheet->row_count >= sheet->row_cap) {
        int new_cap = sheet->row_cap * 2;
        ParsedRow *new_rows = (ParsedRow *)sh_arena_alloc(arena,
            (size_t)new_cap * sizeof(ParsedRow));
        if (!new_rows) return -1;
        memcpy(new_rows, sheet->rows, (size_t)sheet->row_count * sizeof(ParsedRow));
        sheet->rows = new_rows;
        sheet->row_cap = new_cap;
    }
    return 0;
}

static int parse_worksheet(const char *xml, size_t xml_len,
                           SHArena *arena, const SharedStrings *ss,
                           const NxXlsxLimits *limits, NxIssueList *issues,
                           ParsedSheet *sheet)
{
    ShXmlReader r;
    ShXmlToken tok;
    sh_xml_init(&r, xml, xml_len, arena);

    sheet->row_count = 0;
    sheet->row_cap = INITIAL_ROW_CAP;
    sheet->max_cols = 0;
    sheet->rows = (ParsedRow *)sh_arena_alloc(arena,
        (size_t)sheet->row_cap * sizeof(ParsedRow));
    if (!sheet->rows) return -1;

    int in_row = 0, in_c = 0, in_v = 0, in_is = 0, in_t = 0;
    int cur_col = 0, cur_row = 0;
    char cell_type = 'n'; /* default: number */
    char value_buf[4096];
    int value_len = 0;

    /* For inline strings: concatenate <t> text segments */
    char inline_buf[4096];
    int inline_len = 0;

    while (sh_xml_next(&r, &tok) != SH_XML_EOF) {
        if (tok.type == SH_XML_ERROR) return -1;

        if (tok.type == SH_XML_START_TAG || tok.type == SH_XML_SELF_CLOSE) {
            if (tok.name_len == 3 && memcmp(tok.name, "row", 3) == 0) {
                in_row = 1;
                const char *r_attr = sh_xml_attr(&tok, "r");
                cur_row = r_attr ? atoi(r_attr) - 1 : sheet->row_count;
            } else if (in_row && tok.name_len == 1 && tok.name[0] == 'c') {
                in_c = 1;
                value_len = 0;
                inline_len = 0;
                cell_type = 'n';

                const char *ref = sh_xml_attr(&tok, "r");
                if (ref) {
                    int dummy_row;
                    parse_cell_ref(ref, &cur_col, &dummy_row);
                } else {
                    cur_col = 0;
                }

                const char *t_attr = sh_xml_attr(&tok, "t");
                if (t_attr && t_attr[0]) cell_type = t_attr[0];
            } else if (in_c && tok.name_len == 1 && tok.name[0] == 'v') {
                in_v = 1;
                value_len = 0;
            } else if (in_c && tok.name_len == 2 && memcmp(tok.name, "is", 2) == 0) {
                in_is = 1;
                inline_len = 0;
            } else if (in_is && tok.name_len == 1 && tok.name[0] == 't') {
                in_t = 1;
            }
        } else if (tok.type == SH_XML_TEXT) {
            if (in_v) {
                int avail = (int)sizeof(value_buf) - value_len - 1;
                int copy = (int)tok.text_len;
                if (copy > avail) {
                    if (issues)
                        nx_issue_addf(issues, NX_STAGE_A, NX_ISSUE_WARNING,
                                      cur_row, "", "cell_truncated",
                                      "Cell value truncated at %d bytes",
                                      (int)sizeof(value_buf));
                    copy = avail;
                }
                if (copy > 0) {
                    memcpy(value_buf + value_len, tok.text, (size_t)copy);
                    value_len += copy;
                }
            } else if (in_t && in_is) {
                int avail = (int)sizeof(inline_buf) - inline_len - 1;
                int copy = (int)tok.text_len;
                if (copy > avail) copy = avail;
                if (copy > 0) {
                    memcpy(inline_buf + inline_len, tok.text, (size_t)copy);
                    inline_len += copy;
                }
            }
        } else if (tok.type == SH_XML_END_TAG) {
            if (tok.name_len == 1 && tok.name[0] == 'v') {
                in_v = 0;
            } else if (tok.name_len == 1 && tok.name[0] == 't') {
                in_t = 0;
            } else if (tok.name_len == 2 && memcmp(tok.name, "is", 2) == 0) {
                in_is = 0;
            } else if (tok.name_len == 1 && tok.name[0] == 'c') {
                /* End of cell: resolve value */
                value_buf[value_len] = '\0';
                inline_buf[inline_len] = '\0';

                const char *cell_value = "";

                if (cell_type == 's' && value_len > 0) {
                    /* Shared string reference */
                    int idx = atoi(value_buf);
                    if (ss && idx >= 0 && idx < ss->count)
                        cell_value = ss->strings[idx];
                } else if (cell_type == 'i' && inline_len > 0) {
                    /* inlineStr */
                    cell_value = inline_buf;
                } else if (value_len > 0) {
                    /* Number, boolean, or inline string */
                    cell_value = value_buf;
                }

                /* Ensure we have a row for cur_row */
                while (sheet->row_count <= cur_row) {
                    if (ensure_row_cap(sheet, arena) < 0) return -1;
                    ParsedRow *row = &sheet->rows[sheet->row_count];
                    row->cells = NULL;
                    row->col_count = 0;
                    row->row_num = sheet->row_count;
                    sheet->row_count++;
                }

                ParsedRow *row = &sheet->rows[cur_row];

                /* Ensure column capacity */
                if (cur_col >= row->col_count) {
                    int new_cols = cur_col + 1;
                    if (new_cols < INITIAL_COL_CAP) new_cols = INITIAL_COL_CAP;
                    char **new_cells = (char **)sh_arena_calloc(arena,
                        (size_t)new_cols, sizeof(char *));
                    if (!new_cells) return -1;
                    if (row->cells && row->col_count > 0)
                        memcpy(new_cells, row->cells, (size_t)row->col_count * sizeof(char *));
                    row->cells = new_cells;
                    row->col_count = new_cols;
                }

                /* Store cell value */
                size_t vlen = strlen(cell_value);
                char *stored = (char *)sh_arena_alloc(arena, vlen + 1);
                if (!stored) return -1;
                memcpy(stored, cell_value, vlen + 1);
                row->cells[cur_col] = stored;

                if (cur_col + 1 > sheet->max_cols)
                    sheet->max_cols = cur_col + 1;

                in_c = 0;
            } else if (tok.name_len == 3 && memcmp(tok.name, "row", 3) == 0) {
                in_row = 0;
            }
        }
    }

    /* Apply limits */
    if (limits) {
        if (limits->max_rows > 0 && sheet->row_count > limits->max_rows)
            sheet->row_count = limits->max_rows;
        if (limits->max_cols > 0 && sheet->max_cols > limits->max_cols)
            sheet->max_cols = limits->max_cols;
    }

    return 0;
}

/* ============================================================================
 * JSON Output
 * ============================================================================ */

static void write_raw_json(ShJsonWriter *w, const char *filename,
                           const char *sha256_hex,
                           SheetInfo *sheets, ParsedSheet *parsed_sheets,
                           int sheet_count)
{
    sh_json_write_object_start(w);
    sh_json_write_kv_int(w, "nx_raw", 1);

    /* source */
    sh_json_write_key(w, "source");
    sh_json_write_object_start(w);
    sh_json_write_kv_string(w, "filename", filename ? filename : "");
    sh_json_write_kv_string(w, "sha256", sha256_hex);
    sh_json_write_kv_string(w, "format", "xlsx");
    sh_json_write_object_end(w);

    /* tables */
    sh_json_write_key(w, "tables");
    sh_json_write_array_start(w);

    for (int s = 0; s < sheet_count; s++) {
        ParsedSheet *ps = &parsed_sheets[s];
        int max_cols = ps->max_cols;

        /* Find first non-empty row for headers */
        int header_row = -1;
        for (int i = 0; i < ps->row_count; i++) {
            ParsedRow *row = &ps->rows[i];
            if (row->cells) {
                for (int j = 0; j < row->col_count && j < max_cols; j++) {
                    if (row->cells[j] && row->cells[j][0]) {
                        header_row = i;
                        goto found_header;
                    }
                }
            }
        }
        found_header:

        sh_json_write_object_start(w);
        sh_json_write_kv_string(w, "name", sheets[s].name);
        sh_json_write_kv_int(w, "index", s);

        /* headers */
        sh_json_write_key(w, "headers");
        sh_json_write_array_start(w);
        if (header_row >= 0 && header_row < ps->row_count) {
            ParsedRow *hrow = &ps->rows[header_row];
            for (int j = 0; j < max_cols; j++) {
                const char *val = (hrow->cells && j < hrow->col_count && hrow->cells[j])
                    ? hrow->cells[j] : "";
                sh_json_write_string(w, val);
            }
        }
        sh_json_write_array_end(w);

        sh_json_write_kv_int(w, "header_row", header_row >= 0 ? header_row : 0);

        /* rows (skip header row) */
        sh_json_write_key(w, "rows");
        sh_json_write_array_start(w);
        int data_rows = 0;
        for (int i = 0; i < ps->row_count; i++) {
            if (i == header_row) continue;
            ParsedRow *row = &ps->rows[i];

            /* Skip completely empty rows */
            int has_data = 0;
            if (row->cells) {
                for (int j = 0; j < row->col_count && j < max_cols; j++) {
                    if (row->cells[j] && row->cells[j][0]) {
                        has_data = 1;
                        break;
                    }
                }
            }
            if (!has_data) continue;

            sh_json_write_object_start(w);
            sh_json_write_kv_int(w, "row", i);
            sh_json_write_key(w, "cells");
            sh_json_write_array_start(w);
            for (int j = 0; j < max_cols; j++) {
                const char *val = (row->cells && j < row->col_count && row->cells[j])
                    ? row->cells[j] : "";
                sh_json_write_string(w, val);
            }
            sh_json_write_array_end(w);
            sh_json_write_object_end(w);
            data_rows++;
        }
        sh_json_write_array_end(w);

        sh_json_write_kv_int(w, "row_count", data_rows);
        sh_json_write_kv_int(w, "col_count", max_cols);
        sh_json_write_object_end(w);
    }

    sh_json_write_array_end(w);

    /* warnings */
    sh_json_write_key(w, "warnings");
    sh_json_write_array_start(w);
    sh_json_write_array_end(w);

    sh_json_write_object_end(w);
}

/* ============================================================================
 * Extract file from ZIP by name
 * ============================================================================ */

static void *zip_extract(mz_zip_archive *zip, const char *name,
                         size_t *out_size)
{
    int idx = mz_zip_reader_locate_file(zip, name, NULL, 0);
    if (idx < 0) return NULL;
    return mz_zip_reader_extract_to_heap(zip, (mz_uint)idx, out_size, 0);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

const char *nx_xlsx_status_str(NxXlsxStatus status)
{
    switch (status) {
        case NX_XLSX_OK:         return "OK";
        case NX_XLSX_ERR_NULL:   return "NULL input";
        case NX_XLSX_ERR_ZIP:    return "Invalid ZIP / not an XLSX";
        case NX_XLSX_ERR_NO_SHEETS: return "No worksheets found";
        case NX_XLSX_ERR_XML:    return "XML parse error";
        case NX_XLSX_ERR_ARENA:  return "Arena allocation failure";
        case NX_XLSX_ERR_LIMITS: return "Exceeds configured limits";
        default:                 return "Unknown error";
    }
}

NxXlsxStatus nx_xlsx_parse(const void *data, size_t len,
                           const NxXlsxLimits *limits, const char *filename,
                           SHArena *arena, NxIssueList *issues,
                           char **out_json, size_t *out_len)
{
    NxXlsxLimits default_limits = NX_XLSX_DEFAULT_LIMITS;
    mz_zip_archive zip;
    SharedStrings ss = {0};
    SheetInfo sheets[256];
    ParsedSheet parsed_sheets[256];
    int sheet_count = 0;
    NxXlsxStatus result = NX_XLSX_OK;

    if (!data || !out_json || !out_len) return NX_XLSX_ERR_NULL;
    if (!arena) return NX_XLSX_ERR_ARENA;
    if (!limits) limits = &default_limits;

    *out_json = NULL;
    *out_len = 0;

    /* Compute SHA-256 of input */
    char sha256_hex[65];
    sh_sha256_hex(data, len, sha256_hex);

    /* Open ZIP */
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, data, len, 0)) {
        return NX_XLSX_ERR_ZIP;
    }

    /* Parse shared strings */
    size_t ss_size = 0;
    void *ss_xml = zip_extract(&zip, "xl/sharedStrings.xml", &ss_size);
    if (ss_xml) {
        if (parse_shared_strings((const char *)ss_xml, ss_size, arena, issues, &ss) < 0) {
            mz_free(ss_xml);
            mz_zip_reader_end(&zip);
            return NX_XLSX_ERR_XML;
        }
        mz_free(ss_xml);
    }
    /* sharedStrings is optional (some XLSX files have no shared strings) */

    /* Parse workbook for sheet names */
    size_t wb_size = 0;
    void *wb_xml = zip_extract(&zip, "xl/workbook.xml", &wb_size);
    if (wb_xml) {
        sheet_count = parse_workbook((const char *)wb_xml, wb_size, arena,
                                     sheets, 256);
        mz_free(wb_xml);
    }

    /* If no sheets from workbook.xml, try to find worksheets directly */
    if (sheet_count == 0) {
        for (int i = 1; i <= 256; i++) {
            char path[64];
            snprintf(path, sizeof(path), "xl/worksheets/sheet%d.xml", i);
            int idx = mz_zip_reader_locate_file(&zip, path, NULL, 0);
            if (idx < 0) break;
            snprintf(sheets[sheet_count].name, MAX_SHEET_NAME_LEN, "Sheet%d", i);
            sheet_count++;
        }
    }

    if (sheet_count == 0) {
        mz_zip_reader_end(&zip);
        return NX_XLSX_ERR_NO_SHEETS;
    }

    /* Apply sheet limit */
    if (limits->max_sheets > 0 && sheet_count > limits->max_sheets)
        sheet_count = limits->max_sheets;

    /* Parse each worksheet */
    for (int s = 0; s < sheet_count; s++) {
        char path[64];
        snprintf(path, sizeof(path), "xl/worksheets/sheet%d.xml", s + 1);

        size_t ws_size = 0;
        void *ws_xml = zip_extract(&zip, path, &ws_size);
        if (!ws_xml) {
            /* Try without number if only one sheet */
            memset(&parsed_sheets[s], 0, sizeof(ParsedSheet));
            continue;
        }

        if (parse_worksheet((const char *)ws_xml, ws_size, arena, &ss,
                           limits, issues, &parsed_sheets[s]) < 0) {
            mz_free(ws_xml);
            result = NX_XLSX_ERR_XML;
            break;
        }
        mz_free(ws_xml);
    }

    mz_zip_reader_end(&zip);

    if (result != NX_XLSX_OK) return result;

    /* Generate JSON */
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);

    write_raw_json(&w, filename, sha256_hex, sheets, parsed_sheets, sheet_count);

    if (sh_json_writer_error(&w) || !jb.buf) {
        sh_json_buf_free(&jb);
        return NX_XLSX_ERR_ARENA;
    }

    *out_json = sh_json_buf_take(&jb);
    *out_len = jb.len;

    /* sh_json_buf_take resets jb, but we need the len before take */
    if (*out_json)
        *out_len = strlen(*out_json);

    return NX_XLSX_OK;
}
