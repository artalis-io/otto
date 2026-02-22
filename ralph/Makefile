# Ralph LP/MIP Solver Makefile

CC = gcc

# Production build (with hardening)
CFLAGS = -Wall -Wextra -O3 -march=native -ffast-math -fno-finite-math-only \
         -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE -fno-common

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
    # Linux: use gcc's native OpenMP with RELRO
    CFLAGS += -fopenmp
    OMP_LDFLAGS = -fopenmp -Wl,-z,relro,-z,now
endif

# Debug build (with sanitizers)
CFLAGS_DEBUG = -Wall -Wextra -g -O0 -DDEBUG \
               -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS = -fsanitize=address,undefined
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
          $(SRC_DIR)/lu_supernode.c \
          $(SRC_DIR)/lp_log.c \
          $(SRC_DIR)/simplex.c \
          $(SRC_DIR)/lp_telemetry_core.c \
          $(SRC_DIR)/lp_telemetry_solver.c \
          $(SRC_DIR)/lp_telemetry_simplex.c \
          $(SRC_DIR)/lp_telemetry_lu.c \
          $(SRC_DIR)/lp_telemetry_lu_sparse.c \
          $(SRC_DIR)/dual_simplex.c \
          $(SRC_DIR)/presolve.c \
          $(SRC_DIR)/branch_bound.c \
          $(SRC_DIR)/mip_lp_adapter.c \
          $(SRC_DIR)/cuts.c \
          $(SRC_DIR)/mip.c \
          $(SRC_DIR)/mps_reader.c \
          $(SRC_DIR)/lp_reader.c \
          $(SRC_DIR)/lp_writer.c \
          $(SRC_DIR)/ralph.c \
          $(SRC_DIR)/lap.c \
          $(SRC_DIR)/detect.c \
          $(SRC_DIR)/netflow.c \
          $(SRC_DIR)/benders.c \
          $(SRC_DIR)/ralph_api.c \
          $(SRC_DIR)/ralph_parse_lp.c \
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

# NETLIB MPS parser regression tests
MPS_PARSER_TEST_SOURCES = $(TEST_DIR)/test_mps_parser_netlib.c
MPS_PARSER_TEST_EXEC = test_mps_parser_netlib

# API tests
API_TEST_SOURCES = $(TEST_DIR)/test_api.c
API_TEST_EXEC = test_api

# Benders tests
BENDERS_TEST_SOURCES = $(TEST_DIR)/test_benders.c
BENDERS_TEST_EXEC = test_benders
BENDERS_WARMSTART_TEST_SOURCES = $(TEST_DIR)/test_benders_warmstart.c
BENDERS_WARMSTART_TEST_EXEC = test_benders_warmstart
MIP_WARMSTART_TEST_SOURCES = $(TEST_DIR)/test_mip_warmstart.c
MIP_WARMSTART_TEST_EXEC = test_mip_warmstart
MIP_LP_RECOVERY_TEST_SOURCES = $(TEST_DIR)/test_mip_lp_recovery.c
MIP_LP_RECOVERY_TEST_EXEC = test_mip_lp_recovery
MIP_START_SPARSE_TEST_SOURCES = $(TEST_DIR)/test_mip_start_sparse.c
MIP_START_SPARSE_TEST_EXEC = test_mip_start_sparse
MIP_START_REPAIR_TEST_SOURCES = $(TEST_DIR)/test_mip_start_repair.c
MIP_START_REPAIR_TEST_EXEC = test_mip_start_repair
BENDERS_MASTER_START_TEST_SOURCES = $(TEST_DIR)/test_benders_master_start.c
BENDERS_MASTER_START_TEST_EXEC = test_benders_master_start
WARMSTART_SERDE_TEST_SOURCES = $(TEST_DIR)/test_warmstart_serde.c
WARMSTART_SERDE_TEST_EXEC = test_warmstart_serde

# Presolve unit tests
PRESOLVE_TEST_SOURCES = $(TEST_DIR)/test_presolve.c
PRESOLVE_TEST_EXEC = test_presolve

# Beaconfd numerical-regression test
BEACONFD_TEST_SOURCES = $(TEST_DIR)/test_presolve_beaconfd.c
BEACONFD_TEST_EXEC = test_presolve_beaconfd

