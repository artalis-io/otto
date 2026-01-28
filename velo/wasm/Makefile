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
INCLUDES = -I../include -I../../vendor/miniz

# Source files
VELO_SRC = $(wildcard ../src/*.c)
MINIZ_SRC = ../../vendor/miniz/miniz.c ../../vendor/miniz/miniz_tdef.c ../../vendor/miniz/miniz_tinfl.c
WASM_SRC = src/velo_wasm.c

# All source files
ALL_SRC = $(WASM_SRC) $(VELO_SRC) $(MINIZ_SRC)

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
