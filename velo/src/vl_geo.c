/*
 * vl_geo.c - Geographic utilities
 *
 * Haversine distance calculation and coordinate utilities.
 */

#include "vl_types.h"
#include <math.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define DEG_TO_RAD (M_PI / 180.0)
#define RAD_TO_DEG (180.0 / M_PI)

/* ============================================================================
 * Haversine Distance
 * ============================================================================ */

/*
 * Calculate great-circle distance between two coordinates.
 * Uses the haversine formula for numerical stability.
 *
 * Returns distance in meters.
 */
double vl_haversine(VLCoord a, VLCoord b)
{
    double lat1 = a.lat * DEG_TO_RAD;
    double lat2 = b.lat * DEG_TO_RAD;
    double dlat = (b.lat - a.lat) * DEG_TO_RAD;
    double dlon = (b.lon - a.lon) * DEG_TO_RAD;

    double sin_dlat = sin(dlat / 2.0);
    double sin_dlon = sin(dlon / 2.0);

    double h = sin_dlat * sin_dlat +
               cos(lat1) * cos(lat2) * sin_dlon * sin_dlon;

    /* Clamp to handle floating point errors */
    if (h > 1.0) h = 1.0;

    return 2.0 * VL_EARTH_RADIUS_M * asin(sqrt(h));
}

/*
 * Fast equirectangular distance approximation.
 * Much faster than haversine, accurate within ~0.5% for distances < 500km.
 * Uses the approximation that lat/lon form a flat plane with cos(lat) correction.
 *
 * Returns distance in meters.
 */
double vl_distance_fast(VLCoord a, VLCoord b)
{
    double lat1 = a.lat * DEG_TO_RAD;
    double lat2 = b.lat * DEG_TO_RAD;
    double dlat = lat2 - lat1;
    double dlon = (b.lon - a.lon) * DEG_TO_RAD;

    /* Equirectangular approximation with latitude correction */
    double cos_lat = cos((lat1 + lat2) / 2.0);
    double x = dlon * cos_lat;
    double y = dlat;

    return VL_EARTH_RADIUS_M * sqrt(x * x + y * y);
}

/*
 * Calculate haversine distance between fixed-point coordinates.
 */
double vl_haversine_fixed(VLCoordFixed a, VLCoordFixed b)
{
    VLCoord ca = VL_FIXED_TO_COORD(a);
    VLCoord cb = VL_FIXED_TO_COORD(b);
    return vl_haversine(ca, cb);
}

/*
 * Fast equirectangular distance between fixed-point coordinates.
 */
double vl_distance_fast_fixed(VLCoordFixed a, VLCoordFixed b)
{
    VLCoord ca = VL_FIXED_TO_COORD(a);
    VLCoord cb = VL_FIXED_TO_COORD(b);
    return vl_distance_fast(ca, cb);
}

/* ============================================================================
 * Coordinate Utilities
 * ============================================================================ */

/*
 * Check if a coordinate is valid (within valid lat/lon ranges).
 */
int vl_coord_valid(VLCoord c)
{
    return c.lat >= -90.0 && c.lat <= 90.0 &&
           c.lon >= -180.0 && c.lon <= 180.0;
}

/*
 * Check if a coordinate is within a bounding box.
 */
int vl_coord_in_bbox(VLCoord c, VLCoord min, VLCoord max)
{
    return c.lat >= min.lat && c.lat <= max.lat &&
           c.lon >= min.lon && c.lon <= max.lon;
}

/*
 * Calculate the midpoint between two coordinates.
 */
VLCoord vl_coord_midpoint(VLCoord a, VLCoord b)
{
    /* Simple average for small distances */
    VLCoord mid;
    mid.lat = (a.lat + b.lat) / 2.0;
    mid.lon = (a.lon + b.lon) / 2.0;
    return mid;
}

/*
 * Calculate initial bearing from a to b (in degrees, 0-360).
 */
