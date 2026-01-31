/*
 * test_pbf_parse.c - Test PBF parsing with real data
 */

#include "locus.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *filename = argc > 1 ? argv[1] : "../data/monaco-latest.osm.pbf";

    printf("Locus PBF Parser Test\n");
    printf("=====================\n\n");
    printf("Loading: %s\n\n", filename);

    LCEntityStore *store = lc_load_pbf(filename, NULL);
    if (!store) {
        fprintf(stderr, "Failed to load PBF file\n");
        return 1;
    }

    printf("\nSample entities:\n");
    printf("----------------\n");

    int shown = 0;
    for (uint32_t i = 0; i < store->count && shown < 20; i++) {
        const LCEntity *e = lc_get_entity(store, i);
        if (!e || !e->name) continue;

        printf("[%s] %s: %s",
               lc_entity_type_string(e->type),
               lc_class_string(e->fclass),
               e->name);

        if (e->centroid.lat != 0 || e->centroid.lon != 0) {
            printf(" (%.4f, %.4f)", e->centroid.lat, e->centroid.lon);
        }

        if (e->population > 0) {
            printf(" pop=%d", e->population);
        }

        if (e->address.street) {
            printf(" [%s %s]",
                   e->address.housenumber ? e->address.housenumber : "",
                   e->address.street);
        }

        printf("\n");
        shown++;
    }

    printf("\nTotal entities: %u\n", store->count);

    /* Count by class */
    int counts[LC_CLASS_COUNT] = {0};
    for (uint32_t i = 0; i < store->count; i++) {
        const LCEntity *e = lc_get_entity(store, i);
        if (e && e->fclass < LC_CLASS_COUNT) {
            counts[e->fclass]++;
        }
    }

    printf("\nBy class:\n");
    for (int c = 0; c < LC_CLASS_COUNT; c++) {
        if (counts[c] > 0) {
            printf("  %-15s: %d\n", lc_class_string((LCFeatureClass)c), counts[c]);
        }
    }

    lc_entity_store_free(store);
    return 0;
}
