/*
 * fuzz_ralph_api.c - ralph_api_handle() under libFuzzer
 *
 * Takes LP and MPS problem text as well as JSON, so the body reaches two more parsers behind the handler.
 *
 * The shared body is in shared/tests/fuzz/fuzz_api_handler.h, which explains
 * what this checks beyond a crash.
 *
 * Build: make -C ralph fuzz-api
 * Run:   ./fuzz_ralph_api work/ tests/fuzz/corpus_api/ -max_total_time=60
 */

#include "ralph_api.h"

#define FUZZ_API_CTX_TYPE      RalphAPIContext
#define FUZZ_API_CREATE()      ralph_api_create()
#define FUZZ_API_FREE(c)       ralph_api_free(c)
#define FUZZ_API_HANDLE(c,q,s) ralph_api_handle(c, q, s)
#define FUZZ_API_ROUTES        { "/api/v1/solve", "/api/v1/health", "/api/v1/formats" }

#include "fuzz_api_handler.h"