double vl_bearing(VLCoord a, VLCoord b)
{
    double lat1 = a.lat * DEG_TO_RAD;
    double lat2 = b.lat * DEG_TO_RAD;
    double dlon = (b.lon - a.lon) * DEG_TO_RAD;

    double y = sin(dlon) * cos(lat2);
    double x = cos(lat1) * sin(lat2) - sin(lat1) * cos(lat2) * cos(dlon);

    double bearing = atan2(y, x) * RAD_TO_DEG;

    /* Normalize to 0-360 */
    if (bearing < 0) bearing += 360.0;

    return bearing;
}

/*
 * Calculate destination point given start, bearing, and distance.
 */
VLCoord vl_destination(VLCoord start, double bearing_deg, double distance_m)
{
    double lat1 = start.lat * DEG_TO_RAD;
    double lon1 = start.lon * DEG_TO_RAD;
    double bearing = bearing_deg * DEG_TO_RAD;
    double d = distance_m / VL_EARTH_RADIUS_M;

    double sin_d = sin(d);
    double cos_d = cos(d);
    double sin_lat1 = sin(lat1);
    double cos_lat1 = cos(lat1);

    double lat2 = asin(sin_lat1 * cos_d + cos_lat1 * sin_d * cos(bearing));
    double lon2 = lon1 + atan2(sin(bearing) * sin_d * cos_lat1,
                               cos_d - sin_lat1 * sin(lat2));

    VLCoord dest;
    dest.lat = lat2 * RAD_TO_DEG;
    dest.lon = lon2 * RAD_TO_DEG;

    /* Normalize longitude */
    while (dest.lon > 180.0) dest.lon -= 360.0;
    while (dest.lon < -180.0) dest.lon += 360.0;

    return dest;
}

/*
 * Expand a bounding box to include a coordinate.
 */
void vl_bbox_expand(VLCoord *min, VLCoord *max, VLCoord c)
{
    if (c.lat < min->lat) min->lat = c.lat;
    if (c.lat > max->lat) max->lat = c.lat;
    if (c.lon < min->lon) min->lon = c.lon;
    if (c.lon > max->lon) max->lon = c.lon;
}

/*
 * Initialize a bounding box to invalid state.
 */
void vl_bbox_init(VLCoord *min, VLCoord *max)
{
    min->lat = 90.0;
    min->lon = 180.0;
    max->lat = -90.0;
    max->lon = -180.0;
}

/*
 * Check if a bounding box is valid (min < max).
 */
int vl_bbox_valid(VLCoord min, VLCoord max)
{
    return min.lat <= max.lat && min.lon <= max.lon;
}

/* ============================================================================
 * Speed/Duration Utilities
 * ============================================================================ */

/*
 * Get default speed for a road type (km/h).
 */
int vl_default_speed(int road_type)
{
    switch (road_type & VL_EDGE_TYPE_MASK) {
    case VL_EDGE_MOTORWAY:    return VL_SPEED_MOTORWAY;
    case VL_EDGE_TRUNK:       return VL_SPEED_TRUNK;
    case VL_EDGE_PRIMARY:     return VL_SPEED_PRIMARY;
    case VL_EDGE_SECONDARY:   return VL_SPEED_SECONDARY;
    case VL_EDGE_TERTIARY:    return VL_SPEED_TERTIARY;
    case VL_EDGE_RESIDENTIAL: return VL_SPEED_RESIDENTIAL;
    case VL_EDGE_SERVICE:     return VL_SPEED_SERVICE;
    default:                  return VL_SPEED_DEFAULT;
    }
}

/*
 * Calculate travel time for a distance at a given speed.
 * Returns time in seconds.
 */
double vl_travel_time(double distance_m, double speed_kmh)
{
    if (speed_kmh <= 0) speed_kmh = VL_SPEED_DEFAULT;
    return distance_m / (speed_kmh * (1000.0 / 3600.0));
}
