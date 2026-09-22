# Shared Library Makefile
# Common utilities for OTTO platform

# Every compiler difference lives in one place; see mk/toolchain.mk.
include ../mk/toolchain.mk

# Production build (with hardening)
CFLAGS = $(strip $(CC_WARN) $(CC_OPT) $(CC_STD) $(CC_DEFS) $(CC_PORT_DEFS) $(CC_HARDEN))
LDFLAGS = $(strip $(LD_MATH) $(LD_THREAD) $(LD_PLATFORM) $(LD_PIE) $(LD_RELRO) $(LD_STACK))

# Include paths
INCLUDES = -Iinclude -I../vendor/miniz

# Vendor path
VENDOR_DIR = ../vendor/miniz

# Debug build (with sanitizers)
DEBUG_CFLAGS = $(strip $(CC_WARN) $(CC_DEBUG_OPT) $(CC_STD) $(CC_DEFS) $(CC_PORT_DEFS) -DDEBUG $(CC_SANITIZE))
DEBUG_LDFLAGS = $(LD_SANITIZE)

# Source files (core library - no external dependencies beyond miniz)
SRCS = src/sh_pal_posix.c \
       src/sh_mem.c \
       src/sh_pal_windows.c \
       src/sh_api.c \
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

# Keel HTTP helpers. Deliberately not in libshared.a: that would put a transport
# vendor underneath Ralph and Shared and drag Keel into WASM builds that have no
# server (see docs/roadmaps/transport.md). Each API server compiles them itself,
# which is why both are named here but neither is in SRCS.
# Usage: $(CC) $(CFLAGS) -I../shared/include -I../vendor/keel/include \
#            src/sh_httpserver.c src/sh_httpasync.c
HTTPSERVER_SRC = src/sh_httpserver.c src/sh_httpasync.c

MINIZ_SRCS = $(VENDOR_DIR)/miniz.c \
             $(VENDOR_DIR)/miniz_tdef.c \
             $(VENDOR_DIR)/miniz_tinfl.c

# Object files
OBJS = $(SRCS:.c=.o)
MINIZ_OBJS = $(MINIZ_SRCS:.c=.o)
ALL_OBJS = $(OBJS) $(MINIZ_OBJS)

# Library
# Not LIB: that is MSVC's library search path. make re-exports any variable
# that also exists in the environment, so defining LIB here would replace the
# linker's search path with this filename and nothing would link.
LIB_FILE = libshared.a

# Discard objects built with different flags. See mk/flagstamp.mk -- this
# is the guard for `test-asan` leaving an ASan libshared.a that a later
# `make lib` calls current.
DEBUG_GOALS = debug test-asan
include ../mk/flagstamp.mk

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
TEST_PROTOBUF_BIN = test_protobuf
TEST_INFLATE_BIN = test_inflate

# Static analysis. Complements the sanitizers rather than repeating them:
# those need a path to be executed, cppcheck reads code no test reaches.
#
# The gating set is warning,performance,portability with
# --error-exitcode=1. `style` and `information` are left out deliberately --
# they report on taste and on headers cppcheck could not resolve, and
# neither should fail a build. cppcheck-all turns them on for reading.
cppcheck:
	cppcheck --enable=warning,performance,portability --suppress=missingIncludeSystem \
	  --error-exitcode=1 --inline-suppr --std=c11 $(INCLUDES) \
	  $(SRCS)

cppcheck-all:
	cppcheck --enable=all --suppress=missingIncludeSystem --suppress=unusedFunction \
	  --std=c11 $(INCLUDES) $(SRCS)
.PHONY: all clean test test-asan test-pal test-font test-json test-xml test-csv test-geojson test-fs test-sha256 test-eov test-pdf2struc test-protobuf test-inflate fuzz fuzz-lib fuzz-inflate fuzz-protobuf fuzz-pdf2struc fuzz-json fuzz-smoke font-compare lib debug generate-font-data help cppcheck cppcheck-all

# Default target
all: lib $(TEST_BIN)

# Build library
lib: $(LIB_FILE)

$(LIB_FILE): $(ALL_OBJS)
	$(AR_CMD)$@ $^

# Compile rules
src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< $(OBJ_OUT)$@

$(VENDOR_DIR)/%.o: $(VENDOR_DIR)/%.c
	$(CC) $(CFLAGS) -I$(VENDOR_DIR) -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -c $< $(OBJ_OUT)$@

# Test binaries
$(TEST_BIN): tests/test_shared.c $(LIB_FILE)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,shared) $(LDFLAGS) $(EXE_OUT)$@

$(TEST_FONT_BIN): tests/test_font.c $(LIB_FILE)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,shared) $(LDFLAGS) $(EXE_OUT)$@

$(TEST_JSON_BIN): tests/test_sh_json.c $(LIB_FILE)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,shared) $(LDFLAGS) $(EXE_OUT)$@

