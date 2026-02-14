/*
 * sh_csv.c - Streaming CSV Parser (RFC 4180)
 *
 * Pull-style parser for CSV data. Handles quoted fields, escaped quotes,
 * CRLF/LF line endings, UTF-8 BOM, delimiter auto-detection.
 * All returned field values are arena-allocated.
 */

#include "sh_csv.h"
#include "sh_arena.h"
#include <string.h>

/* ============================================================================
 * Helpers
 * ============================================================================ */

static int csv_eof(const ShCsvReader *r)
{
    return r->pos >= r->len;
}

static char csv_peek(const ShCsvReader *r)
{
    if (r->pos >= r->len) return '\0';
    return r->data[r->pos];
}

/* Allocate a null-terminated copy on the arena */
static char *csv_arena_strdup(SHArena *arena, const char *src, size_t len)
{
    char *dst = (char *)sh_arena_alloc(arena, len + 1);
    if (!dst) return NULL;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}

/* Skip UTF-8 BOM (EF BB BF) at the start of data */
static void csv_skip_bom(ShCsvReader *r)
{
    if (r->len >= 3 &&
        (unsigned char)r->data[0] == 0xEF &&
        (unsigned char)r->data[1] == 0xBB &&
        (unsigned char)r->data[2] == 0xBF) {
        r->pos = 3;
    }
}

/* Consume a newline sequence (CRLF or LF). Returns 1 if consumed, 0 if not. */
static int csv_consume_newline(ShCsvReader *r)
{
    if (r->pos >= r->len) return 0;
    if (r->data[r->pos] == '\r') {
        r->pos++;
        if (r->pos < r->len && r->data[r->pos] == '\n')
            r->pos++;
        return 1;
    }
    if (r->data[r->pos] == '\n') {
        r->pos++;
        return 1;
    }
    return 0;
}

/* Check if current position is at a newline */
static int csv_at_newline(const ShCsvReader *r)
{
    if (r->pos >= r->len) return 0;
    return r->data[r->pos] == '\n' || r->data[r->pos] == '\r';
}

/* ============================================================================
 * Delimiter Auto-Detection
 * ============================================================================ */

/*
 * Scan the first line (up to the first unquoted newline) and count
 * occurrences of each candidate delimiter. The one with the highest
 * count wins. If all are 0, default to comma.
 */
static char csv_detect_delimiter(const char *data, size_t len, char quote)
{
    const char candidates[] = { ',', ';', '\t', '|' };
    int counts[4] = { 0, 0, 0, 0 };
    int in_quote = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        char c = data[i];

        if (c == quote) {
            in_quote = !in_quote;
            continue;
        }
        if (in_quote) continue;

        /* Stop at first unquoted newline */
        if (c == '\n' || c == '\r') break;

        for (int j = 0; j < 4; j++) {
            if (c == candidates[j])
                counts[j]++;
        }
    }

    /* Pick the candidate with the highest count */
    int best = 0;
    for (int j = 1; j < 4; j++) {
        if (counts[j] > counts[best])
            best = j;
    }

    /* If all zero, default to comma */
    if (counts[best] == 0) return ',';
    return candidates[best];
}

/* ============================================================================
 * Trimming
 * ============================================================================ */

static void csv_trim(const char **start, size_t *len)
{
    const char *s = *start;
    size_t n = *len;

    /* Trim leading whitespace */
    while (n > 0 && (s[0] == ' ' || s[0] == '\t')) {
        s++;
        n--;
    }
    /* Trim trailing whitespace */
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        n--;
    }
    *start = s;
    *len = n;
}

/* ============================================================================
 * Field Parsing
 * ============================================================================ */

/*
 * Parse a quoted field. The reader position should be at the opening quote.
 * Returns an arena-allocated string with quotes stripped and "" unescaped.
 * Sets *field_len to the length of the unescaped content.
 * Returns NULL on error (field too long or arena full).
 */
