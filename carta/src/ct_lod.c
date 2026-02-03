/*
 * ct_lod.c - Level of Detail configuration for map tiles
 */

#include "ct_lod.h"
#include <stdlib.h>
#include <math.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define LOD_INITIAL_CAPACITY 32

/* Earth radius for distance calculations */
#define EARTH_RADIUS_M 6371000.0
#define DEG_TO_RAD(d) ((d) * 3.14159265358979323846 / 180.0)

/* ============================================================================
 * LOD Configuration Management
 * ============================================================================ */

void ct_lod_init(CTLODConfig *config)
{
    config->rules = NULL;
    config->num_rules = 0;
    config->capacity = 0;
}

void ct_lod_free(CTLODConfig *config)
{
    free(config->rules);
    ct_lod_init(config);
}

CTStatus ct_lod_add_rule(CTLODConfig *config,
                         CTLayer layer, int feature_type,
                         int min_zoom, int max_zoom,
                         float min_area_sqm, float min_length_m)
{
    if (config->num_rules >= config->capacity) {
        int new_cap = config->capacity ? config->capacity * 2 : LOD_INITIAL_CAPACITY;
        CTLODRule *new_rules = realloc(config->rules, new_cap * sizeof(CTLODRule));
        if (!new_rules) return CT_ERROR_OUT_OF_MEMORY;
        config->rules = new_rules;
        config->capacity = new_cap;
    }

    CTLODRule *rule = &config->rules[config->num_rules++];
    rule->layer = layer;
    rule->feature_type = feature_type;
    rule->min_zoom = min_zoom;
    rule->max_zoom = max_zoom;
    rule->min_area_sqm = min_area_sqm;
    rule->min_length_m = min_length_m;

    return CT_OK;
}

/* ============================================================================
 * Predefined LOD Configurations
 * ============================================================================ */

