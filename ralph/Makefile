# Ralph LP/MIP Solver Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O3 -march=native -ffast-math
CFLAGS_DEBUG = -Wall -Wextra -g -O0 -DDEBUG
INCLUDES = -I./include
LDFLAGS = -lm

# Directories
SRC_DIR = src
INCLUDE_DIR = include
TEST_DIR = tests

# Source files
SOURCES = $(SRC_DIR)/sparse.c \
          $(SRC_DIR)/model.c \
          $(SRC_DIR)/lu.c \
          $(SRC_DIR)/lu_sparse.c \
          $(SRC_DIR)/simplex.c \
          $(SRC_DIR)/dual_simplex.c \
          $(SRC_DIR)/presolve.c \
          $(SRC_DIR)/branch_bound.c \
          $(SRC_DIR)/cuts.c \
          $(SRC_DIR)/mip.c \
          $(SRC_DIR)/mps_reader.c \
          $(SRC_DIR)/ralph.c

OBJECTS = $(SOURCES:.c=.o)

# Library
LIB_NAME = libralph.a

# Test executable
TEST_SOURCES = $(TEST_DIR)/test_main.c
TEST_EXEC = test_ralph

# Default target
all: $(LIB_NAME)

# Build library
$(LIB_NAME): $(OBJECTS)
	ar rcs $@ $^

# Build object files
$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Build test executable
$(TEST_EXEC): $(TEST_SOURCES) $(LIB_NAME)
	$(CC) $(CFLAGS) $(INCLUDES) $< -L. -lralph $(LDFLAGS) -o $@

# Debug build
debug: CFLAGS = $(CFLAGS_DEBUG)
debug: clean all

# Run tests
test: $(TEST_EXEC)
	./$(TEST_EXEC)

# Clean
clean:
	rm -f $(OBJECTS) $(LIB_NAME) $(TEST_EXEC)

# Install headers and library
install: $(LIB_NAME)
	mkdir -p /usr/local/include/ralph
	mkdir -p /usr/local/lib
	cp $(INCLUDE_DIR)/ralph.h /usr/local/include/ralph/
	cp $(LIB_NAME) /usr/local/lib/

# Uninstall
uninstall:
	rm -rf /usr/local/include/ralph
	rm -f /usr/local/lib/$(LIB_NAME)

.PHONY: all clean debug test install uninstall
