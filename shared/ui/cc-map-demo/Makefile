# cc_map Demo - Makefile
# Builds WASM module for cc_map demo

# Emscripten compiler
CC = emcc
CFLAGS = -O3 -Wall -Wextra
# Note: Not defining CLAY_WASM - using regular function pointers instead

# Include paths - Clay vendor + clay_components
INCLUDES = -I../../../vendor/clay -I../clay-components/include

# Don't use CLAY_WASM - we'll set measure function via Clay_SetMeasureTextFunction
# This avoids needing JS imports for measureTextFunction/queryScrollOffsetFunction

# Source files (includes immediate mode components)
SOURCES = src/map_ui.c \
          ../clay-components/src/cc_immediate.c

# Output
BUILD_DIR = build
OUTPUT = $(BUILD_DIR)/map_ui.wasm

# WASM export settings - standalone WASM, no JS glue
WASM_FLAGS = -s STANDALONE_WASM=1

# Exports are organized by module:
#   map_*      - Domain-specific (this app)
#   cc_clay_*  - Generic Clay integration (clay-components)
#   cc_*       - Immediate mode components (clay-components)
WASM_FLAGS += -s EXPORTED_FUNCTIONS='[\
  "_map_init","_map_resize","_map_set_center","_map_set_zoom",\
  "_map_get_lat","_map_get_lon","_map_get_zoom","_map_get_layer",\
  "_map_pointer_move","_map_pointer_down","_map_pointer_up","_map_scroll",\
  "_map_handle_click","_map_frame",\
  "_cc_clay_is_initialized","_cc_clay_get_error",\
  "_cc_clay_cmd_count","_cc_clay_cmd_type",\
  "_cc_clay_cmd_x","_cc_clay_cmd_y","_cc_clay_cmd_w","_cc_clay_cmd_h",\
  "_cc_clay_cmd_rect_color","_cc_clay_cmd_rect_radius",\
  "_cc_clay_cmd_text_str","_cc_clay_cmd_text_len","_cc_clay_cmd_text_color","_cc_clay_cmd_text_size",\
  "_cc_clay_cmd_border_color","_cc_clay_cmd_border_radius","_cc_clay_cmd_border_width",\
  "_cc_focused_id","_cc_cursor_pos","_cc_selection_start","_cc_cursor_visible",\
  "_cc_focused_text","_cc_focused_text_len",\
  "_cc_focused_x","_cc_focused_y","_cc_focused_w","_cc_focused_h",\
  "_cc_key_down","_cc_key_char","_cc_blur","_cc_set_pending_click",\
  "_cc_clay_set_glyph_advance","_cc_clay_has_font_metrics"\
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
	@ls -lh $(BUILD_DIR)/map_ui.*

# Development server
serve: all
	@echo "Starting development server at http://localhost:8000"
	@python3 -m http.server 8000 || python -m SimpleHTTPServer 8000

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)

# Help
help:
	@echo "cc_map Demo Build System"
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