# NETLIB regression tests (bandm, blend, lotfi)
BANDM_TEST_SOURCES = $(TEST_DIR)/test_presolve_bandm.c
BANDM_TEST_EXEC = test_presolve_bandm
BLEND_TEST_SOURCES = $(TEST_DIR)/test_presolve_blend.c
BLEND_TEST_EXEC = test_presolve_blend
LOTFI_TEST_SOURCES = $(TEST_DIR)/test_presolve_lotfi.c
LOTFI_TEST_EXEC = test_presolve_lotfi

# LU Markowitz tests
LU_MARKOWITZ_TEST_SOURCES = $(TEST_DIR)/test_lu_markowitz.c
LU_MARKOWITZ_TEST_EXEC = test_lu_markowitz

# LU supernode tests
LU_SUPERNODE_TEST_SOURCES = $(TEST_DIR)/test_lu_supernode.c
LU_SUPERNODE_TEST_EXEC = test_lu_supernode

# Beaconfd Phase-1 failure-trace regression test
BEACONFD_TRACE_TEST_SOURCES = $(TEST_DIR)/test_beaconfd_phase1_trace.c
BEACONFD_TRACE_TEST_EXEC = test_beaconfd_phase1_trace

# Simplex basis-action policy test
SIMPLEX_POLICY_TEST_SOURCES = $(TEST_DIR)/test_simplex_policy.c
SIMPLEX_POLICY_TEST_EXEC = test_simplex_policy

# LP telemetry module tests (split by telemetry domain)
LP_TELEMETRY_SOLVER_TEST_SOURCES = $(TEST_DIR)/test_lp_telemetry_solver.c
LP_TELEMETRY_SOLVER_TEST_EXEC = test_lp_telemetry_solver
LP_TELEMETRY_LU_TEST_SOURCES = $(TEST_DIR)/test_lp_telemetry_lu.c
LP_TELEMETRY_LU_TEST_EXEC = test_lp_telemetry_lu
LP_TELEMETRY_GATE_TEST_SOURCES = $(TEST_DIR)/test_lp_telemetry_runtime_gate.c
LP_TELEMETRY_GATE_TEST_EXEC = test_lp_telemetry_runtime_gate
LP_TELEMETRY_LU_SPARSE_TEST_SOURCES = $(TEST_DIR)/test_lp_telemetry_lu_sparse.c
LP_TELEMETRY_LU_SPARSE_TEST_EXEC = test_lp_telemetry_lu_sparse
LP_TELEMETRY_TEST_EXECS = $(LP_TELEMETRY_SOLVER_TEST_EXEC) \
                          $(LP_TELEMETRY_LU_TEST_EXEC) \
                          $(LP_TELEMETRY_GATE_TEST_EXEC) \
                          $(LP_TELEMETRY_LU_SPARSE_TEST_EXEC)

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
RALPH_BENCHMARK_EXEC = ralph-benchmark

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
	rm -f $@
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

# Debug build (with sanitizers)
debug: CFLAGS = $(CFLAGS_DEBUG)
debug: LDFLAGS += $(DEBUG_LDFLAGS)
debug: clean all

# Run tests
test: $(TEST_EXEC) $(PRESOLVE_TEST_EXEC) $(BANDM_TEST_EXEC) $(BLEND_TEST_EXEC) $(LOTFI_TEST_EXEC) $(SIMPLEX_POLICY_TEST_EXEC) $(LP_TELEMETRY_TEST_EXECS) $(MPS_PARSER_TEST_EXEC) $(API_TEST_EXEC) $(BENDERS_WARMSTART_TEST_EXEC) $(MIP_WARMSTART_TEST_EXEC) $(MIP_LP_RECOVERY_TEST_EXEC) $(MIP_START_SPARSE_TEST_EXEC) $(MIP_START_REPAIR_TEST_EXEC) $(BENDERS_MASTER_START_TEST_EXEC) $(WARMSTART_SERDE_TEST_EXEC)
	./$(TEST_EXEC)
	./$(PRESOLVE_TEST_EXEC)
	./$(BANDM_TEST_EXEC)
	./$(BLEND_TEST_EXEC)
	./$(LOTFI_TEST_EXEC)
	./$(SIMPLEX_POLICY_TEST_EXEC)
	./$(LP_TELEMETRY_SOLVER_TEST_EXEC)
	./$(LP_TELEMETRY_LU_TEST_EXEC)
	./$(LP_TELEMETRY_GATE_TEST_EXEC)
	./$(LP_TELEMETRY_LU_SPARSE_TEST_EXEC)
	./$(MPS_PARSER_TEST_EXEC)
	./$(API_TEST_EXEC)
	./$(BENDERS_WARMSTART_TEST_EXEC)
	./$(MIP_WARMSTART_TEST_EXEC)
	./$(MIP_LP_RECOVERY_TEST_EXEC)
	./$(MIP_START_SPARSE_TEST_EXEC)
	./$(MIP_START_REPAIR_TEST_EXEC)
	./$(BENDERS_MASTER_START_TEST_EXEC)
	./$(WARMSTART_SERDE_TEST_EXEC)

