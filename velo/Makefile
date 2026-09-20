# Velo Makefile
# Zero-dependency OSM routing engine

# Every compiler difference lives in one place; see mk/toolchain.mk.
include ../mk/toolchain.mk

# Production build (with hardening).
#
# Velo asks for fast math but NOT for -fno-finite-math-only, which is why the
# floating-point knobs in the toolchain are separate from each other: Ralph
# wants both, Velo wants one.
CFLAGS = $(strip $(CC_WARN) $(CC_OPT) $(CC_STD) $(CC_DEFS) $(CC_PORT_DEFS) \
                 $(CC_OMP) $(CC_OMP_SIMD) \
                 $(CC_ARCH) $(CC_VECTORIZE) $(CC_FP_MODE) $(CC_FP_FASTMATH) \
                 $(CC_HARDEN))
LDFLAGS = $(strip $(LD_MATH) $(LD_OMP) $(LD_PLATFORM) $(LD_PIE) $(LD_RELRO) $(LD_STACK))

# Include paths
INCLUDES = -Iinclude -I../shared/include -I../vendor/miniz

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
# Same intent as the default CFLAGS above, minus the two OpenMP knobs. It used
# to spell the flags out in GCC syntax, which silently overrode the
# toolchain-derived line above -- so `make NOMP=1 CC=cl` handed cl.exe
# `-Wall -Wextra -O3` and died on D8021. Only velo/api builds with NOMP, so the
# MSVC build never reached this path until the API servers did.
CFLAGS = $(strip $(CC_WARN) $(CC_OPT) $(CC_STD) $(CC_DEFS) $(CC_PORT_DEFS) \
                 $(CC_ARCH) $(CC_VECTORIZE) $(CC_FP_MODE) $(CC_FP_FASTMATH) \
                 $(CC_HARDEN) $(SIMD_FLAGS))
LDFLAGS = $(strip $(LD_MATH) $(LD_PLATFORM) $(LD_PIE) $(LD_RELRO) $(LD_STACK))
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
# Not LIB: that is MSVC's library search path, and make re-exports any variable
# that also exists in the environment. See mk/toolchain.mk.
LIB_FILE = libvelo.a

# Discard objects built with different flags -- a sanitizer or debug
# build must not be left behind for a release build to reuse. See
# mk/flagstamp.mk for what that cost before it existed.
DEBUG_GOALS = debug
STAMP_ARTIFACTS = $(CORE_OBJS) $(LIB_FILE)
include ../mk/flagstamp.mk


# Test binary
TEST_BIN = test_velo
TEST_TRANSPORT_BIN = test_transport_direct
TEST_OSRM = test_osrm_comparison

# Benchmark binaries
BENCH_ROUTING = bench_routing
BENCH_PBF = bench_pbf
ROUTE_BENCH = velo-route-bench

# WASM output
WASM_OUT = velo.js

# Run the suite under AddressSanitizer + UBSan. Same flags as `debug`, but it
# runs the tests instead of only building them.
#
# This module had no sanitizer target at all. Its api server has one, but that
# instruments main.c and the HTTP helpers only -- the module is linked already
# built, so its interior was never sanitized by anything. The same gap hid five
# leaks in Ralph's solver core.
test-asan: CFLAGS = $(DEBUG_CFLAGS)
test-asan: LDFLAGS += $(DEBUG_LDFLAGS)
test-asan: clean test

.PHONY: all clean test test-transport bench lib debug wasm route-bench omp-mode test-osrm help fuzz-api fuzz-api-smoke fuzz-smoke test-asan

# Default target
all: lib $(TEST_BIN)

# Build library
lib: omp-mode $(LIB_FILE)

# Drop objects built in the other OpenMP mode before anything depends on them.
omp-mode:
	@if [ ! -f $(OMP_STAMP) ] || [ "`cat $(OMP_STAMP)`" != "$(OMP_MODE)" ]; then \
		rm -f $(CORE_OBJS) $(LIB_FILE); \
		echo $(OMP_MODE) > $(OMP_STAMP); \
	fi

