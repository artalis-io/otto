# Nexus WebAssembly Build
#
# Compiles the full 3-stage document ingestion pipeline to WASM.
# Output is a single JS file with embedded WASM (SINGLE_FILE=1).
#
# Requires Emscripten SDK: https://emscripten.org/docs/getting_started/
# Install: git clone https://github.com/emscripten-core/emsdk.git
#          cd emsdk && ./emsdk install latest && ./emsdk activate latest
#          source ./emsdk_env.sh

CC = emcc
CFLAGS = -O3 -s WASM=1 -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=67108864 -s STACK_SIZE=1048576
CFLAGS += -s MODULARIZE=1 -s EXPORT_NAME='NexusDemo' -s SINGLE_FILE=1
CFLAGS += -s EXPORTED_RUNTIME_METHODS='["cwrap","UTF8ToString","allocateUTF8"]'
CFLAGS += -DMINIZ_NO_STDIO -DMINIZ_NO_TIME

# Include paths
INCLUDES = -I../../nexus/include -I../../shared/include -I../../vendor/miniz

# WASM wrapper
WASM_SRC = src/nx_wasm.c

# Nexus library sources
NEXUS_SRC = ../../nexus/src/nx_ingest.c \
            ../../nexus/src/nx_xlsx.c \
            ../../nexus/src/nx_pdf.c \
            ../../nexus/src/nx_csv.c \
            ../../nexus/src/nx_merge.c \
            ../../nexus/src/nx_xform.c \
            ../../nexus/src/nx_compute.c \
            ../../nexus/src/nx_validate.c \
            ../../nexus/src/nx_slug.c \
            ../../nexus/src/nx_discover.c \
            ../../nexus/src/nx_emit.c

# Shared library sources (what nexus needs)
SHARED_SRC = ../../shared/src/sh_arena.c \
             ../../shared/src/sh_json.c \
             ../../shared/src/sh_xml.c \
             ../../shared/src/sh_csv.c \
             ../../shared/src/sh_geojson.c \
             ../../shared/src/sh_inflate.c \
             ../../shared/src/sh_fs.c \
             ../../shared/src/sh_eov.c \
             ../../shared/src/sh_hash_sha256.c \
             ../../shared/src/sh_pdf2struc.c \
             ../../shared/src/sh_pdf2struc_text.c

# Vendor (miniz for XLSX ZIP reading)
MINIZ_SRC = ../../vendor/miniz/miniz.c \
            ../../vendor/miniz/miniz_zip.c \
            ../../vendor/miniz/miniz_tdef.c \
            ../../vendor/miniz/miniz_tinfl.c

# All source files
ALL_SRC = $(WASM_SRC) $(NEXUS_SRC) $(SHARED_SRC) $(MINIZ_SRC)

# Exported functions
EXPORTS = _nx_wasm_version
EXPORTS += ,_nx_wasm_extract,_nx_wasm_extract_result,_nx_wasm_extract_result_len
EXPORTS += ,_nx_wasm_transform,_nx_wasm_transform_result,_nx_wasm_transform_result_len
EXPORTS += ,_nx_wasm_validate,_nx_wasm_validate_result,_nx_wasm_validate_result_len
EXPORTS += ,_nx_wasm_discover,_nx_wasm_discover_result,_nx_wasm_discover_result_len
EXPORTS += ,_nx_wasm_emit_geojson,_nx_wasm_emit_csv,_nx_wasm_emit_result,_nx_wasm_emit_result_len
EXPORTS += ,_malloc,_free

# Output
OUTPUT_DIR = build
OUTPUT_JS = $(OUTPUT_DIR)/nexus-demo.js

.PHONY: all clean check-emcc test

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
	@echo ""
	@ls -lh $(OUTPUT_DIR)/

clean:
	rm -rf $(OUTPUT_DIR)

# Quick smoke test with Node.js
test: $(OUTPUT_JS)
	@echo "Testing Nexus WASM build..."
	@node -e "const NexusDemo = require('./$(OUTPUT_JS)'); NexusDemo().then(m => { console.log('Version:', m._nx_wasm_version()); console.log('WASM loaded successfully!'); });"
