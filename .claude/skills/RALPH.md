# Ralph LP/MIP Solver - Usage Guide

## Overview

Ralph is a lightweight, zero-dependency LP/MIP solver written in C. It can be used standalone or compared against GLPK for validation and benchmarking.

## Quick Start

```bash
# Build Ralph
make

# Run tests
make test

# Run size scaling benchmark
make bench_sizes && ./bench_sizes
```

## Building Ralph

### Basic Build

```bash
cd ralph
make          # Builds libralph.a
make test     # Builds and runs test suite
make clean    # Removes all build artifacts
```

### Build Options

```bash
# Debug build (no optimization, includes debug symbols)
make DEBUG=1

# With sanitizers (for debugging memory issues)
make SANITIZE=1
```

## Using Ralph in Your Code

### Basic LP Example

```c
#include "ralph.h"

int main() {
    // Create model
    RalphModel *model = ralph_create();

    // Add variables: ralph_add_var(model, lb, ub, obj_coef, type)
    ralph_add_var(model, 0, RALPH_INFINITY, 3.0, RALPH_CONTINUOUS);  // x0
    ralph_add_var(model, 0, RALPH_INFINITY, 2.0, RALPH_CONTINUOUS);  // x1

    // Add constraint: 2*x0 + x1 <= 10
    int indices[] = {0, 1};
    double values[] = {2.0, 1.0};
    ralph_add_constraint(model, 2, indices, values, RALPH_LESS_EQUAL, 10.0);

    // Solve
    ralph_optimize(model);

    // Get results
    if (ralph_get_status(model) == RALPH_STATUS_OPTIMAL) {
        printf("Objective: %f\n", ralph_get_objval(model));

        double solution[2];
        ralph_get_solution(model, solution);
        printf("x0 = %f, x1 = %f\n", solution[0], solution[1]);
    }

    ralph_free(model);
    return 0;
}
```

### MIP Example (Integer Variables)

```c
#include "ralph.h"

int main() {
    RalphModel *model = ralph_create();

    // Binary knapsack: max 5x + 3y + 2z, s.t. 2x + 4y + 3z <= 7
    ralph_add_var(model, 0, 1, -5.0, RALPH_BINARY);  // x (negate for max)
    ralph_add_var(model, 0, 1, -3.0, RALPH_BINARY);  // y
    ralph_add_var(model, 0, 1, -2.0, RALPH_BINARY);  // z

    int idx[] = {0, 1, 2};
    double val[] = {2.0, 4.0, 3.0};
    ralph_add_constraint(model, 3, idx, val, RALPH_LESS_EQUAL, 7.0);

    ralph_optimize(model);

    double sol[3];
    ralph_get_solution(model, sol);
    printf("Optimal: x=%g, y=%g, z=%g, obj=%g\n",
           sol[0], sol[1], sol[2], -ralph_get_objval(model));

    ralph_free(model);
    return 0;
}
```

### Compiling Your Program

```bash
# Compile with Ralph
gcc -O3 -I./include myprogram.c -L. -lralph -lm -o myprogram
```

## API Reference

### Model Creation/Destruction
- `ralph_create()` - Create new model
- `ralph_free(model)` - Free model and all resources

### Variables
- `ralph_add_var(model, lb, ub, obj, type)` - Add variable
  - Types: `RALPH_CONTINUOUS`, `RALPH_INTEGER`, `RALPH_BINARY`
  - Use `RALPH_INFINITY` for unbounded

### Constraints
- `ralph_add_constraint(model, nnz, indices, values, sense, rhs)`
  - Senses: `RALPH_LESS_EQUAL`, `RALPH_EQUAL`, `RALPH_GREATER_EQUAL`

### Solving
- `ralph_optimize(model)` - Solve the model
- `ralph_get_status(model)` - Get solution status
  - `RALPH_STATUS_OPTIMAL`, `RALPH_STATUS_INFEASIBLE`, `RALPH_STATUS_UNBOUNDED`

