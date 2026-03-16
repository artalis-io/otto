# Nexus Makefile
# Document ingestion pipeline for OTTO

CC ?= gcc

# Platform-specific settings
UNAME_S := $(shell uname -s)

# Production build (with hardening)
CFLAGS = -Wall -Wextra -Werror -O3 -std=c11 -D_GNU_SOURCE \
         -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -fno-common
ifeq ($(UNAME_S),Linux)
    LDFLAGS = -lm -pie -Wl,-z,relro,-z,now
else
    LDFLAGS = -lm
endif

INCLUDES = -Iinclude -I../shared/include -isystem ../vendor/miniz

# Debug build (with sanitizers)
DEBUG_CFLAGS = -Wall -Wextra -Werror -g -O0 -std=c11 -D_GNU_SOURCE -DDEBUG \
               -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS = -fsanitize=address,undefined

SRCS = src/nx_xlsx.c src/nx_pdf.c src/nx_csv.c src/nx_xform.c src/nx_slug.c src/nx_merge.c src/nx_ingest.c src/nx_compute.c src/nx_validate.c src/nx_discover.c src/nx_emit.c src/nx_issue.c src/nx_diff.c
OBJS = $(SRCS:.c=.o)

LIB = libnexus.a
TEST_INGEST_BIN = test_ingest
TEST_XFORM_BIN = test_xform
TEST_VALIDATE_BIN = test_validate
TEST_DISCOVER_BIN = test_discover
TEST_MERGE_BIN = test_merge
TEST_EMIT_BIN = test_emit
TEST_ISSUE_BIN = test_issue
TEST_DIFF_BIN = test_diff
TEST_PIPELINE_BIN = test_pipeline

# CLI tools
NX_RUN_BIN = nx_run
NX_PDF_RUN_BIN = nx_pdf_run
NX_XFORM_RUN_BIN = nx_xform_run
NX_PIPELINE_BIN = nx_pipeline

SHARED_LIB = ../shared/libshared.a
PDF2STRUC_LIB = ../shared/libsh_pdf2struc.a
VENDOR_DIR = ../vendor/miniz

# Vendor miniz objects (ZIP support for XLSX)
MINIZ_ZIP_SRC = $(VENDOR_DIR)/miniz_zip.c
MINIZ_ZIP_OBJ = $(MINIZ_ZIP_SRC:.c=.o)

LINK_LIBS = -L. -lnexus $(MINIZ_ZIP_OBJ) -L../shared -lshared $(LDFLAGS)
LINK_LIBS_PDF = -L. -lnexus $(MINIZ_ZIP_OBJ) -L../shared -lsh_pdf2struc -lshared $(LDFLAGS)

.PHONY: all clean test lib debug tools fuzz fuzz-lib fuzz-xlsx fuzz-pdf fuzz-csv

all: lib $(TEST_INGEST_BIN) $(TEST_XFORM_BIN) $(TEST_VALIDATE_BIN) $(TEST_DISCOVER_BIN) $(TEST_MERGE_BIN) $(TEST_EMIT_BIN) $(TEST_ISSUE_BIN) $(TEST_DIFF_BIN) $(TEST_PIPELINE_BIN)

tools: $(NX_RUN_BIN) $(NX_PDF_RUN_BIN) $(NX_XFORM_RUN_BIN) $(NX_PIPELINE_BIN)

lib: $(LIB)

$(LIB): $(OBJS)
	ar rcs $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(SHARED_LIB):
	$(MAKE) -C ../shared lib

# miniz_zip.o may already exist from shared build, but we need it linked
# Vendor code: no -Werror (upstream warnings are not our problem)
VENDOR_CFLAGS = -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE -fPIE
$(MINIZ_ZIP_OBJ): $(MINIZ_ZIP_SRC)
	$(CC) $(VENDOR_CFLAGS) -isystem $(VENDOR_DIR) -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -c $< -o $@

$(TEST_INGEST_BIN): tests/test_ingest.c $(LIB) $(SHARED_LIB) $(MINIZ_ZIP_OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) -Itests $< $(LINK_LIBS) -o $@

$(TEST_XFORM_BIN): tests/test_xform.c $(LIB) $(SHARED_LIB) $(MINIZ_ZIP_OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) -Itests $< $(LINK_LIBS) -o $@

$(TEST_VALIDATE_BIN): tests/test_validate.c $(LIB) $(SHARED_LIB) $(MINIZ_ZIP_OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) -Itests $< $(LINK_LIBS) -o $@

$(TEST_DISCOVER_BIN): tests/test_discover.c $(LIB) $(SHARED_LIB) $(MINIZ_ZIP_OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) -Itests $< $(LINK_LIBS) -o $@

$(TEST_MERGE_BIN): tests/test_merge.c $(LIB) $(SHARED_LIB) $(MINIZ_ZIP_OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) -Itests $< $(LINK_LIBS) -o $@

$(TEST_EMIT_BIN): tests/test_emit.c $(LIB) $(SHARED_LIB) $(MINIZ_ZIP_OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) -Itests $< $(LINK_LIBS) -o $@

$(TEST_ISSUE_BIN): tests/test_issue.c $(LIB) $(SHARED_LIB) $(MINIZ_ZIP_OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) -Itests $< $(LINK_LIBS) -o $@

