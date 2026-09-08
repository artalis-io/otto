/*
 * ct_metatile.c - Metatile Label Placement Cache
 *
 * Computes labels across 2x2 tile groups for consistent cross-tile
 * placement, with an LRU cache for reuse.
 */

#include "ct_metatile.h"
#include "ct_label.h"
#include "ct_pbf.h"
#include "ct_tile.h"
#include "ct_collision.h"
#include "ct_polylabel.h"
#include "ct_rtree.h"
#include "sh_font.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef __EMSCRIPTEN__
#include "sh_pal.h"
#endif

/* ============================================================================
 * Cache Implementation
 * ============================================================================ */

/* Hash table entry for the metatile cache */
typedef struct CTMetatileCacheEntry {
    uint64_t key;
    CTMetatileLabelResult *result;
    struct CTMetatileCacheEntry *hash_next;  /* Hash chain */
    struct CTMetatileCacheEntry *lru_prev;   /* LRU doubly-linked list */
    struct CTMetatileCacheEntry *lru_next;
} CTMetatileCacheEntry;

#define MT_CACHE_BUCKETS 512

struct CTMetatileLabelCache {
    CTMetatileCacheEntry **buckets;
    size_t num_entries;
    size_t max_entries;

    /* LRU list: head = most recent, tail = least recent */
    CTMetatileCacheEntry *lru_head;
    CTMetatileCacheEntry *lru_tail;

#ifndef __EMSCRIPTEN__
    /*
     * A mutex, not a reader/writer lock, which is what this was.
     *
     * Every one of the three lock sites took the WRITE lock -- there was never
     * an rdlock -- because even the lookup path mutates: a cache hit moves the
     * entry to the head of the LRU list. An rwlock that is only ever
     * write-locked is a mutex with extra machinery, so this now says so, and
     * the PAL does not have to grow an ShRwLock for a lock nothing shares.
     */
    ShMutex lock;
#endif
};

/* Pack metatile coordinate into a uint64_t key.
 * z is at most 30, mx/my fit in 29 bits each. */
static inline uint64_t mt_key(CTMetatileCoord mt)
{
    return ((uint64_t)(mt.z & 0x1F) << 59) |
           ((uint64_t)(mt.mx & 0x1FFFFFFF) << 30) |
           ((uint64_t)(mt.my & 0x1FFFFFFF));
}

static inline size_t mt_bucket(uint64_t key)
{
    /* Mix bits for better distribution */
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    return (size_t)(key % MT_CACHE_BUCKETS);
}

/* Move entry to head of LRU list */
static void lru_touch(CTMetatileLabelCache *cache, CTMetatileCacheEntry *entry)
{
    if (cache->lru_head == entry) return;  /* Already at head */

    /* Remove from current position */
    if (entry->lru_prev) entry->lru_prev->lru_next = entry->lru_next;
    if (entry->lru_next) entry->lru_next->lru_prev = entry->lru_prev;
    if (cache->lru_tail == entry) cache->lru_tail = entry->lru_prev;

    /* Insert at head */
    entry->lru_prev = NULL;
    entry->lru_next = cache->lru_head;
    if (cache->lru_head) cache->lru_head->lru_prev = entry;
    cache->lru_head = entry;
    if (!cache->lru_tail) cache->lru_tail = entry;
}

/* Unlink entry from hash table and LRU (does NOT free — caller frees after unlock) */
static void cache_unlink_entry(CTMetatileLabelCache *cache, CTMetatileCacheEntry *entry)
{
    size_t bucket = mt_bucket(entry->key);
    CTMetatileCacheEntry **pp = &cache->buckets[bucket];
    while (*pp) {
        if (*pp == entry) {
            *pp = entry->hash_next;
            break;
        }
        pp = &(*pp)->hash_next;
    }

    /* Remove from LRU */
    if (entry->lru_prev) entry->lru_prev->lru_next = entry->lru_next;
    if (entry->lru_next) entry->lru_next->lru_prev = entry->lru_prev;
    if (cache->lru_head == entry) cache->lru_head = entry->lru_next;
    if (cache->lru_tail == entry) cache->lru_tail = entry->lru_prev;

    cache->num_entries--;
    entry->hash_next = NULL;
    entry->lru_prev = NULL;
    entry->lru_next = NULL;
}

/* Free an unlinked entry (call outside lock) */
static void cache_free_entry(CTMetatileCacheEntry *entry)
{
    ct_metatile_result_free(entry->result);
    free(entry);
}

CTMetatileLabelCache *ct_metatile_cache_create(size_t max_entries)
{
    if (max_entries == 0) return NULL;

    CTMetatileLabelCache *cache = calloc(1, sizeof(*cache));
    if (!cache) return NULL;

    cache->buckets = calloc(MT_CACHE_BUCKETS, sizeof(CTMetatileCacheEntry *));
    if (!cache->buckets) {
        free(cache);
        return NULL;
    }

    cache->max_entries = max_entries;

#ifndef __EMSCRIPTEN__
    sh_mutex_init(&cache->lock);
#endif

    return cache;
}

