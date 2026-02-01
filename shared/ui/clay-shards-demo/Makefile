# ClayShards Demo - Makefile
# Builds WASM module for cs_map demo

# Emscripten compiler
CC = emcc
CFLAGS = -O3 -Wall -Wextra
# Note: Not defining CLAY_WASM - using regular function pointers instead

# Include paths - Clay vendor + ClayShards
INCLUDES = -I../../../vendor/clay -I../clay-shards/include

# Don't use CLAY_WASM - we'll set measure function via Clay_SetMeasureTextFunction
# This avoids needing JS imports for measureTextFunction/queryScrollOffsetFunction

# Source files (includes immediate mode components)
SOURCES = src/demo.c \
          ../clay-shards/src/cs_immediate.c

# Output
BUILD_DIR = build
OUTPUT = $(BUILD_DIR)/demo.wasm

# WASM export settings - standalone WASM, no JS glue
WASM_FLAGS = -s STANDALONE_WASM=1

# Exports are organized by module:
#   map_*      - Domain-specific (this app)
#   cs_clay_*  - Generic Clay integration (clay-shards)
#   cs_*       - Immediate mode components (clay-shards)
WASM_FLAGS += -s EXPORTED_FUNCTIONS='[\
  "_map_init","_map_resize","_map_set_center","_map_set_zoom",\
  "_map_get_lat","_map_get_lon","_map_get_zoom","_map_get_visual_zoom","_map_get_layer",\
  "_map_pointer_move","_map_pointer_down","_map_pointer_up","_map_scroll",\
  "_map_handle_click","_map_frame",\
  "_cs_clay_is_initialized","_cs_clay_get_error",\
  "_cs_clay_cmd_count","_cs_clay_cmd_type",\
  "_cs_clay_cmd_x","_cs_clay_cmd_y","_cs_clay_cmd_w","_cs_clay_cmd_h",\
  "_cs_clay_cmd_rect_color","_cs_clay_cmd_rect_radius",\
  "_cs_clay_cmd_text_str","_cs_clay_cmd_text_len","_cs_clay_cmd_text_color","_cs_clay_cmd_text_size",\
  "_cs_clay_cmd_border_color","_cs_clay_cmd_border_radius","_cs_clay_cmd_border_width",\
  "_cs_focused_id","_cs_cursor_pos","_cs_selection_start","_cs_cursor_visible",\
  "_cs_focused_text","_cs_focused_text_len",\
  "_cs_focused_x","_cs_focused_y","_cs_focused_w","_cs_focused_h",\
  "_cs_key_down","_cs_key_char","_cs_blur","_cs_set_pending_click",\
  "_cs_clay_set_glyph_advance","_cs_clay_has_font_metrics",\
  "_cs_register_focusable","_cs_focus_next","_cs_focus_prev","_cs_focusable_count",\
  "_cs_map_overlay_count","_cs_map_overlay_type","_cs_map_overlay_id",\
  "_cs_map_overlay_polyline_count","_cs_map_overlay_polyline_lat","_cs_map_overlay_polyline_lon",\
  "_cs_map_overlay_polyline_color_r","_cs_map_overlay_polyline_color_g",\
  "_cs_map_overlay_polyline_color_b","_cs_map_overlay_polyline_color_a",\
  "_cs_map_overlay_polyline_width",\
  "_cs_map_overlay_marker_lat","_cs_map_overlay_marker_lon","_cs_map_overlay_marker_radius",\
  "_cs_map_overlay_marker_color_r","_cs_map_overlay_marker_color_g",\
  "_cs_map_overlay_marker_color_b","_cs_map_overlay_marker_color_a",\
  "_cs_map_overlay_marker_border_r","_cs_map_overlay_marker_border_g",\
  "_cs_map_overlay_marker_border_b","_cs_map_overlay_marker_border_a",\
  "_cs_map_overlay_marker_border_width",\
  "_cs_map_set_hovered_overlay","_cs_map_set_clicked_overlay"\
]'
WASM_FLAGS += -s INITIAL_MEMORY=16777216
WASM_FLAGS += -s ALLOW_MEMORY_GROWTH=1
WASM_FLAGS += --no-entry

.PHONY: all clean serve help

all: $(OUTPUT)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Build standalone WASM directly
$(OUTPUT): $(SOURCES) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(WASM_FLAGS) $(SOURCES) -o $@
	@echo "Built: $@"
	@ls -lh $(BUILD_DIR)/demo.*

# Development server
serve: all
	@echo "Starting development server at http://localhost:8000"
	@python3 -m http.server 8000 || python -m SimpleHTTPServer 8000

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)

# Help
help:
	@echo "ClayShards Demo Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build WASM module (default)"
	@echo "  serve   - Build and start development server"
	@echo "  clean   - Remove build artifacts"
	@echo ""
	@echo "Requirements:"
	@echo "  - Emscripten SDK (emcc)"
	@echo ""
	@echo "Usage:"
	@echo "  make        # Build WASM"
	@echo "  make serve  # Build and serve at localhost:8000"
