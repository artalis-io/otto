/*
 * fuzz_surge_api.c - surge_api_handle() under libFuzzer
 *
 * 2247 lines, the largest of the six, and the one that turns a body into a full VRP model: vehicles, tasks, travel matrices, time windows, commodities, exclusion groups. The one defect the read of these files found was here.
 *
 * The shared body is in shared/tests/fuzz/fuzz_api_handler.h, which explains
 * what this checks beyond a crash.
 *
 * Build: make -C surge fuzz-api
 * Run:   ./fuzz_surge_api work/ tests/fuzz/corpus_api/ -max_total_time=60
 */

#include "sg_api.h"

#define FUZZ_API_CTX_TYPE      SGAPIContext
#define FUZZ_API_CREATE()      sg_api_create()
#define FUZZ_API_FREE(c)       sg_api_free(c)
#define FUZZ_API_HANDLE(c,q,s) sg_api_handle(c, q, s)
#define FUZZ_API_ROUTES        { "/api/v1/solve", "/api/v1/health", "/api/v1/stats", "/api/v1/version" }

#include "fuzz_api_handler.h"
