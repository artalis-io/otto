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
