/*
 * ct_ascii.c - ASCII art tile renderer
 *
 * Uses luminance-based character density mapping with proper
 * aspect ratio correction for accurate representation.
 */

#include "ct_ascii.h"
#include <string.h>
#include <stdio.h>

/* Character ramps ordered by visual density (dark to light) */
static const char *CHARSET_SIMPLE = " .:-=+*#%@";
static const char *CHARSET_EXTENDED = " .'`^\",:;Il!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$";

/* Unicode block elements for high detail */
static const char *BLOCKS_CHARS[] = {
    " ", "\u2591", "\u2592", "\u2593", "\u2588"  /* Light to full block */
};
#define NUM_BLOCKS 5

/* Braille patterns - we'll use simplified 2-level per dot */
/* Full braille would need 256 patterns, simplified uses density */
static const char *BRAILLE_CHARS[] = {
    "\u2800", "\u2801", "\u2803", "\u2807", "\u280F",
    "\u281F", "\u283F", "\u287F", "\u28FF"
};
#define NUM_BRAILLE 9

/* Aspect ratio: terminal characters are typically ~2:1 (height:width) */
#define CHAR_ASPECT_RATIO 2.0

void ct_ascii_default_options(CTAsciiOptions *opts)
{
    opts->width = 80;
    opts->height = 0;  /* Auto-calculate from aspect ratio */
    opts->charset = CT_ASCII_EXTENDED;
    opts->invert = 0;  /* Dark background (typical terminal) */
    opts->color = 0;   /* No ANSI colors by default */
}

/*
 * Convert RGB to grayscale using perceived luminance.
 * Uses Rec. 709 coefficients for proper perceptual weighting.
 */
static inline int rgb_to_luminance(int r, int g, int b)
{
    return (int)(0.2126 * r + 0.7152 * g + 0.0722 * b);
}

/*
 * Sample a block of pixels and return average luminance.
 */
static int sample_block(const uint8_t *pixels, int img_width, int img_height,
                        int block_x, int block_y, int block_w, int block_h)
{
    long total_lum = 0;
    int count = 0;

    int start_x = block_x * block_w;
    int start_y = block_y * block_h;
    int end_x = start_x + block_w;
    int end_y = start_y + block_h;

    if (end_x > img_width) end_x = img_width;
    if (end_y > img_height) end_y = img_height;

    for (int y = start_y; y < end_y; y++) {
        for (int x = start_x; x < end_x; x++) {
            int idx = (y * img_width + x) * 4;
            int r = pixels[idx + 0];
            int g = pixels[idx + 1];
            int b = pixels[idx + 2];
            int a = pixels[idx + 3];

            /* Handle transparency - blend with white for light, black for dark */
            if (a < 255) {
                r = (r * a) / 255;
                g = (g * a) / 255;
                b = (b * a) / 255;
            }

            total_lum += rgb_to_luminance(r, g, b);
            count++;
        }
    }

    return count > 0 ? (int)(total_lum / count) : 0;
}

/*
 * Sample a block and return average RGB color.
 */
static void sample_block_color(const uint8_t *pixels, int img_width, int img_height,
                               int block_x, int block_y, int block_w, int block_h,
                               int *out_r, int *out_g, int *out_b)
{
    long total_r = 0, total_g = 0, total_b = 0;
    int count = 0;

    int start_x = block_x * block_w;
    int start_y = block_y * block_h;
    int end_x = start_x + block_w;
    int end_y = start_y + block_h;

    if (end_x > img_width) end_x = img_width;
    if (end_y > img_height) end_y = img_height;

    for (int y = start_y; y < end_y; y++) {
        for (int x = start_x; x < end_x; x++) {
            int idx = (y * img_width + x) * 4;
            total_r += pixels[idx + 0];
            total_g += pixels[idx + 1];
            total_b += pixels[idx + 2];
            count++;
        }
    }

    if (count > 0) {
        *out_r = (int)(total_r / count);
        *out_g = (int)(total_g / count);
        *out_b = (int)(total_b / count);
    } else {
        *out_r = *out_g = *out_b = 0;
    }
}

/*
 * Map luminance (0-255) to character index.
 */
static int luminance_to_index(int lum, int num_levels, int invert)
{
    if (invert) {
        lum = 255 - lum;
    }
    int idx = (lum * (num_levels - 1)) / 255;
    if (idx >= num_levels) idx = num_levels - 1;
    if (idx < 0) idx = 0;
    return idx;
}

/*
 * Write ANSI 256-color escape code.
 */
