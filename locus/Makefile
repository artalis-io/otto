# Locus Makefile
# Zero-dependency geocoding library

CC ?= gcc

# Optimization flags
SIMD_FLAGS = -march=native -ftree-vectorize -ffast-math
CFLAGS = -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE $(SIMD_FLAGS)
LDFLAGS = -lm

# Include paths
INCLUDES = -Iinclude -I../shared/include -I../vendor/miniz

# Shared library path
SHARED_DIR = ../shared

# Debug build
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -std=c11 -D_GNU_SOURCE -DDEBUG

# Source files
CORE_SRCS = src/locus.c \
            src/lc_types.c \
            src/lc_pbf.c

# Object files
CORE_OBJS = $(CORE_SRCS:.c=.o)

# Library
LIB = liblocus.a

# Test binary
TEST_BIN = test_locus

# Benchmark binary
BENCH_BIN = bench_locus

# WASM output
WASM_OUT = locus.js

.PHONY: all clean test bench lib debug wasm

# Default target
all: lib $(TEST_BIN)

# Build library
lib: $(LIB)

$(LIB): $(CORE_OBJS) $(SHARED_DIR)/libshared.a
	ar rcs $@ $(CORE_OBJS)

# Compile rules
src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build shared library first
$(SHARED_DIR)/libshared.a:
	$(MAKE) -C $(SHARED_DIR) lib

# Test binary
$(TEST_BIN): tests/test_locus.c $(LIB) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -llocus -L$(SHARED_DIR) -lshared $(LDFLAGS) -o $@

# Run tests
test: $(TEST_BIN)
	./$(TEST_BIN)

# Benchmarks
$(BENCH_BIN): benchmarks/bench_locus.c $(LIB) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -llocus -L$(SHARED_DIR) -lshared $(LDFLAGS) -o $@

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
	rm -f $(TEST_BIN) $(BENCH_BIN)
	rm -f $(WASM_OUT) locus.wasm

# Dependencies
src/locus.o: include/locus.h include/lc_types.h include/lc_pbf.h
src/lc_types.o: include/lc_types.h
src/lc_pbf.o: include/lc_types.h include/lc_pbf.h $(SHARED_DIR)/include/sh_protobuf.h $(SHARED_DIR)/include/sh_inflate.h $(SHARED_DIR)/include/sh_pbf.h

# Help
help:
	@echo "Locus Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build library and tests (default)"
	@echo "  lib      - Build static library (liblocus.a)"
	@echo "  test     - Build and run tests"
	@echo "  bench    - Build and run benchmarks"
	@echo "  debug    - Build with debug symbols"
	@echo "  wasm     - Build WebAssembly (requires Emscripten)"
	@echo "  clean    - Remove build artifacts"
	@echo ""
	@echo "Examples:"
	@echo "  make                    # Build library and tests"
	@echo "  make test               # Run test suite"