void ct_metatile_cache_free(CTMetatileLabelCache *cache)
{
    if (!cache) return;

    /* Free all entries (no lock needed — shutting down) */
    for (size_t i = 0; i < MT_CACHE_BUCKETS; i++) {
        CTMetatileCacheEntry *entry = cache->buckets[i];
        while (entry) {
            CTMetatileCacheEntry *next = entry->hash_next;
            cache_free_entry(entry);
            entry = next;
        }
    }

    free(cache->buckets);

#ifndef __EMSCRIPTEN__
    sh_mutex_destroy(&cache->lock);
#endif

    free(cache);
}

const CTMetatileLabelResult *ct_metatile_cache_get(
    CTMetatileLabelCache *cache, CTMetatileCoord mt)
{
    if (!cache) return NULL;

    uint64_t key = mt_key(mt);
    size_t bucket = mt_bucket(key);

#ifndef __EMSCRIPTEN__
    sh_mutex_lock(&cache->lock);
#endif

    CTMetatileCacheEntry *entry = cache->buckets[bucket];
    while (entry) {
        if (entry->key == key) {
            entry->result->refcount++;
            lru_touch(cache, entry);
#ifndef __EMSCRIPTEN__
            sh_mutex_unlock(&cache->lock);
#endif
            return entry->result;
        }
        entry = entry->hash_next;
    }

#ifndef __EMSCRIPTEN__
    sh_mutex_unlock(&cache->lock);
#endif

    return NULL;
}

void ct_metatile_cache_release(
    CTMetatileLabelCache *cache, const CTMetatileLabelResult *result)
{
    if (!cache || !result) return;

#ifndef __EMSCRIPTEN__
    sh_mutex_lock(&cache->lock);
#endif

    /* Safe cast: we only decrement refcount */
    ((CTMetatileLabelResult *)result)->refcount--;

#ifndef __EMSCRIPTEN__
    sh_mutex_unlock(&cache->lock);
#endif
}

void ct_metatile_cache_put(
    CTMetatileLabelCache *cache, CTMetatileCoord mt,
    CTMetatileLabelResult *result)
{
    if (!cache || !result) {
        if (result) ct_metatile_result_free(result);
        return;
    }

    uint64_t key = mt_key(mt);
    size_t bucket = mt_bucket(key);

#ifndef __EMSCRIPTEN__
    sh_mutex_lock(&cache->lock);
#endif

    /* Check if already exists (race: another thread computed it) */
    CTMetatileCacheEntry *existing = cache->buckets[bucket];
    while (existing) {
        if (existing->key == key) {
            /* Already cached — discard the new result */
#ifndef __EMSCRIPTEN__
            sh_mutex_unlock(&cache->lock);
#endif
            ct_metatile_result_free(result);
            return;
        }
        existing = existing->hash_next;
    }

    /* Evict LRU entries if at capacity — collect victims, free after unlock */
    CTMetatileCacheEntry *evict_list = NULL;
    while (cache->num_entries >= cache->max_entries && cache->lru_tail) {
        CTMetatileCacheEntry *victim = cache->lru_tail;
        /* Skip entries that are still in use */
        if (victim->result->refcount > 0) {
            /* Walk backwards to find an evictable entry */
            victim = victim->lru_prev;
            while (victim && victim->result->refcount > 0) {
                victim = victim->lru_prev;
            }
            if (!victim) break;  /* All entries in use, allow over-capacity */
        }
        cache_unlink_entry(cache, victim);
        victim->hash_next = evict_list;  /* Reuse hash_next as singly-linked evict chain */
        evict_list = victim;
    }

    /* Create new entry */
    CTMetatileCacheEntry *entry = calloc(1, sizeof(*entry));
    if (!entry) {
#ifndef __EMSCRIPTEN__
        sh_mutex_unlock(&cache->lock);
#endif
        ct_metatile_result_free(result);
        return;
    }

    entry->key = key;
    entry->result = result;
    result->refcount = 0;

    /* Insert into hash chain */
    entry->hash_next = cache->buckets[bucket];
    cache->buckets[bucket] = entry;

    /* Insert at LRU head */
    entry->lru_prev = NULL;
    entry->lru_next = cache->lru_head;
    if (cache->lru_head) cache->lru_head->lru_prev = entry;
    cache->lru_head = entry;
    if (!cache->lru_tail) cache->lru_tail = entry;

    cache->num_entries++;

#ifndef __EMSCRIPTEN__
    sh_mutex_unlock(&cache->lock);
#endif

    /* Free evicted entries outside the lock */
    while (evict_list) {
        CTMetatileCacheEntry *next = evict_list->hash_next;
        cache_free_entry(evict_list);
        evict_list = next;
    }
}

