/*
 * ct_ascii.h - ASCII art tile renderer
 *
 * Converts rendered tiles to ASCII art using luminance-based
 * character density mapping.
 */

#ifndef CT_ASCII_H
#define CT_ASCII_H

#include "ct_types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Character set options */
typedef enum {
    CT_ASCII_SIMPLE,    /* " .:-=+*#%@" - 10 levels */
    CT_ASCII_EXTENDED,  /* More gradations for smoother output */
    CT_ASCII_BLOCKS,    /* Unicode block characters for high detail */
    CT_ASCII_BRAILLE    /* Unicode braille for maximum detail (2x4 per char) */
} CTAsciiCharset;

/* ASCII render options */
typedef struct {
    int width;              /* Output width in characters (default: 80) */
    int height;             /* Output height in characters (0 = auto from aspect) */
    CTAsciiCharset charset; /* Character set to use */
    int invert;             /* Invert brightness (1 = light bg, 0 = dark bg) */
    int color;              /* Include ANSI color codes (1 = yes) */
} CTAsciiOptions;

/* Initialize options with defaults */
void ct_ascii_default_options(CTAsciiOptions *opts);

/*
 * Render RGBA pixels to ASCII art.
 *
 * Parameters:
 *   pixels     - RGBA pixel data (4 bytes per pixel)
 *   img_width  - Image width in pixels
 *   img_height - Image height in pixels
 *   opts       - Render options (NULL for defaults)
 *   out        - Output buffer for ASCII string
 *   out_size   - Size of output buffer
 *
 * Returns: Number of bytes written (excluding null terminator), or 0 on error
 */
size_t ct_render_ascii(const uint8_t *pixels, int img_width, int img_height,
                       const CTAsciiOptions *opts, char *out, size_t out_size);

/*
 * Calculate required buffer size for ASCII output.
 */
size_t ct_ascii_buffer_size(int width, int height, CTAsciiCharset charset, int color);

#ifdef __cplusplus
}
#endif

#endif /* CT_ASCII_H */
