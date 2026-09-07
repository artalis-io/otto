# FuelWise REST API Makefile

CC = gcc

# Platform-specific RELRO (Linux only)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    RELRO_FLAGS = -Wl,-z,relro,-z,now
else
    RELRO_FLAGS =
endif

# Production build (with hardening)
CFLAGS = -Wall -Wextra -O2 -g \
         -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -fno-common
LDFLAGS_EXTRA = -pie $(RELRO_FLAGS)

# Debug build (with sanitizers)
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -DDEBUG \
               -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS = -fsanitize=address,undefined

INCLUDES = -I../include -I../../ralph/include -I$(KEEL_DIR)/include -I../../shared/include

# OpenMP support (required when linking against libralph.a which uses OpenMP)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    # macOS with Homebrew libomp
    OPENMP_LDFLAGS = -L/opt/homebrew/opt/libomp/lib -lomp
else
    # Linux - native GCC OpenMP
    OPENMP_LDFLAGS = -fopenmp
endif

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
KEELSERVER_OBJ = $(BUILD_DIR)/sh_keelserver.o $(BUILD_DIR)/sh_keelasync.o

# Default target
all: $(BUILD_DIR) $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Build the API server
$(TARGET): $(OBJS) $(KEELSERVER_OBJ) $(FUELWISE_LIB) $(RALPH_LIB) $(SHARED_LIB) $(KEEL_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(OBJS) $(KEELSERVER_OBJ) $(FUELWISE_LIB) $(RALPH_LIB) $(SHARED_LIB) $(KEEL_LIB) -lm -lpthread $(OPENMP_LDFLAGS)

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
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Compile shared Keel HTTP helpers (require Keel includes)
$(BUILD_DIR)/%.o: $(SHARED_DIR)/src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Ensure dependencies are built
$(FUELWISE_LIB):
	$(MAKE) -C ..

$(RALPH_LIB):
	$(MAKE) -C ../../ralph

$(SHARED_LIB):
	$(MAKE) -C ../../shared

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

.PHONY: all clean run run-port test
