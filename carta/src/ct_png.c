/*
 * ct_png.c - PNG encoding for raster tiles
 *
 * Encodes RGBA pixel buffers to PNG format using miniz for compression.
 */

#include "ct_png.h"
#include "ct_render.h"
#include "ct_tile.h"
#include "ct_pbf.h"
#include <stdlib.h>
#include <string.h>

/* Forward declaration */
CTStatus ct_deflate(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_capacity, size_t *actual_len,
                    int level);

/* PNG chunk types */
#define PNG_CHUNK_IHDR 0x49484452  /* IHDR */
#define PNG_CHUNK_IDAT 0x49444154  /* IDAT */
#define PNG_CHUNK_IEND 0x49454E44  /* IEND */

/* PNG signature */
static const uint8_t PNG_SIGNATURE[8] = {137, 80, 78, 71, 13, 10, 26, 10};

/* ============================================================================
 * CRC32 (for PNG chunks)
 * ============================================================================ */

static uint32_t crc32_table[256];
static int crc32_table_computed = 0;

static void make_crc32_table(void)
{
    for (int n = 0; n < 256; n++) {
        uint32_t c = (uint32_t)n;
        for (int k = 0; k < 8; k++) {
            if (c & 1)
                c = 0xedb88320 ^ (c >> 1);
            else
                c = c >> 1;
        }
        crc32_table[n] = c;
    }
    crc32_table_computed = 1;
}

static uint32_t crc32(const uint8_t *data, size_t len)
{
    if (!crc32_table_computed) make_crc32_table();

    uint32_t c = 0xffffffff;
    for (size_t i = 0; i < len; i++) {
        c = crc32_table[(c ^ data[i]) & 0xff] ^ (c >> 8);
    }
    return c ^ 0xffffffff;
}

/* ============================================================================
 * Default Options
 * ============================================================================ */

void ct_png_default_options(CTPNGOptions *opts)
{
    opts->tile_size = 256;
    opts->compression_level = 6;
}

/* ============================================================================
 * PNG Encoding
 * ============================================================================ */

static void write_be32(uint8_t *p, uint32_t v)
{
    p[0] = (v >> 24) & 0xff;
    p[1] = (v >> 16) & 0xff;
    p[2] = (v >> 8) & 0xff;
    p[3] = v & 0xff;
}

static size_t write_chunk(uint8_t *buf, uint32_t type,
                          const uint8_t *data, size_t len)
{
    write_be32(buf, (uint32_t)len);
    write_be32(buf + 4, type);

    if (data && len > 0) {
        memcpy(buf + 8, data, len);
    }

    /* CRC over type + data */
    uint32_t c = crc32(buf + 4, len + 4);
    write_be32(buf + 8 + len, c);

    return 12 + len;
}

size_t ct_png_max_size(int width, int height)
{
    /* Worst case: uncompressed PNG */
    /* Each row has 1 filter byte + width * 4 (RGBA) */
    size_t raw_size = (size_t)height * ((size_t)width * 4 + 1);

    /* DEFLATE worst case: input + 5 bytes per 16KB block + header */
    size_t deflate_overhead = (raw_size / 16384 + 1) * 5 + 6;

    /* PNG overhead: signature + IHDR + IDAT header/CRC + IEND */
    size_t png_overhead = 8 + 25 + 12 + 12;

    return raw_size + deflate_overhead + png_overhead;
}

size_t ct_encode_png(const uint8_t *pixels, int width, int height,
                     const CTPNGOptions *opts,
                     uint8_t *buffer, size_t capacity)
{
    CTPNGOptions default_opts;
    if (!opts) {
        ct_png_default_options(&default_opts);
        opts = &default_opts;
    }

    return ct_encode_png_ex(pixels, width, height, 0, opts->compression_level,
                            buffer, capacity);
}

