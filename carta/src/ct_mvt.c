/*
 * ct_mvt.c - Mapbox Vector Tile encoding
 *
 * Encodes tile data to MVT format (Protocol Buffers).
 * See: https://github.com/mapbox/vector-tile-spec
 */

#include "ct_mvt.h"
#include "ct_tile.h"
#include "ct_pbf.h"
#include "ct_simplify.h"
#include "ct_lod.h"
#include "sh_protobuf.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* MVT field numbers */
#define MVT_TILE_LAYERS         3

#define MVT_LAYER_VERSION       15
#define MVT_LAYER_NAME          1
#define MVT_LAYER_FEATURES      2
#define MVT_LAYER_KEYS          3
#define MVT_LAYER_VALUES        4
#define MVT_LAYER_EXTENT        5

#define MVT_FEATURE_ID          1
#define MVT_FEATURE_TAGS        2
#define MVT_FEATURE_TYPE        3
#define MVT_FEATURE_GEOMETRY    4

#define MVT_VALUE_STRING        1
#define MVT_VALUE_FLOAT         2
#define MVT_VALUE_DOUBLE        3
#define MVT_VALUE_INT           4
#define MVT_VALUE_UINT          5
#define MVT_VALUE_SINT          6
#define MVT_VALUE_BOOL          7

/* Geometry commands */
#define MVT_CMD_MOVETO          1
#define MVT_CMD_LINETO          2
#define MVT_CMD_CLOSEPATH       7

/*
 * MVT CommandInteger: the command id lives in the low three bits and the repeat
 * count in the remaining bits -- (count << 3) | (id & 0x7). The operand order
 * matters: writing (id << 3) | count instead silently produces valid-looking
 * integers only when id and count happen to coincide (e.g. MoveTo with count 1,
 * which is why points and many lines decoded while every ClosePath -- 57 rather
 * than 15 -- and any LineTo with count >= 4 did not). Use this macro so the
 * ordering is stated once.
 */
#define MVT_CMD(id, count)      (((uint32_t)(count) << 3) | ((uint32_t)(id) & 0x7u))

/* ============================================================================
 * Default Options
 * ============================================================================ */

void ct_mvt_default_options(CTMVTOptions *opts)
{
    opts->extent = CT_MVT_EXTENT;
    opts->buffer = 64;
    opts->simplify = 1;
    opts->tolerance = 4.0;
    opts->compress = 0;
}

/* ============================================================================
 * Layer Names
 * ============================================================================ */

const char *ct_mvt_layer_name(CTLayer layer)
{
    switch (layer) {
        case CT_LAYER_ROADS:      return "roads";
        case CT_LAYER_WATER:      return "water";
        case CT_LAYER_BUILDINGS:  return "buildings";
        case CT_LAYER_LANDUSE:    return "landuse";
        case CT_LAYER_RAILWAYS:   return "railways";
        case CT_LAYER_BOUNDARIES: return "boundaries";
        case CT_LAYER_LABELS:     return "labels";
        default:                  return "other";
    }
}

/* ============================================================================
 * Vector schema: feature-class strings
 *
 * Map Carta's internal classification enums (CTFeature.feature_type within a
 * CTLayer) to stable string values emitted as the MVT "class" property. This is
 * Carta's OWN schema (NOT OpenMapTiles); see carta/docs/vector-schema.md.
 * Unknown subtypes fall through to "other" rather than synthesizing a value.
 * ============================================================================ */

static const char *road_class_str(int ft)
{
    switch (ft) {
        case CT_ROAD_MOTORWAY:    return "motorway";
        case CT_ROAD_TRUNK:       return "trunk";
        case CT_ROAD_PRIMARY:     return "primary";
        case CT_ROAD_SECONDARY:   return "secondary";
        case CT_ROAD_TERTIARY:    return "tertiary";
        case CT_ROAD_RESIDENTIAL: return "residential";
        case CT_ROAD_SERVICE:     return "service";
        default:                  return "other";
    }
}

static const char *railway_class_str(int ft)
{
    switch (ft) {
        case CT_RAILWAY_RAIL:         return "rail";
        case CT_RAILWAY_SUBWAY:       return "subway";
        case CT_RAILWAY_TRAM:         return "tram";
        case CT_RAILWAY_NARROW_GAUGE: return "narrow_gauge";
        case CT_RAILWAY_PRESERVED:    return "preserved";
        case CT_RAILWAY_DISUSED:      return "disused";
        default:                      return "other";
    }
}

static const char *water_class_str(int ft)
{
    switch (ft) {
        case CT_WATERWAY_RIVER:  return "river";
        case CT_WATERWAY_CANAL:  return "canal";
        case CT_WATERWAY_STREAM: return "stream";
        case CT_WATERWAY_DRAIN:  return "drain";
        case CT_WATERWAY_DITCH:  return "ditch";
        case CT_WATER_BODY:      return "water";
        case CT_WATER_RIVERBANK: return "riverbank";
        default:                 return "other";
    }
}

