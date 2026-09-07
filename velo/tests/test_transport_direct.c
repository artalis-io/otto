/*
 * test_transport_direct.c - Velo's half of the transport leak detector.
 *
 * Drives vl_api_handle() through sh_transport_direct, with NO transport
 * library on the include path. If someone lets a Keel type into the handler
 * signature, the shared API types, or anything they pull in, this file stops
 * compiling -- which is the point.
 *
 * Until Phase 5 this test could not exist: the handler the server actually ran
 * lived in api/src/main.c next to 25 Keel references. See
 * docs/roadmaps/transport.md ("Invariant to enforce in CI").
 *
 * Routing quality is test_velo.c's job. What is checked here is that routing,
 * parameter parsing and status codes come out of the library rather than out
 * of a server -- including the cases api/src/main.c used to handle itself.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "velo.h"
#include "vl_types.h"
#include "vl_api.h"
#include "sh_api.h"
#include "sh_transport.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(int cond, const char *what)
{
    if (cond) { g_pass++; printf("  PASS: %s\n", what); }
    else      { g_fail++; printf("  FAIL: %s\n", what); }
}

/* The whole point: a plain ShApiHandler, no cast. If vl_api_handle ever stops
 * matching the shared signature this line fails to compile. */
static ShApiHandler g_handler = vl_api_handle;

static VLAPIContext *g_ctx = NULL;

/*
 * Two nodes joined by one bidirectional edge, with a bbox that actually covers
 * them -- vl_api_route() validates against the bbox, and a calloc'd graph has
 * a bbox of (0,0)-(0,0) that would reject every coordinate.
 */
static VLGraph *tiny_graph(void)
{
    VLGraph *g = calloc(1, sizeof(VLGraph));
    if (!g) return NULL;

    g->num_nodes = 2;
    g->nodes = calloc(2, sizeof(VLNode));
    g->num_edges = 2;
    g->edges = calloc(2, sizeof(VLEdge));
    if (!g->nodes || !g->edges) {
        free(g->nodes); free(g->edges); free(g);
        return NULL;
    }

    g->nodes[0].coord.lat = (int32_t)(47.50 * 1e7);
    g->nodes[0].coord.lon = (int32_t)(19.00 * 1e7);
    g->nodes[0].osm_id = 1;
    g->nodes[0].edge_start = 0;
    g->nodes[0].edge_count = 1;

    g->nodes[1].coord.lat = (int32_t)(47.50 * 1e7);
    g->nodes[1].coord.lon = (int32_t)(19.10 * 1e7);
    g->nodes[1].osm_id = 2;
    g->nodes[1].edge_start = 1;
    g->nodes[1].edge_count = 1;

    g->edges[0].target = 1;
    g->edges[0].distance = 7500u * 1000u;   /* mm */
    g->edges[0].duration = 540;             /* deciseconds */
    g->edges[1].target = 0;
    g->edges[1].distance = 7500u * 1000u;
    g->edges[1].duration = 540;

    g->bbox_min.lat = 47.4;
    g->bbox_min.lon = 18.9;
    g->bbox_max.lat = 47.6;
    g->bbox_max.lon = 19.2;

    g->owns_memory = 1;
    return g;
}

static int call(const char *method, const char *path, const char *query,
                const char *body, ShApiResponse *resp)
{
    ShApiRequest req;
    memset(&req, 0, sizeof(req));
    req.method = method;
    req.path = path;
    req.query = query;
    req.body = body;
    req.body_len = body ? strlen(body) : 0;
    return sh_transport_direct_call(g_handler, g_ctx, &req, resp);
}

static int body_has(const ShApiResponse *resp, const char *needle)
{
    return resp->body && strstr((const char *)resp->body, needle) != NULL;
}

