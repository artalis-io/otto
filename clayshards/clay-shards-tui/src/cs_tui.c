/**
 * cs_tui.c - ClayShards TUI Renderer Implementation
 *
 * Processes Clay render commands and outputs ANSI escape sequences.
 */

#include "cs_tui_internal.h"
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>

/* Include Clay for command types */
#include "clay.h"

/* ============================================================================
 * Box Drawing Characters
 * ============================================================================ */

/* ASCII: +--+, | */
static const char *BOX_ASCII[] = {
    "-", "|", "+", "+", "+", "+", "+", "+", "+", "+", "+"
};

/* Light: ─│┌┐└┘├┤┬┴┼ */
static const char *BOX_LIGHT[] = {
    "─", "│", "┌", "┐", "└", "┘", "├", "┤", "┬", "┴", "┼"
};

/* Heavy: ━┃┏┓┗┛┣┫┳┻╋ */
static const char *BOX_HEAVY[] = {
    "━", "┃", "┏", "┓", "┗", "┛", "┣", "┫", "┳", "┻", "╋"
};

/* Double: ═║╔╗╚╝╠╣╦╩╬ */
static const char *BOX_DOUBLE[] = {
    "═", "║", "╔", "╗", "╚", "╝", "╠", "╣", "╦", "╩", "╬"
};

/* Rounded: ─│╭╮╰╯├┤┬┴┼ */
static const char *BOX_ROUNDED[] = {
    "─", "│", "╭", "╮", "╰", "╯", "├", "┤", "┬", "┴", "┼"
};

const char *cs_tui_box_char(CsTuiBoxStyle style, int part) {
    if (part < 0 || part > CS_TUI_BOX_CROSS) return " ";

    switch (style) {
        case CS_TUI_BOX_ASCII:   return BOX_ASCII[part];
        case CS_TUI_BOX_LIGHT:   return BOX_LIGHT[part];
        case CS_TUI_BOX_HEAVY:   return BOX_HEAVY[part];
        case CS_TUI_BOX_DOUBLE:  return BOX_DOUBLE[part];
        case CS_TUI_BOX_ROUNDED: return BOX_ROUNDED[part];
        default:                 return BOX_LIGHT[part];
    }
}

/* ============================================================================
 * Configuration
 * ============================================================================ */

void cs_tui_config_init(CsTuiConfig *config) {
    if (!config) return;

    config->width = 0;              /* Auto-detect */
    config->height = 0;             /* Auto-detect */
    config->color_mode = CS_TUI_COLOR_AUTO;
    config->box_style = CS_TUI_BOX_LIGHT;
    config->alternate_screen = false;
    config->hide_cursor = true;
    config->differential = true;
}

bool cs_tui_is_tty(void) {
    return isatty(STDOUT_FILENO) != 0;
}

CsTuiColorMode cs_tui_detect_color_mode(void) {
    /* Check COLORTERM for true color support */
    const char *colorterm = getenv("COLORTERM");
    if (colorterm) {
        if (strcmp(colorterm, "truecolor") == 0 ||
            strcmp(colorterm, "24bit") == 0) {
            return CS_TUI_COLOR_TRUE;
        }
    }

    /* Check TERM for common true-color terminals */
    const char *term = getenv("TERM");
    if (term) {
        if (strstr(term, "256color") != NULL ||
            strstr(term, "truecolor") != NULL ||
            strstr(term, "alacritty") != NULL ||
            strstr(term, "kitty") != NULL ||
            strstr(term, "iterm") != NULL) {
            return CS_TUI_COLOR_TRUE;
        }
    }

    /* Default fallback: 256-color (widely supported) */
    return CS_TUI_COLOR_256;
}

/* ============================================================================
 * Output Buffer
 * ============================================================================ */

void cs_tui_output_init(CsTuiRenderer *r) {
    r->output_buf = malloc(CS_TUI_OUTPUT_INITIAL_CAP);
    r->output_len = 0;
    r->output_cap = r->output_buf ? CS_TUI_OUTPUT_INITIAL_CAP : 0;
}

void cs_tui_output_free(CsTuiRenderer *r) {
    free(r->output_buf);
    r->output_buf = NULL;
    r->output_len = 0;
    r->output_cap = 0;
}