static const char *landuse_class_str(int ft)
{
    switch (ft) {
        case CT_LANDUSE_FOREST:      return "forest";
        case CT_LANDUSE_PARK:        return "park";
        case CT_LANDUSE_RESIDENTIAL: return "residential";
        case CT_LANDUSE_COMMERCIAL:  return "commercial";
        case CT_LANDUSE_INDUSTRIAL:  return "industrial";
        case CT_LANDUSE_FARMLAND:    return "farmland";
        case CT_LANDUSE_GRASS:       return "grass";
        case CT_LANDUSE_CEMETERY:    return "cemetery";
        case CT_LANDUSE_MILITARY:    return "military";
        default:                     return "other";
    }
}

static const char *place_class_str(CTPlaceType t)
{
    switch (t) {
        case CT_PLACE_COUNTRY:       return "country";
        case CT_PLACE_STATE:         return "state";
        case CT_PLACE_CITY:          return "city";
        case CT_PLACE_TOWN:          return "town";
        case CT_PLACE_VILLAGE:       return "village";
        case CT_PLACE_HAMLET:        return "hamlet";
        case CT_PLACE_SUBURB:        return "suburb";
        case CT_PLACE_NEIGHBOURHOOD: return "neighbourhood";
        case CT_PLACE_LOCALITY:      return "locality";
        case CT_PLACE_ISLAND:        return "island";
        case CT_PLACE_PEAK:          return "peak";
        default:                     return "other";
    }
}

/* Class string for a geometry feature by its layer+subtype. NULL if the layer
 * carries no class (e.g. buildings, boundaries use other props). */
static const char *feature_class_str(const CTFeature *f)
{
    switch (f->layer) {
        case CT_LAYER_ROADS:    return road_class_str(f->feature_type);
        case CT_LAYER_WATER:    return water_class_str(f->feature_type);
        case CT_LAYER_LANDUSE:  return landuse_class_str(f->feature_type);
        case CT_LAYER_RAILWAYS: return railway_class_str(f->feature_type);
        default:                return NULL;
    }
}

/* ============================================================================
 * Vector schema: typed key/value dictionary (per MVT layer)
 *
 * MVT stores feature attributes out-of-line: a per-layer keys[] (strings) and
 * values[] (typed Value messages); each feature carries packed [key_idx,
 * value_idx] tag pairs. We intern keys/values (dedup) as features are encoded,
 * then emit the two dictionaries after the features in the layer message
 * (protobuf field order is irrelevant to decoders). All strings are BORROWED
 * (from the feature/way/label), but enc_write_string copies the bytes into the
 * output, so the encoded tile keeps no pointer into the source context.
 * ============================================================================ */

typedef enum { MVT_V_STRING, MVT_V_INT, MVT_V_BOOL } MVTValType;

typedef struct {
    MVTValType type;
    const char *s;   /* borrowed, for MVT_V_STRING */
    int64_t i;       /* for MVT_V_INT; 0/1 for MVT_V_BOOL */
} MVTVal;

typedef struct {
    const char **keys;  size_t nkeys, keys_cap;
    MVTVal *vals;       size_t nvals, vals_cap;
    int oom;
} MVTDict;

static void dict_init(MVTDict *d) { memset(d, 0, sizeof(*d)); }
static void dict_free(MVTDict *d) { free(d->keys); free(d->vals); memset(d, 0, sizeof(*d)); }

/* Intern a key string; returns its index, or -1 on OOM. */
static int dict_key_index(MVTDict *d, const char *key)
{
    for (size_t k = 0; k < d->nkeys; k++)
        if (strcmp(d->keys[k], key) == 0) return (int)k;
    if (d->nkeys == d->keys_cap) {
        size_t nc = d->keys_cap ? d->keys_cap * 2 : 16;
        const char **nk = realloc(d->keys, nc * sizeof(*nk));
        if (!nk) { d->oom = 1; return -1; }
        d->keys = nk; d->keys_cap = nc;
    }
    d->keys[d->nkeys] = key;
    return (int)d->nkeys++;
}

static int mvtval_eq(const MVTVal *a, const MVTVal *b)
{
    if (a->type != b->type) return 0;
    if (a->type == MVT_V_STRING) return strcmp(a->s, b->s) == 0;
    return a->i == b->i;
}

