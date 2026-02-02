/*
 * lc_types.h - Locus core data types
 *
 * Location Oriented Coordinate Unification System
 * Zero-dependency geocoding library for OSM data
 */

#ifndef LC_TYPES_H
#define LC_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include "sh_geo.h"

/* ============================================================================
 * Status Codes
 * ============================================================================ */

typedef enum {
    LC_OK = 0,
    LC_ERROR_INVALID_PARAM,
    LC_ERROR_OUT_OF_MEMORY,
    LC_ERROR_FILE_NOT_FOUND,
    LC_ERROR_PARSE_FAILED,
    LC_ERROR_INDEX_CORRUPT,
    LC_ERROR_NOT_FOUND,
    LC_ERROR_INTERNAL
} LCStatus;

/* ============================================================================
 * Entity Types
 * ============================================================================ */

typedef enum {
    LC_ENTITY_NODE = 0,
    LC_ENTITY_WAY,
    LC_ENTITY_RELATION
} LCEntityType;

/* ============================================================================
 * Feature Classes
 * ============================================================================ */

typedef enum {
    LC_CLASS_UNKNOWN = 0,

    /* Administrative boundaries (highest priority) */
    LC_CLASS_COUNTRY,       /* admin_level=2 */
    LC_CLASS_STATE,         /* admin_level=4 */
    LC_CLASS_COUNTY,        /* admin_level=6 */
    LC_CLASS_CITY,          /* place=city or admin_level=8 */
    LC_CLASS_TOWN,          /* place=town */
    LC_CLASS_VILLAGE,       /* place=village */
    LC_CLASS_SUBURB,        /* place=suburb */
    LC_CLASS_NEIGHBOURHOOD, /* place=neighbourhood */
    LC_CLASS_HAMLET,        /* place=hamlet */
    LC_CLASS_LOCALITY,      /* place=locality */

    /* Streets and addresses */
    LC_CLASS_STREET,        /* highway=* with name */
    LC_CLASS_ADDRESS,       /* addr:housenumber + addr:street */

    /* Points of interest */
    LC_CLASS_POI,           /* amenity, shop, tourism, etc. */

    /* Water features */
    LC_CLASS_WATER,         /* natural=water, waterway=* */

    /* Other named features */
    LC_CLASS_OTHER,

    LC_CLASS_COUNT
} LCFeatureClass;

/* ============================================================================
 * Structured Address
 * ============================================================================ */

typedef struct {
    char *housenumber;
    char *street;
    char *city;
    char *postcode;
    char *state;
    char *country;
    char *country_code;     /* ISO 3166-1 alpha-2 */
} LCAddress;

/* ============================================================================
 * Street Geometry (LineString)
 * ============================================================================ */

typedef struct {
    SHCoord *points;        /* Array of coordinates along the street */
    uint32_t count;         /* Number of points */
} LCLineString;

/* ============================================================================
 * Geocodable Entity
 * ============================================================================ */

typedef struct {
    uint64_t osm_id;
    LCEntityType type;
    LCFeatureClass fclass;

    /* Names */
    char *name;             /* Primary name */
    char **alt_names;       /* Alternative names (name:en, alt_name, etc.) */
    uint16_t num_alt_names;

    /* Location */
    SHCoord centroid;       /* Representative point */
    SHBBox bbox;            /* Bounding box (for areas) */

    /* Street geometry (for LC_CLASS_STREET only) */
    LCLineString *geometry; /* Line geometry for distance calculations */

    /* Metadata */
    int8_t admin_level;     /* 0-10, 0 if not a boundary */
    int32_t population;     /* 0 if unknown */

    /* Address components (for address entities) */
    LCAddress address;

    /* POI type (for POIs) */
    char *poi_type;         /* e.g., "restaurant", "hotel" */
} LCEntity;

/* ============================================================================
 * Entity Store (dynamic array)
 * ============================================================================ */

typedef struct {
    LCEntity *entities;
    uint32_t count;
    uint32_t capacity;

    /* String pool for memory efficiency */
    char *string_pool;
    size_t string_pool_size;
    size_t string_pool_used;
} LCEntityStore;

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/* Get string name for status code */
const char *lc_status_string(LCStatus status);

/* Get string name for feature class */
const char *lc_class_string(LCFeatureClass fclass);

/* Get string name for entity type */
const char *lc_entity_type_string(LCEntityType type);

/* Get importance weight for ranking (higher = more important) */
int lc_class_importance(LCFeatureClass fclass);

/* ============================================================================
 * Entity Store Functions
 * ============================================================================ */

/* Create entity store with initial capacity */
LCEntityStore *lc_entity_store_create(uint32_t initial_capacity);

/* Free entity store and all entities */
void lc_entity_store_free(LCEntityStore *store);

/* Add entity to store (takes ownership of strings) */
LCStatus lc_entity_store_add(LCEntityStore *store, const LCEntity *entity);

/* Get entity by index */
LCEntity *lc_entity_store_get(LCEntityStore *store, uint32_t index);

/* Intern a string in the store's string pool */
char *lc_entity_store_intern(LCEntityStore *store, const char *str, size_t len);

/* ============================================================================
 * Address Functions
 * ============================================================================ */

/* Initialize address to empty */
void lc_address_init(LCAddress *addr);

/* Free address strings */
void lc_address_free(LCAddress *addr);

/* Copy address (allocates new strings) */
LCStatus lc_address_copy(LCAddress *dst, const LCAddress *src);

/* ============================================================================
 * LineString Functions
 * ============================================================================ */

/* Create a linestring with given capacity */
LCLineString *lc_linestring_create(uint32_t capacity);

/* Free linestring */
void lc_linestring_free(LCLineString *line);

/* Add a point to linestring */
int lc_linestring_add_point(LCLineString *line, SHCoord coord);

/* ============================================================================
 * Entity Functions
 * ============================================================================ */

/* Initialize entity to empty */
void lc_entity_init(LCEntity *entity);

/* Free entity strings (not the entity struct itself) */
void lc_entity_free(LCEntity *entity);

#endif /* LC_TYPES_H */
