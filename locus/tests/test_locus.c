/*
 * test_locus.c - Locus test suite
 */

#include "locus.h"
#include "lc_query.h"
#include "lc_api.h"
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

    /* Strings are separately allocated, not in a contiguous pool */
    ASSERT(s1 != s2);

    /* Memory usage should be tracked */
    ASSERT(store->string_pool_used >= 12);  /* "Hello" + "World" + null terminators */

    /* Free orphaned strings - normally these would be stored in entities */
    free(s1);
    free(s2);
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

    snprintf(buf, sizeof(buf), "%s", "BUDAPEST");
    lc_lowercase(buf);
    ASSERT_STR_EQ(buf, "budapest");

    snprintf(buf, sizeof(buf), "%s", "New York");
    lc_lowercase(buf);
    ASSERT_STR_EQ(buf, "new york");

    snprintf(buf, sizeof(buf), "%s", "ABC123");
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

    snprintf(buf, sizeof(buf), "%s", "  New   York  ");
    lc_normalize_whitespace(buf);
    ASSERT_STR_EQ(buf, "New York");

    snprintf(buf, sizeof(buf), "%s", "Hello\t\nWorld");
    lc_normalize_whitespace(buf);
    ASSERT_STR_EQ(buf, "Hello World");

    snprintf(buf, sizeof(buf), "%s", "   ");
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

/*
 * Regression: truncated/malformed UTF-8 must not make the normalizers advance
 * past the NUL terminator (heap over-read on untrusted query strings). The
 * observable correctness consequence is that a valid byte following a truncated
 * multi-byte lead is preserved: the buggy code advanced by the lead byte's
 * declared length (2-4), skipping that byte and reading past the terminator;
 * the fixed code clamps the length to 1 for truncated sequences.
 * (The pure memory-safety proof is a guard-page harness; this guards a revert.)
 */
