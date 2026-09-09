# Ralph LP/MIP Solver Makefile

# Ralph has always defaulted to gcc rather than make's built-in cc. Kept as a
# plain default: a command-line CC=cl still wins, which is how MSVC gets in.
CC = gcc

# Every compiler difference lives in one place; see mk/toolchain.mk.
include ../mk/toolchain.mk

# Production build (with hardening).
#
# No standard is pinned on GCC deliberately: Ralph builds in the compiler's
# default GNU mode. CC_STD_BASELINE is empty there and names C11 explicitly
# only where the default would be older (MSVC).
CFLAGS = $(strip $(CC_WARN) $(CC_OPT) $(CC_ARCH) \
                 $(CC_FP_MODE) $(CC_FP_FASTMATH) $(CC_FP_KEEP_NONFINITE) \
                 $(CC_HARDEN) $(CC_OMP) $(CC_STD_BASELINE) $(CC_PORT_DEFS))

# Debug build (with sanitizers)
CFLAGS_DEBUG = $(strip $(CC_WARN) $(CC_DEBUG_OPT) -DDEBUG $(CC_SANITIZE) $(CC_STD_BASELINE) $(CC_PORT_DEFS))
DEBUG_LDFLAGS = $(LD_SANITIZE)
INCLUDES = -I./include -I../shared/include
SHARED_LIB = ../shared/libshared.a
LDFLAGS = $(strip $(LD_MATH) $(LD_OMP) $(LD_PLATFORM) $(LD_RELRO) $(LD_STACK))

# Directories
SRC_DIR = src
INCLUDE_DIR = include
TEST_DIR = tests

# Source files
SOURCES = $(SRC_DIR)/sparse.c \
          $(SRC_DIR)/model.c \
          $(SRC_DIR)/lu.c \
          $(SRC_DIR)/lu_update_backend.c \
          $(SRC_DIR)/lu_sparse.c \
          $(SRC_DIR)/lu_supernode.c \
          $(SRC_DIR)/lp_log.c \
          $(SRC_DIR)/lp_determinism.c \
          $(SRC_DIR)/lp_refactor_policy.c \
          $(SRC_DIR)/lp_reinvert_controller.c \
          $(SRC_DIR)/lp_basis_governor.c \
          $(SRC_DIR)/lp_policy_glpk_compat.c \
          $(SRC_DIR)/lp_glpk_strict.c \
          $(SRC_DIR)/lp_glpk_strict_bfcp.c \
          $(SRC_DIR)/lp_bfcp_policy.c \
          $(SRC_DIR)/lp_backend.c \
          $(SRC_DIR)/lp_external_adapter.c \
          $(SRC_DIR)/lp_external_oop.c \
          $(SRC_DIR)/lp_external_glpk_oop.c \
          $(SRC_DIR)/lp_dispatch.c \
          $(SRC_DIR)/lp_conflict.c \
          $(SRC_DIR)/lp_sensitivity.c \
          $(SRC_DIR)/lp_error.c \
          $(SRC_DIR)/simplex.c \
          $(SRC_DIR)/simplex_pricing.c \
          $(SRC_DIR)/simplex_ratio.c \
          $(SRC_DIR)/simplex_perturb.c \
          $(SRC_DIR)/simplex_phase1_decision.c \
          $(SRC_DIR)/simplex_phase1_recovery.c \
          $(SRC_DIR)/simplex_phase1_stabilize.c \
          $(SRC_DIR)/simplex_phase1_trace.c \
          $(SRC_DIR)/simplex_phase1_engine.c \
          $(SRC_DIR)/simplex_phase1_zones.c \
          $(SRC_DIR)/simplex_phase2_zones.c \
          $(SRC_DIR)/simplex_crash.c \
          $(SRC_DIR)/simplex_scaling.c \
          $(SRC_DIR)/simplex_refactor_schedule.c \
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
          $(SRC_DIR)/ralph_test_mod_api.c \
          $(SRC_DIR)/ralph_lp_public.c \
          $(SRC_DIR)/ralph_mip_public.c \
          $(SRC_DIR)/lap.c \
          $(SRC_DIR)/detect.c \
          $(SRC_DIR)/netflow.c \
          $(SRC_DIR)/benders.c \
          $(SRC_DIR)/ralph_api.c \
          $(SRC_DIR)/ralph_parse_lp.c \
          $(SRC_DIR)/optim/subgradient.c \
          $(SRC_DIR)/optim/lagrangian.c \
          $(SRC_DIR)/optim/lagrangian_scp.c \
          $(SRC_DIR)/optim/spp_context.c \
          $(SRC_DIR)/optim/spp_branch.c \
          $(SRC_DIR)/optim/spp_cuts.c \
          $(SRC_DIR)/optim/spp_heuristic.c

OBJECTS = $(SOURCES:.c=.o)
DEPS = $(OBJECTS:.o=.d)
DEPFLAGS = $(CC_DEPFLAGS)

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

# Transport abstraction leak detector -- builds with no transport library on
# the include path. See docs/roadmaps/transport.md.
TRANSPORT_TEST_SOURCES = $(TEST_DIR)/test_transport_direct.c
TRANSPORT_TEST_EXEC = test_transport_direct

# Benders tests
BENDERS_TEST_SOURCES = $(TEST_DIR)/test_benders.c
BENDERS_TEST_EXEC = test_benders
BENDERS_DEBUG_TEST_SOURCES = $(TEST_DIR)/test_benders_debug.c
BENDERS_DEBUG_TEST_EXEC = test_benders_debug
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
MIP_BUG_TEST_SOURCES = $(TEST_DIR)/test_mip_bug.c
MIP_BUG_TEST_EXEC = test_mip_bug
SPP_CONTEXT_TEST_SOURCES = $(TEST_DIR)/test_spp_context.c
SPP_CONTEXT_TEST_EXEC = test_spp_context
SPP_CUTS_TEST_SOURCES = $(TEST_DIR)/test_spp_cuts.c
SPP_CUTS_TEST_EXEC = test_spp_cuts

LIFTED_COVER_TEST_SOURCES = $(TEST_DIR)/test_lifted_cover_validity.c
LIFTED_COVER_TEST_EXEC = test_lifted_cover_validity

# Regression test guarding the artalis-io/otto#86 fix: presolve on/off must
# agree (the base simplex used to report INFEASIBLE for feasible LPs with a
# nonzero lower bound). Now passes and is part of `make test`.
PRESOLVE_DIFF_TEST_SOURCES = $(TEST_DIR)/test_presolve_differential.c
PRESOLVE_DIFF_TEST_EXEC = test_presolve_differential
# Regression test guarding the artalis-io/otto#86 "1e30-infinity-sentinel"
# sibling: the ratio test used to treat a variable's +inf sentinel bound as a
# finite blocking constraint, capping an unbounded ray and reporting a spurious
# OPTIMAL. Genuinely unbounded LPs must report UNBOUNDED on both paths.
UNBOUNDED_INF_TEST_SOURCES = $(TEST_DIR)/test_unbounded_infinite_bound.c
UNBOUNDED_INF_TEST_EXEC = test_unbounded_infinite_bound
# Regression test guarding artalis-io/otto#96: the base simplex used to report
# UNBOUNDED for infeasible LPs whose infeasibility comes from an equality row
# forcing a variable outside its bounds (equality artificial started negative).
INFEAS_EQ_TEST_SOURCES = $(TEST_DIR)/test_infeasible_equality_bounds.c
INFEAS_EQ_TEST_EXEC = test_infeasible_equality_bounds
SPP_HEURISTIC_TEST_SOURCES = $(TEST_DIR)/test_spp_heuristic.c
SPP_HEURISTIC_TEST_EXEC = test_spp_heuristic
SPP_BRANCH_TEST_SOURCES = $(TEST_DIR)/test_spp_branch.c
SPP_BRANCH_TEST_EXEC = test_spp_branch
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

# Phase 1 recovery progress ops tests
PHASE1_RECOVERY_TEST_SOURCES = $(TEST_DIR)/test_simplex_phase1_recovery.c
PHASE1_RECOVERY_TEST_EXEC = test_simplex_phase1_recovery

# Phase 1 zone handler tests
PHASE1_ZONES_TEST_SOURCES = $(TEST_DIR)/test_simplex_phase1_zones.c
PHASE1_ZONES_TEST_EXEC = test_simplex_phase1_zones

