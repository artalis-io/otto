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

.PHONY: all clean deps run test help

all: deps $(TARGET)

# Build dependencies
deps: $(KEEL_LIB)
	$(MAKE) -C $(SURGE_DIR) lib
	$(MAKE) -C $(SHARED_DIR) lib

# Build vendored Keel. Core only (no mbedTLS/nghttp2 integrations), so
# this needs no external libraries.
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
	$(CC) $(CFLAGS) $(OBJS) $(LIBS) $(LDFLAGS) $(EXE_OUT)$@

# Run server
run: $(TARGET)
	./$(TARGET)

# Clean
clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(TARGET).exe $(TARGET).lib $(TARGET).exp

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
