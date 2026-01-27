# Velo Makefile
# Zero-dependency OSM routing engine

CC ?= gcc

# OpenMP and SIMD optimization flags
# -fopenmp: Enable OpenMP parallelization
# -fopenmp-simd: Enable SIMD vectorization pragmas
# -march=native: Optimize for current CPU (AVX/AVX2/etc)
# -ftree-vectorize: Auto-vectorization
# -ffast-math: Allow aggressive FP optimizations (safe for our use)
OPENMP_FLAGS = -fopenmp -fopenmp-simd
SIMD_FLAGS = -march=native -ftree-vectorize -ffast-math

CFLAGS = -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE $(OPENMP_FLAGS) $(SIMD_FLAGS)
LDFLAGS = -lm -fopenmp

# Include paths
INCLUDES = -Iinclude -Ivendor

# Debug build (no OpenMP for easier debugging)
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -std=c11 -D_GNU_SOURCE -DDEBUG

# Non-OpenMP build for compatibility
NOMP_CFLAGS = -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE $(SIMD_FLAGS)

# Source files
CORE_SRCS = src/vl_geo.c \
            src/vl_heap.c \
            src/vl_bucket_heap.c \
            src/vl_protobuf.c \
            src/vl_inflate.c \
            src/vl_pbf.c \
            src/vl_graph.c \
            src/vl_route.c \
            src/vl_landmarks.c \
            src/velo.c

MINIZ_SRCS = vendor/miniz.c \
             vendor/miniz_tdef.c \
             vendor/miniz_tinfl.c

# Object files
CORE_OBJS = $(CORE_SRCS:.c=.o)
MINIZ_OBJS = $(MINIZ_SRCS:.c=.o)
ALL_OBJS = $(CORE_OBJS) $(MINIZ_OBJS)

# Library
LIB = libvelo.a

# Test binary
TEST_BIN = test_velo
TEST_OSRM = test_osrm_comparison

# Benchmark binaries
BENCH_ROUTING = bench_routing
BENCH_PBF = bench_pbf

# WASM output
WASM_OUT = velo.js

.PHONY: all clean test bench lib debug wasm

# Default target
all: lib $(TEST_BIN)

# Build library
lib: $(LIB)

$(LIB): $(ALL_OBJS)
	ar rcs $@ $^

# Compile rules
src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

vendor/%.o: vendor/%.c
	$(CC) $(CFLAGS) -Ivendor -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -c $< -o $@

# Test binary
$(TEST_BIN): tests/test_velo.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lvelo $(LDFLAGS) -o $@

# Run tests
test: $(TEST_BIN)
	./$(TEST_BIN)

# OSRM comparison test
$(TEST_OSRM): tests/test_osrm_comparison.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lvelo $(LDFLAGS) -o $@

test-osrm: $(TEST_OSRM)
	@echo "Run: ./$(TEST_OSRM) <graph.vlg> [osrm_host:port]"

# Benchmarks
$(BENCH_ROUTING): benchmarks/bench_routing.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lvelo $(LDFLAGS) -o $@

$(BENCH_PBF): benchmarks/bench_pbf.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lvelo $(LDFLAGS) -o $@

bench: $(BENCH_ROUTING) $(BENCH_PBF)
	./$(BENCH_ROUTING)

# Debug build
debug: CFLAGS = $(DEBUG_CFLAGS)
debug: clean all

# WASM build (requires Emscripten)
wasm:
	emcc $(CORE_SRCS) $(MINIZ_SRCS) \
		-Iinclude -Ivendor \
		-DMINIZ_NO_STDIO -DMINIZ_NO_TIME \
		-O2 \
		-s WASM=1 \
		-s EXPORTED_FUNCTIONS='["_vl_load_pbf", "_vl_load_binary", "_vl_route", "_vl_route_coords", "_vl_free_route", "_vl_graph_free", "_vl_default_options", "_vl_version", "_malloc", "_free"]' \
		-s EXPORTED_RUNTIME_METHODS='["ccall", "cwrap", "getValue", "setValue"]' \
		-s ALLOW_MEMORY_GROWTH=1 \
		-s INITIAL_MEMORY=33554432 \
		-o $(WASM_OUT)

# Clean
clean:
	rm -f $(CORE_OBJS) $(MINIZ_OBJS) $(LIB)
	rm -f $(TEST_BIN) $(TEST_OSRM) $(BENCH_ROUTING) $(BENCH_PBF)
	rm -f $(WASM_OUT) velo.wasm

# Dependencies
src/velo.o: include/velo.h include/vl_types.h include/vl_pbf.h include/vl_graph.h include/vl_route.h
src/vl_geo.o: include/vl_types.h
src/vl_heap.o: include/vl_types.h
src/vl_protobuf.o: include/vl_types.h
src/vl_inflate.o: include/vl_types.h vendor/miniz.h vendor/miniz_tinfl.h
src/vl_pbf.o: include/vl_types.h
src/vl_graph.o: include/vl_types.h
src/vl_route.o: include/vl_types.h

# Help
help:
	@echo "Velo Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build library and tests (default)"
	@echo "  lib      - Build static library (libvelo.a)"
	@echo "  test     - Build and run tests"
	@echo "  bench    - Build and run benchmarks"
	@echo "  debug    - Build with debug symbols"
	@echo "  wasm     - Build WebAssembly (requires Emscripten)"
	@echo "  clean    - Remove build artifacts"
	@echo ""
	@echo "Examples:"
	@echo "  make                    # Build library and tests"
	@echo "  make test               # Run test suite"
	@echo "  make bench              # Run benchmarks"
	@echo "  ./bench_pbf map.osm.pbf # Benchmark PBF parsing"
