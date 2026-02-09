/**
 * cs_soft.c - ClayShards Software Renderer Core
 *
 * Core renderer implementation with headless platform support.
 */

#include "cs_soft_internal.h"
#include "sh_render.h"
#include "sh_font.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Include Clay for render commands */
#include "clay.h"

/* ============================================================================
 * Configuration
 * ============================================================================ */

void cs_soft_config_init(CsSoftConfig *config)
{
    if (!config) return;

    config->width = 800;
    config->height = 600;
    config->platform = CS_SOFT_PLATFORM_HEADLESS;
    config->double_buffer = true;
    config->vsync = true;
    config->title = "ClayShards";
}

/* ============================================================================
 * Headless Platform Implementation
 * ============================================================================ */

typedef struct {
    uint8_t *pixels;
    int width;
    int height;
    bool should_close;
} HeadlessContext;

CsSoftPlatformCtx *cs_soft_headless_create(int w, int h, const char *title, bool vsync)
{
    (void)title;
    (void)vsync;

    HeadlessContext *ctx = calloc(1, sizeof(HeadlessContext));
    if (!ctx) return NULL;

    ctx->width = w > 0 ? w : 800;
    ctx->height = h > 0 ? h : 600;

    size_t buf_size = (size_t)ctx->width * (size_t)ctx->height * 4;
    ctx->pixels = calloc(buf_size, 1);
    if (!ctx->pixels) {
        free(ctx);
        return NULL;
    }

    ctx->should_close = false;
    return (CsSoftPlatformCtx *)ctx;
}

void cs_soft_headless_destroy(CsSoftPlatformCtx *ctx)
{
    HeadlessContext *h = (HeadlessContext *)ctx;
    if (!h) return;
    free(h->pixels);
    free(h);
}

bool cs_soft_headless_poll_event(CsSoftPlatformCtx *ctx, CsSoftEvent *event)
{
    (void)ctx;
    if (event) event->type = CS_SOFT_EVENT_NONE;
    return false;  /* No events in headless mode */
}

uint8_t *cs_soft_headless_get_framebuffer(CsSoftPlatformCtx *ctx)
{
    HeadlessContext *h = (HeadlessContext *)ctx;
    return h ? h->pixels : NULL;
}

void cs_soft_headless_present(CsSoftPlatformCtx *ctx)
{
    (void)ctx;
    /* No-op for headless */
}

bool cs_soft_headless_should_close(CsSoftPlatformCtx *ctx)
{
    HeadlessContext *h = (HeadlessContext *)ctx;
    return h ? h->should_close : true;
}

void cs_soft_headless_request_close(CsSoftPlatformCtx *ctx)
{
    HeadlessContext *h = (HeadlessContext *)ctx;
    if (h) h->should_close = true;
}

bool cs_soft_headless_resize(CsSoftPlatformCtx *ctx, int w, int h)
{
    HeadlessContext *hctx = (HeadlessContext *)ctx;
    if (!hctx || w <= 0 || h <= 0) return false;

    size_t new_size = (size_t)w * (size_t)h * 4;
    uint8_t *new_pixels = calloc(new_size, 1);
    if (!new_pixels) return false;

    free(hctx->pixels);
    hctx->pixels = new_pixels;
    hctx->width = w;
    hctx->height = h;
    return true;
}

void cs_soft_headless_get_size(CsSoftPlatformCtx *ctx, int *w, int *h)
{
    HeadlessContext *hctx = (HeadlessContext *)ctx;
    if (hctx) {
        if (w) *w = hctx->width;
        if (h) *h = hctx->height;
    } else {
        if (w) *w = 0;
        if (h) *h = 0;
    }
}

static CsSoftPlatformVtable headless_vtable = {
    .create = cs_soft_headless_create,
    .destroy = cs_soft_headless_destroy,
    .poll_event = cs_soft_headless_poll_event,
    .get_framebuffer = cs_soft_headless_get_framebuffer,
    .present = cs_soft_headless_present,
    .should_close = cs_soft_headless_should_close,
    .request_close = cs_soft_headless_request_close,
    .resize = cs_soft_headless_resize,
    .get_size = cs_soft_headless_get_size
};

