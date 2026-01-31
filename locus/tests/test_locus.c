/*
 * test_locus.c - Locus test suite
 */

#include "locus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ============================================================================
 * Test Framework
 * ============================================================================ */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  %-45s", #name); \
    fflush(stdout); \
    test_##name(); \
    tests_run++; \
    tests_passed++; \
    printf("[PASS]\n"); \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("[FAIL]\n    Assertion failed: %s\n    at %s:%d\n", \
               #cond, __FILE__, __LINE__); \
        exit(1); \
    } \
} while(0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NE(a, b) ASSERT((a) != (b))
#define ASSERT_STR_EQ(a, b) ASSERT(strcmp((a), (b)) == 0)

/* ============================================================================
 * Version Tests
 * ============================================================================ */

TEST(version)
{
    const char *v = lc_version();
    ASSERT(v != NULL);
    ASSERT(strlen(v) > 0);
    ASSERT(v[0] == '0');  /* Version starts with 0 */
}

/* ============================================================================
 * Status Tests
 * ============================================================================ */

TEST(status_string_ok)
{
    const char *s = lc_status_string(LC_OK);
    ASSERT_STR_EQ(s, "OK");
}

TEST(status_string_error)
{
    const char *s = lc_status_string(LC_ERROR_FILE_NOT_FOUND);
    ASSERT(s != NULL);
    ASSERT(strlen(s) > 0);
}

/* ============================================================================
 * Feature Class Tests
 * ============================================================================ */

TEST(class_string)
{
    ASSERT_STR_EQ(lc_class_string(LC_CLASS_CITY), "city");
    ASSERT_STR_EQ(lc_class_string(LC_CLASS_STREET), "street");
    ASSERT_STR_EQ(lc_class_string(LC_CLASS_POI), "poi");
}

TEST(class_importance)
{
    ASSERT(lc_class_importance(LC_CLASS_COUNTRY) > lc_class_importance(LC_CLASS_CITY));
    ASSERT(lc_class_importance(LC_CLASS_CITY) > lc_class_importance(LC_CLASS_STREET));
    ASSERT(lc_class_importance(LC_CLASS_STREET) > lc_class_importance(LC_CLASS_POI));
}

TEST(entity_type_string)
{
    ASSERT_STR_EQ(lc_entity_type_string(LC_ENTITY_NODE), "node");
    ASSERT_STR_EQ(lc_entity_type_string(LC_ENTITY_WAY), "way");
    ASSERT_STR_EQ(lc_entity_type_string(LC_ENTITY_RELATION), "relation");
}

/* ============================================================================
 * Tag Classification Tests
 * ============================================================================ */

TEST(classify_place_city)
{
    ASSERT_EQ(lc_classify_place_tag("city"), LC_CLASS_CITY);
}

TEST(classify_place_town)
{
    ASSERT_EQ(lc_classify_place_tag("town"), LC_CLASS_TOWN);
}

TEST(classify_place_village)
{
    ASSERT_EQ(lc_classify_place_tag("village"), LC_CLASS_VILLAGE);
}

TEST(classify_place_suburb)
{
    ASSERT_EQ(lc_classify_place_tag("suburb"), LC_CLASS_SUBURB);
}

TEST(classify_place_neighbourhood)
{
    ASSERT_EQ(lc_classify_place_tag("neighbourhood"), LC_CLASS_NEIGHBOURHOOD);
    ASSERT_EQ(lc_classify_place_tag("neighborhood"), LC_CLASS_NEIGHBOURHOOD);
}

TEST(classify_highway_residential)
{
    ASSERT_EQ(lc_classify_highway_tag("residential"), LC_CLASS_STREET);
}

TEST(classify_highway_primary)
{
    ASSERT_EQ(lc_classify_highway_tag("primary"), LC_CLASS_STREET);
}

TEST(classify_highway_footway)
{
    /* Footways shouldn't be geocodable streets */
    ASSERT_EQ(lc_classify_highway_tag("footway"), LC_CLASS_UNKNOWN);
}

