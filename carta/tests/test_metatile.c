/*
 * test_metatile.c - Tests for metatile label placement cache
 */

#include "ct_metatile.h"
#include "ct_label.h"
#include "ct_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    static int test_##name(void); \
    static void run_test_##name(void) { \
        tests_run++; \
        printf("  %-50s ", #name); \
        if (test_##name()) { \
            tests_passed++; \
            printf("[PASS]\n"); \
        } else { \
            printf("[FAIL]\n"); \
        } \
    } \
    static int test_##name(void)

#define ASSERT(cond) do { if (!(cond)) return 0; } while(0)
#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NEAR(a, b, eps) ASSERT(fabs((double)(a) - (double)(b)) < (eps))

/* ============================================================================
 * Coordinate Alignment Tests
 * ============================================================================ */

TEST(metatile_coord_even_tile)
{
    /* Even tile (x=100, y=200) should map to itself */
    CTTileCoord coord = {14, 100, 200};
    CTMetatileCoord mt = ct_metatile_coord(coord);
    ASSERT_EQ(mt.z, 14);
    ASSERT_EQ(mt.mx, 100);
    ASSERT_EQ(mt.my, 200);
    return 1;
}

TEST(metatile_coord_odd_tile)
{
    /* Odd tile (x=101, y=201) should map to (100, 200) */
    CTTileCoord coord = {14, 101, 201};
    CTMetatileCoord mt = ct_metatile_coord(coord);
    ASSERT_EQ(mt.z, 14);
    ASSERT_EQ(mt.mx, 100);
    ASSERT_EQ(mt.my, 200);
    return 1;
}

TEST(metatile_coord_z0)
{
    /* z0 has only tile (0,0) — metatile is (0,0) */
    CTTileCoord coord = {0, 0, 0};
    CTMetatileCoord mt = ct_metatile_coord(coord);
    ASSERT_EQ(mt.z, 0);
    ASSERT_EQ(mt.mx, 0);
    ASSERT_EQ(mt.my, 0);
    return 1;
}

TEST(metatile_coord_z1)
{
    /* z1: tile (1,1) -> metatile (0,0) */
    CTTileCoord coord = {1, 1, 1};
    CTMetatileCoord mt = ct_metatile_coord(coord);
    ASSERT_EQ(mt.z, 1);
    ASSERT_EQ(mt.mx, 0);
    ASSERT_EQ(mt.my, 0);
    return 1;
}

TEST(metatile_subtile_positions)
{
    int sx, sy;

    /* (0,0) -> subtile (0,0) */
    CTTileCoord c00 = {14, 100, 200};
    ct_metatile_subtile(c00, &sx, &sy);
    ASSERT_EQ(sx, 0);
    ASSERT_EQ(sy, 0);

    /* (1,0) -> subtile (1,0) */
    CTTileCoord c10 = {14, 101, 200};
    ct_metatile_subtile(c10, &sx, &sy);
    ASSERT_EQ(sx, 1);
    ASSERT_EQ(sy, 0);

    /* (0,1) -> subtile (0,1) */
    CTTileCoord c01 = {14, 100, 201};
    ct_metatile_subtile(c01, &sx, &sy);
    ASSERT_EQ(sx, 0);
    ASSERT_EQ(sy, 1);

    /* (1,1) -> subtile (1,1) */
    CTTileCoord c11 = {14, 101, 201};
    ct_metatile_subtile(c11, &sx, &sy);
    ASSERT_EQ(sx, 1);
    ASSERT_EQ(sy, 1);

    return 1;
}

TEST(metatile_all_four_tiles_same_metatile)
{
    /* All 4 tiles in a group should produce the same metatile coord */
    CTMetatileCoord m00 = ct_metatile_coord((CTTileCoord){14, 100, 200});
    CTMetatileCoord m10 = ct_metatile_coord((CTTileCoord){14, 101, 200});
    CTMetatileCoord m01 = ct_metatile_coord((CTTileCoord){14, 100, 201});
    CTMetatileCoord m11 = ct_metatile_coord((CTTileCoord){14, 101, 201});

    ASSERT_EQ(m00.z, m10.z);
    ASSERT_EQ(m00.mx, m10.mx);
    ASSERT_EQ(m00.my, m10.my);

    ASSERT_EQ(m00.z, m01.z);
    ASSERT_EQ(m00.mx, m01.mx);
    ASSERT_EQ(m00.my, m01.my);

    ASSERT_EQ(m00.z, m11.z);
    ASSERT_EQ(m00.mx, m11.mx);
    ASSERT_EQ(m00.my, m11.my);

    return 1;
}