#ifdef __APPLE__
static CsSoftPlatformVtable cocoa_vtable = {
    .create = cs_soft_cocoa_create,
    .destroy = cs_soft_cocoa_destroy,
    .poll_event = cs_soft_cocoa_poll_event,
    .get_framebuffer = cs_soft_cocoa_get_framebuffer,
    .present = cs_soft_cocoa_present,
    .should_close = cs_soft_cocoa_should_close,
    .request_close = cs_soft_cocoa_request_close,
    .resize = cs_soft_cocoa_resize,
    .get_size = cs_soft_cocoa_get_size
};
#endif

/* ============================================================================
 * Renderer Lifecycle
 * ============================================================================ */

CsSoftRenderer *cs_soft_create(const CsSoftConfig *config)
{
    CsSoftRenderer *r = calloc(1, sizeof(CsSoftRenderer));
    if (!r) return NULL;

    /* Apply config or defaults */
    if (config) {
        r->config = *config;
    } else {
        cs_soft_config_init(&r->config);
    }

    /* Select platform */
    CsSoftPlatform platform = r->config.platform;
    if (platform == CS_SOFT_PLATFORM_AUTO) {
#ifdef __APPLE__
        platform = CS_SOFT_PLATFORM_COCOA;
#else
        platform = CS_SOFT_PLATFORM_HEADLESS;
#endif
    }

    switch (platform) {
#ifdef __APPLE__
        case CS_SOFT_PLATFORM_COCOA:
            r->vtable = cocoa_vtable;
            break;
#endif
        case CS_SOFT_PLATFORM_HEADLESS:
        case CS_SOFT_PLATFORM_AUTO:
        default:
            r->vtable = headless_vtable;
            break;
        /* TODO: Add X11, Win32, Wayland backends */
    }

    /* Create platform context */
    r->platform_ctx = r->vtable.create(
        r->config.width,
        r->config.height,
        r->config.title,
        r->config.vsync
    );

    if (!r->platform_ctx) {
        free(r);
        return NULL;
    }

    /* Get framebuffer from platform */
    r->vtable.get_size(r->platform_ctx, &r->width, &r->height);
    r->pixels = r->vtable.get_framebuffer(r->platform_ctx);
    r->stride = r->width * 4;
    r->owns_pixels = false;  /* Platform owns the buffer */

    /* Initialize scissor stack */
    cs_scissor_init(&r->scissor, r->width, r->height);

    return r;
}

void cs_soft_free(CsSoftRenderer *r)
{
    if (!r) return;

    if (r->platform_ctx) {
        r->vtable.destroy(r->platform_ctx);
    }

    free(r);
}

void cs_soft_get_size(CsSoftRenderer *r, int *width, int *height)
{
    if (!r) {
        if (width) *width = 0;
        if (height) *height = 0;
        return;
    }
    if (width) *width = r->width;
    if (height) *height = r->height;
}

bool cs_soft_resize(CsSoftRenderer *r, int width, int height)
{
    if (!r || width <= 0 || height <= 0) return false;

    if (!r->vtable.resize(r->platform_ctx, width, height)) {
        return false;
    }

    r->width = width;
    r->height = height;
    r->stride = width * 4;
    r->pixels = r->vtable.get_framebuffer(r->platform_ctx);

    /* Re-initialize scissor stack with new dimensions */
    cs_scissor_init(&r->scissor, width, height);

    return true;
}

/* ============================================================================
 * Event Handling
 * ============================================================================ */

bool cs_soft_poll_event(CsSoftRenderer *r, CsSoftEvent *event)
{
    if (!r || !event) return false;
    return r->vtable.poll_event(r->platform_ctx, event);
}

bool cs_soft_should_close(CsSoftRenderer *r)
{
    if (!r) return true;
    return r->should_close || r->vtable.should_close(r->platform_ctx);
}

void cs_soft_request_close(CsSoftRenderer *r)
{
    if (!r) return;
    r->should_close = true;
    r->vtable.request_close(r->platform_ctx);
}

/* ============================================================================
 * Rendering
 * ============================================================================ */

void cs_soft_begin(CsSoftRenderer *r)
{
    if (!r) return;

    /* Reset scissor stack */
    cs_scissor_init(&r->scissor, r->width, r->height);
}

void cs_soft_clear(CsSoftRenderer *r, uint32_t color)
{
    if (!r || !r->pixels) return;

    /* Convert ClayShards color to sh_render format */
    uint32_t sh_color = cs_to_sh_color(color);
    sh_clear_buffer(r->pixels, r->width, r->height, sh_color);
}