TEST(classify_boundary_country)
{
    ASSERT_EQ(lc_classify_boundary_tag(2), LC_CLASS_COUNTRY);
}

TEST(classify_boundary_state)
{
    ASSERT_EQ(lc_classify_boundary_tag(4), LC_CLASS_STATE);
}

TEST(classify_boundary_city)
{
    ASSERT_EQ(lc_classify_boundary_tag(8), LC_CLASS_CITY);
}

TEST(highway_is_named)
{
    ASSERT(lc_highway_is_named("residential"));
    ASSERT(lc_highway_is_named("primary"));
    ASSERT(!lc_highway_is_named("footway"));
    ASSERT(!lc_highway_is_named("path"));
}

/* ============================================================================
 * Address Tests
 * ============================================================================ */

TEST(address_init)
{
    LCAddress addr;
    lc_address_init(&addr);
    ASSERT(addr.housenumber == NULL);
    ASSERT(addr.street == NULL);
    ASSERT(addr.city == NULL);
}

TEST(address_copy)
{
    LCAddress src;
    lc_address_init(&src);
    src.housenumber = strdup("42");
    src.street = strdup("Main Street");
    src.city = strdup("Budapest");

    LCAddress dst;
    LCStatus status = lc_address_copy(&dst, &src);
    ASSERT_EQ(status, LC_OK);
    ASSERT_STR_EQ(dst.housenumber, "42");
    ASSERT_STR_EQ(dst.street, "Main Street");
    ASSERT_STR_EQ(dst.city, "Budapest");

    /* Verify independent copies */
    ASSERT(dst.housenumber != src.housenumber);

    lc_address_free(&src);
    lc_address_free(&dst);
}

/* ============================================================================
 * Entity Tests
 * ============================================================================ */

TEST(entity_init)
{
    LCEntity entity;
    lc_entity_init(&entity);
    ASSERT_EQ(entity.osm_id, 0);
    ASSERT(entity.name == NULL);
    ASSERT_EQ(entity.fclass, LC_CLASS_UNKNOWN);
}

/* ============================================================================
 * Entity Store Tests
 * ============================================================================ */

TEST(entity_store_create)
{
    LCEntityStore *store = lc_entity_store_create(100);
    ASSERT(store != NULL);
    ASSERT_EQ(store->count, 0);
    ASSERT(store->capacity >= 100);
    lc_entity_store_free(store);
}

TEST(entity_store_add)
{
    LCEntityStore *store = lc_entity_store_create(10);
    ASSERT(store != NULL);

    LCEntity entity;
    lc_entity_init(&entity);
    entity.osm_id = 12345;
    entity.name = lc_entity_store_intern(store, "Test Place", 0);
    entity.fclass = LC_CLASS_CITY;
    entity.centroid.lat = 47.5;
    entity.centroid.lon = 19.0;

    LCStatus status = lc_entity_store_add(store, &entity);
    ASSERT_EQ(status, LC_OK);
    ASSERT_EQ(store->count, 1);

    LCEntity *e = lc_entity_store_get(store, 0);
    ASSERT(e != NULL);
    ASSERT_EQ(e->osm_id, 12345);
    ASSERT_STR_EQ(e->name, "Test Place");
    ASSERT_EQ(e->fclass, LC_CLASS_CITY);

    lc_entity_store_free(store);
}

TEST(entity_store_grow)
{
    LCEntityStore *store = lc_entity_store_create(2);
    ASSERT(store != NULL);

    for (int i = 0; i < 100; i++) {
        LCEntity entity;
        lc_entity_init(&entity);
        entity.osm_id = (uint64_t)i;
        LCStatus status = lc_entity_store_add(store, &entity);
        ASSERT_EQ(status, LC_OK);
    }

    ASSERT_EQ(store->count, 100);
    ASSERT(store->capacity >= 100);

    /* Verify all entities */
    for (int i = 0; i < 100; i++) {
        LCEntity *e = lc_entity_store_get(store, (uint32_t)i);
        ASSERT(e != NULL);
        ASSERT_EQ(e->osm_id, (uint64_t)i);
    }

    lc_entity_store_free(store);
}

