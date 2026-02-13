/*
 * nx_pipeline.c - Unified Nexus Document Ingestion Pipeline (C)
 *
 * End-to-end pipeline: document → raw JSON → canonical JSON.
 * All processing fully in-process (no external dependencies).
 *
 * Usage:
 *   nx_pipeline input.pdf [--schema schema.json] [--row-tol N] [--col-gap N]
 *   nx_pipeline input.xlsx [--schema schema.json]
 *   nx_pipeline input.json [--schema schema.json]   (pre-extracted text-run JSON)
 *   nx_pipeline --config batch.json
 *
 * Output:
 *   --raw    → raw JSON to stdout (default when no schema)
 *   --canon  → canonical JSON to stdout (default when schema given)
 *   -o file  → write to file instead of stdout
 */

#include "nx_xlsx.h"
#include "nx_pdf.h"
#include "nx_xform.h"
#include "sh_arena.h"
#include "sh_json.h"
#include "sh_pdf2struc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <libgen.h>

#define PIPELINE_ARENA_SIZE (64 * 1024 * 1024) /* 64 MB */
#define MAX_PDF_TEXT_SIZE   (32 * 1024 * 1024)  /* 32 MB max text-run JSON */

/* ============================================================================
 * File I/O
 * ============================================================================ */

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Error: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    *out_len = rd;
    return buf;
}

static int write_file(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Error: cannot write %s\n", path); return -1; }
    fwrite(data, 1, len, f);
    fputc('\n', f);
    fclose(f);
    return 0;
}

/* ============================================================================
 * Format Detection
 * ============================================================================ */

typedef enum { FMT_XLSX, FMT_PDF, FMT_PDF_JSON, FMT_UNKNOWN } FileFormat;

static FileFormat detect_format(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return FMT_UNKNOWN;
    if (strcasecmp(dot, ".xlsx") == 0) return FMT_XLSX;
    if (strcasecmp(dot, ".pdf") == 0) return FMT_PDF;
    if (strcasecmp(dot, ".json") == 0) return FMT_PDF_JSON;
    return FMT_UNKNOWN;
}

static const char *format_name(FileFormat fmt)
{
    switch (fmt) {
        case FMT_XLSX:     return "xlsx";
        case FMT_PDF:      return "pdf";
        case FMT_PDF_JSON: return "pdf-json";
        default:           return "unknown";
    }
}

/* ============================================================================
 * PDF Text Extraction via sh_pdf2struc (pure C, no external dependencies)
 * ============================================================================ */

/* Collector for sh_pdf2struc blocks → text-run JSON */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    int    cur_page;   /* Current page being written (-1 = none) */
    int    text_count; /* Texts in current page */
    int    error;      /* 1 if OOM */
} PdfJsonCollector;

static void json_append(PdfJsonCollector *c, const char *s, size_t slen)
{
    if (c->error) return;
    while (c->len + slen + 1 > c->cap) {
        size_t newcap = c->cap * 2;
        if (newcap > MAX_PDF_TEXT_SIZE) { c->error = 1; return; }
        char *nb = (char *)realloc(c->buf, newcap);
        if (!nb) { c->error = 1; return; }
        c->buf = nb;
        c->cap = newcap;
    }
    memcpy(c->buf + c->len, s, slen);
    c->len += slen;
}

static void json_appendf(PdfJsonCollector *c, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) json_append(c, tmp, (size_t)n);
}

/* Escape a string for JSON output */
static void json_append_escaped(PdfJsonCollector *c, const char *s)
{
    json_append(c, "\"", 1);
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '"':  json_append(c, "\\\"", 2); break;
            case '\\': json_append(c, "\\\\", 2); break;
            case '\b': json_append(c, "\\b", 2); break;
            case '\f': json_append(c, "\\f", 2); break;
            case '\n': json_append(c, "\\n", 2); break;
            case '\r': json_append(c, "\\r", 2); break;
            case '\t': json_append(c, "\\t", 2); break;
            default:
                if ((unsigned char)*p < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)*p);
                    json_append(c, esc, 6);
                } else {
                    json_append(c, p, 1);
                }
                break;
        }
    }
    json_append(c, "\"", 1);
}

static void close_current_page(PdfJsonCollector *c)
{
    if (c->cur_page >= 0) {
        json_append(c, "]}", 2); /* Close texts array and page object */
    }
}

