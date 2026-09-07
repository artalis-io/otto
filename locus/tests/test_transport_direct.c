/*
 * test_transport_direct.c - Locus's half of the transport leak detector.
 *
 * Drives lc_api_handle() through sh_transport_direct, with NO transport
 * library on the include path. If someone lets a Keel type into the handler
 * signature, the shared API types, or anything they pull in, this file stops
 * compiling -- which is the point.
 *
 * Until Phase 5 this test could not exist: the handler that the server
 * actually ran lived in api/src/main.c next to 45 Keel references, and the
 * library's lc_api_handle() had no callers at all. See
 * docs/roadmaps/transport.md ("Invariant to enforce in CI").
 *
 * The routing and validation checks here need no index, so they run against a
 * NULL context. The endpoints that do need one are covered by test_locus.c,
 * which can build an index.
 */
#include <stdio.h>
#include <string.h>

#include "lc_api.h"
#include "sh_api.h"
#include "sh_transport.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(int cond, const char *what)
{
    if (cond) { g_pass++; printf("  PASS: %s\n", what); }
    else      { g_fail++; printf("  FAIL: %s\n", what); }
}

/* The whole point: a plain ShApiHandler, no cast. If lc_api_handle ever stops
 * matching the shared signature this line fails to compile. */
static ShApiHandler g_handler = lc_api_handle;

static int call(const char *method, const char *path, const char *query,
                ShApiResponse *resp)
{
    ShApiRequest req;
    memset(&req, 0, sizeof(req));
    req.method = method;
    req.path = path;
    req.query = query;
    /* NULL ctx: health answers without an index, everything else reports 503.
     * A transport must be able to call the handler before the data is up. */
    return sh_transport_direct_call(g_handler, NULL, &req, resp);
}

int main(void)
{
    ShApiResponse resp;

    printf("=== Transport: direct driver over lc_api_handle ===\n");

    /* Health: the handler routes on req->path. The transport knows nothing
     * about routes and must not need to. */
    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/api/v1/health", NULL, &resp) == 0,
          "Direct call returns 0 for GET /api/v1/health");
    check(resp.status_code == 200, "Health responds 200 without an index");
    check(resp.body != NULL && resp.body_len > 0, "Health has a body");
    check(resp.content_type != NULL &&
          strstr(resp.content_type, "json") != NULL,
          "Health content type is JSON");
    sh_api_response_free(&resp);

    /* Everything that needs data says so, rather than crashing or 500ing. */
    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/api/v1/stats", NULL, &resp) == 0,
          "Stats with no index is handled, not an error");
    check(resp.status_code == 503, "Stats responds 503 with no index");
    sh_api_response_free(&resp);

    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/api/v1/search", "q=monaco", &resp) == 0,
          "Search with no index is handled, not an error");
    check(resp.status_code == 503, "Search responds 503 with no index");
    sh_api_response_free(&resp);

    /* Input validation happens before the index is consulted, so these are
     * 400 even here. They are the checks api/src/main.c used to do itself. */
    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/api/v1/search", NULL, &resp) == 0,
          "Search without 'q' is handled, not an error");
    check(resp.status_code == 400, "Missing 'q' responds 400");
    sh_api_response_free(&resp);

    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/api/v1/autocomplete", "limit=5", &resp) == 0,
          "Autocomplete without 'q' is handled, not an error");
    check(resp.status_code == 400, "Missing 'q' on autocomplete responds 400");
    sh_api_response_free(&resp);

    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/api/v1/reverse", "lat=43.7", &resp) == 0,
          "Reverse without 'lon' is handled, not an error");
    check(resp.status_code == 400, "Missing 'lon' responds 400");
    sh_api_response_free(&resp);

    /* "north" is not a latitude. sh_parse_double gives NaN and the handler
     * rejects it; atof() would have made it 0.0 and geocoded the Atlantic. */
    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/api/v1/reverse", "lat=north&lon=7.4", &resp) == 0,
          "Reverse with a non-numeric lat is handled, not an error");
    check(resp.status_code == 400, "Non-numeric coordinates respond 400");
    sh_api_response_free(&resp);

    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/api/v1/reverse", "lat=91&lon=7.4", &resp) == 0,
          "Reverse with an out-of-range lat is handled, not an error");
    check(resp.status_code == 400, "Out-of-range coordinates respond 400");
    sh_api_response_free(&resp);

    /* Routing decisions belong to the handler, including the 404. A transport
     * must never have to invent one. */
    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/api/v1/nope", NULL, &resp) == 0,
          "Unknown path is handled, not an error");
    check(resp.status_code == 404, "Unknown path responds 404 from the handler");
    sh_api_response_free(&resp);

    /* A request with no path is a caller bug, not a response. */
    memset(&resp, 0, sizeof(resp));
    check(call("GET", NULL, NULL, &resp) != 0,
          "A request with no path is an internal error");

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

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
