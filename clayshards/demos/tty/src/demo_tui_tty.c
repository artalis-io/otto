/**
 * demo_tui.c - ClayShards TUI Demo
 *
 * Demonstrates ClayShards immediate-mode UI components in a terminal:
 *   - cs_button: Clickable buttons
 *   - cs_input: Text input with cursor
 *   - cs_checkbox: Boolean toggle with checkmark
 *   - cs_toggle: On/off switch
 *   - cs_slider: Value slider
 *   - cs_dropdown: Selection dropdown
 *
 * This mirrors the WebGL demo functionality but renders to terminal.
 *
 * Controls:
 *   - Tab: Navigate between widgets
 *   - Enter: Activate focused widget
 *   - Arrow keys: Adjust sliders/navigate dropdowns
 *   - Type: Input text in focused input
 *   - q: Quit
 *
 * Headless mode (--headless):
 *   Reads commands from stdin, outputs debug info to stdout.
 *   Commands: tab, shift-tab, enter, up, down, left, right, char:X, backspace, quit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "sh_pal.h"
#include "sh_time.h"
#include <time.h>

#define CLAY_IMPLEMENTATION
#include "clay.h"
#include "cs_immediate.h"
#include "cs_tui.h"

/* ============================================================================
 * Application State
 * ============================================================================ */

typedef struct {
    /* Input fields */
    char name[64];
    int name_len;
    char email[128];
    int email_len;

    /* Checkboxes */
    bool notifications;
    bool darkmode;

    /* Toggle */
    bool enabled;

    /* Slider */
    float volume;

    /* Dropdown */
    int priority;

    /* Counter (button demo) */
    int counter;

    /* Terminal size */
    int width;
    int height;

    /* Running state */
    bool running;
    bool needs_redraw;
} AppState;

static AppState g_app = {
    .name = "John",
    .name_len = 4,
    .email = "",
    .email_len = 0,
    .notifications = true,
    .darkmode = false,
    .enabled = true,
    .volume = 0.7f,
    .priority = 1,
    .counter = 0,
    .running = true,
    .needs_redraw = true,
};

static bool g_headless = false;
static bool g_ansi_dump = false;  /* Use ANSI colors in dump output */
static int g_frame_number = 0;

/* ============================================================================
 * Theme
 * ============================================================================ */

static const struct {
    Clay_Color bg;
    Clay_Color panel;
    Clay_Color text;
    Clay_Color text_muted;
    Clay_Color border;
    Clay_Color accent;
} THEME = {
    .bg         = {30, 30, 30, 255},
    .panel      = {45, 45, 45, 255},
    .text       = {240, 240, 240, 255},
    .text_muted = {150, 150, 150, 255},
    .border     = {80, 80, 80, 255},
    .accent     = {80, 140, 200, 255},
};

/* ============================================================================
 * Terminal Input
 * ============================================================================ */

static void enable_raw_mode(void) {
    if (sh_term_raw_enter() == 0) {
        atexit(sh_term_raw_leave);
    }
}

static int read_key(void) {
    unsigned char c;
    if (sh_term_read_byte(&c) != 1) return 0;

    /* Escape sequences. Windows is put into virtual-terminal input mode by
     * sh_term_raw_enter(), so arrow keys arrive as the same sequences POSIX
     * sends and this decoding is shared rather than duplicated. */
    if (c == '\x1b') {
        unsigned char seq[3];
        if (sh_term_read_byte(&seq[0]) != 1) return '\x1b';
        if (sh_term_read_byte(&seq[1]) != 1) return '\x1b';

        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return 1001;  /* Up */
                case 'B': return 1002;  /* Down */
                case 'C': return 1003;  /* Right */
                case 'D': return 1004;  /* Left */
                case 'Z': return 1005;  /* Shift-Tab */
            }
        }
        return '\x1b';
    }

    return c;
}

/* ============================================================================
 * Signal Handling
 * ============================================================================ */

/* No SIGWINCH and no SIGINT.
 *
 * Resize is picked up by comparing the terminal size each frame, which costs
 * one cheap syscall in a loop that already sleeps 16ms and works the same on
 * every platform. Ctrl-C arrives as a 0x03 byte through the normal input path,
 * because raw mode turns off the driver's interpretation of it -- see
 * sh_term_raw_enter(). Neither needed a signal, and signals are the part of
 * this that would not have ported. */

/* ============================================================================
 * Headless Mode Support
 * ============================================================================ */