void cs_tui_output_append(CsTuiRenderer *r, const char *str, size_t len) {
    if (!r->output_buf || len == 0) return;

    /* Grow buffer if needed */
    if (r->output_len + len > r->output_cap) {
        size_t new_cap = r->output_cap * 2;
        if (new_cap < r->output_len + len) {
            new_cap = r->output_len + len + CS_TUI_OUTPUT_INITIAL_CAP;
        }
        char *new_buf = realloc(r->output_buf, new_cap);
        if (!new_buf) return;  /* Keep existing data on failure */
        r->output_buf = new_buf;
        r->output_cap = new_cap;
    }

    memcpy(r->output_buf + r->output_len, str, len);
    r->output_len += len;
}

void cs_tui_output_printf(CsTuiRenderer *r, const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len > 0 && (size_t)len < sizeof(buf)) {
        cs_tui_output_append(r, buf, (size_t)len);
    }
}

void cs_tui_output_flush(CsTuiRenderer *r) {
    if (r->output_len > 0 && r->output_buf) {
        (void)write(STDOUT_FILENO, r->output_buf, r->output_len);
        r->output_len = 0;
    }
}

/* ============================================================================
 * Color Conversion
 * ============================================================================ */

int cs_tui_rgb_to_256(uint8_t r, uint8_t g, uint8_t b) {
    /* Check for grayscale */
    if (r == g && g == b) {
        if (r < 8) return 16;       /* Black */
        if (r > 248) return 231;    /* White */
        return 232 + (r - 8) / 10;  /* Grayscale ramp 232-255 */
    }

    /* 6x6x6 color cube */
    int ri = (r * 5) / 255;
    int gi = (g * 5) / 255;
    int bi = (b * 5) / 255;
    return 16 + 36 * ri + 6 * gi + bi;
}

void cs_tui_emit_fg_color(CsTuiRenderer *r, uint8_t red, uint8_t g, uint8_t b) {
    switch (r->effective_color_mode) {
        case CS_TUI_COLOR_TRUE:
            cs_tui_output_printf(r, CS_TUI_FG_TRUE, red, g, b);
            break;
        case CS_TUI_COLOR_256:
            cs_tui_output_printf(r, CS_TUI_FG_256, cs_tui_rgb_to_256(red, g, b));
            break;
        case CS_TUI_COLOR_16:
        default:
            /* Basic 16 colors - approximate */
            {
                int idx = 0;
                if (red > 128) idx |= 1;
                if (g > 128) idx |= 2;
                if (b > 128) idx |= 4;
                cs_tui_output_printf(r, "\x1b[%dm", 30 + idx);
            }
            break;
    }
}

void cs_tui_emit_bg_color(CsTuiRenderer *r, uint8_t red, uint8_t g, uint8_t b) {
    switch (r->effective_color_mode) {
        case CS_TUI_COLOR_TRUE:
            cs_tui_output_printf(r, CS_TUI_BG_TRUE, red, g, b);
            break;
        case CS_TUI_COLOR_256:
            cs_tui_output_printf(r, CS_TUI_BG_256, cs_tui_rgb_to_256(red, g, b));
            break;
        case CS_TUI_COLOR_16:
        default:
            {
                int idx = 0;
                if (red > 128) idx |= 1;
                if (g > 128) idx |= 2;
                if (b > 128) idx |= 4;
                cs_tui_output_printf(r, "\x1b[%dm", 40 + idx);
            }
            break;
    }
}

/* ============================================================================
 * UTF-8 Helpers
 * ============================================================================ */

int cs_tui_utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp < 0x110000) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

int cs_tui_utf8_decode(const char *str, int len, uint32_t *out) {
    if (len <= 0 || !str) return 0;

    uint8_t b0 = (uint8_t)str[0];
    if (b0 < 0x80) {
        *out = b0;
        return 1;
    } else if ((b0 & 0xE0) == 0xC0 && len >= 2) {
        *out = ((b0 & 0x1F) << 6) | (str[1] & 0x3F);
        return 2;
    } else if ((b0 & 0xF0) == 0xE0 && len >= 3) {
        *out = ((b0 & 0x0F) << 12) | ((str[1] & 0x3F) << 6) | (str[2] & 0x3F);
        return 3;
    } else if ((b0 & 0xF8) == 0xF0 && len >= 4) {
        *out = ((b0 & 0x07) << 18) | ((str[1] & 0x3F) << 12) |
               ((str[2] & 0x3F) << 6) | (str[3] & 0x3F);
        return 4;
    }
    *out = '?';
    return 1;
}

/* ============================================================================
 * Clipping
 * ============================================================================ */