/* ============================================================================
 * Metatile Result Lifecycle
 * ============================================================================ */

void ct_metatile_result_free(CTMetatileLabelResult *result)
{
    if (!result) return;
    free(result->labels);
    for (size_t i = 0; i < result->num_roads; i++) {
        free(result->roads[i].glyphs);
    }
    free(result->roads);
    free(result);
}

/* ============================================================================
 * Metatile Label Computation
 * ============================================================================ */

/* Maximum labels/roads per metatile */
#define MT_MAX_LABELS  512
#define MT_MAX_ROADS   120
#define MT_AREA_MAX_CANDIDATES 512
#define MT_AREA_MAX_PER_MT     40

/* Road label constants (same as ct_label.c) */
static const int mt_road_label_min_zoom[] = {
    [CT_ROAD_MOTORWAY]    = 8,
    [CT_ROAD_TRUNK]       = 10,
    [CT_ROAD_PRIMARY]     = 12,
    [CT_ROAD_SECONDARY]   = 13,
    [CT_ROAD_TERTIARY]    = 14,
    [CT_ROAD_RESIDENTIAL] = 16,
    [CT_ROAD_SERVICE]     = 17,
    [CT_ROAD_OTHER]       = 17,
};

static const float mt_road_label_font_size[] = {
    [CT_ROAD_MOTORWAY]    = 10.0f,
    [CT_ROAD_TRUNK]       = 10.0f,
    [CT_ROAD_PRIMARY]     = 10.0f,
    [CT_ROAD_SECONDARY]   = 9.0f,
    [CT_ROAD_TERTIARY]    = 9.0f,
    [CT_ROAD_RESIDENTIAL] = 8.0f,
    [CT_ROAD_SERVICE]     = 8.0f,
    [CT_ROAD_OTHER]       = 8.0f,
};

static const int mt_road_label_priority[] = {
    [CT_ROAD_MOTORWAY]    = 50,
    [CT_ROAD_TRUNK]       = 48,
    [CT_ROAD_PRIMARY]     = 45,
    [CT_ROAD_SECONDARY]   = 40,
    [CT_ROAD_TERTIARY]    = 35,
    [CT_ROAD_RESIDENTIAL] = 20,
    [CT_ROAD_SERVICE]     = 15,
    [CT_ROAD_OTHER]       = 10,
};

#define MT_ROAD_LABEL_MAX_ANGLE  (30.0f * (float)M_PI / 180.0f)

/* Comparison for sorting labels by priority (descending) */
static int mt_compare_labels(const void *a, const void *b)
{
    const CTLabeledPoint *pa = *(const CTLabeledPoint **)a;
    const CTLabeledPoint *pb = *(const CTLabeledPoint **)b;
    if (pa->priority != pb->priority) return pb->priority - pa->priority;
    if (pa->population != pb->population) return pb->population - pa->population;
    if (pa->id < pb->id) return -1;
    if (pa->id > pb->id) return 1;
    return 0;
}

/* Comparison for sorting roads by priority (descending) */
static int mt_compare_roads(const void *a, const void *b)
{
    const CTOSMWay *wa = *(const CTOSMWay **)a;
    const CTOSMWay *wb = *(const CTOSMWay **)b;
    int pa = (wa->feature_type >= 0 && wa->feature_type < CT_ROAD_TYPE_COUNT)
             ? mt_road_label_priority[wa->feature_type] : 0;
    int pb = (wb->feature_type >= 0 && wb->feature_type < CT_ROAD_TYPE_COUNT)
             ? mt_road_label_priority[wb->feature_type] : 0;
    if (pa != pb) return pb - pa;
    if (wa->id < wb->id) return -1;
    if (wa->id > wb->id) return 1;
    return 0;
}

/* Convert geographic coordinates to metatile pixel space */
static inline void geo_to_metatile_pixel(CTMetatileCoord mt, int tile_size,
                                          double lat, double lon,
                                          int *px, int *py)
{
    double n = (double)sh_tiles_per_axis(mt.z);
    *px = (int)((lon + 180.0) / 360.0 * n * tile_size - (double)mt.mx * tile_size);
    double lat_rad = lat * M_PI / 180.0;
    double merc_y = log(tan(lat_rad) + 1.0 / cos(lat_rad));
    *py = (int)((1.0 - merc_y / M_PI) / 2.0 * n * tile_size - (double)mt.my * tile_size);
}

/*
 * Place glyphs along a polyline path (same algorithm as ct_label.c).
 * Returns number of glyphs placed, 0 if path too short/curved.
 */
