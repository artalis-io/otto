# OTTO Fuel Optimizer Demo Plan

## Concept

A self-contained, browser-based fuel optimization demo running in a retro CRT terminal aesthetic. No backend, no signup, no API keys - just a single HTML file with embedded WASM.

## Core Features

1. **Precomputed Route Matrix**: Top 50-100 US cities with distances, durations, and fuel station candidates
2. **City Autocomplete**: Trie-based autocomplete for origin/destination selection
3. **Truck Configuration**: Preset truck types with fuel capacity, current fuel level, MPG
4. **FuelWise + Ralph WASM**: Real LP optimization in the browser
5. **TUI-WebGL Rendering**: Fallout-style CRT terminal with phosphor glow and scanlines

## Data Architecture

### Precomputed Matrix

```
50 cities × 50 cities = 2,500 routes
100 cities × 100 cities = 10,000 routes

Per route:
- distance: 4 bytes
- duration: 4 bytes
- ~8 fuel stations avg: 8 × (lat 4B + lon 4B + price 4B + name 20B) = 256B

Total (50 cities): ~264 bytes × 2,500 = 660 KB raw, ~200 KB compressed
Total (100 cities): ~264 bytes × 10,000 = 2.6 MB raw, ~800 KB compressed
```

### Key Simplification

No polylines needed. Show route as text:
```
Chicago → Station 1 (285 mi) → Station 2 (312 mi) → Dallas
```

The optimization math is the demo, not pretty maps.

## Data Preparation (Offline)

1. Get top 50/100 US cities by trucking volume
2. Use Velo to compute 50×50 or 100×100 distance/duration matrix
3. For each route, use FuelWise station snapping to get candidate stations
4. Store as compressed binary blob embedded in HTML or fetched on load

## UI Flow

### Input Screen

```
╔══════════════════════════════════════════════════════════════╗
║  OTTO FUEL OPTIMIZER v1.0                    [DEMO MODE]     ║
╠══════════════════════════════════════════════════════════════╣
║                                                              ║
║  ORIGIN:      [Chicago, IL________] ↓                        ║
║  DESTINATION: [Dallas, TX_________] ↓                        ║
║                                                              ║
║  TRUCK TYPE:  [Freightliner Cascadia (150 gal)] ↓            ║
║  CURRENT FUEL: ████████░░░░░░░░░░░░ 42%  (63 gal)            ║
║  AVG MPG:     [6.5___]                                       ║
║                                                              ║
║  [  OPTIMIZE ROUTE  ]    [ I'M FEELING LUCKY ]               ║
║                                                              ║
╚══════════════════════════════════════════════════════════════╝
```

### Result Screen

```
╔══════════════════════════════════════════════════════════════╗
║  OPTIMAL REFUELING PLAN                      SAVINGS: $47.20 ║
╠══════════════════════════════════════════════════════════════╣
║                                                              ║
║  Chicago, IL ──────────────────────────────── 0 mi           ║
║      │  285 mi (43.8 gal)                                    ║
║      ▼                                                       ║
║  ⛽ Pilot #4521, Springfield MO ─────────── 285 mi           ║
║      FILL: 52.0 gal @ $3.12 = $162.24                        ║
║      │  312 mi (48.0 gal)                                    ║
║      ▼                                                       ║
║  ⛽ Love's #892, Oklahoma City ──────────── 597 mi           ║
║      FILL: 38.5 gal @ $2.98 = $114.73                        ║
║      │  220 mi (33.8 gal)                                    ║
║      ▼                                                       ║
║  Dallas, TX ──────────────────────────────── 817 mi          ║
║      ARRIVE WITH: 54.7 gal (36%)                             ║
║                                                              ║
║  TOTAL FUEL: 90.5 gal    TOTAL COST: $276.97                 ║
║  vs NAIVE (fill everywhere): $324.17                         ║
║                                                              ║
║  [  NEW ROUTE  ]    [  SHARE  ]    [  LEARN MORE  ]          ║
║                                                              ║
╚══════════════════════════════════════════════════════════════╝
```

## Components Needed

### ClayShards TUI Components

| Component | Status | Notes |
|-----------|--------|-------|
| `cs_tui_label` | Done | Basic text display |
| `cs_tui_button` | Done | Action buttons |
| `cs_tui_input` | Partial | Text input fields |
| `cs_tui_autocomplete` | TODO | Dropdown suggestions from trie |
| `cs_tui_slider` | TODO | Fuel level percentage |
| `cs_tui_select` | TODO | Truck type dropdown |
| `cs_tui_progress` | TODO | Fuel gauge visualization |

### WASM Modules

| Module | Purpose |
|--------|---------|
| FuelWise WASM | Station filtering, route segmentation |
| Ralph WASM | LP optimization for fuel plan |
| Locus WASM (optional) | City autocomplete trie |

## Implementation Phases

| Phase | Task | Effort |
|-------|------|--------|
| **1** | Precompute 50-city matrix offline (Velo + FuelWise) | 4 hrs |
| **2** | Binary format + gzip compression | 2 hrs |
| **3** | `cs_tui_autocomplete` component | 4 hrs |
| **4** | `cs_tui_select` and `cs_tui_slider` components | 3 hrs |
| **5** | Demo page integration + state machine | 4 hrs |
| **6** | CRT styling + phosphor glow polish | 2 hrs |
| **7** | "I'm Feeling Lucky" random route | 1 hr |

**Total: ~20 hours**

## Truck Presets

| Type | Tank Capacity | Default MPG |
|------|---------------|-------------|
| Freightliner Cascadia | 150 gal | 6.5 |
| Peterbilt 579 | 120 gal | 6.2 |
| Volvo VNL | 150 gal | 7.0 |
| Kenworth T680 | 150 gal | 6.8 |

## Fuel Price Strategy

- Use static prices from a recent snapshot
- Display "Prices as of [date]" disclaimer
- Optionally allow user to adjust base price ±20%

## Marketing Value

1. **Instant gratification** - Results in <100ms
2. **Real savings shown** - "$47 saved" is tangible
3. **Shareable** - Screenshot-friendly CRT aesthetic
4. **Zero friction** - No signup, no "request demo"
5. **Technical proof** - WASM, offline-capable, no dependencies
6. **Viral potential** - "Trucking optimization in a Pip-Boy terminal"

## "I'm Feeling Lucky" Feature

Button picks random origin/destination and runs optimization immediately. Provides instant dopamine hit for first-time visitors without requiring any input.

## Future Enhancements

- Share URL with route parameters encoded
- Compare multiple truck configurations
- Show "what if" scenarios (different fuel levels)
- Add more cities over time
- Real-time fuel price API integration (breaks offline, but more accurate)

## File Structure

```
site/
├── fuel-demo.html           # Single-file demo
├── fuel-demo.js             # Demo logic
├── fuel-demo-data.bin.gz    # Precomputed route matrix
├── fuelwise.wasm            # FuelWise module
├── ralph.wasm               # Ralph LP solver
└── tui-renderer.js          # CRT WebGL renderer
```

Or fully embedded single HTML if size permits.
