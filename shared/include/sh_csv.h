/*
 * sh_csv.h - Streaming CSV Parser (RFC 4180)
 *
 * Pull-style parser: call sh_csv_next() in a loop to get fields and row boundaries.
 * Supports quoted fields, escaped quotes, CRLF/LF, BOM skip, delimiter auto-detection.
 *
 * Dependencies: sh_arena.h
 *
 * Usage:
 *   SHArena *arena = sh_arena_create(4096);
 *   ShCsvReader r;
 *   ShCsvOpts opts;
 *   sh_csv_opts_default(&opts);
 *   sh_csv_init(&r, csv_data, csv_len, &opts, arena);
 *
 *   ShCsvToken tok;
 *   ShCsvTokenType type;
 *   while ((type = sh_csv_next(&r, &tok)) != SH_CSV_TOKEN_EOF) {
 *       if (type == SH_CSV_TOKEN_FIELD) {
 *           printf("row %d col %d: %.*s\n", r.row, r.col, (int)tok.len, tok.data);
 *       } else if (type == SH_CSV_TOKEN_ROW_END) {
 *           printf("--- end of row %d ---\n", r.row - 1);
 *       }
 *   }
 *   sh_arena_free(arena);
 */

#ifndef SH_CSV_H
#define SH_CSV_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SHArena SHArena;

typedef enum {
    SH_CSV_OK = 0,
    SH_CSV_ERR_NULL,
    SH_CSV_ERR_ENCODING,
    SH_CSV_ERR_LIMITS
} ShCsvStatus;

typedef enum {
    SH_CSV_TOKEN_FIELD,
    SH_CSV_TOKEN_ROW_END,
    SH_CSV_TOKEN_EOF
} ShCsvTokenType;

typedef struct {
    const char *data;
    size_t      len;
} ShCsvToken;

typedef struct {
    char     delimiter;       /* default ',' (0 = auto-detect) */
    char     quote;           /* default '"' */
    int      has_header;      /* 1 = first row is header */
    int      skip_empty_rows; /* 1 = skip blank lines */
    int      trim_fields;     /* 1 = trim whitespace from unquoted fields */
    size_t   max_field_len;   /* default 32768 */
    size_t   max_columns;     /* default 1024 */
} ShCsvOpts;

typedef struct {
    const char *data;
    size_t      len;
    size_t      pos;
    char        delimiter;
    char        quote;
    int         skip_empty_rows;
    int         trim_fields;
    size_t      max_field_len;
    size_t      max_columns;
    int         row;           /* 0-based current row */
    int         col;           /* 0-based current column */
    int         at_row_start;  /* internal: start of row flag */
    int         need_row_end;  /* internal: ROW_END pending */
    int         eof;           /* internal: hit end */
    SHArena    *arena;
} ShCsvReader;

/*
 * Initialize options to defaults.
 *
 * @param opts  Options to initialize
 */
void sh_csv_opts_default(ShCsvOpts *opts);

/*
 * Initialize reader with CSV data.
 *
 * @param r     Reader to initialize
 * @param data  CSV data buffer (not modified, not copied)
 * @param len   Length of data
 * @param opts  Options (NULL for defaults)
 * @param arena Arena for string allocations
 * @return SH_CSV_OK on success, error code otherwise
 */
ShCsvStatus sh_csv_init(ShCsvReader *r, const char *data, size_t len,
                        const ShCsvOpts *opts, SHArena *arena);

/*
 * Read next token.
 *
 * Returns SH_CSV_TOKEN_FIELD for each field, SH_CSV_TOKEN_ROW_END at end of
 * each row, SH_CSV_TOKEN_EOF at end of input.
 *
 * Sequence for a 2-col, 2-row CSV: FIELD FIELD ROW_END FIELD FIELD ROW_END EOF
 *
 * @param r   Reader
 * @param tok Token output (filled when type is FIELD)
 * @return Token type
 */
ShCsvTokenType sh_csv_next(ShCsvReader *r, ShCsvToken *tok);

/*
 * Get current row number (0-based).
 *
 * @param r Reader
 * @return Current row number, or -1 if r is NULL
 */
int sh_csv_row_number(const ShCsvReader *r);

/*
 * Get current column number (0-based).
 *
 * @param r Reader
 * @return Current column number, or -1 if r is NULL
 */
int sh_csv_column_number(const ShCsvReader *r);

/* ============================================================================
 * CSV Writer (RFC 4180)
 *
 * Streaming writer that outputs via a callback function (like ShJsonWriter).
 * Auto-quotes fields containing delimiter, '"', '\r', or '\n'.
 * Doubles any '"' inside quoted fields per RFC 4180.
 *
 * Usage with ShCsvBuf (in-memory buffer):
 *   ShCsvBuf cb;
 *   sh_csv_buf_init(&cb);
 *   ShCsvWriter w;
 *   sh_csv_writer_init(&w, sh_csv_buf_write, &cb, ',');
 *   sh_csv_write_field_str(&w, "hello");
 *   sh_csv_write_field_str(&w, "world");
 *   sh_csv_write_row_end(&w);
 *   char *csv = sh_csv_buf_take(&cb, NULL);
 *   // ... use csv ...
 *   free(csv);
 * ============================================================================ */

/* Write callback: return 0 on success, non-zero on error */
typedef int (*ShCsvWriteFn)(void *ctx, const char *data, size_t len);

typedef struct {
    ShCsvWriteFn write_fn;  /* Output callback */
    void        *ctx;       /* Callback context */
    char         delimiter; /* Field separator (default ',') */
    int          col;       /* Column index within current row (for delimiter) */
    int          error;     /* Sticky error flag */
} ShCsvWriter;

/* Initialize writer with output callback. Delimiter 0 defaults to ','. */
void sh_csv_writer_init(ShCsvWriter *w, ShCsvWriteFn write_fn, void *ctx,
                        char delimiter);

/* Check if writer has encountered an error. */
int sh_csv_writer_error(const ShCsvWriter *w);

/* Write a field. Auto-quotes if needed. NULL value -> empty field. */
int sh_csv_write_field(ShCsvWriter *w, const char *value, size_t len);

/* Write a field from a null-terminated string (convenience). */
int sh_csv_write_field_str(ShCsvWriter *w, const char *value);

/* End the current row (writes \r\n per RFC 4180). */
int sh_csv_write_row_end(ShCsvWriter *w);

/* ----------------------------------------------------------------------------
 * Buffer Helper (like ShJsonBuf)
 *
 * Growable buffer for building CSV in memory.
 * ---------------------------------------------------------------------------- */

typedef struct {
    char  *buf;    /* Growable buffer (null-terminated) */
    size_t len;    /* Current length (excludes null terminator) */
    size_t cap;    /* Capacity */
} ShCsvBuf;

/* Initialize an empty CSV buffer. */
void sh_csv_buf_init(ShCsvBuf *cb);

/* Free the buffer. */
void sh_csv_buf_free(ShCsvBuf *cb);

/* Reset buffer for reuse (keeps allocated memory). */
void sh_csv_buf_reset(ShCsvBuf *cb);

/* Write callback for ShCsvWriter. Pass to sh_csv_writer_init() with ShCsvBuf*. */
int sh_csv_buf_write(void *ctx, const char *data, size_t len);

/* Take ownership of the buffer. Caller must free(). Buffer resets to empty.
 * *out_len set to buffer length (may be NULL). Returns NULL if empty. */
char *sh_csv_buf_take(ShCsvBuf *cb, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* SH_CSV_H */
