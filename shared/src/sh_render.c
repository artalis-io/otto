/*
 * sh_render.c - Shared Rendering Utilities
 *
 * SIMD-optimized span fill and alpha blending.
 * Extracted from carta/src/ct_render.c for reuse across modules.
 */

#include "sh_render.h"
#include <string.h>

/* ============================================================================
 * SIMD Support Detection
 * ============================================================================ */

#if defined(__AVX2__)
    #include <immintrin.h>
    #define SH_HAVE_AVX2 1
#elif defined(__SSE2__)
    #include <emmintrin.h>
    #define SH_HAVE_SSE2 1
#elif defined(__ARM_NEON) || defined(__aarch64__)
    #include <arm_neon.h>
    #define SH_HAVE_NEON 1
#endif

/* ============================================================================
 * Pixel Operations (Single)
 * ============================================================================ */

uint32_t sh_get_pixel(const uint8_t *pixels, int buf_width, int buf_height,
                      int x, int y)
{
    if (x < 0 || x >= buf_width || y < 0 || y >= buf_height) {
        return 0;
    }
    int offset = (y * buf_width + x) * 4;
    return SH_RGBA(pixels[offset], pixels[offset + 1],
                   pixels[offset + 2], pixels[offset + 3]);
}

void sh_set_pixel(uint8_t *pixels, int buf_width, int buf_height,
                  int x, int y, uint32_t color)
{
    if (x < 0 || x >= buf_width || y < 0 || y >= buf_height) {
        return;
    }
    int offset = (y * buf_width + x) * 4;
    pixels[offset + 0] = SH_COLOR_R(color);
    pixels[offset + 1] = SH_COLOR_G(color);
    pixels[offset + 2] = SH_COLOR_B(color);
    pixels[offset + 3] = SH_COLOR_A(color);
}

void sh_blend_pixel_unchecked(uint8_t *pixel, uint32_t color)
{
    uint8_t sr = SH_COLOR_R(color);
    uint8_t sg = SH_COLOR_G(color);
    uint8_t sb = SH_COLOR_B(color);
    uint8_t sa = SH_COLOR_A(color);

    /* Fast path: fully opaque */
    if (sa == 255) {
        pixel[0] = sr;
        pixel[1] = sg;
        pixel[2] = sb;
        pixel[3] = 255;
        return;
    }

    /* Fast path: fully transparent */
    if (sa == 0) return;

    /* Alpha blending */
    uint8_t dr = pixel[0];
    uint8_t dg = pixel[1];
    uint8_t db = pixel[2];
    uint8_t da = pixel[3];

    /* Porter-Duff source-over: out_a = src_a + dst_a * (1 - src_a) */
    uint16_t out_a = sa + (da * (255 - sa)) / 255;
    if (out_a == 0) return;

    pixel[0] = (sr * sa + dr * da * (255 - sa) / 255) / out_a;
    pixel[1] = (sg * sa + dg * da * (255 - sa) / 255) / out_a;
    pixel[2] = (sb * sa + db * da * (255 - sa) / 255) / out_a;
    pixel[3] = out_a;
}

void sh_blend_pixel(uint8_t *pixels, int buf_width, int buf_height,
                    int x, int y, uint32_t color)
{
    if (x < 0 || x >= buf_width || y < 0 || y >= buf_height) {
        return;
    }
    int offset = (y * buf_width + x) * 4;
    sh_blend_pixel_unchecked(pixels + offset, color);
}

/* ============================================================================
 * Span Fill (SIMD-Optimized)
 * ============================================================================ */

