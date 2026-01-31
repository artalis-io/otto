/*
 * lc_serialize.c - Binary Index Serialization with mmap Support
 *
 * Binary format v2 (mmap-friendly):
 *
 * [Header]
 *   magic: u32 (0x4C4F4355 = "LOCU")
 *   version: u32 (2)
 *   entity_count: u32
 *   string_pool_size: u32
 *   bounds: 4 x f64 (min_lat, min_lon, max_lat, max_lon)
 *
 * [Entity Records] - fixed 64 bytes each
 *   osm_id: u64
 *   type: u8
 *   fclass: u8
 *   admin_level: i8
 *   num_alt_names: u8
 *   population: i32
 *   lat: f64
 *   lon: f64
 *   name_offset: u32 (0xFFFFFFFF = null)
 *   poi_type_offset: u32
 *   alt_names_offset: u32
 *   housenumber_offset: u32
 *   street_offset: u32
 *   city_offset: u32
 *   postcode_offset: u32
 *   state_offset: u32
 *   country_offset: u32
 *   country_code_offset: u32
 *
 * [Alt Name Offsets]
 *   For each entity with alt names: num_alt_names x u32 offsets
 *
 * [String Pool]
 *   All strings concatenated, null-terminated
 */

#include "lc_serialize.h"
#include "lc_normalize.h"
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
    uint32_t entity_count;
    uint32_t string_pool_size;
    double min_lat;
    double min_lon;
    double max_lat;
    double max_lon;
    uint32_t alt_names_offset;  /* Offset to alt names array */
    uint32_t string_pool_offset; /* Offset to string pool */
} LCBinaryHeader;

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
    uint32_t alt_names_offset;  /* Index into alt names array */
    uint32_t housenumber_offset;
    uint32_t street_offset;
    uint32_t city_offset;
    uint32_t postcode_offset;
    uint32_t state_offset;
    uint32_t country_offset;
    uint32_t country_code_offset;
} LCBinaryEntity;

/* ============================================================================
 * String Pool Builder
 * ============================================================================ */

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} StringPool;

static void pool_init(StringPool *pool) {
    pool->data = NULL;
    pool->size = 0;
    pool->capacity = 0;
}

static void pool_free(StringPool *pool) {
    free(pool->data);
    pool->data = NULL;
    pool->size = 0;
    pool->capacity = 0;
}

static uint32_t pool_add(StringPool *pool, const char *str) {
    if (!str) return NULL_OFFSET;

    size_t len = strlen(str) + 1;  /* Include null terminator */

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
    return offset;
}

/* ============================================================================
 * Save Index (v2 format)
 * ============================================================================ */

