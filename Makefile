# FuelWise Platform - Top-Level Makefile
#
# Project Structure:
#   ralph/     - LP/MIP Solver (libralph.a)
#   fuelwise/  - Refueling Optimization Library (libfuelwise.a)
#   velo/      - OSM Routing Engine (libvelo.a)
#   carta/     - Map Tile Generator (libcarta.a)
#   shared/    - Shared Utilities (libshared.a)
#   vendor/    - Third-party libraries (miniz)
#   api/       - REST API Server
#   wasm/      - WebAssembly Builds
#   ui/        - React Application

.PHONY: all clean test ralph fuelwise velo carta shared api wasm ui help

# Default: build all libraries
all: ralph fuelwise shared velo carta

# =============================================================================
# Core Libraries
# =============================================================================

# Ralph LP/MIP solver (no dependencies)
ralph:
	$(MAKE) -C ralph

# FuelWise refueling library (depends on Ralph)
fuelwise: ralph
	$(MAKE) -C fuelwise

# Shared utilities (no dependencies)
shared:
	$(MAKE) -C shared

# Velo routing engine (uses shared vendor/miniz)
velo:
	$(MAKE) -C velo

# Carta tile generator (uses shared vendor/miniz)
carta:
	$(MAKE) -C carta

# =============================================================================
# Applications
# =============================================================================

# REST API server (depends on FuelWise)
api: fuelwise
	$(MAKE) -C api

# Run API server
run-api: api
	$(MAKE) -C api run

# WebAssembly builds (requires Emscripten)
wasm: fuelwise velo carta
	$(MAKE) -C wasm

wasm-types:
	$(MAKE) -C wasm types

# UI (requires Node.js)
ui:
	cd ui && npm install && npm run build

ui-dev:
	cd ui && npm run dev

# =============================================================================
# Testing
# =============================================================================

# Run all tests
test: test-ralph test-fuelwise test-shared test-velo test-carta

test-ralph:
	$(MAKE) -C ralph test

test-fuelwise: fuelwise
	$(MAKE) -C fuelwise test

test-shared: shared
	$(MAKE) -C shared test

test-velo: velo
	$(MAKE) -C velo test

test-carta: carta
	$(MAKE) -C carta test

test-api: api
	$(MAKE) -C api test

# =============================================================================
# Cleanup
# =============================================================================

clean:
	$(MAKE) -C ralph clean
	$(MAKE) -C fuelwise clean
	$(MAKE) -C shared clean
	$(MAKE) -C velo clean
	$(MAKE) -C carta clean
	-$(MAKE) -C api clean 2>/dev/null || true
	-$(MAKE) -C wasm clean 2>/dev/null || true
	-rm -f vendor/miniz/*.o 2>/dev/null || true

clean-all: clean
	cd ui && rm -rf node_modules dist 2>/dev/null || true

# =============================================================================
# Help
# =============================================================================

help:
	@echo "FuelWise Platform Build System"
	@echo ""
	@echo "Libraries:"
	@echo "  all           - Build all C libraries (ralph, fuelwise, shared, velo, carta)"
	@echo "  ralph         - Build Ralph LP/MIP solver"
	@echo "  fuelwise      - Build FuelWise refueling library"
	@echo "  shared        - Build shared utilities library"
	@echo "  velo          - Build Velo routing engine"
	@echo "  carta         - Build Carta tile generator"
	@echo ""
	@echo "Applications:"
	@echo "  api           - Build REST API server"
	@echo "  wasm          - Build WebAssembly modules (requires Emscripten)"
	@echo "  ui            - Build React UI (requires Node.js)"
	@echo "  ui-dev        - Run UI dev server"
	@echo "  run-api       - Run the REST API server"
	@echo ""
	@echo "Testing:"
	@echo "  test          - Run all tests"
	@echo "  test-ralph    - Run Ralph tests (43)"
	@echo "  test-fuelwise - Run FuelWise tests (29)"
	@echo "  test-shared   - Run Shared tests (23)"
	@echo "  test-velo     - Run Velo tests (39)"
	@echo "  test-carta    - Run Carta tests (33)"
	@echo "  test-api      - Test REST API endpoints"
	@echo ""
	@echo "Cleanup:"
	@echo "  clean         - Clean all build artifacts"
	@echo "  clean-all     - Clean everything including node_modules"
