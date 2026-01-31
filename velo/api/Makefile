# Velo Route Server Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L

# Include paths
INCLUDES = -I../include -I../../shared/include -I../../vendor/mongoose -I../../vendor/miniz -Isrc

# Shared library path
SHARED_DIR = ../../shared

# Library paths
LDFLAGS = -L.. -lvelo -L$(SHARED_DIR) -lshared -lm -lpthread -fopenmp

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
	$(MAKE) -C $(SHARED_DIR)
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

# Test the API endpoints
test: all
	@echo "Starting Velo route server for testing..."
	@./$(TARGET) -p 8084 ../../data/hungary-latest.osm.pbf &
	@SERVER_PID=$$!; \
	echo "Waiting for server to load graph and landmarks (this may take a moment)..."; \
	sleep 20; \
	echo ""; \
	echo "=== Testing Velo Route Server API ==="; \
	echo ""; \
	echo "Testing health endpoint..."; \
	HEALTH=$$(curl -s http://localhost:8084/api/v1/health); \
	echo "$$HEALTH" | head -5; \
	if echo "$$HEALTH" | grep -q '"status"'; then \
		echo "  PASS: Health endpoint works"; \
	else \
		echo "  FAIL: Health endpoint"; \
	fi; \
	echo ""; \
	echo "Testing stats endpoint..."; \
	STATS=$$(curl -s http://localhost:8084/api/v1/stats); \
	echo "$$STATS" | head -10; \
	if echo "$$STATS" | grep -q '"num_nodes"'; then \
		echo "  PASS: Stats endpoint works"; \
	else \
		echo "  FAIL: Stats endpoint"; \
	fi; \
	echo ""; \
	echo "Testing route endpoint (Budapest to Szeged)..."; \
	ROUTE=$$(curl -s 'http://localhost:8084/api/v1/route?from=47.497,19.040&to=46.253,20.148&profile=car&mode=fastest'); \
	echo "$$ROUTE" | head -15; \
	if echo "$$ROUTE" | grep -q '"distance"'; then \
		echo "  PASS: Route endpoint works"; \
	else \
		echo "  FAIL: Route endpoint"; \
	fi; \
	echo ""; \
	echo "Testing POST route endpoint (Budapest to Gyor)..."; \
	ROUTE_POST=$$(curl -s -X POST http://localhost:8084/api/v1/route \
		-H "Content-Type: application/json" \
		-d '{"from":"47.497,19.040","to":"47.687,17.635","profile":"truck","mode":"shortest"}'); \
	echo "$$ROUTE_POST" | head -15; \
	if echo "$$ROUTE_POST" | grep -q '"distance"'; then \
		echo "  PASS: POST route endpoint works"; \
	else \
		echo "  FAIL: POST route endpoint"; \
	fi; \
	echo ""; \
	echo "Testing route with different profiles..."; \
	echo "  Bike profile:"; \
	BIKE=$$(curl -s 'http://localhost:8084/api/v1/route?from=47.497,19.040&to=47.48,19.06&profile=bike&mode=shortest'); \
	if echo "$$BIKE" | grep -q '"distance"'; then \
		echo "    PASS: Bike route works"; \
	else \
		echo "    FAIL: Bike route"; \
	fi; \
	echo "  Foot profile:"; \
	FOOT=$$(curl -s 'http://localhost:8084/api/v1/route?from=47.497,19.040&to=47.48,19.06&profile=foot&mode=shortest'); \
	if echo "$$FOOT" | grep -q '"distance"'; then \
		echo "    PASS: Foot route works"; \
	else \
		echo "    FAIL: Foot route"; \
	fi; \
	echo ""; \
	echo "=== Velo API Tests Complete ==="; \
	kill $$SERVER_PID 2>/dev/null || true
