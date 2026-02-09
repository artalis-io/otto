/*
 * test_font.c - Unit tests for MSDF font library
 */

#include "sh_font.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) static int test_##name(void)
#define RUN_TEST(name) do { \
    tests_run++; \
    printf("  %-50s ", #name); \
    fflush(stdout); \
    if (test_##name()) { \
        tests_passed++; \
        printf("[PASS]\n"); \
    } else { \
        printf("[FAIL]\n"); \
    } \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("\n    ASSERT failed: %s (line %d)\n", #cond, __LINE__); \
        return 0; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("\n    ASSERT_EQ failed: %d != %d (line %d)\n", (int)(a), (int)(b), __LINE__); \
        return 0; \
    } \
} while(0)

#define ASSERT_FLOAT_EQ(a, b, eps) do { \
    float _a = (a), _b = (b); \
    if (fabsf(_a - _b) > (eps)) { \
        printf("\n    ASSERT_FLOAT_EQ failed: %.6f != %.6f (line %d)\n", _a, _b, __LINE__); \
        return 0; \
    } \
} while(0)

/* ============================================================================
 * Font Access Tests
 * ============================================================================ */

TEST(get_default_font)
{
    const SHFont *font = sh_font_get_default();
    ASSERT(font != NULL);
    ASSERT(font->glyph_count > 0);
    ASSERT(font->atlas_width > 0);
    ASSERT(font->atlas_height > 0);
    ASSERT(font->atlas_data != NULL);
    return 1;
}

TEST(font_has_ascii)
{
    const SHFont *font = sh_font_get_default();
    ASSERT(font != NULL);

    /* Check common ASCII characters are present */
    ASSERT(font->ascii['A'] != NULL);
    ASSERT(font->ascii['Z'] != NULL);
    ASSERT(font->ascii['a'] != NULL);
    ASSERT(font->ascii['z'] != NULL);
    ASSERT(font->ascii['0'] != NULL);
    ASSERT(font->ascii['9'] != NULL);
    ASSERT(font->ascii[' '] != NULL);
    ASSERT(font->ascii['.'] != NULL);

    return 1;
}

/* ============================================================================
 * Glyph Lookup Tests
 * ============================================================================ */

TEST(get_glyph_ascii)
{
    const SHFont *font = sh_font_get_default();
    ASSERT(font != NULL);

    const SHGlyph *g = sh_font_get_glyph(font, 'A');
    ASSERT(g != NULL);
    ASSERT_EQ(g->unicode, 'A');
    ASSERT(g->advance > 0.0f);

    return 1;
}

TEST(get_glyph_missing)
{
    const SHFont *font = sh_font_get_default();
    ASSERT(font != NULL);

    /* Very high codepoint unlikely to exist */
    const SHGlyph *g = sh_font_get_glyph(font, 0x1FFFF);
    ASSERT(g == NULL);

    return 1;
}

TEST(get_glyph_null_font)
{
    const SHGlyph *g = sh_font_get_glyph(NULL, 'A');
    ASSERT(g == NULL);
    return 1;
}

TEST(get_advance)
{
    const SHFont *font = sh_font_get_default();
    ASSERT(font != NULL);

    float adv = sh_font_get_advance(font, 'M');
    ASSERT(adv > 0.0f);
    ASSERT(adv < 2.0f);  /* Reasonable range */

    /* Missing glyph should return default */
    float adv_missing = sh_font_get_advance(font, 0x1FFFF);
    ASSERT(adv_missing > 0.0f);

    return 1;
}

/* ============================================================================
 * UTF-8 Tests
 * ============================================================================ */

TEST(utf8_decode_ascii)
{
    uint32_t cp;
    int len;

    len = sh_utf8_decode("A", &cp);
    ASSERT_EQ(len, 1);
    ASSERT_EQ(cp, 'A');

    len = sh_utf8_decode("Z", &cp);
    ASSERT_EQ(len, 1);
    ASSERT_EQ(cp, 'Z');

    return 1;
}