# Phase 1 feasibility-engine tests
PHASE1_ENGINE_TEST_SOURCES = $(TEST_DIR)/test_simplex_phase1_engine.c
PHASE1_ENGINE_TEST_EXEC = test_simplex_phase1_engine

# Simplex basis-action policy test
SIMPLEX_POLICY_TEST_SOURCES = $(TEST_DIR)/test_simplex_policy.c
SIMPLEX_POLICY_TEST_EXEC = test_simplex_policy

# Dual ratio flip-mode tests
DUAL_RATIO_FLIP_TEST_SOURCES = $(TEST_DIR)/test_dual_ratio_flip.c
DUAL_RATIO_FLIP_TEST_EXEC = test_dual_ratio_flip

# LP refactor-policy module tests (orthogonal pure-policy unit tests)
LP_REFACTOR_POLICY_TEST_SOURCES = $(TEST_DIR)/test_lp_refactor_policy.c
LP_REFACTOR_POLICY_TEST_EXEC = test_lp_refactor_policy
LP_REINVERT_CONTROLLER_TEST_SOURCES = $(TEST_DIR)/test_lp_reinvert_controller.c
LP_REINVERT_CONTROLLER_TEST_EXEC = test_lp_reinvert_controller

# LP basis-governor module tests (shadow policy module)
LP_BASIS_GOVERNOR_TEST_SOURCES = $(TEST_DIR)/test_lp_basis_governor.c
LP_BASIS_GOVERNOR_TEST_EXEC = test_lp_basis_governor
LP_BASIS_GOVERNOR_RUNTIME_TEST_SOURCES = $(TEST_DIR)/test_lp_basis_governor_runtime.c
LP_BASIS_GOVERNOR_RUNTIME_TEST_EXEC = test_lp_basis_governor_runtime

# LP determinism tests (orthogonal to telemetry/logging)
LP_DETERMINISM_TEST_SOURCES = $(TEST_DIR)/test_lp_determinism.c
LP_DETERMINISM_TEST_EXEC = test_lp_determinism

# LP sensitivity/ranging tests (orthogonal to telemetry/logging)
LP_SENSITIVITY_TEST_SOURCES = $(TEST_DIR)/test_lp_sensitivity.c
LP_SENSITIVITY_TEST_EXEC = test_lp_sensitivity

# LP conflict/IIS tests (orthogonal to telemetry/logging)
LP_CONFLICT_TEST_SOURCES = $(TEST_DIR)/test_lp_conflict.c
LP_CONFLICT_TEST_EXEC = test_lp_conflict

# LP algorithm/capability API tests (orthogonal to telemetry/logging)
LP_ALGORITHM_API_TEST_SOURCES = $(TEST_DIR)/test_lp_algorithm_api.c
LP_ALGORITHM_API_TEST_EXEC = test_lp_algorithm_api
LP_POLICY_GLPK_COMPAT_TEST_SOURCES = $(TEST_DIR)/test_lp_policy_glpk_compat.c
LP_POLICY_GLPK_COMPAT_TEST_EXEC = test_lp_policy_glpk_compat
LP_GLPK_STRICT_TEST_SOURCES = $(TEST_DIR)/test_lp_glpk_strict.c
LP_GLPK_STRICT_TEST_EXEC = test_lp_glpk_strict
LP_GLPK_STRICT_BFCP_TEST_SOURCES = $(TEST_DIR)/test_lp_glpk_strict_bfcp.c
LP_GLPK_STRICT_BFCP_TEST_EXEC = test_lp_glpk_strict_bfcp
LP_BFCP_POLICY_TEST_SOURCES = $(TEST_DIR)/test_lp_bfcp_policy.c
LP_BFCP_POLICY_TEST_EXEC = test_lp_bfcp_policy

# Internal LP dispatch module tests (orthogonal to public API tests)
LP_DISPATCH_TEST_SOURCES = $(TEST_DIR)/test_lp_dispatch.c
LP_DISPATCH_TEST_EXEC = test_lp_dispatch

# Internal LP external adapter module tests (orthogonal to dispatch/api tests)
LP_EXTERNAL_ADAPTER_TEST_SOURCES = $(TEST_DIR)/test_lp_external_adapter.c
LP_EXTERNAL_ADAPTER_TEST_EXEC = test_lp_external_adapter
LP_EXTERNAL_ADAPTER_API_TEST_SOURCES = $(TEST_DIR)/test_lp_external_adapter_api.c
LP_EXTERNAL_ADAPTER_API_TEST_EXEC = test_lp_external_adapter_api
LP_EXTERNAL_INTROSPECTION_API_TEST_SOURCES = $(TEST_DIR)/test_lp_external_introspection_api.c
LP_EXTERNAL_INTROSPECTION_API_TEST_EXEC = test_lp_external_introspection_api
LP_EXTERNAL_FAILURE_API_TEST_SOURCES = $(TEST_DIR)/test_lp_external_failure_api.c
LP_EXTERNAL_FAILURE_API_TEST_EXEC = test_lp_external_failure_api
LP_EXTERNAL_OOP_RUNNER_TEST_SOURCES = $(TEST_DIR)/test_lp_external_oop_runner.c
LP_EXTERNAL_OOP_RUNNER_TEST_EXEC = test_lp_external_oop_runner
LP_EXTERNAL_GLPK_OOP_TEST_SOURCES = $(TEST_DIR)/test_lp_external_glpk_oop.c
LP_EXTERNAL_GLPK_OOP_TEST_EXEC = test_lp_external_glpk_oop
LP_EXTERNAL_GLPK_OOP_INTEGRATION_TEST_SOURCES = $(TEST_DIR)/test_lp_external_glpk_oop_integration.c
LP_EXTERNAL_GLPK_OOP_INTEGRATION_TEST_EXEC = test_lp_external_glpk_oop_integration

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

# Modular public API split tests (LP/MIP orthogonal headers)
LP_PUBLIC_API_SPLIT_TEST_SOURCES = $(TEST_DIR)/test_lp_public_api_split.c
LP_PUBLIC_API_SPLIT_TEST_EXEC = test_lp_public_api_split
MIP_PUBLIC_API_SPLIT_TEST_SOURCES = $(TEST_DIR)/test_mip_public_api_split.c
MIP_PUBLIC_API_SPLIT_TEST_EXEC = test_mip_public_api_split
LP_ERROR_API_TEST_SOURCES = $(TEST_DIR)/test_lp_error_api.c
LP_ERROR_API_TEST_EXEC = test_lp_error_api

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

CORE_REGRESSION_TEST_EXECS = $(TEST_EXEC) $(API_TEST_EXEC)

LP_SUITE_EXECS = $(CORE_REGRESSION_TEST_EXECS) \
                 $(LP_TEST_EXEC) \
                 $(PRESOLVE_TEST_EXEC) \
                 $(BANDM_TEST_EXEC) \
                 $(BLEND_TEST_EXEC) \
                 $(LOTFI_TEST_EXEC) \
                 $(SIMPLEX_POLICY_TEST_EXEC) \
                 $(DUAL_RATIO_FLIP_TEST_EXEC) \
                 $(LP_REFACTOR_POLICY_TEST_EXEC) \
                 $(LP_REINVERT_CONTROLLER_TEST_EXEC) \
                 $(LP_BASIS_GOVERNOR_TEST_EXEC) \
                 $(LP_BASIS_GOVERNOR_RUNTIME_TEST_EXEC) \
                 $(LP_DETERMINISM_TEST_EXEC) \
                 $(LP_SENSITIVITY_TEST_EXEC) \
                 $(LP_CONFLICT_TEST_EXEC) \
                 $(LP_ALGORITHM_API_TEST_EXEC) \
                 $(LP_POLICY_GLPK_COMPAT_TEST_EXEC) \
                 $(LP_GLPK_STRICT_TEST_EXEC) \
                 $(LP_GLPK_STRICT_BFCP_TEST_EXEC) \
                 $(LP_BFCP_POLICY_TEST_EXEC) \
                 $(LP_DISPATCH_TEST_EXEC) \
                 $(LP_EXTERNAL_ADAPTER_TEST_EXEC) \
                 $(LP_EXTERNAL_ADAPTER_API_TEST_EXEC) \
                 $(LP_EXTERNAL_INTROSPECTION_API_TEST_EXEC) \
                 $(LP_EXTERNAL_FAILURE_API_TEST_EXEC) \
                 $(LP_EXTERNAL_OOP_RUNNER_TEST_EXEC) \
                 $(LP_EXTERNAL_GLPK_OOP_TEST_EXEC) \
                 $(LP_EXTERNAL_GLPK_OOP_INTEGRATION_TEST_EXEC) \
                 $(LP_TELEMETRY_TEST_EXECS) \
                 $(LP_PUBLIC_API_SPLIT_TEST_EXEC) \
                 $(LP_ERROR_API_TEST_EXEC) \
                 $(MPS_PARSER_TEST_EXEC) \
                 $(LU_MARKOWITZ_TEST_EXEC) \
                 $(LU_SUPERNODE_TEST_EXEC)

