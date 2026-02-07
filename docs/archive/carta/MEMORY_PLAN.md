# Carta Memory Management & Code Quality Review Plan

## Executive Summary

**Ralph's approach (to replicate):**
1. **Arena allocator** for fixed-size, same-lifetime arrays (single malloc, bulk reset)
2. **Contiguous memory pools** for growing data (offset-based access, no pointer-of-pointers)
3. **SAFE_FREE macro** for post-free hygiene
4. **Hybrid strategy** based on data characteristics

**Carta's current state:**
- Traditional malloc/calloc/realloc/free throughout
- Some good patterns exist (scale buffer pre-allocation, string pool in serialization)
- No shared arena infrastructure
- ~15+ free() calls in context cleanup
- Fragmentation risk in PBF parsing (millions of small allocations)

---

## Phase 1: Infrastructure Setup

### 1.1 Create shared arena allocator
**Files:** `shared/include/sh_arena.h`, `shared/src/sh_arena.c`

- Port Ralph's `RalphArena` pattern to shared library as `SHArena`
- Functions: `sh_arena_create()`, `sh_arena_alloc()`, `sh_arena_calloc()`, `sh_arena_reset()`, `sh_arena_free()`
- 8-byte alignment for double/pointer safety

### 1.2 Create shared memory pool
**Files:** `shared/include/sh_pool.h`, `shared/src/sh_pool.c`

- Contiguous buffer with offset tracking (like Ralph's spike pool)
- Functions: `sh_pool_create()`, `sh_pool_alloc()`, `sh_pool_remaining()`, `sh_pool_reset()`, `sh_pool_free()`

### 1.3 Add SAFE_FREE macro
**File:** `shared/include/shared.h`

```c
#define SAFE_FREE(p) do { free(p); (p) = NULL; } while(0)
```

---

## Phase 2: Code Quality & Security Audit

### 2.1 Security/safety issues to check
- Buffer overflows (unbounded string ops, array access without bounds checking)
- Integer overflow in allocation size calculations
- Use-after-free patterns
- NULL pointer dereference
- Uninitialized variables
- Format string vulnerabilities

### 2.2 Coding standards to verify
- Consistent error handling patterns
- Proper return value checking
- Consistent naming conventions
- Documentation of ownership semantics
- Proper const correctness

### 2.3 Files to audit (in order)

| File | Lines | Priority | Risk Areas |
|------|-------|----------|------------|
| `ct_pbf.c` | 2127 | HIGH | PBF parsing, memory allocation storms |
| `ct_render.c` | ~500 | HIGH | Pixel buffer bounds, coordinate transforms |
| `ct_mvt.c` | ~800 | MEDIUM | Protobuf encoding, geometry commands |
| `ct_png.c` | ~350 | MEDIUM | DEFLATE, buffer sizes |
| `ct_rtree.c` | ~600 | MEDIUM | R-Tree construction, node access |
| `ct_cache.c` | ~400 | MEDIUM | LRU, hash collisions |
| `ct_multipolygon.c` | ~1000 | MEDIUM | Ring assembly, recursive traversal |
| `ct_simplify.c` | ~300 | LOW | Douglas-Peucker stack |
| `ct_label.c` | ~400 | LOW | Collision detection |
| `ct_tile.c` | ~400 | LOW | Web Mercator math |

---

## Phase 3: Arena Integration (High-Impact)

### 3.1 PBF Context arena
Replace 20+ individual allocations:
- Calculate total size from PBF header counts (nodes, ways, relations)
- Arena for: hash map keys/values, way name strings, labeled point names
- Estimated reduction: 15-20 malloc calls → 1

### 3.2 Way coordinate pool
Contiguous storage for all way coordinates:
- Single large buffer for all `CTOSMWay.coords`
- Each way stores offset + count instead of pointer
- Estimated: 100K-1M individual mallocs → 1

### 3.3 Multipolygon assembly arena
- Per-multipolygon arena for ring coordinates
- Reset after serialization/indexing

---

## Phase 4: Per-Tile Memory Optimization

### 4.1 Render arena (thread-local)
- Pre-allocate per-tile working memory
- Reset between tiles (no malloc/free per tile)
- Include: filtered scanlines, compressed buffer, geometry scratch

### 4.2 MVT encoding arena
- Per-layer geometry command buffer
- Coordinate transform scratch space
- Reset between layers

---

## Phase 5: Validation & Testing

### 5.1 Ensure all existing tests pass (33 tests)
```bash
make test-carta
```

### 5.2 Add memory safety tests
- Test arena overflow handling
- Test pool capacity exhaustion
- Test thread-safety with multiple render contexts

### 5.3 Valgrind/AddressSanitizer validation
```bash
CFLAGS="-fsanitize=address,undefined" make carta
./carta/build/test_carta
```

---

## Implementation Order (Recommended)

1. **Phase 1.3** - Add SAFE_FREE macro (minimal change, immediate benefit)
2. **Phase 2** - Security/coding audit (identify issues before refactoring)
3. **Phase 1.1-1.2** - Create shared arena/pool infrastructure
4. **Phase 3.1** - PBF context arena (highest impact)
5. **Phase 3.2** - Way coordinate pool (second highest impact)
6. **Phase 4** - Per-tile optimizations
7. **Phase 5** - Validation

---

## Risk Assessment

| Change | Risk | Mitigation |
|--------|------|------------|
| SAFE_FREE macro | Low | Drop-in replacement |
| Arena infrastructure | Low | New code, no existing changes |
| PBF context arena | Medium | Requires careful API changes |
| Way coordinate pool | High | Changes CTOSMWay struct layout |
| Per-tile arena | Medium | Thread-local complexity |

---

## Estimated Impact

| Metric | Current | After |
|--------|---------|-------|
| malloc/free calls in PBF parse | 1M+ | ~10 |
| Context cleanup complexity | 15+ free() | 2-3 arena_free() |
| Memory fragmentation | High | Minimal |
| Cache locality | Poor (scattered) | Good (contiguous) |
