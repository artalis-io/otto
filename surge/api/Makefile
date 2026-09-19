# Surge API Server Makefile

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

# Directories
SRC_DIR = src
BUILD_DIR = build
SURGE_DIR = ..
ARBOR_DIR = ../../arbor
SHARED_DIR = ../../shared
VENDOR_DIR = ../../vendor
KEEL_DIR = $(VENDOR_DIR)/keel

# Include paths
INCLUDES = -I$(SRC_DIR) \
           -I$(SURGE_DIR)/include \
           -I$(ARBOR_DIR)/include \
           -I$(SHARED_DIR)/include \
           -I$(KEEL_DIR)/include

# Libraries
# Libraries. LD_KEEL_PLATFORM is wider than LD_PLATFORM: Keel reaches for
# mswsock, iphlpapi, advapi32 and shell32 on top of what libshared uses.
LIBS = $(call link_lib,$(SURGE_DIR),surge) \
       $(call link_lib,$(ARBOR_DIR),arbor) \
       $(call link_lib,$(SHARED_DIR),shared) \
       $(KEEL_LIB) \
       $(LD_MATH) $(LD_THREAD) $(LD_KEEL_PLATFORM)

# Link-time hardening. Previously computed into an LDFLAGS_EXTRA that nothing
# referenced, so it never reached the link line.
LDFLAGS = $(strip $(LD_PIE) $(LD_RELRO) $(LD_STACK))

# Keel static library (vendored git submodule, MIT)
KEEL_LIB = $(KEEL_DIR)/libkeel.a

# Shared Keel HTTP helpers (need Keel headers, so not in libshared)
HTTPSERVER_OBJ = $(BUILD_DIR)/sh_httpserver.o $(BUILD_DIR)/sh_httpasync.o

# Source files
SRCS = $(SRC_DIR)/main.c
OBJS = $(BUILD_DIR)/main.o $(HTTPSERVER_OBJ)

# Output
TARGET = surge-solver

.PHONY: all clean deps run test help test-asan FORCE build-asan

all: deps $(TARGET)

# Build dependencies
deps: $(KEEL_LIB)
	$(MAKE) -C $(SURGE_DIR) lib
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

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< $(OBJ_OUT)$@

# Compile shared Keel HTTP helpers (require Keel includes)
$(BUILD_DIR)/%.o: $(SHARED_DIR)/src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< $(OBJ_OUT)$@

# Build server
$(TARGET): $(OBJS) $(KEEL_LIB)
	$(CC) $(CFLAGS) $(OBJS) $(LIBS) $(LDFLAGS) $(EXE_OUT)$@

# Run server
run: $(TARGET)
	./$(TARGET)

# Clean
clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(TARGET).exe $(TARGET).lib $(TARGET).exp

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

# Test API endpoints
# The heavy lifting lives in test_api.sh, which starts its own server, asserts
# on every endpoint and exits non-zero when any of them fails. This target used
# to inline a curl script that printed PASS and FAIL without counting either and
# ended on `kill ... || true`, so it always succeeded -- `make test` here could
# print FAIL five times and still exit 0. CI was unaffected (it calls the script
# directly), but anyone running make locally was told the wrong thing.
test: $(TARGET)
	@bash test_api.sh

# Help
help:
	@echo "Surge API Server"
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build server (default)"
	@echo "  run     - Run server"
	@echo "  test    - Test endpoints (server must be running)"
	@echo "  clean   - Clean build"
	@echo ""
	@echo "Usage:"
	@echo "  ./surge-solver                    # Start on port 8085"
	@echo "  ./surge-solver -p 9000            # Start on custom port"
	@echo "  ./surge-solver --workers 4        # Use 4 solve workers"
