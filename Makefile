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
#     ui/         - Tile Viewer React Application
#     wasm/       - WebAssembly build
#   locus/        - OSM Geocoding Library (liblocus.a)
#   shared/       - Shared Utilities (libshared.a)
#   vendor/       - Third-party libraries (mongoose, miniz)
#   scripts/      - Utility scripts (build-, ci-, data-, demo-, test-, util-)
#   docs/         - Architecture documentation

.PHONY: all lib clean test help
.PHONY: ralph fuelwise velo carta locus shared arbor surge
.PHONY: fuelwise-api carta-api velo-api
.PHONY: wasm wasm-fuelwise wasm-velo wasm-carta wasm-locus wasm-types wasm-test wasm-api-demos
.PHONY: fuelwise-ui fuelwise-ui-dev carta-ui carta-ui-dev clay-map clay-map-serve site-build site-serve
.PHONY: tui-demo-tty tui-demo-wasm tui-demo-serve tui-wasm test-tui
.PHONY: run-fuelwise-api run-carta-api run-velo-api run-ralph-api
.PHONY: benchmark ci api-docs api-docs-check test-api-docs test-api-docs-install download-monaco
.PHONY: test-fuelwise-regression

# =============================================================================
# Default Targets
# =============================================================================

# Build all libraries
all: shared arbor ralph fuelwise velo carta locus surge

# Build libraries only (no tests)
lib:
	$(MAKE) -C arbor lib
	$(MAKE) -C ralph lib
	$(MAKE) -C fuelwise lib
	$(MAKE) -C shared lib
	$(MAKE) -C velo lib
	$(MAKE) -C carta lib
	$(MAKE) -C locus lib
	$(MAKE) -C surge lib

# =============================================================================
# Core Libraries
# =============================================================================

# Ralph LP/MIP solver (no dependencies)
ralph:
	$(MAKE) -C ralph all

# Arbor search framework (depends on shared for RNG)
arbor: shared
	$(MAKE) -C arbor all

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

# Locus geocoding library (uses shared vendor/miniz)
locus: shared
	$(MAKE) -C locus all

# Surge rich VRPTW/PDPTW solver (depends on Arbor + shared)
surge: arbor shared
	$(MAKE) -C surge all

# =============================================================================
# API Servers
# =============================================================================

# FuelWise REST API server (depends on FuelWise)
fuelwise-api: fuelwise
	$(MAKE) -C fuelwise/api

# Carta tile server (api/Makefile handles deps)
carta-api:
	$(MAKE) -C carta/api

# Velo route server (api/Makefile handles deps)
velo-api:
	$(MAKE) -C velo/api

# Locus geocoding server (api/Makefile handles deps)
locus-api:
	$(MAKE) -C locus/api

# Ralph LP/MIP solver server (api/Makefile handles deps)
ralph-api:
	$(MAKE) -C ralph/api

# Run servers
run-fuelwise-api: fuelwise-api
	$(MAKE) -C fuelwise/api run

run-carta-api: carta-api
	@echo "Usage: ./carta/api/carta-tile-server <pbf-file>"
	@echo "Example: ./carta/api/carta-tile-server data/hungary-latest.osm.pbf"

run-velo-api: velo-api
	@echo "Usage: ./velo/api/velo-route-server <pbf-file>"
	@echo "Example: ./velo/api/velo-route-server data/hungary-latest.osm.pbf"

run-locus-api: locus-api
	@echo "Usage: ./locus/api/locus-geocoder <pbf-file>"
	@echo "Example: ./locus/api/locus-geocoder data/monaco-latest.osm.pbf"

run-ralph-api: ralph-api
	$(MAKE) -C ralph/api run

# =============================================================================
# WebAssembly Builds (requires Emscripten)
# =============================================================================

# Build all WASM modules
wasm: wasm-fuelwise wasm-velo wasm-carta wasm-locus

# Individual WASM builds
wasm-fuelwise: fuelwise
	$(MAKE) -C fuelwise/wasm