$(TEST_XML_BIN): tests/test_xml.c $(LIB_FILE)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,shared) $(LDFLAGS) $(EXE_OUT)$@

$(TEST_SHA256_BIN): tests/test_sha256.c $(LIB_FILE)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,shared) $(LDFLAGS) $(EXE_OUT)$@

$(TEST_CSV_BIN): tests/test_csv.c $(LIB_FILE)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,shared) $(LDFLAGS) $(EXE_OUT)$@

$(TEST_GEOJSON_BIN): tests/test_geojson.c $(LIB_FILE)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,shared) $(LDFLAGS) $(EXE_OUT)$@

$(TEST_FS_BIN): tests/test_fs.c $(LIB_FILE)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,shared) $(LDFLAGS) $(EXE_OUT)$@

$(TEST_EOV_BIN): tests/test_eov.c $(LIB_FILE)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run all tests
# test-asan is `clean test`, so anything missing from this list is never seen
# by a sanitizer. pdf2struc was missing: the parser with the largest untrusted
# attack surface in shared/ was the one the sanitizer job did not exercise.
test: $(TEST_BIN) $(TEST_FONT_BIN) $(TEST_JSON_BIN) $(TEST_XML_BIN) $(TEST_CSV_BIN) $(TEST_GEOJSON_BIN) $(TEST_FS_BIN) $(TEST_SHA256_BIN) $(TEST_EOV_BIN) $(TEST_PDF2STRUC_BIN) $(TEST_PROTOBUF_BIN) $(TEST_INFLATE_BIN)
	./$(TEST_BIN)
	./$(TEST_FONT_BIN)
	./$(TEST_JSON_BIN)
	./$(TEST_XML_BIN)
	./$(TEST_CSV_BIN)
	./$(TEST_GEOJSON_BIN)
	./$(TEST_FS_BIN)
	./$(TEST_SHA256_BIN)
	./$(TEST_EOV_BIN)
	./$(TEST_PDF2STRUC_BIN)
	./$(TEST_PROTOBUF_BIN)
	./$(TEST_INFLATE_BIN)

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
	$(AR_CMD)$@ $^

$(TEST_PROTOBUF_BIN): tests/test_protobuf.c $(LIB_FILE)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,shared) $(LDFLAGS) $(EXE_OUT)$@

$(TEST_INFLATE_BIN): tests/test_inflate.c $(LIB_FILE)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,shared) $(LDFLAGS) $(EXE_OUT)$@

