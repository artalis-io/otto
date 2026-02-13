/*
 * nx_xform_run.c - CLI driver for Stage B (schema-driven transform)
 *
 * Usage:
 *   nx_xform_run raw.json schema.json
 *
 * Takes nx_raw JSON from Stage A and a transform schema,
 * outputs nx_canonical JSON to stdout.
 */

#include "nx_xform.h"
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
    if (argc < 3 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        fprintf(stderr, "Usage: %s raw.json schema.json\n", argv[0]);
        fprintf(stderr, "\nApplies schema transform (Stage B) to raw rows JSON.\n");
        return argc < 3 ? 1 : 0;
    }

    const char *raw_path = argv[1];
    const char *schema_path = argv[2];

    size_t raw_len = 0, schema_len = 0;
    char *raw = read_file(raw_path, &raw_len);
    if (!raw) return 1;

    char *schema = read_file(schema_path, &schema_len);
    if (!schema) { free(raw); return 1; }

    SHArena *arena = sh_arena_create(32 * 1024 * 1024); /* 32 MB */
    if (!arena) {
        free(raw);
        free(schema);
        fprintf(stderr, "Arena alloc failed\n");
        return 1;
    }

    char *out = NULL;
    size_t out_len = 0;

    NxXformStatus s = nx_xform_apply(raw, raw_len, schema, schema_len,
                                      arena, &out, &out_len);
    sh_arena_free(arena);

    if (s != NX_XFORM_OK) {
        fprintf(stderr, "Transform failed: %s\n", nx_xform_status_str(s));
        free(raw);
        free(schema);
        return 1;
    }

    fwrite(out, 1, out_len, stdout);
    printf("\n");

    free(out);
    free(raw);
    free(schema);
    return 0;
}