static const char *PRIORITY_OPTIONS[] = {"Low", "Medium", "High", "Critical"};

static void dump_frame(CsTuiRenderer *renderer, const char *input_cmd) {
    printf("=== FRAME %d ===\n", g_frame_number++);
    printf("INPUT: %s", input_cmd);
    if (input_cmd[strlen(input_cmd) - 1] != '\n') printf("\n");
    printf("FOCUS: 0x%08x\n", cs_focused_id());

    /* Add cursor info for text input debugging */
    uint32_t focused = cs_focused_id();
    if (focused != 0) {
        printf("CURSOR: pos=%d, visible=%s, text=\"%s\" (len=%d)\n",
               cs_cursor_pos(),
               cs_cursor_visible() ? "true" : "false",
               cs_focused_text() ? cs_focused_text() : "(null)",
               cs_focused_text_len());
        float x, y, w, h;
        cs_focused_bounds(&x, &y, &w, &h);
        printf("BOUNDS: x=%.0f, y=%.0f, w=%.0f, h=%.0f\n", x, y, w, h);
    }

    printf("STATE: counter=%d, name=\"%s\", email=\"%s\", notifications=%s, darkmode=%s, enabled=%s, volume=%.2f, priority=%d (%s)\n",
           g_app.counter,
           g_app.name,
           g_app.email,
           g_app.notifications ? "true" : "false",
           g_app.darkmode ? "true" : "false",
           g_app.enabled ? "true" : "false",
           g_app.volume,
           g_app.priority,
           PRIORITY_OPTIONS[g_app.priority]);
    printf("BUFFER:\n");
    char *dump = g_ansi_dump ? cs_tui_dump_buffer_ansi(renderer) : cs_tui_dump_buffer(renderer);
    if (dump) {
        printf("%s", dump);
        free(dump);
    }
    printf("\n");
    fflush(stdout);
}

/* Parse a headless command and execute it. Returns false on "quit". */
static bool process_headless_command(const char *cmd) {
    /* Trim whitespace */
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    size_t len = strlen(cmd);
    while (len > 0 && (cmd[len-1] == '\n' || cmd[len-1] == '\r' || cmd[len-1] == ' ')) {
        len--;
    }

    if (len == 0) return true;  /* Empty line, continue */

    if (strncmp(cmd, "quit", 4) == 0 || (len == 1 && cmd[0] == 'q')) {
        return false;
    } else if (strncmp(cmd, "tab", 3) == 0) {
        cs_focus_next();
    } else if (strncmp(cmd, "shift-tab", 9) == 0) {
        cs_focus_prev();
    } else if (strncmp(cmd, "enter", 5) == 0) {
        /* Use key_down for Enter to set pending_enter for buttons */
        cs_key_down(13, false, false);
    } else if (strncmp(cmd, "up", 2) == 0) {
        cs_key_down(38, false, false);  /* Standard key code for Up arrow */
    } else if (strncmp(cmd, "down", 4) == 0) {
        cs_key_down(40, false, false);  /* Standard key code for Down arrow */
    } else if (strncmp(cmd, "left", 4) == 0) {
        cs_key_down(37, false, false);  /* Standard key code for Left arrow */
    } else if (strncmp(cmd, "right", 5) == 0) {
        cs_key_down(39, false, false);  /* Standard key code for Right arrow */
    } else if (strncmp(cmd, "backspace", 9) == 0) {
        cs_key_down(8, false, false);
    } else if (strncmp(cmd, "char:", 5) == 0 && len > 5) {
        cs_key_char((uint32_t)cmd[5]);
    }
    return true;
}

/* ============================================================================
 * UI Layout
 * ============================================================================ */

