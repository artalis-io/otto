# FuelWise Platform - Top-Level Makefile
#
# Project Structure:
#   ralph/        - LP/MIP Solver (libralph.a)
#   fuelwise/     - Refueling Optimization Library
#     api/        - FuelWise REST API
#     wasm/       - WebAssembly build
#     ui/         - React Application
#   velo/         - OSM Routing Engine
#     api/        - Velo Route Server
#   carta/        - Map Tile Generator
#     api/        - Carta Tile Server
#   shared/       - Shared Utilities (libshared.a)
#   vendor/       - Third-party libraries (mongoose, miniz)
#   docs/         - Architecture documentation

.PHONY: all clean test ralph fuelwise velo carta shared api wasm ui tile-server route-server help

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

# FuelWise REST API server (depends on FuelWise)
api: fuelwise
	$(MAKE) -C fuelwise/api

# Run API server
run-api: api
	$(MAKE) -C fuelwise/api run

# Carta tile server (depends on Carta)
tile-server: carta shared
	$(MAKE) -C carta/api

# Run tile server
run-tiles: tile-server
	@echo "Usage: ./carta/api/carta-tile-server <pbf-file>"
	@echo "Example: ./carta/api/carta-tile-server data/hungary-latest.osm.pbf"

# Velo route server (depends on Velo)
route-server: velo
	$(MAKE) -C velo/api

# Run route server
run-routes: route-server
	@echo "Usage: ./velo/api/velo-route-server <pbf-file>"
	@echo "Example: ./velo/api/velo-route-server data/hungary-latest.osm.pbf"

# WebAssembly builds (requires Emscripten)
wasm: fuelwise velo carta
	$(MAKE) -C fuelwise/wasm

wasm-types:
	$(MAKE) -C fuelwise/wasm types

# UI (requires Node.js)
ui:
	cd fuelwise/ui && npm install && npm run build

ui-dev:
	cd fuelwise/ui && npm run dev

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
	$(MAKE) -C fuelwise/api test

# =============================================================================
# Cleanup
# =============================================================================

clean:
	$(MAKE) -C ralph clean
	$(MAKE) -C fuelwise clean
	$(MAKE) -C shared clean
	$(MAKE) -C velo clean
	$(MAKE) -C carta clean
	-$(MAKE) -C fuelwise/api clean 2>/dev/null || true
	-$(MAKE) -C fuelwise/wasm clean 2>/dev/null || true
	-$(MAKE) -C carta/api clean 2>/dev/null || true
	-$(MAKE) -C velo/api clean 2>/dev/null || true
	-rm -f vendor/miniz/*.o 2>/dev/null || true

clean-all: clean
	cd fuelwise/ui && rm -rf node_modules dist 2>/dev/null || true

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
	@echo "  api           - Build FuelWise REST API (fuelwise/api)"
	@echo "  tile-server   - Build Carta tile server (carta/api)"
	@echo "  route-server  - Build Velo route server (velo/api)"
	@echo "  wasm          - Build WebAssembly modules (requires Emscripten)"
	@echo "  ui            - Build React UI (fuelwise/ui)"
	@echo "  ui-dev        - Run UI dev server (fuelwise/ui)"
	@echo "  run-api       - Run the FuelWise REST API server"
	@echo "  run-tiles     - Show tile server usage"
	@echo "  run-routes    - Show route server usage"
	@echo ""
	@echo "Testing:"
	@echo "  test          - Run all tests"
	@echo "  test-ralph    - Run Ralph tests (43)"
	@echo "  test-fuelwise - Run FuelWise tests (29)"
	@echo "  test-shared   - Run Shared tests (23)"
	@echo "  test-velo     - Run Velo tests (39)"
	@echo "  test-carta    - Run Carta tests (33)"
	@echo "  test-api      - Test FuelWise API endpoints"
	@echo ""
	@echo "Cleanup:"
	@echo "  clean         - Clean all build artifacts"
	@echo "  clean-all     - Clean everything including node_modules"