TEST(utf8_decode_2byte)
{
    uint32_t cp;
    int len;

    /* é = U+00E9 = 0xC3 0xA9 */
    len = sh_utf8_decode("\xC3\xA9", &cp);
    ASSERT_EQ(len, 2);
    ASSERT_EQ(cp, 0x00E9);

    return 1;
}

TEST(utf8_decode_3byte)
{
    uint32_t cp;
    int len;

    /* € = U+20AC = 0xE2 0x82 0xAC */
    len = sh_utf8_decode("\xE2\x82\xAC", &cp);
    ASSERT_EQ(len, 3);
    ASSERT_EQ(cp, 0x20AC);

    return 1;
}

TEST(utf8_decode_4byte)
{
    uint32_t cp;
    int len;

    /* 😀 = U+1F600 = 0xF0 0x9F 0x98 0x80 */
    len = sh_utf8_decode("\xF0\x9F\x98\x80", &cp);
    ASSERT_EQ(len, 4);
    ASSERT_EQ(cp, 0x1F600);

    return 1;
}

TEST(utf8_decode_invalid)
{
    uint32_t cp;
    int len;

    /* Invalid continuation byte at start */
    len = sh_utf8_decode("\x80", &cp);
    ASSERT_EQ(len, 1);
    ASSERT_EQ(cp, 0xFFFD);  /* Replacement char */

    /* Truncated sequence */
    len = sh_utf8_decode("\xC3", &cp);
    ASSERT_EQ(len, 1);
    ASSERT_EQ(cp, 0xFFFD);

    return 1;
}

TEST(utf8_decode_null)
{
    uint32_t cp;

    int len = sh_utf8_decode(NULL, &cp);
    ASSERT_EQ(len, 1);
    ASSERT_EQ(cp, 0xFFFD);

    /* Empty string */
    len = sh_utf8_decode("", &cp);
    ASSERT_EQ(len, 0);
    ASSERT_EQ(cp, 0);

    return 1;
}

TEST(utf8_strlen)
{
    ASSERT_EQ(sh_utf8_strlen("Hello"), 5);
    ASSERT_EQ(sh_utf8_strlen(""), 0);
    ASSERT_EQ(sh_utf8_strlen(NULL), 0);

    /* "Héllo" - 5 characters, 6 bytes */
    ASSERT_EQ(sh_utf8_strlen("H\xC3\xA9llo"), 5);

    /* "€100" - 4 characters, 6 bytes */
    ASSERT_EQ(sh_utf8_strlen("\xE2\x82\xAC""100"), 4);

    return 1;
}

/* ============================================================================
 * Text Measurement Tests
 * ============================================================================ */

TEST(text_width_basic)
{
    const SHFont *font = sh_font_get_default();
    ASSERT(font != NULL);

    float w = sh_font_text_width(font, "Hello", 16.0f);
    ASSERT(w > 0.0f);
    ASSERT(w < 200.0f);  /* Reasonable for 5 chars at 16px */

    return 1;
}

TEST(text_width_empty)
{
    const SHFont *font = sh_font_get_default();
    ASSERT(font != NULL);

    ASSERT_FLOAT_EQ(sh_font_text_width(font, "", 16.0f), 0.0f, 0.001f);
    ASSERT_FLOAT_EQ(sh_font_text_width(font, NULL, 16.0f), 0.0f, 0.001f);
    ASSERT_FLOAT_EQ(sh_font_text_width(NULL, "Hello", 16.0f), 0.0f, 0.001f);

    return 1;
}

TEST(text_width_scales_with_size)
{
    const SHFont *font = sh_font_get_default();
    ASSERT(font != NULL);

    float w16 = sh_font_text_width(font, "Test", 16.0f);
    float w32 = sh_font_text_width(font, "Test", 32.0f);

    /* Width should scale linearly with font size */
    ASSERT_FLOAT_EQ(w32 / w16, 2.0f, 0.01f);

    return 1;
}