/* Intern a typed value; returns its index, or -1 on OOM. */
static int dict_val_index(MVTDict *d, MVTVal v)
{
    for (size_t j = 0; j < d->nvals; j++)
        if (mvtval_eq(&d->vals[j], &v)) return (int)j;
    if (d->nvals == d->vals_cap) {
        size_t nc = d->vals_cap ? d->vals_cap * 2 : 32;
        MVTVal *nv = realloc(d->vals, nc * sizeof(*nv));
        if (!nv) { d->oom = 1; return -1; }
        d->vals = nv; d->vals_cap = nc;
    }
    d->vals[d->nvals] = v;
    return (int)d->nvals++;
}

/* ============================================================================
 * Encoder Context
 * ============================================================================ */

void ct_mvt_encoder_init(CTMVTEncoder *enc, uint8_t *buffer, size_t capacity)
{
    enc->buffer = buffer;
    enc->capacity = capacity;
    enc->offset = 0;
    enc->error = 0;
}

static void enc_write_varint(CTMVTEncoder *enc, uint64_t value)
{
    if (enc->error) return;
    if (enc->offset >= enc->capacity) {
        enc->error = 1;
        return;
    }
    int n = sh_pb_write_varint(enc->buffer + enc->offset,
                               enc->capacity - enc->offset, value);
    if (n <= 0) {
        enc->error = 1;
        return;
    }
    enc->offset += n;
}

static void enc_write_svarint(CTMVTEncoder *enc, int64_t value)
{
    /* Zigzag encode. The left shift is done unsigned to avoid overflow UB,
     * which the previous version already handled. The sign mask no longer
     * comes from (value >> 63): right-shifting a negative signed value is
     * implementation-defined, and while every compiler we build with does an
     * arithmetic shift, the encoding does not need to rely on that. */
    uint64_t sign_mask = (value < 0) ? ~(uint64_t)0 : (uint64_t)0;
    uint64_t uval = ((uint64_t)value << 1) ^ sign_mask;
    enc_write_varint(enc, uval);
}

static void enc_write_tag(CTMVTEncoder *enc, uint32_t field, uint32_t wire)
{
    enc_write_varint(enc, ((uint64_t)field << 3) | wire);
}

static void enc_write_bytes(CTMVTEncoder *enc, const uint8_t *data, size_t len)
{
    if (enc->error) return;
    if (enc->offset + len > enc->capacity) {
        enc->error = 1;
        return;
    }
    memcpy(enc->buffer + enc->offset, data, len);
    enc->offset += len;
}

/* Reserve space in the buffer without writing. Returns 1 on success, 0 on error. */
static int enc_reserve(CTMVTEncoder *enc, size_t len)
{
    if (enc->error) return 0;
    if (enc->offset + len > enc->capacity) {
        enc->error = 1;
        return 0;
    }
    enc->offset += len;
    return 1;
}

/* Maximum layer name length for MVT encoding */
#define MVT_MAX_STRING_LEN 256

static void enc_write_string(CTMVTEncoder *enc, const char *str)
{
    size_t len = strnlen(str, MVT_MAX_STRING_LEN);
    enc_write_varint(enc, len);
    enc_write_bytes(enc, (const uint8_t *)str, len);
}

/* ============================================================================
 * Geometry Encoding
 * ============================================================================ */

