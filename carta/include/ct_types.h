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

/* Forward declarations for shared memory structures */
typedef struct SHArena SHArena;
typedef struct SHPool SHPool;

/* Include shared headers */
#include "sh_hashmap.h"
#include "sh_geo.h"

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

/* Bounding box in lat/lon (alias to shared type) */
typedef SHBBox CTBBox;

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
 * Railway Types (for styling)
 * ============================================================================ */

typedef enum {
    CT_RAILWAY_RAIL = 0,       /* Main rail lines (railway=rail) */
    CT_RAILWAY_SUBWAY,         /* Subway/metro (railway=subway) */
    CT_RAILWAY_TRAM,           /* Trams/streetcars (railway=tram, light_rail) */
    CT_RAILWAY_NARROW_GAUGE,   /* Narrow gauge (railway=narrow_gauge) */
    CT_RAILWAY_PRESERVED,      /* Heritage railways (railway=preserved) */
    CT_RAILWAY_DISUSED,        /* Disused/abandoned (railway=disused, abandoned) */
    CT_RAILWAY_OTHER,          /* Other railway types */
    CT_RAILWAY_TYPE_COUNT
} CTRailwayType;

/* ============================================================================
 * Feature Flags (bridge, tunnel, etc.)
 * ============================================================================ */

#define CT_FLAG_NONE     0x00
#define CT_FLAG_BRIDGE   0x01  /* Feature is on a bridge */
#define CT_FLAG_TUNNEL   0x02  /* Feature is in a tunnel */
#define CT_FLAG_ONEWAY   0x04  /* One-way road */

/* ============================================================================
 * Waterway Types (for styling)
 * ============================================================================ */

typedef enum {
    CT_WATERWAY_RIVER = 0,   /* Major rivers (Danube, Rhine, etc.) */
    CT_WATERWAY_CANAL,       /* Navigable canals */
    CT_WATERWAY_STREAM,      /* Small streams */
    CT_WATERWAY_DRAIN,       /* Drainage ditches */
    CT_WATERWAY_DITCH,       /* Small ditches */
    CT_WATERWAY_OTHER,       /* Other waterways */
    CT_WATERWAY_TYPE_COUNT,

    /* Water body types (distinct from linear waterways) */
    CT_WATER_BODY = 100,     /* Lakes, reservoirs, ponds (polygons) */
    CT_WATER_RIVERBANK       /* Riverbank polygons */
} CTWaterwayType;

/* ============================================================================
 * Landuse Types (for LOD filtering)
 * ============================================================================ */

typedef enum {
    CT_LANDUSE_FOREST = 0,   /* natural=wood, landuse=forest */
    CT_LANDUSE_PARK,         /* leisure=park, nature_reserve */
    CT_LANDUSE_RESIDENTIAL,  /* landuse=residential */
    CT_LANDUSE_COMMERCIAL,   /* landuse=commercial, retail */
    CT_LANDUSE_INDUSTRIAL,   /* landuse=industrial */
    CT_LANDUSE_FARMLAND,     /* landuse=farmland, meadow, farmyard */
    CT_LANDUSE_GRASS,        /* landuse=grass, village_green */
    CT_LANDUSE_CEMETERY,     /* landuse=cemetery */
    CT_LANDUSE_MILITARY,     /* landuse=military */
    CT_LANDUSE_OTHER,        /* Other landuse */
    CT_LANDUSE_TYPE_COUNT
} CTLanduseType;

/* ============================================================================
 * Boundary Admin Levels
 * ============================================================================ */

typedef enum {
    CT_BOUNDARY_COUNTRY = 2,  /* admin_level=2 (countries) */
    CT_BOUNDARY_STATE = 4,    /* admin_level=4 (states/provinces) */
    CT_BOUNDARY_COUNTY = 6,   /* admin_level=6 (counties/districts) */
    CT_BOUNDARY_CITY = 8,     /* admin_level=8 (cities/municipalities) */
    CT_BOUNDARY_SUBURB = 10,  /* admin_level=10 (suburbs/neighborhoods) */
    CT_BOUNDARY_OTHER = 99    /* Other or unspecified */
} CTBoundaryLevel;

/* ============================================================================
 * Boundary Types (administrative vs protected areas)
 * ============================================================================ */

