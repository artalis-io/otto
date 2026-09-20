# Ralph LP/MIP Solver - HTTP Server Makefile

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
INCLUDES = -I../include -I../../shared/include -I$(KEEL_DIR)/include -Isrc

# Shared library path
SHARED_DIR = ../../shared

# Library paths. LD_KEEL_PLATFORM is wider than LD_PLATFORM: Keel reaches for
# mswsock, iphlpapi, advapi32 and shell32 on top of what libshared uses.
# LD_OMP carries OpenMP (Ralph's LAP solver) and is empty under MSVC, which has
# no equivalent.
#
# LD_PIE/LD_RELRO were previously computed into an LDFLAGS_EXTRA that nothing
# ever referenced, so the hardening the comments advertised was absent from the
# link line entirely. They are on it now.
LDFLAGS = $(strip $(call link_lib,..,ralph) $(call link_lib,$(SHARED_DIR),shared) \
                  $(LD_OMP) $(LD_MATH) $(LD_THREAD) $(LD_KEEL_PLATFORM) \
                  $(LD_PIE) $(LD_RELRO) $(LD_STACK))

# Source files
SRCS = src/main.c
OBJS = $(SRCS:src/%.c=build/%.o)

# Shared Keel HTTP helpers. They need Keel headers, so each API server compiles
# them directly rather than linking them from libshared.a.
HTTPSERVER_OBJ = build/sh_httpserver.o build/sh_httpasync.o

# Keel static library (vendored git submodule, MIT)
KEEL_DIR = ../../vendor/keel
KEEL_LIB = $(KEEL_DIR)/libkeel.a

TARGET = ralph-solver-server

.PHONY: all clean deps run debug test build test-asan FORCE build-asan

all: deps $(TARGET)

deps: $(KEEL_LIB)
	$(MAKE) -C .. lib
	$(MAKE) -C $(SHARED_DIR) lib

# Build vendored Keel. Core only (no mbedTLS/nghttp2 integrations), so
# this needs no external libraries. CC carries through, so `make CC=cl` builds
# Keel with MSVC too -- supported since Keel v3.1.0.
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

# Compile shared Keel HTTP helpers (require Keel includes).
build/%.o: $(SHARED_DIR)/src/%.c | build
	$(CC) $(CFLAGS) $(INCLUDES) -c $< $(OBJ_OUT)$@

build:
	mkdir -p build

# MSVC drops a .lib import library and a .exp export file beside the binary.
clean:
	rm -rf build $(TARGET) $(TARGET).exe $(TARGET).lib $(TARGET).exp

debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS += $(DEBUG_LDFLAGS)
debug: clean all

# Run the server
run: all
	./$(TARGET)

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

# The build half of test-asan: identical flags, no test run.
#
# It exists because compiling and testing sometimes need different
# environments. The Windows MSVC ASan job is the case: cl must be invoked with
# scripts/msvc-env.sh sourced, so /Fo: and /std:c11 survive as flags, and the
# suites must be invoked without it, because with MSYS2_ARG_CONV_EXCL='*' set
# the `curl -o /tmp/...` they use reaches native curl as a path it cannot
# write. One target that does both cannot satisfy both.
build-asan: CFLAGS = $(DEBUG_CFLAGS)
build-asan: LDFLAGS += $(DEBUG_LDFLAGS)
build-asan: clean all

# Integration test. test_api.sh starts the server itself, picks its own
# port, and exits non-zero on failure -- so this target actually gates.
test: all
	@cd ../.. && bash ralph/api/test_api.sh
