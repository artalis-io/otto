/*
 * lc_types.c - Locus core data types implementation
 */

#include "lc_types.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Status Strings
 * ============================================================================ */

const char *lc_status_string(LCStatus status)
{
    switch (status) {
        case LC_OK:                  return "OK";
        case LC_ERROR_INVALID_PARAM: return "Invalid parameter";
        case LC_ERROR_OUT_OF_MEMORY: return "Out of memory";
        case LC_ERROR_FILE_NOT_FOUND: return "File not found";
        case LC_ERROR_PARSE_FAILED:  return "Parse failed";
        case LC_ERROR_INDEX_CORRUPT: return "Index corrupt";
        case LC_ERROR_NOT_FOUND:     return "Not found";
        case LC_ERROR_INTERNAL:      return "Internal error";
        default:                     return "Unknown error";
    }
}

/* ============================================================================
 * Feature Class Strings
 * ============================================================================ */

const char *lc_class_string(LCFeatureClass fclass)
{
    switch (fclass) {
        case LC_CLASS_UNKNOWN:       return "unknown";
        case LC_CLASS_COUNTRY:       return "country";
        case LC_CLASS_STATE:         return "state";
        case LC_CLASS_COUNTY:        return "county";
        case LC_CLASS_CITY:          return "city";
        case LC_CLASS_TOWN:          return "town";
        case LC_CLASS_VILLAGE:       return "village";
        case LC_CLASS_SUBURB:        return "suburb";
        case LC_CLASS_NEIGHBOURHOOD: return "neighbourhood";
        case LC_CLASS_HAMLET:        return "hamlet";
        case LC_CLASS_LOCALITY:      return "locality";
        case LC_CLASS_STREET:        return "street";
        case LC_CLASS_ADDRESS:       return "address";
        case LC_CLASS_POI:           return "poi";
        case LC_CLASS_WATER:         return "water";
        case LC_CLASS_OTHER:         return "other";
        default:                     return "unknown";
    }
}

const char *lc_entity_type_string(LCEntityType type)
{
    switch (type) {
        case LC_ENTITY_NODE:     return "node";
        case LC_ENTITY_WAY:      return "way";
        case LC_ENTITY_RELATION: return "relation";
        default:                 return "unknown";
    }
}

/* ============================================================================
 * Feature Class Importance (for ranking)
 * ============================================================================ */

int lc_class_importance(LCFeatureClass fclass)
{
    switch (fclass) {
        case LC_CLASS_COUNTRY:       return 100;
        case LC_CLASS_STATE:         return 90;
        case LC_CLASS_COUNTY:        return 80;
        case LC_CLASS_CITY:          return 70;
        case LC_CLASS_TOWN:          return 60;
        case LC_CLASS_VILLAGE:       return 50;
        case LC_CLASS_SUBURB:        return 40;
        case LC_CLASS_NEIGHBOURHOOD: return 35;
        case LC_CLASS_HAMLET:        return 30;
        case LC_CLASS_LOCALITY:      return 25;
        case LC_CLASS_STREET:        return 20;
        case LC_CLASS_ADDRESS:       return 15;
        case LC_CLASS_POI:           return 10;
        case LC_CLASS_WATER:         return 5;
        case LC_CLASS_OTHER:         return 1;
        default:                     return 0;
    }
}

/* ============================================================================
 * Address Functions
 * ============================================================================ */

void lc_address_init(LCAddress *addr)
{
    if (!addr) return;
    memset(addr, 0, sizeof(LCAddress));
}

void lc_address_free(LCAddress *addr)
{
    if (!addr) return;
    free(addr->housenumber);
    free(addr->street);
    free(addr->city);
    free(addr->postcode);
    free(addr->state);
    free(addr->country);
    free(addr->country_code);
    memset(addr, 0, sizeof(LCAddress));
}

static char *str_dup_safe(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (copy) {
        memcpy(copy, s, len + 1);
    }
    return copy;
}

LCStatus lc_address_copy(LCAddress *dst, const LCAddress *src)
{
    if (!dst || !src) return LC_ERROR_INVALID_PARAM;

    lc_address_init(dst);

    dst->housenumber = str_dup_safe(src->housenumber);
    dst->street = str_dup_safe(src->street);
    dst->city = str_dup_safe(src->city);
    dst->postcode = str_dup_safe(src->postcode);
    dst->state = str_dup_safe(src->state);
    dst->country = str_dup_safe(src->country);
    dst->country_code = str_dup_safe(src->country_code);

    return LC_OK;
}

/* ============================================================================
 * LineString Functions
 * ============================================================================ */

LCLineString *lc_linestring_create(uint32_t capacity)
{
    LCLineString *line = calloc(1, sizeof(LCLineString));
    if (!line) return NULL;

    if (capacity > 0) {
        line->points = calloc(capacity, sizeof(SHCoord));
        if (!line->points) {
            free(line);
            return NULL;
        }
    }
    line->count = 0;
    return line;
}

