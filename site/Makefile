# OTTO Landing Page
#
# Builds deployable site into build/ directory.

.PHONY: all build clean serve serve-dev

BUILD_DIR = build

# Source files to deploy
STATIC_FILES = index.html api.html style.css
JS_FILES = $(wildcard js/*.js)
WASM_FILES = $(wildcard wasm/*.js)

# Build deployable site
all: build

build:
	@mkdir -p $(BUILD_DIR)/js $(BUILD_DIR)/wasm
	@cp index.html style.css api.html $(BUILD_DIR)/
	@cp js/*.js $(BUILD_DIR)/js/ 2>/dev/null || true
	@cp wasm/*.js $(BUILD_DIR)/wasm/ 2>/dev/null || true
	@echo "Site built in $(BUILD_DIR)/"
	@echo "  Files: $$(find $(BUILD_DIR) -type f | wc -l | tr -d ' ')"

# Development server (serves build/)
serve: build
	@echo "Serving $(BUILD_DIR)/ at http://localhost:8000"
	@cd $(BUILD_DIR) && python3 -m http.server 8000

# Serve source directory (for development with hot reload)
serve-dev:
	@echo "Serving source at http://localhost:8000"
	@python3 -m http.server 8000

# Clean build output
clean:
	rm -rf $(BUILD_DIR)
