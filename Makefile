# FuelWise Platform - Top-Level Makefile
#
# Project Structure:
#   ralph/     - LP/MIP Solver (libralph.a)
#   fuelwise/  - Refueling Optimization Library (libfuelwise.a)
#   wasm/      - WebAssembly Build (future)
#   api/       - REST API (future)
#   ui/        - React Application (future)

.PHONY: all clean test test-all ralph fuelwise wasm api ui

# Default: build Ralph and FuelWise
all: ralph fuelwise

# Build Ralph LP/MIP solver
ralph:
	$(MAKE) -C ralph

# Build FuelWise library (depends on Ralph)
fuelwise: ralph
	$(MAKE) -C fuelwise

# Run Ralph tests
test-ralph:
	$(MAKE) -C ralph test

# Run FuelWise tests
test-fuelwise: fuelwise
	$(MAKE) -C fuelwise test

# Run all tests
test: test-ralph test-fuelwise

test-all: test

# Clean all
clean:
	$(MAKE) -C ralph clean
	$(MAKE) -C fuelwise clean

# Build REST API (depends on FuelWise)
api: fuelwise
	$(MAKE) -C api

# Run API server
run-api: api
	$(MAKE) -C api run

# Test API
test-api: api
	$(MAKE) -C api test

# Build WebAssembly module (requires Emscripten)
wasm:
	$(MAKE) -C wasm

wasm-types:
	$(MAKE) -C wasm types

# Future targets
ui:
	@echo "UI build not yet implemented"

# Help
help:
	@echo "FuelWise Platform Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all           - Build Ralph and FuelWise libraries"
	@echo "  ralph         - Build Ralph LP/MIP solver"
	@echo "  fuelwise      - Build FuelWise optimization library"
	@echo "  api           - Build REST API server"
	@echo "  wasm          - Build WebAssembly module (requires Emscripten)"
	@echo "  test          - Run all tests"
	@echo "  test-ralph    - Run Ralph tests"
	@echo "  test-fuelwise - Run FuelWise tests"
	@echo "  test-api      - Test REST API endpoints"
	@echo "  run-api       - Run the REST API server"
	@echo "  clean         - Clean all build artifacts"
	@echo "  help          - Show this help"
