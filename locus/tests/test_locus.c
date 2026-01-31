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
 * Normalization Tests
 * ============================================================================ */

TEST(normalize_lowercase)
{
    char buf[64];

    strcpy(buf, "BUDAPEST");
    lc_lowercase(buf);
    ASSERT_STR_EQ(buf, "budapest");

    strcpy(buf, "New York");
    lc_lowercase(buf);
    ASSERT_STR_EQ(buf, "new york");

    strcpy(buf, "ABC123");
    lc_lowercase(buf);
    ASSERT_STR_EQ(buf, "abc123");
}

TEST(normalize_diacritics)
{
    char *result;

    result = lc_remove_diacritics("Zürich");
    ASSERT(result != NULL);
    ASSERT_STR_EQ(result, "Zurich");
    free(result);

    result = lc_remove_diacritics("Kraków");
    ASSERT(result != NULL);
    ASSERT_STR_EQ(result, "Krakow");
    free(result);

    result = lc_remove_diacritics("São Paulo");
    ASSERT(result != NULL);
    ASSERT_STR_EQ(result, "Sao Paulo");
    free(result);

    result = lc_remove_diacritics("Müller");
    ASSERT(result != NULL);
    ASSERT_STR_EQ(result, "Muller");
    free(result);
}

TEST(normalize_whitespace)
{
    char buf[64];

    strcpy(buf, "  New   York  ");
    lc_normalize_whitespace(buf);
    ASSERT_STR_EQ(buf, "New York");

    strcpy(buf, "Hello\t\nWorld");
    lc_normalize_whitespace(buf);
    ASSERT_STR_EQ(buf, "Hello World");

    strcpy(buf, "   ");
    lc_normalize_whitespace(buf);
    ASSERT_STR_EQ(buf, "");
}

TEST(normalize_full)
{
    char *result;

    result = lc_normalize("BUDAPEST");
    ASSERT(result != NULL);
    ASSERT_STR_EQ(result, "budapest");
    free(result);

    result = lc_normalize("  New   York  ");
    ASSERT(result != NULL);
    ASSERT_STR_EQ(result, "new york");
    free(result);
}

TEST(normalize_mixed)
{
    char *result;

    result = lc_normalize("Café Müller!");
    ASSERT(result != NULL);
    ASSERT_STR_EQ(result, "cafe muller");
    free(result);

    result = lc_normalize("ÉLYSÉE-PALACE");
    ASSERT(result != NULL);
    ASSERT_STR_EQ(result, "elysee palace");
    free(result);
}

/* ============================================================================
 * Trie Tests
 * ============================================================================ */

TEST(trie_create)
{
    LCTrie *trie = lc_trie_create();
    ASSERT(trie != NULL);
    ASSERT_EQ(lc_trie_node_count(trie), 1);
    ASSERT_EQ(lc_trie_entry_count(trie), 0);
    lc_trie_free(trie);
}

TEST(trie_insert)
{
    LCTrie *trie = lc_trie_create();
    ASSERT(trie != NULL);

    ASSERT_EQ(lc_trie_insert(trie, "budapest", 1), LC_OK);
    ASSERT_EQ(lc_trie_insert(trie, "budaors", 2), LC_OK);
    ASSERT_EQ(lc_trie_insert(trie, "vienna", 3), LC_OK);

    ASSERT_EQ(lc_trie_entry_count(trie), 3);
    ASSERT(lc_trie_node_count(trie) > 1);

    lc_trie_free(trie);
}

