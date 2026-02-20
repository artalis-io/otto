# Surge WebAssembly Build
#
# Requires Emscripten SDK: https://emscripten.org/docs/getting_started/
# Install: git clone https://github.com/emscripten-core/emsdk.git
#          cd emsdk && ./emsdk install latest && ./emsdk activate latest
#          source ./emsdk_env.sh

CC = emcc
CFLAGS = -O3 -s WASM=1 -s ALLOW_MEMORY_GROWTH=1
CFLAGS += -s EXPORTED_RUNTIME_METHODS='["cwrap","ccall","getValue","setValue","allocateUTF8","UTF8ToString"]'
CFLAGS += -s MODULARIZE=1 -s EXPORT_NAME='Surge'

# Include paths
INCLUDES = -I../include -I../../arbor/include -I../../shared/include

# Source files
SURGE_SRC = $(wildcard ../src/*.c)
ARBOR_SRC = $(wildcard ../../arbor/src/*.c)
# Shared sources needed by Surge
SHARED_SRC = ../../shared/src/sh_json.c ../../shared/src/sh_arena.c \
             ../../shared/src/sh_hashmap.c
WASM_SRC = src/sg_wasm.c

# All source files (low-level API)
ALL_SRC = $(WASM_SRC) $(SURGE_SRC) $(ARBOR_SRC) $(SHARED_SRC)

# Exported functions (low-level API)
EXPORTS = _wasm_malloc,_wasm_free,_wasm_version
EXPORTS += ,_wasm_sg_create,_wasm_sg_free
EXPORTS += ,_wasm_sg_set_dimension_count,_wasm_sg_set_config
EXPORTS += ,_wasm_sg_add_depot,_wasm_sg_depot_set_location,_wasm_sg_depot_set_time_window
EXPORTS += ,_wasm_sg_add_vehicle,_wasm_sg_vehicle_set_depots
EXPORTS += ,_wasm_sg_vehicle_set_shift_time_window,_wasm_sg_vehicle_set_capacity
EXPORTS += ,_wasm_sg_add_task,_wasm_sg_task_set_location,_wasm_sg_task_set_time_window
EXPORTS += ,_wasm_sg_task_set_service_seconds,_wasm_sg_task_set_demand
EXPORTS += ,_wasm_sg_add_request,_wasm_sg_request_bind_delivery_task
EXPORTS += ,_wasm_sg_request_bind_pickup_delivery_tasks,_wasm_sg_request_set_priority_hint
EXPORTS += ,_wasm_sg_solve
EXPORTS += ,_wasm_sg_get_total_cost,_wasm_sg_get_total_distance
EXPORTS += ,_wasm_sg_get_unassigned,_wasm_sg_get_used_vehicle_count
EXPORTS += ,_wasm_sg_get_request_count

# Output files
OUTPUT_DIR = build
OUTPUT_JS = $(OUTPUT_DIR)/surge.js
OUTPUT_WASM = $(OUTPUT_DIR)/surge.wasm

# API Demo files
API_WASM_SRC = src/sg_wasm_api.c
API_SRC = $(API_WASM_SRC) $(SURGE_SRC) $(ARBOR_SRC) $(SHARED_SRC)
API_OUTPUT_JS = $(OUTPUT_DIR)/surge-api-demo.js

# API Demo exports
API_EXPORTS = _surge_api_init,_surge_api_free,_surge_api_ready
API_EXPORTS += ,_surge_api_handle,_surge_api_solve
API_EXPORTS += ,_surge_response_status,_surge_response_content_type
API_EXPORTS += ,_surge_response_body,_surge_response_body_len
API_EXPORTS += ,_surge_api_version_string
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

# API Demo build (REST-compatible, self-contained)
api-demo: check-emcc $(API_OUTPUT_JS)

$(API_OUTPUT_JS): $(API_SRC)
	@mkdir -p $(OUTPUT_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) \
		-s EXPORT_NAME='SurgeAPIDemo' \
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
	@echo "Testing Surge API demo..."
	@node -e " \
		const SurgeAPIDemo = require('./$(API_OUTPUT_JS)'); \
		SurgeAPIDemo().then(m => { \
			console.log('Init:', m._surge_api_init()); \
			console.log('Ready:', m._surge_api_ready()); \
			const vPtr = m._surge_api_version_string(); \
			console.log('Version:', m.UTF8ToString(vPtr)); \
			console.log('API Demo test passed!'); \
		}).catch(e => { console.error(e); process.exit(1); }); \
	"

clean:
	rm -rf $(OUTPUT_DIR)
