/*
 * lc_serialize.c - Binary Index Serialization with mmap Support
 *
 * Binary format v4 (zero-copy with geometry/ngrams):
 *
 * [Header v4] (80 bytes)
 *   magic: u32 (0x4C4F4355 = "LOCU")
 *   version: u32 (4)
 *   entity_count: u32
 *   string_pool_size: u32
 *   trie_node_count: u32
 *   trie_entity_count: u32
 *   grid_width: u32
 *   grid_height: u32
 *   bounds: 4 x f64 (min_lat, min_lon, max_lat, max_lon)
 *   grid_cell_size: f64
 *   ngram_entry_count: u32
 *   ngram_entity_count: u32
 *   geometry_count: u32
 *   geometry_point_count: u32
 *
 * [Section Offsets v4] (96 bytes)
 *   entities_offset: u64
 *   alt_names_offset: u64
 *   string_pool_offset: u64
 *   trie_nodes_offset: u64
 *   trie_entities_offset: u64
 *   grid_cells_offset: u64
 *   grid_entities_offset: u64
 *   ngram_entries_offset: u64
 *   ngram_entities_offset: u64
 *   geometry_offsets_offset: u64
 *   geometry_points_offset: u64
 *
 * [Entity Records] - 72 bytes each (v4 includes geometry_offset)
 * [Alt Name Offsets] - u32 array
 * [String Pool] - deduplicated, null-terminated strings
 * [Trie Nodes] - flattened trie structure
 * [Trie Entity IDs] - entity IDs for trie nodes
 * [Grid Cell Offsets] - offset into grid entities for each cell
 * [Grid Entity IDs] - entity IDs for grid cells
 * [N-gram Entries] - sorted trigram entries
 * [N-gram Entity IDs] - entity IDs for n-gram entries
 * [Geometry Offsets] - entity_id + point_offset + point_count
 * [Geometry Points] - int32 lat_e7, lon_e7 pairs
 */

#include "lc_serialize.h"
#include "lc_mmap.h"
#include "lc_normalize.h"
#include "lc_spatial.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <math.h>

#define NULL_OFFSET 0xFFFFFFFF
#define LC_BINARY_VERSION_V3 3

/* ============================================================================
 * Binary Format Structures (v3 - for backwards compatibility)
 * ============================================================================ */

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
    uint32_t _padding[2];  /* Align to 64 bytes */
} LCBinaryHeaderV3;

typedef struct __attribute__((packed)) {
    uint64_t entities_offset;
    uint64_t alt_names_offset;
    uint64_t string_pool_offset;
    uint64_t trie_nodes_offset;
    uint64_t trie_entities_offset;
    uint64_t grid_cells_offset;
    uint64_t grid_entities_offset;
    uint64_t _padding;  /* Align to 64 bytes */
} LCSectionOffsetsV3;

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
} LCBinaryEntityV3;

/* Serialized trie node - fixed 160 bytes */
typedef struct __attribute__((packed)) {
    uint32_t children[LC_TRIE_ALPHABET_SIZE];  /* Child node indices, NULL_OFFSET if none */
    uint32_t entity_offset;   /* Offset into trie entity array */
    uint16_t entity_count;    /* Number of entities at this node */
    uint16_t _padding;
} LCBinaryTrieNode;

/* ============================================================================
 * String Pool with Deduplication
 * ============================================================================ */

#define HASH_TABLE_SIZE 65536

typedef struct StringEntry {
    const char *str;
    uint32_t offset;
    uint32_t hash;
    int32_t next;  /* Index into entries array, -1 = end */
} StringEntry;

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
    int32_t buckets[HASH_TABLE_SIZE];  /* Index into entries, -1 = empty */
    StringEntry *entries;
    size_t entry_count;
    size_t entry_capacity;
    size_t dedup_hits;
} StringPool;

static uint32_t hash_string(const char *str) {
    uint32_t hash = 5381;
    while (*str) {
        hash = ((hash << 5) + hash) + (uint8_t)*str++;
    }
    return hash;
}

static void pool_init(StringPool *pool) {
    memset(pool, 0, sizeof(StringPool));
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        pool->buckets[i] = -1;
    }
    pool->entry_capacity = 4096;
    pool->entries = malloc(pool->entry_capacity * sizeof(StringEntry));
}

static void pool_free(StringPool *pool) {
    free(pool->data);
    free(pool->entries);
    memset(pool, 0, sizeof(StringPool));
}

static uint32_t pool_add(StringPool *pool, const char *str) {
    if (!str) return NULL_OFFSET;

    /* Check for existing string */
    uint32_t hash = hash_string(str);
    uint32_t bucket = hash % HASH_TABLE_SIZE;

    for (int32_t idx = pool->buckets[bucket]; idx >= 0; idx = pool->entries[idx].next) {
        StringEntry *e = &pool->entries[idx];
        if (e->hash == hash && strcmp(e->str, str) == 0) {
            pool->dedup_hits++;
            return e->offset;
        }
    }

    /* Add new string to pool */
    size_t len = strlen(str) + 1;

    if (pool->size + len > pool->capacity) {
        size_t new_cap = pool->capacity == 0 ? 65536 : pool->capacity * 2;
        while (new_cap < pool->size + len) new_cap *= 2;
        char *new_data = realloc(pool->data, new_cap);
        if (!new_data) return NULL_OFFSET;
        pool->data = new_data;
        pool->capacity = new_cap;
    }

    uint32_t offset = (uint32_t)pool->size;
    memcpy(pool->data + pool->size, str, len);
    pool->size += len;

    /* Add to hash table */
    if (pool->entry_count >= pool->entry_capacity) {
        pool->entry_capacity *= 2;
        pool->entries = realloc(pool->entries, pool->entry_capacity * sizeof(StringEntry));
    }

    int32_t new_idx = (int32_t)pool->entry_count++;
    StringEntry *entry = &pool->entries[new_idx];
    entry->str = pool->data + offset;  /* Point to pool copy */
    entry->offset = offset;
    entry->hash = hash;
    entry->next = pool->buckets[bucket];
    pool->buckets[bucket] = new_idx;

    return offset;
}

/* ============================================================================
 * Trie Serialization
 * ============================================================================ */

typedef struct {
    LCBinaryTrieNode *nodes;
    uint32_t *entity_ids;
    uint32_t node_count;
    uint32_t node_capacity;
    uint32_t entity_count;
    uint32_t entity_capacity;
} TrieSerializer;

static void trie_serializer_init(TrieSerializer *ts) {
    ts->node_capacity = 4096;
    ts->nodes = malloc(ts->node_capacity * sizeof(LCBinaryTrieNode));
    ts->entity_capacity = 8192;
    ts->entity_ids = malloc(ts->entity_capacity * sizeof(uint32_t));
    ts->node_count = 0;
    ts->entity_count = 0;
}

static void trie_serializer_free(TrieSerializer *ts) {
    free(ts->nodes);
    free(ts->entity_ids);
}

/* Stack entry for iterative trie serialization */
typedef struct {
    const LCTrieNode *node;
    uint32_t node_idx;
    int child_idx;  /* Next child to process, -1 = first visit */
} TrieStackEntry;

