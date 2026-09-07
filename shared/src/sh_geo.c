/*
 * sh_geo.c - Geographic utilities implementation
 *
 * Haversine distance, coordinate operations, and Web Mercator projection.
 */

#include "sh_geo.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Version
 * ============================================================================ */

const char *sh_version(void)
{
    return "1.0.0";
}

/* ============================================================================
 * Distance Calculations
 * ============================================================================ */

double sh_haversine(SHCoord a, SHCoord b)
{
    double lat1 = a.lat * SH_DEG_TO_RAD;
    double lat2 = b.lat * SH_DEG_TO_RAD;
    double dlat = (b.lat - a.lat) * SH_DEG_TO_RAD;
    double dlon = (b.lon - a.lon) * SH_DEG_TO_RAD;

    double sin_dlat = sin(dlat / 2.0);
    double sin_dlon = sin(dlon / 2.0);

    double h = sin_dlat * sin_dlat +
               cos(lat1) * cos(lat2) * sin_dlon * sin_dlon;

    /* Clamp to handle floating point errors */
    if (h > 1.0) h = 1.0;

    return 2.0 * SH_EARTH_RADIUS_M * asin(sqrt(h));
}

double sh_distance_fast(SHCoord a, SHCoord b)
{
    double lat1 = a.lat * SH_DEG_TO_RAD;
    double lat2 = b.lat * SH_DEG_TO_RAD;
    double dlat = lat2 - lat1;
    double dlon = (b.lon - a.lon) * SH_DEG_TO_RAD;

    /* Equirectangular approximation with latitude correction */
    double cos_lat = cos((lat1 + lat2) / 2.0);
    double x = dlon * cos_lat;
    double y = dlat;

    return SH_EARTH_RADIUS_M * sqrt(x * x + y * y);
}

double sh_haversine_fixed(SHCoordFixed a, SHCoordFixed b)
{
    SHCoord ca = SH_FIXED_TO_COORD(a);
    SHCoord cb = SH_FIXED_TO_COORD(b);
    return sh_haversine(ca, cb);
}

double sh_distance_fast_fixed(SHCoordFixed a, SHCoordFixed b)
{
    SHCoord ca = SH_FIXED_TO_COORD(a);
    SHCoord cb = SH_FIXED_TO_COORD(b);
    return sh_distance_fast(ca, cb);
}

/* ============================================================================
 * Coordinate Utilities
 * ============================================================================ */

int sh_coord_valid(SHCoord c)
{
    return c.lat >= -90.0 && c.lat <= 90.0 &&
           c.lon >= -180.0 && c.lon <= 180.0;
}

int sh_parse_coord(const char *str, SHCoord *coord)
{
    if (!str || !*str || !coord) return -1;

    /* Copy to mutable buffer for parsing */
    char buf[64];
    size_t len = strlen(str);
    if (len >= sizeof(buf)) return -1;
    memcpy(buf, str, len + 1);

    /* Find comma separator */
    char *comma = strchr(buf, ',');
    if (!comma) return -1;
    *comma = '\0';

    /* Parse latitude using strtod for proper error detection */
    char *end_lat;
    double lat = strtod(buf, &end_lat);
    if (end_lat == buf || *end_lat != '\0') return -1;

    /* Parse longitude */
    char *end_lon;
    double lon = strtod(comma + 1, &end_lon);
    if (end_lon == comma + 1 || *end_lon != '\0') return -1;

    /* Reject inf/NaN from malformed input like "1e1000" */
    if (!isfinite(lat) || !isfinite(lon)) return -1;

    /* Validate ranges */
    if (lat < -90.0 || lat > 90.0) return -1;
    if (lon < -180.0 || lon > 180.0) return -1;

    coord->lat = lat;
    coord->lon = lon;
    return 0;
}

int sh_coord_in_bbox(SHCoord c, SHBBox bbox)
{
    return c.lat >= bbox.min_lat && c.lat <= bbox.max_lat &&
           c.lon >= bbox.min_lon && c.lon <= bbox.max_lon;
}

