# Locus Makefile
# Zero-dependency geocoding library

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
CFLAGS = -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE $(SIMD_FLAGS) \
         -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -fno-common
ifeq ($(UNAME_S),Linux)
    LDFLAGS = -lm -pie $(RELRO_FLAGS)
else
    LDFLAGS = -lm
endif

# libshared now reaches the platform through sh_pal: bcrypt supplies
# sh_pal_random_bytes, ws2_32 the StatsD sink in sh_metrics.
ifneq (,$(findstring MINGW,$(UNAME_S)))
    LDFLAGS += -lbcrypt -lws2_32
endif

# Include paths
INCLUDES = -Iinclude -I../shared/include -I../vendor/miniz

# Shared library path
SHARED_DIR = ../shared

# Debug build (with sanitizers)
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -std=c11 -D_GNU_SOURCE -DDEBUG \
               -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS = -fsanitize=address,undefined

# Source files
CORE_SRCS = src/locus.c \
            src/lc_types.c \
            src/lc_pbf.c \
            src/lc_normalize.c \
            src/lc_trie.c \
            src/lc_ngram.c \
            src/lc_spatial.c \
            src/lc_index.c \
            src/lc_serialize.c \
            src/lc_query.c \
            src/lc_api.c

# Object files
CORE_OBJS = $(CORE_SRCS:.c=.o)

# Library
LIB = liblocus.a

# Test binary
TEST_BIN = test_locus
TEST_TRANSPORT_BIN = test_transport_direct

# Benchmark binary
BENCH_BIN = bench_locus

# WASM output
WASM_OUT = locus.js

.PHONY: all clean test test-transport bench lib debug wasm

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

# Transport invariant: lc_api_handle driven by sh_transport_direct with no
# transport library on the include path. See docs/roadmaps/transport.md.
$(TEST_TRANSPORT_BIN): tests/test_transport_direct.c $(LIB) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -llocus -L$(SHARED_DIR) -lshared $(LDFLAGS) -o $@

test-transport: $(TEST_TRANSPORT_BIN)
	./$(TEST_TRANSPORT_BIN)

# Run tests
test: $(TEST_BIN) $(TEST_TRANSPORT_BIN)
	./$(TEST_BIN)
	./$(TEST_TRANSPORT_BIN)

# Benchmarks
$(BENCH_BIN): benchmarks/bench_locus.c $(LIB) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -llocus -L$(SHARED_DIR) -lshared $(LDFLAGS) -o $@

bench: $(BENCH_BIN)
	./$(BENCH_BIN)

# Debug build (with sanitizers)
debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS = -lm $(DEBUG_LDFLAGS)
debug: clean all

# WASM build (delegates to wasm/ subdirectory)
wasm:
	$(MAKE) -C wasm

# Clean
clean:
	rm -f $(CORE_OBJS) $(LIB)
	rm -f $(TEST_BIN) $(TEST_TRANSPORT_BIN) $(BENCH_BIN)
	rm -f $(WASM_OUT) locus.wasm

# Dependencies
src/locus.o: include/locus.h include/lc_types.h include/lc_pbf.h
src/lc_types.o: include/lc_types.h
src/lc_pbf.o: include/lc_types.h include/lc_pbf.h $(SHARED_DIR)/include/sh_protobuf.h $(SHARED_DIR)/include/sh_inflate.h $(SHARED_DIR)/include/sh_pbf.h
src/lc_api.o: include/lc_api.h include/locus.h

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
