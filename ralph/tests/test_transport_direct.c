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

/* ----------------------------------------------------------------------
 * A synthetic streaming handler. Ralph has no streaming endpoint yet, so
 * the DirectStream path needs its own producer to be exercised at all.
 * ---------------------------------------------------------------------- */

static int g_stream_freed = 0;

static int produce_ok(void *stream_ctx, ShApiStream *out)
{
    (void)stream_ctx;
    if (sh_api_stream_closed(out)) return -1;
    if (sh_api_stream_send(out, NULL, "alpha", 5) != 0) return -1;
    if (sh_api_stream_send(out, "progress", "beta", 4) != 0) return -1;
    return 0;
}

static int produce_fail(void *stream_ctx, ShApiStream *out)
{
    (void)stream_ctx;
    (void)sh_api_stream_send(out, NULL, "partial", 7);
    return -1;   /* producer gives up part-way through */
}

static void stream_freed(void *stream_ctx)
{
    (void)stream_ctx;
    g_stream_freed++;
}

static int streaming_handler(void *ctx, const ShApiRequest *req,
                             ShApiResponse *resp)
{
    (void)ctx; (void)req;
    memset(resp, 0, sizeof(*resp));
    resp->status_code = 200;
    resp->content_type = "text/event-stream";
    resp->stream_fn = produce_ok;
    resp->stream_free = stream_freed;
    return 0;
}

static int failing_stream_handler(void *ctx, const ShApiRequest *req,
                                  ShApiResponse *resp)
{
    (void)ctx; (void)req;
    memset(resp, 0, sizeof(*resp));
    resp->status_code = 200;
    resp->content_type = "text/event-stream";
    resp->stream_fn = produce_fail;
    resp->stream_free = stream_freed;
    return 0;
}

static int stream_case(ShApiResponse *resp)
{
    ShApiRequest req;
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.path = "/stream";
    g_stream_freed = 0;
    return sh_transport_direct_call(streaming_handler, NULL, &req, resp);
}

static int stream_fail_case(ShApiResponse *resp)
{
    ShApiRequest req;
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.path = "/stream";
    return sh_transport_direct_call(failing_stream_handler, NULL, &req, resp);
}

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

    /* ------------------------------------------------------------------
     * Streaming escape hatch.
     *
     * Ralph has no streaming endpoint, so without this the whole
     * DirectStream path would ship untested. A c-audit caught exactly that.
     * ------------------------------------------------------------------ */
    memset(&resp, 0, sizeof(resp));
    check(stream_case(&resp) == 0, "Streaming handler runs to completion");
    check(resp.stream_fn == NULL,
          "stream_fn cleared after the transport consumed it");
    check(g_stream_freed == 1, "stream_free called exactly once");
    check(resp.body != NULL && resp.body_len > 0,
          "Streamed chunks collected into the response body");
    check(resp.body && strstr((const char *)resp.body, "alpha") != NULL &&
          strstr((const char *)resp.body, "beta") != NULL,
          "Both chunks present, in order");
    check(resp.body && strstr((const char *)resp.body, "event: progress") != NULL,
          "Named event recorded");
    sh_api_response_free(&resp);

    /* A stream that reports failure must not hand back a half-built body. */
    memset(&resp, 0, sizeof(resp));
    g_stream_freed = 0;
    check(stream_fail_case(&resp) != 0, "Failing stream reports an error");
    check(resp.body == NULL && resp.body_len == 0,
          "Failing stream leaves no partial body");
    check(g_stream_freed == 1, "stream_free still called on the failure path");
    sh_api_response_free(&resp);

    /* Escaping: a message with quotes/newlines must stay valid JSON. */
    memset(&resp, 0, sizeof(resp));
    check(sh_api_response_error(&resp, 400, "bad \"input\"\nline2\ttab") == 0,
          "sh_api_response_error builds a response");
    check(resp.status_code == 400, "Error response carries the status");
    check(resp.body && strstr((const char *)resp.body, "\\\"input\\\"") != NULL,
          "Quotes escaped");
    check(resp.body && strstr((const char *)resp.body, "\\n") != NULL &&
          strstr((const char *)resp.body, "\\t") != NULL,
          "Newline and tab escaped");
    check(resp.body && strchr((const char *)resp.body, '\n') == NULL,
          "No raw control characters survive into the JSON");
    sh_api_response_free(&resp);

    ralph_api_free(ctx);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
