# Locus WebAssembly Build
#
# Requires Emscripten SDK: https://emscripten.org/docs/getting_started/
# Install: git clone https://github.com/emscripten-core/emsdk.git
#          cd emsdk && ./emsdk install latest && ./emsdk activate latest
#          source ./emsdk_env.sh

CC = emcc
CFLAGS = -O3 -s WASM=1 -s ALLOW_MEMORY_GROWTH=1
CFLAGS += -s EXPORTED_RUNTIME_METHODS='["cwrap","ccall","getValue","setValue","UTF8ToString","stringToUTF8","lengthBytesUTF8"]'
CFLAGS += -s MODULARIZE=1 -s EXPORT_NAME='Locus'
CFLAGS += -s INITIAL_MEMORY=67108864

# Include paths
INCLUDES = -I../include -I../../shared/include -I../../vendor/miniz

# Source files
LOCUS_SRC = $(wildcard ../src/*.c)
SHARED_SRC = $(wildcard ../../shared/src/*.c)
MINIZ_SRC = ../../vendor/miniz/miniz.c ../../vendor/miniz/miniz_tdef.c ../../vendor/miniz/miniz_tinfl.c
WASM_SRC = src/locus_wasm.c

# All source files
ALL_SRC = $(WASM_SRC) $(LOCUS_SRC) $(SHARED_SRC) $(MINIZ_SRC)

# Exported functions
EXPORTS = _wasm_malloc,_wasm_free,_wasm_version
EXPORTS += ,_wasm_index_create,_wasm_index_free
EXPORTS += ,_wasm_index_load_pbf_memory,_wasm_index_entity_count,_wasm_index_memory_usage
EXPORTS += ,_wasm_search,_wasm_autocomplete,_wasm_reverse
EXPORTS += ,_wasm_result_count,_wasm_result_get_id,_wasm_result_get_score
EXPORTS += ,_wasm_result_get_name,_wasm_result_get_lat,_wasm_result_get_lon
EXPORTS += ,_wasm_result_free
EXPORTS += ,_wasm_entity_get_name,_wasm_entity_get_lat,_wasm_entity_get_lon

# Output files
OUTPUT_DIR = build
OUTPUT_JS = $(OUTPUT_DIR)/locus.js
OUTPUT_WASM = $(OUTPUT_DIR)/locus.wasm

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
	@echo "// Locus WASM Type Declarations" > $(OUTPUT_DIR)/locus.d.ts
	@echo "// Auto-generated - do not edit" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "export interface LocusModule {" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  // Memory management" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  _wasm_malloc(size: number): number;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  _wasm_free(ptr: number): void;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  _wasm_version(): number;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  // Index management" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  _wasm_index_create(): number;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  _wasm_index_free(index: number): void;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  _wasm_index_load_pbf_memory(index: number, data: number, size: number): number;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  _wasm_index_entity_count(index: number): number;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  _wasm_index_memory_usage(index: number): number;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  // Search" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  _wasm_search(index: number, query: number, limit: number, fuzzy: number): number;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  _wasm_autocomplete(index: number, prefix: number, limit: number): number;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  _wasm_reverse(index: number, lat: number, lon: number, radius: number): number;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  // Result access" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  _wasm_result_count(result: number): number;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  _wasm_result_get_id(result: number, index: number): number;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  _wasm_result_get_score(result: number, index: number): number;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  _wasm_result_get_name(result: number, index: number): number;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  _wasm_result_get_lat(result: number, index: number): number;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  _wasm_result_get_lon(result: number, index: number): number;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  _wasm_result_free(result: number): void;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  // Entity access" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  _wasm_entity_get_name(index: number, entityId: number): number;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  _wasm_entity_get_lat(index: number, entityId: number): number;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  _wasm_entity_get_lon(index: number, entityId: number): number;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  // Emscripten utilities" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  HEAPF64: Float64Array;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  HEAP32: Int32Array;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  HEAPU8: Uint8Array;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  UTF8ToString(ptr: number): string;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  stringToUTF8(str: string, outPtr: number, maxBytesToWrite: number): void;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "  lengthBytesUTF8(str: string): number;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "}" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "export default function Locus(): Promise<LocusModule>;" >> $(OUTPUT_DIR)/locus.d.ts
	@echo "Generated $(OUTPUT_DIR)/locus.d.ts"

# Test WASM build
test: $(OUTPUT_JS)
	@echo "Testing Locus WASM build..."
	@node -e "const Locus = require('./$(OUTPUT_JS)'); Locus().then(m => { console.log('Version:', m._wasm_version()); console.log('WASM loaded successfully!'); });"

# =============================================================================
# API Demo Build (self-contained with embedded Monaco index)
# =============================================================================

# Shared library sources (core only, no pthread-dependent code)
API_SHARED_SRC = ../../shared/src/sh_geo.c \
             ../../shared/src/sh_protobuf.c \
             ../../shared/src/sh_inflate.c \
             ../../shared/src/sh_pbf.c \
             ../../shared/src/sh_arena.c \
             ../../shared/src/sh_pool.c \
             ../../shared/src/sh_hashmap.c \
             ../../shared/src/sh_heap.c

API_WASM_SRC = src/lc_wasm_api.c
API_ALL_SRC = $(API_WASM_SRC) $(LOCUS_SRC) $(API_SHARED_SRC) $(MINIZ_SRC)

# Exported functions - API demo mode
API_EXPORTS = _locus_api_init,_locus_api_free,_locus_api_ready
API_EXPORTS += ,_locus_api_handle
API_EXPORTS += ,_locus_response_status,_locus_response_content_type
API_EXPORTS += ,_locus_response_body,_locus_response_body_len
API_EXPORTS += ,_locus_api_index_size,_locus_api_version
API_EXPORTS += ,_locus_api_entity_count
API_EXPORTS += ,_malloc,_free

# Output files - API demo mode
API_OUTPUT_JS = $(OUTPUT_DIR)/locus-api-demo.js

.PHONY: api-demo embed-monaco test-api

# Build API demo (self-contained with embedded Monaco)
api-demo: check-emcc $(API_OUTPUT_JS)

$(API_OUTPUT_JS): $(API_ALL_SRC) src/monaco_lcx.h
	@mkdir -p $(OUTPUT_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -Isrc \
		-s EXPORT_NAME='LocusAPIDemo' \
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
	@ls -lh $(OUTPUT_DIR)/locus-api-demo.*

# Test API demo build
test-api: $(API_OUTPUT_JS)
	@echo "Testing Locus API Demo build..."
	@node -e "\
		const LocusAPIDemo = require('./$(API_OUTPUT_JS)'); \
		LocusAPIDemo().then(m => { \
			console.log('Initializing...'); \
			const result = m._locus_api_init(); \
			console.log('Init result:', result); \
			console.log('Index size:', m._locus_api_index_size(), 'bytes'); \
			console.log('Entity count:', m._locus_api_entity_count()); \
			console.log('API ready:', m._locus_api_ready()); \
			console.log('API Demo loaded successfully!'); \
		});"

# Generate embedded index header from Monaco data
embed-monaco:
	@echo "Generating monaco_lcx.h from data/monaco.lcx..."
	@test -f ../../data/monaco.lcx || (echo "Error: data/monaco.lcx not found. Build it first with: ./locus-geocoder --build-only -S data/monaco.lcx data/monaco-latest.osm.pbf" && exit 1)
	xxd -i ../../data/monaco.lcx | sed 's/______data_monaco_lcx/monaco_lcx_data/g' > src/monaco_lcx.h
	@echo "Generated src/monaco_lcx.h"
	@wc -c src/monaco_lcx.h
