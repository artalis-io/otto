# FuelWise - Truck Refueling Optimization Library
# Makefile

CC = gcc

# Platform-specific RELRO (Linux only)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    RELRO_FLAGS = -Wl,-z,relro,-z,now
else
    RELRO_FLAGS =
endif

# Production build (with hardening)
# Note: -pie is implicit on macOS, only add on Linux
CFLAGS = -Wall -Wextra -O3 -march=native -ffast-math -fno-finite-math-only \
         -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -fno-common

# Debug build (with sanitizers)
CFLAGS_DEBUG = -Wall -Wextra -g -O0 -DDEBUG \
               -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS = -fsanitize=address,undefined

INCLUDES = -I./include -I../ralph/include -I../shared/include
ifeq ($(UNAME_S),Linux)
    LDFLAGS = -lm -pie $(RELRO_FLAGS)
else
    LDFLAGS = -lm
endif

# libshared now reaches the platform through sh_pal: bcrypt supplies
# sh_pal_random_bytes, ws2_32 the StatsD sink in sh_metrics.
ifneq (,$(findstring MINGW,$(UNAME_S)))
    LDFLAGS += -lbcrypt -lws2_32
endif

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

# Transport abstraction leak detector -- builds with no transport library on
# the include path. See docs/roadmaps/transport.md.
TRANSPORT_TEST_SOURCES = $(TEST_DIR)/test_transport_direct.c
TRANSPORT_TEST_EXEC = test_transport_direct

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

# Debug build (with sanitizers)
debug: CFLAGS = $(CFLAGS_DEBUG)
debug: LDFLAGS += $(DEBUG_LDFLAGS)
debug: clean all

# Run tests
test: test-transport test-regression

# Build transport tests
$(TRANSPORT_TEST_EXEC): $(TRANSPORT_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lfuelwise -L$(RALPH_DIR) -lralph -L$(SHARED_DIR) -lshared $(LDFLAGS) -o $@

# Run transport tests
# shared/ralph are phony, and $(SHARED_LIB)/$(RALPH_LIB) have no rule here --
# same reason `all` lists them first.
test-transport: shared ralph $(TRANSPORT_TEST_EXEC)
	./$(TRANSPORT_TEST_EXEC)

test-regression: test-unit test-bench

# The unit suite on its own -- seconds, no external tools.
test-unit: $(TEST_EXEC)
	./$(TEST_EXEC)

# The benchmark regression harness. Separate from test-unit because it is much
# slower and because it shells out to glpsol, so a CI job can take one without
# the other.
test-bench: shared ralph $(LIB_NAME)
	@echo ""
	@echo "Running full benchmark regression harness..."
	$(MAKE) -C bench regression-test

# Clean
clean:
	rm -f $(OBJECTS) $(LIB_NAME) $(TEST_EXEC) $(TRANSPORT_TEST_EXEC)
	$(MAKE) -C bench clean

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

.PHONY: all lib clean clean-all debug test test-regression test-unit test-bench test-transport wasm install uninstall ralph shared help

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
	@echo "  test     - Build and run FuelWise plus full regression harness"
	@echo "  test-regression - Run FuelWise tests plus validator + Ralph LP/MIP suites"
	@echo "  wasm     - Build WebAssembly (requires Emscripten)"
	@echo "  debug    - Build with debug symbols"
	@echo "  clean    - Remove build artifacts"
