/*
 * ct_serialize.c - Binary Index Serialization with mmap Support
 *
 * Binary format v2:
 *
 * [Header] (72 bytes)
 *   magic: u32 (0x43525441 = "CRTA")
 *   version: u32 (2)
 *   num_ways: u32
 *   total_coords: u32
 *   string_pool_size: u32
 *   rtree_num_nodes: u32
 *   rtree_num_entries: u32
 *   rtree_root_idx: u32
 *   num_labeled_points: u32
 *   _reserved: u32
 *   bbox: 4 x f64 (min_lat, min_lon, max_lat, max_lon)
 *
 * [Section Offsets] (56 bytes)
 *   ways_offset: u64
 *   coords_offset: u64
 *   string_pool_offset: u64
 *   rtree_nodes_offset: u64
 *   rtree_leaf_indices_offset: u64
 *   labeled_points_offset: u64
 *   _padding: u64
 *
 * [Way Records] - 32 bytes each
 * [Coordinates] - 8 bytes each (int32 lat_e7, lon_e7)
 * [String Pool] - null-terminated strings
 * [R-Tree Nodes] - CTPackedNode array
 * [R-Tree Leaf Indices] - u32 array
 * [Labeled Points] - 32 bytes each
 */

#include "ct_serialize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
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
    uint32_t _reserved;
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

static void string_pool_init(StringPool *pool) {
    pool->data = malloc(STRING_POOL_INITIAL_CAPACITY);
    pool->size = 1;  /* Reserve 0 for NULL strings - offset 0 means empty string */
    pool->capacity = STRING_POOL_INITIAL_CAPACITY;
    pool->data[0] = '\0';
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
        pool->capacity *= 2;
        pool->data = realloc(pool->data, pool->capacity);
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
    string_pool_init(&strings);

    /* Count total coordinates */
    size_t total_coords = 0;
    for (size_t i = 0; i < ctx->num_ways; i++) {
        total_coords += ctx->ways[i].num_coords;
    }

    /* Calculate offsets */
    size_t header_size = sizeof(CTBinaryHeader) + sizeof(CTSectionOffsets);
    size_t ways_size = ctx->num_ways * sizeof(CTBinaryWay);
    size_t coords_size = total_coords * sizeof(CTBinaryCoord);

    /* First pass: build string pool to get its size */
    uint32_t *name_offsets = malloc(ctx->num_ways * sizeof(uint32_t));
    for (size_t i = 0; i < ctx->num_ways; i++) {
        name_offsets[i] = string_pool_add(&strings, ctx->ways[i].name);
    }

    /* Add labeled point names to string pool */
    uint32_t *label_name_offsets = NULL;
    if (ctx->num_labeled_points > 0) {
        label_name_offsets = malloc(ctx->num_labeled_points * sizeof(uint32_t));
        for (size_t i = 0; i < ctx->num_labeled_points; i++) {
            label_name_offsets[i] = string_pool_add(&strings, ctx->labeled_points[i].name);
        }
    }

    size_t string_pool_size = strings.size;
    size_t rtree_nodes_size = ctx->rtree ? ctx->rtree->num_nodes * sizeof(CTPackedNode) : 0;
    size_t rtree_leaf_size = ctx->rtree ? ctx->rtree->num_entries * sizeof(uint32_t) : 0;

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
        ._reserved = 0,
        .min_lat = ctx->bbox.min_lat,
        .min_lon = ctx->bbox.min_lon,
        .max_lat = ctx->bbox.max_lat,
        .max_lon = ctx->bbox.max_lon
    };
    fwrite(&header, sizeof(header), 1, f);

    /* Write section offsets */
    CTSectionOffsets offsets = {
        .ways_offset = header_size,
        .coords_offset = header_size + ways_size,
        .string_pool_offset = header_size + ways_size + coords_size,
        .rtree_nodes_offset = header_size + ways_size + coords_size + string_pool_size,
        .rtree_leaf_indices_offset = header_size + ways_size + coords_size + string_pool_size + rtree_nodes_size,
        .labeled_points_offset = header_size + ways_size + coords_size + string_pool_size + rtree_nodes_size + rtree_leaf_size,
        ._padding = 0
    };
    fwrite(&offsets, sizeof(offsets), 1, f);

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
        fwrite(&bway, sizeof(bway), 1, f);
        coord_offset += way->num_coords;
    }

    /* Write coordinates */
    for (size_t i = 0; i < ctx->num_ways; i++) {
        const CTOSMWay *way = &ctx->ways[i];
        for (int j = 0; j < way->num_coords; j++) {
            CTBinaryCoord coord = {
                .lat_e7 = (int32_t)(way->coords[j].lat * 1e7),
                .lon_e7 = (int32_t)(way->coords[j].lon * 1e7)
            };
            fwrite(&coord, sizeof(coord), 1, f);
        }
    }

    /* Write string pool */
    fwrite(strings.data, strings.size, 1, f);

    /* Write R-Tree nodes */
    if (ctx->rtree && ctx->rtree->nodes) {
        fwrite(ctx->rtree->nodes, sizeof(CTPackedNode), ctx->rtree->num_nodes, f);
    }

    /* Write R-Tree leaf indices */
    if (ctx->rtree && ctx->rtree->leaf_indices) {
        fwrite(ctx->rtree->leaf_indices, sizeof(uint32_t), ctx->rtree->num_entries, f);
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
        fwrite(&blp, sizeof(blp), 1, f);
    }

    free(name_offsets);
    free(label_name_offsets);
    string_pool_free(&strings);
    fclose(f);

    return CT_OK;
}

