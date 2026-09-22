# Surge Makefile
# Rich VRPTW/PDPTW solver built on Arbor

# Every compiler difference lives in one place; see mk/toolchain.mk.
include ../mk/toolchain.mk


# Production build (with hardening)
# Note: -pie is implicit on macOS, only add on Linux
CFLAGS = $(strip $(CC_WARN) $(CC_OPT) $(CC_STD) $(CC_DEFS) $(CC_PORT_DEFS) \
                 -DSG_HAS_THREADS $(CC_HARDEN))
LDFLAGS = $(strip $(LD_MATH) $(LD_THREAD) $(LD_PLATFORM) $(LD_PIE) $(LD_RELRO) $(LD_STACK))

INCLUDES = -Iinclude -I../arbor/include -I../shared/include

# Debug build (with sanitizers)
DEBUG_CFLAGS = $(strip $(CC_WARN) $(CC_DEBUG_OPT) $(CC_STD) $(CC_DEFS) $(CC_PORT_DEFS) \
                      -DSG_HAS_THREADS -DDEBUG $(CC_SANITIZE))
DEBUG_LDFLAGS = $(LD_SANITIZE)

SRCS = src/sg_context.c src/sg_solution.c src/sg_cost.c src/sg_feasibility.c \
       src/sg_construct.c src/sg_construct_cfrs.c src/sg_destroy.c src/sg_repair.c \
       src/sg_postprocess.c src/sg_solve.c src/sg_reader.c src/sg_api.c \
       src/sg_parallel.c src/sg_validate.c src/sg_penalty.c src/sg_time_budget.c \
       src/sg_neighbor.c src/sg_profile_matrix.c src/sg_concat.c
OBJS = $(SRCS:.c=.o)

# Not LIB: that is MSVC's library search path, and make re-exports any
# variable that also exists in the environment. See mk/toolchain.mk.
LIB_FILE = libsurge.a

# Discard objects built with different flags -- a sanitizer or debug
# build must not be left behind for a release build to reuse. See
# mk/flagstamp.mk for what that cost before it existed.
DEBUG_GOALS = debug test-asan
STAMP_ARTIFACTS = $(OBJS) $(LIB_FILE)
include ../mk/flagstamp.mk

TEST_BIN = test_surge
# Transport abstraction leak detector -- builds with no transport library on
# the include path. See docs/roadmaps/transport.md.
TRANSPORT_TEST_BIN = test_transport_direct
TEST_TB_BIN = test_time_budget
TEST_NB_BIN = test_neighbor
TEST_PM_BIN = test_profile_matrix
BENCH_SOL_BIN = bench_solomon
BENCH_PD_BIN = bench_li_lim
BENCH_CORD_BIN = bench_cordeau
BENCH_TUNE_BIN = bench_tune

# Shared library for FFI bindings (Python ctypes, etc.)
ARBOR_SRCS = ../arbor/src/ar_alns.c ../arbor/src/ar_operators.c ../arbor/src/arbor.c
# This has to be the transitive closure of what surge and arbor call in
# shared/, because the two targets that use it compile these sources directly
# rather than linking libshared.a. It was not: sh_workqueue, sh_worker_pool and
# sh_completion call the platform layer (sh_mutex_lock, sh_cond_timedwait,
# sh_monotonic_ms, sh_wall_ns) and sg_solution/sg_repair call sh_heap_*, and
# neither the PAL nor sh_heap was listed. Every other consumer links the whole
# archive and so never noticed.
#
# Both PAL files are listed the way shared/Makefile lists them: each compiles
# to nothing on the platform it is not for.
#
# If you add a shared/ call to surge or arbor, this list is what needs it. A
# link against libshared.a would not need maintaining, and is the better shape
# if these targets are ever reworked.
SHARED_BINDING_SRCS = ../shared/src/sh_json.c ../shared/src/sh_arena.c \
                      ../shared/src/sh_hashmap.c ../shared/src/sh_dist.c \
                      ../shared/src/sh_stepfunc.c ../shared/src/sh_piecewise.c \
                      ../shared/src/sh_completion.c ../shared/src/sh_worker_pool.c \
                      ../shared/src/sh_workqueue.c \
                      ../shared/src/sh_heap.c \
                      ../shared/src/sh_pal_posix.c ../shared/src/sh_pal_windows.c

