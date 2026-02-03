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

# Source files (includes immediate mode components and provider)
SOURCES = src/demo.c \
          ../clay-shards/src/cs_immediate.c \
          ../clay-shards/src/cs_map_provider.c

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
  "_map_get_component_id","_map_get_width","_map_get_height","_map_get_show_tile_info","_map_get_attribution_clicked",\
  "_map_pointer_move","_map_pointer_down","_map_pointer_up","_map_scroll",\
  "_map_handle_click","_map_frame",\
  "_map_debug_has_start","_map_debug_start_lat","_map_debug_start_lon",\
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
  "_cs_map_overlay_marker_border_width","_cs_map_overlay_marker_draggable",\
  "_cs_map_hit_test","_cs_map_set_hovered_overlay","_cs_map_set_clicked_overlay",\
  "_cs_map_get_hovered_overlay","_cs_map_get_dragging_overlay",\
  "_cs_provider_tile_url","_cs_provider_has_direct_tiles",\
  "_cs_provider_set_route_profile","_cs_provider_get_route_profile",\
  "_cs_provider_set_route_mode","_cs_provider_get_route_mode",\
  "_cs_provider_route","_cs_provider_route_status","_cs_provider_route_ready",\
  "_cs_provider_route_count","_cs_provider_route_point","_cs_provider_route_clear",\
  "_cs_provider_route_is_pending","_cs_provider_route_from_lat","_cs_provider_route_from_lon",\
  "_cs_provider_route_to_lat","_cs_provider_route_to_lon","_cs_provider_route_mark_fetching",\
  "_cs_provider_route_server","_cs_provider_route_distance","_cs_provider_route_duration","_cs_provider_route_calc_time",\
  "_cs_provider_route_point_lat","_cs_provider_route_point_lon",\
  "_cs_provider_search","_cs_provider_search_near","_cs_provider_search_status",\
  "_cs_provider_search_ready","_cs_provider_search_count","_cs_provider_search_clear",\
  "_cs_provider_search_is_pending","_cs_provider_search_query","_cs_provider_search_has_bias",\
  "_cs_provider_search_bias_lat","_cs_provider_search_bias_lon","_cs_provider_search_mark_fetching",\
  "_cs_provider_geocode_server","_cs_provider_set_search_result","_cs_provider_on_search_complete",\
  "_cs_provider_on_search_error","_cs_provider_search_result_lat","_cs_provider_search_result_lon",\
  "_cs_provider_search_result_name","_cs_provider_search_result_type","_cs_provider_search_result_score",\
  "_cs_provider_reverse","_cs_provider_reverse_status","_cs_provider_reverse_ready",\
  "_cs_provider_reverse_clear","_cs_provider_reverse_is_pending","_cs_provider_reverse_lat",\
  "_cs_provider_reverse_lon","_cs_provider_reverse_mark_fetching",\
  "_cs_provider_on_reverse_complete","_cs_provider_on_reverse_error",\
  "_cs_provider_reverse_name","_cs_provider_reverse_street","_cs_provider_reverse_city","_cs_provider_reverse_country",\
  "_cs_provider_on_route_complete","_cs_provider_on_route_error",\
  "_cs_provider_set_tile_server","_cs_provider_set_route_server","_cs_provider_set_geocode_server",\
  "_cs_provider_type","_malloc","_free"\
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
