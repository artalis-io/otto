# Velo WebAssembly Build
#
# Requires Emscripten SDK: https://emscripten.org/docs/getting_started/
# Install: git clone https://github.com/emscripten-core/emsdk.git
#          cd emsdk && ./emsdk install latest && ./emsdk activate latest
#          source ./emsdk_env.sh

CC = emcc
CFLAGS = -O3 -s WASM=1 -s ALLOW_MEMORY_GROWTH=1
CFLAGS += -s EXPORTED_RUNTIME_METHODS='["cwrap","ccall","getValue","setValue","UTF8ToString"]'
CFLAGS += -s MODULARIZE=1 -s EXPORT_NAME='Velo'
CFLAGS += -s INITIAL_MEMORY=33554432

# Include paths
INCLUDES = -I../include -I../../shared/include -I../../vendor/miniz

# Source files
VELO_SRC = $(wildcard ../src/*.c)
MINIZ_SRC = ../../vendor/miniz/miniz.c ../../vendor/miniz/miniz_tdef.c ../../vendor/miniz/miniz_tinfl.c
WASM_SRC = src/velo_wasm.c

# Shared library sources needed by Velo
SHARED_CORE_SRC = ../../shared/src/sh_geo.c \
                  ../../shared/src/sh_protobuf.c \
                  ../../shared/src/sh_inflate.c \
                  ../../shared/src/sh_pbf.c \
                  ../../shared/src/sh_arena.c \
                  ../../shared/src/sh_pool.c \
                  ../../shared/src/sh_hashmap.c \
                  ../../shared/src/sh_heap.c \
                  ../../shared/src/sh_pal_posix.c \
                  ../../shared/src/sh_pal_windows.c

# All source files
ALL_SRC = $(WASM_SRC) $(VELO_SRC) $(SHARED_CORE_SRC) $(MINIZ_SRC)

# Exported functions
EXPORTS = _wasm_malloc,_wasm_free,_wasm_version
EXPORTS += ,_wasm_load_graph_memory,_wasm_graph_free
EXPORTS += ,_wasm_graph_node_count,_wasm_graph_edge_count
EXPORTS += ,_wasm_route,_wasm_route_free
EXPORTS += ,_wasm_route_distance,_wasm_route_duration,_wasm_route_node_count
EXPORTS += ,_wasm_route_get_coords,_wasm_nearest_node

# Output files
OUTPUT_DIR = build
OUTPUT_JS = $(OUTPUT_DIR)/velo.js
OUTPUT_WASM = $(OUTPUT_DIR)/velo.wasm

.PHONY: all clean check-emcc debug release types test

all: check-emcc $(OUTPUT_JS)

check-emcc:
	@which emcc > /dev/null || (echo "Error: emcc not found. Install Emscripten SDK first." && exit 1)

$(OUTPUT_JS): $(ALL_SRC)
	@mkdir -p $(OUTPUT_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) \
		-DMINIZ_NO_STDIO -DMINIZ_NO_TIME \
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

clean:
	rm -rf $(OUTPUT_DIR)

# Generate TypeScript declarations
types:
	@mkdir -p $(OUTPUT_DIR)
	@echo "// Velo WASM Type Declarations" > $(OUTPUT_DIR)/velo.d.ts
	@echo "// Auto-generated - do not edit" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "export interface VeloModule {" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "  _wasm_malloc(size: number): number;" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "  _wasm_free(ptr: number): void;" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "  _wasm_version(): number;" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "  _wasm_load_graph_memory(data: number, size: number): number;" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "  _wasm_graph_free(graph: number): void;" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "  _wasm_graph_node_count(graph: number): number;" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "  _wasm_graph_edge_count(graph: number): number;" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "  _wasm_route(graph: number, fromLat: number, fromLon: number, toLat: number, toLon: number, profile: number, mode: number): number;" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "  _wasm_route_free(route: number): void;" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "  _wasm_route_distance(route: number): number;" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "  _wasm_route_duration(route: number): number;" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "  _wasm_route_node_count(route: number): number;" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "  _wasm_route_get_coords(route: number, coords: number): number;" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "  _wasm_nearest_node(graph: number, lat: number, lon: number): number;" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "  HEAPF64: Float64Array;" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "  HEAP32: Int32Array;" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "  HEAPU8: Uint8Array;" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "  UTF8ToString(ptr: number): string;" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "}" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "export default function Velo(): Promise<VeloModule>;" >> $(OUTPUT_DIR)/velo.d.ts
	@echo "Generated $(OUTPUT_DIR)/velo.d.ts"

# Test WASM build
test: $(OUTPUT_JS)
	@echo "Testing Velo WASM build..."
	@node -e "const Velo = require('./$(OUTPUT_JS)'); Velo().then(m => { console.log('Version:', m._wasm_version()); console.log('WASM loaded successfully!'); });"

# =============================================================================
# API Demo Build (self-contained with embedded Monaco graph)
# =============================================================================

# API Demo uses the same shared sources as main build
API_WASM_SRC = src/vl_wasm_api.c
API_ALL_SRC = $(API_WASM_SRC) $(VELO_SRC) $(SHARED_CORE_SRC) $(MINIZ_SRC)

# Exported functions - API demo mode
API_EXPORTS = _velo_api_init,_velo_api_free,_velo_api_ready
API_EXPORTS += ,_velo_api_handle
API_EXPORTS += ,_velo_response_status,_velo_response_content_type
API_EXPORTS += ,_velo_response_body,_velo_response_body_len
API_EXPORTS += ,_velo_api_graph_size,_velo_api_version
API_EXPORTS += ,_velo_api_node_count,_velo_api_edge_count
API_EXPORTS += ,_malloc,_free

# Output files - API demo mode
API_OUTPUT_JS = $(OUTPUT_DIR)/velo-api-demo.js

.PHONY: api-demo embed-monaco test-api

# Build API demo (self-contained with embedded Monaco)
api-demo: check-emcc $(API_OUTPUT_JS)

$(API_OUTPUT_JS): $(API_ALL_SRC) src/monaco_vlg.h
	@mkdir -p $(OUTPUT_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -Isrc \
		-s EXPORT_NAME='VeloAPIDemo' \
		-s SINGLE_FILE=1 \
		-s EXPORTED_RUNTIME_METHODS='["cwrap","ccall","getValue","setValue","UTF8ToString","allocateUTF8"]' \
		-DMINIZ_NO_STDIO -DMINIZ_NO_TIME \
		-s EXPORTED_FUNCTIONS='[$(API_EXPORTS)]' \
		$(API_ALL_SRC) \
		-o $(API_OUTPUT_JS)
	@echo ""
	@echo "API Demo build complete (single-file, works offline)!"
	@echo "  $(API_OUTPUT_JS)"
	@echo ""
	@ls -lh $(OUTPUT_DIR)/velo-api-demo.*

# Test API demo build
test-api: $(API_OUTPUT_JS)
	@echo "Testing Velo API Demo build..."
	@node -e "\
		const VeloAPIDemo = require('./$(API_OUTPUT_JS)'); \
		VeloAPIDemo().then(m => { \
			console.log('Initializing...'); \
			const result = m._velo_api_init(); \
			console.log('Init result:', result); \
			console.log('Graph size:', m._velo_api_graph_size(), 'bytes'); \
			console.log('Node count:', m._velo_api_node_count()); \
			console.log('API ready:', m._velo_api_ready()); \
			console.log('API Demo loaded successfully!'); \
		});"

# Generate embedded graph header from Monaco data
embed-monaco:
	@echo "Generating monaco_vlg.h from data/monaco.vlg..."
	@test -f ../../data/monaco.vlg || (echo "Error: data/monaco.vlg not found. Build it first with: ./velo-route-server --save-index data/monaco.vlg data/monaco-latest.osm.pbf --build-only" && exit 1)
	xxd -i ../../data/monaco.vlg | sed 's/______data_monaco_vlg/monaco_vlg_data/g' > src/monaco_vlg.h
	@echo "Generated src/monaco_vlg.h"
	@wc -c src/monaco_vlg.h