bool cs_tui_clip_point(CsTuiRenderer *r, int x, int y) {
    if (r->scissor_depth == 0) {
        return x >= 0 && x < r->buffer.width && y >= 0 && y < r->buffer.height;
    }

    CsTuiScissor *s = &r->scissor_stack[r->scissor_depth - 1];
    return x >= s->x && x < s->x + s->w && y >= s->y && y < s->y + s->h;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

static bool cs_tui_buffer_init(CsTuiBuffer *buf, int width, int height) {
    size_t count = (size_t)width * (size_t)height;
    if (count == 0 || count > SIZE_MAX / sizeof(CsTuiCell)) {
        return false;
    }

    buf->front = calloc(count, sizeof(CsTuiCell));
    buf->back = calloc(count, sizeof(CsTuiCell));
    if (!buf->front || !buf->back) {
        free(buf->front);
        free(buf->back);
        buf->front = NULL;
        buf->back = NULL;
        return false;
    }

    buf->width = width;
    buf->height = height;

    /* Initialize cells with spaces and default colors */
    for (size_t i = 0; i < count; i++) {
        buf->front[i].codepoint = ' ';
        buf->front[i].fg_r = 255;
        buf->front[i].fg_g = 255;
        buf->front[i].fg_b = 255;
        buf->back[i] = buf->front[i];
    }

    return true;
}

static void cs_tui_buffer_free(CsTuiBuffer *buf) {
    free(buf->front);
    free(buf->back);
    buf->front = NULL;
    buf->back = NULL;
    buf->width = 0;
    buf->height = 0;
}

static void cs_tui_get_terminal_size(int *width, int *height) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        *width = ws.ws_col;
        *height = ws.ws_row;
    } else {
        *width = 80;
        *height = 24;
    }
}

CsTuiRenderer *cs_tui_create(const CsTuiConfig *config) {
    CsTuiRenderer *r = calloc(1, sizeof(CsTuiRenderer));
    if (!r) return NULL;

    /* Apply config */
    if (config) {
        r->config = *config;
    } else {
        cs_tui_config_init(&r->config);
    }

    /* Resolve auto size */
    int width = r->config.width;
    int height = r->config.height;
    if (width == 0 || height == 0) {
        cs_tui_get_terminal_size(&width, &height);
    }

    /* Resolve auto color mode */
    r->effective_color_mode = r->config.color_mode;
    if (r->effective_color_mode == CS_TUI_COLOR_AUTO) {
        r->effective_color_mode = cs_tui_detect_color_mode();
    }

    /* Initialize buffer */
    if (!cs_tui_buffer_init(&r->buffer, width, height)) {
        free(r);
        return NULL;
    }

    /* Initialize output buffer */
    cs_tui_output_init(r);
    if (!r->output_buf) {
        cs_tui_buffer_free(&r->buffer);
        free(r);
        return NULL;
    }

    r->needs_full_redraw = true;

    /* Setup terminal (skip in headless mode) */
    if (!r->config.headless) {
        if (r->config.alternate_screen) {
            cs_tui_output_append(r, CS_TUI_ALT_SCREEN_ON, strlen(CS_TUI_ALT_SCREEN_ON));
            r->alternate_screen_active = true;
        }

        if (r->config.hide_cursor) {
            cs_tui_output_append(r, CS_TUI_CURSOR_HIDE, strlen(CS_TUI_CURSOR_HIDE));
            r->cursor_hidden = true;
        }

        cs_tui_output_flush(r);
    }

    return r;
}

void cs_tui_free(CsTuiRenderer *r) {
    if (!r) return;

    /* Restore terminal (skip in headless mode) */
    if (!r->config.headless) {
        if (r->cursor_hidden) {
            cs_tui_output_append(r, CS_TUI_CURSOR_SHOW, strlen(CS_TUI_CURSOR_SHOW));
        }

        if (r->alternate_screen_active) {
            cs_tui_output_append(r, CS_TUI_ALT_SCREEN_OFF, strlen(CS_TUI_ALT_SCREEN_OFF));
        }

        cs_tui_output_append(r, CS_TUI_RESET, strlen(CS_TUI_RESET));
        cs_tui_output_flush(r);
    }

    cs_tui_buffer_free(&r->buffer);
    cs_tui_output_free(r);
    free(r);
}

/* ============================================================================
 * Size Management
 * ============================================================================ */