static void render_ui(void) {
    /* Root container */
    CLAY(CLAY_ID("Root"), {
        .layout = {
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .sizing = { CLAY_SIZING_FIXED((float)g_app.width), CLAY_SIZING_FIXED((float)g_app.height) },
            .padding = CLAY_PADDING_ALL(1),
            .childGap = 1
        },
        .backgroundColor = THEME.bg
    }) {
        /* Title */
        CLAY(CLAY_ID("TitleBar"), {
            .layout = {
                .padding = {1, 1, 0, 0},
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) }
            },
            .backgroundColor = THEME.accent
        }) {
            CLAY_TEXT(CLAY_STRING(" ClayShards TUI Demo "),
                     CLAY_TEXT_CONFIG({ .fontSize = 14, .textColor = THEME.text }));
        }

        /* Main content area */
        CLAY(CLAY_ID("Content"), {
            .layout = {
                .layoutDirection = CLAY_LEFT_TO_RIGHT,
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                .childGap = 2
            }
        }) {
            /* Left panel - Form inputs */
            CLAY(CLAY_ID("LeftPanel"), {
                .layout = {
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                    .sizing = { CLAY_SIZING_FIXED(30), CLAY_SIZING_GROW(0) },
                    .padding = CLAY_PADDING_ALL(1),
                    .childGap = 1
                },
                .backgroundColor = THEME.panel,
                .border = { .width = {1, 1, 1, 1, 0}, .color = THEME.border }
            }) {
                CLAY_TEXT(CLAY_STRING("Form Inputs"),
                         CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text_muted }));

                /* Name input */
                CLAY_TEXT(CLAY_STRING("Name:"),
                         CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text }));

                const CsInputStyle input_style = {
                    .width = 26,
                    .height = 1,  /* Single row for content */
                    .font_size = 12,
                    .padding = 0,
                    .corner_radius = 0
                };
                cs_input(CS_ID("name_input"),
                    g_app.name, &g_app.name_len, sizeof(g_app.name),
                    "Enter name...", &input_style);

                /* Email input */
                CLAY_TEXT(CLAY_STRING("Email:"),
                         CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text }));

                cs_input(CS_ID("email_input"),
                    g_app.email, &g_app.email_len, sizeof(g_app.email),
                    "user@example.com", &input_style);

                /* Separator */
                CLAY(CLAY_ID("Sep1"), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) } },
                    .backgroundColor = THEME.border
                }) {}

                /* Checkboxes */
                CLAY_TEXT(CLAY_STRING("Options:"),
                         CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text_muted }));

                const CsCheckboxStyle check_style = {
                    .size = 1,  /* Single character checkbox */
                    .font_size = 12,
                    .corner_radius = 0,
                    .gap = 1,
                    .border_width = 0
                };
                cs_checkbox(CS_ID("notif_check"), &g_app.notifications, "Notifications", &check_style);
                cs_checkbox(CS_ID("dark_check"), &g_app.darkmode, "Dark Mode", &check_style);
            }

            /* Right panel - Controls */
            CLAY(CLAY_ID("RightPanel"), {
                .layout = {
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                    .padding = CLAY_PADDING_ALL(1),
                    .childGap = 1
                },
                .backgroundColor = THEME.panel,
                .border = { .width = {1, 1, 1, 1, 0}, .color = THEME.border }
            }) {
                CLAY_TEXT(CLAY_STRING("Controls"),
                         CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text_muted }));

                /* Toggle */
                CLAY(CLAY_ID("ToggleRow"), {
                    .layout = { .childGap = 2, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } }
                }) {
                    CLAY_TEXT(CLAY_STRING("Enabled:"),
                             CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text }));
                    const CsToggleStyle toggle_style = {
                        .width = 4,
                        .height = 1,
                        .font_size = 12,
                        .gap = 1
                    };
                    cs_toggle(CS_ID("enable_toggle"), &g_app.enabled, NULL, &toggle_style);
                }

                /* Slider */
                CLAY(CLAY_ID("SliderRow"), {
                    .layout = {
                        .layoutDirection = CLAY_TOP_TO_BOTTOM,
                        .childGap = 0
                    }
                }) {
                    const CsSliderStyle slider_style = {
                        .width = 20,
                        .height = 1,
                        .thumb_size = 1,
                        .font_size = 12,
                        .corner_radius = 0,
                        .step = 0.1f,
                        .show_value = true
                    };
                    cs_slider(CS_ID("volume_slider"), &g_app.volume, 0.0f, 1.0f, "Volume", &slider_style);
                }

                /* Dropdown */
                CLAY(CLAY_ID("DropdownRow"), {
                    .layout = { .childGap = 2, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } }
                }) {
                    CLAY_TEXT(CLAY_STRING("Priority:"),
                             CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text }));
                    const CsDropdownStyle dd_style = {
                        .width = 30,
                        .height = 1,
                        .font_size = 12,
                        .corner_radius = 0,
                        .max_height = 6
                    };
                    cs_dropdown(CS_ID("priority_dd"), &g_app.priority, PRIORITY_OPTIONS, 4, &dd_style);
                }

                /* Space for dropdown list (4 items + padding = ~5 rows) */
                CLAY(CLAY_ID("DropdownSpace"), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(5) } }
                }) {}

                /* Buttons */
                CLAY_TEXT(CLAY_STRING("Actions:"),
                         CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text_muted }));

                CLAY(CLAY_ID("ButtonRow"), {
                    .layout = { .childGap = 2 }
                }) {
                    const CsButtonStyle btn_style = {
                        .variant = CS_BTN_PRIMARY,
                        .font_size = 12,
                        .corner_radius = 0,
                        .padding_x = 2,
                        .padding_y = 0
                    };
                    const CsButtonStyle btn_default = {
                        .variant = CS_BTN_DEFAULT,
                        .font_size = 12,
                        .corner_radius = 0,
                        .padding_x = 2,
                        .padding_y = 0
                    };

                    if (cs_button(CS_ID("inc_btn"), " + ", &btn_style).clicked) {
                        g_app.counter++;
                    }
                    if (cs_button(CS_ID("dec_btn"), " - ", &btn_default).clicked) {
                        g_app.counter--;
                    }
                    if (cs_button(CS_ID("reset_btn"), " Reset ", &btn_default).clicked) {
                        g_app.counter = 0;
                    }
                }

                /* Counter display */
                char counter_str[32];
                snprintf(counter_str, sizeof(counter_str), "Counter: %d", g_app.counter);
                CLAY_TEXT(((Clay_String){ .chars = counter_str, .length = (int)strlen(counter_str) }),
                         CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text }));
            }
        }

        /* Footer */
        CLAY(CLAY_ID("Footer"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) },
                .padding = {1, 1, 0, 0}
            },
            .backgroundColor = THEME.panel
        }) {
            CLAY_TEXT(CLAY_STRING(" Tab: Navigate | Enter: Activate | q: Quit "),
                     CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text_muted }));
        }
    }
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char *argv[]) {
    /* Check for flags */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            g_headless = true;
        } else if (strcmp(argv[i], "--ansi") == 0) {
            g_ansi_dump = true;
        }
    }

    if (g_headless) {
        g_app.width = 80;
        g_app.height = 24;
    } else {
        if (!cs_tui_is_tty()) {
            fprintf(stderr, "Error: Not running in a terminal\n");
            fprintf(stderr, "Use --headless for non-interactive mode\n");
            return 1;
        }
        enable_raw_mode();
    }

    /* Initialize Clay with TUI character-based coordinates.
     * cs_tui_init_clay() handles:
     *   - Getting terminal size from OS (if width/height are 0)
     *   - Allocating Clay memory
     *   - Initializing Clay with character dimensions
     *   - Setting up 1:1 character text measurement
     */
    void *clay_mem = cs_tui_init_clay(
        g_headless ? g_app.width : 0,   /* 0 = auto-detect from OS */
        g_headless ? g_app.height : 0,
        &g_app.width,
        &g_app.height
    );
    if (!clay_mem) {
        fprintf(stderr, "Error: Failed to initialize Clay\n");
        return 1;
    }

    /* Initialize ClayShards */
    cs_init();

    /* Initialize TUI renderer */
    CsTuiConfig tui_config;
    cs_tui_config_init(&tui_config);
    tui_config.width = g_app.width;
    tui_config.height = g_app.height;
    tui_config.headless = g_headless;
    if (!g_headless) {
        tui_config.alternate_screen = true;
    }

    CsTuiRenderer *renderer = cs_tui_create(&tui_config);
    if (!renderer) {
        fprintf(stderr, "Error: Failed to create TUI renderer\n");
        free(clay_mem);
        return 1;
    }

    if (g_headless) {
        /* ================================================================
         * Headless mode: read commands from stdin, dump frames to stdout
         * ================================================================ */
        char cmd_buf[256];

        /* Helper to render cursor in headless mode - use block character */
        /* Only render for text inputs (when there's active text or bounds are valid) */
        #define RENDER_CURSOR() do { \
            uint32_t fid = cs_focused_id(); \
            if (fid != 0 && cs_cursor_visible() && cs_focused_text_len() >= 0) { \
                float fx, fy, fw, fh; \
                cs_focused_bounds(&fx, &fy, &fw, &fh); \
                /* Only render if bounds are valid (text input sets these) */ \
                if (fw > 0 && fh > 0) { \
                    int cpos = cs_cursor_pos(); \
                    int cx = (int)fx + cpos; \
                    int cy = (int)fy; \
                    /* Render block cursor - visible in headless dump */ \
                    cs_tui_text(renderer, cx, cy, "\xE2\x96\x88", 3, \
                               (CsTuiColor){255, 255, 0, 255}, NULL); /* █ U+2588 */ \
                } \
            } \
        } while(0)

        /* Render initial frame */
        cs_frame_begin();
        Clay_BeginLayout();
        render_ui();
        Clay_RenderCommandArray commands = Clay_EndLayout();
        cs_frame_end(0.016f);

        cs_tui_begin(renderer);
        cs_tui_render_clay_commands(renderer, commands.internalArray, commands.length);
        RENDER_CURSOR();
        cs_tui_end(renderer);
        dump_frame(renderer, "(init)");

        /* Process commands from stdin */
        while (fgets(cmd_buf, sizeof(cmd_buf), stdin) != NULL) {
            if (!process_headless_command(cmd_buf)) {
                break;
            }

            cs_frame_begin();
            Clay_BeginLayout();
            render_ui();
            commands = Clay_EndLayout();
            cs_frame_end(0.016f);

            cs_tui_begin(renderer);
            cs_tui_render_clay_commands(renderer, commands.internalArray, commands.length);
            RENDER_CURSOR();
            cs_tui_end(renderer);
            dump_frame(renderer, cmd_buf);
        }

        #undef RENDER_CURSOR
    } else {
        /* ================================================================
         * Interactive mode: normal terminal UI
         * ================================================================ */
        double last_time = sh_monotonic_seconds();

        while (g_app.running) {
            /* Resize detection, in place of SIGWINCH. */
            int term_w = g_app.width, term_h = g_app.height;
            cs_tui_get_terminal_size(&term_w, &term_h);
            if (term_w != g_app.width || term_h != g_app.height) {
                g_app.width = term_w;
                g_app.height = term_h;
                cs_tui_resize(renderer, g_app.width, g_app.height);
                cs_tui_update_clay_size(g_app.width, g_app.height);
            }

            double now = sh_monotonic_seconds();
            float dt = (float)(now - last_time);
            last_time = now;

            int key = read_key();
            if (key > 0) {
                if (key == 'q' || key == 'Q' || key == 3) {
                    /* 3 is Ctrl-C: raw mode delivers it as a byte rather than
                     * as a signal, so quitting on it is what keeps the key
                     * working at all. */
                    g_app.running = false;
                } else if (key == '\t') {
                    cs_focus_next();
                } else if (key == 1005) {
                    cs_focus_prev();
                } else if (key == '\r' || key == '\n') {
                    cs_key_down(13, false, false);  /* Enter key for activation */
                } else if (key == 1001) {
                    cs_key_down(38, false, false);  /* Up arrow */
                } else if (key == 1002) {
                    cs_key_down(40, false, false);  /* Down arrow */
                } else if (key == 1003) {
                    cs_key_down(39, false, false);  /* Right arrow */
                } else if (key == 1004) {
                    cs_key_down(37, false, false);  /* Left arrow */
                } else if (key == 127 || key == 8) {
                    cs_key_down(8, false, false);
                } else if (key >= 32 && key < 127) {
                    cs_key_char((uint32_t)key);
                }
            }

            cs_frame_begin();
            Clay_BeginLayout();
            render_ui();
            Clay_RenderCommandArray commands = Clay_EndLayout();
            cs_frame_end(dt);

            cs_tui_begin(renderer);
            cs_tui_render_clay_commands(renderer, commands.internalArray, commands.length);

            /* Show cursor only for text inputs (inputs set valid bounds) */
            float fx, fy, fw, fh;
            cs_focused_bounds(&fx, &fy, &fw, &fh);
            if (fw > 0 && fh > 0 && cs_cursor_visible()) {
                int cursor_pos = cs_cursor_pos();
                int cursor_x = (int)fx + cursor_pos;
                int cursor_y = (int)fy;
                cs_tui_set_cursor(renderer, cursor_x, cursor_y, true);
            } else {
                /* Hide cursor when not in text input */
                cs_tui_set_cursor(renderer, 0, 0, false);
            }

            cs_tui_end(renderer);
            sh_sleep_ms(16);
        }

        printf("\nGoodbye!\n");
    }

    cs_tui_free(renderer);
    free(clay_mem);
    return 0;
}
