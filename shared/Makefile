# Shared Library Makefile
# Common utilities for OTTO platform

CC ?= gcc
CFLAGS = -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE
LDFLAGS = -lm

# Include paths
INCLUDES = -Iinclude -I../vendor/miniz

# Vendor path
VENDOR_DIR = ../vendor/miniz

# Debug build
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -std=c11 -D_GNU_SOURCE -DDEBUG

# Source files
SRCS = src/sh_geo.c \
       src/sh_protobuf.c \
       src/sh_inflate.c \
       src/sh_pbf.c

MINIZ_SRCS = $(VENDOR_DIR)/miniz.c \
             $(VENDOR_DIR)/miniz_tdef.c \
             $(VENDOR_DIR)/miniz_tinfl.c

# Object files
OBJS = $(SRCS:.c=.o)
MINIZ_OBJS = $(MINIZ_SRCS:.c=.o)
ALL_OBJS = $(OBJS) $(MINIZ_OBJS)

# Library
LIB = libshared.a

# Test binary
TEST_BIN = test_shared

.PHONY: all clean test lib debug

# Default target
all: lib $(TEST_BIN)

# Build library
lib: $(LIB)

$(LIB): $(ALL_OBJS)
	ar rcs $@ $^

# Compile rules
src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(VENDOR_DIR)/%.o: $(VENDOR_DIR)/%.c
	$(CC) $(CFLAGS) -I$(VENDOR_DIR) -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -c $< -o $@

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
	rm -f $(OBJS) $(MINIZ_OBJS) $(LIB) $(TEST_BIN)

# Dependencies
src/sh_geo.o: include/sh_geo.h include/shared.h
src/sh_protobuf.o: include/sh_protobuf.h
src/sh_inflate.o: include/sh_inflate.h $(VENDOR_DIR)/miniz.h $(VENDOR_DIR)/miniz_tinfl.h
src/sh_pbf.o: include/sh_pbf.h include/sh_protobuf.h include/sh_inflate.h

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