TEST(trie_search_prefix)
{
    LCTrie *trie = lc_trie_create();
    ASSERT(trie != NULL);

    lc_trie_insert(trie, "budapest", 1);
    lc_trie_insert(trie, "budaors", 2);
    lc_trie_insert(trie, "budafok", 3);
    lc_trie_insert(trie, "vienna", 4);

    uint32_t results[10];

    /* Search "buda" should find all three */
    size_t count = lc_trie_search_prefix(trie, "buda", 10, results);
    ASSERT_EQ(count, 3);

    /* Search "budap" should find only budapest */
    count = lc_trie_search_prefix(trie, "budap", 10, results);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(results[0], 1);

    /* Search "vie" should find vienna */
    count = lc_trie_search_prefix(trie, "vie", 10, results);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(results[0], 4);

    /* Search "xyz" should find nothing */
    count = lc_trie_search_prefix(trie, "xyz", 10, results);
    ASSERT_EQ(count, 0);

    lc_trie_free(trie);
}

TEST(trie_search_exact)
{
    LCTrie *trie = lc_trie_create();
    ASSERT(trie != NULL);

    lc_trie_insert(trie, "budapest", 1);
    lc_trie_insert(trie, "buda", 2);  /* Prefix of budapest */

    uint32_t results[10];

    /* Exact match for "buda" */
    size_t count = lc_trie_search_exact(trie, "buda", 10, results);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(results[0], 2);

    /* Exact match for "budapest" */
    count = lc_trie_search_exact(trie, "budapest", 10, results);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(results[0], 1);

    /* No exact match for "budap" */
    count = lc_trie_search_exact(trie, "budap", 10, results);
    ASSERT_EQ(count, 0);

    lc_trie_free(trie);
}

TEST(trie_char_mapping)
{
    /* Test character mapping */
    ASSERT_EQ(lc_trie_char_index('a'), 0);
    ASSERT_EQ(lc_trie_char_index('z'), 25);
    ASSERT_EQ(lc_trie_char_index('0'), 26);
    ASSERT_EQ(lc_trie_char_index('9'), 35);
    ASSERT_EQ(lc_trie_char_index(' '), 36);
    ASSERT_EQ(lc_trie_char_index('!'), -1);

    /* Test reverse mapping */
    ASSERT_EQ(lc_trie_index_char(0), 'a');
    ASSERT_EQ(lc_trie_index_char(25), 'z');
    ASSERT_EQ(lc_trie_index_char(26), '0');
    ASSERT_EQ(lc_trie_index_char(36), ' ');
}

/* ============================================================================
 * N-gram Tests
 * ============================================================================ */

TEST(ngram_generate)
{
    char ngrams[10][4];

    size_t count = lc_ngram_generate("budapest", ngrams, 10);
    ASSERT_EQ(count, 6);  /* bud, uda, dap, ape, pes, est */
    ASSERT_STR_EQ(ngrams[0], "bud");
    ASSERT_STR_EQ(ngrams[1], "uda");
    ASSERT_STR_EQ(ngrams[5], "est");
}

TEST(ngram_similarity)
{
    float sim;

    /* Identical strings */
    sim = lc_ngram_similarity("budapest", "budapest");
    ASSERT(sim > 0.99f);

    /* Similar strings (typo) - Jaccard is 3/9 = 0.333 */
    sim = lc_ngram_similarity("budapest", "budapset");
    ASSERT(sim > 0.3f);

    /* More similar strings (prefix match) */
    sim = lc_ngram_similarity("budapest", "budapesti");
    ASSERT(sim > 0.7f);

    /* Different strings */
    sim = lc_ngram_similarity("budapest", "vienna");
    ASSERT(sim < 0.1f);
}

TEST(ngram_index)
{
    LCNgramIndex *idx = lc_ngram_create();
    ASSERT(idx != NULL);

    ASSERT_EQ(lc_ngram_index_name(idx, "budapest", 1), LC_OK);
    ASSERT_EQ(lc_ngram_index_name(idx, "budapset", 2), LC_OK);  /* Typo */
    ASSERT_EQ(lc_ngram_index_name(idx, "vienna", 3), LC_OK);

    lc_ngram_build(idx);

    LCFuzzyMatch results[10];
    size_t count = lc_ngram_search(idx, "budapest", 0.5f, 10, results);
    ASSERT(count >= 1);
    ASSERT_EQ(results[0].entity_id, 1);  /* Exact match first */

    lc_ngram_free(idx);
}