TEST(text_width_n)
{
    const SHFont *font = sh_font_get_default();
    ASSERT(font != NULL);

    float w_full = sh_font_text_width(font, "Hello World", 16.0f);
    float w_5 = sh_font_text_width_n(font, "Hello World", 5, 16.0f);
    float w_hello = sh_font_text_width(font, "Hello", 16.0f);

    ASSERT(w_5 < w_full);
    ASSERT_FLOAT_EQ(w_5, w_hello, 0.01f);

    return 1;
}

TEST(line_height)
{
    const SHFont *font = sh_font_get_default();
    ASSERT(font != NULL);

    float lh = sh_font_line_height(font, 16.0f);
    ASSERT(lh > 16.0f);  /* Line height > font size */
    ASSERT(lh < 32.0f);

    return 1;
}

/* ============================================================================
 * MSDF Sampling Tests
 * ============================================================================ */

TEST(msdf_sample_bounds)
{
    const SHFont *font = sh_font_get_default();
    ASSERT(font != NULL);

    /* Sample at valid location */
    uint8_t d = sh_font_sample_msdf(font, font->atlas_width / 2, font->atlas_height / 2);
    (void)d;  /* Value depends on atlas content */

    /* Out of bounds should return 0 */
    ASSERT_EQ(sh_font_sample_msdf(font, -1, 0), 0);
    ASSERT_EQ(sh_font_sample_msdf(font, 0, -1), 0);
    ASSERT_EQ(sh_font_sample_msdf(font, font->atlas_width, 0), 0);
    ASSERT_EQ(sh_font_sample_msdf(font, 0, font->atlas_height), 0);

    /* NULL font */
    ASSERT_EQ(sh_font_sample_msdf(NULL, 0, 0), 0);

    return 1;
}

TEST(msdf_coverage)
{
    const SHFont *font = sh_font_get_default();
    ASSERT(font != NULL);

    const SHGlyph *g = sh_font_get_glyph(font, 'O');
    ASSERT(g != NULL);

    /* Center of 'O' should have low coverage (it's a hole) */
    float center = sh_font_msdf_coverage(font, g, 0.5f, 0.5f, 32.0f);
    /* 'O' has a hole, so center might be low */
    ASSERT(center >= 0.0f && center <= 1.0f);

    /* NULL checks */
    ASSERT_FLOAT_EQ(sh_font_msdf_coverage(NULL, g, 0.5f, 0.5f, 32.0f), 0.0f, 0.001f);
    ASSERT_FLOAT_EQ(sh_font_msdf_coverage(font, NULL, 0.5f, 0.5f, 32.0f), 0.0f, 0.001f);

    return 1;
}

TEST(msdf_sample_bilinear)
{
    const SHFont *font = sh_font_get_default();
    if (!font || !font->atlas_data) {
        return 1;  /* Skip if no font */
    }

    /* Sample at valid coordinates */
    float dist = sh_font_sample_msdf_bilinear(font, 50.5f, 50.5f);
    ASSERT(dist >= 0.0f && dist <= 1.0f);

    /* Sample at edges should be clamped */
    dist = sh_font_sample_msdf_bilinear(font, -10.0f, -10.0f);
    ASSERT(dist >= 0.0f && dist <= 1.0f);

    dist = sh_font_sample_msdf_bilinear(font, (float)font->atlas_width + 100.0f, 50.0f);
    ASSERT(dist >= 0.0f && dist <= 1.0f);

    /* NULL font returns 0 */
    ASSERT_FLOAT_EQ(sh_font_sample_msdf_bilinear(NULL, 50.0f, 50.0f), 0.0f, 0.001f);

    return 1;
}

