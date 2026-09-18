/*
 * fuzz_carta_api.c - ct_api_handle() under libFuzzer
 *
 * Carta is the one of the six whose parameters arrive in the *path*:
 * /tiles/{z}/{x}/{y}.{ext} is picked apart by a hand-written parser walking
 * strtol() and a bounded extension copy, and /tiles/{z}/{x}/{y}.txt reads the
 * query string on top of that. So this harness uses the shared header's prefix
 * mode, which appends the input to "/tiles/" and splits it at '?' the way a
 * server splits a request target -- rather than the usual fixed-path mode,
 * which would have left the tile parser untouched.
 *
 * The context is created empty, with no map data loaded. That reaches every
 * request-side parser, the zoom and coordinate validation, and the PNG, MVT
 * and ASCII encoders; what it does not reach is geometry clipping with real
 * features. That is deliberate. The features come from the PBF, not from the
 * request, so they would be identical on every execution and would not be
 * fuzzed by anything here -- and the PBF parser that produces them already has
 * its own harness in fuzz_pbf.c. Populating a context means hand-encoding a
 * valid OSM PBF, which is that other harness's job, not this one's.
 *
 * Build: make -C carta fuzz-api
 * Run:   ./fuzz_carta_api work/ tests/fuzz/corpus_api/ -max_total_time=60
 */

#include "carta.h"
#include "ct_api.h"
#include "ct_pbf.h"
#include <stdlib.h>
#include <string.h>

static CTPBFContext *g_pbf;
static CTAPIContext *g_ctx;

static CTAPIContext *fuzz_ctx(void)
{
    if (!g_ctx) {
        g_pbf = ct_pbf_context_create();
        if (!g_pbf) return NULL;
        /* _from_pbf, so the context does not take ownership and the single
         * PBF context survives for the life of the process. */
        g_ctx = ct_api_create_from_pbf(g_pbf, NULL);
    }
    return g_ctx;
}

#define FUZZ_API_CTX_TYPE      CTAPIContext
#define FUZZ_API_CREATE()      fuzz_ctx()
#define FUZZ_API_FREE(c)       ((void)(c))   /* built once, reused */
#define FUZZ_API_HANDLE(c,q,s) ct_api_handle(c, q, s)
#define FUZZ_API_ROUTES        { "/api/v1/health", "/api/v1/stats", \
                                 "/tiles.json" }
#define FUZZ_API_PATH_PREFIXES "/tiles/",

#include "fuzz_api_handler.h"