static void encode_geometry(CTMVTEncoder *enc, const CTFeature *feature)
{
    /* Defensive check: skip features with no points */
    if (!feature->points || feature->num_points == 0) {
        return;
    }

    /* Encode geometry as packed uint32 array */

    /* Skip length prefix for now */
    enc_write_tag(enc, MVT_FEATURE_GEOMETRY, 2);  /* length-delimited */
    size_t len_pos = enc->offset;
    if (!enc_reserve(enc, 5)) return;  /* Reserve space for length (up to 5 bytes) */

    size_t cmd_start = enc->offset;

    int32_t cx = 0, cy = 0;  /* Cursor position */

    if (feature->type == CT_GEOM_POINT) {
        /* MoveTo command */
        enc_write_varint(enc, MVT_CMD(MVT_CMD_MOVETO, 1));

        int32_t dx = feature->points[0].x - cx;
        int32_t dy = feature->points[0].y - cy;
        enc_write_svarint(enc, dx);
        enc_write_svarint(enc, dy);
    } else if (feature->type == CT_GEOM_LINESTRING) {
        /* MoveTo first point */
        enc_write_varint(enc, MVT_CMD(MVT_CMD_MOVETO, 1));
        int32_t dx = feature->points[0].x - cx;
        int32_t dy = feature->points[0].y - cy;
        enc_write_svarint(enc, dx);
        enc_write_svarint(enc, dy);
        cx = feature->points[0].x;
        cy = feature->points[0].y;

        /* LineTo remaining points */
        if (feature->num_points > 1) {
            enc_write_varint(enc, MVT_CMD(MVT_CMD_LINETO, feature->num_points - 1));

            for (int i = 1; i < feature->num_points; i++) {
                dx = feature->points[i].x - cx;
                dy = feature->points[i].y - cy;
                enc_write_svarint(enc, dx);
                enc_write_svarint(enc, dy);
                cx = feature->points[i].x;
                cy = feature->points[i].y;
            }
        }
    } else if (feature->type == CT_GEOM_POLYGON) {
        /* Handle multipolygons (multiple rings) */
        int num_rings = (feature->num_rings > 0 && feature->ring_ends) ?
                        feature->num_rings : 1;
        int ring_start = 0;

        for (int r = 0; r < num_rings; r++) {
            int ring_end = (feature->ring_ends && r < feature->num_rings) ?
                           feature->ring_ends[r] : feature->num_points;
            int ring_points = ring_end - ring_start;

            if (ring_points < 3) {
                ring_start = ring_end;
                continue;
            }

            /* MoveTo first point of ring */
            enc_write_varint(enc, MVT_CMD(MVT_CMD_MOVETO, 1));
            int32_t dx = feature->points[ring_start].x - cx;
            int32_t dy = feature->points[ring_start].y - cy;
            enc_write_svarint(enc, dx);
            enc_write_svarint(enc, dy);
            cx = feature->points[ring_start].x;
            cy = feature->points[ring_start].y;

            /* LineTo remaining points (excluding last if it closes the ring) */
            int last_idx = ring_end - 1;
            if (feature->points[last_idx].x == feature->points[ring_start].x &&
                feature->points[last_idx].y == feature->points[ring_start].y) {
                last_idx--;
            }

            int lineto_count = last_idx - ring_start;
            if (lineto_count > 0) {
                enc_write_varint(enc, MVT_CMD(MVT_CMD_LINETO, lineto_count));

                for (int i = ring_start + 1; i <= last_idx; i++) {
                    dx = feature->points[i].x - cx;
                    dy = feature->points[i].y - cy;
                    enc_write_svarint(enc, dx);
                    enc_write_svarint(enc, dy);
                    cx = feature->points[i].x;
                    cy = feature->points[i].y;
                }
            }

            /* ClosePath */
            enc_write_varint(enc, MVT_CMD(MVT_CMD_CLOSEPATH, 1));

            ring_start = ring_end;
        }
    }

    /* Write actual length */
    size_t cmd_len = enc->offset - cmd_start;
    size_t saved_offset = enc->offset;

    enc->offset = len_pos;
    enc_write_varint(enc, cmd_len);

    /* Move commands if length varint was shorter than 5 bytes */
    size_t len_size = enc->offset - len_pos;
    if (len_size < 5) {
        memmove(enc->buffer + len_pos + len_size,
                enc->buffer + len_pos + 5, cmd_len);
        enc->offset = len_pos + len_size + cmd_len;
    } else {
        enc->offset = saved_offset;
    }
}

/* ============================================================================
 * Property / tag / value encoding
 * ============================================================================ */

#define MVT_MAX_PROPS 8

typedef struct {
    const char *key;
    MVTVal val;
} MVTProp;

/* Collect the schema properties for a geometry feature (roads/water/landuse/
 * railways carry class + name; roads also carry bridge/tunnel bools). Returns
 * the number of properties written (<= MVT_MAX_PROPS). */
static int collect_geometry_props(const CTFeature *f, MVTProp *out)
{
    int n = 0;
    const char *cls = feature_class_str(f);
    if (cls) {
        out[n].key = "class";
        out[n].val = (MVTVal){ MVT_V_STRING, cls, 0 };
        n++;
    }
    if (f->name && f->name[0]) {
        out[n].key = "name";
        out[n].val = (MVTVal){ MVT_V_STRING, f->name, 0 };
        n++;
    }
    if (f->layer == CT_LAYER_ROADS) {
        if (f->flags & CT_FLAG_BRIDGE) {
            out[n].key = "bridge";
            out[n].val = (MVTVal){ MVT_V_BOOL, NULL, 1 };
            n++;
        }
        if (f->flags & CT_FLAG_TUNNEL) {
            out[n].key = "tunnel";
            out[n].val = (MVTVal){ MVT_V_BOOL, NULL, 1 };
            n++;
        }
    }
    return n;
}

/* Emit a feature's packed tag field (field 2): [key_idx, value_idx] pairs,
 * interning into the layer dictionary. No-op when there are no properties. */
