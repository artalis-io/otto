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
test: $(TARGET)
	@echo "Starting Carta tile server for testing..."
	@./$(TARGET) -p 8083 ../../data/hungary-latest.osm.pbf &
	@SERVER_PID=$$!; \
	echo "Waiting for server to load PBF (this may take a moment)..."; \
	sleep 15; \
	echo ""; \
	echo "=== Testing Carta Tile Server API ==="; \
	echo ""; \
	echo "Testing health endpoint..."; \
	HEALTH=$$(curl -s http://localhost:8083/api/v1/health); \
	echo "$$HEALTH" | head -5; \
	if echo "$$HEALTH" | grep -q '"status"'; then \
		echo "  PASS: Health endpoint works"; \
	else \
		echo "  FAIL: Health endpoint"; \
	fi; \
	echo ""; \
	echo "Testing stats endpoint..."; \
	STATS=$$(curl -s http://localhost:8083/api/v1/stats); \
	echo "$$STATS" | head -10; \
	if echo "$$STATS" | grep -q '"total_nodes"'; then \
		echo "  PASS: Stats endpoint works"; \
	else \
		echo "  FAIL: Stats endpoint"; \
	fi; \
	echo ""; \
	echo "Testing tiles.json endpoint..."; \
	TILEJSON=$$(curl -s http://localhost:8083/tiles.json); \
	echo "$$TILEJSON" | head -10; \
	if echo "$$TILEJSON" | grep -q '"tilejson"'; then \
		echo "  PASS: TileJSON endpoint works"; \
	else \
		echo "  FAIL: TileJSON endpoint"; \
	fi; \
	echo ""; \
	echo "Testing PNG tile endpoint (z=10, x=567, y=357)..."; \
	HTTP_CODE=$$(curl -s -o /dev/null -w "%{http_code}" http://localhost:8083/tiles/10/567/357.png); \
	if [ "$$HTTP_CODE" = "200" ]; then \
		echo "  PASS: PNG tile returned 200 OK"; \
	else \
		echo "  FAIL: PNG tile returned $$HTTP_CODE"; \
	fi; \
	echo ""; \
	echo "Testing MVT tile endpoint (z=10, x=567, y=357)..."; \
	HTTP_CODE=$$(curl -s -o /dev/null -w "%{http_code}" http://localhost:8083/tiles/10/567/357.mvt); \
	if [ "$$HTTP_CODE" = "200" ]; then \
		echo "  PASS: MVT tile returned 200 OK"; \
	else \
		echo "  FAIL: MVT tile returned $$HTTP_CODE"; \
	fi; \
	echo ""; \
	echo "=== Carta API Tests Complete ==="; \
	kill $$SERVER_PID 2>/dev/null || true
