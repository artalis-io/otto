# FuelWise - Truck Refueling Optimization Library
# Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O3 -march=native -ffast-math
CFLAGS_DEBUG = -Wall -Wextra -g -O0 -DDEBUG
INCLUDES = -I./include -I../ralph/include
LDFLAGS = -lm

# Directories
SRC_DIR = src
INCLUDE_DIR = include
TEST_DIR = tests
RALPH_DIR = ../ralph

# Source files
SOURCES = $(SRC_DIR)/fw_geo.c \
          $(SRC_DIR)/fw_route.c \
          $(SRC_DIR)/fw_refuel.c \
          $(SRC_DIR)/fuelwise.c

OBJECTS = $(SOURCES:.c=.o)

# Library
LIB_NAME = libfuelwise.a

# Ralph library
RALPH_LIB = $(RALPH_DIR)/libralph.a

# Test executable
TEST_SOURCES = $(TEST_DIR)/test_fuelwise.c
TEST_EXEC = test_fuelwise

# Default target
all: ralph $(LIB_NAME)

# Build Ralph first
ralph:
	$(MAKE) -C $(RALPH_DIR)

# Build library
$(LIB_NAME): $(OBJECTS)
	ar rcs $@ $^

# Build object files
$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build test executable
$(TEST_EXEC): $(TEST_SOURCES) $(LIB_NAME) $(RALPH_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lfuelwise -L$(RALPH_DIR) -lralph $(LDFLAGS) -o $@

# Debug build
debug: CFLAGS = $(CFLAGS_DEBUG)
debug: clean all

# Run tests
test: $(TEST_EXEC)
	./$(TEST_EXEC)

# Clean
clean:
	rm -f $(OBJECTS) $(LIB_NAME) $(TEST_EXEC)

# Clean all including Ralph
clean-all: clean
	$(MAKE) -C $(RALPH_DIR) clean

# Install headers and library
install: $(LIB_NAME)
	mkdir -p /usr/local/include/fuelwise
	mkdir -p /usr/local/lib
	cp $(INCLUDE_DIR)/*.h /usr/local/include/fuelwise/
	cp $(LIB_NAME) /usr/local/lib/

# Uninstall
uninstall:
	rm -rf /usr/local/include/fuelwise
	rm -f /usr/local/lib/$(LIB_NAME)

.PHONY: all clean clean-all debug test install uninstall ralph