$(TEST_PDF2STRUC_BIN): tests/test_pdf2struc.c $(PDF2STRUC_LIB) $(LIB_FILE)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,sh_pdf2struc) $(call link_lib,.,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run PDF2struc tests only
test-protobuf: $(TEST_PROTOBUF_BIN)
	./$(TEST_PROTOBUF_BIN)

test-inflate: $(TEST_INFLATE_BIN)
	./$(TEST_INFLATE_BIN)

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

$(FONT_COMPARE_BIN): tests/font_compare.c $(LIB_FILE)
	$(CC) $(CFLAGS) $(INCLUDES) $(CARTA_INCLUDES) $< $(call link_lib,.,shared) $(CARTA_LIBS) $(LDFLAGS) $(EXE_OUT)$@
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
# Platform abstraction layer only.
#
# Deliberately standalone: it links just sh_pal_*.c and its test, so the
# Windows CI job can run it today. The rest of shared/ does not build on
# Windows yet -- sh_fs.c calls mkdir with two arguments and sh_pdf2struc*.c
# call memmem -- and waiting for that would leave the PAL unverified on the
# platform it exists for.
TEST_PAL_BIN = test_pal

$(TEST_PAL_BIN): tests/test_pal.c src/sh_pal_posix.c src/sh_pal_windows.c
	$(CC) $(CFLAGS) $(INCLUDES) $^ $(LDFLAGS) $(EXE_OUT)$@

test-pal: $(TEST_PAL_BIN)
	./$(TEST_PAL_BIN)

test-asan: CFLAGS = $(DEBUG_CFLAGS)
test-asan: LDFLAGS += $(DEBUG_LDFLAGS)
# test-pal is named separately because it is not part of `test`: it is the
# only shared suite with its own target, and so was the only one never
# sanitized.
test-asan: clean test test-pal

debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS += $(DEBUG_LDFLAGS)
debug: clean all

# ============================================================================
# Fuzzing
#
# shared had no fuzz target, which is how sh_inflate and sh_protobuf ended up
# with no coverage of any kind. These three decode untrusted bytes: compressed
# streams and protobuf wire data arrive from PDF and OSM extracts, and the PDF
# extractor is the largest parser here.
#
# Both defects found by reading sh_pdf2struc -- the predictor's row_bytes + 1
# and the xref stream's negative /W -- are reachable from files under 500
# bytes, which is the size a fuzzer reaches in seconds. The corpora seed both.
#
# Requires clang. Run with a time budget, e.g.
#   ./fuzz_protobuf tests/fuzz/corpus_protobuf/ -max_total_time=60
# ============================================================================
FUZZ_CC = clang
FUZZ_CFLAGS = -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all               -g -O1 -std=c11 -D_GNU_SOURCE -D_USE_MATH_DEFINES
FUZZ_LIB = libshared_fuzz.a

fuzz: fuzz-inflate fuzz-protobuf fuzz-pdf2struc fuzz-json

fuzz-lib:
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(INCLUDES) -c $(SRCS)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(INCLUDES) -c $(PDF2STRUC_SRCS)
	# sh_inflate is a miniz wrapper, so the fuzz archive needs miniz in it
	# too -- the same flags the normal rule at the top of this file uses.
	$(FUZZ_CC) $(FUZZ_CFLAGS) -I$(VENDOR_DIR) -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -c $(MINIZ_SRCS)
	ar rcs $(FUZZ_LIB) $(notdir $(OBJS)) $(notdir $(PDF2STRUC_OBJS)) $(notdir $(MINIZ_OBJS))

fuzz-inflate: fuzz-lib
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(INCLUDES) tests/fuzz/fuzz_inflate.c 		-L. -lshared_fuzz -lm -o fuzz_inflate
	@echo "Run: ./fuzz_inflate tests/fuzz/corpus_inflate/ -max_total_time=60"

fuzz-protobuf: fuzz-lib
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(INCLUDES) tests/fuzz/fuzz_protobuf.c 		-L. -lshared_fuzz -lm -o fuzz_protobuf
	@echo "Run: ./fuzz_protobuf tests/fuzz/corpus_protobuf/ -max_total_time=60"

fuzz-pdf2struc: fuzz-lib
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(INCLUDES) tests/fuzz/fuzz_pdf2struc.c 		-L. -lshared_fuzz -lm -o fuzz_pdf2struc
	@echo "Run: ./fuzz_pdf2struc tests/fuzz/corpus_pdf2struc/ -max_total_time=60"

# sh_json is the only parser here fed straight from an HTTP request body --
# fuelwise, ralph, surge and velo all call sh_json_parse() on bytes off the
# wire -- and it was the last one with no fuzzer.
fuzz-json: fuzz-lib
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(INCLUDES) tests/fuzz/fuzz_json.c \
		-L. -lshared_fuzz -lm -o fuzz_json
	@echo "Run: ./fuzz_json work/ tests/fuzz/corpus_json/ -max_total_time=60"

# A bounded run over the seed corpora. Short enough for CI, which is the point:
# the fuzz targets that already existed in carta, nexus and surge are run by
# nothing, so they only find what someone remembers to look for.
fuzz-smoke: fuzz
	# The first directory is where libFuzzer writes what it finds; the seed
	# directory after it is only read. Passing the seeds alone has it write
	# its discoveries back into them -- a 15 second local run left 269 new
	# files in the corpora, which is how a curated seed set becomes noise.
	@mkdir -p $(FUZZ_WORK)/inflate $(FUZZ_WORK)/protobuf $(FUZZ_WORK)/pdf2struc $(FUZZ_WORK)/json
	./fuzz_inflate $(FUZZ_WORK)/inflate tests/fuzz/corpus_inflate/ \
		-max_total_time=$(FUZZ_SMOKE_SECONDS) -print_final_stats=1
	./fuzz_protobuf $(FUZZ_WORK)/protobuf tests/fuzz/corpus_protobuf/ \
		-max_total_time=$(FUZZ_SMOKE_SECONDS) -print_final_stats=1
	./fuzz_pdf2struc $(FUZZ_WORK)/pdf2struc tests/fuzz/corpus_pdf2struc/ \
		-max_total_time=$(FUZZ_SMOKE_SECONDS) -print_final_stats=1
	./fuzz_json $(FUZZ_WORK)/json tests/fuzz/corpus_json/ \
		-max_total_time=$(FUZZ_SMOKE_SECONDS) -print_final_stats=1

FUZZ_SMOKE_SECONDS ?= 30
FUZZ_WORK ?= build/fuzz-corpus

# Clean
clean:
	rm -f $(BUILD_STAMP)
	rm -f $(OBJS) $(MINIZ_OBJS) $(LIB_FILE) $(TEST_BIN) $(TEST_FONT_BIN) $(TEST_JSON_BIN) $(TEST_XML_BIN) $(TEST_CSV_BIN) $(TEST_GEOJSON_BIN) $(TEST_FS_BIN) $(TEST_SHA256_BIN) $(TEST_EOV_BIN) $(TEST_PROTOBUF_BIN) $(TEST_INFLATE_BIN) $(FONT_COMPARE_BIN) $(FUZZ_LIB) fuzz_inflate fuzz_protobuf fuzz_pdf2struc fuzz_json
	@rm -f *.obj *.pdb *.ilk *.exp 2>/dev/null || true
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