/* Iteratively serialize trie (avoids stack overflow) */
static void serialize_trie_iterative(TrieSerializer *ts, const LCTrieNode *root) {
    if (!root) return;

    /* Explicit stack for DFS */
    size_t stack_capacity = 1024;
    TrieStackEntry *stack = malloc(stack_capacity * sizeof(TrieStackEntry));
    if (!stack) return;

    size_t stack_size = 0;

    /* Push root */
    stack[stack_size++] = (TrieStackEntry){root, 0, -1};

    while (stack_size > 0) {
        TrieStackEntry *top = &stack[stack_size - 1];

        if (top->child_idx == -1) {
            /* First visit: allocate node and store data */
            if (ts->node_count >= ts->node_capacity) {
                size_t new_capacity = ts->node_capacity * 2;
                LCBinaryTrieNode *new_nodes = realloc(ts->nodes, new_capacity * sizeof(LCBinaryTrieNode));
                if (!new_nodes) {
                    free(stack);
                    return;  /* Out of memory - abort serialization */
                }
                ts->nodes = new_nodes;
                ts->node_capacity = new_capacity;
            }

            top->node_idx = ts->node_count++;
            LCBinaryTrieNode *bn = &ts->nodes[top->node_idx];

            /* Initialize children to NULL */
            for (int i = 0; i < LC_TRIE_ALPHABET_SIZE; i++) {
                bn->children[i] = NULL_OFFSET;
            }

            /* Store entity IDs */
            bn->entity_offset = ts->entity_count;
            bn->entity_count = top->node->num_entities;
            bn->_padding = 0;

            if (top->node->num_entities > 0) {
                if (ts->entity_count + top->node->num_entities > ts->entity_capacity) {
                    size_t new_capacity = ts->entity_capacity;
                    while (ts->entity_count + top->node->num_entities > new_capacity) {
                        new_capacity *= 2;
                    }
                    uint32_t *new_ids = realloc(ts->entity_ids, new_capacity * sizeof(uint32_t));
                    if (!new_ids) {
                        free(stack);
                        return;  /* Out of memory - abort serialization */
                    }
                    ts->entity_ids = new_ids;
                    ts->entity_capacity = new_capacity;
                }
                memcpy(ts->entity_ids + ts->entity_count, top->node->entity_ids,
                       top->node->num_entities * sizeof(uint32_t));
                ts->entity_count += top->node->num_entities;
            }

            top->child_idx = 0;
        }

        /* Find next child to process */
        int found_child = 0;
        while (top->child_idx < LC_TRIE_ALPHABET_SIZE) {
            int i = top->child_idx++;
            if (top->node->children[i]) {
                /* Grow stack if needed */
                if (stack_size >= stack_capacity) {
                    size_t new_capacity = stack_capacity * 2;
                    TrieStackEntry *new_stack = realloc(stack, new_capacity * sizeof(TrieStackEntry));
                    if (!new_stack) {
                        free(stack);
                        return;  /* Out of memory - abort serialization */
                    }
                    stack = new_stack;
                    stack_capacity = new_capacity;
                    top = &stack[stack_size - 1];  /* Realloc may move */
                }

                /* Push child */
                stack[stack_size++] = (TrieStackEntry){top->node->children[i], 0, -1};
                found_child = 1;
                break;
            }
        }

        if (!found_child) {
            /* All children processed, pop this node */
            stack_size--;

            /* Update parent's child pointer */
            if (stack_size > 0) {
                TrieStackEntry *parent = &stack[stack_size - 1];
                int child_slot = parent->child_idx - 1;
                ts->nodes[parent->node_idx].children[child_slot] = top->node_idx;
            }
        }
    }

    free(stack);
}

/* ============================================================================
 * Grid Serialization
 * ============================================================================ */

typedef struct {
    uint32_t *cell_offsets;  /* Offset into entity array for each cell */
    uint32_t *entity_ids;    /* All entity IDs */
    uint32_t cell_count;
    uint32_t entity_count;
} GridSerializer;

static void serialize_grid(const LCSpatialGrid *grid, GridSerializer *gs) {
    if (!grid) {
        gs->cell_offsets = NULL;
        gs->entity_ids = NULL;
        gs->cell_count = 0;
        gs->entity_count = 0;
        return;
    }

    gs->cell_count = (uint32_t)(grid->grid_width * grid->grid_height);
    gs->cell_offsets = malloc((gs->cell_count + 1) * sizeof(uint32_t));

    /* Count total entities */
    gs->entity_count = 0;
    for (uint32_t i = 0; i < gs->cell_count; i++) {
        gs->entity_count += grid->cell_counts[i];
    }

    gs->entity_ids = malloc(gs->entity_count * sizeof(uint32_t));

    /* Fill offsets and copy entity IDs */
    uint32_t offset = 0;
    for (uint32_t i = 0; i < gs->cell_count; i++) {
        gs->cell_offsets[i] = offset;
        if (grid->cell_counts[i] > 0 && grid->cells[i]) {
            memcpy(gs->entity_ids + offset, grid->cells[i],
                   grid->cell_counts[i] * sizeof(uint32_t));
            offset += grid->cell_counts[i];
        }
    }
    gs->cell_offsets[gs->cell_count] = offset;  /* End marker */
}

/* ============================================================================
 * N-gram Serialization (v4)
 * ============================================================================ */

typedef struct {
    LCBinaryNgramEntry *entries;
    uint32_t *entity_ids;
    uint32_t entry_count;
    uint32_t entity_count;
    uint32_t entry_capacity;
    uint32_t entity_capacity;
} NgramSerializer;

static void ngram_serializer_init(NgramSerializer *ns) {
    ns->entry_capacity = 4096;
    ns->entries = malloc(ns->entry_capacity * sizeof(LCBinaryNgramEntry));
    ns->entity_capacity = 16384;
    ns->entity_ids = malloc(ns->entity_capacity * sizeof(uint32_t));
    ns->entry_count = 0;
    ns->entity_count = 0;
}

static void ngram_serializer_free(NgramSerializer *ns) {
    free(ns->entries);
    free(ns->entity_ids);
}

static void serialize_ngram(const LCNgramIndex *ngram, NgramSerializer *ns) {
    if (!ngram || ngram->num_entries == 0) return;

    for (uint32_t i = 0; i < ngram->num_entries; i++) {
        const LCNgramEntry *e = &ngram->entries[i];

        /* Grow entries array if needed */
        if (ns->entry_count >= ns->entry_capacity) {
            uint32_t new_capacity = ns->entry_capacity * 2;
            LCBinaryNgramEntry *new_entries = realloc(ns->entries, new_capacity * sizeof(LCBinaryNgramEntry));
            if (!new_entries) return;  /* Out of memory - abort serialization */
            ns->entries = new_entries;
            ns->entry_capacity = new_capacity;
        }

        /* Grow entity IDs array if needed */
        if (ns->entity_count + e->count > ns->entity_capacity) {
            uint32_t new_capacity = ns->entity_capacity;
            while (ns->entity_count + e->count > new_capacity) {
                new_capacity *= 2;
            }
            uint32_t *new_ids = realloc(ns->entity_ids, new_capacity * sizeof(uint32_t));
            if (!new_ids) return;  /* Out of memory - abort serialization */
            ns->entity_ids = new_ids;
            ns->entity_capacity = new_capacity;
        }

        /* Pack trigram into 24-bit value */
        LCBinaryNgramEntry *be = &ns->entries[ns->entry_count++];
        be->trigram = lc_pack_trigram(e->ngram);
        be->entity_offset = ns->entity_count;
        be->entity_count = (uint16_t)(e->count > 65535 ? 65535 : e->count);
        be->_padding = 0;

        /* Copy entity IDs */
        uint32_t copy_count = e->count > 65535 ? 65535 : e->count;
        memcpy(ns->entity_ids + ns->entity_count, e->entity_ids, copy_count * sizeof(uint32_t));
        ns->entity_count += copy_count;
    }
}