void cs_tui_resize(CsTuiRenderer *r, int width, int height) {
    if (!r) return;

    if (width == 0 || height == 0) {
        cs_tui_get_terminal_size(&width, &height);
    }

    if (width == r->buffer.width && height == r->buffer.height) {
        return;
    }

    cs_tui_buffer_free(&r->buffer);
    cs_tui_buffer_init(&r->buffer, width, height);
    r->needs_full_redraw = true;
}

void cs_tui_get_size(CsTuiRenderer *r, int *width, int *height) {
    if (!r) {
        if (width) *width = 0;
        if (height) *height = 0;
        return;
    }
    if (width) *width = r->buffer.width;
    if (height) *height = r->buffer.height;
}

/* ============================================================================
 * Rendering
 * ============================================================================ */

void cs_tui_begin(CsTuiRenderer *r) {
    if (!r) return;

    /* Clear back buffer */
    size_t count = (size_t)r->buffer.width * (size_t)r->buffer.height;
    for (size_t i = 0; i < count; i++) {
        r->buffer.back[i].codepoint = ' ';
        r->buffer.back[i].fg_r = 255;
        r->buffer.back[i].fg_g = 255;
        r->buffer.back[i].fg_b = 255;
        r->buffer.back[i].bg_r = 0;
        r->buffer.back[i].bg_g = 0;
        r->buffer.back[i].bg_b = 0;
        r->buffer.back[i].flags = 0;
        r->buffer.back[i].z_index = INT16_MIN;  /* Reset z-index */
    }

    /* Reset scissor and z-index */
    r->scissor_depth = 0;
    r->current_z_index = 0;
}

void cs_tui_clear(CsTuiRenderer *r, CsTuiColor color) {
    if (!r) return;

    int16_t z = r->current_z_index;
    size_t count = (size_t)r->buffer.width * (size_t)r->buffer.height;
    for (size_t i = 0; i < count; i++) {
        if (z >= r->buffer.back[i].z_index) {
            r->buffer.back[i].codepoint = ' ';
            r->buffer.back[i].bg_r = color.r;
            r->buffer.back[i].bg_g = color.g;
            r->buffer.back[i].bg_b = color.b;
            r->buffer.back[i].z_index = z;
        }
    }
}

void cs_tui_rect(CsTuiRenderer *r, int x, int y, int w, int h,
                 CsTuiColor bg, const CsTuiColor *border_color, int corner_radius) {
    if (!r || w <= 0 || h <= 0) return;

    CsTuiBoxStyle style = corner_radius > 0 ? CS_TUI_BOX_ROUNDED : r->config.box_style;
    int16_t z = r->current_z_index;

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int cx = x + col;
            int cy = y + row;

            if (!cs_tui_clip_point(r, cx, cy)) continue;

            CsTuiCell *cell = cs_tui_cell_at(&r->buffer, cx, cy);
            if (!cell) continue;

            /* Z-index check: only write if current z >= cell's z */
            if (z < cell->z_index) continue;

            /* Set background and z-index */
            cell->bg_r = bg.r;
            cell->bg_g = bg.g;
            cell->bg_b = bg.b;
            cell->z_index = z;

            /* Handle borders */
            if (border_color) {
                bool is_top = (row == 0);
                bool is_bottom = (row == h - 1);
                bool is_left = (col == 0);
                bool is_right = (col == w - 1);

                if (is_top && is_left) {
                    const char *ch = cs_tui_box_char(style, CS_TUI_BOX_TOP_LEFT);
                    uint32_t cp;
                    cs_tui_utf8_decode(ch, 4, &cp);
                    cell->codepoint = cp;
                    cell->fg_r = border_color->r;
                    cell->fg_g = border_color->g;
                    cell->fg_b = border_color->b;
                } else if (is_top && is_right) {
                    const char *ch = cs_tui_box_char(style, CS_TUI_BOX_TOP_RIGHT);
                    uint32_t cp;
                    cs_tui_utf8_decode(ch, 4, &cp);
                    cell->codepoint = cp;
                    cell->fg_r = border_color->r;
                    cell->fg_g = border_color->g;
                    cell->fg_b = border_color->b;
                } else if (is_bottom && is_left) {
                    const char *ch = cs_tui_box_char(style, CS_TUI_BOX_BOTTOM_LEFT);
                    uint32_t cp;
                    cs_tui_utf8_decode(ch, 4, &cp);
                    cell->codepoint = cp;
                    cell->fg_r = border_color->r;
                    cell->fg_g = border_color->g;
                    cell->fg_b = border_color->b;
                } else if (is_bottom && is_right) {
                    const char *ch = cs_tui_box_char(style, CS_TUI_BOX_BOTTOM_RIGHT);
                    uint32_t cp;
                    cs_tui_utf8_decode(ch, 4, &cp);
                    cell->codepoint = cp;
                    cell->fg_r = border_color->r;
                    cell->fg_g = border_color->g;
                    cell->fg_b = border_color->b;
                } else if (is_top || is_bottom) {
                    const char *ch = cs_tui_box_char(style, CS_TUI_BOX_HORIZ);
                    uint32_t cp;
                    cs_tui_utf8_decode(ch, 4, &cp);
                    cell->codepoint = cp;
                    cell->fg_r = border_color->r;
                    cell->fg_g = border_color->g;
                    cell->fg_b = border_color->b;
                } else if (is_left || is_right) {
                    const char *ch = cs_tui_box_char(style, CS_TUI_BOX_VERT);
                    uint32_t cp;
                    cs_tui_utf8_decode(ch, 4, &cp);
                    cell->codepoint = cp;
                    cell->fg_r = border_color->r;
                    cell->fg_g = border_color->g;
                    cell->fg_b = border_color->b;
                } else {
                    cell->codepoint = ' ';
                }
            } else {
                cell->codepoint = ' ';
            }
        }
    }
}