LCStatus lc_index_save(const LCIndex *index, const char *path)
{
    if (!index || !path) return LC_ERROR_INVALID_PARAM;
    if (!index->entities) return LC_ERROR_INVALID_PARAM;

    FILE *f = fopen(path, "wb");
    if (!f) return LC_ERROR_FILE_NOT_FOUND;

    /* Build string pool and alt names array */
    StringPool pool;
    pool_init(&pool);

    /* Count total alt names */
    uint32_t total_alt_names = 0;
    for (uint32_t i = 0; i < index->num_entities; i++) {
        total_alt_names += index->entities->entities[i].num_alt_names;
    }

    /* Allocate alt names offset array */
    uint32_t *alt_name_offsets = NULL;
    if (total_alt_names > 0) {
        alt_name_offsets = malloc(total_alt_names * sizeof(uint32_t));
        if (!alt_name_offsets) {
            pool_free(&pool);
            fclose(f);
            return LC_ERROR_OUT_OF_MEMORY;
        }
    }

    /* Build entity records */
    LCBinaryEntity *records = malloc(index->num_entities * sizeof(LCBinaryEntity));
    if (!records) {
        free(alt_name_offsets);
        pool_free(&pool);
        fclose(f);
        return LC_ERROR_OUT_OF_MEMORY;
    }

    uint32_t alt_name_idx = 0;
    for (uint32_t i = 0; i < index->num_entities; i++) {
        const LCEntity *e = &index->entities->entities[i];
        LCBinaryEntity *r = &records[i];

        r->osm_id = e->osm_id;
        r->type = (uint8_t)e->type;
        r->fclass = (uint8_t)e->fclass;
        r->admin_level = e->admin_level;
        r->num_alt_names = (uint8_t)(e->num_alt_names > 255 ? 255 : e->num_alt_names);
        r->population = e->population;
        r->lat = e->centroid.lat;
        r->lon = e->centroid.lon;

        r->name_offset = pool_add(&pool, e->name);
        r->poi_type_offset = pool_add(&pool, e->poi_type);

        /* Alt names */
        r->alt_names_offset = alt_name_idx;
        for (uint16_t j = 0; j < e->num_alt_names && j < 255; j++) {
            alt_name_offsets[alt_name_idx++] = pool_add(&pool, e->alt_names[j]);
        }

        /* Address fields */
        r->housenumber_offset = pool_add(&pool, e->address.housenumber);
        r->street_offset = pool_add(&pool, e->address.street);
        r->city_offset = pool_add(&pool, e->address.city);
        r->postcode_offset = pool_add(&pool, e->address.postcode);
        r->state_offset = pool_add(&pool, e->address.state);
        r->country_offset = pool_add(&pool, e->address.country);
        r->country_code_offset = pool_add(&pool, e->address.country_code);
    }

    /* Calculate offsets */
    size_t header_size = sizeof(LCBinaryHeader);
    size_t entities_size = index->num_entities * sizeof(LCBinaryEntity);
    size_t alt_names_size = total_alt_names * sizeof(uint32_t);

    /* Write header */
    LCBinaryHeader header = {
        .magic = LC_BINARY_MAGIC,
        .version = LC_BINARY_VERSION,
        .entity_count = index->num_entities,
        .string_pool_size = (uint32_t)pool.size,
        .min_lat = index->bounds.min_lat,
        .min_lon = index->bounds.min_lon,
        .max_lat = index->bounds.max_lat,
        .max_lon = index->bounds.max_lon,
        .alt_names_offset = (uint32_t)(header_size + entities_size),
        .string_pool_offset = (uint32_t)(header_size + entities_size + alt_names_size)
    };

    if (fwrite(&header, sizeof(header), 1, f) != 1) goto write_error;
    if (fwrite(records, sizeof(LCBinaryEntity), index->num_entities, f) != index->num_entities) goto write_error;
    if (total_alt_names > 0 && fwrite(alt_name_offsets, sizeof(uint32_t), total_alt_names, f) != total_alt_names) goto write_error;
    if (pool.size > 0 && fwrite(pool.data, 1, pool.size, f) != pool.size) goto write_error;

    free(records);
    free(alt_name_offsets);
    pool_free(&pool);
    fclose(f);
    return LC_OK;

write_error:
    free(records);
    free(alt_name_offsets);
    pool_free(&pool);
    fclose(f);
    return LC_ERROR_INTERNAL;
}

/* ============================================================================
 * Load Index via mmap
 * ============================================================================ */

/* Context for mmap'd index */
typedef struct {
    void *map_base;
    size_t map_size;
    int fd;
} LCMmapContext;

static const char *get_string(const char *pool, uint32_t offset) {
    if (offset == NULL_OFFSET) return NULL;
    return pool + offset;
}

