# Ralph LP/MIP Solver Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O3 -march=native -ffast-math

# OpenMP SIMD support (portable vectorization)
# On macOS: brew install libomp && use clang with -Xclang -fopenmp
# On Linux: gcc has native OpenMP support with -fopenmp
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    # macOS: use Homebrew's libomp with clang
    OMP_PREFIX := $(shell brew --prefix libomp 2>/dev/null || echo "/opt/homebrew/opt/libomp")
    CFLAGS += -Xclang -fopenmp -I$(OMP_PREFIX)/include
    OMP_LDFLAGS = -L$(OMP_PREFIX)/lib -lomp
else
    # Linux: use gcc's native OpenMP
    CFLAGS += -fopenmp
    OMP_LDFLAGS = -fopenmp
endif

CFLAGS_DEBUG = -Wall -Wextra -g -O0 -DDEBUG
INCLUDES = -I./include -I../shared/include
SHARED_LIB = ../shared/libshared.a
LDFLAGS = -lm $(OMP_LDFLAGS)

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
          $(SRC_DIR)/lp_reader.c \
          $(SRC_DIR)/lp_writer.c \
          $(SRC_DIR)/ralph.c \
          $(SRC_DIR)/lap.c \
          $(SRC_DIR)/detect.c \
          $(SRC_DIR)/netflow.c \
          $(SRC_DIR)/optim/subgradient.c \
          $(SRC_DIR)/optim/lagrangian.c \
          $(SRC_DIR)/optim/lagrangian_scp.c

OBJECTS = $(SOURCES:.c=.o)

# Library
LIB_NAME = libralph.a

# Test executable
TEST_SOURCES = $(TEST_DIR)/test_main.c
TEST_EXEC = test_ralph

# Edge case tests
EDGE_TEST_SOURCES = $(TEST_DIR)/test_edge_cases.c
EDGE_TEST_EXEC = test_edge_cases

# LAP tests
LAP_TEST_SOURCES = $(TEST_DIR)/test_lap.c
LAP_TEST_EXEC = test_lap

# Network flow tests
NETFLOW_TEST_SOURCES = $(TEST_DIR)/test_netflow.c
NETFLOW_TEST_EXEC = test_netflow

# Network detection tests
DETECT_TEST_SOURCES = $(TEST_DIR)/test_detect.c
DETECT_TEST_EXEC = test_detect

# Optimization module tests
OPTIM_TEST_SOURCES = $(TEST_DIR)/test_optim.c
OPTIM_TEST_EXEC = test_optim

# LP format tests
LP_TEST_SOURCES = $(TEST_DIR)/test_lp.c
LP_TEST_EXEC = test_lp

# Benchmarks
BENCH_DIR = benchmarks
BENCH_SIZES_EXEC = bench_sizes
COMPARE_GLPK_EXEC = compare_glpk
BENCH_VS_GLPK_EXEC = bench_vs_glpk
BENCH_MIP_EXEC = bench_mip
PROFILE_BENCH_EXEC = profile_bench
BENCH_LAP_EXEC = bench_lap
BENCH_LAP_PERF_EXEC = bench_lap_perf
BENCH_NETFLOW_EXEC = bench_netflow

# GLPK configuration (via Homebrew)
GLPK_PREFIX = $(shell brew --prefix glpk 2>/dev/null || echo "/opt/homebrew/opt/glpk")
GLPK_INCLUDES = -I$(GLPK_PREFIX)/include
GLPK_LIBS = -L$(GLPK_PREFIX)/lib -lglpk

# Default target
all: $(SHARED_LIB) lib $(TEST_EXEC)

# Build library only
lib: $(SHARED_LIB) $(LIB_NAME)

# Build shared library (dependency)
$(SHARED_LIB):
	$(MAKE) -C ../shared lib

$(LIB_NAME): $(OBJECTS)
	ar rcs $@ $^

