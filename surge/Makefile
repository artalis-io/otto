# Surge Makefile
# Rich VRPTW/PDPTW solver built on Arbor

CC ?= gcc

# Platform-specific RELRO (Linux only)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    RELRO_FLAGS = -Wl,-z,relro,-z,now
else
    RELRO_FLAGS =
endif

# Production build (with hardening)
# Note: -pie is implicit on macOS, only add on Linux
CFLAGS = -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE \
         -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -fno-common
ifeq ($(UNAME_S),Linux)
    LDFLAGS = -lm -pie $(RELRO_FLAGS)
else
    LDFLAGS = -lm
endif

INCLUDES = -Iinclude -I../arbor/include -I../shared/include

# Debug build (with sanitizers)
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -std=c11 -D_GNU_SOURCE -DDEBUG \
               -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS = -fsanitize=address,undefined

SRCS = src/surge.c src/sg_reader.c
OBJS = $(SRCS:.c=.o)

LIB = libsurge.a
TEST_BIN = test_surge
BENCH_BIN = bench_solomon

.PHONY: all clean test lib debug bench

all: lib $(TEST_BIN)

lib: $(LIB)

$(LIB): $(OBJS) ../arbor/libarbor.a ../shared/libshared.a
	ar rcs $@ $(OBJS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(TEST_BIN): tests/test_surge.c $(LIB) ../arbor/libarbor.a ../shared/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lsurge -L../arbor -larbor -L../shared -lshared $(LDFLAGS) -o $@

$(BENCH_BIN): benchmarks/bench_solomon.c $(LIB) ../arbor/libarbor.a ../shared/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lsurge -L../arbor -larbor -L../shared -lshared $(LDFLAGS) -o $@

../arbor/libarbor.a:
	$(MAKE) -C ../arbor lib

../shared/libshared.a:
	$(MAKE) -C ../shared lib

test: $(TEST_BIN)
	./$(TEST_BIN)

bench: $(BENCH_BIN)
	./$(BENCH_BIN) --dir benchmarks/solomon

debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS = -lm $(DEBUG_LDFLAGS)
debug: clean all

clean:
	rm -f $(OBJS) $(LIB) $(TEST_BIN) $(BENCH_BIN)

help:
	@echo "Surge Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build library and tests (default)"
	@echo "  lib      - Build static library (libsurge.a)"
	@echo "  test     - Build and run tests"
	@echo "  bench    - Build and run Solomon benchmark suite"
	@echo "  debug    - Build with debug symbols"
	@echo "  clean    - Remove build artifacts"