wasm-velo: velo
	$(MAKE) -C velo/wasm

wasm-carta: carta
	$(MAKE) -C carta/wasm

wasm-locus: locus
	$(MAKE) -C locus/wasm

# Generate TypeScript declarations for all WASM modules
wasm-types:
	$(MAKE) -C fuelwise/wasm types
	$(MAKE) -C velo/wasm types
	$(MAKE) -C carta/wasm types
	$(MAKE) -C locus/wasm types

# Test WASM builds
wasm-test: wasm
	$(MAKE) -C fuelwise/wasm test
	$(MAKE) -C velo/wasm test
	$(MAKE) -C carta/wasm test
	$(MAKE) -C locus/wasm test

# =============================================================================
# UI (requires Node.js)
# =============================================================================

fuelwise-ui:
	cd fuelwise/ui && npm install && npm run build

fuelwise-ui-dev:
	cd fuelwise/ui && npm run dev

carta-ui:
	cd carta/ui && npm install && npm run build

carta-ui-dev:
	cd carta/ui && npm run dev

# Clay Map Viewer (WASM + Canvas, requires Emscripten)
clay-map:
	$(MAKE) -C clayshards/clay-shards-demo

clay-map-serve: clay-map
	$(MAKE) -C clayshards/clay-shards-demo serve

# Landing Page
site-build:
	$(MAKE) -C site build

site-serve:
	$(MAKE) -C site serve

# ClayShards TUI
tui-demo-tty:
	$(MAKE) -C clayshards/demos/tty

tui-demo-wasm:
	$(MAKE) -C clayshards/demos/wasm

tui-demo-serve: tui-demo-wasm
	@echo "Open http://localhost:8000/clayshards/demos/wasm/demo.html"
	python3 -m http.server 8000

tui-wasm:
	$(MAKE) -C clayshards/clay-shards-tui/wasm

test-tui:
	$(MAKE) -C clayshards/clay-shards-tui test

# =============================================================================
# Scripts
# =============================================================================

benchmark:
	./scripts/test-benchmark.sh

ci:
	./scripts/ci-pipeline.sh

# =============================================================================
# Testing
# =============================================================================

# Run all tests
test: test-arbor test-ralph test-fuelwise test-shared test-velo test-carta test-locus test-surge

test-arbor: shared
	$(MAKE) -C arbor test

test-ralph:
	$(MAKE) -C ralph test
	$(MAKE) -C ralph test-lap
	$(MAKE) -C ralph test-netflow
	$(MAKE) -C ralph test-netlib
	$(MAKE) -C ralph test-detect

test-fuelwise: fuelwise
	$(MAKE) -C fuelwise test

test-fuelwise-regression: fuelwise
	$(MAKE) -C fuelwise test-regression

test-shared: shared
	$(MAKE) -C shared test

test-velo: velo
	$(MAKE) -C velo test

test-carta: carta
	$(MAKE) -C carta test

test-locus: locus
	$(MAKE) -C locus test

test-surge: surge
	$(MAKE) -C surge test

test-fuelwise-api: fuelwise-api
	$(MAKE) -C fuelwise/api test

test-velo-api: velo-api
	$(MAKE) -C velo/api test

test-carta-api: carta-api
	$(MAKE) -C carta/api test

# Test all API endpoints (requires OSM data in data/)
test-api: test-fuelwise-api test-velo-api test-carta-api

# =============================================================================
# Cleanup
# =============================================================================

