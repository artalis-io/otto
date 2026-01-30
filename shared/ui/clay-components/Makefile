# Clay Components Library - Immediate Mode
#
# Thin immediate mode components on top of Clay layout.
# Include cc_immediate.c in your WASM build directly.

CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c99
INCLUDES = -Iinclude -I../../../vendor/clay

EMCC = emcc
WASM_CFLAGS = -O3 -Wall -Wextra

SOURCES = src/cc_immediate.c
HEADERS = $(wildcard include/*.h)

BUILD_DIR = build

.PHONY: all clean lib wasm

all: lib

# Static library for native builds
lib: $(BUILD_DIR)/libclay_components.a

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/cc_immediate.o: src/cc_immediate.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/libclay_components.a: $(BUILD_DIR)/cc_immediate.o
	ar rcs $@ $^
	@echo "Built: $@"

# WASM objects for inclusion in WASM builds
wasm: $(BUILD_DIR)/cc_immediate.wasm.o

$(BUILD_DIR)/cc_immediate.wasm.o: src/cc_immediate.c $(HEADERS) | $(BUILD_DIR)
	$(EMCC) $(WASM_CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

# Run component tests
test: $(BUILD_DIR)/test_immediate
	@echo "Running immediate mode tests..."
	@./$(BUILD_DIR)/test_immediate

$(BUILD_DIR)/test_immediate: tests/test_immediate.c src/cc_immediate.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) tests/test_immediate.c src/cc_immediate.c -o $@ -lm
	@echo "Built: $@"
