# FuelWise - Truck Refueling Optimization Library
# Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O3 -march=native -ffast-math
CFLAGS_DEBUG = -Wall -Wextra -g -O0 -DDEBUG
INCLUDES = -I./include -I../ralph/include -I../shared/include
LDFLAGS = -lm

# Shared library (required by Ralph)
SHARED_DIR = ../shared
SHARED_LIB = $(SHARED_DIR)/libshared.a

# OpenMP support (required when linking against libralph.a which uses OpenMP)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    # macOS with Homebrew libomp
    OPENMP_CFLAGS = -Xclang -fopenmp
    OPENMP_LDFLAGS = -L/opt/homebrew/opt/libomp/lib -lomp
else
    # Linux - native GCC OpenMP
    OPENMP_CFLAGS = -fopenmp
    OPENMP_LDFLAGS = -fopenmp
endif

LDFLAGS += $(OPENMP_LDFLAGS)

# Directories
SRC_DIR = src
INCLUDE_DIR = include
TEST_DIR = tests
RALPH_DIR = ../ralph

# Source files
SOURCES = $(SRC_DIR)/fw_geo.c \
          $(SRC_DIR)/fw_route.c \
          $(SRC_DIR)/fw_refuel.c \
          $(SRC_DIR)/fw_api.c \
          $(SRC_DIR)/fw_consumption.c \
          $(SRC_DIR)/fuelwise.c

OBJECTS = $(SOURCES:.c=.o)

# Library
LIB_NAME = libfuelwise.a

# Ralph library
RALPH_LIB = $(RALPH_DIR)/libralph.a

# Test executable
TEST_SOURCES = $(TEST_DIR)/test_fuelwise.c
TEST_EXEC = test_fuelwise

# Default target
all: shared ralph lib $(TEST_EXEC)

# Build library only
lib: $(LIB_NAME)

# Build shared library first
shared:
	$(MAKE) -C $(SHARED_DIR) lib

# Build Ralph (depends on shared)
ralph: shared
	$(MAKE) -C $(RALPH_DIR) lib

$(LIB_NAME): $(OBJECTS)
	ar rcs $@ $^

# Build object files
$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build test executable
$(TEST_EXEC): $(TEST_SOURCES) $(LIB_NAME) $(RALPH_LIB) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lfuelwise -L$(RALPH_DIR) -lralph -L$(SHARED_DIR) -lshared $(LDFLAGS) -o $@

# Debug build
debug: CFLAGS = $(CFLAGS_DEBUG)
debug: clean all

# Run tests
test: $(TEST_EXEC)
	./$(TEST_EXEC)

# Clean
clean:
	rm -f $(OBJECTS) $(LIB_NAME) $(TEST_EXEC)

# Clean all including Ralph
clean-all: clean
	$(MAKE) -C $(RALPH_DIR) clean

# Install headers and library
install: $(LIB_NAME)
	mkdir -p /usr/local/include/fuelwise
	mkdir -p /usr/local/lib
	cp $(INCLUDE_DIR)/*.h /usr/local/include/fuelwise/
	cp $(LIB_NAME) /usr/local/lib/

# Uninstall
uninstall:
	rm -rf /usr/local/include/fuelwise
	rm -f /usr/local/lib/$(LIB_NAME)

.PHONY: all lib clean clean-all debug test wasm install uninstall ralph shared help

# Build WASM (delegates to wasm/ subdirectory)
wasm:
	$(MAKE) -C wasm

# Help
help:
	@echo "FuelWise Refueling Library Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build library and tests (default)"
	@echo "  lib      - Build static library (libfuelwise.a)"
	@echo "  test     - Build and run tests"
	@echo "  wasm     - Build WebAssembly (requires Emscripten)"
	@echo "  debug    - Build with debug symbols"
	@echo "  clean    - Remove build artifacts"
