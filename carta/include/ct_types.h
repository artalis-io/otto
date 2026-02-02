/*
 * ct_types.h - Carta data structures
 *
 * Core data types for the Carta tile generator including coordinates,
 * tile storage, features, and configuration options.
 */

#ifndef CT_TYPES_H
#define CT_TYPES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Status Codes
 * ============================================================================ */

typedef enum {
    CT_OK = 0,
    CT_ERROR_INVALID_ARGUMENT,
    CT_ERROR_OUT_OF_MEMORY,
    CT_ERROR_FILE_NOT_FOUND,
    CT_ERROR_FILE_READ,
    CT_ERROR_FILE_WRITE,
    CT_ERROR_PARSE_ERROR,
    CT_ERROR_BUFFER_TOO_SMALL,
    CT_ERROR_UNSUPPORTED_FORMAT,
    CT_ERROR_INTERNAL
} CTStatus;

/* ============================================================================
 * Coordinates
 * ============================================================================ */

/* Geographic coordinate (WGS84) */
typedef struct {
    double lat;
    double lon;
} CTCoord;

/* Fixed-point coordinate (for compact storage) */
typedef struct {
    int32_t lat;  /* lat * 1e7 */
    int32_t lon;  /* lon * 1e7 */
} CTCoordFixed;

/* Bounding box in lat/lon */
typedef struct {
    double min_lat, min_lon;
    double max_lat, max_lon;
} CTBBox;

/* Point in tile coordinates (0 to extent-1) */
typedef struct {
    int32_t x;
    int32_t y;
} CTTilePoint;

/* Convert between coordinate types */
#define CT_COORD_TO_FIXED(c) \
    ((CTCoordFixed){(int32_t)((c).lat * 1e7), (int32_t)((c).lon * 1e7)})
#define CT_FIXED_TO_COORD(f) \
    ((CTCoord){(f).lat * 1e-7, (f).lon * 1e-7})

/* ============================================================================
 * Tile Coordinates (Web Mercator / Slippy Map)
 * ============================================================================ */

typedef struct {
    int z;          /* Zoom level (0-22) */
    int x;          /* Tile X coordinate */
    int y;          /* Tile Y coordinate */
} CTTileCoord;

/* Standard tile extents */
#define CT_MVT_EXTENT      4096    /* Standard MVT extent */
#define CT_RASTER_SIZE_256 256     /* Standard raster tile size */
#define CT_RASTER_SIZE_512 512     /* High-DPI raster tile size */

/* ============================================================================
 * Feature Layers
 * ============================================================================ */

typedef enum {
    CT_LAYER_BACKGROUND = 0,
    CT_LAYER_LANDUSE,
    CT_LAYER_WATER,
    CT_LAYER_BUILDINGS,
    CT_LAYER_ROADS,
    CT_LAYER_RAILWAYS,
    CT_LAYER_BOUNDARIES,
    CT_LAYER_LABELS,
    CT_LAYER_COUNT
} CTLayer;

/* ============================================================================
 * Road Types (for styling)
 * ============================================================================ */

typedef enum {
    CT_ROAD_MOTORWAY = 0,
    CT_ROAD_TRUNK,
    CT_ROAD_PRIMARY,
    CT_ROAD_SECONDARY,
    CT_ROAD_TERTIARY,
    CT_ROAD_RESIDENTIAL,
    CT_ROAD_SERVICE,
    CT_ROAD_OTHER,
    CT_ROAD_TYPE_COUNT
} CTRoadType;

/* ============================================================================
 * Feature Geometry Types
 * ============================================================================ */

typedef enum {
    CT_GEOM_UNKNOWN = 0,
    CT_GEOM_POINT = 1,
    CT_GEOM_LINESTRING = 2,
    CT_GEOM_POLYGON = 3
} CTGeomType;

/* ============================================================================
 * Map Feature
 * ============================================================================ */

typedef struct {
    CTGeomType type;
    CTTilePoint *points;
    int num_points;

    /* For polygons: ring structure */
    int *ring_ends;          /* End index (exclusive) of each ring */
    int num_rings;           /* Number of rings (1 = simple, >1 = with holes) */

    /* Feature classification */
    CTLayer layer;
    int feature_type;        /* Layer-specific subtype (e.g., road type) */

    /* Properties (key-value pairs) */
    char **prop_keys;
    char **prop_values;
    int num_props;
} CTFeature;

/* ============================================================================
 * Tile Data
 * ============================================================================ */

typedef struct {
    CTTileCoord coord;

    CTFeature *features;
    size_t num_features;
    size_t features_capacity;

    /* Memory pool for point arrays */
    CTTilePoint *point_pool;
    size_t point_pool_size;
    size_t point_pool_capacity;
} CTTile;

/* ============================================================================
 * Styling
 * ============================================================================ */

/* RGBA color (0xRRGGBBAA) */
typedef uint32_t CTColor;

#define CT_RGBA(r, g, b, a) \
    (((uint32_t)(r) << 24) | ((uint32_t)(g) << 16) | ((uint32_t)(b) << 8) | (uint32_t)(a))
#define CT_RGB(r, g, b) CT_RGBA(r, g, b, 255)
#define CT_COLOR_R(c) (((c) >> 24) & 0xFF)
#define CT_COLOR_G(c) (((c) >> 16) & 0xFF)
#define CT_COLOR_B(c) (((c) >> 8) & 0xFF)
#define CT_COLOR_A(c) ((c) & 0xFF)