void cs_soft_end(CsSoftRenderer *r)
{
    if (!r) return;
    r->vtable.present(r->platform_ctx);
}

/* ============================================================================
 * Primitives
 * ============================================================================ */

void cs_soft_rect(CsSoftRenderer *r, float x, float y, float w, float h,
                  uint32_t color, float radius)
{
    if (!r || !r->pixels) return;
    if (w <= 0 || h <= 0) return;

    /* Original rect coordinates (before clipping) */
    int orig_x = (int)x;
    int orig_y = (int)y;
    int orig_w = (int)w;
    int orig_h = (int)h;

    uint32_t sh_color = cs_to_sh_color(color);

    if (radius <= 0) {
        /* Simple rectangle - clip and fill row by row */
        int ix = orig_x, iy = orig_y, iw = orig_w, ih = orig_h;
        if (!cs_scissor_clip_rect(&r->scissor, &ix, &iy, &iw, &ih)) {
            return;
        }
        for (int row = 0; row < ih; row++) {
            sh_fill_span(r->pixels, r->width, r->height,
                         iy + row, ix, ix + iw - 1, sh_color);
        }
    } else {
        /* Rounded rectangle - need to handle clipping per-scanline */
        int r_int = (int)radius;
        if (r_int > orig_w / 2) r_int = orig_w / 2;
        if (r_int > orig_h / 2) r_int = orig_h / 2;
        if (r_int < 1) r_int = 1;

        /* Get current scissor bounds */
        CsScissor sc = cs_scissor_current(&r->scissor);

        /* Fill each row with appropriate corner offsets */
        for (int row = 0; row < orig_h; row++) {
            int screen_y = orig_y + row;

            /* Skip if outside scissor vertically */
            if (screen_y < sc.y || screen_y >= sc.y + sc.h) continue;

            /* Calculate x offset for rounded corners */
            int x_offset = 0;
            if (row < r_int) {
                /* Top corner region */
                float dy = r_int - row - 0.5f;
                float dx_sq = radius * radius - dy * dy;
                if (dx_sq > 0) {
                    x_offset = (int)(radius - sqrtf(dx_sq));
                }
            } else if (row >= orig_h - r_int) {
                /* Bottom corner region */
                float dy = row - (orig_h - r_int) + 0.5f;
                float dx_sq = radius * radius - dy * dy;
                if (dx_sq > 0) {
                    x_offset = (int)(radius - sqrtf(dx_sq));
                }
            }

            /* Calculate span with corner offset */
            int x1 = orig_x + x_offset;
            int x2 = orig_x + orig_w - 1 - x_offset;

            /* Clip to scissor horizontally */
            if (x1 < sc.x) x1 = sc.x;
            if (x2 >= sc.x + sc.w) x2 = sc.x + sc.w - 1;

            if (x2 >= x1) {
                sh_fill_span(r->pixels, r->width, r->height,
                             screen_y, x1, x2, sh_color);
            }
        }
    }
}

void cs_soft_border(CsSoftRenderer *r, float x, float y, float w, float h,
                    uint32_t color, float width)
{
    cs_soft_border_sides(r, x, y, w, h, color, width, width, width, width);
}

void cs_soft_border_sides(CsSoftRenderer *r, float x, float y, float w, float h,
                          uint32_t color, float top, float right, float bottom, float left)
{
    if (!r || !r->pixels) return;

    /* Top border */
    if (top > 0) {
        cs_soft_rect(r, x, y, w, top, color, 0);
    }

    /* Bottom border */
    if (bottom > 0) {
        cs_soft_rect(r, x, y + h - bottom, w, bottom, color, 0);
    }

    /* Left border (between top and bottom) */
    if (left > 0) {
        float ly = y + top;
        float lh = h - top - bottom;
        if (lh > 0) {
            cs_soft_rect(r, x, ly, left, lh, color, 0);
        }
    }

    /* Right border (between top and bottom) */
    if (right > 0) {
        float ry = y + top;
        float rh = h - top - bottom;
        if (rh > 0) {
            cs_soft_rect(r, x + w - right, ry, right, rh, color, 0);
        }
    }
}

