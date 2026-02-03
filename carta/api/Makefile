# Carta Tile Server Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L
DEBUG_CFLAGS = -Wall -Wextra -g -O0 -std=c11 -D_POSIX_C_SOURCE=200809L -DDEBUG

# Directories
SRC_DIR = src
BUILD_DIR = build
CARTA_DIR = ..
VENDOR_DIR = ../../vendor
SHARED_DIR = ../../shared

# Include paths
INCLUDES = -I$(SRC_DIR) \
           -I$(CARTA_DIR)/include \
           -I$(SHARED_DIR)/include \
           -I$(VENDOR_DIR)/mongoose \
           -I$(VENDOR_DIR)/miniz

# Libraries
LIBS = -L$(CARTA_DIR) -lcarta \
       -L$(SHARED_DIR) -lshared \
       -lm -lpthread

# Mongoose and miniz objects
VENDOR_OBJS = $(VENDOR_DIR)/mongoose/mongoose.o \
              $(VENDOR_DIR)/miniz/miniz.o \
              $(VENDOR_DIR)/miniz/miniz_tdef.o \
              $(VENDOR_DIR)/miniz/miniz_tinfl.o

# Source files
SRCS = $(SRC_DIR)/main.c
OBJS = $(BUILD_DIR)/main.o

# Target
TARGET = carta-tile-server

.PHONY: all clean debug deps run

all: deps $(TARGET)

# Build dependencies first
deps:
	$(MAKE) -C $(CARTA_DIR)
	$(MAKE) -C $(SHARED_DIR)
	@# Build vendor objects if needed
	@if [ ! -f $(VENDOR_DIR)/mongoose/mongoose.o ]; then \
		$(CC) $(CFLAGS) -c $(VENDOR_DIR)/mongoose/mongoose.c -o $(VENDOR_DIR)/mongoose/mongoose.o; \
	fi
	@if [ ! -f $(VENDOR_DIR)/miniz/miniz.o ]; then \
		$(MAKE) -C $(VENDOR_DIR)/miniz; \
	fi

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(TARGET): $(OBJS) $(VENDOR_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

debug: CFLAGS = $(DEBUG_CFLAGS)
debug: clean deps $(TARGET)

# Run with a sample PBF
run: $(TARGET)
	./$(TARGET) -s static -p 8081 ../../data/hungary-latest.osm.pbf

# Create static directory if it doesn't exist
static:
	mkdir -p static

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

# Install target
install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

# Docker build
docker:
	docker build -t carta-tile-server .

# Test the API endpoints
test: $(TARGET)
	@echo "Starting Carta tile server for testing..."
	@./$(TARGET) -p 8083 ../../data/hungary-latest.osm.pbf &
	@SERVER_PID=$$!; \
	echo "Waiting for server to load PBF (this may take a moment)..."; \
	sleep 15; \
	echo ""; \
	echo "=== Testing Carta Tile Server API ==="; \
	echo ""; \
	echo "Testing health endpoint..."; \
	HEALTH=$$(curl -s http://localhost:8083/api/v1/health); \
	echo "$$HEALTH" | head -5; \
	if echo "$$HEALTH" | grep -q '"status"'; then \
		echo "  PASS: Health endpoint works"; \
	else \
		echo "  FAIL: Health endpoint"; \
	fi; \
	echo ""; \
	echo "Testing stats endpoint..."; \
	STATS=$$(curl -s http://localhost:8083/api/v1/stats); \
	echo "$$STATS" | head -10; \
	if echo "$$STATS" | grep -q '"total_nodes"'; then \
		echo "  PASS: Stats endpoint works"; \
	else \
		echo "  FAIL: Stats endpoint"; \
	fi; \
	echo ""; \
	echo "Testing tiles.json endpoint..."; \
	TILEJSON=$$(curl -s http://localhost:8083/tiles.json); \
	echo "$$TILEJSON" | head -10; \
	if echo "$$TILEJSON" | grep -q '"tilejson"'; then \
		echo "  PASS: TileJSON endpoint works"; \
	else \
		echo "  FAIL: TileJSON endpoint"; \
	fi; \
	echo ""; \
	echo "Testing PNG tile endpoint (z=10, x=567, y=357)..."; \
	HTTP_CODE=$$(curl -s -o /dev/null -w "%{http_code}" http://localhost:8083/tiles/10/567/357.png); \
	if [ "$$HTTP_CODE" = "200" ]; then \
		echo "  PASS: PNG tile returned 200 OK"; \
	else \
		echo "  FAIL: PNG tile returned $$HTTP_CODE"; \
	fi; \
	echo ""; \
	echo "Testing MVT tile endpoint (z=10, x=567, y=357)..."; \
	HTTP_CODE=$$(curl -s -o /dev/null -w "%{http_code}" http://localhost:8083/tiles/10/567/357.mvt); \
	if [ "$$HTTP_CODE" = "200" ]; then \
		echo "  PASS: MVT tile returned 200 OK"; \
	else \
		echo "  FAIL: MVT tile returned $$HTTP_CODE"; \
	fi; \
	echo ""; \
	echo "=== Carta API Tests Complete ==="; \
	kill $$SERVER_PID 2>/dev/null || true
