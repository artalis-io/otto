# ClayShards TUI Renderer
#
# Text UI renderer that processes Clay render commands and outputs
# ANSI escape sequences for modern terminal emulators.

CC ?= gcc
CFLAGS = -O2 -Wall -Wextra -Werror -std=c11
INCLUDES = -Iinclude -I../../vendor/clay -I../clay-shards/include

# Debug build
DEBUG_CFLAGS = -g -O0 -fsanitize=address,undefined

SOURCES = src/cs_tui.c
HEADERS = include/cs_tui.h src/cs_tui_internal.h

BUILD_DIR = build

.PHONY: all clean lib test demo debug

all: lib

# Static library
lib: $(BUILD_DIR)/libcs_tui.a

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/cs_tui.o: src/cs_tui.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/libcs_tui.a: $(BUILD_DIR)/cs_tui.o
	ar rcs $@ $^
	@echo "Built: $@"

# Unit tests
test: $(BUILD_DIR)/test_tui
	@echo "Running ClayShards TUI tests..."
	@./$(BUILD_DIR)/test_tui

$(BUILD_DIR)/test_tui: tests/test_tui.c src/cs_tui.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) tests/test_tui.c src/cs_tui.c -o $@
	@echo "Built: $@"

# Demo application
demo: $(BUILD_DIR)/demo_tui

$(BUILD_DIR)/demo_tui: demo/demo_tui.c src/cs_tui.c ../clay-shards/src/cs_immediate.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -I../clay-shards/include \
		demo/demo_tui.c src/cs_tui.c ../clay-shards/src/cs_immediate.c \
		-o $@ -lm
	@echo "Built: $@"
	@echo "Run with: ./$(BUILD_DIR)/demo_tui"

# Debug build with sanitizers
debug: $(BUILD_DIR)/demo_tui_debug

$(BUILD_DIR)/demo_tui_debug: demo/demo_tui.c src/cs_tui.c ../clay-shards/src/cs_immediate.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(DEBUG_CFLAGS) $(INCLUDES) -I../clay-shards/include \
		demo/demo_tui.c src/cs_tui.c ../clay-shards/src/cs_immediate.c \
		-o $@ -lm
	@echo "Built: $@ (with sanitizers)"

clean:
	rm -rf $(BUILD_DIR)

# Help
help:
	@echo "ClayShards TUI Renderer"
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build library (default)"
	@echo "  lib     - Build static library"
	@echo "  test    - Build and run unit tests"
	@echo "  demo    - Build demo application"
	@echo "  debug   - Build demo with sanitizers"
	@echo "  clean   - Remove build artifacts"
