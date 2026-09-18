/*
 * fuzz_fuelwise_api.c - fuelwise_api_handle() under libFuzzer
 *
 * Parses polylines, route segments and station lists, each an array that becomes a calloc.
 *
 * The shared body is in shared/tests/fuzz/fuzz_api_handler.h, which explains
 * what this checks beyond a crash.
 *
 * Build: make -C fuelwise fuzz-api
 * Run:   ./fuzz_fuelwise_api work/ tests/fuzz/corpus_api/ -max_total_time=60
 */

#include "fw_api.h"

#define FUZZ_API_CTX_TYPE      FWAPIContext
#define FUZZ_API_CREATE()      fw_api_create()
#define FUZZ_API_FREE(c)       fw_api_free(c)
#define FUZZ_API_HANDLE(c,q,s) fw_api_handle(c, q, s)
#define FUZZ_API_ROUTES        { "/api/v1/solve", "/api/v1/filter", "/api/v1/optimize", "/api/v1/health", "/api/v1/stats" }

#include "fuzz_api_handler.h"
