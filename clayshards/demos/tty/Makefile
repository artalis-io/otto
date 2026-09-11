# ClayShards TUI TTY Demo
#
# Native terminal demo using the TUI renderer.

# Every compiler difference lives in one place; see mk/toolchain.mk.
include ../../../mk/toolchain.mk

CC ?= gcc
CFLAGS = $(strip $(CC_OPT_O2) $(CC_WARN) $(CC_WERROR) $(CC_STD) $(CC_PORT_DEFS))

# Include paths (from demos/tty/)
# shared/ is for sh_pal.h and sh_time.h: raw terminal mode, terminal size and
# the monotonic clock are platform primitives, and the house rule is that
# those live in the PAL rather than being #ifdef'd at the call site.
INCLUDES = -I../../clay-shards-tui/include \
           -I../../clay-shards/include \
           -I../../../vendor/clay \
           -I../../../shared/include

SHARED_LIB = ../../../shared/libshared.a

# Debug build
DEBUG_CFLAGS = -g -O0 -fsanitize=address,undefined

# Sources
DEMO_SRC = src/demo_tui_tty.c
TUI_SRC = ../../clay-shards-tui/src/cs_tui.c
CS_SRC = ../../clay-shards/src/cs_immediate.c

BUILD_DIR = build

.PHONY: all clean debug help

all: $(BUILD_DIR)/demo_tui_tty

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(SHARED_LIB):
	$(MAKE) -C ../../../shared lib

$(BUILD_DIR)/demo_tui_tty: $(DEMO_SRC) $(TUI_SRC) $(CS_SRC) $(SHARED_LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $(DEMO_SRC) $(TUI_SRC) $(CS_SRC) \
	    $(call link_lib,../../../shared,shared) $(LD_MATH) $(LD_PLATFORM) $(EXE_OUT)$@
	@echo "Built: $@"
	@echo "Run with: ./$@"

# Debug build with sanitizers
debug: $(BUILD_DIR)/demo_tui_tty_debug

$(BUILD_DIR)/demo_tui_tty_debug: $(DEMO_SRC) $(TUI_SRC) $(CS_SRC) | $(BUILD_DIR)
	$(CC) $(DEBUG_CFLAGS) $(INCLUDES) $(DEMO_SRC) $(TUI_SRC) $(CS_SRC) -o $@ -lm
	@echo "Built: $@ (with sanitizers)"

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "ClayShards TUI TTY Demo"
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build demo (default)"
	@echo "  debug   - Build with sanitizers"
	@echo "  clean   - Remove build artifacts"
