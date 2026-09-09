# FuelWise - Truck Refueling Optimization Library
# Makefile

# FuelWise has always defaulted to gcc rather than make's built-in cc. A
# command-line CC=cl still wins, which is how MSVC gets in.
CC = gcc

# Every compiler difference lives in one place; see mk/toolchain.mk.
include ../mk/toolchain.mk


# Production build (with hardening)
# Note: -pie is implicit on macOS, only add on Linux
# No standard pinned on GCC: FuelWise builds in the default GNU mode, like Ralph.
CFLAGS = $(strip $(CC_WARN) $(CC_OPT) $(CC_ARCH) \
                 $(CC_FP_MODE) $(CC_FP_FASTMATH) $(CC_FP_KEEP_NONFINITE) \
                 $(CC_HARDEN) $(CC_STD_BASELINE) $(CC_PORT_DEFS))

# Debug build (with sanitizers)
CFLAGS_DEBUG = $(strip $(CC_WARN) $(CC_DEBUG_OPT) -DDEBUG $(CC_SANITIZE) $(CC_STD_BASELINE))
DEBUG_LDFLAGS = $(LD_SANITIZE)

INCLUDES = -I./include -I../ralph/include -I../shared/include
LDFLAGS = $(strip $(LD_MATH) $(LD_PLATFORM) $(LD_PIE) $(LD_RELRO) $(LD_STACK))

# Shared library (required by Ralph)
SHARED_DIR = ../shared
SHARED_LIB = $(SHARED_DIR)/libshared.a

# OpenMP support (required when linking against libralph.a which uses OpenMP)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    # macOS with Homebrew libomp
    OPENMP_CFLAGS = -Xclang -fopenmp
    OPENMP_LDFLAGS = $(call link_lib,/opt/homebrew/opt/libomp/lib,omp)
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
	$(AR_CMD)$@ $^

# Build object files
$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< $(OBJ_OUT)$@

# Build test executable
$(TEST_EXEC): $(TEST_SOURCES) $(LIB_NAME) $(RALPH_LIB) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,fuelwise) $(call link_lib,$(RALPH_DIR),ralph) $(call link_lib,$(SHARED_DIR),shared) $(LDFLAGS) $(EXE_OUT)$@

# Debug build (with sanitizers)
debug: CFLAGS = $(CFLAGS_DEBUG)
debug: LDFLAGS += $(DEBUG_LDFLAGS)
debug: clean all

# Run tests
test: test-transport test-regression

# Build transport tests
$(TRANSPORT_TEST_EXEC): $(TRANSPORT_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,fuelwise) $(call link_lib,$(RALPH_DIR),ralph) $(call link_lib,$(SHARED_DIR),shared) $(LDFLAGS) $(EXE_OUT)$@

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
