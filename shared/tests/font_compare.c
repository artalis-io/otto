/**
 * font_compare.c - Software vs WebGL Font Rendering Comparison
 *
 * Renders test strings using the software renderer and outputs PNG files.
 * These can be compared against WebGL-rendered references.
 *
 * Build: gcc -O2 -std=c11 -I../include -I../../vendor/miniz font_compare.c \
 *        -L.. -lshared -L../../carta -lcarta -lm -o font_compare
 *
 * Run: ./font_compare
 * Output: font_compare_*.png files
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "sh_font.h"
#include "sh_render.h"
#include "ct_png.h"  /* carta PNG encoder */

/* Test cases - various font sizes and strings */
typedef struct {
    const char *name;
    const char *text;
    float font_size;
    int width;
    int height;
} TestCase;

static TestCase test_cases[] = {
    { "hello_16",    "Hello World",           16.0f, 200, 40 },
    { "hello_24",    "Hello World",           24.0f, 250, 50 },
    { "hello_32",    "Hello World",           32.0f, 320, 60 },
    { "hello_48",    "Hello World",           48.0f, 480, 80 },
    { "alphabet_16", "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 16.0f, 400, 40 },
    { "alphabet_24", "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 24.0f, 550, 50 },
    { "numbers_24",  "0123456789",            24.0f, 200, 50 },
    { "mixed_24",    "The quick brown fox jumps over the lazy dog.", 24.0f, 800, 50 },
    { "small_12",    "Small text at 12px",    12.0f, 200, 30 },
    { "large_64",    "BIG",                   64.0f, 200, 100 },
    /* Problem characters - for debugging */
    { "Z_24",        "Z",                     24.0f, 40, 40 },
    { "z_24",        "z",                     24.0f, 40, 40 },
    { "R_24",        "R",                     24.0f, 40, 40 },
    { "B_24",        "B",                     24.0f, 40, 40 },
};

static int num_test_cases = sizeof(test_cases) / sizeof(test_cases[0]);

/* Render a test case and save as PNG */
static int render_test_case(const TestCase *tc, const char *output_dir)
{
    const SHFont *font = sh_font_get_default();
    if (!font) {
        fprintf(stderr, "Failed to get default font\n");
        return -1;
    }

    /* Allocate buffer */
    uint8_t *pixels = calloc(tc->width * tc->height, 4);
    if (!pixels) {
        fprintf(stderr, "Failed to allocate buffer\n");
        return -1;
    }

    /* Clear to dark gray (same as WebGL demo background) */
    for (int i = 0; i < tc->width * tc->height; i++) {
        pixels[i * 4 + 0] = 30;   /* R */
        pixels[i * 4 + 1] = 30;   /* G */
        pixels[i * 4 + 2] = 30;   /* B */
        pixels[i * 4 + 3] = 255;  /* A */
    }

    /* Render text - white on dark background */
    /* Position: 10px from left, vertically centered */
    float y = (tc->height - tc->font_size) / 2.0f;

    /* Debug: print glyph position for single-char tests */
    if (strlen(tc->text) == 1) {
        const SHGlyph *g = sh_font_get_glyph(font, tc->text[0]);
        if (g) {
            float glyph_y = y + (1.0f - g->plane.top) * tc->font_size;
            float glyph_h = (g->plane.top - g->plane.bottom) * tc->font_size;
            printf("  %s: y=%.2f, glyph_y=%.2f, glyph_h=%.2f, range=[%.2f, %.2f]\n",
                   tc->name, y, glyph_y, glyph_h, glyph_y, glyph_y + glyph_h);
        }
    }

    sh_font_render_text(pixels, tc->width, tc->height,
                        font, tc->text, -1,
                        10.0f, y, tc->font_size,
                        255, 255, 255, 255);

    /* Debug: check pixels at (12,14-16) for Z_24 */
    if (strcmp(tc->name, "Z_24") == 0) {
        printf("  Buffer check after render:\n");
        for (int dy = 14; dy <= 20; dy++) {
            int idx = (dy * tc->width + 12) * 4;
            printf("    (12,%d): (%d,%d,%d,%d)\n", dy,
                   pixels[idx], pixels[idx+1], pixels[idx+2], pixels[idx+3]);
        }
    }

    /* Encode as PNG */
    size_t png_capacity = ct_png_max_size(tc->width, tc->height);
    uint8_t *png_data = malloc(png_capacity);
    if (!png_data) {
        fprintf(stderr, "Failed to allocate PNG buffer\n");
        free(pixels);
        return -1;
    }

    size_t png_size = ct_encode_png(pixels, tc->width, tc->height, NULL,
                                     png_data, png_capacity);
    if (png_size == 0) {
        fprintf(stderr, "Failed to encode PNG for %s\n", tc->name);
        free(png_data);
        free(pixels);
        return -1;
    }

    /* Write to file */
    char filename[256];
    snprintf(filename, sizeof(filename), "%s/soft_%s.png", output_dir, tc->name);
    FILE *f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "Failed to open %s for writing\n", filename);
        free(png_data);
        free(pixels);
        return -1;
    }
    fwrite(png_data, 1, png_size, f);
    fclose(f);

    printf("  Written: %s (%zu bytes)\n", filename, png_size);

    free(png_data);
    free(pixels);
    return 0;
}