TEST(msdf_coverage_bilinear)
{
    const SHFont *font = sh_font_get_default();
    if (!font) {
        return 1;  /* Skip if no font */
    }

    const SHGlyph *glyph = sh_font_get_glyph(font, 'A');
    if (!glyph) {
        return 1;  /* Skip if no glyph */
    }

    /* Coverage should be in [0, 1] */
    float coverage = sh_font_msdf_coverage_bilinear(font, glyph, 0.5f, 0.5f, 32.0f);
    ASSERT(coverage >= 0.0f && coverage <= 1.0f);

    /* NULL handling */
    ASSERT_FLOAT_EQ(sh_font_msdf_coverage_bilinear(NULL, glyph, 0.5f, 0.5f, 32.0f), 0.0f, 0.001f);
    ASSERT_FLOAT_EQ(sh_font_msdf_coverage_bilinear(font, NULL, 0.5f, 0.5f, 32.0f), 0.0f, 0.001f);
    ASSERT_FLOAT_EQ(sh_font_msdf_coverage_bilinear(font, glyph, 0.5f, 0.5f, 0.0f), 0.0f, 0.001f);
    ASSERT_FLOAT_EQ(sh_font_msdf_coverage_bilinear(font, glyph, 0.5f, 0.5f, -1.0f), 0.0f, 0.001f);

    return 1;
}

TEST(msdf_coverage_threshold)
{
    const SHFont *font = sh_font_get_default();
    if (!font) {
        return 1;  /* Skip if no font */
    }

    const SHGlyph *glyph = sh_font_get_glyph(font, 'A');
    if (!glyph) {
        return 1;  /* Skip if no glyph */
    }

    /* Coverage should be in [0, 1] */
    float coverage = sh_font_msdf_coverage_threshold(font, glyph, 0.5f, 0.5f, 32.0f, 0.5f);
    ASSERT(coverage >= 0.0f && coverage <= 1.0f);

    /* Lower threshold should give more coverage (expanded glyph) */
    float coverage_normal = sh_font_msdf_coverage_threshold(font, glyph, 0.5f, 0.5f, 32.0f, 0.5f);
    float coverage_halo = sh_font_msdf_coverage_threshold(font, glyph, 0.5f, 0.5f, 32.0f, 0.3f);
    /* Halo coverage should be >= normal coverage */
    ASSERT(coverage_halo >= coverage_normal - 0.001f);

    /* NULL handling */
    ASSERT_FLOAT_EQ(sh_font_msdf_coverage_threshold(NULL, glyph, 0.5f, 0.5f, 32.0f, 0.5f), 0.0f, 0.001f);
    ASSERT_FLOAT_EQ(sh_font_msdf_coverage_threshold(font, NULL, 0.5f, 0.5f, 32.0f, 0.5f), 0.0f, 0.001f);

    return 1;
}

/* ============================================================================
 * Glyph Rendering Tests
 * ============================================================================ */

#include "sh_render.h"
#include <stdlib.h>

TEST(render_glyph_basic)
{
    const SHFont *font = sh_font_get_default();
    if (!font) return 1;

    const SHGlyph *glyph = sh_font_get_glyph(font, 'A');
    if (!glyph) return 1;

    /* Create a small test buffer */
    int w = 64, h = 64;
    uint8_t *pixels = calloc(w * h, 4);
    ASSERT(pixels != NULL);

    /* Render glyph at center */
    sh_font_render_glyph(pixels, w, h, font, glyph, 10, 10, 24.0f,
                         255, 255, 255, 255, 0.5f);

    /* Check that some pixels were drawn (non-zero alpha somewhere) */
    int has_pixels = 0;
    for (int i = 0; i < w * h * 4; i += 4) {
        if (pixels[i + 3] > 0) {
            has_pixels = 1;
            break;
        }
    }
    ASSERT(has_pixels);

    free(pixels);
    return 1;
}