void sh_fill_span_unchecked(uint8_t *row, int x_start, int x_end, uint32_t color)
{
    uint8_t sr = SH_COLOR_R(color);
    uint8_t sg = SH_COLOR_G(color);
    uint8_t sb = SH_COLOR_B(color);
    uint8_t sa = SH_COLOR_A(color);

    /* Fast path: opaque color - use SIMD when available */
    if (sa == 255) {
        uint32_t rgba = ((uint32_t)255 << 24) | ((uint32_t)sb << 16) |
                        ((uint32_t)sg << 8) | (uint32_t)sr;
        uint32_t *row32 = (uint32_t *)row;
        int x = x_start;

#if defined(SH_HAVE_AVX2)
        /* AVX2: write 8 pixels (32 bytes) at a time */
        int count = x_end - x_start + 1;
        if (count >= 8) {
            __m256i rgba_vec = _mm256_set1_epi32((int)rgba);
            for (; x + 7 <= x_end; x += 8) {
                _mm256_storeu_si256((__m256i *)&row32[x], rgba_vec);
            }
        }
#elif defined(SH_HAVE_SSE2)
        /* SSE2: write 4 pixels (16 bytes) at a time */
        int count = x_end - x_start + 1;
        if (count >= 4) {
            __m128i rgba_vec = _mm_set1_epi32((int)rgba);
            for (; x + 3 <= x_end; x += 4) {
                _mm_storeu_si128((__m128i *)&row32[x], rgba_vec);
            }
        }
#elif defined(SH_HAVE_NEON)
        /* NEON: write 4 pixels (16 bytes) at a time */
        int count = x_end - x_start + 1;
        if (count >= 4) {
            uint32x4_t rgba_vec = vdupq_n_u32(rgba);
            for (; x + 3 <= x_end; x += 4) {
                vst1q_u32(&row32[x], rgba_vec);
            }
        }
#endif
        /* Scalar remainder */
        for (; x <= x_end; x++) {
            row32[x] = rgba;
        }
        return;
    }

    /* Transparent - nothing to do */
    if (sa == 0) return;

    /*
     * Alpha blending using fast integer approximation.
     * Formula: out = (src * sa + dst * inv_sa + 128) >> 8
     * This approximates division by 255 with good accuracy.
     */
    uint16_t inv_sa = 255 - sa;

    /* Pre-multiply source by alpha */
    uint16_t sr_sa = sr * sa;
    uint16_t sg_sa = sg * sa;
    uint16_t sb_sa = sb * sa;

    int x = x_start;

#if defined(SH_HAVE_SSE2)
    /*
     * SIMD alpha blending: process 4 pixels at a time.
     * Each pixel is RGBA (4 bytes), so 4 pixels = 16 bytes = 128 bits.
     */
    int count = x_end - x_start + 1;
    if (count >= 4) {
        /* Broadcast source alpha and inv_sa to all 8 lanes (16-bit) */
        __m128i inv_alpha = _mm_set1_epi16((short)inv_sa);
        __m128i const_128 = _mm_set1_epi16(128);
        __m128i const_255 = _mm_set1_epi16(255);
        __m128i zero = _mm_setzero_si128();

        /* src pattern for 2 pixels: sr_sa, sg_sa, sb_sa, sa (repeated) */
        __m128i src_pattern = _mm_set_epi16((short)sa, (short)sb_sa,
                                             (short)sg_sa, (short)sr_sa,
                                             (short)sa, (short)sb_sa,
                                             (short)sg_sa, (short)sr_sa);

        for (; x + 3 <= x_end; x += 4) {
            /* Load 4 destination pixels (16 bytes) */
            __m128i dst = _mm_loadu_si128((__m128i *)&row[x * 4]);

            /* Unpack to 16-bit: dst_lo = pixels 0-1, dst_hi = pixels 2-3 */
            __m128i dst_lo = _mm_unpacklo_epi8(dst, zero);
            __m128i dst_hi = _mm_unpackhi_epi8(dst, zero);

            /* Multiply destination by inv_alpha */
            __m128i d_inv_lo = _mm_mullo_epi16(dst_lo, inv_alpha);
            __m128i d_inv_hi = _mm_mullo_epi16(dst_hi, inv_alpha);

            /* Add src + dst*inv + 128 */
            __m128i sum_lo = _mm_add_epi16(d_inv_lo, src_pattern);
            __m128i sum_hi = _mm_add_epi16(d_inv_hi, src_pattern);
            sum_lo = _mm_add_epi16(sum_lo, const_128);
            sum_hi = _mm_add_epi16(sum_hi, const_128);

            /* Shift right by 8 */
            sum_lo = _mm_srli_epi16(sum_lo, 8);
            sum_hi = _mm_srli_epi16(sum_hi, 8);

            /* Clamp to 255 */
            sum_lo = _mm_min_epi16(sum_lo, const_255);
            sum_hi = _mm_min_epi16(sum_hi, const_255);

            /* Pack back to 8-bit */
            __m128i result = _mm_packus_epi16(sum_lo, sum_hi);

            /* Store 4 pixels */
            _mm_storeu_si128((__m128i *)&row[x * 4], result);
        }
    }
#elif defined(SH_HAVE_NEON)
    /*
     * NEON alpha blending: process 4 pixels at a time.
     */
    int count = x_end - x_start + 1;
    if (count >= 4) {
        /* Create source pattern for 4 pixels (RGBA repeated) */
        uint16_t src_vals[8] = {sr_sa, sg_sa, sb_sa, sa, sr_sa, sg_sa, sb_sa, sa};
        uint16x8_t src_pattern = vld1q_u16(src_vals);
        uint16x8_t inv_alpha = vdupq_n_u16(inv_sa);
        uint16x8_t const_128 = vdupq_n_u16(128);

        for (; x + 3 <= x_end; x += 4) {
            /* Load 4 destination pixels (16 bytes) */
            uint8x16_t dst = vld1q_u8(&row[x * 4]);

            /* Unpack to 16-bit: lo = pixels 0-1, hi = pixels 2-3 */
            uint16x8_t dst_lo = vmovl_u8(vget_low_u8(dst));
            uint16x8_t dst_hi = vmovl_u8(vget_high_u8(dst));

            /* Multiply destination by inv_alpha */
            uint16x8_t d_inv_lo = vmulq_u16(dst_lo, inv_alpha);
            uint16x8_t d_inv_hi = vmulq_u16(dst_hi, inv_alpha);

            /* Add src + dst*inv + 128 */
            uint16x8_t sum_lo = vaddq_u16(d_inv_lo, src_pattern);
            uint16x8_t sum_hi = vaddq_u16(d_inv_hi, src_pattern);
            sum_lo = vaddq_u16(sum_lo, const_128);
            sum_hi = vaddq_u16(sum_hi, const_128);

            /* Shift right by 8 */
            sum_lo = vshrq_n_u16(sum_lo, 8);
            sum_hi = vshrq_n_u16(sum_hi, 8);

            /* Pack back to 8-bit (saturating narrow) */
            uint8x8_t result_lo = vqmovn_u16(sum_lo);
            uint8x8_t result_hi = vqmovn_u16(sum_hi);
            uint8x16_t result = vcombine_u8(result_lo, result_hi);

            /* Store 4 pixels */
            vst1q_u8(&row[x * 4], result);
        }
    }
#endif

    /* Scalar remainder */
    for (; x <= x_end; x++) {
        int offset = x * 4;
        uint8_t dr = row[offset + 0];
        uint8_t dg = row[offset + 1];
        uint8_t db = row[offset + 2];
        uint8_t da = row[offset + 3];

        /* Fast approximate blend: (src*sa + dst*inv_sa + 128) >> 8 */
        row[offset + 0] = (uint8_t)((sr_sa + dr * inv_sa + 128) >> 8);
        row[offset + 1] = (uint8_t)((sg_sa + dg * inv_sa + 128) >> 8);
        row[offset + 2] = (uint8_t)((sb_sa + db * inv_sa + 128) >> 8);

        /* Output alpha: sa + da * (1 - sa) */
        uint16_t out_a = sa + ((da * inv_sa + 128) >> 8);
        row[offset + 3] = (uint8_t)(out_a > 255 ? 255 : out_a);
    }
}

