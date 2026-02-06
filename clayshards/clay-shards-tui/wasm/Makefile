# ClayShards TUI WASM Build
#
# Builds the TUI renderer as a standalone WASM module for browser use.

# Emscripten compiler
CC = emcc
CFLAGS = -O3 -Wall -Wextra

# Include paths
INCLUDES = -I../include \
           -I../src \
           -I../../clay-shards/include \
           -I../../../vendor/clay

# Source files - library only
LIB_SOURCES = src/cs_tui_wasm.c \
              ../src/cs_tui.c \
              ../../clay-shards/src/cs_immediate.c

# Demo sources - includes the demo UI
DEMO_SOURCES = src/cs_tui_demo.c \
               ../src/cs_tui.c \
               ../../clay-shards/src/cs_immediate.c

# Output
BUILD_DIR = build
LIB_OUTPUT = $(BUILD_DIR)/cs_tui.wasm
DEMO_OUTPUT = $(BUILD_DIR)/demo.wasm

# WASM export settings - standalone WASM, no JS glue
WASM_FLAGS = -s STANDALONE_WASM=1

# Library exports
LIB_EXPORTS = -s EXPORTED_FUNCTIONS='[\
  "_wasm_tui_init","_wasm_tui_resize","_wasm_tui_destroy",\
  "_wasm_tui_get_buffer","_wasm_tui_get_width","_wasm_tui_get_height",\
  "_wasm_tui_get_cell_size","_wasm_tui_buffer_dirty",\
  "_wasm_tui_key_event","_wasm_tui_char_event",\
  "_wasm_tui_mouse_move","_wasm_tui_mouse_event","_wasm_tui_scroll",\
  "_wasm_tui_render","_wasm_tui_frame_begin","_wasm_tui_frame_end",\
  "_wasm_tui_cell_codepoint","_wasm_tui_cell_fg","_wasm_tui_cell_bg",\
  "_cs_focused_id","_cs_cursor_pos","_cs_selection_start","_cs_cursor_visible",\
  "_cs_focused_text","_cs_focused_text_len",\
  "_cs_focused_x","_cs_focused_y","_cs_focused_w","_cs_focused_h",\
  "_cs_key_down","_cs_key_char","_cs_blur","_cs_set_pending_click",\
  "_cs_register_focusable","_cs_focus_next","_cs_focus_prev","_cs_focusable_count",\
  "_cs_set_scroll_delta","_cs_scroll_container_hovered","_cs_scroll_set_position",\
  "_malloc","_free"\
]'

# Demo exports
DEMO_EXPORTS = -s EXPORTED_FUNCTIONS='[\
  "_demo_init","_demo_resize",\
  "_demo_get_buffer","_demo_get_width","_demo_get_height","_demo_get_cell_size",\
  "_demo_frame","_demo_key_down","_demo_key_char",\
  "_demo_mouse_move","_demo_mouse_click","_demo_mouse_release",\
  "_demo_get_counter","_demo_get_volume","_demo_get_priority",\
  "_demo_get_enabled","_demo_get_notifications","_demo_get_darkmode",\
  "_demo_get_quit",\
  "_malloc","_free"\
]'

# Memory settings (need enough for Clay + TUI buffer)
MEM_FLAGS = -s INITIAL_MEMORY=16777216
MEM_FLAGS += -s ALLOW_MEMORY_GROWTH=1
MEM_FLAGS += -s MAXIMUM_MEMORY=16777216
MEM_FLAGS += --no-entry

.PHONY: all lib demo clean help

all: lib demo

lib: $(LIB_OUTPUT)

demo: $(DEMO_OUTPUT)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(LIB_OUTPUT): $(LIB_SOURCES) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(WASM_FLAGS) $(LIB_EXPORTS) $(MEM_FLAGS) $(LIB_SOURCES) -o $@
	@echo "Built library: $@"
	@ls -lh $@

$(DEMO_OUTPUT): $(DEMO_SOURCES) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(WASM_FLAGS) $(DEMO_EXPORTS) $(MEM_FLAGS) $(DEMO_SOURCES) -o $@
	@echo "Built demo: $@"
	@ls -lh $@

# Debug build with symbols
debug: CFLAGS = -O0 -g -Wall -Wextra -std=c11
debug: WASM_FLAGS += -s ASSERTIONS=1
debug: all
	@echo "Built debug versions"

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "ClayShards TUI WASM Build"
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build library and demo (default)"
	@echo "  lib     - Build library WASM only"
	@echo "  demo    - Build demo WASM only"
	@echo "  debug   - Build with debug symbols and assertions"
	@echo "  clean   - Remove build artifacts"
	@echo ""
	@echo "Requirements:"
	@echo "  - Emscripten SDK (emcc)"
	@echo ""
	@echo "Output:"
	@echo "  $(LIB_OUTPUT)  - Library for custom apps"
	@echo "  $(DEMO_OUTPUT) - Demo with widgets"