static void pdf_block_callback(void *user, const ShPdf2strucBlock *block)
{
    PdfJsonCollector *c = (PdfJsonCollector *)user;
    if (c->error) return;

    /* Start new page if needed */
    if (block->page_index != c->cur_page) {
        close_current_page(c);
        if (c->cur_page >= 0) json_append(c, ",", 1);
        json_appendf(c, "{\"page\":%d,\"width\":842.0,\"height\":595.0,\"texts\":[",
                     block->page_index + 1);
        c->cur_page = block->page_index;
        c->text_count = 0;
    }

    /* Add text entry */
    if (c->text_count > 0) json_append(c, ",", 1);
    json_append(c, "{\"text\":", 8);
    json_append_escaped(c, block->text);
    json_appendf(c, ",\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f}",
                 block->x, block->y, block->w, block->h);
    c->text_count++;
}

/*
 * Extract text runs from PDF using sh_pdf2struc (pure C).
 * Returns heap-allocated JSON string in pdfplumber-compatible format.
 */
static char *extract_pdf_text(const char *pdf_path, size_t *out_len)
{
    ShPdf2strucCtx *ctx = sh_pdf2struc_create();
    if (!ctx) {
        fprintf(stderr, "Error: sh_pdf2struc_create failed\n");
        return NULL;
    }

    ShPdf2strucOpts opts;
    sh_pdf2struc_opts_default(&opts);
    opts.emit_mode = SH_PDF2STRUC_EMIT_BLOCKS;
    opts.origin_top_left = 1;

    /* Set up JSON collector */
    PdfJsonCollector collector = {0};
    collector.cap = 256 * 1024;
    collector.cur_page = -1;
    collector.buf = (char *)malloc(collector.cap);
    if (!collector.buf) {
        sh_pdf2struc_destroy(ctx);
        return NULL;
    }

    /* Start JSON */
    json_append(&collector, "{\"pages\":[", 10);

    /* Extract */
    ShPdf2strucStatus st = sh_pdf2struc_extract_file(
        ctx, pdf_path, &opts, pdf_block_callback, &collector);

    if (st != SH_PDF2STRUC_OK) {
        fprintf(stderr, "Error: PDF extraction failed: %s\n",
                sh_pdf2struc_last_error(ctx));
        sh_pdf2struc_destroy(ctx);
        free(collector.buf);
        return NULL;
    }

    sh_pdf2struc_destroy(ctx);

    if (collector.error) {
        fprintf(stderr, "Error: PDF text output exceeds %d MB\n",
                (int)(MAX_PDF_TEXT_SIZE / (1024 * 1024)));
        free(collector.buf);
        return NULL;
    }

    /* Close JSON */
    close_current_page(&collector);
    json_append(&collector, "]}", 2);
    collector.buf[collector.len] = '\0';

    *out_len = collector.len;
    return collector.buf;
}

/* ============================================================================
 * Pipeline Processing
 * ============================================================================ */

typedef struct {
    const char *input_path;
    const char *schema_path;
    const char *output_path;
    double row_tol;          /* -1 = auto */
    double col_gap;          /* -1 = auto */
    int output_raw;          /* 1 = output raw, 0 = output canonical if schema */
} PipelineOpts;