$(LIB_FILE): $(ALL_OBJS) $(SHARED_DIR)/libshared.a
	$(AR_CMD)$@ $(ALL_OBJS)

# Compile rules
src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< $(OBJ_OUT)$@

# Build shared library first
$(SHARED_DIR)/libshared.a:
	$(MAKE) -C $(SHARED_DIR) lib

# Test binary
$(TEST_BIN): tests/test_velo.c $(LIB_FILE) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,velo) $(call link_lib,$(SHARED_DIR),shared) $(LDFLAGS) $(EXE_OUT)$@

# Transport invariant: vl_api_handle driven by sh_transport_direct with no
# transport library on the include path. See docs/roadmaps/transport.md.
$(TEST_TRANSPORT_BIN): tests/test_transport_direct.c $(LIB_FILE) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,velo) $(call link_lib,$(SHARED_DIR),shared) $(LDFLAGS) $(EXE_OUT)$@

test-transport: $(TEST_TRANSPORT_BIN)
	./$(TEST_TRANSPORT_BIN)

# Run tests
test: $(TEST_BIN) $(TEST_TRANSPORT_BIN)
	./$(TEST_BIN)
	./$(TEST_TRANSPORT_BIN)

# OSRM comparison test
$(TEST_OSRM): tests/test_osrm_comparison.c $(LIB_FILE) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,velo) $(call link_lib,$(SHARED_DIR),shared) $(LDFLAGS) $(EXE_OUT)$@

test-osrm: $(TEST_OSRM)
	@echo "Run: ./$(TEST_OSRM) <graph.vlg> [osrm_host:port]"

# Benchmarks
$(BENCH_ROUTING): benchmarks/bench_routing.c $(LIB_FILE) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,velo) $(call link_lib,$(SHARED_DIR),shared) $(LDFLAGS) $(EXE_OUT)$@

$(BENCH_PBF): benchmarks/bench_pbf.c $(LIB_FILE) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,velo) $(call link_lib,$(SHARED_DIR),shared) $(LDFLAGS) $(EXE_OUT)$@

bench: $(BENCH_ROUTING) $(BENCH_PBF)
	./$(BENCH_ROUTING)

# Route quality benchmark
$(ROUTE_BENCH): benchmarks/velo_route_bench.c $(LIB_FILE) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,velo) $(call link_lib,$(SHARED_DIR),shared) $(LDFLAGS) $(EXE_OUT)$@

route-bench: $(ROUTE_BENCH)
	@echo "Route benchmark built: ./$(ROUTE_BENCH)"
	@echo "Usage: ./$(ROUTE_BENCH) --graph data/map.vlg --suite smoke -v"

# CLI tools
ROUTE_BATCH_BIN = route_batch
MATRIX_BUILD_BIN = matrix_build
ROUTE_GEOMETRY_BIN = route_geometry

$(ROUTE_BATCH_BIN): tools/route_batch.c $(LIB_FILE) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,velo) $(call link_lib,$(SHARED_DIR),shared) $(LDFLAGS) $(EXE_OUT)$@

$(MATRIX_BUILD_BIN): tools/matrix_build.c $(LIB_FILE) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,velo) $(call link_lib,$(SHARED_DIR),shared) $(LDFLAGS) $(EXE_OUT)$@

# route_geometry: stop-to-stop legs -> road-following path geometry (JSONL);
# feeds surge/scripts/surge_map.py to build road-following GeoJSON routes.
$(ROUTE_GEOMETRY_BIN): tools/route_geometry.c $(LIB_FILE) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,velo) $(call link_lib,$(SHARED_DIR),shared) $(LDFLAGS) $(EXE_OUT)$@

