# Shared Library Makefile
# Common utilities for FuelWise platform

CC ?= gcc
CFLAGS = -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE
LDFLAGS = -lm

# Include paths
INCLUDES = -Iinclude

# Debug build
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -std=c11 -D_GNU_SOURCE -DDEBUG

# Source files
SRCS = src/sh_geo.c

# Object files
OBJS = $(SRCS:.c=.o)

# Library
LIB = libshared.a

# Test binary
TEST_BIN = test_shared

.PHONY: all clean test lib debug

# Default target
all: lib $(TEST_BIN)

# Build library
lib: $(LIB)

$(LIB): $(OBJS)
	ar rcs $@ $^

# Compile rules
src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Test binary
$(TEST_BIN): tests/test_shared.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lshared $(LDFLAGS) -o $@

# Run tests
test: $(TEST_BIN)
	./$(TEST_BIN)

# Debug build
debug: CFLAGS = $(DEBUG_CFLAGS)
debug: clean all

# Clean
clean:
	rm -f $(OBJS) $(LIB) $(TEST_BIN)

# Dependencies
src/sh_geo.o: include/sh_geo.h include/shared.h

# Help
help:
	@echo "Shared Library Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build library and tests (default)"
	@echo "  lib      - Build static library (libshared.a)"
	@echo "  test     - Build and run tests"
	@echo "  debug    - Build with debug symbols"
	@echo "  clean    - Remove build artifacts"
