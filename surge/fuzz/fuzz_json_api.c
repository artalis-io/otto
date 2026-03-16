/*
 * libFuzzer harness for Surge JSON API.
 *
 * Fuzzes sg_api_solve() — the main entry point that parses arbitrary JSON
 * and builds a VRP model. This exercises all JSON parsing, validation,
 * and model construction paths.
 *
 * Build:
 *   make -C surge fuzz    (requires clang with -fsanitize=fuzzer)
 *
 * Run:
 *   ./surge/fuzz_json_api corpus/ -max_len=65536 -jobs=4
 *
 * Seed corpus: place valid JSON models in surge/fuzz/corpus/
 */

#include "sg_api.h"
#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    int status_code = 0;
    size_t out_len = 0;
    char *response;

    /* Cap input to prevent excessive memory allocation on huge inputs */
    if (size > 256 * 1024) return 0;

    response = sg_api_solve((const char *)data, size, &status_code, &out_len);
    free(response);

    return 0;
}
