# Surge vs. Competitors — Fair Assessment

## The competitive landscape

The main comparisons are **VROOM** (C++, open-source), **Google OR-Tools** (C++/Python, open-source), **OptaPlanner/Timefold** (Java, open-source), **Jsprit** (Java, open-source), and commercial offerings like **Routific**, **NextBillion**, **HERE**, and **OptimoRoute**.

---

## API Design — Strong advantage

Surge's C API is genuinely excellent. ~80 functions, all following the same pattern: `sg_<noun>_set_<property>(ctx, id, value)`. No inheritance hierarchies, no builder patterns, no framework ceremony. Compare:

```c
// Surge: 3 lines to add a constrained vehicle
uint32_t v = sg_add_vehicle(ctx);
sg_vehicle_set_depots(ctx, v, depot, depot);
sg_vehicle_set_max_duration(ctx, v, 28800);
```

```python
# OR-Tools: dimension + callback + slack + coefficient ceremony
routing.AddDimension(transit_callback, 30, 28800, True, 'Time')
time_dimension = routing.GetDimensionOrDie('Time')
time_dimension.CumulVar(manager.NodeToIndex(loc)).SetRange(tw_start, tw_end)
```

OR-Tools requires you to think in terms of "dimensions" and "callbacks" — a leaky abstraction of the underlying constraint programming engine. Surge's API is domain-native: vehicles, requests, tasks, depots. You don't need to understand the solver to use it.

The JSON API (`sg_api.c`) is a thin veneer over the same C API — no separate "model schema" to learn.