static int process_file(const PipelineOpts *po)
{
    FileFormat fmt = detect_format(po->input_path);
    if (fmt == FMT_UNKNOWN) {
        fprintf(stderr, "Error: cannot detect format of %s\n", po->input_path);
        return 1;
    }

    const char *basename_str = strrchr(po->input_path, '/');
    basename_str = basename_str ? basename_str + 1 : po->input_path;
    fprintf(stderr, "\n[%s] format=%s\n", basename_str, format_name(fmt));

    /* Stage A: Extract raw rows JSON */
    char *raw_json = NULL;
    size_t raw_len = 0;

    SHArena *arena = sh_arena_create(PIPELINE_ARENA_SIZE);
    if (!arena) { fprintf(stderr, "Error: arena allocation failed\n"); return 1; }

    if (fmt == FMT_PDF) {
        /* PDF: extract text via sh_pdf2struc (pure C), then cluster */
        fprintf(stderr, "  Stage A.1: Extracting text runs (sh_pdf2struc)...\n");
        size_t text_len = 0;
        char *text_json = extract_pdf_text(po->input_path, &text_len);
        if (!text_json) { sh_arena_free(arena); return 1; }

        fprintf(stderr, "  Stage A.2: Clustering into table rows...\n");
        NxPdfOptions opts = NX_PDF_AUTO_OPTIONS;
        if (po->row_tol >= 0) opts.row_tolerance = po->row_tol;
        if (po->col_gap >= 0) opts.col_gap_min = po->col_gap;

        NxPdfStatus ps = nx_pdf_extract_tables(text_json, text_len,
                                                &opts, basename_str,
                                                arena, &raw_json, &raw_len);
        free(text_json);

        if (ps != NX_PDF_OK) {
            fprintf(stderr, "  Error: PDF clustering failed: %s\n",
                    nx_pdf_status_str(ps));
            sh_arena_free(arena);
            return 1;
        }
        fprintf(stderr, "  Effective: row_tol=%.2f, col_gap=%.2f\n",
                opts.row_tolerance, opts.col_gap_min);

    } else if (fmt == FMT_PDF_JSON) {
        /* Pre-extracted text-run JSON */
        fprintf(stderr, "  Stage A: Clustering text runs...\n");
        size_t data_len = 0;
        char *data = read_file(po->input_path, &data_len);
        if (!data) { sh_arena_free(arena); return 1; }

        NxPdfOptions opts = NX_PDF_AUTO_OPTIONS;
        if (po->row_tol >= 0) opts.row_tolerance = po->row_tol;
        if (po->col_gap >= 0) opts.col_gap_min = po->col_gap;

        NxPdfStatus ps = nx_pdf_extract_tables(data, data_len,
                                                &opts, basename_str,
                                                arena, &raw_json, &raw_len);
        free(data);

        if (ps != NX_PDF_OK) {
            fprintf(stderr, "  Error: PDF clustering failed: %s\n",
                    nx_pdf_status_str(ps));
            sh_arena_free(arena);
            return 1;
        }
        fprintf(stderr, "  Effective: row_tol=%.2f, col_gap=%.2f\n",
                opts.row_tolerance, opts.col_gap_min);

    } else if (fmt == FMT_XLSX) {
        /* XLSX: parse directly */
        fprintf(stderr, "  Stage A: Parsing XLSX...\n");
        size_t data_len = 0;
        char *data = read_file(po->input_path, &data_len);
        if (!data) { sh_arena_free(arena); return 1; }

        NxXlsxStatus xs = nx_xlsx_parse(data, data_len, NULL, basename_str,
                                         arena, &raw_json, &raw_len);
        free(data);

        if (xs != NX_XLSX_OK) {
            fprintf(stderr, "  Error: XLSX parse failed: %s\n",
                    nx_xlsx_status_str(xs));
            sh_arena_free(arena);
            return 1;
        }
    }

    sh_arena_free(arena);

    if (!raw_json) {
        fprintf(stderr, "  Error: Stage A produced no output\n");
        return 1;
    }

    /* Print raw summary */
    {
        SHArena *pa = sh_arena_create(256 * 1024);
        if (pa) {
            ShJsonValue *root = NULL;
            if (sh_json_parse(raw_json, raw_len, pa, &root) == SH_JSON_OK) {
                ShJsonValue *tables = sh_json_get(root, "tables");
                if (tables && sh_json_array_len(tables) > 0) {
                    ShJsonValue *t0 = sh_json_array_get(tables, 0);
                    fprintf(stderr, "  Raw: %d rows, %d cols\n",
                            sh_json_as_int(sh_json_get(t0, "row_count"), 0),
                            sh_json_as_int(sh_json_get(t0, "col_count"), 0));
                }
            }
            sh_arena_free(pa);
        }
    }

    /* Stage B: Schema transform (optional) */
    char *canon_json = NULL;
    size_t canon_len = 0;

    if (po->schema_path) {
        fprintf(stderr, "  Stage B: Applying schema %s...\n",
                strrchr(po->schema_path, '/') ?
                    strrchr(po->schema_path, '/') + 1 : po->schema_path);

        size_t schema_file_len = 0;
        char *schema = read_file(po->schema_path, &schema_file_len);
        if (!schema) { free(raw_json); return 1; }

        SHArena *arena_b = sh_arena_create(PIPELINE_ARENA_SIZE);
        if (!arena_b) {
            free(schema);
            free(raw_json);
            fprintf(stderr, "  Error: arena allocation failed\n");
            return 1;
        }

        NxXformStatus ts = nx_xform_apply(raw_json, raw_len,
                                           schema, schema_file_len,
                                           arena_b, &canon_json, &canon_len);
        sh_arena_free(arena_b);
        free(schema);

        if (ts != NX_XFORM_OK) {
            fprintf(stderr, "  Error: transform failed: %s\n",
                    nx_xform_status_str(ts));
            free(raw_json);
            return 1;
        }

        /* Print canonical summary */
        SHArena *pa = sh_arena_create(256 * 1024);
        if (pa) {
            ShJsonValue *root = NULL;
            if (sh_json_parse(canon_json, canon_len, pa, &root) == SH_JSON_OK) {
                ShJsonValue *audit = sh_json_get(root, "audit");
                if (audit) {
                    fprintf(stderr, "  Canonical: %d accepted, %d rejected\n",
                            sh_json_as_int(sh_json_get(audit, "rows_accepted"), 0),
                            sh_json_as_int(sh_json_get(audit, "rows_rejected"), 0));
                }
            }
            sh_arena_free(pa);
        }
    }

    /* Output */
    const char *output;
    size_t output_len;
    if (po->output_raw || !canon_json) {
        output = raw_json;
        output_len = raw_len;
    } else {
        output = canon_json;
        output_len = canon_len;
    }

    if (po->output_path) {
        write_file(po->output_path, output, output_len);
        fprintf(stderr, "  Wrote: %s\n", po->output_path);
    } else {
        fwrite(output, 1, output_len, stdout);
        printf("\n");
    }

    free(raw_json);
    free(canon_json);
    return 0;
}

