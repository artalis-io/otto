/*
 * sh_geojson.h - GeoJSON RFC 7946 Encoder
 *
 * Streaming FeatureCollection encoder built on ShJsonWriter/ShJsonBuf.
 * Currently supports Point geometry only (sufficient for facility visualization).
 *
 * Dependencies: sh_json.h
 *
 * Usage:
 *   ShJsonBuf jb;
 *   sh_json_buf_init(&jb);
 *   ShJsonWriter w;
 *   sh_json_writer_init(&w, sh_json_buf_write, &jb);
 *
 *   sh_geojson_begin(&w);
 *
 *   ShGeoJsonProp props[] = { {"city", "Budapest"}, {"type", "depot"} };
 *   sh_geojson_point_feature(&w, "loc-1", 19.07, 47.48, 6, props, 2);
 *
 *   sh_geojson_end(&w);
 *
 *   char *geojson = sh_json_buf_take(&jb);
 *   // ... use geojson ...
 *   free(geojson);
 */

#ifndef SH_GEOJSON_H
#define SH_GEOJSON_H

#include "sh_json.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Property key-value pair for Feature properties */
typedef struct {
    const char *key;
    const char *value;   /* String value (numbers written as strings) */
} ShGeoJsonProp;

/*
 * Write GeoJSON FeatureCollection header.
 * Must be followed by feature writes, then sh_geojson_end().
 */
void sh_geojson_begin(ShJsonWriter *w);

/*
 * Write a single Point Feature.
 *
 * @param w          JSON writer (from sh_geojson_begin)
 * @param id         Feature ID string (NULL to omit)
 * @param lon        Longitude (GeoJSON coordinate order: lon first!)
 * @param lat        Latitude
 * @param precision  Decimal places for coordinates
 * @param props      Property key-value pairs (or NULL for empty)
 * @param prop_count Number of property pairs
 */
void sh_geojson_point_feature(ShJsonWriter *w,
                               const char *id,
                               double lon, double lat, int precision,
                               const ShGeoJsonProp *props, size_t prop_count);

/*
 * End the FeatureCollection (close features array + root object).
 */
void sh_geojson_end(ShJsonWriter *w);

#ifdef __cplusplus
}
#endif

#endif /* SH_GEOJSON_H */