static void mmap_index_free(LCIndex *index) {
    if (!index) return;

    LCMmapContext *ctx = (LCMmapContext *)index->mmap_ctx;
    if (ctx) {
        if (ctx->map_base && ctx->map_base != MAP_FAILED) {
            munmap(ctx->map_base, ctx->map_size);
        }
        if (ctx->fd >= 0) {
            close(ctx->fd);
        }
        free(ctx);
    }

    /* Free entities (shallow - strings point to mmap) */
    if (index->entities) {
        free(index->entities->entities);
        free(index->entities);
    }

    lc_trie_free(index->trie);
    lc_ngram_free(index->ngrams);
    lc_grid_free(index->grid);
    free(index);
}

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
    if (file_size < sizeof(LCBinaryHeader)) {
        close(fd);
        return NULL;
    }

    void *map = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    /* Advise kernel we'll read sequentially */
    madvise(map, file_size, MADV_SEQUENTIAL);

    const LCBinaryHeader *header = (const LCBinaryHeader *)map;

    /* Validate header */
    if (header->magic != LC_BINARY_MAGIC) {
        munmap(map, file_size);
        close(fd);
        return NULL;
    }

    /* Handle version 1 (old format) - fall back to regular load */
    if (header->version == 1) {
        munmap(map, file_size);
        close(fd);
        return lc_index_load(path);
    }

    if (header->version != LC_BINARY_VERSION) {
        munmap(map, file_size);
        close(fd);
        return NULL;
    }

    /* Get pointers into mapped memory */
    const LCBinaryEntity *records = (const LCBinaryEntity *)((char *)map + sizeof(LCBinaryHeader));
    const uint32_t *alt_name_offsets = (const uint32_t *)((char *)map + header->alt_names_offset);
    const char *string_pool = (const char *)map + header->string_pool_offset;

    /* Create entity store */
    LCEntityStore *store = calloc(1, sizeof(LCEntityStore));
    if (!store) {
        munmap(map, file_size);
        close(fd);
        return NULL;
    }

    store->entities = calloc(header->entity_count, sizeof(LCEntity));
    if (!store->entities) {
        free(store);
        munmap(map, file_size);
        close(fd);
        return NULL;
    }
    store->count = header->entity_count;
    store->capacity = header->entity_count;

    /* Populate entities from mapped data */
    for (uint32_t i = 0; i < header->entity_count; i++) {
        const LCBinaryEntity *r = &records[i];
        LCEntity *e = &store->entities[i];

        e->osm_id = r->osm_id;
        e->type = (LCEntityType)r->type;
        e->fclass = (LCFeatureClass)r->fclass;
        e->admin_level = r->admin_level;
        e->population = r->population;
        e->centroid.lat = r->lat;
        e->centroid.lon = r->lon;

        /* Strings point directly into mmap'd region (cast away const for storage) */
        e->name = (char *)get_string(string_pool, r->name_offset);
        e->poi_type = (char *)get_string(string_pool, r->poi_type_offset);

        /* Alt names */
        e->num_alt_names = r->num_alt_names;
        if (r->num_alt_names > 0) {
            e->alt_names = calloc(r->num_alt_names, sizeof(char *));
            if (e->alt_names) {
                for (uint8_t j = 0; j < r->num_alt_names; j++) {
                    uint32_t off = alt_name_offsets[r->alt_names_offset + j];
                    e->alt_names[j] = (char *)get_string(string_pool, off);
                }
            }
        }

        /* Address fields */
        e->address.housenumber = (char *)get_string(string_pool, r->housenumber_offset);
        e->address.street = (char *)get_string(string_pool, r->street_offset);
        e->address.city = (char *)get_string(string_pool, r->city_offset);
        e->address.postcode = (char *)get_string(string_pool, r->postcode_offset);
        e->address.state = (char *)get_string(string_pool, r->state_offset);
        e->address.country = (char *)get_string(string_pool, r->country_offset);
        e->address.country_code = (char *)get_string(string_pool, r->country_code_offset);
    }

    /* Create mmap context */
    LCMmapContext *ctx = malloc(sizeof(LCMmapContext));
    if (!ctx) {
        for (uint32_t i = 0; i < header->entity_count; i++) {
            free(store->entities[i].alt_names);
        }
        free(store->entities);
        free(store);
        munmap(map, file_size);
        close(fd);
        return NULL;
    }
    ctx->map_base = map;
    ctx->map_size = file_size;
    ctx->fd = fd;

    /* Build index */
    LCIndex *index = lc_index_create();
    if (!index) {
        free(ctx);
        for (uint32_t i = 0; i < header->entity_count; i++) {
            free(store->entities[i].alt_names);
        }
        free(store->entities);
        free(store);
        munmap(map, file_size);
        close(fd);
        return NULL;
    }

    /* Store mmap context and override free function */
    index->mmap_ctx = ctx;

    /* Build indexes */
    if (lc_index_build(index, store) != LC_OK) {
        mmap_index_free(index);
        return NULL;
    }

    return index;
}

/* ============================================================================
 * Load Index (traditional file I/O - for v1 compatibility)
 * ============================================================================ */

