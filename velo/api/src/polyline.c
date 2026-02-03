/*
 * polyline.c - Google Polyline encoding/decoding implementation
 */

#include "polyline.h"
#include <math.h>
#include <string.h>
#include <stdint.h>

/*
 * Encode a single value using the Google Polyline algorithm.
 * Returns number of characters written.
 */
static size_t encode_value(int value, char *output, size_t capacity) {
    size_t written = 0;

    /* Left-shift and invert if negative */
    int encoded = value < 0 ? ~(value << 1) : (value << 1);

    /* Break into 5-bit chunks */
    while (encoded >= 0x20) {
        if (written >= capacity) return 0;
        output[written++] = (char)((encoded & 0x1F) | 0x20) + 63;
        encoded >>= 5;
    }

    if (written >= capacity) return 0;
    output[written++] = (char)(encoded + 63);

    return written;
}

size_t polyline_encode(const double *coords, size_t count, int precision,
                       char *output, size_t capacity) {
    if (!coords || !output || count == 0 || capacity == 0) return 0;

    double factor = pow(10, precision);
    int prev_lat = 0;
    int prev_lon = 0;
    size_t written = 0;

    for (size_t i = 0; i < count; i++) {
        double lat = coords[i * 2];
        double lon = coords[i * 2 + 1];

        /* Round to precision and convert to integer */
        int curr_lat = (int)round(lat * factor);
        int curr_lon = (int)round(lon * factor);

        /* Calculate deltas */
        int dlat = curr_lat - prev_lat;
        int dlon = curr_lon - prev_lon;

        /* Encode latitude delta */
        size_t n = encode_value(dlat, output + written, capacity - written);
        if (n == 0 && dlat != 0) return 0;
        written += n;

        /* Encode longitude delta */
        n = encode_value(dlon, output + written, capacity - written);
        if (n == 0 && dlon != 0) return 0;
        written += n;

        prev_lat = curr_lat;
        prev_lon = curr_lon;
    }

    /* Null terminate */
    if (written >= capacity) return 0;
    output[written] = '\0';

    return written;
}

size_t polyline_decode(const char *encoded, int precision,
                       double *coords, size_t capacity) {
    if (!encoded || !coords || capacity == 0) return 0;

    double factor = pow(10, precision);
    size_t len = strlen(encoded);
    size_t pos = 0;
    size_t count = 0;
    int lat = 0;
    int lon = 0;

    while (pos < len) {
        /* Decode latitude */
        int result = 0;
        int shift = 0;
        int b;
        do {
            if (pos >= len) return 0;
            b = encoded[pos++] - 63;
            result |= (b & 0x1F) << shift;
            shift += 5;
        } while (b >= 0x20);
        int dlat = (result & 1) ? ~(result >> 1) : (result >> 1);
        lat += dlat;

        /* Decode longitude */
        result = 0;
        shift = 0;
        do {
            if (pos >= len) return 0;
            b = encoded[pos++] - 63;
            result |= (b & 0x1F) << shift;
            shift += 5;
        } while (b >= 0x20);
        int dlon = (result & 1) ? ~(result >> 1) : (result >> 1);
        lon += dlon;

        /* Store coordinate */
        if (count >= capacity) break;
        coords[count * 2] = lat / factor;
        coords[count * 2 + 1] = lon / factor;
        count++;
    }

    return count;
}

size_t polyline_max_encoded_size(size_t count) {
    /*
     * Each coordinate pair (lat, lon) can have deltas.
     * Worst case: each value needs up to 6 chunks of 5 bits = 6 chars.
     * Two values per point = 12 chars max per point.
     * Add 1 for null terminator.
     */
    if (count > (SIZE_MAX - 1) / 12) {
        return SIZE_MAX;  /* Overflow protection - caller should check */
    }
    return count * 12 + 1;
}
