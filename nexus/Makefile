# Nexus Makefile
# Document ingestion pipeline for OTTO

CC ?= gcc

# Platform-specific settings
UNAME_S := $(shell uname -s)

# Production build (with hardening)
CFLAGS = -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE \
         -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -fno-common
ifeq ($(UNAME_S),Linux)
    LDFLAGS = -lm -pie -Wl,-z,relro,-z,now
else
    LDFLAGS = -lm
endif

INCLUDES = -Iinclude -I../shared/include -I../vendor/miniz

# Debug build (with sanitizers)
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -std=c11 -D_GNU_SOURCE -DDEBUG \
               -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS = -fsanitize=address,undefined

SRCS = src/nx_xlsx.c src/nx_pdf.c src/nx_xform.c src/nx_slug.c src/nx_ingest.c
OBJS = $(SRCS:.c=.o)

LIB = libnexus.a
TEST_INGEST_BIN = test_ingest
TEST_XFORM_BIN = test_xform

# CLI tools
NX_RUN_BIN = nx_run
NX_PDF_RUN_BIN = nx_pdf_run
NX_XFORM_RUN_BIN = nx_xform_run

SHARED_LIB = ../shared/libshared.a
VENDOR_DIR = ../vendor/miniz

# Vendor miniz objects (ZIP support for XLSX)
MINIZ_ZIP_SRC = $(VENDOR_DIR)/miniz_zip.c
MINIZ_ZIP_OBJ = $(MINIZ_ZIP_SRC:.c=.o)

LINK_LIBS = -L. -lnexus $(MINIZ_ZIP_OBJ) -L../shared -lshared $(LDFLAGS)

.PHONY: all clean test lib debug tools

all: lib $(TEST_INGEST_BIN) $(TEST_XFORM_BIN)

tools: $(NX_RUN_BIN) $(NX_PDF_RUN_BIN) $(NX_XFORM_RUN_BIN)

lib: $(LIB)

$(LIB): $(OBJS)
	ar rcs $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(SHARED_LIB):
	$(MAKE) -C ../shared lib

# miniz_zip.o may already exist from shared build, but we need it linked
$(MINIZ_ZIP_OBJ): $(MINIZ_ZIP_SRC)
	$(CC) $(CFLAGS) -I$(VENDOR_DIR) -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -c $< -o $@

$(TEST_INGEST_BIN): tests/test_ingest.c $(LIB) $(SHARED_LIB) $(MINIZ_ZIP_OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) -Itests $< $(LINK_LIBS) -o $@

$(TEST_XFORM_BIN): tests/test_xform.c $(LIB) $(SHARED_LIB) $(MINIZ_ZIP_OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) -Itests $< $(LINK_LIBS) -o $@

$(NX_RUN_BIN): tools/nx_run.c $(LIB) $(SHARED_LIB) $(MINIZ_ZIP_OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LINK_LIBS) -o $@

$(NX_PDF_RUN_BIN): tools/nx_pdf_run.c $(LIB) $(SHARED_LIB) $(MINIZ_ZIP_OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LINK_LIBS) -o $@

$(NX_XFORM_RUN_BIN): tools/nx_xform_run.c $(LIB) $(SHARED_LIB) $(MINIZ_ZIP_OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LINK_LIBS) -o $@

test: $(TEST_INGEST_BIN) $(TEST_XFORM_BIN)
	./$(TEST_INGEST_BIN)
	./$(TEST_XFORM_BIN)

debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS = -lm $(DEBUG_LDFLAGS)
debug: clean all

clean:
	rm -f $(OBJS) $(LIB) $(TEST_INGEST_BIN) $(TEST_XFORM_BIN) $(NX_RUN_BIN) $(NX_PDF_RUN_BIN) $(NX_XFORM_RUN_BIN)

help:
	@echo "Nexus Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build library and tests (default)"
	@echo "  lib      - Build static library (libnexus.a)"
	@echo "  tools    - Build CLI tools (nx_run, nx_pdf_run)"
	@echo "  test     - Build and run tests"
	@echo "  debug    - Build with sanitizers"
	@echo "  clean    - Remove build artifacts"