/*
 * Road width specification at key zoom levels.
 * Widths are linearly interpolated between these points.
 */
typedef struct {
    float z10;  /* Width at zoom 10 */
    float z14;  /* Width at zoom 14 */
    float z18;  /* Width at zoom 18 */
} CTRoadWidth;

typedef struct {
    /* Road styling by type */
    CTColor road_colors[CT_ROAD_TYPE_COUNT];
    CTColor road_outline_colors[CT_ROAD_TYPE_COUNT];
    CTRoadWidth road_widths[CT_ROAD_TYPE_COUNT];  /* Width in pixels at key zoom levels */

    /* Area colors */
    CTColor water_color;
    CTColor land_color;
    CTColor building_color;
    CTColor building_outline_color;
    CTColor forest_color;
    CTColor grass_color;
    CTColor sand_color;

    /* Railway */
    CTColor railway_color;
    float railway_width;

    /* Background */
    CTColor background_color;

    /* Reference zoom for widths */
    int reference_zoom;
} CTStyle;

/* ============================================================================
 * Render Context (for raster tiles)
 * ============================================================================ */

typedef struct {
    uint8_t *pixels;         /* RGBA pixel buffer */
    int width;
    int height;
    int stride;              /* Bytes per row (usually width * 4) */
    CTStyle style;
} CTRenderContext;

/* ============================================================================
 * OSM Feature Classes (for PBF parsing)
 * ============================================================================ */

typedef enum {
    CT_OSM_UNKNOWN = 0,
    CT_OSM_HIGHWAY,
    CT_OSM_WATER,
    CT_OSM_WATERWAY,
    CT_OSM_BUILDING,
    CT_OSM_LANDUSE,
    CT_OSM_NATURAL,
    CT_OSM_RAILWAY,
    CT_OSM_BOUNDARY,
    CT_OSM_AMENITY
} CTOSMFeatureClass;

/* ============================================================================
 * Parsed OSM Data
 * ============================================================================ */

/* Parsed OSM node (temporary, during parsing) */
typedef struct {
    int64_t id;
    double lat;
    double lon;
} CTOSMNode;

/* Parsed OSM way (with resolved geometry) */
typedef struct {
    int64_t id;
    CTCoord *coords;
    int num_coords;
    CTOSMFeatureClass feature_class;
    int feature_type;        /* Subtype within class */
    int is_area;             /* Closed polygon? */
    char *name;              /* Optional name */
    int min_zoom;            /* Minimum zoom for LOD filtering */
    float area_sqm;          /* Estimated area (for polygons) */
    float length_m;          /* Estimated length (for lines) */
} CTOSMWay;

/* R-Tree node for spatial indexing */
typedef struct CTRTreeNode CTRTreeNode;

/* Packed R-Tree node (flat array storage for efficiency) */
typedef struct {
    CTBBox bbox;
    uint32_t first_child;  /* Index of first child in next level, or index into leaf_indices */
    uint16_t num_children; /* Number of children */
    uint16_t is_leaf;      /* 1 if children are way indices, 0 if node indices */
} CTPackedNode;

typedef struct {
    /* Packed array storage (efficient format) */
    CTPackedNode *nodes;          /* Flat array of all nodes */
    uint32_t *leaf_indices;       /* Way indices for leaf nodes */
    size_t num_nodes;             /* Total number of nodes */
    size_t num_entries;           /* Total number of way entries */
    uint32_t root_idx;            /* Index of root node */
} CTRTree;

/* PBF parsing context */
typedef struct {
    /* Node storage (for resolving way references) */
    struct {
        int64_t *ids;
        CTCoord *coords;
        size_t count;
        size_t capacity;
    } nodes;

    /* Node ID to index lookup (hash map) */
    struct {
        int64_t *keys;
        uint32_t *values;
        size_t capacity;
        size_t count;
    } node_map;

    /* Parsed ways */
    CTOSMWay *ways;
    size_t num_ways;
    size_t ways_capacity;

    /* Spatial index for fast tile queries */
    CTRTree *rtree;

    /* Bounding box of loaded data */
    CTBBox bbox;

    /* Statistics */
    size_t total_nodes_parsed;
    size_t total_ways_parsed;
    size_t features_kept;
} CTPBFContext;

/* ============================================================================
 * MVT Encoding Options
 * ============================================================================ */

typedef struct {
    int extent;              /* Coordinate extent (default: 4096) */
    int buffer;              /* Buffer around tile in extent units */
    int simplify;            /* Geometry simplification enabled */
    double tolerance;        /* Simplification tolerance */
    int compress;            /* gzip compress output */
} CTMVTOptions;

/* ============================================================================
 * PNG Encoding Options
 * ============================================================================ */

typedef struct {
    int tile_size;           /* 256 or 512 */
    int compression_level;   /* 0-9 (6 is default) */
} CTPNGOptions;

/* ============================================================================
 * Constants
 * ============================================================================ */

#define CT_PI 3.14159265358979323846
#define CT_EARTH_RADIUS_M 6371000.0
#define CT_MAX_ZOOM 22
#define CT_MIN_ZOOM 0

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/* Status to string */
const char *ct_status_string(CTStatus status);

/* Version */
const char *ct_version(void);

#ifdef __cplusplus
}
#endif

#endif /* CT_TYPES_H */
