# Locus Makefile
# Zero-dependency geocoding library

# Every compiler difference lives in one place; see mk/toolchain.mk.
include ../mk/toolchain.mk


# Production build (with hardening)
# Note: -pie is implicit on macOS, only add on Linux
CFLAGS = $(strip $(CC_WARN) $(CC_OPT) $(CC_STD) $(CC_DEFS) $(CC_PORT_DEFS) \
                 $(CC_ARCH) $(CC_VECTORIZE) $(CC_FP_MODE) $(CC_FP_FASTMATH) \
                 $(CC_HARDEN))
LDFLAGS = $(strip $(LD_MATH) $(LD_PLATFORM) $(LD_PIE) $(LD_RELRO) $(LD_STACK))

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
# Not LIB: that is MSVC's library search path, and make re-exports any
# variable that also exists in the environment. See mk/toolchain.mk.
LIB_FILE = liblocus.a

# Test binary
TEST_BIN = test_locus
TEST_TRANSPORT_BIN = test_transport_direct

# Benchmark binary
BENCH_BIN = bench_locus

# WASM output
WASM_OUT = locus.js

.PHONY: all clean test test-transport bench lib debug wasm help fuzz-api fuzz-api-smoke fuzz-smoke

# Default target
all: lib $(TEST_BIN)

# Build library
lib: $(LIB_FILE)

$(LIB_FILE): $(CORE_OBJS) $(SHARED_DIR)/libshared.a
	$(AR_CMD)$@ $(CORE_OBJS)

# Compile rules
src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< $(OBJ_OUT)$@

# Build shared library first
$(SHARED_DIR)/libshared.a:
	$(MAKE) -C $(SHARED_DIR) lib

# Test binary
$(TEST_BIN): tests/test_locus.c $(LIB_FILE) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,locus) $(call link_lib,$(SHARED_DIR),shared) $(LDFLAGS) $(EXE_OUT)$@

# Transport invariant: lc_api_handle driven by sh_transport_direct with no
# transport library on the include path. See docs/roadmaps/transport.md.
$(TEST_TRANSPORT_BIN): tests/test_transport_direct.c $(LIB_FILE) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,locus) $(call link_lib,$(SHARED_DIR),shared) $(LDFLAGS) $(EXE_OUT)$@

test-transport: $(TEST_TRANSPORT_BIN)
	./$(TEST_TRANSPORT_BIN)

# Run tests
test: $(TEST_BIN) $(TEST_TRANSPORT_BIN)
	./$(TEST_BIN)
	./$(TEST_TRANSPORT_BIN)

# Benchmarks
$(BENCH_BIN): benchmarks/bench_locus.c $(LIB_FILE) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,locus) $(call link_lib,$(SHARED_DIR),shared) $(LDFLAGS) $(EXE_OUT)$@

bench: $(BENCH_BIN)
	./$(BENCH_BIN)

# CLI tools
GEOCODE_BATCH_BIN = geocode_batch

$(GEOCODE_BATCH_BIN): tools/geocode_batch.c $(LIB_FILE) $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,locus) $(call link_lib,$(SHARED_DIR),shared) $(LDFLAGS) $(EXE_OUT)$@

tools: $(GEOCODE_BATCH_BIN)

# Debug build (with sanitizers)
debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS = -lm $(DEBUG_LDFLAGS)
debug: clean all

# WASM build (delegates to wasm/ subdirectory)
wasm:
	$(MAKE) -C wasm


# --- API handler fuzzing ---------------------------------------------------
#
# Drives lc_api_handle() the way the transport does -- a whole ShApiRequest, with the
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
FUZZ_API_BIN = fuzz_locus_api
FUZZ_API_SRC = tests/fuzz/fuzz_locus_api.c

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
	rm -f $(CORE_OBJS) $(LIB_FILE)
	rm -f $(TEST_BIN) $(TEST_TRANSPORT_BIN) $(BENCH_BIN) $(GEOCODE_BATCH_BIN)
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