/* ============================================================================
 * Geometry Serialization (v4)
 * ============================================================================ */

typedef struct {
    LCBinaryGeometry *offsets;
    LCBinaryPoint *points;
    uint32_t offset_count;
    uint32_t point_count;
    uint32_t offset_capacity;
    uint32_t point_capacity;
} GeometrySerializer;

static void geometry_serializer_init(GeometrySerializer *gs) {
    gs->offset_capacity = 4096;
    gs->offsets = malloc(gs->offset_capacity * sizeof(LCBinaryGeometry));
    gs->point_capacity = 32768;
    gs->points = malloc(gs->point_capacity * sizeof(LCBinaryPoint));
    gs->offset_count = 0;
    gs->point_count = 0;
}

static void geometry_serializer_free(GeometrySerializer *gs) {
    free(gs->offsets);
    free(gs->points);
}

/* Returns geometry index for the entity, or NULL_OFFSET if no geometry */
static uint32_t serialize_entity_geometry(const LCEntity *e, uint32_t entity_id, GeometrySerializer *gs) {
    if (!e->geometry || e->geometry->count < 2) {
        return NULL_OFFSET;
    }

    /* Grow offsets array if needed */
    if (gs->offset_count >= gs->offset_capacity) {
        uint32_t new_capacity = gs->offset_capacity * 2;
        LCBinaryGeometry *new_offsets = realloc(gs->offsets, new_capacity * sizeof(LCBinaryGeometry));
        if (!new_offsets) return NULL_OFFSET;  /* Out of memory */
        gs->offsets = new_offsets;
        gs->offset_capacity = new_capacity;
    }

    /* Grow points array if needed */
    if (gs->point_count + e->geometry->count > gs->point_capacity) {
        uint32_t new_capacity = gs->point_capacity;
        while (gs->point_count + e->geometry->count > new_capacity) {
            new_capacity *= 2;
        }
        LCBinaryPoint *new_points = realloc(gs->points, new_capacity * sizeof(LCBinaryPoint));
        if (!new_points) return NULL_OFFSET;  /* Out of memory */
        gs->points = new_points;
        gs->point_capacity = new_capacity;
    }

    uint32_t geom_idx = gs->offset_count++;
    LCBinaryGeometry *geom = &gs->offsets[geom_idx];
    geom->entity_id = entity_id;
    geom->point_offset = gs->point_count;
    geom->point_count = e->geometry->count;

    /* Convert and copy points */
    for (uint32_t i = 0; i < e->geometry->count; i++) {
        LCBinaryPoint *pt = &gs->points[gs->point_count++];
        pt->lat_e7 = (int32_t)(e->geometry->points[i].lat * 1e7);
        pt->lon_e7 = (int32_t)(e->geometry->points[i].lon * 1e7);
    }

    return geom_idx;
}

/* ============================================================================
 * Save Index (v4 format)
 * ============================================================================ */

