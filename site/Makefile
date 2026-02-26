# OTTO Landing Page
#
# Builds deployable site into build/ directory.

.PHONY: all build clean serve serve-dev test test-install

BUILD_DIR = build

# Source files to deploy
STATIC_FILES = index.html api.html style.css
JS_FILES = $(wildcard js/*.js)
WASM_FILES = $(wildcard wasm/*.js)

# Build deployable site
all: build

build:
	@mkdir -p $(BUILD_DIR)/js $(BUILD_DIR)/wasm $(BUILD_DIR)/fonts $(BUILD_DIR)/modules
	@cp index.html style.css api.html $(BUILD_DIR)/
	@cp modules/*.html $(BUILD_DIR)/modules/ 2>/dev/null || true
	@cp js/*.js $(BUILD_DIR)/js/ 2>/dev/null || true
	@cp wasm/*.js $(BUILD_DIR)/wasm/ 2>/dev/null || true
	@cp fonts/*.woff2 $(BUILD_DIR)/fonts/ 2>/dev/null || true
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

# Install test dependencies
test-install:
	@cd tests && npm install

# Run WASM demo tests (requires api.html to be built)
test:
	@echo "Running WASM demo tests..."
	@cd tests && npx playwright test wasm-demos.spec.js
