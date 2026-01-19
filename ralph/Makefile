# Ralph LP/MIP Solver Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O3 -march=native -ffast-math
CFLAGS_DEBUG = -Wall -Wextra -g -O0 -DDEBUG
INCLUDES = -I./include
LDFLAGS = -lm

# Directories
SRC_DIR = src
INCLUDE_DIR = include
TEST_DIR = tests

# Source files
SOURCES = $(SRC_DIR)/sparse.c \
          $(SRC_DIR)/model.c \
          $(SRC_DIR)/lu.c \
          $(SRC_DIR)/lu_sparse.c \
          $(SRC_DIR)/simplex.c \
          $(SRC_DIR)/dual_simplex.c \
          $(SRC_DIR)/presolve.c \
          $(SRC_DIR)/branch_bound.c \
          $(SRC_DIR)/cuts.c \
          $(SRC_DIR)/mip.c \
          $(SRC_DIR)/mps_reader.c \
          $(SRC_DIR)/ralph.c

OBJECTS = $(SOURCES:.c=.o)

# Library
LIB_NAME = libralph.a

# Test executable
TEST_SOURCES = $(TEST_DIR)/test_main.c
TEST_EXEC = test_ralph

# Edge case tests
EDGE_TEST_SOURCES = $(TEST_DIR)/test_edge_cases.c
EDGE_TEST_EXEC = test_edge_cases

# Benchmarks
BENCH_DIR = benchmarks
BENCH_SIZES_EXEC = bench_sizes
COMPARE_GLPK_EXEC = compare_glpk
BENCH_VS_GLPK_EXEC = bench_vs_glpk

# GLPK configuration (via Homebrew)
GLPK_PREFIX = $(shell brew --prefix glpk 2>/dev/null || echo "/opt/homebrew/opt/glpk")
GLPK_INCLUDES = -I$(GLPK_PREFIX)/include
GLPK_LIBS = -L$(GLPK_PREFIX)/lib -lglpk

# Default target
all: $(LIB_NAME)

# Build library
$(LIB_NAME): $(OBJECTS)
	ar rcs $@ $^

# Build object files
$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build test executable
$(TEST_EXEC): $(TEST_SOURCES) $(LIB_NAME)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph $(LDFLAGS) -o $@

# Debug build
debug: CFLAGS = $(CFLAGS_DEBUG)
debug: clean all

# Run tests
test: $(TEST_EXEC)
	./$(TEST_EXEC)

# Build edge case tests
$(EDGE_TEST_EXEC): $(EDGE_TEST_SOURCES) $(LIB_NAME)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph $(LDFLAGS) -o $@

# Run edge case tests
test-edge: $(EDGE_TEST_EXEC)
	./$(EDGE_TEST_EXEC)

# Build size scaling benchmark
$(BENCH_SIZES_EXEC): $(BENCH_DIR)/bench_sizes.c $(LIB_NAME)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph $(LDFLAGS) -o $@

# Run size scaling benchmark
bench: $(BENCH_SIZES_EXEC)
	./$(BENCH_SIZES_EXEC)

# Build GLPK comparison benchmark (old version)
$(COMPARE_GLPK_EXEC): $(BENCH_DIR)/compare_glpk.c $(LIB_NAME)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph $(LDFLAGS) -o $@

# Run GLPK comparison (requires glpsol to be installed)
compare-glpk: $(COMPARE_GLPK_EXEC)
	./$(COMPARE_GLPK_EXEC)

# Build Ralph vs GLPK benchmark (direct library comparison)
$(BENCH_VS_GLPK_EXEC): $(BENCH_DIR)/bench_vs_glpk.c $(LIB_NAME)
	$(CC) $(CFLAGS) $(INCLUDES) $(GLPK_INCLUDES) $< -L. -lralph $(GLPK_LIBS) $(LDFLAGS) -o $@

# Run Ralph vs GLPK benchmark
bench-glpk: $(BENCH_VS_GLPK_EXEC)
	./$(BENCH_VS_GLPK_EXEC)

# Quick benchmark
bench-glpk-quick: $(BENCH_VS_GLPK_EXEC)
	./$(BENCH_VS_GLPK_EXEC) --quick

# Clean
clean:
	rm -f $(OBJECTS) $(LIB_NAME) $(TEST_EXEC) $(EDGE_TEST_EXEC) $(BENCH_SIZES_EXEC) $(COMPARE_GLPK_EXEC) $(BENCH_VS_GLPK_EXEC)

# Install headers and library
install: $(LIB_NAME)
	mkdir -p /usr/local/include/ralph
	mkdir -p /usr/local/lib
	cp $(INCLUDE_DIR)/ralph.h /usr/local/include/ralph/
	cp $(LIB_NAME) /usr/local/lib/

# Uninstall
uninstall:
	rm -rf /usr/local/include/ralph
	rm -f /usr/local/lib/$(LIB_NAME)

.PHONY: all clean debug test test-edge bench compare-glpk bench-glpk bench-glpk-quick install uninstall
