# FuelWise Benchmark Makefile

CC ?= gcc
CFLAGS = -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE

# Include paths
INCLUDES = -I../include -I../../shared/include -I../../ralph/include

# Library paths
# OpenMP is needed for Ralph LAP solver
LDFLAGS = -L.. -L../../shared -L../../ralph -lfuelwise -lshared -lralph -lm -lpthread -L/opt/homebrew/opt/libomp/lib -lomp

# Source files
SRCS = main.c fw_bench.c fw_gen.c fw_validate.c
OBJS = $(SRCS:.c=.o)

# Target binary
TARGET = fuelwise-bench

# Validator test binary
TEST_VALIDATOR = test_validator
TEST_VALIDATOR_OBJS = test_validator.o fw_bench.o fw_gen.o fw_validate.o

.PHONY: all clean test quick validator-test

all: $(TARGET)

$(TARGET): $(OBJS) ../libfuelwise.a ../../shared/libshared.a ../../ralph/libralph.a
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@

$(TEST_VALIDATOR): $(TEST_VALIDATOR_OBJS) ../libfuelwise.a ../../shared/libshared.a ../../ralph/libralph.a
	$(CC) $(CFLAGS) $(TEST_VALIDATOR_OBJS) $(LDFLAGS) -o $@

%.o: %.c fw_bench.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Dependencies
fw_bench.o: fw_bench.c fw_bench.h
fw_gen.o: fw_gen.c fw_bench.h
fw_validate.o: fw_validate.c fw_bench.h
main.o: main.c fw_bench.h

# Ensure libraries are built
../libfuelwise.a:
	$(MAKE) -C .. lib

../../shared/libshared.a:
	$(MAKE) -C ../../shared lib

../../ralph/libralph.a:
	$(MAKE) -C ../../ralph lib

# Run benchmark with default settings
test: $(TARGET)
	./$(TARGET) --runs 10 --all

# Run quick test
quick: $(TARGET)
	./$(TARGET) --runs 5 --scenario highway

# Run validator tests
validator-test: $(TEST_VALIDATOR)
	./$(TEST_VALIDATOR)

clean:
	rm -f $(OBJS) $(TEST_VALIDATOR_OBJS) $(TARGET) $(TEST_VALIDATOR)

help:
	@echo "FuelWise Benchmark"
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build benchmark binary"
	@echo "  test    - Run benchmark (10 runs, all scenarios)"
	@echo "  quick   - Quick test (5 runs, highway)"
	@echo "  clean   - Remove build artifacts"
