# OTTO Design Manifesto

This document explains the design philosophy behind OTTO: why we build from scratch, why C, why zero dependencies, and how the agnostic design pattern applies to both frontend (renderer-agnostic) and backend (transport-agnostic).

---

## 1. Why Build From Scratch

OTTO is built from first principles for three reasons:

### 1.1 Control

Understanding exactly what the code does is non-negotiable for logistics optimization. When a truck arrives late or a fuel stop is suboptimal, we need to trace the decision back to specific constraints and costs. Black-box dependencies make this impossible.

Ralph (the LP/MIP solver) exists because we need to:
- Debug infeasible models by inspecting constraint violations
- Tune solver behavior for logistics-specific patterns
- Understand numerical precision issues in real-world scenarios

If we ever hit limitations, Ralph's API can wrap HiGHS (a mature open-source solver) without changing the domain code.

### 1.2 Agentic Coding Experiment

OTTO is also a testbed for exploring how far AI-assisted development can go. The codebase is structured for effective human-AI collaboration:

- **Comprehensive CLAUDE.md files** at every level with API details, patterns, and pitfalls
- **Defined skills** (`/c-audit`, `/api-run`) that encapsulate common workflows
- **Consistent patterns** (naming conventions, error handling, memory management) that an agent can learn and apply
- **Clear module boundaries** with explicit dependencies

The question "how far can this go?" is genuinely open. OTTO is a stress test because it has real algorithmic complexity (LP solvers, routing algorithms, spatial indexing), not just CRUD operations.

### 1.3 C as Lingua Franca

C is the one language where:
- WASM compilation is straightforward (no runtime, no GC)
- FFI from any other language is trivial
- No hidden allocations or control flow
- The binary does exactly what the source says
- It'll still compile in 30 years

The maintenance burden of implementing primitives (protobuf, zlib, geocoding) is real but bounded. We're not reimplementing OpenSSL—just the specific pieces needed for the domain.

---

## 2. Why C11

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

### Why Not Other Languages?

| Language | Why Not |
|----------|---------|
| **C++** | 200KB+ WASM (exceptions, RTTI), slow compile |
| **Rust** | 150KB+ WASM baseline, borrow checker friction for graphs |
| **Zig** | Immature ecosystem, unstable spec |
| **Go** | 2MB+ WASM (runtime + GC), no memory control |
| **Java** | No viable WASM path, GC pauses |
| **TypeScript** | GC overhead, no SIMD, can't match native perf |

**The C Sweet Spot:**

| Requirement | C Advantage |
|-------------|-------------|
| **Small WASM** | 50-150KB typical, no runtime |
| **Predictable perf** | No GC, no JIT, no surprises |
| **Memory control** | Arena allocators, cache-friendly layouts |
| **FFI simplicity** | Direct function exports, no marshaling |
| **Compile speed** | Seconds, not minutes |

---

## 3. Why Zero Dependencies

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

---

## 4. Why WebAssembly

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

### WASM Defense-in-Depth

Even buggy C code is partially contained in WASM:
- Linear memory is bounds-checked
- Cannot access host memory
- Stack overflow traps instead of corrupting
- No arbitrary code execution

This doesn't excuse bugs, but limits blast radius.

---

## 5. The Agnostic Design Pattern

OTTO applies the same pattern to both frontend and backend:

| Layer | Principle | Implementations |
|-------|-----------|-----------------|
| **Frontend (ClayShards)** | UI is renderer-agnostic | WebGL, TUI, framebuffer |
| **Backend (APIs)** | Logic is transport-agnostic | HTTP, WASM, socket, embedded |

The core code doesn't know or care how it's being rendered or called. Renderers and transports are thin wrappers.

---

## 6. Frontend: Renderer-Agnostic UI (ClayShards)

### First Principles

**1) Immediate mode is the interface**

UI is authored as code that is executed every frame. There is no retained widget tree, no DOM, and no reconciliation/diffing.

> The UI is rebuilt each frame because it is simpler, more predictable, and more portable.

**2) Layout is declarative; components are imperative**

ClayShards combines:
- **Declarative layout composition** (Clay)
- **Imperative immediate-mode components** (widgets)

Layout answers *"where and how big?"*
Components answer *"what is it, how does it behave, what does it do?"*

**3) Stable identity via string hash**

Every component has a stable ID derived from a **string hash** (FNV-1a):

```c
cs_button(CS_ID("submit"), "Submit", NULL);
cs_input(CS_ID("search"), text, &len, max, "Search...", NULL);
```

**4) State ownership follows Dear ImGui model**

- **Business state**: owned by the application (C structs, passed by pointer)
- **UI internal state**: owned by ClayShards (keyed by widget ID)

**The application owns what the widget represents. ClayShards owns how the widget behaves.**

**5) Rendering uses Clay's render commands**

The render command array is the contract:
- UI and layout produce render commands
- Renderer backends consume render commands
- ClayShards does **not** manage rendering state

This separation is what makes "render into anything" real.

### Renderer Portability

ClayShards targets multiple backends:
- **WebGL** — pixel coordinates, GPU-accelerated
- **TUI** — character cell coordinates, terminal escape sequences

> **Design principle:** If a widget works in TUI, it works everywhere. TUI is the strictest renderer—it exposes timing bugs, layout assumptions, and focus handling issues that GPU renderers hide.

### Non-Goals

ClayShards is **not**:
- a DOM
- a retained-mode UI framework
- a React clone
- a CSS engine

### The ClayShards Promise

- **The same UI code runs across targets.**
- Your UI will run in **WASM/WebGL** and **embedded** without rewriting.
- Your business logic remains in **C**.
- Your renderer remains **replaceable** because it consumes render commands.