static int write_ansi_color(char *buf, size_t size, int r, int g, int b)
{
    /* Convert RGB to ANSI 256-color cube (6x6x6 + 24 grayscale) */
    int color;

    /* Check if grayscale */
    if (r == g && g == b) {
        /* Use grayscale ramp (232-255) */
        color = 232 + (r * 23) / 255;
    } else {
        /* Use 6x6x6 color cube (16-231) */
        int ri = (r * 5) / 255;
        int gi = (g * 5) / 255;
        int bi = (b * 5) / 255;
        color = 16 + 36 * ri + 6 * gi + bi;
    }

    return snprintf(buf, size, "\033[38;5;%dm", color);
}

size_t ct_ascii_buffer_size(int width, int height, CTAsciiCharset charset, int color)
{
    /* Each character: up to 4 bytes (UTF-8) + optional color code (~12 bytes) */
    size_t char_size = (charset == CT_ASCII_BLOCKS || charset == CT_ASCII_BRAILLE) ? 4 : 1;
    size_t color_size = color ? 12 : 0;
    size_t line_size = width * (char_size + color_size) + 1;  /* +1 for newline */

    return height * line_size + 16;  /* +16 for reset code and null */
}

size_t ct_render_ascii(const uint8_t *pixels, int img_width, int img_height,
                       const CTAsciiOptions *opts, char *out, size_t out_size)
{
    CTAsciiOptions default_opts;
    if (!opts) {
        ct_ascii_default_options(&default_opts);
        opts = &default_opts;
    }

    int ascii_width = opts->width;
    int ascii_height = opts->height;

    /* Auto-calculate height from aspect ratio */
    if (ascii_height <= 0) {
        double img_aspect = (double)img_width / img_height;
        ascii_height = (int)(ascii_width / (img_aspect * CHAR_ASPECT_RATIO));
        if (ascii_height < 1) ascii_height = 1;
    }

    /* Calculate block size for sampling */
    int block_w = img_width / ascii_width;
    int block_h = img_height / ascii_height;
    if (block_w < 1) block_w = 1;
    if (block_h < 1) block_h = 1;

    /* Get charset info */
    const char *charset = NULL;
    const char **charset_array = NULL;
    int num_levels = 0;
    int is_multibyte = 0;

    switch (opts->charset) {
        case CT_ASCII_SIMPLE:
            charset = CHARSET_SIMPLE;
            num_levels = (int)strlen(charset);
            break;
        case CT_ASCII_EXTENDED:
            charset = CHARSET_EXTENDED;
            num_levels = (int)strlen(charset);
            break;
        case CT_ASCII_BLOCKS:
            charset_array = BLOCKS_CHARS;
            num_levels = NUM_BLOCKS;
            is_multibyte = 1;
            break;
        case CT_ASCII_BRAILLE:
            charset_array = BRAILLE_CHARS;
            num_levels = NUM_BRAILLE;
            is_multibyte = 1;
            break;
    }

    /* Render ASCII */
    size_t pos = 0;

    for (int cy = 0; cy < ascii_height; cy++) {
        for (int cx = 0; cx < ascii_width; cx++) {
            /* Sample this character's block */
            int lum = sample_block(pixels, img_width, img_height,
                                   cx, cy, block_w, block_h);
            int idx = luminance_to_index(lum, num_levels, opts->invert);

            /* Add color code if enabled */
            if (opts->color) {
                int r, g, b;
                sample_block_color(pixels, img_width, img_height,
                                   cx, cy, block_w, block_h, &r, &g, &b);
                if (pos < out_size - 16) {
                    pos += write_ansi_color(out + pos, out_size - pos, r, g, b);
                }
            }

            /* Write character */
            if (is_multibyte) {
                const char *ch = charset_array[idx];
                size_t len = strlen(ch);
                if (pos + len < out_size) {
                    memcpy(out + pos, ch, len);
                    pos += len;
                }
            } else {
                if (pos < out_size - 1) {
                    out[pos++] = charset[idx];
                }
            }
        }

        /* Add newline */
        if (pos < out_size - 1) {
            out[pos++] = '\n';
        }
    }

    /* Reset color if we used colors */
    if (opts->color && pos < out_size - 5) {
        memcpy(out + pos, "\033[0m", 4);
        pos += 4;
    }

    /* Null terminate */
    if (pos < out_size) {
        out[pos] = '\0';
    } else if (out_size > 0) {
        out[out_size - 1] = '\0';
    }

    return pos;
}
