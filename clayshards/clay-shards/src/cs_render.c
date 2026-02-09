/**
 * cs_render.c - Shared Rendering Utilities Implementation
 *
 * Scissor stack and color utilities for ClayShards renderers.
 */

#include "../include/cs_render.h"

/* ============================================================================
 * Scissor Stack
 * ============================================================================ */

void cs_scissor_init(CsScissorStack *s, int screen_w, int screen_h)
{
    if (!s) return;
    s->depth = 0;
    s->screen_w = screen_w;
    s->screen_h = screen_h;
}

void cs_scissor_push(CsScissorStack *s, int x, int y, int w, int h)
{
    if (!s || s->depth >= CS_MAX_SCISSOR_DEPTH) return;

    /* Clamp to screen bounds */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->screen_w) w = s->screen_w - x;
    if (y + h > s->screen_h) h = s->screen_h - y;

    /* Intersect with parent scissor if any */
    if (s->depth > 0) {
        CsScissor *parent = &s->stack[s->depth - 1];
        if (x < parent->x) { w -= (parent->x - x); x = parent->x; }
        if (y < parent->y) { h -= (parent->y - y); y = parent->y; }
        if (x + w > parent->x + parent->w) w = parent->x + parent->w - x;
        if (y + h > parent->y + parent->h) h = parent->y + parent->h - y;
    }

    /* Store the clipped scissor */
    s->stack[s->depth].x = x;
    s->stack[s->depth].y = y;
    s->stack[s->depth].w = w > 0 ? w : 0;
    s->stack[s->depth].h = h > 0 ? h : 0;
    s->depth++;
}

void cs_scissor_pop(CsScissorStack *s)
{
    if (!s || s->depth == 0) return;
    s->depth--;
}

CsScissor cs_scissor_current(const CsScissorStack *s)
{
    if (!s || s->depth == 0) {
        /* Return full screen when stack is empty */
        return (CsScissor){0, 0, s ? s->screen_w : 0, s ? s->screen_h : 0};
    }
    return s->stack[s->depth - 1];
}

bool cs_scissor_test_point(const CsScissorStack *s, int x, int y)
{
    if (!s) return false;

    if (s->depth == 0) {
        /* No scissor - test against screen bounds */
        return x >= 0 && x < s->screen_w && y >= 0 && y < s->screen_h;
    }

    CsScissor *sc = (CsScissor *)&s->stack[s->depth - 1];
    return x >= sc->x && x < sc->x + sc->w && y >= sc->y && y < sc->y + sc->h;
}

bool cs_scissor_test_rect(const CsScissorStack *s, int x, int y, int w, int h)
{
    if (!s) return false;

    CsScissor sc;
    if (s->depth == 0) {
        sc = (CsScissor){0, 0, s->screen_w, s->screen_h};
    } else {
        sc = s->stack[s->depth - 1];
    }

    /* Check for overlap */
    return x < sc.x + sc.w && x + w > sc.x &&
           y < sc.y + sc.h && y + h > sc.y;
}

bool cs_scissor_clip_rect(const CsScissorStack *s, int *x, int *y, int *w, int *h)
{
    if (!s || !x || !y || !w || !h) return false;

    CsScissor sc;
    if (s->depth == 0) {
        sc = (CsScissor){0, 0, s->screen_w, s->screen_h};
    } else {
        sc = s->stack[s->depth - 1];
    }

    /* Clip left/top */
    if (*x < sc.x) {
        *w -= (sc.x - *x);
        *x = sc.x;
    }
    if (*y < sc.y) {
        *h -= (sc.y - *y);
        *y = sc.y;
    }

    /* Clip right/bottom */
    if (*x + *w > sc.x + sc.w) {
        *w = sc.x + sc.w - *x;
    }
    if (*y + *h > sc.y + sc.h) {
        *h = sc.y + sc.h - *y;
    }

    /* Check if anything remains */
    if (*w <= 0 || *h <= 0) {
        *w = 0;
        *h = 0;
        return false;
    }

    return true;
}

/* ============================================================================
 * Color Utilities
 * ============================================================================ */

uint32_t cs_blend_color(uint32_t src, uint32_t dst)
{
    uint8_t sr, sg, sb, sa;
    uint8_t dr, dg, db, da;

    cs_unpack_color(src, &sr, &sg, &sb, &sa);
    cs_unpack_color(dst, &dr, &dg, &db, &da);

    /* Fast path: fully opaque */
    if (sa == 255) return src;

    /* Fast path: fully transparent */
    if (sa == 0) return dst;

    /* Porter-Duff source-over */
    uint16_t inv_sa = 255 - sa;
    uint16_t out_a = sa + (da * inv_sa) / 255;

    if (out_a == 0) return 0;

    uint8_t out_r = (uint8_t)((sr * sa + dr * da * inv_sa / 255) / out_a);
    uint8_t out_g = (uint8_t)((sg * sa + dg * da * inv_sa / 255) / out_a);
    uint8_t out_b = (uint8_t)((sb * sa + db * da * inv_sa / 255) / out_a);

    return cs_pack_color(out_r, out_g, out_b, (uint8_t)out_a);
}

uint32_t cs_lerp_color(uint32_t a, uint32_t b, float t)
{
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;

    uint8_t ar, ag, ab, aa;
    uint8_t br, bg, bb, ba;

    cs_unpack_color(a, &ar, &ag, &ab, &aa);
    cs_unpack_color(b, &br, &bg, &bb, &ba);

    uint8_t r = (uint8_t)(ar + (br - ar) * t);
    uint8_t g = (uint8_t)(ag + (bg - ag) * t);
    uint8_t bl = (uint8_t)(ab + (bb - ab) * t);
    uint8_t al = (uint8_t)(aa + (ba - aa) * t);

    return cs_pack_color(r, g, bl, al);
}
