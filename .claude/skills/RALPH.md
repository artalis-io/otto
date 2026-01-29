# Ralph LP/MIP/LAP Solver - Usage Guide

## Overview

Ralph is a lightweight, zero-dependency LP/MIP/LAP solver written in C. It includes:
- **LP Solver**: Primal/dual simplex with LU factorization
- **MIP Solver**: Branch-and-bound with GMI cuts
- **LAP Solver**: JVC algorithm (86-633× faster than simplex for assignments)

It can be used standalone or compared against GLPK for validation and benchmarking.

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

### OpenMP SIMD Support

Ralph uses OpenMP SIMD pragmas for portable vectorization that works on both x86 and ARM architectures. The Makefile auto-detects the platform and configures OpenMP accordingly.

#### macOS (Apple Silicon & Intel)

```bash
# Install libomp via Homebrew (required)
brew install libomp

# Build (auto-detects and uses libomp)
make
```

The Makefile uses:
- `-Xclang -fopenmp` for clang compiler
- `-I/opt/homebrew/opt/libomp/include` for headers
- `-L/opt/homebrew/opt/libomp/lib -lomp` for linking

#### Linux (Ubuntu/Debian)

```bash
# GCC has native OpenMP support, no extra packages needed
# Or optionally install:
sudo apt-get install libomp-dev

# Build
make
```

The Makefile uses:
- `-fopenmp` for gcc (native support)

#### Verifying SIMD is Active

```bash
# Check if OpenMP is detected during compilation
make clean && make 2>&1 | grep -i openmp

# You should see: -fopenmp or -Xclang -fopenmp in the compile commands
```

#### Disabling OpenMP (if needed)

```bash
# Edit Makefile and remove/comment out the OpenMP section, or:
make CFLAGS="-Wall -Wextra -O3 -march=native -ffast-math"
```

#### SIMD-Optimized Functions

The following functions use `#pragma omp simd` for vectorization:
- `apply_compacted_matrix()` - Dense matrix-vector multiply
- `apply_compacted_matrix_transpose()` - Dense matrix-vector multiply (transpose)
- `sparse_matvec_transpose()` - Sparse matrix-vector product
- `sparse_dot_column()` - Sparse dot product (heavily used in simplex)

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
  - `"detect_special"` - Enable LAP/network detection (0 or 1)

---

## LAP Solver (Linear Assignment Problem)

The LAP solver uses the Jonker-Volgenant-Castanon (JVC) algorithm, which is 86-633× faster than simplex for assignment problems.

### Basic LAP Example

```c
#include "lap.h"

int main() {
    // 3x3 cost matrix (row-major)
    double cost[9] = {
        1, 10, 10,
        10, 2, 10,
        10, 10, 3
    };

    int row_sol[3];  // row_sol[i] = column assigned to row i
    double total_cost;

    ralph_lap_solve(3, cost, RALPH_LAP_MINIMIZE,
                    row_sol, NULL, NULL, NULL, &total_cost);

    printf("Cost: %.2f\n", total_cost);  // 6.0 (diagonal)
    printf("Assignment: 0->%d, 1->%d, 2->%d\n",
           row_sol[0], row_sol[1], row_sol[2]);
    return 0;
}
```

### LAP API Reference

