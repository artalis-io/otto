# Locus API Server Makefile

CC ?= gcc

# Platform-specific RELRO (Linux only)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    RELRO_FLAGS = -Wl,-z,relro,-z,now
else
    RELRO_FLAGS =
endif

# Production build (with hardening)
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE \
         -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -fno-common
LDFLAGS_EXTRA = -pie $(RELRO_FLAGS)

# Debug build (with sanitizers)
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -std=c11 -D_GNU_SOURCE -DDEBUG \
               -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS = -fsanitize=address,undefined

# Directories
SRC_DIR = src
BUILD_DIR = build
LOCUS_DIR = ..
SHARED_DIR = ../../shared
VENDOR_DIR = ../../vendor

# Include paths
INCLUDES = -I$(SRC_DIR) \
           -I$(LOCUS_DIR)/include \
           -I$(SHARED_DIR)/include \
           -I$(KEEL_DIR)/include \
           -I$(VENDOR_DIR)/miniz

# Libraries
LIBS = -L$(LOCUS_DIR) -llocus \
       -L$(SHARED_DIR) -lshared \
       -lm -lpthread

# Keel library
# Keel static library (vendored git submodule, MIT)
KEEL_DIR = $(VENDOR_DIR)/keel
KEEL_LIB = $(KEEL_DIR)/libkeel.a

# Shared Keel HTTP helpers (need Keel headers, so not in libshared)
KEELSERVER_OBJ = $(BUILD_DIR)/sh_keelserver.o

# Source files
SRCS = $(SRC_DIR)/main.c
OBJS = $(BUILD_DIR)/main.o $(KEELSERVER_OBJ)

# Output
TARGET = locus-geocoder

.PHONY: all clean deps run test help

all: deps $(TARGET)

# Build dependencies
deps: $(KEEL_LIB)
	$(MAKE) -C $(LOCUS_DIR) lib
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
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Compile shared Keel HTTP helpers (require Keel includes)
$(KEELSERVER_OBJ): $(SHARED_DIR)/src/sh_keelserver.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build server
$(TARGET): $(OBJS) $(KEEL_LIB)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(KEEL_LIB) $(LIBS)

# Run server
run: $(TARGET)
	./$(TARGET)

# Clean
clean:
	rm -rf $(BUILD_DIR) $(TARGET)

# Test API endpoints
test: $(TARGET)
	@echo "Testing Locus API..."
	@echo "Note: Start server first with: make run"
	@echo ""
	@echo "Testing health endpoint:"
	curl -s http://localhost:8083/api/v1/health | head -c 200
	@echo ""
	@echo ""
	@echo "Testing stats endpoint:"
	curl -s http://localhost:8083/api/v1/stats | head -c 500
	@echo ""

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