$(TEST_DIFF_BIN): tests/test_diff.c $(LIB) $(SHARED_LIB) $(MINIZ_ZIP_OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) -Itests $< $(LINK_LIBS) -o $@

$(TEST_PIPELINE_BIN): tests/test_pipeline.c $(LIB) $(SHARED_LIB) $(MINIZ_ZIP_OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) -Itests $< $(LINK_LIBS) -o $@

$(NX_RUN_BIN): tools/nx_run.c $(LIB) $(SHARED_LIB) $(MINIZ_ZIP_OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LINK_LIBS) -o $@

$(NX_PDF_RUN_BIN): tools/nx_pdf_run.c $(LIB) $(SHARED_LIB) $(MINIZ_ZIP_OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LINK_LIBS) -o $@

$(NX_XFORM_RUN_BIN): tools/nx_xform_run.c $(LIB) $(SHARED_LIB) $(MINIZ_ZIP_OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LINK_LIBS) -o $@

$(PDF2STRUC_LIB):
	$(MAKE) -C ../shared libsh_pdf2struc.a

$(NX_PIPELINE_BIN): tools/nx_pipeline.c $(LIB) $(PDF2STRUC_LIB) $(SHARED_LIB) $(MINIZ_ZIP_OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LINK_LIBS_PDF) -o $@

test: $(TEST_INGEST_BIN) $(TEST_XFORM_BIN) $(TEST_VALIDATE_BIN) $(TEST_DISCOVER_BIN) $(TEST_MERGE_BIN) $(TEST_EMIT_BIN) $(TEST_ISSUE_BIN) $(TEST_DIFF_BIN) $(TEST_PIPELINE_BIN)
	./$(TEST_INGEST_BIN)
	./$(TEST_XFORM_BIN)
	./$(TEST_VALIDATE_BIN)
	./$(TEST_DISCOVER_BIN)
	./$(TEST_MERGE_BIN)
	./$(TEST_EMIT_BIN)
	./$(TEST_ISSUE_BIN)
	./$(TEST_DIFF_BIN)
	./$(TEST_PIPELINE_BIN)

debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS = -lm $(DEBUG_LDFLAGS)
debug: clean all

# Fuzzing build (requires clang with libFuzzer)
FUZZ_CC = clang
FUZZ_CFLAGS = -fsanitize=fuzzer,address,undefined -g -O1 -std=c11 -D_GNU_SOURCE
FUZZ_LDFLAGS = -Wl,-ld_classic
FUZZ_LIB = libnexus_fuzz.a
FUZZ_LINK = -L. -lnexus_fuzz $(MINIZ_ZIP_OBJ) -L../shared -lshared -lm

fuzz: fuzz-xlsx fuzz-pdf fuzz-csv

fuzz-lib:
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(INCLUDES) -c $(SRCS)
	ar rcs $(FUZZ_LIB) $(OBJS)
	$(MAKE) -C ../shared CC=$(FUZZ_CC) CFLAGS="$(FUZZ_CFLAGS)" clean lib
	$(FUZZ_CC) $(FUZZ_CFLAGS) -isystem $(VENDOR_DIR) -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -c $(MINIZ_ZIP_SRC) -o $(MINIZ_ZIP_OBJ)

fuzz-xlsx: fuzz-lib
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_LDFLAGS) $(INCLUDES) tests/fuzz/fuzz_xlsx.c \
		$(FUZZ_LINK) -o fuzz_xlsx
	@echo "Run: ./fuzz_xlsx tests/fuzz/corpus_xlsx/ -max_len=1000000 -max_total_time=60"

fuzz-pdf: fuzz-lib
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_LDFLAGS) $(INCLUDES) tests/fuzz/fuzz_pdf.c \
		$(FUZZ_LINK) -o fuzz_pdf
	@echo "Run: ./fuzz_pdf tests/fuzz/corpus_pdf/ -max_len=1000000 -max_total_time=60"

fuzz-csv: fuzz-lib
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_LDFLAGS) $(INCLUDES) tests/fuzz/fuzz_csv.c \
		$(FUZZ_LINK) -o fuzz_csv
	@echo "Run: ./fuzz_csv tests/fuzz/corpus_csv/ -max_len=1000000 -max_total_time=60"

clean:
	rm -f $(OBJS) $(LIB) $(FUZZ_LIB) fuzz_xlsx fuzz_pdf fuzz_csv $(MINIZ_ZIP_OBJ)
	rm -f $(TEST_INGEST_BIN) $(TEST_XFORM_BIN) $(TEST_VALIDATE_BIN) $(TEST_DISCOVER_BIN) $(TEST_MERGE_BIN) $(TEST_EMIT_BIN) $(TEST_ISSUE_BIN) $(TEST_DIFF_BIN) $(TEST_PIPELINE_BIN) $(NX_RUN_BIN) $(NX_PDF_RUN_BIN) $(NX_XFORM_RUN_BIN) $(NX_PIPELINE_BIN)

help:
	@echo "Nexus Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build library and tests (default)"
	@echo "  lib      - Build static library (libnexus.a)"
	@echo "  tools    - Build CLI tools (nx_run, nx_pdf_run, nx_pipeline)"
	@echo "  test     - Build and run tests"
	@echo "  debug    - Build with sanitizers"
	@echo "  clean    - Remove build artifacts"
