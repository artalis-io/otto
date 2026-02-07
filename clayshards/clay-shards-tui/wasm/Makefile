# ClayShards TUI WASM Library
#
# Builds the TUI renderer as a standalone WASM module.

CC = emcc
CFLAGS = -O3 -Wall -Wextra

# Include paths (from clay-shards-tui/wasm/)
INCLUDES = -I../include \
           -I../src \
           -I../../clay-shards/include \
           -I../../../vendor/clay

# Source files
SOURCES = src/cs_tui_wasm.c \
          ../src/cs_tui.c \
          ../../clay-shards/src/cs_immediate.c

# Output
BUILD_DIR = build
OUTPUT = $(BUILD_DIR)/cs_tui.wasm

# WASM settings
WASM_FLAGS = -s STANDALONE_WASM=1

EXPORTS = -s EXPORTED_FUNCTIONS='[\
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

MEM_FLAGS = -s INITIAL_MEMORY=16777216 \
            -s ALLOW_MEMORY_GROWTH=1 \
            -s MAXIMUM_MEMORY=16777216 \
            --no-entry

.PHONY: all clean help

all: $(OUTPUT)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(OUTPUT): $(SOURCES) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(WASM_FLAGS) $(EXPORTS) $(MEM_FLAGS) $(SOURCES) -o $@
	@echo "Built: $@"
	@ls -lh $@

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "ClayShards TUI WASM Library"
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build library (default)"
	@echo "  clean   - Remove build artifacts"
	@echo ""
	@echo "Output: $(OUTPUT)"
