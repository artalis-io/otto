/*
 * route_batch - batch OD routing with a built-in exact cross-check.
 *
 * Reads "id\tlatA\tlonA\tlatB\tlonB" lines from stdin and, for each pair, routes
 * with the exact reference (Dijkstra) AND the fast production algorithm
 * (bidirectional A*), running the fast one twice to check determinism. Emits one
 * JSON object per line for a verifier to check the internal oracle
 * (dijkstra == fast), determinism, and plausibility. Truck profile.
 *
 *   route_batch <graph.vlg|map.osm.pbf>  < pairs.tsv  > results.jsonl
 */
#include "../include/velo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int route_one(const VLGraph *g, VLCoord a, VLCoord b, VLAlgorithm algo,
                     VLProfile profile, VLWeightType weight,
                     double *dist, double *dur, int *nodes)
{
    VLRouteOptions o;
    vl_default_options(&o);
    o.algorithm = algo;
    o.profile = profile;
    o.weight = weight;
    o.include_geometry = 0;

    VLRoute r;
    memset(&r, 0, sizeof(r));            /* safe vl_free_route even if routing fails */
    VLStatus st = vl_route_coords(g, a, b, &o, &r);
    int ok = (st == VL_OK);
    *dist = ok ? r.distance_m : -1.0;
    *dur  = ok ? r.duration_s : -1.0;
    *nodes = ok ? r.num_nodes : 0;
    vl_free_route(&r);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <graph.vlg|map.osm.pbf> [--profile car|truck|bike|foot|any] [--weight duration|distance] < id\\tlatA\\tlonA\\tlatB\\tlonB\n", argv[0]); return 2; }

    /* profile/weight are caller-chosen (default car/duration; no vehicle baked in) */
    VLProfile profile = VL_PROFILE_CAR;
    VLWeightType weight = VL_WEIGHT_DURATION;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--profile") == 0) {
            const char *p = argv[i+1];
            profile = strcmp(p,"truck")==0 ? VL_PROFILE_TRUCK :
                      strcmp(p,"bike")==0  ? VL_PROFILE_BIKE  :
                      strcmp(p,"foot")==0  ? VL_PROFILE_FOOT  :
                      strcmp(p,"any")==0   ? VL_PROFILE_ANY   : VL_PROFILE_CAR;
        } else if (strcmp(argv[i], "--weight") == 0) {
            weight = strcmp(argv[i+1],"distance")==0 ? VL_WEIGHT_DISTANCE : VL_WEIGHT_DURATION;
        }
    }

    size_t n = strlen(argv[1]);
    VLGraph *g = (n > 4 && strcmp(argv[1] + n - 4, ".vlg") == 0)
                 ? vl_load_binary(argv[1]) : vl_load_pbf(argv[1]);
    if (!g) { fprintf(stderr, "failed to load graph: %s\n", argv[1]); return 1; }
    fprintf(stderr, "loaded graph: %s (profile=%d weight=%d)\n", argv[1], (int)profile, (int)weight);

    char *line = NULL; size_t cap = 0; ssize_t len;
    while ((len = getline(&line, &cap, stdin)) != -1) {
        if (len && line[len-1] == '\n') line[--len] = '\0';
        if (len == 0) continue;
        char id[128]; double la, lo, lb, lob;
        /* id \t latA \t lonA \t latB \t lonB */
        if (sscanf(line, "%127[^\t]\t%lf\t%lf\t%lf\t%lf", id, &la, &lo, &lb, &lob) != 5)
            continue;
        VLCoord A = {la, lo}, B = {lb, lob};

        double dd, du; int dn;
        int dok = route_one(g, A, B, VL_ALGORITHM_DIJKSTRA, profile, weight, &dd, &du, &dn);
        double fd, fu; int fn;
        int fok = route_one(g, A, B, VL_ALGORITHM_ASTAR_BIDIR, profile, weight, &fd, &fu, &fn);
        double fd2, fu2; int fn2;
        int fok2 = route_one(g, A, B, VL_ALGORITHM_ASTAR_BIDIR, profile, weight, &fd2, &fu2, &fn2);

        printf("{\"id\":\"%s\",\"dijkstra\":{\"ok\":%d,\"dist_m\":%.1f,\"dur_s\":%.1f,\"nodes\":%d},"
               "\"fast\":{\"ok\":%d,\"dist_m\":%.1f,\"dur_s\":%.1f,\"nodes\":%d},"
               "\"deterministic\":%d}\n",
               id, dok, dd, du, dn, fok, fd, fu, fn,
               (fok && fok2 && fd == fd2 && fu == fu2) ? 1 : 0);
    }
    free(line);
    vl_graph_free(g);
    return 0;
}
