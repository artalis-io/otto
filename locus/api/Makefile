# Locus API Server Makefile

# Toolchain mapping. Every compiler and linker difference lives in this one
# file, which is what lets the server build under both MinGW and native MSVC
# (`source scripts/msvc-env.sh; make CC=cl`). Never define LIB, INCLUDE, LINK or
# CL here -- make re-exports those and they are MSVC's environment contract.
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
LOCUS_DIR = ..
SHARED_DIR = ../../shared
VENDOR_DIR = ../../vendor

# Keel static library (vendored git submodule, MIT)
KEEL_DIR = $(VENDOR_DIR)/keel
KEEL_LIB = $(KEEL_DIR)/libkeel.a

# Include paths
INCLUDES = -I$(SRC_DIR) \
           -I$(LOCUS_DIR)/include \
           -I$(SHARED_DIR)/include \
           -I$(KEEL_DIR)/include \
           -I$(VENDOR_DIR)/miniz

# Libraries. LD_KEEL_PLATFORM is wider than LD_PLATFORM because Keel reaches
# for mswsock, iphlpapi, advapi32 and shell32 on top of what libshared uses.
LIBS = $(call link_lib,$(LOCUS_DIR),locus) \
       $(call link_lib,$(SHARED_DIR),shared) \
       $(LD_MATH) $(LD_THREAD) $(LD_KEEL_PLATFORM)

# Link-time hardening. This used to be assigned to LDFLAGS_EXTRA and then never
# referenced, so the PIE/RELRO the comment advertised was absent from the link
# line entirely. It is on the link line now.
LDFLAGS = $(strip $(LD_PIE) $(LD_RELRO) $(LD_STACK))

# Shared Keel HTTP helpers (need Keel headers, so not in libshared)
HTTPSERVER_OBJ = $(BUILD_DIR)/sh_httpserver.o $(BUILD_DIR)/sh_httpasync.o

# Source files
SRCS = $(SRC_DIR)/main.c
OBJS = $(BUILD_DIR)/main.o $(HTTPSERVER_OBJ)

# Output
TARGET = locus-geocoder

.PHONY: all clean deps run test help test-asan

all: deps $(TARGET)

# Build dependencies
deps: $(KEEL_LIB)
	$(MAKE) -C $(LOCUS_DIR) lib
	$(MAKE) -C $(SHARED_DIR) lib

# Build vendored Keel. Core only (no mbedTLS/nghttp2 integrations), so
# this needs no external libraries. CC carries through, so `make CC=cl` here
# builds Keel with MSVC too -- supported since Keel v3.1.0.
$(KEEL_LIB):
	@if [ ! -f $(KEEL_DIR)/Makefile ]; then \
		echo "vendor/keel is empty - run: git submodule update --init vendor/keel"; \
		exit 1; \
	fi
	$(MAKE) -C $(KEEL_DIR) CC=$(CC)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< $(OBJ_OUT)$@

# Compile shared Keel HTTP helpers (require Keel includes)
$(BUILD_DIR)/%.o: $(SHARED_DIR)/src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< $(OBJ_OUT)$@

# Build server
$(TARGET): $(OBJS) $(KEEL_LIB)
	$(CC) $(CFLAGS) $(OBJS) $(KEEL_LIB) $(LIBS) $(LDFLAGS) $(EXE_OUT)$@

# Run server
run: $(TARGET)
	./$(TARGET)

# Clean
clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(TARGET).exe

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
	@echo "Locus API Server"
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build server (default)"
	@echo "  run     - Run server"
	@echo "  test    - Test endpoints"
	@echo "  clean   - Clean build"
	@echo ""
	@echo "Usage:"
	@echo "  ./locus-geocoder <pbf-file>"
	@echo "  ./locus-geocoder -p 8083 data/monaco-latest.osm.pbf"
