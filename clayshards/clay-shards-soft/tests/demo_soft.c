/**
 * demo_soft.c - Software Renderer Visual Demo
 *
 * Interactive demo showing rectangles, text, and input handling.
 * Run with: make demo && ./build/demo_soft
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#define CLAY_IMPLEMENTATION
#include "clay.h"

#include "cs_soft.h"
#include "cs_render.h"

/* Animation state */
static float anim_time = 0.0f;
static int mouse_x = 0, mouse_y = 0;
static bool mouse_down = false;

static void draw_demo(CsSoftRenderer *r)
{
    /* Clear to dark gray */
    cs_soft_clear(r, cs_pack_color(30, 30, 30, 255));

    int width, height;
    cs_soft_get_size(r, &width, &height);

    /* Animated gradient background rectangles */
    for (int i = 0; i < 5; i++) {
        float phase = anim_time + i * 0.5f;
        float x = 50 + i * 120 + sinf(phase) * 20;
        float y = 100 + cosf(phase * 0.7f) * 30;
        uint8_t r_col = 50 + i * 40;
        uint8_t g_col = 100 + (int)(sinf(phase) * 50);
        uint8_t b_col = 150 + i * 20;
        cs_soft_rect(r, x, y, 100, 80, cs_pack_color(r_col, g_col, b_col, 255), 10 + i * 3);
    }

    /* Semi-transparent overlay */
    cs_soft_rect(r, 100, 250, 400, 100, cs_pack_color(255, 255, 255, 100), 20);

    /* Mouse tracker rectangle */
    cs_soft_rect(r, (float)mouse_x - 25, (float)mouse_y - 25, 50, 50,
                 mouse_down ? cs_pack_color(255, 100, 100, 200) : cs_pack_color(100, 255, 100, 200),
                 10);

    /* Border demo */
    cs_soft_border(r, 50, 400, 200, 100, cs_pack_color(255, 200, 50, 255), 3);
    cs_soft_border_sides(r, 280, 400, 200, 100, cs_pack_color(100, 200, 255, 255), 5, 2, 5, 2);

    /* Text labels - y is baseline position */
    /* Debug: draw a small marker where text should appear */
    cs_soft_rect(r, 48, 38, 4, 4, cs_pack_color(255, 0, 255, 255), 0);  /* Magenta marker at text origin */
    cs_soft_text(r, "ClayShards Software Renderer", -1, 50, 60, 24.0f, cs_pack_color(255, 255, 255, 255));
    cs_soft_text(r, "Move mouse, click, press ESC to quit", -1, 50, 560, 16.0f, cs_pack_color(180, 180, 180, 255));

    char buf[64];
    snprintf(buf, sizeof(buf), "Mouse: %d, %d", mouse_x, mouse_y);
    cs_soft_text(r, buf, -1, 50, 520, 14.0f, cs_pack_color(150, 150, 150, 255));
}

int main(void)
{
    printf("ClayShards Software Renderer Demo\n");
    printf("==================================\n");
    printf("Move mouse, click, press ESC to quit\n\n");

    /* Create renderer with Cocoa backend (auto-detected on macOS) */
    CsSoftConfig config;
    cs_soft_config_init(&config);
    config.width = 800;
    config.height = 600;
    config.platform = CS_SOFT_PLATFORM_AUTO;
    config.title = "ClayShards Demo";

    CsSoftRenderer *r = cs_soft_create(&config);
    if (!r) {
        fprintf(stderr, "Failed to create renderer\n");
        return 1;
    }

    printf("Renderer created: %dx%d\n", config.width, config.height);

    /* Main loop */
    int frame = 0;
    while (!cs_soft_should_close(r)) {
        /* Poll events */
        CsSoftEvent event;
        while (cs_soft_poll_event(r, &event)) {
            switch (event.type) {
                case CS_SOFT_EVENT_KEY_DOWN:
                    printf("Key down: %d\n", event.key.key_code);
                    if (event.key.key_code == CS_KEY_ESCAPE) {
                        cs_soft_request_close(r);
                    }
                    break;
                case CS_SOFT_EVENT_MOUSE_MOVE:
                    mouse_x = event.mouse.x;
                    mouse_y = event.mouse.y;
                    break;
                case CS_SOFT_EVENT_MOUSE_DOWN:
                    mouse_down = true;
                    mouse_x = event.mouse.x;
                    mouse_y = event.mouse.y;
                    printf("Mouse down at %d, %d\n", mouse_x, mouse_y);
                    break;
                case CS_SOFT_EVENT_MOUSE_UP:
                    mouse_down = false;
                    break;
                case CS_SOFT_EVENT_SCROLL:
                    printf("Scroll: %.1f, %.1f\n", event.scroll.delta_x, event.scroll.delta_y);
                    break;
                case CS_SOFT_EVENT_RESIZE:
                    printf("Resize: %dx%d\n", event.resize.width, event.resize.height);
                    cs_soft_resize(r, event.resize.width, event.resize.height);
                    break;
                case CS_SOFT_EVENT_CLOSE:
                    printf("Close requested\n");
                    break;
                default:
                    break;
            }
        }

        /* Render frame */
        cs_soft_begin(r);
        draw_demo(r);
        cs_soft_end(r);

        /* Update animation */
        anim_time += 0.016f;
        frame++;

        /* Simple frame rate limiting (~60 FPS) */
        usleep(16000);
    }

    printf("\nExiting after %d frames\n", frame);
    cs_soft_free(r);
    return 0;
}
