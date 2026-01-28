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
INCLUDES = -I$(CARTA_DIR)/include \
           -I$(SHARED_DIR)/include \
           -I$(VENDOR_DIR)/mongoose \
           -I$(VENDOR_DIR)/miniz

# Libraries
LIBS = -L$(CARTA_DIR) -lcarta \
       -L$(SHARED_DIR) -lshared \
       -lm

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
