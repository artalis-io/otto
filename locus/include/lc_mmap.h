/*
 * lc_mmap.h - Zero-Copy mmap'd Index Access
 *
 * Provides inline accessors for directly accessing entity data
 * from memory-mapped binary index files (v4 format).
 */

#ifndef LC_MMAP_H
#define LC_MMAP_H

#include <stdint.h>
#include <stddef.h>
#include "sh_geo.h"
#include "lc_types.h"
#include "lc_trie.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Binary Format Structures (v4)
 * ============================================================================ */

#define LC_BINARY_VERSION_V4 4
#define LC_MMAP_NULL_OFFSET 0xFFFFFFFF

/* Header - 96 bytes (extended from v3's 64 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t entity_count;
    uint32_t string_pool_size;
    uint32_t trie_node_count;
    uint32_t trie_entity_count;
    uint32_t grid_width;
    uint32_t grid_height;
    double min_lat;
    double min_lon;
    double max_lat;
    double max_lon;
    double grid_cell_size;
    /* v4 additions */
    uint32_t ngram_entry_count;
    uint32_t ngram_entity_count;
    uint32_t geometry_count;
    uint32_t geometry_point_count;
} LCBinaryHeaderV4;

/* Section offsets - 96 bytes (extended from v3's 64 bytes) */
typedef struct __attribute__((packed)) {
    uint64_t entities_offset;
    uint64_t alt_names_offset;
    uint64_t string_pool_offset;
    uint64_t trie_nodes_offset;
    uint64_t trie_entities_offset;
    uint64_t grid_cells_offset;
    uint64_t grid_entities_offset;
    /* v4 additions */
    uint64_t ngram_entries_offset;
    uint64_t ngram_entities_offset;
    uint64_t geometry_offsets_offset;
    uint64_t geometry_points_offset;
    uint64_t _padding;
} LCSectionOffsetsV4;

/* Entity record - 72 bytes (extended with geometry_offset) */
typedef struct __attribute__((packed)) {
    uint64_t osm_id;
    uint8_t type;
    uint8_t fclass;
    int8_t admin_level;
    uint8_t num_alt_names;
    int32_t population;
    double lat;
    double lon;
    uint32_t name_offset;
    uint32_t poi_type_offset;
    uint32_t alt_names_offset;
    uint32_t housenumber_offset;
    uint32_t street_offset;
    uint32_t city_offset;
    uint32_t postcode_offset;
    uint32_t state_offset;
    uint32_t country_offset;
    uint32_t country_code_offset;
    uint32_t geometry_offset;  /* Index into geometry offsets array, NULL_OFFSET if none */
} LCBinaryEntityV4;

/* Serialized trie node - 160 bytes (same as v3) */
typedef struct __attribute__((packed)) {
    uint32_t children[LC_TRIE_ALPHABET_SIZE];
    uint32_t entity_offset;
    uint16_t entity_count;
    uint16_t _padding;
} LCBinaryTrieNodeV4;

/* N-gram entry - 12 bytes */
typedef struct __attribute__((packed)) {
    uint32_t trigram;        /* 3 chars packed into lower 24 bits */
    uint32_t entity_offset;  /* Offset into ngram entity array */
    uint16_t entity_count;
    uint16_t _padding;
} LCBinaryNgramEntry;

/* Geometry entry - 12 bytes */
typedef struct __attribute__((packed)) {
    uint32_t entity_id;
    uint32_t point_offset;   /* Offset into geometry_points array */
    uint32_t point_count;
} LCBinaryGeometry;

/* Geometry point - 8 bytes (fixed-point int32) */
typedef struct __attribute__((packed)) {
    int32_t lat_e7;          /* latitude * 1e7 */
    int32_t lon_e7;          /* longitude * 1e7 */
} LCBinaryPoint;

/* ============================================================================
 * mmap'd Index Structure
 * ============================================================================ */

struct LCMmapIndex {
    /* Memory mapping */
    void *map_base;
    size_t map_size;
    /*
     * 1 when map_base is a file mapping this index must release, 0 when it
     * points at a caller-owned buffer (the WASM path).
     *
     * This used to be inferred from `fd >= 0`, which worked only because a
     * file-backed index also held a descriptor. sh_map_file_readonly() closes
     * the descriptor once the mapping exists, so ownership needs saying
     * outright.
     */
    int owns_map;
    int fd;   /* legacy descriptor; -1 when there is none to close */

    /* Direct pointers into mmap */
    const LCBinaryHeaderV4 *header;
    const LCBinaryEntityV4 *entities;
    const char *string_pool;
    const uint32_t *alt_name_offsets;

    /* Trie */
    const LCBinaryTrieNodeV4 *trie_nodes;
    const uint32_t *trie_entity_ids;

    /* Grid */
    const uint32_t *grid_cell_offsets;
    const uint32_t *grid_entity_ids;

    /* N-gram */
    const LCBinaryNgramEntry *ngram_entries;
    const uint32_t *ngram_entity_ids;

    /* Geometry */
    const LCBinaryGeometry *geometry_offsets;
    const LCBinaryPoint *geometry_points;
};

/* ============================================================================
 * Inline Entity Accessors
 * ============================================================================ */

static inline const char *lc_mmap_get_string(const LCMmapIndex *idx, uint32_t offset) {
    if (offset == LC_MMAP_NULL_OFFSET) return NULL;
    return idx->string_pool + offset;
}

static inline const char *lc_mmap_entity_name(const LCMmapIndex *idx, uint32_t i) {
    return lc_mmap_get_string(idx, idx->entities[i].name_offset);
}