TEST(render_glyph_null_safety)
{
    const SHFont *font = sh_font_get_default();
    if (!font) return 1;

    const SHGlyph *glyph = sh_font_get_glyph(font, 'A');
    if (!glyph) return 1;

    uint8_t pixels[64 * 4];
    memset(pixels, 0, sizeof(pixels));

    /* NULL pixels - should not crash */
    sh_font_render_glyph(NULL, 64, 1, font, glyph, 0, 0, 16.0f, 255, 255, 255, 255, 0.5f);

    /* NULL font - should not crash */
    sh_font_render_glyph(pixels, 64, 1, NULL, glyph, 0, 0, 16.0f, 255, 255, 255, 255, 0.5f);

    /* NULL glyph - should not crash */
    sh_font_render_glyph(pixels, 64, 1, font, NULL, 0, 0, 16.0f, 255, 255, 255, 255, 0.5f);

    /* Zero/negative font size - should not crash */
    sh_font_render_glyph(pixels, 64, 1, font, glyph, 0, 0, 0.0f, 255, 255, 255, 255, 0.5f);
    sh_font_render_glyph(pixels, 64, 1, font, glyph, 0, 0, -10.0f, 255, 255, 255, 255, 0.5f);

    return 1;
}

TEST(render_glyph_clipping)
{
    const SHFont *font = sh_font_get_default();
    if (!font) return 1;

    const SHGlyph *glyph = sh_font_get_glyph(font, 'A');
    if (!glyph) return 1;

    int w = 32, h = 32;
    uint8_t *pixels = calloc(w * h, 4);
    ASSERT(pixels != NULL);

    /* Render fully outside - should not crash and leave buffer unchanged */
    sh_font_render_glyph(pixels, w, h, font, glyph, -100, -100, 16.0f,
                         255, 255, 255, 255, 0.5f);

    /* Check buffer is still zero */
    int all_zero = 1;
    for (int i = 0; i < w * h * 4; i++) {
        if (pixels[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    ASSERT(all_zero);

    /* Render partially clipped */
    sh_font_render_glyph(pixels, w, h, font, glyph, -5, 5, 16.0f,
                         255, 0, 0, 255, 0.5f);

    /* Some pixels should be drawn now */
    int has_pixels = 0;
    for (int i = 0; i < w * h * 4; i += 4) {
        if (pixels[i + 3] > 0) {
            has_pixels = 1;
            break;
        }
    }
    ASSERT(has_pixels);

    free(pixels);
    return 1;
}

TEST(render_glyph_alpha)
{
    const SHFont *font = sh_font_get_default();
    if (!font) return 1;

    const SHGlyph *glyph = sh_font_get_glyph(font, 'O');  /* Solid glyph */
    if (!glyph) return 1;

    int w = 64, h = 64;
    uint8_t *pixels1 = calloc(w * h, 4);
    uint8_t *pixels2 = calloc(w * h, 4);
    ASSERT(pixels1 && pixels2);

    /* Render with full alpha */
    sh_font_render_glyph(pixels1, w, h, font, glyph, 10, 10, 24.0f,
                         255, 255, 255, 255, 0.5f);

    /* Render with half alpha */
    sh_font_render_glyph(pixels2, w, h, font, glyph, 10, 10, 24.0f,
                         255, 255, 255, 128, 0.5f);

    /* Half-alpha version should have lower max alpha */
    uint8_t max1 = 0, max2 = 0;
    for (int i = 0; i < w * h * 4; i += 4) {
        if (pixels1[i + 3] > max1) max1 = pixels1[i + 3];
        if (pixels2[i + 3] > max2) max2 = pixels2[i + 3];
    }
    ASSERT(max1 > max2);  /* Full alpha should have higher max */

    free(pixels1);
    free(pixels2);
    return 1;
}

TEST(render_text_basic)
{
    const SHFont *font = sh_font_get_default();
    if (!font) return 1;

    int w = 256, h = 64;
    uint8_t *pixels = calloc(w * h, 4);
    ASSERT(pixels != NULL);

    sh_font_render_text(pixels, w, h, font, "Hello", -1,
                        10.0f, 10.0f, 24.0f, 255, 255, 255, 255);

    /* Check that pixels were drawn */
    int has_pixels = 0;
    for (int i = 0; i < w * h * 4; i += 4) {
        if (pixels[i + 3] > 0) {
            has_pixels = 1;
            break;
        }
    }
    ASSERT(has_pixels);

    free(pixels);
    return 1;
}

TEST(render_text_null_safety)
{
    const SHFont *font = sh_font_get_default();
    if (!font) return 1;

    uint8_t pixels[256 * 4];
    memset(pixels, 0, sizeof(pixels));

    /* NULL pixels */
    sh_font_render_text(NULL, 256, 1, font, "Hi", -1, 0, 0, 16.0f, 255, 255, 255, 255);

    /* NULL font */
    sh_font_render_text(pixels, 256, 1, NULL, "Hi", -1, 0, 0, 16.0f, 255, 255, 255, 255);

    /* NULL text */
    sh_font_render_text(pixels, 256, 1, font, NULL, -1, 0, 0, 16.0f, 255, 255, 255, 255);

    /* Empty text */
    sh_font_render_text(pixels, 256, 1, font, "", -1, 0, 0, 16.0f, 255, 255, 255, 255);

    /* Zero font size */
    sh_font_render_text(pixels, 256, 1, font, "Hi", -1, 0, 0, 0.0f, 255, 255, 255, 255);

    return 1;
}

TEST(render_text_explicit_length)
{
    const SHFont *font = sh_font_get_default();
    if (!font) return 1;

    int w = 256, h = 64;
    uint8_t *pixels1 = calloc(w * h, 4);
    uint8_t *pixels2 = calloc(w * h, 4);
    ASSERT(pixels1 && pixels2);

    /* Render "Hello World" with explicit length 5 (just "Hello") */
    sh_font_render_text(pixels1, w, h, font, "Hello World", 5,
                        10.0f, 10.0f, 24.0f, 255, 255, 255, 255);

    /* Render just "Hello" */
    sh_font_render_text(pixels2, w, h, font, "Hello", -1,
                        10.0f, 10.0f, 24.0f, 255, 255, 255, 255);

    /* Both should produce identical output */
    int identical = 1;
    for (int i = 0; i < w * h * 4; i++) {
        if (pixels1[i] != pixels2[i]) {
            identical = 0;
            break;
        }
    }
    ASSERT(identical);

    free(pixels1);
    free(pixels2);
    return 1;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void)
{
    printf("\nFont Tests:\n");

    printf("\nFont Access:\n");
    RUN_TEST(get_default_font);
    RUN_TEST(font_has_ascii);

    printf("\nGlyph Lookup:\n");
    RUN_TEST(get_glyph_ascii);
    RUN_TEST(get_glyph_missing);
    RUN_TEST(get_glyph_null_font);
    RUN_TEST(get_advance);

    printf("\nUTF-8:\n");
    RUN_TEST(utf8_decode_ascii);
    RUN_TEST(utf8_decode_2byte);
    RUN_TEST(utf8_decode_3byte);
    RUN_TEST(utf8_decode_4byte);
    RUN_TEST(utf8_decode_invalid);
    RUN_TEST(utf8_decode_null);
    RUN_TEST(utf8_strlen);

    printf("\nText Measurement:\n");
    RUN_TEST(text_width_basic);
    RUN_TEST(text_width_empty);
    RUN_TEST(text_width_scales_with_size);
    RUN_TEST(text_width_n);
    RUN_TEST(line_height);

    printf("\nMSDF Sampling:\n");
    RUN_TEST(msdf_sample_bounds);
    RUN_TEST(msdf_coverage);
    RUN_TEST(msdf_sample_bilinear);
    RUN_TEST(msdf_coverage_bilinear);
    RUN_TEST(msdf_coverage_threshold);

    printf("\nGlyph Rendering:\n");
    RUN_TEST(render_glyph_basic);
    RUN_TEST(render_glyph_null_safety);
    RUN_TEST(render_glyph_clipping);
    RUN_TEST(render_glyph_alpha);
    RUN_TEST(render_text_basic);
    RUN_TEST(render_text_null_safety);
    RUN_TEST(render_text_explicit_length);

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);

    return tests_passed == tests_run ? 0 : 1;
}