TEST(entity_store_intern)
{
    LCEntityStore *store = lc_entity_store_create(10);
    ASSERT(store != NULL);

    char *s1 = lc_entity_store_intern(store, "Hello", 0);
    char *s2 = lc_entity_store_intern(store, "World", 0);

    ASSERT(s1 != NULL);
    ASSERT(s2 != NULL);
    ASSERT_STR_EQ(s1, "Hello");
    ASSERT_STR_EQ(s2, "World");

    /* Both should be in the string pool */
    ASSERT(s1 >= store->string_pool);
    ASSERT(s1 < store->string_pool + store->string_pool_size);
    ASSERT(s2 >= store->string_pool);
    ASSERT(s2 < store->string_pool + store->string_pool_size);

    lc_entity_store_free(store);
}

/* ============================================================================
 * PBF Context Tests
 * ============================================================================ */

TEST(pbf_context_create)
{
    LCPBFContext *ctx = lc_pbf_context_create();
    ASSERT(ctx != NULL);
    lc_pbf_context_free(ctx);
}

TEST(pbf_default_options)
{
    LCPBFOptions opts;
    lc_pbf_default_options(&opts);
    ASSERT_EQ(opts.include_pois, 1);
    ASSERT_EQ(opts.include_addresses, 1);
    ASSERT_EQ(opts.include_streets, 1);
    ASSERT_EQ(opts.include_boundaries, 1);
    ASSERT_EQ(opts.min_admin_level, 2);
    ASSERT_EQ(opts.max_admin_level, 10);
}

TEST(pbf_stats_empty)
{
    LCPBFContext *ctx = lc_pbf_context_create();
    ASSERT(ctx != NULL);

    LCPBFStats stats;
    lc_pbf_get_stats(ctx, &stats);
    ASSERT_EQ(stats.nodes_processed, 0);
    ASSERT_EQ(stats.ways_processed, 0);
    ASSERT_EQ(stats.relations_processed, 0);

    lc_pbf_context_free(ctx);
}

TEST(pbf_file_not_found)
{
    LCPBFContext *ctx = lc_pbf_context_create();
    ASSERT(ctx != NULL);

    LCStatus status = lc_pbf_parse_file(ctx, "/nonexistent/file.osm.pbf", NULL);
    ASSERT_EQ(status, LC_ERROR_FILE_NOT_FOUND);

    lc_pbf_context_free(ctx);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    printf("\n=== Locus Test Suite ===\n\n");

    printf("Version:\n");
    RUN_TEST(version);

    printf("\nStatus:\n");
    RUN_TEST(status_string_ok);
    RUN_TEST(status_string_error);

    printf("\nFeature Classes:\n");
    RUN_TEST(class_string);
    RUN_TEST(class_importance);
    RUN_TEST(entity_type_string);

    printf("\nTag Classification:\n");
    RUN_TEST(classify_place_city);
    RUN_TEST(classify_place_town);
    RUN_TEST(classify_place_village);
    RUN_TEST(classify_place_suburb);
    RUN_TEST(classify_place_neighbourhood);
    RUN_TEST(classify_highway_residential);
    RUN_TEST(classify_highway_primary);
    RUN_TEST(classify_highway_footway);
    RUN_TEST(classify_boundary_country);
    RUN_TEST(classify_boundary_state);
    RUN_TEST(classify_boundary_city);
    RUN_TEST(highway_is_named);

    printf("\nAddress:\n");
    RUN_TEST(address_init);
    RUN_TEST(address_copy);

    printf("\nEntity:\n");
    RUN_TEST(entity_init);

    printf("\nEntity Store:\n");
    RUN_TEST(entity_store_create);
    RUN_TEST(entity_store_add);
    RUN_TEST(entity_store_grow);
    RUN_TEST(entity_store_intern);

    printf("\nPBF Context:\n");
    RUN_TEST(pbf_context_create);
    RUN_TEST(pbf_default_options);
    RUN_TEST(pbf_stats_empty);
    RUN_TEST(pbf_file_not_found);

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