LCStatus lc_index_save(const LCIndex *index, const char *path)
{
    if (!index || !path) return LC_ERROR_INVALID_PARAM;
    if (!index->entities) return LC_ERROR_INVALID_PARAM;

    FILE *f = fopen(path, "wb");
    if (!f) return LC_ERROR_FILE_NOT_FOUND;

    /* Initialize serializers */
    StringPool pool;
    pool_init(&pool);

    TrieSerializer ts;
    trie_serializer_init(&ts);

    GridSerializer grid_s = {0};

    NgramSerializer ns;
    ngram_serializer_init(&ns);

    GeometrySerializer geom_s;
    geometry_serializer_init(&geom_s);

    /* Count total alt names */
    uint32_t total_alt_names = 0;
    for (uint32_t i = 0; i < index->num_entities; i++) {
        total_alt_names += index->entities->entities[i].num_alt_names;
    }

    /* Allocate arrays */
    uint32_t *alt_name_offsets = NULL;
    LCBinaryEntityV4 *records = NULL;

    if (total_alt_names > 0) {
        alt_name_offsets = malloc(total_alt_names * sizeof(uint32_t));
        if (!alt_name_offsets) goto error;
    }

    records = malloc(index->num_entities * sizeof(LCBinaryEntityV4));
    if (!records) goto error;

    /* Build entity records with deduplicated strings */
    uint32_t alt_name_idx = 0;
    for (uint32_t i = 0; i < index->num_entities; i++) {
        const LCEntity *e = &index->entities->entities[i];
        LCBinaryEntityV4 *r = &records[i];

        r->osm_id = e->osm_id;
        r->type = (uint8_t)e->type;
        r->fclass = (uint8_t)e->fclass;
        r->admin_level = e->admin_level;
        r->num_alt_names = (uint8_t)(e->num_alt_names > 255 ? 255 : e->num_alt_names);
        r->population = e->population;
        r->lat = e->centroid.lat;
        r->lon = e->centroid.lon;

        /* Deduplicated strings */
        r->name_offset = pool_add(&pool, e->name);
        r->poi_type_offset = pool_add(&pool, e->poi_type);
        r->housenumber_offset = pool_add(&pool, e->address.housenumber);
        r->street_offset = pool_add(&pool, e->address.street);
        r->city_offset = pool_add(&pool, e->address.city);
        r->postcode_offset = pool_add(&pool, e->address.postcode);
        r->state_offset = pool_add(&pool, e->address.state);
        r->country_offset = pool_add(&pool, e->address.country);
        r->country_code_offset = pool_add(&pool, e->address.country_code);

        /* Geometry */
        r->geometry_offset = serialize_entity_geometry(e, i, &geom_s);

        /* Alt names */
        r->alt_names_offset = alt_name_idx;
        for (uint16_t j = 0; j < e->num_alt_names && j < 255; j++) {
            alt_name_offsets[alt_name_idx++] = pool_add(&pool, e->alt_names[j]);
        }
    }

    /* Serialize trie */
    if (index->trie && index->trie->root) {
        serialize_trie_iterative(&ts, index->trie->root);
    }

    /* Serialize grid */
    serialize_grid(index->grid, &grid_s);

    /* Serialize n-grams */
    if (index->ngrams) {
        serialize_ngram(index->ngrams, &ns);
    }

    /* Calculate offsets */
    uint64_t offset = sizeof(LCBinaryHeaderV4) + sizeof(LCSectionOffsetsV4);

    LCSectionOffsetsV4 sections = {0};
    sections.entities_offset = offset;
    offset += index->num_entities * sizeof(LCBinaryEntityV4);

    sections.alt_names_offset = offset;
    offset += total_alt_names * sizeof(uint32_t);

    sections.string_pool_offset = offset;
    offset += pool.size;

    sections.trie_nodes_offset = offset;
    offset += ts.node_count * sizeof(LCBinaryTrieNode);

    sections.trie_entities_offset = offset;
    offset += ts.entity_count * sizeof(uint32_t);

    sections.grid_cells_offset = offset;
    offset += (grid_s.cell_count + 1) * sizeof(uint32_t);

    sections.grid_entities_offset = offset;
    offset += grid_s.entity_count * sizeof(uint32_t);

    sections.ngram_entries_offset = offset;
    offset += ns.entry_count * sizeof(LCBinaryNgramEntry);

    sections.ngram_entities_offset = offset;
    offset += ns.entity_count * sizeof(uint32_t);

    sections.geometry_offsets_offset = offset;
    offset += geom_s.offset_count * sizeof(LCBinaryGeometry);

    sections.geometry_points_offset = offset;

    /* Write header */
    LCBinaryHeaderV4 header = {
        .magic = LC_BINARY_MAGIC,
        .version = LC_BINARY_VERSION_V4,
        .entity_count = index->num_entities,
        .string_pool_size = (uint32_t)pool.size,
        .trie_node_count = ts.node_count,
        .trie_entity_count = ts.entity_count,
        .grid_width = index->grid ? (uint32_t)index->grid->grid_width : 0,
        .grid_height = index->grid ? (uint32_t)index->grid->grid_height : 0,
        .min_lat = index->bounds.min_lat,
        .min_lon = index->bounds.min_lon,
        .max_lat = index->bounds.max_lat,
        .max_lon = index->bounds.max_lon,
        .grid_cell_size = index->grid ? index->grid->cell_size_lat : LC_GRID_DEFAULT_CELL_SIZE,
        .ngram_entry_count = ns.entry_count,
        .ngram_entity_count = ns.entity_count,
        .geometry_count = geom_s.offset_count,
        .geometry_point_count = geom_s.point_count,
    };

    /* Write everything */
    if (fwrite(&header, sizeof(header), 1, f) != 1) goto error;
    if (fwrite(&sections, sizeof(sections), 1, f) != 1) goto error;
    if (fwrite(records, sizeof(LCBinaryEntityV4), index->num_entities, f) != index->num_entities) goto error;
    if (total_alt_names > 0 && fwrite(alt_name_offsets, sizeof(uint32_t), total_alt_names, f) != total_alt_names) goto error;
    if (pool.size > 0 && fwrite(pool.data, 1, pool.size, f) != pool.size) goto error;
    if (ts.node_count > 0 && fwrite(ts.nodes, sizeof(LCBinaryTrieNode), ts.node_count, f) != ts.node_count) goto error;
    if (ts.entity_count > 0 && fwrite(ts.entity_ids, sizeof(uint32_t), ts.entity_count, f) != ts.entity_count) goto error;
    if (grid_s.cell_count > 0 && fwrite(grid_s.cell_offsets, sizeof(uint32_t), grid_s.cell_count + 1, f) != grid_s.cell_count + 1) goto error;
    if (grid_s.entity_count > 0 && fwrite(grid_s.entity_ids, sizeof(uint32_t), grid_s.entity_count, f) != grid_s.entity_count) goto error;
    if (ns.entry_count > 0 && fwrite(ns.entries, sizeof(LCBinaryNgramEntry), ns.entry_count, f) != ns.entry_count) goto error;
    if (ns.entity_count > 0 && fwrite(ns.entity_ids, sizeof(uint32_t), ns.entity_count, f) != ns.entity_count) goto error;
    if (geom_s.offset_count > 0 && fwrite(geom_s.offsets, sizeof(LCBinaryGeometry), geom_s.offset_count, f) != geom_s.offset_count) goto error;
    if (geom_s.point_count > 0 && fwrite(geom_s.points, sizeof(LCBinaryPoint), geom_s.point_count, f) != geom_s.point_count) goto error;

    /* Cleanup */
    free(records);
    free(alt_name_offsets);
    pool_free(&pool);
    trie_serializer_free(&ts);
    free(grid_s.cell_offsets);
    free(grid_s.entity_ids);
    ngram_serializer_free(&ns);
    geometry_serializer_free(&geom_s);
    fclose(f);
    return LC_OK;

error:
    free(records);
    free(alt_name_offsets);
    pool_free(&pool);
    trie_serializer_free(&ts);
    free(grid_s.cell_offsets);
    free(grid_s.entity_ids);
    ngram_serializer_free(&ns);
    geometry_serializer_free(&geom_s);
    fclose(f);
    return LC_ERROR_INTERNAL;
}

/* ============================================================================
 * Deserialized Trie (mmap'd)
 * ============================================================================ */

typedef struct {
    const LCBinaryTrieNode *nodes;
    const uint32_t *entity_ids;
    uint32_t node_count;
} MmapTrie;

/* Collect results from serialized trie (DFS) */
static size_t mmap_trie_collect(const MmapTrie *mt, uint32_t node_idx,
                                 size_t max_results, uint32_t *results, size_t count)
{
    if (node_idx == NULL_OFFSET || count >= max_results) return count;

    const LCBinaryTrieNode *node = &mt->nodes[node_idx];

    /* Add this node's entities */
    for (uint16_t i = 0; i < node->entity_count && count < max_results; i++) {
        uint32_t eid = mt->entity_ids[node->entity_offset + i];
        /* Check for duplicates */
        int found = 0;
        for (size_t j = 0; j < count; j++) {
            if (results[j] == eid) { found = 1; break; }
        }
        if (!found) results[count++] = eid;
    }

    /* Recurse into children */
    for (int i = 0; i < LC_TRIE_ALPHABET_SIZE && count < max_results; i++) {
        if (node->children[i] != NULL_OFFSET) {
            count = mmap_trie_collect(mt, node->children[i], max_results, results, count);
        }
    }

    return count;
}

static size_t mmap_trie_search_prefix(const MmapTrie *mt, const char *prefix,
                                       size_t max_results, uint32_t *results)
{
    if (!mt || !mt->nodes || mt->node_count == 0 || !results) return 0;

    uint32_t node_idx = 0;  /* Start at root */

    /* Navigate to prefix node */
    if (prefix) {
        for (const char *p = prefix; *p; p++) {
            int idx = lc_trie_char_index(*p);
            if (idx < 0) continue;

            const LCBinaryTrieNode *node = &mt->nodes[node_idx];
            if (node->children[idx] == NULL_OFFSET) {
                return 0;  /* Prefix not found */
            }
            node_idx = node->children[idx];
        }
    }

    return mmap_trie_collect(mt, node_idx, max_results, results, 0);
}

static size_t mmap_trie_search_exact(const MmapTrie *mt, const char *name,
                                      size_t max_results, uint32_t *results)
{
    if (!mt || !mt->nodes || mt->node_count == 0 || !name || !results) return 0;

    uint32_t node_idx = 0;

    /* Navigate to exact node */
    for (const char *p = name; *p; p++) {
        int idx = lc_trie_char_index(*p);
        if (idx < 0) continue;

        const LCBinaryTrieNode *node = &mt->nodes[node_idx];
        if (node->children[idx] == NULL_OFFSET) {
            return 0;
        }
        node_idx = node->children[idx];
    }

    /* Return only entities at this exact node */
    const LCBinaryTrieNode *node = &mt->nodes[node_idx];
    size_t count = node->entity_count < max_results ? node->entity_count : max_results;
    memcpy(results, mt->entity_ids + node->entity_offset, count * sizeof(uint32_t));
    return count;
}

/* ============================================================================
 * Deserialized Grid (mmap'd)
 * ============================================================================ */

