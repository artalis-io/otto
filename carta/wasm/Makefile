# Carta WebAssembly Build
#
# Requires Emscripten SDK: https://emscripten.org/docs/getting_started/
# Install: git clone https://github.com/emscripten-core/emsdk.git
#          cd emsdk && ./emsdk install latest && ./emsdk activate latest
#          source ./emsdk_env.sh

CC = emcc
CFLAGS = -O3 -s WASM=1 -s ALLOW_MEMORY_GROWTH=1
CFLAGS += -s EXPORTED_RUNTIME_METHODS='["cwrap","ccall","getValue","setValue","HEAPU8"]'
CFLAGS += -s MODULARIZE=1 -s EXPORT_NAME='Carta'
CFLAGS += -s INITIAL_MEMORY=67108864

# Include paths
INCLUDES = -I../include -I../../vendor/miniz

# Source files
CARTA_SRC = $(wildcard ../src/*.c)
MINIZ_SRC = ../../vendor/miniz/miniz.c ../../vendor/miniz/miniz_tdef.c ../../vendor/miniz/miniz_tinfl.c
WASM_SRC = src/carta_wasm.c

# All source files
ALL_SRC = $(WASM_SRC) $(CARTA_SRC) $(MINIZ_SRC)

# Exported functions
EXPORTS = _wasm_malloc,_wasm_free,_wasm_version
EXPORTS += ,_wasm_load_pbf_memory,_wasm_context_free
EXPORTS += ,_wasm_context_node_count,_wasm_context_way_count
EXPORTS += ,_wasm_context_bbox
EXPORTS += ,_wasm_generate_mvt,_wasm_generate_png
EXPORTS += ,_wasm_tile_bounds

# Output files
OUTPUT_DIR = build
OUTPUT_JS = $(OUTPUT_DIR)/carta.js
OUTPUT_WASM = $(OUTPUT_DIR)/carta.wasm

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