LCIndex *lc_index_load(const char *path)
{
    if (!path) return NULL;

    /* Try mmap first for v2 files */
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    uint32_t magic, version;
    if (fread(&magic, 4, 1, f) != 1 || fread(&version, 4, 1, f) != 1) {
        fclose(f);
        return NULL;
    }

    if (magic != LC_BINARY_MAGIC) {
        fclose(f);
        return NULL;
    }

    /* For v2, use mmap path */
    if (version == LC_BINARY_VERSION) {
        fclose(f);
        return lc_index_mmap(path);
    }

    /* v1 format - read traditionally */
    if (version != 1) {
        fclose(f);
        return NULL;
    }

    fseek(f, 0, SEEK_SET);

    /* Read v1 header */
    uint32_t entity_count;
    double min_lat, min_lon, max_lat, max_lon;

    if (fread(&magic, 4, 1, f) != 1 ||
        fread(&version, 4, 1, f) != 1 ||
        fread(&entity_count, 4, 1, f) != 1 ||
        fread(&min_lat, 8, 1, f) != 1 ||
        fread(&min_lon, 8, 1, f) != 1 ||
        fread(&max_lat, 8, 1, f) != 1 ||
        fread(&max_lon, 8, 1, f) != 1) {
        fclose(f);
        return NULL;
    }

    /* Create entity store */
    LCEntityStore *store = lc_entity_store_create(entity_count > 0 ? entity_count : 1024);
    if (!store) {
        fclose(f);
        return NULL;
    }

    /* Read entities (v1 format) */
    for (uint32_t i = 0; i < entity_count; i++) {
        LCEntity e;
        lc_entity_init(&e);

        uint8_t type_u8, fclass_u8;
        uint16_t name_len, num_alts, poi_len;

        /* Core fields */
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

        /* Name */
        if (fread(&name_len, 2, 1, f) != 1) { lc_entity_store_free(store); fclose(f); return NULL; }
        if (name_len > 0) {
            e.name = malloc(name_len + 1);
            if (!e.name || fread(e.name, 1, name_len, f) != name_len) {
                lc_entity_free(&e); lc_entity_store_free(store); fclose(f); return NULL;
            }
            e.name[name_len] = '\0';
        }

        /* Alt names */
        if (fread(&num_alts, 2, 1, f) != 1) { lc_entity_free(&e); lc_entity_store_free(store); fclose(f); return NULL; }
        e.num_alt_names = num_alts;
        if (num_alts > 0) {
            e.alt_names = calloc(num_alts, sizeof(char *));
            if (!e.alt_names) { lc_entity_free(&e); lc_entity_store_free(store); fclose(f); return NULL; }
            for (uint16_t j = 0; j < num_alts; j++) {
                uint16_t alt_len;
                if (fread(&alt_len, 2, 1, f) != 1) { lc_entity_free(&e); lc_entity_store_free(store); fclose(f); return NULL; }
                if (alt_len > 0) {
                    e.alt_names[j] = malloc(alt_len + 1);
                    if (!e.alt_names[j] || fread(e.alt_names[j], 1, alt_len, f) != alt_len) {
                        lc_entity_free(&e); lc_entity_store_free(store); fclose(f); return NULL;
                    }
                    e.alt_names[j][alt_len] = '\0';
                }
            }
        }

        /* POI type */
        if (fread(&poi_len, 2, 1, f) != 1) { lc_entity_free(&e); lc_entity_store_free(store); fclose(f); return NULL; }
        if (poi_len > 0) {
            e.poi_type = malloc(poi_len + 1);
            if (!e.poi_type || fread(e.poi_type, 1, poi_len, f) != poi_len) {
                lc_entity_free(&e); lc_entity_store_free(store); fclose(f); return NULL;
            }
            e.poi_type[poi_len] = '\0';
        }

        /* Address fields (simplified reading) */
        #define READ_ADDR_FIELD(field) do { \
            uint16_t len; \
            if (fread(&len, 2, 1, f) != 1) { lc_entity_free(&e); lc_entity_store_free(store); fclose(f); return NULL; } \
            if (len > 0) { \
                field = malloc(len + 1); \
                if (!field || fread(field, 1, len, f) != len) { \
                    lc_entity_free(&e); lc_entity_store_free(store); fclose(f); return NULL; \
                } \
                field[len] = '\0'; \
            } \
        } while(0)

        READ_ADDR_FIELD(e.address.housenumber);
        READ_ADDR_FIELD(e.address.street);
        READ_ADDR_FIELD(e.address.city);
        READ_ADDR_FIELD(e.address.postcode);
        READ_ADDR_FIELD(e.address.state);
        READ_ADDR_FIELD(e.address.country);
        READ_ADDR_FIELD(e.address.country_code);

        #undef READ_ADDR_FIELD

        lc_entity_store_add(store, &e);
    }

    fclose(f);

    /* Build index */
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
