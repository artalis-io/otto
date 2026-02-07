# ClayShards TUI WASM Demo
#
# Builds the TUI demo as a standalone WASM module.

CC = emcc
CFLAGS = -O3 -Wall -Wextra

# Include paths (from demos/wasm/)
INCLUDES = -I../../clay-shards-tui/include \
           -I../../clay-shards-tui/src \
           -I../../clay-shards/include \
           -I../../../vendor/clay

# Demo sources
SOURCES = src/demo_tui_wasm.c \
          ../../clay-shards-tui/src/cs_tui.c \
          ../../clay-shards/src/cs_immediate.c

# Output
BUILD_DIR = build
OUTPUT = $(BUILD_DIR)/demo.wasm

# WASM settings
WASM_FLAGS = -s STANDALONE_WASM=1

EXPORTS = -s EXPORTED_FUNCTIONS='[\
  "_demo_init","_demo_resize",\
  "_demo_get_buffer","_demo_get_width","_demo_get_height","_demo_get_cell_size",\
  "_demo_frame","_demo_key_down","_demo_key_char",\
  "_demo_mouse_move","_demo_mouse_click","_demo_mouse_release",\
  "_demo_get_counter","_demo_get_volume","_demo_get_priority",\
  "_demo_get_enabled","_demo_get_notifications","_demo_get_darkmode",\
  "_demo_get_quit","_demo_reset_quit",\
  "_malloc","_free"\
]'

MEM_FLAGS = -s INITIAL_MEMORY=16777216 \
            -s ALLOW_MEMORY_GROWTH=1 \
            -s MAXIMUM_MEMORY=16777216 \
            --no-entry

.PHONY: all demo clean help

all: demo

demo: $(OUTPUT)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(OUTPUT): $(SOURCES) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(WASM_FLAGS) $(EXPORTS) $(MEM_FLAGS) $(SOURCES) -o $@
	@echo "Built: $@"
	@ls -lh $@

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "ClayShards TUI WASM Demo"
	@echo ""
	@echo "Targets:"
	@echo "  all/demo - Build demo (default)"
	@echo "  clean    - Remove build artifacts"
	@echo ""
	@echo "Output: $(OUTPUT)"
	@echo ""
	@echo "Run: make tui-demo-serve (from project root)"