void cs_tui_text(CsTuiRenderer *r, int x, int y, const char *text, int len,
                 CsTuiColor fg, const CsTuiColor *bg) {
    if (!r || !text) return;

    if (len < 0) {
        len = (int)strlen(text);
    }

    int16_t z = r->current_z_index;
    int col = x;
    int i = 0;
    while (i < len) {
        uint32_t cp;
        int bytes = cs_tui_utf8_decode(text + i, len - i, &cp);
        if (bytes == 0) break;
        i += bytes;

        if (!cs_tui_clip_point(r, col, y)) {
            col++;
            continue;
        }

        CsTuiCell *cell = cs_tui_cell_at(&r->buffer, col, y);
        if (cell && z >= cell->z_index) {
            cell->codepoint = cp;
            cell->fg_r = fg.r;
            cell->fg_g = fg.g;
            cell->fg_b = fg.b;
            cell->z_index = z;
            if (bg) {
                cell->bg_r = bg->r;
                cell->bg_g = bg->g;
                cell->bg_b = bg->b;
            }
        }
        col++;
    }
}

void cs_tui_border(CsTuiRenderer *r, int x, int y, int w, int h,
                   CsTuiColor color, int corner_radius) {
    if (!r || w <= 0 || h <= 0) return;

    /* Skip single-row borders - they would overlap with content */
    if (h == 1) return;

    CsTuiBoxStyle style = corner_radius > 0 ? CS_TUI_BOX_ROUNDED : r->config.box_style;
    int16_t z = r->current_z_index;

    /* Helper to check if cell has text content (not space/empty) at same or higher z */
    #define HAS_CONTENT(cell) ((cell)->codepoint != 0 && (cell)->codepoint != ' ')
    #define CAN_WRITE(cell) ((cell) && z >= (cell)->z_index && !HAS_CONTENT(cell))

    /* Top edge */
    for (int col = 0; col < w; col++) {
        int cx = x + col;
        if (!cs_tui_clip_point(r, cx, y)) continue;
        CsTuiCell *cell = cs_tui_cell_at(&r->buffer, cx, y);
        if (!CAN_WRITE(cell)) continue;

        const char *ch;
        if (col == 0) {
            ch = cs_tui_box_char(style, CS_TUI_BOX_TOP_LEFT);
        } else if (col == w - 1) {
            ch = cs_tui_box_char(style, CS_TUI_BOX_TOP_RIGHT);
        } else {
            ch = cs_tui_box_char(style, CS_TUI_BOX_HORIZ);
        }
        uint32_t cp;
        cs_tui_utf8_decode(ch, 4, &cp);
        cell->codepoint = cp;
        cell->fg_r = color.r;
        cell->fg_g = color.g;
        cell->fg_b = color.b;
        cell->z_index = z;
    }

    /* Bottom edge */
    for (int col = 0; col < w; col++) {
        int cx = x + col;
        int cy = y + h - 1;
        if (!cs_tui_clip_point(r, cx, cy)) continue;
        CsTuiCell *cell = cs_tui_cell_at(&r->buffer, cx, cy);
        if (!CAN_WRITE(cell)) continue;

        const char *ch;
        if (col == 0) {
            ch = cs_tui_box_char(style, CS_TUI_BOX_BOTTOM_LEFT);
        } else if (col == w - 1) {
            ch = cs_tui_box_char(style, CS_TUI_BOX_BOTTOM_RIGHT);
        } else {
            ch = cs_tui_box_char(style, CS_TUI_BOX_HORIZ);
        }
        uint32_t cp;
        cs_tui_utf8_decode(ch, 4, &cp);
        cell->codepoint = cp;
        cell->fg_r = color.r;
        cell->fg_g = color.g;
        cell->fg_b = color.b;
        cell->z_index = z;
    }

    /* Left and right edges */
    for (int row = 1; row < h - 1; row++) {
        int cy = y + row;

        /* Left */
        if (cs_tui_clip_point(r, x, cy)) {
            CsTuiCell *cell = cs_tui_cell_at(&r->buffer, x, cy);
            if (CAN_WRITE(cell)) {
                const char *ch = cs_tui_box_char(style, CS_TUI_BOX_VERT);
                uint32_t cp;
                cs_tui_utf8_decode(ch, 4, &cp);
                cell->codepoint = cp;
                cell->fg_r = color.r;
                cell->fg_g = color.g;
                cell->fg_b = color.b;
                cell->z_index = z;
            }
        }

        /* Right */
        int rx = x + w - 1;
        if (cs_tui_clip_point(r, rx, cy)) {
            CsTuiCell *cell = cs_tui_cell_at(&r->buffer, rx, cy);
            if (CAN_WRITE(cell)) {
                const char *ch = cs_tui_box_char(style, CS_TUI_BOX_VERT);
                uint32_t cp;
                cs_tui_utf8_decode(ch, 4, &cp);
                cell->codepoint = cp;
                cell->fg_r = color.r;
                cell->fg_g = color.g;
                cell->fg_b = color.b;
                cell->z_index = z;
            }
        }
    }

    #undef HAS_CONTENT
    #undef CAN_WRITE
}