void lc_linestring_free(LCLineString *line)
{
    if (!line) return;
    free(line->points);
    free(line);
}

int lc_linestring_add_point(LCLineString *line, SHCoord coord)
{
    if (!line) return 0;

    /* Grow if needed (start with 8, double each time) */
    uint32_t capacity = line->points ? line->count : 0;
    if (line->count >= capacity) {
        uint32_t new_cap = capacity == 0 ? 8 : capacity * 2;
        SHCoord *new_points = realloc(line->points, new_cap * sizeof(SHCoord));
        if (!new_points) return 0;
        line->points = new_points;
    }

    line->points[line->count++] = coord;
    return 1;
}

/* ============================================================================
 * Entity Functions
 * ============================================================================ */

void lc_entity_init(LCEntity *entity)
{
    if (!entity) return;
    memset(entity, 0, sizeof(LCEntity));
    entity->admin_level = 0;
    entity->population = 0;
    sh_bbox_init(&entity->bbox);
}

void lc_entity_free(LCEntity *entity)
{
    if (!entity) return;

    free(entity->name);
    if (entity->alt_names) {
        for (int i = 0; i < entity->num_alt_names; i++) {
            free(entity->alt_names[i]);
        }
        free(entity->alt_names);
    }
    free(entity->poi_type);
    lc_linestring_free(entity->geometry);
    lc_address_free(&entity->address);

    memset(entity, 0, sizeof(LCEntity));
}

/* ============================================================================
 * Entity Store
 * ============================================================================ */

#define INITIAL_STRING_POOL_SIZE (1024 * 1024)  /* 1 MB */

LCEntityStore *lc_entity_store_create(uint32_t initial_capacity)
{
    LCEntityStore *store = calloc(1, sizeof(LCEntityStore));
    if (!store) return NULL;

    if (initial_capacity == 0) initial_capacity = 1024;

    store->entities = calloc(initial_capacity, sizeof(LCEntity));
    if (!store->entities) {
        free(store);
        return NULL;
    }

    store->capacity = initial_capacity;
    store->count = 0;

    /* Note: We use individual allocations for strings instead of a pool.
     * This avoids pointer invalidation issues when the pool is reallocated. */
    store->string_pool = NULL;
    store->string_pool_size = 0;
    store->string_pool_used = 0;  /* Used for memory tracking only */

    return store;
}

void lc_entity_store_free(LCEntityStore *store)
{
    if (!store) return;

    /* Free all entity strings (individually allocated) */
    for (uint32_t i = 0; i < store->count; i++) {
        LCEntity *e = &store->entities[i];
        free(e->name);
        if (e->alt_names) {
            for (int j = 0; j < e->num_alt_names; j++) {
                free(e->alt_names[j]);
            }
            free(e->alt_names);
        }
        free(e->poi_type);
        lc_linestring_free(e->geometry);

        /* Address strings */
        LCAddress *a = &e->address;
        free(a->housenumber);
        free(a->street);
        free(a->city);
        free(a->postcode);
        free(a->state);
        free(a->country);
        free(a->country_code);
    }

    free(store->entities);
    free(store);
}

LCStatus lc_entity_store_add(LCEntityStore *store, const LCEntity *entity)
{
    if (!store || !entity) return LC_ERROR_INVALID_PARAM;

    /* Grow if needed */
    if (store->count >= store->capacity) {
        uint32_t new_capacity = store->capacity * 2;
        LCEntity *new_entities = realloc(store->entities,
                                         new_capacity * sizeof(LCEntity));
        if (!new_entities) return LC_ERROR_OUT_OF_MEMORY;
        store->entities = new_entities;
        store->capacity = new_capacity;
    }

    /* Copy entity (shallow copy - caller's strings are taken) */
    store->entities[store->count] = *entity;
    store->count++;

    return LC_OK;
}

LCEntity *lc_entity_store_get(LCEntityStore *store, uint32_t index)
{
    if (!store || index >= store->count) return NULL;
    return &store->entities[index];
}

char *lc_entity_store_intern(LCEntityStore *store, const char *str, size_t len)
{
    if (!store || !str) return NULL;
    if (len == 0) len = strlen(str);

    /*
     * IMPORTANT: We cannot use a contiguous string pool with realloc because
     * when the pool is moved to a new address, all existing string pointers
     * that point into the old pool become dangling pointers.
     *
     * Instead, we use individual allocations for each string. This is slightly
     * less memory-efficient but avoids the dangling pointer problem.
     */
    char *copy = malloc(len + 1);
    if (copy) {
        memcpy(copy, str, len);
        copy[len] = '\0';
        store->string_pool_used += len + 1;  /* Track memory usage */
    }
    return copy;
}
