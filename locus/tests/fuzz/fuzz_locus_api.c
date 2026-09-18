/*
 * fuzz_locus_api.c - lc_api_handle() under libFuzzer
 *
 * Locus is the one of the six whose input is free text: /api/v1/search and
 * /api/v1/autocomplete take a user-supplied string through a trigram matcher
 * and a prefix trie, and /api/v1/reverse takes coordinates. All three arrive
 * in the query string, which is why the shared harness routes a GET's input
 * there.
 *
 * The index is built once, in-process, rather than loaded from a .osm.pbf:
 * a few entities are enough to reach the search paths, and it keeps the
 * fuzzer free of fixture files and deterministic between runs.
 *
 * Build: make -C locus fuzz-api
 * Run:   ./fuzz_locus_api work/ tests/fuzz/corpus_api/ -max_total_time=60
 */

#include "locus.h"
#include "lc_api.h"
#include <stdlib.h>
#include <string.h>

static LCIndex *g_index;
static LCAPIContext *g_ctx;

/*
 * One entity of each class the search paths dispatch on, with a street that
 * carries geometry so reverse geocoding has a line to measure against rather
 * than only a centroid. Names are chosen to share trigrams ("Main"/"Maine")
 * so a fuzzed query can produce partial matches rather than only misses.
 */
static LCIndex *build_index(void)
{
    LCIndex *index = lc_index_create();
    LCEntityStore *store = lc_entity_store_create(8);
    LCEntity e;
    LCLineString *geom;
    SHCoord p1, p2;

    if (!index || !store) return NULL;

    memset(&e, 0, sizeof(e));
    e.name = lc_entity_store_intern(store, "Main Street", 0);
    e.fclass = LC_CLASS_STREET;
    e.centroid.lat = 47.0;
    e.centroid.lon = 19.05;
    geom = lc_linestring_create(2);
    if (geom) {
        p1.lat = 47.0; p1.lon = 19.0;
        p2.lat = 47.0; p2.lon = 19.1;
        lc_linestring_add_point(geom, p1);
        lc_linestring_add_point(geom, p2);
        e.geometry = geom;
    }
    lc_entity_store_add(store, &e);

    memset(&e, 0, sizeof(e));
    e.name = lc_entity_store_intern(store, "Maine Avenue", 0);
    e.fclass = LC_CLASS_STREET;
    e.centroid.lat = 47.01;
    e.centroid.lon = 19.06;
    lc_entity_store_add(store, &e);

    memset(&e, 0, sizeof(e));
    e.name = lc_entity_store_intern(store, "Budapest", 0);
    e.fclass = LC_CLASS_CITY;
    e.centroid.lat = 47.5;
    e.centroid.lon = 19.04;
    lc_entity_store_add(store, &e);

    memset(&e, 0, sizeof(e));
    e.name = lc_entity_store_intern(store, "Cafe Central", 0);
    e.fclass = LC_CLASS_POI;
    e.centroid.lat = 47.497;
    e.centroid.lon = 19.045;
    lc_entity_store_add(store, &e);

    lc_index_build(index, store);
    return index;
}

static LCAPIContext *fuzz_ctx(void)
{
    if (!g_ctx) {
        g_index = build_index();
        if (!g_index) return NULL;
        g_ctx = lc_api_create(g_index, NULL);
    }
    return g_ctx;
}

#define FUZZ_API_CTX_TYPE      LCAPIContext
#define FUZZ_API_CREATE()      fuzz_ctx()
#define FUZZ_API_FREE(c)       ((void)(c))   /* built once, reused */
#define FUZZ_API_HANDLE(c,q,s) lc_api_handle(c, q, s)
#define FUZZ_API_ROUTES        { "/api/v1/search", "/api/v1/autocomplete", \
                                 "/api/v1/reverse", "/api/v1/health",      \
                                 "/api/v1/stats" }

#include "fuzz_api_handler.h"