| Function | Description |
|----------|-------------|
| `ralph_lap_solve()` | Dense n×n LAP |
| `ralph_lap_solve_sparse()` | Sparse LAP (CSR format) |
| `ralph_lap_solve_rect()` | Rectangular m×n LAP |
| `ralph_lap_solve_warm()` | Warm start from previous solution |
| `ralph_lap_solve_callback()` | On-demand cost computation (O(n) memory) |
| `ralph_lap_solve_k_best()` | Find k best assignments (Murty's algorithm) |
| `ralph_lap_solve_lp()` | Solve via LP (for verification) |

### Advanced Features

**Sparse LAP (CSR format):**
```c
int row_ptr[] = {0, 2, 4, 6};  // 3 rows
int col_idx[] = {0, 1, 0, 2, 1, 2};
double values[] = {1.0, 5.0, 3.0, 2.0, 4.0, 1.0};

ralph_lap_solve_sparse(3, 6, row_ptr, col_idx, values,
                       RALPH_LAP_MINIMIZE, row_sol, NULL, &cost);
```

**k-Best Assignments:**
```c
int solutions[3 * n];  // k=3 solutions
double costs[3];
int num_found;

ralph_lap_solve_k_best(n, cost, RALPH_LAP_MINIMIZE, 3,
                       solutions, costs, &num_found);
```

**Warm Start (for similar problems):**
```c
RalphLapWorkspace *ws = ralph_lap_workspace_create(n);

// Solve first problem
ralph_lap_solve_warm(n, cost1, RALPH_LAP_MINIMIZE,
                     row_sol, NULL, NULL, NULL, &obj, ws, 0);

// Solve similar problem with warm start
ralph_lap_solve_warm(n, cost2, RALPH_LAP_MINIMIZE,
                     row_sol, NULL, NULL, NULL, &obj, ws, 1);

ralph_lap_workspace_free(ws);
```

**Callback-based (O(n) memory):**
```c
double my_cost(int i, int j, void *data) {
    // Compute cost on-demand
    return compute_distance(i, j, data);
}

ralph_lap_solve_callback(n, my_cost, user_data, RALPH_LAP_MINIMIZE,
                         row_sol, NULL, NULL, NULL, &total_cost);
```

### Unified API (Recommended)

The unified API supports all feature combinations with a single function:

```c
/* Problem: WHAT to solve */
RalphLapProblem prob = {
    .n = 100, .m = 100,
    .cost_type = RALPH_LAP_COST_DENSE,  /* or SPARSE, CALLBACK */
    .dense_cost = cost_matrix,
    .objective = RALPH_LAP_MINIMIZE
};

/* Options: HOW to solve */
RalphLapOptions opts = RALPH_LAP_OPTIONS_DEFAULT;
opts.algorithm = RALPH_LAP_ALG_K_BEST;  /* or STANDARD, BOTTLENECK */
opts.k = 5;
opts.num_forbidden = 3;  /* Forbidden assignments */
opts.forbidden_rows = (int[]){0, 1, 2};
opts.forbidden_cols = (int[]){0, 1, 2};

/* Result storage */
int solutions[500];  /* 5 × 100 */
double costs[5];
RalphLapResult res = {.row_sol = solutions, .costs = costs};

/* Solve */
ralph_lap_solve_ex(&prob, &opts, &res, NULL);
```

**Supported combinations:**
- Representations: dense, sparse (CSR), rectangular, callback
- Algorithms: standard, k-best, bottleneck (minimax/maximin)
- Features: forbidden assignments, warm start, epsilon scaling
- Sparse/callback + k-best supported via dense conversion

### LAP Integration with LP/MIP

Assignment problems formulated as LPs/MIPs can be solved with JVC:

```c
// Create assignment MIP
RalphModel *model = ralph_create();
// ... add binary variables and assignment constraints ...

// Enable LAP detection
ralph_set_int_param(model, "detect_special", 1);

// Solve - will use JVC internally
ralph_optimize(model);
```

### LAP Performance

| Size | JVC (ms) | LP Simplex (ms) | Speedup |
|------|----------|-----------------|---------|
| 10×10 | 0.001 | 0.095 | 86× |
| 20×20 | 0.001 | 0.886 | 633× |
| 100×100 | 0.044 | N/A | - |
| 500×500 | 3.0 | N/A | - |

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
# Build and run LP/MIP tests
make test
# Expected: 73/73 passed

# Build and run LAP tests
make test-lap
# Expected: 255/255 passed
```

### Test Categories

**LP/MIP tests (73 tests):**
- Simple LP problems (2-variable, equality, inequality constraints)
- Diet problem (classic LP)
- Infeasibility detection with Farkas certificates
- Larger LPs (20x10, network flow)
- Binary knapsack (MIP)
- Integer programming
- Mixed integer programming
- Facility location (MIP)
- GMI cuts
- LAP-based MIP (assignment problems)
- API functions

**LAP tests (255 tests):**
- Dense JVC (minimize/maximize)
- Sparse LAP (CSR format)
- Rectangular LAP (m×n)
- Warm start / incremental
- Cost callbacks
- k-Best assignments (Murty)
- Bottleneck LAP (minimax/maximin)
- ε-scaling
- Edge cases (infeasible, ties)
- Unified API combinations (sparse+k-best, etc.)

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
│   ├── ralph.h          # Public LP/MIP API
│   ├── lap.h            # LAP solver API
│   ├── detect.h         # Problem structure detection
│   ├── lp.h             # Internal LP structures
│   ├── mip.h            # MIP structures
│   └── sparse.h         # Sparse matrix utilities
├── src/
│   ├── ralph.c          # API implementation
│   ├── lap.c            # JVC algorithm (LAP)
│   ├── detect.c         # LAP/network detection
│   ├── simplex.c        # Primal simplex
│   ├── dual_simplex.c   # Dual simplex
│   ├── lu.c             # LU factorization
│   ├── branch_bound.c   # MIP solver
│   ├── mip.c            # MIP with LAP integration
│   └── cuts.c           # Cut generation (GMI)
├── tests/
│   ├── test_main.c      # LP/MIP test suite (73 tests)
│   └── test_lap.c       # LAP test suite (213 tests)
├── benchmarks/
│   ├── bench_lap.c      # LAP benchmarks
│   ├── bench_sizes.c    # Size scaling (no GLPK)
│   ├── compare_glpk.c   # Quick comparison
│   ├── bench_vs_glpk.c  # Full LP benchmark
│   └── bench_mip.c      # MIP benchmark
├── docs/
│   ├── LAP_ROADMAP.md   # LAP feature roadmap
│   └── TODO_FEATURES.md # All planned features
├── Makefile
└── CLAUDE.md            # Developer instructions
```
