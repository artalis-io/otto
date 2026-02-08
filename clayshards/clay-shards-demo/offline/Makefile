# ClayShards Offline Demo - Makefile
# Builds WASM module with embedded map provider for offline routing/geocoding

# Emscripten compiler
CC = emcc
CFLAGS = -O3 -Wall -Wextra

# Paths
ROOT = ../../..
CLAY_SHARDS = ../../clay-shards
VELO = $(ROOT)/velo
LOCUS = $(ROOT)/locus
CARTA = $(ROOT)/carta
SHARED = $(ROOT)/shared
VENDOR = $(ROOT)/vendor

# Include paths
INCLUDES = -I$(ROOT)/vendor/clay \
           -I$(CLAY_SHARDS)/include \
           -I$(VELO)/include \
           -I$(LOCUS)/include \
           -I$(CARTA)/include \
           -I$(SHARED)/include \
           -I$(VENDOR)/miniz

# Source files
DEMO_SOURCES = ../src/demo.c \
               $(CLAY_SHARDS)/src/cs_immediate.c \
               $(CLAY_SHARDS)/src/cs_map_provider.c

# Output
BUILD_DIR = build
DATA_DIR = data
OUTPUT = $(BUILD_DIR)/demo.wasm

# Data files (download from Geofabrik)
REGION ?= monaco
PBF_FILE = $(DATA_DIR)/$(REGION)-latest.osm.pbf
VLG_FILE = $(DATA_DIR)/$(REGION).vlg

# WASM export settings
WASM_FLAGS = -s STANDALONE_WASM=1
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
  "_cs_map_set_hovered_overlay","_cs_map_set_clicked_overlay",\
  "_cs_provider_route_is_pending","_cs_provider_route_mark_fetching",\
  "_cs_provider_route_from_lat","_cs_provider_route_from_lon",\
  "_cs_provider_route_to_lat","_cs_provider_route_to_lon",\
  "_cs_provider_on_route_complete","_cs_provider_on_route_error",\
  "_cs_provider_route_status","_cs_provider_route_ready",\
  "_cs_provider_route_count","_cs_provider_route_clear",\
  "_cs_provider_route_distance","_cs_provider_route_duration",\
  "_cs_provider_route_point_lat","_cs_provider_route_point_lon",\
  "_malloc","_free"\
]'
WASM_FLAGS += -s INITIAL_MEMORY=16777216
WASM_FLAGS += -s ALLOW_MEMORY_GROWTH=1
WASM_FLAGS += --no-entry

.PHONY: all clean data velo-wasm locus-wasm carta-wasm wasm-modules serve help

all: $(OUTPUT)

# Create directories
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(DATA_DIR):
	mkdir -p $(DATA_DIR)

# Download OSM data
$(PBF_FILE): | $(DATA_DIR)
	@echo "Downloading $(REGION) OSM data..."
	cd $(ROOT) && ./scripts/data-download-osm.sh $(REGION)
	mv $(ROOT)/data/$(REGION)-latest.osm.pbf $(PBF_FILE) 2>/dev/null || \
		cp $(ROOT)/data/$(REGION)-latest.osm.pbf $(PBF_FILE)

# Build Velo graph from PBF using bench_pbf
$(VLG_FILE): $(PBF_FILE)
	@echo "Building Velo graph..."
	$(MAKE) -C $(VELO) bench
	$(VELO)/build/bench_pbf $(PBF_FILE) $(VLG_FILE)

# Download and process data
data: $(VLG_FILE)

# Build individual WASM modules
velo-wasm:
	$(MAKE) -C $(VELO)/wasm

locus-wasm:
	$(MAKE) -C $(LOCUS)/wasm

carta-wasm:
	$(MAKE) -C $(CARTA)/wasm

wasm-modules: velo-wasm locus-wasm carta-wasm

# Build demo WASM (uses API provider - for WASM provider, load separate modules)
$(OUTPUT): $(DEMO_SOURCES) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(WASM_FLAGS) $(DEMO_SOURCES) -o $@
	@echo "Built: $@"
	@ls -lh $(BUILD_DIR)/demo.*

# Development server
serve: all
	@echo "Starting development server at http://localhost:8000"
	@echo "For offline mode, also run: make data wasm-modules REGION=$(REGION)"
	@python3 -m http.server 8000 || python -m SimpleHTTPServer 8000

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR)

# Help
help:
	@echo "ClayShards Offline Demo Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all           - Build demo WASM module (default)"
	@echo "  data          - Download OSM data and build Velo graph"
	@echo "  wasm-modules  - Build Velo/Locus/Carta WASM modules"
	@echo "  serve         - Build and start development server"
	@echo "  clean         - Remove build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  REGION=monaco - Region to download (default: monaco)"
	@echo "                  Options: monaco, hungary, austria, etc."
	@echo ""
	@echo "Full offline setup:"
	@echo "  make data wasm-modules REGION=hungary"
	@echo "  make serve"
	@echo ""
	@echo "Quick test with Monaco (small ~500KB):"
	@echo "  make data wasm-modules REGION=monaco"
	@echo "  make serve"
