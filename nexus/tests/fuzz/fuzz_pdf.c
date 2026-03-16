/*
 * fuzz_pdf.c - libFuzzer harness for PDF table reconstructor
 *
 * The PDF parser expects text-run JSON (not raw PDF bytes), so fuzzed
 * input = fuzzed JSON.
 *
 * Catches: malformed JSON, negative/NaN coordinates, huge page dimensions,
 * deeply nested JSON, missing required fields.
 *
 * Build: make -C nexus fuzz-pdf
 * Run:   ./fuzz_pdf tests/fuzz/corpus_pdf/ -max_len=1000000 -max_total_time=60
 */

#include "nx_pdf.h"
#include "sh_arena.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > 5 * 1024 * 1024) return 0;

    SHArena *arena = sh_arena_create(4 * 1024 * 1024);
    if (!arena) return 0;

    NxPdfOptions opts = NX_PDF_DEFAULT_OPTIONS;
    char *json = NULL;
    size_t json_len = 0;

    nx_pdf_extract_tables((const char *)data, size, &opts, "fuzz.pdf",
                          arena, NULL, &json, &json_len);

    free(json);
    sh_arena_free(arena);
    return 0;
}
