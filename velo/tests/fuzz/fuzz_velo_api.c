/*
 * fuzz_velo_api.c - vl_api_handle() under libFuzzer
 *
 * /api/v1/route is the one endpoint in the six that reads its parameters from
 * two different places: a GET takes "from"/"to"/"profile"/"mode" from the query
 * string, a POST takes the same fields from a JSON body. The shared harness
 * puts the fuzzer's bytes wherever the method says they belong, so one corpus
 * covers both parsers.
 *
 * The graph is built once, in-process. Loading a .vlg would make the fuzzer
 * depend on a fixture and spend most of its time in the loader; six nodes are
 * enough to reach the routing paths, and coordinate handling -- which is where
 * a query string turns into graph indices -- is the same either way.
 *
 * Build: make -C velo fuzz-api
 * Run:   ./fuzz_velo_api work/ tests/fuzz/corpus_api/ -max_total_time=60
 */

#include "velo.h"
#include "vl_api.h"
#include <stdlib.h>
#include <string.h>

static VLGraph *g_graph;
static VLAPIContext *g_ctx;

/*
 * The same six-node grid test_velo.c routes over:
 *
 *     0 --10-- 1 --15-- 2
 *     |        |        |
 *    20       25       30
 *     |        |        |
 *     3 --35-- 4 --40-- 5
 *
 * Distances in km, speeds in km/h. Two nodes per row means a fuzzed pair of
 * coordinates can land on a node, between two, or outside the bounds entirely.
 */
static VLGraph *build_graph(void)
{
    static const double COORDS[6][2] = {
        { 47.5, 19.0 }, { 47.5, 19.1 }, { 47.5, 19.2 },
        { 47.4, 19.0 }, { 47.4, 19.1 }, { 47.4, 19.2 }
    };
    /* {from, to, dist_km, speed_kmh}, each road present in both directions */
    static const int EDGES[14][4] = {
        { 0, 1, 10, 50 }, { 1, 0, 10, 50 },
        { 1, 2, 15, 50 }, { 2, 1, 15, 50 },
        { 0, 3, 20, 40 }, { 3, 0, 20, 40 },
        { 1, 4, 25, 40 }, { 4, 1, 25, 40 },
        { 2, 5, 30, 40 }, { 5, 2, 30, 40 },
        { 3, 4, 35, 30 }, { 4, 3, 35, 30 },
        { 4, 5, 40, 30 }, { 5, 4, 40, 30 }
    };
    VLGraph *graph;
    uint32_t offset;
    int i;

    graph = (VLGraph *)calloc(1, sizeof(VLGraph));
    if (!graph) return NULL;

    graph->num_nodes = 6;
    graph->nodes = (VLNode *)calloc(6, sizeof(VLNode));
    if (!graph->nodes) { free(graph); return NULL; }

    for (i = 0; i < 6; i++) {
        graph->nodes[i].coord.lat = (int32_t)(COORDS[i][0] * 1e7);
        graph->nodes[i].coord.lon = (int32_t)(COORDS[i][1] * 1e7);
        graph->nodes[i].osm_id = (uint64_t)(i + 1);
    }

    graph->num_edges = 14;
    graph->edges = (VLEdge *)calloc(14, sizeof(VLEdge));
    if (!graph->edges) { free(graph->nodes); free(graph); return NULL; }

    /* CSR: count per source node, turn the counts into offsets, then fill. */
    for (i = 0; i < 14; i++) graph->nodes[EDGES[i][0]].edge_count++;

    offset = 0;
    for (i = 0; i < 6; i++) {
        graph->nodes[i].edge_start = offset;
        offset += graph->nodes[i].edge_count;
        graph->nodes[i].edge_count = 0;   /* reused as a fill cursor below */
    }

    for (i = 0; i < 14; i++) {
        int from = EDGES[i][0];
        uint32_t idx = graph->nodes[from].edge_start
                     + graph->nodes[from].edge_count;
        graph->edges[idx].target   = (uint32_t)EDGES[i][1];
        graph->edges[idx].distance = (uint32_t)(EDGES[i][2] * 1000 * 1000);
        graph->edges[idx].duration =
            (uint16_t)((EDGES[i][2] * 36.0) / EDGES[i][3]);
        graph->edges[idx].flags    = 0;
        graph->nodes[from].edge_count++;
    }

    graph->owns_memory = 1;
    return graph;
}

static VLAPIContext *fuzz_ctx(void)
{
    if (!g_ctx) {
        g_graph = build_graph();
        if (!g_graph) return NULL;
        g_ctx = vl_api_create(g_graph, NULL, NULL);
    }
    return g_ctx;
}

#define FUZZ_API_CTX_TYPE      VLAPIContext
#define FUZZ_API_CREATE()      fuzz_ctx()
#define FUZZ_API_FREE(c)       ((void)(c))   /* built once, reused */
#define FUZZ_API_HANDLE(c,q,s) vl_api_handle(c, q, s)
#define FUZZ_API_ROUTES        { "/api/v1/route", "/api/v1/health", \
                                 "/api/v1/stats" }

#include "fuzz_api_handler.h"