### Solution Access
- `ralph_get_objval(model)` - Get objective value
- `ralph_get_solution(model, x)` - Get variable values
- `ralph_get_dual(model, y)` - Get dual values

### Parameters
- `ralph_set_int_param(model, name, value)`
- `ralph_set_dbl_param(model, name, value)`
  - `"verbose"` - 0=silent, 1=summary, 2=detailed
  - `"max_iter"` - Maximum iterations
  - `"time_limit"` - Time limit in seconds

---

## Installing GLPK

GLPK (GNU Linear Programming Kit) is useful for validating Ralph's results and benchmarking.

### macOS

```bash
# Using Homebrew
brew install glpk

# Verify installation
glpsol --version
# Headers: /opt/homebrew/include/glpk.h
# Library: /opt/homebrew/lib/libglpk.a
```

### Ubuntu/Debian Linux

```bash
# Install GLPK
sudo apt-get update
sudo apt-get install libglpk-dev glpk-utils

# Verify installation
glpsol --version
# Headers: /usr/include/glpk.h
# Library: /usr/lib/x86_64-linux-gnu/libglpk.so
```

### Fedora/RHEL Linux

```bash
sudo dnf install glpk-devel glpk-utils
```

### Arch Linux

```bash
sudo pacman -S glpk
```

### Building from Source

```bash
wget https://ftp.gnu.org/gnu/glpk/glpk-5.0.tar.gz
tar xzf glpk-5.0.tar.gz
cd glpk-5.0
./configure
make
sudo make install
```

---

## Building Benchmarks with GLPK

### macOS (Homebrew)

```bash
cd ralph

# Build comparison benchmark
gcc -O3 -I./include -I/opt/homebrew/include \
    benchmarks/compare_glpk.c \
    -L. -L/opt/homebrew/lib -lralph -lglpk -lm \
    -o compare_glpk

# Build full benchmark suite
gcc -O3 -I./include -I/opt/homebrew/include \
    benchmarks/bench_vs_glpk.c \
    -L. -L/opt/homebrew/lib -lralph -lglpk -lm \
    -o bench_vs_glpk

# Build MIP benchmark
gcc -O3 -I./include -I/opt/homebrew/include \
    benchmarks/bench_mip.c \
    -L. -L/opt/homebrew/lib -lralph -lglpk -lm \
    -o bench_mip
```

### Linux

```bash
cd ralph

# Build comparison benchmark
gcc -O3 -I./include \
    benchmarks/compare_glpk.c \
    -L. -lralph -lglpk -lm \
    -o compare_glpk

# Build full benchmark suite
gcc -O3 -I./include \
    benchmarks/bench_vs_glpk.c \
    -L. -lralph -lglpk -lm \
    -o bench_vs_glpk

# Build MIP benchmark
gcc -O3 -I./include \
    benchmarks/bench_mip.c \
    -L. -lralph -lglpk -lm \
    -o bench_mip
```

---

## Running Tests

### Unit Tests

```bash
# Build and run all tests
make test

# Expected output:
# Test Summary: 59/59 passed (100.0%)
# ✓ All tests passed!
```

### Test Categories

The test suite covers:
- Simple LP problems (2-variable, equality, inequality constraints)
- Diet problem (classic LP)
- Infeasibility detection with Farkas certificates
- Larger LPs (20x10, network flow)
- Binary knapsack (MIP)
- Integer programming
- Mixed integer programming
- Facility location (MIP)
- GMI cuts
- API functions

### Running Specific Tests

```bash
# Build test binary
gcc -O3 -I./include tests/test_main.c -L. -lralph -lm -o test_ralph

# Run all tests
./test_ralph

# The test output shows each test with PASS/FAIL status
```

---

## Running Benchmarks

### Size Scaling Benchmark (No GLPK Required)

```bash
# Build
gcc -O3 -I./include benchmarks/bench_sizes.c -L. -lralph -lm -o bench_sizes

# Run
./bench_sizes
```

