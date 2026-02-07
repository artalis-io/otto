# OTTO Platform - Reproduction Cost Valuation

**Purpose:** Asset appraisal for capital contribution
**Method:** Cost-to-reproduce (engineering hours × market rate)
**Date:** February 2026

---

## Methodology

Reproduction cost = hours required to recreate equivalent functionality from scratch, multiplied by market rate for engineers with requisite expertise (LP/MIP optimization, systems programming, WebGL, WASM).

**Market rate used:** $175/hr (blended senior C/systems engineer, US market)

---

## Component Breakdown

| Component | Functionality | Hours | Value |
|-----------|---------------|-------|-------|
| **Ralph LP Solver** | Revised simplex, LU factorization, scaling, presolve, dual simplex | 1,200 | $210,000 |
| **Ralph MIP Solver** | Branch & bound, cuts, heuristics | 600 | $105,000 |
| **Ralph LAP Solver** | Hungarian, auction algorithm, priority/cardinality constraints | 500 | $87,500 |
| **Ralph Network Simplex** | Network flow, warm start, bottleneck detection | 400 | $70,000 |
| **Velo Routing Engine** | OSM parsing, CSR graph, A*, bidirectional, turn costs | 800 | $140,000 |
| **Velo ALT/Landmarks** | Landmark preprocessing, triangle inequality bounds | 300 | $52,500 |
| **Velo Truck Profiles** | Weight/height restrictions, hazmat, toll avoidance | 200 | $35,000 |
| **Carta Tile Generator** | PBF parsing, R-tree, coordinate transforms, LOD | 600 | $105,000 |
| **Carta MVT Encoder** | Vector tile protobuf encoding, clipping, simplification | 300 | $52,500 |
| **Carta PNG Renderer** | Rasterization, antialiasing, styling, text rendering | 400 | $70,000 |
| **Locus Geocoder** | Trie autocomplete, trigram fuzzy, reverse geocoding | 400 | $70,000 |
| **FuelWise Optimizer** | Station snapping, LP formulation, route segmentation | 300 | $52,500 |
| **Shared Libraries** | Geo utilities, protobuf, rate limiting, work queues, circuit breaker | 500 | $87,500 |
| **ClayShards Core** | Immediate mode UI, layout engine, component library | 400 | $70,000 |
| **ClayShards WebGL** | GPU renderer, batching, texture atlas, map tiles | 300 | $52,500 |
| **ClayShards TUI** | Terminal renderer, ANSI codes, headless mode | 200 | $35,000 |
| **ClayShards TUI-WebGL** | CRT effects, phosphor glow, scanlines, curvature | 150 | $26,250 |
| **API Servers (4)** | HTTP endpoints, rate limiting, work queues, health/stats | 400 | $70,000 |
| **WASM Compilation** | Emscripten integration, zero-dependency constraints | 300 | $52,500 |
| **Test Suite** | 700+ unit tests, integration tests, benchmarks | 400 | $70,000 |
| **Documentation** | Architecture, API docs, developer guides | 150 | $26,250 |

---

## Summary

| Category | Hours | Value |
|----------|-------|-------|
| Core Solvers (Ralph) | 2,700 | $472,500 |
| Routing (Velo) | 1,300 | $227,500 |
| Mapping (Carta) | 1,300 | $227,500 |
| Geocoding (Locus) | 400 | $70,000 |
| Fuel Optimization (FuelWise) | 300 | $52,500 |
| Infrastructure (Shared) | 500 | $87,500 |
| UI Framework (ClayShards) | 1,050 | $183,750 |
| API & Integration | 700 | $122,500 |
| Testing & Documentation | 550 | $96,250 |
| **TOTAL** | **8,800** | **$1,540,000** |

---

## Valuation Range

| Basis | Value |
|-------|-------|
| **Low estimate** (efficient reproduction) | $1,250,000 |
| **Mid estimate** (realistic reproduction) | $1,540,000 |
| **High estimate** (including R&D iterations) | $2,100,000 |

---

## Supporting Factors

1. **Specialized expertise required** - LP solver implementation requires numerical analysis expertise (PhD-level knowledge)
2. **Zero external dependencies** - No GPL/LGPL libraries, clean IP, WASM-compatible
3. **Production quality** - 700+ tests, battle-tested numerical stability
4. **Integrated system** - Components designed to work together, not standalone tools

---

## Recommended Apport Value

**$1,500,000 - $1,750,000**

This represents a defensible reproduction cost that accounts for the specialized nature of the work while remaining conservative enough to withstand scrutiny.

---

*Note: This valuation reflects reproduction cost only, not market value or revenue potential, which could be substantially higher with commercialization.*
