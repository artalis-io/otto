# Shared Library Makefile
# Common utilities for OTTO platform

CC ?= gcc

# Platform-specific settings
UNAME_S := $(shell uname -s)

# Production build (with hardening)
# Note: -pie is implicit on macOS, only add on Linux
CFLAGS = -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE \
         -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -fno-common
ifeq ($(UNAME_S),Linux)
    LDFLAGS = -lm -lpthread -pie -Wl,-z,relro,-z,now
else
    LDFLAGS = -lm -lpthread
endif

# Include paths
INCLUDES = -Iinclude -I../vendor/miniz

# Vendor path
VENDOR_DIR = ../vendor/miniz

# Debug build (with sanitizers)
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -std=c11 -D_GNU_SOURCE -DDEBUG \
               -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS = -fsanitize=address,undefined

# Source files (core library - no external dependencies beyond miniz)
SRCS = src/sh_api.c \
       src/sh_transport_direct.c \
       src/sh_geo.c \
       src/sh_polyline.c \
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
       src/sh_perf.c \
       src/sh_log.c \
       src/sh_trace.c \
       src/sh_metrics.c \
       src/sh_hashmap.c \
       src/sh_heap.c \
       src/sh_pqueue.c \
       src/sh_spatial_grid.c \
       src/sh_query.c \
       src/sh_render.c \
       src/sh_units.c \
       src/sh_piecewise.c \
       src/sh_stepfunc.c \
       src/sh_dist.c \
       src/sh_json.c \
       src/sh_xml.c \
       src/sh_csv.c \
       src/sh_geojson.c \
       src/sh_hash_sha256.c \
       src/sh_fs.c \
       src/sh_eov.c

# Keel HTTP helpers (require Keel headers - compiled separately by API servers)
# Usage: $(CC) $(CFLAGS) -I../shared/include -I../vendor/keel/include \
#            src/sh_keelserver.c
KEELSERVER_SRC = src/sh_keelserver.c

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
TEST_JSON_BIN = test_sh_json
TEST_XML_BIN = test_xml
TEST_SHA256_BIN = test_sha256
TEST_CSV_BIN = test_csv
TEST_GEOJSON_BIN = test_geojson
TEST_FS_BIN = test_fs
TEST_EOV_BIN = test_eov

# PDF2struc - separate sub-system (not compiled into libshared.a)
PDF2STRUC_SRCS = src/sh_pdf2struc.c src/sh_pdf2struc_text.c
PDF2STRUC_OBJS = $(PDF2STRUC_SRCS:.c=.o)
PDF2STRUC_LIB = libsh_pdf2struc.a
TEST_PDF2STRUC_BIN = test_pdf2struc

.PHONY: all clean test test-asan test-font test-json test-xml test-csv test-geojson test-fs test-sha256 test-eov test-pdf2struc font-compare lib debug generate-font-data

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

$(TEST_JSON_BIN): tests/test_sh_json.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lshared $(LDFLAGS) -o $@

$(TEST_XML_BIN): tests/test_xml.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lshared $(LDFLAGS) -o $@

$(TEST_SHA256_BIN): tests/test_sha256.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lshared $(LDFLAGS) -o $@

$(TEST_CSV_BIN): tests/test_csv.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lshared $(LDFLAGS) -o $@

$(TEST_GEOJSON_BIN): tests/test_geojson.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lshared $(LDFLAGS) -o $@

$(TEST_FS_BIN): tests/test_fs.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lshared $(LDFLAGS) -o $@

$(TEST_EOV_BIN): tests/test_eov.c $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lshared $(LDFLAGS) -o $@

# Run all tests
test: $(TEST_BIN) $(TEST_FONT_BIN) $(TEST_JSON_BIN) $(TEST_XML_BIN) $(TEST_CSV_BIN) $(TEST_GEOJSON_BIN) $(TEST_FS_BIN) $(TEST_SHA256_BIN) $(TEST_EOV_BIN)
	./$(TEST_BIN)
	./$(TEST_FONT_BIN)
	./$(TEST_JSON_BIN)
	./$(TEST_XML_BIN)
	./$(TEST_CSV_BIN)
	./$(TEST_GEOJSON_BIN)
	./$(TEST_FS_BIN)
	./$(TEST_SHA256_BIN)
	./$(TEST_EOV_BIN)

