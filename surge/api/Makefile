# Surge API Server Makefile

CC ?= gcc

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
LIBS = -L$(SURGE_DIR) -lsurge \
       -L$(ARBOR_DIR) -larbor \
       -L$(SHARED_DIR) -lshared \
       $(KEEL_LIB) \
       -lm -lpthread $(PLATFORM_LIBS)

# Keel static library (vendored git submodule, MIT)
KEEL_LIB = $(KEEL_DIR)/libkeel.a

# Shared Keel HTTP helpers (need Keel headers, so not in libshared)
KEELSERVER_OBJ = $(BUILD_DIR)/sh_keelserver.o $(BUILD_DIR)/sh_keelasync.o

# Source files
SRCS = $(SRC_DIR)/main.c
OBJS = $(BUILD_DIR)/main.o $(KEELSERVER_OBJ)

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
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Compile shared Keel HTTP helpers (require Keel includes)
$(BUILD_DIR)/%.o: $(SHARED_DIR)/src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build server
$(TARGET): $(OBJS) $(KEEL_LIB)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

# Run server
run: $(TARGET)
	./$(TARGET)

# Clean
clean:
	rm -rf $(BUILD_DIR) $(TARGET)

# Test API endpoints
test: $(TARGET)
	@echo "Testing Surge API..."
	@echo "Note: Start server first with: make run"
	@echo ""
	@echo "Testing health endpoint:"
	curl -s http://localhost:8085/api/v1/health | head -c 200
	@echo ""
	@echo ""
	@echo "Testing version endpoint:"
	curl -s http://localhost:8085/api/v1/version | head -c 200
	@echo ""

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
