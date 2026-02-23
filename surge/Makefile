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
       src/sg_validate.c src/sg_penalty.c
OBJS = $(SRCS:.c=.o)

LIB = libsurge.a
TEST_BIN = test_surge
BENCH_SOL_BIN = bench_solomon
BENCH_PD_BIN = bench_li_lim
BENCH_CORD_BIN = bench_cordeau

# Shared library for FFI bindings (Python ctypes, etc.)
ARBOR_SRCS = ../arbor/src/ar_alns.c ../arbor/src/ar_operators.c ../arbor/src/arbor.c
SHARED_BINDING_SRCS = ../shared/src/sh_json.c ../shared/src/sh_arena.c \
                      ../shared/src/sh_hashmap.c ../shared/src/sh_dist.c \
                      ../shared/src/sh_completion.c ../shared/src/sh_worker_pool.c \
                      ../shared/src/sh_workqueue.c

ifeq ($(UNAME_S),Darwin)
    SHARED_LIB = libsurge_api.dylib
    SHARED_LIB_FLAGS = -dynamiclib -install_name @rpath/libsurge_api.dylib
else
    SHARED_LIB = libsurge_api.so
    SHARED_LIB_FLAGS = -shared -Wl,-soname,libsurge_api.so
endif

.PHONY: all clean test lib debug bench bench-li-lim bench-cordeau shared-lib python-test node-test

all: lib $(TEST_BIN)

lib: $(LIB)

$(LIB): $(OBJS) ../arbor/libarbor.a ../shared/libshared.a
	ar rcs $@ $(OBJS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(TEST_BIN): tests/test_surge.c $(LIB) ../arbor/libarbor.a ../shared/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lsurge -L../arbor -larbor -L../shared -lshared $(LDFLAGS) -o $@

$(BENCH_SOL_BIN): benchmarks/bench_solomon.c $(LIB) ../arbor/libarbor.a ../shared/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lsurge -L../arbor -larbor -L../shared -lshared $(LDFLAGS) -o $@

$(BENCH_PD_BIN): benchmarks/bench_li_lim.c $(LIB) ../arbor/libarbor.a ../shared/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lsurge -L../arbor -larbor -L../shared -lshared $(LDFLAGS) -o $@

$(BENCH_CORD_BIN): benchmarks/bench_cordeau.c $(LIB) ../arbor/libarbor.a ../shared/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lsurge -L../arbor -larbor -L../shared -lshared $(LDFLAGS) -o $@

../arbor/libarbor.a:
	$(MAKE) -C ../arbor lib

../shared/libshared.a:
	$(MAKE) -C ../shared lib

test: $(TEST_BIN)
	./$(TEST_BIN)

bench: $(BENCH_SOL_BIN)
	./$(BENCH_SOL_BIN) --dir benchmarks/solomon

bench-li-lim: $(BENCH_PD_BIN)
	./$(BENCH_PD_BIN) --dir benchmarks/li_lim

bench-cordeau: $(BENCH_CORD_BIN)
	./$(BENCH_CORD_BIN) --dir benchmarks/cordeau

shared-lib: $(SHARED_LIB)

$(SHARED_LIB): $(SRCS) $(ARBOR_SRCS) $(SHARED_BINDING_SRCS)
	$(CC) -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE -DSG_HAS_THREADS -fPIC \
	  $(INCLUDES) $(SHARED_LIB_FLAGS) \
	  $(SRCS) $(ARBOR_SRCS) $(SHARED_BINDING_SRCS) \
	  -lm -lpthread -o $@

python-test: shared-lib
	cd bindings/python && SURGE_LIB_PATH=../../$(SHARED_LIB) python3 -m pytest -v

node-test:
	$(MAKE) -C wasm api-demo
	mkdir -p bindings/node/build
	cp wasm/build/surge-api-demo.js bindings/node/build/
	cd bindings/node && node --test test/*.test.js

debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS = -lm $(DEBUG_LDFLAGS)
debug: clean all

clean:
	rm -f $(OBJS) $(LIB) $(TEST_BIN) $(BENCH_SOL_BIN) $(BENCH_PD_BIN) $(BENCH_CORD_BIN) $(SHARED_LIB)

help:
	@echo "Surge Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build library and tests (default)"
	@echo "  lib      - Build static library (libsurge.a)"
	@echo "  test     - Build and run tests"
	@echo "  bench    - Build and run Solomon benchmark suite"
	@echo "  bench-li-lim - Build and run Li & Lim PDPTW benchmark suite"
	@echo "  bench-cordeau - Build and run Cordeau DARP benchmark suite"
	@echo "  shared-lib - Build shared library for FFI bindings"
	@echo "  python-test - Build shared lib and run Python tests"
	@echo "  node-test   - Build WASM and run Node.js tests"
	@echo "  debug    - Build with debug symbols"
	@echo "  clean    - Remove build artifacts"
