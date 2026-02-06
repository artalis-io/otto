---
name: c23-modernize
description: Analyze C code and suggest C23 modernizations. Creates migration plans for clarity, safety, brevity using GCC 15+ features with Emscripten/WASI compatibility warnings.
user-invocable: true
---

# C23 Modernize Skill

Analyzes a module's C code and suggests C23 feature adoption for improved clarity, security, brevity, and expressivity.

**Target:** $ARGUMENTS (module name or file path)

## Usage

```
/c23-modernize ralph           # Analyze ralph module
/c23-modernize carta/src       # Analyze specific directory
/c23-modernize --conservative  # Only universally-supported features
/c23-modernize --aggressive    # All GCC 15 features (may break WASM)
/c23-modernize --plan          # Generate migration plan without code changes
```

## Compiler Support Matrix

Before suggesting features, verify target compiler support:

| Feature | GCC 15+ | Clang 18+ | Emscripten 3.1.50+ | wasm32-wasi |
|---------|---------|-----------|-------------------|-------------|
| `nullptr` | Yes | Yes | Yes | Yes |
| `constexpr` | Yes | Yes | Partial | Yes |
| `typeof` / `typeof_unqual` | Yes | Yes | Yes | Yes |
| `auto` (type inference) | Yes | Yes | No | Yes |
| `[[nodiscard]]` | Yes | Yes | Yes | Yes |
| `[[maybe_unused]]` | Yes | Yes | Yes | Yes |
| `[[deprecated]]` | Yes | Yes | Yes | Yes |
| `#embed` | Yes | Clang 19+ | No | Clang 19+ |
| `_BitInt(N)` | Yes | Yes | No | Yes |
| `#elifdef` / `#elifndef` | Yes | Yes | Yes | Yes |
| `bool` without stdbool.h | Yes | Yes | Yes | Yes |
| `static_assert` no message | Yes | Yes | Yes | Yes |
| `<stdckdint.h>` | Yes | Partial | No | Partial |
| `unreachable()` | Yes | Yes | Partial | Yes |
| `= {}` empty initializer | Yes | Yes | Yes | Yes |

### WASM Build Strategy

OTTO can use a hybrid WASM approach for better C23 support:

| Module | Current | Recommended | Rationale |
|--------|---------|-------------|-----------|
| Ralph | Emscripten | wasm32-wasi | Pure computation, no browser APIs |
| FuelWise | Emscripten | wasm32-wasi | Pure computation |
| Shared (math) | Emscripten | wasm32-wasi | Pure computation |
| Velo | Emscripten | Either | Computation-heavy, minimal I/O |
| Carta | Emscripten | Either | Computation-heavy |
| Locus | Emscripten | Either | Computation-heavy |
| ClayShards | Emscripten | Emscripten | Requires WebGL bindings |