void cs_tui_scissor_push(CsTuiRenderer *r, int x, int y, int w, int h) {
    if (!r || r->scissor_depth >= CS_TUI_MAX_SCISSOR_DEPTH) return;

    /* Clamp to buffer bounds and parent scissor */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > r->buffer.width) w = r->buffer.width - x;
    if (y + h > r->buffer.height) h = r->buffer.height - y;

    if (r->scissor_depth > 0) {
        CsTuiScissor *parent = &r->scissor_stack[r->scissor_depth - 1];
        if (x < parent->x) { w -= (parent->x - x); x = parent->x; }
        if (y < parent->y) { h -= (parent->y - y); y = parent->y; }
        if (x + w > parent->x + parent->w) w = parent->x + parent->w - x;
        if (y + h > parent->y + parent->h) h = parent->y + parent->h - y;
    }

    r->scissor_stack[r->scissor_depth].x = x;
    r->scissor_stack[r->scissor_depth].y = y;
    r->scissor_stack[r->scissor_depth].w = w > 0 ? w : 0;
    r->scissor_stack[r->scissor_depth].h = h > 0 ? h : 0;
    r->scissor_depth++;
}

void cs_tui_scissor_pop(CsTuiRenderer *r) {
    if (!r || r->scissor_depth == 0) return;
    r->scissor_depth--;
}

void cs_tui_invalidate(CsTuiRenderer *r) {
    if (r) r->needs_full_redraw = true;
}

/* ============================================================================
 * Frame End / Flush
 * ============================================================================ */