typedef enum {
    CT_BOUNDARY_TYPE_ADMIN = 0,      /* boundary=administrative */
    CT_BOUNDARY_TYPE_PROTECTED = 1,  /* boundary=protected_area (national parks) */
    CT_BOUNDARY_TYPE_COUNT
} CTBoundaryType;

/* Assembled boundary linestring (from relation + member ways) */
typedef struct {
    int64_t relation_id;
    CTBoundaryType boundary_type;  /* Admin or protected area */
    int admin_level;               /* 2=country, 4=state, etc. (for admin) */
    CTCoord *coords;               /* Stitched coordinates */
    int num_coords;
    CTBBox bbox;
    float length_m;                /* Total length in meters (for LOD) */
    char *name;                    /* Optional boundary name */
} CTAssembledBoundary;

/* Boundary configuration (parametrizable admin levels) */
typedef struct {
    int min_admin_level;           /* Minimum admin level to extract (default: 2) */
    int max_admin_level;           /* Maximum admin level to extract (default: 6) */
    int include_protected_areas;   /* 1 to include boundary=protected_area */
} CTBoundaryConfig;

/* ============================================================================
 * Place Types (for label rendering)
 * ============================================================================ */

typedef enum {
    CT_PLACE_UNKNOWN = 0,
    CT_PLACE_COUNTRY,        /* place=country (z2+) */
    CT_PLACE_STATE,          /* place=state (z4+) */
    CT_PLACE_CITY,           /* place=city (z6+) */
    CT_PLACE_TOWN,           /* place=town (z9+) */
    CT_PLACE_VILLAGE,        /* place=village (z11+) */
    CT_PLACE_HAMLET,         /* place=hamlet (z13+) */
    CT_PLACE_SUBURB,         /* place=suburb (z12+) */
    CT_PLACE_NEIGHBOURHOOD,  /* place=neighbourhood (z14+) */
    CT_PLACE_LOCALITY,       /* place=locality (z14+) */
    CT_PLACE_ISLAND,         /* place=island (z8+) */
    CT_PLACE_PEAK,           /* natural=peak (z12+) */
    CT_PLACE_TYPE_COUNT
} CTPlaceType;

/* Labeled point (city, town, peak, etc.) for map labels */
typedef struct {
    int64_t id;              /* OSM node ID */
    CTCoord coord;           /* Geographic position */
    CTPlaceType type;        /* Place classification */
    char *name;              /* Display name (UTF-8) */
    int population;          /* Population (0 if unknown) */
    int min_zoom;            /* Minimum zoom level for display */
    int priority;            /* Label priority (higher = more important) */
} CTLabeledPoint;

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
    uint8_t flags;           /* CT_FLAG_BRIDGE, CT_FLAG_TUNNEL, etc. */

    /* Size metrics for LOD filtering */
    float area_sqm;          /* Estimated area in m² (for polygons) */
    float length_m;          /* Estimated length in m (for lines) */

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
} CTTile;

/* ============================================================================
 * Styling
 * ============================================================================ */

/* RGBA color - packed as 0xAABBGGRR for efficient SIMD operations.
 * On little-endian systems, this stores as [R,G,B,A] in memory.
 * Same format as shared library (sh_render.h) for zero-copy interop. */
typedef uint32_t CTColor;

#define CT_RGBA(r, g, b, a) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))
#define CT_RGB(r, g, b) CT_RGBA(r, g, b, 255)
#define CT_COLOR_R(c) ((c) & 0xFF)
#define CT_COLOR_G(c) (((c) >> 8) & 0xFF)
#define CT_COLOR_B(c) (((c) >> 16) & 0xFF)
#define CT_COLOR_A(c) (((c) >> 24) & 0xFF)

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

    /* Waterway styling by type - data-driven widths based on waterway class */
    float waterway_widths[CT_WATERWAY_TYPE_COUNT];  /* Width in pixels */

    /* Area colors */
    CTColor water_color;
    CTColor land_color;
    CTColor building_color;
    CTColor building_outline_color;
    CTColor forest_color;
    CTColor grass_color;
    CTColor park_color;
    CTColor sand_color;
    CTColor residential_color;   /* Urban/residential areas */
    CTColor commercial_color;    /* Commercial/retail areas */
    CTColor industrial_color;    /* Industrial areas */
    CTColor farmland_color;      /* Agricultural areas */
    CTColor cemetery_color;      /* Cemeteries */
    CTColor military_color;      /* Military areas */

    /* Railway styling by type */
    CTColor railway_colors[CT_RAILWAY_TYPE_COUNT];
    CTColor railway_outline_colors[CT_RAILWAY_TYPE_COUNT];
    float railway_widths[CT_RAILWAY_TYPE_COUNT];

    /* Bridge styling (outlines for elevated features) */
    CTColor bridge_outline_color;
    float bridge_outline_width;

    /* Boundaries (admin borders) */
    CTColor boundary_color;
    float boundary_width;

    /* Background */
    CTColor background_color;

    /* Reference zoom for widths */
    int reference_zoom;
} CTStyle;

