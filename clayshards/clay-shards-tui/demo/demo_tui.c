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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/ioctl.h>
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

static struct termios g_orig_termios;
static bool g_raw_mode = false;

static void disable_raw_mode(void) {
    if (g_raw_mode) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_raw_mode = false;
    }
}

static void enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == -1) return;

    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != -1) {
        g_raw_mode = true;
        atexit(disable_raw_mode);
    }
}

static int read_key(void) {
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return 0;

    /* Handle escape sequences */
    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

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

static volatile sig_atomic_t g_resize_pending = 0;

static void handle_sigwinch(int sig) {
    (void)sig;
    g_resize_pending = 1;
}

static void handle_sigint(int sig) {
    (void)sig;
    g_app.running = false;
}

static void get_terminal_size(int *w, int *h) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        *w = ws.ws_col;
        *h = ws.ws_row;
    } else {
        *w = 80;
        *h = 24;
    }
}

/* ============================================================================
 * Clay Text Measurement
 * ============================================================================ */

static Clay_Dimensions measure_text(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData) {
    (void)userData;
    /* Simple monospace measurement: 1 char = 1 column */
    /* Height is based on font_size but we map to rows */
    float width = (float)text.length;
    float height = 1.0f;  /* One row per line in TUI */

    /* Scale width by font size ratio (base = 12) */
    if (config && config->fontSize > 0) {
        /* In TUI, font size doesn't really matter for width, but we can adjust slightly */
        (void)config->fontSize;
    }

    return (Clay_Dimensions){width, height};
}

/* ============================================================================
 * UI Layout
 * ============================================================================ */

