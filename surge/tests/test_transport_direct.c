/*
 * test_transport_direct.c - Surge's half of the transport leak detector.
 *
 * Drives sg_api_handle() through sh_transport_direct, with NO transport
 * library on the include path. If someone lets a Keel type into the handler
 * signature, the shared API types, or anything they pull in, this file stops
 * compiling -- which is the point.
 *
 * The streaming and JSON-escaping paths live in shared/ and are already
 * exercised once by ralph/tests/test_transport_direct.c. Repeating them per
 * module would test the same code six times and hide what is actually
 * module-specific, so this file sticks to Surge's own handler.
 *
 * See docs/roadmaps/transport.md ("Invariant to enforce in CI").
 */
#include <stdio.h>
#include <string.h>

#include "sg_api.h"
#include "sh_api.h"
#include "sh_transport.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(int cond, const char *what)
{
    if (cond) { g_pass++; printf("  PASS: %s\n", what); }
    else      { g_fail++; printf("  FAIL: %s\n", what); }
}

/* The whole point: a plain ShApiHandler, no cast. If sg_api_handle ever stops
 * matching the shared signature this line fails to compile. */
static ShApiHandler g_handler = sg_api_handle;

static int call(SGAPIContext *ctx, const char *method, const char *path,
                const char *query, ShApiResponse *resp)
{
    ShApiRequest req;
    memset(&req, 0, sizeof(req));
    req.method = method;
    req.path = path;
    req.query = query;
    return sh_transport_direct_call(g_handler, ctx, &req, resp);
}

int main(void)
{
    SGAPIContext *ctx;
    ShApiResponse resp;

    printf("=== Transport: direct driver over sg_api_handle ===\n");

    ctx = sg_api_create();
    check(ctx != NULL, "API context created");
    if (!ctx) return 1;

    /* Health: the handler routes on req->path. The transport knows nothing
     * about routes and must not need to. */
    memset(&resp, 0, sizeof(resp));
    check(call(ctx, "GET", "/api/v1/health", NULL, &resp) == 0,
          "Direct call returns 0 for GET /api/v1/health");
    check(resp.status_code == 200, "Health responds 200");
    check(resp.body != NULL && resp.body_len > 0, "Health has a body");
    check(resp.content_type != NULL &&
          strstr(resp.content_type, "json") != NULL,
          "Health content type is JSON");
    sh_api_response_free(&resp);

    memset(&resp, 0, sizeof(resp));
    check(call(ctx, "GET", "/api/v1/version", NULL, &resp) == 0,
          "Direct call returns 0 for GET /api/v1/version");
    check(resp.status_code == 200, "Version responds 200");
    sh_api_response_free(&resp);

    memset(&resp, 0, sizeof(resp));
    check(call(ctx, "GET", "/api/v1/stats", NULL, &resp) == 0,
          "Direct call returns 0 for GET /api/v1/stats");
    check(resp.status_code == 200, "Stats responds 200");
    sh_api_response_free(&resp);

    /* Routing decisions belong to the handler, including the 404. A transport
     * must never have to invent one. */
    memset(&resp, 0, sizeof(resp));
    check(call(ctx, "GET", "/api/v1/nope", NULL, &resp) == 0,
          "Unknown path is handled, not an error");
    check(resp.status_code == 404, "Unknown path responds 404 from the handler");
    sh_api_response_free(&resp);

    /* Transports do this on error paths, so it has to be safe. */
    memset(&resp, 0, sizeof(resp));
    sh_api_response_free(&resp);
    sh_api_response_free(&resp);
    check(resp.body == NULL, "Freeing an empty response twice is safe");

    check(sh_transport_direct.name != NULL &&
          strcmp(sh_transport_direct.name, "direct") == 0,
          "sh_transport_direct is named");
    check(sh_transport_direct.serve != NULL && sh_transport_direct.stop != NULL,
          "sh_transport_direct implements serve and stop");

    sg_api_free(ctx);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