/* ============================================================================
 * Batch Config Mode
 * ============================================================================ */

static int run_batch(const char *config_path)
{
    size_t config_len = 0;
    char *config_data = read_file(config_path, &config_len);
    if (!config_data) return 1;

    SHArena *arena = sh_arena_create(1024 * 1024);
    if (!arena) { free(config_data); return 1; }

    ShJsonValue *root = NULL;
    if (sh_json_parse(config_data, config_len, arena, &root) != SH_JSON_OK) {
        fprintf(stderr, "Error: invalid config JSON\n");
        sh_arena_free(arena);
        free(config_data);
        return 1;
    }

    /* Get output directory */
    const char *output_dir = sh_json_as_string(
        sh_json_get(root, "output_dir"), "./output");

    /* Resolve relative to config file location */
    char config_dir[512];
    snprintf(config_dir, sizeof(config_dir), "%s", config_path);
    char *last_slash = strrchr(config_dir, '/');
    if (last_slash) *last_slash = '\0';
    else snprintf(config_dir, sizeof(config_dir), ".");

    char abs_output_dir[512];
    if (output_dir[0] == '/')
        snprintf(abs_output_dir, sizeof(abs_output_dir), "%s", output_dir);
    else
        snprintf(abs_output_dir, sizeof(abs_output_dir), "%s/%s",
                 config_dir, output_dir);

    /* Create output directory */
    char mkdir_cmd[600];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s\"", abs_output_dir);
    (void)system(mkdir_cmd);

    ShJsonValue *sources = sh_json_get(root, "sources");
    if (!sources) {
        fprintf(stderr, "Error: no 'sources' array in config\n");
        sh_arena_free(arena);
        free(config_data);
        return 1;
    }

    int nsources = (int)sh_json_array_len(sources);
    fprintf(stderr, "Batch: %d sources, output to %s\n",
            nsources, abs_output_dir);

    int failures = 0;
    for (int i = 0; i < nsources; i++) {
        ShJsonValue *src = sh_json_array_get(sources, (size_t)i);

        const char *file = sh_json_as_string(sh_json_get(src, "file"), "");
        const char *schema = sh_json_as_string(sh_json_get(src, "schema"), NULL);

        /* Resolve relative paths */
        char abs_file[512], abs_schema[512];
        if (file[0] == '/')
            snprintf(abs_file, sizeof(abs_file), "%s", file);
        else
            snprintf(abs_file, sizeof(abs_file), "%s/%s", config_dir, file);

        const char *schema_ptr = NULL;
        if (schema) {
            if (schema[0] == '/')
                snprintf(abs_schema, sizeof(abs_schema), "%s", schema);
            else
                snprintf(abs_schema, sizeof(abs_schema), "%s/%s",
                         config_dir, schema);
            schema_ptr = abs_schema;
        }

        /* PDF options */
        double row_tol = -1, col_gap = -1;
        ShJsonValue *pdf_opts = sh_json_get(src, "pdf_options");
        if (pdf_opts) {
            ShJsonValue *rt = sh_json_get(pdf_opts, "row_tolerance");
            ShJsonValue *cg = sh_json_get(pdf_opts, "col_gap_min");
            if (rt && sh_json_type(rt) == SH_JSON_NUMBER)
                row_tol = sh_json_as_double(rt, -1);
            if (cg && sh_json_type(cg) == SH_JSON_NUMBER)
                col_gap = sh_json_as_double(cg, -1);
        }

        /* Build output path: output_dir/filename_raw.json */
        const char *base = strrchr(abs_file, '/');
        base = base ? base + 1 : abs_file;
        char name[256];
        snprintf(name, sizeof(name), "%s", base);
        char *dot = strrchr(name, '.');
        if (dot) *dot = '\0';

        char out_raw[512], out_canon[512];
        snprintf(out_raw, sizeof(out_raw), "%s/%s_raw.json",
                 abs_output_dir, name);

        /* Process: always output raw */
        PipelineOpts po = {0};
        po.input_path = abs_file;
        po.schema_path = NULL;
        po.output_path = out_raw;
        po.row_tol = row_tol;
        po.col_gap = col_gap;
        po.output_raw = 1;

        if (process_file(&po) != 0) {
            failures++;
            continue;
        }

        /* If schema given, also produce canonical */
        if (schema_ptr) {
            snprintf(out_canon, sizeof(out_canon), "%s/%s_canonical.json",
                     abs_output_dir, name);

            /* Read back the raw output and apply schema */
            size_t raw_len = 0;
            char *raw = read_file(out_raw, &raw_len);
            if (!raw) { failures++; continue; }

            size_t schema_len = 0;
            char *schema_data = read_file(schema_ptr, &schema_len);
            if (!schema_data) { free(raw); failures++; continue; }

            SHArena *arena_b = sh_arena_create(PIPELINE_ARENA_SIZE);
            if (!arena_b) { free(raw); free(schema_data); failures++; continue; }

            char *canon = NULL;
            size_t canon_len = 0;
            const char *schema_base = strrchr(schema_ptr, '/');
            schema_base = schema_base ? schema_base + 1 : schema_ptr;
            fprintf(stderr, "  Stage B: Applying schema %s...\n", schema_base);

            NxXformStatus ts = nx_xform_apply(raw, raw_len,
                                               schema_data, schema_len,
                                               arena_b, &canon, &canon_len);
            sh_arena_free(arena_b);
            free(schema_data);
            free(raw);

            if (ts != NX_XFORM_OK) {
                fprintf(stderr, "  Error: transform failed: %s\n",
                        nx_xform_status_str(ts));
                failures++;
            } else {
                write_file(out_canon, canon, canon_len);
                fprintf(stderr, "  Wrote: %s\n", out_canon);
                free(canon);
            }
        }
    }

    sh_arena_free(arena);
    free(config_data);

    fprintf(stderr, "\nBatch complete. %d/%d succeeded.\n",
            nsources - failures, nsources);
    return failures > 0 ? 1 : 0;
}