SHCoord sh_coord_midpoint(SHCoord a, SHCoord b)
{
    SHCoord mid;
    mid.lat = (a.lat + b.lat) / 2.0;
    mid.lon = (a.lon + b.lon) / 2.0;
    return mid;
}

double sh_bearing(SHCoord a, SHCoord b)
{
    double lat1 = a.lat * SH_DEG_TO_RAD;
    double lat2 = b.lat * SH_DEG_TO_RAD;
    double dlon = (b.lon - a.lon) * SH_DEG_TO_RAD;

    double y = sin(dlon) * cos(lat2);
    double x = cos(lat1) * sin(lat2) - sin(lat1) * cos(lat2) * cos(dlon);

    double bearing = atan2(y, x) * SH_RAD_TO_DEG;

    /* Normalize to 0-360 */
    if (bearing < 0) bearing += 360.0;

    return bearing;
}

SHCoord sh_destination(SHCoord start, double bearing_deg, double distance_m)
{
    double lat1 = start.lat * SH_DEG_TO_RAD;
    double lon1 = start.lon * SH_DEG_TO_RAD;
    double bearing = bearing_deg * SH_DEG_TO_RAD;
    double d = distance_m / SH_EARTH_RADIUS_M;

    double sin_d = sin(d);
    double cos_d = cos(d);
    double sin_lat1 = sin(lat1);
    double cos_lat1 = cos(lat1);

    double lat2 = asin(sin_lat1 * cos_d + cos_lat1 * sin_d * cos(bearing));
    double lon2 = lon1 + atan2(sin(bearing) * sin_d * cos_lat1,
                               cos_d - sin_lat1 * sin(lat2));

    SHCoord dest;
    dest.lat = lat2 * SH_RAD_TO_DEG;
    dest.lon = lon2 * SH_RAD_TO_DEG;

    /* Normalize longitude */
    while (dest.lon > 180.0) dest.lon -= 360.0;
    while (dest.lon < -180.0) dest.lon += 360.0;

    return dest;
}

/* ============================================================================
 * Bounding Box Operations
 * ============================================================================ */

void sh_bbox_init(SHBBox *bbox)
{
    bbox->min_lat = 90.0;
    bbox->min_lon = 180.0;
    bbox->max_lat = -90.0;
    bbox->max_lon = -180.0;
}

int sh_bbox_valid(SHBBox bbox)
{
    return bbox.min_lat <= bbox.max_lat && bbox.min_lon <= bbox.max_lon;
}

void sh_bbox_expand(SHBBox *bbox, SHCoord c)
{
    if (c.lat < bbox->min_lat) bbox->min_lat = c.lat;
    if (c.lat > bbox->max_lat) bbox->max_lat = c.lat;
    if (c.lon < bbox->min_lon) bbox->min_lon = c.lon;
    if (c.lon > bbox->max_lon) bbox->max_lon = c.lon;
}

int sh_bbox_intersects(SHBBox a, SHBBox b)
{
    return a.min_lat <= b.max_lat && a.max_lat >= b.min_lat &&
           a.min_lon <= b.max_lon && a.max_lon >= b.min_lon;
}

SHBBox sh_bbox_union(SHBBox a, SHBBox b)
{
    SHBBox u;
    u.min_lat = (a.min_lat < b.min_lat) ? a.min_lat : b.min_lat;
    u.min_lon = (a.min_lon < b.min_lon) ? a.min_lon : b.min_lon;
    u.max_lat = (a.max_lat > b.max_lat) ? a.max_lat : b.max_lat;
    u.max_lon = (a.max_lon > b.max_lon) ? a.max_lon : b.max_lon;
    return u;
}

/* ============================================================================
 * Web Mercator Projection
 * ============================================================================ */

/* Maximum latitude for Web Mercator (atan(sinh(pi)) in degrees) */
#define WEB_MERCATOR_MAX_LAT 85.051128779806589