void ct_lod_default(CTLODConfig *config)
{
    ct_lod_free(config);

    /*
     * OSM Carto-matched LOD rules - tighter filtering to match openstreetmap.org
     */

    /* Roads - matched to OSM Carto zoom levels
     * https://github.com/gravitystorm/openstreetmap-carto
     */
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_MOTORWAY, 6, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_TRUNK, 7, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_PRIMARY, 9, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_SECONDARY, 11, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_TERTIARY, 12, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_RESIDENTIAL, 13, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_SERVICE, 14, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_OTHER, 15, -1, 0, 0);

    /* Buildings - OSM Carto shows at z13+ for large, z14+ for all */
    ct_lod_add_rule(config, CT_LAYER_BUILDINGS, -1, 14, -1, 2000, 0);  /* Large buildings */
    ct_lod_add_rule(config, CT_LAYER_BUILDINGS, -1, 15, -1, 0, 0);     /* All buildings */

    /* Water bodies (lakes, reservoirs, ponds) - by size
     * OSM Carto: large water early, small water later
     * CT_WATER_BODY = 100 to distinguish from linear waterways
     */
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATER_BODY, 4, -1, 100000000, 0);  /* > 100 km² */
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATER_BODY, 6, -1, 10000000, 0);   /* > 10 km² */
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATER_BODY, 8, -1, 1000000, 0);    /* > 1 km² */
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATER_BODY, 10, -1, 100000, 0);    /* > 0.1 km² */
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATER_BODY, 12, -1, 10000, 0);     /* > 0.01 km² */
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATER_BODY, 14, -1, 0, 0);         /* All water bodies */

    /* Riverbank polygons - similar to water bodies */
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATER_RIVERBANK, 6, -1, 1000000, 0);   /* > 1 km² */
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATER_RIVERBANK, 8, -1, 100000, 0);    /* > 0.1 km² */
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATER_RIVERBANK, 10, -1, 0, 0);        /* All riverbanks */

    /* Linear waterways - by type and length
     * OSM Carto: major rivers early, streams very late
     */
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATERWAY_RIVER, 8, -1, 0, 100000);  /* >100km */
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATERWAY_RIVER, 10, -1, 0, 20000);  /* >20km */
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATERWAY_RIVER, 12, -1, 0, 0);      /* All rivers */
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATERWAY_CANAL, 12, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATERWAY_STREAM, 14, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATERWAY_DRAIN, 16, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATERWAY_DITCH, 16, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATERWAY_OTHER, 15, -1, 0, 0);

    /* Railways - OSM Carto shows at z8+ for major lines */
    ct_lod_add_rule(config, CT_LAYER_RAILWAYS, -1, 9, -1, 0, 50000);   /* > 50km */
    ct_lod_add_rule(config, CT_LAYER_RAILWAYS, -1, 12, -1, 0, 0);      /* All railways */

    /* Landuse - OSM Carto is conservative with landuse
     * Large areas appear early, small areas very late
     */
    ct_lod_add_rule(config, CT_LAYER_LANDUSE, CT_LANDUSE_FOREST, 8, -1, 50000000, 0);   /* >50km² */
    ct_lod_add_rule(config, CT_LAYER_LANDUSE, CT_LANDUSE_FOREST, 10, -1, 5000000, 0);   /* >5km² */
    ct_lod_add_rule(config, CT_LAYER_LANDUSE, CT_LANDUSE_FOREST, 12, -1, 500000, 0);    /* >0.5km² */
    ct_lod_add_rule(config, CT_LAYER_LANDUSE, CT_LANDUSE_FOREST, 14, -1, 0, 0);         /* All */

    ct_lod_add_rule(config, CT_LAYER_LANDUSE, CT_LANDUSE_PARK, 11, -1, 1000000, 0);     /* >1km² */
    ct_lod_add_rule(config, CT_LAYER_LANDUSE, CT_LANDUSE_PARK, 13, -1, 100000, 0);      /* >0.1km² */
    ct_lod_add_rule(config, CT_LAYER_LANDUSE, CT_LANDUSE_PARK, 15, -1, 0, 0);           /* All */

    ct_lod_add_rule(config, CT_LAYER_LANDUSE, CT_LANDUSE_RESIDENTIAL, 13, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_LANDUSE, CT_LANDUSE_COMMERCIAL, 13, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_LANDUSE, CT_LANDUSE_INDUSTRIAL, 12, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_LANDUSE, CT_LANDUSE_MILITARY, 11, -1, 0, 0);

    ct_lod_add_rule(config, CT_LAYER_LANDUSE, CT_LANDUSE_FARMLAND, 10, -1, 10000000, 0);  /* >10km² */
    ct_lod_add_rule(config, CT_LAYER_LANDUSE, CT_LANDUSE_FARMLAND, 13, -1, 0, 0);         /* All */

    ct_lod_add_rule(config, CT_LAYER_LANDUSE, CT_LANDUSE_GRASS, 14, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_LANDUSE, CT_LANDUSE_CEMETERY, 15, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_LANDUSE, CT_LANDUSE_OTHER, 15, -1, 0, 0);

    /* Boundaries - by admin level
     * OSM Carto: country borders early, local boundaries late
     */
    ct_lod_add_rule(config, CT_LAYER_BOUNDARIES, CT_BOUNDARY_COUNTRY, 2, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_BOUNDARIES, CT_BOUNDARY_STATE, 4, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_BOUNDARIES, CT_BOUNDARY_COUNTY, 7, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_BOUNDARIES, CT_BOUNDARY_CITY, 10, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_BOUNDARIES, CT_BOUNDARY_SUBURB, 13, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_BOUNDARIES, CT_BOUNDARY_OTHER, 10, -1, 0, 0);
}

void ct_lod_detailed(CTLODConfig *config)
{
    ct_lod_free(config);

    /*
     * Detailed preset - shows more features at lower zoom levels
     * Useful for detailed area maps
     */

    /* Roads - 1 zoom earlier than OSM Carto */
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_MOTORWAY, 4, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_TRUNK, 5, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_PRIMARY, 7, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_SECONDARY, 9, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_TERTIARY, 11, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_RESIDENTIAL, 13, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_SERVICE, 14, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_OTHER, 14, -1, 0, 0);

    /* Buildings - visible earlier, with size filtering */
    ct_lod_add_rule(config, CT_LAYER_BUILDINGS, -1, 12, -1, 5000, 0);
    ct_lod_add_rule(config, CT_LAYER_BUILDINGS, -1, 13, -1, 500, 0);
    ct_lod_add_rule(config, CT_LAYER_BUILDINGS, -1, 14, -1, 0, 0);

    /* Water - all visible early */
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATER_BODY, 4, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATER_RIVERBANK, 4, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATERWAY_RIVER, 6, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATERWAY_CANAL, 8, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATERWAY_STREAM, 10, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATERWAY_DRAIN, 12, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATERWAY_DITCH, 12, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATERWAY_OTHER, 10, -1, 0, 0);

    /* Railways - visible early */
    ct_lod_add_rule(config, CT_LAYER_RAILWAYS, -1, 6, -1, 0, 0);

    /* Landuse - visible early */
    ct_lod_add_rule(config, CT_LAYER_LANDUSE, -1, 8, -1, 0, 0);

    /* Boundaries - very early */
    ct_lod_add_rule(config, CT_LAYER_BOUNDARIES, -1, 3, -1, 0, 0);
}