ifeq ($(UNAME_S),Darwin)
    SHARED_LIB = libsurge_api.dylib
    SHARED_LIB_FLAGS = -dynamiclib -install_name @rpath/libsurge_api.dylib
else
    SHARED_LIB = libsurge_api.so
    SHARED_LIB_FLAGS = -shared -Wl,-soname,libsurge_api.so
endif

FUZZ_BIN = fuzz_json_api
TSAN_TEST_BIN = test_surge_tsan

# TSan build (cannot combine with ASan)
TSAN_CFLAGS = -Wall -Wextra -g -O1 -std=c11 -D_GNU_SOURCE -DSG_HAS_THREADS -DDEBUG \
              -fsanitize=thread -fno-omit-frame-pointer
TSAN_LDFLAGS = -fsanitize=thread

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

.PHONY: all clean test test-transport scripts-test tools lib debug bench bench-li-lim bench-cordeau \
        bench-quick bench-li-lim-quick bench-cordeau-quick bench-all-quick \
        bench-gh-200 bench-gh-all bench-ll-200 bench-ll-all bench-full \
        bench-tune tune-quick tune-all \
        bench-download bench-validate \
        shared-lib python-test node-test cppcheck fuzz tsan fuzz-smoke cppcheck-all help fuzz-api fuzz-api-smoke test-asan

all: lib $(TEST_BIN)

lib: $(LIB_FILE)

$(LIB_FILE): $(OBJS) ../arbor/libarbor.a ../shared/libshared.a
	$(AR_CMD)$@ $(OBJS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< $(OBJ_OUT)$@

$(TEST_BIN): tests/test_surge.c $(LIB_FILE) ../arbor/libarbor.a ../shared/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,surge) $(call link_lib,../arbor,arbor) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# CLI tools. surge_solve: request JSON -> solution JSON (the CLI companion to the
# HTTP /api/v1/solve endpoint; feeds surge/scripts/surge_map.py).
SURGE_SOLVE_BIN = surge_solve

$(SURGE_SOLVE_BIN): tools/surge_solve.c $(LIB_FILE) ../arbor/libarbor.a ../shared/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,surge) $(call link_lib,../arbor,arbor) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

tools: $(SURGE_SOLVE_BIN)

$(TRANSPORT_TEST_BIN): tests/test_transport_direct.c $(LIB_FILE) ../arbor/libarbor.a ../shared/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,surge) $(call link_lib,../arbor,arbor) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

test-transport: $(TRANSPORT_TEST_BIN)
	./$(TRANSPORT_TEST_BIN)

$(TEST_TB_BIN): tests/test_time_budget.c src/sg_time_budget.c src/sg_time_budget.h
	$(CC) $(CFLAGS) -Isrc -I../shared/include tests/test_time_budget.c src/sg_time_budget.c $(LDFLAGS) $(EXE_OUT)$@

$(TEST_NB_BIN): tests/test_neighbor.c $(LIB_FILE) ../arbor/libarbor.a ../shared/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,surge) $(call link_lib,../arbor,arbor) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

$(TEST_PM_BIN): tests/test_profile_matrix.c $(LIB_FILE) ../arbor/libarbor.a ../shared/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,surge) $(call link_lib,../arbor,arbor) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

BENCH_INCLUDES = $(INCLUDES) -Ibenchmarks

$(BENCH_SOL_BIN): benchmarks/bench_solomon.c benchmarks/sg_bench_utils.h $(LIB_FILE) ../arbor/libarbor.a ../shared/libshared.a
	$(CC) $(CFLAGS) $(BENCH_INCLUDES) $< $(call link_lib,.,surge) $(call link_lib,../arbor,arbor) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

$(BENCH_PD_BIN): benchmarks/bench_li_lim.c benchmarks/sg_bench_utils.h $(LIB_FILE) ../arbor/libarbor.a ../shared/libshared.a
	$(CC) $(CFLAGS) $(BENCH_INCLUDES) $< $(call link_lib,.,surge) $(call link_lib,../arbor,arbor) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

$(BENCH_CORD_BIN): benchmarks/bench_cordeau.c benchmarks/sg_bench_utils.h $(LIB_FILE) ../arbor/libarbor.a ../shared/libshared.a
	$(CC) $(CFLAGS) $(BENCH_INCLUDES) $< $(call link_lib,.,surge) $(call link_lib,../arbor,arbor) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

