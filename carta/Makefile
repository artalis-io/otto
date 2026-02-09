# Carta Makefile
# Zero-dependency map tile generator

CC ?= gcc

# Optimization flags
SIMD_FLAGS = -march=native -ftree-vectorize -ffast-math

# Platform-specific RELRO (Linux only)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    RELRO_FLAGS = -Wl,-z,relro,-z,now
else
    RELRO_FLAGS =
endif

# Production build (with hardening)
# Note: -pie is implicit on macOS, only add on Linux
CFLAGS = -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE -DNDEBUG $(SIMD_FLAGS) \
         -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -fno-common
ifeq ($(UNAME_S),Linux)
    LDFLAGS = -lm -pie $(RELRO_FLAGS)
else
    LDFLAGS = -lm
endif

# Include paths
INCLUDES = -Iinclude -I../shared/include -I../vendor/miniz

# Vendor path
VENDOR_DIR = ../vendor/miniz

# Shared library path
SHARED_DIR = ../shared

# Debug build (with sanitizers)
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -std=c11 -D_GNU_SOURCE -DDEBUG \
               -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS = -fsanitize=address,undefined

# Source files (removed ct_protobuf.c and ct_inflate.c - now in shared)
CORE_SRCS = src/carta.c \
            src/ct_tile.c \
            src/ct_pbf.c \
            src/ct_mvt.c \
            src/ct_render.c \
            src/ct_png.c \
            src/ct_ascii.c \
            src/ct_style.c \
            src/ct_lod.c \
            src/ct_simplify.c \
            src/ct_rtree.c \
            src/ct_cache.c \
            src/ct_serialize.c \
            src/ct_multipolygon.c \
            src/ct_boundary.c \
            src/ct_collision.c \
            src/ct_label.c \
            src/ct_api.c

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

# Comparison tool
COMPARE_BIN = carta-compare

# Benchmark binary
BENCH_BIN = bench_carta

# WASM output
WASM_OUT = carta.js

.PHONY: all clean test bench lib debug wasm cli compare fuzz

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

# Comparison tool (requires libcurl)
$(COMPARE_BIN): tools/carta_compare.c $(LIB) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lcarta -L$(SHARED_DIR) -lshared -lcurl $(LDFLAGS) -o $@

compare: $(COMPARE_BIN)

# Run tests
test: $(TEST_BIN)
	./$(TEST_BIN)

# Benchmarks
$(BENCH_BIN): benchmarks/bench_carta.c $(LIB) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lcarta -L$(SHARED_DIR) -lshared $(LDFLAGS) -o $@

bench: $(BENCH_BIN)
	./$(BENCH_BIN)

# Debug build (with sanitizers)
debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS = -lm $(DEBUG_LDFLAGS)
debug: clean all

# WASM build (delegates to wasm/ subdirectory)
wasm:
	$(MAKE) -C wasm

# Fuzzing build (requires clang with libFuzzer)
FUZZ_BIN = fuzz_pbf
FUZZ_CC = clang
FUZZ_CFLAGS = -fsanitize=fuzzer,address,undefined -g -O1 -std=c11 -D_GNU_SOURCE
FUZZ_SRCS = tests/fuzz/fuzz_pbf.c

# Build fuzz target (requires rebuilt library with sanitizers)
fuzz: fuzz-lib $(FUZZ_BIN)

# Rebuild library with sanitizers for fuzzing
fuzz-lib:
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(INCLUDES) -c $(CORE_SRCS)
	ar rcs libcarta_fuzz.a $(CORE_OBJS)
	$(MAKE) -C $(SHARED_DIR) CC=$(FUZZ_CC) CFLAGS="$(FUZZ_CFLAGS)" clean lib

$(FUZZ_BIN): $(FUZZ_SRCS) fuzz-lib
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(INCLUDES) $(FUZZ_SRCS) \
		-L. -lcarta_fuzz -L$(SHARED_DIR) -lshared $(LDFLAGS) -o $@
	@echo ""
	@echo "Fuzzer built successfully. Run with:"
	@echo "  ./$(FUZZ_BIN) tests/fuzz/corpus/ -max_len=1000000"
	@echo ""
	@echo "Or run a quick smoke test:"
	@echo "  ./$(FUZZ_BIN) tests/fuzz/corpus/ -max_len=100000 -max_total_time=60"

# Clean
clean:
	rm -f $(CORE_OBJS) $(LIB) libcarta_fuzz.a
	rm -f $(TEST_BIN) $(CLI_BIN) $(COMPARE_BIN) $(BENCH_BIN) $(FUZZ_BIN)
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
src/ct_ascii.o: include/ct_types.h include/ct_ascii.h
src/ct_multipolygon.o: include/ct_types.h include/ct_multipolygon.h include/ct_rtree.h
src/ct_boundary.o: include/ct_types.h include/ct_boundary.h include/ct_rtree.h
src/ct_collision.o: include/ct_types.h include/ct_collision.h
src/ct_label.o: include/ct_types.h include/ct_label.h include/ct_collision.h include/ct_pbf.h
src/ct_api.o: include/carta.h include/ct_api.h include/ct_lod.h

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
	@echo "  compare  - Build tile comparison tool (requires libcurl)"
	@echo "  debug    - Build with debug symbols"
	@echo "  wasm     - Build WebAssembly (requires Emscripten)"
	@echo "  fuzz     - Build fuzzer (requires clang with libFuzzer)"
	@echo "  clean    - Remove build artifacts"
	@echo ""
	@echo "Examples:"
	@echo "  make                      # Build library and tests"
	@echo "  make test                 # Run test suite"
	@echo "  make cli && ./carta_cli map.osm.pbf --zoom 14"