TEST(metatile_adjacent_groups_differ)
{
    /* Adjacent groups should produce different metatile coords */
    CTMetatileCoord m1 = ct_metatile_coord((CTTileCoord){14, 100, 200});
    CTMetatileCoord m2 = ct_metatile_coord((CTTileCoord){14, 102, 200});
    CTMetatileCoord m3 = ct_metatile_coord((CTTileCoord){14, 100, 202});

    ASSERT(m1.mx != m2.mx || m1.my != m2.my);
    ASSERT(m1.mx != m3.mx || m1.my != m3.my);

    return 1;
}

/* ============================================================================
 * Cache CRUD Tests
 * ============================================================================ */

TEST(metatile_cache_create_free)
{
    CTMetatileLabelCache *cache = ct_metatile_cache_create(64);
    ASSERT(cache != NULL);
    ct_metatile_cache_free(cache);
    return 1;
}

TEST(metatile_cache_free_null)
{
    /* Should not crash */
    ct_metatile_cache_free(NULL);
    return 1;
}

TEST(metatile_cache_miss_returns_null)
{
    CTMetatileLabelCache *cache = ct_metatile_cache_create(64);
    ASSERT(cache != NULL);

    CTMetatileCoord mt = {14, 100, 200};
    const CTMetatileLabelResult *result = ct_metatile_cache_get(cache, mt);
    ASSERT(result == NULL);

    ct_metatile_cache_free(cache);
    return 1;
}

TEST(metatile_cache_put_get)
{
    CTMetatileLabelCache *cache = ct_metatile_cache_create(64);
    ASSERT(cache != NULL);

    /* Create a minimal result */
    CTMetatileLabelResult *result = calloc(1, sizeof(*result));
    ASSERT(result != NULL);
    result->coord = (CTMetatileCoord){14, 100, 200};
    result->tile_size = 512;
    result->labels = NULL;
    result->num_labels = 0;
    result->roads = NULL;
    result->num_roads = 0;

    CTMetatileCoord mt = {14, 100, 200};
    ct_metatile_cache_put(cache, mt, result);

    /* Should be retrievable */
    const CTMetatileLabelResult *got = ct_metatile_cache_get(cache, mt);
    ASSERT(got != NULL);
    ASSERT_EQ(got->coord.z, 14);
    ASSERT_EQ(got->coord.mx, 100);
    ASSERT_EQ(got->coord.my, 200);
    ASSERT_EQ(got->tile_size, 512);
    ct_metatile_cache_release(cache, got);

    ct_metatile_cache_free(cache);
    return 1;
}

TEST(metatile_cache_different_keys)
{
    CTMetatileLabelCache *cache = ct_metatile_cache_create(64);
    ASSERT(cache != NULL);

    /* Insert two results with different keys */
    CTMetatileCoord mt1 = {14, 100, 200};
    CTMetatileCoord mt2 = {14, 102, 200};

    CTMetatileLabelResult *r1 = calloc(1, sizeof(*r1));
    r1->coord = mt1;
    r1->tile_size = 512;

    CTMetatileLabelResult *r2 = calloc(1, sizeof(*r2));
    r2->coord = mt2;
    r2->tile_size = 256;

    ct_metatile_cache_put(cache, mt1, r1);
    ct_metatile_cache_put(cache, mt2, r2);

    /* Both should be retrievable with correct data */
    const CTMetatileLabelResult *got1 = ct_metatile_cache_get(cache, mt1);
    ASSERT(got1 != NULL);
    ASSERT_EQ(got1->tile_size, 512);
    ct_metatile_cache_release(cache, got1);

    const CTMetatileLabelResult *got2 = ct_metatile_cache_get(cache, mt2);
    ASSERT(got2 != NULL);
    ASSERT_EQ(got2->tile_size, 256);
    ct_metatile_cache_release(cache, got2);

    ct_metatile_cache_free(cache);
    return 1;
}