/* ============================================================================
 * CLI
 * ============================================================================ */

static void usage(const char *prog)
{
    fprintf(stderr, "Nexus Document Ingestion Pipeline\n\n");
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s input.pdf [--schema s.json] [--row-tol N] [--col-gap N]\n", prog);
    fprintf(stderr, "  %s input.xlsx [--schema s.json]\n", prog);
    fprintf(stderr, "  %s --config batch.json\n\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --schema    Transform schema for Stage B\n");
    fprintf(stderr, "  --row-tol   PDF row tolerance (default: auto-detect)\n");
    fprintf(stderr, "  --col-gap   PDF column gap minimum (default: auto-detect)\n");
    fprintf(stderr, "  --config    Batch config JSON file\n");
    fprintf(stderr, "  --raw       Output raw JSON even when schema given\n");
    fprintf(stderr, "  -o FILE     Write output to file\n");
}

int main(int argc, char **argv)
{
    PipelineOpts po = {0};
    po.row_tol = -1;
    po.col_gap = -1;
    const char *config_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--schema") == 0 && i + 1 < argc) {
            po.schema_path = argv[++i];
        } else if (strcmp(argv[i], "--row-tol") == 0 && i + 1 < argc) {
            po.row_tol = atof(argv[++i]);
        } else if (strcmp(argv[i], "--col-gap") == 0 && i + 1 < argc) {
            po.col_gap = atof(argv[++i]);
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0)
                   && i + 1 < argc) {
            po.output_path = argv[++i];
        } else if (strcmp(argv[i], "--raw") == 0) {
            po.output_raw = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            po.input_path = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (config_path)
        return run_batch(config_path);

    if (!po.input_path) {
        usage(argv[0]);
        return 1;
    }

    return process_file(&po);
}