/* ============================================================================
 * mmap Load Index
 * ============================================================================ */

CTPBFContext *ct_index_mmap(const char *path) {
    if (!path) return NULL;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return NULL;
    }

    void *map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (map == MAP_FAILED) return NULL;

    /* Validate header */
    const CTBinaryHeader *header = (const CTBinaryHeader *)map;
    if (header->magic != CT_BINARY_MAGIC || header->version != CT_BINARY_VERSION) {
        munmap(map, st.st_size);
        return NULL;
    }

    const CTSectionOffsets *offsets = (const CTSectionOffsets *)((char *)map + sizeof(CTBinaryHeader));

    /* Validate file size - must contain all sections */
    size_t min_size = offsets->coords_offset + header->total_coords * sizeof(CTBinaryCoord);
    if ((size_t)st.st_size < min_size) {
        fprintf(stderr, "Error: Index file truncated (size %ld, need %zu)\n",
                (long)st.st_size, min_size);
        munmap(map, st.st_size);
        return NULL;
    }

    /* Create context */
    CTPBFContext *ctx = calloc(1, sizeof(CTPBFContext));
    if (!ctx) {
        munmap(map, st.st_size);
        return NULL;
    }

    /* Store mmap info for cleanup */
    ctx->mmap_base = map;
    ctx->mmap_size = st.st_size;

    /* Set stats for reporting (from header) */
    ctx->features_kept = header->num_ways;
    ctx->total_ways_parsed = header->num_ways;  /* We don't track original counts in index */
    ctx->total_nodes_parsed = header->total_coords;  /* Approximate */

    /* Set bbox */
    ctx->bbox.min_lat = header->min_lat;
    ctx->bbox.min_lon = header->min_lon;
    ctx->bbox.max_lat = header->max_lat;
    ctx->bbox.max_lon = header->max_lon;

    /* Pointers into mmap */
    const CTBinaryWay *binary_ways = (const CTBinaryWay *)((char *)map + offsets->ways_offset);
    const CTBinaryCoord *binary_coords = (const CTBinaryCoord *)((char *)map + offsets->coords_offset);
    const char *string_pool = (const char *)map + offsets->string_pool_offset;

    /* Reconstruct ways array */
    ctx->num_ways = header->num_ways;
    ctx->ways_capacity = header->num_ways;
    ctx->ways = malloc(header->num_ways * sizeof(CTOSMWay));

    /* Allocate all coordinates in one block for cache efficiency */
    CTCoord *all_coords = malloc(header->total_coords * sizeof(CTCoord));
    if (!all_coords) {
        free(ctx->ways);
        free(ctx);
        munmap(map, st.st_size);
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

        /* Name from string pool */
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
        ctx->labeled_points = malloc(header->num_labeled_points * sizeof(CTLabeledPoint));

        if (!ctx->labeled_points) {
            /* Cleanup on allocation failure */
            for (size_t i = 0; i < ctx->num_ways; i++) {
                free(ctx->ways[i].name);
            }
            free(ctx->ways);
            free(all_coords);
            free(ctx->rtree);
            free(ctx);
            munmap(map, st.st_size);
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

            /* Name from string pool */
            if (blp->name_offset > 0 && blp->name_offset < header->string_pool_size) {
                lp->name = strdup(string_pool + blp->name_offset);
            } else {
                lp->name = NULL;
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
