/*
 * ct_serialize.c - Binary Index Serialization with mmap Support
 *
 * Binary format v5:
 *
 * [Header] (128 bytes)
 *   magic: u32 (0x43525441 = "CRTA")
 *   version: u32 (5)
 *   num_ways: u32
 *   total_coords: u32
 *   string_pool_size: u32
 *   rtree_num_nodes: u32
 *   rtree_num_entries: u32
 *   rtree_root_idx: u32
 *   num_labeled_points: u32
 *   num_multipolygons: u32
 *   total_mp_rings: u32
 *   total_mp_coords: u32
 *   mp_rtree_num_nodes: u32
 *   mp_rtree_num_entries: u32
 *   mp_rtree_root_idx: u32
 *   num_boundaries: u32
 *   total_boundary_coords: u32
 *   boundary_rtree_num_nodes: u32
 *   boundary_rtree_num_entries: u32
 *   boundary_rtree_root_idx: u32
 *   _reserved: u32[4]
 *   bbox: 4 x f64 (min_lat, min_lon, max_lat, max_lon)
 *
 * [Section Offsets] (128 bytes)
 *   ways_offset: u64
 *   coords_offset: u64
 *   string_pool_offset: u64
 *   rtree_nodes_offset: u64
 *   rtree_leaf_indices_offset: u64
 *   labeled_points_offset: u64
 *   multipolygons_offset: u64
 *   mp_rings_offset: u64
 *   mp_coords_offset: u64
 *   mp_rtree_nodes_offset: u64
 *   mp_rtree_leaf_indices_offset: u64
 *   boundaries_offset: u64
 *   boundary_coords_offset: u64
 *   boundary_rtree_nodes_offset: u64
 *   boundary_rtree_leaf_indices_offset: u64
 *   _padding: u64
 *
 * [Way Records] - 32 bytes each
 * [Coordinates] - 8 bytes each (int32 lat_e7, lon_e7)
 * [String Pool] - null-terminated strings
 * [R-Tree Nodes] - CTPackedNode array
 * [R-Tree Leaf Indices] - u32 array
 * [Labeled Points] - 32 bytes each
 * [Multipolygons] - 40 bytes each
 * [Multipolygon Rings] - 12 bytes each
 * [Multipolygon Coordinates] - 8 bytes each
 * [Multipolygon R-Tree Nodes] - CTPackedNode array
 * [Multipolygon R-Tree Leaf Indices] - u32 array
 * [Boundaries] - 32 bytes each
 * [Boundary Coordinates] - 8 bytes each
 * [Boundary R-Tree Nodes] - CTPackedNode array
 * [Boundary R-Tree Leaf Indices] - u32 array
 */

#include "ct_serialize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sh_pal.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define NULL_OFFSET 0xFFFFFFFF

/* ============================================================================
 * Binary Format Structures
 * ============================================================================ */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t num_ways;
    uint32_t total_coords;
    uint32_t string_pool_size;
    uint32_t rtree_num_nodes;
    uint32_t rtree_num_entries;
    uint32_t rtree_root_idx;
    uint32_t num_labeled_points;
    uint32_t num_multipolygons;
    uint32_t total_mp_rings;
    uint32_t total_mp_coords;
    uint32_t mp_rtree_num_nodes;
    uint32_t mp_rtree_num_entries;
    uint32_t mp_rtree_root_idx;
    uint32_t num_boundaries;           /* v5: boundary relations */
    uint32_t total_boundary_coords;    /* v5 */
    uint32_t boundary_rtree_num_nodes; /* v5 */
    uint32_t boundary_rtree_num_entries; /* v5 */
    uint32_t boundary_rtree_root_idx;  /* v5 */
    uint32_t _reserved[4];
    double min_lat;
    double min_lon;
    double max_lat;
    double max_lon;
} CTBinaryHeader;

typedef struct __attribute__((packed)) {
    uint64_t ways_offset;
    uint64_t coords_offset;
    uint64_t string_pool_offset;
    uint64_t rtree_nodes_offset;
    uint64_t rtree_leaf_indices_offset;
    uint64_t labeled_points_offset;
    uint64_t multipolygons_offset;
    uint64_t mp_rings_offset;
    uint64_t mp_coords_offset;
    uint64_t mp_rtree_nodes_offset;
    uint64_t mp_rtree_leaf_indices_offset;
    uint64_t boundaries_offset;               /* v5 */
    uint64_t boundary_coords_offset;          /* v5 */
    uint64_t boundary_rtree_nodes_offset;     /* v5 */
    uint64_t boundary_rtree_leaf_indices_offset; /* v5 */
    uint64_t _padding;
} CTSectionOffsets;

/* Way record - 32 bytes */
typedef struct __attribute__((packed)) {
    int64_t id;
    uint32_t coord_offset;     /* Offset into coords array */
    uint16_t num_coords;
    uint8_t feature_class;
    uint8_t feature_type;
    uint8_t is_area;
    uint8_t min_zoom;
    uint16_t _padding;
    uint32_t name_offset;      /* Offset into string pool, NULL_OFFSET if none */
    float area_sqm;
    float length_m;
} CTBinaryWay;

/* Coordinate - 8 bytes */
typedef struct __attribute__((packed)) {
    int32_t lat_e7;
    int32_t lon_e7;
} CTBinaryCoord;

/* Labeled point - 32 bytes */
typedef struct __attribute__((packed)) {
    int64_t id;
    int32_t lat_e7;
    int32_t lon_e7;
    uint32_t name_offset;      /* Offset into string pool */
    int32_t population;
    uint8_t type;              /* CTPlaceType */
    uint8_t min_zoom;
    uint8_t priority;
    uint8_t _padding;
} CTBinaryLabeledPoint;

