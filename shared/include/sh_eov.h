/*
 * sh_eov.h - Hungarian HD72/EOV ↔ WGS84 Coordinate Conversion
 *
 * Converts between Hungarian Unified National Projection (EOV/HD72)
 * and WGS84 (GPS) coordinates using a simplified transformation.
 *
 * EOV uses meters (Y = easting, X = northing) in the Hungarian grid.
 * WGS84 uses decimal degrees (latitude, longitude).
 */

#ifndef SH_EOV_H
#define SH_EOV_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Convert EOV coordinates to WGS84 (latitude/longitude).
 *
 * @param eov_y   EOV Y (easting, meters) - typically 400000-900000
 * @param eov_x   EOV X (northing, meters) - typically 30000-370000
 * @param lat     Output: WGS84 latitude (degrees)
 * @param lon     Output: WGS84 longitude (degrees)
 * @return 0 on success, -1 if inputs are clearly out of EOV range
 */
int sh_eov_to_wgs84(double eov_y, double eov_x, double *lat, double *lon);

/*
 * Convert WGS84 coordinates to EOV.
 *
 * @param lat     WGS84 latitude (degrees)
 * @param lon     WGS84 longitude (degrees)
 * @param eov_y   Output: EOV Y (easting, meters)
 * @param eov_x   Output: EOV X (northing, meters)
 * @return 0 on success, -1 if inputs are outside Hungary
 */
int sh_wgs84_to_eov(double lat, double lon, double *eov_y, double *eov_x);

#ifdef __cplusplus
}
#endif

#endif /* SH_EOV_H */