clean:
	$(MAKE) -C arbor clean
	$(MAKE) -C ralph clean
	$(MAKE) -C fuelwise clean
	$(MAKE) -C shared clean
	$(MAKE) -C velo clean
	$(MAKE) -C carta clean
	$(MAKE) -C locus clean
	$(MAKE) -C surge clean
	-$(MAKE) -C fuelwise/api clean 2>/dev/null || true
	-$(MAKE) -C fuelwise/wasm clean 2>/dev/null || true
	-$(MAKE) -C carta/api clean 2>/dev/null || true
	-$(MAKE) -C carta/wasm clean 2>/dev/null || true
	-$(MAKE) -C velo/api clean 2>/dev/null || true
	-$(MAKE) -C velo/wasm clean 2>/dev/null || true
	-$(MAKE) -C clayshards/clay-shards-demo clean 2>/dev/null || true
	-$(MAKE) -C clayshards/clay-shards-tui clean 2>/dev/null || true
	-$(MAKE) -C clayshards/clay-shards-tui/wasm clean 2>/dev/null || true
	-$(MAKE) -C clayshards/demos/tty clean 2>/dev/null || true
	-$(MAKE) -C clayshards/demos/wasm clean 2>/dev/null || true
	-rm -f vendor/miniz/*.o 2>/dev/null || true

clean-all: clean
	-cd fuelwise/ui && rm -rf node_modules dist 2>/dev/null || true
	-cd carta/ui && rm -rf node_modules dist 2>/dev/null || true

# =============================================================================
# Demo Data (Monaco - small test dataset)
# =============================================================================

# Download Monaco PBF if missing
data/monaco-latest.osm.pbf:
	@echo "Downloading Monaco OSM data..."
	@./scripts/data-download-osm.sh monaco

# Build Velo graph index from PBF
# The graph format may change when velo/ sources change, so rebuild when sources change
VELO_SOURCES = $(wildcard velo/src/*.c) $(wildcard velo/include/*.h)
data/monaco.vlg: data/monaco-latest.osm.pbf $(VELO_SOURCES) | velo
	@echo "Building Velo graph index from PBF..."
	@./velo/bench_pbf data/monaco-latest.osm.pbf data/monaco.vlg
	@echo "Built: data/monaco.vlg"

# Embed Monaco data into WASM headers
velo/wasm/src/monaco_vlg.h: data/monaco.vlg
	@echo "Embedding Monaco graph into WASM header..."
	@xxd -i data/monaco.vlg | sed 's/data_monaco_vlg/monaco_vlg_data/g' > velo/wasm/src/monaco_vlg.h
	@echo "Generated: velo/wasm/src/monaco_vlg.h"

carta/wasm/src/monaco_pbf.h: data/monaco-latest.osm.pbf
	@echo "Embedding Monaco PBF into WASM header..."
	@xxd -i data/monaco-latest.osm.pbf | sed 's/data_monaco_latest_osm_pbf/monaco_pbf_data/g' > carta/wasm/src/monaco_pbf.h
	@echo "Generated: carta/wasm/src/monaco_pbf.h"

# WASM API demos with embedded Monaco data
VELO_WASM_SOURCES = $(wildcard velo/wasm/src/*.c) $(VELO_SOURCES)
velo/wasm/build/velo-api-demo.js: velo/wasm/src/monaco_vlg.h $(VELO_WASM_SOURCES) | shared
	@echo "Building Velo WASM API demo..."
	$(MAKE) -C velo/wasm api-demo

CARTA_SOURCES = $(wildcard carta/src/*.c) $(wildcard carta/include/*.h)
CARTA_WASM_SOURCES = $(wildcard carta/wasm/src/*.c) $(CARTA_SOURCES)
carta/wasm/build/carta-api-demo.js: carta/wasm/src/monaco_pbf.h $(CARTA_WASM_SOURCES) | shared
	@echo "Building Carta WASM API demo..."
	$(MAKE) -C carta/wasm api-demo

LOCUS_SOURCES = $(wildcard locus/src/*.c) $(wildcard locus/include/*.h)
LOCUS_WASM_SOURCES = $(wildcard locus/wasm/src/*.c) $(LOCUS_SOURCES)
locus/wasm/build/locus-api-demo.js: locus/wasm/src/monaco_lcx.h $(LOCUS_WASM_SOURCES) | shared
	@echo "Building Locus WASM API demo..."
	$(MAKE) -C locus/wasm api-demo

FUELWISE_SOURCES = $(wildcard fuelwise/src/*.c) $(wildcard fuelwise/include/*.h)
FUELWISE_WASM_SOURCES = $(wildcard fuelwise/wasm/src/*.c) $(FUELWISE_SOURCES)
fuelwise/wasm/build/fuelwise-api-demo.js: $(FUELWISE_WASM_SOURCES) | shared ralph
	@echo "Building FuelWise WASM API demo..."
	$(MAKE) -C fuelwise/wasm api-demo

# Build all WASM API demos
wasm-api-demos: velo/wasm/build/velo-api-demo.js carta/wasm/build/carta-api-demo.js \
                locus/wasm/build/locus-api-demo.js fuelwise/wasm/build/fuelwise-api-demo.js
	@echo "WASM API demos built successfully"

# Convenience target for downloading Monaco
download-monaco: data/monaco-latest.osm.pbf
	@echo "Monaco data available: data/monaco-latest.osm.pbf"

# =============================================================================
# Documentation
# =============================================================================

# Header files that api-docs depends on
API_HEADERS = $(wildcard carta/include/*.h) $(wildcard velo/include/*.h) \
              $(wildcard locus/include/*.h) $(wildcard fuelwise/include/*.h)

# Generate API documentation from C header annotations
# Depends on: headers (for annotations), WASM demos (copied to site/js/), generator script
site/api.html: scripts/build-api-docs.py site/api-template.html site/api-config.json $(API_HEADERS) \
               velo/wasm/build/velo-api-demo.js carta/wasm/build/carta-api-demo.js \
               locus/wasm/build/locus-api-demo.js fuelwise/wasm/build/fuelwise-api-demo.js
	@echo "Generating API documentation..."
	@python3 scripts/build-api-docs.py
	@echo "Done: site/api.html"

api-docs: site/api.html

# Check API docs are up-to-date (for CI)
api-docs-check:
	@python3 scripts/build-api-docs.py --check

# Test WASM demos in api.html (requires Playwright)
test-api-docs: api-docs
	@$(MAKE) -C site test

# Install test dependencies for api-docs
test-api-docs-install:
	@$(MAKE) -C site test-install
	@cd site/tests && npx playwright install chromium

# Generate PDF from strategy document (for sharing with partners)
# Note: STRATEGY.md contains REDACT markers - strip them before generating public PDF
strategy-pdf:
	@echo "Generating OTTO_Strategy.pdf..."
	@pandoc docs/business/STRATEGY.md \
		-o OTTO_Strategy.pdf \
		--pdf-engine=xelatex \
		-V geometry:margin=1in \
		-V fontsize=11pt \
		-V colorlinks=true \
		-V linkcolor=blue \
		-V urlcolor=blue \
		-V toccolor=black \
		--toc \
		--toc-depth=2 \
		-V toc-title="Table of Contents" \
		--highlight-style=tango \
		-V mainfont="DejaVu Sans" \
		-V monofont="DejaVu Sans Mono" \
		--metadata title="OTTO Strategy Document" \
		--metadata author="Artalis" \
		--metadata date="$(shell date +%Y-%m-%d)"
	@echo "Created: OTTO_Strategy.pdf"

# =============================================================================
# Help
# =============================================================================

help:
	@echo "FuelWise Platform Build System"
	@echo ""
	@echo "Libraries:"
	@echo "  all              - Build all libraries with tests (default)"
	@echo "  lib              - Build all libraries only (no tests)"
	@echo "  ralph            - Build Ralph LP/MIP solver"
	@echo "  arbor            - Build Arbor search/ALNS framework"
	@echo "  fuelwise         - Build FuelWise refueling library"
	@echo "  shared           - Build shared utilities library"
	@echo "  velo             - Build Velo routing engine"
	@echo "  carta            - Build Carta tile generator"
	@echo "  locus            - Build Locus geocoding library"
	@echo "  surge            - Build Surge rich VRPTW/PDPTW solver"
	@echo ""
	@echo "API Servers:"
	@echo "  fuelwise-api     - Build FuelWise REST API (fuelwise/api)"
	@echo "  carta-api        - Build Carta tile server (carta/api)"
	@echo "  velo-api         - Build Velo route server (velo/api)"
	@echo "  run-fuelwise-api - Run FuelWise API server on :8080"
	@echo "  run-carta-api    - Show Carta tile server usage"
	@echo "  run-velo-api     - Show Velo route server usage"
	@echo ""
	@echo "WebAssembly (requires Emscripten):"
	@echo "  wasm             - Build all WASM modules"
	@echo "  wasm-fuelwise    - Build FuelWise WASM"
	@echo "  wasm-velo        - Build Velo WASM"
	@echo "  wasm-carta       - Build Carta WASM"
	@echo "  wasm-types       - Generate TypeScript declarations"
	@echo "  wasm-test        - Test WASM builds"
	@echo ""
	@echo "UI (requires Node.js):"
	@echo "  fuelwise-ui      - Build FuelWise React UI (fuelwise/ui)"
	@echo "  fuelwise-ui-dev  - Run FuelWise UI dev server on :5173"
	@echo "  carta-ui         - Build Carta Tile Viewer (carta/ui)"
	@echo "  carta-ui-dev     - Run Carta UI dev server"
	@echo "  clay-map         - Build Clay Map Viewer WASM (requires Emscripten)"
	@echo "  clay-map-serve   - Build and serve Clay Map on :8000"
	@echo ""
	@echo "ClayShards TUI:"
	@echo "  tui-demo-tty     - Build native TUI demo (runs in terminal)"
	@echo "  tui-demo-wasm    - Build WASM TUI demo (requires Emscripten)"
	@echo "  tui-demo-serve   - Build + serve TUI WebGL demo on :8000"
	@echo "  tui-wasm         - Build TUI WASM library (requires Emscripten)"
	@echo "  test-tui         - Run TUI renderer tests"
	@echo ""
	@echo "Scripts:"
	@echo "  benchmark        - Run performance benchmarks"
	@echo "  ci               - Run CI pipeline"
	@echo ""
	@echo "Demo Data (Monaco):"
	@echo "  data/monaco-latest.osm.pbf  - Download Monaco OSM data"
	@echo "  data/monaco.vlg             - Build Velo graph from PBF"
	@echo "  wasm-api-demos              - Build WASM API demos with Monaco"
	@echo ""
	@echo "Documentation:"
	@echo "  api-docs              - Generate API docs (rebuilds if sources changed)"
	@echo "  api-docs-check        - Check API docs are up-to-date (for CI)"
	@echo "  test-api-docs         - Test WASM demos in api.html (requires Playwright)"
	@echo "  test-api-docs-install - Install Playwright for testing"
	@echo ""
	@echo "Testing:"
	@echo "  test             - Run all library tests"
	@echo "  test-arbor       - Run Arbor tests"
	@echo "  test-ralph       - Run Ralph tests (73)"
	@echo "  test-fuelwise    - Run FuelWise tests with full regression harness"
	@echo "  test-fuelwise-regression - Run FuelWise full regression harness explicitly"
	@echo "  test-shared      - Run Shared tests (41)"
	@echo "  test-velo        - Run Velo tests (47)"
	@echo "  test-carta       - Run Carta tests (33)"
	@echo "  test-locus       - Run Locus tests (29)"
	@echo "  test-surge       - Run Surge tests"
	@echo "  test-api         - Test all API endpoints (requires OSM data)"
	@echo "  test-fuelwise-api- Test FuelWise API endpoints"
	@echo "  test-velo-api    - Test Velo route API endpoints"
	@echo "  test-carta-api   - Test Carta tile API endpoints"
	@echo ""
	@echo "Cleanup:"
	@echo "  clean            - Clean all build artifacts"
	@echo "  clean-all        - Clean everything including node_modules"