static void encode_tags(CTMVTEncoder *enc, MVTDict *dict,
                        const MVTProp *props, int nprops)
{
    uint32_t idx[2 * MVT_MAX_PROPS];
    int ni = 0;
    if (nprops <= 0) return;
    for (int i = 0; i < nprops; i++) {
        int ki = dict_key_index(dict, props[i].key);
        int vi = dict_val_index(dict, props[i].val);
        if (ki < 0 || vi < 0) return;  /* OOM (dict->oom set): emit no tags */
        idx[ni++] = (uint32_t)ki;
        idx[ni++] = (uint32_t)vi;
    }

    enc_write_tag(enc, MVT_FEATURE_TAGS, 2);
    size_t len_pos = enc->offset;
    if (!enc_reserve(enc, 5)) return;
    size_t start = enc->offset;
    for (int i = 0; i < ni; i++) enc_write_varint(enc, idx[i]);

    size_t clen = enc->offset - start;
    size_t saved = enc->offset;
    enc->offset = len_pos;
    enc_write_varint(enc, clen);
    size_t ls = enc->offset - len_pos;
    if (ls < 5) {
        memmove(enc->buffer + len_pos + ls, enc->buffer + len_pos + 5, clen);
        enc->offset = len_pos + ls + clen;
    } else {
        enc->offset = saved;
    }
}

/* Emit one layer Value sub-message (field 4) for a typed value. */
static void encode_value_message(CTMVTEncoder *enc, const MVTVal *v)
{
    enc_write_tag(enc, MVT_LAYER_VALUES, 2);
    size_t len_pos = enc->offset;
    if (!enc_reserve(enc, 5)) return;
    size_t start = enc->offset;

    if (v->type == MVT_V_STRING) {
        enc_write_tag(enc, MVT_VALUE_STRING, 2);
        enc_write_string(enc, v->s ? v->s : "");
    } else if (v->type == MVT_V_BOOL) {
        enc_write_tag(enc, MVT_VALUE_BOOL, 0);
        enc_write_varint(enc, v->i ? 1 : 0);
    } else { /* MVT_V_INT: int_value (field 4), plain varint int64 */
        enc_write_tag(enc, MVT_VALUE_INT, 0);
        enc_write_varint(enc, (uint64_t)v->i);
    }

    size_t clen = enc->offset - start;
    size_t saved = enc->offset;
    enc->offset = len_pos;
    enc_write_varint(enc, clen);
    size_t ls = enc->offset - len_pos;
    if (ls < 5) {
        memmove(enc->buffer + len_pos + ls, enc->buffer + len_pos + 5, clen);
        enc->offset = len_pos + ls + clen;
    } else {
        enc->offset = saved;
    }
}

/* Emit a layer's keys (field 3) then values (field 4) dictionaries. */
static void encode_dict(CTMVTEncoder *enc, const MVTDict *dict)
{
    for (size_t k = 0; k < dict->nkeys; k++) {
        enc_write_tag(enc, MVT_LAYER_KEYS, 2);
        enc_write_string(enc, dict->keys[k]);
    }
    for (size_t j = 0; j < dict->nvals; j++) {
        encode_value_message(enc, &dict->vals[j]);
    }
}

/* ============================================================================
 * Feature Encoding
 * ============================================================================ */

static void encode_feature(CTMVTEncoder *enc, const CTFeature *feature, uint64_t id,
                           const MVTProp *props, int nprops, MVTDict *dict)
{
    /* Start feature message */
    enc_write_tag(enc, MVT_LAYER_FEATURES, 2);

    /* Skip length for now */
    size_t len_pos = enc->offset;
    if (!enc_reserve(enc, 5)) return;

    size_t content_start = enc->offset;

    /* Feature ID */
    enc_write_tag(enc, MVT_FEATURE_ID, 0);
    enc_write_varint(enc, id);

    /* Tags (key/value indices into the layer dictionary) */
    encode_tags(enc, dict, props, nprops);

    /* Feature type */
    enc_write_tag(enc, MVT_FEATURE_TYPE, 0);
    enc_write_varint(enc, (uint64_t)feature->type);

    /* Geometry */
    encode_geometry(enc, feature);

    /* Write actual length */
    size_t content_len = enc->offset - content_start;
    size_t saved_offset = enc->offset;

    enc->offset = len_pos;
    enc_write_varint(enc, content_len);

    size_t len_size = enc->offset - len_pos;
    if (len_size < 5) {
        memmove(enc->buffer + len_pos + len_size,
                enc->buffer + len_pos + 5, content_len);
        enc->offset = len_pos + len_size + content_len;
    } else {
        enc->offset = saved_offset;
    }
}

/* ============================================================================
 * Layer Encoding
 * ============================================================================ */

/* Finish a layer message started at len_pos (backpatch its length). */
static void finish_layer(CTMVTEncoder *enc, size_t len_pos, size_t content_start)
{
    size_t content_len = enc->offset - content_start;
    size_t saved_offset = enc->offset;
    enc->offset = len_pos;
    enc_write_varint(enc, content_len);
    size_t len_size = enc->offset - len_pos;
    if (len_size < 5) {
        memmove(enc->buffer + len_pos + len_size,
                enc->buffer + len_pos + 5, content_len);
        enc->offset = len_pos + len_size + content_len;
    } else {
        enc->offset = saved_offset;
    }
}

