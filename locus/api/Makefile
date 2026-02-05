# Locus API Server Makefile

CC ?= gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE

# Directories
SRC_DIR = src
BUILD_DIR = build
LOCUS_DIR = ..
SHARED_DIR = ../../shared
VENDOR_DIR = ../../vendor

# Include paths
INCLUDES = -I$(SRC_DIR) \
           -I$(LOCUS_DIR)/include \
           -I$(SHARED_DIR)/include \
           -I$(VENDOR_DIR)/mongoose \
           -I$(VENDOR_DIR)/miniz

# Libraries
LIBS = -L$(LOCUS_DIR) -llocus \
       -L$(SHARED_DIR) -lshared \
       -lm -lpthread

# Mongoose object (compiled separately)
MONGOOSE_OBJ = $(VENDOR_DIR)/mongoose/mongoose.o

# Shared HTTP server (requires mongoose, compiled here not in libshared)
HTTPSERVER_OBJ = $(BUILD_DIR)/sh_httpserver.o

# Source files
SRCS = $(SRC_DIR)/main.c
OBJS = $(BUILD_DIR)/main.o $(HTTPSERVER_OBJ)

# Output
TARGET = locus-geocoder

.PHONY: all clean deps run test help

all: deps $(TARGET)

# Build dependencies
deps:
	$(MAKE) -C $(LOCUS_DIR) lib
	$(MAKE) -C $(SHARED_DIR) lib
	@# Build mongoose object if needed
	@if [ ! -f $(MONGOOSE_OBJ) ]; then \
		$(CC) $(CFLAGS) -c $(VENDOR_DIR)/mongoose/mongoose.c -o $(MONGOOSE_OBJ); \
	fi

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Compile shared HTTP server (requires mongoose includes)
$(HTTPSERVER_OBJ): $(SHARED_DIR)/src/sh_httpserver.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build server
$(TARGET): $(OBJS) $(MONGOOSE_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

# Run server
run: $(TARGET)
	./$(TARGET)

# Clean
clean:
	rm -rf $(BUILD_DIR) $(TARGET)

# Test API endpoints
test: $(TARGET)
	@echo "Testing Locus API..."
	@echo "Note: Start server first with: make run"
	@echo ""
	@echo "Testing health endpoint:"
	curl -s http://localhost:8083/api/v1/health | head -c 200
	@echo ""
	@echo ""
	@echo "Testing stats endpoint:"
	curl -s http://localhost:8083/api/v1/stats | head -c 500
	@echo ""

# Help
help:
	@echo "Locus API Server"
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build server (default)"
	@echo "  run     - Run server"
	@echo "  test    - Test endpoints"
	@echo "  clean   - Clean build"
	@echo ""
	@echo "Usage:"
	@echo "  ./locus-geocoder <pbf-file>"
	@echo "  ./locus-geocoder -p 8083 data/monaco-latest.osm.pbf"