void ct_lod_minimal(CTLODConfig *config)
{
    ct_lod_free(config);

    /*
     * Minimal preset - fewer features, optimized for overview maps
     * Shows features 1-2 zoom levels later than OSM Carto
     */

    /* Roads - 1 zoom later than OSM Carto */
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_MOTORWAY, 6, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_TRUNK, 7, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_PRIMARY, 9, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_SECONDARY, 11, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_TERTIARY, 13, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_RESIDENTIAL, 15, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_SERVICE, 16, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_ROADS, CT_ROAD_OTHER, 16, -1, 0, 0);

    /* Buildings - only large buildings, later visibility */
    ct_lod_add_rule(config, CT_LAYER_BUILDINGS, -1, 14, -1, 5000, 0);
    ct_lod_add_rule(config, CT_LAYER_BUILDINGS, -1, 15, -1, 500, 0);
    ct_lod_add_rule(config, CT_LAYER_BUILDINGS, -1, 16, -1, 0, 0);

    /* Water bodies - only large ones early */
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATER_BODY, 6, -1, 100000000, 0);  /* > 100 km² */
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATER_BODY, 10, -1, 1000000, 0);   /* > 1 km² */
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATER_BODY, 14, -1, 0, 0);         /* All water bodies */
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATER_RIVERBANK, 8, -1, 0, 0);     /* Riverbanks at z8 */

    /* Linear waterways */
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATERWAY_RIVER, 10, -1, 0, 50000); /* >50km rivers */
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATERWAY_RIVER, 14, -1, 0, 0);     /* All rivers */
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATERWAY_CANAL, 14, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATERWAY_STREAM, 16, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATERWAY_DRAIN, 18, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATERWAY_DITCH, 18, -1, 0, 0);
    ct_lod_add_rule(config, CT_LAYER_WATER, CT_WATERWAY_OTHER, 16, -1, 0, 0);

    /* Railways - later visibility */
    ct_lod_add_rule(config, CT_LAYER_RAILWAYS, -1, 10, -1, 0, 10000);  /* > 10km */
    ct_lod_add_rule(config, CT_LAYER_RAILWAYS, -1, 14, -1, 0, 0);      /* All railways */

    /* Landuse - only large areas */
    ct_lod_add_rule(config, CT_LAYER_LANDUSE, -1, 10, -1, 10000000, 0); /* > 10 km² */
    ct_lod_add_rule(config, CT_LAYER_LANDUSE, -1, 12, -1, 1000000, 0);  /* > 1 km² */
    ct_lod_add_rule(config, CT_LAYER_LANDUSE, -1, 15, -1, 0, 0);        /* All landuse */

    /* Boundaries */
    ct_lod_add_rule(config, CT_LAYER_BOUNDARIES, -1, 5, -1, 0, 0);
}

/* ============================================================================
 * LOD Evaluation
 * ============================================================================ */

/*
 * Find the best matching rule for a feature.
 * Returns NULL if no rule matches.
 */