typedef struct {
    const uint32_t *cell_offsets;
    const uint32_t *entity_ids;
    uint32_t grid_width;
    uint32_t grid_height;
    double cell_size;
    SHBBox bounds;
} MmapGrid;

static size_t mmap_grid_query_point(const MmapGrid *mg, SHCoord coord,
                                     size_t max_results, uint32_t *results)
{
    if (!mg || !mg->cell_offsets || !results) return 0;

    /* Calculate cell index */
    int col = (int)((coord.lon - mg->bounds.min_lon) / mg->cell_size);
    int row = (int)((coord.lat - mg->bounds.min_lat) / mg->cell_size);

    if (col < 0 || col >= (int)mg->grid_width ||
        row < 0 || row >= (int)mg->grid_height) {
        return 0;
    }

    uint32_t cell_idx = (uint32_t)(row * mg->grid_width + col);
    uint32_t start = mg->cell_offsets[cell_idx];
    uint32_t end = mg->cell_offsets[cell_idx + 1];
    uint32_t count = end - start;

    if (count > max_results) count = (uint32_t)max_results;
    memcpy(results, mg->entity_ids + start, count * sizeof(uint32_t));
    return count;
}

/* ============================================================================
 * mmap Context for v3 (backwards compatibility)
 * ============================================================================ */

typedef struct {
    void *map_base;
    size_t map_size;
    int fd;
    MmapTrie trie;
    MmapGrid grid;
} LCMmapContextV3;

/* ============================================================================
 * Load Index via mmap (v4 - zero-copy)
 * ============================================================================ */

static LCIndex *lc_index_mmap_v4(const char *path, void *map, size_t file_size, int fd)
{
    (void)path;  /* Reserved for future use (error messages) */
    const LCBinaryHeaderV4 *header = (const LCBinaryHeaderV4 *)map;
    const LCSectionOffsetsV4 *sections = (const LCSectionOffsetsV4 *)((char *)map + sizeof(LCBinaryHeaderV4));

    /* Allocate mmap index */
    LCMmapIndex *mmap_idx = calloc(1, sizeof(LCMmapIndex));
    if (!mmap_idx) {
        munmap(map, file_size);
        close(fd);
        return NULL;
    }

    mmap_idx->map_base = map;
    mmap_idx->map_size = file_size;
    mmap_idx->fd = fd;
    mmap_idx->header = header;

    /* Set up direct pointers into mmap'd memory */
    mmap_idx->entities = (const LCBinaryEntityV4 *)((char *)map + sections->entities_offset);
    mmap_idx->alt_name_offsets = (const uint32_t *)((char *)map + sections->alt_names_offset);
    mmap_idx->string_pool = (const char *)map + sections->string_pool_offset;
    mmap_idx->trie_nodes = (const LCBinaryTrieNodeV4 *)((char *)map + sections->trie_nodes_offset);
    mmap_idx->trie_entity_ids = (const uint32_t *)((char *)map + sections->trie_entities_offset);
    mmap_idx->grid_cell_offsets = (const uint32_t *)((char *)map + sections->grid_cells_offset);
    mmap_idx->grid_entity_ids = (const uint32_t *)((char *)map + sections->grid_entities_offset);
    mmap_idx->ngram_entries = (const LCBinaryNgramEntry *)((char *)map + sections->ngram_entries_offset);
    mmap_idx->ngram_entity_ids = (const uint32_t *)((char *)map + sections->ngram_entities_offset);
    mmap_idx->geometry_offsets = (const LCBinaryGeometry *)((char *)map + sections->geometry_offsets_offset);
    mmap_idx->geometry_points = (const LCBinaryPoint *)((char *)map + sections->geometry_points_offset);

    /* Create index - NO entity store, NO n-gram index, true zero-copy! */
    LCIndex *index = calloc(1, sizeof(LCIndex));
    if (!index) {
        free(mmap_idx);
        munmap(map, file_size);
        close(fd);
        return NULL;
    }

    index->mmap_idx = mmap_idx;
    index->entities = NULL;  /* No entity store - use mmap accessors */
    index->trie = NULL;
    index->ngrams = NULL;    /* No n-gram index - use mmap'd n-grams */
    index->grid = NULL;
    index->mmap_ctx = NULL;  /* v3 context not used */

    index->num_entities = header->entity_count;
    index->bounds.min_lat = header->min_lat;
    index->bounds.min_lon = header->min_lon;
    index->bounds.max_lat = header->max_lat;
    index->bounds.max_lon = header->max_lon;

    return index;
}

/* ============================================================================
 * Load Index via mmap (v3 - with entity store repopulation)
 * ============================================================================ */