/* Write a layer header (version/name/extent); returns content_start via out. */
static size_t begin_layer(CTMVTEncoder *enc, const char *name, int extent,
                          size_t *content_start_out)
{
    enc_write_tag(enc, MVT_TILE_LAYERS, 2);
    size_t len_pos = enc->offset;
    if (!enc_reserve(enc, 5)) { *content_start_out = enc->offset; return len_pos; }
    *content_start_out = enc->offset;
    enc_write_tag(enc, MVT_LAYER_VERSION, 0);
    enc_write_varint(enc, 2);
    enc_write_tag(enc, MVT_LAYER_NAME, 2);
    enc_write_string(enc, name);
    enc_write_tag(enc, MVT_LAYER_EXTENT, 0);
    enc_write_varint(enc, extent);
    return len_pos;
}

static void encode_layer(CTMVTEncoder *enc, const char *name, int extent,
                         const CTFeature *features, size_t num_features)
{
    size_t content_start;
    size_t len_pos = begin_layer(enc, name, extent, &content_start);

    MVTDict dict;
    dict_init(&dict);

    for (size_t i = 0; i < num_features; i++) {
        MVTProp props[MVT_MAX_PROPS];
        int np = collect_geometry_props(&features[i], props);
        encode_feature(enc, &features[i], i + 1, props, np, &dict);
    }

    encode_dict(enc, &dict);
    dict_free(&dict);

    finish_layer(enc, len_pos, content_start);
}

/* Encode the "labels" point layer from labeled points (place labels).
 * Properties: name (string), place_type (string), rank (int), population (int),
 * min_zoom (int). Points are transformed to tile coords and buffer-clipped. */
static void encode_labels_layer(CTMVTEncoder *enc, const CTLabeledPoint **labels,
                                size_t count, CTTileCoord coord,
                                const CTMVTOptions *opts)
{
    size_t content_start;
    size_t len_pos;
    MVTDict dict;
    uint64_t id = 1;

    if (count == 0) return;

    len_pos = begin_layer(enc, ct_mvt_layer_name(CT_LAYER_LABELS), opts->extent,
                          &content_start);
    dict_init(&dict);

    for (size_t i = 0; i < count; i++) {
        const CTLabeledPoint *lp = labels[i];
        CTTilePoint pt;
        CTFeature f;
        MVTProp props[MVT_MAX_PROPS];
        int np = 0;

        if (!lp) continue;

        pt.x = (int32_t)(lp->coord.lon * 1e7);
        pt.y = (int32_t)(lp->coord.lat * 1e7);
        ct_batch_transform_points(coord, opts->extent, &pt, 1);
        if (pt.x < -opts->buffer || pt.x > opts->extent + opts->buffer ||
            pt.y < -opts->buffer || pt.y > opts->extent + opts->buffer) {
            continue;
        }

        memset(&f, 0, sizeof(f));
        f.type = CT_GEOM_POINT;
        f.points = &pt;
        f.num_points = 1;

        if (lp->name && lp->name[0]) {
            props[np].key = "name";
            props[np].val = (MVTVal){ MVT_V_STRING, lp->name, 0 };
            np++;
        }
        props[np].key = "place_type";
        props[np].val = (MVTVal){ MVT_V_STRING, place_class_str(lp->type), 0 };
        np++;
        props[np].key = "rank";
        props[np].val = (MVTVal){ MVT_V_INT, NULL, lp->priority };
        np++;
        if (lp->population > 0) {
            props[np].key = "population";
            props[np].val = (MVTVal){ MVT_V_INT, NULL, lp->population };
            np++;
        }
        if (lp->min_zoom > 0) {
            props[np].key = "min_zoom";
            props[np].val = (MVTVal){ MVT_V_INT, NULL, lp->min_zoom };
            np++;
        }

        encode_feature(enc, &f, id++, props, np, &dict);
    }

    encode_dict(enc, &dict);
    dict_free(&dict);
    finish_layer(enc, len_pos, content_start);
}

/* ============================================================================
 * Public Encoding Functions
 * ============================================================================ */

/* Group a tile's features by layer and encode one MVT layer per non-empty
 * group into the encoder. Shared by ct_encode_mvt (geometry-only, backward
 * compatible) and ct_generate_mvt (which also appends the labels layer).
 * Returns 1 on success, 0 on allocation failure. */
