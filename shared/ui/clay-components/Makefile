# Clay Components Library

CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c99
INCLUDES = -I../../../vendor/clay -Iinclude

# For WASM build
EMCC = emcc
WASM_CFLAGS = -O3 -Wall -Wextra

SOURCES = src/clay_components.c
HEADERS = include/clay_components.h

BUILD_DIR = build

.PHONY: all clean lib wasm

all: lib

# Static library for native builds
lib: $(BUILD_DIR)/libclay_components.a

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/clay_components.o: $(SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $(SOURCES) -o $@

$(BUILD_DIR)/libclay_components.a: $(BUILD_DIR)/clay_components.o
	ar rcs $@ $<
	@echo "Built: $@"

# WASM object for inclusion in WASM builds
wasm: $(BUILD_DIR)/clay_components.wasm.o

$(BUILD_DIR)/clay_components.wasm.o: $(SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(EMCC) $(WASM_CFLAGS) $(INCLUDES) -c $(SOURCES) -o $@
	@echo "Built: $@"

clean:
	rm -rf $(BUILD_DIR)

# Install headers (for use by other projects)
install-headers:
	@echo "Headers are in include/ - add -I path/to/clay-components/include to your build"
