/*
 * ct_lod.h - Level of Detail configuration for map tiles
 *
 * Provides zoom-dependent feature filtering to reduce visual clutter
 * at low zoom levels and improve rendering performance.
 */

#ifndef CT_LOD_H
#define CT_LOD_H

#include "ct_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * LOD Rule Structure
 * ============================================================================ */

/*
 * Single LOD rule that determines when a feature type is visible.
 *
 * Features are visible when:
 *   min_zoom <= current_zoom <= max_zoom  (zoom check)
 *   AND area >= min_area_sqm              (for polygons)
 *   AND length >= min_length_m            (for lines)
 */
typedef struct CTLODRule {
    CTLayer layer;           /* Feature layer */
    int feature_type;        /* Layer-specific subtype (-1 for all types in layer) */
    int min_zoom;            /* Minimum zoom to show this feature */
    int max_zoom;            /* Maximum zoom (-1 for no limit) */
    float min_area_sqm;      /* Minimum area for polygons at this zoom (0 to ignore) */
    float min_length_m;      /* Minimum length for lines at this zoom (0 to ignore) */
} CTLODRule;

/*
 * LOD configuration containing multiple rules.
 */
typedef struct CTLODConfig {
    CTLODRule *rules;
    int num_rules;
    int capacity;
} CTLODConfig;

/* ============================================================================
 * LOD Configuration Management
 * ============================================================================ */

/*
 * Initialize an empty LOD configuration.
 */
void ct_lod_init(CTLODConfig *config);

/*
 * Free LOD configuration memory.
 */
void ct_lod_free(CTLODConfig *config);

/*
 * Deep copy LOD configuration.
 * Destination must be initialized (ct_lod_init) or zeroed.
 */
void ct_lod_copy(CTLODConfig *dest, const CTLODConfig *src);

/*
 * Add a rule to the LOD configuration.
 *
 * @param config       Configuration to modify
 * @param layer        Feature layer
 * @param feature_type Layer-specific type (-1 for all)
 * @param min_zoom     Minimum zoom level to show
 * @param max_zoom     Maximum zoom level (-1 for no limit)
 * @param min_area_sqm Minimum area for polygons (0 to ignore)
 * @param min_length_m Minimum length for lines (0 to ignore)
 * @return CT_OK on success
 */
CTStatus ct_lod_add_rule(CTLODConfig *config,
                         CTLayer layer, int feature_type,
                         int min_zoom, int max_zoom,
                         float min_area_sqm, float min_length_m);

/* ============================================================================
 * Predefined LOD Configurations
 * ============================================================================ */

/*
 * Load default LOD configuration (OSM Carto-style rules).
 *
 * Default rules follow OpenStreetMap's progressive disclosure:
 *   Roads:     Motorway z5+, Trunk z6+, Primary z8+, Secondary z10+,
 *              Tertiary z12+, Residential z14+, Service/Other z15+
 *   Buildings: Large (>5000m²) z13+, Medium (>500m²) z14+, All z15+
 *   Water:     Large (>100km²) z4+, Medium (>1km²) z8+, Small z12+, Streams z14+
 *   Railways:  Main (>10km) z8+, All z12+
 *   Landuse:   Large (>10km²) z8+, Parks (>1km²) z10+, All z14+
 */
void ct_lod_default(CTLODConfig *config);

/*
 * Load detailed LOD configuration.
 * Shows more features at lower zoom levels than default.
 */
void ct_lod_detailed(CTLODConfig *config);

/*
 * Load minimal LOD configuration.
 * Shows fewer features, optimized for overview maps.
 */
void ct_lod_minimal(CTLODConfig *config);

/* ============================================================================
 * LOD Evaluation
 * ============================================================================ */

/*
 * Check if a feature should be visible at a given zoom level.
 *
 * @param config       LOD configuration (NULL = no filtering)
 * @param layer        Feature layer
 * @param feature_type Layer-specific type
 * @param zoom         Current zoom level
 * @param area_sqm     Feature area in square meters (for polygons)
 * @param length_m     Feature length in meters (for lines)
 * @return 1 if visible, 0 if should be filtered out
 */
int ct_lod_is_visible(const CTLODConfig *config,
                      CTLayer layer, int feature_type,
                      int zoom, float area_sqm, float length_m);

/*
 * Get minimum zoom level for a feature type.
 *
 * @param config       LOD configuration (NULL returns 0)
 * @param layer        Feature layer
 * @param feature_type Layer-specific type
 * @return Minimum zoom level, or 0 if no rule found
 */
int ct_lod_get_min_zoom(const CTLODConfig *config,
                        CTLayer layer, int feature_type);

/* ============================================================================
 * Geometry Size Estimation
 * ============================================================================ */

/*
 * Estimate the area of a polygon in square meters.
 *
 * Uses the Shoelace formula with geographic coordinate conversion.
 *
 * @param coords     Array of coordinates
 * @param num_coords Number of coordinates
 * @return Approximate area in square meters
 */
float ct_lod_estimate_area(const CTCoord *coords, int num_coords);

/*
 * Estimate the length of a linestring in meters.
 *
 * Uses Haversine distance between consecutive points.
 *
 * @param coords     Array of coordinates
 * @param num_coords Number of coordinates
 * @return Approximate length in meters
 */
float ct_lod_estimate_length(const CTCoord *coords, int num_coords);

/*
 * Calculate feature bounding box size in pixels.
 *
 * Useful for render-time filtering of features too small to see.
 *
 * @param points     Array of tile points
 * @param num_points Number of points
 * @param width      Output: bounding box width in pixels
 * @param height     Output: bounding box height in pixels
 */
void ct_lod_bbox_pixels(const CTTilePoint *points, int num_points,
                        float scale, float *width, float *height);

#ifdef __cplusplus
}
#endif

#endif /* CT_LOD_H */
