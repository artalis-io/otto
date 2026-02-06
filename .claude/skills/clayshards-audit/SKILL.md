---
name: clayshards-audit
description: Comprehensive audit of ClayShards module - C code security, JavaScript WebGL/WASM safety, and adherence to the ClayShards Manifesto principles.
user-invocable: true
---

# ClayShards Comprehensive Audit Skill

Perform a comprehensive audit of the ClayShards module covering:
1. **C Code Security** - Memory safety, OTTO patterns, thread safety
2. **JavaScript Security** - WebGL resources, WASM interop, API resilience
3. **Manifesto Compliance** - Adherence to ClayShards design principles

**Target:** `clayshards/` directory (automatically scoped)

## Usage

```
/clayshards-audit              # Full audit of all ClayShards code
/clayshards-audit --c-only     # C code audit only
/clayshards-audit --js-only    # JavaScript audit only
/clayshards-audit --manifesto  # Manifesto compliance check only
/clayshards-audit --fix        # Audit and apply fixes where possible
```

---

## Part 1: C Code Audit

Scope: `clayshards/clay-shards/` (C library)

### Files to Audit

```
clayshards/clay-shards/
├── include/
│   ├── cs_common.h, cs_clay.h, cs_button.h, cs_input.h
│   ├── cs_checkbox.h, cs_toggle.h, cs_slider.h, cs_dropdown.h
│   ├── cs_scroll.h, cs_map.h, cs_immediate.h
├── src/
│   ├── cs_common.c, cs_clay.c, cs_button.c, cs_input.c
│   ├── cs_checkbox.c, cs_toggle.c, cs_slider.c, cs_dropdown.c
│   ├── cs_scroll.c, cs_map.c, cs_map_projection.c, cs_map_simplify.c
│   ├── cs_immediate.c, cs_internal.h, cs_map_internal.h
└── tests/
```

### C Audit Categories

#### 1.1 Memory Safety (Critical)
| Issue | Pattern | Severity |
|-------|---------|----------|
| Buffer overflow | `strcpy`, `strcat`, `sprintf` without bounds | Critical |
| Integer overflow | `malloc(a * b)` without overflow check | Critical |
| Use-after-free | Pointer used after `free()` | Critical |
| Null dereference | Pointer used without NULL check | High |
| Missing null terminator | `strncpy` without explicit `\0` | High |

#### 1.2 ClayShards-Specific Checks
| Issue | What to Check |
|-------|---------------|
| Widget state hash collision | IDs collision detection in debug builds |
| Focus model consistency | Tab order matches widget registration |
| TLS correctness | Thread-local storage properly initialized |
| Custom allocator safety | Null checks on allocator function pointers |
| Error tracking | `cs_get_last_error()` set on failure paths |

#### 1.3 TUI Compatibility Checks (Critical)
| Issue | Pattern | Severity |
|-------|---------|----------|
| Input after render | State mutation after CLAY() block | High |
| Border-based focus | `config.border.width` in focus handling | Medium |
| Fixed pixel sizes | Hardcoded heights >3 without TUI fallback | Low |
| Missing state update | `*value = x` without updating local copy | High |

**Input-Before-Render Pattern:**
```c
// BAD: Input processed after rendering = 1-frame lag in TUI
CLAY(...) { render(current_value); }
if (g->pending_arrow) { *value = new_value; }  // Too late!

// GOOD: Input before render = immediate response
if (g->pending_arrow) {
    *value = new_value;
    current_value = *value;  // Update local for rendering
}
CLAY(...) { render(current_value); }
```

**Focus Indication Pattern:**
```c
// BAD: Borders shift layout in TUI (1 char per side)
if (is_focused) {
    config.border.width = {2, 2, 2, 2, 0};
}

// GOOD: Background color change, zero layout impact
Clay_Color bg = is_focused
    ? (Clay_Color){CS_COLOR_BTN_BLUE_FOCUS}
    : (Clay_Color){CS_COLOR_BTN_BLUE};
```

#### 1.3 OTTO Naming Conventions
| Component | Required Prefix |
|-----------|-----------------|
| ClayShards functions | `cs_` |
| ClayShards types | `Cs` |
| ClayShards constants | `CS_` |
| Internal functions | `cs_internal_` or static |

#### 1.4 Thread Safety
- **Library code**: No static/global mutable state (use TLS or context)
- **TLS macros**: Verify `CS_THREAD_LOCAL` works on all targets
- **Clay integration**: Document single-thread requirement for Clay

---

## Part 2: JavaScript Audit

Scope: `clayshards/clay-shards-webgl/`, `clayshards/clay-shards-demo/`

### Files to Audit

```
clayshards/clay-shards-webgl/
├── renderer.js, font.js, shaders.js, utils.js
├── map-tiles.js, map-overlays.js, map-provider.js
├── wasm-loader.js, wasm-map-provider.js
├── render-loop.js, keyboard.js, text-cursor.js
├── text-input-overlay.js, index.js

clayshards/clay-shards-demo/
├── demo.js
├── offline/demo-offline.js
```

