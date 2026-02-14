/*
 * fuzz_xlsx.c - libFuzzer harness for XLSX parser
 *
 * Catches: malformed ZIP containers, invalid XML, out-of-bounds shared
 * string indices, integer overflows in cell coordinates, huge row/col
 * numbers.
 *
 * Build: make -C nexus fuzz-xlsx
 * Run:   ./fuzz_xlsx tests/fuzz/corpus_xlsx/ -max_len=1000000 -max_total_time=60
 */

#include "nx_xlsx.h"
#include "sh_arena.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > 10 * 1024 * 1024) return 0;

    SHArena *arena = sh_arena_create(4 * 1024 * 1024);
    if (!arena) return 0;

    NxXlsxLimits limits = { 1000, 100, 10, 10 * 1024 * 1024, 10000 };
    char *json = NULL;
    size_t json_len = 0;

    nx_xlsx_parse(data, size, &limits, "fuzz.xlsx", arena, NULL,
                  &json, &json_len);

    free(json);
    sh_arena_free(arena);
    return 0;
}