/* Multipolygon record - 40 bytes */
typedef struct __attribute__((packed)) {
    uint32_t ring_offset;      /* Offset into rings array */
    uint16_t num_rings;
    uint8_t feature_class;
    uint8_t feature_type;
    uint32_t name_offset;      /* Offset into string pool */
    float area_sqm;
    float min_lat;
    float min_lon;
    float max_lat;
    float max_lon;
    uint32_t _padding;
} CTBinaryMultipolygon;

/* Multipolygon ring - 12 bytes */
typedef struct __attribute__((packed)) {
    uint32_t coord_offset;     /* Offset into mp_coords array */
    uint32_t num_coords;
    uint8_t is_outer;
    uint8_t _padding[3];
} CTBinaryRing;

/* Boundary record - 32 bytes */
typedef struct __attribute__((packed)) {
    int64_t relation_id;
    uint32_t coord_offset;     /* Offset into boundary coords array */
    uint32_t num_coords;
    uint32_t name_offset;      /* Offset into string pool */
    uint8_t boundary_type;     /* CTBoundaryType */
    uint8_t admin_level;       /* 2=country, 4=state, 6=county, etc. */
    uint8_t _padding[2];
    float length_m;
} CTBinaryBoundary;

/* ============================================================================
 * String Pool
 * ============================================================================ */

/* String pool constants */
#define STRING_POOL_INITIAL_CAPACITY  (1024 * 1024)   /* 1MB initial */
#define STRING_POOL_MAX_STRING_LEN    (64 * 1024)     /* 64KB max string */

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} StringPool;

static int string_pool_init(StringPool *pool) {
    pool->data = malloc(STRING_POOL_INITIAL_CAPACITY);
    if (!pool->data) {
        pool->size = 0;
        pool->capacity = 0;
        return 0;
    }
    pool->size = 1;  /* Reserve 0 for NULL strings - offset 0 means empty string */
    pool->capacity = STRING_POOL_INITIAL_CAPACITY;
    pool->data[0] = '\0';
    return 1;
}

static uint32_t string_pool_add(StringPool *pool, const char *str) {
    if (!str || !str[0]) return 0;  /* Empty string at offset 0 */

    /* Bounded strlen to prevent reading past buffer on malformed input */
    size_t len = 0;
    while (len < STRING_POOL_MAX_STRING_LEN && str[len] != '\0') {
        len++;
    }
    if (len >= STRING_POOL_MAX_STRING_LEN) {
        len = STRING_POOL_MAX_STRING_LEN - 1;  /* Truncate */
    }
    len++;  /* Include null terminator */

    if (pool->size + len > pool->capacity) {
        if (pool->capacity > SIZE_MAX / 2) return 0;  /* Overflow guard */
        size_t new_capacity = pool->capacity * 2;
        if (new_capacity < pool->size + len) new_capacity = pool->size + len;
        char *new_data = realloc(pool->data, new_capacity);
        if (!new_data) return 0;  /* OOM: return empty string offset */
        pool->data = new_data;
        pool->capacity = new_capacity;
    }

    uint32_t offset = (uint32_t)pool->size;
    memcpy(pool->data + pool->size, str, len - 1);
    pool->data[pool->size + len - 1] = '\0';  /* Ensure null termination */
    pool->size += len;
    return offset;
}

static void string_pool_free(StringPool *pool) {
    free(pool->data);
}

/* ============================================================================
 * Save Index
 * ============================================================================ */