static int encode_tile_geometry_layers(CTMVTEncoder *enc, const CTTile *tile,
                                        const CTMVTOptions *opts)
{
    if (tile->num_features == 0) return 1;

    /* Single allocation for all layer features (O(1) mallocs). */
    if (tile->num_features > SIZE_MAX / sizeof(CTFeature)) return 0;
    CTFeature *all_features = malloc(tile->num_features * sizeof(CTFeature));
    if (!all_features) return 0;

    CTFeature *layer_features[CT_LAYER_COUNT];
    size_t layer_counts[CT_LAYER_COUNT];
    memset(layer_counts, 0, sizeof(layer_counts));

    for (size_t i = 0; i < tile->num_features; i++) {
        CTLayer layer = tile->features[i].layer;
        if (layer < CT_LAYER_COUNT) layer_counts[layer]++;
    }

    size_t offset = 0;
    for (int l = 0; l < CT_LAYER_COUNT; l++) {
        layer_features[l] = (layer_counts[l] > 0) ? &all_features[offset] : NULL;
        offset += layer_counts[l];
        layer_counts[l] = 0;
    }

    for (size_t i = 0; i < tile->num_features; i++) {
        CTLayer layer = tile->features[i].layer;
        if (layer < CT_LAYER_COUNT && layer_features[layer]) {
            layer_features[layer][layer_counts[layer]++] = tile->features[i];
        }
    }

    for (int l = 0; l < CT_LAYER_COUNT; l++) {
        if (layer_counts[l] > 0) {
            encode_layer(enc, ct_mvt_layer_name((CTLayer)l), opts->extent,
                         layer_features[l], layer_counts[l]);
        }
    }

    free(all_features);
    return 1;
}

size_t ct_encode_mvt(const CTTile *tile, const CTMVTOptions *opts,
                     uint8_t *buffer, size_t capacity)
{
    if (!tile || !buffer) return 0;

    CTMVTOptions default_opts;
    if (!opts) {
        ct_mvt_default_options(&default_opts);
        opts = &default_opts;
    }

    CTMVTEncoder enc;
    ct_mvt_encoder_init(&enc, buffer, capacity);

    if (!encode_tile_geometry_layers(&enc, tile, opts)) return 0;

    if (enc.error) return 0;
    return enc.offset;
}