static const char *csv_parse_quoted_field(ShCsvReader *r, size_t *field_len)
{
    r->pos++; /* skip opening quote */

    /*
     * First pass: find the end and measure the unescaped length.
     * In RFC 4180, a quote within a quoted field is escaped as "".
     */
    size_t start = r->pos;
    size_t unescaped_len = 0;
    size_t scan = r->pos;

    while (scan < r->len) {
        if (r->data[scan] == r->quote) {
            /* Check for escaped quote ("") */
            if (scan + 1 < r->len && r->data[scan + 1] == r->quote) {
                unescaped_len++;
                scan += 2;
            } else {
                /* End of quoted field */
                break;
            }
        } else {
            unescaped_len++;
            scan++;
        }
    }

    /* Enforce max_field_len */
    if (unescaped_len > r->max_field_len) {
        *field_len = 0;
        return NULL;
    }

    /* Allocate and copy with unescaping */
    char *buf = (char *)sh_arena_alloc(r->arena, unescaped_len + 1);
    if (!buf) {
        *field_len = 0;
        return NULL;
    }

    size_t wi = 0;
    size_t ri = start;
    while (ri < r->len) {
        if (r->data[ri] == r->quote) {
            if (ri + 1 < r->len && r->data[ri + 1] == r->quote) {
                buf[wi++] = r->quote;
                ri += 2;
            } else {
                /* Closing quote */
                ri++; /* skip closing quote */
                break;
            }
        } else {
            buf[wi++] = r->data[ri++];
        }
    }
    buf[wi] = '\0';

    r->pos = ri;
    *field_len = wi;
    return buf;
}

/*
 * Parse an unquoted field. Reads until delimiter, newline, or EOF.
 * Returns an arena-allocated string. Trims if configured.
 * Returns NULL on error (field too long or arena full).
 */