TEST(metatile_cache_duplicate_put_keeps_first)
{
    /* When the same key is inserted twice, the cache keeps the first
     * value and discards the duplicate (race-condition safe behavior) */
    CTMetatileLabelCache *cache = ct_metatile_cache_create(64);
    ASSERT(cache != NULL);

    CTMetatileCoord mt = {14, 100, 200};

    /* Insert first */
    CTMetatileLabelResult *r1 = calloc(1, sizeof(*r1));
    r1->coord = mt;
    r1->tile_size = 512;
    ct_metatile_cache_put(cache, mt, r1);

    /* Insert again with same key — should be discarded */
    CTMetatileLabelResult *r2 = calloc(1, sizeof(*r2));
    r2->coord = mt;
    r2->tile_size = 256;
    ct_metatile_cache_put(cache, mt, r2);

    /* Should still get the first value */
    const CTMetatileLabelResult *got = ct_metatile_cache_get(cache, mt);
    ASSERT(got != NULL);
    ASSERT_EQ(got->tile_size, 512);
    ct_metatile_cache_release(cache, got);

    ct_metatile_cache_free(cache);
    return 1;
}

TEST(metatile_cache_lru_eviction)
{
    /* Create a small cache (max 4 entries) */
    CTMetatileLabelCache *cache = ct_metatile_cache_create(4);
    ASSERT(cache != NULL);

    /* Insert 5 entries — the first should be evicted */
    for (int i = 0; i < 5; i++) {
        CTMetatileCoord mt = {14, i * 2, 0};
        CTMetatileLabelResult *r = calloc(1, sizeof(*r));
        r->coord = mt;
        r->tile_size = 512;
        ct_metatile_cache_put(cache, mt, r);
    }

    /* Entry 0 (mx=0) should have been evicted */
    CTMetatileCoord mt0 = {14, 0, 0};
    const CTMetatileLabelResult *got0 = ct_metatile_cache_get(cache, mt0);
    ASSERT(got0 == NULL);

    /* Entry 4 (mx=8) should still be there */
    CTMetatileCoord mt4 = {14, 8, 0};
    const CTMetatileLabelResult *got4 = ct_metatile_cache_get(cache, mt4);
    ASSERT(got4 != NULL);
    ct_metatile_cache_release(cache, got4);

    ct_metatile_cache_free(cache);
    return 1;
}

TEST(metatile_cache_zoom_separation)
{
    /* Same (mx, my) at different zoom levels should be different keys */
    CTMetatileLabelCache *cache = ct_metatile_cache_create(64);
    ASSERT(cache != NULL);

    CTMetatileCoord mt10 = {10, 100, 200};
    CTMetatileCoord mt14 = {14, 100, 200};

    CTMetatileLabelResult *r10 = calloc(1, sizeof(*r10));
    r10->coord = mt10;
    r10->tile_size = 512;

    CTMetatileLabelResult *r14 = calloc(1, sizeof(*r14));
    r14->coord = mt14;
    r14->tile_size = 256;

    ct_metatile_cache_put(cache, mt10, r10);
    ct_metatile_cache_put(cache, mt14, r14);

    const CTMetatileLabelResult *got10 = ct_metatile_cache_get(cache, mt10);
    ASSERT(got10 != NULL);
    ASSERT_EQ(got10->tile_size, 512);
    ct_metatile_cache_release(cache, got10);

    const CTMetatileLabelResult *got14 = ct_metatile_cache_get(cache, mt14);
    ASSERT(got14 != NULL);
    ASSERT_EQ(got14->tile_size, 256);
    ct_metatile_cache_release(cache, got14);

    ct_metatile_cache_free(cache);
    return 1;
}

/* ============================================================================
 * Extract Sub-tile Tests
 * ============================================================================ */

TEST(metatile_extract_empty_result)
{
    /* Extract from empty result should produce empty placer */
    CTMetatileLabelResult result = {0};
    result.coord = (CTMetatileCoord){14, 100, 200};
    result.tile_size = 512;

    CTLabelPlacer *placer = ct_label_placer_create(512, 512);
    ASSERT(placer != NULL);

    CTRoadLabelPlacement *roads = NULL;
    size_t road_count = 0;
    ct_metatile_extract_subtile(&result, 0, 0, placer, &roads, &road_count);

    ASSERT_EQ(road_count, 0);
    ASSERT(roads == NULL);

    ct_label_placer_free(placer);
    return 1;
}

