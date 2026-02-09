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

.PHONY: all clean test quick

all: $(TARGET)

$(TARGET): $(OBJS) ../libfuelwise.a ../../shared/libshared.a ../../ralph/libralph.a
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@

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

clean:
	rm -f $(OBJS) $(TARGET)

help:
	@echo "FuelWise Benchmark"
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build benchmark binary"
	@echo "  test    - Run benchmark (10 runs, all scenarios)"
	@echo "  quick   - Quick test (5 runs, highway)"
	@echo "  clean   - Remove build artifacts"