MIP_SUITE_EXECS = $(CORE_REGRESSION_TEST_EXECS) \
                  $(MIP_PUBLIC_API_SPLIT_TEST_EXEC) \
                  $(MIP_WARMSTART_TEST_EXEC) \
                  $(MIP_LP_RECOVERY_TEST_EXEC) \
                  $(MIP_START_SPARSE_TEST_EXEC) \
                  $(MIP_START_REPAIR_TEST_EXEC) \
                  $(MIP_BUG_TEST_EXEC) \
                  $(SPP_CONTEXT_TEST_EXEC) \
                  $(SPP_CUTS_TEST_EXEC) \
                  $(SPP_HEURISTIC_TEST_EXEC) \
                  $(SPP_BRANCH_TEST_EXEC) \
                  $(WARMSTART_SERDE_TEST_EXEC)

REGRESSION_SUITE_EXECS = $(CORE_REGRESSION_TEST_EXECS) \
                         $(filter-out $(CORE_REGRESSION_TEST_EXECS),$(LP_SUITE_EXECS) $(MIP_SUITE_EXECS))

define run_test_execs
	@set -e; \
	for exec in $(1); do \
		echo "Running $$exec"; \
		./$$exec; \
	done
endef


# Default target
all: $(SHARED_LIB) lib $(TEST_EXEC)

# Build library only
lib: $(SHARED_LIB) $(LIB_NAME)

# Build shared library (dependency)
$(SHARED_LIB):
	$(MAKE) -C ../shared lib

$(LIB_NAME): $(OBJECTS)
	rm -f $@
	@rm -f *.obj *.pdb *.ilk *.exp 2>/dev/null || true
	$(AR_CMD)$@ $^

# Build object files
$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) $(DEPFLAGS) -c $< $(OBJ_OUT)$@

# Build optim submodule object files
$(SRC_DIR)/optim/%.o: $(SRC_DIR)/optim/%.c
	$(CC) $(CFLAGS) $(INCLUDES) $(DEPFLAGS) -c $< $(OBJ_OUT)$@

