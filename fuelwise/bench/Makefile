# FuelWise Benchmark Makefile

CC ?= gcc
CFLAGS = -Wall -Wextra -O3 -std=c11 -D_GNU_SOURCE

# Include paths
INCLUDES = -I../include -I../../shared/include -I../../ralph/include

# OpenMP flags - platform-specific
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    # macOS with Homebrew
    OMP_LDFLAGS = -L/opt/homebrew/opt/libomp/lib -lomp
else
    # Linux - GCC handles OpenMP natively
    OMP_LDFLAGS = -fopenmp
endif

# Library paths (order matters: fuelwise -> ralph -> shared)
LDFLAGS = -L.. -L../../shared -L../../ralph -lfuelwise -lralph -lshared -lm -lpthread $(OMP_LDFLAGS)

# Source files
SRCS = main.c fw_bench.c fw_gen.c fw_validate.c fw_glpk.c
OBJS = $(SRCS:.c=.o)

# Target binary
TARGET = fuelwise-bench
GATE_TARGET = fuelwise-bench-gate
PROFILE_ARTIFACT_DIR ?= /tmp/fuelwise-benchmark-profile
PROFILE_SCENARIO ?= milp75
PROFILE_SEED ?= 123
PROFILE_RUNS ?= 5
PROFILE_STATS_RUNS ?= 1

# Validator test binary
TEST_VALIDATOR = test_validator
TEST_VALIDATOR_OBJS = test_validator.o fw_bench.o fw_gen.o fw_validate.o fw_glpk.o
GATE_OBJS = bench_gate.o fw_bench.o fw_gen.o fw_validate.o fw_glpk.o
RALPH_DIR = ../../ralph

.PHONY: all clean test quick validator-test regression-test
.PHONY: benchmark-gate benchmark-gate-raw
.PHONY: benchmark-profile benchmark-profile-raw

all: $(TARGET)

$(TARGET): $(OBJS) ../libfuelwise.a ../../shared/libshared.a ../../ralph/libralph.a
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@

$(TEST_VALIDATOR): $(TEST_VALIDATOR_OBJS) ../libfuelwise.a ../../shared/libshared.a ../../ralph/libralph.a
	$(CC) $(CFLAGS) $(TEST_VALIDATOR_OBJS) $(LDFLAGS) -o $@

$(GATE_TARGET): $(GATE_OBJS) ../libfuelwise.a ../../shared/libshared.a ../../ralph/libralph.a
	$(CC) $(CFLAGS) $(GATE_OBJS) $(LDFLAGS) -o $@

%.o: %.c fw_bench.h
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Dependencies
fw_bench.o: fw_bench.c fw_bench.h
fw_gen.o: fw_gen.c fw_bench.h
fw_validate.o: fw_validate.c fw_bench.h
fw_glpk.o: fw_glpk.c fw_bench.h
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

# Run the full regression harness: FuelWise validator + Ralph LP/MIP suites
regression-test: $(TEST_VALIDATOR)
	./$(TEST_VALIDATOR)
	$(MAKE) -C $(RALPH_DIR) test-lp-suite
	$(MAKE) -C $(RALPH_DIR) test-mip-suite

benchmark-gate: $(GATE_TARGET)
	./$(GATE_TARGET)

benchmark-gate-raw: $(GATE_TARGET)
	./$(GATE_TARGET) --compare-raw

benchmark-profile: $(TARGET)
	mkdir -p $(PROFILE_ARTIFACT_DIR)
	./$(TARGET) --scenario $(PROFILE_SCENARIO) --runs $(PROFILE_RUNS) --seed $(PROFILE_SEED) --milp --glpk --presolve --json > $(PROFILE_ARTIFACT_DIR)/$(PROFILE_SCENARIO)_seed$(PROFILE_SEED)_runs$(PROFILE_RUNS).json
	FW_MIP_STATS=1 ./$(TARGET) --scenario $(PROFILE_SCENARIO) --runs $(PROFILE_STATS_RUNS) --seed $(PROFILE_SEED) --milp --glpk --presolve > $(PROFILE_ARTIFACT_DIR)/$(PROFILE_SCENARIO)_seed$(PROFILE_SEED)_stats.txt 2>&1
	@echo "Wrote benchmark JSON to $(PROFILE_ARTIFACT_DIR)/$(PROFILE_SCENARIO)_seed$(PROFILE_SEED)_runs$(PROFILE_RUNS).json"
	@echo "Wrote Ralph MIP stats to $(PROFILE_ARTIFACT_DIR)/$(PROFILE_SCENARIO)_seed$(PROFILE_SEED)_stats.txt"

benchmark-profile-raw: $(TARGET)
	mkdir -p $(PROFILE_ARTIFACT_DIR)
	./$(TARGET) --scenario $(PROFILE_SCENARIO) --runs $(PROFILE_RUNS) --seed $(PROFILE_SEED) --milp --glpk --presolve --compare-raw --json > $(PROFILE_ARTIFACT_DIR)/$(PROFILE_SCENARIO)_seed$(PROFILE_SEED)_runs$(PROFILE_RUNS)_raw.json
	FW_MIP_STATS=1 ./$(TARGET) --scenario $(PROFILE_SCENARIO) --runs $(PROFILE_STATS_RUNS) --seed $(PROFILE_SEED) --milp --glpk --presolve --compare-raw > $(PROFILE_ARTIFACT_DIR)/$(PROFILE_SCENARIO)_seed$(PROFILE_SEED)_stats_raw.txt 2>&1
	@echo "Wrote benchmark JSON to $(PROFILE_ARTIFACT_DIR)/$(PROFILE_SCENARIO)_seed$(PROFILE_SEED)_runs$(PROFILE_RUNS)_raw.json"
	@echo "Wrote Ralph MIP stats to $(PROFILE_ARTIFACT_DIR)/$(PROFILE_SCENARIO)_seed$(PROFILE_SEED)_stats_raw.txt"

clean:
	rm -f $(OBJS) $(TEST_VALIDATOR_OBJS) $(GATE_OBJS) $(TARGET) $(TEST_VALIDATOR) $(GATE_TARGET)

help:
	@echo "FuelWise Benchmark"
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build benchmark binary"
	@echo "  test    - Run benchmark (10 runs, all scenarios)"
	@echo "  quick   - Quick test (5 runs, highway)"
	@echo "  regression-test - Run FuelWise validator plus Ralph LP/MIP suites"
	@echo "  benchmark-gate - Run the MILP Ralph-vs-GLPK benchmark gate"
	@echo "  benchmark-gate-raw - Run the raw MILP benchmark gate"
	@echo "  benchmark-profile - Profile one MILP case with Ralph MIP stats"
	@echo "  benchmark-profile-raw - Raw-model variant of benchmark-profile"
	@echo "  clean   - Remove build artifacts"