void cs_soft_text(CsSoftRenderer *r, const char *text, int len,
                  float x, float y, float size, uint32_t color)
{
    if (!r || !r->pixels || !text) return;

    const SHFont *font = sh_font_get_default();
    if (!font) return;

    if (len < 0) len = (int)strlen(text);

    /* Convert color */
    uint8_t cr = (color >> 24) & 0xFF;
    uint8_t cg = (color >> 16) & 0xFF;
    uint8_t cb = (color >> 8) & 0xFF;

    /* y is top of text bounding box - calculate baseline (same as carta) */
    float baseline_y = y + sh_font_ascent(font, size);
    float cursor_x = x;

    for (int i = 0; i < len; i++) {
        uint32_t codepoint;
        int bytes = sh_utf8_decode(text + i, &codepoint);

        const SHGlyph *glyph = sh_font_get_glyph(font, codepoint);
        if (!glyph) {
            i += bytes - 1;
            continue;
        }

        /* Calculate glyph dimensions in screen pixels (same as carta) */
        float glyph_w = (glyph->plane.right - glyph->plane.left) * size;
        float glyph_h = (glyph->plane.top - glyph->plane.bottom) * size;

        if (glyph_w <= 0.0f || glyph_h <= 0.0f) {
            cursor_x += glyph->advance * size;
            i += bytes - 1;
            continue;
        }

        /* Calculate glyph position (same as carta) */
        float glyph_x = cursor_x + glyph->plane.left * size;
        float glyph_y = baseline_y - glyph->plane.top * size;

        /* Integer dimensions and position for rendering
         * Extend by 1 pixel on each side to capture MSDF anti-aliasing at edges
         * (WebGL renders a quad that covers the full UV range; we need to match) */
        int gx = (int)floorf(glyph_x) - 1;
        int gy = (int)floorf(glyph_y) - 1;
        int gw = (int)ceilf(glyph_w) + 2;
        int gh = (int)ceilf(glyph_h) + 2;

        /* Render glyph using MSDF (same approach as carta ct_render_glyph) */
        for (int py = 0; py < gh; py++) {
            int screen_y = gy + py;
            if (screen_y < 0 || screen_y >= r->height) continue;

            for (int px = 0; px < gw; px++) {
                int screen_x = gx + px;
                if (screen_x < 0 || screen_x >= r->width) continue;

                /* Check scissor */
                if (!cs_scissor_test_point(&r->scissor, screen_x, screen_y)) {
                    continue;
                }

                /* Map screen pixel to local glyph coordinates [0, 1]
                 * Account for the 1-pixel extension on each side */
                float local_x = ((float)px - 0.5f) / glyph_w;
                float local_y = ((float)py - 0.5f) / glyph_h;

                /* Get MSDF coverage with threshold (uses bilinear sampling) */
                float coverage = sh_font_msdf_coverage_threshold(font, glyph,
                                                                  local_x, local_y,
                                                                  size, 0.5f);

                if (coverage <= 0.0f) continue;

                uint8_t alpha = (uint8_t)(coverage * 255.0f);

                /* Blend onto buffer using simple alpha blending
                 * Buffer is in RGBA format (R at offset+0) */
                int offset = (screen_y * r->width + screen_x) * 4;
                uint8_t dr = r->pixels[offset + 0];
                uint8_t dg = r->pixels[offset + 1];
                uint8_t db = r->pixels[offset + 2];
                uint8_t da = r->pixels[offset + 3];

                /* Porter-Duff source-over: out = src + dst * (1 - src_alpha) */
                uint32_t inv_alpha = 255 - alpha;
                r->pixels[offset + 0] = (uint8_t)((cr * alpha + dr * inv_alpha) / 255);
                r->pixels[offset + 1] = (uint8_t)((cg * alpha + dg * inv_alpha) / 255);
                r->pixels[offset + 2] = (uint8_t)((cb * alpha + db * inv_alpha) / 255);
                r->pixels[offset + 3] = (uint8_t)(alpha + (da * inv_alpha) / 255);
            }
        }

        cursor_x += glyph->advance * size;
        i += bytes - 1;
    }
}

/* ============================================================================
 * Clay Command Rendering
 * ============================================================================ */

