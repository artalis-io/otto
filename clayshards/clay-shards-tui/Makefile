# ClayShards TUI Renderer
#
# Text UI renderer that processes Clay render commands and outputs
# ANSI escape sequences for modern terminal emulators.

CC ?= gcc
CFLAGS = -O2 -Wall -Wextra -Werror -std=c11
INCLUDES = -Iinclude -I../../vendor/clay -I../clay-shards/include

SOURCES = src/cs_tui.c
HEADERS = include/cs_tui.h src/cs_tui_internal.h

BUILD_DIR = build

.PHONY: all clean lib test

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

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "ClayShards TUI Renderer (Library)"
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build library (default)"
	@echo "  lib     - Build static library"
	@echo "  test    - Build and run unit tests"
	@echo "  clean   - Remove build artifacts"
	@echo ""
	@echo "Demos are in ../demos/{tty,wasm}/"
