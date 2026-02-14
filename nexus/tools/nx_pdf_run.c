/*
 * nx_pdf_run.c - CLI driver for PDF table extraction with tunable options
 *
 * Usage:
 *   nx_pdf_run [--row-tol N] [--col-gap N] text_runs.json
 *
 * Defaults to auto-detection (from text heights) if no options given.
 */

#include "nx_pdf.h"
#include "sh_arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(int argc, char **argv)
{
    NxPdfOptions opts = NX_PDF_AUTO_OPTIONS;
    const char *input_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--row-tol") == 0 && i + 1 < argc) {
            char *end;
            opts.row_tolerance = strtod(argv[++i], &end);
            if (end == argv[i]) { fprintf(stderr, "Error: invalid --row-tol value\n"); return 1; }
        } else if (strcmp(argv[i], "--col-gap") == 0 && i + 1 < argc) {
            char *end;
            opts.col_gap_min = strtod(argv[++i], &end);
            if (end == argv[i]) { fprintf(stderr, "Error: invalid --col-gap value\n"); return 1; }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "Usage: %s [--row-tol N] [--col-gap N] input.json\n", argv[0]);
            fprintf(stderr, "\nAuto-detects parameters from text heights when not specified.\n");
            return 0;
        } else if (argv[i][0] != '-') {
            input_path = argv[i];
        } else {
            fprintf(stderr, "Usage: %s [--row-tol N] [--col-gap N] input.json\n", argv[0]);
            return 1;
        }
    }

    if (!input_path) {
        fprintf(stderr, "Usage: %s [--row-tol N] [--col-gap N] input.json\n", argv[0]);
        return 1;
    }

    size_t data_len = 0;
    char *data = read_file(input_path, &data_len);
    if (!data) return 1;

    SHArena *arena = sh_arena_create(64 * 1024 * 1024); /* 64 MB */
    if (!arena) { free(data); fprintf(stderr, "Arena alloc failed\n"); return 1; }

    char *json = NULL;
    size_t json_len = 0;

    if (opts.row_tolerance < 0 || opts.col_gap_min < 0)
        fprintf(stderr, "Auto-detecting clustering parameters...\n");
    else
        fprintf(stderr, "Options: row_tol=%.1f, col_gap=%.1f\n",
                opts.row_tolerance, opts.col_gap_min);

    NxPdfStatus s = nx_pdf_extract_tables(data, data_len, &opts, input_path,
                                           arena, NULL, &json, &json_len);
    if (s != NX_PDF_OK) {
        fprintf(stderr, "Failed: %s\n", nx_pdf_status_str(s));
        sh_arena_free(arena);
        free(data);
        return 1;
    }

    fprintf(stderr, "Effective: row_tol=%.2f, col_gap=%.2f\n",
            opts.row_tolerance, opts.col_gap_min);

    fwrite(json, 1, json_len, stdout);
    printf("\n");

    free(json);
    sh_arena_free(arena);
    free(data);
    return 0;
}
