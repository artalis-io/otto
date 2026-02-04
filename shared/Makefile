# Shared Library Makefile
# Common utilities for OTTO platform

CC ?= gcc
CFLAGS = -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE
LDFLAGS = -lm -lpthread

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
       src/sh_pbf.c \
       src/sh_font.c \
       src/sh_font_data.c \
       src/sh_arena.c \
       src/sh_pool.c \
       src/sh_ratelimit.c

MINIZ_SRCS = $(VENDOR_DIR)/miniz.c \
             $(VENDOR_DIR)/miniz_tdef.c \
             $(VENDOR_DIR)/miniz_tinfl.c

# Object files
OBJS = $(SRCS:.c=.o)
MINIZ_OBJS = $(MINIZ_SRCS:.c=.o)
ALL_OBJS = $(OBJS) $(MINIZ_OBJS)

# Library
LIB = libshared.a

# Test binaries
TEST_BIN = test_shared
TEST_FONT_BIN = test_font

.PHONY: all clean test test-font lib debug generate-font-data

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

# Test binaries
$(TEST_BIN): tests/test_shared.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lshared $(LDFLAGS) -o $@

$(TEST_FONT_BIN): tests/test_font.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lshared $(LDFLAGS) -o $@

# Run all tests
test: $(TEST_BIN) $(TEST_FONT_BIN)
	./$(TEST_BIN)
	./$(TEST_FONT_BIN)

# Run font tests only
test-font: $(TEST_FONT_BIN)
	./$(TEST_FONT_BIN)

# Generate font data from JSON/PNG (requires Python 3)
generate-font-data:
	python3 fonts/generate_font_data.py fonts/ui-font.json fonts/ui-font.png src/sh_font_data.c

# Debug build
debug: CFLAGS = $(DEBUG_CFLAGS)
debug: clean all

# Clean
clean:
	rm -f $(OBJS) $(MINIZ_OBJS) $(LIB) $(TEST_BIN) $(TEST_FONT_BIN)

# Dependencies
src/sh_geo.o: include/sh_geo.h include/shared.h
src/sh_protobuf.o: include/sh_protobuf.h
src/sh_inflate.o: include/sh_inflate.h $(VENDOR_DIR)/miniz.h $(VENDOR_DIR)/miniz_tinfl.h
src/sh_pbf.o: include/sh_pbf.h include/sh_protobuf.h include/sh_inflate.h
src/sh_font.o: include/sh_font.h
src/sh_font_data.o: include/sh_font.h
src/sh_arena.o: include/sh_arena.h
src/sh_pool.o: include/sh_pool.h
src/sh_ratelimit.o: include/sh_ratelimit.h

# Help
help:
	@echo "Shared Library Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all                 - Build library and tests (default)"
	@echo "  lib                 - Build static library (libshared.a)"
	@echo "  test                - Build and run all tests"
	@echo "  test-font           - Build and run font tests only"
	@echo "  generate-font-data  - Regenerate src/sh_font_data.c from fonts/"
	@echo "  debug               - Build with debug symbols"
	@echo "  clean               - Remove build artifacts"