static int mt_place_glyphs(const float *px, const float *py, int num_points,
                           const char *text, const SHFont *font, float font_size,
                           CTPathGlyph *glyphs, int max_glyphs)
{
    if (num_points < 2 || !text || !font || !glyphs) return 0;

    float total_length = 0;
    for (int i = 0; i < num_points - 1; i++) {
        float dx = px[i + 1] - px[i];
        float dy = py[i + 1] - py[i];
        total_length += sqrtf(dx * dx + dy * dy);
    }

    float text_width = sh_font_text_width(font, text, font_size);
    if (total_length < text_width + font_size) return 0;

    float start_offset = (total_length - text_width) / 2.0f;
    int glyph_count = 0;
    float cursor = start_offset;
    int seg_idx = 0;
    float seg_dist = 0;
    float prev_angle = -999.0f;

    const char *p = text;
    while (*p && glyph_count < max_glyphs) {
        uint32_t codepoint;
        int len = sh_utf8_decode(p, &codepoint);
        if (len == 0 || codepoint == 0) break;
        p += len;

        const SHGlyph *glyph = sh_font_get_glyph(font, codepoint);
        if (!glyph) { cursor += 0.5f * font_size; continue; }

        float advance = glyph->advance * font_size;
        float glyph_center = cursor + advance / 2.0f;

        while (seg_idx < num_points - 2) {
            float dx = px[seg_idx + 1] - px[seg_idx];
            float dy = py[seg_idx + 1] - py[seg_idx];
            float seg_len = sqrtf(dx * dx + dy * dy);
            if (seg_dist + seg_len > glyph_center) break;
            seg_dist += seg_len;
            seg_idx++;
        }
        if (seg_idx >= num_points - 1) break;

        float dx = px[seg_idx + 1] - px[seg_idx];
        float dy = py[seg_idx + 1] - py[seg_idx];
        float seg_len = sqrtf(dx * dx + dy * dy);
        if (seg_len < 0.001f) { cursor += advance; continue; }

        float t = (glyph_center - seg_dist) / seg_len;
        if (t < 0) t = 0;
        if (t > 1) t = 1;

        float gx = px[seg_idx] + dx * t;
        float gy = py[seg_idx] + dy * t;
        float angle = atan2f(dy, dx);

        if (prev_angle > -900.0f) {
            float delta = fabsf(angle - prev_angle);
            if (delta > (float)M_PI) delta = 2.0f * (float)M_PI - delta;
            if (delta > MT_ROAD_LABEL_MAX_ANGLE) return 0;
        }
        prev_angle = angle;

        glyphs[glyph_count].x = gx;
        glyphs[glyph_count].y = gy;
        glyphs[glyph_count].angle = angle;
        glyph_count++;
        cursor += advance;
    }

    return glyph_count;
}

