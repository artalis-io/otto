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

## Solution Quality — Competitive, approaching top-tier

**Single-threaded** (10k iters, seed 42):
- Solomon benchmarks: 56/56 solved, avgDistGap +0.2% vs. BKS, 39/56 matching vehicle count (avgVehGap +0.30). Solidly in "good metaheuristic" territory.
- Li & Lim (PDPTW): avgDistGap +4.2%, 44/56 equal vehicles (avgVehGap +0.48).

**Population-based** (10k iters, 3 generations, auto threads, Phase S13-S15):
- Solomon: avgDistGap **-0.1%** vs. BKS (beating BKS average on distance), **46/56** matching vehicle count (avgVehGap +0.18). All C1xx/C2xx (17/17) exact BKS match.
- Li & Lim: avgDistGap +3.7%, **47/56** equal vehicles (avgVehGap +0.39). LC2xx (8/8), LR2xx (11/11), LRC2xx (8/8) all exact BKS match.

For context:
- **VROOM**: Typically 2-5% above BKS on Solomon, but much faster. Surge is significantly better on quality.
- **OR-Tools**: With careful tuning, OR-Tools can get within 1-3% of BKS. Surge is now better on Solomon, comparable on PDPTW.
- **HGS-CVRP** (Vidal): State-of-the-art, often matches or sets BKS. Surge is closing the gap — particularly on vehicle count (46/56 vs HGS's ~54/56 on Solomon). The remaining gap is concentrated on tight-TW R1/RC1 instances. See "Why not HGS?" below.
- **LKH-3**: Similar — academic champion, not a deployable product.

The key algorithmic advancement is **HGS-style infeasible-space exploration**: the solver temporarily accepts constraint-violating solutions during search, with adaptive penalty weights that self-adjust per constraint type. This allows vehicle-reducing moves that require deep infeasible traversal (redistributing requests across fewer vehicles temporarily violates time windows). Combined with population-based parallel search, this is the single biggest quality lever — Solomon vehicle matches jumped from 37/56 (pre-infeasible) to 45/56 (population + infeasible).

**Instance-adaptive construction (S24)** further improves initial solutions: feature extraction (spatial CV, TW tightness) classifies instances and drives construction strategy ordering. Service-adjusted TW lower bounds, cluster TW validation, and post-construction route merging help reduce vehicle count in the starting solution, giving ALNS a head start. Most impactful on clustered instances at scale (GH-200+).

**Honest weakness**: The remaining +1 vehicle gap on R1/RC1 tight-TW instances (R104, R109-R112, RC101, RC105-RC108) shows a trade-off pattern: +1 vehicle but often lower distance (e.g., R104 +1 veh / -1.5% dist). Progressive penalty, ejection in repair, and Phase 1.5 vehicle crunch (S13-S15) improved equalVehicles from 45→46 but didn't close this gap. These instances likely require either higher iteration budgets or specialized tight-TW operators (SISRs targeting specific structural patterns).

---

## Performance (Speed) — Very strong

~9.2 seconds per 100-customer Solomon instance single-threaded, ~31 seconds with population mode (optimized build, 10k iterations). This is fast for the quality level.

- **VROOM**: Faster (sub-second on 100-node), but sacrifices quality. Uses construction + basic local search, no metaheuristic.
- **OR-Tools**: Comparable speed at default settings; slower when you tune for quality.
- **OptaPlanner/Timefold**: Significantly slower. JVM startup + garbage collection overhead. Typically 10-30x slower for equivalent quality.
- **Jsprit**: Slow and unmaintained.

The C implementation with arena-allocated solutions, pre-allocated scratch buffers, flat arrays, and cached feasibility — this is genuinely fast. Zero malloc/free in the hot loop. The ~14K lines of library code (excluding `surge.c` monolith and tests) compiles in under 2 seconds.

**Multi-threaded**: `sg_solve_parallel()` runs N independent ALNS solves with different seeds, picking the best. `sg_solve_population()` adds generational warm-starting with SREX crossover and diversity filtering — same compute budget, but guided search finds significantly better solutions. Population mode with infeasible-space exploration and progressive penalty is the strongest configuration: Solomon 46/56 equal vehicles (avgDistGap -0.1%), Li & Lim 47/56 equal vehicles (avgDistGap +3.7%).

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
- Open routes (open start + open end)
- Max duration, max tasks, max distance per vehicle
- Depot capacity (dock limits)
- Waiting/overtime costs
- Warm start
- Time-dependent travel (speed profiles)
- Per-vehicle travel profiles
- LIFO/FIFO PD stacking (per-vehicle)
- Backhaul constraint (linehaul before PD pickups)
- Request locking (NONE/COMMITTED/FROZEN) for live re-optimization
- Vehicle compartments (frozen/chilled/ambient capacity)
- Inter-request precedence (same-vehicle ordering)

This is broader than VROOM (which lacks soft TW, DARP, breaks, multi-trip, setup times, request locking, compartments, precedence). It exceeds OR-Tools in constraint breadth — OR-Tools has flexibility via its legacy CP solver backend but lacks native support for compartments, inter-request precedence, multi-trip, commodity conflicts, exclusion groups, sequence-dependent setup, LIFO/FIFO stacking, and backhaul.

**Weakness**: No skills/technician scheduling constraints (availability calendars, lunch breaks at specific times). No multi-period/strategic planning. No energy/EV cost model. These are things commercial solvers like Ortec or PTV handle — but vehicle compartments (frozen/chilled/ambient) and inter-request precedence ("deliver A before B") are now implemented, closing the last two solver-layer modelling gaps vs PTV/Ortec.

---

## Deployability — Major advantage

This is where Surge genuinely stands out.

- **~40K lines of C** (including tests and benchmarks), zero external dependencies (Arbor and Shared are internal libs). The entire solver is statically linked into a single `.a`.
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
- 340 tests covering every constraint individually. Each test is self-contained and readable.
- Operator telemetry: you can see exactly which destroy/repair operators were used, how often, and how effective they were.
- Deterministic: reproducible bugs.
- ASAN/UBSan clean: no undefined behavior.

Compare with OR-Tools where the relevant code spans across the legacy CP solver, routing library, and constraint solver — hundreds of thousands of lines with complex template hierarchies. Good luck auditing that for a customer.

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

## Parallelism — Done

Two strategies implemented in `sg_parallel.c`:

1. **Independent runs** (`sg_solve_parallel`) — N threads with different seeds, pick the best. `SGContext` is fully self-contained with zero shared state. Embarrassingly parallel. On Li & Lim 100-customer instances (4 threads, 10K iterations): 15 wins vs 0 losses compared to single-threaded.

2. **Population-based search** (`sg_solve_population`) — Multi-generational ALNS with elite pool warm-starting. Each generation runs N parallel threads, harvests best solutions into a sorted pool, and subsequent generations warm-start from elite parents via tournament selection. Same total compute budget as independent runs. Combined with infeasible-space exploration (Phase S12), this is the strongest configuration:

| Benchmark | Metric | Single-thread | Population (3 gen) |
|-----------|--------|---------------|--------------------|
| Solomon | equalVehicles | 39/56 | **46/56** |
| Solomon | avgVehGap | +0.30 | **+0.18** |
| Solomon | avgDistGap | +0.2% | **-0.1%** |
| Li & Lim | equalVehicles | 44/56 | **47/56** |
| Li & Lim | avgVehGap | +0.48 | **+0.39** |
| Li & Lim | avgDistGap | +4.2% | **+3.7%** |

Remaining opportunity: **Parallel move evaluation** — the `sg_route_rank_insertions_for_request()` vehicle loop is read-only per vehicle and could be parallelized with a thread pool for additional intra-solve speedup.

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
- **OR-Tools (C++)**: General-purpose C++ with smart pointers, STL, and heavy abstraction layers (legacy CP solver, dimensions, callbacks). Significant allocation overhead from the framework machinery. No arena strategy in the routing layer.
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

**Status**: Implemented as `sg_solve_population()`. Elite pool with tournament selection, generational warm-starting via the existing `sg_set_initial_routes()` mechanism. Benchmark results on Li & Lim (4 threads, 10K iterations, 3 generations): 10 wins vs 6 losses compared to independent parallel runs, avg distance improvement ~0.6%.

### Bottom line on HGS

ALNS+SA is the right architecture for Surge's constraint portfolio. HGS is worth considering only for a separate, specialized clean-CVRP/VRPTW solver where constraint richness isn't needed. The population-ALNS hybrid is now implemented and delivering measurable quality gains.

---

## Summary Table

| Dimension | Surge | VROOM | OR-Tools | OptaPlanner |
|-----------|-------|-------|----------|-------------|
| API design | A | B+ | C+ | B |
| Solution quality | B+ | B | B+ | B |
| Speed | A- | A | B+ | C |
| Constraint richness | A | C+ | A- | B+ |
| Deployability | A+ | B+ | C | C- |
| Binary size / footprint | A+ | B | D | D |
| Language bindings | A- | B | A | B+ |
| REST API | A | B | B | B+ |
| Auditability | A | B | D | C |
| Parallelism | B+ | C | B | B |
| Community / ecosystem | D | B | A | B+ |

Revised grades vs. initial assessment: Constraint richness A (compartments, precedence, request locking close all solver-layer gaps vs PTV/Ortec — Surge now exceeds OR-Tools on constraint breadth). Language bindings A- (Python + Node.js bindings exist and work; packaging/distribution remaining). REST API A (Mongoose-based server with rate limiting, work queue, Prometheus metrics, CORS). Parallelism B+ (independent runs + population-based search implemented).

---

## Competitive Gap Analysis: What's Missing to Be GOAT

### Current Benchmark Quality (2026-02-25)

- Solomon (VRPTW, 56 cases, population): avgVehGap +0.18, avgDistGap -0.1%, 46/56 equal vehicles, lexiNonWorse 14
- Li & Lim (PDPTW, 56 cases, population): avgVehGap +0.39, avgDistGap +3.7%, 47/56 equal vehicles, lexiNonWorse 27

### Missing Features — Grouped by Impact

#### Infrastructure (High Impact, Straightforward)

| Gap | Status | Notes |
|-----|--------|-------|
| ~~REST API server~~ | **Done** | Mongoose-based, rate limiting, work queue, Prometheus metrics, CORS. E2e test suite added. |
| ~~WASM build~~ | **Done** | Emscripten target compiles and runs. |
| ~~Language bindings~~ | **Done** | Python (ctypes) + Node.js (ffi-napi) exist and work. PyPI/npm packaging remaining. |

#### Modelling Gaps vs Competitors

**High Value (would close real deals):**

| Feature | Who Has It | Impact |
|---------|-----------|--------|
| ~~**Time-dependent travel**~~ | ~~OR-Tools, PTV, HERE~~ | **Done.** Speed profiles (time-dependent duration multipliers) + per-vehicle travel profiles shipped. |
| ~~**Open start (no depot)**~~ | ~~OR-Tools, VROOM~~ | **Done.** Vehicle can skip first depot-to-stop leg (open_start flag, symmetric to open_end). |
| ~~**Global span balancing**~~ | ~~OR-Tools, Ortec, PTV~~ | **Done.** `sg_set_span_cost_duration()` / `sg_set_span_cost_distance()` add `span_cost × (max_metric - min_metric)` penalty across active routes. Stats always report `duration_span` / `distance_span`. 5 tests. |
| ~~**Plan/ETA validation mode**~~ | ~~VROOM, HERE~~ | **Done.** `sg_validate_plan()` validates fixed routes, computes ETAs, reports constraint violations (hard TW, capacity, PD order, ride time, max duration/distance/tasks, forbidden vehicle, qualifications). JSON API `"plan"` key. |

**Medium Value (niche but differentiating):**

| Feature | Who Has It | Impact |
|---------|-----------|--------|
| ~~**Per-vehicle travel matrix**~~ | ~~OR-Tools, VROOM~~ | **Done.** Travel profiles give each vehicle its own distance/duration matrix + speed profile. |
| ~~**Initial vehicle loads**~~ | ~~jsprit~~ | **Done.** `sg_vehicle_set_initial_load()`. First-trip capacity offset with prefix-sum feasibility. |
| ~~**LIFO/FIFO PD stacking**~~ | ~~OR-Tools~~ | **Done.** `sg_vehicle_set_pd_policy()` — LIFO (nested) or FIFO (same-order) per vehicle. Enforced in feasibility, insertion pruning, plan validation. |
| ~~**Backhaul constraint**~~ | ~~jsprit~~ | **Done.** `sg_vehicle_set_backhaul()` — all D-only stops before PD pickups. Enforced in feasibility, both insertion evaluators, plan validation. |
| **Energy/EV cost model** | OR-Tools (experimental) | Battery constraints, charging stops. Growing fast but still niche. |

#### Solution Quality

The persistent +1 vehicle gap on tight-TW instances (R1, RC1, LR1, LRC1) is the main quality weakness. Progressive penalty, ejection in repair, SREX crossover, and Phase 1.5 vehicle crunch (S13-S15) improved equalVehicles to 46/56 on Solomon, but tight-TW instances still show +1 vehicle with often better distance (trade-off pattern, e.g. R104 +1 veh / -1.5% dist).

**What top solvers do differently:**
- **HGS/PyVRP**: Population diversity + education (local search on infeasible solutions with penalty). Surge now has infeasible-space exploration (S12), population with SREX crossover and diversity filtering (S15), and progressive penalty scheduling (S13). The gap is narrowing.
- **LKH-3**: Giant-tour with Lin-Kernighan moves. Not applicable to rich VRP but devastating on clean VRPTW.

**Realistic next quality moves:**
1. **Parallel move evaluation** — the vehicle loop in insertion ranking is embarrassingly parallel. Would double iteration throughput on multi-core.
2. **Larger ALNS neighborhoods** — SISR (string removal) is implemented but could be tuned more aggressively for tight-TW instances.
3. **Large-scale validation** — test on 500-5000 request instances to measure scaling behavior.

#### What Commercial Solvers Have That Open-Source Doesn't

This is where PTV, HERE, Ortec, and OptimoRoute play. Neither OR-Tools nor VROOM has these either — they are commercial-tier features.

**Solver-layer** (requires changes to ALNS/feasibility/insertion):

| Feature | Notes |
|---------|-------|
| ~~**Vehicle compartments**~~ | **Done.** Per-compartment capacity (frozen/chilled/ambient). `sg_add_compartment_type()`, `sg_vehicle_add_compartment()`, `sg_request_set_compartment_type()`. Dual capacity check (vehicle overall + compartment). Zero overhead when unused. 11 tests. |
| ~~**Precedence between requests**~~ | **Done.** `sg_add_precedence(ctx, before_id, after_id)` — same-vehicle ordering with cycle detection. Forward-pass feasibility, precedence bounds in both insertion evaluators, plan validation. JSON API. 11 tests. |
| ~~**Live re-optimization**~~ | **Done.** Three-level request locking (NONE/COMMITTED/FROZEN). Frozen requests stay on designated vehicle via frozen vehicle map + two-pass warm-start construction. All destroy/repair/postprocess operators respect locks. Hardened with infeasible-space fallback. Stress-tested on RC101 + Li & Lim (10 integration tests). |

**Application-layer** (orchestration around the solver, already expressible with current API):

| Feature | Notes |
|---------|-------|
| **Multi-period/strategic planning** | Solve each day independently, pass vehicle end-states as next-day initial loads/positions via `sg_vehicle_set_initial_load()`. Orchestration decides which requests go to which day. |
| **Territory/zone assignment** | Pre-filter which vehicles serve which requests by geography, feed filtered problem to solver. Already expressible via `sg_request_set_allowed_vehicles()`. |
| **Driver skill calendars** | Availability = which vehicles exist today. Map calendar to vehicle set per solve, feed to solver. Qualifications already handle skill matching. |
| **Regulatory compliance** | Country-specific HoS rules map to break policy parameters. ADR routing restrictions map to per-vehicle travel profiles (restricted road network). |

### Priority Stack to GOAT

1. ~~**REST API + WASM + Python binding**~~ — **Done.** Distribution unlocked.
2. ~~**Time-dependent travel**~~ — **Done.** Speed profiles + per-vehicle travel profiles shipped.
3. ~~**Per-vehicle travel matrix**~~ — **Done.** Travel profiles.
4. ~~**Open start**~~ — **Done.**
5. ~~**Validation mode**~~ — **Done.** Dispatching integration use case closed.
6. ~~**Infeasible-space exploration**~~ — **Done.** HGS-style infeasible-space search (S12) + progressive penalty schedule (S13) + ejection chains in repair (S14) + SREX crossover + population diversity filter (S15).
7. ~~**Global span balancing**~~ — **Done.** `sg_set_span_cost_duration()` / `sg_set_span_cost_distance()`.
8. ~~**Live re-optimization**~~ — **Done.** Three-level request locking with hardened warm-start construction.
9. ~~**Vehicle compartments**~~ — **Done.** Multi-temperature fleet modelling (frozen/chilled/ambient). 11 tests.
10. ~~**Inter-request precedence**~~ — **Done.** Same-vehicle ordering constraints with cycle detection. 11 tests.

Modelling parity with OR-Tools is achieved and exceeded. All solver-layer gaps vs. commercial solvers (PTV/Ortec) are now closed: compartments, inter-request precedence, and request locking are all implemented. Surge exceeds OR-Tools on: breaks, multi-trip, DARP ride time, commodity conflicts, exclusion groups, sequence-dependent setup, initial loads, LIFO/FIFO stacking, backhaul, request locking, vehicle compartments, inter-request precedence. The only remaining modelling gap is energy/EV cost (experimental in OR-Tools, niche for trucking). The gap to GOAT is **algorithmic quality** on tight-TW instances — not infrastructure or modelling.

---

## Honest Bottom Line

Surge's strengths are **deployability**, **API cleanliness**, **constraint richness**, and **auditability**. These matter enormously for commercial embedding — if you're selling routing as a feature inside a larger product, Surge is easier to ship than anything else in this space.

As of March 2026, Surge has the broadest constraint coverage of any open-source VRP solver — and arguably matches or exceeds commercial offerings from PTV, Ortec, and HERE on solver-layer modelling. The full list: multi-dimensional capacity, hard/soft/disjunct time windows, PD pairing with ride time limits, multi-trip with reload, break policies (HoS), sequence-dependent setup times, commodity conflicts, exclusion groups, vehicle qualifications, request-vehicle constraints (allowed/forbidden), open routes, max duration/tasks/distance, depot capacity, waiting/overtime costs, warm start, time-dependent travel (speed profiles + time-indexed brackets), per-vehicle travel profiles, LIFO/FIFO PD stacking, backhaul, request locking (NONE/COMMITTED/FROZEN), vehicle compartments (multi-temperature), inter-request precedence, and instance-adaptive construction (feature-driven strategy ordering, service-adjusted lower bounds, cluster TW validation, route merging). 449 tests. ~40K lines of C. Zero external dependencies.

Distribution is solved: REST API server, WASM build, Python bindings, Node.js bindings. Parallelism is solved: independent multi-seed runs + population-based generational search. Memory management is solved: arena allocators with zero malloc/free in the hot loop.

The only remaining modelling gap vs. the entire competitive landscape is energy/EV cost (experimental in OR-Tools, niche for trucking). Everything else on the "what commercial solvers have" list is either implemented at the solver layer or already expressible as application-layer orchestration.

**The gap to GOAT is large-scale validation and the remaining tight-TW vehicle gap — not infrastructure, not modelling, not distribution.** Specifically: the +1 vehicle gap on 10/56 R1/RC1 Solomon tight-TW instances (trade-off: better distance), and the +3.7% distance gap on Li & Lim PDPTW (concentrated on LC1xx/LR1xx). The ALNS architecture scales better than CP-SAT by construction; the 200-800 customer Gehring-Homberger results (42-88% vehicle match, +6-17% distance) demonstrate scaling, with instance-adaptive construction (S24) improving initial solutions at scale. 449 tests, ~40K lines of C. See `docs/analysis/surge-competitive.md` for detailed feature-by-feature comparison against all major competitors.
