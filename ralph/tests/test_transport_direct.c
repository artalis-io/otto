/*
 * test_transport_direct.c - The transport abstraction's leak detector.
 *
 * Drives ralph_api_handle() through sh_transport_direct, with NO transport
 * library on the include path. If someone lets a Keel type into the handler
 * signature, the shared API types, or anything they pull in, this file stops
 * compiling -- which is the point. Catching that here is far cheaper than
 * catching it during the next transport migration.
 *
 * See docs/roadmaps/transport.md ("Invariant to enforce in CI").
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ralph_api.h"
#include "sh_api.h"
#include "sh_transport.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(int cond, const char *what)
{
    if (cond) { g_pass++; printf("  PASS: %s\n", what); }
    else      { g_fail++; printf("  FAIL: %s\n", what); }
}

/* The whole point: a plain ShApiHandler, no cast needed. If ralph_api_handle
 * ever stops matching the shared signature this line fails to compile. */
static ShApiHandler g_handler = ralph_api_handle;

static int call(RalphAPIContext *ctx, const char *method, const char *path,
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
    RalphAPIContext *ctx;
    ShApiResponse resp;

    printf("=== Transport: direct driver over ralph_api_handle ===\n");

    ctx = ralph_api_create();
    check(ctx != NULL, "API context created");
    if (!ctx) return 1;

    /* Health: the handler routes on req->path, the transport knows nothing
     * about routes. */
    memset(&resp, 0, sizeof(resp));
    check(call(ctx, "GET", "/api/v1/health", NULL, &resp) == 0,
          "Direct call returns 0 for GET /api/v1/health");
    check(resp.status_code == 200, "Health responds 200");
    check(resp.body != NULL && resp.body_len > 0, "Health has a body");
    check(resp.content_type != NULL &&
          strstr(resp.content_type, "json") != NULL,
          "Health content type is JSON");
    sh_api_response_free(&resp);

    /* Formats. */
    memset(&resp, 0, sizeof(resp));
    check(call(ctx, "GET", "/api/v1/formats", NULL, &resp) == 0,
          "Direct call returns 0 for GET /api/v1/formats");
    check(resp.status_code == 200, "Formats responds 200");
    sh_api_response_free(&resp);

    /* Routing decisions belong to the handler, including the 404. A transport
     * must never need to invent one. */
    memset(&resp, 0, sizeof(resp));
    check(call(ctx, "GET", "/api/v1/nope", NULL, &resp) == 0,
          "Unknown path is handled, not an error");
    check(resp.status_code == 404, "Unknown path responds 404 from the handler");
    sh_api_response_free(&resp);

    /* A response freed twice, or freed when never filled, must be safe --
     * transports do this on error paths. */
    memset(&resp, 0, sizeof(resp));
    sh_api_response_free(&resp);
    sh_api_response_free(&resp);
    check(resp.body == NULL, "Freeing an empty response twice is safe");

    /* The direct transport satisfies the ShTransport shape. */
    check(sh_transport_direct.name != NULL &&
          strcmp(sh_transport_direct.name, "direct") == 0,
          "sh_transport_direct is named");
    check(sh_transport_direct.serve != NULL && sh_transport_direct.stop != NULL,
          "sh_transport_direct implements serve and stop");

    ralph_api_free(ctx);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
