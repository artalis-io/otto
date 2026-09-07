# FuelWise WebAssembly Build
#
# Requires Emscripten SDK: https://emscripten.org/docs/getting_started/
# Install: git clone https://github.com/emscripten-core/emsdk.git
#          cd emsdk && ./emsdk install latest && ./emsdk activate latest
#          source ./emsdk_env.sh

CC = emcc
CFLAGS = -O3 -s WASM=1 -s ALLOW_MEMORY_GROWTH=1
CFLAGS += -s EXPORTED_RUNTIME_METHODS='["cwrap","ccall","getValue","setValue","allocateUTF8","UTF8ToString"]'
CFLAGS += -s MODULARIZE=1 -s EXPORT_NAME='FuelWise'

# Include paths
INCLUDES = -I../include -I../../ralph/include -I../../shared/include

# Source files
FUELWISE_SRC = $(wildcard ../src/*.c)
RALPH_SRC = $(wildcard ../../ralph/src/*.c) $(wildcard ../../ralph/src/optim/*.c)
# Shared sources needed by Ralph and FuelWise
SHARED_SRC = ../../shared/src/sh_geo.c ../../shared/src/sh_arena.c \
             ../../shared/src/sh_polyline.c ../../shared/src/sh_json.c \
             ../../shared/src/sh_log.c ../../shared/src/sh_perf.c \
             ../../shared/src/sh_pal_posix.c ../../shared/src/sh_pal_windows.c
WASM_SRC = src/fuelwise_wasm.c

# All source files
ALL_SRC = $(WASM_SRC) $(FUELWISE_SRC) $(RALPH_SRC) $(SHARED_SRC)

# Exported functions (low-level API)
EXPORTS = _wasm_malloc,_wasm_free,_wasm_version
EXPORTS += ,_wasm_haversine,_wasm_polyline_length
EXPORTS += ,_wasm_filter_stations
EXPORTS += ,_wasm_solve_simple,_wasm_solve_segments
EXPORTS += ,_wasm_optimize_route

# Output files
OUTPUT_DIR = build
OUTPUT_JS = $(OUTPUT_DIR)/fuelwise.js
OUTPUT_WASM = $(OUTPUT_DIR)/fuelwise.wasm

# API Demo files
API_WASM_SRC = src/fw_wasm_api.c
API_SRC = $(API_WASM_SRC) $(FUELWISE_SRC) $(RALPH_SRC) $(SHARED_SRC)
API_OUTPUT_JS = $(OUTPUT_DIR)/fuelwise-api-demo.js

# API Demo exports
API_EXPORTS = _fuelwise_api_init,_fuelwise_api_free,_fuelwise_api_ready
API_EXPORTS += ,_fuelwise_api_handle
API_EXPORTS += ,_fuelwise_response_status,_fuelwise_response_content_type
API_EXPORTS += ,_fuelwise_response_body,_fuelwise_response_body_len
API_EXPORTS += ,_fuelwise_api_version
API_EXPORTS += ,_wasm_malloc,_wasm_free,_malloc,_free

.PHONY: all clean check-emcc api-demo test-api

all: check-emcc $(OUTPUT_JS)

check-emcc:
	@which emcc > /dev/null || (echo "Error: emcc not found. Install Emscripten SDK first." && exit 1)

$(OUTPUT_JS): $(ALL_SRC)
	@mkdir -p $(OUTPUT_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) \
		-s EXPORTED_FUNCTIONS='[$(EXPORTS)]' \
		$(ALL_SRC) \
		-o $(OUTPUT_JS)
	@echo ""
	@echo "Build complete!"
	@echo "  $(OUTPUT_JS)"
	@echo "  $(OUTPUT_WASM)"
	@echo ""
	@ls -lh $(OUTPUT_DIR)/

# Build with debug info
debug: CFLAGS += -g -s ASSERTIONS=2
debug: $(OUTPUT_JS)

# Build smaller (no assertions, more aggressive optimization)
release: CFLAGS += -Os -s ASSERTIONS=0 --closure 1
release: $(OUTPUT_JS)

# API Demo build (REST-compatible, self-contained for site/api.html)
api-demo: check-emcc $(API_OUTPUT_JS)

$(API_OUTPUT_JS): $(API_SRC)
	@mkdir -p $(OUTPUT_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) \
		-s EXPORT_NAME='FuelWiseAPIDemo' \
		-s SINGLE_FILE=1 \
		-s EXPORTED_FUNCTIONS='[$(API_EXPORTS)]' \
		$(API_SRC) \
		-o $(API_OUTPUT_JS)
	@echo ""
	@echo "API Demo build complete!"
	@echo "  $(API_OUTPUT_JS)"
	@ls -lh $(API_OUTPUT_JS)

# Test API demo with Node.js
test-api: api-demo
	@echo "Testing FuelWise API demo..."
	@node -e " \
		const FuelWiseAPIDemo = require('./$(API_OUTPUT_JS)'); \
		FuelWiseAPIDemo().then(m => { \
			console.log('Init:', m._fuelwise_api_init()); \
			console.log('Ready:', m._fuelwise_api_ready()); \
			const vPtr = m._fuelwise_api_version(); \
			console.log('Version:', m.UTF8ToString(vPtr)); \
			console.log('API Demo test passed!'); \
		}).catch(e => { console.error(e); process.exit(1); }); \
	"

clean:
	rm -rf $(OUTPUT_DIR)

# Generate TypeScript declarations
types:
	@echo "// FuelWise WASM Type Declarations" > $(OUTPUT_DIR)/fuelwise.d.ts
	@echo "// Auto-generated - do not edit" >> $(OUTPUT_DIR)/fuelwise.d.ts
	@echo "" >> $(OUTPUT_DIR)/fuelwise.d.ts
	@echo "export interface FuelWiseModule {" >> $(OUTPUT_DIR)/fuelwise.d.ts
	@echo "  _wasm_malloc(size: number): number;" >> $(OUTPUT_DIR)/fuelwise.d.ts
	@echo "  _wasm_free(ptr: number): void;" >> $(OUTPUT_DIR)/fuelwise.d.ts
	@echo "  _wasm_version(): number;" >> $(OUTPUT_DIR)/fuelwise.d.ts
	@echo "  _wasm_haversine(lat1: number, lon1: number, lat2: number, lon2: number): number;" >> $(OUTPUT_DIR)/fuelwise.d.ts
	@echo "  _wasm_polyline_length(points: number, numPoints: number): number;" >> $(OUTPUT_DIR)/fuelwise.d.ts
	@echo "  _wasm_filter_stations(stations: number, numStations: number, route: number, numPoints: number, maxDistance: number, result: number, resultSize: number): number;" >> $(OUTPUT_DIR)/fuelwise.d.ts
	@echo "  _wasm_solve_simple(stations: number, numStations: number, totalDist: number, tankCap: number, currentFuel: number, mpg: number, minFuel: number, purchases: number, meta: number): number;" >> $(OUTPUT_DIR)/fuelwise.d.ts
	@echo "  _wasm_solve_segments(stations: number, numStations: number, segments: number, numSegments: number, totalDist: number, tankCap: number, currentFuel: number, minFuel: number, minPurchase: number, stopCost: number, purchases: number, meta: number): number;" >> $(OUTPUT_DIR)/fuelwise.d.ts
	@echo "  _wasm_optimize_route(stations: number, numStations: number, route: number, numPoints: number, segments: number, numSegments: number, tankCap: number, currentFuel: number, mpg: number, minFuel: number, maxDist: number, purchases: number, meta: number, stationIds: number): number;" >> $(OUTPUT_DIR)/fuelwise.d.ts
	@echo "  HEAPF64: Float64Array;" >> $(OUTPUT_DIR)/fuelwise.d.ts
	@echo "  HEAP32: Int32Array;" >> $(OUTPUT_DIR)/fuelwise.d.ts
	@echo "  UTF8ToString(ptr: number): string;" >> $(OUTPUT_DIR)/fuelwise.d.ts
	@echo "}" >> $(OUTPUT_DIR)/fuelwise.d.ts
	@echo "" >> $(OUTPUT_DIR)/fuelwise.d.ts
	@echo "export default function FuelWise(): Promise<FuelWiseModule>;" >> $(OUTPUT_DIR)/fuelwise.d.ts
	@echo "Generated $(OUTPUT_DIR)/fuelwise.d.ts"