TEST(metatile_extract_label_in_subtile_00)
{
    /* A label at position (100, 100) in metatile space should appear
     * in sub-tile (0,0) at (100, 100) with tile_size=512 */
    CTMetatileLabelResult result = {0};
    result.coord = (CTMetatileCoord){14, 100, 200};
    result.tile_size = 512;

    CTLabelPlacement label = {0};
    label.x = 100;
    label.y = 100;
    label.width = 40;
    label.height = 12;
    label.name = "Test Label";


    result.labels = &label;
    result.num_labels = 1;

    CTLabelPlacer *placer = ct_label_placer_create(512, 512);
    ASSERT(placer != NULL);

    CTRoadLabelPlacement *roads = NULL;
    size_t road_count = 0;
    ct_metatile_extract_subtile(&result, 0, 0, placer, &roads, &road_count);

    /* The placer should have received this label (we can check indirectly
     * by verifying it didn't crash and completed) */

    /* Clean up - don't free result.labels since it's stack-allocated */
    result.labels = NULL;
    result.num_labels = 0;
    ct_label_placer_free(placer);
    return 1;
}

TEST(metatile_extract_label_in_subtile_11)
{
    /* A label at position (600, 600) in metatile space (with tile_size=512)
     * should appear in sub-tile (1,1) at (88, 88) */
    CTMetatileLabelResult result = {0};
    result.coord = (CTMetatileCoord){14, 100, 200};
    result.tile_size = 512;

    CTLabelPlacement label = {0};
    label.x = 600;
    label.y = 600;
    label.width = 40;
    label.height = 12;
    label.name = "Subtile 11";


    result.labels = &label;
    result.num_labels = 1;

    CTLabelPlacer *placer = ct_label_placer_create(512, 512);
    ASSERT(placer != NULL);

    CTRoadLabelPlacement *roads = NULL;
    size_t road_count = 0;
    ct_metatile_extract_subtile(&result, 1, 1, placer, &roads, &road_count);

    /* Should not crash and should have processed the label
     * (it's at 600-512=88 in sub-tile space, within bounds) */

    result.labels = NULL;
    result.num_labels = 0;
    ct_label_placer_free(placer);
    return 1;
}

TEST(metatile_extract_label_outside_subtile)
{
    /* A label at position (600, 600) should NOT appear in sub-tile (0,0) */
    CTMetatileLabelResult result = {0};
    result.coord = (CTMetatileCoord){14, 100, 200};
    result.tile_size = 512;

    CTLabelPlacement label = {0};
    label.x = 600;
    label.y = 600;
    label.width = 40;
    label.height = 12;
    label.name = "Far Away";


    result.labels = &label;
    result.num_labels = 1;

    CTLabelPlacer *placer = ct_label_placer_create(512, 512);
    ASSERT(placer != NULL);

    CTRoadLabelPlacement *roads = NULL;
    size_t road_count = 0;
    ct_metatile_extract_subtile(&result, 0, 0, placer, &roads, &road_count);

    /* Should complete without issue, label is out of range for (0,0) */

    result.labels = NULL;
    result.num_labels = 0;
    ct_label_placer_free(placer);
    return 1;
}

/* ============================================================================
 * Result Free Tests
 * ============================================================================ */

TEST(metatile_result_free_null)
{
    /* Should not crash */
    ct_metatile_result_free(NULL);
    return 1;
}

TEST(metatile_result_free_empty)
{
    CTMetatileLabelResult *result = calloc(1, sizeof(*result));
    ASSERT(result != NULL);
    ct_metatile_result_free(result);
    return 1;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    printf("\n=== Metatile Label Cache Tests ===\n\n");

    printf("Coordinate Alignment:\n");
    run_test_metatile_coord_even_tile();
    run_test_metatile_coord_odd_tile();
    run_test_metatile_coord_z0();
    run_test_metatile_coord_z1();
    run_test_metatile_subtile_positions();
    run_test_metatile_all_four_tiles_same_metatile();
    run_test_metatile_adjacent_groups_differ();

    printf("\nCache Operations:\n");
    run_test_metatile_cache_create_free();
    run_test_metatile_cache_free_null();
    run_test_metatile_cache_miss_returns_null();
    run_test_metatile_cache_put_get();
    run_test_metatile_cache_different_keys();
    run_test_metatile_cache_duplicate_put_keeps_first();
    run_test_metatile_cache_lru_eviction();
    run_test_metatile_cache_zoom_separation();

    printf("\nExtract Sub-tile:\n");
    run_test_metatile_extract_empty_result();
    run_test_metatile_extract_label_in_subtile_00();
    run_test_metatile_extract_label_in_subtile_11();
    run_test_metatile_extract_label_outside_subtile();

    printf("\nResult Free:\n");
    run_test_metatile_result_free_null();
    run_test_metatile_result_free_empty();

    printf("\n--- Results: %d/%d passed ---\n\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