CTStatus ct_index_save(const CTPBFContext *ctx, const char *path) {
    if (!ctx || !path) return CT_ERROR_INVALID_ARGUMENT;

    FILE *f = fopen(path, "wb");
    if (!f) return CT_ERROR_FILE_WRITE;

    /* Build string pool */
    StringPool strings;
    if (!string_pool_init(&strings)) {
        fclose(f);
        return CT_ERROR_OUT_OF_MEMORY;
    }

    /* Count total coordinates for ways */
    size_t total_coords = 0;
    for (size_t i = 0; i < ctx->num_ways; i++) {
        total_coords += ctx->ways[i].num_coords;
    }

    /* Count total rings and coordinates for multipolygons */
    size_t total_mp_rings = 0;
    size_t total_mp_coords = 0;
    for (size_t i = 0; i < ctx->num_multipolygons; i++) {
        total_mp_rings += ctx->multipolygons[i].num_rings;
        for (int r = 0; r < ctx->multipolygons[i].num_rings; r++) {
            total_mp_coords += ctx->multipolygons[i].rings[r].num_coords;
        }
    }

    /* Count total coordinates for boundaries */
    size_t total_boundary_coords = 0;
    for (size_t i = 0; i < ctx->num_boundaries; i++) {
        total_boundary_coords += ctx->boundaries[i].num_coords;
    }

    /* Calculate sizes */
    size_t header_size = sizeof(CTBinaryHeader) + sizeof(CTSectionOffsets);
    size_t ways_size = ctx->num_ways * sizeof(CTBinaryWay);
    size_t coords_size = total_coords * sizeof(CTBinaryCoord);
    size_t labeled_points_size = ctx->num_labeled_points * sizeof(CTBinaryLabeledPoint);
    size_t multipolygons_size = ctx->num_multipolygons * sizeof(CTBinaryMultipolygon);
    size_t mp_rings_size = total_mp_rings * sizeof(CTBinaryRing);
    size_t mp_coords_size = total_mp_coords * sizeof(CTBinaryCoord);
    size_t boundaries_size = ctx->num_boundaries * sizeof(CTBinaryBoundary);
    size_t boundary_coords_size = total_boundary_coords * sizeof(CTBinaryCoord);

    /* First pass: build string pool to get its size */
    uint32_t *name_offsets = malloc(ctx->num_ways * sizeof(uint32_t));
    if (!name_offsets && ctx->num_ways > 0) {
        string_pool_free(&strings);
        fclose(f);
        return CT_ERROR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < ctx->num_ways; i++) {
        name_offsets[i] = string_pool_add(&strings, ctx->ways[i].name);
    }

    /* Add labeled point names to string pool */
    uint32_t *label_name_offsets = NULL;
    if (ctx->num_labeled_points > 0) {
        label_name_offsets = malloc(ctx->num_labeled_points * sizeof(uint32_t));
        if (!label_name_offsets) {
            free(name_offsets);
            string_pool_free(&strings);
            fclose(f);
            return CT_ERROR_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < ctx->num_labeled_points; i++) {
            label_name_offsets[i] = string_pool_add(&strings, ctx->labeled_points[i].name);
        }
    }

    /* Add multipolygon names to string pool */
    uint32_t *mp_name_offsets = NULL;
    if (ctx->num_multipolygons > 0) {
        mp_name_offsets = malloc(ctx->num_multipolygons * sizeof(uint32_t));
        if (!mp_name_offsets) {
            free(name_offsets);
            free(label_name_offsets);
            string_pool_free(&strings);
            fclose(f);
            return CT_ERROR_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < ctx->num_multipolygons; i++) {
            mp_name_offsets[i] = string_pool_add(&strings, ctx->multipolygons[i].name);
        }
    }

    /* Add boundary names to string pool */
    uint32_t *boundary_name_offsets = NULL;
    if (ctx->num_boundaries > 0) {
        boundary_name_offsets = malloc(ctx->num_boundaries * sizeof(uint32_t));
        if (!boundary_name_offsets) {
            free(name_offsets);
            free(label_name_offsets);
            free(mp_name_offsets);
            string_pool_free(&strings);
            fclose(f);
            return CT_ERROR_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < ctx->num_boundaries; i++) {
            boundary_name_offsets[i] = string_pool_add(&strings, ctx->boundaries[i].name);
        }
    }

    size_t string_pool_size = strings.size;
    size_t rtree_nodes_size = ctx->rtree ? ctx->rtree->num_nodes * sizeof(CTPackedNode) : 0;
    size_t rtree_leaf_size = ctx->rtree ? ctx->rtree->num_entries * sizeof(uint32_t) : 0;
    size_t mp_rtree_nodes_size = ctx->mp_rtree ? ctx->mp_rtree->num_nodes * sizeof(CTPackedNode) : 0;
    size_t mp_rtree_leaf_size = ctx->mp_rtree ? ctx->mp_rtree->num_entries * sizeof(uint32_t) : 0;
    size_t boundary_rtree_nodes_size = ctx->boundary_rtree ? ctx->boundary_rtree->num_nodes * sizeof(CTPackedNode) : 0;
    (void)boundary_rtree_nodes_size;  /* Reserved for future boundary R-tree serialization */

    /* Write header */
    CTBinaryHeader header = {
        .magic = CT_BINARY_MAGIC,
        .version = CT_BINARY_VERSION,
        .num_ways = (uint32_t)ctx->num_ways,
        .total_coords = (uint32_t)total_coords,
        .string_pool_size = (uint32_t)string_pool_size,
        .rtree_num_nodes = ctx->rtree ? (uint32_t)ctx->rtree->num_nodes : 0,
        .rtree_num_entries = ctx->rtree ? (uint32_t)ctx->rtree->num_entries : 0,
        .rtree_root_idx = ctx->rtree ? ctx->rtree->root_idx : 0,
        .num_labeled_points = (uint32_t)ctx->num_labeled_points,
        .num_multipolygons = (uint32_t)ctx->num_multipolygons,
        .total_mp_rings = (uint32_t)total_mp_rings,
        .total_mp_coords = (uint32_t)total_mp_coords,
        .mp_rtree_num_nodes = ctx->mp_rtree ? (uint32_t)ctx->mp_rtree->num_nodes : 0,
        .mp_rtree_num_entries = ctx->mp_rtree ? (uint32_t)ctx->mp_rtree->num_entries : 0,
        .mp_rtree_root_idx = ctx->mp_rtree ? ctx->mp_rtree->root_idx : 0,
        .num_boundaries = (uint32_t)ctx->num_boundaries,
        .total_boundary_coords = (uint32_t)total_boundary_coords,
        .boundary_rtree_num_nodes = ctx->boundary_rtree ? (uint32_t)ctx->boundary_rtree->num_nodes : 0,
        .boundary_rtree_num_entries = ctx->boundary_rtree ? (uint32_t)ctx->boundary_rtree->num_entries : 0,
        .boundary_rtree_root_idx = ctx->boundary_rtree ? ctx->boundary_rtree->root_idx : 0,
        ._reserved = {0, 0, 0, 0},
        .min_lat = ctx->bbox.min_lat,
        .min_lon = ctx->bbox.min_lon,
        .max_lat = ctx->bbox.max_lat,
        .max_lon = ctx->bbox.max_lon
    };
    int write_ok = 1;
    write_ok = write_ok && (fwrite(&header, sizeof(header), 1, f) == 1);

    /* Calculate cumulative offsets */
    size_t offset = header_size;
    size_t ways_offset = offset; offset += ways_size;
    size_t coords_offset = offset; offset += coords_size;
    size_t string_pool_offset = offset; offset += string_pool_size;
    size_t rtree_nodes_offset = offset; offset += rtree_nodes_size;
    size_t rtree_leaf_offset = offset; offset += rtree_leaf_size;
    size_t labeled_points_offset = offset; offset += labeled_points_size;
    size_t multipolygons_offset = offset; offset += multipolygons_size;
    size_t mp_rings_offset = offset; offset += mp_rings_size;
    size_t mp_coords_offset = offset; offset += mp_coords_size;
    size_t mp_rtree_nodes_offset = offset; offset += mp_rtree_nodes_size;
    size_t mp_rtree_leaf_offset = offset; offset += mp_rtree_leaf_size;
    size_t boundaries_offset = offset; offset += boundaries_size;
    size_t boundary_coords_offset = offset; offset += boundary_coords_size;
    size_t boundary_rtree_nodes_offset = offset; offset += boundary_rtree_nodes_size;
    size_t boundary_rtree_leaf_offset = offset;

    /* Write section offsets */
    CTSectionOffsets offsets = {
        .ways_offset = ways_offset,
        .coords_offset = coords_offset,
        .string_pool_offset = string_pool_offset,
        .rtree_nodes_offset = rtree_nodes_offset,
        .rtree_leaf_indices_offset = rtree_leaf_offset,
        .labeled_points_offset = labeled_points_offset,
        .multipolygons_offset = multipolygons_offset,
        .mp_rings_offset = mp_rings_offset,
        .mp_coords_offset = mp_coords_offset,
        .mp_rtree_nodes_offset = mp_rtree_nodes_offset,
        .mp_rtree_leaf_indices_offset = mp_rtree_leaf_offset,
        .boundaries_offset = boundaries_offset,
        .boundary_coords_offset = boundary_coords_offset,
        .boundary_rtree_nodes_offset = boundary_rtree_nodes_offset,
        .boundary_rtree_leaf_indices_offset = boundary_rtree_leaf_offset,
        ._padding = 0
    };
    write_ok = write_ok && (fwrite(&offsets, sizeof(offsets), 1, f) == 1);

    /* Write ways */
    size_t coord_offset = 0;
    for (size_t i = 0; i < ctx->num_ways; i++) {
        const CTOSMWay *way = &ctx->ways[i];
        CTBinaryWay bway = {
            .id = way->id,
            .coord_offset = (uint32_t)coord_offset,
            .num_coords = (uint16_t)way->num_coords,
            .feature_class = (uint8_t)way->feature_class,
            .feature_type = (uint8_t)way->feature_type,
            .is_area = (uint8_t)way->is_area,
            .min_zoom = (uint8_t)way->min_zoom,
            ._padding = 0,
            .name_offset = name_offsets[i],
            .area_sqm = way->area_sqm,
            .length_m = way->length_m
        };
        write_ok = write_ok && (fwrite(&bway, sizeof(bway), 1, f) == 1);
        coord_offset += way->num_coords;
    }

    /* Write way coordinates */
    for (size_t i = 0; i < ctx->num_ways; i++) {
        const CTOSMWay *way = &ctx->ways[i];
        for (int j = 0; j < way->num_coords; j++) {
            CTBinaryCoord coord = {
                .lat_e7 = (int32_t)(way->coords[j].lat * 1e7),
                .lon_e7 = (int32_t)(way->coords[j].lon * 1e7)
            };
            write_ok = write_ok && (fwrite(&coord, sizeof(coord), 1, f) == 1);
        }
    }

    /* Write string pool */
    write_ok = write_ok && (fwrite(strings.data, strings.size, 1, f) == 1);

    /* Write R-Tree nodes */
    if (ctx->rtree && ctx->rtree->nodes) {
        write_ok = write_ok && (fwrite(ctx->rtree->nodes, sizeof(CTPackedNode), ctx->rtree->num_nodes, f) == ctx->rtree->num_nodes);
    }

    /* Write R-Tree leaf indices */
    if (ctx->rtree && ctx->rtree->leaf_indices) {
        write_ok = write_ok && (fwrite(ctx->rtree->leaf_indices, sizeof(uint32_t), ctx->rtree->num_entries, f) == ctx->rtree->num_entries);
    }

    /* Write labeled points */
    for (size_t i = 0; i < ctx->num_labeled_points; i++) {
        const CTLabeledPoint *lp = &ctx->labeled_points[i];
        CTBinaryLabeledPoint blp = {
            .id = lp->id,
            .lat_e7 = (int32_t)(lp->coord.lat * 1e7),
            .lon_e7 = (int32_t)(lp->coord.lon * 1e7),
            .name_offset = label_name_offsets ? label_name_offsets[i] : 0,
            .population = lp->population,
            .type = (uint8_t)lp->type,
            .min_zoom = (uint8_t)lp->min_zoom,
            .priority = (uint8_t)lp->priority,
            ._padding = 0
        };
        write_ok = write_ok && (fwrite(&blp, sizeof(blp), 1, f) == 1);
    }

    /* Write multipolygons */
    size_t ring_offset = 0;
    for (size_t i = 0; i < ctx->num_multipolygons; i++) {
        const CTAssembledMultipolygon *mp = &ctx->multipolygons[i];
        CTBinaryMultipolygon bmp = {
            .ring_offset = (uint32_t)ring_offset,
            .num_rings = (uint16_t)mp->num_rings,
            .feature_class = (uint8_t)mp->feature_class,
            .feature_type = (uint8_t)mp->feature_type,
            .name_offset = mp_name_offsets ? mp_name_offsets[i] : 0,
            .area_sqm = mp->area_sqm,
            .min_lat = (float)mp->bbox.min_lat,
            .min_lon = (float)mp->bbox.min_lon,
            .max_lat = (float)mp->bbox.max_lat,
            .max_lon = (float)mp->bbox.max_lon,
            ._padding = 0
        };
        write_ok = write_ok && (fwrite(&bmp, sizeof(bmp), 1, f) == 1);
        ring_offset += mp->num_rings;
    }

    /* Write multipolygon rings */
    size_t mp_coord_offset = 0;
    for (size_t i = 0; i < ctx->num_multipolygons; i++) {
        const CTAssembledMultipolygon *mp = &ctx->multipolygons[i];
        for (int r = 0; r < mp->num_rings; r++) {
            const CTMultipolygonRing *ring = &mp->rings[r];
            CTBinaryRing bring = {
                .coord_offset = (uint32_t)mp_coord_offset,
                .num_coords = (uint32_t)ring->num_coords,
                .is_outer = (uint8_t)ring->is_outer,
                ._padding = {0, 0, 0}
            };
            write_ok = write_ok && (fwrite(&bring, sizeof(bring), 1, f) == 1);
            mp_coord_offset += ring->num_coords;
        }
    }

    /* Write multipolygon coordinates */
    for (size_t i = 0; i < ctx->num_multipolygons; i++) {
        const CTAssembledMultipolygon *mp = &ctx->multipolygons[i];
        for (int r = 0; r < mp->num_rings; r++) {
            const CTMultipolygonRing *ring = &mp->rings[r];
            for (int j = 0; j < ring->num_coords; j++) {
                CTBinaryCoord coord = {
                    .lat_e7 = (int32_t)(ring->coords[j].lat * 1e7),
                    .lon_e7 = (int32_t)(ring->coords[j].lon * 1e7)
                };
                write_ok = write_ok && (fwrite(&coord, sizeof(coord), 1, f) == 1);
            }
        }
    }

    /* Write Multipolygon R-Tree nodes */
    if (ctx->mp_rtree && ctx->mp_rtree->nodes) {
        write_ok = write_ok && (fwrite(ctx->mp_rtree->nodes, sizeof(CTPackedNode), ctx->mp_rtree->num_nodes, f) == ctx->mp_rtree->num_nodes);
    }

    /* Write Multipolygon R-Tree leaf indices */
    if (ctx->mp_rtree && ctx->mp_rtree->leaf_indices) {
        write_ok = write_ok && (fwrite(ctx->mp_rtree->leaf_indices, sizeof(uint32_t), ctx->mp_rtree->num_entries, f) == ctx->mp_rtree->num_entries);
    }

    /* Write boundaries */
    size_t boundary_coord_offset = 0;
    for (size_t i = 0; i < ctx->num_boundaries; i++) {
        const CTAssembledBoundary *b = &ctx->boundaries[i];
        CTBinaryBoundary bb = {
            .relation_id = b->relation_id,
            .coord_offset = (uint32_t)boundary_coord_offset,
            .num_coords = (uint32_t)b->num_coords,
            .name_offset = boundary_name_offsets ? boundary_name_offsets[i] : 0,
            .boundary_type = (uint8_t)b->boundary_type,
            .admin_level = (uint8_t)b->admin_level,
            ._padding = {0, 0},
            .length_m = b->length_m
        };
        write_ok = write_ok && (fwrite(&bb, sizeof(bb), 1, f) == 1);
        boundary_coord_offset += b->num_coords;
    }

    /* Write boundary coordinates */
    for (size_t i = 0; i < ctx->num_boundaries; i++) {
        const CTAssembledBoundary *b = &ctx->boundaries[i];
        for (int j = 0; j < b->num_coords; j++) {
            CTBinaryCoord coord = {
                .lat_e7 = (int32_t)(b->coords[j].lat * 1e7),
                .lon_e7 = (int32_t)(b->coords[j].lon * 1e7)
            };
            write_ok = write_ok && (fwrite(&coord, sizeof(coord), 1, f) == 1);
        }
    }

    /* Write Boundary R-Tree nodes */
    if (ctx->boundary_rtree && ctx->boundary_rtree->nodes) {
        write_ok = write_ok && (fwrite(ctx->boundary_rtree->nodes, sizeof(CTPackedNode), ctx->boundary_rtree->num_nodes, f) == ctx->boundary_rtree->num_nodes);
    }

    /* Write Boundary R-Tree leaf indices */
    if (ctx->boundary_rtree && ctx->boundary_rtree->leaf_indices) {
        write_ok = write_ok && (fwrite(ctx->boundary_rtree->leaf_indices, sizeof(uint32_t), ctx->boundary_rtree->num_entries, f) == ctx->boundary_rtree->num_entries);
    }

    free(name_offsets);
    free(label_name_offsets);
    free(mp_name_offsets);
    free(boundary_name_offsets);
    string_pool_free(&strings);
    fclose(f);

    return write_ok ? CT_OK : CT_ERROR_FILE_WRITE;
}