$(BENCH_TUNE_BIN): benchmarks/bench_tune.c benchmarks/sg_bench_utils.h $(LIB_FILE) ../arbor/libarbor.a ../shared/libshared.a
	$(CC) $(CFLAGS) $(BENCH_INCLUDES) $< $(call link_lib,.,surge) $(call link_lib,../arbor,arbor) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

../arbor/libarbor.a:
	$(MAKE) -C ../arbor lib

../shared/libshared.a:
	$(MAKE) -C ../shared lib

test: $(TEST_BIN) $(TEST_TB_BIN) $(TEST_NB_BIN) $(TEST_PM_BIN) $(TRANSPORT_TEST_BIN)
	./$(TRANSPORT_TEST_BIN)
	./$(TEST_TB_BIN)
	./$(TEST_NB_BIN)
	./$(TEST_PM_BIN)
	./$(TEST_BIN)

# Python script self-tests (surge_map GeoJSON core). No solver/subprocess/files.
# Kept out of `test` so the Windows C-test jobs (no python3) stay green; CI runs
# it on Linux (see .github/workflows/ci.yml, Test Arbor + Surge).
scripts-test:
	python3 scripts/surge_map_selftest.py

bench: $(BENCH_SOL_BIN)
	./$(BENCH_SOL_BIN) --dir benchmarks/solomon

bench-li-lim: $(BENCH_PD_BIN)
	./$(BENCH_PD_BIN) --dir benchmarks/li_lim

bench-cordeau: $(BENCH_CORD_BIN)
	./$(BENCH_CORD_BIN) --dir benchmarks/cordeau

# Quick benchmarks (1000 iterations) for hyperparameter tuning
bench-quick: $(BENCH_SOL_BIN)
	./$(BENCH_SOL_BIN) --dir benchmarks/solomon --iterations 1000

bench-li-lim-quick: $(BENCH_PD_BIN)
	./$(BENCH_PD_BIN) --dir benchmarks/li_lim --iterations 1000

bench-cordeau-quick: $(BENCH_CORD_BIN)
	./$(BENCH_CORD_BIN) --dir benchmarks/cordeau --iterations 1000

bench-all-quick: $(BENCH_SOL_BIN) $(BENCH_PD_BIN) $(BENCH_CORD_BIN)
	./$(BENCH_SOL_BIN) --dir benchmarks/solomon --iterations 1000
	./$(BENCH_PD_BIN) --dir benchmarks/li_lim --iterations 1000
	./$(BENCH_CORD_BIN) --dir benchmarks/cordeau --iterations 1000

# Gehring-Homberger (large-scale VRPTW) benchmarks
bench-gh-200: $(BENCH_SOL_BIN)
	./$(BENCH_SOL_BIN) --dir benchmarks/gehring_homberger --bks benchmarks/bks/gehring_homberger.csv --size 200

bench-gh-all: $(BENCH_SOL_BIN)
	@for size in 200 400 600 800 1000; do \
		echo "=== Gehring-Homberger $$size ==="; \
		./$(BENCH_SOL_BIN) --dir benchmarks/gehring_homberger --bks benchmarks/bks/gehring_homberger.csv --size $$size --time-limit 60; \
	done

# Extended Li-Lim (large-scale PDPTW) benchmarks
bench-ll-200: $(BENCH_PD_BIN)
	./$(BENCH_PD_BIN) --dir benchmarks/li_lim_extended --bks benchmarks/bks/li_lim_extended.csv --size 200

bench-ll-all: $(BENCH_PD_BIN)
	@for size in 200 400 600 800 1000; do \
		echo "=== Li-Lim Extended $$size ==="; \
		./$(BENCH_PD_BIN) --dir benchmarks/li_lim_extended --bks benchmarks/bks/li_lim_extended.csv --size $$size --time-limit 120; \
	done

# Full benchmark suite (all sizes)
bench-full: bench bench-li-lim bench-cordeau bench-gh-all bench-ll-all

# Hyperparameter tuner
bench-tune: $(BENCH_TUNE_BIN)
	./$(BENCH_TUNE_BIN) --baseline --json

