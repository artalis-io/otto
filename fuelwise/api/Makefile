# FuelWise REST API Makefile

# Toolchain mapping. Every compiler and linker difference lives in that one
# file, which is what lets this server build under both MinGW and native MSVC
# (`source scripts/msvc-env.sh; make CC=cl`). Never define LIB, INCLUDE, LINK
# or CL here -- make re-exports them and they are MSVC's environment contract.
include ../../mk/toolchain.mk

# Production build (with hardening)
CFLAGS = $(strip $(CC_WARN) $(CC_OPT_O2) $(CC_STD_BASELINE) $(CC_DEFS) $(CC_PORT_DEFS) \
                 $(CC_HARDEN))

# Debug build (with sanitizers)
DEBUG_CFLAGS = $(strip $(CC_WARN) $(CC_DEBUG_OPT) $(CC_STD_BASELINE) $(CC_DEFS) \
                       $(CC_PORT_DEFS) -DDEBUG $(CC_SANITIZE))
DEBUG_LDFLAGS = $(strip $(LD_SANITIZE))

INCLUDES = -I../include -I../../ralph/include -I$(KEEL_DIR)/include -I../../shared/include

# OpenMP support (required when linking against libralph.a which uses OpenMP)
UNAME_S := $(shell uname -s)

# Source files
SRC_DIR = src
# Keel static library (vendored git submodule, MIT)
KEEL_DIR = ../../vendor/keel
KEEL_LIB = $(KEEL_DIR)/libkeel.a
SHARED_DIR = ../../shared

# Libraries
FUELWISE_LIB = ../libfuelwise.a
RALPH_LIB = ../../ralph/libralph.a
SHARED_LIB = ../../shared/libshared.a

# Output
TARGET = fuelwise-api
BUILD_DIR = build

# Source files
SRCS = $(SRC_DIR)/main.c
OBJS = $(SRCS:.c=.o)

# Shared Keel HTTP helpers (need Keel headers, so not in libshared)
HTTPSERVER_OBJ = $(BUILD_DIR)/sh_httpserver.o $(BUILD_DIR)/sh_httpasync.o

# Default target
all: $(BUILD_DIR) $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Build the API server
$(TARGET): $(OBJS) $(HTTPSERVER_OBJ) $(FUELWISE_LIB) $(RALPH_LIB) $(SHARED_LIB) $(KEEL_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $(OBJS) $(HTTPSERVER_OBJ) $(FUELWISE_LIB) $(RALPH_LIB) $(SHARED_LIB) $(KEEL_LIB) $(LD_OMP) $(LD_MATH) $(LD_THREAD) $(LD_KEEL_PLATFORM) $(LD_PIE) $(LD_RELRO) $(LD_STACK) $(EXE_OUT)$@

# Build vendored Keel. Core only (no mbedTLS/nghttp2 integrations), so
# this needs no external libraries.
$(KEEL_LIB):
	@if [ ! -f $(KEEL_DIR)/Makefile ]; then \
		echo "vendor/keel is empty - run: git submodule update --init vendor/keel"; \
		exit 1; \
	fi
	$(MAKE) -C $(KEEL_DIR) CC=$(CC)

# Compile source files
$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< $(OBJ_OUT)$@

# Compile shared Keel HTTP helpers (require Keel includes)
$(BUILD_DIR)/%.o: $(SHARED_DIR)/src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< $(OBJ_OUT)$@

# Ensure dependencies are built
# `lib`, not the default goal. A bare $(MAKE) -C .. builds each module's
# whole default target -- tests included -- so building this server pulled in
# fuelwise's test binary, which needs ../ralph/libralph.a and fails on a tree
# where ralph has not been built yet. The other five API Makefiles all name
# the lib goal; this one did not.
$(FUELWISE_LIB):
	$(MAKE) -C .. lib

$(RALPH_LIB):
	$(MAKE) -C ../../ralph lib

$(SHARED_LIB):
	$(MAKE) -C ../../shared lib

# Run the server
run: $(TARGET)
	./$(TARGET)

# Run on a specific port
run-port: $(TARGET)
	./$(TARGET) -p $(PORT)

# Test the API endpoints
test: $(TARGET)
	@./test_api.sh

# Quick test (basic functionality only)
test-quick: $(TARGET)
	@./test_api.sh --quick

# Rate limit test
test-ratelimit: $(TARGET)
	@./test_api.sh --ratelimit

# Work queue test
test-queue: $(TARGET)
	@./test_api.sh --queue

# Clean build artifacts
clean:
	rm -rf $(TARGET) $(SRC_DIR)/*.o $(BUILD_DIR)

.PHONY: all clean run run-port test test-quick test-ratelimit test-queue
