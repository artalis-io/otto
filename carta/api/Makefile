# Carta Tile Server Makefile

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
CARTA_DIR = ..
VENDOR_DIR = ../../vendor

# Keel static library (vendored git submodule, MIT)
KEEL_DIR = $(VENDOR_DIR)/keel
KEEL_LIB = $(KEEL_DIR)/libkeel.a
SHARED_DIR = ../../shared

# Include paths
INCLUDES = -I$(SRC_DIR) \
           -I$(CARTA_DIR)/include \
           -I$(SHARED_DIR)/include \
           -I$(KEEL_DIR)/include \
           -I$(VENDOR_DIR)/miniz

# Libraries
# Libraries. LD_KEEL_PLATFORM is wider than LD_PLATFORM: Keel reaches for
# mswsock, iphlpapi, advapi32 and shell32 on top of what libshared uses.
LIBS = $(call link_lib,$(CARTA_DIR),carta) \
       $(call link_lib,$(SHARED_DIR),shared) \
       $(LD_MATH) $(LD_THREAD) $(LD_KEEL_PLATFORM)

# Link-time hardening. Previously computed into an LDFLAGS_EXTRA that nothing
# referenced, so it never reached the link line.
LDFLAGS = $(strip $(LD_PIE) $(LD_RELRO) $(LD_STACK))

# miniz objects
VENDOR_OBJS = $(VENDOR_DIR)/miniz/miniz.o \
              $(VENDOR_DIR)/miniz/miniz_tdef.o \
              $(VENDOR_DIR)/miniz/miniz_tinfl.o

# Shared Keel HTTP helpers (need Keel headers, so not in libshared)
HTTPSERVER_OBJ = $(BUILD_DIR)/sh_httpserver.o $(BUILD_DIR)/sh_httpasync.o

# Source files
SRCS = $(SRC_DIR)/main.c
OBJS = $(BUILD_DIR)/main.o $(HTTPSERVER_OBJ)

# Target
TARGET = carta-tile-server

.PHONY: all clean debug deps run test test-threading test-threading-quick

all: deps $(TARGET)

# Build dependencies first
deps: $(KEEL_LIB)
	$(MAKE) -C $(CARTA_DIR) lib
	$(MAKE) -C $(SHARED_DIR) lib
	@if [ ! -f $(VENDOR_DIR)/miniz/miniz.o ]; then \
		$(MAKE) -C $(VENDOR_DIR)/miniz; \
	fi

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

$(TARGET): $(OBJS) $(VENDOR_OBJS) $(KEEL_LIB)
	$(CC) $(CFLAGS) $(OBJS) $(VENDOR_OBJS) $(KEEL_LIB) $(LIBS) $(LDFLAGS) $(EXE_OUT)$@

debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LIBS += $(DEBUG_LDFLAGS)
debug: clean deps $(TARGET)

# Run with a sample PBF
run: $(TARGET)
	./$(TARGET) -s static -p 8081 ../../data/hungary-latest.osm.pbf

# Create static directory if it doesn't exist
static:
	mkdir -p static

clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(TARGET).exe $(TARGET).lib $(TARGET).exp

# Install target
install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

# Docker build
docker:
	docker build -t carta-tile-server .

# Threading stress test
test-threading: $(TARGET)
	@./test_threading.sh --threads 4 --clients 16 --requests 200

# Quick threading smoke test
test-threading-quick: $(TARGET)
	@./test_threading.sh --threads 2 --clients 8 --requests 50

# Test the API endpoints
# The heavy lifting lives in test_api.sh, which starts its own server, asserts
# on every endpoint and exits non-zero when any of them fails. This target used
# to inline a curl script that printed PASS and FAIL without counting either and
# ended on `kill ... || true`, so it always succeeded -- `make test` here could
# print FAIL five times and still exit 0. CI was unaffected (it calls the script
# directly), but anyone running make locally was told the wrong thing.
test: $(TARGET)
	@bash test_api.sh