---

## 7. Backend: Transport-Agnostic APIs

### First Principles

**1) The core is a pure function**

Every OTTO API endpoint is fundamentally:

```c
int carta_render_tile(int z, int x, int y, uint8_t **out, size_t *out_len);
int vl_route(VLGraph *g, int from, int to, VLRoute *route);
int lc_search(LCIndex *idx, const char *query, LCResult *results);
int fw_optimize(FWProblem *p, FWSolution *s);
```

No HTTP. No request objects. No response writers. Just input → output.

**2) Transport is a thin wrapper**

Mongoose HTTP is one transport:

```c
static void handle_tile(struct mg_connection *c, struct mg_http_message *hm) {
    int z = parse_int(hm, "z");
    int x = parse_int(hm, "x");
    int y = parse_int(hm, "y");

    uint8_t *png; size_t len;
    int status = carta_render_tile(z, x, y, &png, &len);

    mg_http_reply(c, status == 0 ? 200 : 500,
                  "Content-Type: image/png\r\n", "%.*s", len, png);
}
```

WASM is another transport:

```c
EMSCRIPTEN_KEEPALIVE
int carta_api_handle(const char *path, uint8_t **out, size_t *out_len) {
    int z, x, y;
    parse_tile_path(path, &z, &x, &y);
    return carta_render_tile(z, x, y, out, out_len);
}
```

The core function is identical. Only the wrapper changes.

**3) The demo IS the product**

Because everything compiles to WASM:
- The browser demo runs the actual tile renderer
- The browser demo runs the actual routing engine
- The browser demo runs the actual geocoder
- The browser demo runs the actual optimizer

Not a simulation. Not a mockup. The real algorithms, in the browser.

**4) Zero-infrastructure evaluation**

Send a prospect a single HTML file with embedded WASM and Monaco PBF data:
- No Docker
- No AWS credentials
- No server to provision
- No network required

They open it, click "Try it", and the real API runs locally.

### Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         Application                              │
│  React UI │ CLI tool │ Browser demo │ Embedded system           │
├─────────────────────────────────────────────────────────────────┤
│                    Transport Layer (thin)                        │
│  Mongoose HTTP │ WASM+JS │ Unix socket │ Direct C call          │
├─────────────────────────────────────────────────────────────────┤
│                    Core API (pure C)                             │
│  carta_render_tile() │ vl_route() │ lc_search() │ fw_optimize() │
└─────────────────────────────────────────────────────────────────┘
```

The transport layer is ~50-100 lines per API. The core is thousands.

### Benefits

**For Sales:**
- Demo without infrastructure - Prospects try the real product instantly
- Proof by demonstration - "Will it work on edge?" → "It works in your browser"

**For Development:**
- Test without servers - Unit test the core functions directly
- Debug in browser - Full DevTools access to the actual algorithms
- Single codebase - No separate "demo mode" or "lite version"

**For Deployment:**
- Edge-ready by design - If it runs in WASM, it runs anywhere
- Consistent behavior - Same code path in all environments

### The Transport-Agnostic Promise

- **The same API code runs across transports.**
- Your API will run as **HTTP server**, **WASM module**, and **embedded library** without rewriting.
- Your business logic remains in **pure C**.
- Your transport remains **replaceable** because it's just a thin wrapper.

---

## 8. Module Architecture

### Layered Design

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

### Error Handling

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

---

## 9. Memory Safety

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

---

## 10. Scaling Philosophy

OTTO distinguishes **engines** (novel logic) from **infrastructure** (commodity ops):

| Concern | Solution | Engine? |
|---------|----------|---------|
| More capacity | Horizontal scaling (K8s) | No |
| Faster responses | Caching (Redis, CDN) | No |
| Tile distribution | CDN edge caching | No |
| Async job execution | **Forge** | Yes |
| Pre-computation strategy | **Apex** | Yes |

**Key insight:** Velo, Carta, Locus are stateless. Scaling them is infrastructure. But deciding *what* to pre-compute (Apex) and *how* to interpret user intent is novel logic.

**Deployment tiers (same codebase):**
- **Local:** Single process, in-memory queue
- **Small business:** Redis-backed job queue
- **Enterprise:** Kubernetes, horizontal scaling
- **Managed:** We run it, you use it

---

## Summary

OTTO's design philosophy:

1. **Build from scratch** for control, understanding, and AI-assisted development
2. **C11** for portability, performance, and longevity
3. **Zero dependencies** for auditability and WASM size
4. **WASM-first** for browser deployment and sandboxing
5. **Renderer-agnostic UI** (ClayShards) - same code, any display
6. **Transport-agnostic APIs** - same code, any protocol
7. **Layered architecture** with clean boundaries
8. **Arena allocation** for predictable memory
9. **Errors as values** for explicit control flow

---

## 11. Security Model

OTTO implements defense-in-depth through **role-based privilege separation**:

| Role | Responsibility | Trust Level |
|------|----------------|-------------|
| **Transport (B)** | Protocol framing, TLS, limits | Touches network |
| **Parser (P)** | Parse JSON/MPS/LP, validate | Handles raw bytes |
| **Compute (C)** | Business logic, solver | Sees only clean IR |
| **Dataset (D)** | Index access, queries | Read-only data |
| **WASM (W)** | Sandboxed execution | Zero ambient authority |

**Key principles:**

- **Raw bytes are toxic** — parsing happens in isolated processes
- **IPC is the security boundary** — roles communicate via Unix sockets
- **mmap for speed, not privilege** — read-only derived indexes only
- **Compromise collapses into crash** — no lateral movement

For the complete security architecture, see [internals/security-model.md](internals/security-model.md).
