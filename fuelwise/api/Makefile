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

INCLUDES = -I../include -I../../ralph/include -I../../vendor/mongoose -I../../shared/include

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
MONGOOSE_DIR = ../../vendor/mongoose
SHARED_DIR = ../../shared

# Libraries
FUELWISE_LIB = ../libfuelwise.a
RALPH_LIB = ../../ralph/libralph.a
SHARED_LIB = ../../shared/libshared.a

# Output
TARGET = fuelwise-api
BUILD_DIR = build

# Source files
SRCS = $(SRC_DIR)/main.c $(MONGOOSE_DIR)/mongoose.c
OBJS = $(SRCS:.c=.o)

# Shared HTTP server (requires mongoose, compiled here not in libshared)
HTTPSERVER_OBJ = $(BUILD_DIR)/sh_httpserver.o

# Default target
all: $(BUILD_DIR) $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Build the API server
$(TARGET): $(OBJS) $(HTTPSERVER_OBJ) $(FUELWISE_LIB) $(RALPH_LIB) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(OBJS) $(HTTPSERVER_OBJ) $(FUELWISE_LIB) $(RALPH_LIB) $(SHARED_LIB) -lm -lpthread $(OPENMP_LDFLAGS)

# Compile source files
$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(MONGOOSE_DIR)/%.o: $(MONGOOSE_DIR)/%.c
	$(CC) $(CFLAGS) -DMG_ENABLE_LINES=0 -c $< -o $@

# Compile shared HTTP server (requires mongoose includes)
$(HTTPSERVER_OBJ): $(SHARED_DIR)/src/sh_httpserver.c | $(BUILD_DIR)
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
	rm -rf $(TARGET) $(SRC_DIR)/*.o $(MONGOOSE_DIR)/*.o $(BUILD_DIR)

.PHONY: all clean run run-port test
