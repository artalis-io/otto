# Arbor Makefile
# Reusable optimization/search framework (ALNS core first)

# Every compiler difference lives in one place; see mk/toolchain.mk.
include ../mk/toolchain.mk


# Production build (with hardening)
# Note: -pie is implicit on macOS, only add on Linux
CFLAGS = $(strip $(CC_WARN) $(CC_OPT) $(CC_STD) $(CC_DEFS) $(CC_PORT_DEFS) $(CC_HARDEN))
LDFLAGS = $(strip $(LD_MATH) $(LD_PLATFORM) $(LD_PIE) $(LD_RELRO) $(LD_STACK))

INCLUDES = -Iinclude -I../shared/include

# Debug build (with sanitizers)
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -std=c11 -D_GNU_SOURCE -DDEBUG \
               -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS = -fsanitize=address,undefined

SRCS = src/arbor.c \
       src/ar_alns.c \
       src/ar_operators.c

OBJS = $(SRCS:.c=.o)

# Not LIB: that is MSVC's library search path, and make re-exports any
# variable that also exists in the environment. See mk/toolchain.mk.
LIB_FILE = libarbor.a
TEST_BIN = test_arbor
TEST_OP_BIN = test_ar_operators

.PHONY: all clean test test-asan lib debug

all: lib $(TEST_BIN) $(TEST_OP_BIN)

lib: $(LIB_FILE)

$(LIB_FILE): $(OBJS)
	$(AR_CMD)$@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< $(OBJ_OUT)$@

$(TEST_BIN): tests/test_arbor.c $(LIB_FILE) ../shared/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,arbor) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

$(TEST_OP_BIN): tests/test_ar_operators.c $(LIB_FILE) ../shared/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,arbor) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

../shared/libshared.a:
	$(MAKE) -C ../shared lib

test: $(TEST_BIN) $(TEST_OP_BIN)
	./$(TEST_BIN)
	./$(TEST_OP_BIN)

# Run the suite under AddressSanitizer + UBSan. Same flags as `debug`, but it
# runs the tests instead of only building them.
test-asan: CFLAGS = $(DEBUG_CFLAGS)
test-asan: LDFLAGS = -lm $(DEBUG_LDFLAGS)
test-asan: clean test

debug: CFLAGS = $(DEBUG_CFLAGS)
debug: LDFLAGS = -lm $(DEBUG_LDFLAGS)
debug: clean all

clean:
	rm -f $(OBJS) $(LIB_FILE) $(TEST_BIN) $(TEST_OP_BIN)

help:
	@echo "Arbor Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all      - Build library and tests (default)"
	@echo "  lib      - Build static library (libarbor.a)"
	@echo "  test     - Build and run tests"
	@echo "  debug    - Build with debug symbols"
	@echo "  clean    - Remove build artifacts"
