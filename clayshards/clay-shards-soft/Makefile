# ClayShards Software Renderer
#
# CPU-based rasterizer with platform backends.

# Every compiler difference lives in one place; see mk/toolchain.mk.
include ../../mk/toolchain.mk

CC = gcc
CFLAGS = $(strip $(CC_OPT_O2) $(CC_WARN) $(CC_STD) $(CC_DEFS) $(CC_PORT_DEFS))
INCLUDES = -Iinclude -I../clay-shards/include -I../../shared/include -I../../vendor/clay

# Platform-specific flags
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    X11_CFLAGS = -DHAS_X11
    X11_LIBS = -lX11 -lXext
    PLATFORM_LIBS =
    COCOA_OBJ =
endif
ifeq ($(UNAME_S),Darwin)
    # macOS uses Cocoa backend
    X11_CFLAGS =
    X11_LIBS =
    PLATFORM_LIBS = -framework Cocoa -framework CoreGraphics
    COCOA_OBJ = $(BUILD_DIR)/cs_soft_cocoa.o
endif

# Sources
CORE_SRCS = src/cs_soft.c

BUILD_DIR = build

.PHONY: all clean lib test demo

all: lib

# Static library
lib: $(BUILD_DIR)/libcs_soft.a

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/cs_soft.o: src/cs_soft.c src/cs_soft_internal.h include/cs_soft.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< $(OBJ_OUT)$@

# Cocoa backend (macOS only)
$(BUILD_DIR)/cs_soft_cocoa.o: src/platforms/cs_soft_cocoa.m src/cs_soft_internal.h include/cs_soft.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< $(OBJ_OUT)$@

$(BUILD_DIR)/libcs_soft.a: $(BUILD_DIR)/cs_soft.o $(COCOA_OBJ)
	$(AR_CMD)$@ $^
	@echo "Built: $@"

# Test binary
TEST_DEPS = ../clay-shards/build/libclay_shards.a ../../shared/libshared.a

# TEST_DEPS above was declared, and the rules to build those archives exist
# further down, but nothing ever depended on them -- so the rules never fired
# and a standalone `make test` here died at link on -lclay_shards.
$(BUILD_DIR)/test_soft: tests/test_soft.c $(BUILD_DIR)/libcs_soft.a $(TEST_DEPS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< \
		$(call link_lib,$(BUILD_DIR),cs_soft) \
		$(call link_lib,../clay-shards/build,clay_shards) \
		$(call link_lib,../../shared,shared) \
		$(PLATFORM_LIBS) $(LD_MATH) $(LD_PLATFORM) $(EXE_OUT)$@

test: $(BUILD_DIR)/test_soft
	@echo "Running software renderer tests..."
	@./$(BUILD_DIR)/test_soft

# Demo application
$(BUILD_DIR)/demo_soft: tests/demo_soft.c $(BUILD_DIR)/libcs_soft.a $(TEST_DEPS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< \
		$(call link_lib,$(BUILD_DIR),cs_soft) \
		$(call link_lib,../clay-shards/build,clay_shards) \
		$(call link_lib,../../shared,shared) \
		$(PLATFORM_LIBS) $(LD_MATH) $(LD_PLATFORM) $(EXE_OUT)$@

# DEMO_ARGS exists because only the Cocoa backend can deliver the ESC key
# that closes the window. Everywhere else cs_soft falls back to the headless
# framebuffer, which has no input at all, so an unbounded run never returns:
#   make demo DEMO_ARGS="--frames 120"
DEMO_ARGS ?=

demo: $(BUILD_DIR)/demo_soft
	@echo "Running demo..."
	@./$(BUILD_DIR)/demo_soft $(DEMO_ARGS)

clean:
	rm -rf $(BUILD_DIR)

# Build dependencies
../clay-shards/build/libclay_shards.a:
	$(MAKE) -C ../clay-shards lib

../../shared/libshared.a:
	$(MAKE) -C ../../shared lib
