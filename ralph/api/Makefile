# Ralph LP/MIP Solver - HTTP Server Makefile

CC = gcc

# Platform-specific RELRO (Linux only)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    RELRO_FLAGS = -Wl,-z,relro,-z,now
else
    RELRO_FLAGS =
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
LDFLAGS = -L.. -lralph -L$(SHARED_DIR) -lshared $(OPENMP_LDFLAGS) -lm -lpthread

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

# Integration test (uses port 8085 to avoid conflicts)
TEST_PORT = 8085
test: all
	@echo "Starting Ralph solver server for testing on port $(TEST_PORT)..."
	@./$(TARGET) -p $(TEST_PORT) &
	@SERVER_PID=$$!; \
	sleep 2; \
	echo ""; \
	echo "=== Testing Ralph Solver API ==="; \
	echo ""; \
	echo "Testing health endpoint..."; \
	HEALTH=$$(curl -s http://localhost:8085/api/v1/health); \
	echo "$$HEALTH"; \
	if echo "$$HEALTH" | grep -q '"status":"ok"'; then \
		echo "  PASS: Health endpoint works"; \
	else \
		echo "  FAIL: Health endpoint"; \
	fi; \
	echo ""; \
	echo "Testing formats endpoint..."; \
	FORMATS=$$(curl -s http://localhost:8085/api/v1/formats); \
	echo "$$FORMATS"; \
	if echo "$$FORMATS" | grep -q '"lp"'; then \
		echo "  PASS: Formats endpoint works"; \
	else \
		echo "  FAIL: Formats endpoint"; \
	fi; \
	echo ""; \
	echo "Testing solve endpoint (simple LP)..."; \
	SOLVE=$$(curl -s -X POST http://localhost:8085/api/v1/solve \
		-H "Content-Type: application/json" \
		-d '{"format":"lp","problem":"max: 5 x + 3 y\nsubject to\nwood: 2 x + 4 y <= 40\nlabor: 3 x + 2 y <= 24\nbounds\nx >= 0\ny >= 0\nend"}'); \
	echo "$$SOLVE" | head -20; \
	if echo "$$SOLVE" | grep -q '"status":"OPTIMAL"'; then \
		echo "  PASS: Solve endpoint works"; \
	else \
		echo "  FAIL: Solve endpoint"; \
	fi; \
	echo ""; \
	echo "Testing error handling (invalid LP)..."; \
	ERROR=$$(curl -s -X POST http://localhost:8085/api/v1/solve \
		-H "Content-Type: application/json" \
		-d '{"format":"lp","problem":"invalid syntax"}'); \
	echo "$$ERROR"; \
	if echo "$$ERROR" | grep -q '"error"'; then \
		echo "  PASS: Error handling works"; \
	else \
		echo "  FAIL: Error handling"; \
	fi; \
	echo ""; \
	echo "=== Ralph API Tests Complete ==="; \
	kill $$SERVER_PID 2>/dev/null || true