/* ============================================================================
 * Spatial Tests
 * ============================================================================ */

TEST(grid_create)
{
    SHBBox bounds = {.min_lat = 47.0, .max_lat = 48.0, .min_lon = 19.0, .max_lon = 20.0};
    LCSpatialGrid *grid = lc_grid_create(bounds, 0.1);
    ASSERT(grid != NULL);
    ASSERT_EQ(lc_grid_cell_count(grid), 100);  /* 10x10 */
    lc_grid_free(grid);
}

TEST(grid_insert)
{
    SHBBox bounds = {.min_lat = 47.0, .max_lat = 48.0, .min_lon = 19.0, .max_lon = 20.0};
    LCSpatialGrid *grid = lc_grid_create(bounds, 0.1);
    ASSERT(grid != NULL);

    SHCoord coord = {.lat = 47.5, .lon = 19.5};
    ASSERT_EQ(lc_grid_insert(grid, coord, 1), LC_OK);
    ASSERT_EQ(lc_grid_insert(grid, coord, 2), LC_OK);
    ASSERT_EQ(lc_grid_entry_count(grid), 2);

    lc_grid_free(grid);
}

TEST(grid_query_point)
{
    SHBBox bounds = {.min_lat = 47.0, .max_lat = 48.0, .min_lon = 19.0, .max_lon = 20.0};
    LCSpatialGrid *grid = lc_grid_create(bounds, 0.1);
    ASSERT(grid != NULL);

    SHCoord c1 = {.lat = 47.5, .lon = 19.5};
    SHCoord c2 = {.lat = 47.2, .lon = 19.2};

    lc_grid_insert(grid, c1, 1);
    lc_grid_insert(grid, c1, 2);
    lc_grid_insert(grid, c2, 3);

    uint32_t results[10];
    size_t count = lc_grid_query_point(grid, c1, 10, results);
    ASSERT_EQ(count, 2);

    count = lc_grid_query_point(grid, c2, 10, results);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(results[0], 3);

    lc_grid_free(grid);
}

TEST(grid_query_radius)
{
    SHBBox bounds = {.min_lat = 47.0, .max_lat = 48.0, .min_lon = 19.0, .max_lon = 20.0};
    LCSpatialGrid *grid = lc_grid_create(bounds, 0.1);
    ASSERT(grid != NULL);

    /* Insert entities ~10km apart */
    SHCoord c1 = {.lat = 47.5, .lon = 19.5};
    SHCoord c2 = {.lat = 47.6, .lon = 19.5};  /* ~11km north */

    lc_grid_insert(grid, c1, 1);
    lc_grid_insert(grid, c2, 2);

    uint32_t results[10];

    /* 5km radius should find only one */
    size_t count = lc_grid_query_radius(grid, c1, 5000, 10, results);
    ASSERT_EQ(count, 1);

    /* 15km radius should find both */
    count = lc_grid_query_radius(grid, c1, 15000, 10, results);
    ASSERT_EQ(count, 2);

    lc_grid_free(grid);
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

    printf("\nNormalization:\n");
    RUN_TEST(normalize_lowercase);
    RUN_TEST(normalize_diacritics);
    RUN_TEST(normalize_whitespace);
    RUN_TEST(normalize_full);
    RUN_TEST(normalize_mixed);

    printf("\nTrie:\n");
    RUN_TEST(trie_create);
    RUN_TEST(trie_insert);
    RUN_TEST(trie_search_prefix);
    RUN_TEST(trie_search_exact);
    RUN_TEST(trie_char_mapping);

    printf("\nN-gram:\n");
    RUN_TEST(ngram_generate);
    RUN_TEST(ngram_similarity);
    RUN_TEST(ngram_index);

    printf("\nSpatial:\n");
    RUN_TEST(grid_create);
    RUN_TEST(grid_insert);
    RUN_TEST(grid_query_point);
    RUN_TEST(grid_query_radius);

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
