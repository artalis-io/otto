# Carta WebAssembly Build
#
# Requires Emscripten SDK: https://emscripten.org/docs/getting_started/
# Install: git clone https://github.com/emscripten-core/emsdk.git
#          cd emsdk && ./emsdk install latest && ./emsdk activate latest
#          source ./emsdk_env.sh

CC = emcc
CFLAGS = -O3 -s WASM=1 -s ALLOW_MEMORY_GROWTH=1
CFLAGS += -s EXPORTED_RUNTIME_METHODS='["cwrap","ccall","getValue","setValue","HEAPU8","UTF8ToString","allocateUTF8"]'
CFLAGS += -s MODULARIZE=1
CFLAGS += -s INITIAL_MEMORY=67108864

# Include paths
INCLUDES = -I../include -I../../shared/include -I../../vendor/miniz

# Source files - Carta library
CARTA_SRC = $(wildcard ../src/*.c)

# Source files - Shared library (core only, no pthread-dependent code)
SHARED_SRC = ../../shared/src/sh_geo.c \
             ../../shared/src/sh_protobuf.c \
             ../../shared/src/sh_inflate.c \
             ../../shared/src/sh_pbf.c \
             ../../shared/src/sh_font.c \
             ../../shared/src/sh_font_data.c \
             ../../shared/src/sh_arena.c \
             ../../shared/src/sh_pool.c \
             ../../shared/src/sh_hashmap.c \
             ../../shared/src/sh_heap.c \
             ../../shared/src/sh_spatial_grid.c

# Source files - miniz
MINIZ_SRC = ../../vendor/miniz/miniz.c ../../vendor/miniz/miniz_tdef.c ../../vendor/miniz/miniz_tinfl.c

# =============================================================================
# Standard WASM build (library mode - user provides PBF)
# =============================================================================

WASM_SRC = src/carta_wasm.c
ALL_SRC = $(WASM_SRC) $(CARTA_SRC) $(SHARED_SRC) $(MINIZ_SRC)

# Exported functions - library mode
EXPORTS = _wasm_malloc,_wasm_free,_wasm_version
EXPORTS += ,_wasm_load_pbf_memory,_wasm_context_free
EXPORTS += ,_wasm_context_node_count,_wasm_context_way_count
EXPORTS += ,_wasm_context_bbox
EXPORTS += ,_wasm_generate_mvt,_wasm_generate_png
EXPORTS += ,_wasm_tile_bounds

# Output files - library mode
OUTPUT_DIR = build
OUTPUT_JS = $(OUTPUT_DIR)/carta.js
OUTPUT_WASM = $(OUTPUT_DIR)/carta.wasm

# =============================================================================
# API Demo build (self-contained with embedded Monaco PBF)
# =============================================================================

API_WASM_SRC = src/ct_wasm_api.c
API_ALL_SRC = $(API_WASM_SRC) $(CARTA_SRC) $(SHARED_SRC) $(MINIZ_SRC)

# Exported functions - API demo mode
API_EXPORTS = _carta_api_init,_carta_api_free,_carta_api_ready
API_EXPORTS += ,_carta_api_handle
API_EXPORTS += ,_carta_response_status,_carta_response_content_type
API_EXPORTS += ,_carta_response_body,_carta_response_body_len
API_EXPORTS += ,_carta_api_pbf_size,_carta_api_version
API_EXPORTS += ,_malloc,_free

# Output files - API demo mode
API_OUTPUT_JS = $(OUTPUT_DIR)/carta-api-demo.js
API_OUTPUT_WASM = $(OUTPUT_DIR)/carta-api-demo.wasm

.PHONY: all clean check-emcc debug release types test api-demo

all: check-emcc $(OUTPUT_JS)

check-emcc:
	@which emcc > /dev/null || (echo "Error: emcc not found. Install Emscripten SDK first." && exit 1)

$(OUTPUT_JS): $(ALL_SRC)
	@mkdir -p $(OUTPUT_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) \
		-s EXPORT_NAME='Carta' \
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
	@echo "// Carta WASM Type Declarations" > $(OUTPUT_DIR)/carta.d.ts
	@echo "// Auto-generated - do not edit" >> $(OUTPUT_DIR)/carta.d.ts
	@echo "" >> $(OUTPUT_DIR)/carta.d.ts
	@echo "export interface CartaModule {" >> $(OUTPUT_DIR)/carta.d.ts
	@echo "  _wasm_malloc(size: number): number;" >> $(OUTPUT_DIR)/carta.d.ts
	@echo "  _wasm_free(ptr: number): void;" >> $(OUTPUT_DIR)/carta.d.ts
	@echo "  _wasm_version(): number;" >> $(OUTPUT_DIR)/carta.d.ts
	@echo "  _wasm_load_pbf_memory(data: number, size: number): number;" >> $(OUTPUT_DIR)/carta.d.ts
	@echo "  _wasm_context_free(ctx: number): void;" >> $(OUTPUT_DIR)/carta.d.ts
	@echo "  _wasm_context_node_count(ctx: number): number;" >> $(OUTPUT_DIR)/carta.d.ts
	@echo "  _wasm_context_way_count(ctx: number): number;" >> $(OUTPUT_DIR)/carta.d.ts
	@echo "  _wasm_context_bbox(ctx: number, bbox: number): void;" >> $(OUTPUT_DIR)/carta.d.ts
	@echo "  _wasm_generate_mvt(ctx: number, z: number, x: number, y: number, buffer: number, capacity: number): number;" >> $(OUTPUT_DIR)/carta.d.ts
	@echo "  _wasm_generate_png(ctx: number, z: number, x: number, y: number, size: number, buffer: number, capacity: number): number;" >> $(OUTPUT_DIR)/carta.d.ts
	@echo "  _wasm_tile_bounds(z: number, x: number, y: number, bounds: number): void;" >> $(OUTPUT_DIR)/carta.d.ts
	@echo "  HEAPF64: Float64Array;" >> $(OUTPUT_DIR)/carta.d.ts
	@echo "  HEAP32: Int32Array;" >> $(OUTPUT_DIR)/carta.d.ts
	@echo "  HEAPU8: Uint8Array;" >> $(OUTPUT_DIR)/carta.d.ts
	@echo "  UTF8ToString(ptr: number): string;" >> $(OUTPUT_DIR)/carta.d.ts
	@echo "}" >> $(OUTPUT_DIR)/carta.d.ts
	@echo "" >> $(OUTPUT_DIR)/carta.d.ts
	@echo "export default function Carta(): Promise<CartaModule>;" >> $(OUTPUT_DIR)/carta.d.ts
	@echo "Generated $(OUTPUT_DIR)/carta.d.ts"

# Test WASM build
test: $(OUTPUT_JS)
	@echo "Testing Carta WASM build..."
	@node -e "const Carta = require('./$(OUTPUT_JS)'); Carta().then(m => { console.log('Version:', m._wasm_version()); console.log('WASM loaded successfully!'); });"

# =============================================================================
# API Demo Build
# =============================================================================

# Build API demo (self-contained with embedded Monaco)
api-demo: check-emcc $(API_OUTPUT_JS)

$(API_OUTPUT_JS): $(API_ALL_SRC) src/monaco_pbf.h
	@mkdir -p $(OUTPUT_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -Isrc \
		-s EXPORT_NAME='CartaAPIDemo' \
		-s SINGLE_FILE=1 \
		-DMINIZ_NO_STDIO -DMINIZ_NO_TIME \
		-s EXPORTED_FUNCTIONS='[$(API_EXPORTS)]' \
		$(API_ALL_SRC) \
		-o $(API_OUTPUT_JS)
	@echo ""
	@echo "API Demo build complete (single-file, works offline)!"
	@echo "  $(API_OUTPUT_JS)"
	@echo ""
	@ls -lh $(OUTPUT_DIR)/carta-api-demo.*

# Test API demo build
test-api: $(API_OUTPUT_JS)
	@echo "Testing Carta API Demo build..."
	@node -e "\
		const CartaAPIDemo = require('./$(API_OUTPUT_JS)'); \
		CartaAPIDemo().then(m => { \
			console.log('Initializing...'); \
			const result = m._carta_api_init(); \
			console.log('Init result:', result); \
			console.log('PBF size:', m._carta_api_pbf_size(), 'bytes'); \
			console.log('API ready:', m._carta_api_ready()); \
			console.log('API Demo loaded successfully!'); \
		});"

# Generate embedded PBF header from Monaco data
embed-monaco:
	@echo "Generating monaco_pbf.h from data/monaco-latest.osm.pbf..."
	xxd -i ../../data/monaco-latest.osm.pbf | sed 's/____data_monaco_latest_osm_pbf/monaco_pbf_data/g; s/data_monaco_latest_osm_pbf/monaco_pbf_data/g' > src/monaco_pbf.h
	@echo "Generated src/monaco_pbf.h"
	@wc -c src/monaco_pbf.h
