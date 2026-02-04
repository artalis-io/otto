/**
 * cs_clay.c - Generic Clay Integration Implementation
 *
 * Provides reusable Clay boilerplate so applications don't duplicate it.
 *
 * Thread Safety Note:
 * This module is NOT thread-safe by design. The Clay library uses internal
 * global state for layout computation, so multi-threaded UI rendering is not
 * supported. Each process should have a single Clay context, typically on
 * the main/UI thread. The CsClayState contains an 8MB internal memory buffer,
 * making per-thread copies impractical even if Clay supported it.
 *
 * For multi-threaded applications: perform all UI work on a dedicated thread.
 */

#include "cs_clay.h"
#include "cs_common.h"
#include "clay.h"
#include <string.h>
#include <stdio.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define CS_CLAY_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define CS_CLAY_EXPORT
#endif

/* ============================================================================
 * Internal State
 * ============================================================================ */

#define CS_CLAY_DEFAULT_MEMORY_SIZE (8 * 1024 * 1024)
#define CS_GLYPH_TABLE_SIZE 256  /* ASCII + extended */

typedef struct {
    uint8_t internal_memory[CS_CLAY_DEFAULT_MEMORY_SIZE];
    bool initialized;
    float char_width_ratio;
    char error_message[256];
    Clay_RenderCommandArray commands;

    /* Font metrics - glyph advances (normalized: 1.0 = em size) */
    float glyph_advances[CS_GLYPH_TABLE_SIZE];
    bool has_font_metrics;
} CsClayState;

static CsClayState g_clay = {
    .initialized = false,
    .char_width_ratio = 0.6f,
    .error_message = "",
    .has_font_metrics = false,
};

/* ============================================================================
 * Clay Callbacks
 * ============================================================================ */

static Clay_Dimensions cs_clay_measure_text(
    Clay_StringSlice text,
    Clay_TextElementConfig *config,
    void *userData
) {
    (void)userData;
    float font_size = config->fontSize;
    float width = 0;

    if (g_clay.has_font_metrics) {
        /* Use real glyph advances */
        for (int i = 0; i < text.length; i++) {
            int c = (unsigned char)text.chars[i];
            if (c < CS_GLYPH_TABLE_SIZE && g_clay.glyph_advances[c] > 0) {
                width += g_clay.glyph_advances[c] * font_size;
            } else {
                /* Fallback for unknown glyphs */
                width += g_clay.char_width_ratio * font_size;
            }
        }
    } else {
        /* Fallback: fixed ratio (monospace approximation) */
        width = text.length * g_clay.char_width_ratio * font_size;
    }

    return (Clay_Dimensions){
        .width = width,
        .height = font_size
    };
}

static Clay_Vector2 cs_clay_query_scroll(uint32_t elementId, void *userData) {
    (void)elementId;
    (void)userData;
    return (Clay_Vector2){0, 0};
}

static void cs_clay_handle_error(Clay_ErrorData error) {
    snprintf(g_clay.error_message, sizeof(g_clay.error_message),
             "%.*s", error.errorText.length, error.errorText.chars);
}

/* ============================================================================
 * Public API - Configuration
 * ============================================================================ */

CsClayConfig cs_clay_default_config(void) {
    return (CsClayConfig){
        .memory = NULL,
        .memory_size = CS_CLAY_DEFAULT_MEMORY_SIZE,
        .char_width_ratio = 0.6f,
    };
}

/* ============================================================================
 * Public API - Lifecycle
 * ============================================================================ */

