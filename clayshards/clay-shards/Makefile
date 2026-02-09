# ClayShards Library - Immediate Mode UI Components
#
# Thin immediate mode components on top of Clay layout.
# Include cs_immediate.c in your WASM build directly.

CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c99
INCLUDES = -Iinclude -I../../shared/include -I../../vendor/clay

# Shared library for polyline simplification
SHARED_DIR = ../../shared
SHARED_LIB = $(SHARED_DIR)/libshared.a

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

# Ensure shared library is built
$(SHARED_LIB):
	$(MAKE) -C $(SHARED_DIR) lib

# Note: cs_immediate.c is an amalgamation file that includes cs_map.c, so we only compile cs_immediate.c
# cs_map_provider.c is separate (only for API-based builds)
$(BUILD_DIR)/test_clay_shards: tests/test_clay_shards.c src/cs_immediate.c src/cs_map_provider.c $(HEADERS) $(SHARED_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) tests/test_clay_shards.c src/cs_immediate.c src/cs_map_provider.c -L$(SHARED_DIR) -lshared -o $@ -lm
	@echo "Built: $@"