/* ============================================================================
 * mmap Load Index
 * ============================================================================ */

CTPBFContext *ct_index_mmap(const char *path) {
    if (!path) return NULL;

    /* One PAL call replaces open/fstat/mmap: the descriptor handling was
     * only ever there to reach mmap, and the mapping keeps its own
     * reference to the file. */
    ShFileMap fm;
    if (sh_map_file_readonly(path, &fm) != 0) return NULL;

    void *map = fm.data;
    size_t file_size = fm.size;

    /* Validate header */
    const CTBinaryHeader *header = (const CTBinaryHeader *)map;
    if (header->magic != CT_BINARY_MAGIC) {
        sh_unmap_ptr(map, file_size);
        return NULL;
    }

    /* Support v3 (no multipolygons), v4 (with multipolygons), v5 (with boundaries) */
    if (header->version != CT_BINARY_VERSION && header->version != 4 && header->version != 3) {
        fprintf(stderr, "Error: Unsupported index version %u (expected %d, 4, or 3)\n",
                header->version, CT_BINARY_VERSION);
        sh_unmap_ptr(map, file_size);
        return NULL;
    }

    const CTSectionOffsets *offsets = (const CTSectionOffsets *)((char *)map + sizeof(CTBinaryHeader));

    /* Validate all section offsets fit within file */

#define VALIDATE_SECTION(off, count, elem_size) do { \
    if ((count) > 0) { \
        if ((size_t)(count) > SIZE_MAX / (elem_size)) { sh_unmap_ptr(map, file_size); return NULL; } \
        size_t section_end = (off) + (size_t)(count) * (elem_size); \
        if (section_end < (off) || section_end > file_size) { \
            fprintf(stderr, "Error: Index file truncated\n"); \
            sh_unmap_ptr(map, file_size); return NULL; \
        } \
    } \
} while(0)

    VALIDATE_SECTION(offsets->ways_offset, header->num_ways, sizeof(CTBinaryWay));
    VALIDATE_SECTION(offsets->coords_offset, header->total_coords, sizeof(CTBinaryCoord));
    VALIDATE_SECTION(offsets->string_pool_offset, header->string_pool_size, 1);
    VALIDATE_SECTION(offsets->rtree_nodes_offset, header->rtree_num_nodes, sizeof(CTPackedNode));
    VALIDATE_SECTION(offsets->rtree_leaf_indices_offset, header->rtree_num_entries, sizeof(uint32_t));
    VALIDATE_SECTION(offsets->labeled_points_offset, header->num_labeled_points, sizeof(CTBinaryLabeledPoint));
    if (header->version >= 4) {
        VALIDATE_SECTION(offsets->multipolygons_offset, header->num_multipolygons, sizeof(CTBinaryMultipolygon));
        VALIDATE_SECTION(offsets->mp_rings_offset, header->total_mp_rings, sizeof(CTBinaryRing));
        VALIDATE_SECTION(offsets->mp_coords_offset, header->total_mp_coords, sizeof(CTBinaryCoord));
        VALIDATE_SECTION(offsets->mp_rtree_nodes_offset, header->mp_rtree_num_nodes, sizeof(CTPackedNode));
        VALIDATE_SECTION(offsets->mp_rtree_leaf_indices_offset, header->mp_rtree_num_entries, sizeof(uint32_t));
    }
    if (header->version >= 5) {
        VALIDATE_SECTION(offsets->boundaries_offset, header->num_boundaries, sizeof(CTBinaryBoundary));
        VALIDATE_SECTION(offsets->boundary_coords_offset, header->total_boundary_coords, sizeof(CTBinaryCoord));
        VALIDATE_SECTION(offsets->boundary_rtree_nodes_offset, header->boundary_rtree_num_nodes, sizeof(CTPackedNode));
        VALIDATE_SECTION(offsets->boundary_rtree_leaf_indices_offset, header->boundary_rtree_num_entries, sizeof(uint32_t));
    }