CS_CLAY_EXPORT bool cs_clay_init(const CsClayConfig *config, int width, int height) {
    if (g_clay.initialized) {
        return true;  /* Already initialized */
    }

    /* Use provided memory or internal */
    uint8_t *memory = config->memory ? config->memory : g_clay.internal_memory;
    uint32_t mem_size = config->memory ? config->memory_size : CS_CLAY_DEFAULT_MEMORY_SIZE;

    /* Check Clay requirements */
    uint32_t min_mem = Clay_MinMemorySize();
    if (mem_size < min_mem) {
        snprintf(g_clay.error_message, sizeof(g_clay.error_message),
                 "Need %u bytes, have %u", min_mem, mem_size);
        return false;
    }

    g_clay.char_width_ratio = config->char_width_ratio > 0
        ? config->char_width_ratio : 0.6f;

    /* Initialize Clay */
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(mem_size, memory);

    Clay_Initialize(
        arena,
        (Clay_Dimensions){(float)width, (float)height},
        (Clay_ErrorHandler){cs_clay_handle_error, NULL}
    );

    Clay_SetMeasureTextFunction(cs_clay_measure_text, NULL);
    Clay_SetQueryScrollOffsetFunction(cs_clay_query_scroll, NULL);

    /* Initialize immediate mode components */
    cs_init();

    g_clay.initialized = true;
    g_clay.error_message[0] = '\0';

    return true;
}

CS_CLAY_EXPORT void cs_clay_resize(int width, int height) {
    if (g_clay.initialized) {
        Clay_SetLayoutDimensions((Clay_Dimensions){(float)width, (float)height});
    }
}

CS_CLAY_EXPORT void cs_clay_begin_frame(void) {
    if (!g_clay.initialized) return;
    cs_frame_begin();
    Clay_BeginLayout();
}

CS_CLAY_EXPORT int cs_clay_end_frame(float dt) {
    if (!g_clay.initialized) return -1;
    g_clay.commands = Clay_EndLayout();
    cs_frame_end(dt);
    return g_clay.commands.length;
}

CS_CLAY_EXPORT bool cs_clay_is_initialized(void) {
    return g_clay.initialized;
}

CS_CLAY_EXPORT const char *cs_clay_get_error(void) {
    return g_clay.error_message[0] ? g_clay.error_message : NULL;
}

/* ============================================================================
 * Public API - Render Command Accessors
 * ============================================================================ */

#define CMD_AT(i) g_clay.commands.internalArray[i]
#define CMD_VALID(i) ((i) >= 0 && (i) < g_clay.commands.length)

CS_CLAY_EXPORT int cs_clay_cmd_count(void) {
    return g_clay.commands.length;
}

CS_CLAY_EXPORT int cs_clay_cmd_type(int index) {
    return CMD_VALID(index) ? (int)CMD_AT(index).commandType : -1;
}

CS_CLAY_EXPORT float cs_clay_cmd_x(int index) {
    return CMD_VALID(index) ? CMD_AT(index).boundingBox.x : 0;
}

CS_CLAY_EXPORT float cs_clay_cmd_y(int index) {
    return CMD_VALID(index) ? CMD_AT(index).boundingBox.y : 0;
}

CS_CLAY_EXPORT float cs_clay_cmd_w(int index) {
    return CMD_VALID(index) ? CMD_AT(index).boundingBox.width : 0;
}

CS_CLAY_EXPORT float cs_clay_cmd_h(int index) {
    return CMD_VALID(index) ? CMD_AT(index).boundingBox.height : 0;
}

CS_CLAY_EXPORT uint32_t cs_clay_cmd_rect_color(int index) {
    if (!CMD_VALID(index)) return 0;
    Clay_Color c = CMD_AT(index).renderData.rectangle.backgroundColor;
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) |
           ((uint32_t)c.b << 8)  | (uint32_t)c.a;
}

CS_CLAY_EXPORT float cs_clay_cmd_rect_radius(int index) {
    return CMD_VALID(index)
        ? CMD_AT(index).renderData.rectangle.cornerRadius.topLeft : 0;
}

CS_CLAY_EXPORT const char *cs_clay_cmd_text_str(int index) {
    if (!CMD_VALID(index)) return "";
    const char *str = CMD_AT(index).renderData.text.stringContents.chars;
    return str ? str : "";
}