tools: $(ROUTE_BATCH_BIN) $(MATRIX_BUILD_BIN) $(ROUTE_GEOMETRY_BIN)

# Debug build (with sanitizers)
debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS = -lm $(DEBUG_LDFLAGS)
debug: clean all

# WASM build (delegates to wasm/ subdirectory)
wasm:
	$(MAKE) -C wasm


# --- API handler fuzzing ---------------------------------------------------
#
# Drives vl_api_handle() the way the transport does -- a whole ShApiRequest, with the
# route and method chosen by two control bytes -- so the routing and the
# parameter parsing are fuzzed together rather than one handler in isolation.
#
# The shared sources are compiled in rather than linked from libshared.a, so
# that they are instrumented too. Listed by wildcard so the set cannot drift
# from what is actually in the directory; the two HTTP sources are excluded
# because they include Keel headers this build has no include path for, and
# the transport is not what the harness drives anyway.
FUZZ_SHARED_SRCS = $(filter-out %/sh_httpserver.c %/sh_httpasync.c,\
                   $(wildcard ../shared/src/*.c))
FUZZ_MINIZ_SRCS = $(wildcard ../vendor/miniz/miniz*.c)

# Overridable so the target can be built with a clang that has libFuzzer
# when the default one does not -- the MinGW clang on Windows, say.
FUZZ_API_CC ?= clang
# Overridable alongside FUZZ_API_CC, and for the same reason: a clang that
# targets MSVC has no separate libm -- the maths functions are in the CRT --
# so `-lm` is a link error there rather than a no-op. Building with
# FUZZ_API_CC=<llvm clang> FUZZ_API_LIBS= is then enough to run the harness
# on Windows.
FUZZ_API_LIBS ?= -lm
FUZZ_API_BIN = fuzz_velo_api
FUZZ_API_SRC = tests/fuzz/fuzz_velo_api.c

fuzz-api: $(FUZZ_API_BIN)

$(FUZZ_API_BIN): $(FUZZ_API_SRC) $(CORE_SRCS)
	$(FUZZ_API_CC) -w -g -O1 -std=c11 -D_GNU_SOURCE $(CC_PORT_DEFS) -DMINIZ_NO_STDIO -DMINIZ_NO_TIME  \
	  -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
	  $(INCLUDES) -I../shared/tests/fuzz -I../vendor/miniz \
	  $< $(CORE_SRCS) $(FUZZ_SHARED_SRCS) $(FUZZ_MINIZ_SRCS) \
	  $(FUZZ_API_LIBS) $(EXE_OUT)$@

FUZZ_SMOKE_SECONDS ?= 30
FUZZ_WORK ?= build/fuzz-corpus

# A bounded run over the committed seeds, for CI. The first directory is
# where libFuzzer writes what it finds; the seeds after it are read only --
# passing the seeds alone would make libFuzzer write its discoveries back
# into them, which turns a curated seed set into noise.
#
# Short on purpose: this keeps the harness building and catches shallow
# regressions on every push. It is not a fuzzing campaign.
fuzz-api-smoke: fuzz-api
	@mkdir -p $(FUZZ_WORK)/api
	./$(FUZZ_API_BIN) $(FUZZ_WORK)/api tests/fuzz/corpus_api/ \
		-max_total_time=$(FUZZ_SMOKE_SECONDS) -print_final_stats=1

# The module-wide entry point CI calls. Only the API harness exists here;
# it gains the others as they are written.
fuzz-smoke: fuzz-api-smoke

# Clean
clean:
	rm -f $(BUILD_STAMP)
	rm -f $(CORE_OBJS) $(LIB_FILE) $(OMP_STAMP)
	rm -f $(TEST_BIN) $(TEST_TRANSPORT_BIN) $(TEST_OSRM) $(BENCH_ROUTING) $(BENCH_PBF) $(ROUTE_BENCH) $(ROUTE_BATCH_BIN) $(MATRIX_BUILD_BIN)
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