**Sample Output:**
```
Ralph LP Solver - Size Scaling Benchmark
=========================================

Size (nxm)   Time(s)      Status       Objective
----------   -------      ------       ---------
20x10        0.0001       OPTIMAL      3926.40
50x25        0.0001       OPTIMAL      2488.81
100x50       0.0004       OPTIMAL      2298.23
200x100      0.0017       OPTIMAL      2411.11
500x250      0.0533       OPTIMAL      1653.19
1000x500     0.5090       OPTIMAL      1679.11
```

### Quick GLPK Comparison

```bash
./compare_glpk
```

**Sample Output:**
```
Ralph vs GLPK Comparison
========================

20x10:
  Ralph:   0.0001s  OPTIMAL   obj=     3926.40
  GLPK:    0.0000s  OPTIMAL   obj=     3926.40
  Match: EXACT

100x50:
  Ralph:   0.0004s  OPTIMAL   obj=     2298.23
  GLPK:    0.0000s  OPTIMAL   obj=     2298.23
  Match: EXACT
```

### Full LP Benchmark Suite

```bash
./bench_vs_glpk
```

This runs multiple trials at each problem size and reports:
- Average solve time
- Iteration counts
- Per-iteration timing
- Objective value validation

### MIP Benchmark

```bash
# Quick mode (smaller problems)
./bench_mip --quick

# Full benchmark (may take several minutes)
./bench_mip
```

---

## Performance Characteristics

### LP Solver
- **Small problems (< 100 vars):** Competitive with GLPK
- **Medium problems (100-500 vars):** 2-6x slower than GLPK
- **Large problems (> 500 vars):** 10-15x slower than GLPK

The main bottleneck is LU factorization updates. Ralph uses a simpler implementation compared to GLPK's highly optimized sparse LU code.

### MIP Solver
- Branch-and-bound with GMI cuts
- Effective on small to medium problems (< 100 integer variables)
- Larger MIPs may require significant time

### Iteration Efficiency
Ralph often finds solutions in fewer iterations than GLPK but each iteration is slower due to:
- Less optimized sparse matrix operations
- Simpler LU update strategy
- No advanced preprocessing

---

## Troubleshooting

### Common Issues

**Link error: cannot find -lglpk**
```bash
# macOS: Ensure Homebrew lib path is included
-L/opt/homebrew/lib

# Linux: Install libglpk-dev
sudo apt-get install libglpk-dev
```

**Header not found: glpk.h**
```bash
# macOS: Add Homebrew include path
-I/opt/homebrew/include

# Linux: Install development package
sudo apt-get install libglpk-dev
```

**Runtime error: library not found**
```bash
# Linux: Update library cache
sudo ldconfig

# Or set LD_LIBRARY_PATH
export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH
```

### Debugging Tips

1. Enable verbose output:
   ```c
   ralph_set_int_param(model, "verbose", 2);
   ```

2. Check solution status before accessing results:
   ```c
   if (ralph_get_status(model) == RALPH_STATUS_OPTIMAL) {
       // Safe to access solution
   }
   ```

3. Validate against GLPK:
   ```bash
   ./compare_glpk  # Should show "Match: EXACT" for all problems
   ```

---

## File Structure

```
ralph/
├── include/
│   ├── ralph.h          # Public API
│   ├── lp.h             # Internal LP structures
│   ├── mip.h            # MIP structures
│   └── sparse.h         # Sparse matrix utilities
├── src/
│   ├── ralph.c          # API implementation
│   ├── simplex.c        # Primal simplex
│   ├── dual_simplex.c   # Dual simplex
│   ├── lu.c             # LU factorization
│   ├── branch_bound.c   # MIP solver
│   └── cuts.c           # Cut generation (GMI)
├── tests/
│   └── test_main.c      # Test suite
├── benchmarks/
│   ├── bench_sizes.c    # Size scaling (no GLPK)
│   ├── compare_glpk.c   # Quick comparison
│   ├── bench_vs_glpk.c  # Full LP benchmark
│   └── bench_mip.c      # MIP benchmark
├── Makefile
└── SKILLS.md            # This file
```