static LCIndex *lc_index_mmap_v3(const char *path, void *map, size_t file_size, int fd)
{
    (void)path;  /* Reserved for future use (error messages) */
    const LCBinaryHeaderV3 *header = (const LCBinaryHeaderV3 *)map;
    const LCSectionOffsetsV3 *sections = (const LCSectionOffsetsV3 *)((char *)map + sizeof(LCBinaryHeaderV3));

    /* Get pointers into mapped memory */
    const LCBinaryEntityV3 *records = (const LCBinaryEntityV3 *)((char *)map + sections->entities_offset);
    const uint32_t *alt_name_offsets = (const uint32_t *)((char *)map + sections->alt_names_offset);
    const char *string_pool = (const char *)map + sections->string_pool_offset;
    const LCBinaryTrieNode *trie_nodes = (const LCBinaryTrieNode *)((char *)map + sections->trie_nodes_offset);
    const uint32_t *trie_entity_ids = (const uint32_t *)((char *)map + sections->trie_entities_offset);
    const uint32_t *grid_cell_offsets = (const uint32_t *)((char *)map + sections->grid_cells_offset);
    const uint32_t *grid_entity_ids = (const uint32_t *)((char *)map + sections->grid_entities_offset);

    /* Create entity store */
    LCEntityStore *store = calloc(1, sizeof(LCEntityStore));
    if (!store) goto error;

    store->entities = calloc(header->entity_count, sizeof(LCEntity));
    if (!store->entities) {
        free(store);
        goto error;
    }
    store->count = header->entity_count;
    store->capacity = header->entity_count;

    /* Helper to get string from pool */
    #define GET_STRING(offset) ((offset) == NULL_OFFSET ? NULL : (char *)(string_pool + (offset)))

    /* Populate entities */
    for (uint32_t i = 0; i < header->entity_count; i++) {
        const LCBinaryEntityV3 *r = &records[i];
        LCEntity *e = &store->entities[i];

        e->osm_id = r->osm_id;
        e->type = (LCEntityType)r->type;
        e->fclass = (LCFeatureClass)r->fclass;
        e->admin_level = r->admin_level;
        e->population = r->population;
        e->centroid.lat = r->lat;
        e->centroid.lon = r->lon;

        e->name = GET_STRING(r->name_offset);
        e->poi_type = GET_STRING(r->poi_type_offset);

        /* Alt names */
        e->num_alt_names = r->num_alt_names;
        if (r->num_alt_names > 0) {
            e->alt_names = calloc(r->num_alt_names, sizeof(char *));
            if (e->alt_names) {
                for (uint8_t j = 0; j < r->num_alt_names; j++) {
                    uint32_t off = alt_name_offsets[r->alt_names_offset + j];
                    e->alt_names[j] = GET_STRING(off);
                }
            }
        }

        e->address.housenumber = GET_STRING(r->housenumber_offset);
        e->address.street = GET_STRING(r->street_offset);
        e->address.city = GET_STRING(r->city_offset);
        e->address.postcode = GET_STRING(r->postcode_offset);
        e->address.state = GET_STRING(r->state_offset);
        e->address.country = GET_STRING(r->country_offset);
        e->address.country_code = GET_STRING(r->country_code_offset);
    }
    #undef GET_STRING

    /* Create mmap context */
    LCMmapContextV3 *ctx = calloc(1, sizeof(LCMmapContextV3));
    if (!ctx) goto error_store;

    ctx->map_base = map;
    ctx->map_size = file_size;
    ctx->fd = fd;

    /* Setup mmap'd trie */
    ctx->trie.nodes = trie_nodes;
    ctx->trie.entity_ids = trie_entity_ids;
    ctx->trie.node_count = header->trie_node_count;

    /* Setup mmap'd grid */
    ctx->grid.cell_offsets = grid_cell_offsets;
    ctx->grid.entity_ids = grid_entity_ids;
    ctx->grid.grid_width = header->grid_width;
    ctx->grid.grid_height = header->grid_height;
    ctx->grid.cell_size = header->grid_cell_size;
    ctx->grid.bounds.min_lat = header->min_lat;
    ctx->grid.bounds.min_lon = header->min_lon;
    ctx->grid.bounds.max_lat = header->max_lat;
    ctx->grid.bounds.max_lon = header->max_lon;

    /* Create index - but don't rebuild trie/grid! */
    LCIndex *index = calloc(1, sizeof(LCIndex));
    if (!index) goto error_ctx;

    index->entities = store;
    index->num_entities = header->entity_count;
    index->bounds.min_lat = header->min_lat;
    index->bounds.min_lon = header->min_lon;
    index->bounds.max_lat = header->max_lat;
    index->bounds.max_lon = header->max_lon;
    index->mmap_ctx = ctx;
    index->mmap_idx = NULL;

    /* Trie and grid are accessed via mmap context, not these pointers */
    index->trie = NULL;
    index->grid = NULL;

    /* Create n-gram index (needs to be rebuilt - not serialized in v3) */
    index->ngrams = lc_ngram_create();
    if (!index->ngrams) goto error_index;

    /* Index entities in n-gram - use stack buffer for speed */
    char norm_buf[512];
    for (uint32_t i = 0; i < store->count; i++) {
        const LCEntity *e = &store->entities[i];
        if (e->name && e->name[0]) {
            size_t len = lc_normalize_to(e->name, norm_buf, sizeof(norm_buf));
            if (len > 0 && len < sizeof(norm_buf)) {
                lc_ngram_index_name(index->ngrams, norm_buf, i);
            }
            for (uint16_t j = 0; j < e->num_alt_names; j++) {
                if (e->alt_names[j]) {
                    len = lc_normalize_to(e->alt_names[j], norm_buf, sizeof(norm_buf));
                    if (len > 0 && len < sizeof(norm_buf)) {
                        lc_ngram_index_name(index->ngrams, norm_buf, i);
                    }
                }
            }
        }
    }
    lc_ngram_build(index->ngrams);

    return index;

error_index:
    free(index);
error_ctx:
    free(ctx);
error_store:
    for (uint32_t i = 0; i < store->count; i++) {
        free(store->entities[i].alt_names);
    }
    free(store->entities);
    free(store);
error:
    munmap(map, file_size);
    close(fd);
    return NULL;
}

/* ============================================================================
 * Load Index via mmap (dispatcher)
 * ============================================================================ */

LCIndex *lc_index_mmap(const char *path)
{
    if (!path) return NULL;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return NULL;
    }

    size_t file_size = (size_t)st.st_size;
    void *map = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    madvise(map, file_size, MADV_SEQUENTIAL);

    /* Check magic and version */
    const uint32_t *magic_ptr = (const uint32_t *)map;
    if (*magic_ptr != LC_BINARY_MAGIC) {
        munmap(map, file_size);
        close(fd);
        return NULL;
    }

    uint32_t version = magic_ptr[1];

    /* Handle by version */
    if (version >= LC_BINARY_VERSION_V4) {
        return lc_index_mmap_v4(path, map, file_size, fd);
    } else if (version >= LC_BINARY_VERSION_V3) {
        return lc_index_mmap_v3(path, map, file_size, fd);
    } else {
        /* v1/v2 - fall back to old loader */
        munmap(map, file_size);
        close(fd);
        return lc_index_load(path);
    }
}

/* ============================================================================
 * Load Index (dispatcher)
 * ============================================================================ */

LCIndex *lc_index_load(const char *path)
{
    if (!path) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    uint32_t magic, version;
    if (fread(&magic, 4, 1, f) != 1 || fread(&version, 4, 1, f) != 1) {
        fclose(f);
        return NULL;
    }
    fclose(f);

    if (magic != LC_BINARY_MAGIC) return NULL;

    /* Use mmap for v2+ */
    if (version >= 2) {
        return lc_index_mmap(path);
    }

    /* v1 format - old loader */
    f = fopen(path, "rb");
    if (!f) return NULL;

    /* Skip magic/version already read */
    fseek(f, 8, SEEK_SET);

    uint32_t entity_count;
    double min_lat, min_lon, max_lat, max_lon;

    if (fread(&entity_count, 4, 1, f) != 1 ||
        fread(&min_lat, 8, 1, f) != 1 ||
        fread(&min_lon, 8, 1, f) != 1 ||
        fread(&max_lat, 8, 1, f) != 1 ||
        fread(&max_lon, 8, 1, f) != 1) {
        fclose(f);
        return NULL;
    }

    LCEntityStore *store = lc_entity_store_create(entity_count > 0 ? entity_count : 1024);
    if (!store) {
        fclose(f);
        return NULL;
    }

    /* Read v1 entities... (simplified) */
    for (uint32_t i = 0; i < entity_count; i++) {
        LCEntity e;
        lc_entity_init(&e);

        uint8_t type_u8, fclass_u8;
        uint16_t num_alts;

        if (fread(&e.osm_id, 8, 1, f) != 1 ||
            fread(&type_u8, 1, 1, f) != 1 ||
            fread(&fclass_u8, 1, 1, f) != 1 ||
            fread(&e.admin_level, 1, 1, f) != 1 ||
            fread(&e.population, 4, 1, f) != 1 ||
            fread(&e.centroid.lat, 8, 1, f) != 1 ||
            fread(&e.centroid.lon, 8, 1, f) != 1) {
            lc_entity_store_free(store);
            fclose(f);
            return NULL;
        }

        e.type = (LCEntityType)type_u8;
        e.fclass = (LCFeatureClass)fclass_u8;

        #define READ_STRING(var) do { \
            uint16_t len; \
            if (fread(&len, 2, 1, f) != 1) goto v1_error; \
            if (len > 0) { \
                var = malloc(len + 1); \
                if (!var || fread(var, 1, len, f) != len) goto v1_error; \
                var[len] = '\0'; \
            } \
        } while(0)

        READ_STRING(e.name);

        if (fread(&num_alts, 2, 1, f) != 1) goto v1_error;
        e.num_alt_names = num_alts;
        if (num_alts > 0) {
            e.alt_names = calloc(num_alts, sizeof(char *));
            for (uint16_t j = 0; j < num_alts; j++) {
                READ_STRING(e.alt_names[j]);
            }
        }

        READ_STRING(e.poi_type);
        READ_STRING(e.address.housenumber);
        READ_STRING(e.address.street);
        READ_STRING(e.address.city);
        READ_STRING(e.address.postcode);
        READ_STRING(e.address.state);
        READ_STRING(e.address.country);
        READ_STRING(e.address.country_code);

        #undef READ_STRING

        lc_entity_store_add(store, &e);
        continue;

    v1_error:
        lc_entity_free(&e);
        lc_entity_store_free(store);
        fclose(f);
        return NULL;
    }

    fclose(f);

    LCIndex *index = lc_index_create();
    if (!index) {
        lc_entity_store_free(store);
        return NULL;
    }

    if (lc_index_build(index, store) != LC_OK) {
        lc_index_free(index);
        return NULL;
    }

    return index;
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

