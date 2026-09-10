# ClayShards TUI Renderer
#
# Text UI renderer that processes Clay render commands and outputs
# ANSI escape sequences for modern terminal emulators.

# Every compiler difference lives in one place; see mk/toolchain.mk.
include ../../mk/toolchain.mk

CC ?= gcc
CFLAGS = $(strip $(CC_OPT_O2) $(CC_WARN) $(CC_WERROR) $(CC_STD) $(CC_PORT_DEFS))
# shared/ is here for sh_pal.h: terminal size and is-stdout-a-tty are
# platform primitives, and the house rule is that those live in the PAL
# rather than being #ifdef'd at the call site.
INCLUDES = -Iinclude -I../../vendor/clay -I../clay-shards/include \
           -I../../shared/include
SHARED_LIB = ../../shared/libshared.a

SOURCES = src/cs_tui.c
HEADERS = include/cs_tui.h src/cs_tui_internal.h

BUILD_DIR = build

.PHONY: all clean lib test

all: lib

# Static library
lib: $(BUILD_DIR)/libcs_tui.a

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/cs_tui.o: src/cs_tui.c $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< $(OBJ_OUT)$@

$(BUILD_DIR)/libcs_tui.a: $(BUILD_DIR)/cs_tui.o
	$(AR_CMD)$@ $^
	@echo "Built: $@"

# Unit tests
test: $(BUILD_DIR)/test_tui
	@echo "Running ClayShards TUI tests..."
	@./$(BUILD_DIR)/test_tui

$(SHARED_LIB):
	$(MAKE) -C ../../shared lib

$(BUILD_DIR)/test_tui: tests/test_tui.c src/cs_tui.c $(HEADERS) $(SHARED_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) tests/test_tui.c src/cs_tui.c \
	    $(call link_lib,../../shared,shared) $(LD_MATH) $(LD_PLATFORM) $(EXE_OUT)$@
	@echo "Built: $@"

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "ClayShards TUI Renderer (Library)"
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build library (default)"
	@echo "  lib     - Build static library"
	@echo "  test    - Build and run unit tests"
	@echo "  clean   - Remove build artifacts"
	@echo ""
	@echo "Demos are in ../demos/{tty,wasm}/"
