# Ralph WebAssembly Build
#
# Requires Emscripten SDK: https://emscripten.org/docs/getting_started/
# Install: git clone https://github.com/emscripten-core/emsdk.git
#          cd emsdk && ./emsdk install latest && ./emsdk activate latest
#          source ./emsdk_env.sh

CC = emcc
CFLAGS = -O3 -s WASM=1 -s ALLOW_MEMORY_GROWTH=1
CFLAGS += -s EXPORTED_RUNTIME_METHODS='["cwrap","ccall","getValue","setValue","HEAPU8","UTF8ToString","allocateUTF8","lengthBytesUTF8"]'
CFLAGS += -s MODULARIZE=1
CFLAGS += -s INITIAL_MEMORY=16777216

# Include paths
INCLUDES = -I../include -I../../shared/include

# Source files - Ralph library (all sources including LAP, network, detect)
RALPH_SRC = $(wildcard ../src/*.c) $(wildcard ../src/optim/*.c)

# Source files - Shared library
SHARED_SRC = ../../shared/src/sh_arena.c ../../shared/src/sh_query.c

# =============================================================================
# API Demo build (self-contained)
# =============================================================================

API_WASM_SRC = src/ralph_wasm_api.c
API_ALL_SRC = $(API_WASM_SRC) $(RALPH_SRC) $(SHARED_SRC)

# Exported functions - API demo mode
API_EXPORTS = _ralph_api_init,_ralph_wasm_api_free,_ralph_wasm_api_ready
API_EXPORTS += ,_ralph_wasm_api_handle
API_EXPORTS += ,_ralph_wasm_response_status,_ralph_wasm_response_content_type
API_EXPORTS += ,_ralph_wasm_response_body,_ralph_wasm_response_body_len
API_EXPORTS += ,_ralph_wasm_api_version
API_EXPORTS += ,_malloc,_free

# Output files
OUTPUT_DIR = build
API_OUTPUT_JS = $(OUTPUT_DIR)/ralph-api-demo.js
API_OUTPUT_WASM = $(OUTPUT_DIR)/ralph-api-demo.wasm

.PHONY: all clean check-emcc debug release test api-demo

all: api-demo

check-emcc:
	@which emcc > /dev/null || (echo "Error: emcc not found. Install Emscripten SDK first." && exit 1)

# Build API demo
api-demo: check-emcc $(API_OUTPUT_JS)

$(API_OUTPUT_JS): $(API_ALL_SRC)
	@mkdir -p $(OUTPUT_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) \
		-s EXPORT_NAME='RalphAPIDemo' \
		-s SINGLE_FILE=1 \
		-s EXPORTED_FUNCTIONS='[$(API_EXPORTS)]' \
		$(API_ALL_SRC) \
		-o $(API_OUTPUT_JS)
	@echo ""
	@echo "API Demo build complete (single-file, works offline)!"
	@echo "  $(API_OUTPUT_JS)"
	@echo ""
	@ls -lh $(OUTPUT_DIR)/ralph-api-demo.*

# Build with debug info
debug: CFLAGS += -g -s ASSERTIONS=2
debug: api-demo

# Build smaller (no assertions, more aggressive optimization)
release: CFLAGS += -Os -s ASSERTIONS=0 --closure 1
release: api-demo

clean:
	rm -rf $(OUTPUT_DIR)

# Test WASM build
test: $(API_OUTPUT_JS)
	@echo "Testing Ralph API Demo build..."
	@node -e "\
		const RalphAPIDemo = require('./$(API_OUTPUT_JS)'); \
		RalphAPIDemo().then(m => { \
			console.log('Initializing...'); \
			const result = m._ralph_api_init(); \
			console.log('Init result:', result); \
			console.log('API ready:', m._ralph_wasm_api_ready()); \
			console.log('Version:', m.UTF8ToString(m._ralph_wasm_api_version())); \
			console.log(''); \
			console.log('Testing solve endpoint...'); \
			const method = m.allocateUTF8('POST'); \
			const path = m.allocateUTF8('/api/v1/solve'); \
			const body = m.allocateUTF8(JSON.stringify({ \
				format: 'lp', \
				problem: 'max: 5 x + 3 y\\nsubject to\\nwood: 2 x + 4 y <= 40\\nlabor: 3 x + 2 y <= 24\\nbounds\\nx >= 0\\ny >= 0\\nend' \
			})); \
			const bodyLen = m.lengthBytesUTF8(JSON.stringify({ \
				format: 'lp', \
				problem: 'max: 5 x + 3 y\\nsubject to\\nwood: 2 x + 4 y <= 40\\nlabor: 3 x + 2 y <= 24\\nbounds\\nx >= 0\\ny >= 0\\nend' \
			})); \
			const respPtr = m._ralph_wasm_api_handle(method, path, 0, body, bodyLen); \
			if (respPtr) { \
				const status = m._ralph_wasm_response_status(respPtr); \
				const bodyPtr = m._ralph_wasm_response_body(respPtr); \
				const len = m._ralph_wasm_response_body_len(respPtr); \
				const respBody = m.UTF8ToString(bodyPtr); \
				console.log('Status:', status); \
				console.log('Response:', respBody); \
			} else { \
				console.log('Error: null response'); \
			} \
			m._free(method); \
			m._free(path); \
			m._free(body); \
			m._ralph_wasm_api_free(); \
			console.log(''); \
			console.log('Ralph WASM API Demo loaded successfully!'); \
		});"