tune-quick: $(BENCH_TUNE_BIN)
	./$(BENCH_TUNE_BIN) --tier 0 --iterations 1000 --baseline --json

tune-all: $(BENCH_TUNE_BIN)
	./$(BENCH_TUNE_BIN) --all-tiers --iterations 2500 --baseline --json

# Download published benchmarks (GH + Li-Lim extended + Cordeau)
bench-download:
	bash benchmarks/scripts/download_benchmarks.sh

# Validate all instances parse correctly
bench-validate: $(BENCH_SOL_BIN) $(BENCH_PD_BIN)
	@echo "Validating Solomon 100..."
	./$(BENCH_SOL_BIN) --dir benchmarks/solomon --iterations 1
	@echo "Validating Li-Lim 100..."
	./$(BENCH_PD_BIN) --dir benchmarks/li_lim --iterations 1

shared-lib: $(SHARED_LIB)

$(SHARED_LIB): $(SRCS) $(ARBOR_SRCS) $(SHARED_BINDING_SRCS)
	$(CC) -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE -DSG_HAS_THREADS -fPIC \
	  -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fno-common \
	  $(INCLUDES) $(SHARED_LIB_FLAGS) \
	  $(SRCS) $(ARBOR_SRCS) $(SHARED_BINDING_SRCS) \
	  -lm -lpthread $(EXE_OUT)$@

python-test: shared-lib
	cd bindings/python && SURGE_LIB_PATH=../../$(SHARED_LIB) python3 -m pytest -v

