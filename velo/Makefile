# Velo Makefile
# Zero-dependency OSM routing engine

CC ?= gcc

# OpenMP and SIMD optimization flags
# -fopenmp: Enable OpenMP parallelization
# -fopenmp-simd: Enable SIMD vectorization pragmas
# -march=native: Optimize for current CPU (AVX/AVX2/etc)
# -ftree-vectorize: Auto-vectorization
# -ffast-math: Allow aggressive FP optimizations (safe for our use)
SIMD_FLAGS = -march=native -ftree-vectorize -ffast-math

# Platform-specific OpenMP detection
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    # macOS with Homebrew libomp
    OPENMP_FLAGS = -Xclang -fopenmp
    OPENMP_LDFLAGS = -L/opt/homebrew/opt/libomp/lib -lomp
    OPENMP_INCLUDES = -I/opt/homebrew/opt/libomp/include
    RELRO_FLAGS =
else
    # Linux - native GCC OpenMP with RELRO
    OPENMP_FLAGS = -fopenmp -fopenmp-simd
    OPENMP_LDFLAGS = -fopenmp
    OPENMP_INCLUDES =
    RELRO_FLAGS = -Wl,-z,relro,-z,now
endif

# Production build (with hardening)
# Note: -pie is implicit on macOS, only add on Linux
CFLAGS = -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE $(OPENMP_FLAGS) $(SIMD_FLAGS) \
         -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -fno-common
ifeq ($(UNAME_S),Linux)
    LDFLAGS = -lm $(OPENMP_LDFLAGS) -pie $(RELRO_FLAGS)
else
    LDFLAGS = -lm $(OPENMP_LDFLAGS)
endif

# libshared now reaches the platform through sh_pal: bcrypt supplies
# sh_pal_random_bytes, ws2_32 the StatsD sink in sh_metrics.
ifneq (,$(findstring MINGW,$(UNAME_S)))
    LDFLAGS += -lbcrypt -lws2_32
endif

# Include paths
INCLUDES = -Iinclude -I../shared/include -I../vendor/miniz $(OPENMP_INCLUDES)

# Vendor path
VENDOR_DIR = ../vendor/miniz

# Shared library path
SHARED_DIR = ../shared

# Debug build (with sanitizers, no OpenMP for easier debugging)
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -std=c11 -D_GNU_SOURCE -DDEBUG \
               -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS = -fsanitize=address,undefined

# Non-OpenMP build for compatibility
# Use: make NOMP=1 all
#
# NOMP changes the compile flags but not the output filenames, so a tree built
# one way silently satisfies a make invoked the other way: `make lib` followed
# by `make NOMP=1 lib` reports "Nothing to be done" and hands back an OpenMP
# archive. Whatever links it next fails on undefined GOMP_* symbols. The stamp
# below records which mode the objects were built in and forces a rebuild when
# it flips. See the omp-mode target near `lib`.
OMP_MODE := $(if $(NOMP),nomp,omp)
OMP_STAMP = .omp-mode

ifdef NOMP
CFLAGS = -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE $(SIMD_FLAGS)
LDFLAGS = -lm
OPENMP_INCLUDES =
endif

# Low-memory build: disable transposed landmark layout (saves 50% landmark memory)
# Use: make LOWMEM=1 all
ifdef LOWMEM
CFLAGS += -DVL_LANDMARKS_NO_TRANSPOSE
endif

# Source files (removed vl_protobuf.c, vl_inflate.c, polyline.c - now in shared)
CORE_SRCS = src/vl_geo.c \
            src/vl_heap.c \
            src/vl_bucket_heap.c \
            src/vl_pbf.c \
            src/vl_graph.c \
            src/vl_route.c \
            src/vl_landmarks.c \
            src/vl_api.c \
            src/velo.c

# miniz is now linked via shared library
MINIZ_SRCS =

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
ROUTE_BENCH = velo-route-bench

# WASM output
WASM_OUT = velo.js

.PHONY: all clean test bench lib debug wasm route-bench omp-mode

# Default target
all: lib $(TEST_BIN)

# Build library
lib: omp-mode $(LIB)