size_t ct_generate_mvt(const CTPBFContext *ctx, CTTileCoord coord,
                       const CTMVTOptions *opts,
                       const CTLODConfig *lod,
                       uint8_t *buffer, size_t capacity)
{
    if (!ctx || !buffer) return 0;

    CTMVTOptions default_opts;
    if (!opts) {
        ct_mvt_default_options(&default_opts);
        opts = &default_opts;
    }

    /* Get tile bounds */
    CTBBox bbox = ct_tile_bounds(coord);

    /* Slightly expand for buffer */
    double buf_lat = (bbox.max_lat - bbox.min_lat) * opts->buffer / opts->extent;
    double buf_lon = (bbox.max_lon - bbox.min_lon) * opts->buffer / opts->extent;
    CTBBox expanded_bbox = {
        bbox.min_lat - buf_lat,
        bbox.min_lon - buf_lon,
        bbox.max_lat + buf_lat,
        bbox.max_lon + buf_lon
    };

    /* Get features for this tile */
    CTFeature *raw_features;
    size_t raw_count;
    CTStatus status = ct_pbf_get_bbox_features(ctx, expanded_bbox,
                                               &raw_features, &raw_count);
    if (status != CT_OK || raw_count == 0) {
        free(raw_features);
        return 0;
    }

    /* Convert coordinates from lat/lon to tile pixel coordinates */
    CTTile tile;
    ct_tile_init(&tile, coord);

    int zoom = coord.z;

    for (size_t i = 0; i < raw_count; i++) {
        CTFeature *f = &raw_features[i];

        /* LOD filtering: skip features not visible at this zoom level */
        if (lod && !ct_lod_is_visible(lod, f->layer, f->feature_type,
                                       zoom, f->area_sqm, f->length_m)) {
            free(f->points);
            f->points = NULL;
            continue;
        }

        /* Fast batch coordinate transformation */
        ct_batch_transform_points(coord, opts->extent, f->points, f->num_points);

        /* Clip geometry to tile bounds */
        CTTilePoint *clipped = NULL;
        int clipped_count = 0;

        if (f->type == CT_GEOM_POLYGON) {
            /* For multipolygons, skip clipping to preserve ring structure.
             * Clipping multipolygons correctly requires per-ring clipping
             * which can produce complex results (ring split, eliminated, etc.)
             * The MVT renderer handles out-of-bounds coordinates correctly. */
            if (f->num_rings > 1 && f->ring_ends) {
                /* Just do a quick bbox check - skip if entirely outside */
                int min_x = f->points[0].x, max_x = f->points[0].x;
                int min_y = f->points[0].y, max_y = f->points[0].y;
                for (int j = 1; j < f->num_points; j++) {
                    if (f->points[j].x < min_x) min_x = f->points[j].x;
                    if (f->points[j].x > max_x) max_x = f->points[j].x;
                    if (f->points[j].y < min_y) min_y = f->points[j].y;
                    if (f->points[j].y > max_y) max_y = f->points[j].y;
                }
                /* Check if entirely outside tile with buffer */
                if (max_x < -opts->buffer || min_x > opts->extent + opts->buffer ||
                    max_y < -opts->buffer || min_y > opts->extent + opts->buffer) {
                    free(f->points);
                    free(f->ring_ends);
                    f->points = NULL;
                    f->ring_ends = NULL;
                    continue;
                }
                /* Keep multipolygon as-is (no clipping) */
                clipped = f->points;
                clipped_count = f->num_points;
                f->points = NULL;  /* Transfer ownership */
            } else {
                ct_clip_polygon(f->points, f->num_points,
                               opts->extent, opts->buffer,
                               &clipped, &clipped_count);
                /* Skip degenerate polygons (need at least 3 points) */
                if (clipped_count < 3) {
                    free(clipped);
                    free(f->points);
                    f->points = NULL;
                    continue;
                }
                /* Single-ring polygon after clipping loses ring_ends */
                free(f->ring_ends);
                f->ring_ends = NULL;
                f->num_rings = 0;
            }
        } else if (f->type == CT_GEOM_LINESTRING) {
            int *segments = NULL;
            int seg_count = 0;
            ct_clip_linestring(f->points, f->num_points,
                              opts->extent, opts->buffer,
                              &clipped, &clipped_count,
                              &segments, &seg_count);
            free(segments);  /* We only use first segment for now */
            /* Skip degenerate linestrings */
            if (clipped_count < 2) {
                free(clipped);
                free(f->points);
                f->points = NULL;
                continue;
            }
        } else {
            /* Points: just check if within bounds */
            if (f->num_points > 0 &&
                f->points[0].x >= -opts->buffer &&
                f->points[0].x <= opts->extent + opts->buffer &&
                f->points[0].y >= -opts->buffer &&
                f->points[0].y <= opts->extent + opts->buffer) {
                /* Point is in bounds, keep original */
                clipped = f->points;
                clipped_count = f->num_points;
                f->points = NULL;  /* Transfer ownership */
            } else {
                free(f->points);
                f->points = NULL;
                continue;
            }
        }

        /* Replace original points with clipped version */
        if (f->points != clipped) {
            free(f->points);
        }
        f->points = clipped;
        f->num_points = clipped_count;

        /* Simplify geometry if enabled */
        if (opts->simplify && f->num_points > 2) {
            float tolerance = (float)opts->tolerance;
            if (f->type == CT_GEOM_POLYGON) {
                /* Use ring-aware simplification for multipolygons */
                if (f->num_rings > 1 && f->ring_ends) {
                    ct_simplify_multipolygon_inplace(f->points, &f->num_points,
                                                     f->ring_ends, f->num_rings,
                                                     tolerance);
                } else {
                    ct_simplify_poly_inplace(f->points, &f->num_points, tolerance);
                }
            } else if (f->type == CT_GEOM_LINESTRING) {
                ct_simplify_line_inplace(f->points, &f->num_points, tolerance);
            }
        }

        /* Skip if simplification made geometry degenerate */
        if ((f->type == CT_GEOM_POLYGON && f->num_points < 3) ||
            (f->type == CT_GEOM_LINESTRING && f->num_points < 2)) {
            free(f->points);
            f->points = NULL;
            continue;
        }

        ct_tile_add_feature(&tile, f);
    }

    /* Encode to MVT: geometry layers first, then the "labels" point layer
     * (place labels from the index) appended into the same tile message. */
    CTMVTEncoder enc;
    ct_mvt_encoder_init(&enc, buffer, capacity);

    if (!encode_tile_geometry_layers(&enc, &tile, opts)) {
        free(raw_features);
        ct_tile_free(&tile);
        return 0;
    }

    /* Place labels (cities/towns/...) as a dedicated point layer. Reuse the
     * tile bounds (unbuffered) so labels just outside still render via the
     * encoder's own buffer clip. Failure here is non-fatal: geometry still ships. */
    {
        const CTLabeledPoint **labels = NULL;
        size_t label_count = 0;
        if (ct_pbf_get_bbox_labels(ctx, bbox, zoom, &labels, &label_count) == CT_OK
            && label_count > 0) {
            encode_labels_layer(&enc, labels, label_count, coord, opts);
        }
        free((void *)labels);  /* array only; points owned by ctx */
    }

    size_t mvt_size = enc.error ? 0 : enc.offset;

    /* Cleanup - ct_tile_free handles freeing the points arrays
     * since ct_tile_add_feature took ownership via shallow copy */
    free(raw_features);
    ct_tile_free(&tile);

    return mvt_size;
}

size_t ct_mvt_encoder_finish(CTMVTEncoder *enc)
{
    if (enc->error) return 0;
    return enc->offset;
}