size_t ct_encode_png_ex(const uint8_t *pixels, int width, int height,
                        int filter_type, int compression_level,
                        uint8_t *buffer, size_t capacity)
{
    if (!pixels || !buffer || width <= 0 || height <= 0) return 0;

    size_t max_size = ct_png_max_size(width, height);
    if (capacity < max_size) return 0;

    size_t offset = 0;

    /* PNG signature */
    memcpy(buffer, PNG_SIGNATURE, 8);
    offset += 8;

    /* IHDR chunk */
    uint8_t ihdr[13];
    write_be32(ihdr, (uint32_t)width);
    write_be32(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8;     /* bit depth */
    ihdr[9] = 6;     /* color type: RGBA */
    ihdr[10] = 0;    /* compression */
    ihdr[11] = 0;    /* filter */
    ihdr[12] = 0;    /* interlace */
    offset += write_chunk(buffer + offset, PNG_CHUNK_IHDR, ihdr, 13);

    /* Prepare filtered data (add filter byte per row) */
    size_t row_bytes = (size_t)width * 4;
    size_t filtered_size = (size_t)height * (row_bytes + 1);
    uint8_t *filtered = malloc(filtered_size);
    if (!filtered) return 0;

    for (int y = 0; y < height; y++) {
        size_t row_offset = (size_t)y * (row_bytes + 1);
        filtered[row_offset] = (uint8_t)filter_type;

        const uint8_t *src_row = pixels + y * row_bytes;

        if (filter_type == 0) {
            /* None filter */
            memcpy(filtered + row_offset + 1, src_row, row_bytes);
        } else if (filter_type == 1) {
            /* Sub filter */
            for (size_t x = 0; x < row_bytes; x++) {
                uint8_t left = (x >= 4) ? src_row[x - 4] : 0;
                filtered[row_offset + 1 + x] = src_row[x] - left;
            }
        } else if (filter_type == 2) {
            /* Up filter */
            const uint8_t *prev_row = (y > 0) ? (pixels + (y - 1) * row_bytes) : NULL;
            for (size_t x = 0; x < row_bytes; x++) {
                uint8_t up = prev_row ? prev_row[x] : 0;
                filtered[row_offset + 1 + x] = src_row[x] - up;
            }
        } else {
            /* Default to None */
            memcpy(filtered + row_offset + 1, src_row, row_bytes);
        }
    }

    /* Compress with DEFLATE */
    size_t deflate_capacity = filtered_size + (filtered_size / 16384 + 1) * 5 + 100;
    uint8_t *compressed = malloc(deflate_capacity);
    if (!compressed) {
        free(filtered);
        return 0;
    }

    /* Add zlib header manually (miniz compress2 adds it) */
    size_t compressed_size;
    CTStatus status = ct_deflate(filtered, filtered_size,
                                 compressed, deflate_capacity, &compressed_size,
                                 compression_level);
    free(filtered);

    if (status != CT_OK) {
        free(compressed);
        return 0;
    }

    /* IDAT chunk */
    offset += write_chunk(buffer + offset, PNG_CHUNK_IDAT,
                          compressed, compressed_size);
    free(compressed);

    /* IEND chunk */
    offset += write_chunk(buffer + offset, PNG_CHUNK_IEND, NULL, 0);

    return offset;
}

size_t ct_generate_png(const CTPBFContext *ctx, CTTileCoord coord,
                       const CTStyle *style, const CTPNGOptions *opts,
                       uint8_t *buffer, size_t capacity)
{
    CTPNGOptions default_opts;
    if (!opts) {
        ct_png_default_options(&default_opts);
        opts = &default_opts;
    }

    int tile_size = opts->tile_size;

    /* Create render context */
    CTRenderContext *render = ct_render_create(tile_size, tile_size);
    if (!render) return 0;

    if (style) {
        ct_render_set_style(render, style);
    }

    /* Render tile */
    ct_render_from_pbf(render, ctx, coord);

    /* Encode to PNG */
    size_t png_size = ct_encode_png(ct_render_pixels(render),
                                    tile_size, tile_size, opts,
                                    buffer, capacity);

    ct_render_free(render);
    return png_size;
}