CTMetatileLabelResult *ct_metatile_compute_labels(
    const CTPBFContext *pbf, CTMetatileCoord mt, int tile_size)
{
    if (!pbf) return NULL;

    const SHFont *font = sh_font_get_default();
    if (!font) return NULL;

    int mt_size = tile_size * CT_METATILE_SIZE;  /* 2x tile_size */

    float base_size = ct_label_base_font_size(mt.z, tile_size);

    /* Maximum label pixel extent: country/state labels use 1.6x base font,
     * and long names (~20 chars at ~0.6 advance) can reach this width.
     * Used for both the query overlap buffer and the "too far outside" check. */
    int label_margin = (int)(base_size * 1.6f * 20.0f * 0.6f);
    if (label_margin < 100) label_margin = 100;

    /* Create collision grid extended by label_margin on each side so that
     * overlap-zone labels get full collision coverage.  All placement coords
     * are offset by +label_margin; results are de-offset before packaging. */
    int extended_size = mt_size + 2 * label_margin;
    CTLabelPlacer *placer = ct_label_placer_create(extended_size, extended_size);
    if (!placer) return NULL;

    /* Pre-declare for cleanup path */
    CTRoadLabelPlacement *road_placements = NULL;
    size_t road_count = 0;
    size_t road_cap = 0;

    /* ---- 1. Query labeled points using expanded metatile bbox ---- */
    const CTLabeledPoint **all_points = NULL;
    size_t all_point_count = 0;

    {
        /* Compute metatile bbox from corner sub-tiles */
        CTTileCoord tl = { mt.z, mt.mx, mt.my };
        CTTileCoord br = { mt.z, mt.mx + CT_METATILE_SIZE - 1,
                           mt.my + CT_METATILE_SIZE - 1 };
        int max_coord = (int)sh_tiles_per_axis(mt.z);
        if (br.x >= max_coord) br.x = max_coord - 1;
        if (br.y >= max_coord) br.y = max_coord - 1;
        CTBBox tl_bbox = ct_tile_bounds(tl);
        CTBBox br_bbox = ct_tile_bounds(br);
        CTBBox query_bbox = {
            .min_lat = br_bbox.min_lat, .max_lat = tl_bbox.max_lat,
            .min_lon = tl_bbox.min_lon, .max_lon = br_bbox.max_lon
        };

        /* Overlap buffer sized to max label extent in geographic degrees */
        double deg_per_px_lon = (query_bbox.max_lon - query_bbox.min_lon) / (double)mt_size;
        double deg_per_px_lat = (query_bbox.max_lat - query_bbox.min_lat) / (double)mt_size;
        double buf_lon = (double)label_margin * deg_per_px_lon;
        double buf_lat = (double)label_margin * deg_per_px_lat;
        query_bbox.min_lat -= buf_lat;
        query_bbox.max_lat += buf_lat;
        query_bbox.min_lon -= buf_lon;
        query_bbox.max_lon += buf_lon;

        ct_pbf_get_bbox_labels(pbf, query_bbox, mt.z, &all_points, &all_point_count);
    }

    /* Sort by priority and place point labels */
    if (all_point_count > 0) {
        qsort((void *)all_points, all_point_count, sizeof(CTLabeledPoint *),
              mt_compare_labels);

        for (size_t i = 0; i < all_point_count; i++) {
            const CTLabeledPoint *point = all_points[i];
            int px, py;
            geo_to_metatile_pixel(mt, tile_size, point->coord.lat, point->coord.lon,
                                  &px, &py);

            /* Skip if too far outside metatile (beyond max label extent) */
            if (px < -label_margin || px > mt_size + label_margin ||
                py < -label_margin || py > mt_size + label_margin)
                continue;

            /* Adjust font size based on place type */
            float size = base_size;
            switch (point->type) {
                case CT_PLACE_COUNTRY: case CT_PLACE_STATE:
                    size = base_size * 1.6f; break;
                case CT_PLACE_CITY:
                    size = base_size * 1.3f; break;
                case CT_PLACE_TOWN:
                    size = base_size * 1.0f; break;
                case CT_PLACE_VILLAGE:
                    size = base_size * 0.85f; break;
                case CT_PLACE_HAMLET: case CT_PLACE_LOCALITY:
                    size = base_size * 0.75f; break;
                case CT_PLACE_SUBURB: case CT_PLACE_NEIGHBOURHOOD:
                    size = base_size * 0.8f; break;
                default: break;
            }

            ct_label_place_single(placer, point, px + label_margin, py + label_margin, font, size);
        }
    }
    free(all_points);
    all_points = NULL;

    /* ---- 2. Place area labels ---- */
    if (mt.z >= 10 && pbf->mp_rtree && pbf->num_multipolygons > 0) {
        /* Compute metatile bbox with overlap buffer */
        CTTileCoord tl = { mt.z, mt.mx, mt.my };
        CTTileCoord br = { mt.z, mt.mx + 1, mt.my + 1 };
        int max_coord = (int)sh_tiles_per_axis(mt.z);
        if (br.x >= max_coord) br.x = max_coord - 1;
        if (br.y >= max_coord) br.y = max_coord - 1;
        CTBBox tl_bbox = ct_tile_bounds(tl);
        CTBBox br_bbox = ct_tile_bounds(br);
        CTBBox mt_bbox = {
            .min_lat = br_bbox.min_lat,
            .max_lat = tl_bbox.max_lat,
            .min_lon = tl_bbox.min_lon,
            .max_lon = br_bbox.max_lon
        };

        /* Overlap buffer sized to max label extent */
        double area_deg_per_px_lon = (mt_bbox.max_lon - mt_bbox.min_lon) / (double)mt_size;
        double area_deg_per_px_lat = (mt_bbox.max_lat - mt_bbox.min_lat) / (double)mt_size;
        double area_buf_lon = (double)label_margin * area_deg_per_px_lon;
        double area_buf_lat = (double)label_margin * area_deg_per_px_lat;
        mt_bbox.min_lat -= area_buf_lat;
        mt_bbox.max_lat += area_buf_lat;
        mt_bbox.min_lon -= area_buf_lon;
        mt_bbox.max_lon += area_buf_lon;

        uint32_t *candidates = malloc(MT_AREA_MAX_CANDIDATES * sizeof(uint32_t));
        if (candidates) {
            size_t num_cand = ct_rtree_query(pbf->mp_rtree, mt_bbox, candidates,
                                              MT_AREA_MAX_CANDIDATES);

            double polylabel_precision = 0.01;
            int area_placed = 0;

            for (size_t ci = 0; ci < num_cand && area_placed < MT_AREA_MAX_PER_MT; ci++) {
                uint32_t mp_idx = candidates[ci];
                if (mp_idx >= pbf->num_multipolygons) continue;
                const CTAssembledMultipolygon *mp = &pbf->multipolygons[mp_idx];
                if (!mp->name || mp->name[0] == '\0' || mp->num_rings < 1) continue;

                double pole_x = 0, pole_y = 0, pole_dist = 0;
                const CTCoord **rings = malloc(mp->num_rings * sizeof(CTCoord *));
                int *ring_sizes = malloc(mp->num_rings * sizeof(int));
                if (!rings || !ring_sizes) {
                    free(rings); free(ring_sizes); continue;
                }
                for (int r = 0; r < mp->num_rings; r++) {
                    rings[r] = mp->rings[r].coords;
                    ring_sizes[r] = mp->rings[r].num_coords;
                }
                int found = ct_polylabel_with_holes(rings, ring_sizes, mp->num_rings,
                                                     polylabel_precision,
                                                     &pole_x, &pole_y, &pole_dist);
                free(rings); free(ring_sizes);
                if (!found) continue;

                /* Check if pole is in metatile bbox */
                if (pole_x < mt_bbox.min_lon || pole_x > mt_bbox.max_lon ||
                    pole_y < mt_bbox.min_lat || pole_y > mt_bbox.max_lat)
                    continue;

                float area_size = base_size * 0.85f;
                float text_width = sh_font_text_width(font, mp->name, area_size);

                double deg_per_pixel = (mt_bbox.max_lon - mt_bbox.min_lon) / (double)mt_size;
                if (deg_per_pixel > 0) {
                    double pole_dist_px = pole_dist / deg_per_pixel;
                    if (pole_dist_px * 2.0 < text_width) continue;
                }

                int px, py;
                geo_to_metatile_pixel(mt, tile_size, pole_y, pole_x, &px, &py);
                if (px < -50 || px > mt_size + 50 ||
                    py < -50 || py > mt_size + 50)
                    continue;

                CTLabeledPoint area_point;
                area_point.id = 0;
                area_point.coord.lat = pole_y;
                area_point.coord.lon = pole_x;
                area_point.type = CT_PLACE_UNKNOWN;
                area_point.name = mp->name;
                area_point.population = 0;
                area_point.min_zoom = 10;
                area_point.priority = 5;

                if (ct_label_place_single(placer, &area_point, px + label_margin, py + label_margin, font, area_size)) {
                    /* Fix up: area_point is stack-allocated */
                    CTLabelPlacement *last = &placer->placements[placer->num_placements - 1];
                    last->name = mp->name;
                    last->point = NULL;
                    area_placed++;
                }
            }
            free(candidates);
        }
    }

    /* ---- 3. Query named ways using expanded metatile bbox, place road labels ---- */
    {
        const CTOSMWay **all_ways = NULL;
        size_t all_way_count = 0;

        {
            /* Compute metatile bbox with overlap buffer */
            CTTileCoord tl = { mt.z, mt.mx, mt.my };
            CTTileCoord br = { mt.z, mt.mx + CT_METATILE_SIZE - 1,
                               mt.my + CT_METATILE_SIZE - 1 };
            int max_coord = (int)sh_tiles_per_axis(mt.z);
            if (br.x >= max_coord) br.x = max_coord - 1;
            if (br.y >= max_coord) br.y = max_coord - 1;
            CTBBox tl_bbox = ct_tile_bounds(tl);
            CTBBox br_bbox = ct_tile_bounds(br);
            CTBBox query_bbox = {
                .min_lat = br_bbox.min_lat, .max_lat = tl_bbox.max_lat,
                .min_lon = tl_bbox.min_lon, .max_lon = br_bbox.max_lon
            };

            /* Overlap buffer sized to max label extent */
            double way_deg_per_px_lon = (query_bbox.max_lon - query_bbox.min_lon) / (double)mt_size;
            double way_deg_per_px_lat = (query_bbox.max_lat - query_bbox.min_lat) / (double)mt_size;
            double buf_lon = (double)label_margin * way_deg_per_px_lon;
            double buf_lat = (double)label_margin * way_deg_per_px_lat;
            query_bbox.min_lat -= buf_lat;
            query_bbox.max_lat += buf_lat;
            query_bbox.min_lon -= buf_lon;
            query_bbox.max_lon += buf_lon;

            ct_pbf_get_bbox_named_ways(pbf, query_bbox, &all_ways, &all_way_count);
        }

        if (all_way_count > 0) {
            qsort((void *)all_ways, all_way_count, sizeof(CTOSMWay *),
                  mt_compare_roads);

            /* Precompute Mercator transform for metatile space */
            double merc_n = (double)sh_tiles_per_axis(mt.z);
            double merc_lon_scale = merc_n * tile_size / 360.0;
            double merc_lon_offset = 180.0 * merc_lon_scale - (double)mt.mx * tile_size;
            double merc_lat_scale = -merc_n * tile_size / (2.0 * M_PI);
            double merc_lat_offset = merc_n * tile_size / 2.0 - (double)mt.my * tile_size;

            float *scratch_px = NULL, *scratch_py = NULL;
            size_t scratch_cap = 0;
            CTPathGlyph *scratch_glyphs = NULL;
            size_t glyph_cap = 0;

            for (size_t w = 0; w < all_way_count && road_count < MT_MAX_ROADS; w++) {
                const CTOSMWay *way = all_ways[w];
                int road_type = way->feature_type;
                if (road_type < 0 || road_type >= CT_ROAD_TYPE_COUNT) road_type = CT_ROAD_OTHER;
                if (mt.z < mt_road_label_min_zoom[road_type]) continue;
                if (way->num_coords < 3) continue;

                if ((size_t)way->num_coords > scratch_cap) {
                    scratch_cap = (size_t)way->num_coords * 2;
                    free(scratch_px); free(scratch_py);
                    scratch_px = malloc(scratch_cap * sizeof(float));
                    scratch_py = malloc(scratch_cap * sizeof(float));
                    if (!scratch_px || !scratch_py) break;
                }

                /* Convert to metatile pixel space with Mercator projection */
                for (int i = 0; i < way->num_coords; i++) {
                    double lon = way->coords[i].lon;
                    double lat_rad = way->coords[i].lat * M_PI / 180.0;
                    double merc_y = log(tan(lat_rad) + 1.0 / cos(lat_rad));
                    scratch_px[i] = (float)(lon * merc_lon_scale + merc_lon_offset + label_margin);
                    scratch_py[i] = (float)(merc_y * merc_lat_scale + merc_lat_offset + label_margin);
                }

                /* Reverse if right-to-left */
                if (way->num_coords >= 2 &&
                    scratch_px[way->num_coords - 1] < scratch_px[0]) {
                    for (int i = 0, j = way->num_coords - 1; i < j; i++, j--) {
                        float tmp;
                        tmp = scratch_px[i]; scratch_px[i] = scratch_px[j]; scratch_px[j] = tmp;
                        tmp = scratch_py[i]; scratch_py[i] = scratch_py[j]; scratch_py[j] = tmp;
                    }
                }

                float fsize = mt_road_label_font_size[road_type] * (float)tile_size / 256.0f;
                size_t name_len = strlen(way->name);
                if (name_len > glyph_cap) {
                    glyph_cap = name_len * 2;
                    free(scratch_glyphs);
                    scratch_glyphs = malloc(glyph_cap * sizeof(CTPathGlyph));
                    if (!scratch_glyphs) break;
                }

                int num_glyphs = mt_place_glyphs(
                    scratch_px, scratch_py, way->num_coords,
                    way->name, font, fsize, scratch_glyphs, (int)glyph_cap);
                if (num_glyphs <= 0) continue;

                /* Collision check */
                float line_height = sh_font_line_height(font, fsize);
                int collides = 0;
                for (int g = 0; g < num_glyphs; g++) {
                    float half_w = fsize * 0.4f;
                    float half_h = line_height / 2.0f;
                    int gx = (int)(scratch_glyphs[g].x - half_w);
                    int gy = (int)(scratch_glyphs[g].y - half_h);
                    int gw = (int)(half_w * 2);
                    int gh = (int)(half_h * 2);
                    if (ct_collision_test_padded(placer->collision, gx, gy, gw, gh,
                                                 placer->padding_x, placer->padding_y)) {
                        collides = 1; break;
                    }
                }
                if (collides) continue;

                /* Allocate glyph copy */
                CTPathGlyph *glyph_copy = malloc(num_glyphs * sizeof(CTPathGlyph));
                if (!glyph_copy) continue;
                memcpy(glyph_copy, scratch_glyphs, num_glyphs * sizeof(CTPathGlyph));

                /* Mark collision grid */
                for (int g = 0; g < num_glyphs; g++) {
                    float half_w = fsize * 0.4f;
                    float half_h = line_height / 2.0f;
                    int gx = (int)(scratch_glyphs[g].x - half_w);
                    int gy = (int)(scratch_glyphs[g].y - half_h);
                    int gw = (int)(half_w * 2);
                    int gh = (int)(half_h * 2);
                    ct_collision_mark_padded(placer->collision, gx, gy, gw, gh,
                                             placer->padding_x, placer->padding_y);
                }

                /* Grow road placements array */
                if (road_count >= road_cap) {
                    size_t new_cap = road_cap ? road_cap * 2 : 64;
                    CTRoadLabelPlacement *grown = realloc(road_placements,
                        new_cap * sizeof(CTRoadLabelPlacement));
                    if (!grown) { free(glyph_copy); break; }
                    road_placements = grown;
                    road_cap = new_cap;
                }

                CTRoadLabelPlacement *rp = &road_placements[road_count];
                rp->name = way->name;
                rp->num_glyphs = num_glyphs;
                rp->font_size = fsize;
                rp->priority = mt_road_label_priority[road_type];
                rp->glyphs = glyph_copy;
                road_count++;
            }

            free(scratch_px);
            free(scratch_py);
            free(scratch_glyphs);
        }

        free(all_ways);
    }

    /* ---- Package result ---- */
    CTMetatileLabelResult *result = calloc(1, sizeof(*result));
    if (!result) goto cleanup;

    result->coord = mt;
    result->tile_size = tile_size;

    /* Copy label placements from placer */
    if (placer->num_placements > 0) {
        result->labels = malloc(placer->num_placements * sizeof(CTLabelPlacement));
        if (result->labels) {
            memcpy(result->labels, placer->placements,
                   placer->num_placements * sizeof(CTLabelPlacement));
            result->num_labels = placer->num_placements;
        }
    }

    /* De-offset: convert from extended grid coords back to metatile space */
    for (size_t i = 0; i < result->num_labels; i++) {
        result->labels[i].x -= label_margin;
        result->labels[i].y -= label_margin;
    }

    result->roads = road_placements;
    result->num_roads = road_count;
    road_placements = NULL;  /* Ownership transferred */

    for (size_t i = 0; i < result->num_roads; i++) {
        for (int g = 0; g < result->roads[i].num_glyphs; g++) {
            result->roads[i].glyphs[g].x -= (float)label_margin;
            result->roads[i].glyphs[g].y -= (float)label_margin;
        }
    }

    ct_label_placer_free(placer);
    return result;

cleanup:
    free(all_points);
    /* Free any road placements we allocated */
    if (road_placements) {
        for (size_t i = 0; i < road_count; i++) {
            free(road_placements[i].glyphs);
        }
        free(road_placements);
    }
    ct_label_placer_free(placer);
    return NULL;
}

