/**
 * cc_clay.c - Generic Clay Integration Implementation
 *
 * Provides reusable Clay boilerplate so applications don't duplicate it.
 */

#include "cc_clay.h"
#include "cc_common.h"
#include "clay.h"
#include <string.h>
#include <stdio.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define CC_CLAY_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define CC_CLAY_EXPORT
#endif

/* ============================================================================
 * Internal State
 * ============================================================================ */

#define CC_CLAY_DEFAULT_MEMORY_SIZE (8 * 1024 * 1024)
#define CC_GLYPH_TABLE_SIZE 256  /* ASCII + extended */

typedef struct {
    uint8_t internal_memory[CC_CLAY_DEFAULT_MEMORY_SIZE];
    bool initialized;
    float char_width_ratio;
    char error_message[256];
    Clay_RenderCommandArray commands;

    /* Font metrics - glyph advances (normalized: 1.0 = em size) */
    float glyph_advances[CC_GLYPH_TABLE_SIZE];
    bool has_font_metrics;
} CcClayState;

static CcClayState g_clay = {
    .initialized = false,
    .char_width_ratio = 0.6f,
    .error_message = "",
    .has_font_metrics = false,
};

/* ============================================================================
 * Clay Callbacks
 * ============================================================================ */

static Clay_Dimensions cc_clay_measure_text(
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
            if (c < CC_GLYPH_TABLE_SIZE && g_clay.glyph_advances[c] > 0) {
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

static Clay_Vector2 cc_clay_query_scroll(uint32_t elementId, void *userData) {
    (void)elementId;
    (void)userData;
    return (Clay_Vector2){0, 0};
}

static void cc_clay_handle_error(Clay_ErrorData error) {
    snprintf(g_clay.error_message, sizeof(g_clay.error_message),
             "%.*s", error.errorText.length, error.errorText.chars);
}

/* ============================================================================
 * Public API - Configuration
 * ============================================================================ */

CcClayConfig cc_clay_default_config(void) {
    return (CcClayConfig){
        .memory = NULL,
        .memory_size = CC_CLAY_DEFAULT_MEMORY_SIZE,
        .char_width_ratio = 0.6f,
    };
}

/* ============================================================================
 * Public API - Lifecycle
 * ============================================================================ */

CC_CLAY_EXPORT bool cc_clay_init(const CcClayConfig *config, int width, int height) {
    if (g_clay.initialized) {
        return true;  /* Already initialized */
    }

    /* Use provided memory or internal */
    uint8_t *memory = config->memory ? config->memory : g_clay.internal_memory;
    uint32_t mem_size = config->memory ? config->memory_size : CC_CLAY_DEFAULT_MEMORY_SIZE;

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
        (Clay_ErrorHandler){cc_clay_handle_error, NULL}
    );

    Clay_SetMeasureTextFunction(cc_clay_measure_text, NULL);
    Clay_SetQueryScrollOffsetFunction(cc_clay_query_scroll, NULL);

    /* Initialize immediate mode components */
    cc_init();

    g_clay.initialized = true;
    g_clay.error_message[0] = '\0';

    return true;
}

CC_CLAY_EXPORT void cc_clay_resize(int width, int height) {
    if (g_clay.initialized) {
        Clay_SetLayoutDimensions((Clay_Dimensions){(float)width, (float)height});
    }
}

CC_CLAY_EXPORT void cc_clay_begin_frame(void) {
    if (!g_clay.initialized) return;
    cc_frame_begin();
    Clay_BeginLayout();
}

CC_CLAY_EXPORT int cc_clay_end_frame(float dt) {
    if (!g_clay.initialized) return -1;
    g_clay.commands = Clay_EndLayout();
    cc_frame_end(dt);
    return g_clay.commands.length;
}

CC_CLAY_EXPORT bool cc_clay_is_initialized(void) {
    return g_clay.initialized;
}

CC_CLAY_EXPORT const char *cc_clay_get_error(void) {
    return g_clay.error_message[0] ? g_clay.error_message : NULL;
}

/* ============================================================================
 * Public API - Render Command Accessors
 * ============================================================================ */

#define CMD_AT(i) g_clay.commands.internalArray[i]
#define CMD_VALID(i) ((i) >= 0 && (i) < g_clay.commands.length)

CC_CLAY_EXPORT int cc_clay_cmd_count(void) {
    return g_clay.commands.length;
}

CC_CLAY_EXPORT int cc_clay_cmd_type(int index) {
    return CMD_VALID(index) ? (int)CMD_AT(index).commandType : -1;
}

CC_CLAY_EXPORT float cc_clay_cmd_x(int index) {
    return CMD_VALID(index) ? CMD_AT(index).boundingBox.x : 0;
}

CC_CLAY_EXPORT float cc_clay_cmd_y(int index) {
    return CMD_VALID(index) ? CMD_AT(index).boundingBox.y : 0;
}

CC_CLAY_EXPORT float cc_clay_cmd_w(int index) {
    return CMD_VALID(index) ? CMD_AT(index).boundingBox.width : 0;
}

CC_CLAY_EXPORT float cc_clay_cmd_h(int index) {
    return CMD_VALID(index) ? CMD_AT(index).boundingBox.height : 0;
}

CC_CLAY_EXPORT uint32_t cc_clay_cmd_rect_color(int index) {
    if (!CMD_VALID(index)) return 0;
    Clay_Color c = CMD_AT(index).renderData.rectangle.backgroundColor;
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) |
           ((uint32_t)c.b << 8)  | (uint32_t)c.a;
}