# Run JSON tests only
test-json: $(TEST_JSON_BIN)
	./$(TEST_JSON_BIN)

# Run XML tests only
test-xml: $(TEST_XML_BIN)
	./$(TEST_XML_BIN)

# Run CSV tests only
test-csv: $(TEST_CSV_BIN)
	./$(TEST_CSV_BIN)

# Run GeoJSON tests only
test-geojson: $(TEST_GEOJSON_BIN)
	./$(TEST_GEOJSON_BIN)

# Run filesystem tests only
test-fs: $(TEST_FS_BIN)
	./$(TEST_FS_BIN)

# Run SHA-256 tests only
test-sha256: $(TEST_SHA256_BIN)
	./$(TEST_SHA256_BIN)

# Run EOV tests only
test-eov: $(TEST_EOV_BIN)
	./$(TEST_EOV_BIN)

# PDF2struc library (separate sub-system)
$(PDF2STRUC_LIB): $(PDF2STRUC_OBJS)
	ar rcs $@ $^

$(TEST_PDF2STRUC_BIN): tests/test_pdf2struc.c $(PDF2STRUC_LIB) $(LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lsh_pdf2struc -lshared $(LDFLAGS) -o $@

# Run PDF2struc tests only
test-pdf2struc: $(TEST_PDF2STRUC_BIN)
	./$(TEST_PDF2STRUC_BIN)

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

# Debug build (with sanitizers)
# Run the suite under AddressSanitizer + UBSan.
#
# Same flags as `debug`, but it runs the tests instead of only building them.
# A c-audit of the Phase 1 transport code found two heap overflows and an
# out-of-bounds write that ten warning flags had missed; sanitizers catch that
# class mechanically. See docs/roadmaps/transport.md.
test-asan: CFLAGS = $(DEBUG_CFLAGS)
test-asan: LDFLAGS += $(DEBUG_LDFLAGS)
test-asan: clean test

debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS += $(DEBUG_LDFLAGS)
debug: clean all

# Clean
clean:
	rm -f $(OBJS) $(MINIZ_OBJS) $(LIB) $(TEST_BIN) $(TEST_FONT_BIN) $(TEST_JSON_BIN) $(TEST_XML_BIN) $(TEST_CSV_BIN) $(TEST_GEOJSON_BIN) $(TEST_FS_BIN) $(TEST_SHA256_BIN) $(TEST_EOV_BIN) $(FONT_COMPARE_BIN)
	rm -f $(PDF2STRUC_OBJS) $(PDF2STRUC_LIB) $(TEST_PDF2STRUC_BIN)
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
src/sh_perf.o: include/sh_perf.h
src/sh_log.o: include/sh_log.h
src/sh_trace.o: include/sh_trace.h include/sh_log.h
src/sh_metrics.o: include/sh_metrics.h
src/sh_hashmap.o: include/sh_hashmap.h
src/sh_heap.o: include/sh_heap.h
src/sh_pqueue.o: include/sh_pqueue.h
src/sh_spatial_grid.o: include/sh_spatial_grid.h include/sh_geo.h
src/sh_render.o: include/sh_render.h
src/sh_units.o: include/sh_units.h
src/sh_polyline.o: include/sh_polyline.h include/sh_geo.h
src/sh_json.o: include/sh_json.h include/sh_arena.h
src/sh_xml.o: include/sh_xml.h include/sh_arena.h
src/sh_csv.o: include/sh_csv.h include/sh_arena.h
src/sh_geojson.o: include/sh_geojson.h include/sh_json.h
src/sh_hash_sha256.o: include/sh_hash_sha256.h
src/sh_fs.o: include/sh_fs.h
src/sh_pdf2struc.o: include/sh_pdf2struc.h include/sh_arena.h include/sh_inflate.h src/sh_pdf2struc_internal.h
src/sh_pdf2struc_text.o: include/sh_pdf2struc.h include/sh_arena.h src/sh_pdf2struc_internal.h

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