static inline SHCoord lc_mmap_entity_centroid(const LCMmapIndex *idx, uint32_t i) {
    return (SHCoord){ .lat = idx->entities[i].lat, .lon = idx->entities[i].lon };
}

static inline LCFeatureClass lc_mmap_entity_fclass(const LCMmapIndex *idx, uint32_t i) {
    return (LCFeatureClass)idx->entities[i].fclass;
}

static inline LCEntityType lc_mmap_entity_type(const LCMmapIndex *idx, uint32_t i) {
    return (LCEntityType)idx->entities[i].type;
}

static inline uint64_t lc_mmap_entity_osm_id(const LCMmapIndex *idx, uint32_t i) {
    return idx->entities[i].osm_id;
}

static inline int8_t lc_mmap_entity_admin_level(const LCMmapIndex *idx, uint32_t i) {
    return idx->entities[i].admin_level;
}

static inline int32_t lc_mmap_entity_population(const LCMmapIndex *idx, uint32_t i) {
    return idx->entities[i].population;
}

static inline const char *lc_mmap_entity_poi_type(const LCMmapIndex *idx, uint32_t i) {
    return lc_mmap_get_string(idx, idx->entities[i].poi_type_offset);
}

/* Address accessors */
static inline const char *lc_mmap_entity_housenumber(const LCMmapIndex *idx, uint32_t i) {
    return lc_mmap_get_string(idx, idx->entities[i].housenumber_offset);
}

static inline const char *lc_mmap_entity_street(const LCMmapIndex *idx, uint32_t i) {
    return lc_mmap_get_string(idx, idx->entities[i].street_offset);
}

static inline const char *lc_mmap_entity_city(const LCMmapIndex *idx, uint32_t i) {
    return lc_mmap_get_string(idx, idx->entities[i].city_offset);
}

static inline const char *lc_mmap_entity_postcode(const LCMmapIndex *idx, uint32_t i) {
    return lc_mmap_get_string(idx, idx->entities[i].postcode_offset);
}

static inline const char *lc_mmap_entity_state(const LCMmapIndex *idx, uint32_t i) {
    return lc_mmap_get_string(idx, idx->entities[i].state_offset);
}

static inline const char *lc_mmap_entity_country(const LCMmapIndex *idx, uint32_t i) {
    return lc_mmap_get_string(idx, idx->entities[i].country_offset);
}

static inline const char *lc_mmap_entity_country_code(const LCMmapIndex *idx, uint32_t i) {
    return lc_mmap_get_string(idx, idx->entities[i].country_code_offset);
}

/* Alt names */
static inline uint8_t lc_mmap_entity_num_alt_names(const LCMmapIndex *idx, uint32_t i) {
    return idx->entities[i].num_alt_names;
}

static inline const char *lc_mmap_entity_alt_name(const LCMmapIndex *idx, uint32_t entity_idx, uint8_t alt_idx) {
    const LCBinaryEntityV4 *e = &idx->entities[entity_idx];
    if (alt_idx >= e->num_alt_names) return NULL;
    uint32_t str_offset = idx->alt_name_offsets[e->alt_names_offset + alt_idx];
    return lc_mmap_get_string(idx, str_offset);
}

/* ============================================================================
 * Inline Geometry Accessors
 * ============================================================================ */

static inline int lc_mmap_entity_has_geometry(const LCMmapIndex *idx, uint32_t i) {
    return idx->entities[i].geometry_offset != LC_MMAP_NULL_OFFSET;
}

static inline uint32_t lc_mmap_geometry_point_count(const LCMmapIndex *idx, uint32_t entity_idx) {
    uint32_t geom_idx = idx->entities[entity_idx].geometry_offset;
    if (geom_idx == LC_MMAP_NULL_OFFSET) return 0;
    return idx->geometry_offsets[geom_idx].point_count;
}

static inline SHCoord lc_mmap_geometry_point(const LCMmapIndex *idx, uint32_t entity_idx, uint32_t pt_idx) {
    uint32_t geom_idx = idx->entities[entity_idx].geometry_offset;
    const LCBinaryGeometry *geom = &idx->geometry_offsets[geom_idx];
    const LCBinaryPoint *pt = &idx->geometry_points[geom->point_offset + pt_idx];
    return (SHCoord){ .lat = pt->lat_e7 / 1e7, .lon = pt->lon_e7 / 1e7 };
}

/* ============================================================================
 * N-gram Utilities
 * ============================================================================ */

/* Pack 3 ASCII characters into a 24-bit integer */
static inline uint32_t lc_pack_trigram(const char *trigram) {
    return ((uint32_t)(uint8_t)trigram[0] << 16) |
           ((uint32_t)(uint8_t)trigram[1] << 8) |
           ((uint32_t)(uint8_t)trigram[2]);
}

/* Unpack a 24-bit integer back to 3 characters */
static inline void lc_unpack_trigram(uint32_t packed, char *out) {
    out[0] = (char)((packed >> 16) & 0xFF);
    out[1] = (char)((packed >> 8) & 0xFF);
    out[2] = (char)(packed & 0xFF);
    out[3] = '\0';
}

/* ============================================================================
 * Geometry Distance (mmap version)
 * ============================================================================ */

/*
 * Calculate distance from point to mmap'd entity geometry.
 * For streets with geometry, returns minimum distance to any segment.
 * For other entities, returns haversine to centroid.
 */
double lc_mmap_point_to_entity_distance(const LCMmapIndex *idx, uint32_t entity_idx, SHCoord point);

#ifdef __cplusplus
}
#endif

#endif /* LC_MMAP_H */