/* ============================================================================
 * Sub-tile Extraction
 * ============================================================================ */

void ct_metatile_extract_subtile(
    const CTMetatileLabelResult *result,
    int sx, int sy,
    CTLabelPlacer *placer,
    CTRoadLabelPlacement **out_roads, size_t *out_road_count)
{
    if (out_roads) *out_roads = NULL;
    if (out_road_count) *out_road_count = 0;
    if (!result || !placer) return;

    int tile_size = result->tile_size;
    int ox = sx * tile_size;
    int oy = sy * tile_size;

    /* ---- Extract point/area labels ---- */
    for (size_t i = 0; i < result->num_labels; i++) {
        const CTLabelPlacement *lp = &result->labels[i];

        /* Check if label bbox intersects sub-tile */
        int lx2 = lp->x + lp->width;
        int ly2 = lp->y + lp->height;

        if (lx2 < ox || lp->x >= ox + tile_size ||
            ly2 < oy || lp->y >= oy + tile_size)
            continue;

        /* Grow placer array if needed */
        if (placer->num_placements >= placer->placements_capacity) {
            size_t new_cap = placer->placements_capacity ? placer->placements_capacity * 2 : 64;
            CTLabelPlacement *new_arr = realloc(placer->placements,
                                                new_cap * sizeof(CTLabelPlacement));
            if (!new_arr) continue;
            placer->placements = new_arr;
            placer->placements_capacity = new_cap;
        }

        CTLabelPlacement *dst = &placer->placements[placer->num_placements++];
        *dst = *lp;
        dst->x -= ox;
        dst->y -= oy;
    }

    /* ---- Extract road labels ---- */
    if (!out_roads || !out_road_count || result->num_roads == 0) return;

    size_t road_cap = 32;
    CTRoadLabelPlacement *roads = malloc(road_cap * sizeof(CTRoadLabelPlacement));
    if (!roads) return;
    size_t road_count = 0;

    for (size_t i = 0; i < result->num_roads; i++) {
        const CTRoadLabelPlacement *rp = &result->roads[i];
        if (!rp->glyphs || rp->num_glyphs <= 0) continue;

        /* Check if any glyph is within sub-tile (with font_size margin) */
        float margin = rp->font_size;
        int visible = 0;
        for (int g = 0; g < rp->num_glyphs; g++) {
            float gx = rp->glyphs[g].x;
            float gy = rp->glyphs[g].y;
            if (gx + margin >= ox && gx - margin < ox + tile_size &&
                gy + margin >= oy && gy - margin < oy + tile_size) {
                visible = 1;
                break;
            }
        }
        if (!visible) continue;

        /* Copy glyphs with offset */
        CTPathGlyph *glyphs = malloc(rp->num_glyphs * sizeof(CTPathGlyph));
        if (!glyphs) continue;
        for (int g = 0; g < rp->num_glyphs; g++) {
            glyphs[g].x = rp->glyphs[g].x - (float)ox;
            glyphs[g].y = rp->glyphs[g].y - (float)oy;
            glyphs[g].angle = rp->glyphs[g].angle;
        }

        if (road_count >= road_cap) {
            road_cap *= 2;
            CTRoadLabelPlacement *grown = realloc(roads,
                road_cap * sizeof(CTRoadLabelPlacement));
            if (!grown) { free(glyphs); break; }
            roads = grown;
        }

        roads[road_count].name = rp->name;
        roads[road_count].glyphs = glyphs;
        roads[road_count].num_glyphs = rp->num_glyphs;
        roads[road_count].font_size = rp->font_size;
        roads[road_count].priority = rp->priority;
        road_count++;
    }

    if (road_count == 0) {
        free(roads);
    } else {
        *out_roads = roads;
        *out_road_count = road_count;
    }
}
