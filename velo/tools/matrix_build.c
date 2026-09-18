/*
 * matrix_build.c - All-pairs travel matrix over a Velo graph.
 *
 * Reads "id\tlat\tlon" lines from stdin (one location per line, in the order
 * they should be indexed) and emits, on stdout, an all-pairs travel matrix:
 *
 *   S <i> <snapped_node> <snap_offset_m>     (per location i)
 *   M <i> <j> <dur_s> <dist_m>               (per ordered pair i != j)
 *
 * For each source it runs one one-to-all Dijkstra, optimizing --weight (duration
 * = fastest path, the default; or distance = shortest path), using the same
 * edge-accessibility (vl_edge_accessible) and per-profile cost
 * (vl_edge_duration_s) the router applies. It carries the OTHER metric along the
 * chosen-optimal path -- so a duration matrix reports the distance driven on the
 * fastest route, and a distance matrix reports the time taken on the shortest
 * route. Both duration and distance are always emitted per pair. Locations snap
 * to the routable core (largest SCC) via vl_graph_nearest_node_routable, so
 * every reachable pair resolves; unreachable entries are emitted as -1.
 *
 * This is a thin CLI over the core engine: no vehicle, dataset, or client
 * assumptions are baked in -- profile and weight are caller-chosen and the
 * location list comes entirely from stdin.
 *
 * Usage:
 *   matrix_build <graph.vlg|map.osm.pbf> [--profile car|truck|bike|foot|any]
 *                [--weight duration|distance] < id\tlat\tlon
 */
#include "velo.h"
#include "vl_types.h"
#include "vl_graph.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* velo internals (implemented in libvelo; declared here to avoid pulling in
 * the router's private headers) */
