# OTTO Design Philosophy

This document explains the key design decisions behind OTTO.

## Why Build From Scratch?

OTTO is built from first principles for three reasons:

### 1. Control

Understanding exactly what the code does is non-negotiable for logistics optimization. When a truck arrives late or a fuel stop is suboptimal, we need to trace the decision back to specific constraints and costs. Black-box dependencies make this impossible.

Ralph (the LP/MIP solver) exists because we need to:
- Debug infeasible models by inspecting constraint violations
- Tune solver behavior for logistics-specific patterns
- Understand numerical precision issues in real-world scenarios

If we ever hit limitations, Ralph's API can wrap HiGHS (a mature open-source solver) without changing the domain code.

### 2. Agentic Coding Experiment

OTTO is also a testbed for exploring how far AI-assisted development can go. The codebase is structured for effective human-AI collaboration:

- **Comprehensive CLAUDE.md files** at every level with API details, patterns, and pitfalls
- **Defined skills** (`/c-audit`, `/api-run`) that encapsulate common workflows
- **Consistent patterns** (naming conventions, error handling, memory management) that an agent can learn and apply
- **Clear module boundaries** with explicit dependencies

The question "how far can this go?" is genuinely open. OTTO is a stress test because it has real algorithmic complexity (LP solvers, routing algorithms, spatial indexing), not just CRUD operations.

### 3. C as Lingua Franca

C is the one language where:
- WASM compilation is straightforward (no runtime, no GC)
- FFI from any other language is trivial
- No hidden allocations or control flow
- The binary does exactly what the source says
- It'll still compile in 30 years

The maintenance burden of implementing primitives (protobuf, zlib, geocoding) is real but bounded. We're not reimplementing OpenSSL—just the specific pieces needed for the domain.

## Why C11?

OTTO is written in **C11** for these key advantages:

### Language Features That Matter

| Feature | C11 | Why It Matters |
|---------|-----|----------------|
| **Compound literals** | `(Type){.field = val}` | Inline struct construction |
| **Designated initializers** | `{.x = 1, .y = 2}` | Self-documenting initialization |
| **Anonymous structs/unions** | Nested without names | Cleaner state hierarchies |
| **`_Static_assert`** | Compile-time checks | Catch errors at build time |
| **`_Generic`** | Type-generic macros | Safe type dispatch |
| **`_Thread_local`** | Thread-local storage | Per-thread state without locks |

### Compound Literals as First-Class Concepts

```c
/* Inline configuration - no separate variable */
cs_button(CS_ID("save"), "Save", &(CsButtonStyle){
    .variant = CS_BTN_PRIMARY,
    .font_size = 14,
    .corner_radius = 4
});

/* State initialization with clear defaults */
static AppState g_app = {
    .map = { .lat = 47.4979, .lon = 19.0402, .zoom = 12 },
    .panels = { .show_tile_info = true },
};
```

### Why Not Other Languages?

| Language | Why Not |
|----------|---------|
| **C++** | 200KB+ WASM (exceptions, RTTI), slow compile |
| **Rust** | 150KB+ WASM baseline, borrow checker friction for graphs |
| **Zig** | Immature ecosystem, unstable spec |
| **Go** | 2MB+ WASM (runtime + GC), no memory control |
| **Java** | No viable WASM path, GC pauses |
| **TypeScript** | GC overhead, no SIMD, can't match native perf |

**The C Sweet Spot for OTTO's requirements:**

| Requirement | C Advantage |
|-------------|-------------|
| **Small WASM** | 50-150KB typical, no runtime |
| **Predictable perf** | No GC, no JIT, no surprises |
| **Memory control** | Arena allocators, cache-friendly layouts |
| **FFI simplicity** | Direct function exports, no marshaling |
| **Compile speed** | Seconds, not minutes |

## Why Zero Dependencies?

Every external dependency is a liability. OTTO's core depends only on standard C.

### What Zero Dependencies Enables

| Benefit | Explanation |
|---------|-------------|
| **Deterministic builds** | Same source = same binary |
| **Auditable code** | Every line can be inspected |
| **Minimal attack surface** | No supply chain vulnerabilities |
| **Fast compilation** | No dependency resolution |
| **WASM size control** | Include only what you use |
| **Cross-platform** | No platform-specific deps |

### The Cost (And Why It's Worth It)

We implement some things ourselves:
- **LP solver** (Ralph) - 3000 lines vs pulling GLPK/CLP
- **Routing** (Velo) - 2000 lines vs pulling OSRM
- **Compression** (miniz) - Single-file zlib alternative

This code is tailored to our needs, fully understood, and modifiable without upstream coordination.

## Why WebAssembly (WASM)?

WASM is a first-class deployment target.

### WASM Advantages

| Property | Benefit |
|----------|---------|
| **Sandboxed execution** | Can't access filesystem/network directly |
| **Near-native speed** | LP solving at 80-90% native perf |
| **Portable bytecode** | Same binary runs everywhere |
| **No installation** | Link in HTML, instant deployment |
| **JavaScript interop** | Seamless FFI with typed arrays |
| **Streaming compilation** | Start running while downloading |

