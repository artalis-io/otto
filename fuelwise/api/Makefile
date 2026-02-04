# FuelWise REST API Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
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

# Libraries
FUELWISE_LIB = ../libfuelwise.a
RALPH_LIB = ../../ralph/libralph.a
SHARED_LIB = ../../shared/libshared.a

# Output
TARGET = fuelwise-api

# Source files
SRCS = $(SRC_DIR)/main.c $(MONGOOSE_DIR)/mongoose.c
OBJS = $(SRCS:.c=.o)

# Default target
all: $(TARGET)

# Build the API server
$(TARGET): $(OBJS) $(FUELWISE_LIB) $(RALPH_LIB) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(OBJS) $(FUELWISE_LIB) $(RALPH_LIB) $(SHARED_LIB) -lm -lpthread $(OPENMP_LDFLAGS)

# Compile source files
$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(MONGOOSE_DIR)/%.o: $(MONGOOSE_DIR)/%.c
	$(CC) $(CFLAGS) -DMG_ENABLE_LINES=0 -c $< -o $@

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
	rm -f $(TARGET) $(SRC_DIR)/*.o $(MONGOOSE_DIR)/*.o

.PHONY: all clean run run-port test