/* Generate HTML comparison page */
static int generate_html(const char *output_dir)
{
    char filename[256];
    snprintf(filename, sizeof(filename), "%s/compare.html", output_dir);

    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "Failed to create %s\n", filename);
        return -1;
    }

    fprintf(f, "<!DOCTYPE html>\n");
    fprintf(f, "<html>\n<head>\n");
    fprintf(f, "<title>Font Rendering Comparison: Software vs WebGL</title>\n");
    fprintf(f, "<style>\n");
    fprintf(f, "body { font-family: sans-serif; background: #1a1a1a; color: #fff; padding: 20px; }\n");
    fprintf(f, "h1 { color: #4af; }\n");
    fprintf(f, ".test-case { margin: 20px 0; padding: 15px; background: #2a2a2a; border-radius: 8px; }\n");
    fprintf(f, ".test-case h3 { margin: 0 0 10px 0; color: #aaa; }\n");
    fprintf(f, ".comparison { display: flex; gap: 20px; align-items: flex-start; }\n");
    fprintf(f, ".render { text-align: center; }\n");
    fprintf(f, ".render h4 { margin: 0 0 5px 0; color: #888; font-size: 12px; }\n");
    fprintf(f, ".render canvas, .render img { border: 1px solid #444; }\n");
    fprintf(f, ".diff { background: #000; }\n");
    fprintf(f, ".stats { margin-top: 10px; font-size: 12px; color: #888; }\n");
    fprintf(f, "</style>\n");
    fprintf(f, "</head>\n<body>\n");
    fprintf(f, "<h1>Font Rendering Comparison</h1>\n");
    fprintf(f, "<p>Software renderer (left) vs WebGL MSDF (right). Difference shown in center.</p>\n");

    /* Test cases */
    for (int i = 0; i < num_test_cases; i++) {
        TestCase *tc = &test_cases[i];
        fprintf(f, "<div class=\"test-case\" id=\"%s\">\n", tc->name);
        fprintf(f, "  <h3>%s: \"%s\" @ %.0fpx</h3>\n", tc->name, tc->text, tc->font_size);
        fprintf(f, "  <div class=\"comparison\">\n");
        fprintf(f, "    <div class=\"render\">\n");
        fprintf(f, "      <h4>Software</h4>\n");
        fprintf(f, "      <img src=\"soft_%s.png\" width=\"%d\" height=\"%d\">\n",
                tc->name, tc->width, tc->height);
        fprintf(f, "    </div>\n");
        fprintf(f, "    <div class=\"render\">\n");
        fprintf(f, "      <h4>Difference</h4>\n");
        fprintf(f, "      <canvas id=\"diff_%s\" width=\"%d\" height=\"%d\" class=\"diff\"></canvas>\n",
                tc->name, tc->width, tc->height);
        fprintf(f, "      <div class=\"stats\" id=\"stats_%s\"></div>\n", tc->name);
        fprintf(f, "    </div>\n");
        fprintf(f, "    <div class=\"render\">\n");
        fprintf(f, "      <h4>WebGL</h4>\n");
        fprintf(f, "      <canvas id=\"webgl_%s\" width=\"%d\" height=\"%d\"></canvas>\n",
                tc->name, tc->width, tc->height);
        fprintf(f, "    </div>\n");
        fprintf(f, "  </div>\n");
        fprintf(f, "</div>\n");
    }

    /* JavaScript for WebGL rendering and comparison */
    fprintf(f, "<script type=\"module\">\n");
    fprintf(f, "import { ClayRenderer, MSDFFont } from '../../clayshards/clay-shards-webgl/index.js';\n\n");

    fprintf(f, "const testCases = [\n");
    for (int i = 0; i < num_test_cases; i++) {
        TestCase *tc = &test_cases[i];
        fprintf(f, "  { name: '%s', text: '%s', fontSize: %.1f, width: %d, height: %d },\n",
                tc->name, tc->text, tc->font_size, tc->width, tc->height);
    }
    fprintf(f, "];\n\n");

    fprintf(f, "async function loadImage(src) {\n");
    fprintf(f, "  return new Promise((resolve, reject) => {\n");
    fprintf(f, "    const img = new Image();\n");
    fprintf(f, "    img.onload = () => resolve(img);\n");
    fprintf(f, "    img.onerror = reject;\n");
    fprintf(f, "    img.src = src;\n");
    fprintf(f, "  });\n");
    fprintf(f, "}\n\n");

    fprintf(f, "async function main() {\n");
    fprintf(f, "  // Process one at a time to avoid WebGL context limits\n");
    fprintf(f, "  for (const tc of testCases) {\n");
    fprintf(f, "    const canvas = document.getElementById('webgl_' + tc.name);\n");
    fprintf(f, "    const renderer = new ClayRenderer(canvas);\n");
    fprintf(f, "    const font = new MSDFFont();\n");
    fprintf(f, "    await font.load(renderer.gl, '../../clayshards/fonts/ui-font.json', '../../clayshards/fonts/ui-font.png');\n");
    fprintf(f, "    renderer.setFont(font);\n\n");

    fprintf(f, "    renderer.clear(30/255, 30/255, 30/255);\n");
    fprintf(f, "    const proj = renderer.getProjectionMatrix();\n");
    fprintf(f, "    const y = (tc.height - tc.fontSize) / 2;\n");
    fprintf(f, "    renderer.renderText(tc.text, 10, y, tc.fontSize, [1, 1, 1, 1], proj);\n\n");

    fprintf(f, "    // Read pixels and compare immediately before moving to next\n");
    fprintf(f, "    const gl = renderer.gl;\n");
    fprintf(f, "    const webglPixels = new Uint8Array(tc.width * tc.height * 4);\n");
    fprintf(f, "    gl.readPixels(0, 0, tc.width, tc.height, gl.RGBA, gl.UNSIGNED_BYTE, webglPixels);\n\n");

    fprintf(f, "    const softImg = await loadImage('soft_' + tc.name + '.png');\n");
    fprintf(f, "    comparePngs(tc.name, softImg, webglPixels, tc.width, tc.height);\n");
    fprintf(f, "  }\n");
    fprintf(f, "}\n\n");

    fprintf(f, "function comparePngs(name, softImg, webglPixels, w, h) {\n");
    fprintf(f, "  const diffCanvas = document.getElementById('diff_' + name);\n");
    fprintf(f, "  const ctx = diffCanvas.getContext('2d');\n\n");

    fprintf(f, "  const softCanvas = document.createElement('canvas');\n");
    fprintf(f, "  softCanvas.width = w; softCanvas.height = h;\n");
    fprintf(f, "  const softCtx = softCanvas.getContext('2d');\n");
    fprintf(f, "  softCtx.drawImage(softImg, 0, 0);\n");
    fprintf(f, "  const softData = softCtx.getImageData(0, 0, w, h).data;\n\n");

    fprintf(f, "  // Compute difference (webglPixels is bottom-up)\n");
    fprintf(f, "  const diffData = ctx.createImageData(w, h);\n");
    fprintf(f, "  let totalDiff = 0, maxDiff = 0, diffPixels = 0;\n");
    fprintf(f, "  for (let y = 0; y < h; y++) {\n");
    fprintf(f, "    for (let x = 0; x < w; x++) {\n");
    fprintf(f, "      const si = (y * w + x) * 4;\n");
    fprintf(f, "      const wi = ((h - 1 - y) * w + x) * 4;\n");
    fprintf(f, "      const di = (y * w + x) * 4;\n\n");

    fprintf(f, "      const dr = Math.abs(softData[si] - webglPixels[wi]);\n");
    fprintf(f, "      const dg = Math.abs(softData[si+1] - webglPixels[wi+1]);\n");
    fprintf(f, "      const db = Math.abs(softData[si+2] - webglPixels[wi+2]);\n");
    fprintf(f, "      const da = Math.abs(softData[si+3] - webglPixels[wi+3]);\n");
    fprintf(f, "      const diff = Math.max(dr, dg, db, da);\n\n");

    fprintf(f, "      if (diff > 0) diffPixels++;\n");
    fprintf(f, "      totalDiff += diff;\n");
    fprintf(f, "      maxDiff = Math.max(maxDiff, diff);\n\n");

    fprintf(f, "      // Visualize: red = software brighter, blue = webgl brighter\n");
    fprintf(f, "      const softBright = (softData[si] + softData[si+1] + softData[si+2]) / 3;\n");
    fprintf(f, "      const webglBright = (webglPixels[wi] + webglPixels[wi+1] + webglPixels[wi+2]) / 3;\n");
    fprintf(f, "      if (softBright > webglBright) {\n");
    fprintf(f, "        diffData.data[di] = Math.min(255, diff * 4);   // Red\n");
    fprintf(f, "        diffData.data[di+1] = 0;\n");
    fprintf(f, "        diffData.data[di+2] = 0;\n");
    fprintf(f, "      } else {\n");
    fprintf(f, "        diffData.data[di] = 0;\n");
    fprintf(f, "        diffData.data[di+1] = 0;\n");
    fprintf(f, "        diffData.data[di+2] = Math.min(255, diff * 4); // Blue\n");
    fprintf(f, "      }\n");
    fprintf(f, "      diffData.data[di+3] = 255;\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n");
    fprintf(f, "  ctx.putImageData(diffData, 0, 0);\n\n");

    fprintf(f, "  // Show stats\n");
    fprintf(f, "  const avgDiff = totalDiff / (w * h);\n");
    fprintf(f, "  const pctDiff = (diffPixels / (w * h) * 100).toFixed(1);\n");
    fprintf(f, "  document.getElementById('stats_' + name).textContent = \n");
    fprintf(f, "    `Diff pixels: ${diffPixels} (${pctDiff}%%) | Max diff: ${maxDiff} | Avg diff: ${avgDiff.toFixed(2)}`;\n");
    fprintf(f, "}\n\n");

    fprintf(f, "main().catch(console.error);\n");
    fprintf(f, "</script>\n");
    fprintf(f, "</body>\n</html>\n");

    fclose(f);
    printf("  Written: %s\n", filename);
    return 0;
}

int main(int argc, char **argv)
{
    const char *output_dir = ".";
    if (argc > 1) {
        output_dir = argv[1];
    }

    printf("Font Rendering Comparison Tool\n");
    printf("==============================\n\n");

    printf("Rendering test cases with software renderer...\n");
    for (int i = 0; i < num_test_cases; i++) {
        if (render_test_case(&test_cases[i], output_dir) != 0) {
            fprintf(stderr, "Failed to render %s\n", test_cases[i].name);
        }
    }

    printf("\nGenerating comparison HTML...\n");
    if (generate_html(output_dir) != 0) {
        return 1;
    }

    printf("\nDone! Open %s/compare.html in a browser to see the comparison.\n", output_dir);
    printf("Red pixels = software brighter, Blue pixels = WebGL brighter\n");

    return 0;
}