int lc_is_binary_index(const char *path)
{
    if (!path) return 0;

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    uint32_t magic;
    int result = (fread(&magic, 4, 1, f) == 1) && (magic == LC_BINARY_MAGIC);
    fclose(f);
    return result;
}

uint32_t lc_binary_version(const char *path)
{
    if (!path) return 0;

    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    uint32_t magic, version = 0;
    if (fread(&magic, 4, 1, f) == 1 && magic == LC_BINARY_MAGIC) {
        fread(&version, 4, 1, f);
    }
    fclose(f);
    return version;
}

/* ============================================================================
 * Public mmap'd Index Search Functions (v3 compatibility)
 * ============================================================================ */

size_t lc_mmap_trie_search_exact(void *mmap_ctx, const char *name,
                                  size_t max_results, uint32_t *results)
{
    if (!mmap_ctx || !name || !results) return 0;
    LCMmapContextV3 *ctx = (LCMmapContextV3 *)mmap_ctx;
    return mmap_trie_search_exact(&ctx->trie, name, max_results, results);
}

size_t lc_mmap_trie_search_prefix(void *mmap_ctx, const char *prefix,
                                   size_t max_results, uint32_t *results)
{
    if (!mmap_ctx || !results) return 0;
    LCMmapContextV3 *ctx = (LCMmapContextV3 *)mmap_ctx;
    return mmap_trie_search_prefix(&ctx->trie, prefix, max_results, results);
}

size_t lc_mmap_grid_query_point(void *mmap_ctx, SHCoord coord,
                                 size_t max_results, uint32_t *results)
{
    if (!mmap_ctx || !results) return 0;
    LCMmapContextV3 *ctx = (LCMmapContextV3 *)mmap_ctx;
    return mmap_grid_query_point(&ctx->grid, coord, max_results, results);
}

/* Comparator for sorting nearest results by distance */
static int nearest_compare(const void *a, const void *b)
{
    const LCNearestResult *na = (const LCNearestResult *)a;
    const LCNearestResult *nb = (const LCNearestResult *)b;
    if (na->distance_m < nb->distance_m) return -1;
    if (na->distance_m > nb->distance_m) return 1;
    return 0;
}

size_t lc_mmap_grid_find_nearest(void *mmap_ctx, const LCEntityStore *store,
                                  SHCoord coord, size_t max_results,
                                  LCNearestResult *results)
{
    if (!mmap_ctx || !store || !results || max_results == 0) return 0;

    LCMmapContextV3 *ctx = (LCMmapContextV3 *)mmap_ctx;
    const MmapGrid *mg = &ctx->grid;

    if (!mg->cell_offsets) return 0;

    /* Search in a 3x3 area around the query point */
    int center_col = (int)((coord.lon - mg->bounds.min_lon) / mg->cell_size);
    int center_row = (int)((coord.lat - mg->bounds.min_lat) / mg->cell_size);

    /* Collect candidates from neighboring cells */
    size_t candidate_capacity = 256;
    LCNearestResult *candidates = malloc(candidate_capacity * sizeof(LCNearestResult));
    if (!candidates) return 0;

    size_t candidate_count = 0;

    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            int row = center_row + dr;
            int col = center_col + dc;

            if (col < 0 || col >= (int)mg->grid_width ||
                row < 0 || row >= (int)mg->grid_height) {
                continue;
            }

            uint32_t cell_idx = (uint32_t)(row * mg->grid_width + col);
            uint32_t start = mg->cell_offsets[cell_idx];
            uint32_t end = mg->cell_offsets[cell_idx + 1];

            for (uint32_t i = start; i < end; i++) {
                uint32_t eid = mg->entity_ids[i];
                if (eid >= store->count) continue;

                /* Calculate distance using geometry if available */
                const LCEntity *e = &store->entities[eid];
                double dist = lc_point_to_entity_distance(coord, e);

                /* Add to candidates */
                if (candidate_count >= candidate_capacity) {
                    candidate_capacity *= 2;
                    LCNearestResult *new_candidates = realloc(candidates,
                        candidate_capacity * sizeof(LCNearestResult));
                    if (!new_candidates) {
                        free(candidates);
                        return 0;
                    }
                    candidates = new_candidates;
                }

                candidates[candidate_count].entity_id = eid;
                candidates[candidate_count].distance_m = dist;
                candidate_count++;
            }
        }
    }

    if (candidate_count == 0) {
        free(candidates);
        return 0;
    }

    /* Sort by distance */
    qsort(candidates, candidate_count, sizeof(LCNearestResult), nearest_compare);

    /* Copy top results */
    size_t result_count = candidate_count < max_results ? candidate_count : max_results;
    memcpy(results, candidates, result_count * sizeof(LCNearestResult));

    free(candidates);
    return result_count;
}

int lc_index_is_mmap(const LCIndex *index)
{
    return index && (index->mmap_ctx != NULL || index->mmap_idx != NULL);
}

/* ============================================================================
 * v4 mmap'd Search Functions
 * ============================================================================ */

/* Trie search on v4 mmap'd index */
size_t lc_mmap_v4_trie_search_exact(const LCMmapIndex *idx, const char *name,
                                     size_t max_results, uint32_t *results)
{
    if (!idx || !name || !results || idx->header->trie_node_count == 0) return 0;

    MmapTrie mt = {
        .nodes = (const LCBinaryTrieNode *)idx->trie_nodes,
        .entity_ids = idx->trie_entity_ids,
        .node_count = idx->header->trie_node_count
    };
    return mmap_trie_search_exact(&mt, name, max_results, results);
}

size_t lc_mmap_v4_trie_search_prefix(const LCMmapIndex *idx, const char *prefix,
                                      size_t max_results, uint32_t *results)
{
    if (!idx || !results || idx->header->trie_node_count == 0) return 0;

    MmapTrie mt = {
        .nodes = (const LCBinaryTrieNode *)idx->trie_nodes,
        .entity_ids = idx->trie_entity_ids,
        .node_count = idx->header->trie_node_count
    };
    return mmap_trie_search_prefix(&mt, prefix, max_results, results);
}

/* N-gram search on v4 mmap'd index using binary search */
static int ngram_compare(const void *key, const void *entry) {
    uint32_t trigram = *(const uint32_t *)key;
    const LCBinaryNgramEntry *e = (const LCBinaryNgramEntry *)entry;
    if (trigram < e->trigram) return -1;
    if (trigram > e->trigram) return 1;
    return 0;
}

