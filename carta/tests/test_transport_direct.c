/*
 * test_transport_direct.c - Carta's half of the transport leak detector.
 *
 * Drives ct_api_handle() through sh_transport_direct, with NO transport
 * library on the include path. If someone lets a Keel type into the handler
 * signature, the shared API types, or anything they pull in, this file stops
 * compiling -- which is the point.
 *
 * Until Phase 5 this test could not exist: Carta had no library-side
 * ShApiHandler to link against. The handler lived in api/src/main.c next to
 * 44 Keel references, so "no Keel on the include path" was trivially false
 * for the only translation unit that had one. See docs/roadmaps/transport.md
 * ("Invariant to enforce in CI").
 *
 * The context is built over an empty PBF via ct_pbf_context_create(), so the
 * test needs no data file. Rendering real tiles is test_carta.c's job; what
 * is checked here is that routing, status codes and error bodies come out of
 * the library rather than out of a server.
 */
#include <stdio.h>
#include <string.h>

#include "ct_api.h"
#include "ct_pbf.h"
#include "sh_api.h"
#include "sh_transport.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(int cond, const char *what)
{
    if (cond) { g_pass++; printf("  PASS: %s\n", what); }
    else      { g_fail++; printf("  FAIL: %s\n", what); }
}

/* The whole point: a plain ShApiHandler, no cast. If ct_api_handle ever stops
 * matching the shared signature this line fails to compile. */
static ShApiHandler g_handler = ct_api_handle;

static CTAPIContext *g_ctx = NULL;

static int call(const char *method, const char *path, const char *query,
                ShApiResponse *resp)
{
    ShApiRequest req;
    memset(&req, 0, sizeof(req));
    req.method = method;
    req.path = path;
    req.query = query;
    return sh_transport_direct_call(g_handler, g_ctx, &req, resp);
}

int main(void)
{
    ShApiResponse resp;
    CTPBFContext *pbf;

    printf("=== Transport: direct driver over ct_api_handle ===\n");

    pbf = ct_pbf_context_create();
    if (!pbf) { printf("  FAIL: ct_pbf_context_create\n"); return 1; }
    g_ctx = ct_api_create_from_pbf(pbf, NULL);
    if (!g_ctx) { printf("  FAIL: ct_api_create_from_pbf\n"); return 1; }

    /* Health: the handler routes on req->path. The transport knows nothing
     * about routes and must not need to. */
    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/api/v1/health", NULL, &resp) == 0,
          "Direct call returns 0 for GET /api/v1/health");
    check(resp.status_code == 200, "Health responds 200");
    check(resp.body != NULL && resp.body_len > 0, "Health has a body");
    check(resp.content_type != NULL &&
          strstr(resp.content_type, "json") != NULL,
          "Health content type is JSON");
    sh_api_response_free(&resp);

    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/api/v1/stats", NULL, &resp) == 0,
          "Direct call returns 0 for GET /api/v1/stats");
    check(resp.status_code == 200, "Stats responds 200");
    sh_api_response_free(&resp);

    /* TileJSON reads req->host. A NULL host must not be a crash: the handler
     * substitutes "localhost", because inventing a hostname is not a
     * transport's job either. */
    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/tiles.json", NULL, &resp) == 0,
          "Direct call returns 0 for GET /tiles.json");
    check(resp.status_code == 200, "TileJSON responds 200 with a NULL host");
    sh_api_response_free(&resp);

    /* Tile-path validation moved into the library with the handler. These are
     * the checks api/src/main.c used to do for itself. */
    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/tiles/not-a-tile", NULL, &resp) == 0,
          "Malformed tile path is handled, not an error");
    check(resp.status_code == 400, "Malformed tile path responds 400");
    sh_api_response_free(&resp);

    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/tiles/25/1/1.png", NULL, &resp) == 0,
          "Out-of-range zoom is handled, not an error");
    check(resp.status_code == 400, "Zoom above max_zoom responds 400");
    sh_api_response_free(&resp);

    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/tiles/1/99/1.png", NULL, &resp) == 0,
          "Out-of-range coordinate is handled, not an error");
    check(resp.status_code == 400, "x beyond 2^z responds 400");
    sh_api_response_free(&resp);

    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/tiles/1/0/0.jpeg", NULL, &resp) == 0,
          "Unknown tile format is handled, not an error");
    check(resp.status_code == 400, "Unknown extension responds 400");
    sh_api_response_free(&resp);

    /* Routing decisions belong to the handler, including the 404. A transport
     * must never have to invent one. */
    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/api/v1/nope", NULL, &resp) == 0,
          "Unknown path is handled, not an error");
    check(resp.status_code == 404, "Unknown path responds 404 from the handler");
    sh_api_response_free(&resp);

    /* A handler with no context is a caller bug, not a response. */
    memset(&resp, 0, sizeof(resp));
    check(sh_transport_direct_call(g_handler, NULL,
                                   &(ShApiRequest){ .path = "/api/v1/health" },
                                   &resp) != 0,
          "NULL context is an internal error, not a response");

    check(sh_transport_direct.name != NULL &&
          strcmp(sh_transport_direct.name, "direct") == 0,
          "sh_transport_direct is named");
    check(sh_transport_direct.serve != NULL && sh_transport_direct.stop != NULL,
          "sh_transport_direct implements serve and stop");

    ct_api_free(g_ctx);
    ct_pbf_context_free(pbf);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
