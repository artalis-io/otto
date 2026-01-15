# FuelWise REST API Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
INCLUDES = -I../include -I../fuelwise/include -I../ralph/include -Imongoose

# Source files
SRC_DIR = src
MONGOOSE_DIR = mongoose

# Libraries
FUELWISE_LIB = ../fuelwise/libfuelwise.a
RALPH_LIB = ../ralph/libralph.a

# Output
TARGET = fuelwise-api

# Source files
SRCS = $(SRC_DIR)/main.c $(MONGOOSE_DIR)/mongoose.c
OBJS = $(SRCS:.c=.o)

# Default target
all: $(TARGET)

# Build the API server
$(TARGET): $(OBJS) $(FUELWISE_LIB) $(RALPH_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $(OBJS) $(FUELWISE_LIB) $(RALPH_LIB) -lm

# Compile source files
$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(MONGOOSE_DIR)/%.o: $(MONGOOSE_DIR)/%.c
	$(CC) $(CFLAGS) -DMG_ENABLE_LINES=0 -c $< -o $@

# Ensure dependencies are built
$(FUELWISE_LIB):
	$(MAKE) -C ../fuelwise

$(RALPH_LIB):
	$(MAKE) -C ../ralph

# Run the server
run: $(TARGET)
	./$(TARGET)

# Run on a specific port
run-port: $(TARGET)
	./$(TARGET) -p $(PORT)

# Test the API endpoints
test: $(TARGET)
	@echo "Starting server for testing..."
	@./$(TARGET) -p 8081 &
	@SERVER_PID=$$!; \
	sleep 1; \
	echo "Testing health endpoint..."; \
	curl -s http://localhost:8081/api/v1/health | head -20; \
	echo ""; \
	echo "Testing solve endpoint..."; \
	curl -s -X POST http://localhost:8081/api/v1/solve \
		-H "Content-Type: application/json" \
		-d '{"total_distance": 500, "tank_capacity": 100, "current_fuel": 20, "consumption_mpg": 10, "minimum_fuel": 10, "stations": [{"id": 1, "distance": 100, "price": 3.50}, {"id": 2, "distance": 250, "price": 3.25}, {"id": 3, "distance": 400, "price": 3.75}]}' | head -30; \
	echo ""; \
	kill $$SERVER_PID 2>/dev/null || true

# Clean build artifacts
clean:
	rm -f $(TARGET) $(SRC_DIR)/*.o $(MONGOOSE_DIR)/*.o

.PHONY: all clean run run-port test