# Build test executable
$(TEST_EXEC): $(TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Debug build (with sanitizers)
# Run the transport suite under AddressSanitizer + UBSan.
#
# Scoped to test-transport rather than the whole suite: this is the test that
# exercises sh_api.c and sh_transport_direct.c, which is where the audit found
# the memory-safety defects, and Ralph's full suite under -O0 + ASan would cost
# far more CI time for far less signal.
test-transport-asan: CFLAGS = $(CFLAGS_DEBUG)
test-transport-asan: LDFLAGS += $(DEBUG_LDFLAGS)
test-transport-asan: clean test-transport

debug: CFLAGS = $(CFLAGS_DEBUG)
debug: LDFLAGS += $(DEBUG_LDFLAGS)
debug: clean all

# Run tests
test: $(TEST_EXEC) $(PRESOLVE_TEST_EXEC) $(BANDM_TEST_EXEC) $(BLEND_TEST_EXEC) $(LOTFI_TEST_EXEC) $(SIMPLEX_POLICY_TEST_EXEC) $(PHASE1_RECOVERY_TEST_EXEC) $(PHASE1_ENGINE_TEST_EXEC) $(PHASE1_ZONES_TEST_EXEC) $(DUAL_RATIO_FLIP_TEST_EXEC) $(LP_REFACTOR_POLICY_TEST_EXEC) $(LP_REINVERT_CONTROLLER_TEST_EXEC) $(LP_BASIS_GOVERNOR_TEST_EXEC) $(LP_BASIS_GOVERNOR_RUNTIME_TEST_EXEC) $(LP_DETERMINISM_TEST_EXEC) $(LP_SENSITIVITY_TEST_EXEC) $(LP_CONFLICT_TEST_EXEC) $(LP_ALGORITHM_API_TEST_EXEC) $(LP_POLICY_GLPK_COMPAT_TEST_EXEC) $(LP_GLPK_STRICT_TEST_EXEC) $(LP_BFCP_POLICY_TEST_EXEC) $(LP_DISPATCH_TEST_EXEC) $(LP_EXTERNAL_ADAPTER_TEST_EXEC) $(LP_EXTERNAL_ADAPTER_API_TEST_EXEC) $(LP_EXTERNAL_INTROSPECTION_API_TEST_EXEC) $(LP_EXTERNAL_FAILURE_API_TEST_EXEC) $(LP_EXTERNAL_OOP_RUNNER_TEST_EXEC) $(LP_EXTERNAL_GLPK_OOP_TEST_EXEC) $(LP_TELEMETRY_TEST_EXECS) $(LP_PUBLIC_API_SPLIT_TEST_EXEC) $(MIP_PUBLIC_API_SPLIT_TEST_EXEC) $(LP_ERROR_API_TEST_EXEC) $(MPS_PARSER_TEST_EXEC) $(API_TEST_EXEC) $(MIP_WARMSTART_TEST_EXEC) $(MIP_LP_RECOVERY_TEST_EXEC) $(MIP_START_SPARSE_TEST_EXEC) $(MIP_START_REPAIR_TEST_EXEC) $(MIP_BUG_TEST_EXEC) $(WARMSTART_SERDE_TEST_EXEC) $(LIFTED_COVER_TEST_EXEC) $(PRESOLVE_DIFF_TEST_EXEC) $(UNBOUNDED_INF_TEST_EXEC) $(INFEAS_EQ_TEST_EXEC)
	./$(TEST_EXEC)
	./$(PRESOLVE_TEST_EXEC)
	./$(BANDM_TEST_EXEC)
	./$(BLEND_TEST_EXEC)
	./$(LOTFI_TEST_EXEC)
	./$(SIMPLEX_POLICY_TEST_EXEC)
	./$(PHASE1_RECOVERY_TEST_EXEC)
	./$(PHASE1_ENGINE_TEST_EXEC)
	./$(PHASE1_ZONES_TEST_EXEC)
	./$(DUAL_RATIO_FLIP_TEST_EXEC)
	./$(LP_REFACTOR_POLICY_TEST_EXEC)
	./$(LP_REINVERT_CONTROLLER_TEST_EXEC)
	./$(LP_BASIS_GOVERNOR_TEST_EXEC)
	./$(LP_BASIS_GOVERNOR_RUNTIME_TEST_EXEC)
	./$(LP_DETERMINISM_TEST_EXEC)
	./$(LP_SENSITIVITY_TEST_EXEC)
	./$(LP_CONFLICT_TEST_EXEC)
	./$(LP_ALGORITHM_API_TEST_EXEC)
	./$(LP_POLICY_GLPK_COMPAT_TEST_EXEC)
	./$(LP_GLPK_STRICT_TEST_EXEC)
	./$(LP_BFCP_POLICY_TEST_EXEC)
	./$(LP_DISPATCH_TEST_EXEC)
	./$(LP_EXTERNAL_ADAPTER_TEST_EXEC)
	./$(LP_EXTERNAL_ADAPTER_API_TEST_EXEC)
	./$(LP_EXTERNAL_INTROSPECTION_API_TEST_EXEC)
	./$(LP_EXTERNAL_FAILURE_API_TEST_EXEC)
	./$(LP_EXTERNAL_OOP_RUNNER_TEST_EXEC)
	./$(LP_EXTERNAL_GLPK_OOP_TEST_EXEC)
	./$(LP_TELEMETRY_SOLVER_TEST_EXEC)
	./$(LP_TELEMETRY_LU_TEST_EXEC)
	./$(LP_TELEMETRY_GATE_TEST_EXEC)
	./$(LP_TELEMETRY_LU_SPARSE_TEST_EXEC)
	./$(LP_PUBLIC_API_SPLIT_TEST_EXEC)
	./$(MIP_PUBLIC_API_SPLIT_TEST_EXEC)
	./$(LP_ERROR_API_TEST_EXEC)
	./$(MPS_PARSER_TEST_EXEC)
	./$(API_TEST_EXEC)
	./$(MIP_WARMSTART_TEST_EXEC)
	./$(MIP_LP_RECOVERY_TEST_EXEC)
	./$(MIP_START_SPARSE_TEST_EXEC)
	./$(MIP_START_REPAIR_TEST_EXEC)
	./$(MIP_BUG_TEST_EXEC)
	./$(WARMSTART_SERDE_TEST_EXEC)
	./$(LIFTED_COVER_TEST_EXEC)
	./$(PRESOLVE_DIFF_TEST_EXEC)
	./$(UNBOUNDED_INF_TEST_EXEC)
	./$(INFEAS_EQ_TEST_EXEC)

test-core: $(CORE_REGRESSION_TEST_EXECS)
	$(call run_test_execs,$(CORE_REGRESSION_TEST_EXECS))

test-lp-suite: $(LP_SUITE_EXECS)
	$(call run_test_execs,$(LP_SUITE_EXECS))

test-mip-suite: $(MIP_SUITE_EXECS)
	$(call run_test_execs,$(MIP_SUITE_EXECS))

test-regression-suites: $(REGRESSION_SUITE_EXECS)
	$(call run_test_execs,$(REGRESSION_SUITE_EXECS))

# Known regression tests (beaconfd — Phase 2 pivot failure, degenerate theta=0)
# Run separately; not in the main test gate until ratio test pivot threshold is fixed.
test-beaconfd: $(BEACONFD_TEST_EXEC) $(BEACONFD_TRACE_TEST_EXEC)
	./$(BEACONFD_TEST_EXEC)
	./$(BEACONFD_TRACE_TEST_EXEC)

# Build edge case tests
$(EDGE_TEST_EXEC): $(EDGE_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run edge case tests
test-edge: $(EDGE_TEST_EXEC)
	./$(EDGE_TEST_EXEC)

# Build LAP tests
$(LAP_TEST_EXEC): $(LAP_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LAP tests
test-lap: $(LAP_TEST_EXEC)
	./$(LAP_TEST_EXEC)

# Build network flow tests
$(NETFLOW_TEST_EXEC): $(NETFLOW_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run network flow tests
test-netflow: $(NETFLOW_TEST_EXEC)
	./$(NETFLOW_TEST_EXEC)

# Build network detection tests
$(DETECT_TEST_EXEC): $(DETECT_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run network detection tests
test-detect: $(DETECT_TEST_EXEC)
	./$(DETECT_TEST_EXEC)

# Build optimization module tests
$(OPTIM_TEST_EXEC): $(OPTIM_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Build LP format tests
$(LP_TEST_EXEC): $(LP_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LP format tests
test-lp: $(LP_TEST_EXEC)
	./$(LP_TEST_EXEC)

# Build NETLIB MPS parser regression tests
$(MPS_PARSER_TEST_EXEC): $(MPS_PARSER_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run NETLIB MPS parser regression tests
test-mps-parser: $(MPS_PARSER_TEST_EXEC)
	./$(MPS_PARSER_TEST_EXEC)

# Build API tests
$(API_TEST_EXEC): $(API_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run API tests
test-api: $(API_TEST_EXEC)
	./$(API_TEST_EXEC)

# Build transport tests
$(TRANSPORT_TEST_EXEC): $(TRANSPORT_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run transport tests
test-transport: $(TRANSPORT_TEST_EXEC)
	./$(TRANSPORT_TEST_EXEC)

# Build Benders tests
$(BENDERS_TEST_EXEC): $(BENDERS_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run Benders tests
test-benders: $(BENDERS_TEST_EXEC)
	./$(BENDERS_TEST_EXEC)

# Build Benders debug harness (investigative, not part of main test gate)
$(BENDERS_DEBUG_TEST_EXEC): $(BENDERS_DEBUG_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run Benders debug harness
test-benders-debug: $(BENDERS_DEBUG_TEST_EXEC)
	./$(BENDERS_DEBUG_TEST_EXEC)

# Known Benders regressions (non-gating until fixed).
test-benders-known-regression: $(BENDERS_WARMSTART_TEST_EXEC) $(BENDERS_MASTER_START_TEST_EXEC) $(BENDERS_DEBUG_TEST_EXEC)
	@echo "Running known-regression Benders tests (non-gating)..."
	@status=0; \
	./$(BENDERS_WARMSTART_TEST_EXEC) || status=1; \
	./$(BENDERS_MASTER_START_TEST_EXEC) || status=1; \
	./$(BENDERS_DEBUG_TEST_EXEC) || status=1; \
	if [ $$status -ne 0 ]; then \
		echo "Known Benders regressions reproduced (expected while non-gating)."; \
	else \
		echo "Benders known-regression tests now pass; consider promoting to gating."; \
	fi

# Build focused Benders warm-start tests
$(BENDERS_WARMSTART_TEST_EXEC): $(BENDERS_WARMSTART_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run focused Benders warm-start tests
test-benders-warmstart: $(BENDERS_WARMSTART_TEST_EXEC)
	./$(BENDERS_WARMSTART_TEST_EXEC)

# Build focused MIP warm-start tests
$(MIP_WARMSTART_TEST_EXEC): $(MIP_WARMSTART_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run focused MIP warm-start tests
test-mip-warmstart: $(MIP_WARMSTART_TEST_EXEC)
	./$(MIP_WARMSTART_TEST_EXEC)

# Build focused MIP/LP recovery-contract tests
$(MIP_LP_RECOVERY_TEST_EXEC): $(MIP_LP_RECOVERY_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run focused MIP/LP recovery-contract tests
test-mip-lp-recovery: $(MIP_LP_RECOVERY_TEST_EXEC)
	./$(MIP_LP_RECOVERY_TEST_EXEC)

# Build sparse/partial MIP-start tests
$(MIP_START_SPARSE_TEST_EXEC): $(MIP_START_SPARSE_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run sparse/partial MIP-start tests
test-mip-start-sparse: $(MIP_START_SPARSE_TEST_EXEC)
	./$(MIP_START_SPARSE_TEST_EXEC)

# Build MIP-start repair-mode tests
$(MIP_START_REPAIR_TEST_EXEC): $(MIP_START_REPAIR_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run MIP-start repair-mode tests
test-mip-start-repair: $(MIP_START_REPAIR_TEST_EXEC)
	./$(MIP_START_REPAIR_TEST_EXEC)

# Build MIP regression reproducer test
$(MIP_BUG_TEST_EXEC): $(MIP_BUG_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run MIP regression reproducer test
test-mip-bug: $(MIP_BUG_TEST_EXEC)
	./$(MIP_BUG_TEST_EXEC)

# Build standalone SPP context tests
$(SPP_CONTEXT_TEST_EXEC): $(SPP_CONTEXT_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run standalone SPP context tests
test-spp-context: $(SPP_CONTEXT_TEST_EXEC)
	./$(SPP_CONTEXT_TEST_EXEC)

# Build standalone SPP cut tests
$(LIFTED_COVER_TEST_EXEC): $(LIFTED_COVER_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

test-lifted-cover: $(LIFTED_COVER_TEST_EXEC)
	./$(LIFTED_COVER_TEST_EXEC)

$(PRESOLVE_DIFF_TEST_EXEC): $(PRESOLVE_DIFF_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

test-presolve-differential: $(PRESOLVE_DIFF_TEST_EXEC)
	./$(PRESOLVE_DIFF_TEST_EXEC)

$(UNBOUNDED_INF_TEST_EXEC): $(UNBOUNDED_INF_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

test-unbounded-infinite-bound: $(UNBOUNDED_INF_TEST_EXEC)
	./$(UNBOUNDED_INF_TEST_EXEC)

$(INFEAS_EQ_TEST_EXEC): $(INFEAS_EQ_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

test-infeasible-equality-bounds: $(INFEAS_EQ_TEST_EXEC)
	./$(INFEAS_EQ_TEST_EXEC)

$(SPP_CUTS_TEST_EXEC): $(SPP_CUTS_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run standalone SPP cut tests
test-spp-cuts: $(SPP_CUTS_TEST_EXEC)
	./$(SPP_CUTS_TEST_EXEC)

# Build standalone SPP heuristic tests
$(SPP_HEURISTIC_TEST_EXEC): $(SPP_HEURISTIC_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run standalone SPP heuristic tests
test-spp-heuristic: $(SPP_HEURISTIC_TEST_EXEC)
	./$(SPP_HEURISTIC_TEST_EXEC)

# Build standalone SPP branching/propagation tests
$(SPP_BRANCH_TEST_EXEC): $(SPP_BRANCH_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run standalone SPP branching/propagation tests
test-spp-branch: $(SPP_BRANCH_TEST_EXEC)
	./$(SPP_BRANCH_TEST_EXEC)

# Build Benders initial master-start tests
$(BENDERS_MASTER_START_TEST_EXEC): $(BENDERS_MASTER_START_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run Benders initial master-start tests
test-benders-master-start: $(BENDERS_MASTER_START_TEST_EXEC)
	./$(BENDERS_MASTER_START_TEST_EXEC)

# Build warm-start serialization tests
$(WARMSTART_SERDE_TEST_EXEC): $(WARMSTART_SERDE_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run warm-start serialization tests
test-warmstart-serde: $(WARMSTART_SERDE_TEST_EXEC)
	./$(WARMSTART_SERDE_TEST_EXEC)

# Build presolve unit tests
$(PRESOLVE_TEST_EXEC): $(PRESOLVE_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run presolve unit tests
test-presolve: $(PRESOLVE_TEST_EXEC)
	./$(PRESOLVE_TEST_EXEC)

# Build beaconfd numerical-regression test
$(BEACONFD_TEST_EXEC): $(BEACONFD_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# test-beaconfd target is defined above (known regression, not in main gate)

# Build NETLIB regression tests (bandm, blend, lotfi)
$(BANDM_TEST_EXEC): $(BANDM_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

$(BLEND_TEST_EXEC): $(BLEND_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

$(LOTFI_TEST_EXEC): $(LOTFI_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run NETLIB regression tests individually
test-bandm: $(BANDM_TEST_EXEC)
	./$(BANDM_TEST_EXEC)

test-blend: $(BLEND_TEST_EXEC)
	./$(BLEND_TEST_EXEC)

test-lotfi: $(LOTFI_TEST_EXEC)
	./$(LOTFI_TEST_EXEC)

# Build beaconfd phase-1 trace regression test
$(BEACONFD_TRACE_TEST_EXEC): $(BEACONFD_TRACE_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run beaconfd phase-1 trace regression test
test-beaconfd-trace: $(BEACONFD_TRACE_TEST_EXEC)
	./$(BEACONFD_TRACE_TEST_EXEC)

# Build Phase 1 recovery progress ops test
$(PHASE1_RECOVERY_TEST_EXEC): $(PHASE1_RECOVERY_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -I./src $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run Phase 1 recovery progress ops test
test-phase1-recovery: $(PHASE1_RECOVERY_TEST_EXEC)
	./$(PHASE1_RECOVERY_TEST_EXEC)

# Build Phase 1 zone handler tests
$(PHASE1_ENGINE_TEST_EXEC): $(PHASE1_ENGINE_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -I./src $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

test-phase1-engine: $(PHASE1_ENGINE_TEST_EXEC)
	./$(PHASE1_ENGINE_TEST_EXEC)

$(PHASE1_ZONES_TEST_EXEC): $(PHASE1_ZONES_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -I./src $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run Phase 1 zone handler tests
test-phase1-zones: $(PHASE1_ZONES_TEST_EXEC)
	./$(PHASE1_ZONES_TEST_EXEC)

# Build simplex basis-action policy test
$(SIMPLEX_POLICY_TEST_EXEC): $(SIMPLEX_POLICY_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run simplex basis-action policy test
test-simplex-policy: $(SIMPLEX_POLICY_TEST_EXEC)
	./$(SIMPLEX_POLICY_TEST_EXEC)

# Build LP refactor-policy module test
$(LP_REFACTOR_POLICY_TEST_EXEC): $(LP_REFACTOR_POLICY_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LP refactor-policy module test
test-lp-refactor-policy: $(LP_REFACTOR_POLICY_TEST_EXEC)
	./$(LP_REFACTOR_POLICY_TEST_EXEC)

# Build LP reinvert-controller module test
$(LP_REINVERT_CONTROLLER_TEST_EXEC): $(LP_REINVERT_CONTROLLER_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LP reinvert-controller module test
test-lp-reinvert-controller: $(LP_REINVERT_CONTROLLER_TEST_EXEC)
	./$(LP_REINVERT_CONTROLLER_TEST_EXEC)

# Build LP basis-governor module test
$(LP_BASIS_GOVERNOR_TEST_EXEC): $(LP_BASIS_GOVERNOR_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LP basis-governor module test
test-lp-basis-governor: $(LP_BASIS_GOVERNOR_TEST_EXEC)
	./$(LP_BASIS_GOVERNOR_TEST_EXEC)

# Build LP basis-governor runtime test
$(LP_BASIS_GOVERNOR_RUNTIME_TEST_EXEC): $(LP_BASIS_GOVERNOR_RUNTIME_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LP basis-governor runtime test
test-lp-basis-governor-runtime: $(LP_BASIS_GOVERNOR_RUNTIME_TEST_EXEC)
	./$(LP_BASIS_GOVERNOR_RUNTIME_TEST_EXEC)

# Build LP determinism tests
$(LP_DETERMINISM_TEST_EXEC): $(LP_DETERMINISM_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LP determinism tests
test-lp-determinism: $(LP_DETERMINISM_TEST_EXEC)
	./$(LP_DETERMINISM_TEST_EXEC)

# Build LP sensitivity tests
$(LP_SENSITIVITY_TEST_EXEC): $(LP_SENSITIVITY_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LP sensitivity tests
test-lp-sensitivity: $(LP_SENSITIVITY_TEST_EXEC)
	./$(LP_SENSITIVITY_TEST_EXEC)

# Build LP conflict tests
$(LP_CONFLICT_TEST_EXEC): $(LP_CONFLICT_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LP conflict tests
test-lp-conflict: $(LP_CONFLICT_TEST_EXEC)
	./$(LP_CONFLICT_TEST_EXEC)

# Build LP algorithm API tests
$(LP_ALGORITHM_API_TEST_EXEC): $(LP_ALGORITHM_API_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LP algorithm API tests
test-lp-algorithm-api: $(LP_ALGORITHM_API_TEST_EXEC)
	./$(LP_ALGORITHM_API_TEST_EXEC)

# Build LP GLPK-compatible policy module tests
$(LP_POLICY_GLPK_COMPAT_TEST_EXEC): $(LP_POLICY_GLPK_COMPAT_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LP GLPK-compatible policy module tests
test-lp-policy-glpk-compat: $(LP_POLICY_GLPK_COMPAT_TEST_EXEC)
	./$(LP_POLICY_GLPK_COMPAT_TEST_EXEC)

# Build LP GLPK strict-mode gate tests
$(LP_GLPK_STRICT_TEST_EXEC): $(LP_GLPK_STRICT_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LP GLPK strict-mode gate tests
test-lp-glpk-strict: $(LP_GLPK_STRICT_TEST_EXEC)
	./$(LP_GLPK_STRICT_TEST_EXEC)

# Build LP GLPK strict BFCP dispatch tests
$(LP_GLPK_STRICT_BFCP_TEST_EXEC): $(LP_GLPK_STRICT_BFCP_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LP GLPK strict BFCP dispatch tests
test-lp-glpk-strict-bfcp: $(LP_GLPK_STRICT_BFCP_TEST_EXEC)
	./$(LP_GLPK_STRICT_BFCP_TEST_EXEC)

# Build LP BFCP policy module tests
$(LP_BFCP_POLICY_TEST_EXEC): $(LP_BFCP_POLICY_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LP BFCP policy module tests
test-lp-bfcp-policy: $(LP_BFCP_POLICY_TEST_EXEC)
	./$(LP_BFCP_POLICY_TEST_EXEC)

# Build LP dispatch module tests
$(LP_DISPATCH_TEST_EXEC): $(LP_DISPATCH_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LP dispatch module tests
test-lp-dispatch: $(LP_DISPATCH_TEST_EXEC)
	./$(LP_DISPATCH_TEST_EXEC)

# Build LP external adapter module tests
$(LP_EXTERNAL_ADAPTER_TEST_EXEC): $(LP_EXTERNAL_ADAPTER_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LP external adapter module tests
test-lp-external-adapter: $(LP_EXTERNAL_ADAPTER_TEST_EXEC)
	./$(LP_EXTERNAL_ADAPTER_TEST_EXEC)

# Build public LP external adapter API tests
$(LP_EXTERNAL_ADAPTER_API_TEST_EXEC): $(LP_EXTERNAL_ADAPTER_API_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run public LP external adapter API tests
test-lp-external-adapter-api: $(LP_EXTERNAL_ADAPTER_API_TEST_EXEC)
	./$(LP_EXTERNAL_ADAPTER_API_TEST_EXEC)

# Build LP external introspection API tests
$(LP_EXTERNAL_INTROSPECTION_API_TEST_EXEC): $(LP_EXTERNAL_INTROSPECTION_API_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LP external introspection API tests
test-lp-external-introspection-api: $(LP_EXTERNAL_INTROSPECTION_API_TEST_EXEC)
	./$(LP_EXTERNAL_INTROSPECTION_API_TEST_EXEC)

# Build LP external failure mapping API tests
$(LP_EXTERNAL_FAILURE_API_TEST_EXEC): $(LP_EXTERNAL_FAILURE_API_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LP external failure mapping API tests
test-lp-external-failure-api: $(LP_EXTERNAL_FAILURE_API_TEST_EXEC)
	./$(LP_EXTERNAL_FAILURE_API_TEST_EXEC)

# Build LP external generic OOP runner tests
$(LP_EXTERNAL_OOP_RUNNER_TEST_EXEC): $(LP_EXTERNAL_OOP_RUNNER_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LP external generic OOP runner tests
test-lp-external-oop-runner: $(LP_EXTERNAL_OOP_RUNNER_TEST_EXEC)
	./$(LP_EXTERNAL_OOP_RUNNER_TEST_EXEC)

# Build LP external GLPK out-of-process adapter tests
$(LP_EXTERNAL_GLPK_OOP_TEST_EXEC): $(LP_EXTERNAL_GLPK_OOP_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LP external GLPK out-of-process adapter tests
test-lp-external-glpk-oop: $(LP_EXTERNAL_GLPK_OOP_TEST_EXEC)
	./$(LP_EXTERNAL_GLPK_OOP_TEST_EXEC)

# Build optional real-GLPK integration test (self-skips when glpsol absent)
$(LP_EXTERNAL_GLPK_OOP_INTEGRATION_TEST_EXEC): $(LP_EXTERNAL_GLPK_OOP_INTEGRATION_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run optional real-GLPK integration test
test-lp-external-glpk-oop-integration: $(LP_EXTERNAL_GLPK_OOP_INTEGRATION_TEST_EXEC)
	./$(LP_EXTERNAL_GLPK_OOP_INTEGRATION_TEST_EXEC)

# Build LP telemetry solver tests
$(LP_TELEMETRY_SOLVER_TEST_EXEC): $(LP_TELEMETRY_SOLVER_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Build LP telemetry LU tests
$(LP_TELEMETRY_LU_TEST_EXEC): $(LP_TELEMETRY_LU_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Build LP telemetry runtime gate tests
$(LP_TELEMETRY_GATE_TEST_EXEC): $(LP_TELEMETRY_GATE_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Build LP telemetry sparse-LU tests
$(LP_TELEMETRY_LU_SPARSE_TEST_EXEC): $(LP_TELEMETRY_LU_SPARSE_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

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

# Build modular LP public API split tests
$(LP_PUBLIC_API_SPLIT_TEST_EXEC): $(LP_PUBLIC_API_SPLIT_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run modular LP public API split tests
test-lp-public-api-split: $(LP_PUBLIC_API_SPLIT_TEST_EXEC)
	./$(LP_PUBLIC_API_SPLIT_TEST_EXEC)

# Build modular MIP public API split tests
$(MIP_PUBLIC_API_SPLIT_TEST_EXEC): $(MIP_PUBLIC_API_SPLIT_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run modular MIP public API split tests
test-mip-public-api-split: $(MIP_PUBLIC_API_SPLIT_TEST_EXEC)
	./$(MIP_PUBLIC_API_SPLIT_TEST_EXEC)

# Build structured API error mapping tests
$(LP_ERROR_API_TEST_EXEC): $(LP_ERROR_API_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run structured API error mapping tests
test-lp-error-api: $(LP_ERROR_API_TEST_EXEC)
	./$(LP_ERROR_API_TEST_EXEC)

# Build LU Markowitz tests
$(LU_MARKOWITZ_TEST_EXEC): $(LU_MARKOWITZ_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LU Markowitz tests
test-lu-markowitz: $(LU_MARKOWITZ_TEST_EXEC)
	./$(LU_MARKOWITZ_TEST_EXEC)

# Build dual ratio flip-mode tests
$(DUAL_RATIO_FLIP_TEST_EXEC): $(DUAL_RATIO_FLIP_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run dual ratio flip-mode tests
test-dual-ratio-flip: $(DUAL_RATIO_FLIP_TEST_EXEC)
	./$(DUAL_RATIO_FLIP_TEST_EXEC)

# Build LU supernode tests
$(LU_SUPERNODE_TEST_EXEC): $(LU_SUPERNODE_TEST_SOURCES) $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LU supernode tests
test-lu-supernode: $(LU_SUPERNODE_TEST_EXEC)
	./$(LU_SUPERNODE_TEST_EXEC)

# Run optimization module tests
test-optim: $(OPTIM_TEST_EXEC)
	./$(OPTIM_TEST_EXEC)

# Build size scaling benchmark
$(BENCH_SIZES_EXEC): $(BENCH_DIR)/bench_sizes.c $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run size scaling benchmark
bench: $(BENCH_SIZES_EXEC)
	./$(BENCH_SIZES_EXEC)

# Build GLPK comparison benchmark (old version)
$(COMPARE_GLPK_EXEC): $(BENCH_DIR)/compare_glpk.c $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run GLPK comparison (requires glpsol to be installed)
compare-glpk: $(COMPARE_GLPK_EXEC)
	./$(COMPARE_GLPK_EXEC)

# Build Ralph vs GLPK benchmark (GLPK routed through external OOP adapter)
$(BENCH_VS_GLPK_EXEC): $(BENCH_DIR)/bench_vs_glpk.c $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run Ralph vs GLPK benchmark
bench-glpk: $(BENCH_VS_GLPK_EXEC)
	./$(BENCH_VS_GLPK_EXEC)

# Quick benchmark
bench-glpk-quick: $(BENCH_VS_GLPK_EXEC)
	./$(BENCH_VS_GLPK_EXEC) --quick

# Build MIP benchmark
$(BENCH_MIP_EXEC): $(BENCH_DIR)/bench_mip.c $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $(GLPK_INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(GLPK_LIBS) $(LDFLAGS) $(EXE_OUT)$@

# Run MIP benchmark
bench-mip: $(BENCH_MIP_EXEC)
	./$(BENCH_MIP_EXEC)

# Quick MIP benchmark
bench-mip-quick: $(BENCH_MIP_EXEC)
	./$(BENCH_MIP_EXEC) --quick

# Build profile benchmark
$(PROFILE_BENCH_EXEC): $(BENCH_DIR)/profile_bench.c $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run profile benchmark
profile-bench: $(PROFILE_BENCH_EXEC)
	./$(PROFILE_BENCH_EXEC)

# Build LAP benchmark
$(BENCH_LAP_EXEC): $(BENCH_DIR)/bench_lap.c $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LAP benchmark
bench-lap: $(BENCH_LAP_EXEC)
	./$(BENCH_LAP_EXEC)

# Build LAP performance benchmark
$(BENCH_LAP_PERF_EXEC): $(BENCH_DIR)/bench_lap_perf.c $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run LAP performance benchmark
bench-lap-perf: $(BENCH_LAP_PERF_EXEC)
	./$(BENCH_LAP_PERF_EXEC)

# Quick LAP performance benchmark
bench-lap-perf-quick: $(BENCH_LAP_PERF_EXEC)
	./$(BENCH_LAP_PERF_EXEC) --quick

# Build network flow benchmark
$(BENCH_NETFLOW_EXEC): $(BENCH_DIR)/bench_netflow.c $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

# Run network flow benchmark
bench-netflow: $(BENCH_NETFLOW_EXEC)
	./$(BENCH_NETFLOW_EXEC)

# Build ralph-benchmark (NETLIB benchmark tool)
$(RALPH_BENCHMARK_EXEC): $(BENCH_DIR)/ralph_benchmark.c $(LIB_NAME) $(SHARED_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(call link_lib,.,ralph) $(call link_lib,../shared,shared) $(LDFLAGS) $(EXE_OUT)$@

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

# Deterministic NETLIB fit1p/nesm stress gate (promoted: gating)
test-netlib-gate-stress-fit1p-nesm: $(RALPH_BENCHMARK_EXEC)
	cd $(CURDIR) && bash benchmarks/netlib_regression_gate.sh --bench "$(CURDIR)/$(RALPH_BENCHMARK_EXEC)" --baseline "$(CURDIR)/benchmarks/netlib_stress_fit1p_nesm_baseline.json" --allowlist "$(CURDIR)/benchmarks/netlib_stress_fit1p_nesm.txt"

# Deterministic NETLIB fit1p/nesm stress gate (legacy explicit gating alias)
test-netlib-gate-stress-fit1p-nesm-gating: $(RALPH_BENCHMARK_EXEC)
	cd $(CURDIR) && bash benchmarks/netlib_regression_gate.sh --bench "$(CURDIR)/$(RALPH_BENCHMARK_EXEC)" --baseline "$(CURDIR)/benchmarks/netlib_stress_fit1p_nesm_baseline.json" --allowlist "$(CURDIR)/benchmarks/netlib_stress_fit1p_nesm.txt"

# Deterministic NETLIB fit1p/nesm stress gate (optional non-gating diagnostics)
test-netlib-gate-stress-fit1p-nesm-non-gating: $(RALPH_BENCHMARK_EXEC)
	cd $(CURDIR) && bash benchmarks/netlib_regression_gate.sh --bench "$(CURDIR)/$(RALPH_BENCHMARK_EXEC)" --baseline "$(CURDIR)/benchmarks/netlib_stress_fit1p_nesm_baseline.json" --allowlist "$(CURDIR)/benchmarks/netlib_stress_fit1p_nesm.txt" || { rc=$$?; echo "NOTE: non-gating fit1p/nesm stress gate failed (rc=$$rc)"; exit 0; }

# Deterministic NETLIB pilot-family focus gate (non-gating diagnostics)
test-netlib-gate-pilot-focus: $(RALPH_BENCHMARK_EXEC)
	cd $(CURDIR) && bash benchmarks/netlib_regression_gate.sh --bench "$(CURDIR)/$(RALPH_BENCHMARK_EXEC)" --allowlist "$(CURDIR)/benchmarks/netlib_pilot_focus.txt" --baseline "$(CURDIR)/benchmarks/netlib_pilot_focus_baseline.json" || { rc=$$?; echo "NOTE: non-gating pilot focus gate failed (rc=$$rc)"; exit 0; }

# Deterministic NETLIB timeout22 outlier gate (diagnostics baseline)
test-netlib-gate-timeout22: $(RALPH_BENCHMARK_EXEC)
	cd $(CURDIR) && bash benchmarks/netlib_regression_gate.sh --bench "$(CURDIR)/$(RALPH_BENCHMARK_EXEC)" --allowlist "$(CURDIR)/benchmarks/netlib_timeout22.txt" --baseline "$(CURDIR)/benchmarks/netlib_timeout22_baseline.json"

# Deterministic NETLIB crash6 explicit norelax/noflip harness (non-gating diagnostics)
test-netlib-gate-crash6-norelax-noflip: $(RALPH_BENCHMARK_EXEC)
	cd $(CURDIR) && chmod +x benchmarks/ralph_benchmark_norelax_noflip.sh && bash benchmarks/netlib_regression_gate.sh --bench "$(CURDIR)/benchmarks/ralph_benchmark_norelax_noflip.sh" --allowlist "$(CURDIR)/benchmarks/netlib_crash6.txt" --baseline "$(CURDIR)/benchmarks/netlib_crash6_baseline.json" --bfcp-backend 0 || { rc=$$?; echo "NOTE: non-gating crash6 norelax/noflip gate failed (rc=$$rc)"; exit 0; }

# Deterministic NETLIB full canary with reinvert control_all (non-gating diagnostics)
test-netlib-gate-control-all-canary: $(RALPH_BENCHMARK_EXEC)
	cd $(CURDIR) && bash benchmarks/netlib_regression_gate.sh --bench "$(CURDIR)/$(RALPH_BENCHMARK_EXEC)" --lp-reinvert-controller-mode 3 || { rc=$$?; echo "NOTE: non-gating control_all canary failed (rc=$$rc)"; exit 0; }

# Phase 1 feasibility-engine canaries.
phase1-canaries: $(RALPH_BENCHMARK_EXEC)
	cd $(CURDIR) && bash benchmarks/phase1_canaries.sh --bench "$(CURDIR)/$(RALPH_BENCHMARK_EXEC)"

# Ordered GLPK-strict execution tracker
glpk-strict-plan-init:
	cd $(CURDIR) && bash benchmarks/glpk_strict_plan.sh init

glpk-strict-plan-status:
	cd $(CURDIR) && bash benchmarks/glpk_strict_plan.sh status

glpk-strict-plan-next:
	cd $(CURDIR) && bash benchmarks/glpk_strict_plan.sh next

glpk-strict-plan-phase1:
	cd $(CURDIR) && bash benchmarks/glpk_strict_plan.sh run-phase 1

glpk-strict-plan-phase2:
	cd $(CURDIR) && bash benchmarks/glpk_strict_plan.sh run-phase 2

glpk-strict-plan-phase3:
	cd $(CURDIR) && bash benchmarks/glpk_strict_plan.sh run-phase 3

# Download NETLIB problems
download-netlib:
	cd $(BENCH_DIR) && bash download_netlib.sh

# Clean
clean:
	rm -f $(OBJECTS) $(DEPS) $(SRC_DIR)/lp_telemetry.o $(LIB_NAME) $(TEST_EXEC) $(PRESOLVE_TEST_EXEC) $(EDGE_TEST_EXEC) $(LAP_TEST_EXEC) $(NETFLOW_TEST_EXEC) $(DETECT_TEST_EXEC) $(OPTIM_TEST_EXEC) $(LP_TEST_EXEC) $(MPS_PARSER_TEST_EXEC) $(API_TEST_EXEC) $(BENDERS_TEST_EXEC) $(BENDERS_DEBUG_TEST_EXEC) $(BENDERS_WARMSTART_TEST_EXEC) $(MIP_WARMSTART_TEST_EXEC) $(MIP_LP_RECOVERY_TEST_EXEC) $(MIP_START_SPARSE_TEST_EXEC) $(MIP_START_REPAIR_TEST_EXEC) $(MIP_BUG_TEST_EXEC) $(SPP_CONTEXT_TEST_EXEC) $(SPP_CUTS_TEST_EXEC) $(LIFTED_COVER_TEST_EXEC) $(PRESOLVE_DIFF_TEST_EXEC) $(UNBOUNDED_INF_TEST_EXEC) $(INFEAS_EQ_TEST_EXEC) $(SPP_HEURISTIC_TEST_EXEC) $(SPP_BRANCH_TEST_EXEC) $(BENDERS_MASTER_START_TEST_EXEC) $(WARMSTART_SERDE_TEST_EXEC) $(BEACONFD_TEST_EXEC) $(BEACONFD_TRACE_TEST_EXEC) $(SIMPLEX_POLICY_TEST_EXEC) $(PHASE1_RECOVERY_TEST_EXEC) $(PHASE1_ENGINE_TEST_EXEC) $(PHASE1_ZONES_TEST_EXEC) $(DUAL_RATIO_FLIP_TEST_EXEC) $(LP_REFACTOR_POLICY_TEST_EXEC) $(LP_REINVERT_CONTROLLER_TEST_EXEC) $(LP_BASIS_GOVERNOR_TEST_EXEC) $(LP_BASIS_GOVERNOR_RUNTIME_TEST_EXEC) $(LP_DETERMINISM_TEST_EXEC) $(LP_SENSITIVITY_TEST_EXEC) $(LP_CONFLICT_TEST_EXEC) $(LP_ALGORITHM_API_TEST_EXEC) $(LP_POLICY_GLPK_COMPAT_TEST_EXEC) $(LP_GLPK_STRICT_TEST_EXEC) $(LP_GLPK_STRICT_BFCP_TEST_EXEC) $(LP_BFCP_POLICY_TEST_EXEC) $(LP_DISPATCH_TEST_EXEC) $(LP_EXTERNAL_ADAPTER_TEST_EXEC) $(LP_EXTERNAL_ADAPTER_API_TEST_EXEC) $(LP_EXTERNAL_INTROSPECTION_API_TEST_EXEC) $(LP_EXTERNAL_FAILURE_API_TEST_EXEC) $(LP_EXTERNAL_OOP_RUNNER_TEST_EXEC) $(LP_EXTERNAL_GLPK_OOP_TEST_EXEC) $(LP_EXTERNAL_GLPK_OOP_INTEGRATION_TEST_EXEC) $(LP_TELEMETRY_TEST_EXECS) $(LP_PUBLIC_API_SPLIT_TEST_EXEC) $(MIP_PUBLIC_API_SPLIT_TEST_EXEC) $(LP_ERROR_API_TEST_EXEC) $(LU_MARKOWITZ_TEST_EXEC) $(LU_SUPERNODE_TEST_EXEC) $(BENCH_SIZES_EXEC) $(COMPARE_GLPK_EXEC) $(BENCH_VS_GLPK_EXEC) $(BENCH_MIP_EXEC) $(PROFILE_BENCH_EXEC) $(BENCH_LAP_EXEC) $(BENCH_LAP_PERF_EXEC) $(BENCH_NETFLOW_EXEC) $(RALPH_BENCHMARK_EXEC) $(TRANSPORT_TEST_EXEC)

# Install headers and library
install: $(LIB_NAME)
	mkdir -p /usr/local/include/ralph
	mkdir -p /usr/local/lib
	cp $(INCLUDE_DIR)/ralph_lp.h /usr/local/include/ralph/
	cp $(INCLUDE_DIR)/ralph_mip.h /usr/local/include/ralph/
	cp $(LIB_NAME) /usr/local/lib/

# Uninstall
uninstall:
	rm -rf /usr/local/include/ralph
	rm -f /usr/local/lib/$(LIB_NAME)

.PHONY: all lib clean debug test test-transport test-transport-asan test-core test-lp-suite test-mip-suite test-regression-suites test-edge test-lap test-netflow test-detect test-optim test-api test-benders test-benders-debug test-benders-known-regression test-benders-warmstart test-mip-bug test-mip-warmstart test-mip-lp-recovery test-mip-start-sparse test-mip-start-repair test-spp-context test-spp-cuts test-lifted-cover test-presolve-differential test-spp-heuristic test-spp-branch test-benders-master-start test-warmstart-serde test-beaconfd test-beaconfd-trace test-phase1-recovery test-phase1-engine test-phase1-zones test-simplex-policy test-dual-ratio-flip test-lp-refactor-policy test-lp-reinvert-controller test-lp-basis-governor test-lp-basis-governor-runtime test-lp-determinism test-lp-sensitivity test-lp-conflict test-lp-algorithm-api test-lp-policy-glpk-compat test-lp-glpk-strict test-lp-glpk-strict-bfcp test-lp-bfcp-policy test-lp-dispatch test-lp-external-adapter test-lp-external-adapter-api test-lp-external-introspection-api test-lp-external-failure-api test-lp-external-oop-runner test-lp-external-glpk-oop test-lp-external-glpk-oop-integration test-lp-telemetry test-lp-telemetry-solver test-lp-telemetry-lu test-lp-telemetry-gate test-lp-telemetry-lu-sparse test-lp-public-api-split test-mip-public-api-split test-lp-error-api test-lu-markowitz test-lu-supernode test-netlib test-netlib-full test-netlib-gate test-netlib-gate-small test-netlib-gate-stress-fit1p-nesm test-netlib-gate-stress-fit1p-nesm-gating test-netlib-gate-stress-fit1p-nesm-non-gating test-netlib-gate-pilot-focus test-netlib-gate-timeout22 test-netlib-gate-crash6-norelax-noflip test-netlib-gate-control-all-canary phase1-canaries glpk-strict-plan-init glpk-strict-plan-status glpk-strict-plan-next glpk-strict-plan-phase1 glpk-strict-plan-phase2 glpk-strict-plan-phase3 bench bench-lap bench-lap-perf bench-lap-perf-quick bench-netflow compare-glpk bench-glpk bench-glpk-quick bench-mip bench-mip-quick bench-mip-specialized bench-mip-compare profile-bench build-ralph-benchmark download-netlib install uninstall help

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
	@echo "  test-core  - Run Ralph core regression executable set"
	@echo "  test-lp-suite - Run the complete LP regression suite"
	@echo "  test-mip-suite - Run the complete MIP regression suite"
	@echo "  test-regression-suites - Run the combined LP+MIP regression suites"
	@echo "  test-edge  - Run edge case tests"
	@echo "  test-lap   - Run LAP (assignment problem) tests"
	@echo "  test-netflow - Run network flow tests"
	@echo "  test-detect - Run network detection tests"
	@echo "  test-optim  - Run optimization module tests"
	@echo "  test-benders-known-regression - Run Benders known regressions (non-gating)"
	@echo "  test-beaconfd - Run beaconfd numerical-regression test"
	@echo "  test-simplex-policy - Run simplex basis-action policy tests"
	@echo "  test-lp-reinvert-controller - Run LP reinvert-controller module tests"
	@echo "  test-lp-basis-governor - Run LP basis-governor shadow-module tests"
	@echo "  test-lp-basis-governor-runtime - Run LP basis-governor runtime mode tests"
	@echo "  test-lp-determinism - Run LP determinism module tests"
	@echo "  test-lp-sensitivity - Run LP sensitivity/ranging module tests"
	@echo "  test-lp-conflict - Run LP conflict/IIS refinement module tests"
	@echo "  test-lp-algorithm-api - Run LP algorithm/capability API tests"
	@echo "  test-lp-policy-glpk-compat - Run GLPK-compat policy module tests"
	@echo "  test-lp-bfcp-policy - Run BFCP runtime policy module tests"
	@echo "  test-lp-dispatch - Run internal LP backend dispatch module tests"
	@echo "  test-lp-telemetry - Run LP telemetry module tests"
	@echo "  test-lp-public-api-split - Run modular LP public API split tests"
	@echo "  test-mip-public-api-split - Run modular MIP public API split tests"
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
	@echo "  test-netlib-gate-small - NETLIB small-canary gate (27 cases incl. beaconfd)"
	@echo "  test-netlib-gate-stress-fit1p-nesm - fit1p/nesm stress gate (promoted, gating)"
	@echo "  test-netlib-gate-stress-fit1p-nesm-gating - fit1p/nesm stress gate (explicit gating alias)"
	@echo "  test-netlib-gate-stress-fit1p-nesm-non-gating - fit1p/nesm stress diagnostics (optional)"
	@echo "  test-netlib-gate-pilot-focus - NETLIB pilot-family focus gate (non-gating diagnostics)"
	@echo "  test-netlib-gate-timeout22 - NETLIB timeout22 outlier gate (diagnostics baseline)"
	@echo "  test-netlib-gate-crash6-norelax-noflip - NETLIB crash6 explicit norelax/noflip harness (non-gating diagnostics)"
	@echo "  test-netlib-gate-control-all-canary - Full NETLIB control_all canary (non-gating diagnostics)"
	@echo "  glpk-strict-plan-init   - Initialize GLPK-strict ordered phase tracker"
	@echo "  glpk-strict-plan-status - Show GLPK-strict phase tracker status"
	@echo "  glpk-strict-plan-next   - Show next GLPK-strict phase"
	@echo "  glpk-strict-plan-phase1 - Run ordered phase 1 with gates and mark completion"
	@echo "  glpk-strict-plan-phase2 - Run ordered phase 2 with gates and mark completion"
	@echo "  glpk-strict-plan-phase3 - Run ordered phase 3 with gates and mark completion"
	@echo "  phase1-canaries       - Run Phase 1 feasibility canary gate"
	@echo "  download-netlib        - Download all NETLIB problems"
	@echo ""
	@echo "NETLIB Benchmark Tool:"
	@echo "  build-ralph-benchmark  - Build the benchmark tool"
	@echo ""
	@echo "  ./ralph-benchmark --test              # Correctness test (fast)"
	@echo "  ./ralph-benchmark --test full          # Correctness test (all)"
	@echo "  ./ralph-benchmark --suite tiny -v     # GLPK comparison"
	@echo "  ./ralph-benchmark --netlib afiro -v   # Single problem"

-include $(DEPS)
