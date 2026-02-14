/*
 * fuzz_csv.c - libFuzzer harness for CSV parser
 *
 * Catches: BOM handling, embedded nulls, unclosed quotes, ragged rows,
 * enormous field lengths, binary data.
 *
 * Build: make -C nexus fuzz-csv
 * Run:   ./fuzz_csv tests/fuzz/corpus_csv/ -max_len=1000000 -max_total_time=60
 */

#include "nx_csv.h"
#include "sh_arena.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > 10 * 1024 * 1024) return 0;

    SHArena *arena = sh_arena_create(4 * 1024 * 1024);
    if (!arena) return 0;

    NxCsvLimits limits = { 10000, 100 };
    char *json = NULL;
    size_t json_len = 0;

    nx_csv_parse((const char *)data, size, NULL, &limits, "fuzz.csv",
                 arena, NULL, &json, &json_len);

    free(json);
    sh_arena_free(arena);
    return 0;
}