void cs_tui_end(CsTuiRenderer *r) {
    if (!r) return;

    /* Headless mode: just sync buffers, no terminal output */
    if (r->config.headless) {
        size_t buf_size = (size_t)(r->buffer.width * r->buffer.height) * sizeof(CsTuiCell);
        memcpy(r->buffer.front, r->buffer.back, buf_size);
        r->needs_full_redraw = false;
        return;
    }

    bool full_redraw = r->needs_full_redraw || !r->config.differential;
    r->needs_full_redraw = false;

    /* Track current colors to minimize escape sequences */
    uint8_t cur_fg_r = 0, cur_fg_g = 0, cur_fg_b = 0;
    uint8_t cur_bg_r = 0, cur_bg_g = 0, cur_bg_b = 0;
    bool colors_set = false;

    for (int y = 0; y < r->buffer.height; y++) {
        bool moved = false;

        for (int x = 0; x < r->buffer.width; x++) {
            CsTuiCell *back = &r->buffer.back[y * r->buffer.width + x];
            CsTuiCell *front = &r->buffer.front[y * r->buffer.width + x];

            bool cell_changed = full_redraw ||
                back->codepoint != front->codepoint ||
                back->fg_r != front->fg_r || back->fg_g != front->fg_g || back->fg_b != front->fg_b ||
                back->bg_r != front->bg_r || back->bg_g != front->bg_g || back->bg_b != front->bg_b;

            if (!cell_changed) continue;

            /* Move cursor if needed */
            if (!moved) {
                cs_tui_output_printf(r, CS_TUI_CURSOR_POS(y, x));
                moved = true;
            } else {
                /* Check if we need to reposition (gap in updates) */
                /* For now, just output - consecutive cells are common */
            }

            /* Update colors if changed */
            bool fg_changed = !colors_set ||
                back->fg_r != cur_fg_r || back->fg_g != cur_fg_g || back->fg_b != cur_fg_b;
            bool bg_changed = !colors_set ||
                back->bg_r != cur_bg_r || back->bg_g != cur_bg_g || back->bg_b != cur_bg_b;

            if (fg_changed) {
                cs_tui_emit_fg_color(r, back->fg_r, back->fg_g, back->fg_b);
                cur_fg_r = back->fg_r;
                cur_fg_g = back->fg_g;
                cur_fg_b = back->fg_b;
            }
            if (bg_changed) {
                cs_tui_emit_bg_color(r, back->bg_r, back->bg_g, back->bg_b);
                cur_bg_r = back->bg_r;
                cur_bg_g = back->bg_g;
                cur_bg_b = back->bg_b;
            }
            colors_set = true;

            /* Output character */
            char utf8[5] = {0};
            int utf8_len = cs_tui_utf8_encode(back->codepoint ? back->codepoint : ' ', utf8);
            cs_tui_output_append(r, utf8, (size_t)utf8_len);

            /* Update front buffer */
            *front = *back;
        }
    }

    /* Handle cursor visibility */
    if (r->cursor_visible && !r->cursor_hidden) {
        /* Cursor is already visible */
    } else if (r->cursor_visible && r->cursor_hidden) {
        cs_tui_output_append(r, CS_TUI_CURSOR_SHOW, strlen(CS_TUI_CURSOR_SHOW));
        r->cursor_hidden = false;
    } else if (!r->cursor_visible && !r->cursor_hidden) {
        cs_tui_output_append(r, CS_TUI_CURSOR_HIDE, strlen(CS_TUI_CURSOR_HIDE));
        r->cursor_hidden = true;
    }

    /* Position cursor if visible */
    if (r->cursor_visible) {
        cs_tui_output_printf(r, CS_TUI_CURSOR_POS(r->cursor_y, r->cursor_x));
    }

    cs_tui_output_flush(r);
}

/* ============================================================================
 * Cursor
 * ============================================================================ */

void cs_tui_set_cursor(CsTuiRenderer *r, int x, int y, bool visible) {
    if (!r) return;
    r->cursor_x = x;
    r->cursor_y = y;
    r->cursor_visible = visible;
}

void cs_tui_set_z_index(CsTuiRenderer *r, int16_t z_index) {
    if (!r) return;
    r->current_z_index = z_index;
}

/* ============================================================================
 * Clay Integration
 * ============================================================================ */

