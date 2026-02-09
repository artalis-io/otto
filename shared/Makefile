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

# Source files (core library - no external dependencies beyond miniz)
SRCS = src/sh_geo.c \
       src/sh_protobuf.c \
       src/sh_inflate.c \
       src/sh_pbf.c \
       src/sh_font.c \
       src/sh_font_data.c \
       src/sh_arena.c \
       src/sh_pool.c \
       src/sh_ratelimit.c \
       src/sh_workqueue.c \
       src/sh_completion.c \
       src/sh_worker_pool.c \
       src/sh_capacity.c \
       src/sh_args.c \
       src/sh_circuit.c \
       src/sh_backoff.c \
       src/sh_retry.c \
       src/sh_cors.c \
       src/sh_log.c \
       src/sh_trace.c \
       src/sh_metrics.c \
       src/sh_hashmap.c \
       src/sh_heap.c \
       src/sh_spatial_grid.c \
       src/sh_query.c \
       src/sh_render.c \
       src/sh_units.c \
       src/sh_piecewise.c \
       src/sh_stepfunc.c

# HTTP server components (requires mongoose - compile separately by API servers)
# API servers should compile sh_httpserver.c directly with mongoose includes.
# Usage: $(CC) $(CFLAGS) -I../shared/include -I../vendor/mongoose src/sh_httpserver.c
HTTPSERVER_SRC = src/sh_httpserver.c

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

.PHONY: all clean test test-font font-compare lib debug generate-font-data

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

# Font comparison tool (software vs WebGL)
FONT_COMPARE_BIN = tests/font_compare
CARTA_INCLUDES = -I../carta/include
CARTA_LIBS = -L../carta -lcarta

font-compare: $(FONT_COMPARE_BIN)
	@echo "Run: cd shared && python3 -m http.server 8000"
	@echo "Then open: http://localhost:8000/tests/compare.html"

$(FONT_COMPARE_BIN): tests/font_compare.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $(CARTA_INCLUDES) $< -L. -lshared $(CARTA_LIBS) $(LDFLAGS) -o $@
	./$(FONT_COMPARE_BIN) tests

# Generate font data from JSON/PNG (requires Python 3)
generate-font-data:
	python3 fonts/generate_font_data.py fonts/ui-font.json fonts/ui-font.png src/sh_font_data.c

# Debug build
debug: CFLAGS = $(DEBUG_CFLAGS)
debug: clean all

# Clean
clean:
	rm -f $(OBJS) $(MINIZ_OBJS) $(LIB) $(TEST_BIN) $(TEST_FONT_BIN) $(FONT_COMPARE_BIN)
	rm -f tests/soft_*.png tests/compare.html

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
src/sh_workqueue.o: include/sh_workqueue.h
src/sh_completion.o: include/sh_completion.h
src/sh_worker_pool.o: include/sh_worker_pool.h include/sh_workqueue.h
src/sh_capacity.o: include/sh_capacity.h
src/sh_args.o: include/sh_args.h
src/sh_circuit.o: include/sh_circuit.h
src/sh_backoff.o: include/sh_backoff.h
src/sh_retry.o: include/sh_retry.h include/sh_circuit.h include/sh_backoff.h
src/sh_cors.o: include/sh_cors.h
src/sh_log.o: include/sh_log.h
src/sh_trace.o: include/sh_trace.h include/sh_log.h
src/sh_metrics.o: include/sh_metrics.h
src/sh_hashmap.o: include/sh_hashmap.h
src/sh_heap.o: include/sh_heap.h
src/sh_spatial_grid.o: include/sh_spatial_grid.h include/sh_geo.h
src/sh_render.o: include/sh_render.h
src/sh_units.o: include/sh_units.h

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