size_t lc_mmap_v4_ngram_search(const LCMmapIndex *idx, const char *query,
                                float threshold, size_t max_results, LCFuzzyMatch *results)
{
    if (!idx || !query || !results || idx->header->ngram_entry_count == 0) return 0;

    /* Generate trigrams from query */
    char trigrams[128][4];
    size_t num_trigrams = lc_ngram_generate(query, trigrams, 128);
    if (num_trigrams == 0) return 0;

    /* Count hits per entity */
    uint32_t *hit_counts = calloc(idx->header->entity_count, sizeof(uint32_t));
    if (!hit_counts) return 0;

    /* Look up each trigram using binary search */
    for (size_t i = 0; i < num_trigrams; i++) {
        uint32_t packed = lc_pack_trigram(trigrams[i]);
        const LCBinaryNgramEntry *entry = bsearch(&packed, idx->ngram_entries,
            idx->header->ngram_entry_count, sizeof(LCBinaryNgramEntry), ngram_compare);

        if (entry) {
            for (uint16_t j = 0; j < entry->entity_count; j++) {
                uint32_t eid = idx->ngram_entity_ids[entry->entity_offset + j];
                if (eid < idx->header->entity_count) {
                    hit_counts[eid]++;
                }
            }
        }
    }

    /* Build results for entities meeting threshold */
    size_t count = 0;
    for (uint32_t eid = 0; eid < idx->header->entity_count && count < max_results; eid++) {
        if (hit_counts[eid] > 0) {
            float score = (float)hit_counts[eid] / (float)num_trigrams;
            if (score >= threshold) {
                results[count].entity_id = eid;
                results[count].score = score;
                count++;
            }
        }
    }

    free(hit_counts);

    /* Sort by score descending */
    if (count > 1) {
        for (size_t i = 0; i < count - 1; i++) {
            for (size_t j = i + 1; j < count; j++) {
                if (results[j].score > results[i].score) {
                    LCFuzzyMatch tmp = results[i];
                    results[i] = results[j];
                    results[j] = tmp;
                }
            }
        }
    }

    return count;
}

/* Grid find nearest on v4 mmap'd index with geometry support */
size_t lc_mmap_v4_grid_find_nearest(const LCMmapIndex *idx, SHCoord coord,
                                     size_t max_results, LCNearestResult *results)
{
    if (!idx || !results || max_results == 0) return 0;
    if (idx->header->grid_width == 0 || idx->header->grid_height == 0) return 0;

    const LCBinaryHeaderV4 *h = idx->header;
    double cell_size = h->grid_cell_size;
    SHBBox bounds = { .min_lat = h->min_lat, .min_lon = h->min_lon,
                      .max_lat = h->max_lat, .max_lon = h->max_lon };

    /* Calculate cell index */
    int center_col = (int)((coord.lon - bounds.min_lon) / cell_size);
    int center_row = (int)((coord.lat - bounds.min_lat) / cell_size);

    /* Collect candidates from neighboring cells */
    size_t candidate_capacity = 256;
    LCNearestResult *candidates = malloc(candidate_capacity * sizeof(LCNearestResult));
    if (!candidates) return 0;

    size_t candidate_count = 0;

    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            int row = center_row + dr;
            int col = center_col + dc;

            if (col < 0 || col >= (int)h->grid_width ||
                row < 0 || row >= (int)h->grid_height) {
                continue;
            }

            uint32_t cell_idx = (uint32_t)(row * h->grid_width + col);
            uint32_t start = idx->grid_cell_offsets[cell_idx];
            uint32_t end = idx->grid_cell_offsets[cell_idx + 1];

            for (uint32_t i = start; i < end; i++) {
                uint32_t eid = idx->grid_entity_ids[i];
                if (eid >= h->entity_count) continue;

                /* Calculate distance using mmap'd geometry if available */
                double dist = lc_mmap_point_to_entity_distance(idx, eid, coord);

                /* Add to candidates */
                if (candidate_count >= candidate_capacity) {
                    candidate_capacity *= 2;
                    LCNearestResult *new_candidates = realloc(candidates,
                        candidate_capacity * sizeof(LCNearestResult));
                    if (!new_candidates) {
                        free(candidates);
                        return 0;
                    }
                    candidates = new_candidates;
                }

                candidates[candidate_count].entity_id = eid;
                candidates[candidate_count].distance_m = dist;
                candidate_count++;
            }
        }
    }

    if (candidate_count == 0) {
        free(candidates);
        return 0;
    }

    /* Sort by distance */
    qsort(candidates, candidate_count, sizeof(LCNearestResult), nearest_compare);

    /* Copy top results */
    size_t result_count = candidate_count < max_results ? candidate_count : max_results;
    memcpy(results, candidates, result_count * sizeof(LCNearestResult));

    free(candidates);
    return result_count;
}

/* ============================================================================
 * Geometry Distance for mmap'd v4 index
 * ============================================================================ */

/* Point-to-segment distance using mmap'd geometry */
static double mmap_point_to_segment_distance(SHCoord point, SHCoord seg_a, SHCoord seg_b)
{
    double dx = seg_b.lon - seg_a.lon;
    double dy = seg_b.lat - seg_a.lat;
    double seg_len_sq = dx * dx + dy * dy;

    if (seg_len_sq < 1e-14) {
        return sh_haversine(point, seg_a);
    }

    double px = point.lon - seg_a.lon;
    double py = point.lat - seg_a.lat;
    double t = (px * dx + py * dy) / seg_len_sq;

    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    SHCoord closest = {
        .lat = seg_a.lat + t * dy,
        .lon = seg_a.lon + t * dx
    };

    return sh_haversine(point, closest);
}

double lc_mmap_point_to_entity_distance(const LCMmapIndex *idx, uint32_t entity_idx, SHCoord point)
{
    if (!idx || entity_idx >= idx->header->entity_count) return -1.0;

    const LCBinaryEntityV4 *e = &idx->entities[entity_idx];

    /* For streets with geometry, use line distance */
    if (e->fclass == LC_CLASS_STREET && e->geometry_offset != LC_MMAP_NULL_OFFSET) {
        const LCBinaryGeometry *geom = &idx->geometry_offsets[e->geometry_offset];

        if (geom->point_count < 2) {
            /* Fall back to centroid */
            SHCoord centroid = { .lat = e->lat, .lon = e->lon };
            return sh_haversine(point, centroid);
        }

        /* Find minimum distance to any segment */
        SHCoord p0 = lc_mmap_geometry_point(idx, entity_idx, 0);
        SHCoord p1 = lc_mmap_geometry_point(idx, entity_idx, 1);
        double min_dist = mmap_point_to_segment_distance(point, p0, p1);

        for (uint32_t i = 1; i < geom->point_count - 1; i++) {
            p0 = lc_mmap_geometry_point(idx, entity_idx, i);
            p1 = lc_mmap_geometry_point(idx, entity_idx, i + 1);
            double dist = mmap_point_to_segment_distance(point, p0, p1);
            if (dist < min_dist) {
                min_dist = dist;
            }
        }

        return min_dist;
    }

    /* Fall back to centroid distance */
    SHCoord centroid = { .lat = e->lat, .lon = e->lon };
    return sh_haversine(point, centroid);
}