void cs_tui_render_clay_commands(CsTuiRenderer *r, const void *commands, int count) {
    if (!r || !commands || count <= 0) return;

    const Clay_RenderCommand *cmds = (const Clay_RenderCommand *)commands;

    for (int i = 0; i < count; i++) {
        const Clay_RenderCommand *cmd = &cmds[i];

        /* Set current z-index from Clay command for layered rendering */
        r->current_z_index = cmd->zIndex;

        /* Convert floating-point bounds to integer cell coordinates */
        int x = (int)(cmd->boundingBox.x);
        int y = (int)(cmd->boundingBox.y);
        int w = (int)(cmd->boundingBox.width);
        int h = (int)(cmd->boundingBox.height);

        switch (cmd->commandType) {
            case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
                const Clay_RectangleRenderData *rect = &cmd->renderData.rectangle;
                CsTuiColor bg = {
                    (uint8_t)rect->backgroundColor.r,
                    (uint8_t)rect->backgroundColor.g,
                    (uint8_t)rect->backgroundColor.b,
                    (uint8_t)rect->backgroundColor.a
                };

                /* Skip fully transparent rectangles */
                if (bg.a == 0) break;

                int corner = (int)rect->cornerRadius.topLeft;
                cs_tui_rect(r, x, y, w, h, bg, NULL, corner);
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_TEXT: {
                const Clay_TextRenderData *text = &cmd->renderData.text;
                CsTuiColor fg = {
                    (uint8_t)text->textColor.r,
                    (uint8_t)text->textColor.g,
                    (uint8_t)text->textColor.b,
                    (uint8_t)text->textColor.a
                };

                if (text->stringContents.chars && text->stringContents.length > 0) {
                    cs_tui_text(r, x, y, text->stringContents.chars,
                               text->stringContents.length, fg, NULL);
                }
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_BORDER: {
                const Clay_BorderRenderData *border = &cmd->renderData.border;
                CsTuiColor color = {
                    (uint8_t)border->color.r,
                    (uint8_t)border->color.g,
                    (uint8_t)border->color.b,
                    (uint8_t)border->color.a
                };
                int corner = (int)border->cornerRadius.topLeft;
                cs_tui_border(r, x, y, w, h, color, corner);
                break;
            }

            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
                cs_tui_scissor_push(r, x, y, w, h);
                break;

            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
                cs_tui_scissor_pop(r);
                break;

            case CLAY_RENDER_COMMAND_TYPE_IMAGE:
                /* Show placeholder for images */
                cs_tui_text(r, x, y, "[IMG]", 5,
                           (CsTuiColor){128, 128, 128, 255}, NULL);
                break;

            case CLAY_RENDER_COMMAND_TYPE_CUSTOM:
                /* Custom rendering - could dispatch to user handler */
                break;

            default:
                break;
        }
    }
}

/* ============================================================================
 * Debug / Testing
 * ============================================================================ */

uint32_t cs_tui_get_cell_char(CsTuiRenderer *r, int x, int y) {
    if (!r) return 0;
    CsTuiCell *cell = cs_tui_cell_at(&r->buffer, x, y);
    return cell ? cell->codepoint : 0;
}

CsTuiColor cs_tui_get_cell_fg(CsTuiRenderer *r, int x, int y) {
    CsTuiColor c = {0, 0, 0, 0};
    if (!r) return c;
    CsTuiCell *cell = cs_tui_cell_at(&r->buffer, x, y);
    if (cell) {
        c.r = cell->fg_r;
        c.g = cell->fg_g;
        c.b = cell->fg_b;
        c.a = 255;
    }
    return c;
}

CsTuiColor cs_tui_get_cell_bg(CsTuiRenderer *r, int x, int y) {
    CsTuiColor c = {0, 0, 0, 0};
    if (!r) return c;
    CsTuiCell *cell = cs_tui_cell_at(&r->buffer, x, y);
    if (cell) {
        c.r = cell->bg_r;
        c.g = cell->bg_g;
        c.b = cell->bg_b;
        c.a = 255;
    }
    return c;
}

bool cs_tui_buffer_contains(CsTuiRenderer *r, int x, int y, const char *text) {
    if (!r || !text) return false;

    int len = (int)strlen(text);
    int col = x;
    int i = 0;

    while (i < len) {
        uint32_t expected;
        int bytes = cs_tui_utf8_decode(text + i, len - i, &expected);
        if (bytes == 0) break;
        i += bytes;

        uint32_t actual = cs_tui_get_cell_char(r, col, y);
        if (actual != expected) return false;
        col++;
    }

    return true;
}

char *cs_tui_dump_buffer(CsTuiRenderer *r) {
    if (!r) return NULL;

    /* Estimate size: width * height * 4 (max UTF-8) + newlines */
    size_t size = (size_t)(r->buffer.width * 4 + 1) * (size_t)r->buffer.height + 1;
    char *buf = malloc(size);
    if (!buf) return NULL;

    size_t pos = 0;
    for (int y = 0; y < r->buffer.height; y++) {
        for (int x = 0; x < r->buffer.width; x++) {
            CsTuiCell *cell = cs_tui_cell_at(&r->buffer, x, y);
            if (cell && cell->codepoint) {
                int len = cs_tui_utf8_encode(cell->codepoint, buf + pos);
                pos += (size_t)len;
            } else {
                buf[pos++] = ' ';
            }
        }
        buf[pos++] = '\n';
    }
    buf[pos] = '\0';

    return buf;
}
