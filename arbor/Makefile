# Arbor Makefile
# Reusable optimization/search framework (ALNS core first)

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

INCLUDES = -Iinclude -I../shared/include

# Debug build (with sanitizers)
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -std=c11 -D_GNU_SOURCE -DDEBUG \
               -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS = -fsanitize=address,undefined

SRCS = src/arbor.c \
       src/ar_alns.c \
       src/ar_operators.c

OBJS = $(SRCS:.c=.o)

LIB = libarbor.a
TEST_BIN = test_arbor
TEST_OP_BIN = test_ar_operators

.PHONY: all clean test lib debug

all: lib $(TEST_BIN) $(TEST_OP_BIN)

lib: $(LIB)

$(LIB): $(OBJS)
	ar rcs $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(TEST_BIN): tests/test_arbor.c $(LIB) ../shared/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -larbor -L../shared -lshared $(LDFLAGS) -o $@

$(TEST_OP_BIN): tests/test_ar_operators.c $(LIB) ../shared/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -larbor -L../shared -lshared $(LDFLAGS) -o $@

../shared/libshared.a:
	$(MAKE) -C ../shared lib

test: $(TEST_BIN) $(TEST_OP_BIN)
	./$(TEST_BIN)
	./$(TEST_OP_BIN)

debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS = -lm $(DEBUG_LDFLAGS)
debug: clean all

clean:
	rm -f $(OBJS) $(LIB) $(TEST_BIN) $(TEST_OP_BIN)

help:
	@echo "Arbor Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build library and tests (default)"
	@echo "  lib      - Build static library (libarbor.a)"
	@echo "  test     - Build and run tests"
	@echo "  debug    - Build with debug symbols"
	@echo "  clean    - Remove build artifacts"
