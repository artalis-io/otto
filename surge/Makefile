# Surge Makefile
# Rich VRPTW/PDPTW solver built on Arbor

CC ?= gcc

# Platform-specific RELRO (Linux only)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    RELRO_FLAGS = -Wl,-z,relro,-z,now
else
    RELRO_FLAGS =
endif

# Production build (with hardening)
# Note: -pie is implicit on macOS, only add on Linux
CFLAGS = -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE -DSG_HAS_THREADS \
         -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -fno-common
ifeq ($(UNAME_S),Linux)
    LDFLAGS = -lm -lpthread -pie $(RELRO_FLAGS)
else
    LDFLAGS = -lm -lpthread
endif

INCLUDES = -Iinclude -I../arbor/include -I../shared/include

# Debug build (with sanitizers)
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -std=c11 -D_GNU_SOURCE -DSG_HAS_THREADS -DDEBUG \
               -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS = -fsanitize=address,undefined

SRCS = src/sg_context.c src/sg_solution.c src/sg_cost.c src/sg_feasibility.c \
       src/sg_construct.c src/sg_destroy.c src/sg_repair.c src/sg_postprocess.c \
       src/sg_solve.c src/sg_reader.c src/sg_api.c src/sg_parallel.c \
       src/sg_validate.c src/sg_penalty.c src/sg_time_budget.c src/sg_neighbor.c \
       src/sg_profile_matrix.c
OBJS = $(SRCS:.c=.o)

LIB = libsurge.a
TEST_BIN = test_surge
TEST_TB_BIN = test_time_budget
TEST_NB_BIN = test_neighbor
TEST_PM_BIN = test_profile_matrix
BENCH_SOL_BIN = bench_solomon
BENCH_PD_BIN = bench_li_lim
BENCH_CORD_BIN = bench_cordeau
BENCH_TUNE_BIN = bench_tune

# Shared library for FFI bindings (Python ctypes, etc.)
ARBOR_SRCS = ../arbor/src/ar_alns.c ../arbor/src/ar_operators.c ../arbor/src/arbor.c
SHARED_BINDING_SRCS = ../shared/src/sh_json.c ../shared/src/sh_arena.c \
                      ../shared/src/sh_hashmap.c ../shared/src/sh_dist.c \
                      ../shared/src/sh_stepfunc.c ../shared/src/sh_piecewise.c \
                      ../shared/src/sh_completion.c ../shared/src/sh_worker_pool.c \
                      ../shared/src/sh_workqueue.c

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

.PHONY: all clean test lib debug bench bench-li-lim bench-cordeau \
        bench-quick bench-li-lim-quick bench-cordeau-quick bench-all-quick \
        bench-gh-200 bench-gh-all bench-ll-200 bench-ll-all bench-full \
        bench-tune tune-quick tune-all \
        bench-download bench-validate \
        shared-lib python-test node-test cppcheck fuzz tsan

all: lib $(TEST_BIN)

lib: $(LIB)

$(LIB): $(OBJS) ../arbor/libarbor.a ../shared/libshared.a
	ar rcs $@ $(OBJS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(TEST_BIN): tests/test_surge.c $(LIB) ../arbor/libarbor.a ../shared/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lsurge -L../arbor -larbor -L../shared -lshared $(LDFLAGS) -o $@

$(TEST_TB_BIN): tests/test_time_budget.c src/sg_time_budget.c src/sg_time_budget.h
	$(CC) $(CFLAGS) -Isrc -I../shared/include tests/test_time_budget.c src/sg_time_budget.c $(LDFLAGS) -o $@

$(TEST_NB_BIN): tests/test_neighbor.c $(LIB) ../arbor/libarbor.a ../shared/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lsurge -L../arbor -larbor -L../shared -lshared $(LDFLAGS) -o $@

$(TEST_PM_BIN): tests/test_profile_matrix.c $(LIB) ../arbor/libarbor.a ../shared/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lsurge -L../arbor -larbor -L../shared -lshared $(LDFLAGS) -o $@

BENCH_INCLUDES = $(INCLUDES) -Ibenchmarks

$(BENCH_SOL_BIN): benchmarks/bench_solomon.c benchmarks/sg_bench_utils.h $(LIB) ../arbor/libarbor.a ../shared/libshared.a
	$(CC) $(CFLAGS) $(BENCH_INCLUDES) $< -L. -lsurge -L../arbor -larbor -L../shared -lshared $(LDFLAGS) -o $@

$(BENCH_PD_BIN): benchmarks/bench_li_lim.c benchmarks/sg_bench_utils.h $(LIB) ../arbor/libarbor.a ../shared/libshared.a
	$(CC) $(CFLAGS) $(BENCH_INCLUDES) $< -L. -lsurge -L../arbor -larbor -L../shared -lshared $(LDFLAGS) -o $@

$(BENCH_CORD_BIN): benchmarks/bench_cordeau.c benchmarks/sg_bench_utils.h $(LIB) ../arbor/libarbor.a ../shared/libshared.a
	$(CC) $(CFLAGS) $(BENCH_INCLUDES) $< -L. -lsurge -L../arbor -larbor -L../shared -lshared $(LDFLAGS) -o $@

$(BENCH_TUNE_BIN): benchmarks/bench_tune.c benchmarks/sg_bench_utils.h $(LIB) ../arbor/libarbor.a ../shared/libshared.a
	$(CC) $(CFLAGS) $(BENCH_INCLUDES) $< -L. -lsurge -L../arbor -larbor -L../shared -lshared $(LDFLAGS) -o $@

../arbor/libarbor.a:
	$(MAKE) -C ../arbor lib

../shared/libshared.a:
	$(MAKE) -C ../shared lib

test: $(TEST_BIN) $(TEST_TB_BIN) $(TEST_NB_BIN) $(TEST_PM_BIN)
	./$(TEST_TB_BIN)
	./$(TEST_NB_BIN)
	./$(TEST_PM_BIN)
	./$(TEST_BIN)

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
	  -lm -lpthread -o $@

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
	  -lm -lpthread -o $@

fuzz: $(FUZZ_BIN)
	@echo "Run: ./$(FUZZ_BIN) fuzz/corpus/ -max_len=65536 -jobs=4"

# --- Thread Sanitizer ---

$(TSAN_TEST_BIN): tests/test_surge.c $(SRCS) $(ARBOR_SRCS) ../shared/libshared.a
	$(CC) $(TSAN_CFLAGS) $(INCLUDES) \
	  $< $(SRCS) $(ARBOR_SRCS) \
	  -L../shared -lshared -lm -lpthread $(TSAN_LDFLAGS) -o $@

tsan: $(TSAN_TEST_BIN)
	./$(TSAN_TEST_BIN)

clean:
	rm -f $(OBJS) $(LIB) $(TEST_BIN) $(TEST_TB_BIN) $(TEST_NB_BIN) $(TEST_PM_BIN) $(BENCH_SOL_BIN) $(BENCH_PD_BIN) $(BENCH_CORD_BIN) $(BENCH_TUNE_BIN) $(SHARED_LIB) \
	  $(FUZZ_BIN) $(TSAN_TEST_BIN)

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
