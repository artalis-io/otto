# Carta Makefile
# Zero-dependency map tile generator

CC ?= gcc

# Optimization flags
SIMD_FLAGS = -march=native -ftree-vectorize -ffast-math
CFLAGS = -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE $(SIMD_FLAGS)
LDFLAGS = -lm

# Include paths
INCLUDES = -Iinclude -I../shared/include -I../vendor/miniz

# Vendor path
VENDOR_DIR = ../vendor/miniz

# Shared library path
SHARED_DIR = ../shared

# Debug build
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -std=c11 -D_GNU_SOURCE -DDEBUG

# Source files (removed ct_protobuf.c and ct_inflate.c - now in shared)
CORE_SRCS = src/carta.c \
            src/ct_tile.c \
            src/ct_pbf.c \
            src/ct_mvt.c \
            src/ct_render.c \
            src/ct_png.c \
            src/ct_style.c \
            src/ct_lod.c \
            src/ct_simplify.c \
            src/ct_rtree.c

# miniz is now linked via shared library
MINIZ_SRCS =

# Object files
CORE_OBJS = $(CORE_SRCS:.c=.o)
MINIZ_OBJS = $(MINIZ_SRCS:.c=.o)
ALL_OBJS = $(CORE_OBJS) $(MINIZ_OBJS)

# Library
LIB = libcarta.a

# Test binary
TEST_BIN = test_carta

# CLI tool
CLI_BIN = carta_cli

# Benchmark binary
BENCH_BIN = bench_carta

# WASM output
WASM_OUT = carta.js

.PHONY: all clean test bench lib debug wasm cli

# Default target
all: lib $(TEST_BIN)

# Build library
lib: $(LIB)

$(LIB): $(ALL_OBJS) $(SHARED_DIR)/libshared.a
	ar rcs $@ $(ALL_OBJS)

# Compile rules
src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build shared library first
$(SHARED_DIR)/libshared.a:
	$(MAKE) -C $(SHARED_DIR) lib

# Test binary
$(TEST_BIN): tests/test_carta.c $(LIB) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lcarta -L$(SHARED_DIR) -lshared $(LDFLAGS) -o $@

# CLI tool
$(CLI_BIN): src/cli.c $(LIB) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lcarta -L$(SHARED_DIR) -lshared $(LDFLAGS) -o $@

cli: $(CLI_BIN)

# Run tests
test: $(TEST_BIN)
	./$(TEST_BIN)

# Benchmarks
$(BENCH_BIN): benchmarks/bench_carta.c $(LIB) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lcarta -L$(SHARED_DIR) -lshared $(LDFLAGS) -o $@

bench: $(BENCH_BIN)
	./$(BENCH_BIN)

# Debug build
debug: CFLAGS = $(DEBUG_CFLAGS)
debug: clean all

# WASM build (delegates to wasm/ subdirectory)
wasm:
	$(MAKE) -C wasm

# Clean
clean:
	rm -f $(CORE_OBJS) $(LIB)
	rm -f $(TEST_BIN) $(CLI_BIN) $(BENCH_BIN)
	rm -f $(WASM_OUT) carta.wasm

# Dependencies
src/carta.o: include/carta.h include/ct_types.h
src/ct_tile.o: include/ct_types.h include/ct_tile.h
src/ct_pbf.o: include/ct_types.h include/ct_pbf.h include/ct_lod.h include/ct_rtree.h
src/ct_mvt.o: include/ct_types.h include/ct_mvt.h
src/ct_render.o: include/ct_types.h include/ct_render.h include/ct_lod.h include/ct_simplify.h
src/ct_png.o: include/ct_types.h include/ct_png.h
src/ct_style.o: include/ct_types.h
src/ct_lod.o: include/ct_types.h include/ct_lod.h
src/ct_simplify.o: include/ct_types.h include/ct_simplify.h
src/ct_rtree.o: include/ct_types.h include/ct_rtree.h

# Help
help:
	@echo "Carta Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build library and tests (default)"
	@echo "  lib      - Build static library (libcarta.a)"
	@echo "  test     - Build and run tests"
	@echo "  bench    - Build and run benchmarks"
	@echo "  cli      - Build command-line tool"
	@echo "  debug    - Build with debug symbols"
	@echo "  wasm     - Build WebAssembly (requires Emscripten)"
	@echo "  clean    - Remove build artifacts"
	@echo ""
	@echo "Examples:"
	@echo "  make                      # Build library and tests"
	@echo "  make test                 # Run test suite"
	@echo "  make cli && ./carta_cli map.osm.pbf --zoom 14"