/* ============================================================================
 * Render Options (configurable performance/quality trade-offs)
 * ============================================================================ */

typedef struct {
    /* Layer toggles (disable entire layers) */
    int render_water;
    int render_landuse;
    int render_buildings;
    int render_roads;
    int render_railways;
    int render_boundaries;
    int render_labels;

    /* Detail toggles (expensive visual effects) */
    int render_road_casing;
    int render_railway_casing;
    int render_bridge_outlines;
    int render_building_outlines;
    int render_label_halos;
    int render_boundary_dashes;

    /* Zoom cutoffs (0 = use default) */
    int casing_min_zoom;            /* Default: 14 */
    int building_outlines_min_zoom; /* Default: 14 */
    int labels_min_zoom;            /* Default: 8 */
} CTRenderOptions;

/* ============================================================================
 * Render Context (for raster tiles)
 * ============================================================================ */

typedef struct {
    uint8_t *pixels;         /* RGBA pixel buffer */
    int width;
    int height;
    int stride;              /* Bytes per row (usually width * 4) */
    CTStyle style;
    CTRenderOptions options; /* Configurable rendering options */

    /* Pre-allocated buffers for rendering (avoids per-feature malloc) */
    CTTilePoint *scale_buffer;      /* Reusable point scaling buffer */
    size_t scale_buffer_capacity;   /* Capacity in points */

    /* Pre-allocated edge buffers for polygon fill (avoids per-polygon malloc) */
    void *edge_buffer;              /* Edge table scratch space */
    void *active_buffer;            /* Active edge table scratch space */
    size_t edge_buffer_capacity;    /* Capacity in edges */
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
    uint8_t flags;           /* CT_FLAG_BRIDGE, CT_FLAG_TUNNEL, etc. */
    char *name;              /* Optional name */
    int min_zoom;            /* Minimum zoom for LOD filtering */
    float area_sqm;          /* Estimated area (for polygons) */
    float length_m;          /* Estimated length (for lines) */
} CTOSMWay;

/* ============================================================================
 * OSM Relations and Multipolygons
 * ============================================================================ */

/* Relation member types */
typedef enum {
    CT_MEMBER_NODE = 0,
    CT_MEMBER_WAY = 1,
    CT_MEMBER_RELATION = 2
} CTMemberType;

/* Single relation member */
typedef struct {
    int64_t ref;             /* Member ID (node/way/relation) */
    CTMemberType type;       /* Member type */
    uint32_t role_idx;       /* Role string index (0 = empty role) */
} CTRelationMember;

/* Parsed OSM relation */
typedef struct {
    int64_t id;
    CTRelationMember *members;
    int num_members;

    CTOSMFeatureClass feature_class;
    int feature_type;        /* Subtype within class */
    char *name;              /* Optional name */

    int is_multipolygon;     /* 1 if type=multipolygon */
} CTOSMRelation;

/* Ring in an assembled multipolygon */
typedef struct {
    CTCoord *coords;
    int num_coords;
    int is_outer;            /* 1 = outer ring, 0 = inner (hole) */
} CTMultipolygonRing;

/* Assembled multipolygon (from relation + member ways) */
typedef struct {
    CTMultipolygonRing *rings;
    int num_rings;
    CTBBox bbox;
    float area_sqm;

    CTOSMFeatureClass feature_class;
    int feature_type;
    char *name;
} CTAssembledMultipolygon;

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

/* Progress callback for long-running operations */
typedef void (*CTPBFProgressFn)(const char *phase, size_t current,
                                 size_t total, void *user_data);