static const char *csv_parse_unquoted_field(ShCsvReader *r, size_t *field_len)
{
    size_t start = r->pos;

    while (r->pos < r->len) {
        char c = r->data[r->pos];
        if (c == r->delimiter || c == '\r' || c == '\n')
            break;
        r->pos++;
    }

    const char *fdata = r->data + start;
    size_t flen = r->pos - start;

    /* Trim whitespace if configured */
    if (r->trim_fields) {
        csv_trim(&fdata, &flen);
    }

    /* Enforce max_field_len */
    if (flen > r->max_field_len) {
        *field_len = 0;
        return NULL;
    }

    char *buf = csv_arena_strdup(r->arena, fdata, flen);
    *field_len = flen;
    return buf;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

void sh_csv_opts_default(ShCsvOpts *opts)
{
    if (!opts) return;
    opts->delimiter = 0;       /* auto-detect */
    opts->quote = '"';
    opts->has_header = 0;
    opts->skip_empty_rows = 1;
    opts->trim_fields = 0;
    opts->max_field_len = 32768;
    opts->max_columns = 1024;
}

ShCsvStatus sh_csv_init(ShCsvReader *r, const char *data, size_t len,
                        const ShCsvOpts *opts, SHArena *arena)
{
    if (!r) return SH_CSV_ERR_NULL;
    if (!arena) return SH_CSV_ERR_NULL;

    memset(r, 0, sizeof(*r));

    r->data = data;
    r->len = (data != NULL) ? len : 0;
    r->pos = 0;
    r->arena = arena;
    r->row = 0;
    r->col = 0;
    r->at_row_start = 1;
    r->need_row_end = 0;
    r->eof = 0;

    /* Apply options (or defaults) */
    ShCsvOpts defaults;
    if (!opts) {
        sh_csv_opts_default(&defaults);
        opts = &defaults;
    }

    r->quote = opts->quote;
    r->skip_empty_rows = opts->skip_empty_rows;
    r->trim_fields = opts->trim_fields;
    r->max_field_len = opts->max_field_len > 0 ? opts->max_field_len : 32768;
    r->max_columns = opts->max_columns > 0 ? opts->max_columns : 1024;

    /* Skip BOM if present */
    csv_skip_bom(r);

    /* Delimiter: explicit or auto-detect */
    if (opts->delimiter != 0) {
        r->delimiter = opts->delimiter;
    } else {
        r->delimiter = csv_detect_delimiter(r->data + r->pos,
                                            r->len - r->pos, r->quote);
    }

    return SH_CSV_OK;
}

ShCsvTokenType sh_csv_next(ShCsvReader *r, ShCsvToken *tok)
{
    if (!r || !tok) return SH_CSV_TOKEN_EOF;

    tok->data = NULL;
    tok->len = 0;

    /* If we already hit EOF, stay there */
    if (r->eof) return SH_CSV_TOKEN_EOF;

    /* If a ROW_END is pending, emit it now */
    if (r->need_row_end) {
        r->need_row_end = 0;
        r->row++;
        r->col = 0;
        r->at_row_start = 1;
        return SH_CSV_TOKEN_ROW_END;
    }

    /* Skip empty rows if configured */
    if (r->at_row_start && r->skip_empty_rows) {
        while (!csv_eof(r) && csv_at_newline(r)) {
            csv_consume_newline(r);
        }
    }

    /* Check for EOF */
    if (csv_eof(r)) {
        /*
         * If we were in the middle of a row (not at_row_start), we already
         * emitted the last field. We need to emit ROW_END first.
         */
        if (!r->at_row_start) {
            r->eof = 1;
            r->at_row_start = 1;
            r->row++;
            r->col = 0;
            return SH_CSV_TOKEN_ROW_END;
        }
        r->eof = 1;
        return SH_CSV_TOKEN_EOF;
    }

    r->at_row_start = 0;

    /* Enforce max_columns */
    if ((size_t)r->col >= r->max_columns) {
        r->eof = 1;
        return SH_CSV_TOKEN_EOF;
    }

    /* Parse one field */
    const char *field_data;
    size_t field_len;

    if (csv_peek(r) == r->quote) {
        field_data = csv_parse_quoted_field(r, &field_len);
        if (!field_data) {
            /* Error: field too long or arena full */
            r->eof = 1;
            return SH_CSV_TOKEN_EOF;
        }
    } else {
        field_data = csv_parse_unquoted_field(r, &field_len);
        if (!field_data) {
            r->eof = 1;
            return SH_CSV_TOKEN_EOF;
        }
    }

    tok->data = field_data;
    tok->len = field_len;

    int current_col = r->col;
    r->col++;

    /* What follows the field? */
    if (csv_eof(r)) {
        /* End of input after field. Need to emit ROW_END next. */
        r->need_row_end = 1;
    } else if (r->data[r->pos] == r->delimiter) {
        /* More fields in this row */
        r->pos++; /* consume delimiter */
    } else if (csv_at_newline(r)) {
        /* End of row */
        csv_consume_newline(r);
        r->need_row_end = 1;
    }

    (void)current_col;
    return SH_CSV_TOKEN_FIELD;
}

int sh_csv_row_number(const ShCsvReader *r)
{
    if (!r) return -1;
    return r->row;
}

int sh_csv_column_number(const ShCsvReader *r)
{
    if (!r) return -1;
    return r->col;
}

/* ============================================================================
 * CSV Writer (RFC 4180) — Streaming via callback
 * ============================================================================ */

#include <stdlib.h>

static int csv_emit(ShCsvWriter *w, const char *data, size_t len)
{
    if (w->error) return -1;
    int rc = w->write_fn(w->ctx, data, len);
    if (rc != 0) w->error = 1;
    return rc;
}

void sh_csv_writer_init(ShCsvWriter *w, ShCsvWriteFn write_fn, void *ctx,
                        char delimiter)
{
    if (!w) return;
    memset(w, 0, sizeof(*w));
    w->write_fn = write_fn;
    w->ctx = ctx;
    w->delimiter = delimiter ? delimiter : ',';
}

int sh_csv_writer_error(const ShCsvWriter *w)
{
    return w ? w->error : 1;
}

int sh_csv_write_field(ShCsvWriter *w, const char *value, size_t len)
{
    if (!w || !w->write_fn) return -1;

    /* Prepend delimiter if not first column */
    if (w->col > 0) {
        if (csv_emit(w, &w->delimiter, 1) != 0) return -1;
    }

    if (!value) { value = ""; len = 0; }

    /* Check if quoting needed */
    int needs_quote = 0;
    for (size_t i = 0; i < len; i++) {
        char c = value[i];
        if (c == w->delimiter || c == '"' || c == '\r' || c == '\n') {
            needs_quote = 1;
            break;
        }
    }

    if (!needs_quote) {
        if (len > 0 && csv_emit(w, value, len) != 0) return -1;
    } else {
        if (csv_emit(w, "\"", 1) != 0) return -1;
        /* Write value with quote doubling */
        size_t start = 0;
        for (size_t i = 0; i < len; i++) {
            if (value[i] == '"') {
                /* Flush segment before quote */
                if (i > start) {
                    if (csv_emit(w, value + start, i - start) != 0) return -1;
                }
                if (csv_emit(w, "\"\"", 2) != 0) return -1;
                start = i + 1;
            }
        }
        /* Flush remaining */
        if (len > start) {
            if (csv_emit(w, value + start, len - start) != 0) return -1;
        }
        if (csv_emit(w, "\"", 1) != 0) return -1;
    }

    w->col++;
    return 0;
}

int sh_csv_write_field_str(ShCsvWriter *w, const char *value)
{
    return sh_csv_write_field(w, value, value ? strlen(value) : 0);
}

int sh_csv_write_row_end(ShCsvWriter *w)
{
    if (!w || !w->write_fn) return -1;
    if (csv_emit(w, "\r\n", 2) != 0) return -1;
    w->col = 0;
    return 0;
}

/* ============================================================================
 * CSV Buffer Helper (like ShJsonBuf)
 * ============================================================================ */

void sh_csv_buf_init(ShCsvBuf *cb)
{
    if (!cb) return;
    cb->buf = NULL;
    cb->len = 0;
    cb->cap = 0;
}

void sh_csv_buf_free(ShCsvBuf *cb)
{
    if (!cb) return;
    free(cb->buf);
    cb->buf = NULL;
    cb->len = 0;
    cb->cap = 0;
}

void sh_csv_buf_reset(ShCsvBuf *cb)
{
    if (!cb) return;
    cb->len = 0;
    if (cb->buf) cb->buf[0] = '\0';
}

int sh_csv_buf_write(void *ctx, const char *data, size_t len)
{
    ShCsvBuf *cb = (ShCsvBuf *)ctx;
    if (!cb || !data) return -1;
    if (len == 0) return 0;

    while (cb->len + len + 1 > cb->cap) {
        size_t new_cap = cb->cap * 2;
        if (new_cap < 1024) new_cap = 1024;
        if (new_cap < cb->len + len + 1) new_cap = cb->len + len + 1;
        char *nb = (char *)realloc(cb->buf, new_cap);
        if (!nb) return -1;
        cb->buf = nb;
        cb->cap = new_cap;
    }

    memcpy(cb->buf + cb->len, data, len);
    cb->len += len;
    cb->buf[cb->len] = '\0';
    return 0;
}

char *sh_csv_buf_take(ShCsvBuf *cb, size_t *out_len)
{
    if (!cb || !cb->buf || cb->len == 0) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    char *result = cb->buf;
    if (out_len) *out_len = cb->len;
    cb->buf = NULL;
    cb->len = 0;
    cb->cap = 0;
    return result;
}