# Drop objects built in the other OpenMP mode before anything depends on them.
omp-mode:
	@if [ ! -f $(OMP_STAMP) ] || [ "`cat $(OMP_STAMP)`" != "$(OMP_MODE)" ]; then \
		rm -f $(CORE_OBJS) $(LIB); \
		echo $(OMP_MODE) > $(OMP_STAMP); \
	fi

$(LIB): $(ALL_OBJS) $(SHARED_DIR)/libshared.a
	ar rcs $@ $(ALL_OBJS)

# Compile rules
src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build shared library first
$(SHARED_DIR)/libshared.a:
	$(MAKE) -C $(SHARED_DIR) lib

# Test binary
$(TEST_BIN): tests/test_velo.c $(LIB) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lvelo -L$(SHARED_DIR) -lshared $(LDFLAGS) -o $@

# Run tests
test: $(TEST_BIN)
	./$(TEST_BIN)

# OSRM comparison test
$(TEST_OSRM): tests/test_osrm_comparison.c $(LIB) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lvelo -L$(SHARED_DIR) -lshared $(LDFLAGS) -o $@

test-osrm: $(TEST_OSRM)
	@echo "Run: ./$(TEST_OSRM) <graph.vlg> [osrm_host:port]"

# Benchmarks
$(BENCH_ROUTING): benchmarks/bench_routing.c $(LIB) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lvelo -L$(SHARED_DIR) -lshared $(LDFLAGS) -o $@

$(BENCH_PBF): benchmarks/bench_pbf.c $(LIB) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lvelo -L$(SHARED_DIR) -lshared $(LDFLAGS) -o $@

bench: $(BENCH_ROUTING) $(BENCH_PBF)
	./$(BENCH_ROUTING)

# Route quality benchmark
$(ROUTE_BENCH): benchmarks/velo_route_bench.c $(LIB) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lvelo -L$(SHARED_DIR) -lshared $(LDFLAGS) -o $@

route-bench: $(ROUTE_BENCH)
	@echo "Route benchmark built: ./$(ROUTE_BENCH)"
	@echo "Usage: ./$(ROUTE_BENCH) --graph data/map.vlg --suite smoke -v"

# Debug build (with sanitizers)
debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS = -lm $(DEBUG_LDFLAGS)
debug: clean all

# WASM build (delegates to wasm/ subdirectory)
wasm:
	$(MAKE) -C wasm

# Clean
clean:
	rm -f $(CORE_OBJS) $(LIB) $(OMP_STAMP)
	rm -f $(TEST_BIN) $(TEST_OSRM) $(BENCH_ROUTING) $(BENCH_PBF) $(ROUTE_BENCH)
	rm -f $(WASM_OUT) velo.wasm

# Dependencies
src/velo.o: include/velo.h include/vl_types.h include/vl_pbf.h include/vl_graph.h include/vl_route.h
src/vl_geo.o: include/vl_types.h
src/vl_heap.o: include/vl_types.h
src/vl_pbf.o: include/vl_types.h $(SHARED_DIR)/include/sh_protobuf.h $(SHARED_DIR)/include/sh_inflate.h $(SHARED_DIR)/include/sh_pbf.h
src/vl_graph.o: include/vl_types.h
src/vl_route.o: include/vl_types.h

# Help
help:
	@echo "Velo Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all         - Build library and tests (default)"
	@echo "  lib         - Build static library (libvelo.a)"
	@echo "  test        - Build and run tests"
	@echo "  bench       - Build and run algorithm benchmarks"
	@echo "  route-bench - Build route quality benchmark tool"
	@echo "  debug       - Build with debug symbols"
	@echo "  wasm        - Build WebAssembly (requires Emscripten)"
	@echo "  clean       - Remove build artifacts"
	@echo ""
	@echo "Examples:"
	@echo "  make                           # Build library and tests"
	@echo "  make test                      # Run test suite"
	@echo "  make bench                     # Run algorithm benchmarks"
	@echo "  make route-bench               # Build route quality tool"
	@echo "  ./velo-route-bench --help      # Route benchmark usage"
