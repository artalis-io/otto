# ClayShards Library - Immediate Mode UI Components
#
# Thin immediate mode components on top of Clay layout.
# Include cs_immediate.c in your WASM build directly.

# Every compiler difference lives in one place; see mk/toolchain.mk.
include ../../mk/toolchain.mk

CC = gcc
# c99 rather than the toolchain's c11 default: this library is deliberately
# buildable by anything, and nothing in it needs c11.
CFLAGS = $(strip $(CC_OPT_O2) $(CC_WARN) $(CC_STD_C99) $(CC_PORT_DEFS))
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
	$(CC) $(CFLAGS) $(INCLUDES) -c $< $(OBJ_OUT)$@

$(BUILD_DIR)/libclay_shards.a: $(BUILD_DIR)/cs_immediate.o
	$(AR_CMD)$@ $^
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
	$(CC) $(CFLAGS) $(INCLUDES) tests/test_clay_shards.c src/cs_immediate.c src/cs_map_provider.c $(call link_lib,$(SHARED_DIR),shared) $(LD_MATH) $(LD_PLATFORM) $(EXE_OUT)$@
	@echo "Built: $@"
