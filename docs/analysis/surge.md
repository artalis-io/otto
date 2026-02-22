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
- **HGS-CVRP** (Vidal): State-of-the-art, often matches or sets BKS. Surge doesn't compete here — but HGS is a research solver, not a product.
- **LKH-3**: Similar — academic champion, not a deployable product.

Li & Lim (PDPTW): avgDistGap +5.3%, 39/56 equal vehicles. This is reasonable but weaker. PDPTW is inherently harder and the gap to BKS is larger across all solvers.

**Honest weakness**: The two-phase approach (minimize vehicles, then polish distance) is pragmatic but can get stuck in local optima. A population-based approach (like HGS) or hybrid with exact methods would likely improve quality on larger instances. The single-threaded ALNS with simulated annealing is a well-understood but mid-2010s vintage approach.

---

## Performance (Speed) — Very strong

~6 seconds per 100-customer Solomon instance (optimized build). This is fast for the quality level.

- **VROOM**: Faster (sub-second on 100-node), but sacrifices quality. Uses construction + basic local search, no metaheuristic.
- **OR-Tools**: Comparable speed at default settings; slower when you tune for quality.
- **OptaPlanner/Timefold**: Significantly slower. JVM startup + garbage collection overhead. Typically 10-30x slower for equivalent quality.
- **Jsprit**: Slow and unmaintained.

The C implementation with no allocations in the hot loop, flat arrays, cached feasibility — this is genuinely fast. The ~14K lines of library code (excluding `surge.c` monolith and tests) compiles in under 2 seconds.

**Weakness**: Single-threaded only. No parallelism. On a 5000-request instance, competitors that can spread across 8+ cores have a real wall-clock advantage. The roadmap mentions this but it's not implemented.

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

## Ease of Integration — Good with caveats

The C ABI means you can call Surge from literally anything: Python (ctypes/cffi), Node (ffi-napi), Go (cgo), Rust (bindgen), Java (JNI/Panama), Swift, Kotlin Native, WASM... The JSON API means you can also just send a JSON blob and get a JSON blob back — zero FFI needed.

**Weakness**: No official language bindings yet. Every integrator writes their own wrapper. OR-Tools ships Python, Java, C#, and Go bindings. VROOM has a REST API via `vroom-express`. The JSON API partially addresses this, but it's not a full REST service — you'd need to wrap it.

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
- No generics. Constraints like max_tasks, max_distance, max_duration all follow identical patterns but must be implemented as separate copy-pasted checks. In a language with traits/generics, these could be unified.
- Limited ecosystem for algorithm building blocks (no standard hash maps, balanced trees, etc. — relies on internal Arbor/Shared libs).
- Contributor barrier: the pool of people comfortable writing correct C in 2026 is shrinking.

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
| Language bindings | D | B | A | B+ |
| Auditability | A | B | D | C |
| Parallelism | D | C | B | B |
| Community / ecosystem | D | B | A | B+ |

## Honest Bottom Line

Surge's strengths are **deployability**, **API cleanliness**, **constraint richness**, and **auditability**. These matter enormously for commercial embedding — if you're selling routing as a feature inside a larger product, Surge is easier to ship than anything else in this space.

The weaknesses are **single-threaded execution**, **no official language bindings**, and **solution quality that's good but not state-of-the-art** on academic benchmarks. The community/ecosystem gap is inherent to being a proprietary solver vs. Google-backed open source.

The strategic bet is sound: a lean, embeddable, WASM-ready solver with a clean API fills a real gap that OR-Tools (bloated, hard to embed) and VROOM (limited constraints) don't serve well.