#undef VALIDATE_SECTION

    /* Create context */
    CTPBFContext *ctx = calloc(1, sizeof(CTPBFContext));
    if (!ctx) {
        sh_unmap_ptr(map, file_size);
        return NULL;
    }

    /* Store mmap info for cleanup */
    ctx->mmap_base = map;
    ctx->mmap_size = file_size;

    /* Set stats for reporting (from header) */
    ctx->features_kept = header->num_ways;
    ctx->total_ways_parsed = header->num_ways;
    ctx->total_nodes_parsed = header->total_coords;

    /* Set bbox */
    ctx->bbox.min_lat = header->min_lat;
    ctx->bbox.min_lon = header->min_lon;
    ctx->bbox.max_lat = header->max_lat;
    ctx->bbox.max_lon = header->max_lon;

    /* Pointers into mmap */
    const CTBinaryWay *binary_ways = (const CTBinaryWay *)((char *)map + offsets->ways_offset);
    const CTBinaryCoord *binary_coords = (const CTBinaryCoord *)((char *)map + offsets->coords_offset);
    const char *string_pool = (const char *)map + offsets->string_pool_offset;

    /* Reconstruct ways array (overflow already validated by VALIDATE_SECTION) */
    ctx->num_ways = header->num_ways;
    ctx->ways_capacity = header->num_ways;
    ctx->ways = header->num_ways > 0 ? calloc(header->num_ways, sizeof(CTOSMWay)) : NULL;
    if (!ctx->ways && header->num_ways > 0) {
        free(ctx);
        sh_unmap_ptr(map, file_size);
        return NULL;
    }

    /* Allocate all coordinates in one block for cache efficiency */
    CTCoord *all_coords = header->total_coords > 0 ? calloc(header->total_coords, sizeof(CTCoord)) : NULL;
    if (!all_coords && header->total_coords > 0) {
        free(ctx->ways);
        free(ctx);
        sh_unmap_ptr(map, file_size);
        return NULL;
    }
    size_t coord_idx = 0;

    for (size_t i = 0; i < header->num_ways; i++) {
        const CTBinaryWay *bway = &binary_ways[i];
        CTOSMWay *way = &ctx->ways[i];

        way->id = bway->id;
        way->num_coords = bway->num_coords;
        way->feature_class = (CTOSMFeatureClass)bway->feature_class;
        way->feature_type = bway->feature_type;
        way->is_area = bway->is_area;
        way->min_zoom = bway->min_zoom;
        way->area_sqm = bway->area_sqm;
        way->length_m = bway->length_m;

        /* Name from string pool (strdup may return NULL on OOM; consumers
         * filter on way->name != NULL so missing labels degrade gracefully) */
        if (bway->name_offset > 0 && bway->name_offset < header->string_pool_size) {
            way->name = strdup(string_pool + bway->name_offset);
        } else {
            way->name = NULL;
        }

        /* Convert coordinates */
        way->coords = &all_coords[coord_idx];
        for (int j = 0; j < bway->num_coords; j++) {
            const CTBinaryCoord *bc = &binary_coords[bway->coord_offset + j];
            way->coords[j].lat = bc->lat_e7 * 1e-7;
            way->coords[j].lon = bc->lon_e7 * 1e-7;
        }
        coord_idx += bway->num_coords;
    }

    /* Store allocated coords base for freeing later */
    ctx->mmap_coords = all_coords;

    /* Reconstruct R-Tree - just point to mmap'd data */
    if (header->rtree_num_nodes > 0) {
        ctx->rtree = calloc(1, sizeof(CTRTree));
        ctx->rtree->num_nodes = header->rtree_num_nodes;
        ctx->rtree->num_entries = header->rtree_num_entries;
        ctx->rtree->root_idx = header->rtree_root_idx;

        /* Point directly to mmap'd arrays (read-only) */
        ctx->rtree->nodes = (CTPackedNode *)((char *)map + offsets->rtree_nodes_offset);
        ctx->rtree->leaf_indices = (uint32_t *)((char *)map + offsets->rtree_leaf_indices_offset);

        /* Mark as mmap'd so we don't try to free these */
        ctx->rtree_is_mmap = 1;
    }

    /* Reconstruct labeled points */
    if (header->num_labeled_points > 0) {
        const CTBinaryLabeledPoint *binary_labels =
            (const CTBinaryLabeledPoint *)((char *)map + offsets->labeled_points_offset);

        ctx->num_labeled_points = header->num_labeled_points;
        ctx->labeled_points_capacity = header->num_labeled_points;
        ctx->labeled_points = calloc(header->num_labeled_points, sizeof(CTLabeledPoint));

        if (!ctx->labeled_points) {
            /* Cleanup on allocation failure */
            for (size_t i = 0; i < ctx->num_ways; i++) {
                free(ctx->ways[i].name);
            }
            free(ctx->ways);
            free(all_coords);
            free(ctx->rtree);
            free(ctx);
            sh_unmap_ptr(map, file_size);
            return NULL;
        }

        for (size_t i = 0; i < header->num_labeled_points; i++) {
            const CTBinaryLabeledPoint *blp = &binary_labels[i];
            CTLabeledPoint *lp = &ctx->labeled_points[i];

            lp->id = blp->id;
            lp->coord.lat = blp->lat_e7 * 1e-7;
            lp->coord.lon = blp->lon_e7 * 1e-7;
            lp->type = (CTPlaceType)blp->type;
            lp->population = blp->population;
            lp->min_zoom = blp->min_zoom;
            lp->priority = blp->priority;

            /* Name from string pool (NULL on OOM degrades gracefully) */
            if (blp->name_offset > 0 && blp->name_offset < header->string_pool_size) {
                lp->name = strdup(string_pool + blp->name_offset);
            } else {
                lp->name = NULL;
            }
        }
    }

    /* Reconstruct multipolygons (v4+ only) */
    if (header->version >= 4 && header->num_multipolygons > 0) {
        const CTBinaryMultipolygon *binary_mps =
            (const CTBinaryMultipolygon *)((char *)map + offsets->multipolygons_offset);
        const CTBinaryRing *binary_rings =
            (const CTBinaryRing *)((char *)map + offsets->mp_rings_offset);
        const CTBinaryCoord *binary_mp_coords =
            (const CTBinaryCoord *)((char *)map + offsets->mp_coords_offset);

        ctx->num_multipolygons = header->num_multipolygons;
        ctx->multipolygons_capacity = header->num_multipolygons;
        ctx->multipolygons = calloc(header->num_multipolygons, sizeof(CTAssembledMultipolygon));

        if (!ctx->multipolygons) {
            /* Continue without multipolygons rather than fail completely */
            ctx->num_multipolygons = 0;
        } else {
            /* Allocate all multipolygon coordinates in one block */
            CTCoord *all_mp_coords = calloc(header->total_mp_coords, sizeof(CTCoord));
            if (!all_mp_coords) {
                free(ctx->multipolygons);
                ctx->multipolygons = NULL;
                ctx->num_multipolygons = 0;
            } else {
                /* Store for cleanup */
                ctx->mmap_mp_coords = all_mp_coords;

                /* Allocate all rings in one block */
                CTMultipolygonRing *all_rings = calloc(header->total_mp_rings, sizeof(CTMultipolygonRing));
                if (!all_rings) {
                    free(all_mp_coords);
                    free(ctx->multipolygons);
                    ctx->multipolygons = NULL;
                    ctx->num_multipolygons = 0;
                    ctx->mmap_mp_coords = NULL;
                } else {
                    ctx->mmap_mp_rings = all_rings;

                    size_t ring_idx = 0;
                    size_t mp_coord_idx = 0;

                    for (size_t i = 0; i < header->num_multipolygons; i++) {
                        const CTBinaryMultipolygon *bmp = &binary_mps[i];
                        CTAssembledMultipolygon *mp = &ctx->multipolygons[i];

                        mp->num_rings = bmp->num_rings;
                        mp->feature_class = (CTOSMFeatureClass)bmp->feature_class;
                        mp->feature_type = bmp->feature_type;
                        mp->area_sqm = bmp->area_sqm;
                        mp->bbox.min_lat = bmp->min_lat;
                        mp->bbox.min_lon = bmp->min_lon;
                        mp->bbox.max_lat = bmp->max_lat;
                        mp->bbox.max_lon = bmp->max_lon;

                        /* Name from string pool (NULL on OOM degrades gracefully) */
                        if (bmp->name_offset > 0 && bmp->name_offset < header->string_pool_size) {
                            mp->name = strdup(string_pool + bmp->name_offset);
                        } else {
                            mp->name = NULL;
                        }

                        /* Point to pre-allocated rings */
                        mp->rings = &all_rings[ring_idx];

                        /* Reconstruct rings */
                        for (int r = 0; r < bmp->num_rings; r++) {
                            const CTBinaryRing *bring = &binary_rings[bmp->ring_offset + r];
                            CTMultipolygonRing *ring = &mp->rings[r];

                            ring->num_coords = bring->num_coords;
                            ring->is_outer = bring->is_outer;

                            /* Point to pre-allocated coords */
                            ring->coords = &all_mp_coords[mp_coord_idx];

                            /* Convert coordinates */
                            for (uint32_t j = 0; j < bring->num_coords; j++) {
                                const CTBinaryCoord *bc = &binary_mp_coords[bring->coord_offset + j];
                                ring->coords[j].lat = bc->lat_e7 * 1e-7;
                                ring->coords[j].lon = bc->lon_e7 * 1e-7;
                            }
                            mp_coord_idx += bring->num_coords;
                        }
                        ring_idx += bmp->num_rings;
                    }

                    /* Reconstruct multipolygon R-Tree */
                    if (header->mp_rtree_num_nodes > 0) {
                        ctx->mp_rtree = calloc(1, sizeof(CTRTree));
                        ctx->mp_rtree->num_nodes = header->mp_rtree_num_nodes;
                        ctx->mp_rtree->num_entries = header->mp_rtree_num_entries;
                        ctx->mp_rtree->root_idx = header->mp_rtree_root_idx;

                        /* Point directly to mmap'd arrays */
                        ctx->mp_rtree->nodes = (CTPackedNode *)((char *)map + offsets->mp_rtree_nodes_offset);
                        ctx->mp_rtree->leaf_indices = (uint32_t *)((char *)map + offsets->mp_rtree_leaf_indices_offset);

                        ctx->mp_rtree_is_mmap = 1;
                    }
                }
            }
        }
    }

    /* Reconstruct boundaries (v5+ only) */
    if (header->version >= 5 && header->num_boundaries > 0) {
        const CTBinaryBoundary *binary_boundaries =
            (const CTBinaryBoundary *)((char *)map + offsets->boundaries_offset);
        const CTBinaryCoord *binary_boundary_coords =
            (const CTBinaryCoord *)((char *)map + offsets->boundary_coords_offset);

        ctx->num_boundaries = header->num_boundaries;
        ctx->boundaries_capacity = header->num_boundaries;
        ctx->boundaries = calloc(header->num_boundaries, sizeof(CTAssembledBoundary));

        if (ctx->boundaries) {
            /* Allocate all boundary coordinates in one block */
            CTCoord *all_boundary_coords = calloc(header->total_boundary_coords, sizeof(CTCoord));
            if (!all_boundary_coords) {
                free(ctx->boundaries);
                ctx->boundaries = NULL;
                ctx->num_boundaries = 0;
            } else {
                ctx->mmap_boundary_coords = all_boundary_coords;
                size_t boundary_coord_idx = 0;

                for (size_t i = 0; i < header->num_boundaries; i++) {
                    const CTBinaryBoundary *bb = &binary_boundaries[i];
                    CTAssembledBoundary *b = &ctx->boundaries[i];

                    b->relation_id = bb->relation_id;
                    b->num_coords = bb->num_coords;
                    b->boundary_type = (CTBoundaryType)bb->boundary_type;
                    b->admin_level = bb->admin_level;
                    b->length_m = bb->length_m;

                    /* Name from string pool (NULL on OOM degrades gracefully) */
                    if (bb->name_offset > 0 && bb->name_offset < header->string_pool_size) {
                        b->name = strdup(string_pool + bb->name_offset);
                    } else {
                        b->name = NULL;
                    }

                    /* Point to pre-allocated coords */
                    b->coords = &all_boundary_coords[boundary_coord_idx];

                    /* Convert coordinates and compute bbox */
                    b->bbox.min_lat = 90.0;
                    b->bbox.max_lat = -90.0;
                    b->bbox.min_lon = 180.0;
                    b->bbox.max_lon = -180.0;

                    for (uint32_t j = 0; j < bb->num_coords; j++) {
                        const CTBinaryCoord *bc = &binary_boundary_coords[bb->coord_offset + j];
                        double lat = bc->lat_e7 * 1e-7;
                        double lon = bc->lon_e7 * 1e-7;
                        b->coords[j].lat = lat;
                        b->coords[j].lon = lon;

                        if (lat < b->bbox.min_lat) b->bbox.min_lat = lat;
                        if (lat > b->bbox.max_lat) b->bbox.max_lat = lat;
                        if (lon < b->bbox.min_lon) b->bbox.min_lon = lon;
                        if (lon > b->bbox.max_lon) b->bbox.max_lon = lon;
                    }
                    boundary_coord_idx += bb->num_coords;
                }

                /* Reconstruct boundary R-Tree */
                if (header->boundary_rtree_num_nodes > 0) {
                    ctx->boundary_rtree = calloc(1, sizeof(CTRTree));
                    ctx->boundary_rtree->num_nodes = header->boundary_rtree_num_nodes;
                    ctx->boundary_rtree->num_entries = header->boundary_rtree_num_entries;
                    ctx->boundary_rtree->root_idx = header->boundary_rtree_root_idx;

                    /* Point directly to mmap'd arrays */
                    ctx->boundary_rtree->nodes = (CTPackedNode *)((char *)map + offsets->boundary_rtree_nodes_offset);
                    ctx->boundary_rtree->leaf_indices = (uint32_t *)((char *)map + offsets->boundary_rtree_leaf_indices_offset);

                    ctx->boundary_rtree_is_mmap = 1;
                }
            }
        }
    }

    return ctx;
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

int ct_is_binary_index(const char *path) {
    if (!path) return 0;

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    uint32_t magic;
    size_t n = fread(&magic, sizeof(magic), 1, f);
    fclose(f);

    return n == 1 && magic == CT_BINARY_MAGIC;
}

uint32_t ct_binary_version(const char *path) {
    if (!path) return 0;

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    CTBinaryHeader header;
    size_t n = fread(&header, sizeof(header), 1, f);
    fclose(f);

    if (n != 1 || header.magic != CT_BINARY_MAGIC) return 0;
    return header.version;
}
