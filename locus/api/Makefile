# Locus API Server Makefile

CC ?= gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE

# Include paths
INCLUDES = -I../include -I../../shared/include -I../../vendor/mongoose -I../../vendor/miniz -Isrc

# Library paths
LOCUS_DIR = ..
SHARED_DIR = ../../shared
MONGOOSE_DIR = ../../vendor/mongoose

# Linker flags
LDFLAGS = -L$(LOCUS_DIR) -llocus -L$(SHARED_DIR) -lshared -lm -lpthread

# Source files
SRCS = src/main.c
MONGOOSE_SRC = $(MONGOOSE_DIR)/mongoose.c

# Output
TARGET = locus-geocoder

.PHONY: all clean deps run test

all: deps $(TARGET)

# Build dependencies
deps:
	$(MAKE) -C $(LOCUS_DIR) lib
	$(MAKE) -C $(SHARED_DIR) lib

# Build server
$(TARGET): $(SRCS) $(MONGOOSE_SRC) $(LOCUS_DIR)/liblocus.a $(SHARED_DIR)/libshared.a
	$(CC) $(CFLAGS) $(INCLUDES) $(SRCS) $(MONGOOSE_SRC) $(LDFLAGS) -o $@

# Run server
run: $(TARGET)
	./$(TARGET)

# Clean
clean:
	rm -f $(TARGET)

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
