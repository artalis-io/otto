/*
 * polyline.h - Google Polyline encoding/decoding
 *
 * Implements the Google Polyline Algorithm for encoding sequences of
 * coordinates into ASCII strings. Used by Google Maps, Mapbox, etc.
 *
 * Reference: https://developers.google.com/maps/documentation/utilities/polylinealgorithm
 */

#ifndef POLYLINE_H
#define POLYLINE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Encode a sequence of coordinates to Google Polyline format.
 *
 * @param coords    Array of [lat, lon] pairs (lat0, lon0, lat1, lon1, ...)
 * @param count     Number of coordinate pairs
 * @param precision Decimal precision (5 for standard, 6 for high precision)
 * @param output    Output buffer for encoded string
 * @param capacity  Output buffer capacity
 * @return Number of characters written (excluding null terminator), or 0 on error
 */
size_t polyline_encode(const double *coords, size_t count, int precision,
                       char *output, size_t capacity);

/*
 * Decode a Google Polyline string to coordinates.
 *
 * @param encoded   Null-terminated encoded polyline string
 * @param precision Decimal precision used in encoding
 * @param coords    Output buffer for [lat, lon] pairs
 * @param capacity  Maximum number of coordinate pairs that fit in output
 * @return Number of coordinate pairs decoded, or 0 on error
 */
size_t polyline_decode(const char *encoded, int precision,
                       double *coords, size_t capacity);

/*
 * Calculate maximum buffer size needed for encoding.
 *
 * @param count Number of coordinate pairs
 * @return Maximum bytes needed (including null terminator)
 */
size_t polyline_max_encoded_size(size_t count);

#ifdef __cplusplus
}
#endif

#endif /* POLYLINE_H */