### JS Audit Categories

#### 2.1 WASM-JavaScript Boundary (Critical)
| Issue | Pattern | Severity |
|-------|---------|----------|
| Memory view invalidation | TypedArray held across WASM calls | Critical |
| Unbounded memory read | No bounds check on `new Uint8Array(buffer, ptr, len)` | Critical |
| Missing null terminator | String to WASM without `\0` | High |
| Missing export validation | WASM function called without existence check | Medium |

#### 2.2 WebGL Resources (High)
| Issue | Pattern | Severity |
|-------|---------|----------|
| Shader compile unchecked | `gl.compileShader()` without status check | High |
| Texture leak | `createTexture()` without `deleteTexture()` | Medium |
| Context loss ignored | No `webglcontextlost` handler | High |
| Unbounded cache | Texture/tile cache without eviction | Medium |

#### 2.3 API Client Resilience (High)
| Issue | Pattern | Severity |
|-------|---------|----------|
| No circuit breaker | Direct fetch without circuit breaker | High |
| No retry/backoff | Single fetch without retry logic | High |
| Missing status handling | Not handling 429/503/504 | High |
| No timeout | fetch without AbortController | Medium |

Required: Use `shared/js/` resilience utilities:
- `createResilientFetch()` - Fetch wrapper with retry + circuit breaker
- `CircuitBreaker` - Circuit breaker class
- `createBackoff()` - Exponential backoff iterator

#### 2.4 URL and Input Security (Medium)
| Issue | Pattern | Severity |
|-------|---------|----------|
| Missing encodeURIComponent | User input in URL query params | High |
| Unvalidated tile URL | User-controlled tile server URL | Medium |
| Clipboard without try/catch | `navigator.clipboard` without error handling | Medium |

---

## Part 3: Manifesto Compliance

Reference: `clayshards/clay-shards/MANIFESTO.md`

### Core Principles Checklist

#### 3.1 Immediate Mode Interface
- [ ] UI rebuilt each frame (no retained widget tree)
- [ ] No DOM, no reconciliation/diffing
- [ ] Components return results immediately (`CsButtonResult`, etc.)

**Check:** Look for retained state that violates immediate mode:
```c
// BAD: Retained button state
static bool button_pressed = false;

// GOOD: Immediate mode - returns result
CsButtonResult result = cs_button(id, label, style);
if (result.clicked) { ... }
```

#### 3.2 Declarative Layout + Imperative Components
- [ ] Clay handles layout ("where and how big")
- [ ] ClayShards handles behavior ("what does it do")
- [ ] Components internally call Clay API

**Check:** Components should use Clay for layout:
```c
// GOOD: Component uses Clay internally
CsButtonResult cs_button(uint32_t id, const char *label, ...) {
    CLAY(CLAY_ID_LOCAL("button"), ...) {
        // Layout via Clay
    }
    // Behavior via ClayShards
}
```

#### 3.3 Stable Identity via String Hash
- [ ] IDs derived from FNV-1a string hash
- [ ] `CS_ID(name)` macro for all components
- [ ] No implicit ID stacks
- [ ] Duplicate ID detection in debug builds

**Check:** All component calls use explicit IDs:
```c
// GOOD
cs_button(CS_ID("submit"), "Submit", NULL);
cs_input(CS_ID("search"), text, &len, max, "Search...", NULL);

// BAD: Magic numbers or implicit IDs
cs_button(12345, "Submit", NULL);
```

#### 3.4 State Ownership (Dear ImGui Model)
- [ ] Business state owned by application (passed by pointer)
- [ ] UI internal state owned by ClayShards (keyed by widget ID)
- [ ] Widgets may mutate business state inline

**Check:** APIs pass pointers for app state:
```c
// GOOD: App owns lat/lon/zoom, ClayShards modifies via pointer
CsMapResult cs_map(uint32_t id, double *lat, double *lon, int *zoom, ...);

// BAD: ClayShards owns business state
static double internal_lat;  // WRONG
```

#### 3.5 Full Redraw Default
- [ ] No partial redraw requirement
- [ ] Frame model: input → UI code → layout → render commands → backend

**Check:** No incremental update assumptions in component code.

#### 3.6 Render Commands Contract
- [ ] ClayShards produces Clay render commands
- [ ] No global GL state assumptions in library code
- [ ] Renderer backends consume commands independently

**Check:** Library code should NOT:
- Call `gl.*` directly
- Assume any graphics context
- Store rendering state

#### 3.7 Determinism
- [ ] Same inputs → same outputs
- [ ] No time-dependent behavior in layout (animation is delta-based)
- [ ] Testable and reproducible

**Check:** No `rand()`, `time()`, or non-deterministic behavior in layout/component logic.

### 3.8 TUI Renderer Portability

Components must work across all renderer targets. TUI is the strictest test:

- [ ] **Input before render**: All state mutations happen before `CLAY()` blocks
- [ ] **Focus via background color**: No `border.width` changes for focus indication
- [ ] **Local value updates**: After `*value = x`, also update `current_value = *value`
- [ ] **Transparent overlay handling**: Dropdown items use solid background, not transparent

**Why TUI is the strictest renderer:**
- Differential updates only redraw changed cells (timing bugs visible)
- Borders add 1 character width (layout shifts visible)
- Character grid exposes coordinate assumptions
- No anti-aliasing hides alignment issues

### Non-Goals Verification
Ensure ClayShards does NOT:
- [ ] Implement a DOM
- [ ] Use retained-mode patterns
- [ ] Require diffing or virtual DOM
- [ ] Need runtime reflection
- [ ] Require garbage collection

---

## Audit Procedure

When `/clayshards-audit` is invoked:

### Step 1: C Code Audit
1. Scan `clayshards/clay-shards/src/*.c` for:
   - Unsafe string functions (`strcpy`, `sprintf`, etc.)
   - Missing NULL checks
   - Memory leaks
   - Thread safety issues
2. Verify OTTO naming conventions (`cs_*`, `Cs*`, `CS_*`)
3. Check error handling (`cs_get_last_error()` usage)

### Step 2: JavaScript Audit
1. Scan `clayshards/clay-shards-webgl/*.js` for:
   - WASM memory boundary issues
   - WebGL resource leaks
   - Missing error handling
2. Scan `clayshards/clay-shards-demo/*.js` for:
   - API client resilience (circuit breaker, retry)
   - URL construction safety
3. Verify use of `shared/js/` resilience utilities

### Step 3: Manifesto Compliance
1. Review component APIs for immediate mode pattern
2. Verify state ownership model
3. Check for Clay integration patterns
4. Ensure determinism requirements met

### Step 4: Generate Combined Report

---

## Report Format

```markdown
# ClayShards Comprehensive Audit Report

**Date:** YYYY-MM-DD
**Auditor:** Claude Code (clayshards-audit skill)

## Summary

| Category | Files | Issues |
|----------|-------|--------|
| C Code | N | Critical: N, High: N, Medium: N, Low: N |
| JavaScript | N | Critical: N, High: N, Medium: N, Low: N |
| Manifesto | - | Compliant: N/N |

## Part 1: C Code Audit

### Critical Issues
| File:Line | Issue | Current Code | Suggested Fix |
|-----------|-------|--------------|---------------|

### High Issues
...

### OTTO Pattern Compliance
- [ ] Naming conventions followed
- [ ] Error handling consistent
- [ ] Thread safety verified

## Part 2: JavaScript Audit

### Critical Issues
| File:Line | Issue | Current Code | Suggested Fix |
|-----------|-------|--------------|---------------|

### WebGL Resource Management
- [ ] Shader compilation checked
- [ ] Context loss handled
- [ ] Texture cache bounded

### API Client Resilience
- [ ] Uses shared/js resilience utilities
- [ ] Circuit breaker implemented
- [ ] Retry with backoff

## Part 3: Manifesto Compliance

### Principle Compliance

| Principle | Status | Notes |
|-----------|--------|-------|
| Immediate mode interface | ✅/⚠️/❌ | |
| Declarative layout + imperative components | ✅/⚠️/❌ | |
| Stable identity via string hash | ✅/⚠️/❌ | |
| State ownership (Dear ImGui model) | ✅/⚠️/❌ | |
| Full redraw default | ✅/⚠️/❌ | |
| Render commands contract | ✅/⚠️/❌ | |
| Determinism | ✅/⚠️/❌ | |
| TUI renderer portability | ✅/⚠️/❌ | |

### TUI Compatibility
| Check | Status | Notes |
|-------|--------|-------|
| Input before render | ✅/⚠️/❌ | State changes before CLAY() blocks |
| Focus via background | ✅/⚠️/❌ | No border-based focus indication |
| Local value updates | ✅/⚠️/❌ | current_value updated after *value |
| Solid overlay backgrounds | ✅/⚠️/❌ | No transparent dropdown items |

### Violations Found
...

## Recommendations

1. **C Code**: ...
2. **JavaScript**: ...
3. **Manifesto**: ...

## Overall Assessment

[Summary statement about codebase health]
```

---

## Fix Mode (--fix)

When `--fix` is specified:

**C Auto-fixable:**
- `strcpy` → `strncpy` with null terminator
- `sprintf` → `snprintf`
- Missing NULL checks (add early return)
- Unused variables (remove)

**JS Auto-fixable:**
- Missing `encodeURIComponent`
- `==` → `===`
- Missing WebGL status checks

**NOT Auto-fixable:**
- WASM memory boundary redesign
- State ownership refactoring
- Manifesto architectural violations

---

## Integration

This skill combines:
- `/c-audit` patterns for C code (memory safety, OTTO conventions)
- `/js-audit` patterns for JavaScript (WebGL, WASM, API resilience)
- ClayShards Manifesto compliance checking

Run regularly during ClayShards development to ensure code quality and architectural consistency.