static const char *PRIORITY_OPTIONS[] = {"Low", "Medium", "High", "Critical"};

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
                    .height = 1,
                    .font_size = 12,
                    .padding = 1,
                    .corner_radius = 0
                };
                CsInputResult name_result = cs_input(CS_ID("name_input"),
                    g_app.name, &g_app.name_len, sizeof(g_app.name),
                    "Enter name...", &input_style);
                if (name_result.changed) {
                    g_app.needs_redraw = true;
                }

                /* Email input */
                CLAY_TEXT(CLAY_STRING("Email:"),
                         CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text }));

                CsInputResult email_result = cs_input(CS_ID("email_input"),
                    g_app.email, &g_app.email_len, sizeof(g_app.email),
                    "user@example.com", &input_style);
                if (email_result.changed) {
                    g_app.needs_redraw = true;
                }

                /* Separator */
                CLAY(CLAY_ID("Sep1"), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) } },
                    .backgroundColor = THEME.border
                }) {}

                /* Checkboxes */
                CLAY_TEXT(CLAY_STRING("Options:"),
                         CLAY_TEXT_CONFIG({ .fontSize = 12, .textColor = THEME.text_muted }));

                const CsCheckboxStyle check_style = {
                    .size = 1,
                    .font_size = 12,
                    .corner_radius = 0,
                    .gap = 1
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
                        .width = 6,
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
                        .width = 12,
                        .height = 1,
                        .font_size = 12,
                        .corner_radius = 0
                    };
                    cs_dropdown(CS_ID("priority_dd"), &g_app.priority, PRIORITY_OPTIONS, 4, &dd_style);
                }

                /* Separator */
                CLAY(CLAY_ID("Sep2"), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1) } },
                    .backgroundColor = THEME.border
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
                        g_app.needs_redraw = true;
                    }
                    if (cs_button(CS_ID("dec_btn"), " - ", &btn_default).clicked) {
                        g_app.counter--;
                        g_app.needs_redraw = true;
                    }
                    if (cs_button(CS_ID("reset_btn"), " Reset ", &btn_default).clicked) {
                        g_app.counter = 0;
                        g_app.needs_redraw = true;
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

int main(void) {
    /* Check if running in a terminal */
    if (!cs_tui_is_tty()) {
        fprintf(stderr, "Error: Not running in a terminal\n");
        return 1;
    }

    /* Get initial terminal size */
    get_terminal_size(&g_app.width, &g_app.height);

    /* Setup signal handlers */
    signal(SIGWINCH, handle_sigwinch);
    signal(SIGINT, handle_sigint);

    /* Enable raw mode for keyboard input */
    enable_raw_mode();

    /* Initialize Clay */
    uint64_t clay_mem_size = Clay_MinMemorySize();
    void *clay_mem = malloc(clay_mem_size);
    if (!clay_mem) {
        fprintf(stderr, "Error: Failed to allocate Clay memory\n");
        return 1;
    }

    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(clay_mem_size, clay_mem);
    Clay_Initialize(arena, (Clay_Dimensions){(float)g_app.width, (float)g_app.height},
                    (Clay_ErrorHandler){0});
    Clay_SetMeasureTextFunction(measure_text, NULL);

    /* Initialize ClayShards */
    cs_init();

    /* Initialize TUI renderer */
    CsTuiConfig tui_config;
    cs_tui_config_init(&tui_config);
    tui_config.width = g_app.width;
    tui_config.height = g_app.height;
    tui_config.alternate_screen = true;
    tui_config.hide_cursor = false;  /* Show cursor for input fields */

    CsTuiRenderer *renderer = cs_tui_create(&tui_config);
    if (!renderer) {
        fprintf(stderr, "Error: Failed to create TUI renderer\n");
        free(clay_mem);
        return 1;
    }

    /* Main loop */
    struct timespec last_time, now;
    clock_gettime(CLOCK_MONOTONIC, &last_time);

    while (g_app.running) {
        /* Handle resize */
        if (g_resize_pending) {
            g_resize_pending = 0;
            get_terminal_size(&g_app.width, &g_app.height);
            cs_tui_resize(renderer, g_app.width, g_app.height);
            Clay_SetLayoutDimensions((Clay_Dimensions){(float)g_app.width, (float)g_app.height});
            g_app.needs_redraw = true;
        }

        /* Calculate delta time */
        clock_gettime(CLOCK_MONOTONIC, &now);
        float dt = (float)(now.tv_sec - last_time.tv_sec) +
                   (float)(now.tv_nsec - last_time.tv_nsec) / 1e9f;
        last_time = now;

        /* Read input (non-blocking) */
        int key = read_key();
        if (key > 0) {
            g_app.needs_redraw = true;

            if (key == 'q' || key == 'Q') {
                g_app.running = false;
            } else if (key == '\t') {
                /* Tab - focus next */
                cs_focus_next();
            } else if (key == 1005) {
                /* Shift-Tab - focus previous */
                cs_focus_prev();
            } else if (key == '\r' || key == '\n') {
                /* Enter - activate */
                cs_set_pending_click();
            } else if (key == 1001) {
                /* Up arrow */
                cs_key_down(1001, false, false);
            } else if (key == 1002) {
                /* Down arrow */
                cs_key_down(1002, false, false);
            } else if (key == 1003) {
                /* Right arrow */
                cs_key_down(1003, false, false);
            } else if (key == 1004) {
                /* Left arrow */
                cs_key_down(1004, false, false);
            } else if (key == 127 || key == 8) {
                /* Backspace */
                cs_key_down(8, false, false);
            } else if (key >= 32 && key < 127) {
                /* Printable character */
                cs_key_char((uint32_t)key);
            }
        }

        /* Render frame */
        cs_frame_begin();
        Clay_BeginLayout();

        render_ui();

        Clay_RenderCommandArray commands = Clay_EndLayout();
        cs_frame_end(dt);

        /* Render to terminal */
        cs_tui_begin(renderer);
        cs_tui_render_clay_commands(renderer, commands.internalArray, commands.length);

        /* Position cursor for focused input */
        uint32_t focused = cs_focused_id();
        if (focused != 0 && cs_cursor_visible()) {
            /* Get cursor position from ClayShards */
            int cursor_pos = cs_cursor_pos();
            /* For now, estimate position - would need element bounds in real impl */
            cs_tui_set_cursor(renderer, cursor_pos + 5, 5, true);
        } else {
            cs_tui_set_cursor(renderer, 0, 0, false);
        }

        cs_tui_end(renderer);

        /* Sleep a bit to avoid spinning */
        usleep(16000);  /* ~60 FPS */
    }

    /* Cleanup */
    cs_tui_free(renderer);
    free(clay_mem);

    printf("\nGoodbye!\n");
    return 0;
}