# Known regression tests (beaconfd — Phase 2 pivot failure, degenerate theta=0)
# Run separately; not in the main test gate until ratio test pivot threshold is fixed.
test-beaconfd: $(BEACONFD_TEST_EXEC) $(BEACONFD_TRACE_TEST_EXEC)
	./$(BEACONFD_TEST_EXEC)
	./$(BEACONFD_TRACE_TEST_EXEC)

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

# Build NETLIB MPS parser regression tests
$(MPS_PARSER_TEST_EXEC): $(MPS_PARSER_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run NETLIB MPS parser regression tests
test-mps-parser: $(MPS_PARSER_TEST_EXEC)
	./$(MPS_PARSER_TEST_EXEC)

# Build API tests
$(API_TEST_EXEC): $(API_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run API tests
test-api: $(API_TEST_EXEC)
	./$(API_TEST_EXEC)

# Build Benders tests
$(BENDERS_TEST_EXEC): $(BENDERS_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run Benders tests
test-benders: $(BENDERS_TEST_EXEC)
	./$(BENDERS_TEST_EXEC)

# Build focused Benders warm-start tests
$(BENDERS_WARMSTART_TEST_EXEC): $(BENDERS_WARMSTART_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run focused Benders warm-start tests
test-benders-warmstart: $(BENDERS_WARMSTART_TEST_EXEC)
	./$(BENDERS_WARMSTART_TEST_EXEC)

# Build focused MIP warm-start tests
$(MIP_WARMSTART_TEST_EXEC): $(MIP_WARMSTART_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run focused MIP warm-start tests
test-mip-warmstart: $(MIP_WARMSTART_TEST_EXEC)
	./$(MIP_WARMSTART_TEST_EXEC)

# Build focused MIP/LP recovery-contract tests
$(MIP_LP_RECOVERY_TEST_EXEC): $(MIP_LP_RECOVERY_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run focused MIP/LP recovery-contract tests
test-mip-lp-recovery: $(MIP_LP_RECOVERY_TEST_EXEC)
	./$(MIP_LP_RECOVERY_TEST_EXEC)

# Build sparse/partial MIP-start tests
$(MIP_START_SPARSE_TEST_EXEC): $(MIP_START_SPARSE_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run sparse/partial MIP-start tests
test-mip-start-sparse: $(MIP_START_SPARSE_TEST_EXEC)
	./$(MIP_START_SPARSE_TEST_EXEC)

# Build MIP-start repair-mode tests
$(MIP_START_REPAIR_TEST_EXEC): $(MIP_START_REPAIR_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run MIP-start repair-mode tests
test-mip-start-repair: $(MIP_START_REPAIR_TEST_EXEC)
	./$(MIP_START_REPAIR_TEST_EXEC)

# Build Benders initial master-start tests
$(BENDERS_MASTER_START_TEST_EXEC): $(BENDERS_MASTER_START_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run Benders initial master-start tests
test-benders-master-start: $(BENDERS_MASTER_START_TEST_EXEC)
	./$(BENDERS_MASTER_START_TEST_EXEC)

# Build warm-start serialization tests
$(WARMSTART_SERDE_TEST_EXEC): $(WARMSTART_SERDE_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run warm-start serialization tests
test-warmstart-serde: $(WARMSTART_SERDE_TEST_EXEC)
	./$(WARMSTART_SERDE_TEST_EXEC)

# Build presolve unit tests
$(PRESOLVE_TEST_EXEC): $(PRESOLVE_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run presolve unit tests
test-presolve: $(PRESOLVE_TEST_EXEC)
	./$(PRESOLVE_TEST_EXEC)

# Build beaconfd numerical-regression test
$(BEACONFD_TEST_EXEC): $(BEACONFD_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# test-beaconfd target is defined above (known regression, not in main gate)

# Build NETLIB regression tests (bandm, blend, lotfi)
$(BANDM_TEST_EXEC): $(BANDM_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

$(BLEND_TEST_EXEC): $(BLEND_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

$(LOTFI_TEST_EXEC): $(LOTFI_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run NETLIB regression tests individually
test-bandm: $(BANDM_TEST_EXEC)
	./$(BANDM_TEST_EXEC)

test-blend: $(BLEND_TEST_EXEC)
	./$(BLEND_TEST_EXEC)

test-lotfi: $(LOTFI_TEST_EXEC)
	./$(LOTFI_TEST_EXEC)

# Build beaconfd phase-1 trace regression test
$(BEACONFD_TRACE_TEST_EXEC): $(BEACONFD_TRACE_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run beaconfd phase-1 trace regression test
test-beaconfd-trace: $(BEACONFD_TRACE_TEST_EXEC)
	./$(BEACONFD_TRACE_TEST_EXEC)

# Build simplex basis-action policy test
$(SIMPLEX_POLICY_TEST_EXEC): $(SIMPLEX_POLICY_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run simplex basis-action policy test
test-simplex-policy: $(SIMPLEX_POLICY_TEST_EXEC)
	./$(SIMPLEX_POLICY_TEST_EXEC)

# Build LP telemetry solver tests
$(LP_TELEMETRY_SOLVER_TEST_EXEC): $(LP_TELEMETRY_SOLVER_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Build LP telemetry LU tests
$(LP_TELEMETRY_LU_TEST_EXEC): $(LP_TELEMETRY_LU_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Build LP telemetry runtime gate tests
$(LP_TELEMETRY_GATE_TEST_EXEC): $(LP_TELEMETRY_GATE_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Build LP telemetry sparse-LU tests
$(LP_TELEMETRY_LU_SPARSE_TEST_EXEC): $(LP_TELEMETRY_LU_SPARSE_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run LP telemetry solver tests
test-lp-telemetry-solver: $(LP_TELEMETRY_SOLVER_TEST_EXEC)
	./$(LP_TELEMETRY_SOLVER_TEST_EXEC)

# Run LP telemetry LU tests
test-lp-telemetry-lu: $(LP_TELEMETRY_LU_TEST_EXEC)
	./$(LP_TELEMETRY_LU_TEST_EXEC)

# Run LP telemetry runtime gate tests
test-lp-telemetry-gate: $(LP_TELEMETRY_GATE_TEST_EXEC)
	./$(LP_TELEMETRY_GATE_TEST_EXEC)

# Run LP telemetry sparse-LU tests
test-lp-telemetry-lu-sparse: $(LP_TELEMETRY_LU_SPARSE_TEST_EXEC)
	./$(LP_TELEMETRY_LU_SPARSE_TEST_EXEC)

# Run all LP telemetry tests
test-lp-telemetry: $(LP_TELEMETRY_TEST_EXECS)
	./$(LP_TELEMETRY_SOLVER_TEST_EXEC)
	./$(LP_TELEMETRY_LU_TEST_EXEC)
	./$(LP_TELEMETRY_GATE_TEST_EXEC)
	./$(LP_TELEMETRY_LU_SPARSE_TEST_EXEC)

# Build LU Markowitz tests
$(LU_MARKOWITZ_TEST_EXEC): $(LU_MARKOWITZ_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run LU Markowitz tests
test-lu-markowitz: $(LU_MARKOWITZ_TEST_EXEC)
	./$(LU_MARKOWITZ_TEST_EXEC)

# Build LU supernode tests
$(LU_SUPERNODE_TEST_EXEC): $(LU_SUPERNODE_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Run LU supernode tests
test-lu-supernode: $(LU_SUPERNODE_TEST_EXEC)
	./$(LU_SUPERNODE_TEST_EXEC)

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

# Build ralph-benchmark (NETLIB benchmark tool)
$(RALPH_BENCHMARK_EXEC): $(BENCH_DIR)/ralph_benchmark.c $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph -L../shared -lshared $(LDFLAGS) -o $@

# Build target alias for ralph-benchmark
build-ralph-benchmark: $(RALPH_BENCHMARK_EXEC)
	@echo "Built: ./ralph-benchmark"
	@echo "Run: ./ralph-benchmark --help"

# Fast NETLIB correctness test (tiers 0-1)
test-netlib: $(RALPH_BENCHMARK_EXEC)
	cd $(CURDIR) && ./$(RALPH_BENCHMARK_EXEC) --test fast

# Full NETLIB correctness test (all tiers)
test-netlib-full: $(RALPH_BENCHMARK_EXEC)
	cd $(CURDIR) && ./$(RALPH_BENCHMARK_EXEC) --test full

# Deterministic NETLIB no-regression gate (baseline-locked)
test-netlib-gate: $(RALPH_BENCHMARK_EXEC)
	cd $(CURDIR) && bash benchmarks/netlib_regression_gate.sh --bench "$(CURDIR)/$(RALPH_BENCHMARK_EXEC)"

# Deterministic NETLIB small-canary gate (tiers 0-1 + beaconfd)
test-netlib-gate-small: $(RALPH_BENCHMARK_EXEC)
	cd $(CURDIR) && bash benchmarks/netlib_regression_gate.sh --bench "$(CURDIR)/$(RALPH_BENCHMARK_EXEC)" --allowlist "$(CURDIR)/benchmarks/netlib_small_canary.txt"

# Download NETLIB problems
download-netlib:
	cd $(BENCH_DIR) && bash download_netlib.sh

# Clean
clean:
	rm -f $(OBJECTS) $(SRC_DIR)/lp_telemetry.o $(LIB_NAME) $(TEST_EXEC) $(PRESOLVE_TEST_EXEC) $(EDGE_TEST_EXEC) $(LAP_TEST_EXEC) $(NETFLOW_TEST_EXEC) $(DETECT_TEST_EXEC) $(OPTIM_TEST_EXEC) $(LP_TEST_EXEC) $(MPS_PARSER_TEST_EXEC) $(API_TEST_EXEC) $(BENDERS_TEST_EXEC) $(BENDERS_WARMSTART_TEST_EXEC) $(MIP_WARMSTART_TEST_EXEC) $(MIP_LP_RECOVERY_TEST_EXEC) $(MIP_START_SPARSE_TEST_EXEC) $(MIP_START_REPAIR_TEST_EXEC) $(BENDERS_MASTER_START_TEST_EXEC) $(WARMSTART_SERDE_TEST_EXEC) $(BEACONFD_TEST_EXEC) $(BEACONFD_TRACE_TEST_EXEC) $(SIMPLEX_POLICY_TEST_EXEC) $(LP_TELEMETRY_TEST_EXECS) $(LU_MARKOWITZ_TEST_EXEC) $(LU_SUPERNODE_TEST_EXEC) $(BENCH_SIZES_EXEC) $(COMPARE_GLPK_EXEC) $(BENCH_VS_GLPK_EXEC) $(BENCH_MIP_EXEC) $(PROFILE_BENCH_EXEC) $(BENCH_LAP_EXEC) $(BENCH_LAP_PERF_EXEC) $(BENCH_NETFLOW_EXEC) $(RALPH_BENCHMARK_EXEC)

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

.PHONY: all lib clean debug test test-edge test-lap test-netflow test-detect test-optim test-api test-benders test-benders-warmstart test-mip-warmstart test-mip-start-sparse test-mip-start-repair test-benders-master-start test-warmstart-serde test-beaconfd test-beaconfd-trace test-simplex-policy test-lp-telemetry test-lp-telemetry-solver test-lp-telemetry-lu test-lp-telemetry-gate test-lp-telemetry-lu-sparse test-lu-markowitz test-lu-supernode test-netlib test-netlib-full test-netlib-gate test-netlib-gate-small bench bench-lap bench-lap-perf bench-lap-perf-quick bench-netflow compare-glpk bench-glpk bench-glpk-quick bench-mip bench-mip-quick bench-mip-specialized bench-mip-compare profile-bench build-ralph-benchmark download-netlib install uninstall help

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
	@echo "  test-beaconfd - Run beaconfd numerical-regression test"
	@echo "  test-simplex-policy - Run simplex basis-action policy tests"
	@echo "  test-lp-telemetry - Run LP telemetry module tests"
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
	@echo ""
	@echo "NETLIB Correctness Tests:"
	@echo "  test-netlib            - Fast test (tiers 0-1, ~30s)"
	@echo "  test-netlib-full       - Full test (all tiers, ~10min)"
	@echo "  test-netlib-gate       - Baseline-locked no-regression NETLIB gate"
	@echo "  test-netlib-gate-small - NETLIB small-canary gate (26 cases incl. beaconfd)"
	@echo "  download-netlib        - Download all NETLIB problems"
	@echo ""
	@echo "NETLIB Benchmark Tool:"
	@echo "  build-ralph-benchmark  - Build the benchmark tool"
	@echo ""
	@echo "  ./ralph-benchmark --test              # Correctness test (fast)"
	@echo "  ./ralph-benchmark --test full          # Correctness test (all)"
	@echo "  ./ralph-benchmark --suite tiny -v     # GLPK comparison"
	@echo "  ./ralph-benchmark --netlib afiro -v   # Single problem"
