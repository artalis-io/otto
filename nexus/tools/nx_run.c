/*
 * nx_run.c - CLI driver for Nexus ingestion pipeline
 *
 * Usage:
 *   nx_run --xlsx file.xlsx [--schema schema.json]
 *   nx_run --pdf-json text_runs.json [--schema schema.json]
 *
 * Outputs raw JSON (Stage A) to stdout.
 * If --schema provided, also outputs canonical JSON (Stage B) to stderr.
 */

#include "nx_ingest.h"
#include "nx_xlsx.h"
#include "nx_pdf.h"
#include "nx_xform.h"
#include "sh_arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open %s\n", path);
        return NULL;
    }
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

static void usage(const char *prog)
{
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s --xlsx file.xlsx [--schema schema.json]\n", prog);
    fprintf(stderr, "  %s --pdf-json text_runs.json [--schema schema.json]\n", prog);
    fprintf(stderr, "\nOutputs raw JSON (Stage A) to stdout.\n");
    fprintf(stderr, "If --schema provided, canonical JSON (Stage B) to stderr.\n");
}

int main(int argc, char **argv)
{
    const char *input_path = NULL;
    const char *schema_path = NULL;
    NxIngestFormat format = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--xlsx") == 0 && i + 1 < argc) {
            format = NX_FORMAT_XLSX;
            input_path = argv[++i];
        } else if (strcmp(argv[i], "--pdf-json") == 0 && i + 1 < argc) {
            format = NX_FORMAT_PDF_JSON;
            input_path = argv[++i];
        } else if (strcmp(argv[i], "--schema") == 0 && i + 1 < argc) {
            schema_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!input_path || (int)format == -1) {
        usage(argv[0]);
        return 1;
    }

    /* Read input file */
    size_t data_len = 0;
    char *data = read_file(input_path, &data_len);
    if (!data) return 1;

    /* Read schema if provided */
    char *schema = NULL;
    size_t schema_len = 0;
    if (schema_path) {
        schema = read_file(schema_path, &schema_len);
        if (!schema) { free(data); return 1; }
    }

    /* Extract filename from path */
    const char *filename = strrchr(input_path, '/');
    filename = filename ? filename + 1 : input_path;

    /* Run pipeline */
    char *raw = NULL, *canon = NULL;
    size_t raw_len = 0, canon_len = 0;

    NxIngestStatus s = nx_ingest(data, data_len, format, filename,
                                  schema, schema_len,
                                  &raw, &raw_len, &canon, &canon_len);

    if (s != NX_INGEST_OK) {
        fprintf(stderr, "Pipeline failed: %s\n", nx_ingest_status_str(s));
        free(data);
        free(schema);
        return 1;
    }

    /* Output raw JSON to stdout */
    if (raw && raw_len > 0) {
        fwrite(raw, 1, raw_len, stdout);
        printf("\n");
    }

    /* Output canonical JSON to stderr if schema was provided */
    if (canon && canon_len > 0) {
        fprintf(stderr, "\n=== Canonical JSON (Stage B) ===\n");
        fwrite(canon, 1, canon_len, stderr);
        fprintf(stderr, "\n");
    }

    free(raw);
    free(canon);
    free(data);
    free(schema);
    return 0;
}