**wasm32-wasi advantages:**
- Native Clang support (no separate toolchain)
- Better C23 support (current Clang, not Emscripten's lagging LLVM)
- 50-70% smaller binaries (no POSIX emulation layer)
- Standardized syscall interface

**Build command:**
```bash
# wasm32-wasi (pure computation modules)
clang --target=wasm32-wasi -std=c23 -O2 -o module.wasm src/*.c

# Emscripten (browser integration modules)
emcc -std=c2x -O2 -o module.js src/*.c
```

## C23 Features by Value

### Tier 1: High Value, Universal Support (Always Recommend)

#### `nullptr` (replaces NULL)
**Benefit:** Type-safe null pointer, avoids integer/pointer confusion.
**Risk:** None
**Migration:** Simple find-replace

```c
// Before (C11)
void *p = NULL;
if (ptr == NULL) { ... }

// After (C23)
void *p = nullptr;
if (ptr == nullptr) { ... }
```

**Detection pattern:** `grep -n '\bNULL\b'`

---

#### `[[nodiscard]]` attribute
**Benefit:** Compiler warns if return value ignored. Catches bugs like unchecked malloc.
**Risk:** None (advisory)
**Migration:** Add to functions whose return value must be checked

```c
// Before
void *arena_alloc(Arena *a, size_t size);
RalphStatus ralph_optimize(RalphModel *m);

// After
[[nodiscard]] void *arena_alloc(Arena *a, size_t size);
[[nodiscard]] RalphStatus ralph_optimize(RalphModel *m);
```

**Detection pattern:** Functions returning error codes, pointers, or status enums

---

#### `[[maybe_unused]]` attribute
**Benefit:** Suppresses unused variable warnings cleanly (replaces `(void)x;` hack)
**Risk:** None
**Migration:** Replace `(void)var;` patterns

```c
// Before
void callback(void *ctx, int event) {
    (void)ctx;  // Suppress warning
    handle_event(event);
}

// After
void callback([[maybe_unused]] void *ctx, int event) {
    handle_event(event);
}
```

**Detection pattern:** `grep -n '(void)'` for cast-to-void suppressions

---

#### `bool` / `true` / `false` as keywords
**Benefit:** No `#include <stdbool.h>` needed
**Risk:** None
**Migration:** Remove stdbool.h includes

```c
// Before
#include <stdbool.h>
bool is_valid = true;

// After (C23)
bool is_valid = true;  // Just works
```

**Detection pattern:** `grep -n '#include <stdbool.h>'`

---

#### `static_assert` without message
**Benefit:** Cleaner compile-time assertions
**Risk:** None
**Migration:** Remove redundant messages

```c
// Before
static_assert(sizeof(int) == 4, "int must be 4 bytes");

// After (C23) - message optional
static_assert(sizeof(int) == 4);
```

---

#### `= {}` empty initializer
**Benefit:** Zero-initialize any type without knowing its structure
**Risk:** None
**Migration:** Replace `= {0}` patterns

```c
// Before
RalphModel model = {0};
double arr[100] = {0};

// After (C23)
RalphModel model = {};
double arr[100] = {};
```

**Detection pattern:** `grep -n '= {0}'`

---

#### `#elifdef` / `#elifndef`
**Benefit:** Cleaner preprocessor conditionals
**Risk:** None
**Migration:** Simplify #elif defined() chains

```c
// Before
#ifdef DEBUG
    ...
#elif defined(RELEASE)
    ...
#endif

// After (C23)
#ifdef DEBUG
    ...
#elifdef RELEASE
    ...
#endif
```

---

### Tier 2: High Value, Partial Support (Recommend with Warning)

#### `typeof` / `typeof_unqual`
**Benefit:** Type-safe macros, reduce repetition
**Risk:** Low (was GNU extension, now standard)
**Emscripten:** Supported (was already using GNU extension)

```c
// Before - error-prone macro
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// After (C23) - type-safe
#define MAX(a, b) ({ \
    typeof(a) _a = (a); \
    typeof(b) _b = (b); \
    _a > _b ? _a : _b; \
})

// Or for swap without knowing type
#define SWAP(x, y) do { \
    typeof(x) _tmp = (x); \
    (x) = (y); \
    (y) = _tmp; \
} while(0)
```

---

#### `constexpr`
**Benefit:** Compile-time constants, replaces some #define usage
**Risk:** Medium - Emscripten support partial
**Migration:** Replace #define constants where appropriate

```c
// Before
#define MAX_NODES 10000
#define PI 3.14159265358979

// After (C23)
constexpr int MAX_NODES = 10000;
constexpr double PI = 3.14159265358979;
```

**Emscripten warning:** May not work. Use `#define` fallback or guard with `#if __STDC_VERSION__ >= 202311L`.

---

#### `unreachable()`
**Benefit:** Optimizer hint for impossible code paths
**Risk:** Low
**Migration:** Replace `__builtin_unreachable()` or assert(0)

```c
// Before (GNU)
switch (state) {
    case A: return handle_a();
    case B: return handle_b();
    default: __builtin_unreachable();
}

// After (C23)
#include <stddef.h>  // or <stdlib.h>
switch (state) {
    case A: return handle_a();
    case B: return handle_b();
    default: unreachable();
}
```

---

### Tier 3: High Value, Limited Support (GCC 15 / wasm32-wasi only)

#### `auto` type inference
**Benefit:** Reduces repetition, especially with complex types
**Risk:** High - Emscripten does NOT support
**WASI:** Supported with Clang

```c
// Before
struct VeryLongTypeName *node = get_node();
for (size_t i = 0; i < count; i++) { ... }

// After (C23) - GCC 15+ / wasm32-wasi only
auto node = get_node();  // Infers struct VeryLongTypeName*
for (auto i = 0uz; i < count; i++) { ... }  // uz = size_t literal
```

**Warning:** Do NOT use if targeting Emscripten. Use only in wasm32-wasi builds.

---

#### `#embed`
**Benefit:** Include binary data directly (replaces xxd or bin2c)
**Risk:** High - Clang 19+, GCC 15+, NO Emscripten
**WASI:** Supported with Clang 19+

```c
// Before (xxd -i generated)
static const unsigned char font_data[] = {
    0x00, 0x01, 0x02, ... // thousands of lines
};
static const size_t font_data_len = 12345;

// After (C23)
static const unsigned char font_data[] = {
    #embed "assets/font.ttf"
};
constexpr size_t font_data_len = sizeof(font_data);
```

**Warning:** Emscripten doesn't support. Keep xxd fallback or use only for wasm32-wasi.

---

#### `_BitInt(N)`
**Benefit:** Exact-width integers beyond int64_t
**Risk:** Medium - Emscripten does NOT support
**Use case:** Crypto, hash functions, arbitrary precision

```c
// C23
_BitInt(128) big_hash;
_BitInt(256) sha256_result;
```

---

### Tier 4: Situational / Low Priority

#### `<stdckdint.h>` checked arithmetic
**Benefit:** Overflow-safe integer operations
**Risk:** High - Poor support, runtime overhead
**Recommendation:** Skip for now. Use existing overflow checks.

```c
// C23 (when supported)
#include <stdckdint.h>
int result;
if (ckd_add(&result, a, b)) {
    // Overflow occurred
}
```

---

#### `[[deprecated]]` attribute
**Benefit:** Mark functions for removal
**Use case:** API evolution, migration guidance

```c
[[deprecated("Use new_api() instead")]]
void old_api(void);
```

---

## Audit Procedure

When `/c23-modernize <module>` is invoked:

### 1. Check current compiler flags

```bash
grep -r 'std=c' Makefile */Makefile
```

Verify module uses `-std=c11` or `-std=c17`. Note if already using `-std=c2x`.

### 2. Determine build targets

```bash
# Check if module has WASM target
grep -l 'emcc\|emscripten\|wasm' Makefile */Makefile
```

- **Native only:** Can use all GCC 15 features
- **Emscripten WASM:** Restrict to Tier 1 + Tier 2 (with guards)
- **wasm32-wasi:** Can use Tier 1-3

### 3. Scan for modernization opportunities

```bash
# Tier 1: Universal
grep -rn '\bNULL\b' $MODULE/src $MODULE/include
grep -rn '#include <stdbool.h>' $MODULE/src $MODULE/include
grep -rn '= {0}' $MODULE/src $MODULE/include
grep -rn '(void)[^)]' $MODULE/src $MODULE/include  # (void)x; pattern

# Tier 2: With warnings
grep -rn '#define [A-Z_]* [0-9]' $MODULE/include  # Numeric constants

# Functions needing [[nodiscard]]
grep -rn 'Status\|Result\|Error' $MODULE/include/*.h | grep -v typedef
```

### 4. Generate migration plan

For each finding, categorize:

```markdown
## Migration Plan: <module>

### Phase 1: Safe Changes (Tier 1)

| File | Line | Current | Proposed | Risk |
|------|------|---------|----------|------|
| src/foo.c | 42 | `NULL` | `nullptr` | None |
| src/bar.c | 15 | `= {0}` | `= {}` | None |

### Phase 2: With Guards (Tier 2)

| File | Line | Current | Proposed | Guard Needed |
|------|------|---------|----------|--------------|
| include/foo.h | 10 | `#define MAX 100` | `constexpr int MAX = 100` | `#if __STDC_VERSION__ >= 202311L` |

### Phase 3: wasm32-wasi Only (Tier 3)

| File | Line | Feature | Blocked By |
|------|------|---------|------------|
| src/big.c | 55 | `auto` | Emscripten |

### Recommendations

1. **Compiler flag change:** `-std=c11` → `-std=c23` (GCC 15+) or `-std=c2x` (Clang)
2. **WASM strategy:** Consider wasm32-wasi for pure computation modules
3. **Estimated effort:** X hours
```

### 5. Apply changes (if --fix)

Only apply Tier 1 changes automatically. Tier 2+ require manual review.

## Compatibility Macros

For gradual migration, provide these macros:

```c
// c23_compat.h
#ifndef C23_COMPAT_H
#define C23_COMPAT_H

#if __STDC_VERSION__ >= 202311L
    // C23 native
    #define NODISCARD [[nodiscard]]
    #define MAYBE_UNUSED [[maybe_unused]]
    #define DEPRECATED(msg) [[deprecated(msg)]]
#elif defined(__GNUC__) || defined(__clang__)
    // GNU/Clang extensions
    #define NODISCARD __attribute__((warn_unused_result))
    #define MAYBE_UNUSED __attribute__((unused))
    #define DEPRECATED(msg) __attribute__((deprecated(msg)))
#else
    // Fallback - no effect
    #define NODISCARD
    #define MAYBE_UNUSED
    #define DEPRECATED(msg)
#endif

// nullptr compatibility
#if __STDC_VERSION__ >= 202311L
    // nullptr is a keyword
#elif !defined(nullptr)
    #define nullptr ((void*)0)
#endif

#endif // C23_COMPAT_H
```

## Example Output

```
/c23-modernize ralph

## C23 Modernization Analysis: ralph

**Compiler:** GCC (current: -std=c11)
**WASM target:** Emscripten (restricts to Tier 1-2)
**Recommendation:** Consider wasm32-wasi for pure solver code

### Findings

#### Tier 1: Safe to Apply (47 occurrences)

| Category | Count | Files |
|----------|-------|-------|
| NULL → nullptr | 23 | simplex.c, lu.c, branch_bound.c |
| {0} → {} | 15 | model.c, solution.c |
| Remove stdbool.h | 4 | ralph.h, types.h |
| (void)x → [[maybe_unused]] | 5 | callbacks.c |

#### Tier 2: Apply with Guards (12 occurrences)

| Category | Count | Notes |
|----------|-------|-------|
| #define → constexpr | 8 | Numeric constants in ralph.h |
| Add [[nodiscard]] | 4 | ralph_optimize, ralph_solve, etc. |

#### Tier 3: wasm32-wasi Only (blocked by Emscripten)

| Feature | Benefit | Workaround |
|---------|---------|------------|
| auto | Cleaner iterators | Keep explicit types |
| #embed | Embed test data | Keep xxd |

### Migration Plan

1. **Phase 1** (1 hour): Apply Tier 1 changes
2. **Phase 2** (30 min): Add [[nodiscard]] to public API
3. **Phase 3** (2 hours): Evaluate wasm32-wasi migration for Ralph

### WASM Strategy Recommendation

Ralph is pure computation with no browser API dependencies.
Migrating to wasm32-wasi would:
- Enable full C23 support (including `auto`, `#embed`)
- Reduce WASM binary size by ~60%
- Require custom JS loader (trivial for computation-only module)

Proceed with changes? [--fix to apply Tier 1]
```

## Questions to Clarify

Before major changes, ask:

1. "Is GCC 15+ available in CI/CD pipeline?"
2. "Should we maintain C11 compatibility fallbacks?"
3. "Is wasm32-wasi migration in scope for this module?"
4. "Any third-party consumers expecting specific C standard?"