static const CTLODRule *find_matching_rule(const CTLODConfig *config,
                                           CTLayer layer, int feature_type,
                                           float area_sqm, float length_m)
{
    const CTLODRule *best = NULL;
    int best_specificity = -1;

    for (int i = 0; i < config->num_rules; i++) {
        const CTLODRule *rule = &config->rules[i];

        /* Must match layer */
        if (rule->layer != layer) continue;

        /* Must match feature type (or rule applies to all types) */
        if (rule->feature_type != -1 && rule->feature_type != feature_type) continue;

        /* Check size constraints */
        if (rule->min_area_sqm > 0 && area_sqm < rule->min_area_sqm) continue;
        if (rule->min_length_m > 0 && length_m < rule->min_length_m) continue;

        /* Specificity: exact type match > wildcard */
        int specificity = (rule->feature_type == -1) ? 0 : 1;
        if (specificity > best_specificity) {
            best = rule;
            best_specificity = specificity;
        }
    }

    return best;
}

int ct_lod_is_visible(const CTLODConfig *config,
                      CTLayer layer, int feature_type,
                      int zoom, float area_sqm, float length_m)
{
    /* No config = no filtering */
    if (!config || config->num_rules == 0) return 1;

    const CTLODRule *rule = find_matching_rule(config, layer, feature_type,
                                                area_sqm, length_m);

    /* No matching rule = visible (conservative default) */
    if (!rule) return 1;

    /* Check zoom range */
    if (zoom < rule->min_zoom) return 0;
    if (rule->max_zoom >= 0 && zoom > rule->max_zoom) return 0;

    return 1;
}

int ct_lod_get_min_zoom(const CTLODConfig *config,
                        CTLayer layer, int feature_type)
{
    if (!config || config->num_rules == 0) return 0;

    /* Find matching rule with 0 size constraints */
    const CTLODRule *rule = find_matching_rule(config, layer, feature_type, 0, 0);
    if (!rule) return 0;

    return rule->min_zoom;
}

/* ============================================================================
 * Geometry Size Estimation
 * ============================================================================ */

float ct_lod_estimate_area(const CTCoord *coords, int num_coords)
{
    if (num_coords < 3) return 0;

    /* Shoelace formula with approximate meter conversion */
    double sum = 0;
    double center_lat = 0;

    /* Find center latitude for projection */
    for (int i = 0; i < num_coords; i++) {
        center_lat += coords[i].lat;
    }
    center_lat /= num_coords;

    /* Meters per degree at this latitude */
    double lat_scale = EARTH_RADIUS_M * DEG_TO_RAD(1.0);
    double lon_scale = lat_scale * cos(DEG_TO_RAD(center_lat));

    /* Shoelace formula */
    for (int i = 0; i < num_coords; i++) {
        int j = (i + 1) % num_coords;

        double x1 = coords[i].lon * lon_scale;
        double y1 = coords[i].lat * lat_scale;
        double x2 = coords[j].lon * lon_scale;
        double y2 = coords[j].lat * lat_scale;

        sum += x1 * y2 - x2 * y1;
    }

    return (float)fabs(sum / 2.0);
}

float ct_lod_estimate_length(const CTCoord *coords, int num_coords)
{
    if (num_coords < 2) return 0;

    double total = 0;

    for (int i = 0; i < num_coords - 1; i++) {
        /* Haversine distance */
        double lat1 = DEG_TO_RAD(coords[i].lat);
        double lon1 = DEG_TO_RAD(coords[i].lon);
        double lat2 = DEG_TO_RAD(coords[i + 1].lat);
        double lon2 = DEG_TO_RAD(coords[i + 1].lon);

        double dlat = lat2 - lat1;
        double dlon = lon2 - lon1;

        double a = sin(dlat / 2) * sin(dlat / 2) +
                   cos(lat1) * cos(lat2) * sin(dlon / 2) * sin(dlon / 2);
        double c = 2 * atan2(sqrt(a), sqrt(1 - a));

        total += EARTH_RADIUS_M * c;
    }

    return (float)total;
}

void ct_lod_bbox_pixels(const CTTilePoint *points, int num_points,
                        float scale, float *width, float *height)
{
    if (num_points < 1) {
        *width = 0;
        *height = 0;
        return;
    }

    int min_x = points[0].x, max_x = points[0].x;
    int min_y = points[0].y, max_y = points[0].y;

    for (int i = 1; i < num_points; i++) {
        if (points[i].x < min_x) min_x = points[i].x;
        if (points[i].x > max_x) max_x = points[i].x;
        if (points[i].y < min_y) min_y = points[i].y;
        if (points[i].y > max_y) max_y = points[i].y;
    }

    *width = (max_x - min_x) * scale;
    *height = (max_y - min_y) * scale;
}
