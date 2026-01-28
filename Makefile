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
#     wasm/       - WebAssembly build
#   carta/        - Map Tile Generator
#     api/        - Carta Tile Server
#     wasm/       - WebAssembly build
#   shared/       - Shared Utilities (libshared.a)
#   vendor/       - Third-party libraries (mongoose, miniz)
#   docs/         - Architecture documentation

.PHONY: all lib clean test help
.PHONY: ralph fuelwise velo carta shared
.PHONY: api tile-server route-server
.PHONY: wasm wasm-fuelwise wasm-velo wasm-carta wasm-types wasm-test
.PHONY: ui ui-dev
.PHONY: run-api run-tiles run-routes

# =============================================================================
# Default Targets
# =============================================================================

# Build all libraries
all: ralph fuelwise shared velo carta

# Build libraries only (no tests)
lib:
	$(MAKE) -C ralph lib
	$(MAKE) -C fuelwise lib
	$(MAKE) -C shared lib
	$(MAKE) -C velo lib
	$(MAKE) -C carta lib

# =============================================================================
# Core Libraries
# =============================================================================

# Ralph LP/MIP solver (no dependencies)
ralph:
	$(MAKE) -C ralph all

# FuelWise refueling library (depends on Ralph)
fuelwise: ralph
	$(MAKE) -C fuelwise all

# Shared utilities (no dependencies)
shared:
	$(MAKE) -C shared all

# Velo routing engine (uses shared vendor/miniz)
velo:
	$(MAKE) -C velo all

# Carta tile generator (uses shared vendor/miniz)
carta:
	$(MAKE) -C carta all

# =============================================================================
# API Servers
# =============================================================================

# FuelWise REST API server (depends on FuelWise)
api: fuelwise
	$(MAKE) -C fuelwise/api

# Carta tile server (depends on Carta)
tile-server: carta shared
	$(MAKE) -C carta/api

# Velo route server (depends on Velo)
route-server: velo
	$(MAKE) -C velo/api

# Run servers
run-api: api
	$(MAKE) -C fuelwise/api run

run-tiles: tile-server
	@echo "Usage: ./carta/api/carta-tile-server <pbf-file>"
	@echo "Example: ./carta/api/carta-tile-server data/hungary-latest.osm.pbf"

run-routes: route-server
	@echo "Usage: ./velo/api/velo-route-server <pbf-file>"
	@echo "Example: ./velo/api/velo-route-server data/hungary-latest.osm.pbf"

# =============================================================================
# WebAssembly Builds (requires Emscripten)
# =============================================================================

# Build all WASM modules
wasm: wasm-fuelwise wasm-velo wasm-carta

# Individual WASM builds
wasm-fuelwise: fuelwise
	$(MAKE) -C fuelwise/wasm

wasm-velo: velo
	$(MAKE) -C velo/wasm

wasm-carta: carta
	$(MAKE) -C carta/wasm

# Generate TypeScript declarations for all WASM modules
wasm-types:
	$(MAKE) -C fuelwise/wasm types
	$(MAKE) -C velo/wasm types
	$(MAKE) -C carta/wasm types

# Test WASM builds
wasm-test: wasm
	$(MAKE) -C fuelwise/wasm test
	$(MAKE) -C velo/wasm test
	$(MAKE) -C carta/wasm test

# =============================================================================
# UI (requires Node.js)
# =============================================================================

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
	-$(MAKE) -C carta/wasm clean 2>/dev/null || true
	-$(MAKE) -C velo/api clean 2>/dev/null || true
	-$(MAKE) -C velo/wasm clean 2>/dev/null || true
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
	@echo "  all           - Build all libraries with tests (default)"
	@echo "  lib           - Build all libraries only (no tests)"
	@echo "  ralph         - Build Ralph LP/MIP solver"
	@echo "  fuelwise      - Build FuelWise refueling library"
	@echo "  shared        - Build shared utilities library"
	@echo "  velo          - Build Velo routing engine"
	@echo "  carta         - Build Carta tile generator"
	@echo ""
	@echo "API Servers:"
	@echo "  api           - Build FuelWise REST API (fuelwise/api)"
	@echo "  tile-server   - Build Carta tile server (carta/api)"
	@echo "  route-server  - Build Velo route server (velo/api)"
	@echo "  run-api       - Run FuelWise API server"
	@echo "  run-tiles     - Show tile server usage"
	@echo "  run-routes    - Show route server usage"
	@echo ""
	@echo "WebAssembly (requires Emscripten):"
	@echo "  wasm          - Build all WASM modules"
	@echo "  wasm-fuelwise - Build FuelWise WASM"
	@echo "  wasm-velo     - Build Velo WASM"
	@echo "  wasm-carta    - Build Carta WASM"
	@echo "  wasm-types    - Generate TypeScript declarations"
	@echo "  wasm-test     - Test WASM builds"
	@echo ""
	@echo "UI (requires Node.js):"
	@echo "  ui            - Build React UI (fuelwise/ui)"
	@echo "  ui-dev        - Run UI dev server"
	@echo ""
	@echo "Testing:"
	@echo "  test          - Run all tests (~190)"
	@echo "  test-ralph    - Run Ralph tests (65)"
	@echo "  test-fuelwise - Run FuelWise tests (32)"
	@echo "  test-shared   - Run Shared tests (23)"
	@echo "  test-velo     - Run Velo tests (39)"
	@echo "  test-carta    - Run Carta tests (33)"
	@echo "  test-api      - Test FuelWise API endpoints"
	@echo ""
	@echo "Cleanup:"
	@echo "  clean         - Clean all build artifacts"
	@echo "  clean-all     - Clean everything including node_modules"