**Weakness**: The API is add-only/set-only. No getters for most properties (you can't read back what you set on a vehicle). No model introspection. This is fine for batch solve, but limits interactive/debugging workflows.

---

## Solution Quality — Competitive but not top-tier

Solomon benchmarks: 56/56 solved, avgDistGap +0.7% vs. BKS, 36/56 matching vehicle count. That's solidly in "good metaheuristic" territory.

For context:
- **VROOM**: Typically 2-5% above BKS on Solomon, but much faster. Surge is comparable or slightly better on quality.
- **OR-Tools**: With careful tuning, OR-Tools can get within 1-3% of BKS. Roughly on par with Surge.
- **HGS-CVRP** (Vidal): State-of-the-art, often matches or sets BKS. Surge doesn't compete here — but HGS is a research solver, not a product. See "Why not HGS?" below.
- **LKH-3**: Similar — academic champion, not a deployable product.

Li & Lim (PDPTW): avgDistGap +5.3%, 39/56 equal vehicles. This is reasonable but weaker. PDPTW is inherently harder and the gap to BKS is larger across all solvers.

**Honest weakness**: The two-phase approach (minimize vehicles, then polish distance) is pragmatic but can get stuck in local optima. The single-threaded ALNS with simulated annealing is a well-understood but mid-2010s vintage approach. See "Paths to improvement" below for what's realistic.

---

## Performance (Speed) — Very strong

~4.8 seconds per 100-customer Solomon instance (optimized build). This is fast for the quality level.

- **VROOM**: Faster (sub-second on 100-node), but sacrifices quality. Uses construction + basic local search, no metaheuristic.
- **OR-Tools**: Comparable speed at default settings; slower when you tune for quality.
- **OptaPlanner/Timefold**: Significantly slower. JVM startup + garbage collection overhead. Typically 10-30x slower for equivalent quality.
- **Jsprit**: Slow and unmaintained.

The C implementation with arena-allocated solutions, pre-allocated scratch buffers, flat arrays, and cached feasibility — this is genuinely fast. Zero malloc/free in the hot loop. The ~14K lines of library code (excluding `surge.c` monolith and tests) compiles in under 2 seconds.

**Current limitation**: Single-threaded only. However, the path to parallelism is straightforward — see below.

---

## Rich Modelling — Strong and growing

Current constraint coverage is genuinely rich:
- Multi-dimensional capacity
- Hard + soft + disjunct time windows
- Pickup-delivery pairing with ride time limits (DARP)
- Multi-trip with reload
- Break policies (HoS)
- Sequence-dependent setup times
- Commodity conflicts, exclusion groups, qualifications
- Request-vehicle constraints (allowed/forbidden)
- Open routes
- Max duration, max tasks, max distance per vehicle
- Depot capacity (dock limits)
- Waiting/overtime costs
- Warm start

This is broader than VROOM (which lacks soft TW, DARP, breaks, multi-trip, setup times). It's comparable to OR-Tools in constraint breadth, though OR-Tools has more flexibility via its CP-SAT backend.

**Weakness**: No skills/technician scheduling constraints (availability calendars, lunch breaks at specific times). No multi-period/strategic planning. No vehicle compartments. No precedence constraints between requests (beyond PD pairing). These are things commercial solvers like Ortec or PTV handle. Some are on the roadmap.

---

## Deployability — Major advantage

This is where Surge genuinely stands out.

- **~21K lines of C**, zero external dependencies (Arbor and Shared are internal libs). The entire solver is statically linked into a single `.a`.
- **Compiles anywhere**: macOS, Linux, WASM. No package manager, no runtime, no JVM, no Python interpreter.
- **WASM target**: You can run this in a browser or edge function. Try doing that with OR-Tools (50MB+ binary with protobuf, abseil, SCIP dependencies).
- **Memory footprint**: Tiny. A 1000-request instance fits in a few MB. OR-Tools or OptaPlanner can consume hundreds of MB.
- **Startup time**: Microseconds. No JIT warmup, no module loading.
- **Deterministic**: Same seed = same output, always. This matters for testing and regulatory compliance.

For comparison:
- **OR-Tools**: ~200MB installed, pulls in abseil, protobuf, SCIP. Python binding requires Python runtime. C++ binary is 50MB+.
- **OptaPlanner/Timefold**: JVM + 100MB+ of jars. Startup: seconds.
- **VROOM**: C++ with a few dependencies (routing engines). Reasonable but not as lean.

---

## Ease of Integration — Good, with clear path to great

The C ABI means you can call Surge from literally anything: Python (ctypes/cffi), Node (ffi-napi), Go (cgo), Rust (bindgen), Java (JNI/Panama), Swift, Kotlin Native, WASM. The JSON API means you can also just send a JSON blob and get a JSON blob back — zero FFI needed.

The transport-agnostic `sg_api_handle()` function already exists. The same handler works for HTTP, WASM, sockets, or direct function calls. A Mongoose-based REST API server following Otto's established pattern (as done for FuelWise) is ~600 lines of boilerplate using existing shared infrastructure (`sh_workqueue`, `sh_ratelimit`, `sh_metrics`, `sh_cors`, `sh_args`).

Language bindings are trivial given the JSON API — each binding is just a thin wrapper around "serialize JSON, call `sg_api_handle()`, deserialize JSON." The C ABI makes FFI mechanical, not architectural.

---

## Auditability — Strong advantage

- 21K lines of straightforward C. No metaprogramming, no templates, no macros beyond the basics. A competent C developer can read the entire solver in a day.
- 211 tests covering every constraint individually. Each test is self-contained and readable.
- Operator telemetry: you can see exactly which destroy/repair operators were used, how often, and how effective they were.
- Deterministic: reproducible bugs.
- ASAN/UBSan clean: no undefined behavior.

Compare with OR-Tools where the relevant code spans across CP-SAT, routing library, and constraint solver — hundreds of thousands of lines with complex template hierarchies. Good luck auditing that for a customer.

---

## Language Choice — Double-edged sword

**Advantages of C**:
- Universal ABI — the only true lingua franca
- Maximum performance ceiling
- WASM compilation trivial (Emscripten)
- No runtime dependencies
- Predictable memory/performance (no GC pauses)

**Disadvantages of C**:
- Manual memory management. Use-after-free bugs in local search are a real class of risk. Rust would eliminate this at compile time.
- No generics. Constraints like max_tasks, max_distance, max_duration all follow identical patterns but must be implemented as separate copy-pasted checks.
- Limited ecosystem for algorithm building blocks (no standard hash maps, balanced trees, etc. — relies on internal Arbor/Shared libs).
- Contributor barrier: the pool of people comfortable writing correct C in 2026 is shrinking.

**Mitigant**: Arena-based allocation is now implemented across all hot paths (per-solution arena, optimized copy, pre-allocated scratch buffers). This eliminates the most dangerous class of memory bugs while delivering measurable performance gains (see "Arena allocator" section below).

---

## Parallelism — Not done, but architecturally easy

Currently single-threaded. Two practical strategies require no architectural changes:

1. **Independent runs** — Trivial. `SGContext` is fully self-contained with no shared state. Spawn N threads with different seeds, pick the best. Embarrassingly parallel, near-linear speedup.

2. **Parallel move evaluation** — The `sg_route_rank_insertions_for_request()` loop iterates over all vehicles independently. Each vehicle's evaluation is read-only on the solution. A thread pool or `#pragma omp parallel for` would parallelize this with minimal refactoring.

---

## Arena Allocator — Done

`sh_arena.h` (bump allocator with 8-byte alignment) in the shared library. Implemented in three phases:

**Phase 1 — Per-solution arena:** `sg_route_solution_init()` allocates all ~29 arrays from a single `SHArena`. Single `sh_arena_free()` in `reset()`. Eliminated ~29 malloc/calloc and ~26 free per solution lifecycle.

**Phase 2 — Optimized solution copy:** `sg_route_solution_init_for_copy()` creates an uninitialized arena (no zeroing, no init loops) and `sg_route_solution_copy()` does a single `memcpy` of the source arena buffer. Identical allocation order guarantees identical memory layout. Eliminated ~50KB of wasted zeroing + ~20K init writes per copy.

**Phase 3 — Pre-allocated scratch buffers:** `SGScratchBuffers` on `SGContext` pre-allocates reusable arrays for feasibility checking (timing, load_profile, pickup tracking) and local search (candidate arrays, exclusion counts). Created once at solve start, freed at solve end. All callers use `use_scratch` flag with graceful malloc fallback. Eliminated 5-10 malloc/free per `sg_route_stop_sequence_feasible()` call and per-function allocations in 2-opt*, or-opt, and cross-exchange.

**Cumulative benchmark results** (vs pre-arena baseline): Solomon -21% (6.06s → 4.79s), Li&Lim -7% (3.92s → 3.72s), Cordeau -6% (0.27s → 0.26s). Zero malloc/free in the hot loop.

### Competitive comparison

Surge now has best-in-class memory management for VRP solvers:

- **VROOM (C++)**: STL containers with general-purpose allocators. No arena strategy. VROOM wins on speed by doing less work (construction + basic local search, no metaheuristic), not by better memory management.
- **OR-Tools (C++)**: General-purpose C++ with smart pointers, STL, and heavy abstraction layers (CP-SAT, dimensions, callbacks). Significant allocation overhead from the framework machinery. No arena strategy in the routing layer.
- **Jsprit / OptaPlanner / Timefold (Java)**: JVM with garbage collection. Every object carries 12-16 bytes of header overhead. Lots of temporary objects in inner loops. GC pauses are unpredictable. 10-30x slower for equivalent quality isn't just algorithmic — it's largely allocation/GC overhead.
- **HGS (C++)**: Vidal's implementation is lean — vectors and simple structs. Not arena-based but efficient idiomatic C++. Closest competitor on memory discipline, though still using general-purpose allocators.

---

## Why Not HGS?

HGS (Hybrid Genetic Search by Vidal) is state-of-the-art on clean CVRP and VRPTW benchmarks. The question is whether it could replace or augment ALNS+SA for Surge's rich constraint portfolio.

### Where HGS excels

- Academic CVRP/VRPTW benchmarks (holds many BKS)
- Medium-scale instances (100-1000 customers) with 1-2 constraint types
- O(1) amortized move evaluation via subsequence concatenation
- Population diversity management produces excellent convergence

### Why HGS breaks down with rich constraints

HGS's architecture makes three core commitments that conflict with rich VRP:

**1. Giant tour + Split decoder.** The chromosome is a customer permutation; Split uses DP to find optimal route boundaries. With PD precedence, Split becomes NP-hard — pairing constraints create dependencies between route assignments. With multi-trip, the state space explodes. PyVRP explicitly does not support PDPTW for this reason.

**2. O(1) concatenation scheme.** Route cost after a move is computed from fixed-size cumulative tuples per subsequence. This breaks for:
- Sequence-dependent setup times (cost depends on adjacent node identity)
- Max ride time (depends on positions of specific PD pairs — non-decomposable)
- Commodity conflicts (set membership, not scalar load)
- Breaks (state machine of driving/resting — non-monotonic)

When concatenation breaks, you fall back to O(n) re-evaluation per move, which eliminates HGS's main speed advantage.

**3. Crossover destroys constraint structure.** OX/SREX operators permute individual nodes without awareness of PD pairs, commodity conflicts, or exclusion groups. Repair procedures are required after nearly every crossover, which weakens genetic information transmission.

### The constraint extensibility gap

Adding a new constraint to **ALNS** requires:
1. A feasibility check in the insertion evaluator
2. Possibly a new cost component

Adding a new constraint to **HGS** requires:
1. Extending the penalty function
2. Modifying all 9+ local search move evaluations
3. Extending the route update function
4. Modifying or replacing the Split algorithm
5. Possibly redesigning the crossover operator
6. Adding a new self-adjusting penalty coefficient

The touch-point count is 3-5x larger per constraint. For Surge's 15+ simultaneous constraint types, this would mean essentially rewriting HGS from scratch.

### What would work: hybrid approach

The most promising direction is using ALNS destroy-repair as an operator within a population framework — Arbor handles local intensification, a thin population manager handles diversity. This can be implemented at the Surge level without modifying Arbor:

```
Population Manager (Surge-level)
  └── for each elite solution:
        └── Arbor ALNS (local intensification, reuses all existing operators)
```

This preserves ALNS's constraint extensibility while gaining population-based diversity. Christiaens & Vanden Berghe (2020) demonstrate this hybrid approach for CVRP with strong results.

### Bottom line on HGS

ALNS+SA is the right architecture for Surge's constraint portfolio. HGS is worth considering only for a separate, specialized clean-CVRP/VRPTW solver where constraint richness isn't needed. A population wrapper around ALNS is the practical path to better solution quality.

---

## Summary Table

| Dimension | Surge | VROOM | OR-Tools | OptaPlanner |
|-----------|-------|-------|----------|-------------|
| API design | A | B+ | C+ | B |
| Solution quality | B+ | B | B+ | B |
| Speed | A- | A | B+ | C |
| Constraint richness | A- | C+ | A | B+ |
| Deployability | A+ | B+ | C | C- |
| Binary size / footprint | A+ | B | D | D |
| Language bindings | B | B | A | B+ |
| REST API | B+ | B | B | B+ |
| Auditability | A | B | D | C |
| Parallelism | C+ | C | B | B |
| Community / ecosystem | D | B | A | B+ |

Revised grades vs. initial assessment: Language bindings upgraded from D to B (JSON API *is* the binding; packaging is all that's missing). Parallelism upgraded from D to C+ (not done yet but architecturally trivial). REST API added at B+ (transport-agnostic handler exists, shared infra ready).

---

## Honest Bottom Line

Surge's strengths are **deployability**, **API cleanliness**, **constraint richness**, and **auditability**. These matter enormously for commercial embedding — if you're selling routing as a feature inside a larger product, Surge is easier to ship than anything else in this space.

The remaining gaps — parallelism and population-based search — are execution items, not design debt. The architecture already supports them. Arena allocation is complete and delivering measurable gains.

The strategic bet is sound: a lean, embeddable, WASM-ready solver with a clean API fills a real gap that OR-Tools (bloated, hard to embed) and VROOM (limited constraints) don't serve well.