void sh_fill_span(uint8_t *pixels, int buf_width, int buf_height,
                  int y, int x_start, int x_end, uint32_t color)
{
    /* Bounds check y once */
    if (y < 0 || y >= buf_height) return;

    /* Clip x range to buffer */
    if (x_start < 0) x_start = 0;
    if (x_end >= buf_width) x_end = buf_width - 1;
    if (x_start > x_end) return;

    uint8_t *row = pixels + y * buf_width * 4;
    sh_fill_span_unchecked(row, x_start, x_end, color);
}

/* ============================================================================
 * Buffer Operations
 * ============================================================================ */

void sh_clear_buffer(uint8_t *pixels, int buf_width, int buf_height,
                     uint32_t color)
{
    uint8_t r = SH_COLOR_R(color);
    uint8_t g = SH_COLOR_G(color);
    uint8_t b = SH_COLOR_B(color);
    uint8_t a = SH_COLOR_A(color);

    int num_pixels = buf_width * buf_height;

    /* Fast path: if all components are the same, use memset */
    if (r == g && g == b && b == a) {
        memset(pixels, r, (size_t)num_pixels * 4);
        return;
    }

    /* Use SIMD for non-uniform colors */
    uint32_t rgba = ((uint32_t)a << 24) | ((uint32_t)b << 16) |
                    ((uint32_t)g << 8) | (uint32_t)r;
    uint32_t *pixels32 = (uint32_t *)pixels;
    int i = 0;

#if defined(SH_HAVE_AVX2)
    /* AVX2: clear 8 pixels at a time */
    __m256i rgba_vec = _mm256_set1_epi32((int)rgba);
    for (; i + 7 < num_pixels; i += 8) {
        _mm256_storeu_si256((__m256i *)&pixels32[i], rgba_vec);
    }
#elif defined(SH_HAVE_SSE2)
    /* SSE2: clear 4 pixels at a time */
    __m128i rgba_vec = _mm_set1_epi32((int)rgba);
    for (; i + 3 < num_pixels; i += 4) {
        _mm_storeu_si128((__m128i *)&pixels32[i], rgba_vec);
    }
#elif defined(SH_HAVE_NEON)
    /* NEON: clear 4 pixels at a time */
    uint32x4_t rgba_vec = vdupq_n_u32(rgba);
    for (; i + 3 < num_pixels; i += 4) {
        vst1q_u32(&pixels32[i], rgba_vec);
    }
#endif

    /* Scalar remainder */
    for (; i < num_pixels; i++) {
        pixels32[i] = rgba;
    }
}

void sh_clear_rect(uint8_t *pixels, int buf_width, int buf_height,
                   int x, int y, int w, int h, uint32_t color)
{
    /* Clip to buffer bounds */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > buf_width) w = buf_width - x;
    if (y + h > buf_height) h = buf_height - y;
    if (w <= 0 || h <= 0) return;

    /* Fill each row as a span */
    for (int row = y; row < y + h; row++) {
        sh_fill_span(pixels, buf_width, buf_height, row, x, x + w - 1, color);
    }
}
