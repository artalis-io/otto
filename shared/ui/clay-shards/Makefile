# ClayShards Library - Immediate Mode UI Components
#
# Thin immediate mode components on top of Clay layout.
# Include cs_immediate.c in your WASM build directly.

CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c99
INCLUDES = -Iinclude -I../../../vendor/clay

EMCC = emcc
WASM_CFLAGS = -O3 -Wall -Wextra

SOURCES = src/cs_immediate.c
HEADERS = $(wildcard include/*.h)

BUILD_DIR = build

.PHONY: all clean lib wasm test

all: lib

# Static library for native builds
lib: $(BUILD_DIR)/libclay_shards.a

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/cs_immediate.o: src/cs_immediate.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/libclay_shards.a: $(BUILD_DIR)/cs_immediate.o
	ar rcs $@ $^
	@echo "Built: $@"

# WASM objects for inclusion in WASM builds
wasm: $(BUILD_DIR)/cs_immediate.wasm.o

$(BUILD_DIR)/cs_immediate.wasm.o: src/cs_immediate.c $(HEADERS) | $(BUILD_DIR)
	$(EMCC) $(WASM_CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

# Run component tests
test: $(BUILD_DIR)/test_clay_shards
	@echo "Running ClayShards tests..."
	@./$(BUILD_DIR)/test_clay_shards

$(BUILD_DIR)/test_clay_shards: tests/test_clay_shards.c src/cs_immediate.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) tests/test_clay_shards.c src/cs_immediate.c -o $@ -lm
	@echo "Built: $@"
