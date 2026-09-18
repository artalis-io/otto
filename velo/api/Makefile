# Velo Route Server Makefile

# Toolchain mapping. Every compiler and linker difference lives in that one
# file, which is what lets this server build under both MinGW and native MSVC
# (`source scripts/msvc-env.sh; make CC=cl`). Never define LIB, INCLUDE, LINK
# or CL here -- make re-exports them and they are MSVC's environment contract.
include ../../mk/toolchain.mk

# Production build (with hardening)
CFLAGS = $(strip $(CC_WARN) $(CC_OPT_O2) $(CC_STD) $(CC_DEFS) $(CC_PORT_DEFS) \
                 $(CC_HARDEN))

# Debug build (with sanitizers)
DEBUG_CFLAGS = $(strip $(CC_WARN) $(CC_DEBUG_OPT) $(CC_STD) $(CC_DEFS) \
                       $(CC_PORT_DEFS) -DDEBUG $(CC_SANITIZE))
DEBUG_LDFLAGS = $(strip $(LD_SANITIZE))

# Include paths
INCLUDES = -I../include -I../../shared/include -I$(KEEL_DIR)/include -I../../vendor/miniz -Isrc

# Shared library path
SHARED_DIR = ../../shared

# Library paths (no OpenMP - build velo with NOMP=1 for compatibility)
# Library paths. Velo is built with NOMP=1 for the API server, so no OpenMP
# here. LD_KEEL_PLATFORM is wider than LD_PLATFORM: Keel reaches for mswsock,
# iphlpapi, advapi32 and shell32 on top of what libshared uses.
#
# LD_PIE/LD_RELRO were previously computed into an LDFLAGS_EXTRA that nothing
# referenced, so the hardening never reached the link line.
LDFLAGS = $(strip $(call link_lib,..,velo) $(call link_lib,$(SHARED_DIR),shared) \
                  $(LD_MATH) $(LD_THREAD) $(LD_KEEL_PLATFORM) \
                  $(LD_PIE) $(LD_RELRO) $(LD_STACK))

# Source files (polyline.c moved to shared library)
SRCS = src/main.c
OBJS = $(SRCS:src/%.c=build/%.o)

# Keel static library (vendored git submodule, MIT)
KEEL_DIR = ../../vendor/keel
KEEL_LIB = $(KEEL_DIR)/libkeel.a

# Shared Keel HTTP helpers (need Keel headers, so not in libshared)
HTTPSERVER_OBJ = build/sh_httpserver.o build/sh_httpasync.o

TARGET = velo-route-server

.PHONY: all clean deps run debug build test test-asan FORCE

all: deps $(TARGET)

deps: $(KEEL_LIB)
	$(MAKE) -C .. NOMP=1 lib
	$(MAKE) -C $(SHARED_DIR) lib

# Build vendored Keel. Core only (no mbedTLS/nghttp2 integrations), so
# this needs no external libraries.
# Always recurse, rather than treating the archive's existence as proof it is
# current. A target with no prerequisites is "up to date" the moment the file
# is there, so a libkeel.a built before the v3.1.1 submodule bump went on being
# linked against v3.1.1 headers indefinitely. That mismatch is not a link
# error: the server starts, prints "listening", and segfaults inside
# kl_http_router_match() on the first request -- including /api/v1/health.
#
# Keel's own Makefile knows what is out of date and answers "Nothing to be
# done" when nothing is, leaving the archive's timestamp alone, so this costs
# a sub-make and causes no needless relinking.
$(KEEL_LIB): FORCE
	@if [ ! -f $(KEEL_DIR)/Makefile ]; then \
		echo "vendor/keel is empty - run: git submodule update --init vendor/keel"; \
		exit 1; \
	fi
	$(MAKE) -C $(KEEL_DIR) CC=$(CC)

# An always-out-of-date prerequisite. Keeping it here rather than marking
# $(KEEL_LIB) .PHONY, because a phony target is never considered a file and
# the link line needs the archive to still be one.
FORCE:

$(TARGET): $(OBJS) $(HTTPSERVER_OBJ) $(KEEL_LIB)
	$(CC) $(CFLAGS) $(OBJS) $(HTTPSERVER_OBJ) $(KEEL_LIB) $(LDFLAGS) $(EXE_OUT)$@

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) $(INCLUDES) -c $< $(OBJ_OUT)$@

# Compile shared Keel HTTP helpers (require Keel includes)
build/%.o: $(SHARED_DIR)/src/%.c | build
	$(CC) $(CFLAGS) $(INCLUDES) -c $< $(OBJ_OUT)$@

build:
	mkdir -p build

clean:
	rm -rf build $(TARGET) $(TARGET).exe $(TARGET).lib $(TARGET).exp

debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS += $(DEBUG_LDFLAGS)
debug: clean all

# Run with sample data
run: all
	./$(TARGET) ../../data/hungary-latest.osm.pbf

# The server under ASan and UBSan, driven by the same integration suite.
#
# The six API servers are the only OTTO code that reads bytes off a socket,
# and until this target they were the only modules never built with a
# sanitizer -- covered solely by test_api.sh, which judges the server by what
# curl gets back and cannot see a diagnostic on its stderr.
#
# What this instruments is the server itself: main.c and the shared HTTP
# helpers it compiles in, which is where a request is parsed and routed.
# libralph and libshared are linked as already built, so their interiors are
# not instrumented -- but ASan intercepts the allocator process-wide, so
# their heap allocations still get redzones. Instrumenting the whole
# dependency tree is a larger change and a separate one.
test-asan: CFLAGS = $(DEBUG_CFLAGS)
test-asan: LDFLAGS += $(DEBUG_LDFLAGS)
test-asan: clean test

# Test the API endpoints (uses --no-landmarks for faster startup)
# The heavy lifting lives in test_api.sh, which starts its own server, asserts
# on every endpoint and exits non-zero when any of them fails. This target used
# to inline a curl script that printed PASS and FAIL without counting either and
# ended on `kill ... || true`, so it always succeeded -- `make test` here could
# print FAIL five times and still exit 0. CI was unaffected (it calls the script
# directly), but anyone running make locally was told the wrong thing.
test: all
	# test_api.sh resolves both the server and its data file relative to the
	# repo root (SERVER=./velo/api/..., data/monaco...), so it has to run
	# from there. Without the cd this target could never work -- CI has been
	# invoking the script directly from the root and never used it.
	# The suite needs data/monaco.vlg, whose rule lives in the root
	# Makefile. Asking for it here is what makes this target -- and
	# `make test-velo-api`, which delegates to it -- work from a clean
	# checkout. Previously it failed and told the reader to go and build
	# the file by hand. A no-op once the file exists, so CI, which
	# fetches it in an earlier step, pays nothing.
	@cd ../.. && $(MAKE) data/monaco.vlg
	@cd ../.. && bash velo/api/test_api.sh