VLHeap  *vl_heap_create(size_t n);
void     vl_heap_free(VLHeap *h);
void     vl_heap_clear(VLHeap *h);
int      vl_heap_empty(const VLHeap *h);
VLStatus vl_heap_pop(VLHeap *h, VLHeapEntry *e);
VLStatus vl_heap_push_or_decrease(VLHeap *h, uint32_t node, double prio);
double   vl_haversine(VLCoord a, VLCoord b);

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <graph.vlg|map.osm.pbf> "
                        "[--profile car|truck|bike|foot|any] "
                        "[--weight duration|distance] < id\\tlat\\tlon\n", argv[0]);
        return 2;
    }

    VLProfile prof = VL_PROFILE_CAR;
    int use_dist = 0;   /* 0 = optimize duration (fastest), 1 = optimize distance (shortest) */
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            const char *p = argv[++i];
            prof = strcmp(p, "truck") == 0 ? VL_PROFILE_TRUCK :
                   strcmp(p, "bike")  == 0 ? VL_PROFILE_BIKE  :
                   strcmp(p, "foot")  == 0 ? VL_PROFILE_FOOT  :
                   strcmp(p, "any")   == 0 ? VL_PROFILE_ANY   : VL_PROFILE_CAR;
        } else if (strcmp(argv[i], "--weight") == 0 && i + 1 < argc) {
            use_dist = strcmp(argv[++i], "distance") == 0;
        }
    }
    uint16_t amask = vl_profile_access_mask(prof);

    size_t n = strlen(argv[1]);
    VLGraph *g = (n > 4 && strcmp(argv[1] + n - 4, ".vlg") == 0)
                 ? vl_load_binary(argv[1]) : vl_load_pbf(argv[1]);
    if (!g) { fprintf(stderr, "failed to load graph: %s\n", argv[1]); return 1; }
    if (vl_graph_compute_core(g, prof) != VL_OK) {
        fprintf(stderr, "failed to compute routable core\n"); vl_graph_free(g); return 1;
    }

    /* read locations */
    int cap = 512, ns = 0;
    double *lat = malloc(cap * sizeof(double)), *lon = malloc(cap * sizeof(double));
    uint32_t *node = malloc(cap * sizeof(uint32_t));
    if (!lat || !lon || !node) { fprintf(stderr, "oom\n"); return 1; }
    char line[512], id[128];
    while (fgets(line, sizeof line, stdin)) {
        double la, lo;
        if (sscanf(line, "%127[^\t]\t%lf\t%lf", id, &la, &lo) != 3) continue;
        if (ns == cap) {
            cap *= 2;
            lat  = realloc(lat,  cap * sizeof(double));
            lon  = realloc(lon,  cap * sizeof(double));
            node = realloc(node, cap * sizeof(uint32_t));
            if (!lat || !lon || !node) { fprintf(stderr, "oom\n"); return 1; }
        }
        lat[ns] = la; lon[ns] = lo; ns++;
    }
    fprintf(stderr, "matrix_build: %d locations, graph nodes=%u, profile=%d, weight=%s\n",
            ns, g->num_nodes, (int)prof, use_dist ? "distance" : "duration");

    /* snap each location to the routable core, report node + offset */
    for (int i = 0; i < ns; i++) {
        VLCoord c = { lat[i], lon[i] };
        node[i] = vl_graph_nearest_node_routable(g, c, prof);
        double snap = 0.0;
        if (node[i] != VL_INVALID_NODE) {
            VLCoord nc = VL_FIXED_TO_COORD(g->nodes[node[i]].coord);
            snap = vl_haversine(c, nc);
        }
        printf("S\t%d\t%u\t%.1f\n", i, node[i], snap);
    }

    /* one-to-all Dijkstra per source */
    size_t N = g->num_nodes;
    double *dt = malloc(N * sizeof(double));   /* time (cost) to each node */
    double *dm = malloc(N * sizeof(double));   /* distance along that path */
    VLHeap *h = vl_heap_create(N);
    if (!dt || !dm || !h) { fprintf(stderr, "oom\n"); return 1; }

    for (int s = 0; s < ns; s++) {
        for (size_t k = 0; k < N; k++) { dt[k] = VL_INF; dm[k] = VL_INF; }
        vl_heap_clear(h);
        uint32_t src = node[s];
        if (src == VL_INVALID_NODE) {
            for (int t = 0; t < ns; t++) if (t != s) printf("M\t%d\t%d\t-1\t-1\n", s, t);
            continue;
        }
        dt[src] = 0.0; dm[src] = 0.0;
        vl_heap_push_or_decrease(h, src, 0.0);
        VLHeapEntry e;
        while (!vl_heap_empty(h) && vl_heap_pop(h, &e) == VL_OK) {
            uint32_t u = e.node;
            double key_u = use_dist ? dm[u] : dt[u];   /* the metric being minimized */
            if (e.priority > key_u) continue;          /* stale heap entry */
            const VLNode *nd = &g->nodes[u];
            for (uint32_t x = 0; x < nd->edge_count; x++) {
                const VLEdge *ed = &g->edges[nd->edge_start + x];
                if (!vl_edge_accessible(ed->flags, prof, amask)) continue;
                double ndur  = dt[u] + vl_edge_duration_s(ed, prof);
                double ndist = dm[u] + ed->distance / 1000.0;
                double nkey  = use_dist ? ndist : ndur;
                double vkey  = use_dist ? dm[ed->target] : dt[ed->target];
                if (nkey < vkey) {
                    /* carry BOTH metrics along the key-optimal path */
                    dt[ed->target] = ndur;
                    dm[ed->target] = ndist;
                    vl_heap_push_or_decrease(h, ed->target, nkey);
                }
            }
        }
        for (int t = 0; t < ns; t++) {
            if (t == s) continue;
            uint32_t v = node[t];
            if (v == VL_INVALID_NODE || dt[v] >= VL_INF)
                printf("M\t%d\t%d\t-1\t-1\n", s, t);
            else
                printf("M\t%d\t%d\t%.1f\t%.1f\n", s, t, dt[v], dm[v]);
        }
    }

    free(dt); free(dm); vl_heap_free(h);
    free(lat); free(lon); free(node);
    vl_graph_free(g);
    return 0;
}
