# Ralph LP/MIP Solver - HTTP Server Makefile

CC = gcc

# Platform-specific RELRO (Linux only)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    RELRO_FLAGS = -Wl,-z,relro,-z,now
else
    RELRO_FLAGS =
endif

# Windows: Keel talks to winsock, and libshared reaches bcrypt through
# sh_pal_random_bytes. iphlpapi comes with Keel's adapter enumeration.
ifneq (,$(findstring MINGW,$(UNAME_S)))
    PLATFORM_LIBS = -lws2_32 -lbcrypt -liphlpapi
else
    PLATFORM_LIBS =
endif

# Production build (with hardening)
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L \
         -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -fno-common
LDFLAGS_EXTRA = -pie $(RELRO_FLAGS)

# Debug build (with sanitizers)
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -std=c11 -D_POSIX_C_SOURCE=200809L -DDEBUG \
               -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS = -fsanitize=address,undefined

# Include paths
INCLUDES = -I../include -I../../shared/include -I$(KEEL_DIR)/include -Isrc

# Shared library path
SHARED_DIR = ../../shared

# OpenMP (required for Ralph's LAP solver)
ifeq ($(shell uname -s),Darwin)
    OPENMP_LDFLAGS = -L/opt/homebrew/opt/libomp/lib -lomp
else
    OPENMP_LDFLAGS = -fopenmp
endif

# Library paths (link shared library for arena allocator)
LDFLAGS = -L.. -lralph -L$(SHARED_DIR) -lshared $(OPENMP_LDFLAGS) -lm -lpthread $(PLATFORM_LIBS)

# Source files
SRCS = src/main.c
OBJS = $(SRCS:src/%.c=build/%.o)

# Keel static library (vendored git submodule, MIT)
KEEL_DIR = ../../vendor/keel
KEEL_LIB = $(KEEL_DIR)/libkeel.a

TARGET = ralph-solver-server

.PHONY: all clean deps run debug test

all: deps $(TARGET)

deps: $(KEEL_LIB)
	$(MAKE) -C .. lib
	$(MAKE) -C $(SHARED_DIR) lib

# Build vendored Keel. Core only (no mbedTLS/nghttp2 integrations), so
# this needs no external libraries.
$(KEEL_LIB):
	@if [ ! -f $(KEEL_DIR)/Makefile ]; then \
		echo "vendor/keel is empty - run: git submodule update --init vendor/keel"; \
		exit 1; \
	fi
	$(MAKE) -C $(KEEL_DIR) CC=$(CC)

$(TARGET): $(OBJS) $(KEEL_LIB)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(KEEL_LIB) $(LDFLAGS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build $(TARGET)

debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS += $(DEBUG_LDFLAGS)
debug: clean all

# Run the server
run: all
	./$(TARGET)

# Integration test. test_api.sh starts the server itself, picks its own
# port, and exits non-zero on failure -- so this target actually gates.
test: all
	@cd ../.. && bash ralph/api/test_api.sh
