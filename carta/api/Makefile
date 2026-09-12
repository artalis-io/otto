# Carta Tile Server Makefile

CC = gcc

# Platform-specific RELRO (Linux only)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    RELRO_FLAGS = -Wl,-z,relro,-z,now
else
    RELRO_FLAGS =
endif

# Windows: Keel talks to winsock, and libshared reaches bcrypt through
# sh_pal_random_bytes. iphlpapi comes with Keel's adapter enumeration.
ifneq (,$(findstring MINGW,$(UNAME_S)))
    PLATFORM_LIBS = -lws2_32 -lbcrypt -liphlpapi
else
    PLATFORM_LIBS =
endif

# Production build (with hardening)
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L \
         -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -fno-common
LDFLAGS_EXTRA = -pie $(RELRO_FLAGS)

# Debug build (with sanitizers)
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -std=c11 -D_POSIX_C_SOURCE=200809L -DDEBUG \
               -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS = -fsanitize=address,undefined

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
LIBS = -L$(CARTA_DIR) -lcarta \
       -L$(SHARED_DIR) -lshared \
       -lm -lpthread $(PLATFORM_LIBS)

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
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Compile shared Keel HTTP helpers (require Keel includes)
$(BUILD_DIR)/%.o: $(SHARED_DIR)/src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(TARGET): $(OBJS) $(VENDOR_OBJS) $(KEEL_LIB)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(VENDOR_OBJS) $(KEEL_LIB) $(LIBS)

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
	rm -rf $(BUILD_DIR) $(TARGET)

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

