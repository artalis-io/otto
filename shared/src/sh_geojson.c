/*
 * sh_geojson.c - GeoJSON RFC 7946 Encoder
 *
 * Streaming FeatureCollection encoder using ShJsonWriter.
 * Supports Point geometry for facility/depot visualization.
 */

#include "sh_geojson.h"
#include <stdio.h>
#include <string.h>

/* Write coordinate with fixed decimal places (unlike %.*g which uses sig figs) */
static void geojson_write_coord(ShJsonWriter *w, double val, int precision)
{
    char buf[64];
    int len;
    if (precision <= 0) {
        len = snprintf(buf, sizeof(buf), "%.0f", val);
    } else {
        len = snprintf(buf, sizeof(buf), "%.*f", precision, val);
        /* Trim trailing zeros after decimal point */
        if (len > 0 && memchr(buf, '.', (size_t)len)) {
            while (len > 1 && buf[len - 1] == '0') len--;
            if (len > 0 && buf[len - 1] == '.') len--;
        }
    }
    if (len > 0) sh_json_write_raw(w, buf, (size_t)len);
}

void sh_geojson_begin(ShJsonWriter *w)
{
    if (!w) return;
    sh_json_write_object_start(w);
    sh_json_write_kv_string(w, "type", "FeatureCollection");
    sh_json_write_key(w, "features");
    sh_json_write_array_start(w);
}

void sh_geojson_point_feature(ShJsonWriter *w,
                               const char *id,
                               double lon, double lat, int precision,
                               const ShGeoJsonProp *props, size_t prop_count)
{
    if (!w) return;

    sh_json_write_object_start(w);

    sh_json_write_kv_string(w, "type", "Feature");

    if (id) {
        sh_json_write_kv_string(w, "id", id);
    }

    /* Geometry */
    sh_json_write_key(w, "geometry");
    sh_json_write_object_start(w);
    sh_json_write_kv_string(w, "type", "Point");
    sh_json_write_key(w, "coordinates");
    sh_json_write_array_start(w);
    geojson_write_coord(w, lon, precision);
    geojson_write_coord(w, lat, precision);
    sh_json_write_array_end(w);
    sh_json_write_object_end(w);

    /* Properties */
    sh_json_write_key(w, "properties");
    sh_json_write_object_start(w);
    if (props) {
        for (size_t i = 0; i < prop_count; i++) {
            if (props[i].key) {
                sh_json_write_kv_string(w, props[i].key,
                                         props[i].value ? props[i].value : "");
            }
        }
    }
    sh_json_write_object_end(w);

    sh_json_write_object_end(w);
}

void sh_geojson_end(ShJsonWriter *w)
{
    if (!w) return;
    sh_json_write_array_end(w);
    sh_json_write_object_end(w);
}
