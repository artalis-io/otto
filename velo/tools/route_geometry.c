/*
 * route_geometry - emit road-following geometry for stop-to-stop legs.
 *
 * Reads "id\tlatA\tlonA\tlatB\tlonB" lines from stdin, routes each pair over
 * the graph, and writes one JSONL object per leg with the full path geometry:
 *   {"id":"..","ok":1,"dist_m":..,"dur_s":..,"coords":[[lat,lon],...]}
 *
 * Companion to surge/scripts/surge_map.py, which turns a Surge solution into
 * stop-to-stop legs, feeds them here, and stitches the returned geometry into
 * road-following GeoJSON routes.
 *
 *   route_geometry <graph.vlg|map.osm.pbf> [--profile car|truck|bike|foot|any]
 *                  [--weight duration|distance] [--save out.vlg]  < legs.tsv
 *
 * Loading a large .osm.pbf takes tens of seconds; pass --save to write a .vlg
 * once, then reuse it for instant loads.
 */
#include "velo.h"
#include "vl_route.h"
#include "vl_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "usage: %s <graph.vlg|map.osm.pbf> [--profile car|truck|bike|foot|any] "
            "[--weight duration|distance] [--save out.vlg]  < id\\tlatA\\tlonA\\tlatB\\tlonB\n",
            argv[0]);
        return 2;
    }
    VLProfile profile = VL_PROFILE_TRUCK;
    VLWeightType weight = VL_WEIGHT_DISTANCE;
    const char *save = NULL;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--profile") == 0) {
            const char *p = argv[i + 1];
            if      (strcmp(p, "car")   == 0) profile = VL_PROFILE_CAR;
            else if (strcmp(p, "truck") == 0) profile = VL_PROFILE_TRUCK;
            else if (strcmp(p, "bike")  == 0) profile = VL_PROFILE_BIKE;
            else if (strcmp(p, "foot")  == 0) profile = VL_PROFILE_FOOT;
            else if (strcmp(p, "any")   == 0) profile = VL_PROFILE_ANY;
        } else if (strcmp(argv[i], "--weight") == 0) {
            weight = strcmp(argv[i + 1], "distance") == 0 ? VL_WEIGHT_DISTANCE : VL_WEIGHT_DURATION;
        } else if (strcmp(argv[i], "--save") == 0) {
            save = argv[i + 1];
        }
    }

    size_t n = strlen(argv[1]);
    VLGraph *g = (n > 4 && strcmp(argv[1] + n - 4, ".vlg") == 0)
                 ? vl_load_binary(argv[1]) : vl_load_pbf(argv[1]);
    if (!g) { fprintf(stderr, "route_geometry: failed to load graph: %s\n", argv[1]); return 1; }
    fprintf(stderr, "route_geometry: graph loaded (profile=%d weight=%d)\n", (int)profile, (int)weight);
    if (save && vl_save_binary(g, save) == VL_OK) fprintf(stderr, "route_geometry: saved %s\n", save);

    VLRouteOptions o;
    vl_default_options(&o);
    o.algorithm = VL_ALGORITHM_ASTAR_BIDIR;
    o.weight = weight;
    o.profile = profile;
    o.include_geometry = 1;

    char *line = NULL; size_t cap = 0; ssize_t len;
    int done = 0, failed = 0;
    while ((len = getline(&line, &cap, stdin)) > 0) {
        char *id = strtok(line, "\t");
        char *sa = strtok(NULL, "\t"), *oa = strtok(NULL, "\t");
        char *sb = strtok(NULL, "\t"), *ob = strtok(NULL, "\t\r\n");
        if (!id || !sa || !oa || !sb || !ob) continue;
        VLCoord A = { atof(sa), atof(oa) }, B = { atof(sb), atof(ob) };
        VLRoute r; memset(&r, 0, sizeof(r));
        VLStatus st = vl_route_coords(g, A, B, &o, &r);
        int ok = (st == VL_OK);
        if (!ok) failed++;
        printf("{\"id\":\"%s\",\"ok\":%d,\"dist_m\":%.1f,\"dur_s\":%.1f,\"coords\":[",
               id, ok, r.distance_m, r.duration_s);
        if (ok && r.coords) {
            for (int i = 0; i < r.num_coords; i++)
                printf("%s[%.6f,%.6f]", i ? "," : "", r.coords[i].lat, r.coords[i].lon);
        }
        printf("]}\n");
        vl_free_route(&r);
        done++;
    }
    fprintf(stderr, "route_geometry: processed %d legs (%d failed)\n", done, failed);
    free(line);
    vl_graph_free(g);
    return 0;
}