CS_CLAY_EXPORT int cs_clay_cmd_text_len(int index) {
    return CMD_VALID(index)
        ? CMD_AT(index).renderData.text.stringContents.length : 0;
}

CS_CLAY_EXPORT uint32_t cs_clay_cmd_text_color(int index) {
    if (!CMD_VALID(index)) return 0;
    Clay_Color c = CMD_AT(index).renderData.text.textColor;
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) |
           ((uint32_t)c.b << 8)  | (uint32_t)c.a;
}

CS_CLAY_EXPORT int cs_clay_cmd_text_size(int index) {
    return CMD_VALID(index)
        ? CMD_AT(index).renderData.text.fontSize : 0;
}

CS_CLAY_EXPORT uint32_t cs_clay_cmd_border_color(int index) {
    if (!CMD_VALID(index)) return 0;
    Clay_Color c = CMD_AT(index).renderData.border.color;
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) |
           ((uint32_t)c.b << 8)  | (uint32_t)c.a;
}

CS_CLAY_EXPORT float cs_clay_cmd_border_radius(int index) {
    return CMD_VALID(index)
        ? CMD_AT(index).renderData.border.cornerRadius.topLeft : 0;
}

CS_CLAY_EXPORT int cs_clay_cmd_border_width(int index) {
    return CMD_VALID(index)
        ? CMD_AT(index).renderData.border.width.left : 0;
}

#undef CMD_AT
#undef CMD_VALID

/* ============================================================================
 * Public API - Pointer/Input Helpers
 * ============================================================================ */

CS_CLAY_EXPORT void cs_clay_set_pointer(float x, float y, bool is_down) {
    Clay_SetPointerState((Clay_Vector2){x, y}, is_down);
    /* Also store in component state for click-to-position and drag tracking */
    cs_set_pointer(x, y);
    cs_set_pointer_down(is_down);
}

CS_CLAY_EXPORT bool cs_clay_pointer_over(const char *element_id) {
    if (!element_id) return false;

    /* Use Clay's hash function to match CLAY_ID() */
    Clay_String str = { .chars = element_id, .length = (int)strlen(element_id) };
    Clay_ElementId id = Clay__HashString(str, 0);
    return Clay_PointerOver(id);
}

/* ============================================================================
 * Font Metrics
 * ============================================================================ */

CS_CLAY_EXPORT void cs_clay_set_glyph_advance(int unicode, float advance) {
    if (unicode >= 0 && unicode < CS_GLYPH_TABLE_SIZE) {
        g_clay.glyph_advances[unicode] = advance;
        g_clay.has_font_metrics = true;
    }
}

CS_CLAY_EXPORT void cs_clay_set_glyph_advances(const float *advances, int count) {
    if (!advances || count <= 0) return;

    int limit = count < CS_GLYPH_TABLE_SIZE ? count : CS_GLYPH_TABLE_SIZE;
    for (int i = 0; i < limit; i++) {
        g_clay.glyph_advances[i] = advances[i];
    }
    g_clay.has_font_metrics = true;
}

CS_CLAY_EXPORT bool cs_clay_has_font_metrics(void) {
    return g_clay.has_font_metrics;
}

/**
 * Calculate cursor position from X offset within text.
 * Returns the character index closest to the given X position.
 */
CS_CLAY_EXPORT int cs_clay_x_to_cursor(const char *text, int len, float x_offset, float font_size) {
    if (!text || len <= 0 || x_offset <= 0) return 0;

    float x = 0;
    for (int i = 0; i < len; i++) {
        int c = (unsigned char)text[i];
        float advance;
        if (g_clay.has_font_metrics && c < CS_GLYPH_TABLE_SIZE && g_clay.glyph_advances[c] > 0) {
            advance = g_clay.glyph_advances[c] * font_size;
        } else {
            advance = g_clay.char_width_ratio * font_size;
        }

        /* Check if click is closer to this char or the next */
        if (x + advance / 2 > x_offset) {
            return i;
        }
        x += advance;
    }

    return len;  /* Click was past end of text */
}