CC_CLAY_EXPORT float cc_clay_cmd_rect_radius(int index) {
    return CMD_VALID(index)
        ? CMD_AT(index).renderData.rectangle.cornerRadius.topLeft : 0;
}

CC_CLAY_EXPORT const char *cc_clay_cmd_text_str(int index) {
    return CMD_VALID(index)
        ? CMD_AT(index).renderData.text.stringContents.chars : "";
}

CC_CLAY_EXPORT int cc_clay_cmd_text_len(int index) {
    return CMD_VALID(index)
        ? CMD_AT(index).renderData.text.stringContents.length : 0;
}

CC_CLAY_EXPORT uint32_t cc_clay_cmd_text_color(int index) {
    if (!CMD_VALID(index)) return 0;
    Clay_Color c = CMD_AT(index).renderData.text.textColor;
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) |
           ((uint32_t)c.b << 8)  | (uint32_t)c.a;
}

CC_CLAY_EXPORT int cc_clay_cmd_text_size(int index) {
    return CMD_VALID(index)
        ? CMD_AT(index).renderData.text.fontSize : 0;
}

CC_CLAY_EXPORT uint32_t cc_clay_cmd_border_color(int index) {
    if (!CMD_VALID(index)) return 0;
    Clay_Color c = CMD_AT(index).renderData.border.color;
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) |
           ((uint32_t)c.b << 8)  | (uint32_t)c.a;
}

CC_CLAY_EXPORT float cc_clay_cmd_border_radius(int index) {
    return CMD_VALID(index)
        ? CMD_AT(index).renderData.border.cornerRadius.topLeft : 0;
}

CC_CLAY_EXPORT int cc_clay_cmd_border_width(int index) {
    return CMD_VALID(index)
        ? CMD_AT(index).renderData.border.width.left : 0;
}

#undef CMD_AT
#undef CMD_VALID

/* ============================================================================
 * Public API - Pointer/Input Helpers
 * ============================================================================ */

CC_CLAY_EXPORT void cc_clay_set_pointer(float x, float y, bool is_down) {
    Clay_SetPointerState((Clay_Vector2){x, y}, is_down);
    /* Also store in component state for click-to-position */
    cc_set_pointer(x, y);
}

CC_CLAY_EXPORT bool cc_clay_pointer_over(const char *element_id) {
    /* Use consistent FNV-1a hash matching cc_hash_id() / CC_ID() */
    uint32_t hash = cc_hash_id(element_id);
    Clay_ElementId id = { .id = hash, .stringId = { .chars = element_id, .length = (int)strlen(element_id) } };
    return Clay_PointerOver(id);
}

/* ============================================================================
 * Font Metrics
 * ============================================================================ */

CC_CLAY_EXPORT void cc_clay_set_glyph_advance(int unicode, float advance) {
    if (unicode >= 0 && unicode < CC_GLYPH_TABLE_SIZE) {
        g_clay.glyph_advances[unicode] = advance;
        g_clay.has_font_metrics = true;
    }
}

CC_CLAY_EXPORT void cc_clay_set_glyph_advances(const float *advances, int count) {
    if (!advances || count <= 0) return;

    int limit = count < CC_GLYPH_TABLE_SIZE ? count : CC_GLYPH_TABLE_SIZE;
    for (int i = 0; i < limit; i++) {
        g_clay.glyph_advances[i] = advances[i];
    }
    g_clay.has_font_metrics = true;
}

CC_CLAY_EXPORT bool cc_clay_has_font_metrics(void) {
    return g_clay.has_font_metrics;
}

/**
 * Calculate cursor position from X offset within text.
 * Returns the character index closest to the given X position.
 */
CC_CLAY_EXPORT int cc_clay_x_to_cursor(const char *text, int len, float x_offset, float font_size) {
    if (!text || len <= 0 || x_offset <= 0) return 0;

    float x = 0;
    for (int i = 0; i < len; i++) {
        int c = (unsigned char)text[i];
        float advance;
        if (g_clay.has_font_metrics && c < CC_GLYPH_TABLE_SIZE && g_clay.glyph_advances[c] > 0) {
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
