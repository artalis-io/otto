# LU Factorization Implementation

## Overview

The LU factorization is the computational heart of the revised simplex method. It allows us to efficiently solve systems of the form Bx = b where B is the basis matrix.

## Mathematical Background

### LU Decomposition

Given a square matrix B, we compute:
```
PB = LU
```
where:
- P is a permutation matrix (row pivoting)
- L is unit lower triangular
- U is upper triangular

### Solving Systems

**Forward solve (Bx = b):**
```
1. Permute: b' = Pb
2. Forward substitution: Ly = b'
3. Backward substitution: Ux = y
```

**Transpose solve (B'x = b):**
```
1. Forward substitution: U'y = b
2. Backward substitution: L'z = y
3. Permute: x = P'z
```

## Implementation

### Data Structure

```c
typedef struct {
    int m;                      // Matrix dimension

    // L matrix (unit lower triangular, stored in CSC)
    int *L_colptr, *L_rowidx;
    double *L_values;
    int nnz_L;

    // U matrix (upper triangular, stored in CSC)
    int *U_colptr, *U_rowidx;
    double *U_values;
    int nnz_U;

    // Row permutation
    int *perm;                  // perm[i] = original row that is now row i
    int *perm_inv;              // perm_inv[i] = current position of original row i

    // Eta file for updates
    int num_eta;
    int max_updates;
    int *eta_col;               // Column index for each eta
    double **eta_vectors;       // The eta vectors
} LUFactorization;
```

### Initial Factorization

**File:** `lu.c`, function `lu_factorize()`

Uses Gaussian elimination with partial pivoting:

```c
for (k = 0; k < m; k++) {
    // Find pivot (largest magnitude in column k, rows k to m-1)
    pivot_row = argmax(|A[k:m, k]|);

    // Swap rows if necessary
    if (pivot_row != k) {
        swap_rows(A, k, pivot_row);
        swap(perm, k, pivot_row);
    }

    // Eliminate below diagonal
    for (i = k+1; i < m; i++) {
        mult = A[i,k] / A[k,k];
        A[i,k] = mult;  // Store L entry
        A[i, k+1:m] -= mult × A[k, k+1:m];
    }
}
```

### Forward Substitution (solve Ly = b)

**File:** `lu.c`, function `solve_L()`

```c
void solve_L(lu, b, x) {
    // Apply row permutation
    for (i = 0; i < m; i++)
        x[i] = b[perm[i]];

    // Forward substitution (L[j,j] = 1)
    for (j = 0; j < m; j++) {
        for each L[i,j] where i > j {
            x[i] -= L[i,j] × x[j];
        }
    }
}
```

### Backward Substitution (solve Ux = y)

**File:** `lu.c`, function `solve_U()`

```c
void solve_U(lu, y, x) {
    copy(x, y);

    for (j = m-1; j >= 0; j--) {
        x[j] /= U[j,j];
        for each U[i,j] where i < j {
            x[i] -= U[i,j] × x[j];
        }
    }
}
```

## Eta File Updates

### Motivation

After a simplex pivot, the basis matrix changes by one column. Rather than refactorizing (O(m³)), we store the change as an "eta matrix" E that satisfies:

```
B_new = B_old × E
```

where E is the identity except for one column.

### Eta Matrix Structure

If column `leaving_pos` is replaced:
```
E = I with column leaving_pos = η

where η = B_old⁻¹ × a_entering
```

The inverse of E is:
```
E⁻¹ = I with column leaving_pos = η'

where η'[leaving_pos] = 1/η[leaving_pos]
      η'[i] = -η[i]/η[leaving_pos]  for i ≠ leaving_pos
```

### Applying Eta Updates

**File:** `lu.c`, function `apply_eta_forward()`

**CRITICAL:** This function must update ALL components, not just the pivot column.

```c
void apply_eta_forward(lu, x) {
    for (k = 0; k < num_eta; k++) {
        col = eta_col[k];
        eta = eta_vectors[k];
        xc = x[col];  // Save original value

        // Apply E⁻¹ × x
        for (i = 0; i < m; i++) {
            if (i == col) {
                x[i] = eta[col] × xc;      // Division by pivot
            } else {
                x[i] += eta[i] × xc;       // Add contribution
            }
        }
    }
}
```

**For transpose solve:**
```c
void apply_eta_backward(lu, x) {
    for (k = num_eta - 1; k >= 0; k--) {
        col = eta_col[k];
        eta = eta_vectors[k];

        // Apply (E⁻¹)' × x
        xc = 0;
        for (i = 0; i < m; i++) {
            xc += eta[i] × x[i];
        }
        x[col] = xc;
    }
}
```

### When to Refactorize

After many eta updates:
- Numerical errors accumulate
- Storage grows
- Apply operations become slow

Refactorize when `num_eta >= max_updates` (default 50).

## Complete Solve Process

**Solve Bx = b where B = B₀ × E₁ × E₂ × ... × Eₖ:**

```c
void lu_solve(lu, rhs, solution) {
    // Step 1: Solve L × y = P × rhs
    solve_L(lu, rhs, work);

    // Step 2: Solve U × z = y
    solve_U(lu, work, solution);

    // Step 3: Apply eta updates
    // solution = Eₖ⁻¹ × ... × E₁⁻¹ × z
    apply_eta_forward(lu, solution);
}
```

**Solve B'x = b (for computing duals):**

```c
void lu_solve_transpose(lu, rhs, solution) {
    // Step 1: Apply eta updates in reverse
    copy(work, rhs);
    apply_eta_backward(lu, work);

    // Step 2: Solve U' × y = work
    solve_Ut(lu, work, solution);

    // Step 3: Solve L' × z = y, then apply P'
    solve_Lt(lu, solution, work);
    copy(solution, work);
}
```

## Numerical Considerations

### Pivot Tolerance

Reject pivots smaller than `RALPH_PIVOT_TOL` (1e-8) to avoid division by near-zero.

### Growth Factor

Monitor the ratio max|U[i,j]| / max|A[i,j]|. If too large, numerical instability may occur.

### Sparse Storage

For large sparse problems, L and U are stored in CSC format to save memory and computation time.