void cs_soft_render_clay_commands(CsSoftRenderer *r, void *commands_ptr)
{
    if (!r || !commands_ptr) return;

    Clay_RenderCommandArray *commands = (Clay_RenderCommandArray *)commands_ptr;

    for (int i = 0; i < commands->length; i++) {
        Clay_RenderCommand *cmd = Clay_RenderCommandArray_Get(commands, i);
        if (!cmd) continue;

        Clay_BoundingBox box = cmd->boundingBox;
        float x = box.x;
        float y = box.y;
        float w = box.width;
        float h = box.height;

        switch (cmd->commandType) {
            case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
                Clay_RectangleRenderData *rect = &cmd->renderData.rectangle;
                uint32_t color = cs_pack_color(
                    (uint8_t)(rect->backgroundColor.r * 255),
                    (uint8_t)(rect->backgroundColor.g * 255),
                    (uint8_t)(rect->backgroundColor.b * 255),
                    (uint8_t)(rect->backgroundColor.a * 255)
                );
                float radius = rect->cornerRadius.topLeft;
                cs_soft_rect(r, x, y, w, h, color, radius);
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_TEXT: {
                Clay_TextRenderData *txt = &cmd->renderData.text;
                uint32_t color = cs_pack_color(
                    (uint8_t)(txt->textColor.r * 255),
                    (uint8_t)(txt->textColor.g * 255),
                    (uint8_t)(txt->textColor.b * 255),
                    (uint8_t)(txt->textColor.a * 255)
                );
                cs_soft_text(r, txt->stringContents.chars, txt->stringContents.length,
                            x, y, (float)txt->fontSize, color);
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_BORDER: {
                Clay_BorderRenderData *border = &cmd->renderData.border;
                /* Draw each border side if present */
                if (border->width.top > 0) {
                    uint32_t color = cs_pack_color(
                        (uint8_t)(border->color.r * 255),
                        (uint8_t)(border->color.g * 255),
                        (uint8_t)(border->color.b * 255),
                        (uint8_t)(border->color.a * 255)
                    );
                    cs_soft_rect(r, x, y, w, (float)border->width.top, color, 0);
                }
                if (border->width.bottom > 0) {
                    uint32_t color = cs_pack_color(
                        (uint8_t)(border->color.r * 255),
                        (uint8_t)(border->color.g * 255),
                        (uint8_t)(border->color.b * 255),
                        (uint8_t)(border->color.a * 255)
                    );
                    cs_soft_rect(r, x, y + h - (float)border->width.bottom, w,
                                (float)border->width.bottom, color, 0);
                }
                if (border->width.left > 0) {
                    uint32_t color = cs_pack_color(
                        (uint8_t)(border->color.r * 255),
                        (uint8_t)(border->color.g * 255),
                        (uint8_t)(border->color.b * 255),
                        (uint8_t)(border->color.a * 255)
                    );
                    cs_soft_rect(r, x, y, (float)border->width.left, h, color, 0);
                }
                if (border->width.right > 0) {
                    uint32_t color = cs_pack_color(
                        (uint8_t)(border->color.r * 255),
                        (uint8_t)(border->color.g * 255),
                        (uint8_t)(border->color.b * 255),
                        (uint8_t)(border->color.a * 255)
                    );
                    cs_soft_rect(r, x + w - (float)border->width.right, y,
                                (float)border->width.right, h, color, 0);
                }
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
                cs_scissor_push(&r->scissor, (int)x, (int)y, (int)w, (int)h);
                break;

            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
                cs_scissor_pop(&r->scissor);
                break;

            case CLAY_RENDER_COMMAND_TYPE_IMAGE:
                /* TODO: Image rendering */
                break;

            case CLAY_RENDER_COMMAND_TYPE_CUSTOM:
                /* Custom elements handled by application */
                break;

            default:
                break;
        }
    }
}

/* ============================================================================
 * Buffer Access
 * ============================================================================ */

uint8_t *cs_soft_get_pixels(CsSoftRenderer *r)
{
    return r ? r->pixels : NULL;
}

uint32_t cs_soft_get_pixel(CsSoftRenderer *r, int x, int y)
{
    if (!r || !r->pixels) return 0;
    if (x < 0 || x >= r->width || y < 0 || y >= r->height) return 0;

    int offset = (y * r->width + x) * 4;
    return cs_pack_color(r->pixels[offset], r->pixels[offset + 1],
                        r->pixels[offset + 2], r->pixels[offset + 3]);
}