int main(void)
{
    ShApiResponse resp;
    VLGraph *graph;

    printf("=== Transport: direct driver over vl_api_handle ===\n");

    graph = tiny_graph();
    if (!graph) { printf("  FAIL: tiny_graph\n"); return 1; }
    g_ctx = vl_api_create(graph, NULL, NULL);
    if (!g_ctx) { printf("  FAIL: vl_api_create\n"); return 1; }

    /* Health: the handler routes on req->path. The transport knows nothing
     * about routes and must not need to. */
    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/api/v1/health", NULL, NULL, &resp) == 0,
          "Direct call returns 0 for GET /api/v1/health");
    check(resp.status_code == 200, "Health responds 200");
    check(resp.body != NULL && resp.body_len > 0, "Health has a body");
    check(resp.content_type != NULL &&
          strstr(resp.content_type, "json") != NULL,
          "Health content type is JSON");
    sh_api_response_free(&resp);

    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/api/v1/stats", NULL, NULL, &resp) == 0,
          "Direct call returns 0 for GET /api/v1/stats");
    check(resp.status_code == 200, "Stats responds 200");
    check(body_has(&resp, "num_nodes"), "Stats reports the node count");
    sh_api_response_free(&resp);

    /* Parameter validation moved into the library with the handler. */
    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/api/v1/route", NULL, NULL, &resp) == 0,
          "Route with no parameters is handled, not an error");
    check(resp.status_code == 400, "Missing 'from' responds 400");
    sh_api_response_free(&resp);

    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/api/v1/route", "from=47.5,19.0", NULL, &resp) == 0,
          "Route with no 'to' is handled, not an error");
    check(resp.status_code == 400, "Missing 'to' responds 400");
    sh_api_response_free(&resp);

    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/api/v1/route", "from=nonsense&to=47.5,19.1",
               NULL, &resp) == 0,
          "Route with an unparseable coordinate is handled, not an error");
    check(resp.status_code == 400, "Unparseable coordinate responds 400");
    sh_api_response_free(&resp);

    /* Outside the graph's bbox. This check used to live in api/src/main.c. */
    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/api/v1/route", "from=10.0,10.0&to=47.5,19.1",
               NULL, &resp) == 0,
          "Route from outside the graph is handled, not an error");
    check(resp.status_code == 400, "Origin outside the bbox responds 400");
    sh_api_response_free(&resp);

    /* A POST with no body is malformed, not a request to fall back to the
     * query string. */
    memset(&resp, 0, sizeof(resp));
    check(call("POST", "/api/v1/route", NULL, NULL, &resp) == 0,
          "POST with no body is handled, not an error");
    check(resp.status_code == 400, "POST with no body responds 400");
    sh_api_response_free(&resp);

    memset(&resp, 0, sizeof(resp));
    check(call("POST", "/api/v1/route", NULL, "{not json", &resp) == 0,
          "POST with invalid JSON is handled, not an error");
    check(resp.status_code == 400, "POST with invalid JSON responds 400");
    sh_api_response_free(&resp);

    /* A well-formed request gets past validation and into routing. Whether
     * this two-node graph yields a route is test_velo.c's business; what
     * matters here is that the handler no longer answers 400. */
    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/api/v1/route", "from=47.5,19.0&to=47.5,19.1",
               NULL, &resp) == 0,
          "A well-formed route request is handled");
    check(resp.status_code != 400,
          "Valid in-bounds coordinates get past validation");
    if (resp.status_code == 200) {
        /* Geometry defaults on -- what the HTTP server has always returned,
         * and what the WASM copy used to get wrong. */
        check(body_has(&resp, "geometry"),
              "Geometry is included by default");
        check(body_has(&resp, "meta"), "The response carries a meta object");
    } else {
        printf("  NOTE: no route on the tiny graph (status %d); geometry"
               " default is covered by test_velo.c\n", resp.status_code);
    }
    sh_api_response_free(&resp);

    /* geometry=false must turn it off. */
    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/api/v1/route",
               "from=47.5,19.0&to=47.5,19.1&geometry=false", NULL, &resp) == 0,
          "geometry=false is handled");
    if (resp.status_code == 200) {
        check(!body_has(&resp, "geometry"), "geometry=false omits the geometry");
    }
    sh_api_response_free(&resp);

    /* Routing decisions belong to the handler, including the 404. A transport
     * must never have to invent one. */
    memset(&resp, 0, sizeof(resp));
    check(call("GET", "/api/v1/nope", NULL, NULL, &resp) == 0,
          "Unknown path is handled, not an error");
    check(resp.status_code == 404, "Unknown path responds 404 from the handler");
    sh_api_response_free(&resp);

    /* A handler with no context, or a request with no path, is a caller bug
     * rather than a response. */
    memset(&resp, 0, sizeof(resp));
    check(call("GET", NULL, NULL, NULL, &resp) != 0,
          "A request with no path is an internal error");
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

    vl_api_free(g_ctx);
    vl_graph_free(graph);

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
