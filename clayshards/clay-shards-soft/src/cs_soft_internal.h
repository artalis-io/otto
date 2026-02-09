/**
 * cs_soft_internal.h - Internal types for software renderer
 */

#ifndef CS_SOFT_INTERNAL_H
#define CS_SOFT_INTERNAL_H

#include "../include/cs_soft.h"
#include "cs_render.h"

/* ============================================================================
 * Platform vtable
 * ============================================================================ */

typedef struct CsSoftPlatformCtx CsSoftPlatformCtx;

typedef struct {
    CsSoftPlatformCtx *(*create)(int w, int h, const char *title, bool vsync);
    void (*destroy)(CsSoftPlatformCtx *ctx);
    bool (*poll_event)(CsSoftPlatformCtx *ctx, CsSoftEvent *event);
    uint8_t *(*get_framebuffer)(CsSoftPlatformCtx *ctx);
    void (*present)(CsSoftPlatformCtx *ctx);
    bool (*should_close)(CsSoftPlatformCtx *ctx);
    void (*request_close)(CsSoftPlatformCtx *ctx);
    bool (*resize)(CsSoftPlatformCtx *ctx, int w, int h);
    void (*get_size)(CsSoftPlatformCtx *ctx, int *w, int *h);
} CsSoftPlatformVtable;

/* ============================================================================
 * Renderer structure
 * ============================================================================ */

struct CsSoftRenderer {
    CsSoftConfig config;

    /* Framebuffer (owned by renderer for headless, or by platform otherwise) */
    uint8_t *pixels;
    int width;
    int height;
    int stride;         /* Bytes per row (width * 4) */

    /* Platform backend */
    CsSoftPlatformVtable vtable;
    CsSoftPlatformCtx *platform_ctx;

    /* Scissor stack */
    CsScissorStack scissor;

    /* State */
    bool should_close;
    bool owns_pixels;   /* True if we allocated pixels (headless mode) */
};

/* ============================================================================
 * Headless platform (built-in)
 * ============================================================================ */

CsSoftPlatformCtx *cs_soft_headless_create(int w, int h, const char *title, bool vsync);
void cs_soft_headless_destroy(CsSoftPlatformCtx *ctx);
bool cs_soft_headless_poll_event(CsSoftPlatformCtx *ctx, CsSoftEvent *event);
uint8_t *cs_soft_headless_get_framebuffer(CsSoftPlatformCtx *ctx);
void cs_soft_headless_present(CsSoftPlatformCtx *ctx);
bool cs_soft_headless_should_close(CsSoftPlatformCtx *ctx);
void cs_soft_headless_request_close(CsSoftPlatformCtx *ctx);
bool cs_soft_headless_resize(CsSoftPlatformCtx *ctx, int w, int h);
void cs_soft_headless_get_size(CsSoftPlatformCtx *ctx, int *w, int *h);

/* ============================================================================
 * macOS/Cocoa platform
 * ============================================================================ */

#ifdef __APPLE__
CsSoftPlatformCtx *cs_soft_cocoa_create(int w, int h, const char *title, bool vsync);
void cs_soft_cocoa_destroy(CsSoftPlatformCtx *ctx);
bool cs_soft_cocoa_poll_event(CsSoftPlatformCtx *ctx, CsSoftEvent *event);
uint8_t *cs_soft_cocoa_get_framebuffer(CsSoftPlatformCtx *ctx);
void cs_soft_cocoa_present(CsSoftPlatformCtx *ctx);
bool cs_soft_cocoa_should_close(CsSoftPlatformCtx *ctx);
void cs_soft_cocoa_request_close(CsSoftPlatformCtx *ctx);
bool cs_soft_cocoa_resize(CsSoftPlatformCtx *ctx, int w, int h);
void cs_soft_cocoa_get_size(CsSoftPlatformCtx *ctx, int *w, int *h);
#endif

/* ============================================================================
 * Color conversion
 * ============================================================================ */

/* ClayShards color format: (r << 24) | (g << 16) | (b << 8) | a */
/* sh_render format: (a << 24) | (b << 16) | (g << 8) | r */

static inline uint32_t cs_to_sh_color(uint32_t cs_color)
{
    uint8_t r = (cs_color >> 24) & 0xFF;
    uint8_t g = (cs_color >> 16) & 0xFF;
    uint8_t b = (cs_color >> 8) & 0xFF;
    uint8_t a = cs_color & 0xFF;
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

#endif /* CS_SOFT_INTERNAL_H */
