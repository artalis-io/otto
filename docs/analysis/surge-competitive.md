# Surge Competitive Analysis — Feature-by-Feature

Last updated: 2026-02-25

## Positioning

Surge is a **VRP optimization kernel**, not a logistics platform. Comparing it against full
platforms (PTV xServer, HERE Tour Planning) on platform features (real-time traffic, driver
apps, monitoring) is a category error — those are application-layer concerns handled by other
OTTO modules (Velo for routing, Pulse for execution tracking, Fuse for vehicle state).

This document compares solvers **as optimization kernels** — algorithmic quality, constraint
modeling depth, embeddability, and deployability.

---

## Competitor Profiles

### OR-Tools (Google)

Open-source CP-SAT / legacy CP solver with a routing library on top. The most widely used
open-source VRP solver. Constraint modeling is flexible via the underlying constraint
programming engine, but the API is notoriously awkward — you think in "dimensions" and
"callbacks" rather than vehicles and requests.

**Strengths**: Large community, language bindings (Python/Java/C#/.NET), flexible constraint
model via CP backend.

**Weaknesses**: CP-SAT's constraint propagation degrades badly beyond ~500 requests — memory
footprint explodes with large distance matrices. Google uses it internally for small-instance
subproblems, not fleet-scale dispatch. The C++ build is heavy (50MB+ binary, pulls abseil,
protobuf, SCIP). No WASM story. No edge deployment story. The routing library API is a leaky
abstraction over constraint programming internals.

### PTV xServer

Commercial JRE-based routing and optimization platform. European market leader for
logistics planning. Academic pedigree from Richter and Hasle — but their published results
are on clean textbook VRPTW without rich constraints.

**Strengths**: Deep European road network data, HoS compliance (EU regulations), large
enterprise customer base.

**Weaknesses**: JRE monolith, heavy to deploy, impossible to embed. The xServer licensing
model is hostile (per-transaction or per-server, consultants required for deployment). No WASM,
no edge, no offline. Black box optimization — you can't tune it, debug it, or understand
why it made a routing decision. Their solver is good on textbook problems but the product
is architecturally stuck in 2010.

### HERE Tour Planning

Cloud API for route optimization. You POST JSON, you wait, you get a result. No local
execution, no edge capability, no offline mode.

**Strengths**: HERE's map data is world-class. Simple REST API if all you need is basic
optimization.

**Weaknesses**: API-only — no solver runs on your hardware. For a truck with intermittent
connectivity, HERE is useless. Optimization quality is decent but opaque — you can't tune it,
debug it, or understand the decisions. Constraint model is limited compared to solver-native
offerings. Distance matrix must be shipped as JSON POST, which doesn't scale to large
instances. Pricing per request.

### VROOM

Open-source C++ heuristic solver. Fast construction + basic local search. Needs OSRM or
Valhalla for road network routing.

**Strengths**: Fast (sub-second on 100-node instances). Active community. Good OSRM
integration. Simple REST API via Docker.

**Weaknesses**: Very limited constraint model — no soft TW, no DARP, no breaks (basic
support added recently), no multi-trip, no setup times, no compartments, no precedence, no
request locking. Sacrifices quality for speed — no metaheuristic, just construction + local
search. Not suitable for problems requiring rich constraint modeling.

### jsprit

Java-based VRP solver. Effectively abandoned (last meaningful development years ago).

**Strengths**: Good constraint model for its era. Hundreds of real-world integrations
through GraphHopper.

**Weaknesses**: JVM, slow, unmaintained. No active development. Memory-hungry (JVM object
overhead). 10-30x slower than native implementations for equivalent quality.

### Ortec

Enterprise-grade solver platform. Serious customers (Coca-Cola, Unilever). Rich constraint
modeling. Consultants-required deployment.

**Strengths**: Deep constraint model (temperature zones, driver regulations, loading
sequences). Large enterprise customer base with proven production deployments.

**Weaknesses**: JRE beast with enterprise licensing. On-prem consultants-required deployment.
Zero embeddability story. Same vendor lock-in problem as PTV. You're buying a full platform
whether you want the platform or not. Black box optimization.

### cuOpt (NVIDIA)

GPU-accelerated VRP solver. Massive parallelism for brute-force neighborhood evaluation.

**Strengths**: Raw throughput on simple CVRPTW. GPU parallelism can evaluate millions of
moves per second. Fast wall-clock time on unconstrained problems.

**Weaknesses**: Shallow constraint model — basic CVRPTW, limited PD support, no compartments,
no breaks, no setup times, no multi-trip, no precedence, no request locking. Requires NVIDIA
GPU hardware and CUDA runtime. For trucking fleets needing edge deployment, requiring a GPU
cluster is a non-starter. Quality on constrained problems is mediocre — GPU parallelism helps
when the bottleneck is evaluation count, not modeling fidelity. A solution looking for a
problem.

### Gurobi/CPLEX + Custom MIP Model

Build a custom MIP formulation and throw a commercial solver at it.

**Strengths**: Provably optimal for small instances. Excellent for lower bounds.

**Weaknesses**: MIP formulation of rich VRP with 1000+ requests is computationally
intractable. Variable count explodes combinatorially. Even with cutting planes and heuristics,
you're looking at hours of compute for problems ALNS solves in seconds. MIP is the right
tool for 50-request exact solutions and lower bounds, not operational fleet dispatch.
License cost $50K+/year.

---

## Kernel-Level Comparison

| | Surge | OR-Tools | PTV | HERE | VROOM | jsprit | Ortec | cuOpt |
|---|---|---|---|---|---|---|---|---|
| **100-req quality** | BKS-level | Good | Good | Decent | Decent | Decent | Good | Good |
| **Constraint richness** | Excellent | Excellent | Good | Good | Limited | Good | Excellent | Limited |
| **Large-scale (1000+)** | Untested | Degrades badly | Decent (JRE) | Opaque API | Good | Poor | Good (JRE) | Fast but rigid |
| **Embeddability** | C/WASM, zero deps | C++, heavy | JRE monolith | API only | C++, needs OSRM | JVM | JRE + license | GPU + CUDA |
| **Edge/offline** | Native | Possible but heavy | No | No | Needs server | No | No | No (needs GPU) |
| **Deployability** | Static binary or WASM | Complex build | JRE + license | SaaS only | Docker + OSRM | JVM | JRE + license | GPU cluster |
| **Tunability** | Full source | Good | Black box | Black box | Limited | Good | Black box | Limited |
| **Cost** | Open source | Open source | $$$$$ | $$$ API | Open source | Open source | $$$$$ | $$$ GPU |

---

## Feature-by-Feature Constraint Comparison

### Legend

- **Y** = Supported, production-quality
- **P** = Partial / limited support
- **N** = Not supported
- **?** = Opaque / undocumented (black box APIs)

### Time & Scheduling

| Constraint | Surge | OR-Tools | PTV | HERE | VROOM | jsprit | Ortec | cuOpt |
|---|---|---|---|---|---|---|---|---|
| Hard time windows | Y | Y | Y | Y | Y | Y | Y | Y |
| Soft time windows (penalty) | Y | Y | P | ? | N | P | Y | N |
| Disjunct time windows (multiple per task) | Y | P (via disjunctions) | P | N | N | N | P | N |
| Max route duration | Y | Y | Y | Y | N | Y | Y | P |
| Max ride time (DARP) | Y | N | N | N | N | N | P | N |
| Waiting cost | Y | Y | P | ? | N | N | Y | N |
| Overtime cost | Y | Y | P | ? | N | N | Y | N |
| Time-dependent travel (speed profiles) | Y | Y | Y | Y | N | N | Y | N |
| Time-indexed travel brackets | Y | P | P | ? | N | N | P | N |
| Per-vehicle travel profiles | Y | Y | P | N | P (via OSRM profiles) | N | P | N |

### Capacity & Loading

| Constraint | Surge | OR-Tools | PTV | HERE | VROOM | jsprit | Ortec | cuOpt |
|---|---|---|---|---|---|---|---|---|
| Multi-dimensional capacity | Y | Y | Y | P (1-2 dims) | Y | Y | Y | P (1-2 dims) |
| Vehicle compartments (temp zones) | Y | N | P | N | N | N | Y | N |
| LIFO PD stacking | Y | P (via custom) | N | N | N | N | Y | N |
| FIFO PD stacking | Y | P (via custom) | N | N | N | N | P | N |
| Backhaul (linehaul before pickups) | Y | P (via custom) | P | N | N | P | Y | N |
| Initial vehicle loads | Y | P (via custom) | P | N | N | Y | P | N |
| Commodity conflicts (incompatible goods) | Y | N | P | N | N | N | Y | N |

### Vehicle Constraints

| Constraint | Surge | OR-Tools | PTV | HERE | VROOM | jsprit | Ortec | cuOpt |
|---|---|---|---|---|---|---|---|---|
| Vehicle qualifications / skills | Y | Y | Y | P | Y | Y | Y | P |
| Request-vehicle allowed/forbidden | Y | Y | Y | P | P (via skills) | Y | Y | P |
| Vehicle fixed cost | Y | Y | Y | Y | N | Y | Y | P |
| Vehicle per-distance cost | Y | Y | Y | ? | N | Y | Y | P |
| Vehicle per-duration cost | Y | Y | Y | ? | N | Y | Y | N |
| Max tasks per vehicle | Y | Y | Y | ? | Y | P | Y | P |
| Max distance per vehicle | Y | Y | Y | ? | Y | P | Y | P |
| Multi-trip (capacity reload at depot) | Y | N | P | N | N | P (hack) | Y | N |
| Open end (no return to depot) | Y | Y | Y | P | P | Y | Y | P |
| Open start (no depot departure) | Y | Y | P | N | P | P | P | N |

### Pickup & Delivery

| Constraint | Surge | OR-Tools | PTV | HERE | VROOM | jsprit | Ortec | cuOpt |
|---|---|---|---|---|---|---|---|---|
| PD pairing (same vehicle) | Y | Y | P | P | P (shipments) | Y | Y | P |
| PD precedence (pickup before delivery) | Y | Y | P | P | P | Y | Y | P |
| Independent PD placement (non-adjacent) | Y | Y | P | ? | N | Y | Y | ? |
| Max ride time per PD pair | Y | N | N | N | N | N | P | N |

### Breaks & Regulations

| Constraint | Surge | OR-Tools | PTV | HERE | VROOM | jsprit | Ortec | cuOpt |
|---|---|---|---|---|---|---|---|---|
| Driver breaks (max continuous work) | Y | P (via intervals) | Y | P | P (basic) | P | Y | N |
| Max total work per shift | Y | P | Y | ? | N | N | Y | N |
| Break injection in route | Y | P | Y | ? | P | N | Y | N |
| EU/US HoS rules | P (abstract model) | N | Y | P | N | N | Y | N |

### Sequencing & Ordering

| Constraint | Surge | OR-Tools | PTV | HERE | VROOM | jsprit | Ortec | cuOpt |
|---|---|---|---|---|---|---|---|---|
| Sequence-dependent setup times | Y | P (via transit callback) | P | N | N | P (via state) | Y | N |
| Inter-request precedence | Y | P (via custom) | N | N | N | N | P | N |
| Exclusion groups (≤1 per vehicle) | Y | N | N | N | N | N | P | N |

### Depot Constraints

| Constraint | Surge | OR-Tools | PTV | HERE | VROOM | jsprit | Ortec | cuOpt |
|---|---|---|---|---|---|---|---|---|
| Multiple depots | Y | Y | Y | Y | Y | Y | Y | Y |
| Depot dock capacity (max simultaneous) | Y | N | P | N | N | N | Y | N |
| Depot time windows | Y | Y | Y | Y | Y | Y | Y | P |

### Re-optimization & Warm Start

| Constraint | Surge | OR-Tools | PTV | HERE | VROOM | jsprit | Ortec | cuOpt |
|---|---|---|---|---|---|---|---|---|
| Warm start (initial solution) | Y | Y | ? | N | N | P | Y | N |
| Request locking (FROZEN to vehicle) | Y | N | ? | N | N | N | P | N |
| Request locking (COMMITTED must-serve) | Y | N | ? | N | N | N | P | N |
| Plan/ETA validation mode | Y | N | P | Y | Y | N | Y | N |

### Objective Function

| Constraint | Surge | OR-Tools | PTV | HERE | VROOM | jsprit | Ortec | cuOpt |
|---|---|---|---|---|---|---|---|---|
| Lexicographic (unassigned→vehicles→distance) | Y | P (via custom) | ? | ? | P | P | Y | P |
| Per-request drop penalty | Y | Y (disjunctions) | ? | ? | N | P | Y | N |
| Global span balancing (min-max spread) | Y | Y | P | N | N | N | Y | N |
| Configurable cost weights | Y | Y | ? | N | P | Y | Y | P |

---

## Deployment & Integration Comparison

| | Surge | OR-Tools | PTV | HERE | VROOM | jsprit | Ortec | cuOpt |
|---|---|---|---|---|---|---|---|---|
| **Language** | C11 | C++ | Java | REST API | C++ | Java | Java | CUDA/Python |
| **Binary size** | ~200KB | 50MB+ | 100MB+ JRE | N/A (SaaS) | ~5MB | 100MB+ JRE | 100MB+ JRE | ~500MB + driver |
| **WASM target** | Y | N | N | N | N | N | N | N |
| **Static link** | Y | Difficult | N | N | Possible | N | N | N |
| **Zero dependencies** | Y | N (abseil, protobuf, SCIP) | N (JRE) | N (internet) | N (OSRM/Valhalla) | N (JRE) | N (JRE) | N (CUDA, GPU) |
| **Startup time** | μs | ms | seconds | N/A | ms | seconds | seconds | seconds |
| **GC pauses** | None | None | Unpredictable | N/A | None | Unpredictable | Unpredictable | None |
| **Deterministic** | Y (same seed = same output) | Y | ? | N | P | P | ? | P |
| **Edge deployment** | Y | Difficult | N | N | N | N | N | N |
| **Offline capable** | Y | Y | N | N | N (needs OSRM) | Y | Y | Y (needs GPU) |
| **Source available** | Y (open) | Y (open) | N | N | Y (open) | Y (open) | N | N |
| **JSON API** | Y | P (protobuf native) | Y | Y | Y | N | Y | Y |
| **REST server** | Y (Mongoose) | N (build your own) | Y | Y (SaaS) | Y | N | Y | Y (cloud) |
| **Python bindings** | Y (ctypes) | Y (native) | Y | Y (REST) | Y (REST) | N | Y | Y (native) |

---

## Per-Competitor Gap Summary

### Surge vs OR-Tools

**Surge wins on**: Embeddability (WASM, zero deps, 200KB), API design (domain-native vs
dimension/callback abstraction), large-scale scaling potential (ALNS vs CP-SAT), breaks,
multi-trip, DARP ride time, commodity conflicts, exclusion groups, setup times, initial loads,
LIFO/FIFO stacking, backhaul, request locking, compartments, inter-request precedence, depot
dock capacity.

**OR-Tools wins on**: Community/ecosystem, language bindings maturity (native Python/Java),
small-instance optimality proof (CP-SAT), documentation volume.

**Verdict**: Surge has strictly broader constraint coverage. OR-Tools has a larger user base
and better documentation. On optimization quality, Surge is now competitive or better on
standard benchmarks. OR-Tools degrades badly on large instances; Surge's ALNS architecture
should scale better (untested above 100).

### Surge vs PTV xServer

**Surge wins on**: Embeddability (WASM vs JRE monolith), deployability (static binary vs
JRE + license server), cost (open source vs enterprise licensing), tunability (full source vs
black box), LIFO/FIFO stacking, inter-request precedence, exclusion groups, commodity
conflicts, request locking.

**PTV wins on**: EU HoS compliance depth (specific regulation encoding vs abstract break
model), road network data (integrated vs requires Velo), enterprise support/SLAs, installed
base in European logistics.

**Verdict**: PTV's optimization is not demonstrably better — their published results are on
clean VRPTW. Their advantage is the full platform (maps + routing + compliance + support),
not the solver kernel. For customers who want to own their optimization stack, Surge is
strictly better. For customers who want to buy a turnkey platform and talk to account managers,
PTV wins by being PTV.

### Surge vs HERE Tour Planning

**Surge wins on**: Everything except map data. Edge/offline capability (HERE requires
internet), constraint model depth, tunability, debuggability, cost, embeddability, large
distance matrix handling (HERE chokes on large JSON POST payloads).

**HERE wins on**: Map data quality, zero-setup for small problems (just call the API), brand
recognition.

**Verdict**: HERE Tour Planning is a convenient API for simple problems. For anything requiring
rich constraints, offline capability, edge deployment, or large-scale optimization, HERE
is not competitive. Different market segment entirely.

### Surge vs VROOM

**Surge wins on**: Solution quality (BKS-level vs 2-5% gap), constraint richness (VROOM
lacks soft TW, DARP, multi-trip, setup times, compartments, precedence, request locking,
breaks), optimizer depth (ALNS metaheuristic vs construction + local search).

**VROOM wins on**: Speed on simple problems (sub-second vs seconds), OSRM integration
(real road network out of the box), community size, Docker deployment simplicity.

**Verdict**: Different tools for different jobs. VROOM is the right choice for simple CVRPTW
with real road networks and sub-second response requirements. Surge is the right choice
when constraint richness or solution quality matters. As Surge gains Velo integration for
road network distances, VROOM's routing advantage diminishes.

### Surge vs jsprit

**Surge wins on**: Everything. Quality, speed, constraint coverage, deployability, active
development.

**jsprit wins on**: Existing integrations through GraphHopper (legacy).

**Verdict**: jsprit is effectively dead. Surge is better in every dimension.

### Surge vs Ortec

**Surge wins on**: Embeddability (C/WASM vs JRE), deployability (static binary vs enterprise
deployment), cost (open source vs $$$$$), tunability (full source vs black box), LIFO/FIFO
stacking, inter-request precedence, exclusion groups.

**Ortec wins on**: Specific EU/US HoS regulation encoding (vs abstract break model), proven
enterprise-scale deployments, loading sequence optimization (3D bin packing), customer
support, installed base.

**Verdict**: Ortec is the closest competitor on constraint depth. The main gap is
regulation-specific HoS encoding (Ortec has encoded specific EU/US rules; Surge provides
abstract break parameters that can express the same constraints but requires the application
layer to map regulations to parameters). On the optimization kernel itself, Surge is
competitive. Ortec's moat is enterprise relationships and consultant-driven deployment —
the same lock-in that makes Surge's open-source story compelling.

### Surge vs cuOpt (NVIDIA)

**Surge wins on**: Constraint richness (cuOpt has basic CVRPTW only), deployability (no GPU
required), edge capability, cost, offline capability, tunability.

**cuOpt wins on**: Raw throughput on simple unconstrained problems (GPU parallelism), wall-clock
time when the problem is simple enough to not need rich constraints.

**Verdict**: cuOpt is irrelevant for rich VRP. GPU parallelism helps when the bottleneck is
move evaluation count on a simple model. For trucking fleets with real constraints (breaks,
compartments, stacking, setup times), cuOpt can't model the problem at all. Also requires
GPU hardware, which rules out edge deployment entirely.

### Surge vs Gurobi/CPLEX + Custom MIP

**Surge wins on**: Scaling (ALNS handles 1000+ requests; MIP chokes beyond ~50-100),
deployment (no $50K/year license), speed (seconds vs hours for fleet-scale problems),
constraint modeling ergonomics (domain-native API vs mathematical formulation).

**MIP wins on**: Provable optimality for small instances, lower bounds, exact solutions
when compute budget is unlimited.

**Verdict**: MIP is the wrong tool for operational fleet dispatch. The variable count
explodes combinatorially with rich constraints and large instances. MIP is useful for
lower bound computation and small-instance verification, not for production routing.

---

## Unique Position

Surge is the only rich VRP solver that is simultaneously:

1. **Competitive with academic BKS** on standard benchmarks (Solomon avgVehGap +0.18, avgDistGap -0.1%)
2. **Richer in constraint modeling** than most commercial offerings (25+ constraint types)
3. **Embeddable anywhere** — browser, edge device, server, static binary
4. **Zero external dependencies** — depends only on Arbor (ALNS framework) and Shared (geo, data structures)
5. **Open source** — full algorithmic transparency and auditability
6. **Written in C** — universal ABI, WASM-compilable, no runtime, no GC

No other solver checks all six boxes. Individual boxes have stronger entries (HGS for pure
quality on clean VRPTW, Ortec for enterprise constraint depth, cuOpt for GPU throughput on
simple problems), but nobody else occupies this intersection.

Large-scale validation is underway: Gehring-Homberger benchmarks at 200, 400, and 800
customers show 42-88% vehicle match with 60-120s time budgets. Instance-adaptive
construction (S24) improves initial solutions at scale. The remaining proof point is
1000-5000 requests, which is a benchmark run + tuning campaign, not an architecture change.