# Build object files
$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build optim submodule object files
$(SRC_DIR)/optim/%.o: $(SRC_DIR)/optim/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build test executable
$(TEST_EXEC): $(TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Debug build
debug: CFLAGS = $(CFLAGS_DEBUG)
debug: clean all

# Run tests
test: $(TEST_EXEC)
	./$(TEST_EXEC)

# Build edge case tests
$(EDGE_TEST_EXEC): $(EDGE_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run edge case tests
test-edge: $(EDGE_TEST_EXEC)
	./$(EDGE_TEST_EXEC)

# Build LAP tests
$(LAP_TEST_EXEC): $(LAP_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run LAP tests
test-lap: $(LAP_TEST_EXEC)
	./$(LAP_TEST_EXEC)

# Build network flow tests
$(NETFLOW_TEST_EXEC): $(NETFLOW_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run network flow tests
test-netflow: $(NETFLOW_TEST_EXEC)
	./$(NETFLOW_TEST_EXEC)

# Build network detection tests
$(DETECT_TEST_EXEC): $(DETECT_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run network detection tests
test-detect: $(DETECT_TEST_EXEC)
	./$(DETECT_TEST_EXEC)

# Build optimization module tests
$(OPTIM_TEST_EXEC): $(OPTIM_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Build LP format tests
$(LP_TEST_EXEC): $(LP_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run LP format tests
test-lp: $(LP_TEST_EXEC)
	./$(LP_TEST_EXEC)

# Run optimization module tests
test-optim: $(OPTIM_TEST_EXEC)
	./$(OPTIM_TEST_EXEC)

# Build size scaling benchmark
$(BENCH_SIZES_EXEC): $(BENCH_DIR)/bench_sizes.c $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run size scaling benchmark
bench: $(BENCH_SIZES_EXEC)
	./$(BENCH_SIZES_EXEC)

# Build GLPK comparison benchmark (old version)
$(COMPARE_GLPK_EXEC): $(BENCH_DIR)/compare_glpk.c $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run GLPK comparison (requires glpsol to be installed)
compare-glpk: $(COMPARE_GLPK_EXEC)
	./$(COMPARE_GLPK_EXEC)

# Build Ralph vs GLPK benchmark (direct library comparison)
$(BENCH_VS_GLPK_EXEC): $(BENCH_DIR)/bench_vs_glpk.c $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $(GLPK_INCLUDES) $< -L. -lralph -L../shared -lshared $(GLPK_LIBS) $(LDFLAGS) -o $@

# Run Ralph vs GLPK benchmark
bench-glpk: $(BENCH_VS_GLPK_EXEC)
	./$(BENCH_VS_GLPK_EXEC)

# Quick benchmark
bench-glpk-quick: $(BENCH_VS_GLPK_EXEC)
	./$(BENCH_VS_GLPK_EXEC) --quick

# Build MIP benchmark
$(BENCH_MIP_EXEC): $(BENCH_DIR)/bench_mip.c $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $(GLPK_INCLUDES) $< -L. -lralph -L../shared -lshared $(GLPK_LIBS) $(LDFLAGS) -o $@

# Run MIP benchmark
bench-mip: $(BENCH_MIP_EXEC)
	./$(BENCH_MIP_EXEC)

# Quick MIP benchmark
bench-mip-quick: $(BENCH_MIP_EXEC)
	./$(BENCH_MIP_EXEC) --quick

# Build profile benchmark
$(PROFILE_BENCH_EXEC): $(BENCH_DIR)/profile_bench.c $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run profile benchmark
profile-bench: $(PROFILE_BENCH_EXEC)
	./$(PROFILE_BENCH_EXEC)

# Build LAP benchmark
$(BENCH_LAP_EXEC): $(BENCH_DIR)/bench_lap.c $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run LAP benchmark
bench-lap: $(BENCH_LAP_EXEC)
	./$(BENCH_LAP_EXEC)

# Build LAP performance benchmark
$(BENCH_LAP_PERF_EXEC): $(BENCH_DIR)/bench_lap_perf.c $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run LAP performance benchmark
bench-lap-perf: $(BENCH_LAP_PERF_EXEC)
	./$(BENCH_LAP_PERF_EXEC)

# Quick LAP performance benchmark
bench-lap-perf-quick: $(BENCH_LAP_PERF_EXEC)
	./$(BENCH_LAP_PERF_EXEC) --quick

# Build network flow benchmark
$(BENCH_NETFLOW_EXEC): $(BENCH_DIR)/bench_netflow.c $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run network flow benchmark
bench-netflow: $(BENCH_NETFLOW_EXEC)
	./$(BENCH_NETFLOW_EXEC)

# Clean
clean:
	rm -f $(OBJECTS) $(LIB_NAME) $(TEST_EXEC) $(EDGE_TEST_EXEC) $(LAP_TEST_EXEC) $(NETFLOW_TEST_EXEC) $(DETECT_TEST_EXEC) $(OPTIM_TEST_EXEC) $(LP_TEST_EXEC) $(BENCH_SIZES_EXEC) $(COMPARE_GLPK_EXEC) $(BENCH_VS_GLPK_EXEC) $(BENCH_MIP_EXEC) $(PROFILE_BENCH_EXEC) $(BENCH_LAP_EXEC) $(BENCH_LAP_PERF_EXEC) $(BENCH_NETFLOW_EXEC)

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

.PHONY: all lib clean debug test test-edge test-lap test-netflow test-detect test-optim bench bench-lap bench-lap-perf bench-lap-perf-quick bench-netflow compare-glpk bench-glpk bench-glpk-quick bench-mip bench-mip-quick bench-mip-specialized bench-mip-compare profile-bench install uninstall help

# MIP benchmark with specialized solvers (LAP, Network, SCP)
bench-mip-specialized: $(BENCH_MIP_EXEC)
	./$(BENCH_MIP_EXEC) --specialized

# MIP benchmark comparing generic vs specialized vs GLPK
bench-mip-compare: $(BENCH_MIP_EXEC)
	./$(BENCH_MIP_EXEC) --compare-all

# Quick versions of specialized benchmarks
bench-mip-specialized-quick: $(BENCH_MIP_EXEC)
	./$(BENCH_MIP_EXEC) --specialized --quick

bench-mip-compare-quick: $(BENCH_MIP_EXEC)
	./$(BENCH_MIP_EXEC) --compare-all --quick

# Help
help:
	@echo "Ralph LP/MIP Solver Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all        - Build library and tests (default)"
	@echo "  lib        - Build static library (libralph.a)"
	@echo "  test       - Build and run tests"
	@echo "  test-edge  - Run edge case tests"
	@echo "  test-lap   - Run LAP (assignment problem) tests"
	@echo "  test-netflow - Run network flow tests"
	@echo "  test-detect - Run network detection tests"
	@echo "  test-optim  - Run optimization module tests"
	@echo "  bench      - Run size scaling benchmark"
	@echo "  bench-lap  - Run LAP benchmark (JVC vs LP)"
	@echo "  bench-netflow - Run network flow benchmark"
	@echo "  debug      - Build with debug symbols"
	@echo "  clean      - Remove build artifacts"
	@echo ""
	@echo "GLPK Benchmarks (requires GLPK):"
	@echo "  bench-glpk - Compare Ralph vs GLPK (LP only)"
	@echo "  bench-mip  - MIP benchmark: generic Ralph vs GLPK"
	@echo "  bench-mip-specialized - MIP benchmark: Ralph with LAP/SCP/Network detection vs GLPK"
	@echo "  bench-mip-compare     - MIP benchmark: generic vs specialized vs GLPK (side-by-side)"
	@echo ""
	@echo "Quick benchmark variants (smaller problem sizes):"
	@echo "  bench-mip-quick, bench-mip-specialized-quick, bench-mip-compare-quick"