/* PBF parsing context */
typedef struct {
    /* Configuration (initial estimates - all structures grow as needed) */
    struct {
        size_t initial_coord_capacity;  /* Initial coord pool capacity */
        size_t arena_size;              /* Size of parse_arena */
        size_t memory_limit;            /* Max total memory (0 = unlimited) */
    } config;

    /* Memory tracking */
    size_t memory_used;             /* Current total memory usage */

    /* Progress reporting */
    CTPBFProgressFn progress_callback;
    void *progress_user_data;
    size_t progress_interval;       /* Report every N blobs */

    /* Node storage (for resolving way references) */
    struct {
        int64_t *ids;
        CTCoord *coords;
        size_t count;
        size_t capacity;
    } nodes;

    /* Node ID to index lookup (hash map) */
    SHHashmapI64 *node_map;

    /* Parsed ways */
    CTOSMWay *ways;
    size_t num_ways;
    size_t ways_capacity;

    /* Way ID to index lookup (for relation member resolution) */
    SHHashmapI64 *way_map;

    /* Parsed relations */
    CTOSMRelation *relations;
    size_t num_relations;
    size_t relations_capacity;

    /* Role strings from relations (shared pool) */
    char **role_strings;
    size_t num_role_strings;
    size_t role_strings_capacity;

    /* Role string hash table for deduplication (thread-safe: per-context) */
    struct {
        uint32_t hash;      /* Hash of the role string (0 = empty slot) */
        uint32_t role_idx;  /* Index into role_strings */
    } role_hash[256];
    int role_hash_initialized;

    /* Assembled multipolygons */
    CTAssembledMultipolygon *multipolygons;
    size_t num_multipolygons;
    size_t multipolygons_capacity;

    /* R-Tree for multipolygons */
    CTRTree *mp_rtree;

    /* Boundary relations (raw, before assembly) */
    CTOSMRelation *boundary_relations;
    size_t num_boundary_relations;
    size_t boundary_relations_capacity;

    /* Assembled boundaries (stitched linestrings) */
    CTAssembledBoundary *boundaries;
    size_t num_boundaries;
    size_t boundaries_capacity;

    /* R-Tree for boundaries */
    CTRTree *boundary_rtree;

    /* Boundary extraction configuration */
    CTBoundaryConfig boundary_config;

    /* Spatial index for fast tile queries (ways) */
    CTRTree *rtree;

    /* Labeled points (cities, towns, peaks, etc.) */
    CTLabeledPoint *labeled_points;
    size_t num_labeled_points;
    size_t labeled_points_capacity;

    /* Bounding box of loaded data */
    CTBBox bbox;

    /* Statistics */
    size_t total_nodes_parsed;
    size_t total_ways_parsed;
    size_t total_relations_parsed;
    size_t features_kept;
    size_t multipolygons_assembled;

    /* mmap support (for binary index loading) */
    void *mmap_base;          /* mmap'd file base, NULL if not mmap'd */
    size_t mmap_size;         /* mmap'd file size */
    void *mmap_coords;        /* Allocated coordinate block (for mmap'd ways) */
    void *mmap_mp_coords;     /* Allocated coordinate block (for mmap'd multipolygons) */
    void *mmap_mp_rings;      /* Allocated ring block (for mmap'd multipolygons) */
    void *mmap_boundary_coords;  /* Allocated coordinate block (for mmap'd boundaries) */
    int rtree_is_mmap;        /* 1 if R-Tree points into mmap */
    int mp_rtree_is_mmap;     /* 1 if multipolygon R-Tree points into mmap */
    int boundary_rtree_is_mmap;  /* 1 if boundary R-Tree points into mmap */

    /* Arena for parsing temporaries (reset after each PrimitiveBlock) */
    SHArena *parse_arena;

    /* Coordinate pool for way geometry (eliminates per-way malloc) */
    SHPool *coord_pool;

    /* Scratch buffers for multipolygon assembly (eliminates per-relation malloc) */
    struct {
        void *outer_segs;           /* WaySegment scratch for outer members */
        void *inner_segs;           /* WaySegment scratch for inner members */
        size_t outer_capacity;      /* Capacity in WaySegments */
        size_t inner_capacity;      /* Capacity in WaySegments */
    } mp_scratch;
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