void sh_latlon_to_mercator(double lat, double lon, double *x, double *y)
{
    /* Clamp latitude to valid range */
    if (lat > WEB_MERCATOR_MAX_LAT) lat = WEB_MERCATOR_MAX_LAT;
    if (lat < -WEB_MERCATOR_MAX_LAT) lat = -WEB_MERCATOR_MAX_LAT;

    *x = lon * SH_DEG_TO_RAD * SH_EARTH_RADIUS_M;
    *y = log(tan(M_PI / 4.0 + lat * SH_DEG_TO_RAD / 2.0)) * SH_EARTH_RADIUS_M;
}

void sh_mercator_to_latlon(double x, double y, double *lat, double *lon)
{
    *lon = x / SH_EARTH_RADIUS_M * SH_RAD_TO_DEG;
    *lat = (2.0 * atan(exp(y / SH_EARTH_RADIUS_M)) - M_PI / 2.0) * SH_RAD_TO_DEG;
}

void sh_latlon_to_tile(double lat, double lon, int zoom, int *tile_x, int *tile_y)
{
    double n = (double)sh_tiles_per_axis(zoom);

    /* Clamp latitude */
    if (lat > WEB_MERCATOR_MAX_LAT) lat = WEB_MERCATOR_MAX_LAT;
    if (lat < -WEB_MERCATOR_MAX_LAT) lat = -WEB_MERCATOR_MAX_LAT;

    double lat_rad = lat * SH_DEG_TO_RAD;

    *tile_x = (int)floor((lon + 180.0) / 360.0 * n);
    *tile_y = (int)floor((1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * n);

    /* Clamp to valid range */
    int max_tile = sh_tile_max_index(zoom);
    if (*tile_x < 0) *tile_x = 0;
    if (*tile_x > max_tile) *tile_x = max_tile;
    if (*tile_y < 0) *tile_y = 0;
    if (*tile_y > max_tile) *tile_y = max_tile;
}

SHBBox sh_tile_bounds(int zoom, int tile_x, int tile_y)
{
    double n = (double)sh_tiles_per_axis(zoom);
    SHBBox bbox;

    /* Longitude bounds */
    bbox.min_lon = (double)tile_x / n * 360.0 - 180.0;
    bbox.max_lon = (double)(tile_x + 1) / n * 360.0 - 180.0;

    /* Latitude bounds (inverted y axis in tile coordinates) */
    double lat_rad_max = atan(sinh(M_PI * (1.0 - 2.0 * (double)tile_y / n)));
    double lat_rad_min = atan(sinh(M_PI * (1.0 - 2.0 * (double)(tile_y + 1) / n)));

    bbox.max_lat = lat_rad_max * SH_RAD_TO_DEG;
    bbox.min_lat = lat_rad_min * SH_RAD_TO_DEG;

    return bbox;
}

/* ============================================================================
 * Local Cartesian Projection
 * ============================================================================ */

void sh_latlon_to_local(SHCoord coord, SHCoord ref, double *x, double *y)
{
    double cos_lat = cos(ref.lat * SH_DEG_TO_RAD);
    *x = (coord.lon - ref.lon) * SH_DEG_TO_RAD * SH_EARTH_RADIUS_M * cos_lat;
    *y = (coord.lat - ref.lat) * SH_DEG_TO_RAD * SH_EARTH_RADIUS_M;
}

void sh_local_to_latlon(double x, double y, SHCoord ref, SHCoord *coord)
{
    double cos_lat = cos(ref.lat * SH_DEG_TO_RAD);

    /* Guard against division by near-zero at poles (|lat| > 89.9 degrees).
     * Trucking routes don't go to poles, but handle gracefully. */
    if (cos_lat < 1e-6) {
        cos_lat = 1e-6;
    }

    coord->lon = ref.lon + (x / (SH_EARTH_RADIUS_M * cos_lat)) * SH_RAD_TO_DEG;
    coord->lat = ref.lat + (y / SH_EARTH_RADIUS_M) * SH_RAD_TO_DEG;
}
