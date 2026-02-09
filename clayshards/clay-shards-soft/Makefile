# ClayShards Software Renderer
#
# CPU-based rasterizer with platform backends.

CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE
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
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Cocoa backend (macOS only)
$(BUILD_DIR)/cs_soft_cocoa.o: src/platforms/cs_soft_cocoa.m src/cs_soft_internal.h include/cs_soft.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/libcs_soft.a: $(BUILD_DIR)/cs_soft.o $(COCOA_OBJ)
	ar rcs $@ $^
	@echo "Built: $@"

# Test binary
TEST_DEPS = ../clay-shards/build/libclay_shards.a ../../shared/libshared.a

$(BUILD_DIR)/test_soft: tests/test_soft.c $(BUILD_DIR)/libcs_soft.a | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< \
		-L$(BUILD_DIR) -lcs_soft \
		-L../clay-shards/build -lclay_shards \
		-L../../shared -lshared \
		$(PLATFORM_LIBS) -lm -o $@

test: $(BUILD_DIR)/test_soft
	@echo "Running software renderer tests..."
	@./$(BUILD_DIR)/test_soft

# Demo application
$(BUILD_DIR)/demo_soft: tests/demo_soft.c $(BUILD_DIR)/libcs_soft.a | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< \
		-L$(BUILD_DIR) -lcs_soft \
		-L../clay-shards/build -lclay_shards \
		-L../../shared -lshared \
		$(PLATFORM_LIBS) -lm -o $@

demo: $(BUILD_DIR)/demo_soft
	@echo "Running demo..."
	@./$(BUILD_DIR)/demo_soft

clean:
	rm -rf $(BUILD_DIR)

# Build dependencies
../clay-shards/build/libclay_shards.a:
	$(MAKE) -C ../clay-shards lib

../../shared/libshared.a:
	$(MAKE) -C ../../shared lib