TEST(normalize_truncated_utf8)
{
    char buf[64];

    /* 3-byte lead 0xE0 immediately followed by ASCII 'z': the truncated lead is
       dropped, 'z' survives. Buggy code skipped 'z' (and over-read). */
    buf[0] = (char)0xE0; buf[1] = 'z'; buf[2] = '\0';
    lc_lowercase(buf);
    ASSERT_STR_EQ(buf, "z");

    /* 4-byte lead 0xF0 + ASCII 'a' through punctuation stripper. */
    buf[0] = (char)0xF0; buf[1] = 'a'; buf[2] = '\0';
    lc_remove_punctuation(buf);
    ASSERT(strchr(buf, 'a') != NULL);
    ASSERT(strlen(buf) <= 2);

    /* Lone truncated lead byte: dropped, yields empty string, stays terminated. */
    buf[0] = (char)0xF0; buf[1] = '\0';
    lc_lowercase(buf);
    ASSERT_EQ((int)strlen(buf), 0);

    /* 2-byte lead 0xC3 + space + 'x' through whitespace collapser. */
    buf[0] = (char)0xC3; buf[1] = ' '; buf[2] = 'x'; buf[3] = '\0';
    lc_normalize_whitespace(buf);
    ASSERT(strchr(buf, 'x') != NULL);

    /* Truncated lead into diacritic remover must terminate output safely. */
    char out[64];
    char in[3] = { (char)0xE0, (char)0x80, '\0' };  /* lead + 1 continuation, truncated */
    size_t n = lc_remove_diacritics_to(in, out, sizeof(out));
    ASSERT(n < sizeof(out));
    ASSERT_EQ((int)out[n], 0);
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
 * LineString Tests
 * ============================================================================ */

TEST(linestring_create)
{
    LCLineString *line = lc_linestring_create(4);
    ASSERT(line != NULL);
    ASSERT_EQ(line->count, 0);

    lc_linestring_free(line);
}

TEST(linestring_add_points)
{
    LCLineString *line = lc_linestring_create(0);  /* Start with no capacity */
    ASSERT(line != NULL);

    SHCoord p1 = {.lat = 47.0, .lon = 19.0};
    SHCoord p2 = {.lat = 47.1, .lon = 19.1};
    SHCoord p3 = {.lat = 47.2, .lon = 19.2};

    ASSERT(lc_linestring_add_point(line, p1));
    ASSERT(lc_linestring_add_point(line, p2));
    ASSERT(lc_linestring_add_point(line, p3));

    ASSERT_EQ(line->count, 3);
    ASSERT(fabs(line->points[0].lat - 47.0) < 0.001);
    ASSERT(fabs(line->points[1].lat - 47.1) < 0.001);
    ASSERT(fabs(line->points[2].lat - 47.2) < 0.001);

    lc_linestring_free(line);
}

/* ============================================================================
 * Point-to-Line Distance Tests
 * ============================================================================ */

TEST(point_to_segment_on_endpoint)
{
    /* Point is exactly on segment start */
    SHCoord point = {.lat = 47.0, .lon = 19.0};
    SHCoord seg_a = {.lat = 47.0, .lon = 19.0};
    SHCoord seg_b = {.lat = 47.1, .lon = 19.1};

    double dist = lc_point_to_segment_distance(point, seg_a, seg_b);
    ASSERT(dist < 1.0);  /* Should be ~0 meters */
}

TEST(point_to_segment_perpendicular)
{
    /* Point perpendicular to segment middle */
    /* Segment goes from (47.0, 19.0) to (47.0, 19.2) - horizontal line */
    SHCoord seg_a = {.lat = 47.0, .lon = 19.0};
    SHCoord seg_b = {.lat = 47.0, .lon = 19.2};

    /* Point is 0.01 degrees north of segment midpoint */
    SHCoord point = {.lat = 47.01, .lon = 19.1};

    double dist = lc_point_to_segment_distance(point, seg_a, seg_b);
    /* 0.01 degrees latitude is about 1.1 km */
    ASSERT(dist > 1000);
    ASSERT(dist < 1200);
}

TEST(point_to_segment_off_endpoint)
{
    /* Point closest to segment endpoint (not projected onto segment) */
    SHCoord seg_a = {.lat = 47.0, .lon = 19.0};
    SHCoord seg_b = {.lat = 47.0, .lon = 19.1};

    /* Point is past the end of the segment */
    SHCoord point = {.lat = 47.0, .lon = 19.2};

    double dist = lc_point_to_segment_distance(point, seg_a, seg_b);
    /* Should be distance from point to seg_b (0.1 degrees longitude ~= 7.4 km) */
    ASSERT(dist > 7000);
    ASSERT(dist < 8000);
}

TEST(point_to_linestring_simple)
{
    LCLineString *line = lc_linestring_create(4);
    ASSERT(line != NULL);

    /* Create an L-shaped linestring */
    SHCoord p1 = {.lat = 47.0, .lon = 19.0};
    SHCoord p2 = {.lat = 47.0, .lon = 19.1};
    SHCoord p3 = {.lat = 47.1, .lon = 19.1};

    lc_linestring_add_point(line, p1);
    lc_linestring_add_point(line, p2);
    lc_linestring_add_point(line, p3);

    /* Point on the corner */
    SHCoord query = {.lat = 47.0, .lon = 19.1};
    double dist = lc_point_to_linestring_distance(query, line);
    ASSERT(dist < 1.0);  /* Should be ~0 */

    /* Point between first two points */
    SHCoord query2 = {.lat = 47.0, .lon = 19.05};
    dist = lc_point_to_linestring_distance(query2, line);
    ASSERT(dist < 1.0);  /* Should be ~0 (on the line) */

    lc_linestring_free(line);
}

TEST(point_to_entity_with_geometry)
{
    LCEntity entity;
    lc_entity_init(&entity);
    entity.fclass = LC_CLASS_STREET;
    entity.centroid.lat = 47.05;  /* Centroid in the middle */
    entity.centroid.lon = 19.05;

    /* Create a street geometry */
    LCLineString *line = lc_linestring_create(2);
    SHCoord p1 = {.lat = 47.0, .lon = 19.0};
    SHCoord p2 = {.lat = 47.1, .lon = 19.1};
    lc_linestring_add_point(line, p1);
    lc_linestring_add_point(line, p2);
    entity.geometry = line;

    /* Point near the start of the street (far from centroid) */
    SHCoord query = {.lat = 47.0, .lon = 19.0};
    double dist = lc_point_to_entity_distance(query, &entity);
    ASSERT(dist < 10.0);  /* Very close to street start */

    /* Point near the centroid but not on the line */
    SHCoord query2 = {.lat = 47.05, .lon = 19.0};  /* West of the line */
    double dist2 = lc_point_to_entity_distance(query2, &entity);
    /* Should be closer to line than to centroid would be */
    ASSERT(dist2 < 4000);  /* Should be ~3.7 km (perpendicular to diagonal line) */

    lc_linestring_free(line);
    entity.geometry = NULL;  /* Prevent double free */
}

TEST(reverse_with_street_geometry)
{
    LCIndex *index = lc_index_create();
    LCEntityStore *store = lc_entity_store_create(10);

    /* Add a street with geometry going from (47.0, 19.0) to (47.0, 19.1) */
    LCEntity street = {0};
    street.name = lc_entity_store_intern(store, "Test Street", 0);
    street.fclass = LC_CLASS_STREET;
    street.centroid.lat = 47.0;
    street.centroid.lon = 19.05;  /* Centroid at midpoint */

    /* Create geometry */
    LCLineString *geometry = lc_linestring_create(2);
    SHCoord p1 = {.lat = 47.0, .lon = 19.0};
    SHCoord p2 = {.lat = 47.0, .lon = 19.1};
    lc_linestring_add_point(geometry, p1);
    lc_linestring_add_point(geometry, p2);
    street.geometry = geometry;

    lc_entity_store_add(store, &street);
    lc_index_build(index, store);

    /* Query a point on the street start (far from centroid) */
    SHCoord query = {.lat = 47.0, .lon = 19.0};
    LCReverseResult result;
    LCStatus status = lc_reverse(index, query, NULL, &result);
    ASSERT_EQ(status, LC_OK);
    ASSERT(result.street != NULL);
    ASSERT_STR_EQ(result.street->name, "Test Street");
    ASSERT(result.distance_m < 10.0);  /* Should be very close using line distance */

    lc_reverse_result_free(&result);
    lc_index_free(index);
}

/* ============================================================================
 * Index Tests
 * ============================================================================ */

TEST(index_create)
{
    LCIndex *index = lc_index_create();
    ASSERT(index != NULL);
    ASSERT_EQ(lc_index_entity_count(index), 0);
    lc_index_free(index);
}

TEST(index_build)
{
    LCIndex *index = lc_index_create();
    ASSERT(index != NULL);

    /* Create a small entity store */
    LCEntityStore *store = lc_entity_store_create(10);
    ASSERT(store != NULL);

    LCEntity e1 = {0};
    e1.osm_id = 1;
    e1.fclass = LC_CLASS_CITY;
    e1.name = lc_entity_store_intern(store, "Budapest", 0);
    e1.population = 1750000;
    e1.centroid.lat = 47.497912;
    e1.centroid.lon = 19.040235;
    lc_entity_store_add(store, &e1);

    LCEntity e2 = {0};
    e2.osm_id = 2;
    e2.fclass = LC_CLASS_CITY;
    e2.name = lc_entity_store_intern(store, "Vienna", 0);
    e2.population = 1900000;
    e2.centroid.lat = 48.208174;
    e2.centroid.lon = 16.373819;
    lc_entity_store_add(store, &e2);

    LCStatus status = lc_index_build(index, store);
    ASSERT_EQ(status, LC_OK);
    ASSERT_EQ(lc_index_entity_count(index), 2);

    lc_index_free(index);
}

TEST(search_exact)
{
    LCIndex *index = lc_index_create();
    LCEntityStore *store = lc_entity_store_create(10);

    LCEntity e1 = {0};
    e1.osm_id = 1;
    e1.fclass = LC_CLASS_CITY;
    e1.name = lc_entity_store_intern(store, "Budapest", 0);
    e1.population = 1750000;
    e1.centroid.lat = 47.497912;
    e1.centroid.lon = 19.040235;
    lc_entity_store_add(store, &e1);

    lc_index_build(index, store);

    LCSearchResult result;
    LCStatus status = lc_search(index, "Budapest", NULL, &result);
    ASSERT_EQ(status, LC_OK);
    ASSERT(result.num_results >= 1);

    const LCEntity *found = lc_search_get_entity(index, &result.matches[0]);
    ASSERT(found != NULL);
    ASSERT_STR_EQ(found->name, "Budapest");

    lc_search_result_free(&result);
    lc_index_free(index);
}

TEST(search_prefix)
{
    LCIndex *index = lc_index_create();
    LCEntityStore *store = lc_entity_store_create(10);

    LCEntity e1 = {0};
    e1.name = lc_entity_store_intern(store, "Budapest", 0);
    e1.fclass = LC_CLASS_CITY;
    lc_entity_store_add(store, &e1);

    LCEntity e2 = {0};
    e2.name = lc_entity_store_intern(store, "Budaors", 0);
    e2.fclass = LC_CLASS_TOWN;
    lc_entity_store_add(store, &e2);

    lc_index_build(index, store);

    LCSearchResult result;
    lc_search(index, "Buda", NULL, &result);
    ASSERT_EQ(result.num_results, 2);

    lc_search_result_free(&result);
    lc_index_free(index);
}

TEST(autocomplete)
{
    LCIndex *index = lc_index_create();
    LCEntityStore *store = lc_entity_store_create(10);

    LCEntity e1 = {0};
    e1.name = lc_entity_store_intern(store, "Budapest", 0);
    e1.fclass = LC_CLASS_CITY;
    e1.population = 1750000;
    lc_entity_store_add(store, &e1);

    LCEntity e2 = {0};
    e2.name = lc_entity_store_intern(store, "Berlin", 0);
    e2.fclass = LC_CLASS_CITY;
    e2.population = 3600000;
    lc_entity_store_add(store, &e2);

    lc_index_build(index, store);

    LCSearchResult result;
    lc_autocomplete(index, "Bu", 5, &result);
    ASSERT_EQ(result.num_results, 1);

    const LCEntity *found = lc_search_get_entity(index, &result.matches[0]);
    ASSERT_STR_EQ(found->name, "Budapest");

    lc_search_result_free(&result);
    lc_index_free(index);
}

TEST(reverse_basic)
{
    LCIndex *index = lc_index_create();
    LCEntityStore *store = lc_entity_store_create(10);

    LCEntity e1 = {0};
    e1.name = lc_entity_store_intern(store, "Main Street", 0);
    e1.fclass = LC_CLASS_STREET;
    e1.centroid.lat = 47.5;
    e1.centroid.lon = 19.5;
    lc_entity_store_add(store, &e1);

    lc_index_build(index, store);

    SHCoord coord = {.lat = 47.5001, .lon = 19.5001};  /* Very close to street */
    LCReverseResult result;
    LCStatus status = lc_reverse(index, coord, NULL, &result);
    ASSERT_EQ(status, LC_OK);
    ASSERT(result.street != NULL);
    ASSERT_STR_EQ(result.street->name, "Main Street");

    lc_reverse_result_free(&result);
    lc_index_free(index);
}

/* ============================================================================
 * Query Parser Tests
 * ============================================================================ */

TEST(query_parse_european_style)
{
    /* European style: "Street Name 123" */
    LCParsedQuery pq;

    ASSERT(lc_parse_address_query("Edvi Illés út 7", &pq));
    ASSERT(pq.has_housenumber);
    ASSERT_STR_EQ(pq.street, "Edvi Illés út");
    ASSERT_STR_EQ(pq.housenumber, "7");
    lc_parsed_query_free(&pq);

    ASSERT(lc_parse_address_query("Kossuth tér 5/A", &pq));
    ASSERT(pq.has_housenumber);
    ASSERT_STR_EQ(pq.street, "Kossuth tér");
    ASSERT_STR_EQ(pq.housenumber, "5/A");
    lc_parsed_query_free(&pq);

    ASSERT(lc_parse_address_query("Váci utca 12-14", &pq));
    ASSERT(pq.has_housenumber);
    ASSERT_STR_EQ(pq.street, "Váci utca");
    ASSERT_STR_EQ(pq.housenumber, "12-14");
    lc_parsed_query_free(&pq);
}

TEST(query_parse_us_style)
{
    /* US style: "123 Street Name" */
    LCParsedQuery pq;

    ASSERT(lc_parse_address_query("123 Main Street", &pq));
    ASSERT(pq.has_housenumber);
    ASSERT_STR_EQ(pq.street, "Main Street");
    ASSERT_STR_EQ(pq.housenumber, "123");
    lc_parsed_query_free(&pq);

    ASSERT(lc_parse_address_query("42 Oak Avenue", &pq));
    ASSERT(pq.has_housenumber);
    ASSERT_STR_EQ(pq.street, "Oak Avenue");
    ASSERT_STR_EQ(pq.housenumber, "42");
    lc_parsed_query_free(&pq);
}

TEST(query_parse_no_number)
{
    /* No house number - entire query is street name */
    LCParsedQuery pq;

    ASSERT(lc_parse_address_query("Budapest", &pq));
    ASSERT(!pq.has_housenumber);
    ASSERT_STR_EQ(pq.street, "Budapest");
    ASSERT(pq.housenumber == NULL);
    lc_parsed_query_free(&pq);

    ASSERT(lc_parse_address_query("Andrássy út", &pq));
    ASSERT(!pq.has_housenumber);
    ASSERT_STR_EQ(pq.street, "Andrássy út");
    lc_parsed_query_free(&pq);
}

TEST(query_parse_whitespace)
{
    /* Handle extra whitespace */
    LCParsedQuery pq;

    ASSERT(lc_parse_address_query("  Váci utca  15  ", &pq));
    ASSERT(pq.has_housenumber);
    ASSERT_STR_EQ(pq.street, "Váci utca");
    ASSERT_STR_EQ(pq.housenumber, "15");
    lc_parsed_query_free(&pq);
}

TEST(query_is_housenumber)
{
    /* Valid house numbers */
    ASSERT(lc_is_housenumber("7"));
    ASSERT(lc_is_housenumber("123"));
    ASSERT(lc_is_housenumber("5/A"));
    ASSERT(lc_is_housenumber("12-14"));
    ASSERT(lc_is_housenumber("3B"));
    ASSERT(lc_is_housenumber("42/1"));

    /* Invalid house numbers */
    ASSERT(!lc_is_housenumber(""));
    ASSERT(!lc_is_housenumber("ABC"));
    ASSERT(!lc_is_housenumber("utca"));
    ASSERT(!lc_is_housenumber("/5"));
    ASSERT(!lc_is_housenumber("-12"));
}

TEST(query_parse_edge_cases)
{
    LCParsedQuery pq;

    /* Empty query */
    ASSERT(!lc_parse_address_query("", &pq));
    ASSERT(!lc_parse_address_query("   ", &pq));

    /* NULL query */
    ASSERT(!lc_parse_address_query(NULL, &pq));

    /* Single word that's a number (treat as street) */
    ASSERT(lc_parse_address_query("123", &pq));
    /* This is ambiguous - could be just a number. Current behavior: no house number extracted */
    lc_parsed_query_free(&pq);
}

/* ============================================================================
 * Address Search Tests
 * ============================================================================ */

TEST(search_address_with_housenumber)
{
    LCIndex *index = lc_index_create();
    LCEntityStore *store = lc_entity_store_create(10);

    /* Add a street */
    LCEntity street = {0};
    street.name = lc_entity_store_intern(store, "Váci utca", 0);
    street.fclass = LC_CLASS_STREET;
    street.centroid.lat = 47.5;
    street.centroid.lon = 19.05;
    lc_entity_store_add(store, &street);

    /* Add an address on that street */
    LCEntity addr1 = {0};
    addr1.fclass = LC_CLASS_ADDRESS;
    addr1.address.street = lc_entity_store_intern(store, "Váci utca", 0);
    addr1.address.housenumber = lc_entity_store_intern(store, "15", 0);
    addr1.centroid.lat = 47.501;
    addr1.centroid.lon = 19.051;
    lc_entity_store_add(store, &addr1);

    /* Add another address with different number */
    LCEntity addr2 = {0};
    addr2.fclass = LC_CLASS_ADDRESS;
    addr2.address.street = lc_entity_store_intern(store, "Váci utca", 0);
    addr2.address.housenumber = lc_entity_store_intern(store, "20", 0);
    addr2.centroid.lat = 47.502;
    addr2.centroid.lon = 19.052;
    lc_entity_store_add(store, &addr2);

    lc_index_build(index, store);

    /* Search for "Váci utca 15" - should find address with housenumber 15 first */
    LCSearchResult result;
    LCStatus status = lc_search(index, "Váci utca 15", NULL, &result);
    ASSERT_EQ(status, LC_OK);
    ASSERT(result.num_results >= 1);

    /* First result should be the address with housenumber 15 */
    const LCEntity *first = &index->entities->entities[result.matches[0].entity_id];
    ASSERT_EQ(first->fclass, LC_CLASS_ADDRESS);
    ASSERT_STR_EQ(first->address.housenumber, "15");

    lc_search_result_free(&result);

    /* Search for "Váci utca 20" - should find address with housenumber 20 first */
    status = lc_search(index, "Váci utca 20", NULL, &result);
    ASSERT_EQ(status, LC_OK);
    ASSERT(result.num_results >= 1);

    first = &index->entities->entities[result.matches[0].entity_id];
    ASSERT_EQ(first->fclass, LC_CLASS_ADDRESS);
    ASSERT_STR_EQ(first->address.housenumber, "20");

    lc_search_result_free(&result);
    lc_index_free(index);
}

TEST(search_address_street_only)
{
    LCIndex *index = lc_index_create();
    LCEntityStore *store = lc_entity_store_create(10);

    /* Add a street */
    LCEntity street = {0};
    street.name = lc_entity_store_intern(store, "Andrássy út", 0);
    street.fclass = LC_CLASS_STREET;
    street.centroid.lat = 47.5;
    street.centroid.lon = 19.05;
    lc_entity_store_add(store, &street);

    lc_index_build(index, store);

    /* Search for street only (no house number) */
    LCSearchResult result;
    LCStatus status = lc_search(index, "Andrássy út", NULL, &result);
    ASSERT_EQ(status, LC_OK);
    ASSERT(result.num_results >= 1);

    /* Should find the street */
    const LCEntity *first = &index->entities->entities[result.matches[0].entity_id];
    ASSERT_EQ(first->fclass, LC_CLASS_STREET);
    ASSERT_STR_EQ(first->name, "Andrássy út");

    lc_search_result_free(&result);
    lc_index_free(index);
}

/* ============================================================================
 * Main
 * ============================================================================ */

/* ============================================================================
 * API Handler Tests
 * ============================================================================ */

/* Build an index of `count` entities whose names all share a search token and
 * are long enough to be interesting to a JSON writer. */
static LCIndex *build_long_name_index(int count)
{
    LCEntityStore *store = lc_entity_store_create(256);
    if (!store) return NULL;

    for (int i = 0; i < count; i++) {
        char name[600];
        LCEntity e;

        snprintf(name, sizeof(name), "zebra%0*d", 560, i);

        memset(&e, 0, sizeof(e));
        e.name = lc_entity_store_intern(store, name, 0);
        e.osm_id = (uint64_t)(1000 + i);
        e.type = LC_ENTITY_NODE;
        e.centroid.lat = 43.7 + i * 0.0001;
        e.centroid.lon = 7.4 + i * 0.0001;
        if (lc_entity_store_add(store, &e) != LC_OK) {
            lc_entity_store_free(store);
            return NULL;
        }
    }

    LCIndex *index = lc_index_create();
    if (!index) { lc_entity_store_free(store); return NULL; }
    if (lc_index_build(index, store) != LC_OK) {
        lc_index_free(index);
        return NULL;
    }
    return index;   /* index owns the store from here */
}

/*
 * Regression: lc_api_search() built its JSON into a fixed 64 KB malloc and
 * accumulated snprintf's would-have-written return value into an unclamped
 * `offset`, so a full page of long-named results walked `json + offset` past
 * the end of the allocation. Under -D_FORTIFY_SOURCE=2 this aborted the
 * process with "buffer overflow detected"; without it, it was a heap write
 * out of bounds. The streaming writer has no fixed ceiling.
 */
TEST(api_search_large_result_set)
{
    LCIndex *index = build_long_name_index(120);
    ASSERT(index != NULL);

    LCAPIContext *api = lc_api_create(index, NULL);
    ASSERT(api != NULL);

    int status = 0;
    size_t len = 0;
    char *json = lc_api_search(api, "zebra", 100, &status, &len);

    ASSERT(json != NULL);
    ASSERT_EQ(status, 200);
    /* The point of the test: comfortably past the old 64 KB ceiling. */
    ASSERT(len > 64 * 1024);
    ASSERT_EQ(strlen(json), len);
    ASSERT(json[0] == '{');
    ASSERT(json[len - 1] == '}');

    free(json);
    lc_api_free(api);
    lc_index_free(index);
}

/* A name containing a quote must not be able to break out of the JSON string.
 * The WASM copy of this endpoint interpolated names with a bare %s. */
TEST(api_search_escapes_names)
{
    LCEntityStore *store = lc_entity_store_create(4);
    ASSERT(store != NULL);

    LCEntity e;
    memset(&e, 0, sizeof(e));
    e.name = lc_entity_store_intern(store, "Cafe \"Quote\" Bar", 0);
    e.osm_id = 7;
    e.type = LC_ENTITY_NODE;
    e.centroid.lat = 43.7;
    e.centroid.lon = 7.4;
    ASSERT_EQ(lc_entity_store_add(store, &e), LC_OK);

    LCIndex *index = lc_index_create();
    ASSERT(index != NULL);
    ASSERT_EQ(lc_index_build(index, store), LC_OK);

    LCAPIContext *api = lc_api_create(index, NULL);
    ASSERT(api != NULL);

    int status = 0;
    size_t len = 0;
    char *json = lc_api_search(api, "Cafe", 10, &status, &len);
    ASSERT(json != NULL);
    ASSERT_EQ(status, 200);
    /* Escaped, not raw. */
    ASSERT(strstr(json, "\\\"Quote\\\"") != NULL);

    free(json);
    lc_api_free(api);
    lc_index_free(index);
}

/* Reverse geocoding must reject text that is not a number. sh_parse_double
 * yields NaN, and NaN fails every range comparison, so the isnan() guard is
 * what turns it into a 400 rather than a lookup at (0, 0). */
TEST(api_reverse_rejects_nan)
{
    LCIndex *index = build_long_name_index(1);
    ASSERT(index != NULL);

    LCAPIContext *api = lc_api_create(index, NULL);
    ASSERT(api != NULL);

    int status = 0;
    size_t len = 0;
    char *json = lc_api_reverse(api, NAN, 7.4, &status, &len);
    ASSERT(json == NULL);
    ASSERT_EQ(status, 400);

    json = lc_api_reverse(api, 43.7, NAN, &status, &len);
    ASSERT(json == NULL);
    ASSERT_EQ(status, 400);

    lc_api_free(api);
    lc_index_free(index);
}

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
    RUN_TEST(normalize_truncated_utf8);

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

    printf("\nLineString:\n");
    RUN_TEST(linestring_create);
    RUN_TEST(linestring_add_points);

    printf("\nPoint-to-Line Distance:\n");
    RUN_TEST(point_to_segment_on_endpoint);
    RUN_TEST(point_to_segment_perpendicular);
    RUN_TEST(point_to_segment_off_endpoint);
    RUN_TEST(point_to_linestring_simple);
    RUN_TEST(point_to_entity_with_geometry);
    RUN_TEST(reverse_with_street_geometry);

    printf("\nIndex:\n");
    RUN_TEST(index_create);
    RUN_TEST(index_build);
    RUN_TEST(search_exact);
    RUN_TEST(search_prefix);
    RUN_TEST(autocomplete);
    RUN_TEST(reverse_basic);

    printf("\nQuery Parser:\n");
    RUN_TEST(query_parse_european_style);
    RUN_TEST(query_parse_us_style);
    RUN_TEST(query_parse_no_number);
    RUN_TEST(query_parse_whitespace);
    RUN_TEST(query_is_housenumber);
    RUN_TEST(query_parse_edge_cases);

    printf("\nAddress Search:\n");
    RUN_TEST(search_address_with_housenumber);
    RUN_TEST(search_address_street_only);

    printf("\nAPI Handler:\n");
    RUN_TEST(api_search_large_result_set);
    RUN_TEST(api_search_escapes_names);
    RUN_TEST(api_reverse_rejects_nan);

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