node-test:
	$(MAKE) -C wasm api-demo
	mkdir -p bindings/node/build
	cp wasm/build/surge-api-demo.js bindings/node/build/surge-api-demo.cjs
	cd bindings/node && node --test test/*.test.js

debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS = -lm $(DEBUG_LDFLAGS)
debug: clean all

# --- Static Analysis ---

cppcheck:
	cppcheck --enable=warning,performance,portability --suppress=missingIncludeSystem \
	  --error-exitcode=1 --inline-suppr --std=c11 \
	  -I include -I ../arbor/include -I ../shared/include \
	  $(SRCS)

cppcheck-all:
	cppcheck --enable=all --suppress=missingIncludeSystem --suppress=unusedFunction \
	  --std=c11 -I include -I ../arbor/include -I ../shared/include \
	  $(SRCS)

# --- Fuzz Testing (requires clang with libFuzzer) ---

$(FUZZ_BIN): fuzz/fuzz_json_api.c $(SRCS) $(ARBOR_SRCS) $(SHARED_BINDING_SRCS)
	clang -Wall -Wextra -g -O1 -std=c11 -D_GNU_SOURCE -DSG_HAS_THREADS \
	  -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
	  $(INCLUDES) \
	  $< $(SRCS) $(ARBOR_SRCS) $(SHARED_BINDING_SRCS) \
	  -lm -lpthread $(EXE_OUT)$@

fuzz: $(FUZZ_BIN)
	@echo "Run: ./$(FUZZ_BIN) fuzz/corpus/ -max_len=65536 -jobs=4"

FUZZ_SMOKE_SECONDS ?= 30
FUZZ_WORK ?= build/fuzz-corpus
# A bounded run over the committed seed corpora, for CI. The first directory
# is where libFuzzer writes what it finds; the seeds after it are read only.
# Passing the seeds alone would make libFuzzer write its discoveries back
# into them, which turns a curated seed set into noise.
#
# Short on purpose: this keeps the harnesses building and catches shallow
# regressions on every push. It is not a fuzzing campaign.
# Both harnesses. fuzz_json_api calls sg_api_solve() directly, so a mutation
# maps one-to-one onto body bytes; fuzz_surge_api goes through sg_api_handle()
# and covers routing, methods and the response contract as well.
fuzz-smoke: fuzz fuzz-api-smoke
	@mkdir -p $(FUZZ_WORK)/json
	./$(FUZZ_BIN) $(FUZZ_WORK)/json fuzz/corpus/ \
		-max_total_time=$(FUZZ_SMOKE_SECONDS) -max_len=65536 -print_final_stats=1

# --- Fuzzing the API handler (requires clang with libFuzzer) ---
#
# *_api_handle() is an ordinary C function taking (ctx, request,
# response), so this needs no socket, no event loop and no server
# lifecycle. The api/src/main.c wrappers are thin marshalling -- see
# docs/analysis/api-server-read.md -- and this is the code behind them,
# where a request body becomes a model.
#
# The shared sources are compiled in rather than linked from libshared.a
# because sh_json is the parser under test and an uninstrumented copy of
# it would defeat the point. Listed by wildcard so the set cannot drift
# from what is actually in the directory.
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
FUZZ_API_LIBS ?= -lm -lpthread
FUZZ_API_BIN = fuzz_surge_api
FUZZ_API_SRC = tests/fuzz/fuzz_surge_api.c

fuzz-api: $(FUZZ_API_BIN)

$(FUZZ_API_BIN): $(FUZZ_API_SRC) $(SRCS) $(ARBOR_SRCS)
	$(FUZZ_API_CC) -w -g -O1 -std=c11 -D_GNU_SOURCE $(CC_PORT_DEFS) -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DSG_HAS_THREADS \
	  -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
	  $(INCLUDES) -I../shared/tests/fuzz -I../vendor/miniz  \
	  $< $(SRCS) $(ARBOR_SRCS) $(FUZZ_SHARED_SRCS) $(FUZZ_MINIZ_SRCS) \
	  $(FUZZ_API_LIBS) $(EXE_OUT)$@

FUZZ_SMOKE_SECONDS ?= 30
FUZZ_WORK ?= build/fuzz-corpus

# A bounded run over the committed seeds, for CI. The first directory is
# where libFuzzer writes what it finds; the seeds after it are read only.
fuzz-api-smoke: fuzz-api
	@mkdir -p $(FUZZ_WORK)/api
	./$(FUZZ_API_BIN) $(FUZZ_WORK)/api tests/fuzz/corpus_api/ \
		-max_total_time=$(FUZZ_SMOKE_SECONDS) -print_final_stats=1

# --- Thread Sanitizer ---

$(TSAN_TEST_BIN): tests/test_surge.c $(SRCS) $(ARBOR_SRCS) ../shared/libshared.a
	$(CC) $(TSAN_CFLAGS) $(INCLUDES) \
	  $< $(SRCS) $(ARBOR_SRCS) \
	  $(call link_lib,../shared,shared) -lm -lpthread $(TSAN_LDFLAGS) $(EXE_OUT)$@

tsan: $(TSAN_TEST_BIN)
	./$(TSAN_TEST_BIN)

clean:
	rm -f $(BUILD_STAMP)
	rm -f $(OBJS) $(LIB_FILE) $(TEST_BIN) $(TEST_TB_BIN) $(TEST_NB_BIN) $(TEST_PM_BIN) $(TRANSPORT_TEST_BIN) $(BENCH_SOL_BIN) $(BENCH_PD_BIN) $(BENCH_CORD_BIN) $(BENCH_TUNE_BIN) $(SHARED_LIB) \
	  $(FUZZ_BIN) $(TSAN_TEST_BIN)
	rm -f $(FUZZ_API_BIN) $(FUZZ_API_BIN).exe

help:
	@echo "Surge Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build library and tests (default)"
	@echo "  lib      - Build static library (libsurge.a)"
	@echo "  test     - Build and run tests"
	@echo "  bench    - Build and run Solomon benchmark suite (10000 iters)"
	@echo "  bench-li-lim - Build and run Li & Lim PDPTW benchmark suite (10000 iters)"
	@echo "  bench-cordeau - Build and run Cordeau DARP benchmark suite (10000 iters)"
	@echo "  bench-quick - Solomon quick bench (1000 iters, for tuning)"
	@echo "  bench-li-lim-quick - Li & Lim quick bench (1000 iters, for tuning)"
	@echo "  bench-cordeau-quick - Cordeau quick bench (1000 iters, for tuning)"
	@echo "  bench-all-quick - Run all three quick benchmarks"
	@echo "  shared-lib - Build shared library for FFI bindings"
	@echo "  python-test - Build shared lib and run Python tests"
	@echo "  node-test   - Build WASM and run Node.js tests"
	@echo "  debug    - Build with debug symbols"
	@echo "  cppcheck - Run cppcheck static analysis"
	@echo "  fuzz     - Build libFuzzer harness for JSON API (requires clang)"
	@echo "  tsan     - Build and run tests with ThreadSanitizer"
	@echo "  clean    - Remove build artifacts"