### Design Constraints for WASM

1. **No threads in hot paths** - WASM threads have limitations
2. **Fixed-size allocations** - Arena allocators, no unbounded malloc
3. **Explicit exports** - `EMSCRIPTEN_KEEPALIVE` on public functions
4. **No filesystem assumptions** - Data passed via memory
5. **No global constructors** - Explicit `_init()` functions

## Module Design

### Layered Architecture

```
┌───────────────────────────────────────────┐
│  Applications (UI, API, CLI)              │ ← User-facing
├───────────────────────────────────────────┤
│  Domain Libraries (FuelWise, HoSE, etc.)  │ ← Business logic
├───────────────────────────────────────────┤
│  Core Engines (Ralph, Velo, Carta, Locus) │ ← Algorithms
├───────────────────────────────────────────┤
│  Shared Utilities (geo, protobuf)         │ ← Common code
└───────────────────────────────────────────┘
```

**Rules:**
- Lower layers never depend on higher layers
- Each layer exposes a clean C API
- State is passed explicitly (no hidden globals)

### State Management Pattern

Single `AppState` structure per application:

```c
typedef struct {
    MapState map;
    UIPanels panels;
    UIText text;
} AppState;

static AppState g_app = {
    .map = { .lat = 47.5, .lon = 19.0, .zoom = 12 },
    .panels = { .show_tile_info = true },
};
```

**Benefits:** Clear ownership, easy serialization, simple debugging.

### Component API Design (Immediate Mode)

```c
/* Call component, check result, react */
CsButtonResult r = cs_button(CS_ID("save"), "Save", &style);
if (r.clicked) {
    save_document();
}
```

**Properties:**
- No retained state outside explicit buffers
- Result fully determined by inputs
- Caller owns all data
- Components are pure functions + global interaction state

## UI System Design

OTTO combines two paradigms:

| Layer | Style | Purpose |
|-------|-------|---------|
| **Clay** | Declarative | Layout computation |
| **ClayShards** | Immediate mode | Interaction handling |

```c
/* Declarative layout wraps immediate mode interaction */
CLAY(CLAY_ID("Panel"), { .layout = { .padding = CLAY_PADDING_ALL(16) } }) {
    if (cs_button(CS_ID("btn"), "Click", NULL).clicked) {
        handle_click();
    }
}
```

**ClayShards internals:**
- Thread-local storage for multi-threaded use
- Custom allocator support
- Error tracking API
- Iterative Douglas-Peucker for stack-safe polyline simplification

## Error Handling

```c
/* Errors are values, not exceptions */
typedef struct {
    bool ok;
    const char *error;
} Result;

Result r = do_something();
if (!r.ok) {
    log_error(r.error);
    return;
}
```

**No hidden control flow. No exceptions. No surprises.**

## Scaling Philosophy

OTTO distinguishes **engines** (novel logic) from **infrastructure** (commodity ops):

| Concern | Solution | Engine? |
|---------|----------|---------|
| More capacity | Horizontal scaling (K8s) | No |
| Faster responses | Caching (Redis, CDN) | No |
| Tile distribution | CDN edge caching | No |
| Async job execution | **Forge** | Yes |
| Pre-computation strategy | **Apex** | Yes |
| Natural language interface | **Iris** | Yes |

**Key insight:** Velo, Carta, Locus are stateless. Scaling them is infrastructure. But deciding *what* to pre-compute (Apex) and *how* to interpret user intent (Iris) is novel logic.

**Deployment tiers (same codebase):**
- **Local:** Single process, in-memory queue
- **Small business:** Redis-backed job queue
- **Enterprise:** Kubernetes, horizontal scaling
- **Managed:** We run it, you use it

## C Memory Safety

### Arena Allocation (Preferred)

```c
Arena arena = arena_create(buffer, size);
Node *nodes = arena_alloc(&arena, n * sizeof(Node));
/* ... use nodes ... */
arena_reset(&arena);  /* Free everything at once */
```

**Benefits:** No double-free, no dangling pointers, no fragmentation.

### Ownership Rules

```c
/* _create() implies _destroy() */
Graph *g = graph_create();
graph_destroy(g);
g = NULL;  /* Prevent use-after-free */
```

### Buffer Safety

```c
/* NEVER: strcpy, sprintf, gets */
/* ALWAYS: strncpy, snprintf with size */
snprintf(buf, sizeof(buf), "%s", str);
```

### Build Flags for Safety

```bash
# Warnings
gcc -Wall -Wextra -Werror -Wconversion -Wshadow

# Sanitizers
gcc -fsanitize=address,undefined -g

# Valgrind
valgrind --leak-check=full ./test_runner
```

### WASM Defense-in-Depth

Even buggy C code is partially contained in WASM:
- Linear memory is bounds-checked
- Cannot access host memory
- Stack overflow traps instead of corrupting
- No arbitrary code execution

This doesn't excuse bugs, but limits blast radius.
