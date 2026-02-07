# ClayShards TUI TTY Demo
#
# Native terminal demo using the TUI renderer.

CC ?= gcc
CFLAGS = -O2 -Wall -Wextra -Werror -std=c11

# Include paths (from demos/tty/)
INCLUDES = -I../../clay-shards-tui/include \
           -I../../clay-shards/include \
           -I../../../vendor/clay

# Debug build
DEBUG_CFLAGS = -g -O0 -fsanitize=address,undefined

# Sources
DEMO_SRC = src/demo_tui_tty.c
TUI_SRC = ../../clay-shards-tui/src/cs_tui.c
CS_SRC = ../../clay-shards/src/cs_immediate.c

BUILD_DIR = build

.PHONY: all clean debug help

all: $(BUILD_DIR)/demo_tui_tty

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/demo_tui_tty: $(DEMO_SRC) $(TUI_SRC) $(CS_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(DEMO_SRC) $(TUI_SRC) $(CS_SRC) -o $@ -lm
	@echo "Built: $@"
	@echo "Run with: ./$@"

# Debug build with sanitizers
debug: $(BUILD_DIR)/demo_tui_tty_debug

$(BUILD_DIR)/demo_tui_tty_debug: $(DEMO_SRC) $(TUI_SRC) $(CS_SRC) | $(BUILD_DIR)
	$(CC) $(DEBUG_CFLAGS) $(INCLUDES) $(DEMO_SRC) $(TUI_SRC) $(CS_SRC) -o $@ -lm
	@echo "Built: $@ (with sanitizers)"

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "ClayShards TUI TTY Demo"
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build demo (default)"
	@echo "  debug   - Build with sanitizers"
	@echo "  clean   - Remove build artifacts"
