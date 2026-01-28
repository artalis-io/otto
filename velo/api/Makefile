# Velo Route Server Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L

# Include paths
INCLUDES = -I../include -I../../vendor/mongoose -I../../vendor/miniz -Isrc

# Library paths
LDFLAGS = -L.. -lvelo -lm -lpthread -fopenmp

# Source files
SRCS = src/main.c src/polyline.c
OBJS = $(SRCS:src/%.c=build/%.o)

# Mongoose object (shared)
MONGOOSE_OBJ = ../../vendor/mongoose/mongoose.o

TARGET = velo-route-server

.PHONY: all clean deps run debug

all: deps $(TARGET)

deps:
	$(MAKE) -C ..
	@if [ ! -f $(MONGOOSE_OBJ) ]; then \
		$(CC) -c ../../vendor/mongoose/mongoose.c -o $(MONGOOSE_OBJ); \
	fi

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(MONGOOSE_OBJ) $(LDFLAGS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build $(TARGET)

debug: CFLAGS += -g -DDEBUG
debug: clean all

# Run with sample data
run: all
	./$(TARGET) ../../data/hungary-latest.osm.pbf

# Test route
test: all
	@echo "Starting server..."
	@./$(TARGET) -p 8082 ../../data/hungary-latest.osm.pbf &
	@sleep 5
	@echo "Testing health endpoint..."
	@curl -s http://localhost:8082/api/v1/health | head -5
	@echo ""
	@echo "Testing route endpoint..."
	@curl -s 'http://localhost:8082/api/v1/route?from=47.5,19.0&to=46.2,20.1&profile=car&mode=fastest' | head -20
	@pkill -f velo-route-server || true
