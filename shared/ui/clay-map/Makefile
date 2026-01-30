# Clay Map Viewer - Makefile
# Builds WASM module for browser-based map viewer

# Emscripten compiler
CC = emcc
CFLAGS = -O3 -Wall -Wextra
# Note: Not defining CLAY_WASM - using regular function pointers instead

# Include paths
INCLUDES = -I../../../vendor/clay

# Don't use CLAY_WASM - we'll set measure function via Clay_SetMeasureTextFunction
# This avoids needing JS imports for measureTextFunction/queryScrollOffsetFunction

# Source files
SOURCES = src/map_ui.c

# Output
BUILD_DIR = build
OUTPUT = $(BUILD_DIR)/map_ui.wasm

# WASM export settings - use Emscripten module with JS glue
WASM_FLAGS = -s EXPORTED_FUNCTIONS='["_map_init","_map_resize","_map_set_center","_map_set_zoom","_map_get_lat","_map_get_lon","_map_get_zoom","_map_get_layer","_map_pointer_move","_map_pointer_down","_map_pointer_up","_map_scroll","_map_handle_click","_map_frame","_map_cmd_type","_map_cmd_x","_map_cmd_y","_map_cmd_w","_map_cmd_h","_map_cmd_rect_color","_map_cmd_rect_radius","_map_cmd_text_str","_map_cmd_text_len","_map_cmd_text_color","_map_cmd_text_size","_map_cmd_border_color","_map_cmd_border_radius","_map_cmd_border_width","_malloc","_free"]'
WASM_FLAGS += -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","getValue","UTF8ToString"]'
WASM_FLAGS += -s INITIAL_MEMORY=16777216
WASM_FLAGS += -s ALLOW_MEMORY_GROWTH=1
WASM_FLAGS += -s MODULARIZE=1
WASM_FLAGS += -s EXPORT_NAME='createMapModule'
WASM_FLAGS += -s ENVIRONMENT='web'

.PHONY: all clean serve help

all: $(OUTPUT)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Build outputs both .js and .wasm
$(BUILD_DIR)/map_ui.js: $(SOURCES) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(WASM_FLAGS) $(SOURCES) -o $@
	@echo "Built: $@ and $(BUILD_DIR)/map_ui.wasm"
	@ls -lh $(BUILD_DIR)/map_ui.*

$(OUTPUT): $(BUILD_DIR)/map_ui.js

# Development server
serve: all
	@echo "Starting development server at http://localhost:8000"
	@python3 -m http.server 8000 || python -m SimpleHTTPServer 8000

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)

# Help
help:
	@echo "Clay Map Viewer Build System"
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
