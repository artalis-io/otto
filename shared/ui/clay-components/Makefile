# Clay Components Library

CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c99
INCLUDES = -Iinclude

EMCC = emcc
WASM_CFLAGS = -O3 -Wall -Wextra

SOURCES = src/cc_text_input.c src/cc_button.c
HEADERS = $(wildcard include/*.h)

BUILD_DIR = build

.PHONY: all clean lib wasm

all: lib

# Static library for native builds
lib: $(BUILD_DIR)/libclay_components.a

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/cc_text_input.o: src/cc_text_input.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/cc_button.o: src/cc_button.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/libclay_components.a: $(BUILD_DIR)/cc_text_input.o $(BUILD_DIR)/cc_button.o
	ar rcs $@ $^
	@echo "Built: $@"

# WASM objects for inclusion in WASM builds
wasm: $(BUILD_DIR)/cc_text_input.wasm.o $(BUILD_DIR)/cc_button.wasm.o

$(BUILD_DIR)/cc_text_input.wasm.o: src/cc_text_input.c $(HEADERS) | $(BUILD_DIR)
	$(EMCC) $(WASM_CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/cc_button.wasm.o: src/cc_button.c $(HEADERS) | $(BUILD_DIR)
	$(EMCC) $(WASM_CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

# Test the library compiles
test: lib
	@echo "Library built successfully"
