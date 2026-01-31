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

    /* Allocate string pool */
    store->string_pool_size = INITIAL_STRING_POOL_SIZE;
    store->string_pool = malloc(store->string_pool_size);
    if (!store->string_pool) {
        free(store->entities);
        free(store);
        return NULL;
    }
    store->string_pool_used = 0;

    return store;
}

void lc_entity_store_free(LCEntityStore *store)
{
    if (!store) return;

    /* Free individual entity strings that aren't in the pool */
    for (uint32_t i = 0; i < store->count; i++) {
        LCEntity *e = &store->entities[i];
        /* Only free strings not in the pool */
        if (e->name && (e->name < store->string_pool ||
                        e->name >= store->string_pool + store->string_pool_size)) {
            free(e->name);
        }
        if (e->alt_names) {
            for (int j = 0; j < e->num_alt_names; j++) {
                if (e->alt_names[j] &&
                    (e->alt_names[j] < store->string_pool ||
                     e->alt_names[j] >= store->string_pool + store->string_pool_size)) {
                    free(e->alt_names[j]);
                }
            }
            free(e->alt_names);
        }
        if (e->poi_type &&
            (e->poi_type < store->string_pool ||
             e->poi_type >= store->string_pool + store->string_pool_size)) {
            free(e->poi_type);
        }
        /* Address strings */
        LCAddress *a = &e->address;
        if (a->housenumber && (a->housenumber < store->string_pool ||
                               a->housenumber >= store->string_pool + store->string_pool_size)) {
            free(a->housenumber);
        }
        if (a->street && (a->street < store->string_pool ||
                          a->street >= store->string_pool + store->string_pool_size)) {
            free(a->street);
        }
        if (a->city && (a->city < store->string_pool ||
                        a->city >= store->string_pool + store->string_pool_size)) {
            free(a->city);
        }
        if (a->postcode && (a->postcode < store->string_pool ||
                            a->postcode >= store->string_pool + store->string_pool_size)) {
            free(a->postcode);
        }
        if (a->state && (a->state < store->string_pool ||
                         a->state >= store->string_pool + store->string_pool_size)) {
            free(a->state);
        }
        if (a->country && (a->country < store->string_pool ||
                           a->country >= store->string_pool + store->string_pool_size)) {
            free(a->country);
        }
        if (a->country_code && (a->country_code < store->string_pool ||
                                a->country_code >= store->string_pool + store->string_pool_size)) {
            free(a->country_code);
        }
    }

    free(store->string_pool);
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

    /* Check if we need to grow the pool */
    size_t needed = len + 1;
    if (store->string_pool_used + needed > store->string_pool_size) {
        size_t new_size = store->string_pool_size * 2;
        while (store->string_pool_used + needed > new_size) {
            new_size *= 2;
        }
        char *new_pool = realloc(store->string_pool, new_size);
        if (!new_pool) {
            /* Fall back to regular allocation */
            char *copy = malloc(len + 1);
            if (copy) {
                memcpy(copy, str, len);
                copy[len] = '\0';
            }
            return copy;
        }
        store->string_pool = new_pool;
        store->string_pool_size = new_size;
    }

    /* Copy into pool */
    char *dest = store->string_pool + store->string_pool_used;
    memcpy(dest, str, len);
    dest[len] = '\0';
    store->string_pool_used += needed;

    return dest;
}
